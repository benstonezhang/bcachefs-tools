/*
 * Putting a question to whoever is at the machine, during a mount.
 *
 * Two facts about the boot shape the whole module. At boot stdin *is* a
 * terminal - /dev/console - but plymouth owns the screen, so a question
 * written there is never seen; systemd's password agents include one that
 * draws on the splash. And the agents are boot-time units, so on a running
 * system /run/systemd/ask-password exists with nothing listening, and a
 * question posted to it times out having shown the user nothing (measured:
 * all three agents inactive, `--no-tty` returns "Timer expired").
 *
 * Hence agent_plausibly_listening(): there is no direct test, so we want
 * evidence before handing a question over.
 *
 * key.rs keeps its own path - a passphrase needs termios echo-off with the
 * ICRNL/ICANON repair for an unconfigured initramfs console, zeroizing, and
 * keyring caching.
 */

#include "libbcachefs.h"
#include "tools-util.h"

#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <uuid/uuid.h>

#include "util/printbuf.h"

#define ASK_PASSWORD_DIR	"/run/systemd/ask-password"

/* Why bch_prompt_detect came back NONE, for the caller to put in front of
 * its own refusal.
 *
 * There is one such reason because there is one such return: reaching it
 * means stdin is not a terminal - the last branch tests that - and that no
 * agent was plausibly listening either. Somebody reading this in a boot log
 * needs the concrete condition, not "nobody was there": the fix is a console
 * or an installed systemd-ask-password, and which one is what they have to
 * work out.
 */
const char bch_prompt_no_one_to_ask[] =
	"no terminal to ask on, and no systemd password agent";

/*
 * The only place an answer is written down: both rendered forms and the parse
 * derive from this, so a question cannot offer a letter it will not accept.
 */

static bool streq_icase(const char *l, size_t n, const char *r)
{
	return strlen(r) == n && strncasecmp(l, r, n) == 0;
}

static long question_parse(const struct bch_prompt_question *q, const char *reply)
{
	const char *trimmed = reply;
	size_t len;
	unsigned i;

	while (isspace((unsigned char)*trimmed))
		trimmed++;
	len = strlen(trimmed);
	while (len && isspace((unsigned char)trimmed[len - 1]))
		len--;

	for (i = 0; i < q->nr_choices; i++) {
		const struct bch_prompt_choice *c = &q->choices[i];
		char key[2] = { c->key, 0 };
		const char *const *a;

		if (streq_icase(trimmed, len, key))
			return c->answer;

		if (c->aliases)
			for (a = c->aliases; *a; a++)
				if (streq_icase(trimmed, len, *a))
					return c->answer;
	}

	return q->silence;
}

#define BCH_PROMPT_ANSWER_SILENCE	0
#define BCH_PROMPT_ANSWER_SAID		1
#define BCH_PROMPT_ANSWER_MOOT		2

/*
 * The rendered question, in both the forms its destinations need.
 */
struct bch_prompt_ask {
	const char	*prompt;
	const char	**choices;
	unsigned	nr_choices;
	char		*brief;
	bool		alarm;
	char		*id;
	u64		timeout_secs;
};

static char *str_asprintf(const char *fmt, ...)
{
	char *str = NULL;
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vasprintf(&str, fmt, args);
	va_end(args);
	if (ret < 0)
		die("%s: out of memory", __func__);
	return str;
}

static const char **question_lines(const struct bch_prompt_question *q)
{
	const char **lines = xmalloc(q->nr_choices * sizeof(lines[0]));
	unsigned i;

	for (i = 0; i < q->nr_choices; i++)
		lines[i] = str_asprintf("  %c  %s",
					q->choices[i].key, q->choices[i].blurb);
	return lines;
}

/*
 * The answer silence resolves to is the one shown capitalised. The brief is
 * the bracketed summary a bare Enter and a timed-out boot prompt both mean.
 */
static char *question_brief(const struct bch_prompt_question *q)
{
	struct printbuf buf = PRINTBUF;
	unsigned i;

	prt_printf(&buf, "[");
	for (i = 0; i < q->nr_choices; i++) {
		const struct bch_prompt_choice *c = &q->choices[i];

		if (i)
			prt_printf(&buf, " / ");
		if (c->answer == q->silence)
			prt_printf(&buf, "%c", toupper((unsigned char)c->key));
		else if (!c->short_name || !strlen(c->short_name))
			prt_printf(&buf, "%c", c->key);
		else
			prt_printf(&buf, "%c=%s", c->key, c->short_name);
	}
	prt_printf(&buf, "]");
	return printbuf_str(&buf);
}

