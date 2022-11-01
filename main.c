/*
 * Copyright (c) 2017, 2019-2020 Tuxera US Inc
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Entry point for mkssi-fast-export.
 */
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <getopt.h>
#include "interfaces.h"
#include "gram.h"
#include "lex.h"

const char *mkssi_rcs_dir_path; /* --rcs-dir */
const char *mkssi_proj_dir_path; /* --proj-dir */
const char *source_dir_path; /* --source-dir */
const char *pname_dir_path; /* --pname-dir */
const char *rcs_projectpj_name; /* project.pj name in RCS directory */
const char *proj_projectpj_name; /* project.pj name in project directory */
const char *proj_projectvpj_name; /* project.vpj name in proj directory */
struct rcs_file *files;
struct rcs_file *file_hash_table[1024];
struct rcs_file *corrupt_files;
struct rcs_file *dummy_files; /* "Other" files with no RCS masters */
struct rcs_file *project; /* RCS project.pj */
struct mkssi_branch *project_branches;
struct mkssi_branch *master_branch;
struct rcs_number trunk_branch; /* --trunk-branch */
bool author_list; /* --authorlist */

/*
 * The project revision number currently being exported and whether it's the tip
 * revision for its branch.  Used for RCS keyword expansion.
 */
struct rcs_number pj_revnum_cur;
bool exporting_tip;

/* print usage and exit */
static void
usage(const char *name, bool error)
{
	FILE *f;
	int status;

	if (error) {
		f = stderr;
		status = 1;
	} else {
		f = stdout;
		status = 0;
	}

	fprintf(f, "usage: %s [options]\n", name);
	fprintf(f, "Fast-export history from an MKSSI (v7.5a) repository.\n\n");
	fprintf(f, "The following options are supported:\n");
	fprintf(f, "  -p --proj-dir=path  Path to MKSSI project directory.\n");
	fprintf(f, "  -r --rcs-dir=path  Path to MKSSI RCS directory.\n");
	fprintf(f, "  -S --source-dir=path  Directory to use for $Source$ "
		"keyword\n");
	fprintf(f, "  -P --pname-dir  Directory to use for $ProjectName$ "
		"keyword\n");
	fprintf(f, "  -b --trunk-branch=rev  Trunk branch revision number "
		"(trunk as branch)\n");
	fprintf(f, "  -A --authormap=file  Author map (same as "
		"cvs-fast-export)\n");
	fprintf(f, "  -a --authorlist  Dump authors not in author map and "
		"exit\n");
	fprintf(f, "  -h --help  This help message\n");
	exit(status);
}

/* validate a user-supplied directory path */
static void
dir_validate(const char *dir_path)
{
	struct stat info;

	if (stat(dir_path, &info))
		fatal_system_error("cannot stat \"%s\"", dir_path);
	if (!S_ISDIR(info.st_mode))
		fatal_error("not a directory: \"%s\"", dir_path);
}

/* find a file name in a directory, with case-insensitive matching */
static char *
dir_find_case(const char *dir_path, const char *fname)
{
	DIR *dirp;
	struct dirent *de;
	char *fname_canonical;

	dirp = opendir(dir_path);
	if (!dirp)
		fatal_system_error("cannot open directory at \"%s\"", dir_path);

	for (;;) {
		errno = 0;
		de = readdir(dirp);
		if (!de) {
			if (errno)
				fatal_system_error("error reading from "
					"directory at \"%s\"", dir_path);

			/* If the file name isn't found, return NULL. */
			fname_canonical = NULL;
			break;
		}

		if (!strcasecmp(fname, de->d_name)) {
			/* Return the name with canonical capitalization. */
			fname_canonical = xstrdup(de->d_name, __func__);
			break;
		}
	}

	closedir(dirp);

	return fname_canonical;
}

/* validate the user-supplied MKSSI RCS directory */
static void
mkssi_rcs_dir_validate(const char *dir_path)
{
	char *path, head[4];
	FILE *pjfile;

	dir_validate(dir_path);

	/*
	 * Make sure project.pj exists.  MKSSI is case-insensitive, so
	 * project.pj could have any capitalization variant, which we save for
	 * later use.
	 */
	errno = 0;
	rcs_projectpj_name = dir_find_case(dir_path, "project.pj");
	if (!rcs_projectpj_name)
		fatal_system_error("no project.pj file in RCS directory");

	/* Open the project.pj */
	path = sprintf_alloc("%s/%s", dir_path, rcs_projectpj_name);
	if (!(pjfile = fopen(path, "r")))
		fatal_system_error("cannot open \"%s\"", path);

	/*
	 * MKSSI projects can be a bit confusing because there are two project
	 * directories: and the one with the RCS history is not the one the user
	 * normally interacts with.  The project directory which is normally
	 * used to create sandboxes and the like does not contain any revision
	 * history, only one revision of each file, project.pj included.  There
	 * is a second project directory which contains all of the RCS file
	 * masters, including the revisioned project.pj.  Because this is
	 * potentially confusing, we want to make sure that the project.pj file
	 * in the MKSSI RCS directory is the revisioned version -- if
	 * revisioned, the first four bytes should be "head".
	 */
	errno = 0;
	if (fread(head, 1, sizeof head, pjfile) != sizeof head)
		fatal_system_error("cannot read from \"%s\"", path);
	if (strncmp(head, "head", 4))
		fatal_error("bad MKSSI RCS directory: project.pj is not RCS");

	fclose(pjfile);
	free(path);
}

