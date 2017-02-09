/* Merge individual changes into commits */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "interfaces.h"

/*
 * Lines are appended to the commit messages to summarize the changes in terms
 * of MKSSI file revisions.  The below string is used as the prefix for those
 * lines of text.
 */
#define PREFIX "#mkssi: "

/*
 * Used for deleted and reverted files, since MKSSI saves no authorship for
 * such events.
 */
static const struct git_author unknown_author = {
	.name = "Unknown",
	.email = "unknown"
};

/* lookup the MKSSI label for a file revision */
static const char *
file_revision_label(const struct rcs_file *file, const struct rcs_number *rev)
{
	const struct rcs_symbol *s;

	for (s = file->symbols; s; s = s->next)
		if (rcs_number_equal(&s->number, rev))
			return s->symbol_name;
	return NULL;
}

/* generate commit message for an add commit */
static char *
commit_msg_adds(const struct file_change *adds)
{
	unsigned int count;
	const struct file_change *a;
	char *msg;

	/*
	 * MKSSI does not allow developers to provide a comment when checking in
	 * a new file: it automatically puts "Initial revision" into the RCS log
	 * field, which is not very useful.  This function auto-generates a
	 * commit message for the added files.
	 */

	/* count the number of files being added */
	for (a = adds, count = 0; a; a = a->next, ++count)
		;

	if (count > 1)
		msg = sprintf_alloc("Add %u files\n\n", count);
	else
		msg = sprintf_alloc("Add file %s\n\n", adds->file->name);

	for (a = adds; a; a = a->next)
		msg = sprintf_alloc_append(msg, PREFIX "add %s rev. %s\n",
			a->file->name, rcs_number_string_sb(&a->newrev));

	return msg;
}

/* generate commit message for an update commit */
static char *
commit_msg_updates(const struct file_change *updates)
{
	unsigned int count;
	char *msg;
	const char *log, *label, *pos;
	char revstr_old[RCS_MAX_REV_LEN], revstr_new[RCS_MAX_REV_LEN];
	const struct rcs_patch *patch;
	const struct file_change *u;

	/*
	 * MKSSI allows developers to provide a free-form comment for each
	 * updated file as it is checked-in.  We use that text as the commit
	 * message, unless it was left blank (something MKSSI allows), in which
	 * case we auto-generate a commit message.
	 *
	 * Git commit messages, by convention, start with a short (<=50 chars)
	 * one-line summary of the commit, optionally followed by additional
	 * paragraphs describing the changes.  We do not attempt to impose such
	 * a convention here, since MKSSI revision history comments are not in
	 * that format, and it would be inaccurate to assume that the first
	 * sentence of the comment is an accurate summary of the commit.
	 */

	/* count the number of files being updated */
	for (u = updates, count = 0; u; u = u->next, ++count)
		;

	u = updates;
	if (rcs_number_compare(&u->newrev, &u->oldrev) < 0) {
		/*
		 * Reverted file revisions always stand alone; they are not
		 * merged with other changes.
		 */
		if (count != 1)
			fatal_error("internal error: merged reversions");

		msg = sprintf_alloc("Revert file %s to rev. %s\n\n",
			u->file->name, rcs_number_string_sb(&u->newrev));
	} else {
		/* All the updates should have the same check-in comment. */
		log = NULL;
		for (u = updates; u; u = u->next) {
			patch = rcs_file_find_patch(u->file, &u->newrev, true);
			if (!log)
				log = patch->log;
			else if (strcmp(patch->log, log))
				fatal_error("internal error: log fields not "
					"the same in update commit");
		}

		/*
		 * If the log message is empty or contains nothing but white
		 * space, ignore it and auto-generate a message instead.
		 */
		for (pos = log; *pos; ++pos)
			if (!isspace(*pos))
				break;
		if (*pos)
			msg = sprintf_alloc("%s\n\n", log);
		else if (count > 1)
			msg = sprintf_alloc("Update %u files\n\n", count);
		else
			msg = sprintf_alloc("Update file %s to rev. %s\n\n",
				updates->file->name,
				rcs_number_string_sb(&updates->newrev));
	}

	for (u = updates; u; u = u->next) {
		rcs_number_string(&u->newrev, revstr_new, sizeof revstr_new);
		rcs_number_string(&u->oldrev, revstr_old, sizeof revstr_old);
		label = file_revision_label(u->file, &u->newrev);
		msg = sprintf_alloc_append(msg, PREFIX "check-in %s rev. %s "
			"(was rev. %s)%s%s\n", u->file->name,
			revstr_new, revstr_old,
			label ? " labeled " : "",
			label ? label : "");
	}

	return msg;
}

