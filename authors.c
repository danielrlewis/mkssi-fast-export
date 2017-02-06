/*
 * Map MKSSI usernames (e.g., johns) to Git identities (e.g., "John Smith
 * <john.smith@example.org>").
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "interfaces.h"

/* map from an RCS author username to a Git author identity */
struct author_map {
	struct author_map *next;
	const char *rcs_author;
	struct git_author git_author;
};

static struct author_map *authors_unmapped_list;
static struct author_map *authors_mapped_list;

/* instantiate an author mapping */
static struct author_map *
new_author_map(const char *username, const char *name, const char *email)
{
	struct author_map *am;

	am = xcalloc(1, sizeof *am, __func__);
	am->rcs_author = username;
	am->git_author.name = name;
	am->git_author.email = email;
	return am;
}

/* create a record for a new unmapped author */
static const struct author_map *
new_unmapped_author(const char *author)
{
	struct author_map *am;

	/*
	 * Fake the Git author identification.  This allows higher-level code to
	 * use the git_author structure even if no mapping exists.
	 *
	 * Using the RCS author name for both name and email is what
	 * cvs-fast-export does when there is no author map.
	 */
	am = new_author_map(author, author, author);

	am->next = authors_unmapped_list;
	authors_unmapped_list = am;

	return am;
}

/* return the next line from the author map file */
static const char *
next_line(FILE *f, unsigned int lineno)
{
	static char line[1024]; /* big enough */
	char *pos;
	int c;

	for (pos = line; pos < &line[sizeof line]; ++pos) {
		c = fgetc(f);
		if (c == EOF) {
			if (pos == line)
				return NULL;
			break;
		}
		if (c == '\n')
			break;
		*pos = (char)c;
	}

	if (pos >= &line[sizeof line])
		fatal_error("author map file line %lu too long (max is %zu "
			"bytes)", lineno, sizeof line);

	*pos = '\0';
	return line;
}

/* clone a string from start (inclusive) to end (exclusive) */
static char *
strdup_range(const char *start, const char *end)
{
	size_t len;
	char *str;

	len = end - start;
	str = xmalloc(len + 1, __func__);
	memcpy(str, start, len);
	str[len] = '\0';
	return str;
}

/* parse a line from the author map file */
static struct author_map *
parse_author_map_line(const char *line, unsigned int lineno)
{
	const char *username_start, *username_end;
	const char *real_name_start, *real_name_end;
	const char *email_start, *email_end;

	/*
	 * Format is the same as cvs-fast-export:
	 *
	 *	ferd = Ferd J. Foonly <foonly@foo.com> America/Chicago
	 *
	 * The timezone is optional for cvs-fast-export.  For this program, it
	 * is ignored.
	 */

	/* Username starts at the beginning */
	username_start = line;

	/* Trim any leading whitespace */
	while (isspace(*username_start))
		++username_start;

	/* Username ends prior to equals sign */
	username_end = strchr(username_start, '=');
	if (!username_end || username_end == username_start)
		fatal_error("empty user name in author map file, line %u",
			lineno);

	/* Trim any white space between username and equals sign */
	--username_end;
	while (isspace(*username_end) && username_end > username_start)
		--username_end;

	if (username_end == username_start)
		fatal_error("empty user name in author map file, line %u",
			lineno);

	++username_end; /* make end pointer exclusive */

	/* Real name starts after equals sign */
	real_name_start = strchr(username_start, '=') + 1;

	/* Trim any white space between equals sign and name */
	while (isspace(*real_name_start))
		++real_name_start;

	/* Real name ends before email address */
	real_name_end = strchr(username_end, '<');
	if (!real_name_end)
		fatal_error("missing email in author map file, line %u",
			lineno);
	if (real_name_end > real_name_start)
		--real_name_end; /* Move back one character from '<' */

	/* Trim any white space between name and email address */
	while (isspace(*real_name_end) && real_name_end > real_name_start)
		--real_name_end;

	if (real_name_end == real_name_start)
		fatal_error("empty real name in author map file, line %u",
			lineno);

	++real_name_end; /* make end pointer exclusive */

	/* Email address is between '<' and '>' */
	email_start = strchr(real_name_end, '<') + 1;
	email_end = strchr(real_name_end, '>');
	if (!email_end)
		fatal_error("missing email in author map file, line %u",
			lineno);

	if (email_end == email_start)
		fatal_error("empty email in author map file, line %u", lineno);

	return new_author_map(
		strdup_range(username_start, username_end),
		strdup_range(real_name_start, real_name_end),
		strdup_range(email_start, email_end));
}

