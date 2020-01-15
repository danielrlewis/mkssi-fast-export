/*
 * Copyright (c) 2017, 2019-2020 Datalight, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Merge individual changes into commits.
 */
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

/* Commit messages note for revisions dependent on missing RCS patches. */
static const char msg_missing[] = "\
(Note: This commit represents a file revision whose contents have been lost due\n\
to MKSSI project corruption.  Specifically, this file revision is dependent on\n\
an RCS patch which is missing from the RCS file.  When MKSSI attempts to check-\n\
out such a file revision, it creates an empty file.  In emulation of that\n\
behavior, mkssi-fast-export has exported an empty file for this revision.)\n";

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
	const struct rcs_patch *patch;
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
		msg = sprintf_alloc("Add file %s\n\n", adds->canonical_name);

	if (!adds->file->dummy) {
		patch = rcs_file_find_patch(adds->file, &adds->newrev, true);
		if (patch->missing) {
			if (count > 1)
				fatal_error("internal error: merged adds with "
					"missing RCS patches");

			msg = sprintf_alloc_append(msg, "%s\n", msg_missing);
		}
	}

	for (a = adds; a; a = a->next)
		msg = sprintf_alloc_append(msg, PREFIX "add %s rev. %s%s\n",
			a->canonical_name, rcs_number_string_sb(&a->newrev),
			a->member_type_other ? " (\"other\")" : "");

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
	const struct rcs_patch *patch = NULL; /* Init'd for compiler warning */
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
			u->canonical_name, rcs_number_string_sb(&u->newrev));
	} else {
		/* All the updates should have the same check-in comment. */
		log = NULL;
		for (u = updates; u; u = u->next) {
			patch = rcs_file_find_patch(u->file, &u->newrev, true);
			if (patch->missing && (u != updates || u->next))
				fatal_error("internal error: merged update "
					"with missing RCS patch");

			if (!log)
				log = patch->log;
			else if (strcmp(patch->log, log))
				fatal_error("internal error: log fields not "
					"the same in update commit");
		}

		/* If patch->missing, log might be NULL. */
		if (log) {
			/*
			 * If the log message is empty or contains nothing but
			 * white space, ignore it and auto-generate a message
			 * instead.
			 */
			for (pos = log; *pos; ++pos)
				if (!isspace(*pos))
					break;
			if (!*pos)
				log = NULL;
		}

		if (log)
			msg = sprintf_alloc("%s\n\n", log);
		else if (count > 1)
			msg = sprintf_alloc("Update %u files\n\n", count);
		else
			msg = sprintf_alloc("Update file %s to rev. %s\n\n",
				updates->canonical_name,
				rcs_number_string_sb(&updates->newrev));

		if (patch->missing)
			msg = sprintf_alloc_append(msg, "%s\n", msg_missing);
	}

	for (u = updates; u; u = u->next) {
		rcs_number_string(&u->newrev, revstr_new, sizeof revstr_new);
		rcs_number_string(&u->oldrev, revstr_old, sizeof revstr_old);
		label = file_revision_label(u->file, &u->newrev);
		msg = sprintf_alloc_append(msg, PREFIX "check-in %s rev. %s%s "
			"(was rev. %s)%s%s\n", u->canonical_name,
			revstr_new,
			u->member_type_other ? " (\"other\")" : "",
			revstr_old,
			label ? " labeled " : "",
			label ? label : "");
	}

	return msg;
}

/* generate commit message for a delete commit */
static char *
commit_msg_deletes(const struct file_change *deletes)
{
	unsigned int count;
	const struct file_change *d;
	char *msg;

	/*
	 * MKSSI has no revision history comments for deleted files, so this
	 * function auto-generates a commit message for the deleted files.
	 */

	/* count the number of files being deleted */
	for (d = deletes, count = 0; d; d = d->next, ++count)
		;

	if (count > 1)
		msg = sprintf_alloc("Delete %u files\n\n", count);
	else
		msg = sprintf_alloc("Delete file %s\n\n",
			deletes->canonical_name);

	for (d = deletes; d; d = d->next)
		msg = sprintf_alloc_append(msg, PREFIX "delete %s rev. %s\n",
			d->canonical_name, rcs_number_string_sb(&d->oldrev));

	return msg;
}

