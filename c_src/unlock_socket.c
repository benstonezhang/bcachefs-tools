// SPDX-License-Identifier: GPL-2.0
/*
 * A second way to answer the passphrase prompt.
 *
 * Ported from src/unlock_socket.rs.
 *
 * While mount.bcachefs is waiting at the terminal for a passphrase, it also
 * listens on a unix socket, and takes whichever arrives first. That is what
 * makes remote unlock work without a second mechanism: an initramfs blocks on
 * the prompt as it always did, and someone who has sshed in over dropbear
 * writes the passphrase to the socket instead of walking to the machine.
 *
 * Why a socket rather than a fifo, which is what people build by hand: a
 * connection gives us somewhere to put the answer. A fifo takes the passphrase
 * and tells you nothing, so a typo over ssh looks exactly like a working
 * unlock until the boot fails. Here a wrong passphrase gets told so, and - the
 * point of checking here rather than upstack - it does not disturb the
 * terminal prompt at all. Somebody fat-fingering it remotely cannot blow away
 * what you have half-typed at the console, and can try again.
 *
 * There is no client. The protocol is a passphrase and a newline in, one
 * status line out, deliberately: in an initramfs with nothing installed,
 * `socat - UNIX-CONNECT:...` has to be enough.
 *
 * It is a passphrase oracle, so the directory is 0700 and root-only. It is
 * also strictly an addition - if we cannot create it we say so and go on
 * prompting, because failing a mount over a convenience would be absurd.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "libbcachefs.h"

#define UNLOCK_SOCKET_DIR "/run/bcachefs/unlock"

struct bch_unlock_socket {
	int			listener;
	char			*path;
	struct bch_sb_handle	*sb;
	bool			bound;
	/* The one that checked out, waiting for take(). */
	struct bch_passphrase_correct got;
	bool			have_got;
	struct bch_prompt_watch watch;
};

static int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0)
		return -errno;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -errno;
	return 0;
}

static void trim_line_ending(char *line)
{
	size_t len = strlen(line);

	while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';
}

/*
 * A socket left behind by a mount that died holds the address, and we would
 * rather listen than refuse. Connecting is what distinguishes a corpse from a
 * live mount also prompting for this filesystem - only the corpse refuses.
 */
static int clear_stale(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (access(path, F_OK))
		return 0;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	int connected = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	close(fd);

	if (!connected)
		return -EADDRINUSE;

	if (unlink(path))
		return -errno;

	return 0;
}

/*
 * One connection: a passphrase, a verdict, and the connection is done. We
 * never hold it open - the caller is a shell script with socat, and having to
 * know when to hang up would be another thing to get wrong.
 */
static bool serve(struct bch_unlock_socket *s, int conn)
{
	char line[4096];
	ssize_t nr;
	char *nl;
	bool ok;
	struct bch_passphrase_correct correct;

	nr = read(conn, line, sizeof(line) - 1);
	if (nr < 0)
		nr = 0;
	line[nr] = '\0';

	/* Only the first line is the passphrase; the rest is the caller's. */
	nl = strchr(line, '\n');
	if (nl)
		*nl = '\0';
	trim_line_ending(line);

	ok = bch2_passphrase_correct(s->sb->sb, line, &correct);

	if (ok) {
		if (write(conn, "ok\n", 3) < 0)
			;
		s->got = correct;
		s->have_got = true;
	} else {
		/*
		 * Say so, and stay up: whoever typed it is right there and can
		 * try again, which is the whole reason this is a socket.
		 */
		if (write(conn, "incorrect passphrase\n", 21) < 0)
			;
		fprintf(stderr, "remote unlock: incorrect passphrase, still listening\n");
	}

	memzero_explicit(line, sizeof(line));
	return ok;
}

static int unlock_socket_raw_fd(void *ctx)
{
	return ((struct bch_unlock_socket *)ctx)->listener;
}