static void ask_from_question(const struct bch_prompt_question *q,
			      struct bch_prompt_ask *ask)
{
	ask->prompt		= q->prompt;
	ask->choices		= question_lines(q);
	ask->nr_choices		= q->nr_choices;
	ask->brief		= question_brief(q);
	ask->alarm		= q->alarm;
	ask->id			= str_asprintf("bcachefs:UUID=%s", q->uuid);
	ask->timeout_secs	= q->timeout_secs;
}

static void ask_free(struct bch_prompt_ask *ask)
{
	unsigned i;

	for (i = 0; i < ask->nr_choices; i++)
		free((void *)ask->choices[i]);
	free(ask->choices);
	free(ask->brief);
	free(ask->id);
}

static void read_all(int fd, char **out)
{
	size_t size = 0, capacity = 4096;
	char *buf = xmalloc(capacity);

	for (;;) {
		ssize_t r = read(fd, buf + size, capacity - size - 1);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			die("read error: %m");
		}
		if (r == 0)
			break;
		size += r;
		if (size + 1 == capacity) {
			capacity *= 2;
			buf = xrealloc(buf, capacity);
		}
	}
	buf[size] = 0;
	*out = buf;
}

static int wait_for_answer(int fd, u64 timeout_secs,
			   const struct bch_prompt_watch *watch, char **reply);

/*
 * Its label, or its UUID - several filesystems come up at once at boot, and
 * someone facing two questions needs to see whether they are the same disk.
 */
const char *bch_prompt_fs_name(struct bch_sb *sb, char *buf, size_t buflen)
{
	size_t len = strnlen((const char *)sb->label, BCH_SB_LABEL_SIZE);

	if (len) {
		len = min_t(size_t, len, buflen - 1);
		memcpy(buf, sb->label, len);
		buf[len] = 0;
		return buf;
	}

	/* Rust prompt::fs_name uses sb.sb().uuid(), which is user_uuid. */
	uuid_unparse_lower(sb->user_uuid.b, buf);
	if (buflen > 36)
		buf[36] = 0;
	return buf;
}

/*
 * Installed, note - not listening. detect() supplies the evidence for that.
 */
static bool ask_password_installed(void)
{
	struct stat st;
	const char *path = getenv("PATH");
	bool found = false;

	if (stat(ASK_PASSWORD_DIR, &st) || !S_ISDIR(st.st_mode))
		return false;
	if (!path)
		return false;

	while (*path) {
		const char *end = strchrnul(path, ':');
		size_t len = end - path;
		char *dir = xstrndup(path, len);
		size_t need = strlen(dir) + 1 + strlen("systemd-ask-password") + 1;
		char *candidate = xmalloc(need);
		int ret = snprintf(candidate, need, "%s%s%s", dir,
				   dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "",
				   "systemd-ask-password");

		if (ret < 0 || (size_t)ret >= need)
			die("asprintf: %m");
		if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode))
			found = true;
		free(candidate);
		free(dir);
		path = *end ? end + 1 : end;
	}

	return found;
}

/*
 * Stdin being /dev/null is the signature of a process started by init: a
 * terminal means a person, a pipe or a file means a script, /dev/null means
 * neither, so the question has to go wherever init's own prompts go.
 */
bool bch_prompt_stdin_is_dev_null(void)
{
	struct stat st;

	if (fstat(STDIN_FILENO, &st))
		return false;

	return S_ISCHR(st.st_mode) && major(st.st_rdev) == 1 &&
	       minor(st.st_rdev) == 3;
}

/*
 * Plymouth answering a ping is an agent itself; stdin on /dev/null is what a
 * unit started by init gets, which puts us in the boot where the agents live.
 * A pipe or a file is a script redirecting us, with nobody behind it.
 */
static bool agent_plausibly_listening(bool tty)
{
	return (tty || bch_prompt_stdin_is_dev_null()) && ask_password_installed();
}

/*
 * None when nobody can be reached; callers take their safe answer without
 * composing a question, and say bch_prompt_no_one_to_ask when they do.
 */