/* merge renames of the same type: for a file or for a directory */
static struct git_commit *
merge_renames_sub(const char *branch, struct file_change **renames,
	time_t cp_date, const char *commit_message, bool directory)
{
	struct git_commit *c;
	struct file_change *r, *rnext, **list_prev, **commit_prev;

	c = xcalloc(1, sizeof *c, __func__);
	c->branch = branch;
	c->committer = &tool_author;
	c->date = cp_date;
	c->commit_msg = xstrdup(commit_message, __func__);

	list_prev = renames;
	commit_prev = &c->changes.renames;
	for (r = *list_prev; r; r = rnext) {
		rnext = r->next;

		/*
		 * The file pointer is NULL for directories and non-NULL for
		 * files.
		 */
		if (!r->file == directory) {
			/* Remove r from renames list */
			*list_prev = r->next;
			r->next = NULL;

			/* Add r to c->changes.renames list */
			*commit_prev = r;
			commit_prev = &r->next;
		} else
			list_prev = &r->next;
	}

	if (!c->changes.renames) {
		/* No renames, no commit */
		free(c->commit_msg);
		free(c);
		c = NULL;
	}
	return c;
}

/* create changes to update RCS keywords for a renamed directory */
static void
update_keywords_for_dir_rename(struct git_commit *rename)
{
	const struct rcs_file_revision *f;
	struct file_change *r, *rr, **prev_next, *u;

	if (!rename)
		return;

	/*
	 * Loop through the renamed directories and check whether any of the
	 * files in that directory have RCS keywords whose expanded values will
	 * need to change due to the rename.
	 */
	prev_next = &rename->changes.updates;
	for (r = rename->changes.renames; r; r = r->next) {
		for (f = r->old_frevs; f; f = f->next)
			/*
			 * If this file resides in the renamed directory and it
			 * has an RCS keyword which expands to a path...
			 */
			if (f->ver && f->ver->kw_path &&
			 is_parent_dir(r->canonical_name, f->file->name)) {
				/*
				 * If the file has multiple parent directories
				 * that are being renamed in the same commit,
				 * ignore all but the longest parent directory.
				 */
				for (rr = r->next; rr; rr = rr->next)
					if (is_parent_dir(rr->canonical_name,
					 f->file->name) &&
					 strlen(rr->canonical_name) >
					 strlen(r->canonical_name))
						break;
				if (rr)
					continue;

				/*
				 * We should have already detected this
				 * condition when parsing the project revisions
				 * and file revisions, and set the file revision
				 * as requiring a just-in-time export.
				 */
				if (!f->ver || !f->ver->jit)
					fatal_error("interal error: %s rev. %s "
						"should be JIT for rename",
						f->file->name,
						rcs_number_string_sb(&f->rev));

				/*
				 * Add a file modification to the rename commit.
				 */
				u = xcalloc(1, sizeof *u, __func__);
				u->file = f->file;
				u->buf = xstrdup(f->file->name, __func__);
				memcpy(u->buf, r->canonical_name,
					strlen(r->canonical_name));
				u->canonical_name = u->buf;
				u->oldrev = u->newrev = f->rev;
				*prev_next = u;
				prev_next = &u->next;
			}
	}
}

