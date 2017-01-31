#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interfaces.h"

const struct rcs_file_revision *
find_checkpoint_file_revisions(const struct rcs_number *pjrev)
{
	const struct cp_files *cpf;

	for (cpf = cp_files; cpf; cpf = cpf->next)
		if (rcs_number_equal(&cpf->pjver->number, pjrev))
			return cpf->frevs;
	fatal_error("no recorded file revision list for project revision %s",
		rcs_number_string_sb(pjrev));
	return NULL; /* unreachable */
}

static const char *
pjrev_find_checkpoint(const struct rcs_number *pjrev)
{
	struct rcs_symbol *cp;

	for (cp = project->symbols; cp; cp = cp->next)
		if (rcs_number_equal(&cp->number, pjrev))
			return cp->symbol_name;
	return NULL;
}

static const char *
pjrev_find_branch(const struct rcs_number *pjrev)
{
	struct rcs_symbol *b;

	if (rcs_number_is_trunk(pjrev))
		return "master";

	for (b = project_branches; b; b = b->next)
		if (rcs_number_partial_match(pjrev, &b->number)
		 && !rcs_number_equal(pjrev, &b->number))
			return b->symbol_name;

	return NULL;
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

static struct commit *
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
export_file_revision_data(struct rcs_file *file,
	const struct rcs_number *revnum)
{
	char *data;

	data = rcs_revision_read(file, revnum);
	printf("data %zu\n", strlen(data));
	printf("%s\n", data);
	free(data);
}

static void
export_commit(const struct commit *commit)
{
	struct file_change *c;

	printf("commit refs/heads/%s\n", commit->branch);
	printf("committer %s <%s> %lu -0800\n", commit->committer_name,
		commit->committer_email, (unsigned long)commit->date);
	printf("data %zu\n", strlen(commit->commit_msg));
	printf("%s\n", commit->commit_msg);
	for (c = commit->changes.adds; c; c = c->next) {
		printf("M %o inline %s\n", file_mode(c->file), c->file->name);
		export_file_revision_data(c->file, &c->newrev);
	}
	for (c = commit->changes.updates; c; c = c->next) {
		printf("M %o inline %s\n", file_mode(c->file), c->file->name);
		export_file_revision_data(c->file, &c->newrev);
	}
	for (c = commit->changes.deletes; c; c = c->next)
		printf("D %s\n", c->file->name);
}

static void
export_checkpoint_tag(const char *tag, const char *from_branch,
	const struct rcs_number *cprevnum)
{
	const struct rcs_version *ver;
	const struct rcs_patch *patch;

	ver = rcs_file_find_version(project, cprevnum, true);
	patch = rcs_file_find_patch(project, cprevnum, true);

	printf("tag %s\n", tag);
	printf("from refs/heads/%s\n", from_branch);
	printf("tagger %s <%s> %lu -800\n", ver->author, ver->author,
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

static void
export_project_revision_changes(const struct rcs_number *pjrev_old,
	const struct rcs_number *pjrev_new)
{
	const char *cpname, *branch;
	struct commit *commits, *c;
	struct rcs_symbol *b;

	cpname = pjrev_find_checkpoint(pjrev_new);
	branch = pjrev_find_branch(pjrev_new);
	if (!branch) {
		fprintf(stderr, "warning: project rev. %s does not have a "
			"branch\n", rcs_number_string_sb(pjrev_new));
		return;
	}

	progress_println("exporting project rev. %s "
		"(branch=%s checkpoint=%s)\n",
		rcs_number_string_sb(pjrev_new), branch,
		cpname ? cpname : "<none>");

	commits = get_commit_list(branch, pjrev_old, pjrev_new);
	for (c = commits; c; c = c->next)
		export_commit(c);
	free_commits(commits);

	for (b = project_branches; b; b = b->next)
		if (rcs_number_equal(&b->number, pjrev_new))
			export_branch_create(branch, b->symbol_name);

	if (cpname)
		export_checkpoint_tag(cpname, branch, pjrev_new);
}

static void
export_project(void)
{
	struct rcs_number pjrev_old, pjrev_new;
	struct rcs_number pjrev_branch_old, pjrev_branch_new;
	struct rcs_version *ver, *bver;
	struct rcs_branch *b;

	/* start at rev. 1.1 */
	pjrev_new.n[0] = 1;
	pjrev_new.n[1] = 1;
	pjrev_new.c = 2;
	export_project_revision_changes(NULL, &pjrev_new);

	for (;;) {
		pjrev_old = pjrev_new;
		rcs_number_increment(&pjrev_new);
		ver = rcs_file_find_version(project, &pjrev_new, false);
		if (!ver)
			break;

		/* Export changes from these trunk revisions */
		export_project_revision_changes(&pjrev_old, &pjrev_new);

		/* Export changes for any branch that starts here */
		pjrev_branch_old = pjrev_new;
		for (b = ver->branches; b; b = b->next) {
			pjrev_branch_new = b->number;
			do {
				export_project_revision_changes(
					&pjrev_branch_old,
					&pjrev_branch_new);

				bver = rcs_file_find_version(project,
					&pjrev_branch_new, true);
				pjrev_branch_old = pjrev_branch_new;
				pjrev_branch_new = bver->parent;
			} while (pjrev_branch_new.c);
		}
	}
}

/* export a stream of git fast-import commands */
void
export(void)
{
	project_read_checkpoints();
	export_project();
}