enum bch_prompt_kind bch_prompt_detect(void)
{
	bool tty = isatty(STDIN_FILENO);

	if (tty && !bch_plymouth_active())
		return BCH_PROMPT_TERMINAL;

	if (agent_plausibly_listening(tty))
		return BCH_PROMPT_AGENT;

	// Under the splash with no agent to draw on it: printing here is poor,
	// but better than refusing without asking.
	if (tty)
		return BCH_PROMPT_TERMINAL;

	return BCH_PROMPT_NONE;
}

/*
 * Block until @fd is readable, the deadline passes, or @watch settles the
 * question.
 *
 * A watch firing is not by itself an answer: any block device arriving wakes a
 * device watch, and a wrong passphrase wakes an unlock socket, so ask the
 * watch whether *this* wakeup settled anything and go back to waiting if it
 * did not.
 *
 * Callers that poll a terminal want it already in ICANON: then readable means
 * a whole line has been typed, and the read that follows will not block
 * either.
 *
 * Returns an enum bch_prompt_waited, or -errno on poll failure.
 */
int bch_prompt_wait(int fd, u64 timeout_secs,
		    const struct bch_prompt_watch *watch)
{
	struct timespec start, deadline;
	bool have_deadline = timeout_secs != 0;

	if (have_deadline) {
		clock_gettime(CLOCK_MONOTONIC, &start);
		deadline.tv_sec	 = timeout_secs;
		deadline.tv_nsec = 0;
	}

	for (;;) {
		struct pollfd fds[2];
		int nfds = 1;
		int ret;

		fds[0].fd	= fd;
		fds[0].events	= POLLIN;
		if (watch && watch->raw_fd) {
			fds[1].fd	= watch->raw_fd(watch->ctx);
			fds[1].events	= POLLIN;
			nfds = 2;
		}

		if (have_deadline) {
			struct timespec now, remain;

			clock_gettime(CLOCK_MONOTONIC, &now);
			remain.tv_sec  = deadline.tv_sec  - now.tv_sec;
			remain.tv_nsec = deadline.tv_nsec - now.tv_nsec;
			if (remain.tv_nsec < 0) {
				remain.tv_nsec += 1000000000;
				remain.tv_sec--;
			}
			if (remain.tv_sec < 0)
				remain.tv_sec = remain.tv_nsec = 0;
			ret = ppoll(fds, nfds, &remain, NULL);
		} else {
			ret = poll(fds, nfds, -1);
		}

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		if (ret == 0)
			return BCH_PROMPT_WAITED_TIMEOUT;

		/* Read the readiness out before touching @watch again. */
		if (fds[0].revents & (POLLIN | POLLHUP))
			return BCH_PROMPT_WAITED_READABLE;

		if (nfds > 1 && (fds[1].revents & POLLIN) &&
		    watch->stirred) {
			switch (watch->stirred(watch->ctx)) {
			case BCH_PROMPT_STIR_MOOT:
				return BCH_PROMPT_WAITED_MOOT;
			case BCH_PROMPT_STIR_ANSWERED:
				return BCH_PROMPT_WAITED_ANSWERED;
			default:
				break;
			}
		}
	}
}

/*
 * prompt_wait, in the terms a posted question is answered in. @reply is set
 * only on a readable outcome.
 *
 * Returns BCH_PROMPT_ANSWER_* or -errno.
 */
static int wait_for_answer(int fd, u64 timeout_secs,
			   const struct bch_prompt_watch *watch, char **reply)
{
	int waited = bch_prompt_wait(fd, timeout_secs, watch);

	switch (waited) {
	case BCH_PROMPT_WAITED_READABLE:
		return BCH_PROMPT_ANSWER_SAID;
	case BCH_PROMPT_WAITED_TIMEOUT:
		return BCH_PROMPT_ANSWER_SILENCE;
		// Nothing that answers a multiple-choice question this way exists
		// yet; the socket answers the passphrase prompt, which has its
		// own path.
	case BCH_PROMPT_WAITED_MOOT:
	case BCH_PROMPT_WAITED_ANSWERED:
		return BCH_PROMPT_ANSWER_MOOT;
	default:
		return waited;
	}
}

/*
 * --echo=yes because this isn't a password: the default is `masked`, an
 * asterisk per character plus a lock-and-key emoji.
 *
 * Spawned rather than run to completion so it can be killed: with a watch,
 * the question can stop applying while the agent is still displaying it, and
 * leaving it on someone's screen after the fact is worse than not asking.
 */
