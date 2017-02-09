/*
 * Build a list of changes that occurred between two project revisions (that is,
 * between project checkpoints).  The list shows added files, updated files, and
 * deleted files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interfaces.h"

/* find added files */
static struct file_change *
find_adds(const struct rcs_file_revision *old,
	const struct rcs_file_revision *new)
{
	const struct rcs_file_revision *o, *n;
	struct file_change *head, **prev_next, *change;

	/* Any file in new missing from old is an added file */
	head = NULL;
	prev_next = &head;
	for (n = new; n; n = n->next) {
		for (o = old; o; o = o->next)
			if (n->file == o->file)
				break;
		if (!o) {
			change = xcalloc(1, sizeof *change, __func__);
			change->file = n->file;
			change->newrev = n->rev;
			*prev_next = change;
			prev_next = &change->next;
		}
	}
	return head;
}

/* find updated file revisions */
static struct file_change *
find_updates(const struct rcs_file_revision *old,
	const struct rcs_file_revision *new)
{
	const struct rcs_file_revision *o, *n;
	struct file_change *head, **prev_next, *change;

	/*
	 * Any file in both old and new with an altered file revision is
	 * updated.  This applies whether the file revision has been
	 * incremented, branched, or reverted.
	 */
	head = NULL;
	prev_next = &head;
	for (o = old; o; o = o->next)
		for (n = new; n; n = n->next)
			if (o->file == n->file
			 && !rcs_number_equal(&o->rev, &n->rev)) {
				change = xcalloc(1, sizeof *change, __func__);
				change->file = n->file;
				change->oldrev = o->rev;
				change->newrev = n->rev;
				*prev_next = change;
				prev_next = &change->next;
			}
	return head;
}

/* find deleted files */
static struct file_change *
find_deletes(const struct rcs_file_revision *old,
	const struct rcs_file_revision *new)
{
	const struct rcs_file_revision *o, *n;
	struct file_change *head, **prev_next, *change;

	/* Any file in old missing from new is a deleted file */
	head = NULL;
	prev_next = &head;
	for (o = old; o; o = o->next) {
		for (n = new; n; n = n->next)
			if (o->file == n->file)
				break;
		if (!n) {
			change = xcalloc(1, sizeof *change, __func__);
			change->file = o->file;
			change->oldrev = o->rev;
			*prev_next = change;
			prev_next = &change->next;
		}
	}
	return head;
}

/* look for files that were added _and_ updated */
static struct file_change *
adjust_adds(struct file_change *adds, time_t old_date)
{
	struct file_change *c, *update, *new_updates;
	const struct rcs_version *prevver;
	struct rcs_number prevrev;

	/*
	 * We might see a new file with rev. 1.3, but the file was actually
	 * added with rev. 1.1 and updated to 1.2 then 1.3.  We want the add
	 * change to point to the original revision and to generate updates
	 * for subsequent revisions.  Note that the added revision is not
	 * always rev. 1.1 -- not if the file was deleted and re-added.
	 */
	new_updates = NULL;
	for (c = adds; c; c = c->next) {
		prevrev = c->newrev;
		for (;;) {
			if (!rcs_number_decrement(&prevrev))
				break; /* Nothing is previous to rev. 1.1 */

			prevver = rcs_file_find_version(c->file, &prevrev,
				false);
			if (!prevver || prevver->checkpointed
			 || prevver->date <= old_date)
				break;

			/*
			 * Previous version exists, was never checkpointed,
			 * and has a timestamp subsequent to the previous
			 * checkpoint.  Thus we assume the add was actually
			 * of the previous version or its predecessors and
			 * that an update followed.
			 */
			update = xcalloc(1, sizeof *update, __func__);
			update->file = c->file;
			update->oldrev = prevrev;
			update->newrev = c->newrev;
			update->next = new_updates;
			new_updates = update;

			c->newrev = prevrev;
		}
	}
	return new_updates;
}

