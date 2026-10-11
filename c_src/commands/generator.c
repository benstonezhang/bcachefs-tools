/*
 * systemd system generator: raise the mount timeout for bcachefs
 * filesystems, written before units are loaded. A mount helper can't raise
 * it from the inside - mount units get no NOTIFY_SOCKET, so
 * EXTEND_TIMEOUT_USEC= is unreachable, and a recovery killed at 90 seconds
 * starts over from scratch on the next boot, never finishing. See the
 * comments on TIMEOUT below.
 *
 * Two constraints on anything added here. All generators share one 90-second
 * budget and overrunning kills the whole batch, so nothing in here may probe
 * a device. And drop-in precedence sorts by basename across every directory,
 * not within one - the 10- prefix is load-bearing.
 *
 * Ported from src/commands/generator.rs.
 * GPLv2
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cmds.h"
#include "tools-util.h"

/*
 * No ceiling, as systemd-fsck@.service does it. A recovery making progress
 * must not be killed for taking a long time - and if one is genuinely
 * wedged, hanging visibly is a better failure than an unbootable system
 * whose symptom points somewhere else.
 */
#define GENERATOR_TIMEOUT	"infinity"

/* systemd's UNIT_NAME_MAX, from src/basic/unit-name.h. At this length a
 * name stops being escaped and starts being hashed - see unit_name(). */
#define SYSTEMD_UNIT_NAME_MAX	256

/* The mount point systemd-fstab-generator gives the cmdline root in an
 * initrd. */
#define SYSROOT		"/sysroot"

struct fstab_entry {
	char	*target;
	char	*fstype;
	char	*opts;
};

static void fstab_entries_exit(DARRAY(struct fstab_entry) *entries)
{
	darray_for_each(*entries, e) {
		free(e->target);
		free(e->fstype);
		free(e->opts);
	}
	darray_exit(entries);
}

static char *read_text_file(const char *path)
{
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return NULL;

	size_t size = 0, capacity = 4096;
	char *buf = xmalloc(capacity);

	while (1) {
		if (size + 1 >= capacity) {
			capacity *= 2;
			buf = xrealloc(buf, capacity);
		}

		ssize_t r = read(fd, buf + size, capacity - size - 1);
		if (r < 0) {
			int saved = errno;
			close(fd);
			free(buf);
			errno = saved;
			return NULL;
		}
		if (r == 0)
			break;
		size += r;
	}
	close(fd);

	buf[size] = '\0';
	return buf;
}

static char *read_line(char **p)
{
	char *line = *p, *end;

	if (!line)
		return NULL;

	end = strchr(line, '\n');
	if (end) {
		*end = '\0';
		*p = end + 1;
	} else
		*p = NULL;

	return line;
}

/*
 * systemd's unit-name escaping for a path, as unit_name_path_escape() does
 * it: simplify, drop leading and trailing slashes, '/' becomes '-', and
 * anything that isn't a valid unit-name character - including a literal '-'
 * - becomes \xNN. The root directory is "-".
 *
 * Done here rather than by shelling out to systemd-escape because a
 * generator runs in early boot, where that binary may not be present.
 *
 * Returns NULL for a path systemd itself refuses: unit_name_path_escape()
 * runs path_is_normalized() first and returns -EINVAL for anything with a
 * "." or ".." component, so such an fstab line has no unit to attach a
 * drop-in to and writing one would leave a file nothing will ever read.
 */
