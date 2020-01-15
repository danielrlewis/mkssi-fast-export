/*
 * Copyright (c) 2006 by Keith Packard
 * Copyright (c) 2017, 2019-2020 Datalight, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Types, macros, and prototypes.
 */
#ifndef INTERFACES_H
#define INTERFACES_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#define RCS_MAX_DIGITS 10 /* max digits in decimal numbers */
#define RCS_MAX_BRANCHWIDTH 10
#define RCS_MAX_DEPTH (2 * RCS_MAX_BRANCHWIDTH + 2)
#define RCS_MAX_REV_LEN (RCS_MAX_DEPTH * (RCS_MAX_DIGITS + 1))

#define TIP_REVNUM (const struct rcs_number *)NULL

/* digested form of an RCS revision */
struct rcs_number {
	short c;
	short n[RCS_MAX_DEPTH];
};

/* an RCS symbol-to-revision association */
struct rcs_symbol {
	struct rcs_symbol *next;

	/* RCS metadata */
	char *symbol_name;
	struct rcs_number number;
};

/* a project branch within MKSSI */
struct mkssi_branch {
	struct mkssi_branch *next; /* next in linked list */
	struct mkssi_branch *parent; /* parent branch */

	char *branch_name; /* sanitized branch name */
	char *pj_name; /* vpNNNN.pj file with branch's tip revisions */
	time_t mtime;  /* mtime of *.pj file */
	struct rcs_number number; /* project revision number for branch */
	struct rcs_number tip_number; /* revision number in vpNNNN.pj */
	const struct rcs_file_revision *tip_frevs; /* file revisions for tip */
	unsigned long ncommit_total; /* # of commits on this branch */
	unsigned long ncommit_orig; /* commits originating on this branch */
	bool created; /* whether the branchpoint has been exported */
};

/* an RCS branch revision */
struct rcs_branch {
	struct rcs_branch *next;

	/* RCS metadata */
	struct rcs_number number;
};

/* RCS revision timestamp */
struct rcs_timestamp {
	time_t value; /* Time expressed as seconds since the Unix epoch */
	const char *string; /* Time expressed as an MKSSI-style string */
};

/* metadata of a delta within an RCS file */
struct rcs_version {
	struct rcs_version *next;
	bool checkpointed; /* revision listed in a project checkpoint */
	bool executable; /* revision data looks like a Linux/Unix executable */
	unsigned long blob_mark; /* mark # of data blob for this version */

	/*
	 * Keep track of whether this version has RCS keywords that potentially
	 * require special handling.
	 */
	bool kw_name; /* Has keyword that expands to the file name */
	bool kw_path; /* Has keyword that expands to the file path */
	bool kw_projrev; /* Has $ProjectRevision$ keyword */

	/*
	 * Usually, a file revision has the same data regardless of which
	 * project revision references it.  However, there are times when that
	 * isn't true, due to RCS keyword expansion edge cases.  In those cases,
	 * this file revision must be exported just-in-time for the project
	 * revision.
	 */
	bool jit;

	/* RCS metadata */
	struct rcs_number number;
	struct rcs_timestamp date; /* revision timestamp */
	const char *author, *state;
	struct rcs_branch *branches;
	struct rcs_number parent; /* next in ,v file */
};

/* a reference to a @-encoded text fragment in an RCS file */
struct rcs_text {
	off_t offset; /* position of initial '@' */
	size_t length; /* includes terminating '@' */
};

/* an RCS patch structure */
struct rcs_patch {
	struct rcs_patch *next;

	/*
	 * This patch, or one of its antecedents, is missing from the RCS file.
	 * If log (below) is NULL, the patch itself was missing; otherwise it
	 * was an antecedent patch.
	 */
	bool missing;

	/* RCS metadata */
	struct rcs_number number;
	char *log;
	struct rcs_text text;
};

/* an RCS lock structure */
struct rcs_lock {
	struct rcs_lock *next;
	char *locker; /* username of user who holds the lock */
	struct rcs_number number; /* revision number that's locked */
};

/* this represents the entire metadata content of an RCS master file */
struct rcs_file {
	struct rcs_file *next; /* next in complete list */
	struct rcs_file *hash_next; /* next in hash table bucket */
	char *name; /* relative file path (without project directory) */
	char *master_name; /* path to RCS master file */

	/*
	 * Keep track of how many times the capitalization of the path, and the
	 * file name portion of the path, are adjusted based on the project file
	 * listing.  If these values are >1, then special handling will be
	 * required if RCS keywords that expand to the name or path are present
	 * in the file.
	 */
	unsigned long path_changes, name_changes;

	/*
	 * Indicates that this is a dummy instance for a file which doesn't
	 * exist in the RCS directory.  Only used for "other" member types,
	 * which can be exported from the project directory.  If set, the RCS
	 * metadata is not populated.
	 */
	bool dummy;

