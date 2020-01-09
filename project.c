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

/* return start of next line after str; or NULL if no more lines */
static const char *
next_line(const char *str)
{
	str = strchr(str, '\n');
	if (str) {
		++str;
		if (!*str)
			str = NULL;
	}

	return str;
}

/* find a line in str; returns NULL if not found */
static const char *
find_line(const char *str, const char *line)
{
	const char *p, *lnstart;
	size_t lnlen;

	lnlen = strlen(line);

	for (p = str; p; p = next_line(p)) {
		if (strncmp(p, line, lnlen))
			continue;

		lnstart = p;

		/* Make sure there is nothing else on the line. */
		p += lnlen;
		if (*p == '\r')
			++p;
		if (*p != '\n')
			continue; /* Line has other chars, not a match */

		p = lnstart;
		break;
	}

	return p;
}

/* validate that a string looks like a given revision of project.pj */
static void
validate_project_data(const char *pjdata, const struct rcs_number *revnum)
{
	static const char hdr_trunk[] = "--MKS Project--";
	static const char hdr_branch[] = "--MKS Variant Project--";
	const char *pos;
	int nl;
	char rev_str[11 + RCS_MAX_REV_LEN + 1];

	/*
	 * Sanity check: each revision of project.pj should start with "--MKS
	 * Project--" or "--MKS Variant Project--", followed by a newline.
	 */
	if (!strncmp(pjdata, hdr_trunk, sizeof hdr_trunk - 1))
		pos = pjdata + sizeof hdr_trunk - 1;
	else if (!strncmp(pjdata, hdr_branch, sizeof hdr_branch - 1))
		pos = pjdata + sizeof hdr_branch - 1;
	else {
		fatal_error("%s rev. %s is corrupt (no header)",
			project->master_name,
			rcs_number_string_sb(revnum));
		return; /* unreachable; but it quiets warnings. */
	}

	nl = *pos++;
	if (nl == '\r')
		nl = *pos++;
	if (nl != '\n')
		fatal_error("%s rev. %s is corrupt (no header newline)",
			project->master_name,
			rcs_number_string_sb(revnum));

	/*
	 * Sanity check: each revision of project.pj should include a string
	 * with that revision number.
	 */
	sprintf(rev_str, "$Revision: %s", rcs_number_string_sb(revnum));
	if (!strstr(pos, rev_str)) {
		/*
		 * project.pj rev. 1.1 might have an unexpanded $Revision$
		 * keyword.
		 */
		if (revnum->c == 2 && revnum->n[0] == 1 && revnum->n[1] == 1
		 && strstr(pos, "$Revision$"))
			return;

		fatal_error("%s rev. %s is missing its revision marker",
			project->master_name, rcs_number_string_sb(revnum));
	}
}

