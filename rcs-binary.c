/* Read revision data from binary MKSSI RCS files */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "interfaces.h"

/* represent data from RCS masters of binary files */
struct binary_data {
	unsigned char *buf; /* may include NUL bytes */
	size_t len, maxlen;
};

/* buffer an RCS patch in a structured list of such patches */
struct rcs_binary_patch_buffer {
	/*
	 * Parent patch.  This is the subsequent revision whose contents are
	 * derived from this revision.  For trunk revisions, this is a lower
	 * revision number; for branch revisions, a higher one.
	 */
	struct rcs_binary_patch_buffer *parent;

	/*
	 * List of branches based on this revision.  For example, say this is
	 * the patch for revision 1.2.  branches would point at the first
	 * branch, say 1.2.1.1; that branch would have parent pointers leading
	 * to 1.2.1.2, 1.2.1.3, etc.  If there is more than one branch based
	 * on revision 1.2, the branch_next will be used.  So for revision 1.2,
	 * its branches->branch_next might lead to rev. 1.2.2.1 (which might
	 * also have its own parent revisions).
	 */
	struct rcs_binary_patch_buffer *branches;
	struct rcs_binary_patch_buffer *branch_next;

	/* RCS version and patch structures (for convenience) */
	const struct rcs_version *ver;
	const struct rcs_patch *patch;

	/* Content of the patch */
	struct binary_data text;
};

/* grow buffer until it can store at least min_capacity bytes */
static void
buffer_grow(struct binary_data *vb, size_t min_capacity)
{
	if (vb->maxlen >= min_capacity)
		return;

	if (!vb->maxlen)
		vb->maxlen = 4096;
	while (vb->maxlen < min_capacity)
		vb->maxlen *= 2;

	vb->buf = xrealloc(vb->buf, vb->maxlen, __func__);
}

/* insert range of bytes into variable-length data buffer */
static bool
buffer_insert(struct binary_data *vb, const unsigned char *buf, size_t off,
	size_t len)
{
	if (off > vb->len) {
		fprintf(stderr,
			"buffer_insert at %zu beyond end of buffer at %zu\n",
			off, vb->len);
		return false;
	}
	buffer_grow(vb, vb->len + len);
	memmove(&vb->buf[off + len], &vb->buf[off], vb->len - off);
	memcpy(&vb->buf[off], buf, len);
	vb->len += len;
	return true;
}

/* delete range of bytes from variable-length data buffer */
static bool
buffer_delete(struct binary_data *vb, size_t off, size_t len)
{
	if (off + len > vb->len) {
		fprintf(stderr, "buffer_delete offset + length "
			"(%zu + %zu = %zu) longer than varbuf (%zu)\n",
			off, len, off + len, vb->len);
		return false;
	}
	memmove(&vb->buf[off], &vb->buf[off + len],
		vb->len - (off + len));
	vb->len -= len;
	return true;
}

/* instantiate a copy of a variable-length data buffer */
static void
buffer_copy(const struct binary_data *vb, struct binary_data *vbcopy)
{
	if (vb->buf) {
		vbcopy->buf = xmalloc(vb->len, __func__);
		memcpy(vbcopy->buf, vb->buf, vb->len);
	}
	vbcopy->len = vbcopy->maxlen = vb->len;
}

/* parse offset/length for RCS insert ('a') or delete ('d') commands */
static unsigned int
get_offset_length(const unsigned char *buf, size_t *off, size_t *len)
{
	const char *str;
	char *end, errnum[16];

	str = (const char *)buf;

	errno = 0;
	*off = (size_t)strtoul(str, &end, 10);
	if (end == str || *end != ' ' || errno) {
		memcpy(errnum, str, sizeof errnum - 1);
		errnum[sizeof errnum - 1] = '\0';
		fatal_system_error("bad offset number starting at \"%s\"",
			errnum);
	}
	str = end + 1;

	errno = 0;
	*len = (size_t)strtoul(str, &end, 10);
	if (end == str || *end != '\n' || errno) {
		memcpy(errnum, str, sizeof errnum - 1);
		errnum[sizeof errnum - 1] = '\0';
		fatal_system_error("bad length number starting at \"%s\"",
			errnum);
	}
	str = end + 1;

	return (const unsigned char *)str - buf;
}

