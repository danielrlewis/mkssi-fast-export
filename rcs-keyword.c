/*
 * Copyright (c) 2017, 2019-2020 Tuxera US Inc
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Expand RCS keywords in text files.
 */
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "interfaces.h"

/* unescape double-@@ characters to single-@ */
void
rcs_data_unescape_ats(struct rcs_line *dlines)
{
	struct rcs_line *dl;
	char *lp, *at;

	for (dl = dlines; dl; dl = dl->next) {
		/*
		 * If this line contains an "@@", make sure its line buffer is
		 * writable.
		 */
		if (line_findstr(dl->line, "@@"))
			line_allocate(dl);

		/*
		 * Every time we see an "@@", shift the line contents to the
		 * left to squash the 2nd '@'.
		 */
		lp = dl->line;
		while ((at = line_findstr(lp, "@@"))) {
			memmove(at + 1, at + 2, line_length(at + 2) + 1);
			lp = at + 1;
			dl->len--;
		}
	}
}

/* re-escape single-@ characters to double-@@ */
static void
rcs_data_reescape_ats(struct rcs_line *dlines)
{
	struct rcs_line *dl;
	unsigned int at_count;
	const char *lp;
	char *lnbuf, *pos;

	for (dl = dlines; dl; dl = dl->next) {
		at_count = 0;
		for (lp = dl->line; lp < &dl->line[dl->len]; ++lp)
			if (*lp == '@')
				++at_count;

		if (!at_count)
			continue;

		lnbuf = pos = xmalloc(dl->len + at_count + 1, __func__);
		for (lp = dl->line; lp < &dl->line[dl->len]; ++lp) {
			*pos++ = *lp;
			if (*lp == '@')
				*pos++ = '@';
		}
		*pos++ = '\0';

		if (dl->line_allocated)
			free(dl->line);

		dl->line = lnbuf;
		dl->len += at_count;
		dl->line_allocated = true;
	}
}

/* search for an RCS lock for a version of a file */
static const struct rcs_lock *
lock_find(const struct rcs_file *file, const struct rcs_version *ver)
{
	const struct rcs_lock *l;

	for (l = file->locks; l; l = l->next)
		if (rcs_number_equal(&l->number, &ver->number))
			return l;
	return NULL;
}

/* mark version as having an RCS keyword that expands to a file name */
static void
name_keyword(const struct rcs_file *file, struct rcs_version *ver)
{
	ver->kw_name = true;

	/*
	 * If the canonical capitalization of the file name changes over time,
	 * then this file revision will need to be exported just-in-time, in the
	 * context of the project revision, in order for the keyword to expand
	 * to the canonical capitalization which is correct for that project
	 * revision.
	 */
	if (file->name_changes > 1)
		ver->jit = true;
}

/* mark version as having an RCS keyword that expands to a file path */
static void
path_keyword(const struct rcs_file *file, struct rcs_version *ver)
{
	ver->kw_path = true;

	/* See comment in name_keyword(): the same applies for the path. */
	if (file->path_changes > 1)
		ver->jit = true;
}

/* signature for a keyword expander function */
typedef char *(keyword_expander_t)(const struct rcs_file *file,
	struct rcs_version *ver);

/* generate an expanded $Author$ keyword string */
static char *
expanded_author_str(const struct rcs_file *file, struct rcs_version *ver)
{
	return sprintf_alloc("$Author: %s $", ver->author);
}

/* generate an expanded $Date$ keyword string */
static char *
expanded_date_str(const struct rcs_file *file, struct rcs_version *ver)
{
	return sprintf_alloc("$Date: %s $", ver->date.string);
}

/* generate an expanded $Header$ keyword string */
static char *
expanded_header_str(const struct rcs_file *file, struct rcs_version *ver)
{
	const char *path;

	path_keyword(file, ver);

	/* See comment about the source dirpath in expanded_source_str(). */
	if (source_dir_path)
		path = source_dir_path;
	else {
		fprintf(stderr, "warning: $Header$ in %s rev. %s is being "
			"incorrectly expanded, because --source-dir was not "
			"provided\n", file->name,
			rcs_number_string_sb(&ver->number));
		path = mkssi_rcs_dir_path;
	}

	return sprintf_alloc("$Header: %s/%s %s %s %s %s $", path, file->name,
		rcs_number_string_sb(&ver->number), ver->date.string,
		ver->author, ver->state);
}