/* generate commit message for an add commit */
static char *
commit_msg_deletes(const struct file_change *deletes)
{
	unsigned int count;
	const struct file_change *d;
	char *msg;

	/* count the number of files being deleted */
	for (d = deletes, count = 0; d; d = d->next, ++count)
		;

	if (count > 1)
		msg = sprintf_alloc("Delete %u files\n\n", count);
	else
		msg = sprintf_alloc("Delete file %s\n\n", deletes->file->name);

	for (d = deletes; d; d = d->next)
		msg = sprintf_alloc_append(msg, PREFIX "delete %s rev. %s\n",
			d->file->name, rcs_number_string_sb(&d->oldrev));

	return msg;
}

/* merge adds into commits */
static struct git_commit *
merge_adds(const char *branch, struct file_change *add_list, time_t cp_date)
{
	struct file_change *add, *a, **old_prev_next, **new_prev_next;
	struct git_commit *head, **prev_next, *c;
	const struct rcs_version *ver, *add_ver;

	/*
	 * Batch all adds with the same author into the same commit.  Added
	 * files have no revision history, so that cannot be used for grouping;
	 * and since the MKSSI timestamp is unreliable, it cannot either; thus
	 * the author is the only basis for batching.
	 */
	prev_next = &head;
	for (add = add_list; add; add = add_list) {
		add_list = add->next;

		ver = rcs_file_find_version(add->file, &add->newrev, true);

		c = xcalloc(1, sizeof *c, __func__);
		c->branch = branch;
		c->committer = author_map(ver->author);
		c->date = cp_date;
		c->changes.adds = add;

		old_prev_next = &add_list;
		new_prev_next = &add->next;
		for (a = add_list; a; a = a->next) {
			add_ver = rcs_file_find_version(a->file, &a->newrev,
				true);
			if (!strcasecmp(add_ver->author, ver->author)) {
				/* Append this add to the commit. */
				*new_prev_next = a;
				new_prev_next = &a->next;

				/* Remove from the old list. */
				*old_prev_next = a->next;
			} else
				/*
				 * Save the next pointer, so that if the next
				 * add has the same author, it can be removed
				 * from the original list.
				 */
				old_prev_next = &a->next;
		}
		*new_prev_next = NULL;

		c->commit_msg = commit_msg_adds(c->changes.adds);

		/* Append this commit to the list. */
		*prev_next = c;
		prev_next = &c->next;
	}
	*prev_next = NULL;
	return head;
}

/* merge any un-merged updates which match the given update */
static void
merge_matching_updates(struct file_change *merge_head,
	struct file_change **unmerged_head)
{
	struct file_change *unmerged, *unmerged_next, *u;
	const struct rcs_version *ver, *upd_ver;
	const struct rcs_patch *patch, *upd_patch;
	struct file_change **unmerged_prev_next, **merged_prev_next;
	int cmp;

	ver = rcs_file_find_version(merge_head->file, &merge_head->newrev,
		true);
	patch = rcs_file_find_patch(merge_head->file, &merge_head->newrev,
		true);

	merge_head->next = NULL;

