/* Read revision data from plain-text MKSSI RCS files */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "interfaces.h"

/* line from an RCS patch or file revision data */
struct rcs_line {
	/* Next in linked list of lines */
	struct rcs_line *next;

	/*
	 * _Original_ RCS line number.  Does not change while a patch is being
	 * applied; only updated after the whole patch is applied.  This is
	 * because the RCS patch line numbers refer to the previous unpatched
	 * version of the data.  While a patch is being applied, inserted lines
	 * have no line number.
	 */
	unsigned int lineno;

	/*
	 * Pointer to the line; terminated by newline _or_ NUL.  If
	 * line_allocated is false, this is pointing into another buffer and
	 * must _not_ be modified or passed to free().  May be NULL for deleted
	 * lines while a patch is being applied.
	 */
	char *line;

	/*
	 * Whether the line buffer is an independently allocated buffer.  The
	 * line cannot be modified unless this is true.
	 */
	bool line_allocated;

	/* Length of the line, excluding terminating newline/NUL */
	size_t len;
};

/* buffer an RCS patch in a structured list of such patches */
struct rcs_patch_buffer {
	/*
	 * Parent patch.  This is the subsequent revision whose contents are
	 * derived from this revision.  For trunk revisions, this is a lower
	 * revision number; for branch revisions, a higher one.
	 */
	struct rcs_patch_buffer *parent;

	/*
	 * List of branches based on this revision.  For example, say this is
	 * the patch for revision 1.2.  branches would point at the first
	 * branch, say 1.2.1.1; that branch would have parent pointers leading
	 * to 1.2.1.2, 1.2.1.3, etc.  If there is more than one branch based
	 * on revision 1.2, the branch_next will be used.  So for revision 1.2,
	 * its branches->branch_next might lead to rev. 1.2.2.1 (which might
	 * also have its own parent revisions).
	 */
	struct rcs_patch_buffer *branches;
	struct rcs_patch_buffer *branch_next;

	/* RCS version and patch structures (for convenience) */
	const struct rcs_version *ver;
	const struct rcs_patch *patch;

	/* Raw text of the patch.  The lines buffer points into this. */
	char *text;

	/* Lines buffer for the patch */
	struct rcs_line *lines;
};

/* find the length of a line string (newline or NUL terminated) */
static size_t
line_length(const char *line)
{
	const char *s;

	for (s = line; *s && *s != '\n'; ++s)
		;
	return s - line;
}

/* case-insensitive search for string in line -- like strcasestr() */
static char *
line_findstrcase(const char *line, const char *str)
{
	const char *lp, *sp;
	char lc, sc;

	for (lp = line; *lp && *lp != '\n'; ++lp) {
		for (sp = str; *sp; ++sp) {
			lc = *(lp + (sp - str));
			sc = *sp;

			if (isalpha(lc))
				lc = tolower(lc);
			if (isalpha(sc))
				sc = tolower(sc);

			if (lc != sc)
				break;
		}
		if (!*sp)
			return (char *)lp;
	}
	return NULL;
}

/* print a line, excluding the newline */
static void
line_fprint(FILE *out, const char *line)
{
	const char *s;

	for (s = line; *s && *s != '\n'; ++s)
		fputc(*s, out);
}

/* convert a string into a list of numbered lines */
static struct rcs_line *
string_to_lines(char *str)
{
	unsigned int lineno;
	struct rcs_line *head, **prev_next, *ln;
	char *lnptr;

	lineno = 1;
	prev_next = &head;
	for (lnptr = str; *lnptr;) {
		ln = xmalloc(sizeof *ln, __func__);
		ln->lineno = lineno;
		ln->line = lnptr;
		ln->line_allocated = false;
		ln->len = line_length(ln->line);

		*prev_next = ln;
		prev_next = &ln->next;
		lineno++;

		lnptr += ln->len;
		if (*lnptr == '\n')
			++lnptr;
	}
	*prev_next = NULL;
	return head;
}

