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
#include <stat.h>

/* Read a whole stream into a malloc'd, NUL-terminated buffer.
 * path == NULL reads stdin.  A regular file with a non-zero st_size is read up
 * to st_size, so a filesystem that does not report EOF on read() past the end
 * cannot make the caller loop forever.  A size-0 regular file (procfs/sysfs
 * virtual file) is read to EOF instead: its st_size is unknown, not empty.
 * Returns the buffer (caller frees) and stores the byte count in *len, or NULL
 * on error. */
char *cact_ub_read_all(const char *path, size_t *len);

/* Write the whole buffer, retrying short writes.  0 on success, -1 on error. */
int cact_ub_write_all(int fd, const void *buf, size_t len);

/* Shell control channel.  cactsole hands every child the write end of a pipe
 * in $CACT_CTL_FD and applies the requests it receives after each foreground
 * command (see cactsole's shell.c: ctl_apply).  This sends one "\n"-terminated
 * request line and appends the newline itself.  Returns 0 on success, -1 when
 * not running under cactsole (no channel) or on a write error. */
int cact_ub_shell_ctl(const char *line);

/* Callback for cact_ub_dir_foreach(): return non-zero to stop the walk. */
typedef int (*cact_ub_dirfn)(const char *full, const char *name, void *ud);

/* ── iterative tree walk ─────────────────────────────────────────────────────
 * Depth-first walk of a directory tree using an explicit heap stack instead of
 * recursion, so depth is limited by memory rather than by the 16 KiB user
 * stack.  `sel` (optional) decides which entries take part at all — they are
 * dropped from the walk, so "the last entry of a directory" means the last one
 * that survived the filter.  `enter` is called for every remaining entry below
 * `root` (never for `root` itself) in pre-order with that entry's own lstat;
 * return non-zero to stop the whole walk.  `leave` is called for every
 * directory (root included) in post-order once its entries are done — that is
 * what `du` sums with.  `max_depth` limits how deep entries are reported and
 * descended (-1 = no limit).  Returns 0, 1 if a callback stopped the walk,
 * -1 on error. */
typedef int (*cact_ub_walk_sel)(const char *name, const struct stat *st, void *ud);
typedef int (*cact_ub_walk_enter)(const char *full, const char *name, int depth,
                                  int is_last, const struct stat *st, void *ud);
typedef int (*cact_ub_walk_leave)(const char *full, int depth, void *ud);

typedef struct {
    int              max_depth;   /* deepest level reported, -1 = unlimited */
    cact_ub_walk_sel sel;         /* optional: which entries take part at all */
    int              sort;        /* sort each directory's entries by name */
} cact_ub_walk_opts_t;

int cact_ub_tree_walk(const char *root, int root_depth,
                      const cact_ub_walk_opts_t *opts,
                      cact_ub_walk_enter enter, cact_ub_walk_leave leave, void *ud);

/* Call fn for every entry of dir, skipping "." and "..".  full is the joined
 * path (path-safe for any depth).  Returns 0, or the first non-zero value the
 * callback returned; -1 if the directory cannot be read. */
int cact_ub_dir_foreach(const char *dir,
                        int (*fn)(const char *full, const char *name, void *ud),
                        void *ud);

/* Human-readable size: 0 -> "0", 532 -> "532", 1536 -> "1.5K", 3 MiB -> "3.0M". */
void cact_ub_human_size(unsigned long long v, char *out, size_t n);

/* Parse a size: decimal or 0x-hex, with an optional K/M/G (1024-based) or b
 * (512) suffix.  Returns the value as a long. */
long cact_ub_parse_size(const char *s);

/* mkdir -p: create every missing component of path.  0 on success, -1 on error. */
int cact_ub_mkdir_p(const char *path, int mode);

/* 1 if path exists and is a directory, 0 otherwise. */
int cact_ub_is_dir(const char *path);

/* Stream src into dst (created/truncated with `mode`).  Returns 0 on success,
 * -1 on error with errno describing the failing step.  Copying a directory
 * fails; the caller handles recursion. */
int cact_ub_copy_file(const char *src, const char *dst, int mode);

#endif /* CACT_UB_UTIL_H */
