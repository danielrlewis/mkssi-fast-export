/*
 * Copyright (c) 2017, 2019-2020 Datalight, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
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
			change->member_type_other = n->member_type_other;
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
			 && (!rcs_number_equal(&o->rev, &n->rev)
			 || o->member_type_other != n->member_type_other)) {
				change = xcalloc(1, sizeof *change, __func__);
				change->file = n->file;
				change->canonical_name = n->canonical_name;
				change->oldrev = o->rev;
				change->newrev = n->rev;
				change->member_type_other =
					n->member_type_other;
				*prev_next = change;
				prev_next = &change->next;
			}

	/*
	 * The project revision for the tip of each branch is the same as the
	 * project revision of the last checkpoint on that branch, so there is
	 * no need to update $ProjectRevision$ when exporting tip revisions.
	 */
	if (exporting_tip)
		goto out;

	/*
	 * If a file has the $ProjectRevision$ keyword, then each new project
	 * revision will update the file.
	 */
	for (n = new; n; n = n->next)
		if (n->ver && n->ver->kw_projrev) {
			/*
			 * Ignore files that weren't part of the prior project
			 * revision -- i.e., files which are being added.  The
			 * $ProjectRevision$ will be updated as part of the add,
			 * so there is no need for a separate update.
			 */
			for (o = old; o; o = o->next)
				if (o->file == n->file)
					break;
			if (!o)
				continue;

			/*
			 * Ignore files that are already on the update list.
			 * The $ProjectRevision$ will be updated implicitly; we
			 * do not need a separate update for it.
			 */
			for (change = head; change; change = change->next)
				if (change->file == n->file)
					break;
			if (change)
				continue;

			/* Create $ProjectRevision$ update */
			change = xcalloc(1, sizeof *change, __func__);
			change->file = n->file;
			change->canonical_name = n->canonical_name;
			change->projrev_update = true;
			change->oldrev = change->newrev = n->rev;
			*prev_next = change;
			prev_next = &change->next;
		}

out:
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

/* update rename path to account for parent directory renames */
static void
apply_parent_dir_renames_to_rename(struct file_change *rename,
	const struct file_change *dir_renames)
{
	const struct file_change *r, *rlong;

	/*
	 * If the given rename occurs in a directory that is *also* being
	 * renamed, then this rename needs to use the new name of the renamed
	 * directory.  The new name is already being used in the new path, but
	 * it needs to be used in the old path, too.
	 *
	 * If more than one of the parent directories is being renamed, use the
	 * longest match, which includes all the renamed directories.
	 */
	rlong = NULL;
	for (r = dir_renames; r; r = r->next)
		if (is_parent_dir(r->old_canonical_name,
		 rename->old_canonical_name) &&
		 (!rlong || strlen(r->canonical_name) >
		 strlen(rlong->canonical_name)))
			rlong = r;

	/*
	 * If there is a renamed parent directory, update the old path to
	 * account for that prior rename.
	 *
	 * TODO: rename->old_canonical_name is const-qualified, but we're
	 * ignoring that because we know that it points to memory that was just
	 * allocated and thus can be modified.
	 */
	if (rlong)
		memcpy((char *)rename->old_canonical_name,
			rlong->canonical_name,
			strlen(rlong->canonical_name));
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
				 *
				 * The paths include a trailing path separator,
				 * so the -2 on the next line gets us a pointer
				 * to the last name character for the directory.
				 */
				oname = od->path + od->len - 2;
				while (oname > od->path && *oname != '/')
					--oname;
				nname = nd->path + (oname - od->path);
				if (!strncmp(oname, nname, nd->len - (nname -
				 nd->path)))
					continue;

				rename = xcalloc(1, sizeof *rename, __func__);

				/*
				 * This rename might require file modifications
				 * to update RCS keyword expansions.  This list
				 * is used later to determine whether that is
				 * necessary.
				 */
				rename->old_frevs = old;

				/*
				 * Note that rename->file is left as NULL.
				 * Directories don't have RCS files, so there
				 * would be nothing to point at.  Later on we
				 * use file == NULL as an indicator that the
				 * rename is for a directory.
				 */

				/*
				 * The rename commit cannot have trailing path
				 * separators, but the in-memory paths have them
				 * (included in the length), which is why the
				 * name copy is cut off at len-1.
				 */
				rename->buf = xmalloc(od->len + nd->len,
					__func__);
				path = rename->buf;
				memcpy(path, od->path, od->len-1);
				path[od->len-1] = '\0';
				rename->old_canonical_name = path;
				path += od->len;
				memcpy(path, nd->path, nd->len-1);
				path[nd->len-1] = '\0';
				rename->canonical_name = path;

				/*
				 * If the old path references a parent directory
				 * that is also being renamed, we need to
				 * account for that.
				 *
				 * This is dependent on sort order.  The
				 * directory lists are sorted by name and later
				 * the rename list is also sorted by name.
				 */
				apply_parent_dir_renames_to_rename(
					rename, head);

				/*
				 * Insert the new rename at the head of the
				 * list.  Note that the list is later resorted
				 * by name.
				 */
				rename->next = head;
				head = rename;
			}

	dir_list_free(old_dirs);
	dir_list_free(new_dirs);

	return head;
}

