/* Utilities for working with numbered lines from an RCS file */
#include <stdlib.h>
#include <string.h>
#include "interfaces.h"

/*
 * Note on line endings: MKSSI RCS files use Unix line endings.  For most files,
 * that includes the patches -- however, a handful of files use Windows newlines
 * in the patches.  These are implicitly converted to Unix newlines by this
 * module.  Note that a \r which is NOT followed by a \n is not considered to be
 * a newline -- these show up inside patch text sometimes, but MKSSI does not
 * count them as line endings.
 */

/* convert a string into a list of numbered lines */
struct rcs_line *
string_to_lines(char *str)
{
	unsigned int lineno;
	struct rcs_line *head, **prev_next, *ln;
	char *lnptr;

	lineno = 1;
	prev_next = &head;
	for (lnptr = str; *lnptr;) {
		ln = xcalloc(1, sizeof *ln, __func__);
		ln->lineno = lineno;
		ln->line = lnptr;
		ln->line_allocated = false;
		ln->len = line_length(ln->line);

		*prev_next = ln;
		prev_next = &ln->next;
		lineno++;

		lnptr += ln->len;

		if (*lnptr == '\r' && lnptr[1] == '\n')
			++lnptr;

		if (*lnptr == '\n')
			++lnptr;
		else
			ln->no_newline = true;
	}
	*prev_next = NULL;

	/*
	 * If str was a zero-length string (an empty RCS patch), the lines list
	 * will be NULL.
	 */
	if (!head) {
		/*
		 * Some of the callers of this function want a non-NULL list in
		 * all cases, so allocate an empty line.
		 */
		head = xcalloc(1, sizeof *head, __func__);
		head->lineno = lineno;
		head->line = lnptr;
		head->no_newline = true;
	}

	return head;
}

/* convert a list of numbered lines into a string */
char *
lines_to_string(const struct rcs_line *lines)
{
	size_t len;
	const struct rcs_line *ln;
	char *str, *pos;

	/* add up the lengths of all lines */
	len = 0;
	for (ln = lines; ln; ln = ln->next)
		if (ln->line)
			len += ln->len + 1; /* +1 for newline */

	str = xmalloc(len + 1, __func__);

	/* copy line contents into string buffer */
	pos = str;
	for (ln = lines; ln; ln = ln->next)
		if (ln->line) {
			memcpy(pos, ln->line, ln->len);
			pos += ln->len;
			if (!ln->no_newline)
				*pos++ = '\n';
		}
	*pos = '\0';

	return str;
}

/* free a list of numbered lines */
void
lines_free(struct rcs_line *lines)
{
	struct rcs_line *ln, *nextln;

	for (ln = lines; ln; ln = nextln) {
		nextln = ln->next;
		if (ln->line && ln->line_allocated)
			free(ln->line);
		free(ln);
	}
}

/* reset a list of numbered lines */
void
lines_reset(struct rcs_line **lines)
{
	unsigned int lineno;
	struct rcs_line *ln, **prev_next;

	/*
	 * When a line is deleted, its buffer is set to NULL, but it remains in
	 * the list and subsequent lines are not renumbered.  Likewise, when
	 * lines are inserted, they are added without line numbers and
	 * subsequent lines are not renumbered.  This works well for the RCS
	 * patch format, where all lines number references are anchored to the
	 * previous revision prior to any patching.  However, once a patch is
	 * applied, the list needs to be reset for the next patch.
	 */
	lineno = 0;
	prev_next = lines;
	for (ln = *prev_next; ln; ln = *prev_next) {
		if (ln->line) {
			ln->lineno = ++lineno;
			prev_next = &ln->next;
		} else {
			*prev_next = ln->next;
			free(ln);
		}
	}
}

/* find the length of a line string (\n, \r\n, or NUL terminated) */
size_t
line_length(const char *line)
{
	const char *s;

	for (s = line; *s; ++s) {
		if (*s == '\n')
			break;
		if (*s == '\r' && s[1] == '\n')
			break;
	}
	return s - line;
}

