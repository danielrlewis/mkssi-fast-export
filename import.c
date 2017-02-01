#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "interfaces.h"
#include "gram.h"
#include "lex.h"

/* add an RCS file to the file hash table */
static void
rcs_file_add(struct rcs_file *file)
{
	uint32_t bucket;
	struct rcs_file *f, **nextp;

	bucket = hash_string(file->name) % ARRAY_SIZE(file_hash_table);
	file->hash_next = file_hash_table[bucket];
	file_hash_table[bucket] = file;

	/* As a sanity check, make sure there are no duplicates. */
	for (f = file->hash_next; f; f = f->hash_next)
		if (!strcasecmp(f->name, file->name))
			fatal_error("found duplicate file name %s", f->name);

	/*
	 * Sort the file list so that the order in which files are processed is
	 * predictable.
	 */
	nextp = &files;
	for (f = files; f; f = f->next) {
		if (strcasecmp(f->name, file->name) > 0)
			break;
		nextp = &f->next;
	}
	file->next = f;
	*nextp = file;
}

/* import an RCS master file into memory */
static struct rcs_file *
import_rcs_file(const char *relative_path)
{
	struct rcs_file *file;
	struct stat buf;
	yyscan_t scanner;
	FILE *in;
	int err;

	file = xcalloc(1, sizeof *file, __func__);
	file->master_name = xmalloc(strlen(mkssi_dir_path) + 1 +
		strlen(relative_path) + 1, __func__);
	sprintf(file->master_name, "%s/%s", mkssi_dir_path, relative_path);
	file->name = xstrdup(relative_path, __func__);

	/* Lexer/parser do not like empty files */
	if (stat(file->master_name, &buf))
		fatal_system_error("cannot stat \"%s\"", file->master_name);
	if (!buf.st_size) {
		file->corrupt = true;
		fprintf(stderr, "warning: RCS file \"%s\" is empty\n",
				file->master_name);
		return file;
	}

	if (!(in = fopen(file->master_name, "r")))
		fatal_system_error("cannot open \"%s\"", file->master_name);

	/*
	 * Lexically analyze and parse the RCS master file, putting its data
	 * into the rcs_file structure.  See lex.l and gram.y.
	 */
	yylex_init(&scanner);
	yyset_in(in, scanner);
	err = yyparse(scanner, file);
	yylex_destroy(scanner);

	if (err) {
		if (file->corrupt)
			/*
			 * It would be nice if this was a fatal error, but at
			 * some projects seem to have such problems...
			 */
			fprintf(stderr, "warning: RCS file \"%s\" is corrupt\n",
				file->master_name);
		else
			fatal_error("yyparse aborted with unexpected error");
	}

	fclose(in);

	return file;
}

/* Should a given file be ignored during the import process? */
static bool
ignore_file(const char *name)
{
	const char *dotpj;

	/* Ignore dot and dot-dot for obvious reasons */
	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return true;

	/*
	 * Ignore *.pj files: project.pj is imported separately, and other files
	 * like "Copy of PROJECT.PJ" are not needed or wanted.
	 */
	dotpj = strcasestr(name, ".pj");
	if (dotpj && !dotpj[3])
		return true;

	/*
	 * MKSSI sometimes puts files like vc_04f4.000 or vc_09d5.000 in the
	 * project directory, for its own unknown and unknowable purposes.
	 * These files are large and sometimes have screwed up RCS metadata,
	 * so ignore them.
	 */
	if (!strncmp(name, "vc_", 3) && is_hex_digit(name[3])
	 && is_hex_digit(name[4]) && is_hex_digit(name[5])
	 && is_hex_digit(name[6]) && !strcmp(&name[7], ".000")) {
		return true;
	}

	return false;
}

/* import all RCS master files in given directory */
static void
import_rcs_files_in_dir(const char *relative_dir_path)
{
	char *relative_path;
	DIR *dir;
	struct dirent *de;
	struct rcs_file *file;

	/* 1024 should be big enough for any file in this directory */
	relative_path = xmalloc(strlen(mkssi_dir_path) + 1 +
		strlen(relative_dir_path) + 1 + 1024, __func__);

	if (*relative_dir_path)
		sprintf(relative_path, "%s/%s", mkssi_dir_path,
			relative_dir_path);
	else
		strcpy(relative_path, mkssi_dir_path);

	if(!(dir = opendir(relative_path)))
		fatal_system_error("cannot opendir \"%s\"", relative_path);

	for (;;) {
		errno = 0;
		de = readdir(dir);
		if (!de) {
			if (errno)
				fatal_system_error("cannot readdir");
			break;
		}

		if (ignore_file(de->d_name))
			continue;

		if (*relative_dir_path)
			sprintf(relative_path, "%s/%s", relative_dir_path,
				de->d_name);
		else
			strcpy(relative_path, de->d_name);

		if (de->d_type == DT_DIR)
			import_rcs_files_in_dir(relative_path);
		else if (de->d_type == DT_REG) {
			file = import_rcs_file(relative_path);
			if (file->corrupt) {
				file->next = corrupt_files;
				corrupt_files = file;
			} else
				rcs_file_add(file);
		} else
			fatal_error("%s/%s: unexpected file type %d",
				mkssi_dir_path, relative_path, de->d_type);
	}
	closedir(dir);

	free(relative_path);
}

/* import RCS master files from MKSSI project */
void
import(void)
{
	export_progress("importing RCS master files from \"%s\"",
		mkssi_dir_path);

	/* Import project.pj first, so we fail quickly if something is wrong. */
	project = import_rcs_file("project.pj");
	if (project->corrupt)
		fatal_error("%s/%s is corrupt", project->name, mkssi_dir_path);

	/* Import the rest of the RCS master files. */
	import_rcs_files_in_dir("");
}
