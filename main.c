/* Entry point for mkssi-fast-export */
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

const char *mkssi_rcs_dir_path;
const char *mkssi_proj_dir_path;
const char *source_dir_path;
struct rcs_file *files;
struct rcs_file *file_hash_table[1024];
struct rcs_file *corrupt_files;
struct rcs_file *project; /* project.pj */
struct mkssi_branch *project_branches;
struct rcs_number trunk_branch;
bool author_list;

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

/* open the project.pj file in an MKSSI directory */
static FILE *
open_project(const char *mkssi_dir)
{
	char path[1024]; /* big enough */
	FILE *pjfile;

	/*
	 * Try the less-common 8.3 variant of the name first, so that the error
	 * message (which prints path) will print the LFN variant.
	 */
	snprintf(path, sizeof path, "%s/PROJECT.PJ", mkssi_dir);
	if (!(pjfile = fopen(path, "r"))) {
		snprintf(path, sizeof path, "%s/project.pj", mkssi_dir);
		if (!(pjfile = fopen(path, "r")))
			fatal_system_error("cannot open \"%s\"", path);
	}

	return pjfile;
}

/* validate the user-supplied MKSSI RCS directory */
static void
mkssi_rcs_dir_validate(const char *mkssi_rcs_dir)
{
	char head[4];
	FILE *pjfile;

	dir_validate(mkssi_rcs_dir);
	pjfile = open_project(mkssi_rcs_dir);

	/*
	 * MKSSI projects can be a bit confusing because there are two project
	 * directories: and the one with the RCS history is not the one the user
	 * normally interacts with.  The project directory which is normally
	 * used to create sandboxes and the like does not contain any revision
	 * history, only one revision of each file, project.pj included.  There
	 * is a second project directory which contains all of the RCS file
	 * masters, including the revisioned project.pj.  Because this is
	 * potentially confusing, we want to make sure that the project.pj file
	 * in the MKSSI directory is the revisioned version -- if revisioned,
	 * the first four bytes should be "head".
	 */
	errno = 0;
	if (fread(head, 1, sizeof head, pjfile) != sizeof head)
		fatal_system_error("cannot read from \"%s/project.pj\"",
			mkssi_rcs_dir);
	if (strncmp(head, "head", 4))
		fatal_error("bad MKSSI RCS directory: project.pj is not RCS");

	fclose(pjfile);
}

/* validate the user-supplied MKSSI project directory */
static void
mkssi_proj_dir_validate(const char *mkssi_tip_dir)
{
	const char header[] = "--MKS Project--";
	char firstln[sizeof header - 1];
	int nl;
	FILE *pjfile;

	dir_validate(mkssi_tip_dir);
	pjfile = open_project(mkssi_tip_dir);

	errno = 0;
	if (fread(firstln, 1, sizeof firstln, pjfile) != sizeof firstln)
		fatal_system_error("cannot read from \"%s/project.pj\"",
			mkssi_tip_dir);

	/* Header should be followed by a newline, possibly preceded by a CR */
	nl = fgetc(pjfile);
	if (nl == '\r')
		nl = fgetc(pjfile);

	if (strncmp(firstln, header, sizeof firstln) || nl != '\n')
		fatal_error("bad MKSSI tip directory: project.pj is not an "
			"MKSSI project");

	fclose(pjfile);
}

int
main(int argc, char *argv[])
{
	static const struct option options[] = {
		{ "proj-dir", required_argument, 0, 'p' },
		{ "rcs-dir", required_argument, 0, 'r' },
		{ "source-dir", required_argument, 0, 'S' },
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
	 *
	 * Note that the revision number for the trunk branch isn't initialized;
	 * it isn't important unless the --trunk-branch parameter is specified.
	 */
	project_branches = xcalloc(1, sizeof *project_branches, __func__);
	project_branches->branch_name = xstrdup("master", __func__);

	/* Parse options */
	author_map = NULL;
	for (;;) {
		c = getopt_long(argc, argv, "p:r:S:b:A:ah", options, NULL);
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
		case 'b':
			trunk_branch = lex_number(optarg);
			if (!trunk_branch.c || (trunk_branch.c & 1))
				fatal_error("invalid revision number: %s\n",
					optarg);

			project_branches->number = trunk_branch;
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
