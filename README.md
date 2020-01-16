README
======

mkssi-fast-export exports revision control history from an MKS Source Integrity
(MKSSI) project, version 7.5a or earlier, as a stream of git fast-import
commands that will recreate the project and its history as a Git repository.

MKSSI Versions Supported
-------------------------

mkssi-fast-export is designed specifically for MKSSI v7.5a (released in the year
2000), and has not been tested with any other version, but earlier versions
might work.  These old releases of MKSSI stored revision history in RCS
(Revision Control System) format, with extensions.

Newer versions of MKS Source Integrity, since renamed to PTC Source Integrity,
store revision history in a database and are not supported by this tool, but
there may be other options (the Polarian Subversion Importer supports MKS2009
and later, and converting Subversion to Git is a well-trod path).

Disclaimer
----------

Like most such programs, this one was improved until it was "good enough" to
convert one organization's projects to Git.  MKSSI was an enterprise product
which had features not used by these projects and not correctly handled by
mkssi-fast-export.  There are likely edge cases which mkssi-fast-export does not
handle correctly.

Limitations
-----------

mkssi-fast-export lacks support for these MKSSI features:
* Encrypted RCS archives: if seen, mkssi-fast-export will print a warning and
  not export changes for the encrypted file.
* Subprojects ("included" or "subscribed"): if seen, mkssi-fast-export will
  abort.

The timezone for MKSSI is currently hard-coded in export.c: see the TIMEZONE
macro.

MKSSI uses the last modification time (mtime) of a file as the revision
timestamp.  This is unfortunate, because the mtime can be much older (hours,
days, months -- even years older) than the actual time of check-in.  As a
result, the order of check-ins is not entirely known, and cannot be fully
preserved in the commits.  To compensate, mkssi-fast-export relies heavily upon
MKSSI checkpoints when it groups sets of revisions into commits.  The result
will look strange if the project was rarely checkpointed.

MKSSI branch and checkpoint names which are not legal in Git are munged to make
them so.  For example, spaces are changed to underscores.  There are edge cases
where this might result in duplicate names.  Note that MKSSI checkpoints are
exported as Git tags.

Many limitations with the resulting Git repositories are a result of MKSSI
limitations or differences between MKSSI and Git.  A couple of examples...
MKSSI does not record authorship when files are deleted, so file deletions will
have an unknown author in the Git repository.  MKSSI does not permit a user to
specify a check-in comment when adding files, so the commit messages in the Git
history for added files are generic.

Corrupted MKSSI Projects
------------------------

MKSSI projects have no checksums or built-in consistency checking, so it is
quite easy for them to become corrupt.  mkssi-fast-export will tolerate many
forms of project corruption, printing a warning message and exporting what it
can.  However, some forms of project corruption will result in a fatal error; if
you run into this, you will need to update mkssi-fast-export to workaround that
form of corruption.

Building
--------

To build mkssi-fast-export, you must use a Linux system (or Cygwin) with Flex
and Bison installed, along with a C compiler.  Run "make" to create the
executable.

Usage
-----

Run "mkssi-fast-export --help" for usage information.

To convert a repository, run something like the following sequence of commands:

	$ mkdir foobar.git
	$ cd foobar.git
	$ git init --bare
	$ mkssi-fast-export \
		--authormap=authors.txt \
		--source-dir=T:/mks/projects/mks/foobar \
		--proj-dir=../foobar_mkssi_proj
		--rcs-dir=../foobar_mkssi_rcs \
		| git fast-import
	$ git gc --aggressive --prune=now
	$ git fsck

--authormap is not required but highly recommended: it maps from MKSSI usernames
to the human names and email addresses used for Git commit authors.  The format
for the author map is the same as cvs-fast-export (see its documentation),
except that the optional timezone field is not currently implemented and will
be ignored if present.

To find a list of usernames which should be in the author map, run the
following:

        $ mkssi-fast-export --authorlist --rcs-dir=foobar_mkssi_rcs

--source-dir is used only to expand the $Source$ and $Header$ RCS keywords.  If
you want the Git repository to be identical to the MKSSI project, it's important
to specify the original location of the MKSSI RCS directory, so that those
keywords are expanded the same way that MKSSI would have done.  In the above
example, MKSSI was being used on Windows, and the RCS directory was on a mapped
network drive at T:/mks/projects/mks/foobar.

--proj-dir specifies the MKSSI project directory.  This is the directory that
MKSSI users typically interact with: for example, when creating a new sandbox,
MKSSI will ask the user to specify a project.pj file.  The directory containing
the project.pj file is this project directory.   This directory contains very
little of the revision history, but it is needed in order to export changes that
are not included in any MKSSI checkpoint.  If this option isn't specified,
mkssi-fast-export will only export the checkpointed history.

--rcs-dir specifies the MKSSI RCS directory, which is a separate directory where
all the RCS revision history is located.  When MKSSI creates a new project, the
RCS directory is created automatically in a preconfigured location.  If you
cannot find the RCS directory, talk to your local administrator or MKSSI guru.

On a large project with a long history, the --rcs-dir will be large and
mkssi-fast-export reads from it constantly.  If it resides on a network file
system, copy it to a local file system to speed up the export.  On the other
hand, only a handful of files are needed from --proj-dir, so accessing it
remotely will not slow down the export.

Time and Resource Requirements
------------------------------

mkssi-fast-export might take a few minutes on large repositories; the longest I
have seen is fifteen minutes.

mkssi-fast-export allocates lots of memory; I have seen it use over 1 GB.

Derivative Code
---------------

mkssi-fast-export borrows code from cvs-fast-export; it is thus a derivative
work and, like the original, is licensed under the GNU GPL v2 or later.  The
following code was borrowed:

Several data structures in interfaces.h -- struct rcs_number, struct rcs_symbol,
struct rcs_branch, struct rcs_version, struct rcs_text, struct rcs_patch, and
struct rcs_file -- were based on data structures from cvs-fast-export's cvs.h.

lex.l and gram.y are both based on the cvs-fast-export files of the same name.
They have been modified to remove support for RCS features not used by MKSSI, to
add support for MKSSI RCS extensions, and to work with the altered data
structures.

Some of the functions in utils.c and rcs-number.c are from cvs-fast-export.
