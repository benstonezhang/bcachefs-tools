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
	u64 key_bytes;
	u64 nr_keys;
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

struct snapshot_tree_result {
	u32 master_subvol;
	u32 root_snapshot;
	unsigned nr;
	struct bch_ioctl_snapshot_node_v2 *nodes;
};

/* Helper: query snapshot tree usage */
static struct snapshot_tree_result *query_snapshot_tree(int fd)
{
	struct snapshot_tree_result *r = xcalloc(1, sizeof(*r));
	struct bch_ioctl_snapshot_tree_query_v2 *q = NULL;
	unsigned nr = 64;

	while (1) {
		q = xrealloc(q, sizeof(*q) + nr * sizeof(q->nodes[0]));
		q->nr = nr;
		q->tree_id = 0;
		q->node_size = sizeof(q->nodes[0]);

		if (!ioctl(fd, BCH_IOCTL_SNAPSHOT_TREE_v2, q)) {
			/* clamp to what we allocated: a kernel returning
			 * more than the capacity would read past the buffer
			 * (rust min(nr, capacity)) */
			unsigned actual = min(q->nr, nr);
			r->master_subvol = q->master_subvol;
			r->root_snapshot = q->root_snapshot;
			r->nr = actual;
			r->nodes = xcalloc(actual, sizeof(r->nodes[0]));
			memcpy(r->nodes, q->nodes,
			       actual * sizeof(r->nodes[0]));
			free(q);
			return r;
		}

		if (errno == ERANGE) {
			nr = q->total;
			continue;
		}

		if (errno != ENOTTY) {
			free(q);
			free(r);
			return NULL;
		}
		break;
	}
	free(q);

	/* Kernel predates v2: fall back, without the key counters */
	struct bch_ioctl_snapshot_tree_query *v1 = NULL;
	nr = 64;

	while (1) {
		v1 = xrealloc(v1, sizeof(*v1) + nr * sizeof(v1->nodes[0]));
		v1->nr = nr;
		v1->tree_id = 0;

		if (!ioctl(fd, BCH_IOCTL_SNAPSHOT_TREE, v1)) {
			r->master_subvol = v1->master_subvol;
			r->root_snapshot = v1->root_snapshot;
			r->nr = v1->nr;
			r->nodes = xcalloc(v1->nr, sizeof(r->nodes[0]));
			for (unsigned i = 0; i < v1->nr; i++) {
				struct bch_ioctl_snapshot_node_v2 *n =
					&r->nodes[i];
				n->id		= v1->nodes[i].id;
				n->parent	= v1->nodes[i].parent;
				n->children[0]	= v1->nodes[i].children[0];
				n->children[1]	= v1->nodes[i].children[1];
				n->subvol	= v1->nodes[i].subvol;
				n->flags	= v1->nodes[i].flags;
				n->sectors	= v1->nodes[i].sectors;
			}
			free(v1);
			return r;
		}

		if (errno == ERANGE) {
			nr = v1->total;
			continue;
		}

		free(v1);
		free(r);
		return NULL;
	}
}

static u64 subvol_size(struct snapshot_tree_result *q, u32 subvolid)
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
		struct bch_ioctl_snapshot_node_v2 *n = NULL;
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
	if (!tm || !strftime(buf, 64, "%Y-%m-%d %H:%M", tm))
		sprintf(buf, "%lld", (long long)sec);
	return buf;
}

// ---- Display: subvolume list ----

/*
 * Skipped subtrees, ported from rust's Skipped: a descent into a subvolume
 * discovered by walking can fail (the directory gone, permissions, a deep
 * ioctl error), and that shouldn't lose the rest of the listing - the
 * failure is reported (once per directory: a directory is reported once
 * however many traversals reach it) and recorded. A listing that skipped
 * anything exits nonzero: the results printed are real, but incomplete, and
 * only the exit status says so.
 */
struct skipped_dirs {
	DARRAY(char *)	dirs;
};

static bool skipped_any(const struct skipped_dirs *s)
{
	return s->dirs.nr != 0;
}

