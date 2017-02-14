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
			change->canonical_name = n->canonical_name;
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
				change->canonical_name = n->canonical_name;
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
			change->canonical_name = o->canonical_name;
			change->oldrev = o->rev;
			*prev_next = change;
			prev_next = &change->next;
		}
	}
	return head;
}

/* find directories that were implicitly renamed by added/deleted files */
static struct file_change *
find_implicit_dir_renames(const struct rcs_file_revision *old,
	const struct rcs_file_revision *new)
{
	const struct rcs_file_revision *o, *n;
	struct file_change *rename, *head;
	struct dir_path *old_dirs, *new_dirs, *path_dirs, *od, *nd;
	const char *oname, *nname;
	char *path;

	/* get a list of directories in the previous project revision */
	old_dirs = NULL;
	for (o = old; o; o = o->next) {
		path_dirs = dir_list_from_path(o->canonical_name);
		path_dirs = dir_list_remove_duplicates(path_dirs, old_dirs);
		old_dirs = dir_list_append(old_dirs, path_dirs);
	}

	/* get a list of directories in the current project revision */
	new_dirs = NULL;
	for (n = new; n; n = n->next) {
		path_dirs = dir_list_from_path(n->canonical_name);
		path_dirs = dir_list_remove_duplicates(path_dirs, new_dirs);
		new_dirs = dir_list_append(new_dirs, path_dirs);
	}

	/*
	 * Search for directories which have been implicitly renamed (same name
	 * but different case) by added/deleted files.
	 */
	head = NULL;
	for (od = old_dirs; od; od = od->next)
		for (nd = new_dirs; nd; nd = nd->next)
			if (od->len == nd->len && !strncasecmp(od->path,
			 nd->path, nd->len) && strncmp(od->path, nd->path,
			 nd->len)) {
			 	/*
			 	 * If the difference does not occur in the final
			 	 * directory, ignore it, it will be handled by a
			 	 * different iteration through the loop.
			 	 */
				oname = od->path + od->len - 1;
				while (oname > od->path && *oname != '/')
					--oname;
				nname = nd->path + (oname - od->path);
				if (!strncmp(oname, nname, nd->len - (nname -
				 nd->path)))
					continue;

				rename = xcalloc(1, sizeof *rename, __func__);

				/*
				 * TODO: These memory allocated for these paths
				 * is leaked; it is needed only briefly but is
				 * never freed.  This code path is executed very
				 * rarely in a typical MKSSI project, so
				 * overlooking the memory leak for now.
				 */
				path = xmalloc(od->len + 1, __func__);
				memcpy(path, od->path, od->len);
				path[od->len] = '\0';
				rename->old_canonical_name = path;
				path = xmalloc(nd->len + 1, __func__);
				memcpy(path, nd->path, nd->len);
				path[nd->len] = '\0';
				rename->canonical_name = path;

				rename->next = head;
				head = rename;
			}

	dir_list_free(old_dirs);
	dir_list_free(new_dirs);

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
			update->canonical_name = c->canonical_name;
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
			update->canonical_name = c->canonical_name;
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
			update->canonical_name = c->canonical_name;
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

/* compare two changes by name for sorting purposes */
static int
compare_by_name(const struct file_change *a, const struct file_change *b)
{
	return strcasecmp(a->canonical_name, b->canonical_name);
}

/* compare two changes by date for sorting purposes */
static int
compare_by_date(const struct file_change *a, const struct file_change *b)
{
	struct rcs_version *aver, *bver;

	aver = rcs_file_find_version(a->file, &a->newrev, true);
	bver = rcs_file_find_version(b->file, &b->newrev, true);
	if (aver->date < bver->date)
		return -1;
	if (aver->date > bver->date)
		return 1;

	/* If the timestamp is the same for some reason, sort by name. */
	return compare_by_name(a, b);
}

/* compare two changes by file revision */
static int
compare_by_rev(const struct file_change *a, const struct file_change *b)
{
	/*
	 * When dealing with updated revisions to a file, the earlier revision
	 * must sort before the later revision.
	 */
	if (a->file == b->file)
		return rcs_number_compare(&a->newrev, &b->newrev);

	return 0;
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

	changes->renames = find_implicit_dir_renames(old, new);
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

	changes->renames = sort_changes(changes->renames, compare_by_name);
	changes->adds = sort_changes(changes->adds, compare_by_date);
	changes->updates = sort_changes(changes->updates, compare_by_date);
	changes->updates = sort_changes(changes->updates, compare_by_rev);
	changes->deletes = sort_changes(changes->deletes, compare_by_name);
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
	change_list_free(changes->renames);
	change_list_free(changes->adds);
	change_list_free(changes->updates);
	change_list_free(changes->deletes);
	changes->renames = NULL;
	changes->adds = NULL;
	changes->updates = NULL;
	changes->deletes = NULL;
}