/* convert a list of numbered lines into a string */
static char *
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
			*pos++ = '\n';
		}
	*pos = '\0';

	return str;
}

/* free a list of numbered lines */
static void
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
static void
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

/* deep copy a list of numbered lines */
static struct rcs_line *
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
		*prev_next = copy;
		prev_next = &copy->next;
	}
	*prev_next = NULL;
	return head;
}

/* if line buffer is unallocated, allocate it and copy previous data */
static void
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
static bool
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
		addln = xmalloc(sizeof *addln, __func__);
		addln->lineno = 0; /* Added lines have no number */
		addln->line = insert->line;
		addln->line_allocated = false;
		addln->len = insert->len;

		addln->next = nextln;
		*prev_next = addln;
		prev_next = &addln->next;
		insert = insert->next;
	}
	return true;
}

/* delete lines from a list of numbered lines */
static bool
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

/* parse line number and line count from an RCS patch line */
static bool
get_lineno_and_count(const char *s, unsigned int *lineno, unsigned int *count)
{
	char *end;

	/* Expected format: number, space, number, newline/NUL */

	errno = 0;
	*lineno = (unsigned int)strtoul(s, &end, 10);
	if (end == s || *end != ' ' || errno)
		return false;

	s = end + 1;
	errno = 0;
	*count = (unsigned int)strtoul(s, &end, 10);
	if (end == s || (*end && *end != '\n') || errno)
		return false;

	return true;
}

/* patch the preceding revision to yield the new revision */
static struct rcs_line *
apply_patch(const struct rcs_file *file, const struct rcs_number *revnum,
	struct rcs_line *data_lines, struct rcs_line *patch_lines)
{
	struct rcs_line *pln;
	unsigned int ln, ct, i;
	char cmd;

	for (pln = patch_lines; pln;) {
		cmd = pln->line[0];

		/* Skip blank lines */
		if (cmd == '\n' || cmd == '\0') {
			pln = pln->next;
			continue;
		}

		/*
		 * RCS patches only have two commands: 'a' for insert and 'd'
		 * for delete.
		 */
		if (cmd != 'a' && cmd != 'd') {
			fprintf(stderr, "unrecognized patch command '%c' "
				"(0x%02x)\n", cmd, (unsigned int)cmd);
			goto error;
		}

		/*
		 * Both 'a' and 'd' have a line number and line count.  The line
		 * number is where the insert/delete starts.  Importantly, this
		 * line number is the *original* line number, prior to applying
		 * any changes from the patch.  The line count is the number of
		 * lines to insert/delete.
		 */
		if (!get_lineno_and_count(&pln->line[1], &ln, &ct)) {
			fprintf(stderr, "cannot parse line number and count\n");
			goto error;
		}

		if (cmd == 'a') {
			if (!lines_insert(&data_lines, pln->next, ln, ct)) {
				fprintf(stderr, "cannot insert lines\n");
				goto error;
			}

			/* Move past insert command */
			pln = pln->next;

			/* Move past inserted lines */
			for (i = 0; i < ct; ++i)
				pln = pln->next;
		} else if (cmd == 'd') {
			if (!lines_delete(data_lines, ln, ct)) {
				fprintf(stderr, "cannot delete lines\n");
				goto error;
			}

			/* Move past delete command */
			pln = pln->next;
		}
	}

	/*
	 * Once the patch is completely applied, we can remove deleted lines
	 * from the line buffer and renumber the lines.
	 */
	lines_reset(&data_lines);
	return data_lines;

error:
	fprintf(stderr, "cannot patch to \"%s\" rev. %s\n", file->name,
		rcs_number_string_sb(revnum));
	fprintf(stderr, "bad patch line %u: \"", pln->lineno);
	line_fprint(stderr, pln->line);
	fprintf(stderr, "\"\n");
	fatal_error("bad RCS patch");
	return NULL; /* unreachable */
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
		if (line_findstrcase(dl->line, "@@"))
			line_allocate(dl);

