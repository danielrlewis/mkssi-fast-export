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

%s CONTENT SYMBOLSS SKIP AUTHORSS FORMATS
%%
<INITIAL>head BEGIN(CONTENT); return HEAD;
<INITIAL>branch BEGIN(CONTENT); return BRANCH;
<INITIAL>access return ACCESS;
<INITIAL>symbols BEGIN(SYMBOLSS); return SYMBOLS;
<INITIAL>locks BEGIN(CONTENT); return LOCKS;
<INITIAL>comment BEGIN(CONTENT); return COMMENT;
<INITIAL>date BEGIN(CONTENT); return DATE;
<INITIAL>branches BEGIN(CONTENT); return BRANCHES;
<INITIAL>next BEGIN(CONTENT); return NEXT;
<INITIAL>strict BEGIN(CONTENT); return STRICT;
<INITIAL>author BEGIN(AUTHORSS); return AUTHOR;
<INITIAL>state BEGIN(CONTENT); return STATE;
<INITIAL>desc return DESC;
<INITIAL>log return LOG;
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
<FORMATS>binary BEGIN(INITIAL); return FMTBINARY;
<FORMATS>text BEGIN(INITIAL); return FMTTEXT;
[0-9]+\.[0-9.]* {
		yylval->number = lex_number(yytext);
		return NUMBER;
	}
<SYMBOLSS>[-a-zA-Z_0-9+%\*][-a-zA-Z_0-9+/%=.~^\\*?#!\[\]()<>\*]* {
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
	int max, cur;
	char *string;
	char buf[1024];
};

static void
varbuf_init(struct varbuf *buf)
{
	buf->max = sizeof buf->buf;
	buf->cur = 0;
	buf->string = buf->buf;
}

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

static void
varbuf_free(struct varbuf *buf)
{
	if (buf->string != buf->buf)
		free(buf->string);
}

static char *
varbuf_dup(struct varbuf *buf, const char *legend)
{
	char *dup = xmalloc(buf->cur, legend);
	memcpy(dup, buf->string, buf->cur);
	return dup;
}

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
	ret = varbuf_dup(&buf, "parse_data");
	varbuf_free(&buf);
	return ret;
}

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

time_t
lex_date(const struct rcs_number *n, yyscan_t yyscanner, struct rcs_file *file)
{
	struct tm tm;
	time_t d;

	tm.tm_year = n->n[0];
	if (tm.tm_year > 1900)
		tm.tm_year -= 1900;
	tm.tm_mon = n->n[1] - 1;
	tm.tm_mday = n->n[2];
	tm.tm_hour = n->n[3];
	tm.tm_min = n->n[4];
	tm.tm_sec = n->n[5];
	tm.tm_isdst = 0;
	tm.tm_zone = 0;

	d = mktime(&tm);
	if (d == 0) {
		int i;
		fprintf(stderr, "%s: (%d) unparsable date: ",
			file->master_name, yyget_lineno(yyscanner));
		for (i = 0; i < n->c; i++) {
			if (i)
				fprintf(stderr, ".");
			fprintf(stderr, "%d", n->n[i]);
		}
		fprintf(stderr, "\n");
	}
	return d;
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