static int ask_via_agent(const struct bch_prompt_ask *ask,
			 const struct bch_prompt_watch *watch, char **reply)
{
	char *id = str_asprintf("--id=%s", ask->id);
	char *timeout = str_asprintf("--timeout=%llu",
				     (unsigned long long)ask->timeout_secs);
	char *message = str_asprintf("%s %s", ask->prompt, ask->brief);
	char *const argv[] = {
		"systemd-ask-password",
		"--no-tty",
		"--echo=yes",
		"--icon=drive-harddisk",
		id,
		timeout,
		"-n",
		message,
		NULL,
	};
	int pipefd[2], status, answer;
	pid_t pid;

	if (pipe(pipefd))
		die("pipe error: %m");

	pid = fork();
	if (pid < 0)
		die("fork error: %m");

	if (pid == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);
		execvp(argv[0], argv);
		_exit(127);
	}

	close(pipefd[1]);

	// systemd-ask-password enforces its own --timeout, so let it: passing 0
	// here means we wait on the child rather than racing it.
	answer = wait_for_answer(pipefd[0], 0, watch, reply);

	if (answer == BCH_PROMPT_ANSWER_MOOT) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		close(pipefd[0]);
		free(id);
		free(timeout);
		free(message);
		return BCH_PROMPT_ANSWER_MOOT;
	}

	read_all(pipefd[0], reply);
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		free(*reply);
		*reply = NULL;
		free(id);
		free(timeout);
		free(message);
		return BCH_PROMPT_ANSWER_SILENCE;
	}

	free(id);
	free(timeout);
	free(message);
	return BCH_PROMPT_ANSWER_SAID;
}

static int ask_on_terminal(const struct bch_prompt_ask *ask,
			   const struct bch_prompt_watch *watch, char **reply)
{
	unsigned i;
	int answer;

	// stdout, not stdin: the question is read where it is drawn, and the two
	// can be different files.
	if (ask->alarm && isatty(STDOUT_FILENO))
		fprintf(stdout, "\033[1;31m%s\033[0m\n", ask->prompt);
	else
		fprintf(stdout, "%s\n", ask->prompt);

	for (i = 0; i < ask->nr_choices; i++)
		fprintf(stdout, "%s\n", ask->choices[i]);

	fprintf(stdout, "%s ", ask->brief);
	fflush(stdout);

	answer = wait_for_answer(STDIN_FILENO, ask->timeout_secs, watch, reply);
	if (answer == BCH_PROMPT_ANSWER_SAID) {
		char *line = NULL;
		size_t cap = 0;

		if (getline(&line, &cap, stdin) < 0)
			die("read error: %m");
		*reply = line;
		return BCH_PROMPT_ANSWER_SAID;
	}

	// Leave the half-written prompt behind a newline rather than letting
	// whatever comes next start mid-line.
	fprintf(stdout, "\n");
	fflush(stdout);
	return answer;
}

/*
 * Put the question and interpret the reply.
 *
 * @kind tells which route to use (bch_prompt_detect()).
 *
 * Returns 0 on success with @out set to one of the question's own answers -
 * the silence answer on a bare Enter or timeout, so a caller never has to
 * decide what an unrecognised reply meant - or -1 when the question stopped
 * applying while it was up (a watch settled it), or -errno.
 */
int bch_prompt_put(enum bch_prompt_kind kind,
		   const struct bch_prompt_question *q,
		   const struct bch_prompt_watch *watch, long *out)
{
	struct bch_prompt_ask ask;
	char *reply = NULL;
	int answer;

	ask_from_question(q, &ask);

	if (kind == BCH_PROMPT_AGENT)
		answer = ask_via_agent(&ask, watch, &reply);
	else
		answer = ask_on_terminal(&ask, watch, &reply);

	ask_free(&ask);

	if (answer < 0)
		return answer;

	switch (answer) {
	case BCH_PROMPT_ANSWER_SAID:
		*out = question_parse(q, reply);
		free(reply);
		return 0;
	case BCH_PROMPT_ANSWER_SILENCE:
		*out = q->silence;
		return 0;
	case BCH_PROMPT_ANSWER_MOOT:
	default:
		return -1;
	}
}