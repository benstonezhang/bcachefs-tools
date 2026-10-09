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
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

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

/*
 * Derive and verify the key a passphrase produces. Returns whether the
 * passphrase was right, and fills @out either way - a wrong passphrase leaves a
 * key that simply will not decrypt the superblock key.
 */
bool bch2_passphrase_correct(struct bch_sb *sb, const char *passphrase,
			     struct bch_passphrase_correct *out)
{
	if (!bch2_passphrase_check(sb, passphrase,
				   &out->passphrase_key, &out->cleartext_sb_key))
		return false;

	memcpy(&out->uuid, &sb->user_uuid, sizeof(out->uuid));
	return true;
}

/*
 * From a line somebody typed or sent. The line ending is theirs, not part of
 * the passphrase - and it is CR, LF or both, because an initramfs console that
 * nothing has configured sends CR for enter.
 */
static void trim_line_ending(char *line)
{
	size_t len = strlen(line);

	while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';
}

/*
 * Prompt for a passphrase with echo disabled. Returns a newly allocated line,
 * or NULL when @watch answered before anything was typed.
 */
static char *read_from_terminal(const char *prompt,
				const struct bch_prompt_watch *watch)
{
	struct termios old, new;
	char *line = NULL;
	size_t cap = 0;
	ssize_t read_len = 0;
	int waited;

	if (tcgetattr(STDIN_FILENO, &old))
		die("error getting terminal attrs");

	new = old;
	/*
	 * We may be prompting on an early-boot console (initramfs) that no
	 * shell has ever configured: without ICRNL, enter sends '\r', which
	 * getline() doesn't terminate on - keystrokes appear eaten, and the
	 * eventually-assembled passphrase has embedded '\r's and is rejected.
	 * Ensure line-input sanity rather than inheriting it:
	 */
	new.c_lflag &= ~ECHO;
	new.c_lflag |= ICANON;
	new.c_iflag |= ICRNL;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new))
		die("error setting terminal attrs");

	fprintf(stderr, "%s", prompt);
	fflush(stderr);

	/*
	 * ICANON above is what makes this safe to poll: stdin only becomes
	 * readable once a whole line has been typed, so the read below has one
	 * waiting and does not block either.
	 */
	waited = bch_prompt_wait(STDIN_FILENO, 0, watch);

	if (waited == BCH_PROMPT_WAITED_READABLE)
		read_len = getline(&line, &cap, stdin);

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &old))
		die("error setting terminal attrs");
	fprintf(stderr, "\n");

	if (waited < 0)
		die("error waiting for passphrase: %m");

	if (waited != BCH_PROMPT_WAITED_READABLE) {
		free(line);
		return NULL;
	}

	/*
	 * End of file, not an empty passphrase: stdin closed, or input that
	 * arrived before the prompt and was discarded by the flush above.
	 * Calling that a wrong passphrase sends whoever hits it looking for
	 * the wrong thing entirely.
	 */
	if (read_len <= 0)
		die("no passphrase read: end of file on stdin");

	trim_line_ending(line);
	return line;
}

/* Blocks indefinitely if no input is available on stdin. */
static char *read_from_stdin(void)
{
	char *line = NULL;
	size_t cap = 0;

	fprintf(stderr, "Trying to read passphrase from stdin...\n");

	/*
	 * Zero bytes is end of file, not an empty passphrase: whoever piped us
	 * nothing would otherwise be told their passphrase was wrong.
	 */
	if (getline(&line, &cap, stdin) <= 0) {
		free(line);
		die("no passphrase read: end of file on stdin");
	}

	trim_line_ending(line);
	return line;
}

/*
 * Ask systemd to put the question to whatever can answer it, and check every
 * passphrase it hands back. Returns 0 on success, and dies otherwise.
 */
static int ask_from_systemd_and_check(struct bch_sb_handle *sb,
				      struct bch_passphrase_correct *out)
{
	struct bch_sb *s = sb->sb;
	char label[sizeof(s->label) + 1];
	char uuid[40];
	char message[sizeof(label) + 64];
	char id[sizeof(uuid) + 32];

	bch_prompt_fs_name(s, label, sizeof(label));
	uuid_unparse_lower(s->user_uuid.b, uuid);
	snprintf(message, sizeof(message),
		 "Please enter passphrase for disk %s:", label);
	snprintf(id, sizeof(id), "--id=cryptsetup:UUID=%s", uuid);