static char *escape_path(const char *path)
{
	size_t len = strlen(path);
	size_t start = 0, end = len;

	while (start < len && path[start] == '/')
		start++;
	while (end > start && path[end - 1] == '/')
		end--;

	/* A "." or ".." component: systemd refuses the unit outright */
	for (size_t i = start; i < end; ) {
		size_t j = i;

		while (j < end && path[j] != '/')
			j++;

		size_t clen = j - i;

		if ((clen == 1 && path[i] == '.') ||
		    (clen == 2 && path[i] == '.' && path[i + 1] == '.'))
			return NULL;

		i = j + 1;
	}

	if (start >= end)
		return strdup("-");

	char *out = xmalloc((end - start) * 4 + 1);
	char *o = out;
	bool prev_slash = false;

	for (size_t i = start; i < end; i++) {
		unsigned char c = (unsigned char)path[i];

		/* A leading dot is escaped even though '.' is otherwise
		 * valid, as systemd's do_escape() does: a unit file starting
		 * with one is hidden. */
		if (i == start && c == '.') {
			o += sprintf(o, "\\x2e");
			continue;
		}

		if (c == '/') {
			/* Collapse repeated slashes, which path_simplify()
			 * would have removed. */
			if (!prev_slash)
				*o++ = '-';
			prev_slash = true;
			continue;
		}
		prev_slash = false;

		if ((c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') ||
		    c == ':' || c == '_' || c == '.')
			*o++ = c;
		else
			o += sprintf(o, "\\x%02x", c);
	}
	*o = '\0';

	return out;
}

/* fstab escapes whitespace in paths as \0NN. Nothing else is escaped. */
static void unescape_octal(char *s)
{
	char *out = s;
	const char *in = s;

	while (*in) {
		if (in[0] == '\\' && in[1] && in[2] && in[3]) {
			char digits[4] = { in[1], in[2], in[3], '\0' };
			char *end;
			long v = strtol(digits, &end, 8);

			if (end == digits + 3) {
				*out++ = (char)v;
				in += 4;
				continue;
			}
		}
		*out++ = *in++;
	}
	*out = '\0';
}

static void parse_fstab(char *text, DARRAY(struct fstab_entry) *entries)
{
	char *line, *p = text;

	while ((line = read_line(&p))) {
		char *hash = strchr(line, '#');

		if (hash)
			*hash = '\0';

		char *save = NULL;
		char *target = strtok_r(NULL, " \t", &save);
		char *fstype = strtok_r(NULL, " \t", &save);
		char *opts = strtok_r(NULL, " \t", &save) ?:
			(char *) "defaults";

		if (!target || !fstype)
			continue;

		unescape_octal(target);

		struct fstab_entry e = {
			.target	= strdup(target),
			.fstype	= strdup(fstype),
			.opts	= strdup(opts),
		};

		darray_push(entries, e);
	}
}

static bool has_own_timeout(const char *opts)
{
	bool ret = false;
	char *copy = strdup(opts), *save = NULL, *tok;

	for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		if (!strncmp(tok, "x-systemd.mount-timeout=", 24)) {
			ret = true;
			break;
		}
	}

	free(copy);
	return ret;
}

/* Last occurrence of `key=` on the kernel cmdline, which is the one the
 * kernel and systemd both take. */
static char *cmdline_param(const char *cmdline, const char *key)
{
	char *ret = NULL;
	const char *p = cmdline;
	size_t keylen = strlen(key);

	while (*p) {
		const char *end = p;

		while (*end && *end != ' ' && *end != '\t' && *end != '\n')
			end++;

		const char *eq = memchr(p, '=', end - p);

		if (eq && (size_t)(eq - p) == keylen && !strncmp(p, key, keylen)) {
			free(ret);
			ret = xstrndup(eq + 1, end - eq - 1);
		}

		p = *end ? end + 1 : end;
	}

	return ret;
}

/* systemd's own test, from in_initrd() in src/basic/initrd-util.c. Every
 * initramfs builder writes this file - mkinitcpio unconditionally, dracut
 * too. */
static bool in_initrd(void)
{
	struct stat st;

	return !stat("/etc/initrd-release", &st);
}

/*
 * The host's fstab entries that systemd mounts in the initrd, with the
 * paths they get there - parse_fstab(prefix_sysroot = true) and
 * mount_in_initrd() in systemd's fstab-generator.c.
 */
