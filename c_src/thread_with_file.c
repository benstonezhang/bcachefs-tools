/*
 * The userspace end of a kernel `thread_with_stdio` file descriptor.
 *
 * The kernel hands out one of these whenever an operation needs to talk to a
 * person while it runs: `BCH_IOCTL_FSCK_ONLINE` returns one, and so does the
 * `status_fd` fsconfig(2) parameter on the mount path. Both ends are the same
 * shape - the fd is readable for whatever the filesystem is saying and writable
 * for the answers - so both callers want the same relay, differing only in
 * where the filesystem's side is written.
 *
 * Bytes pass through unchanged in both directions. The kernel writes its
 * questions without a trailing newline, because the answer belongs on the same
 * line, and it reads them back with a readline that waits for one - so neither
 * direction survives being tidied into lines here.
 *
 * Both fds are put in non-blocking mode so that neither direction can stall the
 * other: a question written while nobody is draining must not stop us reading
 * the answer, and vice versa. stdin's original flags are restored on the way
 * out, because it does not belong to us - the mount path has its own prompts to
 * put on it afterwards.
 *
 * A caller may also keep a live display below the conversation - mount draws
 * recovery progress there. The relay drives it rather than the other way round
 * because the relay is what knows when the filesystem is about to print, and
 * two writers taking turns with one cursor is the whole problem.
 */

#include "libbcachefs.h"
#include "tools-util.h"

#include <fcntl.h>
#include <poll.h>

/*
 * Move what's readable on @rfd to @wfd.
 *
 * Returns true on end of file, false when something moved or there was nothing
 * to move yet.
 */
static bool splice_fds(int rfd, int wfd)
{
	char buf[4096];
	ssize_t n;
	size_t off;

	n = read(rfd, buf, sizeof(buf));
	if (n == 0)
		return true;
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return false;
		die("read error: %m");
	}

	off = 0;
	while (off < (size_t)n) {
		ssize_t w = write(wfd, buf + off, n - off);

		if (w > 0) {
			off += w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd p = { .fd = wfd, .events = POLLOUT };
			poll(&p, 1, -1);
			continue;
		}
		die("write error: %m");
	}

	return false;
}

static void relay_locked(int fd, int out, int stdin_fd,
			 struct bch_status_display *display)
{
	bool stdin_closed = false;

	for (;;) {
		struct pollfd pollfds[2];
		int nfds = 1;
		int timeout = display ? display->interval_ms(display->ctx) : -1;
		bool eof;

		pollfds[0].fd		= fd;
		pollfds[0].events	= POLLIN;
		if (!stdin_closed) {
			pollfds[1].fd		= stdin_fd;
			pollfds[1].events	= POLLIN;
			nfds = 2;
		}

		(void)poll(pollfds, nfds, timeout);

		if (display)
			display->erase(display->ctx);

		eof = splice_fds(fd, out);

		// Our own end running dry is not the end of the conversation: the
		// filesystem may have a great deal left to say.
		if (!stdin_closed && splice_fds(stdin_fd, fd))
			stdin_closed = true;

		// Leave the terminal as we found it - the caller has its own
		// result to print, and it doesn't belong under a stale progress
		// block.
		if (eof)
			return;

		if (display)
			display->draw(display->ctx);
	}
}

/*
 * Carry the conversation on @fd until the kernel side is finished with it:
 * what the filesystem says goes to @out, and stdin goes back to the
 * filesystem.
 *
 * Returns when the kernel marks the channel done, which it does on every path
 * out of the operation - so this ends on its own without the caller arranging
 * it. @display may be NULL; otherwise it drives a live block below the
 * conversation.
 */
int bch_thread_relay(int fd, int out, struct bch_status_display *display)
{
	int saved;

	if (out < 0)
		return -EINVAL;

	saved = fcntl(STDIN_FILENO, F_GETFL);
	if (saved < 0)
		return -errno;
	if (fcntl(STDIN_FILENO, F_SETFL, saved | O_NONBLOCK) < 0)
		return -errno;

	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
		(void)fcntl(STDIN_FILENO, F_SETFL, saved);
		return -errno;
	}

	relay_locked(fd, out, STDIN_FILENO, display);

	// stdin does not belong to us; restore its flags even if the relay died.
	(void)fcntl(STDIN_FILENO, F_SETFL, saved);
	return 0;
}