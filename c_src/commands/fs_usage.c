/*
 * fs usage: Display detailed filesystem space usage.
 *
 * Displays space usage broken down by category: replicas (data/metadata
 * replication), btree (per-btree space), compression (ratios and savings),
 * rebalance_work (pending reconcile work), and per-device breakdown.
 *
 * Ported from src/commands/fs_usage.rs.
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <ctype.h>

#include <uuid/uuid.h>

#include "linux/sort.h"
#include "linux/rcupdate.h"

#include "libbcachefs.h"
#include "alloc/buckets.h"
#include "sb/io.h"
#include "util/darray.h"
#include "cmds.h"

enum usage_field {
	FIELD_replicas,
	FIELD_btree,
	FIELD_compression,
	FIELD_rebalance_work,
	FIELD_devices,
	FIELD_NR
};

static const char *usage_field_strs[] = { "replicas",	 "btree",
					  "compression", "rebalance_work",
					  "devices",	 NULL };

struct fs_usage_config {
	unsigned fields_mask;
	bool human_readable;
	enum device_name_mode name_mode;
};

/* Replicas summary structures */

/* Durability x degraded matrix: matrix[durability][degraded] = sectors */
struct durability_matrix {
	darray_u64 *data;
	unsigned nr;
};

static void durability_matrix_add(struct durability_matrix *m, unsigned dur,
				  unsigned deg, u64 sectors)
{
	while (m->nr <= dur) {
		struct durability_matrix new_m = {
			.data = xcalloc(m->nr + 1, sizeof(*m->data)),
			.nr = m->nr + 1,
		};
		if (m->nr)
			memcpy(new_m.data, m->data, m->nr * sizeof(*m->data));
		free(m->data);
		*m = new_m;
	}

	darray_u64 *row = &m->data[dur];
	while (row->nr <= deg) {
		u64 zero = 0;
		darray_push(row, zero);
	}
	row->data[deg] += sectors;
}

/* Print the degradation header row: "undegraded  -1x  -2x ..." */
static void prt_degraded_header(struct printbuf *out, unsigned max_degraded)
{
	for (unsigned i = 0; i < max_degraded; i++) {
		if (i == 0) {
			prt_printf(out, "\tundegraded\r");
		} else {
			prt_printf(out, "-%ux\r", i);
		}
	}
	prt_newline(out);
}

/* Print a row of sector values, right-justified in columns. */
static void prt_sector_row(struct printbuf *out, darray_u64 row)
{
	darray_for_each(row, i)
	{
		if (*i) {
			prt_units_u64(out, *i << 9);
			prt_tab_rjust(out);
		}
	}
	prt_newline(out);
}

static void durability_matrix_to_text(struct printbuf *out,
				      struct durability_matrix m)
{
	unsigned max_degraded = 0;
	for (unsigned i = 0; i < m.nr; i++)
		max_degraded = max(max_degraded, m.data[i].nr);

	if (max_degraded == 0)
		return;

	printbuf_tabstop_push(out, 8);
	printbuf_tabstop_push(out, 10);
	for (unsigned i = 1; i < max_degraded; i++)
		printbuf_tabstop_push(out, 8);

	prt_degraded_header(out, max_degraded);

	for (unsigned i = 0; i < m.nr; i++) {
		if (m.data[i].nr == 0)
			continue;
		prt_printf(out, "%ux:\t", i);
		prt_sector_row(out, m.data[i]);
	}

	printbuf_tabstops_reset(out);
}

struct ec_config {
	u8 nr_data;
	u8 nr_parity;
	darray_u64 degraded;
};

typedef DARRAY(struct ec_config) ec_configs;

static void ec_config_add(ec_configs *configs, u8 nr_required, u8 nr_devs,
			  unsigned degraded, u64 sectors)
{
	u8 nr_parity = nr_devs - nr_required;
	struct ec_config *cfg = NULL;

	darray_for_each(*configs, i)
	{
		if (i->nr_data == nr_required && i->nr_parity == nr_parity) {
			cfg = i;
			break;
		}
	}

	if (!cfg) {
		struct ec_config new_cfg = { .nr_data = nr_required,
					     .nr_parity = nr_parity,
					     .degraded = { 0 } };
		darray_push(configs, new_cfg);
		cfg = &darray_last(*configs);
	}

	while (cfg->degraded.nr <= degraded) {
		u64 zero = 0;
		darray_push(&cfg->degraded, zero);
	}
	cfg->degraded.data[degraded] += sectors;
}