/* apply a patch for a binary file that is stored by reference */
static void
apply_reference_patch(const struct rcs_file *file,
	struct rcs_binary_patch_buffer *pbuf, struct binary_data *data)
{
	char *master_dir_path, *refdir_path, *refrev_path;
	struct stat info;
	int fd;

	/*
	 * Ignore the patch text for now.  In the observed cases (a very small
	 * number), the patch text starts with an "rN M" command, where the
	 * meaning of N is unknown and M is the file size for the revision.  For
	 * revisions other than the head revision, that is followed by a "d"
	 * command which deletes the entirety of the previous revision; the "d"
	 * command seems to work like it is implemented in apply_patch().
	 * Finally, there is an "a" command which inserts the new data, with an
	 * offset of byte 1 (the first byte) and a length of the file size for
	 * the revision.  Unlike with apply_patch(), the "a" command has no
	 * data, because it comes from the reference file.
	 *
	 * None of that needs to be implemented here, because returning the
	 * contents of the reference file for each revision has the same effect
	 * (at least in the observed cases).
	 */

	master_dir_path = path_parent_dir(file->master_name);
	refdir_path = sprintf_alloc("%s/%s", master_dir_path,
		file->reference_subdir);

	if (stat(refdir_path, &info))
		fatal_system_error("missing reference directory \"%s\" for "
			" file \"%s\"", refdir_path, file->name);
	if (!S_ISDIR(info.st_mode))
		fatal_error("reference directory is not a directory: \"%s\"",
			refdir_path);

	refrev_path = sprintf_alloc("%s/%s", refdir_path,
		rcs_number_string_sb(&pbuf->ver->number));

	if (stat(refrev_path, &info)) {
		/*
		 * The reference file doesn't exist if the file is zero-sized
		 * for that revision.
		 */
		if (errno == ENOENT) {
			data->len = 0;
			goto out;
		}
		fatal_system_error("could not stat \"%s\"", refrev_path);
	}

	buffer_grow(data, info.st_size);
	data->len = info.st_size;

	if ((fd = open(refrev_path, O_RDONLY)) == -1)
		fatal_system_error("cannot open \"%s\"", refrev_path);

	errno = 0;
	if (read(fd, data->buf, data->len) != data->len)
		fatal_system_error("cannot read from \"%s\"", refrev_path);

	close(fd);

out:
	free(refrev_path);
	free(refdir_path);
	free(master_dir_path);
}

/* patch the preceding revision to yield the new revision */
static void
apply_patch(const struct rcs_file *file, struct rcs_binary_patch_buffer *pbuf,
	struct binary_data *data)
{
	const struct binary_data *patch;
	size_t i, j, adjust, off, len;
	unsigned int byte;

	/*
	 * Can't apply patch if it (or its antecedents) are missing from the RCS
	 * file.
	 */
	if (pbuf->patch->missing)
		return;

	/*
	 * If this file is stored by reference, there isn't really a patch at
	 * all; each revision is stored as a separate file.  Handle that
	 * separately.
	 */
	if (file->reference_subdir)
		return apply_reference_patch(file, pbuf, data);

	patch = &pbuf->text;

	/* Run through patch diff and merge changes into data */
	adjust = 0;
	for (i = 0; i < patch->len;) {
		if (patch->buf[i] == 'd') {
			++i;
			i += get_offset_length(&patch->buf[i], &off, &len);
			if (!buffer_delete(data, off - 1 + adjust, len))
				goto error;
			adjust += len;
		} else if (patch->buf[i] == 'a') {
			++i;
			i += get_offset_length(&patch->buf[i], &off, &len);
			if (!buffer_insert(data, &patch->buf[i], off - adjust,
			 len))
				goto error;
			adjust -= len;
			i += len;
		} else {
			fprintf(stderr, "unknown patch command 0x%02x at %zu\n",
				patch->buf[i], i);
			goto error;
		}
	}
	return;

error:
	fprintf(stderr, "cannot patch to \"%s\" rev. %s\n", file->name,
		rcs_number_string_sb(&pbuf->ver->number));
	fprintf(stderr, "context: ");
	for (j = i-min(i, 16); j < min(i+16, patch->len); ++j) {
		byte = patch->buf[j];
		if (j == i)
			fprintf(stderr, "<%02x> ", byte);
		else
			fprintf(stderr, "%02x ", byte);
	}
	fprintf(stderr, "\n");
	fatal_error("bad binary RCS patch");
}