static void sysroot_fstab(DARRAY(struct fstab_entry) *entries)
{
	const char *env = getenv("SYSTEMD_SYSROOT_FSTAB");
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s",
		 env ?: SYSROOT "/etc/fstab");

	/* Absent on the first pass: /sysroot/etc/fstab appears once the root
	 * is mounted, and initrd-parse-etc.service reruns the generators
	 * then. */
	char *text = read_text_file(path);

	if (!text)
		return;

	DARRAY(struct fstab_entry) parsed = {};
	parse_fstab(text, &parsed);
	free(text);

	darray_for_each(parsed, e) {
		bool x_initrd = false;
		char *opts_copy = strdup(e->opts), *save = NULL, *tok;

		for (tok = strtok_r(opts_copy, ",", &save); tok;
		     tok = strtok_r(NULL, ",", &save))
			if (!strcmp(tok, "x-initrd.mount"))
				x_initrd = true;

		free(opts_copy);

		size_t tlen = strlen(e->target);

		while (tlen > 1 && e->target[tlen - 1] == '/')
			tlen--;

		/* target == "/usr" (after trimming its trailing slash): */
		if (!x_initrd &&
		    !(tlen == 4 && !strncmp(e->target, "/usr", 4)))
			continue;

		struct fstab_entry n = {
			.target	= mprintf(SYSROOT "%s", e->target),
			.fstype	= strdup(e->fstype),
			.opts	= strdup(e->opts),
		};

		darray_push(entries, n);
	}

	fstab_entries_exit(&parsed);
}

/*
 * The initrd's root from the cmdline, as an Entry, when it's ours.
 *
 * Only a root= naming a device makes sysroot.mount from the cmdline; fstab,
 * off and gpt-auto hand the root to something else (validate_root_or_usr_
 * mount_source() in fstab-generator.c), and tmpfs and bind: aren't devices.
 *
 * Deciding whether it's ours may not touch the device: generators share one
 * 90-second budget and overrunning kills the whole batch, so blkid-ing the
 * root is out. That leaves what the cmdline says, and one inference:
 *
 *   rootfstype=bcachefs   ours, say so.
 *   rootfstype=<other>    explicitly not ours - leave it alone.
 *   rootfstype unset      assume ours. We are only *in* an initrd because
 *                         somebody added the bcachefs hook to build us in,
 *                         and that hook exists to boot a bcachefs root.
 *
 * The last arm is the one with a cost: get it wrong and a non-bcachefs root
 * that genuinely can't mount hangs at boot instead of failing at 90s. It
 * needs an image built with our hook *and* a root that isn't bcachefs *and*
 * no rootfstype= - and dropping it would give up every user who doesn't set
 * rootfstype=, which is most of them, in exactly the case that motivated
 * this.
 */
static bool cmdline_root(const char *cmdline, struct fstab_entry *e)
{
	char *root = cmdline_param(cmdline, "root");
	bool ours = false;

	if (root && *root &&
	    strcmp(root, "fstab") && strcmp(root, "off") &&
	    strcmp(root, "tmpfs") &&
	    strncmp(root, "gpt-auto", 8) && strncmp(root, "bind:", 5)) {
		char *fstype = cmdline_param(cmdline, "rootfstype");

		/* rootfstype=<other>: explicitly not ours */
		if (!fstype || !strcmp(fstype, "bcachefs"))
			ours = true;

		free(fstype);
	}
	free(root);

	if (!ours)
		return false;

	e->target = strdup(SYSROOT);
	e->fstype = strdup("bcachefs");
	e->opts = cmdline_param(cmdline, "rootflags") ?: strdup("defaults");

	return true;
}

/*
 * The name systemd will give this mount's unit.
 *
 * systemd-fstab-generator resolves the mount point before escaping it -
 * canonicalize_mount_path() at fstab-generator.c, which chases symlinks -
 * so /mnt/data pointing at /srv/data becomes srv-data.mount. Escaping the
 * raw fstab text instead would put the drop-in beside a unit that doesn't
 * exist: right directory, exit 0, no effect, and nothing says so until
 * somebody eats the timeout it was meant to remove.
 *
 * A mount point that isn't there yet can't be resolved, which at generator
 * time is most of them; then the raw path is the best guess available and
 * is what systemd's own chase() falls back to for the part that doesn't
 * exist.
 *
 * Returns NULL (silently, for a non-normalized path; with a warning for a
 * name systemd would hash) if there's no unit to attach a drop-in to.
 */
