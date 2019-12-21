/* Parsing project.pj */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interfaces.h"

/* list of file revisions for each project.pj revision */
struct pjrev_files {
	struct pjrev_files *next;
	const struct rcs_version *pjver;
	const struct rcs_file_revision *frevs;
};

/* Linked list of file revisions included in each project revision */
static struct pjrev_files *pjrev_files;

/* validate that a string looks like a given revision of project.pj */
static void
validate_project_data(const char *pjdata, const struct rcs_number *revnum)
{
	char rev_str[11 + RCS_MAX_REV_LEN + 1];

	/*
	 * Sanity check: each revision of project.pj should start with "--MKS
	 * Project--" or "--MKS Variant Project--".
	 */
	if (strncmp(pjdata, "--MKS Project--\n", 16)
	 && strncmp(pjdata, "--MKS Variant Project--\n", 24))
		fatal_error("%s rev. %s is corrupt", project->master_name,
			rcs_number_string_sb(revnum));

	/*
	 * Sanity check: each revision of project.pj should include a string
	 * with that revision number.
	 */
	sprintf(rev_str, "$Revision: %s", rcs_number_string_sb(revnum));
	if (!strstr(pjdata, rev_str))
		fatal_error("%s rev. %s is missing its revision marker",
			project->master_name, rcs_number_string_sb(revnum));
}

/* find an RCS file in the hash table by name */
static struct rcs_file *
rcs_file_find(const char *name)
{
	uint32_t bucket;
	struct rcs_file *f;

	bucket = hash_string(name) % ARRAY_SIZE(file_hash_table);
	for (f = file_hash_table[bucket]; f; f = f->hash_next)
		if (!strcasecmp(f->name, name)) {
			/*
			 * Kluge to correct capitalization for keyword
			 * expansion, which uses this file name rather than the
			 * canonical file names generated later.  This breaks
			 * down if the file name capitalization changes over
			 * time.
			 */
			if (strcmp(f->name, name))
				strcpy(f->name, name);

			return f;
		}

	/* Try the corrupt files (hopefully a short list!) */
	for (f = corrupt_files; f; f = f->next)
		if (!strcasecmp(f->name, name))
			return f;

	return NULL;
}

/* fix inconsistent directory name capitalization */
static void
fix_directory_capitalization(const struct rcs_file_revision *frevs)
{
	const struct rcs_file_revision *f, *ff;
	struct dir_path *adjusted_dirs, *dirs, *d;

	adjusted_dirs = NULL;

	/*
	 * Current assumption is that the canonical capitalization of a
	 * directory is the capitalization used in its first appearance in the
	 * list.  Most likely, MKSSI creates the files and directories in-order,
	 * using the capitalization that it encounters first, and if subsequent
	 * entries in that directory are listed with a different directory name
	 * capitalization, it has no effect since Windows file systems are case
	 * insensitive.
	 */
	for (f = frevs; f; f = f->next) {
		dirs = dir_list_from_path(f->canonical_name);
		dirs = dir_list_remove_duplicates(dirs, adjusted_dirs);

		for (ff = f->next; ff; ff = ff->next)
			for (d = dirs; d; d = d->next)
				if (!strncasecmp(ff->canonical_name, d->path,
				 d->len))
					memcpy(ff->canonical_name, d->path,
						d->len);

		adjusted_dirs = dir_list_append(adjusted_dirs, dirs);
	}

	dir_list_free(adjusted_dirs);
}

