/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * The block mount.bcachefs draws while a filesystem is coming up.
 *
 * Until the mount returns there is no sysfs and nothing in its own output to
 * say whether recovery is working or wedged; BCH_IOCTL_RECOVERY_STATUS is the
 * only thing that can say.
 *
 * Whether we draw decides whether we poll at all: the kernel stops logging
 * progress to dmesg as soon as anything reads that ioctl, on the grounds that
 * whoever read it is showing it.
 *
 * poll() on the status fd means "text to read", not "progress moved", so the
 * numbers are one interval stale.
 */

#include "libbcachefs.h"
#include "tools-util.h"
#include "init/passes.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* systemd repaints its own job line on the same row every 333ms
 * (JOBS_IN_PROGRESS_PERIOD_USEC), and nothing arbitrates. 50ms is what
 * systemd-fsck uses to win that race. */
#define INTERVAL_MS	50
#define BAR_WIDTH	20

/* Every line is truncated to the width: a wrapped line occupies two rows, and
 * the redraw's cursor arithmetic is off by one from then on. */
#define FALLBACK_COLS	80
#define MIN_COLS	20

/*
 * In descending order of how many rows we get. The console gets one, because
 * systemd is drawing its own self-overwriting line there and cannot be told to
 * make room - there is no way for a unit to hand systemd a status to render.
 * See task #249.
 */
enum recovery_sink {
	RECOVERY_SINK_TERMINAL,
	/*
	 * (The Plymouth sink lives in the Rust version; this port drops it - see
	 * the conversion notes. A display-message replaces the previous one in
	 * place, so the block would go there whole with nothing to erase, hence
	 * no counter.)
	 */
	RECOVERY_SINK_CONSOLE,
};

struct recovery_fields {
	const char	*source;
	const char	*pass;
	u32		done, total;
	u64		elapsed;

	/*
	 * Progress within @pass. pass_total is zero when the pass can't estimate
	 * its own size, which is a bare count instead of a bar.
	 */
	u64		seen;
	u64		pass_total;
	const char	*units;

	char		*devices;
};

struct recovery_display {
	struct bch_status_display status;

	int		fd;
	int		sink;
	char		*source;

	/*
	 * The member devices as the scan found them - see
	 * device_scan::devices_from_superblocks(). Fixed for the life of a mount.
	 */
	dev_names	devs;

	char		*devices_line;

	/*
	 * Set from the first status we manage to read, not from when we started
	 * polling: the fd exists from fsconfig(status_fd), which is before there
	 * is a filesystem behind it.
	 */
	bool		started;
	u64		started_secs;

	/* Cleared on ENOTTY - a kernel without the ioctl, so stop asking. */
	bool		supported;

	/* Terminal sink: rows the block occupies, for the cursor move. */
	unsigned	rows;

	/* Console sink. */
	int		console_fd;
	unsigned	cols;
};

static u64 monotonic_secs(void);
static char *pb_take(struct printbuf *);
static int open_console(void);
static void dev_names_copy(dev_names *, const dev_names *);
static u64 status_interval_ms(void *);
static void write_all(int, const char *, size_t);
static int status_emit(struct recovery_display *, char **, unsigned);
static int status_erase(void *);
static int status_draw(void *);
static bool status_read(struct recovery_display *, struct bch_ioctl_recovery_status *);
static void fields_build(struct recovery_display *, const struct bch_ioctl_recovery_status *,
			 struct recovery_fields *);
static bool field_pct(const struct recovery_fields *, u64 *);
static void render_block(struct recovery_fields *, unsigned, char **, unsigned *);
static char *render_line(struct recovery_fields *, unsigned);
static bool pass_complete(const struct bch_ioctl_recovery_status *, unsigned);
static char *devices_line(struct recovery_display *, const struct bch_ioctl_recovery_status *);
static bool fs_usage(int, struct bch_ioctl_fs_usage **, unsigned *);
static bool read_redundancy(struct recovery_display *, int *);
static u32 mask_count(const struct bch_recovery_pass_mask *);
static const char *pass_name(u32);
static const char *units_name(u32);
static void bar_fill(char *, u64);
static void prt_elapsed(struct printbuf *, u64);
static void truncate_line(char *, unsigned);

