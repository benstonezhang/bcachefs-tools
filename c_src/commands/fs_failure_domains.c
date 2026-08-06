/*
 * bcachefs fs failure-domains - what failure domain losses would cost,
 * from the replicas accounting.
 *
 * The replicas accounting gives us, for every distinct replica set in the
 * filesystem, the exact device list and sector count: a complete inventory
 * of what fails together, entirely in userspace.
 *
 * An entry tolerates some number of failed devices among its set:
 *   - replicated data (nr_required <= 1): all but one may fail
 *   - erasure coded stripes (nr_required = nr_data): nr_redundant may fail
 *
 * One table: the failure domains, with what losing each would cost. A
 * nonzero 'lost' cell is a separation violation - data that a single domain
 * failure would take out. Devices with no failure domain set are their own
 * domains, so with none configured this is the per device view.
 *
 * Limits: this is aggregate - it says how much data is exposed, not which
 * extents (that's reconcile's job to find and fix). Extents whose copies
 * are partly in stripes (nr_required = 0 entries) are protected by their
 * stripes and reported via the stripe entries.
 *
 * Ported from src/commands/fs_failure_domains.rs.
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>

#include "linux/kernel.h"
#include "linux/sort.h"

#include "libbcachefs.h"
#include "cmds.h"

#define MAX_ENTRY_DEVS	64

/* One replicas accounting entry, reduced to what failure analysis needs. */
struct entry {
	u8	devs[MAX_ENTRY_DEVS];
	unsigned nr_devs;
	/* Failed devices tolerated before data is lost: */
	u8	tolerates;
	u64	sectors;
};

typedef DARRAY(struct entry) darray_entry;

struct exposure {
	darray_entry entries;
	/* All sectors with durability to lose - replicated + stripes: */
	u64	total;
};

struct score {
	/* Unreadable - too few copies or stripe blocks survive: */
	u64	lost;
	/* Survives, with less redundancy: */
	u64	degraded;
};

/* One row of the report: a named failure domain, or a single device with no
 * failure domain set (which is a domain of its own). */
struct row {
	char	*name;
	/* Device count for named domains, none for a lone unlabeled device: */
	bool	named;
	unsigned nr_devs;
	unsigned nr_offline;
	struct score score;
};

typedef DARRAY(struct row) darray_row;

static struct entry entry_make(const u8 *devs, unsigned nr_devs, u8 tolerates,
			       u64 sectors)
{
	struct entry en = { 0 };

	memcpy(en.devs, devs, min(nr_devs, MAX_ENTRY_DEVS));
	en.nr_devs = min(nr_devs, MAX_ENTRY_DEVS);
	en.tolerates = tolerates;
	en.sectors = sectors;
	return en;
}

static void exposure_from_entries(darray_entry *entries, struct exposure *e)
{
	e->entries = *entries;	/* takes ownership */

	darray_for_each(e->entries, en)
		e->total += en->sectors;
}

static void exposure_from_accounting(struct exposure *e,
				     struct accounting_result *res)
{
	darray_for_each(res->entries, ip)
	{
		struct accounting_entry *ae = *ip;

		if (ae->pos.type != BCH_DISK_ACCOUNTING_replicas)
			continue;

		u8 nr_devs	= ae->pos.replicas.nr_devs;
		u8 nr_required	= ae->pos.replicas.nr_required;
		u8 data_type	= ae->pos.replicas.data_type;
		u64 sectors	= ae->nr_counters ? ae->counters[0] : 0;

		/* Cached data has no durability to lose; nr_required == 0
		 * extents are backed by stripes, accounted via the stripe
		 * entries: */
		if (sectors == 0 || data_type == BCH_DATA_cached ||
		    nr_required == 0)
			continue;

		/* BCH_SB_MEMBER_INVALID: dead stripe blocks */
		u8 devs[MAX_ENTRY_DEVS];
		unsigned nr = 0;

		for (unsigned i = 0; i < nr_devs && i < MAX_ENTRY_DEVS; i++) {
			u8 d = ae->pos.replicas.devs[i];

			if (d != BCH_SB_MEMBER_INVALID)
				devs[nr++] = d;
		}
		if (!nr)
			continue;

		/* Replicated: survives until every copy is gone. Erasure
		 * coded (nr_required = nr_data > 1): survives nr_redundant
		 * failures. */
		u8 tolerates = nr_required > 1 ? nr_devs - nr_required :
							nr - 1;

		darray_push(&e->entries,
			    entry_make(devs, nr, tolerates, sectors));
		e->total += sectors;
	}
}

