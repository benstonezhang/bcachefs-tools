/*
 * list_journal: List filesystem journal entries.
 *
 * Supports filtering entries by sequence range, btree, transaction function,
 * or key range. Includes real-time progress reporting and optional
 * sanitization for privacy.
 *
 * Ported from src/commands/list_journal.rs.
 *
 * GPLv2
 */

#include <fcntl.h>
#include <getopt.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>

#include <linux/fs_parser.h>

#include "libbcachefs.h"
#include "btree/iter.h"
#include "init/error.h"
#include "init/fs.h"
#include "journal/read.h"
#include "journal/seq_blacklist.h"
#include "journal/validate.h"
#include "cmds.h"

static const char *NORMAL = "\x1B[0m";
static const char *RED = "\x1B[31m";

static void star_start_of_lines(char *buf)
{
	char *p = buf;

	if (*p == ' ')
		*p = '*';

	while ((p = strstr(p, "\n ")))
		p[1] = '*';
}

static inline bool entry_is_transaction_start(struct jset_entry *entry)
{
	return entry->type == BCH_JSET_ENTRY_log && !entry->level;
}

static inline bool entry_log_str_eq(struct jset_entry *entry, const char *s)
{
	struct jset_entry_log *l =
		container_of(entry, struct jset_entry_log, entry);
	unsigned b = jset_entry_log_msg_bytes(l);
	return b == strlen(s) && !strncmp(s, (char *)l->d, b);
}

static inline bool entry_is_log_msg(struct jset_entry *entry)
{
	if (entry->type != BCH_JSET_ENTRY_log || !entry->level)
		return false;

	// Filter out internal subsystem markers
	if (entry_log_str_eq(entry, "rebalance") ||
	    entry_log_str_eq(entry, "reconcile") ||
	    entry_log_str_eq(entry, "copygc") ||
	    entry_log_str_eq(entry, "promote"))
		return false;

	return true;
}

static inline bool entry_is_print_key(struct jset_entry *entry)
{
	switch (entry->type) {
	case BCH_JSET_ENTRY_btree_root:
	case BCH_JSET_ENTRY_btree_keys:
	case BCH_JSET_ENTRY_write_buffer_keys:
	case BCH_JSET_ENTRY_overwrite:
		return true;
	default:
		return false;
	}
}

static inline bool entry_is_non_transaction(struct jset_entry *entry)
{
	switch (entry->type) {
	case BCH_JSET_ENTRY_btree_root:
	case BCH_JSET_ENTRY_datetime:
	case BCH_JSET_ENTRY_usage:
	case BCH_JSET_ENTRY_clock:
		return true;
	default:
		return false;
	}
}

struct msg_filter_entry {
	int sign;
	char *msg;
};

typedef struct {
	DARRAY(struct msg_filter_entry) f;
} transaction_msg_filter;

struct key_filter_entry {
	int sign;
	struct bbpos_range range;
};

typedef struct {
	DARRAY(struct key_filter_entry) f;
} transaction_key_filter;

typedef struct {
	bool blacklisted;
	bool flush_only;
	bool datetime_only;
	bool headers_only;
	bool all_headers;
	bool log;
	bool log_only;
	bool print_offset;
	bool filtering;
	u64 btree_filter;
	transaction_msg_filter transaction;
	transaction_key_filter key;
	bool bkey_val;
} journal_filter;

static int parse_sign(char **str)
{
	if (**str == '+') {
		(*str)++;
		return 1;
	}

	if (**str == '-') {
		(*str)++;
		return -1;
	}

	return 0;
}

static bool entry_matches_btree_filter(journal_filter f,
				       struct jset_entry *entry)
{
	return f.btree_filter == ~0ULL ||
	       (entry->level == 0 && entry->type != BCH_JSET_ENTRY_btree_root &&
		BIT_ULL(entry->btree_id) & f.btree_filter);
}

static bool transaction_matches_btree_filter(journal_filter f,
					     struct jset_entry *entry,
					     struct jset_entry *end)
{
	for (entry = vstruct_next(entry); entry < end;
	     entry = vstruct_next(entry))
		if (entry_is_print_key(entry) &&
		    entry_matches_btree_filter(f, entry))
			return true;

	return false;
}