static u64 monotonic_secs(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec;
}

static char *pb_take(struct printbuf *buf)
{
	char *ret = xstrdup(buf->buf ? buf->buf : "");

	printbuf_exit(buf);
	return ret;
}

/*
 * /run/systemd/show-status is PID 1 saying it is displaying status; it removes
 * the file when the admin asked for quiet. Honouring it is how `quiet` keeps
 * meaning something once we've stopped going through printk.
 */
static int open_console(void)
{
	if (access("/run/systemd/show-status", F_OK))
		return -1;

	return open("/dev/console", O_WRONLY);
}

static void dev_names_copy(dev_names *dst, const dev_names *src)
{
	for (unsigned i = 0; i < src->nr; i++) {
		const struct dev_name *s = &src->data[i];
		struct dev_name d = {
			.idx		= s->idx,
			.durability	= s->durability,
			.online		= s->online,
			.dev		= s->dev ? xstrdup(s->dev) : NULL,
			.label		= s->label ? xstrdup(s->label) : NULL,
			.failure_domain	= s->failure_domain ? xstrdup(s->failure_domain) : NULL,
		};

		memcpy(&d.uuid, &s->uuid, sizeof(uuid_t));
		darray_push(dst, d);
	}
}

static u64 status_interval_ms(void *ctx)
{
	return INTERVAL_MS;
}

/*
 * NULL when there is nowhere to draw; the caller must then not poll either, for
 * the reason at the top of this file.
 */
struct bch_status_display *bch2_recovery_display_new(int fd, const char *source,
						     const dev_names *devs)
{
	/*
	 * Connect, don't send: an empty display-message is a real one, and it
	 * blanks the splash for as long as it takes us to draw the first block.
	 */
	struct recovery_display *d = xcalloc(1, sizeof(*d));

	d->fd		= fd;
	d->source	= xstrdup(source);
	dev_names_copy(&d->devs, devs);

	if (isatty(STDERR_FILENO)) {
		d->sink = RECOVERY_SINK_TERMINAL;
	} else {
		int console = open_console();

		if (console < 0) {
			free(d->source);
			dev_names_free(&d->devs);
			free(d);
			return NULL;
		}
		d->sink = RECOVERY_SINK_CONSOLE;
		d->console_fd = console;
	}

	d->supported = true;
	d->status.interval_ms	= status_interval_ms;
	d->status.erase		= status_erase;
	d->status.draw		= status_draw;
	d->status.ctx		= d;
	return &d->status;
}

void bch2_recovery_display_free(struct bch_status_display *status)
{
	struct recovery_display *d;

	if (!status)
		return;

	d = container_of(status, struct recovery_display, status);
	if (d->sink == RECOVERY_SINK_CONSOLE)
		close(d->console_fd);
	free(d->devices_line);
	free(d->source);
	dev_names_free(&d->devs);
	free(d);
}

static void write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		buf += n;
		len -= n;
	}
}

static int status_emit(struct recovery_display *d, char **lines, unsigned nr)
{
	struct printbuf b = PRINTBUF;

	if (d->sink == RECOVERY_SINK_TERMINAL) {
		/*
		 * `\x1b[{n}F` is n rows up, column zero; `\x1b[J` clears to end
		 * of display. One write, because two would show the gap.
		 */
		if (d->rows > 0)
			prt_printf(&b, "\x1b[%uF\x1b[J", d->rows);
		for (unsigned i = 0; i < nr; i++) {
			prt_str(&b, lines[i]);
			prt_char(&b, '\n');
		}

		d->rows = nr;
		fwrite(b.buf ?: "", 1, b.pos, stderr);
		fflush(stderr);
		printbuf_exit(&b);
		return 0;
	}

	/*
	 * Console: only one recovering filesystem draws. Re-taken rather than
	 * remembered, so whoever is second picks the line up when the first
	 * finishes; re-locking our own fd is a no-op.
	 */
	if (flock(d->console_fd, LOCK_EX | LOCK_NB))
		return 0;

	/*
	 * No newline in either direction: return to column zero, lay the line
	 * down, return again. Trailing spaces cover whatever of the previous
	 * line was longer, because without a newline nothing else erases it.
	 */
	const char *line = nr ? lines[0] : "";
	size_t len = strlen(line);
	size_t pad = d->cols > len ? d->cols - len : 0;

	prt_char(&b, '\r');
	prt_str(&b, line);
	prt_chars(&b, ' ', pad);
	prt_char(&b, '\r');

	d->cols = len;
	write_all(d->console_fd, b.buf, b.pos);
	printbuf_exit(&b);
	return 0;
}