static int ec_config_cmp(const void *_l, const void *_r)
{
	const struct ec_config *l = _l, *r = _r;
	return cmp_int(l->nr_data, r->nr_data) ?:
			     cmp_int(l->nr_parity, r->nr_parity);
}

static void ec_configs_to_text(struct printbuf *out, ec_configs *configs)
{
	if (configs->nr == 0)
		return;

	sort(configs->data, configs->nr, sizeof(configs->data[0]),
	     ec_config_cmp, NULL);

	unsigned max_degraded = 0;
	darray_for_each(*configs, i)
		max_degraded = max(max_degraded, i->degraded.nr);

	printbuf_indent_add(out, 2);
	printbuf_tabstop_push(out, 8);
	printbuf_tabstop_push(out, 10);
	for (unsigned i = 1; i < max_degraded; i++)
		printbuf_tabstop_push(out, 8);

	prt_degraded_header(out, max_degraded);

	darray_for_each(*configs, i)
	{
		prt_printf(out, "%u+%u:\t", i->nr_data, i->nr_parity);
		prt_sector_row(out, i->degraded);
	}
	printbuf_tabstops_reset(out);
	printbuf_indent_sub(out, 2);
}

struct replicas_durability {
	unsigned durability;
	unsigned degraded;
};

static struct replicas_durability get_replicas_durability(u8 nr_devs,
							  u8 nr_required,
							  const u8 *dev_list,
							  dev_names *devs)
{
	struct replicas_durability d = { 0 };

	for (unsigned i = 0; i < nr_devs; i++) {
		struct dev_name *dev = dev_idx_to_name(devs, dev_list[i]);
		unsigned dur = dev ? dev->durability : 1;

		if (!dev)
			d.degraded += dur;
		d.durability += dur;
	}

	if (nr_required > 1)
		d.durability = nr_devs - nr_required + 1;

	return d;
}

static void prt_dev_list(struct printbuf *out, u8 nr_devs, const u8 *dev_list,
			 dev_names *devs)
{
	for (unsigned i = 0; i < nr_devs; i++) {
		if (i > 0)
			prt_str(out, " ");
		if (dev_list[i] == BCH_SB_MEMBER_INVALID) {
			prt_str(out, "none");
		} else {
			struct dev_name *d = dev_idx_to_name(devs, dev_list[i]);
			if (d)
				prt_str(out, d->dev);
			else
				prt_printf(out, "%u", dev_list[i]);
		}
	}
}

static int accounting_p_cmp(const void *_l, const void *_r)
{
	const struct accounting_entry *const *l = _l;
	const struct accounting_entry *const *r = _r;
	return bpos_cmp((*l)->pos._pad, (*r)->pos._pad);
}