/* load file revision list from a revision of project.pj */
static struct rcs_file_revision *
project_revision_read_files(const char *pjdata)
{
	const char flist_start_marker[] = "\nEndOptions\n";
	const char file_prefix[] = "$(projectdir)/";
	struct rcs_file_revision *head, **prev, *frev;
	struct rcs_file *file;
	const char *flist, *line, *lp, *endline;
	char file_path[1024], errline[1024], rcsnumstr[RCS_MAX_REV_LEN];
	struct rcs_number revnum;
	char *fp, *rp;
	bool in_quote;

	prev = &head;

	flist = strstr(pjdata, flist_start_marker);
	if (!flist)
		fatal_error("missing \"EndOptions\" in %s",
			project->master_name);
	flist += strlen(flist_start_marker);

	/*
	 * Each line in the file list looks something like this:
	 *
	 * 	$(projectdir)/path/to/file.txt a 1.13
	 *
	 * Or on a branch revision:
	 *
	 * 	$(projectdir)/path/to/file.txt a 1.13.1.7 _mks_variant=1.13.1
	 *
	 * If the file name has spaces, it looks like this:
	 *
	 * 	$(projectdir)/path/to/"file with spaces.txt" a 1.42
	 *
	 * If a directory name has spaces, it can look like this:
	 *
	 * 	"$(projectdir)/dir with spaces"/file.txt a 1.42
	 */
	for (line = flist; *line; line = *endline ? endline + 1 : endline) {
		endline = strchr(line, '\n');
		if (!endline)
			endline = line + strlen(line);

		/*
		 * In case of error, we want to be able to print the erroneous
		 * line, which requires we have a copy of it which is terminated
		 * by a NUL character.
		 */
		memcpy(errline, line, min(sizeof errline, endline - line));
		errline[min(sizeof errline - 1, endline - line)] = '\0';

		in_quote = false;
		lp = line;

		if (*lp == '\n')
			continue; /* ignore blank lines */

		if (*lp == '"') {
			in_quote = true;
			++lp;
		}

		if (strncmp(lp, file_prefix, strlen(file_prefix))) {
			fprintf(stderr, "warning: ignoring file with unexpected"
				" project directory prefix:\n");
			fprintf(stderr, "\t%s\n", errline);
			continue;
		}

		lp += strlen(file_prefix);

		/*
		 * Copy file name into NUL-terminated buffer.  MKSSI adds quotes
		 * to file names with spaces; parse these but do not copy them
		 * into the file name buffer.
		 */
		for (fp = file_path;; ++lp) {
			if (!*lp || *lp == '\n') {
				fprintf(stderr, "error on line:\n\t%s\n",
					errline);
				fatal_error("unexpected end-of-line");
			}
			if (!in_quote && *lp == ' ')
				break;
			if (*lp == '"') {
				in_quote = !in_quote;
				continue;
			}
			if (fp - file_path >= sizeof file_path - 1) {
				fprintf(stderr, "error on line:\n\t%s\n",
					errline);
				fatal_error("file name too long");
			}
			*fp++ = *lp;
		}
		*fp++ = '\0';

		if (!strncmp(lp, " a ", 3)) {
			lp += 3;

			/*
			 * Copy the revision number into a NUL-terminated buffer
			 */
			rp = rcsnumstr;
			while (*lp == '.' || (*lp >= '0' && *lp <= '9')) {
				if (rp - rcsnumstr >= sizeof rcsnumstr - 1) {
					fprintf(stderr,
						"error on line:\n\t%s\n",
						errline);
					fatal_error("revision number too long");
				}
				*rp++ = *lp++;
			}
			*rp++ = '\0';

			revnum = lex_number(rcsnumstr);
		} else if (!strncmp(lp, " f", 2))
			/*
			 * According the manual, "f" means other, but there is
			 * no explanation of what that means.  It seems to be
			 * rare, and related to deleting and re-adding files.
			 *
			 * In the observed cases (small sample size), if the
			 * file is a binary file, MKSSI grabs the head revision;
			 * if it is a text file, it grabs rev. 1.1 without doing
			 * RCS keyword expansion.  This seems strange so perhaps
			 * there is actually another rule at play.
			 */
			 revnum.c = 0; /* Will grab correct revision below */
		else if (!strncmp(lp, " i", 2) || !strncmp(lp, " s", 2)) {
			/*
			 * According to the manual, "i" means included sub-
			 * project and "s" means subscribed sub-project.
			 * Neither are supported by this tool.
			 */
			fprintf(stderr, "error on line:\n\t%s\n", errline);
			fatal_error("unsupported member type\n");
		} else {
			fprintf(stderr, "error on line:\n\t%s\n", errline);
			fatal_error("unrecognized member type\n");
		}

		frev = xcalloc(1, sizeof *frev, __func__);
		file = rcs_file_find(file_path);
		if (!file) {
			fprintf(stderr, "warning: ignoring file without RCS "
				" master file:\n");
			fprintf(stderr, "\t%s\n", errline);
			free(frev);
			continue;
		}
		if (file->corrupt) {
			free(frev);
			continue;
		}
		frev->canonical_name = xstrdup(file_path, __func__);
		if (revnum.c)
			frev->rev = revnum;
		else if (file->binary)
			frev->rev = file->head;
		else {
			/* rev. 1.1 */
			frev->rev.n[0] = 1;
			frev->rev.n[1] = 1;
			frev->rev.c = 2;

			/*
			 * MKSSI does not seem to expand RCS keywords when it
			 * gets rev. 1.1 for an "other" member type.  That means
			 * we need to export a special edition of rev. 1.1 w/out
			 * keyword expansion.
			 */
			frev->member_type_other = true;
			file->has_member_type_other = true;
		}
		frev->file = file;
		*prev = frev;
		prev = &frev->next;
	}
	*prev = NULL;

	fix_directory_capitalization(head);

	return head;
}