/* generate an expanded $Id$ keyword string */
static char *
expanded_id_str(const struct rcs_file *file, struct rcs_version *ver)
{
	const struct rcs_lock *lock;

	name_keyword(file, ver);

	/* If the file is locked, $Id$ includes the locker username. */
	lock = lock_find(file, ver);

	return sprintf_alloc("$Id: %s %s %s %s %s%s%s $", path_to_name(file->name),
		rcs_number_string_sb(&ver->number), ver->date.string,
		ver->author, ver->state,
		lock ? " " : "",
		lock ? lock->locker : "");
}

/* generate an expanded $Locker$ keyword string */
static char *
expanded_locker_str(const struct rcs_file *file, struct rcs_version *ver)
{
	const struct rcs_lock *lock;

	lock = lock_find(file, ver);
	if (!lock)
		return xstrdup("$Locker: $", __func__);

	return sprintf_alloc("$Locker: %s $", lock->locker);
}

/* generate an expanded $ProjectName$ keyword string */
static char *
expanded_projectname_str(const struct rcs_file *file, struct rcs_version *ver)
{
	const char *path, *name;

	/*
	 * The correct expansion of this keyword uses the project.pj name in the
	 * project directory.  However, if there is no project directory,
	 * fallback on the project.pj name in the RCS directory.
	 */
	if (proj_projectpj_name)
		name = proj_projectpj_name;
	else
		name = rcs_projectpj_name;

	/*
	 * The correct expansion of this keyword will use a quasi-Windows style
	 * path with a drive letter (but forward slashes for the path
	 * separators) pointing to the project directory.  This cannot be
	 * derived; it must be provided via the --pname-dir argument.  If that
	 * argument wasn't provided, fallback on other directories.
	 */
	if (pname_dir_path)
		path = pname_dir_path;
	else if (mkssi_proj_dir_path)
		path = mkssi_proj_dir_path;
	else
		path = mkssi_rcs_dir_path;

	if (!proj_projectpj_name || !pname_dir_path)
		fprintf(stderr, "warning: $ProjectName$ in %s rev. %s is being "
			"incorrectly expanded, because --proj-dir or "
			"--pname-dir was not provided\n", file->name,
			rcs_number_string_sb(&ver->number));

	return sprintf_alloc("$ProjectName: %s/%s $", path, name);
}

/* generate an expanded $ProjectRevision$ keyword string */
static char *
expanded_projectrevision_str(const struct rcs_file *file,
	struct rcs_version *ver)
{
	const struct rcs_number *revnum;

	/*
	 * $ProjectRevision$ is an unfortunate keyword.  It expands to the
	 * project.pj file revision which is being used to check-out the file.
	 * Thus, if a given file revision is referenced from multiple branches
	 * or checkpoints, and if that file revision has a $ProjectRevision$
	 * keyword, the contents of the file revision will be different for the
	 * various branches and checkpoints that reference it, depending on the
	 * respective project.pj revision.  This means that we cannot export a
	 * single blob for the file revision and reuse it everywhere.
	 */

	ver->kw_projrev = true; /* Version has $ProjectRevision$ */
	ver->jit = true; /* Version will need to be just-in-time exported */

	/*
	 * pj_revnum_cur is the project.pj revision number that is currently
	 * being exported.  It is the correct number to use for this keyword.
	 *
	 * If we aren't yet exporting project revision, pj_revnum_cur will be
	 * unpopulated.  The revision number in that case is unimportant, since
	 * the blob we are exporting will never be used.
	 */
	if (pj_revnum_cur.c)
		revnum = &pj_revnum_cur;
	else
		revnum = &project->head;

