/* Read revision data from plain-text MKSSI RCS files */
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "interfaces.h"

/* buffer an RCS patch in a structured list of such patches */
struct rcs_patch_buffer {
	/*
	 * Parent patch.  This is the subsequent revision whose contents are
	 * derived from this revision.  For trunk revisions, this is a lower
	 * revision number; for branch revisions, a higher one.
	 */
	struct rcs_patch_buffer *parent;

	/*
	 * List of branches based on this revision.  For example, say this is
	 * the patch for revision 1.2.  branches would point at the first
	 * branch, say 1.2.1.1; that branch would have parent pointers leading
	 * to 1.2.1.2, 1.2.1.3, etc.  If there is more than one branch based
	 * on revision 1.2, the branch_next will be used.  So for revision 1.2,
	 * its branches->branch_next might lead to rev. 1.2.2.1 (which might
	 * also have its own parent revisions).
	 */
	struct rcs_patch_buffer *branches;
	struct rcs_patch_buffer *branch_next;

	/* RCS version and patch structures (for convenience) */
	const struct rcs_version *ver;
	const struct rcs_patch *patch;

	/* Raw text of the patch.  The lines buffer points into this. */
	char *text;

	/* Lines buffer for the patch */
	struct rcs_line *lines;
};

/* parse line number and line count from an RCS patch line */
static bool
get_lineno_and_count(const char *s, unsigned int *lineno, unsigned int *count)
{
	char *end;

	/* Expected format: number, space, number, newline/NUL */

	errno = 0;
	*lineno = (unsigned int)strtoul(s, &end, 10);
	if (end == s || *end != ' ' || errno)
		return false;

	s = end + 1;
	errno = 0;
	*count = (unsigned int)strtoul(s, &end, 10);
	if (end == s || (*end && *end != '\n') || errno)
		return false;

	return true;
}

/* patch the preceding revision to yield the new revision */
static struct rcs_line *
apply_patch(const struct rcs_file *file, const struct rcs_number *revnum,
	struct rcs_line *data_lines, struct rcs_line *patch_lines)
{
	struct rcs_line *pln;
	unsigned int ln, ct, i;
	char cmd;

	for (pln = patch_lines; pln;) {
		cmd = pln->line[0];

		/* Skip blank lines */
		if (cmd == '\n' || cmd == '\0') {
			pln = pln->next;
			continue;
		}

		/*
		 * RCS patches only have two commands: 'a' for insert and 'd'
		 * for delete.
		 */
		if (cmd != 'a' && cmd != 'd') {
			fprintf(stderr, "unrecognized patch command '%c' "
				"(0x%02x)\n", cmd, (unsigned int)cmd);
			goto error;
		}

		/*
		 * Both 'a' and 'd' have a line number and line count.  The line
		 * number is where the insert/delete starts.  Importantly, this
		 * line number is the *original* line number, prior to applying
		 * any changes from the patch.  The line count is the number of
		 * lines to insert/delete.
		 */
		if (!get_lineno_and_count(&pln->line[1], &ln, &ct)) {
			fprintf(stderr, "cannot parse line number and count\n");
			goto error;
		}

		if (cmd == 'a') {
			if (!lines_insert(&data_lines, pln->next, ln, ct)) {
				fprintf(stderr, "cannot insert lines\n");
				goto error;
			}

			/* Move past insert command */
			pln = pln->next;

			/* Move past inserted lines */
			for (i = 0; i < ct; ++i)
				pln = pln->next;
		} else if (cmd == 'd') {
			if (!lines_delete(data_lines, ln, ct)) {
				fprintf(stderr, "cannot delete lines\n");
				goto error;
			}

			/* Move past delete command */
			pln = pln->next;
		}
	}

	/*
	 * Once the patch is completely applied, we can remove deleted lines
	 * from the line buffer and renumber the lines.
	 */
	lines_reset(&data_lines);
	return data_lines;

error:
	fprintf(stderr, "cannot patch to \"%s\" rev. %s\n", file->name,
		rcs_number_string_sb(revnum));
	fprintf(stderr, "bad patch line %u: \"", pln->lineno);
	line_fprint(stderr, pln->line);
	fprintf(stderr, "\"\n");
	fatal_error("bad RCS patch");
	return NULL; /* unreachable */
}

