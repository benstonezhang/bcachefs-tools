/*
 * Talking to the boot splash.
 *
 * Two things in the mount path care about plymouth and for opposite reasons:
 * the status display wants to draw *on* it, and the prompt wants to know it is
 * there so as not to put a question *under* it. They asked separately, and one
 * of them asked a different question than it meant to - so one module knows
 * about plymouth and both callers ask it.
 *
 * The protocol is ply-boot-protocol.h: a command byte, a flag byte, a length
 * byte counting the NUL, then the text. The length being a single byte is
 * where MAX comes from, and plymouth asserts it rather than checking, so
 * callers truncate before asking rather than after being told.
 */

#include "libbcachefs.h"
#include "tools-util.h"

#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#define PLYMOUTH_SOCKET		"/org/freedesktop/plymouthd"
#define PLYMOUTH_MAX		254
#define PLYMOUTH_PROTO		0x02

static int plymouth_connect(void)
{
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};
	int fd;
	size_t path_len = strlen(PLYMOUTH_SOCKET);

	strncpy(addr.sun_path + 1, PLYMOUTH_SOCKET, sizeof(addr.sun_path) - 2);

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	if (connect(fd, (void *)&addr,
		    offsetof(struct sockaddr_un, sun_path) + 1 + path_len) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

/*
 * As much of @s as will fit, cut on a character boundary.
 */
static size_t plymouth_truncate_len(const char *s, size_t max)
{
	size_t len = strlen(s);

	if (len <= max)
		return len;

	len = max;
	while (len && ((unsigned char)s[len] & 0xc0) == 0x80)
		len--;
	return len;
}

/*
 * One request and nothing else: command, 0x02, a length byte counting the
 * NUL, the text, the NUL. That is the whole of what plymouth's own client
 * puts on the wire for a display-message.
 *
 * Nothing may be appended. Bytes after the NUL are parsed as a further
 * request, and the tempting one - PROGRESS_UNPAUSE, "a" - is a trap:
 * ply_progress_unpause() does `start_time += now - pause_time`, and
 * pause_time is zero unless PROGRESS_PAUSE was sent, so each one shifts
 * plymouth's clock forward by the entire current time. At one message per
 * frame that destroys the splash's own progress estimate, which is a
 * division by that elapsed time. display-message does not touch progress,
 * so there was never anything to unpause.
 *
 * The limit is enforced here rather than asked of callers. It used to be a
 * line in the doc comment, and the one caller kept it by dropping any line
 * that would not fit - so a single long line left nothing to send, and an
 * empty display-message is not "no room", it is the one that *clears the
 * splash*. Truncating is both the right answer and the one that cannot be
 * forgotten somewhere else later.
 */
void bch_plymouth_send(const char *text)
{
	char msg[1 + 1 + 1 + PLYMOUTH_MAX + 1];
	size_t len = plymouth_truncate_len(text, PLYMOUTH_MAX);
	int fd = plymouth_connect();

	if (fd < 0)
		return;

	msg[0] = 'M';
	msg[1] = PLYMOUTH_PROTO;
	msg[2] = len + 1;
	memcpy(msg + 3, text, len);
	msg[3 + len] = 0;

	len += 4;
	while (len) {
		ssize_t w = write(fd, msg, len);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (w == 0)
			break;
		len -= w;
	}
	close(fd);
}

/*
 * Is plymouth drawing over the console?
 *
 * `plymouth --ping` is the canonical test and exits non-zero when it isn't
 * running; not installed means not covering us, so a spawn failure is the same
 * answer.
 */
bool bch_plymouth_active(void)
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return false;

	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);

		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2)
				close(devnull);
		}
		execlp("plymouth", "plymouth", "--ping", NULL);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0)
		return false;

	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}