	return sprintf_alloc("$ProjectRevision: %s $",
		rcs_number_string_sb(revnum));
}

/* generate an expanded $RCSfile$ keyword string */
static char *
expanded_rcsfile_str(const struct rcs_file *file, struct rcs_version *ver)
{
	name_keyword(file, ver);

	return sprintf_alloc("$RCSfile: %s $", path_to_name(file->name));
}

/* generate an expanded $Log$ keyword string */
static char *
expanded_log_str(const struct rcs_file *file, struct rcs_version *ver)
{
	name_keyword(file, ver);

	return sprintf_alloc("$Log: %s $", path_to_name(file->name));
}

/* generate an expanded $Revision$ keyword string */
static char *
expanded_revision_str(const struct rcs_file *file, struct rcs_version *ver)
{
	return sprintf_alloc("$Revision: %s $",
		rcs_number_string_sb(&ver->number));
}

/* generate an expanded $Source$ keyword string */
static char *
expanded_source_str(const struct rcs_file *file, struct rcs_version *ver)
{
	const char *path;

	path_keyword(file, ver);

	/*
	 * The correct expansion of the $Source$ (and $Header$) keyword will use
	 * a quasi-Windows style path with a drive letter (but forward slashes
	 * for path separators) pointing to the RCS directory.  This cannot be
	 * derived, so it needs to be specified with --source-dir.
	 */
	if (source_dir_path)
		path = source_dir_path;
	else {
		fprintf(stderr, "warning: $Source$ in %s rev. %s is being "
			"incorrectly expanded, because --source-dir was not "
			"provided\n", file->name,
			rcs_number_string_sb(&ver->number));
		path = mkssi_rcs_dir_path;
	}

	return sprintf_alloc("$Source: %s/%s $", path, file->name);
}

/* generate an expanded $State$ keyword string */
static char *
expanded_state_str(const struct rcs_file *file, struct rcs_version *ver)
{
	return sprintf_alloc("$State: %s $", ver->state);
}

/* replace a keyword on a line with its expanded revision */
static void
expand_keyword(struct rcs_line *line, size_t kw_start, size_t kw_end,
	const char *expanded_str)
{
	char *lnbuf, *pos;

	pos = lnbuf = xmalloc(kw_start + strlen(expanded_str) + (line->len -
		kw_end) + 1, __func__);
	memcpy(pos, line->line, kw_start);
	pos += kw_start;
	strcpy(pos, expanded_str);
	pos += strlen(expanded_str);
	memcpy(pos, line->line + kw_end, line->len - kw_end);
	pos += line->len - kw_end;
	*pos++ = '\0';
	if (line->line_allocated)
		free(line->line);
	line->line = lnbuf;
	line->len = pos - lnbuf - 1;
	line->line_allocated = true;
}

/* expand a generic RCS keyword */
static void
rcs_data_expand_generic_keyword(const struct rcs_file *file,
	struct rcs_version *ver, struct rcs_line *dlines,
	const char *keyword, keyword_expander_t expander)
{
	struct rcs_line *dl;
	char *lp, *kw, *expanded;

	for (dl = dlines; dl; dl = dl->next) {
		/* Look for the keyword on this line */
		kw = line_findstr(dl->line, keyword);
		if (!kw)
			continue;

		/*
		 * Keyword must be "$Keyword$" or "$Keyword: blah $", where
		 * "blah" is most likely a previous expansion of the keyword.
		 */
		lp = kw + strlen(keyword);
		if (*lp == ':')
			for (++lp; *lp && *lp != '\n' && *lp != '$'; ++lp)
				;

		/* If no closing '$' was found, then not a keyword */
		if (*lp != '$')
			continue;
		++lp; /* Move past the '$' */

		expanded = expander(file, ver);
		expand_keyword(dl, kw - dl->line, lp - dl->line, expanded);
		free(expanded);
	}
}

