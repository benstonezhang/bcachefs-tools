/*
 * Key management and encryption helpers.
 *
 * Key detail: we prioritize retrieving the passphrase from the kernel
 * keyring (bcachefs:UUID) if it exists, followed by systemd-ask-password,
 * and finally falling back to a manual terminal prompt.
 *
 * Ported from src/key.rs.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <keyutils.h>
#include <linux/random.h>
#include <sodium/crypto_pwhash_scryptsalsa208sha256.h>
#include <uuid/uuid.h>

#include "crypto.h"

#include "data/checksum.h"

static char *read_passphrase_ask_password(const char *uuid, const char *label)
{
	/*
	 * Share keyname/credential with cryptsetup so a passphrase entered
	 * for either tool can be reused from the kernel keyring / credentials.
	 * Retries without --accept-cached after the first attempt.
	 */
	const char *disp = (label && *label) ? label : uuid;

	for (int i = 0; i < 3; i++) {
		char *cmd = mprintf(
			"systemd-ask-password --icon=drive-harddisk "
			"--id=cryptsetup:UUID=%s --keyname=cryptsetup "
			"--credential=cryptsetup.passphrase --timeout=0 "
			"--multiple -n%s 'Please enter passphrase for disk %s:'",
			uuid, i == 0 ? " --accept-cached" : "", disp);

		FILE *f = popen(cmd, "r");
		free(cmd);
		if (!f)
			continue;

		char *buf = NULL;
		size_t buflen = 0;
		if (getline(&buf, &buflen, f) < 0) {
			free(buf);
			buf = NULL;
		}
		int status = pclose(f);
		if (status || !buf) {
			free(buf);
			continue;
		}

		size_t len = strlen(buf);
		if (len && buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		return buf;
	}

	return NULL;
}

static bool is_dev_null(int fd)
{
	struct stat st;
	if (fstat(fd, &st))
		return false;
	return S_ISCHR(st.st_mode) && major(st.st_rdev) == 1 &&
	       minor(st.st_rdev) == 3;
}

char *read_passphrase_uuid(const __uuid_t *user_uuid, const char *label,
			   const char *prompt)
{
	char uuid[40];
	uuid_unparse_lower(user_uuid->b, uuid);

	if (isatty(STDIN_FILENO)) {
		char *pass = read_passphrase_ask_password(uuid, label);
		if (pass)
			return pass;
	} else if (is_dev_null(STDIN_FILENO)) {
		return read_passphrase_ask_password(uuid, label);
	}

	return read_passphrase(prompt);
}

char *read_passphrase(const char *prompt)
{
	char *buf = NULL;
	size_t buflen = 0;
	ssize_t len;

	if (isatty(STDIN_FILENO)) {
		struct termios old, new;

		fprintf(stderr, "%s", prompt);
		fflush(stderr);

		if (tcgetattr(STDIN_FILENO, &old))
			die("error getting terminal attrs");

		new = old;
		/*
		 * We may be prompting on an early-boot console (initramfs) that
		 * no shell has ever configured: without ICRNL, enter sends '\r',
		 * which getline() doesn't terminate on - keystrokes appear eaten,
		 * and the eventually-assembled passphrase has embedded '\r's and
		 * is rejected. Ensure line-input sanity rather than inheriting it:
		 */
		new.c_iflag |= ICRNL;
		new.c_lflag |= ICANON;
		new.c_lflag &= ~ECHO;
		if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new))
			die("error setting terminal attrs");

		len = getline(&buf, &buflen, stdin);

		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		fprintf(stderr, "\n");
	} else {
		len = getline(&buf, &buflen, stdin);
	}

	if (len < 0)
		die("error reading passphrase");
	if (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[len - 1] = '\0';

	return buf;
}

char *read_passphrase_twice(const char *prompt)
{
	char *pass = read_passphrase(prompt);

	if (!isatty(STDIN_FILENO))
		return pass;

	char *pass2 = read_passphrase("Enter same passphrase again: ");

	if (strcmp(pass, pass2)) {
		memzero_explicit(pass, strlen(pass));
		memzero_explicit(pass2, strlen(pass2));
		die("Passphrases do not match");
	}

	memzero_explicit(pass2, strlen(pass2));
	free(pass2);

	return pass;
}