static void fs_usage_v1_to_text(struct printbuf *out, struct bchfs_handle fs,
				dev_names *devs, struct fs_usage_config cfg)
{
	unsigned accounting_types =
		(1 << BCH_DISK_ACCOUNTING_replicas) |
		(1 << BCH_DISK_ACCOUNTING_persistent_reserved);

	if (cfg.fields_mask & (1 << FIELD_compression))
		accounting_types |= 1 << BCH_DISK_ACCOUNTING_compression;
	if (cfg.fields_mask & (1 << FIELD_btree))
		accounting_types |= 1 << BCH_DISK_ACCOUNTING_btree;
	if (cfg.fields_mask & (1 << FIELD_rebalance_work)) {
		if (bcachefs_kernel_version() <
		    bcachefs_metadata_version_reconcile) {
			accounting_types |=
				1 << BCH_DISK_ACCOUNTING_rebalance_work;
		} else {
			accounting_types |=
				(1 << BCH_DISK_ACCOUNTING_reconcile_work) |
				(1 << BCH_DISK_ACCOUNTING_dev_leaving);
		}
	}

	struct accounting_result res =
		bchu_fs_accounting_query(fs, accounting_types);
	if (!res.entries.nr)
		return;

	sort(res.entries.data, res.entries.nr, sizeof(res.entries.data[0]),
	     accounting_p_cmp, NULL);

	prt_printf(out, "Filesystem: ");
	pr_uuid(out, fs.uuid.b);
	prt_newline(out);

	printbuf_tabstop_push(out, 18);
	prt_printf(out, "Size:\t");
	prt_units_u64(out, res.capacity << 9);
	prt_printf(out, "\nUsed:\t");
	prt_units_u64(out, res.used << 9);
	prt_printf(out, "\nOnline reserved:\t");
	prt_units_u64(out, res.online_reserved << 9);
	prt_newline(out);
	printbuf_tabstops_reset(out);

	/* Replicas summary */
	struct durability_matrix replicated = { 0 };
	ec_configs ec = { 0 };
	u64 cached = 0, reserved = 0;

	darray_for_each(res.entries, i)
	{
		struct accounting_entry *e = *i;
		if (e->pos.type == BCH_DISK_ACCOUNTING_persistent_reserved) {
			reserved += e->counters[0];
		} else if (e->pos.type == BCH_DISK_ACCOUNTING_replicas) {
			if (e->pos.replicas.data_type == BCH_DATA_cached) {
				cached += e->counters[0];
				continue;
			}
			struct replicas_durability d = get_replicas_durability(
				e->pos.replicas.nr_devs,
				e->pos.replicas.nr_required,
				e->pos.replicas.devs, devs);
			if (e->pos.replicas.nr_required > 1)
				ec_config_add(&ec, e->pos.replicas.nr_required,
					      e->pos.replicas.nr_devs,
					      d.degraded, e->counters[0]);
			else
				durability_matrix_add(&replicated, d.durability,
						      d.degraded,
						      e->counters[0]);
		}
	}

	if (replicated.nr > 0 || ec.nr > 0) {
		prt_printf(out, "\nReplicated:\n");
		durability_matrix_to_text(out, replicated);
	}
	if (ec.nr > 0) {
		prt_printf(out, "\nErasure coded (data+parity):\n");
		ec_configs_to_text(out, &ec);
	}

	if (cached || reserved) {
		prt_newline(out);
		if (cached) {
			prt_printf(out, "cached:\t");
			prt_units_u64(out, cached << 9);
		}
		if (reserved) {
			prt_printf(out, "reserved:\t");
			prt_units_u64(out, reserved << 9);
		}
		prt_newline(out);
	}

	/* Detailed Replicas */
	if (cfg.fields_mask & (1 << FIELD_replicas)) {
		printbuf_tabstop_push(out, 12);
		printbuf_tabstop_push(out, 16);
		printbuf_tabstop_push(out, 12);
		printbuf_tabstop_push(out, 8);
		prt_printf(
			out,
			"\nData type\tRequired/total\tDurability\tUsage\tDevices\n");
		printbuf_indent_add(out, 2);
		darray_for_each(res.entries, i)
		{
			struct accounting_entry *e = *i;
			if (e->pos.type ==
			    BCH_DISK_ACCOUNTING_persistent_reserved) {
				if (!e->counters[0])
					continue;
				prt_printf(
					out, "reserved\t\t1/%u\t[]",
					e->pos.persistent_reserved.nr_replicas);
				prt_units_u64(out, e->counters[0] << 9);
				prt_newline(out);
			} else if (e->pos.type ==
				   BCH_DISK_ACCOUNTING_replicas) {
				if (!e->counters[0])
					continue;
				struct replicas_durability d =
					get_replicas_durability(
						e->pos.replicas.nr_devs,
						e->pos.replicas.nr_required,
						e->pos.replicas.devs, devs);
				bch2_prt_data_type(out,
						   e->pos.replicas.data_type);
				prt_printf(out, "\t%u/%u\t%u\t",
					   e->pos.replicas.nr_required,
					   e->pos.replicas.nr_devs,
					   d.durability);
				prt_units_u64(out, e->counters[0] << 9);
				prt_printf(out, "\t[");
				prt_dev_list(out, e->pos.replicas.nr_devs,
					     e->pos.replicas.devs, devs);
				prt_printf(out, "]");
				prt_newline(out);
			}
		}
		printbuf_indent_sub(out, 2);
		printbuf_tabstops_reset(out);
	}

	/* Compression */
	if (cfg.fields_mask & (1 << FIELD_compression)) {
		bool first = true;
		darray_for_each(res.entries, i)
		{
			struct accounting_entry *e = *i;
			if (e->pos.type != BCH_DISK_ACCOUNTING_compression)
				continue;

			if (first) {
				prt_printf(out, "\nCompression:\n");
				printbuf_indent_add(out, 2);
				printbuf_tabstop_push(out, 15);
				printbuf_tabstop_push(out, 1);
				printbuf_tabstop_push(out, 12);
				printbuf_tabstop_push(out, 14);
				printbuf_tabstop_push(out, 20);
				prt_printf(
					out,
					"type\t\tcompressed\runcompressed\raverage_extent_size\r\n");
				first = false;
			}
			bch2_prt_compression_type(out, e->pos.compression.type);
			prt_tab(out);
			prt_tab(out);
			prt_units_u64(out, e->counters[2] << 9);
			prt_tab_rjust(out);
			prt_units_u64(out, e->counters[1] << 9);
			prt_tab_rjust(out);
			u64 avg = e->counters[0] ? (e->counters[1] << 9) /
							   e->counters[0] :
							 0;
			prt_units_u64(out, avg);
			prt_tab_rjust(out);
			prt_newline(out);
		}
		if (!first) {
			printbuf_tabstops_reset(out);
			printbuf_indent_sub(out, 2);
		}
	}

	/* Btree usage */
	if (cfg.fields_mask & (1 << FIELD_btree)) {
		bool first = true;
		darray_for_each(res.entries, i)
		{
			struct accounting_entry *e = *i;
			if (e->pos.type != BCH_DISK_ACCOUNTING_btree)
				continue;

			if (first) {
				prt_printf(out, "\nBtree usage:");
				printbuf_indent_add(out, 2);
				printbuf_tabstop_push(out, 22);
				printbuf_tabstop_push(out, 8);
				first = false;
				prt_newline(out);
			}
			prt_printf(out, "%s\t",
				   bch2_btree_id_str(e->pos.btree.id));
			prt_units_u64(out, e->counters[0] << 9);
			prt_tab_rjust(out);
			prt_newline(out);
		}
		if (!first) {
			printbuf_tabstops_reset(out);
			printbuf_indent_sub(out, 2);
		}
	}

	/* Rebalance / reconcile work */
	if (cfg.fields_mask & (1 << FIELD_rebalance_work)) {
		bool first = true;
		darray_for_each(res.entries, i)
		{
			struct accounting_entry *e = *i;

			if (e->pos.type == BCH_DISK_ACCOUNTING_rebalance_work) {
				prt_printf(out, "\nPending rebalance work:\n");
				prt_units_u64(out, e->counters[0] << 9);
				prt_newline(out);
		} else if (e->pos.type ==
			   BCH_DISK_ACCOUNTING_reconcile_work) {
			if (first) {
				printbuf_indent_add(out, 2);
				printbuf_tabstop_push(out, 10);
				printbuf_tabstop_push(out, 8);
				printbuf_tabstop_push(out, 8);
				prt_printf(
					out,
					"\nPending reconcile:\tdata\tmetadata\n");
				first = false;
			}
				bch2_prt_reconcile_accounting_type(
					out, e->pos.reconcile_work.type);
				prt_tab(out);
				prt_units_u64(out, e->counters[0] << 9);
				prt_tab_rjust(out);
				prt_units_u64(out, e->counters[1] << 9);
				prt_tab_rjust(out);
				prt_newline(out);
			}
		}
		if (!first) {
			printbuf_tabstops_reset(out);
			printbuf_indent_sub(out, 2);
		}
	}

	for (unsigned i = 0; i < replicated.nr; i++)
		darray_exit(&replicated.data[i]);
	free(replicated.data);
	darray_for_each(ec, i) darray_exit(&i->degraded);
	darray_exit(&ec);
	bchu_accounting_result_free(&res);
}

