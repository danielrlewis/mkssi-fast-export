/*
 * Copyright (c) 2017, 2019-2020 Tuxera US Inc
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Import RCS files into in-memory structures.
 */
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

/* is a file an encrypted MKSSI RCS archive? */
static bool
is_encrypted_archive(FILE *f)
{
	static const char encrypt_header[] = "#!encrypt\n";
	char buf[sizeof encrypt_header - 1];
	bool encrypted;

	encrypted = false;

	/* Encrypted archives start with the string in encrypt_header */
	if (fread(buf, 1, sizeof buf, f) == sizeof buf
	 && !memcmp(buf, encrypt_header, sizeof buf))
		encrypted = true;

	/* Reset file position to start */
	fseek(f, 0, SEEK_SET);

	return encrypted;
}

/* skip the optional archive header in an RCS file */
static void
skip_archive_header(FILE *f)
{
	static const char archive_header[] = "--MKS-Archive--";
	char buf[sizeof archive_header - 1];
	int nl;

	if (fread(buf, 1, sizeof buf, f) == sizeof buf
	 && !memcmp(buf, archive_header, sizeof buf)) {
		/* Expect the header to be followed by LF or CRLF. */
		nl = fgetc(f);
		if (nl == '\r')
			nl = fgetc(f);
		if (nl == '\n')
			/*
			 * The header exists; return without seeking back to the
			 * beginning of the file.
			 */
			return;
	}

	/* Reset file position to start */
	fseek(f, 0, SEEK_SET);
}

/* create placeholders for missing patches, starting at the given revision */
static void
create_missing_patches_from_rev(struct rcs_file *file,
	const struct rcs_number *head, bool missing_antecedent)
{
	const struct rcs_version *v;
	struct rcs_patch *p;
	const struct rcs_branch *b;
	struct rcs_number n;

	/* Loop through the file's revisions, from newest to oldest. */
	for (n = *head; n.c; n = v->parent) {
		v = rcs_file_find_version(file, &n, true);
		p = rcs_file_find_patch(file, &n, false);
		if (!p) {
			fprintf(stderr, "warning: \"%s\" missing patch for "
				"rev. %s\n", file->master_name,
				rcs_number_string_sb(&n));

			/*
			 * If this patch is missing, any branches or ancestors
			 * have to be treated as missing.  RCS patches are
			 * applied in sequence, so it's impossible to recover
			 * the contents of a revision if any of the antecedent
			 * patches are missing.
			 */
			missing_antecedent = true;

			p = xcalloc(1, sizeof *p, __func__);
			p->number = n;

			/*
			 * The patches list isn't sorted, so for convenience
			 * just add the patch at the front of the list.
			 */
			p->next = file->patches;
			file->patches = p;
		}

		p->missing = missing_antecedent;

		/* Recursively handle any branches from this revision. */
		for (b = v->branches; b; b = b->next)
			create_missing_patches_from_rev(file, &b->number,
				missing_antecedent);
	}
}

/* create placeholders for any patches missing from the RCS file */
static void
create_missing_patches(struct rcs_file *file)
{
	create_missing_patches_from_rev(file, &file->head, false);
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
	file->master_name = sprintf_alloc("%s/%s", mkssi_rcs_dir_path,
		relative_path);
	file->name = xstrdup(relative_path, __func__);

	if (!(in = fopen(file->master_name, "r")))
		fatal_system_error("cannot open \"%s\"", file->master_name);

	/* Lexer/parser do not like empty files */
	if (stat(file->master_name, &buf))
		fatal_system_error("cannot stat \"%s\"", file->master_name);
	if (!buf.st_size) {
		file->corrupt = true;
		fprintf(stderr, "warning: RCS file \"%s\" is empty\n",
				file->master_name);
		goto out;
	}

	/* No support for MKSSI encrypted archives */
	if (is_encrypted_archive(in)) {
		file->corrupt = true; /* Not really, but close enough */
		fprintf(stderr, "warning: RCS file \"%s\" is encrypted\n",
			file->master_name);
		fprintf(stderr,
			"Encryption not supported, file will be ignored\n");
		goto out;
	}

	/*
	 * Very rarely, MKSSI RCS files will start with a header line.  This
	 * line is not in RCS format and will confuse the lexer/parser; so
	 * skip over such lines, when present.
	 */
	skip_archive_header(in);

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
			 * It would be nice if this was a fatal error, but some
			 * projects seem to have such problems...
			 */
			fprintf(stderr, "warning: RCS file \"%s\" is corrupt\n",
				file->master_name);
		else
			fatal_error("yyparse aborted with unexpected error");
	} else
		create_missing_patches(file);

out:
	fclose(in);
	return file;
}

/* does a string end with the given postfix? */
static bool
str_ends_with(const char *str, const char *postfix, bool ignore_case)
{
	const char *pos;

	if (strlen(str) < strlen(postfix))
		return false;

	pos = str + strlen(str) - strlen(postfix);

	if (ignore_case)
		return !strcasecmp(pos, postfix);
	return !strcmp(pos, postfix);
}

/* should a given file be ignored during the import process? */
static bool
ignore_file(const char *name)
{
	/* Ignore dot and dot-dot for obvious reasons */
	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return true;

	/*
	 * Ignore *.pj files: project.pj is imported separately, and other files
	 * like "Copy of PROJECT.PJ" are not needed or wanted.
	 */
	if (str_ends_with(name, ".pj", true))
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

	/*
	 * Directories named mks.<archive_name>.revs are used to store file
	 * revision data outside the RCS master.  Files in such directories
	 * are not RCS files and should not be parsed at this point.
	 */
	if (!strncmp(name, "mks.", 4) && str_ends_with(name, ".revs", false))
		return true;

	return false;
}

/* import all RCS master files in given directory */
static void
import_rcs_files_in_dir(const char *relative_dir_path)
{
	char *relative_path;
	DIR *dir;
	const struct dirent *de;
	struct rcs_file *file;

	/* 1024 should be big enough for any file in this directory */
	relative_path = xmalloc(strlen(mkssi_rcs_dir_path) + 1 +
		strlen(relative_dir_path) + 1 + 1024, __func__);

	if (*relative_dir_path)
		sprintf(relative_path, "%s/%s", mkssi_rcs_dir_path,
			relative_dir_path);
	else
		strcpy(relative_path, mkssi_rcs_dir_path);

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
				mkssi_rcs_dir_path, relative_path, de->d_type);
	}
	closedir(dir);

	free(relative_path);
}

/* import RCS master files from MKSSI project */
void
import(void)
{
	/*
	 * If getting the author list, suppress progress messages intended for
	 * git fast-import.
	 */
	if (!author_list)
		export_progress("importing RCS master files from \"%s\"",
			mkssi_rcs_dir_path);

	/* Import project.pj first, so we fail quickly if something is wrong. */
	project = import_rcs_file(rcs_projectpj_name);
	if (project->corrupt)
		fatal_error("%s/%s is corrupt", project->name,
			mkssi_rcs_dir_path);

	/*
	 * At this point, the branch list only has one entry, for the master
	 * branch.  If the master branch wasn't explicitly assigned a revision
	 * number via --trunk-branch, ...
	 */
	if (!master_branch->number.c)
		/*
		 * ... then save the project.pj head revision as the master
		 * branch revision number.
		 */
		master_branch->number = project->head;

	/* Import the rest of the RCS master files. */
	import_rcs_files_in_dir("");
}