/* remove illegal characters and MKSSI encodings from branch names */
static void
sanitize_branch_name(char *branch_name)
{
	char *sp, *tp;
	size_t len;
	int ch;

	/*
	 * TODO: Fix to enforce all the rules:
	 * https://git-scm.com/docs/git-check-ref-format
	 */
	for (sp = tp = branch_name; *sp; sp += len) {
		len = parse_mkssi_branch_char(sp, &ch);
		if (ch != -1)
			*tp++ = (char)ch;
	}
	*tp = '\0';
	if (!*branch_name)
		fatal_error("branch name was empty after sanitization");
}

/* parse a project.pj variant project line */
static struct mkssi_branch *
parse_project_branch_line(const char *line, const char *endline)
{
	char rcs_num_str[RCS_MAX_REV_LEN];
	const char *pos, *name_start;
	struct mkssi_branch *branch;
	unsigned int i;

	branch = xcalloc(1, sizeof *branch, __func__);

	/*
	 * These lines are formatted as such:
	 * 	revnum=vpNNNN.pj, "BranchName"
	 * For example:
	 * 	1.2=vp0000.pj, "v1_0_Release"
	 */

	/* parse the branch revision number */
	for (i = 0, pos = line; *pos != '='; ++pos, ++i) {
		if (i == sizeof rcs_num_str - 1)
			fatal_error("revision number too long: %s", line);
		if (*pos != '.' && !(*pos >= '0' && *pos <= '9'))
			fatal_error("invalid revision number: %s", line);
		rcs_num_str[i] = *pos;
	}
	rcs_num_str[i] = '\0';
	branch->number = lex_number(rcs_num_str);

	/* parse the branch project file name */
	++pos; /* Move past the '=' */
	name_start = pos;
	pos = strchr(pos, ',');
	if (!pos || pos > endline)
		fatal_error("missing project file: %s", line);
	branch->pj_name = xcalloc(1, pos - name_start + 1, __func__);
	memcpy(branch->pj_name, name_start, pos - name_start);

	/* parse the branch name */
	pos = strchr(pos, '"');
	if (!pos || pos > endline)
		fatal_error("missing branch name: %s", line);
	++pos; /* Move past the '"' */
	name_start = pos;
	pos = strchr(pos, '"');
	if (!pos || pos > endline)
		fatal_error("unterminated branch name: %s", line);
	branch->branch_name = xcalloc(1, pos - name_start + 1, __func__);
	memcpy(branch->branch_name, name_start, pos - name_start);
	sanitize_branch_name(branch->branch_name);

	return branch;
}

