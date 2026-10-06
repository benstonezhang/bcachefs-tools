/*
 * Ported from src/commands/format.rs.
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
#include "fs/inode.h"
#include "util/darray.h"
#include "crypto.h"
#include "cmds.h"

static void format_usage(void)
{
	puts("bcachefs format - create a new bcachefs filesystem on one or more devices\n"
	     "Usage: bcachefs format [OPTION]... <devices>\n"
	     "\n"
	     "Options:");

	bch2_opts_usage(OPT_FORMAT | OPT_FS, 0);

	puts("      --replicas=#            Sets both data and metadata replicas\n"
	     "      --encrypted             Enable whole filesystem encryption (chacha20/poly1305)\n"
	     "      --passphrase_file=file   File containing passphrase used for encryption/decryption\n"
	     "      --no_passphrase         Don't encrypt master encryption key\n"
	     "  -L, --fs_label=label\n"
	     "  -U, --uuid=uuid\n"
	     "      --superblock_size=size\n"
	     "      --version=version        Create filesystem with specified on disk format version instead of the latest\n"
	     "      --source=path           Initialize the bcachefs filesystem from this root directory\n"
	     "\n"
	     "Device specific options:");

	bch2_opts_usage(OPT_DEVICE, 0);

	puts("      --fs_size=size          Size of filesystem on device\n"
	     "  -l, --label=label           Disk label\n"
	     "\n"
	     "  -f, --force\n"
	     "  -q, --quiet                 Only print errors\n"
	     "  -v, --verbose               Verbose filesystem initialization\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Device specific options must come before corresponding devices, e.g.\n"
	     "  bcachefs format --label cache /dev/sdb /dev/sdc\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static void build_fs(struct bch_fs *c, const char *src_path)
{
	struct copy_fs_state s = {};
	int src_fd = xopen(src_path, O_RDONLY | O_NOATIME);

	copy_fs(c, src_fd, src_path, &s);
}

static u8 requested_replicas(struct bch_opts *fs_opts)
{
	u8 data = opt_defined(*fs_opts, data_replicas) ?
			  fs_opts->data_replicas :
			  bch2_opts_default.data_replicas;
	u8 metadata = opt_defined(*fs_opts, metadata_replicas) ?
			      fs_opts->metadata_replicas :
			      bch2_opts_default.metadata_replicas;

	return data > metadata ? data : metadata;
}

struct parent_disk_group {
	char *parent;
	char *paths;
};

/*
 * Warn when replicas > 1 but devices resolve to fewer physical disks than
 * requested (e.g. multiple partitions on one drive). Ported from
 * warn_same_parent_disk_replicas in format.rs.
 */
static void warn_same_parent_disk_replicas(bool quiet, struct bch_opts *fs_opts,
					   dev_opts_list *devices)
{
	if (quiet)
		return;

	u8 replicas = requested_replicas(fs_opts);
	if (replicas <= 1)
		return;

	DARRAY(struct parent_disk_group) by_parent = {};

	darray_for_each(*devices, dev)
	{
		if (!dev->bdev)
			continue;
		char *parent = fd_to_parent_disk_sysfs(dev->bdev->bd_fd);
		if (!parent)
			continue;

		bool found = false;
		darray_for_each(by_parent, e)
		{
			if (!strcmp(e->parent, parent)) {
				char *joined =
					mprintf("%s, %s", e->paths, dev->path);
				free(e->paths);
				e->paths = joined;
				found = true;
				break;
			}
		}
		if (!found) {
			darray_push(&by_parent, ((struct parent_disk_group){
							.parent = parent,
							.paths = strdup(dev->path) }));
			parent = NULL;
		}
		free(parent);
	}

	if (!by_parent.nr || by_parent.nr >= replicas)
		goto out;

	bool have_dup = false;
	struct printbuf dups = PRINTBUF;
	darray_for_each(by_parent, e)
	{
		if (strchr(e->paths, ',')) {
			if (have_dup)
				prt_str(&dups, "; ");
			prt_str(&dups, e->paths);
			have_dup = true;
		}
	}
	if (!have_dup) {
		printbuf_exit(&dups);
		goto out;
	}

	fprintf(stderr,
		"warning: requested %u replicas, but the devices resolve to only %zu physical disk%s\n",
		replicas, by_parent.nr, by_parent.nr == 1 ? "" : "s");
	fprintf(stderr,
		"warning: multiple format devices share a parent disk: %s\n",
		dups.buf);
	printbuf_exit(&dups);

out:
	darray_for_each(by_parent, e)
	{
		free(e->parent);
		free(e->paths);
	}
	darray_exit(&by_parent);
}

