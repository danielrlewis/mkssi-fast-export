/* Miscellaneous utility functions */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include "interfaces.h"

/* print error message, errno string (if errno != 0), and exit */
void
fatal_system_error(const char *fmt, ...)
{
	va_list args;
	int errno_save;

	errno_save = errno;

	fprintf(stderr, "fatal: ");
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	if (errno_save) {
		fprintf(stderr, ": ");
		errno = errno_save;
		perror(NULL);
	} else
		fprintf(stderr, "\n");
	exit(1);
}

/* print error message and exit */
void
fatal_error(const char *fmt, ...)
{
	va_list args;

	fprintf(stderr, "fatal: ");
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	exit(1);
}

/* append formatted content to realloc()'d buffer */
char *
sprintf_alloc_append(char *buf, const char *fmt, ...)
{
	va_list args;
	int oldlen, len;

	/* Determine how big the output buffer needs to be */
	oldlen = buf ? strlen(buf) : 0;
	va_start(args, fmt);
	len = oldlen + vsnprintf(NULL, 0, fmt, args) + 1; /* +1 for NUL */
	va_end(args);

	/* (Re)allocate the output buffer */
	buf = xrealloc(buf, len, __func__);

	/* Populate the output buffer */
	va_start(args, fmt);
	vsnprintf(buf + oldlen, len, fmt, args);
	va_end(args);

	return buf;
}

/* convert a time value to an MKSSI time string */
const char *
time2string(time_t date)
{
	static char timestr[23];
	struct tm *tm;

	tm = localtime(&date);
	(void)strftime(timestr, sizeof timestr, "%Y/%m/%d %H:%M:%SZ", tm);
	return timestr;
}

/* hash a string, case insensitive */
uint32_t
hash_string(const char *s)
{
	uint32_t hash;
	char c;

	/* The djb2 hash algorithm, invented by Dan Bernstein */
	hash = 5381;
	for (; *s; ++s) {
		c = isalpha(*s) ? tolower(*s) : *s;
		hash = (hash << 5) + hash + (uint32_t)c;
	}
	return hash;
}

/* is a character a hexadecimal digit */
bool
is_hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return true;
	if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
		return true;
	return false;
}

/* like malloc(), but abort on failure */
void *
xmalloc(size_t size, const char *legend)
{
	void *ret;

	if (!(ret = malloc(size)))
		fatal_system_error("Out of memory, malloc(%zu) failed in %s",
			size, legend);
	return ret;
}

/* like calloc(), but abort on failure */
void *
xcalloc(size_t nmemb, size_t size, const char *legend)
{
	void *ret;

	if (!(ret = calloc(nmemb, size)))
		fatal_system_error(
			"Out of memory, calloc(%zu, %zu) failed in %s",
			nmemb, size, legend);
	return ret;
}

/* like realloc(), but abort on failure */
void *
xrealloc(void *ptr, size_t size, const char *legend)
{
	void *ret;

	if (!(ret = realloc(ptr, size)))
		fatal_system_error("Out of memory, realloc(%zu) failed in %s",
			size, legend);
	return ret;
}

/* like strdup(), but abort on failure */
char *
xstrdup(const char *s, const char *legend)
{
	char *ret;

	if (!(ret = strdup(s)))
		fatal_system_error("Out of memory, strdup(\"%s\") failed in %s",
			s, legend);
	return ret;
}

/* sanitize a character from an MKSSI branch name; -1 return means skip char */
static int
sanitize_mkssi_branch_char(char c)
{
	const char badchars[] = "\\*?,:[";

	if (isspace(c))
		return '_';
	if (strchr(badchars, c) || !isgraph(c))
		return -1; /* Skip character */
	return c;
}

/* parse a character from MKSSI branch name (might be multibyte) */
size_t
parse_mkssi_branch_char(const char *s, int *cp)
{
	char ch, hexnum[3];
	long num;
	size_t len;

	ch = *s;
	len = 1;

	/*
	 * Characters other than letters, numbers, and underscores are allowed
	 * in MKSSI branch names, but they are encoded since the RCS format does
	 * not support them.  For example, space is "%20" and period is "%2E".
	 */
	if (ch == '%' && is_hex_digit(s[1]) && is_hex_digit(s[2])) {
		hexnum[0] = s[1];
		hexnum[1] = s[2];
		hexnum[2] = '\0';
		num = strtol(hexnum, NULL, 16);
		/*
		 * If the escape characters are non-ASCII, hard to say how they
		 * should be interpreted.  Unknown if these are legal in MKSSI.
		 * This code ignores such non-ASCII escape sequences, treating
		 * them as literal characters.
		 */
		if (num >= 0 && num <= 0x7f) {
			ch = (char)num;
			len = 3;
		}
	}

	/* Git disallows periods at the end of the refname */
	if (ch == '.' && !s[len])
		ch = '_';

	*cp = sanitize_mkssi_branch_char(ch);
	return len;
}

/* find a file version by revision number */
struct rcs_version *
rcs_file_find_version(const struct rcs_file *file,
	const struct rcs_number *revnum, bool fatalerr)
{
	struct rcs_version *v;

	for (v = file->versions; v; v = v->next)
		if (rcs_number_equal(&v->number, revnum))
			return v;
	if (!v && fatalerr)
		fatal_error("\"%s\" missing version for rev. %s",
			file->master_name,
			rcs_number_string_sb(revnum));
	return NULL;
}

/* find a file patch by revision number */
struct rcs_patch *
rcs_file_find_patch(const struct rcs_file *file,
	const struct rcs_number *revnum, bool fatalerr)
{
	struct rcs_patch *p;

	for (p = file->patches; p; p = p->next)
		if (rcs_number_equal(&p->number, revnum))
			return p;
	if (!p && fatalerr)
		fatal_error("\"%s\" missing patch for rev. %s",
			file->master_name,
			rcs_number_string_sb(revnum));
	return NULL;
}
