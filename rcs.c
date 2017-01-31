#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "interfaces.h"

/* line from an RCS patch or file revision data */
struct line {
	struct line *next; /* next in linked list of lines */
	unsigned int lineno; /* RCS line number */
	char *line; /* Line string (terminated by newline, not NUL) */
};

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

/* convert a string into a list of numbered lines */
static struct line *
string_to_lines(char *str)
{
	unsigned int lineno;
	char *lnptr, *endln;
	size_t lnlen;
	struct line *head, **prev_next, *ln;

	head = NULL;
	prev_next = &head;

	lineno = 1;
	lnptr = str;
	for (;;) {
		for (endln = lnptr; *endln && *endln != '\n'; ++endln)
			;
		if (!*endln && endln == lnptr)
			break;
		ln = xmalloc(sizeof *ln, __func__);
		ln->next = NULL;
		ln->lineno = lineno;
		lnlen = endln - lnptr;
		ln->line = xmalloc(lnlen + 1, __func__);
		memcpy(ln->line, lnptr, lnlen);
		ln->line[lnlen] = '\0';

		*prev_next = ln;

		if (!*endln)
			break;

		prev_next = &ln->next;
		lineno++;
		lnptr = endln + 1;
	}
	return head;
}

/* reset a list of numbered lines */
static void
lines_reset(struct line **lines)
{
	struct line *ln, **prev_next;
	unsigned int lineno;

	/*
	 * Lines that have been deleted need to be removed.  Lines must also be
	 * renumbered to account for insertions and deletions.
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

/* copy a list of numbered lines */
static struct line *
lines_copy(struct line *lines)
{
	struct line *ln, *new, *head, **prev_next;

	prev_next = &head;
	for (ln = lines; ln; ln = ln->next) {
		new = xmalloc(sizeof *new, __func__);
		new->lineno = ln->lineno;
		new->line = xstrdup(ln->line, __func__);
		*prev_next = new;
		prev_next = &new->next;
	}
	*prev_next = NULL;
	return head;
}

/* free a list of numbered lines */
static void
lines_free(struct line *lines)
{
	struct line *ln, *nextln;

	for (ln = lines; ln; ln = nextln) {
		nextln = ln->next;
		if (ln->line) /* might be NULL if line was deleted */
			free(ln->line);
		free(ln);
	}
}

/* convert a list of numbered lines into a string */
static char *
lines_to_string(struct line *lines)
{
	size_t len;
	struct line *ln;
	char *str, *pos;

	/* add up the lengths of all lines */
	len = 1; /* +1 for NUL */
	for (ln = lines; ln; ln = ln->next)
		if (ln->line)
			len += strlen(ln->line) + 1; /* +1 for newline */

	str = xcalloc(1, len, __func__);
	pos = str;
	for (ln = lines; ln; ln = ln->next)
		if (ln->line) {
			len = strlen(ln->line);
			memcpy(pos, ln->line, len);
			pos += len;
			*pos++ = '\n';
		}
	*pos = '\0';

	return str;
}

/* insert lines into a list of numbered lines */
static bool
lines_insert(struct line **lines, struct line *insert, unsigned int lineno,
	unsigned int count)
{
	struct line *ln, *addln, *nextln, **prev_next;
	unsigned int i;

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
		addln->line = xstrdup(insert->line, __func__);
		addln->next = nextln;
		*prev_next = addln;
		prev_next = &addln->next;
		insert = insert->next;
	}
	return true;
}

/* delete lines from a list of numbered lines */
static bool
lines_delete(struct line *lines, unsigned int lineno, unsigned int count)
{
	struct line *ln;
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
			free(ln->line);
			ln->line = NULL;
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

	/* Expected format: number, space, number, NUL */

	errno = 0;
	*lineno = (unsigned int)strtoul(s, &end, 10);
	if (end == s || *end != ' ' || errno)
		return false;

	s = end + 1;
	errno = 0;
	*count = (unsigned int)strtoul(s, &end, 10);
	if (end == s || *end || errno)
		return false;

	return true;
}