static bool bkey_matches_range(struct jset_entry *entry, struct bkey_i *k,
				const struct bbpos_range *range)
{
	struct bbpos k_start =
		BBPOS(entry->btree_id, bkey_start_pos(&k->k));
	struct bbpos k_end = BBPOS(entry->btree_id, k->k.p);

	if (!range->start.pos.snapshot && !range->end.pos.snapshot) {
		k_start.pos.snapshot = 0;
		k_end.pos.snapshot = 0;
	}

	/* Always point comparison for journal listing */
	k_start = k_end;

	return bbpos_cmp(k_start, range->start) >= 0 &&
	       bbpos_cmp(k_end, range->end) <= 0;
}

static bool entry_matches_range(struct jset_entry *entry,
				const struct bbpos_range *range)
{
	if (!entry->level && (entry->type == BCH_JSET_ENTRY_btree_keys ||
			      entry->type == BCH_JSET_ENTRY_overwrite))
		jset_entry_for_each_key(entry, k)
		{
			if (!k->k.u64s)
				break;

			if (bkey_matches_range(entry, k, range))
				return true;
		}
	return false;
}

static bool transaction_matches_transaction_filter(transaction_key_filter f,
						   struct jset_entry *entry,
						   struct jset_entry *end)
{
	bool has_positive = false, has_negative = false;
	darray_for_each(f.f, i) {
		if (i->sign < 0)
			has_negative = true;
		else
			has_positive = true;
	}

	if (has_negative) {
		darray_for_each(f.f, i) {
			if (i->sign < 0) {
				for (struct jset_entry *e = vstruct_next(entry); e < end; e = vstruct_next(e)) {
					if (entry_matches_range(e, &i->range))
						return false;
				}
			}
		}
	}

	if (has_positive) {
		darray_for_each(f.f, i) {
			if (i->sign >= 0) {
				for (struct jset_entry *e = vstruct_next(entry); e < end; e = vstruct_next(e)) {
					if (entry_matches_range(e, &i->range))
						return true;
				}
			}
		}
		return false;
	}

	return true;
}

static bool entry_matches_msg_filter(transaction_msg_filter f,
				     struct jset_entry *entry)
{
	bool has_positive = false, has_negative = false;
	darray_for_each(f.f, i) {
		if (i->sign < 0)
			has_negative = true;
		else
			has_positive = true;
	}

	if (has_negative) {
		darray_for_each(f.f, i) {
			if (i->sign < 0 && entry_log_str_eq(entry, i->msg))
				return false;
		}
	}

	if (has_positive) {
		darray_for_each(f.f, i) {
			if (i->sign >= 0 && entry_log_str_eq(entry, i->msg))
				return true;
		}
		return false;
	}

	return true;
}

static bool entry_is_log_only(struct jset_entry *entry, struct jset_entry *end)
{
	bool have_log = false;

	for (entry = vstruct_next(entry); entry < end;
	     entry = vstruct_next(entry)) {
		if (entry->u64s && !entry_is_log_msg(entry))
			return false;
		have_log = true;
	}

	return have_log;
}

static bool entry_has_log(struct jset_entry *entry, struct jset_entry *end)
{
	for (entry = vstruct_next(entry); entry < end;
	     entry = vstruct_next(entry))
		if (entry_is_log_msg(entry))
			return true;

	return false;
}

static struct jset_entry *transaction_end(struct jset_entry *entry,
					  struct jset_entry *end)
{
	do
		entry = vstruct_next(entry);
	while (entry < end && !entry_is_transaction_start(entry) &&
	       !entry_is_non_transaction(entry));

	return entry;
}

static bool should_print_transaction(journal_filter f, struct jset_entry *entry,
				     struct jset_entry *end)
{
	BUG_ON(entry->type != BCH_JSET_ENTRY_log);

	if (f.log && entry_is_log_only(entry, end))
		return true;

	if (f.log_only && !entry_has_log(entry, end))
		return false;

	if (f.btree_filter != ~0ULL &&
	    !transaction_matches_btree_filter(f, entry, end))
		return false;

	if (f.transaction.f.nr &&
	    !entry_matches_msg_filter(f.transaction, entry))
		return false;

	if (f.key.f.nr &&
	    !transaction_matches_transaction_filter(f.key, entry, end))
		return false;

	return true;
}

static void journal_entry_header_to_text(struct printbuf *out, struct bch_fs *c,
					 struct journal_replay *p,
					 bool blacklisted)
{
	prt_printf(out,
		   "\n%s"
		   "journal entry     %llu\n"
		   "  bytes           %zu\n"
		   "  sectors         %lu\n"
		   "  version         %u\n"
		   "  last seq        %llu\n"
		   "  flush           %u\n"
		   "  written at      ",
		   blacklisted ? "blacklisted " : "", le64_to_cpu(p->j.seq),
		   vstruct_bytes(&p->j), vstruct_sectors(&p->j, c->block_bits),
		   le32_to_cpu(p->j.version), le64_to_cpu(p->j.last_seq),
		   !JSET_NO_FLUSH(&p->j));
	bch2_journal_ptrs_to_text(out, c, p);
	prt_newline(out);
}