static int status_erase(void *ctx)
{
	return status_emit(ctx, NULL, 0);
}

static int status_draw(void *ctx)
{
	struct recovery_display *d = ctx;
	bool console = d->sink == RECOVERY_SINK_CONSOLE;
	unsigned cols;
	struct bch_ioctl_recovery_status s;
	struct recovery_fields f;

	if (console) {
		/*
		 * The console isn't ours to measure, so assume the conservative
		 * width.
		 */
		cols = FALLBACK_COLS;
	} else {
		/*
		 * Ok(0) is not a width. A terminal with no winsize set - a
		 * serial console, hvc0 under a VM - answers the query
		 * successfully and says zero, which truncate() turns into a
		 * block of " ..." with every line's content thrown away. Only a
		 * plausible answer counts.
		 */
		struct winsize ws;
		unsigned c = 0;

		if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0)
			c = ws.ws_col;
		cols = c >= MIN_COLS ? c : FALLBACK_COLS;
	}

	if (!status_read(d, &s))
		return status_emit(ctx, NULL, 0);

	fields_build(d, &s, &f);

	if (console) {
		char *line = render_line(&f, cols);
		int ret = status_emit(ctx, &line, 1);

		free(line);
		free(f.devices);
		return ret;
	} else {
		char *lines[8];
		unsigned nr = 0;

		render_block(&f, cols, lines, &nr);
		int ret = status_emit(ctx, lines, nr);

		for (unsigned i = 0; i < nr; i++)
			free(lines[i]);
		free(f.devices);
		return ret;
	}
}

/*
 * ENODEV is ordinary, not a failure: the ioctl only answers while a filesystem
 * is coming up on this channel.
 */
static bool status_read(struct recovery_display *d, struct bch_ioctl_recovery_status *s)
{
	if (!d->supported)
		return false;

	memset(s, 0, sizeof(*s));

	if (ioctl(d->fd, BCH_IOCTL_RECOVERY_STATUS, s) < 0) {
		d->supported = errno != ENOTTY;
		return false;
	}

	if (!d->started) {
		d->started_secs = monotonic_secs();
		d->started = true;
	}
	return true;
}

static void fields_build(struct recovery_display *d,
			 const struct bch_ioctl_recovery_status *s,
			 struct recovery_fields *f)
{
	bool running = s->pass != 0;

	/*
	 * The denominator is what this run will have touched, which grows if a
	 * pass reschedules an earlier one - so the count can go backwards, and
	 * that is the truth rather than a glitch.
	 */
	u32 complete = mask_count(&s->passes_complete);
	u32 remaining = mask_count(&s->passes_remaining);

	f->source	= d->source;
	f->pass		= running ? pass_name(s->pass) : NULL;
	f->done		= complete + (running ? 1 : 0);
	f->total	= complete + remaining + (running ? 1 : 0);
	f->elapsed	= d->started ? monotonic_secs() - d->started_secs : 0;
	f->seen		= s->seen;
	f->pass_total	= s->total;
	f->units	= units_name(s->units);
	f->devices	= devices_line(d, s);
}

static bool field_pct(const struct recovery_fields *f, u64 *pct)
{
	if (!f->pass_total)
		return false;

	*pct = f->seen * 100 / f->pass_total;
	return true;
}

/*
 * Most-useful-first, because the truncation at the end drops the tail. A
 * percentage is deliberately not the headline: 57% of one pass out of fifty
 * implies something about how much longer, and it's wrong.
 */
