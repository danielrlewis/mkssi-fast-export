#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "interfaces.h"

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
	int len, i;
	const struct file_change *a;
	char *lead, *msg;

	for (a = adds, count = 0; a; a = a->next, ++count)
		;

	if (count > 1) {
		len = snprintf(NULL, 0, "Add %u files\n\n", count) + 1;
		lead = xmalloc(len, __func__);
		snprintf(lead, len, "Add %u files\n\n", count);
	} else {
		len = snprintf(NULL, 0, "Add file %s\n\n", adds->file->name)
			+ 1;
		if (len <= 80) {
			lead = xmalloc(len, __func__);
			snprintf(lead, len, "Add file %s\n\n",
				adds->file->name);
		} else {
			len = strlen("Add 1 file\n\n") + 1;
			lead = xmalloc(len, __func__);
			strcpy(lead, "Add 1 file\n\n");
		}
	}

	len = strlen(lead);
	for (a = adds; a; a = a->next)
		len += snprintf(NULL, 0, "#mkssi: add %s rev. %s\n",
			a->file->name, rcs_number_string_sb(&a->newrev));

	++len; /* For terminating NUL */
	msg = xmalloc(len, __func__);

	i = snprintf(msg, len, "%s", lead);
	free(lead);

	for (a = adds; a; a = a->next)
		i += snprintf(msg + i, len - i, "#mkssi: add %s rev. %s\n",
			a->file->name, rcs_number_string_sb(&a->newrev));

	return msg;
}

/* generate commit message for an update commit */
static char *
commit_msg_updates(const struct file_change *updates)
{
	unsigned int count;
	int len, i;
	char *msg, *lead;
	const char *shared_log, *shared_label, *label, *pos;
	char revstr_old[RCS_MAX_REV_LEN], revstr_new[RCS_MAX_REV_LEN];
	struct rcs_patch *patch;
	const struct file_change *u;

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

		len = snprintf(NULL, 0, "Revert file %s to rev. %s\n\n",
			u->file->name, rcs_number_string_sb(&u->newrev)) + 1;
		if (len <= 80) {
			lead = xmalloc(len, __func__);
			snprintf(lead, len, "Revert file %s to rev. %s\n\n",
				u->file->name,
				rcs_number_string_sb(&u->newrev));
		} else {
			len = strlen("Revert file revision\n\n") + 1;
			lead = xmalloc(len, __func__);
			strcpy(lead, "Revert file revision\n\n");
		}

		goto generate_message;
	}

	/* Do all the updates have the same check-in comment? */
	shared_log = NULL;
	for (u = updates; u; u = u->next) {
		patch = rcs_file_find_patch(u->file, &u->newrev, true);
		if (!shared_log)
			shared_log = patch->log;
		else if (strcmp(shared_log, patch->log)) {
			shared_log = NULL;
			break;
		}
	}

	/* Do all the updates have a common MKSSI label? */
	shared_label = NULL;
	for (u = updates; u; u = u->next) {
		label = file_revision_label(u->file, &u->newrev);
		if (!label) {
			shared_label = NULL;
			break;
		}
		if (!shared_label)
			shared_label = label;
		else if (strcmp(shared_label, label)) {
			shared_label = NULL;
			break;
		}
	}

	if (shared_log) {
		/* Only use the shared log message if it is non-empty. */
		for (pos = shared_log; *pos; ++pos)
			if (!isspace(*pos))
				break;
		if (*pos) {
			len = strlen(shared_log) + strlen("\n\n") + 1;
			lead = xmalloc(len, __func__);
			strcpy(lead, shared_log);
			strcat(lead, "\n\n");
		} else if (count > 1) {
			len = snprintf(NULL, 0, "Update %u files\n\n", count)
				+ 1;
			lead = xmalloc(len, __func__);
			snprintf(lead, len, "Update %u files\n\n", count);
		} else {
			len = snprintf(NULL, 0,
				"Update file %s to rev. %s\n\n",
				updates->file->name,
				rcs_number_string_sb(&updates->newrev)) + 1;
			if (len <= 80) {
				lead = xmalloc(len, __func__);
				snprintf(lead, len,
					"Update file %s to rev. %s\n\n",
					updates->file->name,
					rcs_number_string_sb(&updates->newrev));
			} else {
				len = strlen("Update 1 file\n\n") + 1;
				lead = xmalloc(len, __func__);
				strcpy(lead, "Update 1 file\n\n");
			}
		}
	} else if (shared_label) {
		len = snprintf(NULL, 0, "Check-in labeled %s\n\n",
			shared_label) + 1;
		lead = xmalloc(len, __func__);
		snprintf(lead, len, "Check-in labeled %s\n\n",
			shared_label);
	} else {
		/*
		 * If there is no shared log or label, currently the updates
		 * should not get put into the same commit.
		 */
		fatal_error("update commit with unassociated revisions");
		return NULL; /* unreachable */
	}