		/*
		 * Every time we see an "@@", shift the line contents to the
		 * left to squash the 2nd '@'.
		 */
		lp = dl->line;
		while ((at = line_findstrcase(lp, "@@"))) {
			memmove(at + 1, at + 2, line_length(at + 2) + 1);
			lp = at + 1;
			dl->len--;
		}
	}
}

/* insert revision number at "$Revision$" keyword */
static void
rcs_data_expand_revision_keyword(const struct rcs_version *ver,
	struct rcs_line *dlines)
{
	struct rcs_line *dl;
	char *lp, *kw, *lnbuf, *pos, revision[16 + RCS_MAX_REV_LEN];
	size_t len, before_revstr_len, revstr_len, after_revstr_len;

	snprintf(revision, sizeof revision, "$Revision: %s $",
		rcs_number_string_sb(&ver->number));
	for (dl = dlines; dl; dl = dl->next) {
		/* Look for the $Revision$ keyword on this line */
		kw = line_findstrcase(dl->line, "$Revision");
		if (!kw)
			continue;

		/*
		 * Before the closing '$', there can be other characters, such
		 * as a space or another revision number.
		 */
		lp = kw + strlen("$Revision");
		for (; *lp && *lp != '\n' && *lp != '$'; ++lp)
			;

		/* If no closing '$' was found, then not a revision keyword */
		if (*lp != '$')
			continue;

		++lp; /* Move past the '$' */

		/* Find lengths of the updated line and its components */
		before_revstr_len = kw - dl->line;
		revstr_len = strlen(revision);
		after_revstr_len = line_length(lp);
		len = before_revstr_len + revstr_len + after_revstr_len;

		/* Allocate and populate the updated line */
		lnbuf = pos = xmalloc(len + 1, __func__);
		memcpy(pos, dl->line, before_revstr_len);
		pos += before_revstr_len;
		memcpy(pos, revision, revstr_len);
		pos += revstr_len;
		memcpy(pos, lp, after_revstr_len);
		pos += after_revstr_len;
		*pos = '\0';

		/* Replace the original line with the updated line */
		if (dl->line_allocated)
			free(dl->line);
		dl->line = lnbuf;
		dl->line_allocated = true;
		dl->len = len;
	}
}