/* read the text of an RCS patch from disk */
static struct binary_data
read_patch_text(const struct rcs_file *file, const struct rcs_patch *patch)
{
	struct binary_data text;
	int fd;

	/*
	 * patch->text.length includes the opening/closing @ characters, which
	 * we do not want to read.
	 */
	text.len = text.maxlen = patch->text.length - 2;

	if (!text.len) {
		text.buf = NULL;
		return text;
	}

	text.buf = xmalloc(text.len, __func__);

	if ((fd = open(file->master_name, O_RDONLY)) == -1)
		fatal_system_error("cannot open \"%s\"", file->master_name);

	errno = 0;
	if (pread(fd, text.buf, text.len, patch->text.offset + 1) != text.len)
		fatal_system_error("cannot read from \"%s\"",
			file->master_name);

	close(fd);

	return text;
}

/* unescape double-@@ characters to single-@ */
static void
rcs_binary_data_unescape_ats(struct binary_data *data)
{
	size_t *atat_positions, atats, max_atats, i, end;

	if (!data->len)
		return;

	/* find the location of every @@ */
	atats = max_atats = 0;
	atat_positions = NULL;
	for (i = 0; i < data->len - 1; ++i) {
		if (data->buf[i] != '@' || data->buf[i+1] != '@')
			continue;
		if (atats == max_atats) {
			if (max_atats)
				max_atats *= 2;
			else
				max_atats = 256;
			atat_positions = xrealloc(atat_positions,
				max_atats * sizeof *atat_positions, __func__);
		}
		atat_positions[atats++] = i++;
	}

	/*
	 * Memmove to change every @@ to @, such that no region of memory is
	 * moved more than once.  For a large binary file, this is much faster
	 * memmoving up to the end of the buffer for every @@.
	 */
	for (i = 0; i < atats; ++i) {
		if (i == atats - 1)
			end = data->len;
		else
			end = atat_positions[i + 1];

		memmove(&data->buf[atat_positions[i] - i],
			&data->buf[atat_positions[i] + 1],
			end - (atat_positions[i] + 1));
	}
	data->len -= atats;
}

/* instantiate a patch buffer */
static struct rcs_binary_patch_buffer *new_patch_buf(
	const struct rcs_file *file, const struct rcs_number *revnum)
{
	struct rcs_binary_patch_buffer *pbuf;

	pbuf = xcalloc(1, sizeof *pbuf, __func__);
	pbuf->ver = rcs_file_find_version(file, revnum, true);
	pbuf->patch = rcs_file_find_patch(file, revnum, true);
	if (!pbuf->patch->missing) {
		pbuf->text = read_patch_text(file, pbuf->patch);

		/*
		 * Double-@@ sequences are counted as a single byte for offsets
		 * and lengths, so it is better to remove them.
		 */
		rcs_binary_data_unescape_ats(&pbuf->text);
	}
	return pbuf;
}

