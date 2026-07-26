/*
 * mount: Mount a bcachefs filesystem by its UUID or label.
 *
 * Devices are discovered automatically by scanning for the filesystem
 * UUID or label. Unlike btrfs, this is handled entirely in userspace.
 *
 * Use OLD_BLKID_UUID=<uuid> in fstab entries when systemd consumes
 * UUID=<uuid> before the bcachefs mount helper can scan all members.
 *
 * If the filesystem is encrypted, the passphrase will be looked up in the
 * kernel keyring first; if not found, the user is prompted interactively.
 *
 * Ported from src/commands/mount.rs.
 *
 * GPLv2
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "sb/io.h"
#include "cmds.h"
#include "crypto.h"

typedef struct {
	const char *name;
	unsigned long mask;
} mount_flag_opt;

static const mount_flag_opt mount_flag_opts[] = {
	{ "dirsync", MS_DIRSYNC },
	{ "lazytime", 1UL << 25 }, /* MS_LAZYTIME */
	{ "mand", MS_MANDLOCK },
	{ "noatime", MS_NOATIME },
	{ "nodev", MS_NODEV },
	{ "nodiratime", MS_NODIRATIME },
	{ "noexec", MS_NOEXEC },
	{ "nosuid", MS_NOSUID },
	{ "relatime", MS_RELATIME },
	{ "remount", MS_REMOUNT },
	{ "ro", MS_RDONLY },
	{ "rw", 0 },
	{ "strictatime", MS_STRICTATIME },
	{ "sync", MS_SYNCHRONOUS },
	/* Userspace-only fstab options — not passed to the kernel */
	{ "auto", 0 },
	{ "noauto", 0 },
	{ "nofail", 0 },
	{ "_netdev", 0 },
	{ "user", 0 },
	{ "nouser", 0 },
	{ "users", 0 },
	{ "group", 0 },
	{ "owner", 0 },
};

static void mount_usage(void)
{
	puts("bcachefs mount - mount a bcachefs filesystem\n"
	     "Usage: bcachefs mount [OPTION]... <device|UUID=|LABEL=> [mountpoint]\n"
	     "\n"
	     "Options:\n"
	     "  -o options             Mount options (comma separated)\n"
	     "  -t, --type=type        Filesystem type (e.g. bcachefs, bcachefs.fuse)\n"
	     "      --passphrase_file=path\n"
	     "                         Read passphrase from file\n"
	     "  -k, --key_location=(fail|wait|ask|stdin)\n"
	     "                         Passphrase policy (default: ask)\n"
	     "  -f, --fake             Do everything except the mount syscall\n"
	     "  -n, --no-mtab          Accepted for mount(8) compatibility (no-op)\n"
	     "  -s, --sloppy           Accepted for mount(8) compatibility (no-op)\n"
	     "  -v, --verbose          Verbose output\n"
	     "  -h, --help             Display this help and exit\n"
	     "\n"
	     "Device may be a path, UUID=<uuid>, OLD_BLKID_UUID=<uuid>, or LABEL=<label>.\n"
	     "\n"
	     "Mount options include both standard filesystem independent options (see\n"
	     "mount(8)) and bcachefs specific options:");

	bch2_opts_usage(OPT_MOUNT, 0);
}

/*
 * Parse comma-separated mount options and split out mountflags (MS_*)
 * from filesystem-specific options.
 */
static unsigned long parse_mount_options(char *options, char **fs_options)
{
	unsigned long flags = 0;
	struct printbuf buf = PRINTBUF;
	char *s, *orig = options ? strdup(options) : NULL;
	char *p = orig;

	while ((s = strsep(&p, ","))) {
		bool found = false;
		for (unsigned i = 0; i < ARRAY_SIZE(mount_flag_opts); i++) {
			if (!strcmp(s, mount_flag_opts[i].name)) {
				flags |= mount_flag_opts[i].mask;
				found = true;
				break;
			}
		}

		if (found)
			continue;

		if (!strncmp(s, "x-", 2) || !strncmp(s, "comment=", 8))
			continue;

		if (*s) {
			if (buf.pos)
				prt_char(&buf, ',');
			prt_str(&buf, s);
		}
	}

	*fs_options = buf.buf;
	free(orig);
	return flags;
}

