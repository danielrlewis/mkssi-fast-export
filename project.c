#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interfaces.h"

/* list of file revisions for each checkpoint */
struct cp_files {
	struct cp_files *next;
	const struct rcs_version *pjver;
	const struct rcs_file_revision *frevs;
};

/* Linked list of file revisions included in each checkpoint */
static struct cp_files *cp_files;

/* validate that a string looks like a given revision of project.pj */
static void
validate_project_data(const struct rcs_number *revnum, const char *pjdata)
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
	const char *s;

	bucket = hash_string(name) % ARRAY_SIZE(file_hash_table);
	for (f = file_hash_table[bucket]; f; f = f->hash_next) {
		if (strcasecmp(f->name, name))
			continue;

		/*
		 * Kluge: For historical reasons, some of the old source files
		 * with 8.3 file names are stored with UPPERCASE file names even
		 * though they are lower- or mixed-case in the MKSSI file
		 * revision list, and thus get converted to lower- or mixed-case
		 * in MKSSI sandboxes.  We assume here that a case mismatch of
		 * an uppercase 8.3 name is due to this problem, and thus allow
		 * the search name to supersede the file name.  This avoids
		 * exporting the file with letters of the wrong case, causing
		 * build failures.  This really should happen elsewhere.
		 */
		if (strcmp(f->name, name)) {
			s = f->name + strlen(f->name);
			for (; *s != '/' && s > f->name; --s)
				;

			/* If the file name looks like 8.3 */
			if (strlen(s) <= 12 && string_is_upper(s)) {
				fprintf(stderr, "warning: name case fixup: "
					"\"%s\" -> \"%s\"\n",
					f->name, name);
				strcpy(f->name, name);
			}
		}
		return f;
	}

	/* Try the corrupt files (hopefully a short list!) */
	for (f = corrupt_files; f; f = f->next)
		if (!strcasecmp(f->name, name))
			/*
			 * Note the UPPERCASE-fixing kluge is not needed here,
			 * since corrupt files are not exported.
			 */
			return f;

	return NULL;
}

/* load file revision list from a revision of project.pj */
static struct rcs_file_revision *
project_revision_read_files(const char *pjdata)
{
	const char flist_start_marker[] = "\nEndOptions\n";
	const char file_prefix[] = "$(projectdir)/";
	struct rcs_file_revision *head, **prev, *frev;
	const char *flist, *line, *lp, *endline;
	char file_path[1024], errline[1024], rcsnumstr[RCS_MAX_REV_LEN];
	char *fp, *rp;
	bool in_quote;

	head = NULL;
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

		if (!strncmp(lp, " a ", 3))
			lp += 3;
		else if (!strncmp(lp, " f", 2))
			/*
			 * According the manual, "f" means other, but there is
			 * no explanation of what that means.  It might be
			 * related to deleting and re-adding files.  Seems to be
			 * rare.  Maybe can be ignored?
			 */
			continue;
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

		/* Copy the revision number into a NUL-terminated buffer */
		rp = rcsnumstr;
		while (*lp == '.' || (*lp >= '0' && *lp <= '9')) {
			if (rp - rcsnumstr >= sizeof rcsnumstr - 1) {
				fprintf(stderr, "error on line:\n\t%s\n",
					errline);
				fatal_error("revision number too long");
			}
			*rp++ = *lp++;
		}
		*rp++ = '\0';

		frev = xcalloc(1, sizeof *frev, __func__);
		frev->file = rcs_file_find(file_path);
		if (!frev->file) {
			fprintf(stderr, "error on line:\n\t%s\n", errline);
			fatal_error("no RCS master for file \"%s\"", file_path);
		}
		if (frev->file->corrupt || frev->file->binary /* TEMP */)
			free(frev);
		else {
			frev->rev = lex_number(rcsnumstr);
			*prev = frev;
			prev = &frev->next;
		}
	}

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
static struct rcs_symbol *
parse_project_branch_line(const char *line, const char *endline)
{
	char rcs_num_str[RCS_MAX_REV_LEN];
	const char *pos, *name_start;
	struct rcs_symbol *branch;
	unsigned int i;

	branch = xcalloc(1, sizeof *branch, __func__);

	/*
	 * These lines are formatted as such:
	 * 	revnum=vpNNNN.pj, "BranchName"
	 * For example:
	 * 	1.2=vp0000.pj, "v1_0_Release"
	 */

	for (i = 0, pos = line; *pos != '='; ++pos, ++i) {
		if (i == sizeof rcs_num_str - 1)
			fatal_error("revision number too long: %s", line);
		if (*pos != '.' && !(*pos >= '0' && *pos <= '9'))
			fatal_error("invalid revision number: %s", line);
		rcs_num_str[i] = *pos;
	}
	rcs_num_str[i] = '\0';

	branch->number = lex_number(rcs_num_str);

	pos = strstr(pos, "\"");
	if (!pos || pos > endline)
		fatal_error("missing branch name: %s", line);

	++pos; /* Move past the '"' */
	name_start = pos;
	pos = strstr(pos, "\"");
	if (!pos || pos > endline)
		fatal_error("unterminated branch name: %s", line);

	branch->symbol_name = xcalloc(1, pos - name_start + 1, __func__);
	memcpy(branch->symbol_name, name_start, pos - name_start);
	sanitize_branch_name(branch->symbol_name);

	return branch;
}