static void render_block(struct recovery_fields *f, unsigned cols,
			 char **lines, unsigned *nr_out)
{
	struct printbuf b;
	unsigned nr = 0;

	b = PRINTBUF;
	prt_printf(&b, "Recovering %s: %u of %u passes, ", f->source, f->done, f->total);
	prt_elapsed(&b, f->elapsed);
	lines[nr++] = pb_take(&b);

	b = PRINTBUF;
	prt_printf(&b, "  %s", f->devices);
	lines[nr++] = pb_take(&b);

	if (f->pass) {
		u64 pct;

		b = PRINTBUF;
		prt_printf(&b, "  %s", f->pass);
		if (field_pct(f, &pct)) {
			char bar[BAR_WIDTH + 8];

			bar_fill(bar, pct);
			prt_printf(&b, "  %3llu%% [%s] %llu/%llu %s",
				   (unsigned long long)pct, bar,
				   (unsigned long long)f->seen,
				   (unsigned long long)f->pass_total, f->units);
		} else {
			prt_printf(&b, "  %llu %s", (unsigned long long)f->seen, f->units);
		}
		lines[nr++] = pb_take(&b);
	}

	b = PRINTBUF;
	prt_printf(&b, "  %s...", bch_splines_reticulate(f->elapsed));
	lines[nr++] = pb_take(&b);

	for (unsigned i = 0; i < nr; i++)
		truncate_line(lines[i], cols);

	*nr_out = nr;
}

/* The same, folded onto one row. */
static char *render_line(struct recovery_fields *f, unsigned cols)
{
	struct printbuf b = PRINTBUF;
	char *line;

	prt_printf(&b, "Recovering %s: ", f->source);

	if (f->pass) {
		u64 pct;

		prt_printf(&b, "%s ", f->pass);
		if (field_pct(f, &pct))
			prt_printf(&b, "%llu%% ", (unsigned long long)pct);
	}

	prt_printf(&b, "(%u/%u passes) ", f->done, f->total);
	prt_elapsed(&b, f->elapsed);
	prt_printf(&b, " - %s", f->devices);

	line = pb_take(&b);
	truncate_line(line, cols);
	return line;
}

static bool pass_complete(const struct bch_ioctl_recovery_status *s, unsigned pass)
{
	unsigned n = pass;

	return s->passes_complete.v[n / 64] & (1ULL << (n % 64));
}

/*
 * Redundancy is unknown, not zero, until accounting_read - the replicas
 * entries live in accounting. Saying "0" during a degraded mount because we
 * haven't looked yet is the worst thing this could do.
 *
 * Cached, not polled: accounting settles under mark_lock and doesn't move
 * again.
 */
static char *devices_line(struct recovery_display *d,
			  const struct bch_ioctl_recovery_status *s)
{
	struct printbuf out = PRINTBUF;
	size_t online = 0;
	int r;

	if (d->devices_line)
		return xstrdup(d->devices_line);

	for (size_t i = 0; i < d->devs.nr; i++)
		if (d->devs.data[i].online)
			online++;

	prt_printf(&out, "mounting with %zu/%zu devices", online, d->devs.nr);

	if (!pass_complete(s, BCH_RECOVERY_PASS_accounting_read)) {
		prt_str(&out, ", redundancy unknown (reading accounting)");
		return pb_take(&out);
	}

	if (read_redundancy(d, &r))
		prt_printf(&out, ", current redundancy %d", r);
	else
		prt_str(&out, ", redundancy unavailable");

	d->devices_line = xstrdup(out.buf ?: "");
	return pb_take(&out);
}

/*
 * One BCH_IOCTL_FS_USAGE call, growing the buffer until the reply fits.
 *
 * FS_USAGE and not QUERY_ACCOUNTING because QUERY_ACCOUNTING answers keys you
 * name, and the replicas entries are exactly what we don't know in advance:
 * which combinations of devices exist is the question.
 */