static int do_mount(const char *src, const char *target, const char *fstype,
		    unsigned long flags, const char *data)
{
	int ret;

	while (1) {
		ret = mount(src, target, fstype, flags, data);
		if (!ret || (errno != EACCES && errno != EROFS) ||
		    (flags & MS_RDONLY))
			break;

		fprintf(stderr,
			"mount: %s: device write-protected, mounting read-only\n",
			src);
		flags |= MS_RDONLY;
	}

	if (ret) {
		if (errno == EBUSY)
			fprintf(stderr,
				"mount: %s: %s already mounted or mount point busy\n",
				target, src);
		else
			fprintf(stderr, "mount: %s: %m\n", src);
		return -errno;
	}

	return 0;
}

/*
 * If a user explicitly specifies policy or passphrase file then use that
 * without falling back to other mechanisms. If these options are not used,
 * then search for the key in keyring or ask for it.
 */
static void handle_unlock(struct bch_sb_handle *sb,
			  const char *passphrase_file,
			  enum bch_unlock_policy policy,
			  bool policy_explicit,
			  unsigned verbosity)
{
	const char *dev = sb->sb_name;

	/* Priority order (matching Rust):
	 * 1. Explicit --key_location (policy_explicit=true)
	 * 2. --passphrase_file (without explicit --key_location)
	 * 3. Keyring search
	 * 4. Interactive prompt (default policy)
	 */
	if (!policy_explicit && passphrase_file) {
		/* Use passphrase_file without falling back to policy */
		char *passphrase = read_file_str(AT_FDCWD, passphrase_file);
		if (!passphrase)
			die("Error reading passphrase file %s: %m",
			    passphrase_file);

		int ret = bch2_key_handle_new(sb->sb, passphrase,
					      BCH_KEYRING_user);
		memzero_explicit(passphrase, strlen(passphrase));
		free(passphrase);
		if (ret < 0)
			die("Error unlocking %s: %s", dev, strerror(-ret));

		if (ret == 0 && verbosity)
			printf("superblock unlocked: %s\n", dev);
		return;
	}

	/* Use explicit policy or default fallback */
	int ret = bch2_unlock_policy_apply(policy, sb, NULL);
	if (ret < 0)
		die("Error unlocking %s: %s", dev, strerror(-ret));

	if (ret == 0 && verbosity)
		printf("superblock unlocked: %s\n", dev);
}

struct module_check {
	bool loaded;
	char *modprobe_error;
};

static struct module_check check_bcachefs_module(void)
{
	struct module_check r = { .loaded = false, .modprobe_error = NULL };
	struct stat st;

	if (!stat("/sys/module/bcachefs", &st)) {
		r.loaded = true;
		return r;
	}

	int status = system("modprobe bcachefs");
	if (status == -1)
		r.modprobe_error = strdup("could not run modprobe bcachefs");
	else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		r.modprobe_error =
			strdup("modprobe bcachefs exited unsuccessfully");

	r.loaded = !stat("/sys/module/bcachefs", &st);
	return r;
}