static void skipped_descend(struct skipped_dirs *skipped, const char *dir,
			    int err, const char *what)
{
	darray_for_each(&skipped->dirs, d)
		if (!strcmp(*d, dir))
			return;

	darray_push(&skipped->dirs, strdup(dir));
	fprintf(stderr, "warning: %s %s: %s\n", what, dir, strerror(-err));
}

static void skipped_exit(struct skipped_dirs *s)
{
	darray_for_each(&s->dirs, d)
		free(*d);
	darray_exit(&s->dirs);
}

static int collect_subvols(int fd, const char *path, const char *prefix,
			   bool recursive, subvol_entries *all,
			   struct skipped_dirs *skipped)
{
	u32 pos = 0;
	char *buf = xmalloc(64 << 10);
	int ret = 0;

	while (1) {
		struct bch_ioctl_subvol_readdir i = {
			.pos = pos,
			.buf_size = 64 << 10,
			.buf = (unsigned long)buf,
		};

		/*
		 * End of listing is used == 0; an ioctl error used to pass
		 * for it, silently listing a part of the tree and exiting 0
		 * (rust propagates the error, with the ioctl name as its
		 * context).
		 */
		if (ioctl(fd, BCH_IOCTL_SUBVOLUME_LIST, &i)) {
			ret = -errno;
			break;
		}

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

				/*
				 * Only what was discovered by walking may be
				 * skipped (rust Skipped::descend): the child's
				 * open failure and everything under it are
				 * reported and recorded here, and the walk
				 * carries on with the siblings.
				 */
				if (child_fd < 0)
					skipped_descend(skipped, child_path,
							-errno,
							"Failed to open");
				else {
					ret = collect_subvols(child_fd,
							      child_path,
							      full_path, true,
							      all, skipped);
					close(child_fd);
					if (ret)
						skipped_descend(skipped,
								child_path, ret,
								"BCH_IOCTL_SUBVOLUME_LIST");
				}
				free(child_path);
			}
		}
	}

	free(buf);
	return ret;
}

static int subvol_cmp_name(const void *_a, const void *_b)
{
	const struct subvol_entry *a = _a, *b = _b;
	return strcmp(a->full_path, b->full_path);
}

static int subvol_cmp_time(const void *_a, const void *_b)
{
	const struct subvol_entry *a = _a, *b = _b;
	/* rust sort_by_key(Reverse(otime_sec)): seconds only, no nsec
	 * tiebreak */
	return (b->otime_sec > a->otime_sec) - (b->otime_sec < a->otime_sec);
}

static int subvol_cmp_size(const void *_a, const void *_b)
{
	const struct subvol_entry *a = _a, *b = _b;
	if (a->sectors != b->sectors)
		return b->sectors > a->sectors ? 1 : -1;
	return 0;
}

static int print_subvol_tree_recursive(int fd, const char *path,
				       const char *prefix, bool show_snapshots,
				       struct snapshot_tree_result *q,
				       struct skipped_dirs *skipped)
{
	subvol_entries entries = { 0 };
	int cret = collect_subvols(fd, path, "", false, &entries, skipped);

	if (cret)
		return cret;

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

		if (child_fd < 0)
			skipped_descend(skipped, child_path, -errno,
					"Failed to open");
		else {
			int cret = print_subvol_tree_recursive(child_fd,
							       child_path,
							       next_prefix,
							       show_snapshots,
							       q, skipped);
			close(child_fd);
			if (cret)
				skipped_descend(skipped, child_path, cret,
						"BCH_IOCTL_SUBVOLUME_LIST");
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

	return 0;
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
				    const char *path,
				    struct subvol_entry *e,
				    struct snapshot_tree_result *q,
				    bool recursive, bool show_snapshots,
				    bool readonly,
				    struct skipped_dirs *skipped)
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
		/*
		 * full_path is relative to the subvolume root the listing
		 * was anchored at; join it onto the directory these entries
		 * were collected from (Rust: dir.join(&e.path)) - opening
		 * the bare relative name resolved against the process's
		 * cwd, so the recursive "children" key silently vanished
		 * unless cwd happened to be the subvolume root.
		 */
		char *child_path = mprintf("%s/%s", path, e->full_path);
		int child_fd = open(child_path, O_RDONLY | O_DIRECTORY);
		if (child_fd < 0) {
			/* The children key is omitted for a skipped subtree:
			 * rust descends here too, and absence says skipped */
			skipped_descend(skipped, child_path, -errno,
					"Failed to open");
		} else {
			subvol_entries children = { 0 };
			int cret = collect_subvols(child_fd, child_path, "",
						   false, &children, skipped);

			if (cret)
				skipped_descend(skipped, child_path, cret,
						"BCH_IOCTL_SUBVOLUME_LIST");

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
								child_path,
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
		free(child_path);
	}