/* read a file's patches from a given starting revision into patch buffers */
static struct rcs_binary_patch_buffer *
read_patches_from_rev(struct rcs_file *file, const struct rcs_number *startrev)
{
	struct rcs_number rev;
	struct rcs_binary_patch_buffer *head, *pbuf, *br_pbuf;
	struct rcs_binary_patch_buffer **parent_prev_next, **br_prev_next;
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
static struct rcs_binary_patch_buffer *
read_patches(struct rcs_file *file)
{
	/* Start reading from the head revision */
	return read_patches_from_rev(file, &file->head);
}

/* free a list of patch buffers */
static void
free_patch_buffers(struct rcs_binary_patch_buffer *patches)
{
	struct rcs_binary_patch_buffer *p, *pparent, *bp, *bpnext;

	/* Walk the list of revisions */
	for (p = patches; p; p = pparent) {
		/* Recursively free each branch which starts here */
		for (bp = p->branches; bp; bp = bpnext) {
			bpnext = bp->branch_next;
			free_patch_buffers(bp);
		}

		/* Free the patch data */
		if (p->text.buf)
			free(p->text.buf);

		pparent = p->parent;
		free(p);
	}
}

/* apply patches and pass the resulting revision data to the callback */
static void
apply_patches_and_emit(rcs_revision_binary_data_handler_t *callback,
	struct rcs_file *file, struct binary_data *data,
	struct rcs_binary_patch_buffer *patches)
{
	struct rcs_binary_patch_buffer *p, *bp;
	struct binary_data branch_data, ref_data;

	/*
	 * Files stored by reference don't have a head revision with the full
	 * contents of the file.  So, instead of using the text of the head
	 * revision as the initial data buffer, use an empty data buffer.
	 */
	if (file->reference_subdir && !data) {
		data = &ref_data;
		data->buf = NULL;
		data->len = data->maxlen = 0;
	}

	for (p = patches; p; p = p->parent) {
		if (data)
			/*
			 * Apply the patch to transmute the previous revision
			 * data into the data for the current revision.
			 */
			apply_patch(file, p, data);
		else
			/*
			 * Patch for the head revision is the data for that
			 * revision.
			 */
			data = &p->text;

		/* Pass the revision data to the callback */
		callback(file, &p->ver->number, data->buf, data->len, false);

		/*
		 * Binary files with member type "other" should use the copy of
		 * the file in the project directory.  If the other blob mark is
		 * still zero, however, then either the project directory is not
		 * available or the file doesn't exist in the project directory.
		 * In such cases, substitute the head revision from the RCS
		 * directory.
		 */
		if (file->has_member_type_other && !file->other_blob_mark &&
		 rcs_number_equal(&p->ver->number, &file->head))
			file->other_blob_mark = p->ver->blob_mark;

		/* Iterate through all branches which start at this revision */
		for (bp = p->branches; bp; bp = bp->branch_next) {
			/*
			 * Branch patches apply against current data, but since
			 * we still need that data for subsequent revisions on
			 * this level, make a copy.
			 */
			buffer_copy(data, &branch_data);

			/*
			 * Recursively apply patches and emit revision data for
			 * this chain of branch patches.
			 */
			apply_patches_and_emit(callback, file, &branch_data,
				bp);

			/* Free the copied data */
			free(branch_data.buf);
		}
	}
}

/* export file from project directory (for "other" member type) */
static void
export_projdir_revision(struct rcs_file *file,
	rcs_revision_binary_data_handler_t *callback)
{
	struct stat info;
	char *path;
	unsigned char *fdata;
	size_t flen;

	if (!mkssi_proj_dir_path)
		return;

	path = sprintf_alloc("%s/%s", mkssi_proj_dir_path, file->name);

	/* If the file exists in the project directory... */
	if (!stat(path, &info)) {
		fdata = file_buffer(path, &flen);
		callback(file, &file->head, fdata, flen, true);
		free(fdata);
	}
	free(path);
}

/* read every RCS revision for a binary file, passing data to the callback */
void
rcs_binary_file_read_all_revisions(struct rcs_file *file,
	rcs_revision_binary_data_handler_t *callback)
{
	struct rcs_binary_patch_buffer *patches;

	/*
	 * Special handling for binary files with member type "other": export
	 * the version of the file in the project directory.
	 */
	if (file->has_member_type_other)
		export_projdir_revision(file, callback);

	/* Dummy files have no RCS metadata, so nothing else can be exported. */
	if (file->dummy)
		return;

	/* Read every patch. */
	patches = read_patches(file);

	/*
	 * Apply the patches in sequence and emit the resulting revision data
	 * to the callback.
	 */
	apply_patches_and_emit(callback, file, NULL, patches);

	/* Free the patch buffers */
	free_patch_buffers(patches);
}
