/*
 * Member devices whose history has diverged from the filesystem's.
 *
 * Not "a device is stale": a stale device's history is a *prefix* and normal
 * recovery catches it up. A divergent one holds writes the filesystem never
 * saw, so the two histories are concurrent, nothing merges them, and somebody
 * has to choose.
 *
 * This runs before device_scan.c's filter_current_sbs() because
 * bch2_sbs_filter_dead() drops divergent devices *and frees their
 * superblocks* - and drops them the same way it drops devices that were
 * properly removed, so all that is left afterwards is a short device count,
 * which reads as "you are missing a disk".
 *
 * bch2_dev_in_fs() (fs/init/dev.c) decides what diverged; this only decides
 * what to say. Only BCH_ERR_device_splitbrain means divergence - the other
 * returns are different problems, left to the kernel to report. Its own
 * account reaches stderr first, ending in "Not using <dev>", which describes
 * what bch2_sbs_filter_dead() would do rather than what we do.
 *
 * Ported from src/splitbrain.rs.
 */

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "tools-util.h"

#include "init/dev.h"
#include "sb/members.h"
#include "util/printbuf.h"

#define PROMPT_TIMEOUT	60

static u64 sb_seq(const struct bch_sb *sb)
{
	return le64_to_cpu(sb->seq);
}

static u64 sb_write_time(const struct bch_sb *sb)
{
	return le64_to_cpu(sb->write_time);
}

/*
 * Highest seq, then newest write time - sb_cmp() in fs/init/fs.c.
 *
 * Only authoritative in the sense the kernel means it: newest wins, which
 * across a genuine fork is arbitrary and quite possibly wrong - a stale
 * rescue image booted later is newer than the filesystem the user wants. Used
 * to have something to compare against, never to decide anything.
 *
 * Like rust's max_by_key, a tie goes to the *last* such member.
 */
static int authoritative(const bch_scanned_sbs *sbs)
{
	unsigned i, best = 0;

	if (!sbs->nr)
		return -1;

	for (i = 1; i < sbs->nr; i++) {
		u64 a_seq   = sb_seq(sbs->data[best].sb.sb);
		u64 i_seq   = sb_seq(sbs->data[i].sb.sb);
		u64 a_wtime = sb_write_time(sbs->data[best].sb.sb);
		u64 i_wtime = sb_write_time(sbs->data[i].sb.sb);

		if (i_seq > a_seq || (i_seq == a_seq && i_wtime >= a_wtime))
			best = i;
	}

	return best;
}

/*
 * Devices that have diverged. Never fails: anything that is not divergence is
 * somebody else's problem, reported at mount.
 */
void bch2_splitbrain_find(const bch_scanned_sbs *sbs,
			  const struct bch_opts *opts,
			  bch_divergents *out)
{
	struct bch_sb_handle *best_handle;
	struct bch_sb_field_members_v2 *best_members;
	struct bch_opts opts_copy = *opts;
	unsigned i;

	int best_idx = authoritative(sbs);
	if (best_idx < 0 || best_idx >= sbs->nr)
		return;

	best_handle = (struct bch_sb_handle *)&sbs->data[best_idx].sb;
	best_members = bch2_sb_field_get(best_handle->sb, members_v2);

	for (i = 0; i < sbs->nr; i++) {
		struct bch_sb_handle *handle = (struct bch_sb_handle *)&sbs->data[i].sb;
		struct bch_sb *sb = handle->sb;
		int ret;

		if (i == best_idx)
			continue;

		/*
		 * bch2_dev_in_fs() compares two superblocks and formats a
		 * message; it mutates neither handle. Both outlive the call.
		 * The non-const pointers are the C signature, not a licence.
		 */
		ret = bch2_dev_in_fs(best_handle, handle, &opts_copy);
		if (!bch2_err_matches(ret, BCH_ERR_device_splitbrain))
			continue;

		struct bch_divergent d = {
			.path		= xstrdup(sbs->data[i].path),
			.dev_idx	= sb->dev_idx,
			.seq		= sb_seq(sb),
			.write_time	= sb_write_time(sb),
			/*
			 * What the surviving side last recorded for this
			 * device. 0 only when it has no member entry at all -
			 * a seq collision still has one, and it is the most
			 * useful line in the report ("believed it to be at
			 * 63, it says 65"), which is the whole vector-clock
			 * argument in one sentence.
			 */
			.expected_seq	= 0,
		};

		if (best_members && sb->dev_idx < best_handle->sb->nr_devices)
			d.expected_seq =
				le64_to_cpu(bch2_members_v2_get(best_members,
								sb->dev_idx).seq);

		darray_push(out, d);
	}
}

void bch2_splitbrain_divergents_exit(bch_divergents *v)
{
	unsigned i;

	for (i = 0; i < v->nr; i++)
		free(v->data[i].path);
	free(v->data);
	memset(v, 0, sizeof(*v));
}

static char *datetime(u64 secs)
{
	struct printbuf buf = PRINTBUF;

	bch2_prt_datetime(&buf, secs);
	return printbuf_str(&buf);
}

/*
 * What to call the side that is not diverging.
 *
 * "The rest of the filesystem" reads fine when nine devices agree and one
 * does not. With two devices there is no rest - there are two halves, and
 * which one counts as "the filesystem" is the arbitrary newest-wins pick that
 * authoritative() exists to warn about. Naming the device instead of implying
 * a verdict keeps the prose as neutral as the code.
 */