/* What failing exactly the devices in @f does: */
static struct score exposure_score(struct exposure *e, const u8 *f,
				   unsigned nr_f)
{
	struct score s = { 0, 0 };
	bool *seen = xcalloc(e->entries.nr, sizeof(bool));

	for (unsigned i = 0; i < nr_f; i++) {
		u8 d = f[i];

		darray_for_each(e->entries, ep) {
			struct entry *en = ep;
			unsigned idx = ep - e->entries.data;
			bool has = false;

			for (unsigned j = 0; j < en->nr_devs; j++)
				if (en->devs[j] == d) {
					has = true;
					break;
				}
			if (!has || seen[idx])
				continue;
			seen[idx] = true;

			unsigned nr_failed = 0;

			for (unsigned j = 0; j < en->nr_devs; j++)
				for (unsigned k = 0; k < nr_f; k++)
					if (en->devs[j] == f[k]) {
						nr_failed++;
						break;
					}

			if (nr_failed > en->tolerates)
				s.lost += en->sectors;
			else
				s.degraded += en->sectors;
		}
	}

	free(seen);
	return s;
}

/* ── The failure domains ────────────────────────────────────────────── */

/* Devices sharing a failure_domain string are grouped and scored together;
 * a device with none set is its own single device domain. Named domains
 * first (sorted), then the lone devices. */
static void domain_rows(struct exposure *e, dev_names *devs, darray_row *rows)
{
	struct domain_devs {
		char	*name;
		u8	idxs[MAX_ENTRY_DEVS];
		unsigned nr;
		unsigned nr_offline;
	};
	typedef DARRAY(struct domain_devs) darray_domain;
	darray_domain domains;
	darray_init(&domains);

	for (unsigned i = 0; i < devs->nr; i++) {
		struct dev_name *d = &devs->data[i];
		struct domain_devs *dom = NULL;

		if (!d->failure_domain)
			continue;

		darray_for_each(domains, dm)
			if (!strcmp(dm->name, d->failure_domain)) {
				dom = dm;
				break;
			}
		if (!dom) {
			struct domain_devs nd = { 0 };

			nd.name = xstrdup(d->failure_domain);
			darray_push(&domains, nd);
			dom = &darray_last(domains);
		}

		dom->idxs[dom->nr++] = d->idx;
		if (!d->online)
			dom->nr_offline++;
	}

	sort(domains.data, domains.nr, sizeof(domains.data[0]),
	     (cmp_func_t)(void *)strcmp, NULL);

	darray_for_each(domains, dom)
	{
		struct row r = {
			.name		= dom->name,	/* ownership transfers */
			.named		= true,
			.nr_devs	= dom->nr,
			.nr_offline	= dom->nr_offline,
		};

		r.score = exposure_score(e, dom->idxs, dom->nr);
		darray_push(rows, r);
	}
	darray_exit(&domains);

	for (unsigned i = 0; i < devs->nr; i++) {
		struct dev_name *d = &devs->data[i];

		if (d->failure_domain)
			continue;

		struct row r = {
			.name		= xstrdup(d->dev),
			.named		= false,
			.nr_devs	= 1,
			.nr_offline	= !d->online,
		};
		u8 idx = d->idx;

		r.score = exposure_score(e, &idx, 1);
		darray_push(rows, r);
	}
}

static void rows_free(darray_row *rows)
{
	darray_for_each(*rows, r)
		free(r->name);
	darray_exit(rows);
}

/* ── Output ─────────────────────────────────────────────────────────── */

