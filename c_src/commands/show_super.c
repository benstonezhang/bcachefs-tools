/*
 * show-super: Display superblock contents for a bcachefs device.
 *
 * Key detail: we use ca->disk_sb.sb (per-device superblock) not c->disk_sb.sb
 * (filesystem-level copy), because __copy_super deliberately omits fields like
 * magic and layout from the fs-level copy. The per-device copy has everything.
 *
 * Ported from src/commands/super_cmd.rs.
 *
 * GPLv2
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "init/fs.h"
#include "sb/io.h"
#include "sb/members.h"
#include "cmds.h"

static void show_super_usage(void)
{
	puts("bcachefs show-super \n"
	     "Usage: bcachefs show-super [OPTION].. device\n"
	     "\n"
	     "Options:\n"
	     "  -f, --fields=(fields)       list of sections to print\n"
	     "  -F, --field-only=field      print superblock section only, no header\n"
	     "  -l, --layout                print superblock layout\n"
	     "  -h, --help                  display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
	exit(EXIT_SUCCESS);
}

int cmd_show_super(int argc, char *argv[])
{
	static const struct option longopts[] = { { "fields", 1, NULL, 'f' },
						  { "field-only", 1, NULL,
						    'F' },
						  { "layout", 0, NULL, 'l' },
						  { "help", 0, NULL, 'h' },
						  { NULL } };
	unsigned fields = 1u << BCH_SB_FIELD_ext;
	int field_only = -1;
	bool print_layout = false;
	bool print_default_fields = true;
	int opt;

	while ((opt = getopt_long(argc, argv, "f:F:lh", longopts, NULL)) != -1)
		switch (opt) {
		case 'f':
			fields = !strcmp(optarg, "all") ?
					 ~0 :
					       (unsigned)read_flag_list_or_die(
						 optarg, bch2_sb_fields,
						 "superblock field");
			print_default_fields = false;
			break;
		case 'F':
			field_only = read_string_list_or_die(
				optarg, bch2_sb_fields, "superblock field");
			print_default_fields = false;
			break;
		case 'l':
			print_layout = true;
			break;
		case 'h':
			show_super_usage();
			break;
		}
	args_shift(optind);

	char *dev = arg_pop();
	if (!dev)
		die("please supply a device");
	if (argc)
		die("too many arguments");

	struct bch_opts opts = bch2_opts_empty();

	opt_set(opts, noexcl, true);
	opt_set(opts, nochanges, true);
	opt_set(opts, no_version_check, true);
	opt_set(opts, nostart, true);

	darray_const_str devices = {};
	darray_push(&devices, dev);

	struct bch_fs *c = bch2_fs_open(&devices, &opts, NULL);
	if (IS_ERR(c))
		die("Error opening %s: %s", dev, bch2_err_str(PTR_ERR(c)));

	for_each_online_member(c, ca, 0)
	{
		struct bch_sb *sb = ca->disk_sb.sb;

		unsigned local_fields = fields;
		if (print_default_fields) {
			local_fields |= bch2_sb_field_get(sb, members_v2) ?
						1u << BCH_SB_FIELD_members_v2 :
						      1u << BCH_SB_FIELD_members_v1;
			local_fields |= 1u << BCH_SB_FIELD_errors;
		}

		struct printbuf buf = PRINTBUF;
		buf.human_readable_units = true;

		bch2_sb_to_text_with_names(&buf, c, sb, print_layout,
					   local_fields, field_only);
		printf("%s", buf.buf);
		printbuf_exit(&buf);
	}

	bch2_fs_stop(c);
	darray_exit(&devices);

	return 0;
}
