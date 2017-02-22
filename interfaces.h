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

#define RCS_MAX_DIGITS 10 /* max digits in decimal numbers */
#define RCS_MAX_BRANCHWIDTH 10
#define RCS_MAX_DEPTH (2 * RCS_MAX_BRANCHWIDTH + 2)
#define RCS_MAX_REV_LEN (RCS_MAX_DEPTH * (RCS_MAX_DIGITS + 1))

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
	unsigned long blob_mark;

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

	/* RCS metadata */
	struct rcs_number number;
	char *log;
	struct rcs_text text;
};

/* this represents the entire metadata content of an RCS master file */
struct rcs_file {
	struct rcs_file *next; /* next in complete list */
	struct rcs_file *hash_next; /* next in hash table bucket */
	char *name; /* relative file path (without project directory) */
	char *master_name; /* path to RCS master file */
	bool binary, corrupt;

	/* RCS metadata */
	struct rcs_number head, branch;
	struct rcs_symbol *symbols;
	struct rcs_version *versions;
	struct rcs_patch *patches;
};

/* list of file revisions */
struct rcs_file_revision {
	struct rcs_file_revision *next;
	const struct rcs_file *file;
	struct rcs_number rev;
	char *canonical_name; /* name with capitalization fixes */
};

/* list of changes to files */
struct file_change {
	struct file_change *next;
	const struct rcs_file *file;
	const char *canonical_name; /* name with capitalization fixes */
	const char *old_canonical_name; /* used only for renames */
	struct rcs_number oldrev, newrev;
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
extern const char *mkssi_dir_path;
extern const char *source_dir_path;
extern struct rcs_file *files;
extern struct rcs_file *file_hash_table[1024];
extern struct rcs_file *corrupt_files;
extern struct rcs_file *project; /* project.pj */
extern struct rcs_symbol *project_branches;
extern struct rcs_number trunk_branch;
extern bool author_list;

/* import.c */
void import(void);

/* lex.l */
struct rcs_number lex_number(const char *s);
struct rcs_timestamp lex_date(const struct rcs_number *n, void *yyscanner,
	const struct rcs_file *file);

/* project.c */
void project_read_all_revisions(void);
const struct rcs_file_revision *find_checkpoint_file_revisions(
	const struct rcs_number *pjrev);

/* changeset.c */
void changeset_build(const struct rcs_file_revision *old,
	time_t old_date, const struct rcs_file_revision *new, time_t new_date,
	struct file_change_lists *changes);
void changeset_free(struct file_change_lists *changes);

/* merge.c */
struct git_commit *merge_changeset_into_commits(const char *branch,
	struct file_change_lists *changes, time_t cp_date);
void free_commits(struct git_commit *commit_list);

/* export.c */
void export(void);
void export_progress(const char *fmt, ...);

/* rcs-text.c */
typedef void rcs_revision_data_handler_t(struct rcs_file *file,
	const struct rcs_number *revnum, const char *data);
void rcs_file_read_all_revisions(struct rcs_file *file,
	rcs_revision_data_handler_t *callback);

/* rcs-binary.c */
typedef void rcs_revision_binary_data_handler_t(struct rcs_file *file,
	const struct rcs_number *revnum, const unsigned char *data,
	size_t datalen);
void rcs_binary_file_read_all_revisions(struct rcs_file *file,
	rcs_revision_binary_data_handler_t *callback);

/* rcs-keyword.c */
void rcs_data_keyword_expansion(const struct rcs_file *file,
	const struct rcs_version *ver, const struct rcs_patch *patch,
	struct rcs_line *dlines);

/* rcs-number.c */
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
void *xmalloc(size_t size, const char *legend);
void *xcalloc(size_t nmemb, size_t size, const char *legend);
void *xrealloc(void *ptr, size_t size, const char *legend);
char *xstrdup(const char *s, const char *legend);
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
