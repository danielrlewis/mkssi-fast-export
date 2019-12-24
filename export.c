/*
 * Export a stream of commands for git fast-import.
 *
 * Recommended reading: the git-fast-import(1) man page.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interfaces.h"

/*
 * If this program was to be more general-purpose, this should be a parameter,
 * which could be overridden on an individual basis by author timezones in the
 * author map.
 */
#define TIMEZONE "-0800"

static unsigned long blob_mark_counter;

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

/* find a branch (optionally after another branch) by project revision number */
static const struct mkssi_branch *
pjrev_find_branch_after(const struct rcs_number *pjrev,
	const struct mkssi_branch *prev_branch)
{
	const struct mkssi_branch *b;

	/* Trunk project revisions go on the trunk, unless... */
	if (rcs_number_is_trunk(pjrev)) {
		/*
		 * ...unless we are dealing with one of those weird projects
		 * where the trunk history somehow got put onto an nameless
		 * branch revision (the trunk_branch), in which case any
		 * revisions >trunk_branch are ignored.
		 */
		if (trunk_branch.c
		 && rcs_number_compare(pjrev, &trunk_branch) > 0)
			return NULL;

		/*
		 * It's assumed that there aren't multiple branches sharing the
		 * trunk revision.
		 */
		if (prev_branch)
			return NULL;

		/* Find and return the trunk branch. */
		for (b = project_branches; b; b = b->next)
			if (!strcmp(b->branch_name, "master"))
				return b;

		fatal_error("internal error: trunk branch is missing");
		return NULL; /* unreachable */
	}

	/*
	 * A project branch at 1.4 would match 1.4.1.x, but not 1.4 (which is
	 * trunk) or 1.4.1.x.1.y (which is a branch of a branch).
	 */
	for (b = project_branches; b; b = b->next) {
		/*
		 * MKSSI allows multiple branches to have the same revision
		 * number.  To enumerate all the branches for a revision number,
		 * specify the previous branch with that revision number as
		 * prev_branch.
		 */
		if (b == prev_branch) {
			prev_branch = NULL;
			continue;
		}
		if (prev_branch)
			continue;

		if (pjrev->c - 2 == b->number.c
		 && rcs_number_partial_match(pjrev, &b->number))
			return b;
	}

	/* No matching branch for this project revision */
	return NULL;
}

