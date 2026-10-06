#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libbcachefs.h"
#include "init/fs.h"
#include "journal/read.h"
#include "cmds.h"

static void journal_rewind_info_usage(void)
{
	puts("bcachefs journal-rewind-info - Show journal rewind candidates\n"
	     "Usage: bcachefs journal-rewind-info [OPTION]... <devices>\n"
	     "\n"
	     "Options:\n"
	     "  -o mount_options        Additional mount options\n"
	     "  -n nr                   Maximum number of flush candidates to list (0 = unlimited)\n"
	     "  -v                      Verbose mount output\n"
	     "  -h, --help              Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static u64 entry_payload_le64(const struct jset_entry *entry)
{
	if (entry->type == BCH_JSET_ENTRY_datetime) {
		struct jset_entry_datetime *dt =
			container_of(entry, struct jset_entry_datetime, entry);
		return le64_to_cpu(dt->seconds);
	} else if (entry->type == BCH_JSET_ENTRY_rewind_limit) {
		struct jset_entry_rewind_limit *r =
			container_of(entry, struct jset_entry_rewind_limit, entry);
		return le64_to_cpu(r->seq);
	}
	return 0;
}

static void fmt_secs(struct printbuf *out, u64 secs)
{
	time_t t = secs;
	struct tm tm;
	char buf[64];

	if (!gmtime_r(&t, &tm)) {
		prt_printf(out, "(invalid: %llu)", (unsigned long long)secs);
		return;
	}

	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
	prt_str(out, buf);
}

int cmd_journal_rewind_info(int argc, char *argv[])
{
	struct printbuf opts_str = PRINTBUF;
	int nr_to_list = 0;
	bool verbose = false;
	int opt;

	while ((opt = getopt(argc, argv, "o:n:vh")) != -1)
		switch (opt) {
		case 'o':
			if (opts_str.pos)
				prt_char(&opts_str, ',');
			prt_str(&opts_str, optarg);
			break;
		case 'n':
			nr_to_list = atoi(optarg);
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			journal_rewind_info_usage();
			exit(EXIT_SUCCESS);
		default:
			journal_rewind_info_usage();
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (!argc)
		die("Please supply device(s) to open");

	darray_const_str devices = { 0 };
	for (int i = 0; i < argc; i++)
		darray_push(&devices, argv[i]);

	struct bch_opts opts = bch2_opts_empty();
	struct printbuf err = PRINTBUF;

	if (bch2_parse_mount_opts(NULL, &opts, &err, opts_str.buf, false))
		die("error parsing options: %s", err.buf);

	opt_set(opts, noexcl, true);
	opt_set(opts, nochanges, true);
	opt_set(opts, norecovery, true);
	opt_set(opts, read_only, true);
	opt_set(opts, degraded, BCH_DEGRADED_very);
	opt_set(opts, errors, BCH_ON_ERROR_continue);
	opt_set(opts, fix_errors, FSCK_FIX_yes);
	opt_set(opts, retain_recovery_info, true);
	opt_set(opts, read_journal_only, true);
	opt_set(opts, read_entire_journal, true);
	if (verbose)
		opt_set(opts, verbose, true);

	struct bch_fs *c = bch2_fs_open(&devices, &opts, NULL);
	if (IS_ERR(c))
		die("error opening %s: %s", devices.data[0], bch2_err_str(PTR_ERR(c)));

	struct journal_replay *p, **_p;
	struct genradix_iter iter;
	u64 latest_seq = 0;
	struct journal_replay *latest_p = NULL;
	size_t nr_entries = 0;

	genradix_for_each(&c->journal_entries, iter, _p) {
		p = *_p;
		if (!p)
			continue;

		u64 seq = le64_to_cpu(p->j.seq);
		if (!latest_p || seq > latest_seq) {
			latest_seq = seq;
			latest_p = p;
		}
		nr_entries++;
	}

	if (!latest_p)
		die("no journal entries found");

	u64 floor_seq = 0;
	bool fell_back = true;

	vstruct_for_each(&latest_p->j, e) {
		if (e->type == BCH_JSET_ENTRY_rewind_limit) {
			floor_seq = entry_payload_le64(e);
			fell_back = false;
			break;
		}
	}

	if (fell_back) {
		floor_seq = latest_seq;
		genradix_for_each(&c->journal_entries, iter, _p) {
			p = *_p;
			if (!p)
				continue;
			u64 seq = le64_to_cpu(p->j.seq);
			if (seq < floor_seq)
				floor_seq = seq;
		}
	}

	struct printbuf out = PRINTBUF;

	if (fell_back) {
		prt_printf(&out,
			"warning: most recent journal entry has no rewind_limit sub-entry;\n"
			"         falling back to lowest seq present on disk.\n");
	}

	prt_printf(&out, "rewind limit:  seq %llu  (oldest safe)\n", (unsigned long long)floor_seq);
	prt_printf(&out, "newest:        seq %llu", (unsigned long long)latest_seq);

	vstruct_for_each(&latest_p->j, e) {
		if (e->type == BCH_JSET_ENTRY_datetime) {
			prt_str(&out, "  ");
			fmt_secs(&out, entry_payload_le64(e));
			break;
		}
	}
	prt_newline(&out);
	prt_newline(&out);

	struct candidate {
		u64 seq;
		u64 datetime;
		bool has_datetime;
	} *candidates = calloc(nr_entries, sizeof(*candidates));
	size_t nr_candidates = 0;
	size_t total_entries_in_window = 0;

	genradix_for_each(&c->journal_entries, iter, _p) {
		p = *_p;
		if (!p)
			continue;

		u64 s = le64_to_cpu(p->j.seq);

		if (s < floor_seq || s > latest_seq)
			continue;

		total_entries_in_window++;

		if (JSET_NO_FLUSH(&p->j))
			continue;

		candidates[nr_candidates].seq = s;
		vstruct_for_each(&p->j, e) {
			if (e->type == BCH_JSET_ENTRY_datetime) {
				candidates[nr_candidates].datetime = entry_payload_le64(e);
				candidates[nr_candidates].has_datetime = true;
				break;
			}
		}
		nr_candidates++;
	}

	// Sort candidates by seq
	for (size_t i = 0; i < nr_candidates; i++) {
		for (size_t j = i + 1; j < nr_candidates; j++) {
			if (candidates[i].seq > candidates[j].seq) {
				struct candidate tmp = candidates[i];
				candidates[i] = candidates[j];
				candidates[j] = tmp;
			}
		}
	}

	prt_printf(&out, "rewind candidates (flush entries):\n");
	if (nr_candidates == 0) {
		prt_printf(&out, "  (none — window contains no flush entries)\n");
	} else {
		size_t limit = (nr_to_list == 0) ? nr_candidates : 
			(nr_to_list < nr_candidates ? nr_to_list : nr_candidates);

		if (nr_candidates > limit) {
			prt_printf(&out, "  (%zu earlier candidates omitted; use -n 0 to see all)\n",
				 nr_candidates - limit);
		}

		for (size_t i = nr_candidates - limit; i < nr_candidates; i++) {
			prt_printf(&out, "  seq %-12llu  ", (unsigned long long)candidates[i].seq);
			if (candidates[i].has_datetime)
				fmt_secs(&out, candidates[i].datetime);
			else
				prt_str(&out, "(no datetime)");
			prt_newline(&out);
		}
	}

	prt_newline(&out);
	prt_printf(&out, "  %zu flush candidates across %zu entries in window\n",
		 nr_candidates, total_entries_in_window);

	printf("%s", out.buf);

	printbuf_exit(&out);
	free(candidates);
	bch2_fs_stop(c);
	darray_exit(&devices);
	printbuf_exit(&opts_str);
	printbuf_exit(&err);

	return 0;
}
