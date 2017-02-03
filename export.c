#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interfaces.h"

/* find a named project checkpoint by project revision number */
static const char *
pjrev_find_checkpoint(const struct rcs_number *pjrev)
{
	struct rcs_symbol *cp;

	for (cp = project->symbols; cp; cp = cp->next)
		if (rcs_number_equal(&cp->number, pjrev))
			return cp->symbol_name;
	return NULL;
}

/* find a branch by project revision number */
static const char *
pjrev_find_branch(const struct rcs_number *pjrev)
{
	struct rcs_symbol *b;

	/* Trunk project revisions go on the trunk, unless... */
	if (rcs_number_is_trunk(pjrev)) {
		/*
		 * ...unless we are dealing with one of those weird projects
		 * where the trunk history somehow got put onto an nameless
		 * branch revision (the trunk_branch), in which case any
		 * revisions >trunk_branch are not actually trunk revisions;
		 * they actually have no branch.
		 */
		if (trunk_branch.c
		 && rcs_number_compare(pjrev, &trunk_branch) > 0)
			return NULL;

		return "master";
	}

	for (b = project_branches; b; b = b->next)
		if (pjrev->c - 2 == b->number.c
		 && rcs_number_partial_match(pjrev, &b->number))
			return b->symbol_name;

	/* No matching branch for this project revision */
	return NULL;
}

/* print a progress message */
void
export_progress(const char *fmt, ...)
{
	va_list args;

	/*
	 * git fast-import will print any lines starting with "progress " to
	 * stdout.  The printed message includes the "progress" text.
	 */
	printf("progress - ");
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	printf("\n");
}

static void
blob_data_handler(struct rcs_file *file, const struct rcs_number *revnum,
	const char *data)
{
	static unsigned long blob_mark;
	struct rcs_version *ver;

	/*
	 * Put a comment in the stream which identifies the blob.  This is only
	 * for debugging.
	 */
	printf("# %s rev. %s\n", file->name, rcs_number_string_sb(revnum));

	printf("blob\n");
	printf("mark :%lu\n", ++blob_mark);
	printf("data %zu\n", strlen(data));
	printf("%s\n", data);

	ver = rcs_file_find_version(file, revnum, true);
	ver->blob_mark = blob_mark;
}

static void
export_blobs(void)
{
	struct rcs_file *f;
	unsigned long nf, i, progress, progress_printed;

	export_progress("exporting file revision blobs");

	/* count files to allow progress to be shown */
	nf = 0;
	for (f = files; f; f = f->next)
		++nf;

	progress_printed = 0;
	for (i = 0, f = files; f; f = f->next, ++i) {
		if (f->binary)
			continue; /* TODO... */

		rcs_file_read_all_revisions(f, blob_data_handler);

		/*
		 * This is one of the slowest parts of the conversion, so
		 * reassure the user by printing our progress.
		 */
		progress = i * 100 / nf;
		if (progress > progress_printed) {
			export_progress("exported file revision blobs for "
				"%lu%% of all files", progress);
			progress_printed = progress;
		}
	}
}

static bool
looks_like_script(const char *path)
{
	const char *shext[] = {".sh", ".bash", ".pl", ".py"};
	const char *s;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(shext); ++i) {
		s = path;
		while ((s = strstr(s, shext[i]))) {
			s += strlen(shext[i]);
			if (!*s)
				return true;
		}
	}
	return false;
}

static unsigned int
file_mode(const struct rcs_file *file)
{
	/*
	 * If the file name looks like a script which is executable in Unix
	 * environments where execute permissions actually matter, give the
	 * file execute bits.
	 *
	 * If this is too simplistic, we could actually examine the contents of
	 * the file for "#!/bin/sh" or the like.
	 */
	if (looks_like_script(file->name))
		return 0755;

	/*
	 * Everything else, including Windows executables and batch files, is
	 * a normal file.
	 */
	return 0644;
}

static void
export_filemodifies(struct file_change *mods)
{
	struct file_change *m;
	struct rcs_version *ver;

	for (m = mods; m; m = m->next) {
		ver = rcs_file_find_version(m->file, &m->newrev, true);
		printf("M %o :%lu %s\n", file_mode(m->file), ver->blob_mark,
			m->file->name);
	}
}

static void
export_deletes(struct file_change *deletes)
{
	struct file_change *d;

	for (d = deletes; d; d = d->next)
		printf("D %s\n", d->file->name);
}

static void
export_commit(const struct git_commit *commit)
{
	printf("commit refs/heads/%s\n", commit->branch);
	printf("committer %s <%s> %lu -0800\n", commit->committer->name,
		commit->committer->email, (unsigned long)commit->date);
	printf("data %zu\n", strlen(commit->commit_msg));
	printf("%s\n", commit->commit_msg);
	export_filemodifies(commit->changes.adds);
	export_filemodifies(commit->changes.updates);
	export_deletes(commit->changes.deletes);
}