/* validate the user-supplied MKSSI project directory */
static void
mkssi_proj_dir_validate(const char *dir_path)
{
	const char header[] = "--MKS Project--";
	char *path, firstln[sizeof header - 1];
	int nl;
	FILE *pjfile;

	dir_validate(dir_path);

	/*
	 * Make sure project.pj exists.  MKSSI is case-insensitive, so
	 * project.pj could have any capitalization variant, which we save for
	 * later use.
	 */
	errno = 0;
	proj_projectpj_name = dir_find_case(dir_path, "project.pj");
	if (!proj_projectpj_name)
		fatal_system_error("no project.pj file in project directory");

	/*
	 * Look for the project.vpj directory.  project.vpj only exists for
	 * MKSSI projects that have branches.  MKSSI is case-insensitive, so
	 * project.vpj could have any capitalization variant, which we save for
	 * later use.
	 */
	proj_projectvpj_name = dir_find_case(dir_path, "project.vpj");
	if (proj_projectvpj_name) {
		/* If project.vpj exists, it should be a directory. */
		path = sprintf_alloc("%s/%s", dir_path, proj_projectvpj_name);
		dir_validate(path);
		free(path);
	}

	/* Open the project.pj */
	path = sprintf_alloc("%s/%s", dir_path, proj_projectpj_name);
	if (!(pjfile = fopen(path, "r")))
		fatal_system_error("cannot open \"%s\"", path);

	/* Make sure project.pj has the expected header. */
	errno = 0;
	if (fread(firstln, 1, sizeof firstln, pjfile) != sizeof firstln)
		fatal_system_error("cannot read from \"%s\"", path);

	/* Header should be followed by a newline, possibly preceded by a CR */
	nl = fgetc(pjfile);
	if (nl == '\r')
		nl = fgetc(pjfile);

	if (strncmp(firstln, header, sizeof firstln) || nl != '\n')
		fatal_error("bad MKSSI project directory: project.pj is not an "
			"MKSSI project");

	fclose(pjfile);
	free(path);
}

int
main(int argc, char *argv[])
{
	static const struct option options[] = {
		{ "proj-dir", required_argument, 0, 'p' },
		{ "rcs-dir", required_argument, 0, 'r' },
		{ "source-dir", required_argument, 0, 'S' },
		{ "pname-dir", required_argument, 0, 'P' },
		{ "trunk-branch", required_argument, 0, 'b'},
		{ "authormap", required_argument, 0, 'A'},
		{ "authorlist", no_argument, 0, 'a'},
		{ "help", no_argument, 0, 'h'},
		{ NULL }
	};
	int c;
	const char *author_map;

	if (argc == 1)
		usage(argv[0], false);

	/*
	 * Create the trunk (a.k.a. "master") branch.  Arguably, the MKSSI trunk
	 * isn't really a branch, but it simplifies things to treat it as such.
	 */
	master_branch = xcalloc(1, sizeof *master_branch, __func__);
	master_branch->branch_name = xstrdup("master", __func__);
	master_branch->created = true;
	project_branches = master_branch;

	/* Parse options */
	author_map = NULL;
	for (;;) {
		c = getopt_long(argc, argv, "p:r:S:P:b:A:ah", options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'r':
			mkssi_rcs_dir_validate(optarg);
			mkssi_rcs_dir_path = optarg;
			break;
		case 'p':
			mkssi_proj_dir_validate(optarg);
			mkssi_proj_dir_path = optarg;
			break;
		case 'S':
			source_dir_path = optarg;
			break;
		case 'P':
			pname_dir_path = optarg;
			break;
		case 'b':
			trunk_branch = lex_number(optarg);
			if (!trunk_branch.c || (trunk_branch.c & 1))
				fatal_error("invalid revision number: %s\n",
					optarg);

			master_branch->number = trunk_branch;
			break;
		case 'A':
			author_map = optarg;
			break;
		case 'a':
			author_list = true;
			break;
		case 'h':
			usage(argv[0], false);
			break;
		default: /* error message already emitted */
			fprintf(stderr, "try `%s --help' for more "
				"information.\n", argv[0]);
			exit(1);
		}
	}

	/* There should be exactly no arguments after the options. */
	if (argc != optind) {
		fprintf(stderr, "unrecognized arguments on command line:\n");
		for (; optind < argc; optind++)
			fprintf(stderr, "\t\"%s\"\n", argv[optind]);
		fprintf(stderr, "\n");

		usage(argv[0], true);
	}

	/* RCS directory is a mandatory argument. */
	if (!mkssi_rcs_dir_path) {
		fprintf(stderr, "no MKSSI RCS directory specified (use "
			"--rcs-dir)\n");
		usage(argv[0], true);
	}

	/*
	 * Project directory is optional, but it should typically be provided.
	 * Without it, we can only export changes that have been checkpointed.
	 */
	if (!mkssi_proj_dir_path && !author_list)
		fprintf(stderr, "warning: no MKSSI project directory "
			"specified (only checkpointed changes will be "
			"exported)\n");

	if (!author_list)
		/*
		 * This tells git fast-import that the stream is incomplete if
		 * we abort prior to sending the "done" command.
		 */
		printf("feature done\n");

	/* Initialize mapping of MKSSI authors to Git identities */
	if (author_map)
		author_map_initialize(author_map);

	/* Import the RCS masters from the MKSSI project */
	import();

	if (author_list) {
		/*
		 * Dump authors found in the RCS files but not found in the
		 * author_map (if no author map was given, then all of the
		 * authors get dumped).  This functionality is provided to make
		 * it easier to build an author map, or to verify that an
		 * existing author map is not missing any of a project's
		 * authors.
		 */
		dump_unmapped_authors();
		exit(0);
	}

	/* Export the git fast-import commands for the project */
	export();

	/* Tell git fast-import that we completed successfully */
	printf("done\n");

	exit(0);
}