static u64 dev_used_buckets(const struct bch_ioctl_dev_usage_v2 *u)
{
	u64 used = 0;
	for (unsigned i = 0; i < u->nr_data_types && i < BCH_DATA_NR; i++)
		if (!data_type_is_empty(i) && !data_type_is_hidden(i))
			used += u->d[i].buckets;
	return used;
}

static u64 dev_used_sectors(const struct bch_ioctl_dev_usage_v2 *u)
{
	u64 used = 0;
	for (unsigned i = 0; i < u->nr_data_types && i < BCH_DATA_NR; i++)
		if (i != BCH_DATA_unstriped)
			used += u->d[i].sectors;
	return used;
}

static u64 dev_hidden_sectors(const struct bch_ioctl_dev_usage_v2 *u)
{
	u64 hidden = 0;
	for (unsigned i = 0; i < u->nr_data_types && i < BCH_DATA_NR; i++)
		if (data_type_is_hidden(i))
			hidden += u->d[i].sectors;
	return hidden;
}

static int dev_name_cmp(const void *_a, const void *_b)
{
	const struct dev_name *a = _a, *b = _b;
	int r;

	r = strcmp(a->label ?: "", b->label ?: "");
	if (r)
		return r;
	r = strcmp(a->dev ?: "", b->dev ?: "");
	if (r)
		return r;
	return (int)a->idx - (int)b->idx;
}

