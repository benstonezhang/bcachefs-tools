/*
 * Answering degraded=ask.
 *
 * bch2_fs_may_start() acts on yes and very and refuses everything else, `ask`
 * included - correct for a kernel, which has no user to ask. So `ask` is ours
 * to resolve, and since it is the default, a filesystem that loses a member is
 * refused outright until something here answers.
 *
 * The question is put *after* a refusal rather than before the mount, and that
 * is the whole shape of this file. What there is to consent to depends on
 * whether the data on the missing devices has another copy, which is a reading
 * of the replicas table against the devices actually here - the kernel's to
 * make, at the moment it decides. So we attempt the mount and let
 * bch2_fs_may_start() classify its own refusal, as
 * insufficient_devices_data_intact or insufficient_devices_data_lost.
 *
 * Attempting is cheap: bch2_fs_may_start() is the first thing
 * __bch2_fs_start() does, before recovery reads and sorts the journal, which
 * after an unclean shutdown is where the time goes. This would not be worth
 * doing if it were the other way round.
 *
 * The user is then told which of the two situations they are in, and asked
 * only what is theirs to decide: mount, mount read-only, or don't. Which
 * degraded= value that needs is ours to work out, not theirs.
 *
 * Ported from src/degraded.rs.
 */

#include "libbcachefs.h"
#include "tools-util.h"

#include "opts.h"

#define PROMPT_TIMEOUT	60

/* warn! level: stuck in the boot log, so it has to carry the whole message. */
#define degraded_warn(fmt, ...)	fprintf(stderr, "bcachefs: " fmt "\n", ##__VA_ARGS__)

/*
 * info! level: rust's default filter is Warn, so these only reach someone who
 * asked with -v; the C fork has no log levels, so they're gated on the mount
 * option of the same name.
 */