/* read the text of an RCS patch from disk */
static char *
read_patch_text(const struct rcs_file *file, const struct rcs_patch *patch)
{
	ssize_t len;
	char *text;
	int fd;

	/*
	 * patch->text.length includes the opening/closing @ characters, which
	 * we do not want to read.
	 */
	len = patch->text.length - 2;
	text = xmalloc(len + 1, __func__);

	if ((fd = open(file->master_name, O_RDONLY)) == -1)
		fatal_system_error("cannot open \"%s\"", file->master_name);

	errno = 0;
	if (pread(fd, text, len, patch->text.offset + 1) != len)
		fatal_system_error("cannot read from \"%s\"",
			file->master_name);

	close(fd);

	text[len] = '\0';
	return text;
}

/* instantiate a patch buffer */
static struct rcs_patch_buffer *new_patch_buf(const struct rcs_file *file,
	const struct rcs_number *revnum)
{
	struct rcs_patch_buffer *pbuf;

	pbuf = xcalloc(1, sizeof *pbuf, __func__);
	pbuf->ver = rcs_file_find_version(file, revnum, true);
	pbuf->patch = rcs_file_find_patch(file, revnum, true);
	if (!pbuf->patch->missing) {
		pbuf->text = read_patch_text(file, pbuf->patch);
		pbuf->lines = string_to_lines(pbuf->text);
	}
	return pbuf;
}

/* read a file's patches from a given starting revision into patch buffers */
static struct rcs_patch_buffer *
read_patches_from_rev(const struct rcs_file *file,
	const struct rcs_number *startrev)
{
	struct rcs_number rev;
	struct rcs_patch_buffer *head, *pbuf, *br_pbuf;
	struct rcs_patch_buffer **parent_prev_next, **br_prev_next;
	const struct rcs_branch *b;

	parent_prev_next = &head;
	for (rev = *startrev; rev.c; rev = pbuf->ver->parent) {
		/* Read this patch into a patch buffer */
		pbuf = new_patch_buf(file, &rev);

		/* Recursively read any branch patch chains which start here */
		br_prev_next = &pbuf->branches;
		for (b = pbuf->ver->branches; b; b = b->next) {
			br_pbuf = read_patches_from_rev(file, &b->number);
			*br_prev_next = br_pbuf;
			br_prev_next = &br_pbuf->branch_next;
		}
		*br_prev_next = NULL;

		*parent_prev_next = pbuf;
		parent_prev_next = &pbuf->parent;
	}
	*parent_prev_next = NULL;
	return head;
}

/* read all of a file's patches into a list of patch buffers */
static struct rcs_patch_buffer *
read_patches(const struct rcs_file *file)
{
	/* Start reading from the head revision */
	return read_patches_from_rev(file, &file->head);
}

/* free a list of patch buffers */
static void
free_patch_buffers(struct rcs_patch_buffer *patches)
{
	struct rcs_patch_buffer *p, *pparent, *bp, *bpnext;

	/* Walk the list of revisions */
	for (p = patches; p; p = pparent) {
		/* Recursively free each branch which starts here */
		for (bp = p->branches; bp; bp = bpnext) {
			bpnext = bp->branch_next;
			free_patch_buffers(bp);
		}

		/* Free the patch text and lines */
		lines_free(p->lines);
		if (p->text)
			free(p->text);

		pparent = p->parent;
		free(p);
	}
}

