/* Expand RCS keywords in text files */
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "interfaces.h"

/* signature for a keyword expander function */
typedef char *(keyword_expander_t)(const struct rcs_file *file,
	const struct rcs_version *ver);

/* get the name element of a path */
const char *
path_to_name(const char *path)
{
	const char *name;

	/* For example: "a/b/c" yields "c", "a" yields "a" */

	if (!*path)
		return path;

	name = path + strlen(path) - 1;
	while (name > path && *name != '/')
		--name;
	if (*name == '/')
		++name;

	return name;
}

/* generate an expanded $Author$ keyword string */
static char *
expanded_author_str(const struct rcs_file *file,
	const struct rcs_version *ver)
{
	return sprintf_alloc("$Author: %s $", ver->author);
}

/* generate an expanded $Date$ keyword string */
static char *
expanded_date_str(const struct rcs_file *file,
	const struct rcs_version *ver)
{
	return sprintf_alloc("$Date: %s $", time2string(ver->date));
}

/* generate an expanded $Header$ keyword string */
static char *
expanded_header_str(const struct rcs_file *file,
	const struct rcs_version *ver)
{
	return sprintf_alloc("$Id: %s %s %s %s %s $", file->name,
		rcs_number_string_sb(&ver->number), time2string(ver->date),
		ver->author, ver->state);
}

/* generate an expanded $Id$ keyword string */
static char *
expanded_id_str(const struct rcs_file *file,
	const struct rcs_version *ver)
{
	return sprintf_alloc("$Id: %s %s %s %s %s $", path_to_name(file->name),
		rcs_number_string_sb(&ver->number), time2string(ver->date),
		ver->author, ver->state);
}

/* generate an expanded $Log$ keyword string */
static char *
expanded_log_str(const struct rcs_file *file,
	const struct rcs_version *ver)
{
	return sprintf_alloc("$Log: %s $", path_to_name(file->name));
}

/* generate an expanded $Revision$ keyword string */
static char *
expanded_revision_str(const struct rcs_file *file,
	const struct rcs_version *ver)
{
	return sprintf_alloc("$Revision: %s $",
		rcs_number_string_sb(&ver->number));
}

/* generate an expanded $Source$ keyword string */
static char *
expanded_source_str(const struct rcs_file *file,
	const struct rcs_version *ver)
{
	const char *path;

	if (source_dir_path)
		path = source_dir_path;
	else
		path = mkssi_dir_path;

	return sprintf_alloc("$Source: %s/%s $", path, file->name);
}

/* generate an expanded $Source$ keyword string */
static char *
expanded_state_str(const struct rcs_file *file,
	const struct rcs_version *ver)
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
	const struct rcs_version *ver, struct rcs_line *dlines,
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
		 * Before the closing '$', there can be other characters, such
		 * as a previously expanded version of the keyword.
		 */
		lp = kw + strlen(keyword);
		for (; *lp && *lp != '\n' && *lp != '$'; ++lp)
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

/* expand "$Log$" keyword and insert revision history comment after it */
static void
rcs_data_expand_log_keyword(const struct rcs_file *file,
	const struct rcs_version *ver, const struct rcs_patch *patch,
	struct rcs_line *dlines)
{
	struct rcs_line *dl, *loghdr, *loglines, *ll;
	char *lp, *kw, *lnbuf, *log, *hdr, *pos;
	size_t prefix_len, postfix_len, postfix_start;

	for (dl = dlines; dl; dl = dl->next) {
		/* Look for the $Log$ keyword on this line */
		kw = line_findstr(dl->line, "$Log");
		if (!kw)
			continue;

		/*
		 * Before the closing '$', there can be other characters, such
		 * as the file name or spaces.
		 */
		lp = kw + strlen("$Log");
		for (; *lp && *lp != '\n' && *lp != '$'; ++lp)
			;

		/* If no closing '$' was found, then not a log keyword */
		if (*lp != '$')
			continue;
		++lp; /* Move past '$' */

		/*
		 * Any white space or comment characters preceding and following
		 * the log keyword need to be included on the log lines.
		 */
		prefix_len = kw - dl->line;
		postfix_start = lp - dl->line;
		postfix_len = dl->len - postfix_start;

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
		 * An example of what this might look like:
		 *
		 * 	Revision 1.8  2012/12/11 23:45:55Z  daniel.lewis
		 */
		loghdr = xcalloc(1, sizeof *loghdr, __func__);
		hdr = sprintf_alloc("Revision %s  %s  %s",
			rcs_number_string_sb(&ver->number),
			time2string(ver->date), ver->author);
		loghdr->len = prefix_len + strlen(hdr) + postfix_len;
		loghdr->line = pos = xmalloc(loghdr->len + 1, __func__);
		memcpy(pos, dl->line, prefix_len);
		pos += prefix_len;
		strcpy(pos, hdr);
		pos += strlen(hdr);
		memcpy(pos, &dl->line[postfix_start], postfix_len);
		pos += postfix_len;
		*pos++ = '\0';
		free(hdr);

		/*
		 * Add lines for the check-in comment.  The prefix must be added
		 * to each of them.
		 */
		if (*patch->log) {
			/* Break the log (check-in comment) text into lines */
			loglines = string_to_lines(patch->log);

			/* Add the prefix/postfix onto each log line */
			for (ll = loglines; ll; ll = ll->next) {
				/* lnbuf = prefix + ll->line */
				lnbuf = pos = xmalloc(prefix_len + ll->len +
					postfix_len + 1, __func__);
				memcpy(pos, dl->line, prefix_len);
				pos += prefix_len;
				memcpy(pos, ll->line, ll->len);
				pos += ll->len;
				memcpy(pos, &dl->line[postfix_start],
					postfix_len);
				pos += postfix_len;
				*pos++ = '\0';

				/*
				 * Replace original log line with the version
				 * containing the prefix.
				 */
				if (ll->line_allocated)
					free(ll->line);
				ll->line = lnbuf;
				ll->line_allocated = true;
				ll->len = pos - lnbuf - 1;
				ll->no_newline = false;
			}

			/* Make ll point at the last of the log lines */
			for (ll = loglines; ll->next; ll = ll->next)
				;

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
	}
}

/* unescape double-@@ characters to single-@ */
static void
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

/* expand RCS escapes and keywords */
void
rcs_data_keyword_expansion(const struct rcs_file *file,
	const struct rcs_version *ver, const struct rcs_patch *patch,
	struct rcs_line *dlines)
{
	rcs_data_unescape_ats(dlines);

	rcs_data_expand_generic_keyword(file, ver, dlines, "$Author",
		expanded_author_str);
	rcs_data_expand_generic_keyword(file, ver, dlines, "$Date",
		expanded_date_str);
	rcs_data_expand_generic_keyword(file, ver, dlines, "$Header",
		expanded_header_str);
	rcs_data_expand_generic_keyword(file, ver, dlines, "$Id",
		expanded_id_str);
	rcs_data_expand_generic_keyword(file, ver, dlines, "$Revision",
		expanded_revision_str);
	rcs_data_expand_generic_keyword(file, ver, dlines, "$Source",
		expanded_source_str);
	rcs_data_expand_generic_keyword(file, ver, dlines, "$State",
		expanded_state_str);

	rcs_data_expand_log_keyword(file, ver, patch, dlines);
}