static void header_to_text(struct printbuf *out, struct exposure *e,
			   dev_names *devs)
{
	unsigned offline = 0;

	for (unsigned i = 0; i < devs->nr; i++)
		if (!devs->data[i].online)
			offline++;

	prt_printf(out, "Devices: %zu", devs->nr);
	if (offline)
		prt_printf(out, " (%u offline)", offline);
	prt_newline(out);

	prt_str(out, "Data: ");
	prt_units_u64(out, e->total << 9);
	prt_newline(out);
}

static void report_to_text(struct printbuf *out, struct exposure *e,
			   dev_names *devs)
{
	darray_row rows;
	darray_init(&rows);
	domain_rows(e, devs, &rows);

	bool unlabeled = true;
	for (unsigned i = 0; i < devs->nr; i++)
		if (devs->data[i].failure_domain) {
			unlabeled = false;
			break;
		}

	prt_newline(out);
	if (unlabeled)
		prt_printf(out, "No failure domains configured: each device is "
			   "its own failure domain\n");

	/* The verdict: does any single domain failure lose data? */
	struct row *worst = NULL;

	darray_for_each(rows, r)
		if (!worst || r->score.lost > worst->score.lost)
			worst = r;

	if (worst) {
		const char *what = unlabeled ? "device" : "failure domain";

		if (worst->score.lost) {
			prt_str(out, "Failure domain separation violated: ");
			prt_units_u64(out, worst->score.lost << 9);
			prt_printf(out, " lost if %s %s fails\n", what,
				   worst->name);
		} else {
			prt_printf(out, "All data survives losing any one %s\n",
				   what);
		}
	}

	prt_printf(out, "\nWhat losing each failure domain would cost:\n");

	/* Write the table into a sub-buffer with no tabstops (so \r and \t
	 * stay literal), then align it, matching Printbuf::aligned. */
	struct printbuf sub = PRINTBUF;
	sub.human_readable_units = out->human_readable_units;

	prt_printf(&sub, "domain\tdevices\rlost\rdegraded\r\n");

	darray_for_each(rows, r)
	{
		prt_printf(&sub, "%s\t", r->name);
		/* online/total: */
		if (r->named)
			prt_printf(&sub, "%u/%u", r->nr_devs - r->nr_offline,
				   r->nr_devs);
		else if (r->nr_offline)
			prt_printf(&sub, "0/1");
		prt_str(&sub, "\r");
		prt_units_u64(&sub, r->score.lost << 9);
		prt_str(&sub, "\r");
		prt_units_u64(&sub, r->score.degraded << 9);
		prt_str(&sub, "\r\n");
	}

	bch2_printbuf_tabstop_align(&sub);
	prt_printf(out, "%s", sub.buf);
	printbuf_exit(&sub);

	rows_free(&rows);
}

/* ── JSON output ────────────────────────────────────────────────────── */

/* serde_json with no preserve_order serializes maps as BTreeMaps, so keys
 * come out sorted; match that so the JSON is byte-compatible. */

static void json_indent(struct printbuf *out, unsigned depth)
{
	for (unsigned i = 0; i < depth; i++)
		prt_str(out, "  ");
}

static void json_str(struct printbuf *out, const char *s)
{
	prt_char(out, '"');

	for (; *s; s++)
		switch (*s) {
		case '"':
			prt_str(out, "\\\"");
			break;
		case '\\':
			prt_str(out, "\\\\");
			break;
		case '\n':
			prt_str(out, "\\n");
			break;
		case '\r':
			prt_str(out, "\\r");
			break;
		case '\t':
			prt_str(out, "\\t");
			break;
		default:
			prt_char(out, *s);
			break;
		}

	prt_char(out, '"');
}

