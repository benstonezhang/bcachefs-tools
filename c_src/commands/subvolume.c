/*
 * subvolume: Manage subvolumes and snapshots.
 *
 * Supports creating, deleting, and listing subvolumes. Snapshot listing
 * includes tree-based display and space usage accounting per-node.
 *
 * Ported from src/commands/subvolume.rs.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "cmds.h"

// ---- Data types ----

typedef enum {
	SORT_NAME,
	SORT_SIZE,
	SORT_TIME,
} sort_by;

struct subvol_entry {
	u32 subvolid;
	u32 flags;
	u32 snapshot_parent;
	s64 otime_sec;
	u32 otime_nsec;
	char *name;
	char *full_path;
	u64 sectors;
};

struct subvol_summary {
	char *path;
	u32 subvol;
	u64 own_sectors;
	u64 total_sectors;
	u32 flags;
};

typedef DARRAY(struct subvol_entry) subvol_entries;

// ---- Ioctl layer / Low-level helpers ----

/* Helper: resolve subvolume ID to path relative to mount point */
static char *subvol_to_path(int fd, u32 subvolid)
{
	char *buf = xmalloc(4097);
	struct bch_ioctl_subvol_to_path i = {
		.subvolid = subvolid,
		.buf_size = 4096,
		.buf = (unsigned long)(buf + 1),
	};

	if (ioctl(fd, BCH_IOCTL_SUBVOLUME_TO_PATH, &i)) {
		free(buf);
		return NULL;
	}

	/* Prepend '/' to match Rust's format */
	buf[0] = '/';
	return buf;
}

/* Helper: query snapshot tree usage */
static struct bch_ioctl_snapshot_tree_query *query_snapshot_tree(int fd)
{
	struct bch_ioctl_snapshot_tree_query *q = NULL;
	unsigned nr = 64;

	while (1) {
		q = xrealloc(q, sizeof(*q) + nr * sizeof(q->nodes[0]));
		q->nr = nr;
		q->tree_id = 0;

		if (!ioctl(fd, BCH_IOCTL_SNAPSHOT_TREE, q))
			return q;

		if (errno != ERANGE) {
			free(q);
			return NULL;
		}
		nr = q->total;
	}
}

static u64 subvol_size(struct bch_ioctl_snapshot_tree_query *q, u32 subvolid)
{
	u32 snapshot_id = 0;
	for (unsigned i = 0; i < q->nr; i++) {
		if (q->nodes[i].subvol == subvolid) {
			snapshot_id = q->nodes[i].id;
			break;
		}
	}
	if (!snapshot_id)
		return 0;

	u64 cumulative = 0;
	u32 cur = snapshot_id;
	while (cur) {
		struct bch_ioctl_snapshot_node *n = NULL;
		for (unsigned i = 0; i < q->nr; i++) {
			if (q->nodes[i].id == cur) {
				n = &q->nodes[i];
				break;
			}
		}
		if (!n)
			break;
		cumulative += n->sectors;
		cur = n->parent;
	}
	return cumulative;
}

// ---- Formatting helpers ----

static char *flags_str(u32 flags)
{
	struct printbuf buf = PRINTBUF;
	if (flags & (1 << 0)) /* BCH_SUBVOLUME_RO */
		prt_printf(&buf, "ro");
	if (flags & (1 << 2)) { /* BCH_SUBVOLUME_UNLINKED */
		if (buf.pos)
			prt_printf(&buf, ",");
		prt_printf(&buf, "unlinked");
	}
	return buf.buf ?: strdup("-");
}

static char *format_time(s64 sec)
{
	if (sec == 0)
		return strdup("-");
	struct tm *tm = localtime((time_t *)&sec);
	char *buf = xmalloc(64);
	strftime(buf, 64, "%Y-%m-%d %H:%M", tm);
	return buf;
}

// ---- Display: subvolume list ----