static enum bch_prompt_stirred unlock_socket_stirred(void *ctx)
{
	struct bch_unlock_socket *s = ctx;
	struct sockaddr_un addr;
	socklen_t addrlen = sizeof(addr);
	int conn;
	bool ok;

	/*
	 * Non-blocking, so a spurious wakeup costs us an EAGAIN and nothing
	 * else. A connection that goes wrong is that caller's problem, not a
	 * reason to stop listening or to disturb the prompt.
	 */
	conn = accept(s->listener, (struct sockaddr *)&addr, &addrlen);
	if (conn < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			fprintf(stderr, "remote unlock: accept failed: %m\n");
		return BCH_PROMPT_STIR_NOTHING;
	}

	ok = serve(s, conn);
	close(conn);

	return ok ? BCH_PROMPT_STIR_ANSWERED : BCH_PROMPT_STIR_NOTHING;
}

/*
 * Where to tell someone to write. One per filesystem: several can be prompting
 * at once at boot, and they want different passphrases.
 */
static char *unlock_socket_path(struct bch_sb_handle *sb)
{
	char uuid[40];

	uuid_unparse_lower(sb->sb->user_uuid.b, uuid);
	return mprintf("%s/%s", UNLOCK_SOCKET_DIR, uuid);
}

/*
 * NULL if we could not get a socket up - the caller prompts without one,
 * exactly as it did before there was a socket to want.
 *
 * Not a warning. Nothing is wrong: an encrypted filesystem mounted somewhere
 * /run is absent or read-only would otherwise say so on every mount, about a
 * facility whoever is mounting never asked for.
 */
struct bch_unlock_socket *bch2_unlock_socket_open(struct bch_sb_handle *sb)
{
	struct bch_unlock_socket *s = xcalloc(1, sizeof(*s));
	struct sockaddr_un addr;
	int saved_errno;

	s->listener = -1;
	s->sb = sb;
	s->path = unlock_socket_path(sb);

	if (mkdir(UNLOCK_SOCKET_DIR, 0700) && errno != EEXIST)
		goto err;

	/*
	 * The directory is the gate, not the socket: bind(2) takes the mode
	 * from the umask and there is no atomic way to hand it one, so a socket
	 * briefly readable by the world would be a real window. A 0700
	 * directory closes it without needing the socket to be anything.
	 */
	if (chmod(UNLOCK_SOCKET_DIR, 0700))
		goto err;

	if (clear_stale(s->path))
		goto err;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(s->path) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		goto err;
	}
	strcpy(addr.sun_path, s->path);

	s->listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s->listener < 0)
		goto err;
	if (bind(s->listener, (struct sockaddr *)&addr, sizeof(addr)))
		goto err;
	s->bound = true;

	if (set_nonblocking(s->listener))
		goto err;

	s->watch.raw_fd	 = unlock_socket_raw_fd;
	s->watch.stirred = unlock_socket_stirred;
	s->watch.ctx	 = s;

	fprintf(stderr, "remote unlock: write the passphrase to %s\n", s->path);
	return s;
err:
	saved_errno = errno;
	fprintf(stderr,
		"no remote unlock socket (%s); the prompt is the only way in\n",
		strerror(saved_errno));
	bch2_unlock_socket_free(s);
	return NULL;
}

const struct bch_prompt_watch *bch2_unlock_socket_watch(struct bch_unlock_socket *s)
{
	return &s->watch;
}

/* The passphrase that checked out, once Watch::stirred has said one did. */
bool bch2_unlock_socket_take(struct bch_unlock_socket *s,
			     struct bch_passphrase_correct *out)
{
	if (!s->have_got)
		return false;

	*out = s->got;
	s->have_got = false;
	return true;
}

void bch2_unlock_socket_free(struct bch_unlock_socket *s)
{
	if (!s)
		return;

	if (s->listener >= 0)
		close(s->listener);

	if (s->bound)
		unlink(s->path);

	memzero_explicit(&s->got, sizeof(s->got));
	free(s->path);
	free(s);
}