	prt_str(buf, "}");
}

static int print_subvol_json(int fd, const char *path, bool recursive,
			     bool show_snapshots, bool readonly,
			     struct snapshot_tree_result *q,
			     struct skipped_dirs *skipped)
{
	/*
	 * Collect top-level entries only for the outer loop.
	 * Recursive children are handled by print_subvol_json_entry.
	 */
	subvol_entries entries = { 0 };
	int cret = collect_subvols(fd, path, "", false, &entries, skipped);

	if (cret)
		return cret;

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
		print_subvol_json_entry(&buf, fd, path, e, q, recursive,
					show_snapshots, readonly, skipped);
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

	return 0;
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
	sort_by sort = SORT_NONE;
	struct skipped_dirs skipped = { 0 };
	int opt;

	while ((opt = getopt_long(argc, argv, "tRsjh", longopts, NULL)) != -1)
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

	/*
	 * BCH_IOCTL_SUBVOLUME_LIST returns paths relative to the subvolume
	 * root; anchor on it like Rust's subvol_root(&target)? so a target
	 * inside a subdirectory works (the root is also what the tree and
	 * JSON views display).
	 */
	char *root;
	int sret = subvol_root(path, &root);
	if (sret)
		die("error resolving subvolume root of %s: %s", path,
		    strerror(-sret));
	path = root;

	struct bchfs_handle fs = bcache_fs_open(path);
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		die("error opening %s: %m", path);

	struct snapshot_tree_result *q = query_snapshot_tree(fd);

	if (json) {
		int jret = print_subvol_json(fd, path, recursive, snapshots,
					     readonly, q, &skipped);
		free(q);
		close(fd);
		bcache_fs_close(fs);

		if (jret)
			die("BCH_IOCTL_SUBVOLUME_LIST: %s", strerror(-jret));

		int rc = skipped_any(&skipped) ? 1 : 0;
		skipped_exit(&skipped);
		return rc;
	}