/* add a project branch to the list, if it is not there already */
static void
project_branch_add(struct rcs_symbol **branches, struct rcs_symbol *branch)
{
	struct rcs_symbol *b, **bptr;
	int cmp;

	/*
	 * The special "trunk branch" is used for weird projects where the trunk
	 * somehow becomes an namless branch revision.  The trunk branch should
	 * never have the same branch number as an actual legit branch.
	 */
	if (rcs_number_equal(&branch->number, &trunk_branch))
		fatal_error("specified trunk branch rev. %s is used by an "
			"actual branch named \"%s\"",
			rcs_number_string_sb(&trunk_branch),
			branch->symbol_name);

	/*
	 * This branch might have already been recorded from another revision of
	 * project.pj.  If so, ignore it.
	 */
	bptr = branches;
	for (b = *bptr; b; b = *bptr) {
		if (!strcmp(b->symbol_name, branch->symbol_name)) {
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
				free(b->symbol_name);
				free(b);
				goto insert;
			}

			/* Branch already in list. */
			free(branch);
			return;
		}
		bptr = &b->next;
	}

insert:
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
project_revision_read_branches(struct rcs_symbol **branches, const char *pjdata)
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

static void
mark_checkpointed_revisions(struct rcs_file_revision *frevs)
{
	struct rcs_version *ver;
	struct rcs_file_revision *frev;

	for (frev = frevs; frev; frev = frev->next) {
		ver = rcs_file_find_version(frev->file, &frev->rev, false);
		if (ver)
			ver->checkpointed = true;
	}
}

static void
save_checkpoint_file_revisions(const struct rcs_number *pjrev,
	const struct rcs_file_revision *frev_list)
{
	struct cp_files *cpf;

	cpf = xcalloc(1, sizeof *cpf, __func__);
	cpf->pjver = rcs_file_find_version(project, pjrev, true);
	cpf->frevs = frev_list;
	cpf->next = cp_files;
	cp_files = cpf;
}

const struct rcs_file_revision *
find_checkpoint_file_revisions(const struct rcs_number *pjrev)
{
	const struct cp_files *cpf;

	for (cpf = cp_files; cpf; cpf = cpf->next)
		if (rcs_number_equal(&cpf->pjver->number, pjrev))
			return cpf->frevs;
	fatal_error("no saved file revision list for project rev. %s",
		rcs_number_string_sb(pjrev));
	return NULL; /* unreachable */
}

static void
project_data_handler(struct rcs_file *file, const struct rcs_number *revnum,
	const char *data)
{
	struct rcs_file_revision *frev_list;

	export_progress("parsing project revision %s",
		rcs_number_string_sb(revnum));

	validate_project_data(revnum, data);

	frev_list = project_revision_read_files(data);
	mark_checkpointed_revisions(frev_list);
	save_checkpoint_file_revisions(revnum, frev_list);

	project_revision_read_branches(&project_branches, data);
}

void
project_read_all_revisions(void)
{
	export_progress("reading checkpointed project revisions");
	rcs_file_read_all_revisions(project, project_data_handler);
}