static void collect_subvols(int fd, const char *path, const char *prefix,
			    bool recursive, subvol_entries *all)
{
	u32 pos = 0;
	char *buf = xmalloc(64 << 10);

	while (1) {
		struct bch_ioctl_subvol_readdir i = {
			.pos = pos,
			.buf_size = 64 << 10,
			.buf = (unsigned long)buf,
		};

		if (ioctl(fd, BCH_IOCTL_SUBVOLUME_LIST, &i))
			break;

		if (!i.used)
			break;

		pos = i.pos;

		for (struct bch_ioctl_subvol_dirent *d = (void *)buf;
		     (char *)d < buf + i.used;
		     d = (void *)((char *)d + d->reclen)) {
			char *full_path =
				prefix[0] ? mprintf("%s/%s", prefix, d->path) :
						  strdup(d->path);

			struct subvol_entry e = {
				.subvolid = d->subvolid,
				.flags = d->flags,
				.snapshot_parent = d->snapshot_parent,
				.otime_sec = d->otime_sec,
				.otime_nsec = d->otime_nsec,
				.name = strdup(d->path),
				.full_path = full_path,
			};
			darray_push(all, e);

			if (recursive) {
				char *child_path =
					mprintf("%s/%s", path, d->path);
				int child_fd = open(child_path,
						    O_RDONLY | O_DIRECTORY);
				if (child_fd >= 0) {
					collect_subvols(child_fd, child_path,
							full_path, true, all);
					close(child_fd);
				}
				free(child_path);
			}
		}
	}
	free(buf);
}

static int subvol_cmp_name(const void *_a, const void *_b)
{
	const struct subvol_entry *a = _a, *b = _b;
	return strcmp(a->full_path, b->full_path);
}

static int subvol_cmp_time(const void *_a, const void *_b)
{
	const struct subvol_entry *a = _a, *b = _b;
	if (a->otime_sec != b->otime_sec)
		return b->otime_sec - a->otime_sec;
	return b->otime_nsec - a->otime_nsec;
}

static int subvol_cmp_size(const void *_a, const void *_b)
{
	const struct subvol_entry *a = _a, *b = _b;
	if (a->sectors != b->sectors)
		return b->sectors > a->sectors ? 1 : -1;
	return 0;
}

static void print_subvol_tree_recursive(int fd, const char *path,
					const char *prefix, bool show_snapshots,
					struct bch_ioctl_snapshot_tree_query *q)
{
	subvol_entries entries = { 0 };
	collect_subvols(fd, path, "", false, &entries);

	/* Filter */
	size_t n = 0;
	darray_for_each(entries, e)
	{
		if (!show_snapshots && e->snapshot_parent) {
			free(e->name);
			free(e->full_path);
		} else {
			entries.data[n++] = *e;
		}
	}
	entries.nr = n;

	darray_for_each(entries, e)
	{
		bool is_last = (e - entries.data) == (long)entries.nr - 1;
		const char *connector = is_last ? "└── " : "├── ";
		const char *child_indent = is_last ? "    " : "│   ";

		struct printbuf ann = PRINTBUF;
		if (e->snapshot_parent) {
			char *p = subvol_to_path(fd, e->snapshot_parent);
			prt_printf(&ann, "snap of %s", p ?: "(unknown)");
			free(p);
		}

		char *f = flags_str(e->flags);
		if (strcmp(f, "-")) {
			if (ann.pos)
				prt_str(&ann, ", ");
			prt_str(&ann, f);
		}
		free(f);

		if (q) {
			u64 sectors = subvol_size(q, e->subvolid);
			if (ann.pos)
				prt_str(&ann, ", ");
			char *h = fmt_sectors_human(sectors);
			prt_str(&ann, h);
			free(h);
		}

		char *t = format_time(e->otime_sec);
		if (strcmp(t, "-")) {
			if (ann.pos)
				prt_str(&ann, ", ");
			prt_str(&ann, t);
		}
		free(t);

		printf("%s%s%s%s%s%s\n", prefix, connector, e->name,
		       ann.pos ? " [" : "", ann.buf ?: "", ann.pos ? "]" : "");
		printbuf_exit(&ann);

		char *next_prefix = mprintf("%s%s", prefix, child_indent);
		char *child_path = mprintf("%s/%s", path, e->name);
		int child_fd = open(child_path, O_RDONLY | O_DIRECTORY);
		if (child_fd >= 0) {
			print_subvol_tree_recursive(child_fd, child_path,
						    next_prefix, show_snapshots,
						    q);
			close(child_fd);
		}
		free(next_prefix);
		free(child_path);
	}