static void degraded_info(const struct bch_opts *opts, const char *fmt, ...)
{
	va_list args;

	if (!opt_get(*opts, verbose))
		return;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

static bool sb_opts(struct bch_sb *sb, struct bch_opts *out)
{
	memset(out, 0, sizeof(*out));
	return bch2_opts_from_sb(out, sb) == 0;
}

/*
 * What the filesystem says to do about a missing device, unless the caller
 * said otherwise on the command line.
 */
static u8 degraded_action(const bch_scanned_sbs *sbs,
			  const struct bch_opts *cli_opts)
{
	if (opt_defined(*cli_opts, degraded))
		return opt_get(*cli_opts, degraded);

	if (sbs->nr) {
		struct bch_opts opts;

		if (sb_opts(sbs->data[0].sb.sb, &opts))
			return opt_get(opts, degraded);
	}

	return BCH_DEGRADED_ask;
}

/*
 * errcode matching without the kernel's BUG_ON(): __bch2_err_matches() bounds
 * nothing and the chain walk would index out of range on a code from a kernel
 * module newer than this binary - the normal state of affairs for a filesystem
 * that ships DKMS-only. "Doesn't abort" matters as much as "doesn't match".
 */
static bool err_matches(int err, int err_class)
{
	if (abs(err) >= BCH_ERR_MAX)
		return false;
	return __bch2_err_matches(err, err_class);
}

/* Which refusal we are answering, and so what is at stake. */
enum bch_degraded_situation {
	/*
	 * Every replica set still has a readable copy. Less redundancy, no
	 * holes.
	 */
	BCH_DEGRADED_DATA_INTACT,
	/* Some does not. */
	BCH_DEGRADED_DATA_LOST,
};

/*
 * Which refusal we are answering, and so what is at stake.
 *
 * The kernel decided this; we only relay it. Recomputing it here would mean
 * reading the superblock's replicas table against the devices we found and
 * getting the same answer - or, one version skew later, a different one.
 */
static bool situation_of(int err, enum bch_degraded_situation *out)
{
	if (err_matches(err, BCH_ERR_insufficient_devices_data_intact)) {
		*out = BCH_DEGRADED_DATA_INTACT;
		return true;
	} else if (err_matches(err, BCH_ERR_insufficient_devices_data_lost)) {
		*out = BCH_DEGRADED_DATA_LOST;
		return true;
	}

	return false;
}

/*
 * What the kernel has to be told before it will go ahead - the half of the
 * decision the user should not have to know about.
 */
static const char *situation_opt(enum bch_degraded_situation s)
{
	switch (s) {
	case BCH_DEGRADED_DATA_INTACT:
		return "degraded=yes";
	case BCH_DEGRADED_DATA_LOST:
		return "degraded=very";
	}
	return NULL;
}

/*
 * What it costs, and the question that follows from it. Neither says which
 * devices: the count is in front of this and bch2_fs_may_start() has already
 * listed them, with everything it knows about each.
 */
static void situation_line(enum bch_degraded_situation s,
			   const char **costs, const char **question)
{
	switch (s) {
	case BCH_DEGRADED_DATA_INTACT:
		*costs    = "All your data is still readable, with less "
			    "redundancy than it should have.";
		*question = "Mount?";
		break;
	case BCH_DEGRADED_DATA_LOST:
		*costs    = "Some of your data has no other copy, and reads "
			    "of it will fail.";
		*question = "Mount anyway?";
		break;
	}
}

/*
 * Everything the degraded question needs, taken from the scan before the
 * superblocks are closed.
 *
 * Held across the mount attempt that provokes it, which is why it holds no
 * struct bch_sb_handle: those are open block devices, and the kernel wants to
 * open them itself.
 */
struct bch_degraded_ask {
	char		*name;
	uuid_t		uuid;
	unsigned	missing;
	unsigned	expected;
	struct bch_opts	opts;
	bool		use_udev;
};

static int degraded_watch_fd(void *ctx)
{
	return bch2_device_watch_fd(ctx);
}

static enum bch_prompt_stirred degraded_watch_stirred(void *ctx)
{
	return bch2_device_watch_every_member_present(ctx)
		? BCH_PROMPT_STIR_MOOT : BCH_PROMPT_STIR_NOTHING;
}

/*
 * NULL when there is nothing here for us to decide: every member is present,
 * or the filesystem's degraded action is not `ask`, in which case the kernel
 * acts on it itself and an explicit -o degraded= is the user's decision
 * already.
 */
struct bch_degraded_ask *bch2_degraded_ask_new(const bch_scanned_sbs *sbs,
					       const struct bch_opts *cli_opts)
{
	struct bch_sb_handle *first;
	struct bch_degraded_ask *ask;
	unsigned expected, present;
	char name[BCH_SB_LABEL_SIZE + 1];

	if (!sbs->nr)
		return NULL;

	first = &sbs->data[0].sb;

	/*
	 * By device, not by path: see device_scan.c's present_devices(). A
	 * member found twice - multipath, or udev and the block scan both
	 * contributing - would otherwise make up the count for one that is
	 * missing.
	 */
	expected = bch2_scanned_expected_devices(sbs);
	present  = bch2_scanned_present_devices(sbs);

	if (present >= expected ||
	    degraded_action(sbs, cli_opts) != BCH_DEGRADED_ask)
		return NULL;

	ask = xmalloc(sizeof(*ask));
	bch_prompt_fs_name(first->sb, name, sizeof(name));
	ask->name	= xstrdup(name);
	uuid_copy(ask->uuid, first->sb->user_uuid.b);
	ask->missing	= expected - present;
	ask->expected	= expected;
	ask->opts	= *cli_opts;
	ask->use_udev	= opt_get(*cli_opts, mount_trusts_udev) != 0;
	return ask;
}

void bch2_degraded_ask_free(struct bch_degraded_ask *ask)
{
	free(ask->name);
	free(ask);
}

/*
 * Which filesystem, how much of it is gone, and what that costs. Whose
 * filesystem it is matters: at boot there can be several, and the systemd
 * prompt is one line with no other context.
 */
static char *degraded_question(const struct bch_degraded_ask *ask,
			       enum bch_degraded_situation s)
{
	const char *costs, *tail;

	situation_line(s, &costs, &tail);
	return mprintf("Filesystem %s is missing %u of its %u devices. %s %s",
		       ask->name, ask->missing, ask->expected, costs, tail);
}

/*
 * Put the question, if @err is one we know how to ask about.
 *
 * The devices themselves are not named here: bch2_fs_may_start() already
 * printed them, along with everything it knows about each, on its way to
 * throwing @err.
 *
 * Returns 0 with @outcome set, or -errno.
 */
int bch2_degraded_ask_put(const struct bch_degraded_ask *ask, int err,
			  struct bch_degraded_outcome *outcome)
{
	static const char *const yes_aliases[] = { "yes", NULL };
	static const char *const ro_aliases[] = { "ro", "readonly", NULL };
	static const char *const no_aliases[] = { "no", NULL };

	/*
	 * What the person at the machine gets to decide. Not *whether* data is
	 * missing - we know that, and telling them is our job, not theirs.
	 */
	static const struct bch_prompt_choice choices[] = {
		{ 'y', yes_aliases, "",
		  "mount", BCH_DEGRADED_ANSWER_YES },
		{ 'r', ro_aliases, "read-only",
		  "mount read-only: nothing gets written or re-replicated",
		  BCH_DEGRADED_ANSWER_READ_ONLY },
		{ 'n', no_aliases, "",
		  "don't mount", BCH_DEGRADED_ANSWER_NO },
	};
	struct bch_prompt_question q = {
		.choices	= choices,
		.nr_choices	= ARRAY_SIZE(choices),
		/*
		 * Anything not positively recognised as consent is a refusal -
		 * including an empty answer, which is what a bare Enter and a
		 * timed-out systemd prompt both produce.
		 */
		.silence	= BCH_DEGRADED_ANSWER_NO,
		.alarm		= false,
		.uuid		= NULL,
		.timeout_secs	= PROMPT_TIMEOUT,
	};
	enum bch_degraded_situation s;
	enum bch_prompt_kind kind;
	struct bch_prompt_watch watch = {
		.raw_fd		= degraded_watch_fd,
		.stirred	= degraded_watch_stirred,
		.ctx		= NULL,
	};
	struct bch_prompt_watch *pw = NULL;
	struct bch_device_watch *dw = NULL;
	char uuid_str[37];
	char *qtext;
	long answer;
	int ret;

	outcome->kind = BCH_DEGRADED_NO;

	if (!situation_of(err, &s))
		return 0;

	qtext = degraded_question(ask, s);

	/*
	 * Mounting degraded is a decision about data, so with nobody there to
	 * make it we say what we would have asked, and refuse. One warning,
	 * because this is one event: the question, and what we did instead.
	 */
	kind = bch_prompt_detect();
	if (kind == BCH_PROMPT_NONE) {
		degraded_warn("%s\n%s; refusing (mount -o %s to allow it)",
			      qtext, bch_prompt_no_one_to_ask,
			      situation_opt(s));
		free(qtext);
		return 0;
	}

	/*
	 * These choices are meaningless if the missing device turns up while
	 * the question is up - that is the answer, and the right outcome is a
	 * fresh device list rather than a degraded mount.
	 */
	dw = bch2_device_watch_new(ask->uuid, &ask->opts, ask->use_udev);
	if (dw) {
		watch.ctx = dw;
		pw = &watch;
	}

	uuid_unparse_lower(ask->uuid, uuid_str);

	q.prompt	= qtext;
	q.alarm		= s == BCH_DEGRADED_DATA_LOST;
	q.uuid		= uuid_str;

	ret = bch_prompt_put(kind, &q, pw, &answer);

	bch2_device_watch_free(dw);
	free(qtext);

	if (ret == -1) {
		degraded_warn("device turned up while asking; mounting normally");
		outcome->kind = BCH_DEGRADED_RESCAN;
		return 0;
	}
	if (ret < 0)
		return ret;

	if (answer == BCH_DEGRADED_ANSWER_NO)
		return 0;

	/*
	 * info, not warn: this only ever runs having just been answered, so by
	 * default it would tell the user what they typed a moment ago. The
	 * record of the decision is the kernel's "with options:" line, which
	 * says degraded=yes,read_only and is written by the thing that acted
	 * on it. Kept at all for -v, and for the mount(2) fallback, where
	 * there is no status channel to carry that line.
	 */
	degraded_info(&ask->opts, "mounting %swith %s",
		      answer == BCH_DEGRADED_ANSWER_READ_ONLY
			? "read-only " : "",
		      situation_opt(s));

	outcome->kind		= BCH_DEGRADED_MOUNT;
	outcome->fs_opt		= situation_opt(s);
	outcome->read_only	= answer == BCH_DEGRADED_ANSWER_READ_ONLY;
	return 0;
}

/*
 * Append one option to a mount option string, which may be absent or empty -
 * a stray leading comma is a parse error, not a cosmetic problem.
 */
char *bch2_degraded_append_opt(char *fs_opts, const char *opt)
{
	if (fs_opts && fs_opts[0])
		return mprintf("%s,%s", fs_opts, opt);
	return xstrdup(opt);
}