static void bch2_journal_entry_keys_noval_to_text(struct printbuf *out,
						  struct jset_entry *entry)
{
	jset_entry_for_each_key(entry, k)
	{
		if (!k->k.u64s)
			break;

		bch2_prt_jset_entry_type(out, entry->type);
		prt_str(out, ": ");
		bch2_btree_id_level_to_text(out, entry->btree_id, entry->level);
		prt_char(out, ' ');
		bch2_bkey_to_text(out, &k->k);
		prt_newline(out);
	}
}

static unsigned journal_entry_indent(struct jset_entry *entry)
{
	if (entry_is_transaction_start(entry) ||
	    entry->type == BCH_JSET_ENTRY_btree_root ||
	    entry->type == BCH_JSET_ENTRY_datetime ||
	    entry->type == BCH_JSET_ENTRY_usage)
		return 2;
	return 4;
}

static void print_one_entry(struct printbuf *out, struct bch_fs *c,
			    journal_filter f, struct journal_replay *p,
			    struct jset_entry *entry)
{
	if (entry_is_print_key(entry) && !entry->u64s)
		return;

	if (entry_is_print_key(entry) && !entry_matches_btree_filter(f, entry))
		return;

	bool highlight = false;
	darray_for_each(f.key.f, i) {
		if (entry_matches_range(entry, &i->range)) {
			highlight = true;
			break;
		}
	}
	if (highlight)
		prt_str(out, RED);

	unsigned indent = journal_entry_indent(entry);
	printbuf_indent_add(out, indent);

	if (f.print_offset)
		prt_printf(out, "%4lu ",
			   (unsigned long)(entry->_data - p->j._data));

	if (!f.bkey_val && entry_is_print_key(entry))
		bch2_journal_entry_keys_noval_to_text(out, entry);
	else {
		bch2_journal_entry_to_text(out, c, entry);
		prt_newline(out);
	}

	printbuf_indent_sub(out, indent);

	if (highlight)
		prt_str(out, NORMAL);
}

static void journal_replay_print(struct bch_fs *c, journal_filter f,
				 struct journal_replay *p)
{
	struct printbuf buf = PRINTBUF;
	bool blacklisted = p->ignore_blacklisted ||
			   bch2_journal_seq_is_blacklisted(
				   c, le64_to_cpu(p->j.seq), false);
	bool printed_header = false;

	if (f.datetime_only) {
		prt_printf(&buf,
			   "%s"
			   "journal entry     %-8llu ",
			   blacklisted ? "blacklisted " : "",
			   le64_to_cpu(p->j.seq));
		vstruct_for_each(&p->j, entry) if (entry->type ==
						   BCH_JSET_ENTRY_datetime)
		{
			bch2_journal_entry_to_text(&buf, c, entry);
			break;
		}

		prt_newline(&buf);
		goto print;
	}

	if (!f.filtering) {
		journal_entry_header_to_text(&buf, c, p, blacklisted);
		printed_header = true;

		if (!f.headers_only)
			vstruct_for_each(&p->j, entry)
				print_one_entry(&buf, c, f, p, entry);
	} else {
		if (f.all_headers) {
			journal_entry_header_to_text(&buf, c, p, blacklisted);
			printed_header = true;
		}

		struct jset_entry *entry = p->j.start;
		struct jset_entry *end = vstruct_last(&p->j);

		while (entry < end) {
			if (!entry_is_transaction_start(entry)) {
				entry = vstruct_next(entry);
				continue;
			}

			struct jset_entry *t_end = transaction_end(entry, end);

			if (should_print_transaction(f, entry, t_end)) {
				if (!printed_header) {
					journal_entry_header_to_text(
						&buf, c, p, blacklisted);
					printed_header = true;
				}

				for (; entry < t_end;
				     entry = vstruct_next(entry))
					print_one_entry(&buf, c, f, p, entry);
			} else {
				entry = t_end;
			}
		}
	}
print:
	if (buf.buf) {
		if (blacklisted)
			star_start_of_lines(buf.buf);
		fwrite(buf.buf, 1, buf.pos, stdout);
	}
	printbuf_exit(&buf);
}

