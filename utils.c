/*
 * Copyright (c) 2017, 2019-2020 Datalight, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Miscellaneous utility functions.
 */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
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

/* get a parent directory path from a path */
char *
path_parent_dir(const char *path)
{
	const char *name;
	size_t len;
	char *dir_path;

	/* For example: "a/b/c" yields "a/b", "a" yields "" */

	/* Determine the length of the path, sans the final component */
	name = path_to_name(path);
	len = name - path;
	if (len)
		len--; /* Don't include path separator. */

	/* Create a path buffer for the parent directory */
	dir_path = xmalloc(len + 1, __func__);
	memcpy(dir_path, path, len);
	dir_path[len] = '\0';

	return dir_path;
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

/* buffer a file */
unsigned char *
file_buffer(const char *path, size_t *size)
{
	struct stat info;
	unsigned char *fdata;
	FILE *f;

	if (stat(path, &info))
		fatal_system_error("cannot stat file at \"%s\"", path);
	if (!S_ISREG(info.st_mode))
		fatal_error("not a regular file: \"%s\"", path);

	*size = info.st_size;

	if (!(f = fopen(path, "r")))
		fatal_system_error("cannot open file at \"%s\"", path);

	/*
	 * The +1 is for file_as_string(), which uses this function to read the
	 * file data but wants an extra byte for a NUL terminator.
	 */
	fdata = xmalloc(info.st_size + 1, __func__);

	errno = 0;
	if (fread(fdata, 1, info.st_size, f) != info.st_size)
		fatal_system_error("cannot read from \"%s\"", path);

	fclose(f);

	return fdata;
}

/* get allocated string buffer containing the entire contents of a file */
char *
file_as_string(const char *path)
{
	char *fdata;
	size_t size;

	fdata = (char *)file_buffer(path, &size);

	/*
	 * NUL-terminate the string.  file_buffer() allocated an extra byte in
	 * the buffer so that it's safe to do this.
	 */
	fdata[size] = '\0';

	return fdata;
}

/* get the mtime (time of last modification) of a file */
time_t
file_mtime(const char *path)
{
	struct stat info;

	if (stat(path, &info))
		fatal_system_error("cannot stat \"%s\"", path);

	return info.st_mtime;
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

	if (file->dummy)
		fatal_error("internal error: version search within dummy "
			"file \"%s\"", file->name);

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

	if (file->dummy)
		fatal_error("internal error: patch search within dummy "
			"file \"%s\"", file->name);

	for (p = file->patches; p; p = p->next)
		if (rcs_number_equal(&p->number, revnum))
			return p;
	if (!p && fatalerr)
		fatal_error("\"%s\" missing patch for rev. %s",
			file->master_name,
			rcs_number_string_sb(revnum));
	return NULL;
}

/* return list of directories in a path */
struct dir_path *
dir_list_from_path(const char *path)
{
	struct dir_path *dir, *head, **prev_next;
	const char *pos;

	/*
	 * For example, if path is "a/b/c/foo.txt", the returned list will be
	 * "a/", "a/b/", and "a/b/c/".
	 */
	prev_next = &head;
	for (pos = path; *pos; ++pos) {
		if (*pos != '/')
			continue;
		dir = xmalloc(sizeof *dir, __func__);
		dir->path = path;
		dir->len = pos - path + 1;
		*prev_next = dir;
		prev_next = &dir->next;
	}
	*prev_next = NULL;
	return head;
}

/* remove from the new list anything which is listed in the old list */
struct dir_path *
dir_list_remove_duplicates(struct dir_path *new_list,
	const struct dir_path *old_list)
{
	const struct dir_path *o;
	struct dir_path *n, **prev_next;

	prev_next = &new_list;
	for (n = new_list; n; n = *prev_next) {
		for (o = old_list; o; o = o->next)
			if (n->len == o->len
			 && !strncasecmp(n->path, o->path, n->len))
				break;
		if (o) {
			*prev_next = n->next;
			free(n);
		} else
			prev_next = &n->next;
	}
	return new_list;
}

/* append a new list of directories to an old list */
struct dir_path *
dir_list_append(struct dir_path *old_list, struct dir_path *new_list)
{
	struct dir_path *d, **prev_next;

	prev_next = &old_list;
	for (d = old_list; d; d = d->next)
		prev_next = &d->next;
	*prev_next = new_list;
	return old_list;
}

/* free a list of directories */
void
dir_list_free(struct dir_path *list)
{
	struct dir_path *d, *dnext;

	for (d = list; d; d = dnext) {
		dnext = d->next;
		free(d);
	}
}