	/* Search for updates sharing an author and comment */
	unmerged_prev_next = unmerged_head;
	merged_prev_next = &merge_head->next;
	for (unmerged = *unmerged_head; unmerged; unmerged = unmerged_next) {
		unmerged_next = unmerged->next;

		/*
		 * Never update the same file more than once in any commit --
		 * that would lose revision history.
		 */
		for (u = merge_head; u; u = u->next)
			if (u->file == unmerged->file)
				goto not_match;

		/*
		 * Don't merge a later revision of a file such that it is
		 * committed before an earlier revision.  (No need to look at
		 * the revision numbers, because the list is sorted and later
		 * entries will have a higher revision number.)
		 */
		for (u = *unmerged_head; u != unmerged; u = u->next)
			if (u->file == unmerged->file)
				goto not_match;

		/*
		 * Never merge reverted revisions -- these have
		 * no true author or log.  They are also rare.
		 */
		cmp = rcs_number_compare(&unmerged->newrev, &unmerged->oldrev);
		if (cmp < 0)
			goto not_match;

		upd_ver = rcs_file_find_version(unmerged->file,
			&unmerged->newrev, true);
		upd_patch = rcs_file_find_patch(unmerged->file,
			&unmerged->newrev, true);

		if (!strcasecmp(upd_ver->author, ver->author)
		 && !strcmp(upd_patch->log, patch->log)) {
			/* Remove from the old list. */
			*unmerged_prev_next = unmerged->next;

			/* Append this add to the commit. */
			*merged_prev_next = unmerged;
			merged_prev_next = &unmerged->next;
			unmerged->next = NULL;
		} else {
not_match:
			/*
			 * Save the next pointer, so that if the next update has
			 * the same author and comment, it can be removed from
			 * the original list.
			 */
			unmerged_prev_next = &unmerged->next;
		}
	}
}

/* merge updates into commits */
static struct git_commit *
merge_updates(const char *branch, struct file_change *update_list,
	time_t cp_date)
{
	struct file_change *update;
	struct git_commit *head, **prev_next, *c;

	/*
	 * Batch together updates which have the same author and revision
	 * comment.
	 *
	 * The MKSSI timestamp is unreliable and is not used.
	 */
	prev_next = &head;
	for (update = update_list; update; update = update_list) {
		update_list = update->next;

		c = xcalloc(1, sizeof *c, __func__);
		c->branch = branch;
		c->date = cp_date;
		c->changes.updates = update;

		if (rcs_number_compare(&update->newrev, &update->oldrev) < 0) {
			/*
			 * If a file was reverted to an earlier version, there
			 * is no way to know who did it.
			 */
			c->committer = &unknown_author;
			update->next = NULL;
		} else {
			c->committer = author_map(rcs_file_find_version(
				update->file, &update->newrev, true)->author);
			merge_matching_updates(update, &update_list);
		}

		c->commit_msg = commit_msg_updates(c->changes.updates);

		/* Append this commit to the list. */
		*prev_next = c;
		prev_next = &c->next;
	}
	*prev_next = NULL;
	return head;
}

/* merge all deletions into a single delete commit */
static struct git_commit *
merge_deletes(const char *branch, struct file_change *deletes, time_t cp_date)
{
	struct git_commit *c;

	if (!deletes)
		return NULL;

	/*
	 * In MKSSI, deletions have no recorded author, timestamp, or revision
	 * history.  Batch all the deletes into a single commit.
	 */
	c = xcalloc(1, sizeof *c, __func__);
	c->branch = branch;
	c->committer = &unknown_author;
	c->date = cp_date;
	c->commit_msg = commit_msg_deletes(deletes);
	c->changes.deletes = deletes;
	return c;
}

/* append a list to another list */
static void
commit_list_append(struct git_commit **list, struct git_commit *append)
{
	struct git_commit **prev_next, *c;

	prev_next = list;
	for (c = *prev_next; c; c = *prev_next)
		prev_next = &c->next;
	*prev_next = append;
}

/* merge individual changes into commits */
struct git_commit *
merge_changeset_into_commits(const char *branch,
	struct file_change_lists *changes, time_t cp_date)
{
	struct git_commit *add_commits, *update_commits, *delete_commit;

	add_commits = merge_adds(branch, changes->adds, cp_date);
	changes->adds = NULL;

	update_commits = merge_updates(branch, changes->updates, cp_date);
	changes->updates = NULL;

	delete_commit = merge_deletes(branch, changes->deletes, cp_date);
	changes->deletes = NULL;

	commit_list_append(&add_commits, update_commits);
	commit_list_append(&add_commits, delete_commit);
	return add_commits;
}

/* free a list of commits */
void
free_commits(struct git_commit *commit_list)
{
	struct git_commit *c, *cnext;

	for (c = commit_list; c; c = cnext) {
		cnext = c->next;
		free(c->commit_msg);
		changeset_free(&c->changes);
		free(c);
	}
}
