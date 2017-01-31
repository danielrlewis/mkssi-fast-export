#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "interfaces.h"
#include "gram.h"
#include "lex.h"

const char *mkssi_dir_path;
struct rcs_file *files;
struct rcs_file *file_hash_table[1024];
struct rcs_file *corrupt_files;
struct rcs_file *project; /* project.pj */
struct rcs_symbol *project_branches;
struct cp_files *cp_files;

/* print usage to stderr and exit */
static void
usage(const char *name)
{
	fprintf(stderr, "usage: %s mkssi_input_dir\n", name);
	fprintf(stderr, "Fast-export history from an MKSSI (v7.5a) "
		"repository.\n");
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
	if (argc != 2)
		usage(argv[0]);

	/*
	 * This tells git fast-import that the stream is incomplete if we abort
	 * prior to sending the "done" command.
	 */
	printf("feature done\n");

	/* Validate the user-supplied project directory. */
	mkssi_dir_validate(argv[1]);
	mkssi_dir_path = argv[1];

	import();
	export();

	printf("done\n");

	exit(0);
}