static void json_domain_object(struct printbuf *out, struct row *r,
			       unsigned depth)
{
	json_indent(out, depth);
	prt_str(out, "{\n");
	depth++;

	json_indent(out, depth);
	prt_str(out, "\"degraded_bytes\": ");
	prt_printf(out, "%llu", (unsigned long long)(r->score.degraded << 9));
	prt_str(out, ",\n");

	if (r->named) {
		json_indent(out, depth);
		prt_str(out, "\"devices\": ");
		prt_printf(out, "%u", r->nr_devs);
		prt_str(out, ",\n");
	}

	json_indent(out, depth);
	prt_str(out, "\"domain\": ");
	json_str(out, r->name);
	prt_str(out, ",\n");

	json_indent(out, depth);
	prt_str(out, "\"lost_bytes\": ");
	prt_printf(out, "%llu", (unsigned long long)(r->score.lost << 9));
	prt_str(out, ",\n");

	json_indent(out, depth);
	prt_str(out, "\"offline_devices\": ");
	prt_printf(out, "%u", r->nr_offline);
	prt_str(out, "\n");

	depth--;
	json_indent(out, depth);
	prt_str(out, "}");
}

/* @filesystem (the uuid, or NULL for the demo path) is emitted last, sorted
 * between "domains" and "offline_devices". */
static void report_to_json(struct printbuf *out, struct exposure *e,
			   dev_names *devs, const char *filesystem)
{
	darray_row rows;
	darray_init(&rows);
	domain_rows(e, devs, &rows);

	bool separation_violated = false;
	unsigned offline = 0;

	darray_for_each(rows, r)
		if (r->score.lost)
			separation_violated = true;

	for (unsigned i = 0; i < devs->nr; i++)
		if (!devs->data[i].online)
			offline++;

	prt_str(out, "{\n");

	json_indent(out, 1);
	prt_str(out, "\"data_bytes\": ");
	prt_printf(out, "%llu", (unsigned long long)(e->total << 9));
	prt_str(out, ",\n");

	json_indent(out, 1);
	prt_str(out, "\"devices\": ");
	prt_printf(out, "%zu", devs->nr);
	prt_str(out, ",\n");

	json_indent(out, 1);
	prt_str(out, "\"domains\": ");
	if (!rows.nr) {
		prt_str(out, "[]");
	} else {
		prt_str(out, "[\n");
		unsigned i = 0;

		darray_for_each(rows, r) {
			json_domain_object(out, r, 2);
			if (++i < rows.nr)
				prt_str(out, ",");
			prt_str(out, "\n");
		}
		json_indent(out, 1);
		prt_str(out, "]");
	}

	if (filesystem) {
		prt_str(out, ",\n");
		json_indent(out, 1);
		prt_str(out, "\"filesystem\": ");
		json_str(out, filesystem);
	}

	prt_str(out, ",\n");
	json_indent(out, 1);
	prt_str(out, "\"offline_devices\": ");
	prt_printf(out, "%u", offline);
	prt_str(out, ",\n");

	json_indent(out, 1);
	prt_str(out, "\"separation_violated\": ");
	prt_printf(out, "%s", separation_violated ? "true" : "false");
	prt_str(out, "\n");

	prt_str(out, "}\n");

	rows_free(&rows);
}

/* ── Demo scenarios - synthetic replica sets, no filesystem needed ──── */

struct demo_rng {
	u64	state;
};

/* Deterministic - same output every run: */
static u64 demo_next(struct demo_rng *r, u64 bound)
{
	r->state = r->state * 6364136223846793005ULL +
		   1442695040888963407ULL;
	return (r->state >> 33) % bound;
}

static struct dev_name demo_dev(unsigned idx, int rack)
{
	struct dev_name n = { 0 };
	char buf[16];

	n.idx = idx;
	snprintf(buf, sizeof(buf), "dev%u", idx);
	n.dev = xstrdup(buf);
	n.label = NULL;
	if (rack >= 0) {
		snprintf(buf, sizeof(buf), "rack%d", rack);
		n.failure_domain = xstrdup(buf);
	}
	n.durability = 1;
	n.online = true;
	return n;
}

struct pair_entry {
	u8	devs[2];
	u64	sectors;
};

typedef DARRAY(struct pair_entry) darray_pair;

static void pair_add(darray_pair *pairs, u8 a, u8 b, u64 sectors)
{
	if (a > b) {
		u8 t = a;
		a = b;
		b = t;
	}

	darray_for_each(*pairs, p)
		if (p->devs[0] == a && p->devs[1] == b) {
			p->sectors += sectors;
			return;
		}

	struct pair_entry pe = { .devs = { a, b }, .sectors = sectors };

	darray_push(pairs, pe);
}

