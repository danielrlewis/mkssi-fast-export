# README

`mkssi-fast-export` exports revision control history from an MKS Source
Integrity (MKSSI) project as a stream of `git fast-import` commands that will
recreate the project and its history as a Git repository.

## MKSSI Versions Supported

`mkssi-fast-export` only works with the MKSSI versions that stored revision
history in RCS (Revision Control System) format.  It has mostly been tested with
projects that were created with MKSSI v7.5a, released in the year 2000.  Other
versions might also work.

Newer versions of MKS Source Integrity, since renamed to PTC Source Integrity,
store revision history in a database and are not supported by this tool, but
there may be other options for projects created with these newer versions: e.g.,
the Polarian Subversion Importer supports MKS2009 and later, and once the
project is in Subversion, going from there to Git is a well-trod path.

## Disclaimer

While `mkssi-fast-export` was meticulously tested with about 100 MKSSI projects
that were available to its author, that does not mean it will work for every
MKSSI project under the sun.  MKSSI was an enterprise product with many
features, some of which are known to be unsupported by this program (see the
"Bugs" section).  Furthermore, MKSSI projects can get into weird states and have
unusual edge cases, which might not be handled correctly if such states/cases
did not exist in the projects that `mkssi-fast-export` was tested with.

## Building

To build `mkssi-fast-export`, you must use a Linux system (or Cygwin) with Flex
and Bison installed, along with a C compiler.  Run `make` to create the
executable.

## Usage

Run `mkssi-fast-export --help` for a terse usage summary.

To convert a repository, run something like the following sequence of commands:

	$ mkdir foobar.git
	$ cd foobar.git
	$ git init --bare
	$ mkssi-fast-export \
		--rcs-dir=../foobar_mkssi_rcs \
		--proj-dir=../foobar_mkssi_proj \
		--authormap=../authors.txt \
		--source-dir=x:/mks/projects/MKS/foobar \
		--pname-dir=X:/MKS/foobar \
		| git fast-import
	$ git gc --aggressive --prune=now
	$ git fsck

### Parameters

`--rcs-dir` is the only required argument.  It specifies the MKSSI RCS
directory, which is the directory where all the RCS revision history is located.
When MKSSI creates a new project, the RCS directory is created automatically in
a preconfigured location.  If you cannot find the RCS directory, talk to your
local administrator or MKSSI guru.  On a large project with a long history, the
RCS directory will be large and `mkssi-fast-export` reads from it constantly.
If it resides on a network file system, copy it to a local file system to speed
up the export.

`--proj-dir` specifies the MKSSI project directory.  This is the directory that
MKSSI users typically interact with: for example, when creating a new sandbox,
MKSSI will ask the user to specify a project.pj file.  The directory containing
the project.pj file is this project directory.   This directory contains very
little of the revision history, but it is needed in order to export changes that
are not included in any MKSSI checkpoint.  If this option isn't specified,
`mkssi-fast-export` will only export the checkpointed history.

`--authormap` is highly recommended: it maps from MKSSI usernames to the human
names and email addresses used for Git commit authors.  The format for the
author map is the same as `cvs-fast-export` (see its documentation), except that
a) the usernames are case-insensitive; and b) the optional timezone field is not
currently implemented and will be ignored if present.  In brief, the author map
is a text file with lines like the following:

	johnd = John Doe <john.doe@example.com>

To find a list of usernames which should be in the author map, run the
following:

	$ mkssi-fast-export --authorlist --rcs-dir=foobar_mkssi_rcs

You can use `--authorlist` and `--authormap` at the same time to find only the
usernames which are not already in the authormap.

#### Keyword Parameters

`--source-dir` and `--pname-dir` are only used for keyword expansion.  If you
want the Git repository to be identical to the MKSSI project, it's important to
specify these parameters so that the relevant keywords are expanded in exactly
the same way as MKSSI would have done.  `--source-dir` is a path to the RCS
directory, used for the `$Source$` and `$Header$` RCS keywords.  `--proj-dir` is
a path to the project directory, used for the `$ProjectName$` keyword.  As the
example shows, these should be quasi-Windows style paths to emulate the behavior
of MKSSI on the Windows platform.  However, there are idiosyncrasies in how
MKSSI expands these paths.