/* create changes to update RCS keywords for a renamed file */
static void
update_keywords_for_file_rename(struct git_commit *rename)
{
	const struct rcs_file_revision *f;
	struct file_change *r, **prev_next, *u;

	if (!rename)
		return;

	/*
	 * Loop through the renamed files and check whether any of them have RCS
	 * keywords whose expanded values will need to change due to the rename.
	 */
	prev_next = &rename->changes.updates;
	for (r = rename->changes.renames; r; r = r->next) {
		/* Find the renamed file in the file revision list. */
		for (f = r->old_frevs; f; f = f->next)
			if (f->file == r->file)
				break;
		if (!f)
			fatal_error("internal error: renaming non-existent "
				"file %s\n", r->canonical_name);

		/*
		 * If the current file revision for the renamed file has an RCS
		 * keyword that expands to a file name or path, then the file
		 * needs to be modified to update the RCS keyword as part of the
		 * rename.
		 */
		if (f->ver && (f->ver->kw_name || f->ver->kw_path)) {
			/*
			 * We should have already detected this condition when
			 * parsing the project revisions and file revisions, and
			 * set the file revision as requiring a just-in-time
			 * export.
			 */
			if (!f->ver->jit)
				fatal_error("interal error: %s rev. %s "
					"should be JIT for rename",
					f->file->name,
					rcs_number_string_sb(&f->rev));

			/* Add a file modification to the rename commit. */
			u = xcalloc(1, sizeof *u, __func__);
			u->file = f->file;
			u->buf = xstrdup(r->canonical_name, __func__);
			u->canonical_name = u->buf;
			u->oldrev = u->newrev = f->rev;
			*prev_next = u;
			prev_next = &u->next;
		}
	}
}

/* merge renames into rename commits */
static struct git_commit *
merge_renames(const char *branch, struct file_change *renames, time_t cp_date)
{
	struct git_commit *cdir, *cfile;
	static const char msg_dir[] =
"Rename to change directory name capitalization\n\
\n\
This commit has been automatically generated to represent an implicit change to\n\
the capitalization of one or more directory names within the MKSSI project.\n\
MKSSI (at least the DOS/Windows versions) is case insensitive, and the\n\
capitalization of directory names in the project listing can change over time.\n\
Since Git is case sensitive, these changes to directory name capitalization\n\
require a commit to explicitly rename the affected directories.\n";
	static const char msg_file[] =
"Rename to change file name capitalization\n\
\n\
This commit has been automatically generated to represent an implicit change to\n\
the capitalization of one or more file names within the MKSSI project.  MKSSI\n\
(at least the DOS/Windows versions) is case insensitive, and the capitalization\n\
of file names in the project listing can change over time.  Since Git is case\n\
sensitive, these changes to file name capitalization require a commit to\n\
explicitly rename the affected files.\n";

	if (!renames)
		return NULL;

	/*
	 * MKSSI does not have rename operations; these renames are for implicit
	 * directory and file name capitalization changes.
	 */

	cdir = merge_renames_sub(branch, &renames, cp_date, msg_dir, true);
	update_keywords_for_dir_rename(cdir);
	cfile = merge_renames_sub(branch, &renames, cp_date, msg_file, false);
	update_keywords_for_file_rename(cfile);

	/*
	 * If there are both directory and file renames, the directory renames
	 * must occur first.
	 */
	if (cdir && cfile)
		cdir->next = cfile;

	return cdir ? cdir : cfile;
}