static bool fs_usage(int fd, struct bch_ioctl_fs_usage **hdr_out, unsigned *bytes_out)
{
	size_t bytes = 4096;

	for (;;) {
		size_t alloc = sizeof(struct bch_ioctl_fs_usage) + bytes;
		struct bch_ioctl_fs_usage *u = xmalloc(alloc);
		int err;

		memset(u, 0, alloc);
		u->replica_entries_bytes = bytes;

		if (ioctl(fd, BCH_IOCTL_FS_USAGE, u) == 0) {
			*hdr_out = u;
			*bytes_out = u->replica_entries_bytes;
			return true;
		}

		err = errno;
		free(u);

		if (err == ERANGE && bytes < (1 << 20)) {
			bytes *= 2;
			continue;
		}
		return false;
	}
}

static bool read_redundancy(struct recovery_display *d, int *out)
{
	struct bch_ioctl_fs_usage *hdr;
	unsigned bytes;
	unsigned char *base;
	size_t trailing_off = offsetof(struct bch_ioctl_fs_usage, replicas);
	size_t off = 0;
	bool have = false;
	int min = 0;

	if (!fs_usage(d->fd, &hdr, &bytes))
		return false;

	base = (unsigned char *)hdr;

	while (off + sizeof(struct bch_replicas_usage) <= bytes) {
		struct bch_replicas_usage *u =
			(struct bch_replicas_usage *)(base + trailing_off + off);
		unsigned len = replicas_usage_bytes(u);

		/*
		 * A truncated tail is the kernel and us disagreeing about the
		 * layout; stop rather than read past what it wrote.
		 */
		if (!len || off + len > bytes)
			break;

		if (u->r.data_type != BCH_DATA_cached) {
			int spare = bch2_replicas_spare_redundancy(u->r.nr_devs,
								   u->r.nr_required,
								   u->r.devs, &d->devs);

			if (!have || spare < min) {
				min = spare;
				have = true;
			}
		}

		off += len;
	}

	free(hdr);

	if (!have)
		return false;
	*out = min;
	return true;
}

static u32 mask_count(const struct bch_recovery_pass_mask *m)
{
	return __builtin_popcountll(m->v[0]) + __builtin_popcountll(m->v[1]);
}

/*
 * bindgen types the unsized `bch2_recovery_passes[]` as `[*const c_char; 0]`,
 * so `.get()` returns None for every index, not just out-of-range ones - a
 * lookup that reads like a miss and cannot succeed. Index off the address.
 */
static const char *pass_name(u32 pass)
{
	const char *p;

	if (pass >= BCH_RECOVERY_PASS_NR)
		return "(unknown)";

	p = bch2_recovery_passes[pass];
	return p ?: "(unknown)";
}

static const char *units_name(u32 units)
{
	return units == BCH_PROGRESS_UNITS_keys ? "keys" : "nodes";
}

static void bar_fill(char *out, u64 pct)
{
	unsigned filled = pct * BAR_WIDTH / 100;
	unsigned i = 0;
	unsigned dashes;

	if (filled > BAR_WIDTH)
		filled = BAR_WIDTH;

	dashes = filled ? filled - 1 : 0;
	while (i < dashes)
		out[i++] = '=';
	if (filled > 0)
		out[i++] = '>';
	while (i < BAR_WIDTH)
		out[i++] = ' ';
	out[i] = '\0';
}

static void prt_elapsed(struct printbuf *out, u64 secs)
{
	if (secs >= 3600)
		prt_printf(out, "%lluh%02llum%02llus",
			   (unsigned long long)(secs / 3600),
			   (unsigned long long)((secs / 60) % 60),
			   (unsigned long long)(secs % 60));
	else
		prt_printf(out, "%llum%02llus",
			   (unsigned long long)(secs / 60),
			   (unsigned long long)(secs % 60));
}

/*
 * Truncate to `cols` display columns, ellipsizing so it reads as cut short
 * rather than as a complete list. Pass names are ASCII, so bytes are columns.
 */
static void truncate_line(char *s, unsigned cols)
{
	size_t len = strlen(s);

	if (len > cols) {
		size_t keep = cols > 4 ? cols - 4 : 0;

		s[keep] = '\0';
		strcat(s, " ...");
	}
}