/* patch the preceding revision to yield the new revision */
static struct line *
apply_patch(const struct rcs_file *file, const struct rcs_number *revnum,
	struct line *data_lines, struct line *patch_lines)
{
	struct line *pln;
	unsigned int ln, ct, i;
	char cmd;

	for (pln = patch_lines; pln;) {
		cmd = pln->line[0];
		if (cmd == '\0') {
			pln = pln->next;
			continue;
		}
		if (cmd != 'a' && cmd != 'd') {
			fprintf(stderr, "unrecognized patch command '%c' "
				"(0x%02x)\n", cmd, (unsigned int)cmd);
			goto error;
		}
		if (!get_lineno_and_count(&pln->line[1], &ln, &ct)) {
			fprintf(stderr, "cannot parse line number and count\n");
			goto error;
		}

		if (cmd == 'a') {
			if (!lines_insert(&data_lines, pln->next, ln, ct)) {
				fprintf(stderr, "cannot insert lines\n");
				goto error;
			}
			pln = pln->next;
			for (i = 0; i < ct; ++i)
				pln = pln->next;
		} else if (cmd == 'd') {
			if (!lines_delete(data_lines, ln, ct)) {
				fprintf(stderr, "cannot delete lines\n");
				goto error;
			}
			pln = pln->next;
		}
	}
	return data_lines;

error:
	fprintf(stderr, "cannot patch to \"%s\" rev. %s\n", file->name,
		rcs_number_string_sb(revnum));
	fprintf(stderr, "bad patch line %u: \"%s\"\n", pln->lineno, pln->line);
	fatal_error("bad RCS patch");
	return NULL; /* unreachable */
}

/* expand RCS escapes and keywords */
static void
rcs_data_expansion(const struct rcs_version *ver, const struct rcs_patch *patch,
	struct line *dlines)
{
	struct line *dl, *loghdr, *loglines, *ll;
	char *lp, *at, *kw, *lnbuf, revision[16 + RCS_MAX_REV_LEN];
	const char *revstr, *revdate;
	size_t prefixlen;

	/* convert double-@@ (an escaped @) into a single @ */
	for (dl = dlines; dl; dl = dl->next) {
		lp = dl->line;
		while ((at = strstr(lp, "@@"))) {
			memmove(at + 1, at + 2, strlen(at + 2) + 1);
			lp = at + 1;
		}
	}

	/* expand revision keyword */
	snprintf(revision, sizeof revision, "$Revision: %s $",
		rcs_number_string_sb(&ver->number));
	for (dl = dlines; dl; dl = dl->next) {
		kw = strcasestr(dl->line, "$Revision");
		if (!kw)
			continue;

		lp = kw + strlen("$Revision");
		for (; *lp && *lp != '$'; ++lp)
			;
		if (!*lp)
			continue;
		++lp;

		/* allocating more than needed rather than doing the math */
		lnbuf = xcalloc(1, strlen(dl->line) + strlen(revision) + 1,
			__func__);
		memcpy(lnbuf, dl->line, kw - dl->line);
		strcat(lnbuf, revision);
		strcat(lnbuf, lp);
		free(dl->line);
		dl->line = lnbuf;
	}