/* pass file revision data to the callback*/
static void
emit_revision_data(rcs_revision_data_handler_t *callback,
	struct rcs_file *file, const struct rcs_version *ver,
	const struct rcs_patch *patch, const struct rcs_line *data_lines,
	bool has_member_type_other)
{
	struct rcs_line *data_lines_expanded;
	char *data;

	/*
	 * If the patch (or any of its antecedants) was missing from the RCS
	 * file, emit an empty revision.  This emulates how MKSSI handles
	 * RCS files that are corrupt in this manner.
	 */
	if (patch->missing) {
		callback(file, &ver->number, "", has_member_type_other);
		return;
	}

	/*
	 * Need to do RCS keyword expansion.  The provided data_lines may still
	 * be needed in their original form to patch to the subsequent revision,
	 * so make a copy for the expansion.
	 */
	data_lines_expanded = lines_copy(data_lines);

	/*
	 * No keyword expansion for "other" member types, but it is assumed (not
	 * much data) that "@@" characters still need to be un-escaped.
	 */
	if (has_member_type_other)
		rcs_data_unescape_ats(data_lines_expanded);
	else
		rcs_data_keyword_expansion(file, ver, patch,
			data_lines_expanded);

	/* Convert the data lines into a string and pass to the callback */
	data = lines_to_string(data_lines_expanded);
	callback(file, &ver->number, data, has_member_type_other);
	free(data);

	/* Free the copied data lines */
	lines_free(data_lines_expanded);
}

/* pass file revision data(s) to the callback */
static void
emit_revision(rcs_revision_data_handler_t *callback,
	struct rcs_file *file, const struct rcs_version *ver,
	const struct rcs_patch *patch, const struct rcs_line *data_lines)
{
	/*
	 * Rare special case: for text files with member type "other", MKSSI
	 * seems to grab rev. 1.1 without doing keyword expansion, so we need
	 * to export a special version without expanding keywords.
	 *
	 * Still need to export this rev. 1.1 with keyword expansion afterward,
	 * because it might also be needed as a normal member type "archive".
	 */
	if (file->has_member_type_other && !file->binary && ver->number.c == 2
	 && ver->number.n[0] == 1 && ver->number.n[1] == 1)
		emit_revision_data(callback, file, ver, patch, data_lines,
			true);

	emit_revision_data(callback, file, ver, patch, data_lines, false);
}

/* apply patches and pass the resulting revision data to the callback */
static struct rcs_line *
apply_patches_and_emit(rcs_revision_data_handler_t *callback,
	struct rcs_file *file, struct rcs_line *prev_data_lines,
	struct rcs_patch_buffer *patches)
{
	struct rcs_patch_buffer *p, *bp;
	struct rcs_line *branch_data_lines, *data_lines;

	data_lines = NULL;

	for (p = patches; p; p = p->parent) {
		if (prev_data_lines)
			/*
			 * Apply the patch to transmute the previous revision
			 * data lines into the data lines for the current
			 * revision.
			 */
			data_lines = apply_patch(file, &p->ver->number,
				prev_data_lines, p->lines);
		else
			/*
			 * Patch for the head revision is the data for that
			 * revision.
			 */
			data_lines = p->lines;

		/* Pass the revision data to the callback */
		emit_revision(callback, file, p->ver, p->patch, data_lines);

		/* Iterate through all branches which start at this revision */
		for (bp = p->branches; bp; bp = bp->branch_next) {
			/*
			 * Branch patches apply against data_lines, but since
			 * we still need that data for subsequent revisions on
			 * this level, make a copy.
			 */
			branch_data_lines = lines_copy(data_lines);

			/*
			 * Recursively apply patches and emit revision data for
			 * this chain of branch patches.
			 */
			branch_data_lines = apply_patches_and_emit(callback,
				file, branch_data_lines, bp);

			/* Free the copied data lines */
			lines_free(branch_data_lines);
		}

		prev_data_lines = data_lines;
	}

	/*
	 * data_lines will have changed from prev_data_lines if a) the latter
	 * was NULL; or b) if applying a patch changed the first line.  Return
	 * the possibly changed pointer so that it can be freed correctly.
	 */
	return data_lines;
}

/* read every RCS revision for a file, passing the data to the callback */
void
rcs_file_read_all_revisions(struct rcs_file *file,
	rcs_revision_data_handler_t *callback)
{
	struct rcs_patch_buffer *patches;

	/*
	 * Read every patch.  These must remain in memory until we are done
	 * with the file, since portions of a patch (the inserted lines) are
	 * incorporated into the text of subsequent revisions.
	 */
	patches = read_patches(file);

	/*
	 * Apply the patches in sequence and emit the resulting revision data
	 * to the callback.
	 */
	patches->lines = apply_patches_and_emit(callback, file, NULL, patches);

	/* Free the patch buffers */
	free_patch_buffers(patches);
}
