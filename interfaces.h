#ifndef INTERFACES_H
#define INTERFACES_H

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

/* metadata of a delta within an RCS file */
struct rcs_version {
	struct rcs_version *next;
	bool checkpointed; /* revision listed in a project checkpoint */
	unsigned long blob_mark;

	/* RCS metadata */
	struct rcs_number number;
	time_t date; /* revision timestamp (possibly adjusted) */
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
	struct rcs_file *file;
	struct rcs_number rev;
};

/* list of changes to files */
struct file_change {
	struct file_change *next;
	struct rcs_file *file;
	struct rcs_number oldrev, newrev;
};

/* set of all changes between project revisions */
struct file_change_lists {
	struct file_change *adds, *updates, *deletes;
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

/* main.c */
extern const char *mkssi_dir_path;
extern struct rcs_file *files;
extern struct rcs_file *file_hash_table[1024];
extern struct rcs_file *corrupt_files;
extern struct rcs_file *project; /* project.pj */
extern struct rcs_symbol *project_branches;
extern struct rcs_number trunk_branch;
extern bool author_list;

/* import.c */
void import(void);

/* export.c */
void export(void);
void export_progress(const char *fmt, ...);

/* changeset.c */
void changeset_build(const struct rcs_file_revision *old,
	time_t old_date, const struct rcs_file_revision *new, time_t new_date,
	struct file_change_lists *changes);
void changeset_free(struct file_change_lists *changes);

/* merge.c */
struct git_commit *merge_changeset_into_commits(const char *branch,
	struct file_change_lists *changes, time_t cp_date);
void free_commits(struct git_commit *commit_list);

/* project.c */
void project_read_all_revisions(void);
const struct rcs_file_revision *find_checkpoint_file_revisions(
	const struct rcs_number *pjrev);

/* lex.l */
struct rcs_number lex_number(const char *s);
time_t lex_date(const struct rcs_number *n, void *yyscanner,
	struct rcs_file *file);

/* rcs.c */
typedef void rcs_revision_data_handler_t(struct rcs_file *file,
	const struct rcs_number *revnum, const char *data);
void rcs_file_read_all_revisions(struct rcs_file *file,
	rcs_revision_data_handler_t *callback);

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
void author_map_initialize(const char *author_map_path);
const struct git_author *author_map(const char *author);
void dump_unmapped_authors(void);

/* utils.c */
void progress_println(const char *fmt, ...);
void fatal_error(char const *fmt, ...);
void fatal_system_error(char const *fmt, ...);
char *time2string(time_t date);
char *time2string_mkssi(time_t date);
uint32_t hash_string(const char *s);
bool string_is_upper(const char *s);
bool is_hex_digit(char c);
void *xmalloc(size_t size, const char *legend);
void *xcalloc(size_t nmemb, size_t size, const char *legend);
void *xrealloc(void *ptr, size_t size, const char *legend);
char *xstrdup(const char *s, const char *legend);
size_t parse_mkssi_branch_char(const char *s, int *cp);
struct rcs_version *rcs_file_find_version(struct rcs_file *file,
	const struct rcs_number *revnum, bool fatalerr);
struct rcs_patch *rcs_file_find_patch(struct rcs_file *file,
	const struct rcs_number *revnum, bool fatalerr);


#define YY_DECL int yylex \
	(YYSTYPE *yylval_param, yyscan_t yyscanner, struct rcs_file *file)


#endif /* INTERFACES_H */