/* search for string in line -- like strstr() */
char *
line_findstr(const char *line, const char *str)
{
	const char *lp, *sp;

	for (lp = line; *lp && *lp != '\n'; ++lp) {
		for (sp = str; *sp; ++sp)
			if (*(lp + (sp - str)) != *sp)
				break;
		if (!*sp)
			return (char *)lp;
	}
	return NULL;
}

/* print a line, excluding the newline */
void
line_fprint(FILE *out, const char *line)
{
	const char *s;

	for (s = line; *s && *s != '\n'; ++s)
		fputc(*s, out);
}

/* deep copy a list of numbered lines */
struct rcs_line *
lines_copy(const struct rcs_line *lines)
{
	const struct rcs_line *ln;
	struct rcs_line *copy, *head, **prev_next;

	prev_next = &head;
	for (ln = lines; ln; ln = ln->next) {
		copy = xmalloc(sizeof *copy, __func__);
		copy->lineno = ln->lineno;
		copy->line = xmalloc(ln->len + 1, __func__);
		memcpy(copy->line, ln->line, ln->len);
		copy->line[ln->len] = '\0';
		copy->line_allocated = true;
		copy->len = ln->len;
		copy->no_newline = ln->no_newline;
		*prev_next = copy;
		prev_next = &copy->next;
	}
	*prev_next = NULL;
	return head;
}

/* if line buffer is unallocated, allocate it and copy previous data */
void
line_allocate(struct rcs_line *line)
{
	char *lnbuf;

	if (line->line_allocated)
		return;

	lnbuf = xmalloc(line->len + 1, __func__);
	memcpy(lnbuf, line->line, line->len);
	lnbuf[line->len] = '\0';

	line->line = lnbuf;
	line->line_allocated = true;
}

/* insert lines into a list of numbered lines */
bool
lines_insert(struct rcs_line **lines, struct rcs_line *insert,
	unsigned int lineno, unsigned int count)
{
	struct rcs_line *ln, *addln, *nextln, **prev_next;
	unsigned int i;

	/* Find the insertion point */
	if (lineno) {
		for (ln = *lines; ln; ln = ln->next)
			if (ln->lineno >= lineno)
				break;
		if (!ln) {
			fprintf(stderr, "a%u %u: line %u missing\n", lineno,
				count, lineno);
			return false;
		}
		if (ln->lineno != lineno) {
			fprintf(stderr, "a%u %u: line %u missing, found %u\n",
				lineno, count, lineno, ln->lineno);
			return false;
		}
		prev_next = &ln->next;
	} else
		prev_next = lines;

	nextln = *prev_next;
	for (i = 0; i < count; ++i) {
		if (!insert) {
			fprintf(stderr, "a%u %u: missing insert line %u\n",
				lineno, count, i);
			return false;
		}
		addln = xcalloc(1, sizeof *addln, __func__);
		addln->lineno = 0; /* Added lines have no number */
		addln->line = insert->line;
		addln->line_allocated = false;
		addln->len = insert->len;

		addln->next = nextln;
		*prev_next = addln;
		prev_next = &addln->next;

		/*
		 * If inserting the last line at the very end of the buffer, do
		 * not add a newline if the inserted line lacks one.
		 */
		if (!addln->next && !insert->next)
			addln->no_newline = insert->no_newline;

		insert = insert->next;
	}
	return true;
}

/* delete lines from a list of numbered lines */
bool
lines_delete(struct rcs_line *lines, unsigned int lineno, unsigned int count)
{
	struct rcs_line *ln;
	unsigned int i;

	for (ln = lines; ln; ln = ln->next)
		if (ln->lineno >= lineno)
			break;
	for (i = 0; i < count; ++i) {
		if (!ln) {
			fprintf(stderr, "d%u %u: line %u missing\n", lineno,
				count, lineno + i);
			return false;
		}
		if (ln->lineno != lineno + i) {
			fprintf(stderr, "d%u %u: line %u missing, found %u\n",
				lineno, count, lineno + i, ln->lineno);
			return false;
		}
		if (ln->line) {
			/*
			 * Remove the line buffer, but leave the line number
			 * intact and keep the line in the list, as the original
			 * line number might be needed later in the patch.
			 */
			if (ln->line_allocated)
				free(ln->line);
			ln->line = NULL;
			ln->line_allocated = false;
		}
		ln = ln->next;
	}
	return true;
}