static void pairs_to_entries(darray_pair *pairs, darray_entry *entries)
{
	darray_for_each(*pairs, p)
	{
		struct entry en = { 0 };

		en.devs[0] = p->devs[0];
		en.devs[1] = p->devs[1];
		en.nr_devs = 2;
		en.tolerates = 1;
		en.sectors = p->sectors;
		darray_push(entries, en);
	}
}

/* Six racks of ten, replicas=2 spread correctly: copies always cross racks.
 * No whole rack failure loses anything. */
static void scenario_racks_spread(darray_entry *entries, dev_names *devs)
{
	for (unsigned i = 0; i < 60; i++)
		darray_push(devs, demo_dev(i, i / 10));

	struct demo_rng rng = { .state = 1 };
	darray_pair pairs;
	darray_init(&pairs);

	for (unsigned i = 0; i < 6000; i++) {
		u8 a = demo_next(&rng, 60);
		u8 b;

		do {
			b = demo_next(&rng, 60);
		} while (b / 10 == a / 10);

		pair_add(&pairs, a, b, (demo_next(&rng, 7) + 1) * 2048);
	}

	pairs_to_entries(&pairs, entries);
	darray_exit(&pairs);
}

/* No failure domains, and the filesystem grew in stages: heavy old data on
 * the first eight devices, later data spread wider. The old device pairs
 * carry correlated risk. */
static void scenario_grew_in_stages(darray_entry *entries, dev_names *devs)
{
	struct era {
		u64	nr_devs;
		u64	writes;
		u64	size;
	} eras[] = {
		{ 8, 3000, 4096 },	/* the early days: 8 devices */
		{ 28, 1500, 2048 },	/* first expansion */
		{ 60, 800, 2048 },	/* current */
	};

	for (unsigned i = 0; i < 60; i++)
		darray_push(devs, demo_dev(i, -1));

	struct demo_rng rng = { .state = 2 };
	darray_pair pairs;
	darray_init(&pairs);

	for (unsigned i = 0; i < ARRAY_SIZE(eras); i++)
		for (u64 w = 0; w < eras[i].writes; w++) {
			u8 a = demo_next(&rng, eras[i].nr_devs);
			u8 b;

			do {
				b = demo_next(&rng, eras[i].nr_devs);
			} while (b == a);

			pair_add(&pairs, a, b,
				 (demo_next(&rng, 7) + 1) * eras[i].size);
		}

	pairs_to_entries(&pairs, entries);
	darray_exit(&pairs);
}

/* Failure domains, but partly off: a stretch of data was written with copies
 * landing in one rack (domains misconfigured, a rack's worth of devices
 * down, data predating the labels...). */
static void scenario_partly_off(darray_entry *entries, dev_names *devs)
{
	for (unsigned i = 0; i < 60; i++) {
		struct dev_name n = demo_dev(i, i / 10);

		/* rack5 is down - one of the ways data ends up under-spread: */
		n.online = !(i >= 50 && i < 60);
		darray_push(devs, n);
	}

	struct demo_rng rng = { .state = 3 };
	darray_pair pairs;
	darray_init(&pairs);

	for (unsigned i = 0; i < 6000; i++) {
		u8 a = demo_next(&rng, 60);
		bool same_rack = i % 8 == 0;
		u8 b;

		do {
			b = demo_next(&rng, 60);
		} while (b == a || ((b / 10 == a / 10) != same_rack));

		pair_add(&pairs, a, b, (demo_next(&rng, 7) + 1) * 2048);
	}

	pairs_to_entries(&pairs, entries);
	darray_exit(&pairs);
}