For an actual RCS directory with a Windows path of "X:\MKS\PROJECTS\MKS\foobar",
MKSSI has been observed to expand the `$Source$` and `$Header$` keywords to
include "x:/mks/projects/MKS/foobar".  Note that the path separators became
forward slashes, and the drive letter and some of the directory names became
lower case.

For an actual project directory with a Windows path of "X:\MKS\foobar", MKSSI
has been observed to expand the `$ProjectName$` keyword to "X:/MKS/foobar".
Unlike the RCS directory path, the original capitalization was retained.

If you still have a functional MKSSI installation, you can create an
experimental project and add to it a file with `$Source$` and `$ProjectName$`
keywords to see what the correct expansion of these keywords is in your
environment.

If you know for a fact that none of these keywords were ever used in the project
being exported, or if you don't care whether the keywords are expanded
correctly, you can ignore these parameters.

## Time and Resource Requirements

`mkssi-fast-export` might take a few minutes on large repositories; the longest
I have seen is thirty minutes.

`mkssi-fast-export` allocates lots of memory; I have seen it use over 1 GB.

## Bugs

Encrypted RCS archives are unsupported.  If seen, `mkssi-fast-export` will print
a warning and ignore the encrypted file.

Subprojects (either "included" or "subscribed") are unsupported.  If seen,
`mkssi-fast-export` will abort.

The timezone for commits is currently hard-coded to Pacific Standard Time
(`"-0800"`): see the `TIMEZONE` macro in export.c.

MKSSI branch and checkpoint names which are not legal in Git are munged to make
them so.  For example, spaces are changed to underscores.  There are edge cases
where this might result in duplicate names; `mkssi-fast-export` does not check
for such duplicates, so if they arise, the likely result would be a fatal error
from `git fast-import`.

## Challenges

MKSSI has characteristics which lower the quality of the history that can be
exported from its projects.

MKSSI, like CVS, tracks history on a per-file basis.  The per-file revision
history should be merged into multi-file commits when appropriate.  Unlike
modern versions of CVS, MKSSI does _not_ have commitids that can be used to
group per-file changes.  With `cvs-fast-import`, for projects without commitids,
the file revision timestamps are used to help merge the per-file revisions into
commits.  That approach doesn't work with MKSSI, because MKSSI uses the last
modification time (mtime) of a file as the revision timestamp.  This is
unfortunate, because the mtime can be much older (hours, days, months, even
years older) than the actual time of check-in.  Thus, the timestamps are not a
reliable indicator of when, or in what order, file revisions were checked-in to
the project.  This has far-reaching consequences.  Without commitids or accurate
timestamps, there are limited options for accurately merging file changes into
commits.  And without accurate timestamps, the true ordering of changes is
partially unknown, which may lead to out-of-order commits in the exported
history.

MKSSI does not store any metadata for deleted files.  There is no author,
timestamp, or revision history comment.  Likewise for files that are reverted to
an earlier revision.

MKSSI does not support rename operations, so users must delete files and re-add
them to effect a rename.

MKSSI does not permit the user to supply a revision history comment when a file
is added to the project.  In the RCS file, the `log` field for the initial
revision is always populated with "Initial revision".

MKSSI (at least the Windows and DOS versions) is case insensitive.  Projects
contain file listings which do not always use the same capitalization as the
on-disk file and directory names.  The capitalization of directory names in the
project file listing can be inconsistent; whichever capitalization variant is
used first becomes the canonical capitalization on the case-insensitive Windows
file system.  Furthermore, the revisioned file listings used for branches and
checkpoints will often list the same paths with different capitalization
variants.  In effect, the capitalization can change over time, and MKSSI does
not consider this to be a change to the project: "foo.c" and "FOO.C" are
considered to be the same file.

## Characteristics of Exported History

### Ordering Events