	for (unsigned attempt = 0; attempt < 3; attempt++) {
		const char *argv[16];
		unsigned i = 0;
		int pipefd[2];
		pid_t pid;
		int status;
		char *buf = NULL;
		size_t cap = 0, len = 0;

		argv[i++] = "systemd-ask-password";
		/*
		 * Reached either with no tty, where this is a no-op, or with
		 * one plymouth is covering, where it is what stops us
		 * prompting onto the invisible terminal we just declined to
		 * use.
		 */
		argv[i++] = "--no-tty";
		argv[i++] = "--icon=drive-harddisk";
		argv[i++] = id;
		argv[i++] = "--keyname=cryptsetup";
		argv[i++] = "--credential=cryptsetup.passphrase";
		argv[i++] = "--timeout=0";
		argv[i++] = "--multiple";
		argv[i++] = "-n";
		if (!attempt)
			argv[i++] = "--accept-cached";
		argv[i++] = message;
		argv[i++] = NULL;

		if (pipe(pipefd))
			die("pipe error: %m");

		pid = fork();
		if (pid < 0)
			die("fork error: %m");

		if (!pid) {
			close(pipefd[0]);
			if (dup2(pipefd[1], STDOUT_FILENO) < 0)
				_exit(127);
			close(pipefd[1]);
			execvp(argv[0], (char **)argv);
			_exit(127);
		}

		close(pipefd[1]);

		for (;;) {
			if (len == cap) {
				cap = cap ? cap * 2 : 4096;
				buf = xrealloc(buf, cap);
			}

			ssize_t nr = read(pipefd[0], buf + len, cap - len);
			if (nr < 0) {
				if (errno == EINTR)
					continue;
				die("read error: %m");
			}
			if (!nr)
				break;
			len += nr;
		}

		close(pipefd[0]);

		if (waitpid(pid, &status, 0) < 0)
			die("waitpid error: %m");

		if (!WIFEXITED(status) || WEXITSTATUS(status))
			die("systemd-ask-password returned an error");

		bool found = false;

		for (size_t off = 0; off < len; ) {
			size_t end = off;

			while (end < len && buf[end])
				end++;

			if (bch2_passphrase_correct(s, buf + off, out)) {
				found = true;
				break;
			}

			off = end + 1;
		}

		if (buf) {
			memzero_explicit(buf, len);
			free(buf);
		}

		if (found)
			return 0;
	}

	die("incorrect passphrase limit reached");
}

int bch2_passphrase_ask_and_check(struct bch_sb_handle *sb,
				  struct bch_unlock_socket *socket,
				  struct bch_passphrase_correct *out)
{
	const struct bch_prompt_watch *watch =
		socket ? bch2_unlock_socket_watch(socket) : NULL;
	char *passphrase = NULL;

	if (isatty(STDIN_FILENO)) {
		/*
		 * A terminal plymouth is drawing over shows nothing, and this
		 * prompt has no timeout - it waits there for a person who cannot
		 * see it. Which way a question should go when that happens is
		 * bch_prompt_detect()'s decision, and asking it is not the same
		 * as copying it: it answers AGENT for a covered terminal only
		 * where there is an agent framework to reach plymouth by, and
		 * TERMINAL where there isn't - an initramfs that isn't systemd,
		 * which leaves the invisible terminal as the only thing we have.
		 */
		if (bch_prompt_detect() == BCH_PROMPT_AGENT)
			return ask_from_systemd_and_check(sb, out);

		passphrase = read_from_terminal("Enter passphrase: ", watch);
	} else if (bch_prompt_stdin_is_dev_null()) {
		return ask_from_systemd_and_check(sb, out);
	} else {
		passphrase = read_from_stdin();
	}

	if (passphrase) {
		bool ok = bch2_passphrase_correct(sb->sb, passphrase, out);

		memzero_explicit(passphrase, strlen(passphrase));
		free(passphrase);

		if (ok)
			return 0;
	}

	/*
	 * Nothing typed was right - but the socket answers by ending the
	 * prompt, so a passphrase waiting there is why we are here.
	 */
	if (socket && bch2_unlock_socket_take(socket, out))
		return 0;