/* add a project branch to the list, if it is not there already */
static void
project_branch_add(struct mkssi_branch **branches, struct mkssi_branch *branch)
{
	struct mkssi_branch *b, **bptr;
	int cmp;

	/*
	 * The special "trunk branch" is used for weird projects where the trunk
	 * somehow becomes an nameless branch revision.  The trunk branch should
	 * never have the same branch number as an actual legit branch.
	 */
	if (rcs_number_equal(&branch->number, &trunk_branch))
		fatal_error("specified trunk branch rev. %s is used by an "
			"actual branch named \"%s\"",
			rcs_number_string_sb(&trunk_branch),
			branch->branch_name);

	/*
	 * This branch might have already been recorded from another revision of
	 * project.pj.  If so, ignore it.
	 */
	bptr = branches;
	for (b = *bptr; b; b = *bptr) {
		if (!strcmp(b->branch_name, branch->branch_name)) {
			/*
			 * If the branch is found multiple times with different
			 * revision numbers, let the highest revision take
			 * precedence.  This is simplistic but it should work
			 * for normal use cases.
			 */
			cmp = rcs_number_compare(&b->number, &branch->number);
			if (cmp < 0) {
				/* Remove from list and reinsert */
				*bptr = b->next;
				free(b->branch_name);
				free(b->pj_name);
				free(b);
				break;
			}

			/* Branch already in list. */
			free(branch->branch_name);
			free(branch->pj_name);
			free(branch);
			return;
		}
		bptr = &b->next;
	}

	/* Insert into branch list in sorted order. */
	bptr = branches;
	for (b = *bptr; b; b = b->next) {
		cmp = rcs_number_compare(&b->number, &branch->number);
		if (cmp >= 0)
			break;
		bptr = &b->next;
	}
	branch->next = b;
	*bptr = branch;
}

/* extract all project branches from a revision of project.pj */
static void
project_revision_read_branches(struct mkssi_branch **branches,
	const char *pjdata)
{
	const char start_marker[] = "block _mks_variant_projects\n";
	const char *start, *end, *line, *endline;

	start = strstr(pjdata, start_marker);
	if (!start)
		return;

	start += strlen(start_marker);
	end = strstr(start, "\nend\n");
	if (!end)
		fatal_error("unterminated block of variant projects");

	for (line = start; line < end; line = endline + 1) {
		endline = strchr(line, '\n');
		project_branch_add(branches,
			parse_project_branch_line(line, endline));
	}
}

/* mark all file revisions recorded in this project.pj revision */
static void
mark_checkpointed_revisions(const struct rcs_file_revision *frevs)
{
	struct rcs_version *ver;
	const struct rcs_file_revision *frev;

	for (frev = frevs; frev; frev = frev->next) {
		ver = rcs_file_find_version(frev->file, &frev->rev, false);
		if (ver)
			ver->checkpointed = true;
	}
}

/* save a list of files and their revision numbers found in a project rev */
static void
save_checkpoint_file_revisions(const struct rcs_number *pjrev,
	const struct rcs_file_revision *frev_list)
{
	struct pjrev_files *f;

	f = xcalloc(1, sizeof *f, __func__);
	f->pjver = rcs_file_find_version(project, pjrev, true);
	f->frevs = frev_list;
	f->next = pjrev_files;
	pjrev_files = f;
}

/* find a list of files and their revision numbers for a project revision */
const struct rcs_file_revision *
find_checkpoint_file_revisions(const struct rcs_number *pjrev)
{
	const struct pjrev_files *f;

	for (f = pjrev_files; f; f = f->next)
		if (rcs_number_equal(&f->pjver->number, pjrev))
			return f->frevs;
	fatal_error("no saved file revision list for project rev. %s",
		rcs_number_string_sb(pjrev));
	return NULL; /* unreachable */
}