/* expand multi-revision updates to include an update for every revision */
static struct file_change *
adjust_updates(struct file_change *updates)
{
	struct file_change *c, *new_updates, *update;
	struct rcs_number prevrev;
	const struct rcs_patch *patch;

	/*
	 * For example, if a file revision jumps from 1.2 to 1.5, we want to
	 * have separate update events for 1.2 -> 1.3, 1.3 -> 1.4, and 1.4 ->
	 * 1.5.
	 */
	new_updates = NULL;
	for (c = updates; c; c = c->next) {
		/*
		 * Make no adjustment if the revision number is moving backward
		 * (e.g., from 1.24 to 1.22; something MKSSI allows).  Reverted
		 * file revisions will be outputted as a single, unique event.
		 */
		if (rcs_number_compare(&c->oldrev, &c->newrev) > 0)
			continue;

		prevrev = c->newrev;
		for (;;) {
			rcs_number_decrement(&prevrev);
			if (rcs_number_compare(&c->oldrev, &prevrev) >= 0)
				break;

			patch = rcs_file_find_patch(c->file, &prevrev, false);
			if (!patch) {
				/*
				 * It would be nice if this was a fatal error,
				 * but we do run into it...
				 */
				fprintf(stderr, "warning: cannot export file "
					"\"%s\" rev. %s, missing patch\n",
					c->file->name,
					rcs_number_string_sb(&prevrev));
				continue;
			}

			/*
			 * Don't clutter the history with duplicate revisions.
			 *
			 * When a file is branched, a duplicate revision is
			 * automatically generated; it contains no changes.
			 * File rev. 1.7 might have a duplicate rev. 1.7.1.1.
			 * The actual changes would normally be checked in at
			 * rev. 1.7.1.2.  To avoid cluttering the Git history,
			 * we show rev. 1.7 -> 1.7.1.2, skipping the duplicate
			 * revision.
			 *
			 * The duplicate revision is only shown if it was
			 * created and checkpointed without any subsequent
			 * revision.
			 */
			if (!strcmp(patch->log, "Duplicate revision\n"))
				continue;

			update = xcalloc(1, sizeof *update, __func__);
			update->file = c->file;
			update->oldrev = prevrev;
			update->newrev = c->newrev;
			update->next = new_updates;
			new_updates = update;

			c->newrev = prevrev;
		}
	}
	return new_updates;
}

/* look for files that were updated _and_ deleted */
static struct file_change *
adjust_deletes(struct file_change *deletes, time_t new_date)
{
	struct file_change *c, *update, *new_updates;
	const struct rcs_version *nextver;
	struct rcs_number nextrev;

	/*
	 * A file can be updated and deleted as part of the same checkpoint.
	 * For example, checkpoint A might point to file rev. 1.1, and
	 * checkpoint B might show the file as deleted.  However, prior to the
	 * deletion, the file revision might have been updated.  Such new
	 * revisions must be distinguished from revisions which exist because
	 * the file was re-added at a later date.
	 */
	new_updates = NULL;
	for (c = deletes; c; c = c->next) {
		nextrev = c->oldrev;
		for (;;) {
			rcs_number_increment(&nextrev);
			nextver = rcs_file_find_version(c->file, &nextrev,
				false);
			if (!nextver || nextver->checkpointed
			 || nextver->date > new_date)
				break;

			/*
			 * Next version exists, was never checkpointed, and
			 * has a timestamp prior to the current checkpoint.
			 * Thus we assume the delete was actually of the next
			 * version or its successors and that an update
			 * preceded.
			 */
			update = xcalloc(1, sizeof *update, __func__);
			update->file = c->file;
			update->oldrev = c->oldrev;
			update->newrev = nextrev;
			update->next = new_updates;
			new_updates = update;

			c->oldrev = nextrev;
		}
	}
	return new_updates;
}

/* remove nonexistent file revisions from list */
static struct file_change *
remove_nonexistent_file_revisions(struct file_change *changes)
{
	struct file_change *c, **prev_next;
	const struct rcs_version *ver;
	const struct rcs_patch *patch;

	prev_next = &changes;
	for (c = *prev_next; c; c = *prev_next) {
		ver = rcs_file_find_version(c->file, &c->newrev, false);
		patch = rcs_file_find_patch(c->file, &c->newrev, false);
		if (ver && patch)
			prev_next = &c->next;
		else {
			/*
			 * It would be nice if this was a fatal error, but we
			 * do run into it...
			 */
			fprintf(stderr, "warning: cannot export file \"%s\" "
				"rev. %s, missing patch or version metadata\n",
				c->file->name,
				rcs_number_string_sb(&c->newrev));
			*prev_next = c->next;
		}
	}
	return changes;
}