static void devs_usage_to_text(struct printbuf *out, struct bchfs_handle fs,
			       dev_names devs, struct fs_usage_config cfg)
{
	bool full = cfg.fields_mask & (1 << FIELD_devices);

	u32 accounting_types = (1 << BCH_DISK_ACCOUNTING_dev_leaving);
	struct accounting_result res =
		bchu_fs_accounting_query(fs, accounting_types);

	printbuf_tabstop_push(out, 20);
	printbuf_tabstop_push(out, 8);
	printbuf_tabstop_push(out, 10);
	printbuf_tabstop_push(out, 12);
	prt_newline(out);

	if (!full) {
		prt_printf(out, "Device label\tDevice\tState\tSize\rUsed\rUse%%\r");
		prt_newline(out);
	}

	qsort(devs.data, devs.nr, sizeof(devs.data[0]), dev_name_cmp);

	darray_for_each(devs, d)
	{
		u64 leaving = 0;

		darray_for_each(res.entries, i)
		{
			struct accounting_entry *e = *i;
			if (e->pos.type == BCH_DISK_ACCOUNTING_dev_leaving &&
			    e->pos.dev_leaving.dev == d->idx) {
				leaving = e->counters[0];
				break;
			}
		}

		if (!d->online) {
			if (full)
				prt_printf(out,
					   "%s (device %u):\t%s\toffline\tusage unavailable\n",
					   d->label ?: "(no label)", d->idx,
					   d->dev);
			else
				prt_printf(out,
					   "%s (device %u):\t%s\toffline\t-\t-\t-\n",
					   d->label ?: "(no label)", d->idx,
					   d->dev);
			continue;
		}

		struct bch_ioctl_dev_usage_v2 *u = bchu_dev_usage(fs, d->idx);
		u64 used_buckets = dev_used_buckets(u);

		if (full) {
			prt_printf(out, "%s (device %u): %s %s %llu%%\n",
				   d->label ?: "(no label)", d->idx, d->dev,
				   bch2_member_states[u->state],
				   u->nr_buckets ? (used_buckets * 100 /
						    u->nr_buckets) :
							 0);
			prt_printf(out, "\tdata\rbuckets\rfragmented\r\n");
			printbuf_indent_add(out, 2);
			for (unsigned i = 0; i < u->nr_data_types; i++) {
				bch2_prt_data_type(out, i);
				prt_tab(out);
				u64 sectors = (data_type_is_empty(i) ||
					       data_type_is_hidden(i)) ?
						      u->d[i].buckets *
							      u->bucket_size :
							    u->d[i].sectors;
				prt_units_u64(out, sectors << 9);
				prt_printf(out, "\r%llu\r", u->d[i].buckets);
				if (u->d[i].fragmented)
					prt_units_u64(out,
						      u->d[i].fragmented << 9);
				prt_tab_rjust(out);
				prt_newline(out);
			}
			prt_printf(out, "capacity\t");
			prt_units_u64(out, (u->nr_buckets * u->bucket_size)
						   << 9);
			prt_printf(out, "\r%llu\r\n", u->nr_buckets);
			prt_printf(out, "bucket size\t");
			prt_units_u64(out, u->bucket_size << 9);
			prt_tab_rjust(out);
			printbuf_indent_sub(out, 2);
			prt_newline(out);
			prt_newline(out);
		} else {
			u64 hidden = dev_hidden_sectors(u);
			u64 capacity =
				(u->nr_buckets * u->bucket_size) - hidden;
			u64 used = dev_used_sectors(u) - hidden;

			prt_printf(out, "%s (device %u): %s %s ",
				   d->label ?: "(no label)", d->idx, d->dev,
				   bch2_member_states[u->state]);
			prt_units_u64(out, capacity << 9);
			prt_str(out, " ");
			prt_units_u64(out, used << 9);
			prt_printf(out, " %llu%% ",
				   u->nr_buckets ? (used_buckets * 100 /
						    u->nr_buckets) :
							 0);
			prt_newline(out);
		}
		if (leaving) {
			printbuf_indent_add(out, 2);
			prt_printf(out, "leaving:\t");
			prt_units_u64(out, leaving << 9);
			prt_newline(out);
			printbuf_indent_sub(out, 2);
		}
		free(u);
	}
	printbuf_tabstops_reset(out);
	bchu_accounting_result_free(&res);
}