	darray_for_each(entries, e)
	{
		free(e->name);
		free(e->full_path);
	}
	darray_exit(&entries);
}

static void prt_json_str(struct printbuf *buf, const char *s)
{
	prt_char(buf, '"');
	for (; *s; s++) {
		switch (*s) {
		case '"':  prt_str(buf, "\\\""); break;
		case '\\': prt_str(buf, "\\\\"); break;
		case '\n': prt_str(buf, "\\n");  break;
		case '\r': prt_str(buf, "\\r");  break;
		case '\t': prt_str(buf, "\\t");  break;
		default:   prt_char(buf, *s);     break;
		}
	}
	prt_char(buf, '"');
}

static void print_subvol_json_entry(struct printbuf *buf, int fd,
				    struct subvol_entry *e,
				    struct bch_ioctl_snapshot_tree_query *q,
				    bool recursive, bool show_snapshots,
				    bool readonly)
{
	prt_str(buf, "{");

	prt_printf(buf, "\"subvolid\":%u", e->subvolid);
	prt_str(buf, ",\"path\":");
	prt_json_str(buf, e->full_path);

	if (e->otime_sec) {
		char *t = format_time(e->otime_sec);
		prt_str(buf, ",\"otime\":");
		prt_json_str(buf, t);
		prt_printf(buf, ",\"otime_unix\":%lld",
			   (long long)e->otime_sec);
		free(t);
	}

	if (e->snapshot_parent) {
		char *p = subvol_to_path(fd, e->snapshot_parent);
		prt_str(buf, ",\"snapshot_parent\":");
		prt_json_str(buf, p ?: "(unknown)");
		free(p);
	}

	char *f = flags_str(e->flags);
	if (strcmp(f, "-")) {
		prt_str(buf, ",\"flags\":");
		prt_json_str(buf, f);
	}
	free(f);

	if (q) {
		u64 sectors = subvol_size(q, e->subvolid);
		if (sectors) {
			char *h = fmt_sectors_human(sectors);
			prt_str(buf, ",\"size\":");
			prt_json_str(buf, h);
			free(h);
			prt_printf(buf, ",\"sectors\":%llu",
				   (unsigned long long)sectors);
		}
	}

	if (recursive) {
		int child_fd = open(e->full_path, O_RDONLY | O_DIRECTORY);
		if (child_fd >= 0) {
			subvol_entries children = { 0 };
			collect_subvols(child_fd, e->full_path, "", false,
					&children);

			size_t n = 0;
			darray_for_each(children, ce)
			{
				bool skip = false;
				if (!show_snapshots && ce->snapshot_parent)
					skip = true;
				if (readonly && !(ce->flags & 1))
					skip = true;
				if (skip) {
					free(ce->name);
					free(ce->full_path);
				} else {
					ce->sectors = q ?
						subvol_size(q, ce->subvolid) :
						0;
					children.data[n++] = *ce;
				}
			}
			children.nr = n;

			if (children.nr) {
				prt_str(buf, ",\"children\":[");
				darray_for_each(children, ce)
				{
					if (ce != children.data)
						prt_str(buf, ",");
					print_subvol_json_entry(buf, child_fd,
								ce, q, true,
								show_snapshots,
								readonly);
				}
				prt_str(buf, "]");
			}

			darray_for_each(children, ce)
			{
				free(ce->name);
				free(ce->full_path);
			}
			darray_exit(&children);
			close(child_fd);
		}
	}

	prt_str(buf, "}");
}

static void print_subvol_json(int fd, const char *path, bool recursive,
			      bool show_snapshots, bool readonly,
			      struct bch_ioctl_snapshot_tree_query *q)
{
	/*
	 * Collect top-level entries only for the outer loop.
	 * Recursive children are handled by print_subvol_json_entry.
	 */
	subvol_entries entries = { 0 };
	collect_subvols(fd, path, "", false, &entries);