int cmd_mount(int argc, char *argv[])
{
	enum {
		OPT_PASSPHRASE_FILE = 1000,
	};
	static const struct option longopts[] = {
		{ "options", required_argument, NULL, 'o' },
		{ "type", required_argument, NULL, 't' },
		{ "passphrase_file", required_argument, NULL,
		  OPT_PASSPHRASE_FILE },
		{ "key_location", required_argument, NULL, 'k' },
		{ "fake", no_argument, NULL, 'f' },
		{ "no-mtab", no_argument, NULL, 'n' },
		{ "sloppy", no_argument, NULL, 's' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	char *options = NULL, *fs_options = NULL, *fstype = "bcachefs";
	char *passphrase_file = NULL;
	enum bch_unlock_policy policy = BCH_UNLOCK_POLICY_ask;
	bool policy_explicit = false;
	bool fake = false;
	unsigned verbosity = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "o:t:k:fnsvh", longopts,
				  NULL)) != -1)
		switch (opt) {
		case 'o':
			options = optarg;
			break;
		case 't':
			fstype = optarg;
			break;
		case OPT_PASSPHRASE_FILE:
			passphrase_file = optarg;
			break;
		case 'k':
			if (!strcmp(optarg, "fail"))
				policy = BCH_UNLOCK_POLICY_fail;
			else if (!strcmp(optarg, "wait"))
				policy = BCH_UNLOCK_POLICY_wait;
			else if (!strcmp(optarg, "ask"))
				policy = BCH_UNLOCK_POLICY_ask;
			else if (!strcmp(optarg, "stdin"))
				policy = BCH_UNLOCK_POLICY_stdin;
			else
				die("invalid key_location %s", optarg);
			policy_explicit = true;
			break;
		case 'f':
			fake = true;
			break;
		case 'n':
			/* mount(8) compat: mount.bcachefs does not update mtab */
			break;
		case 's':
			/* mount(8) compat: bcachefs already ignores unknown opts */
			break;
		case 'v':
			verbosity++;
			break;
		case 'h':
			mount_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (argc < 1)
		die("Please supply device(s)");
	if (argc > 2)
		die("Too many arguments");

	char *dev = argv[0];
	char *mountpoint = argv[1];

#ifdef BCACHEFS_FUSE
	if (!strcmp(fstype, "bcachefs.fuse")) {
		if (fake) {
			if (verbosity)
				printf("fake mount (-f/--fake): skipping FUSE mount\n");
			return 0;
		}
		return cmd_fusemount(argc + optind, argv - optind);
	}
#else
	if (!strcmp(fstype, "bcachefs.fuse"))
		die("FUSE support not compiled in (build with BCACHEFS_FUSE=1)");
#endif

	struct module_check module = check_bcachefs_module();

	unsigned long mount_flags = parse_mount_options(options, &fs_options);

	char *devs_str = bch2_scan_devices(dev);
	if (!devs_str) {
		free(fs_options);
		free(module.modprobe_error);
		die("no devices found for %s", dev);
	}

	/* Use the first device for checking encryption — read superblock once */
	char *first_dev = strdup(devs_str);
	char *sep = strchr(first_dev, ':');
	if (sep)
		*sep = '\0';

	{
		struct bch_opts opts = bch2_opts_empty();
		opt_set(opts, noexcl, true);
		opt_set(opts, nochanges, true);
		if (verbosity)
			opt_set(opts, verbose, true);
		struct bch_sb_handle sb;
		int ret = bch2_read_super(first_dev, &opts, &sb);
		if (ret) {
			fprintf(stderr, "Error opening %s: %s\n", first_dev,
				bch2_err_str(ret));
			free(first_dev);
			free(devs_str);
			free(fs_options);
			free(module.modprobe_error);
			exit(EXIT_FAILURE);
		}
		/* Only attempt unlock if the filesystem is encrypted */
		if (bch2_sb_is_encrypted(sb.sb))
			handle_unlock(&sb, passphrase_file, policy, policy_explicit, verbosity);
		bch2_free_super(&sb);
	}
	free(first_dev);

	int ret = 0;
	if (mountpoint) {
		if (fake) {
			if (verbosity)
				printf("fake mount (-f/--fake): skipping the mount syscall for %s\n",
				       mountpoint);
		} else {
			if (verbosity)
				printf("mounting %s to %s with options %s\n",
				       devs_str, mountpoint, fs_options ?: "");
			ret = do_mount(devs_str, mountpoint, "bcachefs",
				       mount_flags, fs_options);
			if (ret && !module.loaded) {
				fprintf(stderr, "bcachefs module not loaded?\n");
				if (module.modprobe_error)
					fprintf(stderr, "%s\n",
						module.modprobe_error);
			}
		}
	}

	free(devs_str);
	free(fs_options);
	free(module.modprobe_error);

	return ret;
}
