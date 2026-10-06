/*
 * Key management and encryption helpers.
 *
 * Ported from src/key.rs.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <keyutils.h>
#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "crypto.h"
#include "data/checksum.h"
#include "sb/io.h"

struct bch_encrypted_key bch2_unencrypted_key(struct bch_key key)
{
	struct bch_encrypted_key k;
	k.magic = cpu_to_le64(BCH_KEY_MAGIC);
	k.key = key;
	return k;
}

int bch2_keyring_to_id(enum bch_keyring keyring)
{
	switch (keyring) {
	case BCH_KEYRING_session:
		return KEY_SPEC_SESSION_KEYRING;
	case BCH_KEYRING_user:
		return KEY_SPEC_USER_KEYRING;
	case BCH_KEYRING_user_session:
		return KEY_SPEC_USER_SESSION_KEYRING;
	default:
		return KEY_SPEC_USER_KEYRING;
	}
}

char *bch2_format_key_name(const __uuid_t *uuid)
{
	char uuid_str[40];
	uuid_unparse_lower(uuid->b, uuid_str);
	return mprintf("bcachefs:%s", uuid_str);
}

bool bch2_key_search(struct bch_sb *sb)
{
	char *description = bch2_format_key_name(&sb->user_uuid);
	static const int keyrings[] = {
		KEY_SPEC_SESSION_KEYRING,
		KEY_SPEC_USER_KEYRING,
		KEY_SPEC_USER_SESSION_KEYRING,
	};

	for (unsigned i = 0; i < ARRAY_SIZE(keyrings); i++) {
		if (keyctl_search(keyrings[i], "user", description, 0) > 0) {
			free(description);
			return true;
		}
	}

	free(description);
	return false;
}

void bch2_wait_for_unlock(struct bch_sb *sb)
{
	while (!bch2_key_search(sb))
		sleep(1);
}

int bch2_key_handle_new(struct bch_sb *sb, const char *passphrase,
			enum bch_keyring keyring)
{
	struct bch_key passphrase_key;
	struct bch_encrypted_key sb_key;

	if (bch2_passphrase_check(sb, passphrase, &passphrase_key, &sb_key))
		return -EINVAL;

	char *description = bch2_format_key_name(&sb->user_uuid);
	int keyring_id = bch2_keyring_to_id(keyring);

	long key_id = add_key("user", description, &passphrase_key,
			      sizeof(passphrase_key), keyring_id);

	free(description);
	memzero_explicit(&passphrase_key, sizeof(passphrase_key));

	if (key_id <= 0)
		return -errno;

	return 0;
}

int bch2_unlock_policy_apply(enum bch_unlock_policy policy,
			     struct bch_sb_handle *sb,
			     const char *passphrase_file)
{
	if (!bch2_sb_is_encrypted(sb->sb))
		return 0;

	if (bch2_key_search(sb->sb))
		return 0;

	char *passphrase = NULL;

	if (passphrase_file) {
		passphrase = read_file_str(AT_FDCWD, passphrase_file);
		if (!passphrase)
			die("Error reading passphrase file %s: %m",
			    passphrase_file);
	} else {
		switch (policy) {
		case BCH_UNLOCK_POLICY_fail:
			die("Filesystem is encrypted and no key found in keyring");
		case BCH_UNLOCK_POLICY_wait:
			bch2_wait_for_unlock(sb->sb);
			return 0;
		case BCH_UNLOCK_POLICY_ask: {
			char label[sizeof(sb->sb->label) + 1];
			size_t llen = strnlen((const char *)sb->sb->label,
					      sizeof(sb->sb->label));
			memcpy(label, sb->sb->label, llen);
			label[llen] = '\0';
			passphrase = read_passphrase_uuid(&sb->sb->user_uuid,
							  label,
							  "Enter passphrase: ");
			break;
		}
		case BCH_UNLOCK_POLICY_stdin:
			passphrase = read_passphrase("Enter passphrase: ");
			break;
		}
	}

	if (passphrase) {
		int ret = bch2_key_handle_new(sb->sb, passphrase,
					      BCH_KEYRING_user);
		memzero_explicit(passphrase, strlen(passphrase));
		free(passphrase);
		return ret;
	}

	return 0;
}
