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
#include "sb/io.h"
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

/*
 * Open a filesystem for superblock modification: never started.
 *
 * @user_key: the passphrase-derived key, for a passphrase-protected
 * filesystem. Without it the open looks in the keyring, and failing
 * that prompts for the passphrase on stdin itself (bch2_request_key()).
 *
 * Rust: open_nostart() in src/commands/key.rs.
 */
static struct bch_fs *open_nostart(darray_const_str *devs,
				   const struct bch_key *user_key)
{
	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, true);
	opt_set(opts, will_not_start, true);

	struct bch_fs *c = bch2_fs_open(devs, &opts, user_key);
	if (IS_ERR(c))
		die("Error opening devices: %s", bch2_err_str(PTR_ERR(c)));
	return c;
}

/*
 * Open filesystem, verify encryption is enabled, and obtain the raw key.
 *
 * If the key is encrypted (passphrase-protected), prompts for and verifies
 * the current passphrase. If the key is unencrypted (formatted with
 * --no_passphrase), reads the raw key directly.
 *
 * Asks before opening, from a superblock read ahead: opening a
 * passphrase-protected filesystem needs the key, and without one the open
 * asks for the passphrase itself - reading the line from stdin that we were
 * going to read, so ours got EOF.
 *
 * Rust: open_and_verify() in src/commands/key.rs.
 */
static struct bch_fs *open_and_verify(darray_const_str *devs,
				      struct bch_key *raw_key)
{
	/* Rust: scan_sbs() expands a lone device into the rest of the fs */
	if (devs->nr == 1) {
		char *scanned = bch2_scan_devices(devs->data[0]);

		if (scanned) {
			darray_const_str members = {};

			if (!bch2_split_devs(scanned, &members)) {
				darray_exit(devs);
				*devs = members;
			} else {
				darray_exit(&members);
			}
			free(scanned);
		}
	}

	struct bch_sb_handle sb = { NULL };
	struct bch_opts scan_opts = bch2_opts_empty();

	int ret = bch2_read_super(devs->data[0], &scan_opts, &sb);
	if (ret)
		die("Error opening %s: %s", devs->data[0], bch2_err_str(ret));

	struct bch_sb_field_crypt *crypt = bch2_sb_field_get(sb.sb, crypt);
	if (!crypt) {
		bch2_free_super(&sb);
		die("Filesystem does not have encryption enabled");
	}

	struct bch_key passphrase_key = {};
	struct bch_key *user_key = NULL;

	if (bch2_key_is_encrypted(&crypt->key)) {
		char label[sizeof(sb.sb->label) + 1];
		size_t llen = strnlen((const char *) sb.sb->label,
				      sizeof(sb.sb->label));
		memcpy(label, sb.sb->label, llen);
		label[llen] = '\0';

		char *passphrase = read_passphrase_uuid(&sb.sb->user_uuid, label,
							"Enter passphrase: ");
		struct bch_encrypted_key cleartext_sb_key;
		bool correct = bch2_passphrase_check(sb.sb, passphrase,
						     &passphrase_key,
						     &cleartext_sb_key);

		memzero_explicit(passphrase, strlen(passphrase));
		free(passphrase);

		if (!correct) {
			memzero_explicit(&passphrase_key, sizeof(passphrase_key));
			bch2_free_super(&sb);
			die("incorrect passphrase");
		}

		*raw_key = cleartext_sb_key.key;
		memzero_explicit(&cleartext_sb_key, sizeof(cleartext_sb_key));
		user_key = &passphrase_key;
	} else {
		*raw_key = crypt->key.key;
	}

	bch2_free_super(&sb);

	struct bch_fs *c = open_nostart(devs, user_key);

	memzero_explicit(&passphrase_key, sizeof(passphrase_key));
	return c;
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

	struct bch_key raw_key;
	struct bch_fs *c = open_and_verify(&devs, &raw_key);

	struct bch_sb *sb = c->disk_sb.sb;
	struct bch_sb_field_crypt *crypt = bch2_sb_field_get(sb, crypt);
	if (!crypt)
		die("Filesystem does not have encryption enabled");

	char *new_passphrase = read_passphrase_twice("Enter new passphrase: ");

	/*
	 * bch_crypt_update_passphrase() wraps @raw_key with the new passphrase,
	 * initializing the KDF first when the key is still unencrypted:
	 * Rust's init_crypt_kdf() + Passphrase::encrypt_key().
	 *
	 * Rust: set_crypt_key() (caller holds sb_lock) + bch2_revoke_key() +
	 * write_crypt_super().
	 */
	{
		guard(mutex_noio)(&c->sb_lock);
		bch_crypt_update_passphrase(sb, crypt, &raw_key, new_passphrase);
		bch2_revoke_key(c->disk_sb.sb);

		int ret = bch2_write_super(c);
		if (ret)
			die("error writing superblock: %s", bch2_err_str(ret));
	}

	bch2_fs_stop(c);

	memzero_explicit(new_passphrase, strlen(new_passphrase));
	free(new_passphrase);
	memzero_explicit(&raw_key, sizeof(raw_key));
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

	struct bch_key raw_key;
	struct bch_fs *c = open_and_verify(&devs, &raw_key);

	struct bch_sb *sb = c->disk_sb.sb;
	struct bch_sb_field_crypt *crypt = bch2_sb_field_get(sb, crypt);
	if (!crypt)
		die("Filesystem does not have encryption enabled");

	/*
	 * Rust: set_crypt_key(&fs, bch_encrypted_key::new_unencrypted(raw_key))
	 * (caller holds sb_lock) + write_crypt_super().
	 */
	{
		guard(mutex_noio)(&c->sb_lock);
		crypt->key = bch2_unencrypted_key(raw_key);

		int ret = bch2_write_super(c);
		if (ret)
			die("error writing superblock: %s", bch2_err_str(ret));
	}

	bch2_fs_stop(c);

	memzero_explicit(&raw_key, sizeof(raw_key));
	darray_exit(&devs);

	return 0;
}