/* generate header for log text */
static struct rcs_line *
log_header(const struct rcs_version *ver, const char *template_line,
	size_t prefix_len, size_t postfix_start, size_t postfix_len)
{
	struct rcs_line *loghdr;
	char *pos, *hdr;

	/*
	 * Generate the first line of the log message from the metadata.  An
	 * example of what this might look like:
	 *
	 * 	Revision 1.8  2012/12/11 23:45:55Z  daniel.lewis
	 */
	loghdr = xcalloc(1, sizeof *loghdr, __func__);
	hdr = sprintf_alloc("Revision %s  %s  %s",
		rcs_number_string_sb(&ver->number),
		ver->date.string, ver->author);
	loghdr->len = prefix_len + strlen(hdr) + postfix_len;
	loghdr->line = pos = xmalloc(loghdr->len + 1, __func__);
	memcpy(pos, template_line, prefix_len);
	pos += prefix_len;
	strcpy(pos, hdr);
	pos += strlen(hdr);
	memcpy(pos, &template_line[postfix_start], postfix_len);
	pos += postfix_len;
	*pos++ = '\0';
	free(hdr);

	return loghdr;
}

/* convert log text into a sequence of lines to insert into the source file */
static struct rcs_line *
log_text_to_lines(char *log, const char *template_line, size_t prefix_len,
	size_t postfix_start, size_t postfix_len)
{
	struct rcs_line *loglines, *ll;
	char *lnbuf, *pos;

	/* Break the log (check-in comment) text into lines */
	loglines = string_to_lines(log);

	/*
	 * Duplicate an MKSSI bug: any '@' character in a revision history
	 * comment will show up as "@@" when the log keyword is expanded, due to
	 * a failure to unescape the "@@" character sequence in this context.
	 * (Remarkably, in other contexts, such as the member archive GUI, the
	 * log message is correctly displayed with just a single '@'.)
	 */
	rcs_data_reescape_ats(loglines);

	/* Add the prefix/postfix onto each log line */
	for (ll = loglines; ll; ll = ll->next) {
		/* lnbuf = prefix + ll->line */
		lnbuf = pos = xmalloc(prefix_len + ll->len + postfix_len + 1,
			__func__);
		memcpy(pos, template_line, prefix_len);
		pos += prefix_len;
		memcpy(pos, ll->line, ll->len);
		pos += ll->len;
		memcpy(pos, &template_line[postfix_start], postfix_len);
		pos += postfix_len;
		*pos++ = '\0';

		/*
		 * Replace original log line with the version containing the
		 * prefix.
		 */
		if (ll->line_allocated)
			free(ll->line);
		ll->line = lnbuf;
		ll->line_allocated = true;
		ll->len = pos - lnbuf - 1;
		ll->no_newline = false;
	}

	return loglines;
}

/* expand "$Log$" keyword and insert revision history comment after it */
static void
rcs_data_expand_log_keyword(const struct rcs_file *file,
	struct rcs_version *ver, const struct rcs_patch *patch,
	struct rcs_line *dlines)
{
	struct rcs_line *dl, *loghdr, *loglines, *ll;
	char *lp, *kw, *log, *template_line;
	size_t prefix_len, postfix_len, postfix_start;
	struct rcs_number num;
	const struct rcs_version *pver;
	const struct rcs_patch *ppatch;