static void
export_checkpoint_tag(const char *tag, const char *from_branch,
	const struct rcs_number *cprevnum)
{
	const struct rcs_version *ver;
	const struct rcs_patch *patch;
	const struct git_author *tagger;

	ver = rcs_file_find_version(project, cprevnum, true);
	patch = rcs_file_find_patch(project, cprevnum, true);
	tagger = author_map(ver->author);

	printf("tag %s\n", tag);
	printf("from refs/heads/%s\n", from_branch);
	printf("tagger %s <%s> %lu -800\n", tagger->name, tagger->email,
		(unsigned long)ver->date);
	printf("data %zu\n", strlen(patch->log));
	printf("%s\n", patch->log);
}

static void
export_branch_create(const char *from_branch, const char *new_branch)
{
	printf("reset refs/heads/%s\n", new_branch);
	printf("from refs/heads/%s\n\n", from_branch);
}

static struct git_commit *
get_commit_list(const char *branch, const struct rcs_number *pjrev_old,
	const struct rcs_number *pjrev_new)
{
	const struct rcs_file_revision *frevs_old, *frevs_new;
	const struct rcs_version *pjver_old, *pjver_new;
	struct file_change_lists changes;

	if (pjrev_old) {
		frevs_old = find_checkpoint_file_revisions(pjrev_old);
		pjver_old = rcs_file_find_version(project, pjrev_old, true);
	} else {
		frevs_old = NULL;
		pjver_old = NULL;
	}

	frevs_new = find_checkpoint_file_revisions(pjrev_new);
	pjver_new = rcs_file_find_version(project, pjrev_new, true);

	changeset_build(frevs_old, pjver_old ? pjver_old->date : 0, frevs_new,
		pjver_new->date, &changes);
	return merge_changeset_into_commits(branch, &changes, pjver_new->date);
}

static void
export_project_revision_changes(const struct rcs_number *pjrev_old,
	const struct rcs_number *pjrev_new)
{
	const char *cpname, *branch;
	struct git_commit *commits, *c;
	struct rcs_symbol *b;

	cpname = pjrev_find_checkpoint(pjrev_new);
	branch = pjrev_find_branch(pjrev_new);
	if (!branch) {
		fprintf(stderr, "warning: project rev. %s does not have a "
			"branch\n", rcs_number_string_sb(pjrev_new));
		return;
	}

	export_progress("exporting project rev. %s "
		"(branch=%s checkpoint=%s)\n",
		rcs_number_string_sb(pjrev_new), branch,
		cpname ? cpname : "<none>");

	commits = get_commit_list(branch, pjrev_old, pjrev_new);
	for (c = commits; c; c = c->next)
		export_commit(c);
	free_commits(commits);

	for (b = project_branches; b; b = b->next)
		if (rcs_number_equal(&b->number, pjrev_new)
		 && strcmp(b->symbol_name, "master"))
			export_branch_create(branch, b->symbol_name);

	if (cpname)
		export_checkpoint_tag(cpname, branch, pjrev_new);
}

static void
export_project_branch_changes(const struct rcs_number *pjrev_start,
	struct rcs_branch *branches)
{
	struct rcs_branch *b;
	struct rcs_version *bver;
	struct rcs_number pjrev_branch_new, pjrev_branch_old;

	pjrev_branch_old = *pjrev_start;
	for (b = branches; b; b = b->next) {
		pjrev_branch_new = b->number;
		do {
			export_project_revision_changes(
				&pjrev_branch_old,
				&pjrev_branch_new);

			bver = rcs_file_find_version(project,
				&pjrev_branch_new, true);
			export_project_branch_changes(&pjrev_branch_new,
				bver->branches);

			pjrev_branch_old = pjrev_branch_new;
			pjrev_branch_new = bver->parent;
		} while (pjrev_branch_new.c);
	}
}

static void
export_project_changes(void)
{
	struct rcs_number pjrev_old, pjrev_new;
	struct rcs_version *ver;
	bool first;

	pjrev_new.n[0] = 1;
	pjrev_new.n[1] = 0;
	pjrev_new.c = 2;
	first = true;
	for (;;) {
		pjrev_old = pjrev_new;
		rcs_number_increment(&pjrev_new);

		ver = rcs_file_find_version(project, &pjrev_new, false);
		if (!ver)
			break;

		/* Export changes from these trunk revisions */
		export_project_revision_changes(first ? NULL : &pjrev_old,
			&pjrev_new);

		/* Export changes for any branch that starts here */
		export_project_branch_changes(&pjrev_new, ver->branches);

		first = false;
	}
}

/* export a stream of git fast-import commands */
void
export(void)
{
	project_read_all_revisions();
	export_blobs();
	export_project_changes();
}
