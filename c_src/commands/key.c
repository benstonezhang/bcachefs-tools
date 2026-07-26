/*
 * GPLv2
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "data/checksum.h"
#include "init/fs.h"
#include "cmds.h"
#include "crypto.h"

static void unlock_usage(void)
{
	puts("bcachefs unlock - unlock an encrypted filesystem so it can be mounted\n"
	     "Usage: bcachefs unlock [OPTION] device\n"
	     "\n"
	     "Options:\n"
	     "  -c, --check            Report encryption and lock state, then exit\n"
	     "  -k, --keyring (session|user|user_session)\n"
	     "                         Keyring to add to (default: user)\n"
	     "  -f, --file             Passphrase file to read from (disables passphrase prompt)\n"
	     "  -h, --help             Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_unlock(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "check", no_argument, NULL, 'c' },
		{ "keyring", required_argument, NULL, 'k' },
		{ "file", required_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	enum bch_keyring keyring = BCH_KEYRING_user;
	bool check = false;
	const char *passphrase_file_path = NULL;
	char *passphrase = NULL;

	int opt;

	while ((opt = getopt_long(argc, argv, "cf:k:h", longopts, NULL)) != -1)
		switch (opt) {
		case 'c':
			check = true;
			break;
		case 'k':
			if (!strcmp(optarg, "session"))
				keyring = BCH_KEYRING_session;
			else if (!strcmp(optarg, "user"))
				keyring = BCH_KEYRING_user;
			else if (!strcmp(optarg, "user_session"))
				keyring = BCH_KEYRING_user_session;
			else
				die("unknown keyring %s", optarg);
			break;
		case 'f':
			passphrase_file_path = optarg;
			break;
		case 'h':
			unlock_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	char *dev = arg_pop();
	if (!dev)
		die("Please supply a device");

	if (argc)
		die("Too many arguments");

	struct bch_opts opts = bch2_opts_empty();

	opt_set(opts, noexcl, true);
	opt_set(opts, nochanges, true);

	struct bch_sb_handle sb;
	int ret = bch2_read_super(dev, &opts, &sb);
	if (ret)
		die("Error opening %s: %s", dev, bch2_err_str(ret));

	if (check) {
		/*
		 * --check reports state without prompting. Unencrypted exits
		 * nonzero so boot scripts that branch on exit code do not
		 * prompt for a nonexistent passphrase. Encrypted (locked or
		 * unlocked) exits 0.
		 */
		if (!bch2_sb_is_encrypted(sb.sb)) {
			puts("Device has no encryption");
			bch2_free_super(&sb);
			return 1;
		}
		if (bch2_key_search(sb.sb))
			puts("Device is encrypted and unlocked");
		else
			puts("Device is encrypted and locked");
		bch2_free_super(&sb);
		return 0;
	}

	if (!bch2_sb_is_encrypted(sb.sb)) {
		bch2_free_super(&sb);
		die("%s is not encrypted", dev);
	}

	if (passphrase_file_path) {
		passphrase = read_file_str(AT_FDCWD, passphrase_file_path);
		if (!passphrase)
			die("Error reading passphrase file %s",
			    passphrase_file_path);
	} else {
		char label[sizeof(sb.sb->label) + 1];
		size_t llen = strnlen((const char *)sb.sb->label,
				      sizeof(sb.sb->label));
		memcpy(label, sb.sb->label, llen);
		label[llen] = '\0';

		passphrase = read_passphrase_uuid(&sb.sb->user_uuid, label,
						  "Enter passphrase: ");
	}

	/* First attempt */
	if (!bch2_key_handle_new(sb.sb, passphrase, keyring))
		goto success;

	if (passphrase_file_path)
		die("incorrect passphrase");

	/* Retry up to 2 more times */
	for (int i = 0; i < 2; i++) {
		memzero_explicit(passphrase, strlen(passphrase));
		free(passphrase);

		fprintf(stderr, "incorrect passphrase\n");
		passphrase = read_passphrase("Enter passphrase: ");
		if (!bch2_key_handle_new(sb.sb, passphrase, keyring))
			goto success;
	}
	die("incorrect passphrase limit reached");

success:
	bch2_free_super(&sb);
	if (passphrase) {
		memzero_explicit(passphrase, strlen(passphrase));
		free(passphrase);
	}
	return 0;
}

static void set_passphrase_usage(void)
{
	puts("bcachefs set-passphrase - set or change passphrase on an existing filesystem\n"
	     "Usage: bcachefs set-passphrase device\n"
	     "\n"
	     "Options:\n"
	     "  -h, --help             Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_set_passphrase(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' }, { NULL }
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			set_passphrase_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	if (!argc)
		die("Please supply one or more devices");

	darray_const_str devs = get_or_split_cmdline_devs(argc, argv);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, true);

	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	if (IS_ERR(c))
		die("Error opening devices: %s", bch2_err_str(PTR_ERR(c)));

	struct bch_sb *sb = c->disk_sb.sb;
	struct bch_sb_field_crypt *crypt = bch2_sb_field_get(sb, crypt);
	if (!crypt)
		die("Filesystem does not have encryption enabled");

	struct bch_key key;
	int ret = bch2_decrypt_sb_key(c, crypt, &key);
	if (ret)
		die("Error getting current key: %s", bch2_err_str(ret));

	char *new_passphrase = read_passphrase_twice("Enter new passphrase: ");

	bch_crypt_update_passphrase(sb, crypt, &key, new_passphrase);

	bch2_revoke_key(c->disk_sb.sb);
	bch2_write_super(c);
	bch2_fs_stop(c);

	memzero_explicit(new_passphrase, strlen(new_passphrase));
	free(new_passphrase);
	memzero_explicit(&key, sizeof(key));
	darray_exit(&devs);

	return 0;
}

static void remove_passphrase_usage(void)
{
	puts("bcachefs remove-passphrase - remove passphrase from an existing filesystem\n"
	     "Usage: bcachefs remove-passphrase device\n"
	     "\n"
	     "Options:\n"
	     "  -h, --help             Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_remove_passphrase(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' }, { NULL }
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			remove_passphrase_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	if (!argc)
		die("Please supply one or more devices");

	darray_const_str devs = get_or_split_cmdline_devs(argc, argv);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, true);

	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	if (IS_ERR(c))
		die("Error opening devices: %s", bch2_err_str(PTR_ERR(c)));

	struct bch_sb *sb = c->disk_sb.sb;
	struct bch_sb_field_crypt *crypt = bch2_sb_field_get(sb, crypt);
	if (!crypt)
		die("Filesystem does not have encryption enabled");

	struct bch_key key;
	int ret = bch2_decrypt_sb_key(c, crypt, &key);
	if (ret)
		die("Error getting current key: %s", bch2_err_str(ret));

	bch_crypt_update_passphrase(sb, crypt, &key, NULL);

	bch2_write_super(c);
	bch2_fs_stop(c);

	memzero_explicit(&key, sizeof(key));
	darray_exit(&devs);

	return 0;
}