/* add a new author mapping to the list */
static void
add_author_mapping(struct author_map *am, unsigned int lineno)
{
	struct author_map *old;

	/* Check for duplicate mapping */
	for (old = authors_mapped_list; old; old = old->next) {
		if (strcasecmp(old->rcs_author, am->rcs_author))
			continue;

	 	/*
	 	 * Ignore a duplicate entry if the name and email are
	 	 * _exactly_ the same in both.
	 	 */
		if (!strcmp(old->git_author.name, am->git_author.name)
		 && !strcmp(old->git_author.email, am->git_author.email)) {
			free(am);
			am = NULL;
			break;
		}

		fprintf(stderr, "duplicate author mapping on line %u\n",
			lineno);
		fprintf(stderr, "original:  %s = %s <%s>\n", old->rcs_author,
			old->git_author.name, old->git_author.email);
		fprintf(stderr, "duplicate: %s = %s <%s>\n", am->rcs_author,
			am->git_author.name, am->git_author.email);
		fatal_error("duplicate in author map");
	}

	/* If this was not a duplicate... */
	if (am) {
		/* Prepend mapped author to front of list */
		am->next = authors_mapped_list;
		authors_mapped_list = am;
	}
}

/* initialize the author map from a user-supplied file */
void
author_map_initialize(const char *author_map_path)
{
	FILE *f;
	const char *line;
	unsigned int lineno;

	if (!(f = fopen(author_map_path, "r")))
		fatal_system_error("cannot open author map file at \"%s\"",
			author_map_path);

	for (lineno = 1; (line = next_line(f, lineno)); ++lineno) {
		/*
		 * Mimic cvs-fast-export: "Lines beginning with a # or not
		 * containing an equals sign are silently ignored."
		 */
		if (*line == '#' || !strchr(line, '='))
			continue;

		/* Parse the line and add it to the list */
		add_author_mapping(parse_author_map_line(line, lineno), lineno);
	}

	fclose(f);
}

/* map an RCS author to a Git author */
const struct git_author *
author_map(const char *author)
{
	struct author_map *am;

	/* Check the list of properly mapped authors */
	for (am = authors_mapped_list; am; am = am->next)
		if (!strcasecmp(am->rcs_author, author))
			return &am->git_author;

	/* Check the list of unmapped authors */
	for (am = authors_unmapped_list; am; am = am->next)
		if (!strcasecmp(am->rcs_author, author))
			return &am->git_author;

	/* Add a new unmapped author */
	return &new_unmapped_author(author)->git_author;
}

/* dump a list of unmapped authors to stdout; useful for creating author map */
void
dump_unmapped_authors(void)
{
	const struct rcs_file *f;
	const struct rcs_version *v;
	struct author_map *am;
	const char *s;
	char c;

	/*
	 * Ignore the return value from author_map(); we are calling it for the
	 * side effect of building a list of unmapped authors.
	 */
	for (f = files; f; f = f->next)
		for (v = f->versions; v; v = v->next)
			author_map(v->author);
	for (v = project->versions; v; v = v->next)
		author_map(v->author);

	for (am = authors_unmapped_list; am; am = am->next) {
		for (s = am->rcs_author; *s; ++s) {
			/*
			 * MKSSI authors are case-insensitive; in the same
			 * repository, an author can be upper-case, lower-case,
			 * and mixed-case.  Output the name in lower-case so
			 * that is not random which variant is listed.
			 */
			c = isalpha(*s) ? tolower(*s) : *s;
			putchar(c);
		}
		putchar('\n');
	}
}