	size_t n = 0;
	darray_for_each(entries, e)
	{
		bool skip = false;
		if (!show_snapshots && e->snapshot_parent)
			skip = true;
		if (readonly && !(e->flags & 1))
			skip = true;
		if (skip) {
			free(e->name);
			free(e->full_path);
		} else {
			e->sectors = q ? subvol_size(q, e->subvolid) : 0;
			entries.data[n++] = *e;
		}
	}
	entries.nr = n;

	struct printbuf buf = PRINTBUF;
	prt_str(&buf, "[");
	darray_for_each(entries, e)
	{
		if (e != entries.data)
			prt_str(&buf, ",");
		print_subvol_json_entry(&buf, fd, e, q, recursive,
					show_snapshots, readonly);
	}
	prt_str(&buf, "]\n");
	printf("%s", buf.buf);
	printbuf_exit(&buf);

	darray_for_each(entries, e)
	{
		free(e->name);
		free(e->full_path);
	}
	darray_exit(&entries);
}

static void subvolume_list_usage(void)
{
	puts("bcachefs subvolume list - list subvolumes\n"
	     "Usage: bcachefs subvolume list [OPTION]... <path>\n"
	     "\n"
	     "Options:\n"
	     "  -t, --tree                  Show subvolume tree structure\n"
	     "  -R, --recursive             List subvolumes recursively\n"
	     "  -s, --snapshots             Include snapshot subvolumes\n"
	     "  -r, --readonly              Only show read-only subvolumes\n"
	     "  -S, --sort=(name|size|time) Sort order\n"
	     "  -j, --json                  Output as JSON\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_subvolume_list(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "tree", no_argument, NULL, 't' },
		{ "recursive", no_argument, NULL, 'R' },
		{ "snapshots", no_argument, NULL, 's' },
		{ "readonly", no_argument, NULL, 'r' },
		{ "sort", required_argument, NULL, 'S' },
		{ "json", no_argument, NULL, 'j' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	bool tree = false, recursive = false, snapshots = false,
	     readonly = false, json = false;
	sort_by sort = SORT_NAME;
	int opt;

	while ((opt = getopt_long(argc, argv, "tRsrS:jh", longopts, NULL)) != -1)
		switch (opt) {
		case 't':
			tree = true;
			recursive = true;
			break;
		case 'R':
			recursive = true;
			break;
		case 's':
			snapshots = true;
			break;
		case 'r':
			readonly = true;
			break;
		case 'S':
			if (!strcmp(optarg, "name"))
				sort = SORT_NAME;
			else if (!strcmp(optarg, "size"))
				sort = SORT_SIZE;
			else if (!strcmp(optarg, "time"))
				sort = SORT_TIME;
			else
				die("invalid sort order %s", optarg);
			break;
		case 'j':
			json = true;
			break;
		case 'h':
			subvolume_list_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	char *path = arg_pop() ?: ".";

	struct bchfs_handle fs = bcache_fs_open(path);
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		die("error opening %s: %m", path);

	struct bch_ioctl_snapshot_tree_query *q = query_snapshot_tree(fd);

	if (json) {
		print_subvol_json(fd, path, recursive, snapshots, readonly, q);
		free(q);
		close(fd);
		bcache_fs_close(fs);
		return 0;
	}

	if (tree) {
		printf("%s\n", path);
		print_subvol_tree_recursive(fd, path, "", snapshots, q);
	} else {
		subvol_entries entries = { 0 };
		collect_subvols(fd, path, "", recursive, &entries);

		/* Filter and populate sizes if needed */
		size_t n = 0;
		darray_for_each(entries, e)
		{
			bool skip = false;
			if (!snapshots && e->snapshot_parent)
				skip = true;
			if (readonly && !(e->flags & 1))
				skip = true;

			if (skip) {
				free(e->name);
				free(e->full_path);
			} else {
				if (q)
					e->sectors = subvol_size(q, e->subvolid);
				entries.data[n++] = *e;
			}
		}
		entries.nr = n;

		if (sort == SORT_NAME)
			qsort(entries.data, entries.nr, sizeof(entries.data[0]),
			      subvol_cmp_name);
		else if (sort == SORT_TIME)
			qsort(entries.data, entries.nr, sizeof(entries.data[0]),
			      subvol_cmp_time);
		else if (sort == SORT_SIZE)
			qsort(entries.data, entries.nr, sizeof(entries.data[0]),
			      subvol_cmp_size);

		if (snapshots)
			printf("%-24s %-8s %-16s %-12s %-12s %s\n", "Path",
			       "ID", "Created", "Flags", "Size", "Snapshot");
		else
			printf("%-24s %-8s %-16s %-12s %s\n", "Path", "ID",
			       "Created", "Flags", "Size");

		darray_for_each(entries, e)
		{
			char *t = format_time(e->otime_sec);
			char *f = flags_str(e->flags);
			char *sz = fmt_sectors_human(e->sectors);
			if (snapshots) {
				char *parent = NULL;
				if (e->snapshot_parent)
					parent = subvol_to_path(
						fd, e->snapshot_parent);
				printf("%-24s %-8u %-16s %-12s %-12s %s\n",
				       e->full_path, e->subvolid, t, f, sz,
				       parent ?: "");
				free(parent);
			} else {
				printf("%-24s %-8u %-16s %-12s %s\n",
				       e->full_path, e->subvolid, t, f, sz);
			}
			free(t);
			free(f);
			free(sz);
			free(e->name);
			free(e->full_path);
		}
		darray_exit(&entries);
	}

	free(q);
	close(fd);
	bcache_fs_close(fs);
	return 0;
}

// ---- Display: snapshot tree ----

static void print_snapshot_subtree(struct bch_ioctl_snapshot_tree_query *q,
				   int fd, u32 id, const char *prefix,
				   bool is_last)
{
	struct bch_ioctl_snapshot_node *n = NULL;
	for (unsigned i = 0; i < q->nr; i++) {
		if (q->nodes[i].id == id) {
			n = &q->nodes[i];
			break;
		}
	}
	if (!n)
		return;

	char *path = NULL;
	if (n->subvol)
		path = subvol_to_path(fd, n->subvol);

	printf("%s%s%s [%s]", prefix, is_last ? "└── " : "├── ",
	       path ?: "(shared)", fmt_sectors_human(n->sectors));
	free(path);

	char *f = flags_str(n->flags);
	if (strcmp(f, "-"))
		printf(" (%s)", f);
	free(f);
	printf("\n");

	char *child_prefix = mprintf("%s%s", prefix, is_last ? "    " : "│   ");

	unsigned nr_children = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(n->children); i++)
		if (n->children[i])
			nr_children++;

	unsigned current_child = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(n->children); i++) {
		if (n->children[i]) {
			current_child++;
			print_snapshot_subtree(q, fd, n->children[i],
					       child_prefix,
					       current_child == nr_children);
		}
	}
	free(child_prefix);
}

static void subvolume_list_snapshots_usage(void)
{
	puts("bcachefs subvolume list-snapshots - list snapshots and their usage\n"
	     "Usage: bcachefs subvolume list-snapshots [OPTION]... <path>\n"
	     "\n"
	     "Lists snapshots with disk usage attribution. The default tree view shows\n"
	     "the snapshot hierarchy. Use --flat for a tabular view showing own and\n"
	     "cumulative (total) usage per snapshot. Use --recursive (-R) to list snapshot\n"
	     "trees for nested subvolumes too. Use --json for machine-readable output\n"
	     "including snapshot IDs, parent relationships, and sector counts.\n"
	     "\n"
	     "Options:\n"
	     "  -f, --flat                  Show flat list instead of tree\n"
	     "  -R, --recursive             Include nested subvolumes\n"
	     "  -r, --readonly              Only show read-only snapshots (flat view only)\n"
	     "  -S, --sort=(name|size)      Sort order (flat view only)\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

/*
 * Print a flat list of snapshots matching Rust's print_snapshot_flat:
 * - Only includes nodes with an associated subvolume (subvol != 0)
 * - Resolves subvolume ID to path
 * - Shows Path, ID, Own, Total, Flags columns
 * - Supports --readonly filter and --sort ordering
 */
static void print_snapshot_flat(struct bch_ioctl_snapshot_tree_query *q,
				int fd, bool readonly, sort_by sort)
{
	/* Build array of entries with subvol != 0 */
	DARRAY(struct subvol_summary) entries = {};

	for (unsigned i = 0; i < q->nr; i++) {
		struct bch_ioctl_snapshot_node *n = &q->nodes[i];
		if (!n->subvol)
			continue;
		if (readonly && !(n->flags & (1 << 0)))
			continue;

		char *path = subvol_to_path(fd, n->subvol);
		if (!path) {
			path = mprintf("subvol %u", n->subvol);
		}

		struct subvol_summary s = {
			.path		= path,
			.subvol		= n->subvol,
			.own_sectors	= n->sectors,
			.total_sectors	= subvol_size(q, n->subvol),
			.flags		= n->flags,
		};
		darray_push(&entries, s);
	}

	/* Sort */
	if (sort == SORT_NAME) {
		/* qsort using path comparison */
		for (unsigned i = 0; i < entries.nr; i++)
			for (unsigned j = i + 1; j < entries.nr; j++)
				if (strcmp(entries.data[i].path,
					   entries.data[j].path) > 0) {
					typeof(*entries.data) tmp = entries.data[i];
					entries.data[i] = entries.data[j];
					entries.data[j] = tmp;
				}
	} else if (sort == SORT_SIZE) {
		/* Sort by total_sectors descending */
		for (unsigned i = 0; i < entries.nr; i++)
			for (unsigned j = i + 1; j < entries.nr; j++)
				if (entries.data[i].total_sectors <
				    entries.data[j].total_sectors) {
					typeof(*entries.data) tmp = entries.data[i];
					entries.data[i] = entries.data[j];
					entries.data[j] = tmp;
				}
	}

	printf("%-24s %-8s %-12s %-12s %s\n", "Path", "ID", "Own", "Total",
	       "Flags");

	darray_for_each(entries, e)
	{
		char *f = flags_str(e->flags);
		char *own = fmt_sectors_human(e->own_sectors);
		char *total = fmt_sectors_human(e->total_sectors);
		printf("%-24s %-8u %-12s %-12s %s\n",
		       e->path, e->subvol, own, total, f);
		free(f);
		free(own);
		free(total);
		free(e->path);
	}
	darray_exit(&entries);
}

struct snapshot_list_target {
	char *name;
	char *full;
	int fd;
};

int cmd_subvolume_list_snapshots(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "flat",      no_argument,       NULL, 'f' },
		{ "recursive", no_argument,       NULL, 'R' },
		{ "readonly",  no_argument,       NULL, 'r' },
		{ "sort",      required_argument, NULL, 'S' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL }
	};
	bool flat = false, readonly = false, recursive = false;
	sort_by sort = SORT_NAME;
	int opt;
	while ((opt = getopt_long(argc, argv, "fRrS:h", longopts, NULL)) != -1)
		switch (opt) {
		case 'f':
			flat = true;
			break;
		case 'R':
			recursive = true;
			break;
		case 'r':
			readonly = true;
			break;
		case 'S':
			if (!strcmp(optarg, "name"))
				sort = SORT_NAME;
			else if (!strcmp(optarg, "size"))
				sort = SORT_SIZE;
			else
				die("invalid sort order %s", optarg);
			break;
		case 'h':
			subvolume_list_snapshots_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	char *path = arg_pop() ?: ".";
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		die("error opening %s: %m", path);

	/* Collect targets: root path, plus nested non-snapshot subvols if -R. */
	DARRAY(struct snapshot_list_target) targets = {};
	darray_push(&targets, ((struct snapshot_list_target){
					.name = strdup(path),
					.full = strdup(path),
					.fd = fd }));

	if (recursive) {
		subvol_entries entries = { 0 };
		collect_subvols(fd, path, "", true, &entries);
		darray_for_each(entries, e)
		{
			if (e->snapshot_parent) {
				free(e->name);
				free(e->full_path);
				continue;
			}
			char *child = mprintf("%s/%s", path, e->full_path);
			int cfd = open(child, O_RDONLY | O_DIRECTORY);
			if (cfd >= 0)
				darray_push(&targets,
					    ((struct snapshot_list_target){
						    .name = strdup(e->full_path),
						    .full = child,
						    .fd = cfd }));
			else
				free(child);
			free(e->name);
			free(e->full_path);
		}
		darray_exit(&entries);
	}

	bool first = true;
	darray_for_each(targets, t)
	{
		struct bch_ioctl_snapshot_tree_query *q =
			query_snapshot_tree(t->fd);
		if (!q) {
			if (errno == ENOTTY)
				fprintf(stderr,
					"snapshot tree ioctl not supported by this kernel\n");
			else
				die("snapshot tree ioctl error: %m");
			continue;
		}

		if (recursive) {
			if (!first)
				printf("\n");
			printf("%s:\n", t->name);
			first = false;
		}

		if (flat)
			print_snapshot_flat(q, t->fd, readonly, sort);
		else if (q->nr)
			print_snapshot_subtree(q, t->fd, q->root_snapshot, "",
					       true);
		else
			printf("(no snapshot nodes)\n");

		free(q);
	}

	darray_for_each(targets, t)
	{
		close(t->fd);
		free(t->name);
		free(t->full);
	}
	darray_exit(&targets);
	return 0;
}

// ---- Command handlers ----

static void subvolume_create_usage(void)
{
	puts("bcachefs subvolume create - create a new subvolume\n"
	     "Usage: bcachefs subvolume create [OPTION]... path\n"
	     "\n"
	     "Creates a new subvolume at the given path. Subvolumes are independently\n"
	     "mountable filesystem trees, each with their own inode number space.\n"
	     "Subvolume roots may be renamed or moved as subvolume roots, but ordinary\n"
	     "files and directories cannot be renamed across subvolume boundaries.\n"
	     "\n"
	     "Options:\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_subvolume_create(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' }, { NULL }
	};
	char *path;
	int opt;

	while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			subvolume_create_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	if (!argc) {
		subvolume_create_usage();
		exit(EXIT_FAILURE);
	}

	while ((path = arg_pop())) {
		char *dir_tmp = strdup(path);
		char *dir = dirname(dir_tmp);

		struct bchfs_handle fs = bcache_fs_open(dir);

		struct bch_ioctl_subvolume_v2 v2 = {
			.dirfd = AT_FDCWD,
			.mode = 0777,
			.dst_ptr = (unsigned long)path,
		};
		struct bch_ioctl_subvolume v1 = {
			.dirfd = AT_FDCWD,
			.mode = 0777,
			.dst_ptr = (unsigned long)path,
		};

		xbchu_ioctl(fs, BCH_IOCTL_SUBVOLUME_CREATE_v2,
			    BCH_IOCTL_SUBVOLUME_CREATE, v2, v1);
		bcache_fs_close(fs);
		free(dir_tmp);
	}

	return 0;
}

static void subvolume_delete_usage(void)
{
	puts("bcachefs subvolume delete - delete an existing subvolume\n"
	     "Usage: bcachefs subvolume delete [OPTION]... path\n"
	     "\n"
	     "Options:\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_subvolume_delete(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' }, { NULL }
	};
	char *path;
	int opt;

	while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			subvolume_delete_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	if (!argc) {
		subvolume_delete_usage();
		exit(EXIT_FAILURE);
	}

	while ((path = arg_pop())) {
		char *dir_tmp = strdup(path);
		char *dir = dirname(dir_tmp);

		struct bchfs_handle fs = bcache_fs_open(dir);

		struct bch_ioctl_subvolume_v2 v2 = {
			.dirfd = AT_FDCWD,
			.dst_ptr = (unsigned long)path,
		};
		struct bch_ioctl_subvolume v1 = {
			.dirfd = AT_FDCWD,
			.dst_ptr = (unsigned long)path,
		};

		xbchu_ioctl(fs, BCH_IOCTL_SUBVOLUME_DESTROY_v2,
			    BCH_IOCTL_SUBVOLUME_DESTROY, v2, v1);
		bcache_fs_close(fs);
		free(dir_tmp);
	}

	return 0;
}

static void snapshot_create_usage(void)
{
	puts("bcachefs subvolume snapshot - create a snapshot \n"
	     "Usage: bcachefs subvolume snapshot [OPTION]... <source> <dest>\n"
	     "\n"
	     "Create a snapshot of <source> at <dest>. If specified, <source> must be a subvolume;\n"
	     "if not specified the snapshot will be of the subvolme containing <dest>.\n"
	     "Options:\n"
	     "  -r                          Make snapshot read only\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_subvolume_snapshot(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' }, { NULL }
	};
	unsigned flags = BCH_SUBVOL_SNAPSHOT_CREATE;
	int opt;

	while ((opt = getopt_long(argc, argv, "rh", longopts, NULL)) != -1)
		switch (opt) {
		case 'r':
			flags |= BCH_SUBVOL_SNAPSHOT_RO;
			break;
		case 'h':
			snapshot_create_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	char *src = arg_pop();
	char *dst = arg_pop();

	if (argc)
		die("Too many arguments");

	if (!dst) {
		dst = src;
		src = NULL;
	}
	if (!dst)
		die("Please specify a path to create");

	char *dir_tmp = strdup(dst);
	char *dir = dirname(dir_tmp);

	struct bchfs_handle fs = bcache_fs_open(dir);

	struct bch_ioctl_subvolume_v2 v2 = {
		.flags = flags,
		.dirfd = AT_FDCWD,
		.mode = 0777,
		.src_ptr = (unsigned long)src,
		.dst_ptr = (unsigned long)dst,
	};
	struct bch_ioctl_subvolume v1 = {
		.flags = flags,
		.dirfd = AT_FDCWD,
		.mode = 0777,
		.src_ptr = (unsigned long)src,
		.dst_ptr = (unsigned long)dst,
	};

	xbchu_ioctl(fs, BCH_IOCTL_SUBVOLUME_CREATE_v2,
		    BCH_IOCTL_SUBVOLUME_CREATE, v2, v1);
	bcache_fs_close(fs);
	free(dir_tmp);
	return 0;
}

static int usage_subvolume(void)
{
	puts("bcachefs subvolume - manage subvolumes and snapshots\n"
	     "Usage: bcachefs subvolume <CMD> [OPTION]\n"
	     "\n"
	     "Commands:\n"
	     "  create                  create a subvolume\n"
	     "  delete                  delete a subvolume\n"
	     "  snapshot                create a snapshot\n"
	     "  list                    list subvolumes\n"
	     "  list-snapshots          list snapshots and their usage\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
	return 0;
}

int cmd_subvolume(int argc, char *argv[])
{
	char *cmd = pop_cmd(&argc, argv);
	if (!cmd)
		return usage_subvolume();
	if (!strcmp(cmd, "create") || !strcmp(cmd, "new"))
		return cmd_subvolume_create(argc, argv);
	if (!strcmp(cmd, "delete") || !strcmp(cmd, "del"))
		return cmd_subvolume_delete(argc, argv);
	if (!strcmp(cmd, "snapshot") || !strcmp(cmd, "snap"))
		return cmd_subvolume_snapshot(argc, argv);
	if (!strcmp(cmd, "list") || !strcmp(cmd, "ls"))
		return cmd_subvolume_list(argc, argv);
	if (!strcmp(cmd, "list-snapshots") || !strcmp(cmd, "ls-snap"))
		return cmd_subvolume_list_snapshots(argc, argv);

	printf("Unknown subvolume command: %s\n", cmd);
	usage_subvolume();
	return -EINVAL;
}
