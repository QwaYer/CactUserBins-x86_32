/*
 * ex_util.h — small shared helpers for the CactUserBins utilities.
 *
 * Only the low-level pieces that several tools need land here: whole-file I/O
 * with a st_size bound for regular files, a directory-entry iterator, a
 * human-readable size formatter and a recursive mkdir.  Tool-specific logic
 * stays in its own ex_*.c.
 */

#ifndef CACT_UB_UTIL_H
#define CACT_UB_UTIL_H

#include <stddef.h>

/* Read a whole stream into a malloc'd, NUL-terminated buffer.
 * path == NULL reads stdin.  Regular files are read up to st_size, so a
 * filesystem that does not report EOF on read() past the end cannot make the
 * caller loop forever.  Returns the buffer (caller frees) and stores the byte
 * count in *len, or NULL on error. */
char *cact_ub_read_all(const char *path, size_t *len);

/* Write the whole buffer, retrying short writes.  0 on success, -1 on error. */
int cact_ub_write_all(int fd, const void *buf, size_t len);

/* Callback for cact_ub_dir_foreach(): return non-zero to stop the walk. */
typedef int (*cact_ub_dirfn)(const char *full, const char *name, void *ud);

/* Call fn for every entry of dir, skipping "." and "..".  full is the joined
 * path (path-safe for any depth).  Returns 0, or the first non-zero value the
 * callback returned; -1 if the directory cannot be read. */
int cact_ub_dir_foreach(const char *dir,
                        int (*fn)(const char *full, const char *name, void *ud),
                        void *ud);

/* Human-readable size: 0 -> "0", 532 -> "532", 1536 -> "1.5K", 3 MiB -> "3.0M". */
void cact_ub_human_size(unsigned long long v, char *out, size_t n);

/* mkdir -p: create every missing component of path.  0 on success, -1 on error. */
int cact_ub_mkdir_p(const char *path, int mode);

/* 1 if path exists and is a directory, 0 otherwise. */
int cact_ub_is_dir(const char *path);

/* Stream src into dst (created/truncated with `mode`).  Returns 0 on success,
 * -1 on error with errno describing the failing step.  Copying a directory
 * fails; the caller handles recursion. */
int cact_ub_copy_file(const char *src, const char *dst, int mode);

#endif /* CACT_UB_UTIL_H */