/* parse file list and optionally branches in a revision of project.pj */
static const struct rcs_file_revision *
project_parse_revision(const char *pjdata, const struct rcs_number *revnum,
	bool save_branches)
{
	/* Sanity check the revision text */
	validate_project_data(pjdata, revnum);

	if (save_branches)
		project_revision_read_branches(&project_branches, pjdata);

	/*
	 * Read the list of files and file revision numbers that were current
	 * at the time of this project revision.
	 */
	return project_revision_read_files(pjdata);
}

/* interpret a given revision of project.pj */
static void
project_data_handler(struct rcs_file *file, const struct rcs_number *revnum,
	const char *data, bool unused)
{
	const struct rcs_file_revision *frev_list;
	bool save_branches;

	export_progress("parsing project revision %s",
		rcs_number_string_sb(revnum));

	/*
	 * If we won't be reading the project.pj in the project directory and
	 * this is the head revision of project.pj in the RCS directory, then
	 * we are looking at the newest copy of the branch list that we'll ever
	 * see -- save it.
	 *
	 * We used to parse the branch list from every project.pj revision and
	 * merge the results, but this resulted in exporting branches that are
	 * no longer in MKSSI.  Sometimes these branches were corrupt, causing
	 * the export to fail.
	 */
	if (!mkssi_proj_dir_path && rcs_number_equal(revnum, &file->head))
		save_branches = true;
	else
		save_branches = false;

	frev_list = project_parse_revision(data, revnum, save_branches);

	/* Save said list for later. */
	save_checkpoint_file_revisions(revnum, frev_list);

	/*
	 * Mark all of these file revisions as checkpointed.  This information
	 * is useful later when building the changesets.
	 */
	mark_checkpointed_revisions(frev_list);
}

/* read and parse every checkpointed revision of project.pj */
void
project_read_checkpointed_revisions(void)
{
	export_progress("reading checkpointed project revisions");
	rcs_file_read_all_revisions(project, project_data_handler);
}

/* read and parse the tip revisions for a branch */
static void
project_branch_read_tip_revision(struct mkssi_branch *b)
{
	char *path, *pjdata;
	bool is_master;

	export_progress("reading tip revisions for branch %s", b->branch_name);

	is_master = !strcmp(b->branch_name, "master");

	/*
	 * The trunk (a.k.a. master) has its tip revisions in project.pj; the
	 * branches have their tip revisions in a project.vpj subdirectory,
	 * with a file name that is listed in project.pj (this file name was
	 * previously parsed and saved).
	 */
	if (is_master)
		path = sprintf_alloc("%s/project.pj", mkssi_proj_dir_path);
	else
		path = sprintf_alloc("%s/project.vpj/%s",
			mkssi_proj_dir_path, b->pj_name);

	/* Get the entire project file as a NUL-terminated string. */
	pjdata = file_as_string(path);

	/*
	 * Get the list of file revisions.  If this is the master, also save the
	 * branch list.
	 */
	b->tip_frevs = project_parse_revision(pjdata, &b->number, is_master);

	free(pjdata);
	free(path);
}

/* read and parse the tip revisions for the trunk and branches */
void
project_read_tip_revisions(void)
{
	struct mkssi_branch *b;

	/* This step is skipped if the project directory wasn't provided. */
	if (!mkssi_proj_dir_path)
		return;

	export_progress("reading tip project revisions");

	/*
	 * At this point, the master branch should be the first and only branch
	 * on the project_branches list.
	 */
	if (!project_branches || project_branches->next
	 || strcmp(project_branches->branch_name, "master"))
		fatal_error("internal error: unexpected branch list");

	/*
	 * Read the tip revision of the master branch first, so that the
	 * project_branches list will be fully populated before we try to loop
	 * through it.
	 */
	project_branch_read_tip_revision(project_branches);

	/*
	 * Read the tip revision of every branch.
	 *
	 * Note that project_branches might have been updated: it might no
	 * longer be the master branch.  Thus, loop from the beginning and skip
	 * the master branch, which we already read.
	 */
	for (b = project_branches; b; b = b->next)
		if (strcmp(b->branch_name, "master"))
			project_branch_read_tip_revision(b);
}
