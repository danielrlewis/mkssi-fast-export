/* Lex/Flex parser for MKSSI-style RCS files */
%{
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <ctype.h>
#include "interfaces.h"
#include "gram.h"

static char *parse_data(yyscan_t scanner);
static void parse_text(struct rcs_text *text, yyscan_t scanner,
	struct rcs_file *file);
static void fast_export_sanitize(yyscan_t scanner, struct rcs_file *file);

#define YY_INPUT(buf, result, max_size) { \
	int c = getc(yyget_in(yyscanner)); \
	result = (c == EOF) ? YY_NULL : (buf[0] = c, 1); \
}

YY_DECL;
%}
%option reentrant bison-bridge
%option warn nodefault
%option pointer
%option noyywrap noyyget_extra noyyget_leng noyyset_lineno
%option noyyget_out noyyset_out noyyget_lval noyyset_lval
%option noyyget_lloc noyyset_lloc noyyget_debug noyyset_debug

%s CONTENT SYMBOLSS SKIP AUTHORSS STORAGES FORMATS
%%
<INITIAL>head BEGIN(CONTENT); return HEAD;
<INITIAL>branch BEGIN(CONTENT); return BRANCH;
<INITIAL>access return ACCESS;
<INITIAL>symbols BEGIN(SYMBOLSS); return SYMBOLS;
<INITIAL>locks BEGIN(CONTENT); return LOCKS;
<INITIAL>storage BEGIN(STORAGES); return STORAGE;
<INITIAL>comment BEGIN(CONTENT); return COMMENT;
<INITIAL>date BEGIN(CONTENT); return DATE;
<INITIAL>branches BEGIN(CONTENT); return BRANCHES;
<INITIAL>next BEGIN(CONTENT); return NEXT;
<INITIAL>strict BEGIN(CONTENT); return STRICT;
<INITIAL>author BEGIN(AUTHORSS); return AUTHOR;
<INITIAL>state BEGIN(CONTENT); return STATE;
<INITIAL>desc return DESC;
<INITIAL>log return LOG;
<INITIAL>reference BEGIN(SKIP); return REFERENCE;
<INITIAL>ext return EXTENSION;
<INITIAL>format BEGIN(FORMATS); return FORMAT;
<INITIAL>text BEGIN(SKIP); return TEXT;
<SKIP>@ {
		parse_text(&yylval->text, yyscanner, file);
		BEGIN(INITIAL);
		return TEXT_DATA;
	}
<CONTENT>[-a-zA-Z_+%][-a-zA-Z_0-9+/%=.~^\\*?#!\[\]()<>]* {
		fast_export_sanitize(yyscanner, file);
		yylval->s = xstrdup(yytext, "lex.l:CONTENT");
		return TOKEN;
	}
<AUTHORSS>[-a-zA-Z_0-9+%][-a-zA-Z_0-9+/%=.~^\\*?]* {
		fast_export_sanitize(yyscanner, file);
		yylval->s = xstrdup(yytext, "lex.l:AUTHORSS");
		return TOKEN;
	}
<STORAGES>reference BEGIN(CONTENT); return REFERENCE;
<FORMATS>binary BEGIN(INITIAL); return FMTBINARY;
<FORMATS>text BEGIN(INITIAL); return FMTTEXT;
[0-9]+\.[0-9.]* {
		yylval->number = lex_number(yytext);
		return NUMBER;
	}
<SYMBOLSS>[-a-zA-Z_0-9+/%=.~^\\*?#!\[\]()<>\*&]+ {
		fast_export_sanitize(yyscanner, file);
		yylval->s = xstrdup(yytext, "lex.l:SYMBOLSS");
		return TOKEN;
	}
; BEGIN(INITIAL); return SEMI;
: return COLON;
<INITIAL,CONTENT>@ {
		yylval->s = parse_data(yyscanner);
		return DATA;
	}
" " ;
\t ;
\n ;
<INITIAL>. {
		fprintf(stderr, "%s:%d unrecognized input %c\n",
			file->master_name, yylineno, yytext[0]);
		return CORRUPT;
	}
. {
		fatal_error("%s:%d unrecognized input %c\n", file->master_name,
			yylineno, yytext[0]);
	}
%%

/*
 * A variable-length buffer, allocated on the stack first but can grow to use
 * the heap.
 */
struct varbuf {
	unsigned int max, cur;
	char *string;
	char buf[1024];
};

/* initialize a variable-length string buffer */
static void
varbuf_init(struct varbuf *buf)
{
	buf->max = sizeof buf->buf;
	buf->cur = 0;
	buf->string = buf->buf;
}

/* append a character to a variable length string buffer */
static void
varbuf_add(struct varbuf *buf, char c)
{
	if (buf->cur == buf->max) {
		if (buf->string == buf->buf) {
			buf->max *= 2;
			buf->string = xmalloc(buf->max, __func__);
			memcpy(buf->string, buf->buf, buf->cur);
		} else {
			buf->max *= 2;
			buf->string = xrealloc(buf->string, buf->max, __func__);
		}
	}
	buf->string[buf->cur++] = c;
}

/* free a variable length string buffer */
static void
varbuf_free(struct varbuf *buf)
{
	if (buf->string != buf->buf)
		free(buf->string);
}

/* copy the string from a variable-length string buffer */
static char *
varbuf_dup(const struct varbuf *buf, const char *legend)
{
	char *dup;

	dup = xmalloc(buf->cur, legend);
	memcpy(dup, buf->string, buf->cur);
	return dup;
}

