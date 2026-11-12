/*
 * Wire format of the host-resolved file system, mirrored from the patched
 * PostgreSQL header src/include/embed_vfs.h — keep both in sync.  The library
 * cannot include that header: it redirects open/read/write and friends to the
 * client side, which is the last thing the host wants applied to itself.
 *
 * Each connected process gets a region of memory shared with the host, and
 * every exchange happens there: the request, the names it concerns, the reply
 * and the bytes moved all sit in the region.  The socket only carries a byte
 * saying "your turn" in each direction, plus the one thing memory cannot hold —
 * a descriptor, when an open is answered with the file itself.
 *
 * Moving the bytes out of the socket is what makes this fast: a page no longer
 * goes through the kernel twice and through socket buffers that macOS keeps at
 * 8kB, and the host's filesystem reads into and writes from the region itself.
 * The sending side's system call orders its stores before the other side's
 * receive returns, so the doorbell is also the only synchronisation needed.
 */
#ifndef EMBED_VFS_PROTOCOL_H
#define EMBED_VFS_PROTOCOL_H

#define EMBED_VFS_PATH_MAX 1024

/* Largest payload in one exchange; bigger transfers are split by the client */
#define EMBED_VFS_IO_MAX (1024 * 1024)

/* Most directory entries delivered in one exchange, in bytes */
#define EMBED_VFS_DIR_BATCH (64 * 1024)

#define EMBED_VFS_MAGIC     0x53465645  /* "EVFS" */
#define EMBED_VFS_VERSION   2

/* The byte on the socket */
#define EMBED_VFS_MSG_HELLO     'H'     /* host → client, carries the region */
#define EMBED_VFS_MSG_REQUEST   'Q'     /* client → host: a request is ready */
#define EMBED_VFS_MSG_REPLY     'A'     /* host → client: the reply is ready */

enum {
    EMBED_VFS_OP_GETATTR = 1,
    EMBED_VFS_OP_ACCESS,
    EMBED_VFS_OP_READLINK,      /* answer in data */
    EMBED_VFS_OP_MKDIR,
    EMBED_VFS_OP_UNLINK,
    EMBED_VFS_OP_RMDIR,
    EMBED_VFS_OP_SYMLINK,       /* path is the target, path2 the link */
    EMBED_VFS_OP_RENAME,        /* uses path2 */
    EMBED_VFS_OP_LINK,          /* uses path2 */
    EMBED_VFS_OP_CHMOD,
    EMBED_VFS_OP_UTIMENS,       /* atime in offset, mtime in length */
    EMBED_VFS_OP_STATFS,
    EMBED_VFS_OP_TRUNCATE,
    EMBED_VFS_OP_OPEN,          /* reply carries the handle */
    EMBED_VFS_OP_CREATE,        /* reply carries the handle */
    EMBED_VFS_OP_READ,          /* bytes read in data */
    EMBED_VFS_OP_WRITE,         /* bytes to write in data */
    EMBED_VFS_OP_FSYNC,
    EMBED_VFS_OP_FALLOCATE,
    EMBED_VFS_OP_RELEASE,
    EMBED_VFS_OP_OPENDIR,       /* reply carries the handle */
    EMBED_VFS_OP_READDIR,       /* a batch of embed_vfs_dirent in data */
    EMBED_VFS_OP_RELEASEDIR,
};

typedef struct embed_vfs_request {
    int                 op;
    int                 flags;      /* open flags, access mask, fsync datasync */
    unsigned int        mode;       /* open/mkdir/chmod mode */
    unsigned long long  fh;         /* the host's handle, when there is one */
    long long           offset;     /* read/write offset, utimens atime */
    long long           length;     /* byte count, truncate size, utimens mtime */
    long long           index;      /* readdir position */
} embed_vfs_request;

typedef struct embed_vfs_reply {
    int                 result;     /* 0 on success, -1 on failure */
    int                 err;        /* errno when result is -1 */
    int                 has_fd;     /* open: a descriptor came with the doorbell */
    unsigned long long  fh;         /* open/opendir: the host's handle */
    long long           length;     /* bytes in data */
    long long           index;      /* readdir: where the next batch starts */

    /* getattr */
    long long           size;
    long long           mtime;
    long long           atime;
    long long           ctime;
    long long           dev;
    long long           ino;        /* the backend compares these to identify files */
    long long           blocks;
    unsigned int        mode;
    unsigned int        nlink;
    unsigned int        uid;
    unsigned int        gid;

    /* statfs */
    unsigned long long  f_bsize;
    unsigned long long  f_blocks;
    unsigned long long  f_bfree;
    unsigned long long  f_bavail;
    unsigned long long  f_files;
    unsigned long long  f_ffree;
} embed_vfs_reply;

/* One directory entry in a readdir batch; `reclen` keeps the next one aligned */
typedef struct embed_vfs_dirent {
    long long           ino;
    unsigned int        mode;
    unsigned short      reclen;     /* this record, name and padding included */
    char                name[];     /* NUL-terminated */
} embed_vfs_dirent;

/* The region one process shares with the host */
typedef struct embed_vfs_shared {
    unsigned int        magic;
    unsigned int        version;
    embed_vfs_request   req;
    embed_vfs_reply     reply;
    char                path[EMBED_VFS_PATH_MAX];
    char                path2[EMBED_VFS_PATH_MAX];
    char                data[EMBED_VFS_IO_MAX] __attribute__((aligned(64)));
} embed_vfs_shared;

#endif /* EMBED_VFS_PROTOCOL_H */