	bool corrupt; /* File has corrupt RCS metadata */
	bool binary; /* File is binary (using the binary RCS format) */

	/*
	 * Files listed in the project with member type "other":
	 *
	 * For text files, MKSSI seems to grab rev. 1.1 without doing RCS
	 * keyword expansion.  Setting has_member_type_other to true lets us
	 * know we need to export a version of rev. 1.1 without keyword
	 * expansion; the blob marker for it is saved in other_blob_mark.
	 *
	 * For binary files, MKSSI seems to grab the contents of the file from
	 * the project directory.  The file is not required to exist in the RCS
	 * directory.  However, if the project directory isn't available, we
	 * will use the head revision from the RCS directory, which is often
	 * (not always) identical to the copy that would be in the project
	 * directory.
	 */
	bool has_member_type_other;
	unsigned long other_blob_mark;

	/* RCS metadata */
	struct rcs_number head, branch;
	struct rcs_lock *locks;
	char *reference_subdir;
	struct rcs_symbol *symbols;
	struct rcs_version *versions;
	struct rcs_patch *patches;
};

/* list of file revisions */
struct rcs_file_revision {
	struct rcs_file_revision *next;
	struct rcs_file *file;
	struct rcs_number rev; /* revision number */
	struct rcs_version *ver; /* associated file revision */
	char *canonical_name; /* name with capitalization fixes */
	bool member_type_other; /* listed with "other" member type */
};

/* list of changes to files */
struct file_change {
	struct file_change *next;
	struct rcs_file *file;
	const char *canonical_name; /* name with capitalization fixes */
	const char *old_canonical_name; /* used only for renames */
	char *buf; /* sometimes allocated for canonical names */

	/*
	 * Revision numbers
	 * oldrev: populated for updates and deletes.
	 * newrev: populates for updates and adds.
	 * Neither is populated for renames, which have no file revision
	 */
	struct rcs_number oldrev, newrev;

	bool member_type_other; /* add/update of "other" member type */
	bool projrev_update; /* update for $ProjectRevision$ keyword */

	/*
	 * This is populated only for renames.  It is the full list of file
	 * revisions for the prior project revision.  It is used to append file
	 * modifications to the rename commit, if such is necessary to update
	 * RCS keywords that expand to a name or path.
	 */
	const struct rcs_file_revision *old_frevs;
};

/* set of all changes between project revisions */
struct file_change_lists {
	struct file_change *renames, *adds, *updates, *deletes;
};

/* represent a Git author */
struct git_author {
	const char *name; /* proper name, e.g., "John Doe" */
	const char *email; /* email address, e.g., "johnd@example.com" */
};

/* represent a Git commit */
struct git_commit {
	struct git_commit *next;
	const char *branch;
	const struct git_author *committer;
	time_t date;
	char *commit_msg;
	struct file_change_lists changes;
};

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
	 * Pointer to the line; terminated by \n, \r\n, _or_ NUL.  If
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

	/* The very last line of a buffer might not include a newline. */
	bool no_newline;
};

/* represent list of directories */
struct dir_path {
	struct dir_path *next;
	const char *path; /* not NUL terminated; use len */
	size_t len;
};

/* main.c */
extern const char *mkssi_rcs_dir_path;
extern const char *mkssi_proj_dir_path;
extern const char *source_dir_path;
extern const char *pname_dir_path;
extern const char *rcs_projectpj_name;
extern const char *proj_projectpj_name;
extern const char *proj_projectvpj_name;
extern struct rcs_file *files;
extern struct rcs_file *file_hash_table[1024];
extern struct rcs_file *corrupt_files;
extern struct rcs_file *dummy_files;
extern struct rcs_file *project; /* RCS-revisioned project.pj */
extern struct mkssi_branch *project_branches;
extern struct mkssi_branch *master_branch;
extern struct rcs_number trunk_branch;
extern bool author_list;
extern struct rcs_number pj_revnum_cur;
extern bool exporting_tip;

/* import.c */
void import(void);

/* lex.l */
struct rcs_number lex_number(const char *s);
struct rcs_timestamp lex_date(const struct rcs_number *n, void *yyscanner,
	const struct rcs_file *file);
char *lex_locker(const char *locker);

/* project.c */
void project_read_checkpointed_revisions(void);
void project_read_tip_revisions(void);
const struct rcs_file_revision *find_checkpoint_file_revisions(
	const struct rcs_number *pjrev);

/* changeset.c */
void changeset_build(const struct rcs_file_revision *old,
	time_t old_date, const struct rcs_file_revision *new, time_t new_date,
	struct file_change_lists *changes);
void changeset_free(struct file_change_lists *changes);
struct file_change *change_list_sort_by_name(struct file_change *list);