Since MKSSI timestamps are file mtimes that cannot be relied upon for ordering,
`mkssi-fast-export` leans heavily on checkpoints for this purpose.  Changes
referenced by earlier checkpoints must be older than changes referenced by newer
checkpoints.  For files that were updated multiple times in the course of a
single checkpoint, we also know that earlier revisions are older than later
revisions.  However, across files and in-between checkpoints, it's anyone's
guess as to which changes came first; MKSSI simply does not store the right
metadata to determine that.  Since change ordering in-between checkpoints cannot
be made accurate, `mkssi-fast-export` will order things in a way that is
convenient for per-file consistency: first commits which add files, then commits
which update files, and finally commits which delete files.  Within the set of
adds and updates, the timestamps are used for ordering; except that, for
updates, the file revision trumps the timestamp, so earlier revisions always
precede later revisions, even if the earlier revision has an newer timestamp.

This works okay for projects which have been checkpointed regularly (e.g., by a
nightly build process), but less so otherwise.  However, even in the best case,
there is no guarantee that commits in-between checkpoints represent a consistent
project state.  For example, if a given logical changeset adds, updates, and
deletes files, the exported history will break that changeset up into multiple
commits, possibly separated by unrelated commits.  Unfortunately, it is
essentially impossible to do better with the available metadata.

### Timestamps

For lack of a better alternative, MKSSI file revision timestamps are used as
commit timestamps for commits which add or update files.  Since these timestamps
are the file modification times, rather than the time of check-in, the commit
history may have timestamps which are non-sequential: a child commit may have an
older timestamp than a parent commit.

MKSSI does not have timestamps for file deletions.  `mkssi-fast-export` uses the
timestamp of the subsequent checkpoint for deletion commit timestamps.  If there
is no subsequent checkpoint, `mkssi-fast-export` uses the mtime of the branch's
project file.

### Authorship

MKSSI revisions have a username for the author.  If an author map is provided,
and the username is listed in the author map, then the human name and email
address provided for that username will be used for authorship of file revisions
by that username.

If there is no author map, or the username is not listed in the author map, then
the username will be used as both the human name and the email address.

For events with no authorship metadata, most notably file deletions, the human
name is listed as "Unknown" and the email address as "unknown".

The demarcating tags (see below) and certain auto-generated commits will use an
author with the human name and address of "mkssi-fast-export", to indicate that
this tool was the author.

### Merging

`mkssi-fast-export` will, in some cases, merge changes to individual files into
commits that change multiple files.  Changes are only merged within a
checkpoint, never across checkpoint boundaries.  The merging isn't always
satisfactory, since MKSSI provides very little metadata that can be used to
recognize that separate per-file changes are part of a set of related changes.

Files that are added to the project within the same checkpoint are merged if the
author is the same.  Since the revision history comment for added files is
always "Initial revision", the comment is not used for merging.  Since the
timestamp is the mtime of the file, which can be years apart even for files that
were added within seconds of each other, no attempt is made to use the timestamp
for merging adds.

Files which are updated within the same checkpoint are merged if the author is
the same and the revision history comment is identical.  This is not a perfect
system, since oftentimes related changes that would ideally be in the same
commit will not be merged, because the revision history comments are slightly
different.  Since the timestamp is the mtime of the file, which can be years
apart even for files that were updated within seconds of each other, no attempt
is made to use the timestamp for merging updates.

Files which are deleted within the same checkpoint are all merged into the same
commit, because there is no authorship, comment, or timestamp metadata which
could be used to merge them into smaller sets.

### Commit Messages

Updated files will use the MKSSI revision history comment as the commit message.
No attempt is made to reformat these revision history comments to adhere to Git
conventions.  If the revision history comment is empty, `mkssi-fast-export` will
generate a commit message of the form "Updated file foo.c" or "Updated 42
files".

Added files have a useless "Initial revision" revision history comment.  Instead
of using that, `mkssi-fast-export` will generate a commit message of the form
"Added file foo.c" or "Added 42 files".

Deleted files have no revision history comment.  `mkssi-fast-export` will
generate a commit message of the form "Deleted file foo.c" or "Deleted 42
files".

Rarer events, such as files reverted to an earlier revision, will also have an
automatically generated commit message.