	for (dl = dlines; dl; dl = dl->next) {
		/* Look for the $Log$ keyword on this line */
		kw = line_findstr(dl->line, "$Log");
		if (!kw)
			continue;

		/*
		 * Keyword must be "$Log$" or "$Log: blah $", where "blah" is
		 * most likely a previous expansion of the keyword.
		 */
		lp = kw + strlen("$Log");
		if (*lp == ':')
			for (++lp; *lp && *lp != '\n' && *lp != '$'; ++lp)
				;

		/* If no closing '$' was found, then not a log keyword */
		if (*lp != '$')
			continue;
		++lp; /* Move past '$' */

		/*
		 * Any characters (usually white space or comment delimiters)
		 * preceding and following the log keyword need to be included
		 * on the log lines.
		 */
		prefix_len = kw - dl->line;
		postfix_start = lp - dl->line;
		postfix_len = dl->len - postfix_start;

		/*
		 * As mentioned just above, the original $Log$ line is the
		 * template for leading/trailing characters.  However, dl->line
		 * is about to be modified to expand the $Log$ keyword, which --
		 * if there are trailing characters -- can alter the portions of
		 * the line needed for the template.  Avoid this by making a
		 * temporary copy of the template line.
		 */
		template_line = xmalloc(dl->len + 1, __func__);
		memcpy(template_line, dl->line, dl->len);
		template_line[dl->len] = '\0';

		/*
		 * Whatever "$Log...$" string we find needs to be replaced by
		 * "$Log: filename $", where filename is the basename of the
		 * file.
		 */
		log = expanded_log_str(file, ver);
		expand_keyword(dl, prefix_len, postfix_start, log);
		free(log);

		/*
		 * Generate the first line of the log message from the metadata.
		 */
		loghdr = log_header(ver, template_line, prefix_len,
			postfix_start, postfix_len);

		/*
		 * Add lines for the check-in comment.  The prefix must be added
		 * to each of them.
		 */
		if (*patch->log) {
			/* Break the log (check-in comment) text into lines */
			loglines = log_text_to_lines(patch->log, template_line,
				prefix_len, postfix_start, postfix_len);

			/* Make ll point at the last of the log lines */
			for (ll = loglines; ll->next; ll = ll->next)
				;

			/*
			 * If this is a duplicate revision, the log text of the
			 * revision from which it was duplicated must be
			 * included.  This is done only once, even if the
			 * revision is a duplicate of a duplicate.
			 */
			num = ver->number;
			if (!strcmp(patch->log, "Duplicate revision\n")
			 && num.c >= 4 && num.n[num.c - 1] == 1) {
				rcs_number_decrement(&num);
				pver = rcs_file_find_version(file, &num, false);
				ppatch = rcs_file_find_patch(file, &num, false);
				if (!pver || !ppatch)
					break;

				ll->next = log_header(pver, template_line,
					prefix_len, postfix_start, postfix_len);
				ll = ll->next;
				ll->next = log_text_to_lines(ppatch->log,
					template_line, prefix_len,
					postfix_start, postfix_len);
				for (; ll->next; ll = ll->next)
					;
			}

			/* Link the log message into the list of lines */
			ll->next = dl->next;
			loghdr->next = loglines;
			dl->next = loghdr;
			dl = ll;
		} else {
			/*
			 * Empty check-in comment, so just link the log header
			 * into the list of lines.
			 */
			loghdr->next = dl->next;
			dl->next = loghdr;
			dl = loghdr;
		}

		free(template_line);
	}
}

/* expand RCS escapes and keywords */
void
rcs_data_keyword_expansion(const struct rcs_file *file, struct rcs_version *ver,
	const struct rcs_patch *patch, struct rcs_line *dlines)
{
	struct {
		const char *keyword;
		keyword_expander_t *expandfn;
	} keywords[] = {
		{"$Author", expanded_author_str},
		{"$Date", expanded_date_str},
		{"$Header", expanded_header_str},
		{"$Id", expanded_id_str},
		{"$Locker", expanded_locker_str},
		{"$ProjectName", expanded_projectname_str},
		{"$ProjectRevision", expanded_projectrevision_str},
		{"$RCSfile", expanded_rcsfile_str},
		{"$Revision", expanded_revision_str},
		{"$Source", expanded_source_str},
		{"$State", expanded_state_str},
	};
	unsigned int i;

	/* Replace "@@" escape sequences with "@" */
	rcs_data_unescape_ats(dlines);

	/* Expand RCS keywords */
	for (i = 0; i < ARRAY_SIZE(keywords); ++i)
		rcs_data_expand_generic_keyword(file, ver, dlines,
			keywords[i].keyword, keywords[i].expandfn);

	/*
	 * Expand the "$Log$" RCS keyword -- this is somewhat more complicated,
	 * so it needs its own function, rather than using the "generic" code.
	 */
	rcs_data_expand_log_keyword(file, ver, patch, dlines);
}