	/* expand log keyword */
	for (dl = dlines; dl; dl = dl->next) {
		kw = strcasestr(dl->line, "$Log");
		if (!kw)
			continue;

		lp = kw + strlen("$Log");
		for (; *lp && *lp != '$'; ++lp)
			;
		if (!*lp)
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
		revdate = time2string_mkssi(ver->date);
		loghdr->line = xcalloc(1, prefixlen + strlen("Revision ") +
			strlen(revstr) + strlen("  ") + strlen(revdate) +
			strlen("  ") + strlen(ver->author) + 1, __func__);
		memcpy(loghdr->line, dl->line, prefixlen);
		sprintf(&loghdr->line[prefixlen], "Revision %s  %s  %s", revstr,
			revdate, ver->author);

		/*
		 * Add lines for the check-in comment.  The prefix must be added
		 * to each of them.
		 */
		if (*patch->log) {
			loglines = string_to_lines(patch->log);
			for (ll = loglines; ll; ll = ll->next) {
				lnbuf = xcalloc(1, prefixlen + strlen(ll->line)
					+ 1, __func__);
				memcpy(lnbuf, dl->line, prefixlen);
				strcat(lnbuf, ll->line);
				free(ll->line);
				ll->line = lnbuf;
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
			loghdr->next = dl->next;
			dl->next = loghdr;
			dl = loghdr;
		}
	}
}

/* read a given revision from an RCS file */
char *
rcs_revision_read(struct rcs_file *file, const struct rcs_number *revnum)
{
	struct line *data_lines, *patch_lines;
	char *patch_text, *data_text;
	const struct rcs_version *ver;
	const struct rcs_patch *patch;
	const struct rcs_branch *b;
	struct rcs_number getrev, brrev;
	int cmp;

	/*
	 * A bit hacky, but these cache variables save a _lot_ of time when
	 * reading every revision of a large project.pj from newest to oldest.
	 */
	static const struct rcs_file *cache_file;
	static const struct rcs_version *cache_ver;
	static struct line *cache_data_lines;

	if (file == cache_file) {
		getrev = *revnum;

		if (rcs_number_is_trunk(revnum))
			rcs_number_increment(&getrev);
		else
			rcs_number_decrement(&getrev);

		if (rcs_number_equal(&getrev, &cache_ver->number)) {
			data_lines = cache_data_lines;
			cache_data_lines = NULL;
			getrev = *revnum;
			goto loop;
		}
	}

	data_lines = NULL;
	getrev = file->head;
loop:
	ver = rcs_file_find_version(file, &getrev, true);
	patch = rcs_file_find_patch(file, &getrev, true);
	if (data_lines) {
		patch_text = read_patch_text(file, patch);
		patch_lines = string_to_lines(patch_text);
		free(patch_text);
		data_lines = apply_patch(file, &ver->number, data_lines,
			patch_lines);
		lines_free(patch_lines);
		lines_reset(&data_lines);
	} else {
		patch_text = read_patch_text(file, patch);
		data_lines = string_to_lines(patch_text);
		free(patch_text);
	}

	if (rcs_number_equal(&getrev, revnum)) {
		cache_file = file;
		cache_ver = ver;
		if (cache_data_lines)
			lines_free(cache_data_lines);
		cache_data_lines = lines_copy(data_lines);

		rcs_data_expansion(ver, patch, data_lines);

		data_text = lines_to_string(data_lines);
		lines_free(data_lines);
		return data_text;
	}

	if (rcs_number_partial_match(revnum, &getrev)) {
		for (b = ver->branches; b; b = b->next) {
			/*
			 * We want a branchier rev like 1.145.1.82.1.1 to match
			 * a less branchy rev that leads to it like 1.145.1.1.
			 */
			brrev = *revnum;
			while (brrev.c > b->number.c)
				brrev.c -= 2;

			if (rcs_number_same_branch(&brrev, &b->number)) {
				getrev = b->number;
				goto loop;
			}
		}
	} else if (ver->parent.c) {
		/*
		 * Get the parent revision next as long as it brings us closer
		 * to the request revision.  Remember that trunk revisions are
		 * descending and branch revisions are ascending.
		 */
		cmp = rcs_number_compare(&ver->parent, &getrev);
		if (rcs_number_is_trunk(&ver->parent) ? cmp <= 0 : cmp >= 0) {
			getrev = ver->parent;
			goto loop;
		}
	}

	fatal_error("cannot find \"%s\" rev. %s", file->name,
		rcs_number_string_sb(revnum));

	return NULL; /* unreachable */
}