/* merge adds into commits */
static struct git_commit *
merge_adds(const char *branch, struct file_change *add_list, time_t cp_date)
{
	struct file_change *add, *a, **old_prev_next, **new_prev_next;
	struct git_commit *head, **prev_next, *c;
	const struct rcs_version *ver, *add_ver;
	const struct rcs_patch *patch, *add_patch;

	/*
	 * Batch all adds with the same author into the same commit.  Added
	 * files have no revision history, so that cannot be used for grouping;
	 * and since the MKSSI timestamp is unreliable, it cannot either; thus
	 * the author is the only basis for batching.
	 */
	prev_next = &head;
	for (add = add_list; add; add = add_list) {
		add_list = add->next;

		old_prev_next = &add_list;
		new_prev_next = &add->next;

		c = xcalloc(1, sizeof *c, __func__);
		c->branch = branch;
		c->changes.adds = add;

		/*
		 * Dummy files have no RCS metadata, so use the unknown author
		 * and the timestamp of the subsequent checkpoint.
		 */
		if (add->file->dummy) {
			c->committer = &unknown_author;
			c->date = cp_date;
			goto skip_merge; /* Don't merge with other adds. */
		}

		ver = rcs_file_find_version(add->file, &add->newrev, true);
		patch = rcs_file_find_patch(add->file, &add->newrev, true);

		c->committer = author_map(ver->author);
		c->date = ver->date.value; /* See comment below on timestamps */

		/*
		 * If the file to be added has no RCS patch, don't merge it with
		 * any other added files.
		 */
		if (patch->missing)
			goto skip_merge;

		for (a = add_list; a; a = a->next) {
			/* Don't merge this add with an add of a dummy file. */
			if (a->file->dummy)
				continue;

			add_ver = rcs_file_find_version(a->file, &a->newrev,
				true);
			add_patch = rcs_file_find_patch(a->file, &a->newrev,
				true);

			/*
			 * Should this add *not* be merged?  Adds based on
			 * corrupt RCS patches are never merged.  More
			 * typically, adds from different authors are not
			 * merged.
			 */
			if (add_patch->missing ||
			 strcasecmp(add_ver->author, ver->author)) {
				/*
				 * Save the next pointer, so that if the next
				 * add has the same author, it can be removed
				 * from the original list.
				 */
				old_prev_next = &a->next;

				continue; /* don't merge */
			}

			/* Append this add to the commit. */
			*new_prev_next = a;
			new_prev_next = &a->next;

			/* Remove from the old list. */
			*old_prev_next = a->next;

			/*
			 * Use the file revision with the newest date as the
			 * commit timestamp.
			 *
			 * Note: MKSSI stores the mtime of the file being added
			 * as the revision timestamp, which can differ
			 * considerably from when the user added the file to the
			 * project.  For example, a file last modified several
			 * years ago will, when added to the project, have a
			 * revision timestamp from several years ago.  We use
			 * this timestamp anyway, for lack of a better
			 * alternative.
			 */
			c->date = max(c->date, add_ver->date.value);
		}
skip_merge:
		*new_prev_next = NULL;

		/*
		 * Re-sort the adds within the commit, so that the changes will
		 * be listed in the commit message in the same order as tools
		 * like git diff and gitk will display.
		 */
		c->changes.adds = change_list_sort_by_name(c->changes.adds);

		c->commit_msg = commit_msg_adds(c->changes.adds);

		/* Append this commit to the list. */
		*prev_next = c;
		prev_next = &c->next;
	}
	*prev_next = NULL;
	return head;
}

/* merge updates for the $ProjectRevision$ keyword */
static void
merge_projrev_updates(struct file_change *merge_head,
	struct file_change **unmerged_head)
{
	struct file_change *unmerged, *unmerged_next;
	struct file_change **unmerged_prev_next, **merged_prev_next;

	/*
	 * Updates for the $ProjectRevision$ keyword are all merged together.
	 */
	unmerged_prev_next = unmerged_head;
	merged_prev_next = &merge_head->next;
	for (unmerged = *unmerged_head; unmerged; unmerged = unmerged_next) {
		unmerged_next = unmerged->next;

		if (unmerged->projrev_update) {
			/* Remove from the old list. */
			*unmerged_prev_next = unmerged->next;

			/* Append this update to the commit. */
			*merged_prev_next = unmerged;
			merged_prev_next = &unmerged->next;
			unmerged->next = NULL;
		} else
			/*
			 * Save the next pointer, so that if the next update has
			 * is merged, it can be removed from the original list.
			 */
			unmerged_prev_next = &unmerged->next;
	}

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

	/* An update based on a missing RCS patch matches nothing. */
	if (patch->missing)
		return;