	die("incorrect passphrase");
}

int bch2_passphrase_read_from_file(const char *path, struct bch_sb *sb,
				   struct bch_passphrase_correct *out)
{
	fprintf(stderr, "Attempting to unlock key with passphrase from file %s\n",
		path);

	char *passphrase = read_file_str(AT_FDCWD, path);

	if (!passphrase)
		return -errno;

	trim_line_ending(passphrase);

	bool ok = bch2_passphrase_correct(sb, passphrase, out);

	memzero_explicit(passphrase, strlen(passphrase));
	free(passphrase);

	return ok ? 0 : -EINVAL;
}

int bch2_unlock_policy_apply(enum bch_unlock_policy policy,
			     struct bch_sb_handle *sb,
			     struct bch_unlocked *out)
{
	struct bch_sb *s = sb->sb;
	char uuid[40];
	static const char *names[] = { "fail", "wait", "ask", "stdin" };

	uuid_unparse_lower(s->user_uuid.b, uuid);
	fprintf(stderr, "Using filesystem unlock policy '%s' on %s\n",
		names[policy], uuid);

	switch (policy) {
	case BCH_UNLOCK_POLICY_fail:
		/* Somebody else's key, in a keyring: we never see the bytes. */
		if (!bch2_key_search(s))
			die("incorrect passphrase");
		out->have_key = false;
		return 0;
	case BCH_UNLOCK_POLICY_wait:
		bch2_wait_for_unlock(s);
		out->have_key = false;
		return 0;
	case BCH_UNLOCK_POLICY_ask: {
		char *passphrase = read_from_terminal("Enter passphrase: ", NULL);
		bool ok = passphrase &&
			  bch2_passphrase_correct(s, passphrase, &out->correct);

		if (passphrase) {
			memzero_explicit(passphrase, strlen(passphrase));
			free(passphrase);
		}

		if (!ok)
			die("incorrect passphrase");

		out->have_key = true;
		return 0;
	}
	case BCH_UNLOCK_POLICY_stdin: {
		char *passphrase = read_from_stdin();
		bool ok = bch2_passphrase_correct(s, passphrase, &out->correct);

		memzero_explicit(passphrase, strlen(passphrase));
		free(passphrase);

		if (!ok)
			die("incorrect passphrase");

		out->have_key = true;
		return 0;
	}
	default:
		die("unknown unlock policy");
	}
}

/*
 * The key as the kernel's `user_key` mount parameter wants it, if we have it to
 * give.
 *
 * The same bytes add_key() would have put in a keyring: both ends treat a
 * struct bch_key as an opaque block, so there is no byte order here to get
 * wrong.
 */
char *bch2_unlocked_hex(const struct bch_unlocked *unlocked)
{
	if (!unlocked->have_key)
		return NULL;

	const unsigned char *bytes =
		(const unsigned char *)&unlocked->correct.passphrase_key;
	size_t n = sizeof(unlocked->correct.passphrase_key);
	char *hex = xmalloc(n * 2 + 1);

	for (size_t i = 0; i < n; i++)
		sprintf(hex + i * 2, "%02x", bytes[i]);

	return hex;
}

/*
 * Put the key where bch2_request_key() will look, for the mount(2) path - that
 * has nowhere to carry a parameter, so the keyring is the only channel.
 *
 * The kernel searches the invoking task's keyring tree, and a fresh session
 * need not link the user keyring: add it to the session keyring the mount
 * syscall will run under, and to the user keyring as before, for anything after
 * us that expects it there.
 */
int bch2_unlocked_to_keyring(const struct bch_unlocked *unlocked)
{
	if (!unlocked->have_key)
		return 0;

	char *description = bch2_format_key_name(&unlocked->correct.uuid);
	long user, session;

	user = add_key("user", description, &unlocked->correct.passphrase_key,
		       sizeof(unlocked->correct.passphrase_key),
		       KEY_SPEC_USER_KEYRING);
	session = add_key("user", description, &unlocked->correct.passphrase_key,
			  sizeof(unlocked->correct.passphrase_key),
			  KEY_SPEC_SESSION_KEYRING);

	free(description);

	return (user > 0 || session > 0) ? 0 : -errno;
}