/* read revision number from project data */
static void
project_data_extract_revnum(const char *pjdata, struct rcs_number *revnum)
{
	const char *pos;

	/*
	 * This function is called after validate_project_data(), so the fatal
	 * error cases are unexpected (thus it's okay that the error messages
	 * aren't very detailed).
	 */

	pos = strstr(pjdata, "\n$Revision");
	if (!pos)
		fatal_error("missing revision number");

	pos += 10;
	if (*pos == '$') {
		/* Unexpanded $Revision$ means rev. 1.1 */
		revnum->n[0] = 1;
		revnum->n[1] = 1;
		revnum->c = 2;
		return;
	}

	if (*pos++ != ':' || *pos++ != ' ')
		fatal_error("incorrectly formatted revision number");

	*revnum = lex_number(pos);
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

/* find or add a file to the list of "dummy" files (no RCS masters) */
static struct rcs_file *
rcs_file_dummy_find_or_add(const char *name)
{
	struct rcs_file *f;

	/* Search for the dummy file in the list */
	for (f = dummy_files; f; f = f->next)
		if (!strcasecmp(f->name, name))
			return f;

	/* Not on list, create a new dummy file. */
	f = xcalloc(1, sizeof *f, __func__);
	f->dummy = true;
	f->name = xstrdup(name, __func__);

	/*
	 * File has no revision number, but pretend it's rev. 1.1 since this
	 * gets printed in commit messages.
	 */
	f->head.n[0] = 1;
	f->head.n[1] = 1;
	f->head.c = 2;

	/*
	 * There is no way to know whether a dummy file is a binary file, since
	 * that information is stored in the missing RCS master.  However,
	 * treating the dummy file as a binary file is convenient, since it
	 * allows exporting the copy of the file in the project directory (if it
	 * exists).
	 */
	f->binary = true;

	/* Add new dummy file to the list of dummy files */
	f->next = dummy_files;
	dummy_files = f;

	return f;
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
	static const char flist_start_marker[] = "EndOptions";
	static const char file_prefix[] = "$(projectdir)/";
	struct rcs_file_revision *head, **prev, *frev;
	struct rcs_file *file;
	const char *flist, *line, *lp, *endline;
	char file_path[1024], errline[1024], rcsnumstr[RCS_MAX_REV_LEN];
	struct rcs_number revnum;
	char *fp, *rp;
	bool in_quote;

	prev = &head;

	flist = find_line(pjdata, flist_start_marker);
	if (!flist)
		fatal_error("missing \"%s\" in %s", flist_start_marker,
			project->master_name);
	flist = next_line(flist);

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
	for (line = flist; line; line = next_line(line)) {
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

		/*
		 * project.pj can point to RCS files outside the RCS directory,
		 * in which case the prefix will be different.  However, since
		 * we don't have access to these RCS files outside the project
		 * (usually they were on someone's local machine and are thus
		 * lost forever) we have little choice but to ignore these
		 * files.
		 */
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
			 * file is a binary file, MKSSI grabs the copy of the
			 * file in the project directory (which is sometimes,
			 * but not always, identical to the head revision); if
			 * it is a text file, it grabs rev. 1.1 without doing
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
			if (revnum.c) {
				fprintf(stderr, "warning: ignoring file "
					"without RCS master file:\n");
				fprintf(stderr, "\t%s\n", errline);
				free(frev);
				continue;
			}

			/*
			 * Files with member type "other" can be exported even
			 * when the RCS file is missing.  We use a dummy file
			 * structure to make that work with the rest of this
			 * program.
			 */
			file = rcs_file_dummy_find_or_add(file_path);
		}
		if (file->corrupt) {
			free(frev);
			continue;
		}
		frev->canonical_name = xstrdup(file_path, __func__);
		if (revnum.c)
			frev->rev = revnum;
		else { /* "Other" member type */
			/*
			 * Flag this as the "other" type so that we can export
			 * the special binary blobs for them.
			 */
			frev->member_type_other = true;
			file->has_member_type_other = true;

			/*
			 * For binary files, we want the copy of the file in the
			 * project directory.  However, if the project directory
			 * isn't available, we will substitute the head
			 * revision.
			 *
			 * For text files, we want rev. 1.1 without RCS keyword
			 * expansion.
			 */
			if (file->binary)
				frev->rev = file->head;
			else {
				/* rev. 1.1 */
				frev->rev.n[0] = 1;
				frev->rev.n[1] = 1;
				frev->rev.c = 2;
			}
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
	static const char start_marker[] = "block _mks_variant_projects";
	static const char end_marker[] = "end";
	const char *start, *end, *line, *endline;

	/* Find the start of the branch list. */
	start = find_line(pjdata, start_marker);
	if (!start)
		return; /* No branches in this project.pj */

	start = next_line(start); /* Branch list starts on the next line. */

	/* Find the end of the branch list. */
	end = find_line(start, end_marker);
	if (!end)
		fatal_error("unterminated block of variant projects");

	for (line = start; line < end; line = next_line(line)) {
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
		if (frev->file->dummy)
			continue;

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
		path = sprintf_alloc("%s/%s", mkssi_proj_dir_path,
			proj_projectpj_name);
	else
		path = sprintf_alloc("%s/%s/%s", mkssi_proj_dir_path,
			proj_projectvpj_name, b->pj_name);

	/* Get the entire project file as a NUL-terminated string. */
	pjdata = file_as_string(path);

	/*
	 * Get the list of file revisions.  If this is the master, also save the
	 * branch list.
	 */
	b->tip_frevs = project_parse_revision(pjdata, &b->number, is_master);

	/*
	 * Save the revision number which appears in the project data for this
	 * branch.  This is used for disambiguation when a project.pj revision
	 * has multiple branches.
	 */
	project_data_extract_revnum(pjdata, &b->tip_number);

	/*
	 * Save the mtime of the project file.  This will be used as the commit
	 * timestamp for events (like file deletions) that have no RCS timestamp
	 */
	b->mtime = file_mtime(path);

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
	 * If the project.vpj directory doesn't exist in the project directory,
	 * then there are no branches.
	 */
	if (!proj_projectvpj_name) {
		/*
		 * Print a warning if the project.vpj directory _should_ have
		 * existed, because the project has branches.  In such cases,
		 * we won't be able to export the tip revisions for the
		 * branches.  We can still export everything else, though, so
		 * this isn't a fatal error.
		 */
		if (project_branches->next)
			fprintf(stderr, "warning: project has branches but "
				"there is no project.vpj directory\n");

		return;
	}

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