static void fs_usage_usage(void)
{
	puts("bcachefs fs usage - display detailed filesystem usage\n"
	     "Usage: bcachefs fs usage [OPTION]... <mountpoint>\n"
	     "\n"
	     "Options:\n"
	     "  -f, --fields=FIELDS               Comma-separated list of fields to show\n"
	     "                                    (replicas, btree, compression, rebalance_work, devices)\n"
	     "  -a, --all                         Print all accounting fields\n"
	     "  -h, --human-readable              Human readable units\n"
	     "      --mapper-names                Show mapper names for dm-multipath devices\n"
	     "  -H, --help                        Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_fs_usage(int argc, char *argv[])
{
	enum { OPT_MAPPER_NAMES = 1000 };
	static const struct option longopts[] = {
		{ "fields", required_argument, NULL, 'f' },
		{ "all", no_argument, NULL, 'a' },
		{ "human-readable", no_argument, NULL, 'h' },
		{ "mapper-names", no_argument, NULL, OPT_MAPPER_NAMES },
		{ "help", no_argument, NULL, 'H' },
		{ NULL }
	};
	struct fs_usage_config cfg = { .fields_mask =
					       (1 << FIELD_rebalance_work),
				       .human_readable = false,
				       .name_mode = DEVICE_NAME_RAW };
	int opt;

	while ((opt = getopt_long(argc, argv, "f:ahH", longopts, NULL)) != -1)
		switch (opt) {
		case 'f':
			cfg.fields_mask = read_flag_list_or_die(
				optarg, usage_field_strs, "usage field");
			break;
		case 'a':
			cfg.fields_mask = (1 << FIELD_NR) - 1;
			break;
		case 'h':
			cfg.human_readable = true;
			break;
		case OPT_MAPPER_NAMES:
			cfg.name_mode = DEVICE_NAME_MAPPER;
			break;
		case 'H':
			fs_usage_usage();
			exit(EXIT_SUCCESS);
		default:
			fs_usage_usage();
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	struct printbuf buf = PRINTBUF;
	buf.human_readable_units = cfg.human_readable;

	if (!argc) {
		struct bchfs_handle fs = bcache_fs_open(".");
		dev_names devs = bchu_fs_get_devices_mode(fs, cfg.name_mode);
		fs_usage_v1_to_text(&buf, fs, &devs, cfg);
		devs_usage_to_text(&buf, fs, devs, cfg);
		dev_names_free(&devs);
		bcache_fs_close(fs);
		printf("%s", buf.buf);
	} else {
		char *path;
		while ((path = arg_pop())) {
			printbuf_reset(&buf);
			struct bchfs_handle fs = bcache_fs_open(path);
			dev_names devs =
				bchu_fs_get_devices_mode(fs, cfg.name_mode);
			fs_usage_v1_to_text(&buf, fs, &devs, cfg);
			devs_usage_to_text(&buf, fs, devs, cfg);
			dev_names_free(&devs);
			bcache_fs_close(fs);
			printf("%s", buf.buf);
		}
	}

	printbuf_exit(&buf);
	return 0;
}