/* find files whose name capitalization changed */
static struct file_change *
find_implicit_file_renames(const struct rcs_file_revision *old,
	const struct rcs_file_revision *new,
	const struct file_change *dir_renames)
{
	const struct rcs_file_revision *o, *n;
	struct file_change *rename, *head;
	const char *opath, *npath, *oname, *nname;
	char *path;

	/*
	 * Search for files which had their file name capitalization changed.
	 *
	 * Not exactly sure how this situation arises.  Only seen in project
	 * history from the 1990s, so it might have something to do with
	 * handling of LFN/8.3 names.
	 */
	head = NULL;
	for (o = old; o; o = o->next)
		for (n = new; n; n = n->next) {
			opath = oname = o->canonical_name;
			npath = nname = n->canonical_name;
			if (strchr(opath, '/'))
				oname = strrchr(opath, '/') + 1;
			if (strchr(npath, '/'))
				nname = strrchr(npath, '/') + 1;

			/*
			 * Check for paths which are the same, case insensitive,
			 * but differ in the final component (the file name).
			 *
			 * Higher level path components are directories, which
			 * are handled separately.
			 */
			if (!strcasecmp(opath, npath) && strcmp(oname, nname)) {
				rename = xcalloc(1, sizeof *rename, __func__);

				/*
				 * This rename might require file modifications
				 * to update RCS keyword expansions.  This list
				 * is used later to determine whether that is
				 * necessary.
				 */
				rename->old_frevs = old;

				/*
				 * Populate the file pointer.  While we don't
				 * need any of the RCS metadata, having a
				 * non-NULL file pointer is used later to
				 * distinguish file renames from directory
				 * renames.
				 */
				rename->file = n->file;

				rename->buf = xmalloc(strlen(opath) +
					strlen(npath) + 2, __func__);
				path = rename->buf;
				strcpy(path, opath);
				rename->old_canonical_name = path;
				path += strlen(opath) + 1;
				strcpy(path, npath);
				rename->canonical_name = path;

				/*
				 * If the old path references a parent directory
				 * that is also being renamed, we need to
				 * account for that.
				 *
				 * This assumes that directory renames are
				 * committed prior to file renames.
				 */
				apply_parent_dir_renames_to_rename(
					rename, dir_renames);

				/*
				 * Insert the new rename at the head of the
				 * list.  Note that the list is later resorted
				 * by name.
				 */
				rename->next = head;
				head = rename;
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
			if (c->file->dummy)
				break;

			if (!rcs_number_decrement(&prevrev))
				break; /* Nothing is previous to rev. 1.1 */

			prevver = rcs_file_find_version(c->file, &prevrev,
				false);
			if (!prevver || prevver->checkpointed
			 || prevver->date.value <= old_date)
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
			if (patch->log &&
			 !strcmp(patch->log, "Duplicate revision\n"))
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
			if (c->file->dummy)
				break;

			rcs_number_increment(&nextrev);
			nextver = rcs_file_find_version(c->file, &nextrev,
				false);
			if (!nextver || nextver->checkpointed
			 || nextver->date.value > new_date)
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
	const struct rcs_file *f;
	const struct rcs_version *ver;
	const struct rcs_patch *patch;
	bool keep;

	prev_next = &changes;
	for (c = *prev_next; c; c = *prev_next) {
		f = c->file;

		if (f->dummy)
			/*
			 * Keep files with no RCS master if there's a copy in
			 * the project directory that we're exporting.
			 */
			keep = f->binary && f->other_blob_mark;
		else {
			ver = rcs_file_find_version(f, &c->newrev, false);
			patch = rcs_file_find_patch(f, &c->newrev, false);
			keep = ver && patch;
		}

		if (keep)
			prev_next = &c->next;
		else {
			/*
			 * It would be nice if this was a fatal error, but we
			 * do run into it...
			 */
			fprintf(stderr, "warning: cannot export file \"%s\" "
				"rev. %s, missing patch or version metadata\n",
				f->name, rcs_number_string_sb(&c->newrev));
			*prev_next = c->next;

			if (c->buf)
				free(c->buf);
			free(c);
		}
	}
	return changes;
}

/* adjust delete paths for renames */
static void
adjust_deletes_for_renames(const struct file_change *renames,
	struct file_change *deletes)
{
	const struct file_change *r, *rlong;
	struct file_change *d;

	/*
	 * Renames are committed prior to deletions.  However, deletions use the
	 * old pre-rename path: deletions are inferred from the absence of a
	 * file that was previously listed, and the path comes from the old
	 * listing.  This isn't necessary for adds/updates, where the paths are
	 * from the new version of the project file.
	 */
	for (d = deletes; d; d = d->next) {
		rlong = NULL;
		for (r = renames; r; r = r->next) {
			if (r->file) { /* If this is a rename of a file */
				if (!strcasecmp(d->canonical_name,
				 r->canonical_name) && strcmp(d->canonical_name,
				 r->canonical_name)) {
					rlong = r;
					break;
				}
			} else if (is_parent_dir(r->canonical_name,
			 d->canonical_name) && strncmp(d->canonical_name,
			 r->canonical_name, strlen(r->canonical_name))) {
				if (!rlong || strlen(r->canonical_name) >
				 strlen(rlong->canonical_name))
					rlong = r;
			}
		}
		if (rlong) {
			d->buf = xstrdup(d->canonical_name, __func__);
			memcpy(d->buf, rlong->canonical_name,
				strlen(rlong->canonical_name));
			d->canonical_name = d->buf;
		}
	}
}

/* compare two changes by name for sorting purposes */
static int
compare_by_name(const struct file_change *a, const struct file_change *b)
{
	return strcmp(a->canonical_name, b->canonical_name);
}

/* get a timestamp for a file change */
static time_t
get_change_date(const struct file_change *c)
{
	struct rcs_version *ver;

	/*
	 * Updates for the $ProjectRevision$ have no file revision.  The correct
	 * date to use is the date of the project revision itself, since that is
	 * what caused the $ProjectRevision$ keyword to be updated.
	 */
	if (c->projrev_update) {
		ver = rcs_file_find_version(project, &pj_revnum_cur, true);
		return ver->date.value;
	}

	/*
	 * Dummy files have no RCS metadata and thus no dates.  Return a large
	 * value so that dummy files will sort after normal files.
	 */
	if (c->file->dummy)
		return (time_t)~0;

	/* The normal case: use the file revision date. */
	ver = rcs_file_find_version(c->file, &c->newrev, true);
	return ver->date.value;
}

/* compare two changes by date for sorting purposes */
static int
compare_by_date(const struct file_change *a, const struct file_change *b)
{
	time_t adate, bdate;

	adate = get_change_date(a);
	bdate = get_change_date(b);
	if (adate < bdate)
		return -1;
	if (adate > bdate)
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
	struct file_change *extra_updates, *dir_renames, *file_renames;

	dir_renames = find_implicit_dir_renames(old, new);
	file_renames = find_implicit_file_renames(old, new, dir_renames);

	/*
	 * Code elsewhere assumes that directory renames will occur prior to
	 * file renames.
	 */
	dir_renames = sort_changes(dir_renames, compare_by_name);
	file_renames = sort_changes(file_renames, compare_by_name);
	changes->renames = dir_renames;
	change_list_append(&changes->renames, file_renames);

	changes->adds = find_adds(old, new);
	changes->updates = find_updates(old, new);
	changes->deletes = find_deletes(old, new);

	extra_updates = adjust_adds(changes->adds, old_date);
	change_list_append(&changes->updates, extra_updates);

	adjust_deletes_for_renames(changes->renames, changes->deletes);
	extra_updates = adjust_deletes(changes->deletes, new_date);
	change_list_append(&changes->updates, extra_updates);

	extra_updates = adjust_updates(changes->updates);
	change_list_append(&changes->updates, extra_updates);

	changes->adds = remove_nonexistent_file_revisions(changes->adds);
	changes->updates = remove_nonexistent_file_revisions(changes->updates);

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

		if (c->buf)
			free(c->buf);
		free(c);
	}
}

/* sort a list of changes by name */
struct file_change *
change_list_sort_by_name(struct file_change *list)
{
	return sort_changes(list, compare_by_name);
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