static char *other_side(const bch_scanned_sbs *sbs,
			const bch_divergents *divergent)
{
	unsigned surviving = sbs->nr - divergent->nr;
	int best_idx = authoritative(sbs);

	if (surviving == 1 && best_idx >= 0)
		return xstrdup(sbs->data[best_idx].path);

	return xstrdup("the rest of the filesystem");
}

/*
 * Timestamps first: "that is the rescue boot I did on Tuesday" is how someone
 * identifies which half is which. The sequence numbers say roughly how much
 * happened on each side, which is the other half of the judgement.
 */
char *bch2_splitbrain_report(const bch_scanned_sbs *sbs,
			     const bch_divergents *divergent)
{
	struct printbuf out = PRINTBUF;
	unsigned n = divergent->nr;
	const char *plural = n == 1 ? "device has" : "devices have";
	char *other = other_side(sbs, divergent);
	unsigned i;

	prt_printf(&out, "Split brain: %u %s writes %s never saw.\n",
		   n, plural, other);

	for (i = 0; i < n; i++) {
		const struct bch_divergent *d = &divergent->data[i];
		char *dt;

		prt_printf(&out, "\n");
		dt = datetime(d->write_time);
		prt_printf(&out, "  %s (device %u) last written %s, seq %llu\n",
			   d->path, d->dev_idx, dt,
			   (unsigned long long)d->seq);
		free(dt);

		if (d->expected_seq != 0)
			prt_printf(&out, "      %s believed it to be at seq %llu\n",
				   other, (unsigned long long)d->expected_seq);
	}

	int best_idx = authoritative(sbs);
	if (best_idx >= 0) {
		struct bch_sb *best = sbs->data[best_idx].sb.sb;
		char *dt = datetime(sb_write_time(best));

		prt_printf(&out, "\n");
		prt_printf(&out, "  %s last written %s, seq %llu.\n",
			   other, dt, (unsigned long long)sb_seq(best));
		free(dt);
	}

	prt_printf(&out, "\n");
	prt_printf(&out, "Both sides hold real data and nothing can merge "
		   "them, so continuing with one leaves the other's writes "
		   "behind.\n");

	// Both routes out, named, whether or not anyone is here to be asked -
	// this is also what someone reads in the journal after a boot refused.
	prt_printf(&out, "\n");
	for (i = 0; i < n; i++)
		prt_printf(&out, "  To continue with %s's history instead, "
			   "mount naming only its devices.\n",
			   divergent->data[i].path);
	prt_printf(&out, "  To rejoin a diverged device once mounted: "
		   "`bcachefs device remove` it and add it back, which rewrites "
		   "it and discards what it holds.\n");

	free(other);
	return printbuf_str(&out);
}

/*
 * Terminal only, and not a limitation to fix later: a destructive choice may
 * only be offered where its evidence fits, and the evidence here - which
 * device, written when, how far each side got - does not fit the agent
 * protocol's one-line `Message=`. Offering the choice without it is worse
 * than refusing, because they will take the default.
 *
 * Yes erases nothing: the diverged devices are left out of this mount and
 * untouched on disk, so the other history is still there to mount afterwards.
 * That is what makes it askable at all. Rewriting a device so it rejoins
 * needs a mounted filesystem, and is left to the user via report().
 */
bool bch2_splitbrain_ask(struct bch_sb_handle *sb_handle)
{
	static const char *const continue_aliases[] = { "continue", NULL };
	static const char *const no_aliases[] = { "no", NULL };
	static const struct bch_prompt_choice choices[] = {
		{ 'c', continue_aliases, "continue",
		  "continue, leaving the diverged device(s) out of this mount", 1 },
		{ 'n', no_aliases, "",
		  "don't mount", 0 },
	};
	struct bch_prompt_question q = {
		.prompt		= NULL,
		.choices	= choices,
		.nr_choices	= 2,
		/*
		 * Not an alarm: what this question risks is in the report the
		 * scan already printed above it, not in the one line here.
		 */
		.alarm		= false,
		.uuid		= NULL,
		.timeout_secs	= PROMPT_TIMEOUT,
	};
	struct bch_sb *sb = sb_handle->sb;
	char name[BCH_SB_LABEL_SIZE + 1];
	char uuid_str[37];
	char *prompt;
	long out;
	int ret;
	enum bch_prompt_kind kind = bch_prompt_detect();

	/*
	 * The only answer that continues is an explicit `c`; `y` matches
	 * nothing and falls to silence, which refuses. This is not a yes/no:
	 * someone answering a prompt they did not read out of habit should
	 * not thereby choose which of two histories to keep.
	 */

	if (kind == BCH_PROMPT_NONE) {
		fprintf(stderr,
			"%s: cannot ask which history to continue with; "
			"refusing\n", bch_prompt_no_one_to_ask);
		return false;
	}

	if (kind == BCH_PROMPT_AGENT) {
		fprintf(stderr, "bcachefs: cannot show two histories through "
			"a one-line prompt; refusing\n");
		return false;
	}

	bch_prompt_fs_name(sb, name, sizeof(name));
	uuid_unparse_lower(sb->user_uuid.b, uuid_str);

	prompt = mprintf("Continue with %s's surviving history?", name);
	q.prompt = prompt;
	q.uuid = uuid_str;

	ret = bch_prompt_put(kind, &q, NULL, &out);

	free(prompt);
	if (ret < 0)
		return false;

	return out != 0;
}