/* merge.c */
struct git_commit *merge_changeset_into_commits(const char *branch,
	struct file_change_lists *changes, time_t cp_date);
void free_commits(struct git_commit *commit_list);

/* export.c */
void export(void);
void export_progress(const char *fmt, ...);

/* rcs-text.c */
typedef void rcs_revision_data_handler_t(struct rcs_file *file,
	const struct rcs_number *revnum, const char *data,
	bool member_type_other);
void rcs_file_read_all_revisions(struct rcs_file *file,
	rcs_revision_data_handler_t *callback);
char *rcs_file_read_revision(struct rcs_file *file,
	const struct rcs_number *revnum);

/* rcs-binary.c */
typedef void rcs_revision_binary_data_handler_t(struct rcs_file *file,
	const struct rcs_number *revnum, const unsigned char *data,
	size_t datalen, bool member_type_other);
void rcs_binary_file_read_all_revisions(struct rcs_file *file,
	rcs_revision_binary_data_handler_t *callback);

/* rcs-keyword.c */
void rcs_data_unescape_ats(struct rcs_line *dlines);
void rcs_data_keyword_expansion(const struct rcs_file *file,
	struct rcs_version *ver, const struct rcs_patch *patch,
	struct rcs_line *dlines);

/* rcs-number.c */
bool rcs_number_same_branch(const struct rcs_number *a,
	const struct rcs_number *b);
bool rcs_number_equal(const struct rcs_number *n1, const struct rcs_number *n2);
bool rcs_number_partial_match(const struct rcs_number *num,
	const struct rcs_number *spec);
int rcs_number_compare(const struct rcs_number *a, const struct rcs_number *b);
bool rcs_number_is_trunk(const struct rcs_number *number);
struct rcs_number *rcs_number_increment(struct rcs_number *number);
struct rcs_number *rcs_number_decrement(struct rcs_number *number);
char *rcs_number_string(const struct rcs_number *n, char *str, size_t maxlen);
const char *rcs_number_string_sb(const struct rcs_number *n);

/* authors.c */
extern const struct git_author unknown_author;
extern const struct git_author tool_author;
void author_map_initialize(const char *author_map_path);
const struct git_author *author_map(const char *author);
void dump_unmapped_authors(void);

/* line.c */
struct rcs_line *string_to_lines(char *str);
char *lines_to_string(const struct rcs_line *lines);
void lines_free(struct rcs_line *lines);
void lines_reset(struct rcs_line **lines);
size_t line_length(const char *line);
char *line_findstr(const char *line, const char *str);
void line_fprint(FILE *out, const char *line);
struct rcs_line *lines_copy(const struct rcs_line *lines);
void line_allocate(struct rcs_line *line);
bool lines_insert(struct rcs_line **lines, struct rcs_line *insert,
	unsigned int lineno, unsigned int count);
bool lines_delete(struct rcs_line *lines, unsigned int lineno,
	unsigned int count);

/* utils.c */
void fatal_error(char const *fmt, ...);
void fatal_system_error(char const *fmt, ...);
char *sprintf_alloc_append(char *buf, const char *fmt, ...);
uint32_t hash_string(const char *s);
bool is_hex_digit(char c);
const char *path_to_name(const char *path);
char *path_parent_dir(const char *path);
bool is_parent_dir(const char *dirpath, const char *path);
void *xmalloc(size_t size, const char *legend);
void *xcalloc(size_t nmemb, size_t size, const char *legend);
void *xrealloc(void *ptr, size_t size, const char *legend);
char *xstrdup(const char *s, const char *legend);
unsigned char *file_buffer(const char *path, size_t *size);
char *file_as_string(const char *path);
time_t file_mtime(const char *path);
size_t parse_mkssi_branch_char(const char *s, int *cp);
struct rcs_version *rcs_file_find_version(const struct rcs_file *file,
	const struct rcs_number *revnum, bool fatalerr);
struct rcs_patch *rcs_file_find_patch(const struct rcs_file *file,
	const struct rcs_number *revnum, bool fatalerr);
struct dir_path *dir_list_from_path(const char *path);
struct dir_path *dir_list_remove_duplicates(struct dir_path *new_list,
	const struct dir_path *old_list);
struct dir_path *dir_list_append(struct dir_path *old_list,
	struct dir_path *new_list);
void dir_list_free(struct dir_path *list);

/* like sprintf(), but malloc() the output buffer (must be freed by caller) */
#define sprintf_alloc(fmt, ...) sprintf_alloc_append(NULL, fmt, ##__VA_ARGS__)


#define YY_DECL int yylex \
	(YYSTYPE *yylval_param, yyscan_t yyscanner, struct rcs_file *file)


#endif /* INTERFACES_H */
