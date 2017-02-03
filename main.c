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

const char *mkssi_dir_path;
struct rcs_file *files;
struct rcs_file *file_hash_table[1024];
struct rcs_file *corrupt_files;
struct rcs_file *project; /* project.pj */
struct rcs_symbol *project_branches;
struct rcs_number trunk_branch;
bool author_list;

/* print usage to stderr and exit */
static void
usage(const char *name)
{
	fprintf(stderr, "usage: %s [options] mkssi_input_dir\n", name);
	fprintf(stderr, "Fast-export history from an MKSSI (v7.5a) "
		"repository.\n\n");
	fprintf(stderr, "The following options are supported:\n");
	fprintf(stderr, "  -h --help  This help message\n");
	fprintf(stderr, "  -A --authormap=file  Author map (same as "
		"cvs-fast-export)\n");
	fprintf(stderr, "  -a --authorlist  Dump authors not in author map and "
		"exit\n");
	fprintf(stderr, "  -b --trunk-branch=rev  Trunk branch revision number "
		"(trunk as branch)\n");
	exit(1);
}

/* validate the user-supplied MKSSI project directory */
static void
mkssi_dir_validate(const char *mkssi_dir)
{
	struct stat info;
	char path[1024]; /* big enough */
	char head[4];
	FILE *pjfile;

	if (stat(mkssi_dir, &info))
		fatal_system_error("cannot stat \"%s\"", mkssi_dir);
	if (!S_ISDIR(info.st_mode))
		fatal_error("not a directory: \"%s\"", mkssi_dir);

	snprintf(path, sizeof path, "%s/project.pj", mkssi_dir);
	if (!(pjfile = fopen(path, "r")))
		fatal_system_error("cannot open \"%s\"", path);

	/*
	 * MKSSI projects can be a bit confusing because there are two project
	 * directories, and the one we want is not the one the user normally
	 * interacts with.  The project directory which is normally used to
	 * create sandboxes and the like does not contain any revision history,
	 * only the newest copy of each file, project.pj included.  There is a
	 * second project directory which contains all of the RCS file masters,
	 * including the revisioned project.pj.  Because this is potentially
	 * confusing, we want to make sure that the project.pj file in the MKSSI
	 * directory is the revisioned version -- if revisioned, the first four
	 * bytes should be "head".
	 */
	errno = 0;
	if (fread(head, 1, sizeof head, pjfile) != sizeof head)
		fatal_system_error("cannot read from \"%s\"", path);
	if (strncmp(head, "head", 4))
		fatal_error("bad MKSSI directory: project.pj is not RCS");

	fclose(pjfile);
}

int
main(int argc, char *argv[])
{
	static const struct option options[] = {
		{ "help", no_argument, 0, 'h'},
		{ "authormap", required_argument, 0, 'A'},
		{ "authorlist", no_argument, 0, 'a'},
		{ "trunk-branch", required_argument, 0, 'b'},
		{ NULL }
	};
	int c;
	const char *author_map;

	author_map = NULL;
	for (;;) {
		c = getopt_long(argc, argv, "hA:ab:", options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'h':
			usage(argv[0]);
			break;
		case 'A':
			author_map = optarg;
			break;
		case 'a':
			author_list = true;
			break;
		case 'b':
			trunk_branch = lex_number(optarg);
			if (!trunk_branch.c || (trunk_branch.c & 1))
				fatal_error("invalid revision number: %s\n",
					optarg);

			project_branches = xcalloc(1, sizeof *project_branches,
				__func__);
			project_branches->symbol_name = xstrdup("master",
				__func__);
			project_branches->number = trunk_branch;
			break;
		default: /* error message already emitted */
			fprintf(stderr, "try `%s --help' for more "
				"information.\n", argv[0]);
            		exit(1);
		}
	}

	if (argc != optind + 1)
		usage(argv[0]);

	if (!author_list) {
		/*
		 * This tells git fast-import that the stream is incomplete if
		 * we abort prior to sending the "done" command.
		 */
		printf("feature done\n");
	}

	if (author_map)
		author_map_initialize(author_map);

	/* Validate the user-supplied project directory. */
	mkssi_dir_validate(argv[optind]);
	mkssi_dir_path = argv[optind];

	import();

	if (author_list) {
		dump_unmapped_authors();
		exit(0);
	}

	export();

	printf("done\n");

	exit(0);
}