	/* Search for updates sharing an author and comment */
	unmerged_prev_next = unmerged_head;
	merged_prev_next = &merge_head->next;
	for (unmerged = *unmerged_head; unmerged; unmerged = unmerged_next) {
		unmerged_next = unmerged->next;

		/* $ProjectRevision$ updates are merged separately. */
		if (unmerged->projrev_update)
			goto not_match;

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

		/* Don't merge updates when the RCS patch is missing. */
		if (upd_patch->missing)
			goto not_match;

		if (!strcasecmp(upd_ver->author, ver->author)
		 && !strcmp(upd_patch->log, patch->log)) {
			/* Remove from the old list. */
			*unmerged_prev_next = unmerged->next;

			/* Append this update to the commit. */
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
	static const char projrev_update_msg[] =
"Update $ProjectRevision$ keyword\n\
\n\
$ProjectRevision$ is an RCS-style keyword supported by MKS Source Integrity.  It\n\
expands to the project.pj file revision being used to check-out the file.  This\n\
commit has been automatically generated to update this keyword, in all files\n\
which have it, after a new project.pj revision.  The author and timestamp of\n\
this commit are the author and timestamp of the project.pj revision.\n";

	struct file_change *update;
	const struct file_change *u;
	const struct rcs_version *ver;
	struct git_commit *head, **prev_next, *c;

	/*
	 * Batch together updates which have the same author and revision
	 * comment.
	 */
	prev_next = &head;
	for (update = update_list; update; update = update_list) {
		update_list = update->next;

		c = xcalloc(1, sizeof *c, __func__);
		c->branch = branch;
		c->changes.updates = update;

		/* Updates for $ProjectRevision$ are handled differently */
		if (update->projrev_update) {
			/*
			 * Use the timestamp and author of the project revision
			 * (the checkpoint) as the timestamp and author of the
			 * $ProjectRevision$ update.
			 */
			ver = rcs_file_find_version(project, &pj_revnum_cur,
				true);
			c->date = cp_date;
			c->committer = author_map(ver->author);

			c->commit_msg = xstrdup(projrev_update_msg, __func__);

			/* Merge with other $ProjectRevision$ updates */
			merge_projrev_updates(update, &update_list);
			goto next;
		}

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

		/*
		 * Use the file revision with the newest date as the commit
		 * timestamp.
		 *
		 * Note: MKSSI stores the mtime of the file as the revision
		 * timestamp, which can differ considerably from when the user
		 * checked-in the updated file.  For example, if a user edits
		 * a file but waits two months to check-in that change, the
		 * timestamp stored for that file revision will be from two
		 * months ago.  We use this timestamp anyway, for lack of a
		 * better alternative.
		 */
		c->date = 0;
		for (u = c->changes.updates; u; u = u->next) {
			ver = rcs_file_find_version(u->file, &u->newrev, true);
			c->date = max(c->date, ver->date.value);
		}

		/*
		 * Re-sort the updates within the commit, so that the changes
		 * will be listed in the commit message in the same order as
		 * tools like git diff and gitk will display.
		 */
		c->changes.updates =
			change_list_sort_by_name(c->changes.updates);

		c->commit_msg = commit_msg_updates(c->changes.updates);

next:
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
	struct git_commit *rename_commit, *add_commits, *update_commits,
		*delete_commit, *list;

	rename_commit = merge_renames(branch, changes->renames, cp_date);
	changes->renames = NULL;

	add_commits = merge_adds(branch, changes->adds, cp_date);
	changes->adds = NULL;

	update_commits = merge_updates(branch, changes->updates, cp_date);
	changes->updates = NULL;

	delete_commit = merge_deletes(branch, changes->deletes, cp_date);
	changes->deletes = NULL;

	/* The ordering of these commits is important. */
	list = NULL;
	commit_list_append(&list, rename_commit);
	commit_list_append(&list, add_commits);
	commit_list_append(&list, update_commits);
	commit_list_append(&list, delete_commit);
	return list;
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