/* compare two adds for sorting purposes */
static int
compare_adds(const struct file_change *a, const struct file_change *b)
{
	struct rcs_version *aver, *bver;

	/*
	 * Ideally we would sort the files by when they were added to the
	 * project.  We cannot do so, since the MKSSI timestamp is the mtime of
	 * the file when it was added, rather than the actual time it was added.
	 * Sort by timestamp anyway, as an approximation of the ideal sort.
	 */
	aver = rcs_file_find_version(a->file, &a->newrev, true);
	bver = rcs_file_find_version(b->file, &b->newrev, true);
	if (aver->date < bver->date)
		return -1;
	if (aver->date > bver->date)
		return 1;

	/* If the timestamp is the same for some reason, sort by name. */
	return strcasecmp(a->file->name, b->file->name);
}

/* compare two updates for sorting purposes */
static int
compare_updates(const struct file_change *a, const struct file_change *b)
{
	/*
	 * When dealing with updated revisions to a file, the earlier revision
	 * must sort before the later revision.
	 */
	if (a->file == b->file)
		return rcs_number_compare(&a->newrev, &b->newrev);

	/* Otherwise the sort is the same as added files */
	return compare_adds(a, b);
}

/* compare two deletes for sorting purposes */
static int
compare_deletes(const struct file_change *a, const struct file_change *b)
{
	/* Deletes have no timestamp, so sort by name. */
	return strcasecmp(a->file->name, b->file->name);
}

/* sort a list of changes using the given comparison function */
static struct file_change *
sort_changes(struct file_change *list,
	int (*compare)(const struct file_change *a,
		const struct file_change *b))
{
	struct file_change *head, **prev_next, *c, *cc;

	/*
	 * Pop each change off the front of the list and insertion sort it into
	 * the new list.
	 */
	head = NULL;
	for (c = list; c; c = list) {
		list = c->next;
		prev_next = &head;
		for (cc = head; cc; cc = cc->next) {
			if (compare(c, cc) < 0)
				break;
			prev_next = &cc->next;
		}
		c->next = cc;
		*prev_next = c;
	}
	return head;
}

/* append a list to another list */
static void
change_list_append(struct file_change **list, struct file_change *append)
{
	struct file_change **prev_next, *c;

	prev_next = list;
	for (c = *prev_next; c; c = *prev_next)
		prev_next = &c->next;
	*prev_next = append;
}

/* find changeset between two lists of file revisions */
void
changeset_build(const struct rcs_file_revision *old, time_t old_date,
	const struct rcs_file_revision *new, time_t new_date,
	struct file_change_lists *changes)
{
	struct file_change *extra_updates;

	changes->adds = find_adds(old, new);
	changes->updates = find_updates(old, new);
	changes->deletes = find_deletes(old, new);

	extra_updates = adjust_adds(changes->adds, old_date);
	change_list_append(&changes->updates, extra_updates);

	extra_updates = adjust_deletes(changes->deletes, new_date);
	change_list_append(&changes->updates, extra_updates);

	extra_updates = adjust_updates(changes->updates);
	change_list_append(&changes->updates, extra_updates);

	changes->adds = remove_nonexistent_file_revisions(changes->adds);
	changes->updates = remove_nonexistent_file_revisions(changes->updates);

	changes->adds = sort_changes(changes->adds, compare_adds);
	changes->updates = sort_changes(changes->updates, compare_updates);
	changes->deletes = sort_changes(changes->deletes, compare_deletes);
}

/* free a list of changes */
static void
change_list_free(struct file_change *list)
{
	struct file_change *c, *cnext;

	for (c = list; c; c = cnext) {
		cnext = c->next;
		free(c);
	}
}

/* free a changeset */
void
changeset_free(struct file_change_lists *changes)
{
	change_list_free(changes->adds);
	change_list_free(changes->updates);
	change_list_free(changes->deletes);
	changes->adds = NULL;
	changes->updates = NULL;
	changes->deletes = NULL;
}