	if (tree) {
		printf("%s\n", path);
		int tret = print_subvol_tree_recursive(fd, path, "", snapshots,
						       q, &skipped);
		if (tret)
			die("BCH_IOCTL_SUBVOLUME_LIST: %s", strerror(-tret));
	} else {
		subvol_entries entries = { 0 };
		int cret = collect_subvols(fd, path, "", recursive, &entries,
					   &skipped);

		if (cret)
			die("BCH_IOCTL_SUBVOLUME_LIST: %s", strerror(-cret));

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

	int rc = skipped_any(&skipped) ? 1 : 0;
	skipped_exit(&skipped);
	return rc;
}

// ---- Display: snapshot tree ----

static void print_snapshot_subtree(struct snapshot_tree_result *q,
				   int fd, u32 id, const char *prefix,
				   bool is_last)
{
	struct bch_ioctl_snapshot_node_v2 *n = NULL;
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

	char *sectors = fmt_sectors_human(n->sectors);
	char *keys = fmt_num_human(n->nr_keys);
	printf("%s%s%s [%s, %s]", prefix, is_last ? "└── " : "├── ",
	       path ?: "(shared)", sectors, keys);
	free(sectors);
	free(keys);
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
	     "  -j, --json                  Output as JSON\n"
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
static void print_snapshot_flat(struct snapshot_tree_result *q,
				int fd, bool readonly, sort_by sort)
{
	/* Build array of entries with subvol != 0 */
	DARRAY(struct subvol_summary) entries = {};

	for (unsigned i = 0; i < q->nr; i++) {
		struct bch_ioctl_snapshot_node_v2 *n = &q->nodes[i];
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
			.key_bytes	= n->key_bytes,
			.nr_keys	= n->nr_keys,
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

	printf("%-24s %-8s %-12s %-10s %-8s %-12s %s\n", "Path", "ID",
	       "Own", "Meta", "Keys", "Total", "Flags");

	darray_for_each(entries, e)
	{
		char *f = flags_str(e->flags);
		char *own = fmt_sectors_human(e->own_sectors);
		char *meta = fmt_bytes_human(e->key_bytes);
		char *keys = fmt_num_human(e->nr_keys);
		char *total = fmt_sectors_human(e->total_sectors);
		printf("%-24s %-8u %-12s %-10s %-8s %-12s %s\n",
		       e->path, e->subvol, own, meta, keys, total, f);
		free(f);
		free(own);
		free(meta);
		free(keys);
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

/*
 * Print the snapshot tree as JSON, matching Rust's snapshot_json_value:
 * {"query_root": {...}, "nodes": [...]} with per-node key counters.
 */
static void prt_snapshot_json(struct printbuf *buf, int fd,
			      struct snapshot_tree_result *q)
{
	prt_str(buf, "{\"query_root\":{");
	prt_printf(buf, "\"subvol\":%u,\"snapshot\":%u",
		   q->master_subvol, q->root_snapshot);

	char *path = subvol_to_path(fd, q->master_subvol);
	if (path) {
		prt_str(buf, ",\"path\":");
		prt_json_str(buf, path);
		free(path);
	}

	prt_str(buf, "},\"nodes\":[");

	for (unsigned i = 0; i < q->nr; i++) {
		struct bch_ioctl_snapshot_node_v2 *n = &q->nodes[i];
		if (i)
			prt_str(buf, ",");

		prt_printf(buf, "{\"id\":%u,\"parent\":%u,\"children\":[",
			   n->id, n->parent);
		bool first = true;
		for (unsigned c = 0; c < ARRAY_SIZE(n->children); c++) {
			if (n->children[c]) {
				if (!first)
					prt_str(buf, ",");
				first = false;
				prt_printf(buf, "%u", n->children[c]);
			}
		}

		char *size = fmt_sectors_human(n->sectors);
		prt_printf(buf, "],\"subvol\":%u,\"sectors\":%llu,\"size\":",
			   n->subvol, (unsigned long long)n->sectors);
		prt_json_str(buf, size);
		free(size);

		prt_printf(buf, ",\"nr_keys\":%llu,\"key_bytes\":%llu",
			   (unsigned long long)n->nr_keys,
			   (unsigned long long)n->key_bytes);

		char *f = flags_str(n->flags);
		if (strcmp(f, "-")) {
			prt_str(buf, ",\"flags\":");
			prt_json_str(buf, f);
		}
		free(f);

		if (n->subvol) {
			char *p = subvol_to_path(fd, n->subvol);
			if (p) {
				prt_str(buf, ",\"path\":");
				prt_json_str(buf, p);
				free(p);
			}
		}

		prt_str(buf, "}");
	}

	prt_str(buf, "]}");
}

int cmd_subvolume_list_snapshots(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "flat",      no_argument,       NULL, 'f' },
		{ "recursive", no_argument,       NULL, 'R' },
		{ "readonly",  no_argument,       NULL, 'r' },
		{ "sort",      required_argument, NULL, 'S' },
		{ "json",      no_argument,       NULL, 'j' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL }
	};
	bool flat = false, readonly = false, recursive = false, json = false;
	sort_by sort = SORT_NAME;
	int opt;
	while ((opt = getopt_long(argc, argv, "fRrS:jh", longopts, NULL)) != -1)
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
			else if (!strcmp(optarg, "time"))
				sort = SORT_TIME;
			else
				die("invalid sort order %s", optarg);
			break;
		case 'j':
			json = true;
			break;
		case 'h':
			subvolume_list_snapshots_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	char *path = arg_pop() ?: ".";

	/* Same anchoring as cmd_subvolume_list: readdir paths are relative
	 * to the subvolume root (Rust: subvol_root(&target)?). */
	char *root;
	int sret = subvol_root(path, &root);
	if (sret)
		die("error resolving subvolume root of %s: %s", path,
		    strerror(-sret));
	path = root;

	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		die("error opening %s: %m", path);

	/* Collect targets: root path, plus nested non-snapshot subvols if -R. */
	struct skipped_dirs skipped = { 0 };
	DARRAY(struct snapshot_list_target) targets = {};
	darray_push(&targets, ((struct snapshot_list_target){
					.name = strdup(path),
					.full = strdup(path),
					.fd = fd }));

	if (recursive) {
		subvol_entries entries = { 0 };
		int cret = collect_subvols(fd, path, "", true, &entries,
					   &skipped);

		if (cret)
			die("BCH_IOCTL_SUBVOLUME_LIST: %s", strerror(-cret));

		darray_for_each(entries, e)
		{
			if (e->snapshot_parent) {
				free(e->name);
				free(e->full_path);
				continue;
			}
			char *child = mprintf("%s/%s", path, e->full_path);
			int cfd = open(child, O_RDONLY | O_DIRECTORY);
			if (cfd >= 0) {
				darray_push(&targets,
					    ((struct snapshot_list_target){
						    .name = strdup(e->full_path),
						    .full = child,
						    .fd = cfd }));
			} else {
				/* A discovered subvolume that can't be opened
				 * is reported and left out - its snapshots go
				 * unreported (rust descend_discovered) */
				skipped_descend(&skipped, child, -errno,
						"Failed to open");
				free(child);
			}
			free(e->name);
			free(e->full_path);
		}
		darray_exit(&entries);
	}

	if (json) {
		struct printbuf buf = PRINTBUF;
		bool first = true;

		if (recursive)
			prt_str(&buf, "[");

		darray_for_each(targets, t)
		{
			struct snapshot_tree_result *q =
				query_snapshot_tree(t->fd);

			if (!q) {
				/* targets[0] is the subvolume root the
				 * argument resolved to; only what was
				 * discovered by walking may be skipped */
				if (t == targets.data)
					die("snapshot tree ioctl error: %m");
				skipped_descend(&skipped, t->full, -errno,
						"snapshot tree ioctl");
				continue;
			}

			if (recursive) {
				if (!first)
					prt_str(&buf, ",");
				first = false;
				prt_str(&buf, "{\"path\":");
				prt_json_str(&buf, t->name);
				prt_str(&buf, ",\"snapshots\":");
				prt_snapshot_json(&buf, t->fd, q);
				prt_str(&buf, "}");
			} else {
				prt_snapshot_json(&buf, t->fd, q);
			}
			free(q);
		}

		if (recursive)
			prt_str(&buf, "]");
		prt_char(&buf, '\n');
		printf("%s", buf.buf);
		printbuf_exit(&buf);

		darray_for_each(targets, t)
		{
			close(t->fd);
			free(t->name);
			free(t->full);
		}
		darray_exit(&targets);

		int rc = skipped_any(&skipped) ? 1 : 0;
		skipped_exit(&skipped);
		return rc;
	}

	bool first = true;
	darray_for_each(targets, t)
	{
		struct snapshot_tree_result *q =
			query_snapshot_tree(t->fd);
		if (!q) {
			if (errno == ENOTTY) {
				fprintf(stderr,
					"snapshot tree ioctl not supported by this kernel\n");
				continue;
			}

			/* targets[0] is the subvolume root the argument
			 * resolved to; only what was discovered by walking
			 * may be skipped */
			if (t == targets.data)
				die("snapshot tree ioctl error: %m");
			skipped_descend(&skipped, t->full, -errno,
					"snapshot tree ioctl");
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

	int rc = skipped_any(&skipped) ? 1 : 0;
	skipped_exit(&skipped);
	return rc;
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

/*
 * Open a handle on the filesystem @path is in, or will be in: walk the
 * path's ancestors to the nearest one that exists, like rust's
 * fs_for_path(). So a path whose parent doesn't exist yet still reaches
 * the kernel for a proper ENOENT instead of failing here first.
 */
static struct bchfs_handle fs_for_path(const char *path)
{
	char *tmp = xstrdup(path);
	char *dir = NULL;

	while (strcmp(tmp, "/") && strcmp(tmp, ".")) {
		struct stat st;
		if (!stat(tmp, &st)) {
			dir = tmp;
			break;
		}
		char *parent = dirname(tmp);
		if (!strcmp(parent, tmp))
			break;
		char *next = xstrdup(parent);
		free(tmp);
		tmp = next;
	}

	if (!dir)
		dir = xstrdup(".");

	struct bchfs_handle fs = bcache_fs_open(dir);
	free(dir);
	return fs;
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
		struct bchfs_handle fs = fs_for_path(path);

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
		/* rust canonicalizes first with a descriptive error */
		char *resolved = realpath(path, NULL);
		if (!resolved)
			die("subvolume path does not exist or can not be canonicalized: %s",
			    path);

		struct bchfs_handle fs = fs_for_path(resolved);
		free(resolved);

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
	}

	return 0;
}

static void snapshot_create_usage(void)
{
	puts("bcachefs subvolume snapshot - create a snapshot \n"
	     "Usage: bcachefs subvolume snapshot [OPTION]... <source> <dest>\n"
	     "\n"
	     "Creates an instant, COW snapshot of a subvolume. Snapshots initially share\n"
	     "all data with the source and only consume additional space as either diverges.\n"
	     "Snapshots are read-only by default; use --rw for a writable snapshot.\n"
	     "Options:\n"
	     "  -r, --read-only             Make snapshot read only\n"
	     "      --rw                    Make snapshot writable\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_subvolume_snapshot(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "rw",        no_argument, NULL, 'w' },
		{ "read-only", no_argument, NULL, 'r' },
		{ "help",      no_argument, NULL, 'h' }, { NULL }
	};
	unsigned flags = BCH_SUBVOL_SNAPSHOT_CREATE;
	bool ro = false, rw = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "rh", longopts, NULL)) != -1)
		switch (opt) {
		case 'r':
			ro = true;
			break;
		case 'w':
			rw = true;
			break;
		case 'h':
			snapshot_create_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	/* rust clap conflicts_with: both is an error */
	if (ro && rw)
		die("--rw and --read-only are incompatible");

	if (ro)
		flags |= BCH_SUBVOL_SNAPSHOT_RO;

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

	struct bchfs_handle fs = fs_for_path(dst);

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
	if (!cmd || !strcmp(cmd, "--help") || !strcmp(cmd, "-h") ||
	    !strcmp(cmd, "help"))
		return usage_subvolume();
	if (!strcmp(cmd, "create") || !strcmp(cmd, "new"))
		return cmd_subvolume_create(argc, argv);
	if (!strcmp(cmd, "delete") || !strcmp(cmd, "del"))
		return cmd_subvolume_delete(argc, argv);
	if (!strcmp(cmd, "snapshot") || !strcmp(cmd, "snap"))
		return cmd_subvolume_snapshot(argc, argv);
	if (!strcmp(cmd, "list") || !strcmp(cmd, "ls"))
		return cmd_subvolume_list(argc, argv);
	if (!strcmp(cmd, "list-snapshots") || !strcmp(cmd, "ls-snap") ||
	    !strcmp(cmd, "list-snap"))
		return cmd_subvolume_list_snapshots(argc, argv);

	printf("Unknown subvolume command: %s\n", cmd);
	usage_subvolume();
	return 1;
}