static char *unit_name(const char *target)
{
	char *resolved = realpath(target, NULL);
	char *escaped = escape_path(resolved ?: target);

	free(resolved);

	if (!escaped)
		return NULL;

	if (strlen(escaped) + strlen(".mount") >= SYSTEMD_UNIT_NAME_MAX) {
		fprintf(stderr,
			"generator: %s escapes to a unit name systemd would hash, which we can't reproduce - its mount timeout is NOT raised. Set x-systemd.mount-timeout=infinity on the fstab entry.\n",
			target);
		free(escaped);
		return NULL;
	}

	return escaped;
}

static void write_dropin(const char *normal_dir, const struct fstab_entry *e)
{
	char *unit = unit_name(e->target);

	if (!unit)
		return;

	char *dir = mprintf("%s/%s.mount.d", normal_dir, unit);
	char *file = mprintf("%s/10-bcachefs-timeout.conf", dir);

	if (mkdir(dir, 0777) && errno != EEXIST)
		goto err;

	FILE *f = fopen(file, "w");

	if (!f)
		goto err;

	fprintf(f,
		"# Written by bcachefs's systemd generator.\n"
		"#\n"
		"# A recovery can run for much longer than DefaultTimeoutStartSec, and a\n"
		"# mount helper has no way to ask for more time - mount units get no\n"
		"# NOTIFY_SOCKET, so EXTEND_TIMEOUT_USEC= is unreachable. Killing a\n"
		"# recovery that is making progress only means doing it again from the\n"
		"# start on the next boot, and never finishing.\n"
		"#\n"
		"# Set x-systemd.mount-timeout= on the fstab entry to override.\n"
		"[Mount]\n"
		"TimeoutSec=" GENERATOR_TIMEOUT "\n");

	bool bad = ferror(f);

	if (fclose(f))
		bad = true;

	if (bad)
		goto err;

	free(dir);
	free(file);
	free(unit);
	return;
err:
	fprintf(stderr, "generator: no drop-in for %s: %m\n", e->target);
	free(dir);
	free(file);
	free(unit);
}

/*
 * systemd calls a generator with three directories - normal, early, late -
 * and ignores its output. Failing here must never fail the boot, so
 * everything is a warning and we keep going: a filesystem that mounts
 * slowly is a much better outcome than one that doesn't mount at all.
 */
int cmd_systemd_generator(int argc, char *argv[])
{
	const char *normal_dir = argc >= 2 ? argv[1] : NULL;

	if (!normal_dir) {
		fprintf(stderr, "generator: no output directory given\n");
		return 0;
	}

	DARRAY(struct fstab_entry) entries = {};
	char *fstab_text = read_text_file("/etc/fstab");

	if (fstab_text)
		parse_fstab(fstab_text, &entries);

	bool initrd = in_initrd();
	if (initrd)
		sysroot_fstab(&entries);

	/* An fstab that names the root already said what it is, fstype
	 * included; the bcachefs entries below cover it if it's ours. */
	bool root_in_fstab = false;

	if (initrd) {
		darray_for_each(entries, e) {
			size_t tlen = strlen(e->target);

			while (tlen > 1 && e->target[tlen - 1] == '/')
				tlen--;

			if (tlen == strlen(SYSROOT) &&
			    !strncmp(e->target, SYSROOT, tlen)) {
				root_in_fstab = true;
				break;
			}
		}
	}

	/* Keep only the bcachefs entries: */
	{
		size_t n = 0;

		darray_for_each(entries, e) {
			if (!strcmp(e->fstype, "bcachefs")) {
				entries.data[n++] = *e;
			} else {
				free(e->target);
				free(e->fstype);
				free(e->opts);
			}
		}
		entries.nr = n;
	}

	if (initrd && !root_in_fstab) {
		char *cmdline = read_text_file("/proc/cmdline");

		if (cmdline) {
			struct fstab_entry root = { NULL, NULL, NULL };

			if (cmdline_root(cmdline, &root))
				darray_push(&entries, root);

			free(cmdline);
		}
	}

	darray_for_each(entries, e) {
		if (has_own_timeout(e->opts))
			continue;

		write_dropin(normal_dir, e);
	}

	fstab_entries_exit(&entries);

	return 0;
}
