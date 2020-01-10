/*
 * Copyright (c) 2006 by Keith Packard
 * Copyright (c) 2017, 2020 Datalight, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Bison/Yacc grammar for MKSSI-style RCS files.
 */
%{
#include "interfaces.h"
#include "gram.h"
#include "lex.h"

extern void yyerror(yyscan_t scanner, struct rcs_file *file, const char *msg);
extern YY_DECL;
%}

/*
 * Properly, the first declaration in %parse-params should have the type
 * yyscan_t, but this runs into the problem that this type is both declared in
 * lex.h and needed in gram.y -- which lex.h needs.  We used to kluge around
 * this by declaring typredef void *yyscan_t, but this caused other problems
 * including complaints from compilers like clang that barf on duplicate
 * typedefs.
 */
%define api.pure full
%lex-param {yyscan_t scanner}
%lex-param {struct rcs_file *rcsfile}
%parse-param {void *scanner}
%parse-param {struct rcs_file *rcsfile}

%union {
	char *s;
	struct rcs_text text;
	struct rcs_number number;
	struct rcs_timestamp date;
	struct rcs_symbol *symbol;
	struct rcs_lock *lock;
	struct rcs_version *version;
	struct rcs_version **vlist;
	struct rcs_patch *patch;
	struct rcs_patch **patches;
	struct rcs_branch *branch;
	struct rcs_file *file;
}

/*
 * There's a good description of the RCS master format at:
 * http://www.opensource.apple.com/source/cvs/cvs-19/cvs/doc/RCSFILES?txt
 *
 * Note that this grammer has been tailored for the MKSSI variant of RCS.  It
 * does not support RCS features which MKSSI does not use, and it supports
 * MKSSI extensions to RCS.
 */

%token ACCESS
%token AUTHOR
%token BRANCH
%token BRANCHES
%token COLON
%token COMMENT
%token CORRUPT
%token <s> DATA
%token DATE
%token DESC
%token EXTENSION
%token FORMAT
%token FMTBINARY
%token FMTTEXT
%token HEAD
%token LOCKS
%token LOG
%token NEXT
%token <number> NUMBER
%token REFERENCE
%token SEMI
%token STATE
%token STORAGE
%token STRICT
%token SYMBOLS
%token TEXT
%token <text> TEXT_DATA
%token <s> TOKEN

%type <text> text
%type <s> log name author state
%type <symbol> symbollist symbol symbols
%type <lock> lock locks
%type <version> revision
%type <vlist> revisions
%type <date> date
%type <branch> branches numbers
%type <number> next opt_number
%type <patch> patch
%type <patches> patches

%%
file : headers revisions ext desc patches
	;
headers : header headers
	|
	;
header : HEAD opt_number SEMI
		{ rcsfile->head = $2; }
	| BRANCH opt_number SEMI
		{ rcsfile->branch = $2; }
	| ACCESS SEMI
	| symbollist
		{ rcsfile->symbols = $1; }
	| LOCKS locks SEMI lock_type
		{ rcsfile->locks = $2; }
	| storage
	| COMMENT DATA SEMI
		{ free($2); }
	| format
	| CORRUPT
		{ rcsfile->corrupt = true; YYABORT; }
	;
locks : locks lock
		{ $2->next = $1; $$ = $2; }
	|
		{ $$ = NULL; }
	;
lock : TOKEN COLON NUMBER
		{
			$$ = xcalloc(1, sizeof(struct rcs_lock),
				"making lock");
			$$->locker = lex_locker($1);
			$$->number = $3;
		}
	;
lock_type : STRICT SEMI
	|
	;
storage : STORAGE REFERENCE TOKEN SEMI
		{ rcsfile->reference_subdir = xstrdup($3, "refdir"); }
	| STORAGE SEMI
	;
symbollist : SYMBOLS symbols SEMI
		{ $$ = $2; }
	;
symbols : symbols symbol
		{ $2->next = $1; $$ = $2; }
	|
		{ $$ = NULL; }
	;
symbol : name COLON NUMBER
		{
			$$ = xcalloc(1, sizeof(struct rcs_symbol),
				"making symbol");
			$$->symbol_name = $1;
			$$->number = $3;
		}
	;
format : FORMAT FMTBINARY SEMI
		{ rcsfile->binary = true; }
	| FORMAT FMTTEXT SEMI
		{ rcsfile->binary = false; }
	;
name : TOKEN
	| NUMBER
		{
			char name[RCS_MAX_REV_LEN];
			rcs_number_string(&$1, name, sizeof name);
			$$ = xstrdup(name, __func__);
		}
	;
revisions : revisions revision
		{ *$1 = $2; $$ = &$2->next; }
	|
		{ $$ = &rcsfile->versions; }
	;
revision : NUMBER date author state branches next
		{
			$$ = xcalloc(1, sizeof(struct rcs_version),
				"gram.y::revision");
			$$->number = $1;
			$$->date = $2;
			$$->author = $3;
			$$->state = $4;
			$$->branches = $5;
			$$->parent = $6;
		}
	;
date : DATE NUMBER SEMI
		{ $$ = lex_date(&$2, scanner, rcsfile); }
	;
author : AUTHOR TOKEN SEMI
		{ $$ = $2; }
	;
state : STATE TOKEN SEMI
		{ $$ = $2; }
	;
branches : BRANCHES numbers SEMI
		{ $$ = $2; }
	;
numbers : NUMBER numbers
		{
			$$ = xcalloc(1, sizeof(struct rcs_branch),
				"gram.y::numbers");
			$$->next = $2;
			$$->number = $1;
		}
	|
		{ $$ = NULL; }
	;
next : NEXT opt_number SEMI
		{ $$ = $2; }
	;
opt_number : NUMBER
		{ $$ = $1; }
	|
		{ $$.c = 0; }
	;
desc : DESC DATA
		{ free($2); }
	;
ext : EXTENSION DATA
		{ free($2); }
	|
	;
patches : patches patch
		{ *$1 = $2; $$ = &$2->next; }
	|
		{ $$ = &rcsfile->patches; }
	;
patch : NUMBER log text
		{
			$$ = xcalloc(1, sizeof(struct rcs_patch),
				"gram.y::patch");
			$$->number = $1;
			$$->log = xstrdup($2, __func__);
			$$->text = $3;
			free($2);
		}
	;
log : LOG DATA
		{ $$ = $2; }
	;
text : TEXT TEXT_DATA
		{ $$ = $2; }
	| REFERENCE TEXT_DATA
		{ $$ = $2; }
	;
%%

/* output an error message */
void
yyerror(yyscan_t scanner, struct rcs_file *file, const char *msg)
{
	fatal_error("parse error %s at %s\n", msg, yyget_text(scanner));
}