The MKSSI metadata for the changed files is appended to the end of the commit
message.  The rationale for doing this is that if a file revision is mentioned
in email or a bug tracker (e.g., "Fixed this problem in foo.c rev. 1.42"), it
should be possible to search the commit messages to find that change.  A commit
message for a set of updated files might look something like this:

	Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod
	tempor incididunt ut labore et dolore magna aliqua.  Ut enim ad minim
	veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea
	commodo consequat.

	#mkssi: check-in foo/bar/asdf.h rev. 1.26 (was rev. 1.25)
	#mkssi: check-in foo/bar/asdf.c rev. 1.126 (was rev. 1.125)
	#mkssi: check-in include/foobar.h rev. 1.13 (was rev. 1.12)

### Renaming to Change Capitalization

`mkssi-fast-export` will detect when the capitalization of a directory name or
file name has implicitly changed, and generate commits which explicitly rename
the affected files.  If the files have RCS keywords that expand to a name or
path which is affected by the rename, the file will be updated with the new
expansion of those RCS keywords as part of the same rename commit.

These capitalization changes will be the only rename operations in the exported
history, since MKSSI itself doesn't have rename operations.

### Branches and Tags

MKSSI branches are exported as Git branches.  MKSSI checkpoints are exported as
Git annotated tags.  MKSSI allows nearly any character in branch or checkpoint
names; Git is more restrictive.  Thus, the MKSSI branch and checkpoint names
might be munged to make legal Git branch and tag names.  Most frequently, spaces
are changed to underscores.

#### Demarcating Tags

`mkssi-fast-export` will automatically tag the final commit on each branch with
an annotated tag named "BranchName\_mkssi".  The idea is to mark the boundary
between the MKSSI history and the subsequent native Git history.

### RCS Keyword Expansion

`mkssi-fast-export` implements RCS keyword expansion, including for proprietary
MKSSI keywords like `$ProjectRevision$` and `$ProjectName$`.  The implementation
is bug-for-bug compatible with MKSSI RCS keyword expansion, and thus does not
always behave the same as GNU RCS keyword expansion.

As the exported repository is updated with native Git commits, the RCS keywords
will no longer be expanded, so their expanded values will become stale.  Thus,
it might be worthwhile to remove the RCS keywords from the source code; for a
large project, doing this via a script is recommended.

## Corrupted MKSSI Projects

It is quite easy for an MKSSI project to become corrupt.  Inherently, the
RCS-based metadata has no checksums or built-in consistency checking, so file
system corruption that affects the project is likely to go unnoticed.  Also,
check-ins require updating multiple files, which is not an atomic operation, so
an interruption can lead to inconsistencies in the project.

`mkssi-fast-export` will tolerate many forms of project corruption, printing a
warning message to stderr and exporting what it can.  However, some forms of
project corruption will result in a fatal error; if you run into this, you will
need to update `mkssi-fast-export` to work around that form of corruption.

## Testing

If you have a functional MKSSI installation, you can test the Git repository by
comparing it to the MKSSI sandbox for the project.  Every branch in the Git
repository should, when checked-out, have files that are byte-for-byte identical
to the files in a sandbox for the equivalent MKSSI branch.  Every tag in the Git
repository should, when checked-out, have files that are byte-for-byte identical
to the files in a sandbox for the equivalent MKSSI checkpoint.  It is relatively
straightforward to write a script which checks-out all of the branches and
tags/checkpoints with both Git and MKSSI and then recursively diffs the trees.

## Copyright, License, and Derivative Code

The code in this distribution is copyright:

- (c) 2017, 2019-2020 by Tuxera US Inc
- (c) 2012 by Eric S. Raymond
- (c) 2006 by Keith Packard

`mkssi-fast-export` borrows code from `cvs-fast-export`; it is thus a derivative
work and, like the original, is licensed under the GNU GPL v2 or later.  The
following code was borrowed:

Several data structures in interfaces.h -- `struct rcs_number`, `struct
rcs_symbol`, `struct rcs_branch`, `struct rcs_version`, `struct rcs_text`,
`struct rcs_patch`, and `struct rcs_file` -- were based on data structures from
`cvs-fast-export`'s cvs.h.

lex.l and gram.y are both based on the `cvs-fast-export` files of the same name.
They have been modified to remove support for RCS features not used by MKSSI, to
add support for MKSSI RCS extensions, and to work with the altered data
structures.

Some of the functions in utils.c and rcs-number.c are from `cvs-fast-export`.