generate_message:
	len = strlen(lead);
	for (u = updates; u; u = u->next) {
		rcs_number_string(&u->newrev, revstr_new, sizeof revstr_new);
		rcs_number_string(&u->oldrev, revstr_old, sizeof revstr_old);
		label = file_revision_label(u->file, &u->newrev);
		len += snprintf(NULL, 0, "#mkssi: check-in %s rev. %s "
			"(was rev. %s)%s%s\n", u->file->name,
			revstr_new, revstr_old,
			label ? " labeled " : "",
			label ? label : "");
	}

	++len; /* For terminating NUL */
	msg = xmalloc(len, __func__);

	i = snprintf(msg, len, "%s", lead);
	free(lead);

	for (u = updates; u; u = u->next) {
		rcs_number_string(&u->newrev, revstr_new, sizeof revstr_new);
		rcs_number_string(&u->oldrev, revstr_old, sizeof revstr_old);
		label = file_revision_label(u->file, &u->newrev);
		i += snprintf(msg + i, len - i, "#mkssi: check-in %s rev. %s "
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
	int len, i;
	const struct file_change *d;
	char *lead, *msg;

	for (d = deletes, count = 0; d; d = d->next, ++count)
		;

	if (count > 1) {
		len = snprintf(NULL, 0, "Delete %u files\n\n", count) + 1;
		lead = xmalloc(len, __func__);
		snprintf(lead, len, "Delete %u files\n\n", count);
	} else {
		len = snprintf(NULL, 0, "Delete file %s\n\n",
			deletes->file->name)
			+ 1;
		if (len <= 80) {
			lead = xmalloc(len, __func__);
			snprintf(lead, len, "Delete file %s\n\n",
				deletes->file->name);
		} else {
			len = strlen("Delete 1 file\n\n") + 1;
			lead = xmalloc(len, __func__);
			strcpy(lead, "Delete 1 file\n\n");
		}
	}

	len = strlen(lead);
	for (d = deletes; d; d = d->next)
		len += snprintf(NULL, 0, "#mkssi: delete %s rev. %s\n",
			d->file->name, rcs_number_string_sb(&d->oldrev));

	++len; /* For terminating NUL */
	msg = xmalloc(len, __func__);

	i = snprintf(msg, len, "%s", lead);
	free(lead);

	for (d = deletes; d; d = d->next)
		i += snprintf(msg + i, len - i, "#mkssi: delete %s rev. %s\n",
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

	head = NULL;
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
	return head;
}

/* merge updates into commits */
static struct git_commit *
merge_updates(const char *branch, struct file_change *update_list,
	time_t cp_date)
{
	struct file_change *u, *update, **old_prev_next, **new_prev_next;
	struct git_commit *head, **prev_next, *c;
	const struct rcs_version *ver, *upd_ver;
	const struct rcs_patch *patch, *upd_patch;
	const char *label, *upd_label;
	bool found_label_matches;

	/*
	 * Batch together updates which:
	 *	a) have the same author and revision label; or
	 *	b) have the same author and revision comment
	 *
	 * The MKSSI timestamp is unreliable and is not used.
	 */

	head = NULL;
	prev_next = &head;
	for (update = update_list; update; update = update_list) {
		update_list = update->next;

		ver = rcs_file_find_version(update->file, &update->newrev,
			true);
		patch = rcs_file_find_patch(update->file, &update->newrev,
			true);

		c = xcalloc(1, sizeof *c, __func__);
		c->branch = branch;
		c->date = cp_date;
		c->changes.updates = update;

		/*
		 * If a file was reverted to an earlier version, there is no way
		 * to know who did it.
		 */
		if (rcs_number_compare(&update->newrev, &update->oldrev) < 0) {
			c->committer = &unknown_author;
			update->next = NULL;
			goto commit_message;
		}

		c->committer = author_map(ver->author);

		/* Is this revision labeled? */
		label = file_revision_label(update->file, &update->newrev);

		found_label_matches = false;
		if (label && false) {
			/* Search for updates sharing an author and label */
			old_prev_next = &update_list;
			new_prev_next = &update->next;
			for (u = update_list; u; u = u->next) {
				/*
				 * Never update the same file more than once in
				 * any commit -- that would lose revision
				 * history.
				 */
				if (u->file == update->file)
					goto not_label_match;
				/*
				 * Never merge reverted revisions -- these have
				 * no true author or label.  They are also rare.
				 */
				if (rcs_number_compare(&u->newrev, &u->oldrev)
				 < 0) {
					goto not_label_match;
				}
				upd_label = file_revision_label(u->file,
					&u->newrev);
				if (!upd_label)
					continue;
				upd_ver = rcs_file_find_version(u->file,
					&u->newrev, true);
				if (!strcasecmp(upd_ver->author, ver->author)
				 && !strcmp(upd_label, label)) {
				 	found_label_matches = true;

					/* Remove from the old list. */
					*old_prev_next = u->next;

					/* Append this add to the commit. */
					*new_prev_next = u;
					new_prev_next = &u->next;
				} else {
not_label_match:
					/*
					 * Save the next pointer, so that if the
					 * next update has the same author and
					 * label, it can be removed from the
					 * original list.
					 */
					old_prev_next = &u->next;
				}
			}
			*new_prev_next = NULL;
		}

		if (!found_label_matches) {
			/* Search for updates sharing an author and comment */
			old_prev_next = &update_list;
			new_prev_next = &update->next;
			for (u = update_list; u; u = u->next) {
				/*
				 * Never update the same file more than once in
				 * any commit -- that would lose revision
				 * history.
				 */
				if (u->file == update->file)
					goto not_log_match;
				/*
				 * Never merge reverted revisions -- these have
				 * no true author or log.  They are also rare.
				 */
				if (rcs_number_compare(&u->newrev, &u->oldrev)
				 < 0) {
					goto not_log_match;
				}
				upd_ver = rcs_file_find_version(u->file,
					&u->newrev, true);
				upd_patch = rcs_file_find_patch(u->file,
					&u->newrev, true);
				if (!strcasecmp(upd_ver->author, ver->author)
				 && !strcmp(upd_patch->log, patch->log)) {
					/* Append this add to the commit. */
					*new_prev_next = u;
					new_prev_next = &u->next;

					/* Remove from the old list. */
					*old_prev_next = u->next;
				} else {
not_log_match:
					/*
					 * Save the next pointer, so that if the
					 * next update has the same author and
					 * comment, it can be removed from the
					 * original list.
					 */
					old_prev_next = &u->next;
				}
			}
			*new_prev_next = NULL;
		}

commit_message:
		c->commit_msg = commit_msg_updates(c->changes.updates);

		/* Append this commit to the list. */
		*prev_next = c;
		prev_next = &c->next;
	}
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