static void list_journal_usage(void)
{
	puts("bcachefs list_journal - print contents of journal\n"
	     "Usage: bcachefs list_journal [OPTION]... <devices>\n"
	     "\n"
	     "Options:\n"
	     "  -a, --all                        Read entire journal, not just contiguous entries\n"
	     "  -d, --dirty-only                 Only read dirty entries\n"
	     "  -n, --nr-entries=nr              Number of journal entries to print\n"
	     "  -s, --seq=seq                    Sequence number or range (seq or seq..seq)\n"
	     "  -B, --blacklisted                Include blacklisted entries\n"
	     "  -F, --flush-only                 Only print flush entries/commits\n"
	     "  -D, --datetime                   Print datetime entries only\n"
	     "  -H, --headers-only               Headers only\n"
	     "      --all-headers                Print all headers even if no transactions matched\n"
	     "  -l, --log                        When filtering, include log-only entries\n"
	     "  -L, --log-only                   Only print transactions containing log messages\n"
	     "  -O, --offset                     Print offset of each subentry\n"
	     "  -o, --opt=option                 Additional mount options\n"
	     "  -b, --btree=(+|-)btree1,btree2   Filter keys matching or not updating btree(s)\n"
	     "  -t, --transaction=(+|-)fn1,fn2   Filter transactions matching or not matching fn(s)\n"
	     "  -k, --key=(+|-)bbpos1,bbpos2     Filter transactions updating bbpos\n"
	     "  -V, --bkey-val=(true|false)      Print bkey values (default: true)\n"
	     "  -v, --verbose                    Verbose mode\n"
	     "  -h, --help                       Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_list_journal(int argc, char *argv[])
{
	enum {
		OPT_ALL_HEADERS = 256,
	};
	static const struct option longopts[] = {
		{ "all", no_argument, NULL, 'a' },
		{ "dirty-only", no_argument, NULL, 'd' },
		{ "nr-entries", required_argument, NULL, 'n' },
		{ "seq", required_argument, NULL, 's' },
		{ "blacklisted", no_argument, NULL, 'B' },
		{ "flush-only", no_argument, NULL, 'F' },
		{ "datetime", no_argument, NULL, 'D' },
		{ "headers-only", no_argument, NULL, 'H' },
		{ "all-headers", no_argument, NULL, OPT_ALL_HEADERS },
		{ "log", no_argument, NULL, 'l' },
		{ "log-only", no_argument, NULL, 'L' },
		{ "offset", no_argument, NULL, 'O' },
		{ "opt", required_argument, NULL, 'o' },
		{ "btree", required_argument, NULL, 'b' },
		{ "transaction", required_argument, NULL, 't' },
		{ "key", required_argument, NULL, 'k' },
		{ "bkey-val", required_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	struct bch_opts opts = bch2_opts_empty();
	u32 nr_entries = 0;
	u64 seq_start = 0, seq_end = U64_MAX;
	journal_filter f = { .btree_filter = ~0ULL, .bkey_val = true };
	bool contiguous_only = true;
	char *t;
	struct printbuf opts_str = PRINTBUF;
	int opt, ret;

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

	while ((opt = getopt_long(argc, argv, "adn:s:BFMDHLo:Ob:t:k:V:vh",
				  longopts, NULL)) != -1)
		switch (opt) {
		case 'a':
			contiguous_only = false;
			break;
		case 'd':
			opt_set(opts, read_entire_journal, false);
			break;
		case 'n':
			if (kstrtouint(optarg, 10, &nr_entries))
				die("error parsing nr_entries");
			opt_set(opts, read_entire_journal, true);
			break;
		case 's':
			t = optarg;
			char *start_s = strsep(&t, "..");
			char *end_s = t;

			if (start_s && *start_s)
				if (kstrtoull(start_s, 10, &seq_start))
					die("error parsing seq start");
			if (end_s && *end_s)
				if (kstrtoull(end_s, 10, &seq_end))
					die("error parsing seq end");
			if (!end_s)
				seq_end = seq_start;
			if (seq_start > seq_end)
				swap(seq_start, seq_end);

			contiguous_only = false;
			opt_set(opts, read_entire_journal, true);
			break;
		case 'B':
			f.blacklisted = true;
			break;
		case 'F':
			f.flush_only = true;
			break;
		case 'D':
			f.datetime_only = true;
			break;
		case 'H':
			f.headers_only = true;
			break;
		case OPT_ALL_HEADERS:
			f.all_headers = true;
			break;
		case 'l':
			f.log = true;
			break;
		case 'L':
			f.log_only = true;
			f.filtering = true;
			break;
		case 'O':
			f.print_offset = true;
			break;
		case 'o':
			if (opts_str.pos)
				prt_char(&opts_str, ',');
			prt_str(&opts_str, optarg);
			break;
		case 'b':
			ret = parse_sign(&optarg);
			f.btree_filter = read_flag_list_or_die(
				optarg, __bch2_btree_ids, "btree id");
			if (ret < 0)
				f.btree_filter = ~f.btree_filter;
			f.filtering = true;
			break;
		case 't': {
			int sign = parse_sign(&optarg);
			while ((t = strsep(&optarg, ",")))
				darray_push(&f.transaction.f, ((struct msg_filter_entry){
					.sign = sign,
					.msg = strdup(t),
				}));
			f.filtering = true;
			break;
		}
		case 'k': {
			int sign = parse_sign(&optarg);
			while ((t = strsep(&optarg, ",")))
				darray_push(&f.key.f, ((struct key_filter_entry){
					.sign = sign,
					.range = bbpos_range_parse(t),
				}));
			f.filtering = true;
			break;
		}
		case 'V':
			ret = lookup_constant(bool_names, optarg, -EINVAL);
			if (ret < 0)
				die("error parsing %s", optarg);
			f.bkey_val = ret;
			break;
		case 'v':
			opt_set(opts, verbose, true);
			break;
		case 'h':
			list_journal_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (opts_str.pos) {
		struct printbuf err = PRINTBUF;
		if (bch2_parse_mount_opts(NULL, &opts, &err, opts_str.buf, false))
			die("error parsing options: %s", err.buf);
		printbuf_exit(&err);
	}
	printbuf_exit(&opts_str);

	if (!argc)
		die("Please supply device(s) to open");

	darray_const_str devs = get_or_split_cmdline_devs(argc, argv);

	struct bch_fs *c = bch2_fs_open(&devs, &opts, NULL);
	if (IS_ERR(c))
		die("error opening devices: %s", bch2_err_str(PTR_ERR(c)));

	struct journal_replay *p, **_p;
	struct genradix_iter iter;
	u64 min_seq_to_print = 0;

	if (contiguous_only) {
		u64 seq = 0;
		genradix_for_each(&c->journal_entries, iter, _p) {
			p = *_p;
			if (!p)
				continue;

			if (!seq)
				seq = le64_to_cpu(p->j.seq);

			struct u64_range missing;
			while ((missing = bch2_journal_entry_missing_range(
					c, seq, le64_to_cpu(p->j.seq)))
				       .start)
				seq = min_seq_to_print = missing.end;

			seq = le64_to_cpu(p->j.seq) + 1;
		}
	}

	if (nr_entries) {
		u64 max_seq = 0;
		genradix_for_each(&c->journal_entries, iter, _p)
			if (*_p)
				max_seq =
					max(max_seq, le64_to_cpu((*_p)->j.seq));

		min_seq_to_print = max(
			min_seq_to_print,
			max_seq >= nr_entries ? max_seq - nr_entries + 1 : 0);
	}

	u64 seq = 0;
	genradix_for_each(&c->journal_entries, iter, _p) {
		p = *_p;
		if (!p)
			continue;

		u64 p_seq = le64_to_cpu(p->j.seq);

		if (p_seq < min_seq_to_print || p_seq < seq_start)
			continue;

		if (p_seq > seq_end)
			break;

		if (!seq)
			seq = p_seq;

		struct u64_range missing;
		while ((missing =
				bch2_journal_entry_missing_range(c, seq, p_seq))
			       .start) {
			printf("missing %llu entries at %llu-%llu%s\n",
			       missing.end - missing.start, missing.start,
			       missing.end - 1,
			       missing.end < c->journal.last_seq_ondisk ?
				       " (not dirty)" :
					     "");
			seq = missing.end;
		}

		seq = p_seq + 1;

		if (!f.blacklisted &&
		    (p->ignore_blacklisted ||
		     bch2_journal_seq_is_blacklisted(c, p_seq, false)))
			continue;

		if (f.flush_only && JSET_NO_FLUSH(&p->j))
			continue;

		journal_replay_print(c, f, p);
	}

	bch2_fs_stop(c);
	darray_for_each(f.transaction.f, s) free(s->msg);
	darray_exit(&f.transaction.f);
	darray_exit(&f.key.f);
	darray_exit(&devs);
	return 0;
}