/* parse textual data from an RCS metadata field */
static char *
parse_data(yyscan_t yyscanner)
{
	int c;
	char *ret;
	struct varbuf buf;

	varbuf_init(&buf);

	for (;;) {
		c = getc(yyget_in(yyscanner));
		if (c == '@') {
			c = getc(yyget_in(yyscanner));
			if (c != '@')
				break;
		}
		varbuf_add(&buf, c);
	}
	ungetc(c, yyget_in(yyscanner));

	varbuf_add(&buf, '\0');
	ret = varbuf_dup(&buf, __func__);
	varbuf_free(&buf);

	return ret;
}

/* parse a "text" block from an RCS file */
static void
parse_text(struct rcs_text *text, yyscan_t yyscanner, struct rcs_file *file)
{
	int c;
	size_t length;

	text->offset = ftell(yyget_in(yyscanner)) - 1;
	length = 1;

	while ((c = getc(yyget_in(yyscanner))) != EOF) {
		++length;
		if (c == '@') {
			/* lookahead to see if we hit @@ */
			c = getc(yyget_in(yyscanner));
			if (c == '@') {
				++length;
			} else {
				/*
				 * We consume only the closing single @, leaving
				 * it included in the length
				 */
				ungetc(c, yyget_in(yyscanner));
				break;
			}
		}
	}
	text->length = length;
}

/* parse a number with multiple fields separated by periods (like "1.7.1.42") */
struct rcs_number
lex_number(const char *s)
{
	struct rcs_number n;
	const char *next;

	n.c = 0;
	while (*s) {
		n.n[n.c] = (short)strtol(s, (char **)&next, 10);
		if (next == s)
			break;
		if (*next == '.')
			next++;
		s = next;
		if (n.c > RCS_MAX_DEPTH)
			fatal_error(
				"revision too long, increase RCS_MAX_DEPTH");
		n.c++;
	}
	return n;
}

/* convert date/time fields into Unix time and time string */
struct rcs_timestamp
lex_date(const struct rcs_number *n, yyscan_t yyscanner,
	const struct rcs_file *file)
{
	struct rcs_timestamp ts;
	struct tm tm;
	int i;

	tm.tm_year = n->n[0];
	if (tm.tm_year > 1900)
		tm.tm_year -= 1900;
	tm.tm_mon = n->n[1] - 1;
	tm.tm_mday = n->n[2];
	tm.tm_hour = n->n[3];
	tm.tm_min = n->n[4];
	tm.tm_sec = n->n[5];
	tm.tm_isdst = -1;
	tm.tm_zone = 0;
	ts.value = mktime(&tm);
	if (!ts.value) {
		fprintf(stderr, "%s: (%d) unparsable date: ",
			file->master_name, yyget_lineno(yyscanner));
		for (i = 0; i < n->c; i++) {
			if (i)
				fprintf(stderr, ".");
			fprintf(stderr, "%d", n->n[i]);
		}
		fprintf(stderr, "\n");
	}

	/*
	 * Store the timestamp as a string.  In almost all cases, this could be
	 * derived from the Unix time value; but there is an edge case involving
	 * the hour skipped by daylight saving time (DST).  For example, an
	 * MKSSI file revision might have the following timestamp:
	 *
	 * 2009/03/08 02:29:46Z
	 *
	 * Now in 2009, DST started on March 8th at 2 a.m.  The above timestamp
	 * thus falls into the hour which was skipped.  As a result, when the
	 * timestamp is converted into Unix time by mktime() (see above) and
	 * converted back with localtime() and strftime(), it ends up an hour
	 * off:
	 *
	 * 2009/03/08 03:29:46Z
	 *
	 * To avoid this problem, allocate a timestamp string here using the
	 * values from the RCS metadata, rather than deriving it later from the
	 * Unix time.
	 */
	ts.string = sprintf_alloc("%04u/%02u/%02u %02u:%02u:%02uZ",
		n->n[0], n->n[1], n->n[2], n->n[3], n->n[4], n->n[5]);

	return ts;
}

/* extract a username from an MKSSI locker string */
char *
lex_locker(const char *locker)
{
	const char *p;
	char *usr;
	size_t len;

	/*
	 * MKSSI locker strings are a username followed by a timestamp in
	 * parentheses, for example:
	 *
	 * johndoe(2020.01.01.00.00.00)
	 *
	 * For now, we don't care about the timestamp.  We only need the
	 * username.
	 */
	for (p = locker; *p && *p != '('; ++p);
		;
	len = p - locker;
	usr = xmalloc(len+1, __func__);
	memcpy(usr, locker, len);
	usr[len] = '\0';
	return usr;
}

#define SUFFIX(a, s) \
	((strlen(a) >= strlen(s)) && !strcmp((a) + strlen(a) - strlen(s), (s)))

static void
fast_export_sanitize(yyscan_t yyscanner, struct rcs_file *file)
{
	char *sp, *tp;
	size_t len;
	int ch;

	for (sp = tp = yyget_text(yyscanner); *sp; sp += len) {
		/*
		 * TODO: Fix to enforce all the rules:
		 * https://git-scm.com/docs/git-check-ref-format
		 */
		len = parse_mkssi_branch_char(sp, &ch);
		if (ch == -1)
			continue;

		*tp++ = (char)ch;
		if (SUFFIX(yyget_text(yyscanner), "@{") ||
		    SUFFIX(yyget_text(yyscanner), "..")) {
			fatal_error("%s: (%d) tag or branch name %s is "
				"ill-formed.", file->master_name,
				yyget_lineno(yyscanner),
				yyget_text(yyscanner));
		}
	}
	*tp = '\0';
	if (!strlen(yyget_text(yyscanner))) {
		fatal_error("%s: (%d) tag or branch name was empty after "
			"sanitization.", file->master_name,
			yyget_lineno(yyscanner));
	}
}
