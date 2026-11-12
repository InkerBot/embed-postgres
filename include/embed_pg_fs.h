/*
 * The filesystem PostgreSQL runs on.
 *
 * The shape of this interface is borrowed from libfuse — operations named by
 * path, an open file carried in a small info struct the filesystem fills in, a
 * directory delivered through a filling callback, and a negative errno for
 * failure.  That design has had a long time to become complete, which is worth
 * more here than inventing something.  The names and the types are this
 * library's own, though: nothing is shared with libfuse and nothing is
 * compatible with it.
 *
 * Callbacks are invoked in the host process.  There is no mount, no /dev/fuse
 * and no kernel involvement — PostgreSQL's file operations are carried to the
 * host over a socket and land here.
 *
 * Both the names and the contents are the filesystem's to decide: nothing has
 * to correspond to a file on disk, or to a file system at all.  Paths always
 * arrive absolute.
 */
#ifndef EMBED_PG_FS_H
#define EMBED_PG_FS_H

#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct flock;

/*
 * An open file or directory.
 *
 * `fh` is the filesystem's own handle, set by open/create/opendir and handed
 * back on every later call for that file.  Nothing outside the filesystem
 * interprets it.
 */
typedef struct embed_pg_fs_file_info {
    int         flags;          /* the open(2) flags that were requested */
    uint64_t    fh;             /* the filesystem's handle */
} embed_pg_fs_file_info;

/*
 * Reports one directory entry.  `st` may be NULL, or may carry only st_ino and
 * the file type in st_mode.  Returns non-zero to stop the listing early.
 */
typedef int (*embed_pg_fs_fill_dir)(void *buf, const char *name,
                                    const struct stat *st);

/*
 * What a filesystem can read about the call it is serving.  Available for the
 * duration of a callback and no longer.
 */
typedef struct embed_pg_fs_context {
    uid_t       uid;
    gid_t       gid;
    pid_t       pid;
    mode_t      umask;
    void       *private_data;   /* embed_pg_options.fs_private_data */
} embed_pg_fs_context;

const embed_pg_fs_context *embed_pg_fs_get_context(void);

/*
 * Filesystem operations.
 *
 * Every callback returns 0 on success and a negative errno on failure, except
 * read and write, which return a byte count, and lseek, which returns the
 * resulting offset.  A NULL callback means the operation is not offered, and is
 * reported to PostgreSQL as ENOSYS.
 *
 * `fi` is NULL where the caller named the file by path and had no handle to
 * give — getattr and truncate reach files both ways.
 *
 * Callbacks run on a thread of the library's own and may be entered
 * concurrently on behalf of different server processes, so they must be
 * thread-safe.
 *
 * PostgreSQL reaches for getattr, access, open, create, read, write, truncate,
 * fsync, release, opendir, readdir, releasedir, mkdir, unlink, rmdir, rename,
 * chmod, symlink, readlink, link, utimens and statfs.  The rest may be left
 * NULL unless the cluster is doing something unusual.
 */
typedef struct embed_pg_fs {
    /* ---- Attributes ---- */
    int (*getattr)(const char *path, struct stat *st,
                   embed_pg_fs_file_info *fi);
    int (*access)(const char *path, int mask);
    int (*readlink)(const char *path, char *buf, size_t size);
    int (*statfs)(const char *path, struct statvfs *st);

    /* ---- Names ---- */
    int (*mknod)(const char *path, mode_t mode, dev_t dev);
    int (*mkdir)(const char *path, mode_t mode);
    int (*unlink)(const char *path);
    int (*rmdir)(const char *path);
    int (*symlink)(const char *target, const char *linkpath);
    int (*rename)(const char *from, const char *to, unsigned int flags);
    int (*link)(const char *from, const char *to);
    int (*chmod)(const char *path, mode_t mode, embed_pg_fs_file_info *fi);
    int (*chown)(const char *path, uid_t uid, gid_t gid,
                 embed_pg_fs_file_info *fi);
    int (*truncate)(const char *path, off_t size, embed_pg_fs_file_info *fi);
    int (*utimens)(const char *path, const struct timespec tv[2],
                   embed_pg_fs_file_info *fi);

    /* ---- Open files ---- */
    int (*create)(const char *path, mode_t mode, embed_pg_fs_file_info *fi);
    int (*open)(const char *path, embed_pg_fs_file_info *fi);

    /*
     * Optional, and the only reason to prefer it: a descriptor the server may
     * use directly.
     *
     * Every other operation here costs a round trip to this process, which is
     * an order of magnitude more than the read it stands for.  A filesystem
     * whose contents can be named by a descriptor — an anonymous memory file, a
     * shared memory object, a real file — can return one instead, and the
     * server will read, write and sync it itself at the speed it would have had
     * with no filesystem at all.  The descriptor is the caller's to close.
     *
     * Returns a descriptor, or a negative errno.  Returning -ENOSYS or -EOPNOTSUPP
     * for a particular path falls back to open/create and the slow path, so the
     * choice can be made per file.
     */
    int (*open_fd)(const char *path, int flags, mode_t mode);
    int (*read)(const char *path, char *buf, size_t size, off_t offset,
                embed_pg_fs_file_info *fi);
    int (*write)(const char *path, const char *buf, size_t size, off_t offset,
                 embed_pg_fs_file_info *fi);
    int (*flush)(const char *path, embed_pg_fs_file_info *fi);
    int (*fsync)(const char *path, int datasync, embed_pg_fs_file_info *fi);
    int (*fallocate)(const char *path, int mode, off_t offset, off_t length,
                     embed_pg_fs_file_info *fi);
    off_t (*lseek)(const char *path, off_t offset, int whence,
                   embed_pg_fs_file_info *fi);
    int (*release)(const char *path, embed_pg_fs_file_info *fi);

    /* ---- Directories ---- */
    int (*opendir)(const char *path, embed_pg_fs_file_info *fi);
    int (*readdir)(const char *path, void *buf, embed_pg_fs_fill_dir filler,
                   embed_pg_fs_file_info *fi);
    int (*fsyncdir)(const char *path, int datasync, embed_pg_fs_file_info *fi);
    int (*releasedir)(const char *path, embed_pg_fs_file_info *fi);

    /* ---- Extended attributes ---- */
    int (*setxattr)(const char *path, const char *name, const char *value,
                    size_t size, int flags);
    int (*getxattr)(const char *path, const char *name, char *value,
                    size_t size);
    int (*listxattr)(const char *path, char *list, size_t size);
    int (*removexattr)(const char *path, const char *name);

    /* ---- Locking ---- */
    int (*lock)(const char *path, embed_pg_fs_file_info *fi, int cmd,
                struct flock *lock);
    int (*flock)(const char *path, embed_pg_fs_file_info *fi, int op);

    /* ---- Lifecycle ---- */
    void *(*init)(void *private_data);
    void (*destroy)(void *private_data);
} embed_pg_fs;

/* Everything resolved against the real file system; used when none is given */
extern const embed_pg_fs embed_pg_fs_passthrough;

#ifdef __cplusplus
}
#endif

#endif /* EMBED_PG_FS_H */