struct bch_key derive_passphrase(struct bch_sb_field_crypt *crypt,
				 const char *passphrase)
{
	const unsigned char salt[] = "bcache";
	struct bch_key key;
	int ret;

	switch (BCH_CRYPT_KDF_TYPE(crypt)) {
	case BCH_KDF_SCRYPT:
		ret = crypto_pwhash_scryptsalsa208sha256_ll(
			(void *)passphrase, strlen(passphrase),
			salt, sizeof(salt),
			1ULL << BCH_KDF_SCRYPT_N(crypt),
			1ULL << BCH_KDF_SCRYPT_R(crypt),
			1ULL << BCH_KDF_SCRYPT_P(crypt),
			(void *)&key, sizeof(key));
		if (ret)
			die("scrypt error: %i", ret);
		break;
	default:
		die("unknown kdf type %llu", BCH_CRYPT_KDF_TYPE(crypt));
	}

	return key;
}

bool bch2_passphrase_check(struct bch_sb *sb, const char *passphrase,
			   struct bch_key *passphrase_key,
			   struct bch_encrypted_key *sb_key)
{
	struct bch_sb_field_crypt *crypt = bch2_sb_field_get(sb, crypt);
	if (!crypt)
		die("filesystem is not encrypted");

	*sb_key = crypt->key;

	if (!bch2_key_is_encrypted(sb_key))
		die("filesystem does not have encryption key");

	*passphrase_key = derive_passphrase(crypt, passphrase);

	bch2_chacha20(passphrase_key, __bch2_sb_key_nonce(sb), sb_key,
		      sizeof(*sb_key));

	if (bch2_key_is_encrypted(sb_key))
		return true;

	return false;
}

bool bch2_add_key(struct bch_sb *sb, const char *type, const char *keyring_str,
		  const char *passphrase)
{
	struct bch_key passphrase_key;
	struct bch_encrypted_key sb_key;
	int keyring;

	if (!strcmp(keyring_str, "session"))
		keyring = KEY_SPEC_SESSION_KEYRING;
	else if (!strcmp(keyring_str, "user"))
		keyring = KEY_SPEC_USER_KEYRING;
	else if (!strcmp(keyring_str, "user_session"))
		keyring = KEY_SPEC_USER_SESSION_KEYRING;
	else
		die("unknown keyring %s", keyring_str);

	if (bch2_passphrase_check(sb, passphrase, &passphrase_key, &sb_key))
		return true;

	char uuid[40];
	uuid_unparse_lower(sb->user_uuid.b, uuid);

	char *description = mprintf("bcachefs:%s", uuid);

	if (add_key(type, description, &passphrase_key, sizeof(passphrase_key),
		    keyring) < 0)
		die("add_key error: %m");

	memzero_explicit(description, strlen(description));
	free(description);
	memzero_explicit(&passphrase_key, sizeof(passphrase_key));
	memzero_explicit(&sb_key, sizeof(sb_key));

	return false;
}

void bch_sb_crypt_init(struct bch_sb *sb, struct bch_sb_field_crypt *crypt,
		       const char *passphrase)
{
	struct bch_key key;
	get_random_bytes(&key, sizeof(key));

	crypt->key.magic = cpu_to_le64(BCH_KEY_MAGIC);
	crypt->key.key = key;

	bch_crypt_update_passphrase(sb, crypt, &key, passphrase);
}

void bch_crypt_update_passphrase(struct bch_sb *sb,
				 struct bch_sb_field_crypt *crypt,
				 struct bch_key *key,
				 const char *new_passphrase)
{
	struct bch_encrypted_key new_key;
	new_key.magic = BCH_KEY_MAGIC;
	new_key.key = *key;

	if (!new_passphrase) {
		crypt->key = new_key;
		return;
	}

	// If crypt already has an encrypted key reuse it's encryption params
	if (!bch2_key_is_encrypted(&crypt->key)) {
		SET_BCH_CRYPT_KDF_TYPE(crypt, BCH_KDF_SCRYPT);
		SET_BCH_KDF_SCRYPT_N(crypt, ilog2(16384));
		SET_BCH_KDF_SCRYPT_R(crypt, ilog2(8));
		SET_BCH_KDF_SCRYPT_P(crypt, ilog2(16));
	}

	struct bch_key passphrase_key =
		derive_passphrase(crypt, new_passphrase);

	bch2_chacha20(&passphrase_key, __bch2_sb_key_nonce(sb), &new_key,
		      sizeof(new_key));

	memzero_explicit(&passphrase_key, sizeof(passphrase_key));

	crypt->key = new_key;
	assert(bch2_key_is_encrypted(&crypt->key));
}