int cmd_format(int argc, char *argv[])
{
	dev_opts_list devices = {};
	darray_const_str device_paths = {};
	struct format_opts opts = format_opts_default();
	struct dev_opts dev_opts = dev_opts_default();
	bool force = false, no_passphrase = false, quiet = false,
	     initialize = true, verbose = false;
	bool unconsumed_dev_option = false;

	struct bch_opt_strs fs_opt_strs = {};
	struct bch_opts fs_opts = bch2_opts_empty();

	const unsigned opt_flags = OPT_FORMAT | OPT_FS | OPT_DEVICE;

	bool version_given = false;

	int i = 1;
	while (i < argc) {
		char *arg = argv[i];

		if (!strcmp(arg, "--")) {
			i++;
			while (i < argc) {
				char *path = strdup(argv[i]);
				darray_push(&device_paths, path);
				dev_opts.path = path;

				struct dev_opts new_dev = dev_opts;
				new_dev.opt_strs = (typeof(new_dev.opt_strs)){};
				dev_opts_opt_strs_clone(&new_dev, &dev_opts);
				darray_push(&devices, new_dev);

				unconsumed_dev_option = false;
				i++;
			}
			break;
		}

		if (!strncmp(arg, "--", 2) && strlen(arg) > 2) {
			char *opt_part = strdup(arg + 2);
			char *raw_name = opt_part;
			char *inline_val = strchr(opt_part, '=');
			if (inline_val) {
				*inline_val = '\0';
				inline_val++;
			}

			/* Format-specific long options first to prevent interception */
			if (!strcmp(raw_name, "replicas")) {
				const char *val_str = inline_val;
				if (!val_str) {
					i++;
					if (i >= argc)
						die("option --replicas requires a value");
					val_str = argv[i];
				}
				unsigned v = atoi(val_str);
				if (v == 0 || v > BCH_REPLICAS_MAX)
					die("invalid replicas");
				opt_set(fs_opts, metadata_replicas, v);
				opt_set(fs_opts, data_replicas, v);
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "encrypted")) {
				opts.encrypted = true;
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "passphrase_file")) {
				const char *val_str = inline_val;
				if (!val_str) {
					i++;
					if (i >= argc)
						die("option --passphrase_file requires a value");
					val_str = argv[i];
				}
				free(opts.passphrase_file);
				opts.passphrase_file = strdup(val_str);
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "no_passphrase")) {
				no_passphrase = true;
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "fs_label")) {
				const char *val_str = inline_val;
				if (!val_str) {
					i++;
					if (i >= argc)
						die("option --fs_label requires a value");
					val_str = argv[i];
				}
				free(opts.label);
				opts.label = strdup(val_str);
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "uuid")) {
				const char *val_str = inline_val;
				if (!val_str) {
					i++;
					if (i >= argc)
						die("option --uuid requires a value");
					val_str = argv[i];
				}
				if (uuid_parse(val_str, opts.uuid.b))
					die("Bad uuid");
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "fs_size")) {
				const char *val_str = inline_val;
				if (!val_str) {
					i++;
					if (i >= argc)
						die("option --fs_size requires a value");
					val_str = argv[i];
				}
				if (bch2_strtoull_h(val_str, &dev_opts.fs_size))
					die("invalid filesystem size");
				unconsumed_dev_option = true;
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "superblock_size")) {
				const char *val_str = inline_val;
				if (!val_str) {
					i++;
					if (i >= argc)
						die("option --superblock_size requires a value");
					val_str = argv[i];
				}
				u64 size;
				if (bch2_strtoull_h(val_str, &size))
					die("invalid superblock size");
				opts.superblock_size = size >> 9;
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "label")) {
				const char *val_str = inline_val;
				if (!val_str) {
					i++;
					if (i >= argc)
						die("option --label requires a value");
					val_str = argv[i];
				}
				dev_opt_str_push(&dev_opts, Opt_label, val_str);
				unconsumed_dev_option = true;
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "version")) {
				const char *val_str = inline_val;
				if (!val_str) {
					i++;
					if (i >= argc)
						die("option --version requires a value");
					val_str = argv[i];
				}
				opts.version = version_parse((char *)val_str);
				version_given = true;
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "no_initialize")) {
				initialize = false;
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "source")) {
				const char *val_str = inline_val;
				if (!val_str) {
					i++;
					if (i >= argc)
						die("option --source requires a value");
					val_str = argv[i];
				}
				free(opts.source);
				opts.source = strdup(val_str);
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "force")) {
				force = true;
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "quiet")) {
				quiet = true;
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "verbose")) {
				verbose = true;
				opt_set(fs_opts, verbose, 1);
				free(opt_part);
				i++;
				continue;
			} else if (!strcmp(raw_name, "help")) {
				format_usage();
				exit(EXIT_SUCCESS);
			}

			/* Generic option lookup */
			char *name = strdup(raw_name);
			for (char *p = name; *p; p++)
				if (*p == '-')
					*p = '_';

			bool negated = false;
			int opt_id = bch2_opt_lookup_negated(name, &negated);

			if (opt_id >= 0 &&
			    (bch2_opt_table[opt_id].flags & opt_flags)) {
				const struct bch_option *opt = &bch2_opt_table[opt_id];
				const char *val_str;

				if (negated) {
					val_str = "0";
				} else if (inline_val) {
					val_str = inline_val;
				} else if (opt->type != BCH_OPT_BOOL) {
					i++;
					if (i >= argc)
						die("option --%s requires a value", raw_name);
					val_str = argv[i];
				} else {
					val_str = "1";
				}

				u64 v;
				struct printbuf err = PRINTBUF;
				int ret = bch2_opt_parse(NULL, opt, val_str, &v, &err);
				if (ret == -BCH_ERR_option_needs_open_fs) {
					if (opt->flags & OPT_DEVICE) {
						dev_opt_str_push(&dev_opts, opt_id,
								 val_str);
						unconsumed_dev_option = true;
					} else {
						fs_opt_strs.by_id[opt_id] =
							strdup(val_str);
					}
				} else if (ret) {
					die("invalid option %s: %s", val_str, err.buf);
				} else {
					if (opt->type == BCH_OPT_STR_MEMBER) {
						dev_opt_str_push(&dev_opts, opt_id,
								 val_str);
						unconsumed_dev_option = true;
					} else if (opt->flags & OPT_DEVICE) {
						bch2_opt_set_by_id(&dev_opts.opts, opt_id, v);
						unconsumed_dev_option = true;
					} else if (opt->flags & OPT_FS) {
						bch2_opt_set_by_id(&fs_opts, opt_id, v);
					}
				}
				printbuf_exit(&err);
				free(name);
				free(opt_part);
				i++;
				continue;
			}
			free(name);
			die("unknown option: %s", arg);
		}

		if (arg[0] == '-' && strlen(arg) > 1) {
			/* Short options */
			for (size_t j = 1; j < strlen(arg); j++) {
				char c = arg[j];
				const char *val_str = NULL;

				switch (c) {
				case 'L':
					if (arg[j + 1]) {
						val_str = arg + j + 1;
						j = strlen(arg);
					} else {
						i++;
						if (i >= argc)
							die("-L requires a value");
						val_str = argv[i];
					}
					free(opts.label);
					opts.label = strdup(val_str);
					break;
				case 'l':
					if (arg[j + 1]) {
						val_str = arg + j + 1;
						j = strlen(arg);
					} else {
						i++;
						if (i >= argc)
							die("-l requires a value");
						val_str = argv[i];
					}
					dev_opt_str_push(&dev_opts, Opt_label,
							 val_str);
					unconsumed_dev_option = true;
					break;
				case 'U':
					if (arg[j + 1]) {
						val_str = arg + j + 1;
						j = strlen(arg);
					} else {
						i++;
						if (i >= argc)
							die("-U requires a value");
						val_str = argv[i];
					}
					if (uuid_parse(val_str, opts.uuid.b))
						die("Bad uuid");
					break;
				case 'f':
					force = true;
					break;
				case 'q':
					quiet = true;
					break;
				case 'v':
					verbose = true;
					opt_set(fs_opts, verbose, 1);
					break;
				case 'h':
					format_usage();
					exit(EXIT_SUCCESS);
				default:
					die("unknown option: -%c", c);
				}
			}
			i++;
			continue;
		}

		/* Positional argument: device path */
		char *path = strdup(arg);
		darray_push(&device_paths, path);
		dev_opts.path = path;

		struct dev_opts new_dev = dev_opts;
		new_dev.opt_strs = (typeof(new_dev.opt_strs)){};
		dev_opts_opt_strs_clone(&new_dev, &dev_opts);
		darray_push(&devices, new_dev);

		unconsumed_dev_option = false;
		i++;
	}

	if (unconsumed_dev_option)
		die("Options for devices apply to subsequent devices; got a device option with no device");

	if (!devices.nr) {
		format_usage();
		die("Please supply a device");
	}

	if (opts.source && !initialize)
		die("--source, --no_initialize are incompatible");

	if (opts.source && version_given)
		die("--version cannot be used with --source: populating the "
		    "filesystem runs the current code's write path, which "
		    "upgrades it to the current version as soon as it goes "
		    "read-write - the requested version would not survive. "
		    "Format with --version alone, then populate using tools "
		    "of that version.");

	if (opts.passphrase_file && !opts.encrypted)
		die("--passphrase_file requires --encrypted");

	if (opts.passphrase_file && no_passphrase)
		die("--passphrase_file, --no_passphrase are incompatible");

	if (opts.encrypted && !no_passphrase) {
		if (opts.passphrase_file) {
			opts.passphrase =
				read_file_str(AT_FDCWD, opts.passphrase_file);
			if (!opts.passphrase)
				die("Error reading passphrase file %s: %m",
				    opts.passphrase_file);
		} else {
			opts.passphrase =
				read_passphrase_twice("Enter new passphrase: ");
			if (!opts.passphrase)
				die("Error reading passphrase");
		}
		initialize = false;
	}

	if (!opts.source) {
		if (getenv("BCACHEFS_KERNEL_ONLY"))
			initialize = false;

		if (opts.version != bcachefs_metadata_version_current) {
			printf("version mismatch, not initializing\n");
			initialize = false;
		}
	}

	darray_for_each(devices, dev)
	{
		char *mpath = find_multipath_holder(dev->path);
		if (mpath) {
			warn_multipath_component(dev->path, mpath);
			free(mpath);
			if (!force)
				die("Use -f/--force to format anyway");
		}

		int ret = open_for_format(dev, 0, force);
		if (ret)
			die("Error opening %s: %s", dev->path, strerror(-ret));
	}

	warn_same_parent_disk_replicas(quiet, &fs_opts, &devices);

	/* Default shard_inode_numbers_bits if the user didn't set it. The policy
	 * (cpu-scaled, fs-size-capped, clamped to [0, 8]) lives in C —
	 * bch2_shard_inode_numbers_bits_default() — so the format-time default and
	 * the kernel sb_validate rewrite of legacy bits=0 filesystems can't diverge.
	 */
	if (!opt_defined(fs_opts, shard_inode_numbers_bits)) {
		unsigned nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);
		if (nr_cpus < 1)
			nr_cpus = 1;

		u64 total_fs_size = 0;
		darray_for_each(devices, dev)
		{
			if (!dev->fs_size)
				dev->fs_size = get_size(dev->bdev->bd_fd);
			total_fs_size += dev->fs_size;
		}

		u64 btree_node_bytes = fs_opts.btree_node_size
			? (u64)fs_opts.btree_node_size
			: 256 << 10;

		u8 bits = bch2_shard_inode_numbers_bits_default(
				nr_cpus, total_fs_size, btree_node_bytes);
		opt_set(fs_opts, shard_inode_numbers_bits, bits);
	}

	struct bch_sb *sb = bch2_format(fs_opt_strs, fs_opts, opts, devices);
	if (!sb)
		die("format returned null");

	if (!quiet) {
		struct printbuf buf = PRINTBUF;
		buf.human_readable_units = true;

		bch2_sb_to_text_with_names(&buf, NULL, sb, false,
					   1 << BCH_SB_FIELD_members_v2, -1);
		printf("%s", buf.buf);
		printbuf_exit(&buf);
	}

	if (opts.passphrase) {
		memzero_explicit(opts.passphrase, strlen(opts.passphrase));
		free(opts.passphrase);
	}

	/*
	 * We must close the devices before calling bch2_fs_open(), or the kernel
	 * will return EBUSY (because we opened them with BLK_OPEN_EXCL):
	 */
	darray_for_each(devices, dev)
	{
		if (dev->file)
			bdev_fput(dev->file);
	}

	if (initialize) {
		/*
		 * Start the filesystem once, to allocate the journal and create
		 * the root directory:
		 */
		struct bch_opts open_opts = bch2_opts_empty();
		struct bch_fs *c = bch2_fs_open(&device_paths, &open_opts, NULL);
		if (IS_ERR(c))
			die("error opening %s: %s", device_paths.data[0],
			    bch2_err_str(PTR_ERR(c)));

		if (opts.source)
			build_fs(c, opts.source);

		bch2_fs_stop(c);
	} else {
		if (!quiet)
			printf("Initialization skipped (encrypted or version mismatch); journal will be initialized on first mount.\n");
	}

	if (!quiet)
		printf("Done.\n");

	free(sb);
	bch2_opt_strs_free(&fs_opt_strs);
	darray_for_each(devices, dev) dev_opts_opt_strs_exit(dev);
	darray_exit(&devices);
	darray_for_each(device_paths, p) free((char *)*p);
	darray_exit(&device_paths);
	free(opts.label);
	free(opts.source);
	free(opts.passphrase_file);
	dev_opts_opt_strs_exit(&dev_opts);
	return 0;
}