static void fs_failure_domains_usage(void)
{
	puts("bcachefs fs failure-domains - show failure domains and what losing "
	     "each would cost\n"
	     "Usage: bcachefs fs failure-domains [OPTION]... <mountpoint>\n"
	     "\n"
	     "Analyzes the replicas accounting to show, for each failure domain, "
	     "the data that would be lost if it failed - too few copies or stripe "
	     "blocks left to reconstruct - and the data that would survive "
	     "degraded. Lost data means failure domain separation is being "
	     "violated. Devices with no failure domain set are their own failure "
	     "domains.\n"
	     "\n"
	     "Options:\n"
	     "  -h, --human-readable              Human readable units\n"
	     "      --json                       JSON output: sizes in bytes; named "
	     "domains carry a \"devices\" count, single-device rows don't\n"
	     "      --demo=SCENARIO              Render a synthetic demo scenario "
	     "(domains, no-domains, partly-off) instead of a filesystem\n"
	     "      --mapper-names               Show mapper names for dm-multipath "
	     "devices\n"
	     "      --help                       Print help\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_fs_failure_domains(int argc, char *argv[])
{
	enum {
		OPT_JSON = 1000,
		OPT_DEMO,
		OPT_MAPPER_NAMES,
		OPT_HELP,
	};
	static const struct option longopts[] = {
		{ "human-readable", no_argument, NULL, 'h' },
		{ "json", no_argument, NULL, OPT_JSON },
		{ "demo", required_argument, NULL, OPT_DEMO },
		{ "mapper-names", no_argument, NULL, OPT_MAPPER_NAMES },
		{ "help", no_argument, NULL, OPT_HELP },
		{ NULL }
	};

	bool json = false;
	const char *demo = NULL;
	enum device_name_mode name_mode = DEVICE_NAME_RAW;
	int opt;

	while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			break;	/* human-readable: on by default */
		case OPT_JSON:
			json = true;
			break;
		case OPT_DEMO:
			demo = optarg;
			break;
		case OPT_MAPPER_NAMES:
			name_mode = DEVICE_NAME_MAPPER;
			break;
		case OPT_HELP:
			fs_failure_domains_usage();
			exit(EXIT_SUCCESS);
		default:
			fs_failure_domains_usage();
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	char *mountpoint = argc ? arg_pop() : ".";

	struct printbuf out = PRINTBUF;
	out.human_readable_units = true;

	if (demo) {
		dev_names devs;
		darray_init(&devs);

		darray_entry entries;
		darray_init(&entries);

		if (!strcmp(demo, "domains"))
			scenario_racks_spread(&entries, &devs);
		else if (!strcmp(demo, "no-domains"))
			scenario_grew_in_stages(&entries, &devs);
		else if (!strcmp(demo, "partly-off"))
			scenario_partly_off(&entries, &devs);
		else
			die("unknown scenario '%s' (have: domains, no-domains, "
			    "partly-off)", demo);

		struct exposure e = { 0 };
		exposure_from_entries(&entries, &e);

		if (json) {
			report_to_json(&out, &e, &devs, NULL);
		} else {
			prt_printf(&out, "Demo scenario: %s\n", demo);
			header_to_text(&out, &e, &devs);
			report_to_text(&out, &e, &devs);
		}

		darray_exit(&e.entries);
		dev_names_free(&devs);
	} else {
		struct bchfs_handle fs = bcache_fs_open(mountpoint);
		dev_names devs = bchu_fs_get_devices_mode(fs, name_mode);

		struct accounting_result res = bchu_fs_accounting_query(
			fs, 1 << BCH_DISK_ACCOUNTING_replicas);

		struct exposure e = { 0 };
		darray_init(&e.entries);
		exposure_from_accounting(&e, &res);

		char uuid_str[40];
		uuid_unparse(fs.uuid.b, uuid_str);

		if (json) {
			report_to_json(&out, &e, &devs, uuid_str);
		} else {
			prt_printf(&out, "Filesystem: %s\n", uuid_str);
			header_to_text(&out, &e, &devs);
			report_to_text(&out, &e, &devs);
		}

		darray_exit(&e.entries);
		bchu_accounting_result_free(&res);
		dev_names_free(&devs);
		bcache_fs_close(fs);
	}

	printf("%s", out.buf);
	printbuf_exit(&out);
	return 0;
}