/* find a branch by project revision number */
static const struct mkssi_branch *
pjrev_find_branch(const struct rcs_number *pjrev)
{
	return pjrev_find_branch_after(pjrev, NULL);
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

/* does a file name have a Linux/Unix script file name extension? */
static bool
has_script_extension(const char *path)
{
	/*
	 * This list of extensions is incomplete, but it covers a lot of ground,
	 * especially when paired with a check for shebang.
	 *
	 * Do *not* add ".bat", ".ps1", or other Windows scripting extensions to
	 * this list.  Such files are not executable in any environment which
	 * actually cares about execute permissions.
	 */
	const char *shext[] = {".sh", ".bash", ".csh", ".pl", ".py", ".rb"};
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

/* does a file revision look like a Linux/Unix executable? */
static bool
looks_like_executable(const struct rcs_file *file, const char *data)
{
	/* anything starting with a shebang is assumed to be a script */
	if (data[0] == '#' && data[1] == '!')
		return true;

	/*
	 * Assume anything with certain file extensions is an executable, even
	 * if the shebang is absent.
	 */
	if (has_script_extension(file->name))
		return true;

	/* look for the magic number of an ELF executable */
	if (data[0] == 0x7f && !strncmp(&data[1], "ELF", 3))
		return true;

	/*
	 * Other files are not executable.
	 *
	 * Windows/DOS executables (.exe, .bat, .ps1, .com, etc.) are NOT
	 * executable in any operating system where execute permissions actually
	 * matter, and so they are treated as normal files.
	 */
	return false;
}

/* export a blob for the given file revision data */
static void
export_revision_blob(struct rcs_file *file, const struct rcs_number *revnum,
	const char *data, bool member_type_other)
{
	struct rcs_version *ver;

	/*
	 * Put a comment in the stream which identifies the blob.  This is only
	 * for debugging.
	 */
	printf("# %s rev. %s%s\n", file->name, rcs_number_string_sb(revnum),
		member_type_other ? " (no keyword expansion)" : "");

	/*
	 * Each blob is given a unique mark number.  Later when committing file
	 * modifications, we refer back to the data blob we want by its mark.
	 */
	printf("blob\n");
	printf("mark :%lu\n", ++blob_mark_counter);
	printf("data %zu\n", strlen(data));
	printf("%s\n", data);

	ver = rcs_file_find_version(file, revnum, true);
	ver->executable = looks_like_executable(file, data);

	/* Save the mark */
	if (member_type_other)
		file->other_blob_mark = blob_mark_counter;
	else
		ver->blob_mark = blob_mark_counter;
}

/* export a blob for the given binary file revision data */
static void
export_binary_revision_blob(struct rcs_file *file,
	const struct rcs_number *revnum, const unsigned char *data,
	size_t datalen)
{
	struct rcs_version *ver;

	/*
	 * Put a comment in the stream which identifies the blob.  This is only
	 * for debugging.
	 */
	printf("# %s rev. %s\n", file->name, rcs_number_string_sb(revnum));

	/*
	 * Each blob is given a unique mark number.  Later when committing file
	 * modifications, we refer back to the data blob we want by its mark.
	 */
	printf("blob\n");
	printf("mark :%lu\n", ++blob_mark_counter);
	printf("data %zu\n", datalen);
	fwrite(data, 1, datalen, stdout);
	putchar('\n');

	ver = rcs_file_find_version(file, revnum, true);
	ver->blob_mark = blob_mark_counter; /* Save the mark */
	ver->executable = looks_like_executable(file, (const char *)data);
}

/* export blobs for every revision of every file */
static void
export_blobs(void)
{
	struct rcs_file *f;
	unsigned long nf, i, progress, progress_printed;

	export_progress("exporting file revision blobs");
	export_progress("(may _appear_ to hang -- be patient...)");

	/* count files to allow progress to be shown */
	nf = 0;
	for (f = files; f; f = f->next)
		++nf;

	progress_printed = 0;
	for (i = 0, f = files; f; f = f->next, ++i) {
		if (f->binary)
			rcs_binary_file_read_all_revisions(f,
				export_binary_revision_blob);
		else
			rcs_file_read_all_revisions(f, export_revision_blob);

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

/* export file renames */
static void
export_filerenames(const struct file_change *renames)
{
	const struct file_change *r;

	for (r = renames; r; r = r->next)
		printf("R \"%s\" \"%s\"\n", r->old_canonical_name,
			r->canonical_name);
}

/* export file modifications (added or updated files) */
static void
export_filemodifies(const struct file_change *mods)
{
	const struct file_change *m;
	const struct rcs_version *ver;

	for (m = mods; m; m = m->next) {
		ver = rcs_file_find_version(m->file, &m->newrev, true);
		printf("M %o :%lu %s\n", ver->executable ? 0755 : 0644,
			m->member_type_other ? m->file->other_blob_mark :
			ver->blob_mark, m->canonical_name);
	}
}

/* export file deletions */
static void
export_deletes(const struct file_change *deletes)
{
	const struct file_change *d;

	for (d = deletes; d; d = d->next)
		printf("D %s\n", d->canonical_name);
}

/* export a commit */
static void
export_commit(const struct git_commit *commit)
{
	printf("commit refs/heads/%s\n", commit->branch);
	printf("committer %s <%s> %lu %s\n", commit->committer->name,
		commit->committer->email, (unsigned long)commit->date,
		TIMEZONE);
	printf("data %zu\n", strlen(commit->commit_msg));
	printf("%s\n", commit->commit_msg);
	export_filerenames(commit->changes.renames);
	export_filemodifies(commit->changes.adds);
	export_filemodifies(commit->changes.updates);
	export_deletes(commit->changes.deletes);
}

/* export a tag to represent an MKSSI checkpoint */
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
	printf("tagger %s <%s> %lu %s\n", tagger->name, tagger->email,
		(unsigned long)ver->date.value, TIMEZONE);
	printf("data %zu\n", strlen(patch->log));
	printf("%s\n", patch->log);
}

/* export tag to demarcate MKSSI history from subsequent Git history */
static void
export_demarcating_tag(const char *branch)
{
	char *tag, *msg;
	static const char msg_template[] =
"Final commit exported from MKSSI for branch %s\n\
\n\
This tag marks the final commit on this branch that was exported from MKS\n\
Source Integrity (MKSSI).  The tagged commit and all antecedents were exported\n\
by mkssi-fast-export into Git via git-fast-import(1).\n\
\n\
";

	export_progress("exporting demarcating tag for branch %s", branch);

	tag = sprintf_alloc("%s_mkssi", branch);
	msg = sprintf_alloc(msg_template, branch);

	printf("tag %s\n", tag);
	printf("from refs/heads/%s\n", branch);
	printf("tagger %s <%s> %lu %s\n", "mkssi-fast-export", "none",
		(unsigned long)time(NULL), TIMEZONE);
	printf("data %zu\n", strlen(msg));
	printf("%s\n", msg);

	free(msg);
	free(tag);
}

/* export a branchpoint */
static void
export_branchpoint(const char *from_branch, const char *new_branch)
{
	export_progress("exporting branchpoint for branch %s (from %s)",
		new_branch, from_branch);
	printf("reset refs/heads/%s\n", new_branch);
	printf("from refs/heads/%s\n\n", from_branch);
}

/* generate commits to move from one project revision to the next */
static struct git_commit *
get_commit_list(const struct mkssi_branch *branch,
	const struct rcs_number *pjrev_old, const struct rcs_number *pjrev_new)
{
	const struct rcs_file_revision *frevs_old, *frevs_new;
	const struct rcs_version *pjver_old, *pjver_new;
	time_t new_date;
	struct file_change_lists changes;

	if (pjrev_old) {
		frevs_old = find_checkpoint_file_revisions(pjrev_old);
		pjver_old = rcs_file_find_version(project, pjrev_old, true);
	} else {
		frevs_old = NULL;
		pjver_old = NULL;
	}

	if (pjrev_new == TIP_REVNUM) {
		/* file revisions from the branch's tip */
		frevs_new = branch->tip_frevs;
		new_date = branch->mtime;
	} else {
		/* file revisions from a branch's checkpoint */
		frevs_new = find_checkpoint_file_revisions(pjrev_new);
		pjver_new = rcs_file_find_version(project, pjrev_new, true);
		new_date = pjver_new->date.value;
	}

	/*
	 * Build a list of changes between the old and new lists of file
	 * revisions.
	 */
	changeset_build(frevs_old, pjver_old ? pjver_old->date.value : 0,
		frevs_new, new_date, &changes);

	/* Merge these changes into a list of commits. */
	return merge_changeset_into_commits(branch->branch_name, &changes,
		new_date);
}

/* export all changes from a given project revision onto branch */
static void
export_project_revision_changes_onto_branch(const struct rcs_number *pjrev_old,
	const struct rcs_number *pjrev_new, const struct mkssi_branch *branch,
	const char *tagname)
{
	struct git_commit *commits;
	const struct git_commit *c;
	const struct mkssi_branch *b;

	export_progress("exporting project rev. %s "
		"(branch=%s tag=%s)\n",
		rcs_number_string_sb(pjrev_new), branch->branch_name,
		tagname ? tagname : "<none>");

	/* Build a list of commits and export them. */
	commits = get_commit_list(branch, pjrev_old, pjrev_new);
	for (c = commits; c; c = c->next)
		export_commit(c);
	free_commits(commits);

	/* The tip has no derived branches or checkpoint. */
	if (pjrev_new == TIP_REVNUM)
		return;

	/*
	 * If this project revision is the starting point for any branch(es),
	 * create those branches now.
	 */
	for (b = project_branches; b; b = b->next)
		if (rcs_number_equal(&b->number, pjrev_new)
		 && strcmp(b->branch_name, "master"))
			export_branchpoint(branch->branch_name, b->branch_name);

	/* Create a tag to represent a named checkpoint */
	if (tagname)
		export_checkpoint_tag(tagname, branch->branch_name, pjrev_new);
}

/* export all changes from a given project revision */
static void
export_project_revision_changes(const struct rcs_number *pjrev_old,
	const struct rcs_number *pjrev_new)
{
	const struct rcs_number *pjrev_branch;
	const struct mkssi_branch *branch;
	const char *cpname;
	char *tagname;
	unsigned bcount;

	/*
	 * Find the branch associated with this project revision.  Not all
	 * project revisions have a branch; it is unknown how that happens.
	 */
	pjrev_branch = pjrev_new == TIP_REVNUM ? pjrev_old : pjrev_new;
	branch = pjrev_find_branch(pjrev_branch);

	/*
	 * Find the checkpoint name associated with this project revision.  Not
	 * all project revisions have a named checkpoint; it is unknown how such
	 * project revisions come into existence.
	 *
	 * Note that the tip of the branch is inherently not checkpointed.
	 */
	if (pjrev_new == TIP_REVNUM)
		cpname = NULL;
	else

		cpname = pjrev_find_checkpoint(pjrev_new);

	/*
	 * If there is no branch, stop because we don't have anywhere to commit
	 * the changes.
	 */
	if (!branch) {
		if (pjrev_new == TIP_REVNUM)
			fprintf(stderr, "warning: tip revision (after rev. %s) "
				"does not have a branch\n",
				rcs_number_string_sb(pjrev_branch));
		else {
			cpname = pjrev_find_checkpoint(pjrev_branch);
			fprintf(stderr, "warning: project rev. %s "
				"(checkpoint=%s) does not have a branch\n",
				rcs_number_string_sb(pjrev_branch),
				cpname ? cpname : "<none>");
		}
		return;
	}

	tagname = cpname ? xstrdup(cpname, __func__) : NULL;

	for (bcount = 1; branch; bcount++) {
		if (tagname && bcount > 1) {
			/*
			 * If there is a branch with the same revision number,
			 * it will have the same MKSSI checkpoints.  Git won't
			 * let us use the same tag name on multiple branches,
			 * so in this (rare) case, munge the tag name.
			 */
			free(tagname);
			tagname = sprintf_alloc("%s-%u", cpname, bcount + 1);
		}

		export_project_revision_changes_onto_branch(
			pjrev_old, pjrev_new, branch, tagname);

		/*
		 * MKSSI allows multiple branches to use the same revision
		 * number.  In such cases, export the changes onto each of those
		 * branches.
		 */
		branch = pjrev_find_branch_after(pjrev_branch, branch);
	}

	if (tagname)
		free(tagname);
}

/* export project changes occurring on a given branch */
static void
export_project_branch_changes(const struct rcs_number *pjrev_start,
	struct rcs_branch *branches)
{
	const struct rcs_branch *b;
	const struct rcs_version *bver;
	struct rcs_number pjrev_branch_new, pjrev_branch_old;
	const struct mkssi_branch *mb;

	pjrev_branch_old = *pjrev_start;
	for (b = branches; b; b = b->next) {
		pjrev_branch_new = b->number;
		do {
			/* Export changes */
			export_project_revision_changes(
				&pjrev_branch_old,
				&pjrev_branch_new);

			/*
			 * If a branch has been created from this branch,
			 * export its changes.
			 */
			bver = rcs_file_find_version(project,
				&pjrev_branch_new, true);
			export_project_branch_changes(&pjrev_branch_new,
				bver->branches);

			pjrev_branch_old = pjrev_branch_new;
			pjrev_branch_new = bver->parent;
		} while (pjrev_branch_new.c);

		/* Export uncheckpointed changes from the tip of the branch */
		if (mkssi_proj_dir_path)
			export_project_revision_changes(
				&pjrev_branch_old, TIP_REVNUM);
	}

	/*
	 * Handle another weird case: branches which are listed in the
	 * "_mks_variant_projects" block in project.pj, but which don't have
	 * branch revisions in the RCS metadata.  MKSSI allows such branches to
	 * be checked-out, so they should be exported.
	 */
	for (mb = project_branches; mb; mb = mb->next) {
		/* Does this MKSSI branch start from this project revision? */
		if (!rcs_number_equal(&mb->number, pjrev_start))
			continue;

		/*
		 * Check the RCS branch revisions.  If we find a branch which
		 * corresponds to the MKSSI branch, then the MKSSI branch was
		 * already exported above.
		 */
		for (b = branches; b; b = b->next)
			if (rcs_number_partial_match(&b->number, &mb->number))
				break;
		if (b)
			continue; /* Already exported this branch. */

		/*
		 * Export the tip revisions for this branch.  Since the branch
		 * has no RCS branch, presumably there aren't any checkpoints to
		 * be exported.
		 */
		if (mkssi_proj_dir_path)
			export_project_revision_changes_onto_branch(
				pjrev_start, TIP_REVNUM, mb, NULL);
	}
}

/* export git fast-import commands for all project changes */
static void
export_project_changes(void)
{
	struct rcs_number pjrev_old, pjrev_new;
	const struct rcs_version *ver;
	bool first;

	/*
	 * Initialize to 1.0.  This is incremented to 1.1 at the start of the
	 * loop, which is the first valid project revision.
	 */
	pjrev_new.n[0] = 1;
	pjrev_new.n[1] = 0;
	pjrev_new.c = 2;

	for (first = true;; first = false) {
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
	}

	/* Export uncheckpointed changes from the tip of the trunk */
	if (!first && mkssi_proj_dir_path)
		export_project_revision_changes(&pjrev_old, TIP_REVNUM);
}

/* tag each branch to demarcate MKSSI history from subsequent Git history */
static void
export_demarcating_tags(void)
{
	const struct mkssi_branch *b;

	export_progress("exporting demarcating tags");

	for (b = project_branches; b; b = b->next)
		/* Skip MKSSI branches which don't have Git branches. */
		if (pjrev_find_branch(&b->number)
		 || !strcmp(b->branch_name, "master"))
			export_demarcating_tag(b->branch_name);
}

/* export a stream of git fast-import commands */
void
export(void)
{
	/*
	 * Read all the revisions of project.pj, extracting and saving from each
	 * a list of files and their current revision numbers.
	 *
	 * This also builds a list of project branches.
	 */
	project_read_checkpointed_revisions();
	project_read_tip_revisions();

	/*
	 * Export blobs for every revision of every project file.  Doing this
	 * up-front is an optimization (very worthwhile), since it allows the
	 * RCS revisioning for each file to be parsed once and only once.
	 */
	export_blobs();

	/*
	 * Export a stream of git fast-import commands which represent the
	 * history of the MKSSI project.
	 */
	export_project_changes();

	/*
	 * Export tags that will demarcate the MKSSI history from future commits
	 * made via Git.
	 */
	export_demarcating_tags();
}