/* insert revision history comment after "$Log$" keyword */
static void
rcs_data_expand_log_keyword(const struct rcs_version *ver,
	const struct rcs_patch *patch, struct rcs_line *dlines)
{
	struct rcs_line *dl, *loghdr, *loglines, *ll;
	char *lp, *kw, *lnbuf;
	const char *revstr, *revdate;
	size_t prefixlen;

	for (dl = dlines; dl; dl = dl->next) {
		/* Look for the $Log$ keyword on this line */
		kw = line_findstrcase(dl->line, "$Log");
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

		/*
		 * Any white space or comment characters preceding the log
		 * keyword need to be included on the log lines.
		 */
		prefixlen = kw - dl->line;

		/*
		 * Generate the first line of the log message from the metadata.
		 * An example of what this might look like:
		 *
		 * 	Revision 1.8  2012/12/11 23:45:55Z  daniel.lewis
		 */
		loghdr = xcalloc(1, sizeof *loghdr, __func__);
		revstr = rcs_number_string_sb(&ver->number);
		revdate = time2string(ver->date);
		loghdr->len = prefixlen + strlen("Revision ") +
			strlen(revstr) + strlen("  ") + strlen(revdate) +
			strlen("  ") + strlen(ver->author);
		loghdr->line = xmalloc(loghdr->len + 1, __func__);
		loghdr->line_allocated = true;
		memcpy(loghdr->line, dl->line, prefixlen);
		sprintf(&loghdr->line[prefixlen], "Revision %s  %s  %s", revstr,
			revdate, ver->author);

		/*
		 * Add lines for the check-in comment.  The prefix must be added
		 * to each of them.
		 */
		if (*patch->log) {
			/* Break the log (check-in comment) text into lines */
			loglines = string_to_lines(patch->log);

			/* Prepend the prefix onto each log line */
			for (ll = loglines; ll; ll = ll->next) {
				/* lnbuf = prefix + ll->line */
				lnbuf = xmalloc(prefixlen + ll->len + 1,
					__func__);
				memcpy(lnbuf, dl->line, prefixlen);
				memcpy(&lnbuf[prefixlen], ll->line, ll->len);
				lnbuf[prefixlen + ll->len] = '\0';

				/*
				 * Replace original log line with the version
				 * containing the prefix.
				 */
				if (ll->line_allocated)
					free(ll->line);
				ll->line = lnbuf;
				ll->line_allocated = true;
				ll->len = prefixlen + ll->len;
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

/* expand RCS escapes and keywords */
static void
rcs_data_keyword_expansion(const struct rcs_version *ver,
	const struct rcs_patch *patch, struct rcs_line *dlines)
{
	rcs_data_unescape_ats(dlines);
	rcs_data_expand_revision_keyword(ver, dlines);
	rcs_data_expand_log_keyword(ver, patch, dlines);
}

/* read the text of an RCS patch from disk */
static char *
read_patch_text(const struct rcs_file *file, const struct rcs_patch *patch)
{
	ssize_t len;
	char *text;
	int fd;

	/*
	 * patch->text.length includes the opening/closing @ characters, which
	 * we do not want to read.
	 */
	len = patch->text.length - 2;
	text = xmalloc(len + 1, __func__);

	if ((fd = open(file->master_name, O_RDONLY)) == -1)
		fatal_system_error("cannot open \"%s\"", file->master_name);

	errno = 0;
	if (pread(fd, text, len, patch->text.offset + 1) != len)
		fatal_system_error("cannot read from \"%s\"",
			file->master_name);

	close(fd);

	text[len] = '\0';
	return text;
}

/* instantiate a patch buffer */
static struct rcs_patch_buffer *new_patch_buf(const struct rcs_file *file,
	const struct rcs_number *revnum)
{
	struct rcs_patch_buffer *pbuf;

	pbuf = xcalloc(1, sizeof *pbuf, __func__);
	pbuf->ver = rcs_file_find_version(file, revnum, true);
	pbuf->patch = rcs_file_find_patch(file, revnum, true);
	pbuf->text = read_patch_text(file, pbuf->patch);
	pbuf->lines = string_to_lines(pbuf->text);
	return pbuf;
}

/* read a file's patches from a given starting revision into patch buffers */
static struct rcs_patch_buffer *
read_patches_from_rev(const struct rcs_file *file,
	const struct rcs_number *startrev)
{
	struct rcs_number rev;
	struct rcs_patch_buffer *head, *pbuf, *br_pbuf;
	struct rcs_patch_buffer **parent_prev_next, **br_prev_next;
	const struct rcs_branch *b;

	parent_prev_next = &head;
	for (rev = *startrev; rev.c; rev = pbuf->ver->parent) {
		/* Read this patch into a patch buffer */
		pbuf = new_patch_buf(file, &rev);

		/* Recursively read any branch patch chains which start here */
		br_prev_next = &pbuf->branches;
		for (b = pbuf->ver->branches; b; b = b->next) {
			br_pbuf = read_patches_from_rev(file, &b->number);
			*br_prev_next = br_pbuf;
			br_prev_next = &br_pbuf->branch_next;
		}
		*br_prev_next = NULL;

		*parent_prev_next = pbuf;
		parent_prev_next = &pbuf->parent;
	}
	*parent_prev_next = NULL;
	return head;
}

/* read all of a file's patches into a list of patch buffers */
static struct rcs_patch_buffer *
read_patches(const struct rcs_file *file)
{
	/* Start reading from the head revision */
	return read_patches_from_rev(file, &file->head);
}

/* free a list of patch buffers */
static void
free_patch_buffers(struct rcs_patch_buffer *patches)
{
	struct rcs_patch_buffer *p, *pparent, *bp, *bpnext;

	/* Walk the list of revisions */
	for (p = patches; p; p = pparent) {
		/* Recursively free each branch which starts here */
		for (bp = p->branches; bp; bp = bpnext) {
			bpnext = bp->branch_next;
			free_patch_buffers(bp);
		}

		/* Free the patch text and lines */
		lines_free(p->lines);
		free(p->text);

		pparent = p->parent;
		free(p);
	}
}

/* pass file revision data to the callback */
static void
emit_revision(rcs_revision_data_handler_t *callback,
	struct rcs_file *file, const struct rcs_version *ver,
	const struct rcs_patch *patch, const struct rcs_line *data_lines)
{
	struct rcs_line *data_lines_expanded;
	char *data;

	/*
	 * Need to do RCS keyword expansion.  The provided data_lines may still
	 * be needed in their original form to patch to the subsequent revision,
	 * so make a copy for the expansion.
	 */
	data_lines_expanded = lines_copy(data_lines);
	rcs_data_keyword_expansion(ver, patch, data_lines_expanded);

	/* Convert the data lines into a string and pass to the callback */
	data = lines_to_string(data_lines_expanded);
	callback(file, &ver->number, data);
	free(data);

	/* Free the copied data lines */
	lines_free(data_lines_expanded);
}

/* apply patches and pass the resulting revision data to the callback */
static struct rcs_line *
apply_patches_and_emit(rcs_revision_data_handler_t *callback,
	struct rcs_file *file, struct rcs_line *prev_data_lines,
	struct rcs_patch_buffer *patches)
{
	struct rcs_patch_buffer *p, *bp;
	struct rcs_line *branch_data_lines, *data_lines;

	data_lines = NULL;

	for (p = patches; p; p = p->parent) {
		if (prev_data_lines)
			/*
			 * Apply the patch to transmute the previous revision
			 * data lines into the data lines for the current
			 * revision.
			 */
			data_lines = apply_patch(file, &p->ver->number,
				prev_data_lines, p->lines);
		else
			/*
			 * Patch for the head revision is the data for that
			 * revision.
			 */
			data_lines = p->lines;

		/* Pass the revision data to the callback */
		emit_revision(callback, file, p->ver, p->patch, data_lines);

		/* Iterate through all branches which start at this revision */
		for (bp = p->branches; bp; bp = bp->branch_next) {
			/*
			 * Branch patches apply against data_lines, but since
			 * we still need that data for subsequent revisions on
			 * this level, make a copy.
			 */
			branch_data_lines = lines_copy(data_lines);

			/*
			 * Recursively apply patches and emit revision data for
			 * this chain of branch patches.
			 */
			branch_data_lines = apply_patches_and_emit(callback,
				file, branch_data_lines, bp);

			/* Free the copied data lines */
			lines_free(branch_data_lines);
		}

		prev_data_lines = data_lines;
	}

	/*
	 * data_lines will have changed from prev_data_lines if a) the latter
	 * was NULL; or b) if applying a patch changed the first line.  Return
	 * the possibly changed pointer so that it can be freed correctly.
	 */
	return data_lines;
}

/* read every RCS revision for a file, passing the data to the callback */
void
rcs_file_read_all_revisions(struct rcs_file *file,
	rcs_revision_data_handler_t *callback)
{
	struct rcs_patch_buffer *patches;

	/*
	 * Read every patch.  These must remain in memory until we are done
	 * with the file, since portions of a patch (the inserted lines) are
	 * incorporated into the text of subsequent revisions.
	 */
	patches = read_patches(file);

	/*
	 * Apply the patches in sequence and emit the resulting revision data
	 * to the callback.
	 */
	patches->lines = apply_patches_and_emit(callback, file, NULL, patches);

	/* Free the patch buffers */
	free_patch_buffers(patches);
}
