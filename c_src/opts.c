/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * C implementation of bcachefs option helpers.
 * Ported from src/commands/opts.rs.
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "libbcachefs.h"

/*
 * Look up a bcachefs option by name, handling --nooption and --no-option
 * negation for booleans.
 *
 * Returns (opt_id, opt_ref, negated).
 */
int bch2_opt_lookup_negated(const char *name, bool *negated)
{
	int id = bch2_opt_lookup(name);
	if (id >= 0) {
		*negated = false;
		return id;
	}

	if (!strncmp(name, "no", 2)) {
		id = bch2_opt_lookup(name + 2);
		if (id >= 0 && bch2_opt_table[id].type == BCH_OPT_BOOL) {
			*negated = true;
			return id;
		}
	}

	if (!strncmp(name, "no_", 3)) {
		id = bch2_opt_lookup(name + 3);
		if (id >= 0 && bch2_opt_table[id].type == BCH_OPT_BOOL) {
			*negated = true;
			return id;
		}
	}

	return -1;
}

int parse_opt_val(const struct bch_option *opt, const char *val_str, u64 *res)
{
	struct printbuf err = PRINTBUF;
	int ret = bch2_opt_parse(NULL, opt, val_str, res, &err);
	if (ret < 0 && ret != -BCH_ERR_option_needs_open_fs) {
		if (err.pos)
			die("invalid option: %s", err.buf);
		else
			die("invalid option: %s", val_str);
	}
	printbuf_exit(&err);
	return ret;
}

static bool opt_type_filter(const struct bch_option *opt, unsigned flags_all,
			    unsigned flags_none)
{
	if (opt->flags & (flags_none | OPT_HIDDEN))
		return false;

	if ((flags_all & OPT_FORMAT) && !opt->set_sb && !opt->set_member)
		return false;

	return (opt->flags & flags_all) == flags_all;
}

#define newline(c)            \
	do {                  \
		printf("\n"); \
		c = 0;        \
	} while (0)

void bch2_opts_usage(unsigned flags_all, unsigned flags_none)
{
	const struct bch_option *opt;
	unsigned i, c = 0, helpcol = 32;

	for (opt = bch2_opt_table; opt < bch2_opt_table + bch2_opts_nr; opt++) {
		if (!opt_type_filter(opt, flags_all, flags_none))
			continue;

		c += printf("      --%s", opt->attr.name);

		switch (opt->type) {
		case BCH_OPT_BOOL:
			break;
		case BCH_OPT_STR:
			c += printf("=(");
			for (i = 0; opt->choices[i]; i++) {
				if (i)
					c += printf("|");
				c += printf("%s", opt->choices[i]);
			}
			c += printf(")");
			break;
		default:
			if (opt->hint)
				c += printf("=%s", opt->hint);
			break;
		}

		if (opt->help) {
			const char *l = opt->help;

			if (c >= helpcol)
				newline(c);

			while (1) {
				const char *n = strchrnul(l, '\n');

				while (c < helpcol) {
					putchar(' ');
					c++;
				}
				printf("%.*s", (int)(n - l), l);
				newline(c);

				if (!*n)
					break;
				l = n + 1;
			}
		} else {
			newline(c);
		}
	}
}

const struct bch_option *bch2_cmdline_opt_parse(int argc, char *argv[],
						unsigned opt_types)
{
	if (optind >= argc)
		return NULL;

	if (argv[optind][0] != '-' || argv[optind][1] != '-')
		return NULL;

	char *optstr = strdup(argv[optind] + 2);
	optarg = argv[optind + 1];

	char *eq = strchr(optstr, '=');
	if (eq) {
		*eq = '\0';
		optarg = eq + 1;
	}

	if (!optarg)
		optarg = "1";

	bool negated = false;
	int optid = bch2_opt_lookup_negated(optstr, &negated);
	if (optid < 0)
		goto noopt;

	const struct bch_option *opt = bch2_opt_table + optid;
	if (!opt_type_filter(opt, opt_types, 0))
		goto noopt;

	if (negated)
		optarg = "0";

	optind++;

	if (opt->type != BCH_OPT_BOOL) {
		if (optarg == argv[optind])
			optind++;
	} else {
		optarg = negated ? "0" : NULL;
	}

	free(optstr);
	return opt;
noopt:
	free(optstr);
	return NULL;
}

/*
 * Extract bcachefs options from command line arguments.
 *
 * Scans argv for --option or --nooption matching the given flag_filter.
 * Matching options are extracted and removed from argv, update *argc accordingly.
 */
struct bch_opt_strs bch2_cmdline_opts_get(int *argc, char *argv[],
					  unsigned flag_filter)

{
	struct bch_opt_strs strs = {};
	int i = 1, j = 1;

	while (i < *argc) {
		int old_optind = optind;
		optind = i;
		const struct bch_option *opt =
			bch2_cmdline_opt_parse(*argc, argv, flag_filter);

		if (opt) {
			strs.by_id[opt - bch2_opt_table] =
				strdup(optarg ?: "1");
			i = optind;
		} else {
			optind = old_optind;
			argv[j++] = argv[i++];
		}
	}

	*argc = j;
	return strs;
}

struct bch_opts bch2_parse_opts(struct bch_opt_strs strs)
{
	struct bch_opts opts = bch2_opts_empty();
	struct printbuf err = PRINTBUF;

	for (unsigned i = 0; i < bch2_opts_nr; i++) {
		if (!strs.by_id[i])
			continue;

		u64 v;
		int ret = bch2_opt_parse(NULL, &bch2_opt_table[i],
					 strs.by_id[i], &v, &err);
		if (ret < 0 && ret != -BCH_ERR_option_needs_open_fs)
			die("Invalid option %s: %s", bch2_opt_table[i].attr.name,
			    err.buf ? err.buf : "unknown error");

		bch2_opt_set_by_id(&opts, i, v);
	}

	printbuf_exit(&err);
	return opts;
}

void bch2_opt_strs_free(struct bch_opt_strs *opts)
{
	for (unsigned i = 0; i < bch2_opts_nr; i++) {
		free(opts->by_id[i]);
		opts->by_id[i] = NULL;
	}
}
