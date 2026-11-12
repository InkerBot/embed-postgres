/*
 * Host side of the file system PostgreSQL runs on.
 *
 * Each server process that connects gets a thread here, which answers what it
 * asks: it resolves names and moves bytes, through the embed_pg_fs callbacks
 * the caller supplied, in a region of memory the two share (see
 * embed_vfs_protocol.h).  Nothing in this file assumes those callbacks touch a
 * disk.
 *
 * The default set at the bottom does, though — it resolves everything against
 * the real file system, which is what a caller that never thinks about this
 * gets.
 */
#include "embed_postgres.h"
#include "embed_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * What a filesystem finds when it asks about the call it is serving.  Kept per
 * thread: each serving thread sets it once, before its first callback, and
 * nothing a callback does changes it.
 */
static __thread embed_pg_fs_context current_context;

const embed_pg_fs_context *
embed_pg_fs_get_context(void)
{
    return &current_context;
}

/*
 * Make `private_data` what the filesystem will find in its context.
 * Every path into a callback goes through here first — each serving thread as
 * it starts, and embed_pg_initdb() for the few it makes directly.
 */
void
embed_pg_fs_enter(void *private_data)
{
    memset(&current_context, 0, sizeof(current_context));
    current_context.uid = getuid();
    current_context.gid = getgid();
    current_context.pid = getpid();
    current_context.umask = 022;
    current_context.private_data = private_data;
}

struct embed_vfs_server {
    embed_pg_fs fs;
    void           *private_data;
    pthread_t       thread;         /* accepts connections */
    int             running;
    int             listen_fd;
    int             shutdown_fd;
    int             shutdown_write_fd;
    char            sock_path[PATH_MAX];

    /* Every connection whose thread has not been joined yet */
    pthread_mutex_t channels_mutex;
    struct channel *channels;
};

/*
 * One connected process, and the thread that serves it.
 *
 * Every process gets a thread of its own, so that one waiting on a slow
 * callback — an fsync, or a filesystem that goes to the network — holds up
 * nobody else.  The thread blocks on the socket between requests and costs
 * nothing while the process is idle.
 *
 * A process that exits does not release its files first — it simply goes away —
 * so the handles it left behind are closed here when its connection breaks.
 * Without that, every backend leaks whatever it had open into the host.
 */
struct channel {
    struct embed_vfs_server *server;
    pthread_t           thread;
    int                 fd;
    int                 done;       /* the thread has returned; join and free */
    embed_vfs_shared   *shared;     /* the region the requests arrive in */

    unsigned long long *handles;
    char              **paths;
    int                *isdir;
    int                 count;
    int                 capacity;

    /*
     * Directory listings, collected when the directory is opened.  readdir()
     * walks them in batches, but the filesystem interface delivers them all in
     * one call, so they are kept until the directory is released.  Only this
     * connection can name its handles, so they are kept here, unlocked.
     */
    struct dir_listing *dirs;

    struct channel     *next;
};

struct dir_listing {
    unsigned long long fh;
    char          **names;
    unsigned int   *modes;
    long long      *inos;
    int             count;
    int             capacity;
    struct dir_listing *next;
};

/* ------------------------------------------------------------------
 * The default: everything is a real file
 * ------------------------------------------------------------------ */

static int
real_getattr(const char *path, struct stat *st, embed_pg_fs_file_info *fi)
{
    if (fi != NULL && fi->fh != 0)
        return fstat((int) fi->fh, st) == 0 ? 0 : -errno;
    return stat(path, st) == 0 ? 0 : -errno;
}

static int
real_access(const char *path, int mask)
{
    return access(path, mask) == 0 ? 0 : -errno;
}

static int
real_readlink(const char *path, char *buf, size_t size)
{
    ssize_t n = readlink(path, buf, size - 1);

    if (n < 0)
        return -errno;
    buf[n] = '\0';
    return 0;
}

static int
real_mknod(const char *path, mode_t mode, dev_t dev)
{
    return mknod(path, mode, dev) == 0 ? 0 : -errno;
}

static int
real_mkdir(const char *path, mode_t mode)
{
    return mkdir(path, mode) == 0 ? 0 : -errno;
}

static int
real_unlink(const char *path)
{
    return unlink(path) == 0 ? 0 : -errno;
}

static int
real_rmdir(const char *path)
{
    return rmdir(path) == 0 ? 0 : -errno;
}

static int
real_symlink(const char *target, const char *linkpath)
{
    return symlink(target, linkpath) == 0 ? 0 : -errno;
}

static int
real_rename(const char *from, const char *to, unsigned int flags)
{
    if (flags != 0)
        return -EINVAL;         /* RENAME_NOREPLACE and friends are not offered */
    return rename(from, to) == 0 ? 0 : -errno;
}

static int
real_link(const char *from, const char *to)
{
    return link(from, to) == 0 ? 0 : -errno;
}

static int
real_chmod(const char *path, mode_t mode, embed_pg_fs_file_info *fi)
{
    if (fi != NULL && fi->fh != 0)
        return fchmod((int) fi->fh, mode) == 0 ? 0 : -errno;
    return chmod(path, mode) == 0 ? 0 : -errno;
}

static int
real_chown(const char *path, uid_t uid, gid_t gid, embed_pg_fs_file_info *fi)
{
    if (fi != NULL && fi->fh != 0)
        return fchown((int) fi->fh, uid, gid) == 0 ? 0 : -errno;
    return chown(path, uid, gid) == 0 ? 0 : -errno;
}

static int
real_truncate(const char *path, off_t size, embed_pg_fs_file_info *fi)
{
    if (fi != NULL && fi->fh != 0)
        return ftruncate((int) fi->fh, size) == 0 ? 0 : -errno;
    return truncate(path, size) == 0 ? 0 : -errno;
}

static int
real_open(const char *path, embed_pg_fs_file_info *fi)
{
    int fd = open(path, fi->flags);

    if (fd < 0)
        return -errno;
    fi->fh = (uint64_t) fd;
    return 0;
}

static int
real_create(const char *path, mode_t mode, embed_pg_fs_file_info *fi)
{
    int fd = open(path, fi->flags, mode);

    if (fd < 0)
        return -errno;
    fi->fh = (uint64_t) fd;
    return 0;
}

/* The real filesystem can always answer with a descriptor */
static int
real_open_fd(const char *path, int flags, mode_t mode)
{
    int fd = open(path, flags, mode);

    return fd < 0 ? -errno : fd;
}

static int
real_read(const char *path, char *buf, size_t size, off_t offset,
          embed_pg_fs_file_info *fi)
{
    ssize_t n;

    (void) path;
    n = pread((int) fi->fh, buf, size, offset);
    return n < 0 ? -errno : (int) n;
}

static int
real_write(const char *path, const char *buf, size_t size, off_t offset,
           embed_pg_fs_file_info *fi)
{
    ssize_t n;

    (void) path;
    n = pwrite((int) fi->fh, buf, size, offset);
    return n < 0 ? -errno : (int) n;
}

static int
real_statfs(const char *path, struct statvfs *buf)
{
    return statvfs(path, buf) == 0 ? 0 : -errno;
}

static int
real_flush(const char *path, embed_pg_fs_file_info *fi)
{
    (void) path;
    (void) fi;
    return 0;                   /* the descriptor stays open; nothing to push */
}

static int
real_release(const char *path, embed_pg_fs_file_info *fi)
{
    (void) path;
    return close((int) fi->fh) == 0 ? 0 : -errno;
}

static int
real_fsync(const char *path, int datasync, embed_pg_fs_file_info *fi)
{
    (void) path;
#ifdef HAVE_FDATASYNC
    if (datasync)
        return fdatasync((int) fi->fh) == 0 ? 0 : -errno;
#else
    (void) datasync;
#endif
    return fsync((int) fi->fh) == 0 ? 0 : -errno;
}

static int
real_opendir(const char *path, embed_pg_fs_file_info *fi)
{
    DIR *dir = opendir(path);

    if (dir == NULL)
        return -errno;
    fi->fh = (uint64_t) (uintptr_t) dir;
    return 0;
}

static int
real_readdir(const char *path, void *buf, embed_pg_fs_fill_dir filler,
             embed_pg_fs_file_info *fi)
{
    DIR *dir = (DIR *) (uintptr_t) fi->fh;
    struct dirent *entry;

    (void) path;
    rewinddir(dir);
    while ((entry = readdir(dir)) != NULL)
    {
        struct stat st;

        memset(&st, 0, sizeof(st));
        st.st_ino = entry->d_ino;
        st.st_mode = (mode_t) (entry->d_type << 12);
        if (filler(buf, entry->d_name, &st) != 0)
            break;
    }
    return 0;
}

static int
real_releasedir(const char *path, embed_pg_fs_file_info *fi)
{
    (void) path;
    return closedir((DIR *) (uintptr_t) fi->fh) == 0 ? 0 : -errno;
}

static int
real_fsyncdir(const char *path, int datasync, embed_pg_fs_file_info *fi)
{
    (void) datasync;
    (void) path;
    return fsync(dirfd((DIR *) (uintptr_t) fi->fh)) == 0 ? 0 : -errno;
}

static int
real_utimens(const char *path, const struct timespec tv[2],
             embed_pg_fs_file_info *fi)
{
    struct timeval times[2];

    (void) fi;
    times[0].tv_sec = tv[0].tv_sec;
    times[0].tv_usec = (int) (tv[0].tv_nsec / 1000);
    times[1].tv_sec = tv[1].tv_sec;
    times[1].tv_usec = (int) (tv[1].tv_nsec / 1000);
    return utimes(path, times) == 0 ? 0 : -errno;
}

static int
real_fallocate(const char *path, int mode, off_t offset, off_t length,
               embed_pg_fs_file_info *fi)
{
    (void) path;
    (void) mode;
#ifdef __APPLE__
    {
        fstore_t store = {F_ALLOCATECONTIG, F_PEOFPOSMODE, offset,
                          offset + length, 0};

        if (fcntl((int) fi->fh, F_PREALLOCATE, &store) < 0)
        {
            store.fst_flags = F_ALLOCATEALL;
            if (fcntl((int) fi->fh, F_PREALLOCATE, &store) < 0)
                return -errno;
        }
        return 0;
    }
#else
    return posix_fallocate((int) fi->fh, offset, length) == 0 ? 0 : -errno;
#endif
}

static off_t
real_lseek(const char *path, off_t off, int whence, embed_pg_fs_file_info *fi)
{
    off_t result;

    (void) path;
    result = lseek((int) fi->fh, off, whence);
    return result < 0 ? -errno : result;
}

const embed_pg_fs embed_pg_fs_passthrough = {
    .getattr    = real_getattr,
    .readlink   = real_readlink,
    .mknod      = real_mknod,
    .mkdir      = real_mkdir,
    .unlink     = real_unlink,
    .rmdir      = real_rmdir,
    .symlink    = real_symlink,
    .rename     = real_rename,
    .link       = real_link,
    .chmod      = real_chmod,
    .chown      = real_chown,
    .truncate   = real_truncate,
    .open       = real_open,
    .open_fd    = real_open_fd,
    .read       = real_read,
    .write      = real_write,
    .statfs     = real_statfs,
    .flush      = real_flush,
    .release    = real_release,
    .fsync      = real_fsync,
    .opendir    = real_opendir,
    .readdir    = real_readdir,
    .releasedir = real_releasedir,
    .fsyncdir   = real_fsyncdir,
    .access     = real_access,
    .create     = real_create,
    .utimens    = real_utimens,
    .fallocate  = real_fallocate,
    .lseek      = real_lseek,
};

/* ------------------------------------------------------------------
 * Directory listings
 * ------------------------------------------------------------------ */

static int
listing_add(void *buf, const char *name, const struct stat *st)
{
    struct dir_listing *listing = buf;

    if (listing->count == listing->capacity)
    {
        int newcap = listing->capacity ? listing->capacity * 2 : 32;
        char **names = realloc(listing->names, (size_t) newcap * sizeof(*names));
        unsigned int *modes = realloc(listing->modes,
                                      (size_t) newcap * sizeof(*modes));
        long long *inos = realloc(listing->inos, (size_t) newcap * sizeof(*inos));

        if (!names || !modes || !inos)
        {
            listing->names = names ? names : listing->names;
            listing->modes = modes ? modes : listing->modes;
            listing->inos = inos ? inos : listing->inos;
            return 1;           /* stop collecting */
        }
        listing->names = names;
        listing->modes = modes;
        listing->inos = inos;
        listing->capacity = newcap;
    }

    listing->names[listing->count] = strdup(name);
    if (listing->names[listing->count] == NULL)
        return 1;
    listing->modes[listing->count] = st ? (unsigned int) st->st_mode : 0;
    listing->inos[listing->count] = st ? (long long) st->st_ino : 0;
    listing->count++;
    return 0;
}

static struct dir_listing *
listing_find(struct channel *ch, unsigned long long fh)
{
    struct dir_listing *listing;

    for (listing = ch->dirs; listing != NULL; listing = listing->next) {
        if (listing->fh == fh)
            break;
    }
    return listing;
}

static void
listing_free(struct dir_listing *listing)
{
    for (int i = 0; i < listing->count; i++)
        free(listing->names[i]);
    free(listing->names);
    free(listing->modes);
    free(listing->inos);
    free(listing);
}

static void
listing_drop(struct channel *ch, unsigned long long fh)
{
    for (struct dir_listing **link = &ch->dirs; *link != NULL;
         link = &(*link)->next) {
        struct dir_listing *listing = *link;

        if (listing->fh == fh) {
            *link = listing->next;
            listing_free(listing);
            return;
        }
    }
}

/*
 * Pack entries from `index` on into `buf`, as many as fit.  Returns the bytes
 * used and leaves in `*next` the index the following batch starts at.
 */
static size_t
listing_pack(const struct dir_listing *listing, long long index, char *buf,
             size_t size, long long *next)
{
    size_t used = 0;

    for (; index >= 0 && index < listing->count; index++) {
        size_t namelen = strlen(listing->names[index]);
        size_t reclen = offsetof(embed_vfs_dirent, name) + namelen + 1;
        embed_vfs_dirent *ent;

        reclen = (reclen + 7) & ~(size_t) 7;
        if (reclen > size - used || reclen > USHRT_MAX)
            break;

        ent = (embed_vfs_dirent *) (buf + used);
        memset(ent, 0, reclen);
        ent->ino = listing->inos[index];
        ent->mode = listing->modes[index];
        ent->reclen = (unsigned short) reclen;
        memcpy(ent->name, listing->names[index], namelen + 1);
        used += reclen;
    }
    *next = index;
    return used;
}

/* ------------------------------------------------------------------
 * Tracking what each connection has open
 * ------------------------------------------------------------------ */

static void
channel_track(struct channel *ch, unsigned long long fh, const char *path,
              int isdir)
{
    if (ch->count == ch->capacity) {
        int newcap = ch->capacity ? ch->capacity * 2 : 16;
        unsigned long long *h = realloc(ch->handles, (size_t) newcap * sizeof(*h));
        char **p = realloc(ch->paths, (size_t) newcap * sizeof(*p));
        int *d = realloc(ch->isdir, (size_t) newcap * sizeof(*d));

        if (!h || !p || !d) {
            ch->handles = h ? h : ch->handles;
            ch->paths = p ? p : ch->paths;
            ch->isdir = d ? d : ch->isdir;
            return;             /* untracked; it leaks only if the peer dies */
        }
        ch->handles = h;
        ch->paths = p;
        ch->isdir = d;
        ch->capacity = newcap;
    }
    ch->handles[ch->count] = fh;
    ch->paths[ch->count] = strdup(path);
    ch->isdir[ch->count] = isdir;
    ch->count++;
}

static void
channel_forget(struct channel *ch, unsigned long long fh)
{
    for (int i = 0; i < ch->count; i++) {
        if (ch->handles[i] == fh) {
            free(ch->paths[i]);
            ch->handles[i] = ch->handles[ch->count - 1];
            ch->paths[i] = ch->paths[ch->count - 1];
            ch->isdir[i] = ch->isdir[ch->count - 1];
            ch->count--;
            return;
        }
    }
}

/* Release whatever the process left open, and the region it talked through */
static void
channel_release_all(struct channel *ch)
{
    const embed_pg_fs *fs = &ch->server->fs;

    for (int i = 0; i < ch->count; i++) {
        embed_pg_fs_file_info fi;

        memset(&fi, 0, sizeof(fi));
        fi.fh = ch->handles[i];
        if (ch->isdir[i]) {
            if (fs->releasedir)
                fs->releasedir(ch->paths[i], &fi);
        } else if (fs->release) {
            fs->release(ch->paths[i], &fi);
        }
        free(ch->paths[i]);
    }
    free(ch->handles);
    free(ch->paths);
    free(ch->isdir);
    ch->handles = NULL;
    ch->paths = NULL;
    ch->isdir = NULL;
    ch->count = ch->capacity = 0;

    while (ch->dirs != NULL) {
        struct dir_listing *listing = ch->dirs;

        ch->dirs = listing->next;
        listing_free(listing);
    }

    if (ch->shared != NULL)
        munmap(ch->shared, sizeof(*ch->shared));
    ch->shared = NULL;
}

/* ------------------------------------------------------------------
 * The channel: a shared region and a doorbell
 * ------------------------------------------------------------------ */

/* One byte to the process, with a descriptor when `pass_fd` is not -1 */
static int
send_byte(int fd, char byte, int pass_fd)
{
    struct iovec iov = { &byte, 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    ssize_t n;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (pass_fd >= 0) {
        struct cmsghdr *cmsg;

        memset(cmsgbuf, 0, sizeof(cmsgbuf));
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &pass_fd, sizeof(int));
    }

    do {
        n = sendmsg(fd, &msg, EMBED_SEND_FLAGS);
    } while (n < 0 && errno == EINTR);

    return n == 1 ? 0 : -1;
}

/*
 * Create the region a new connection will talk through, and hand it over.
 *
 * It is a POSIX shared memory object, unlinked as soon as it exists: from then
 * on only the two mappings keep it alive, so nothing is left behind however
 * either side goes away.
 */
static embed_vfs_shared *
region_offer(int fd)
{
    static unsigned int counter;
    char name[32];
    int region = -1;
    void *mem;

    for (int attempt = 0; attempt < 16 && region < 0; attempt++) {
        /* macOS allows 31 characters */
        snprintf(name, sizeof(name), "/embed-vfs.%d.%u", (int) getpid(),
                 __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED));
        region = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (region < 0 && errno != EEXIST)
            return NULL;
    }
    if (region < 0)
        return NULL;
    shm_unlink(name);

    if (ftruncate(region, (off_t) sizeof(embed_vfs_shared)) < 0) {
        close(region);
        return NULL;
    }
    mem = mmap(NULL, sizeof(embed_vfs_shared), PROT_READ | PROT_WRITE,
               MAP_SHARED, region, 0);
    if (mem == MAP_FAILED) {
        close(region);
        return NULL;
    }
    ((embed_vfs_shared *) mem)->magic = EMBED_VFS_MAGIC;
    ((embed_vfs_shared *) mem)->version = EMBED_VFS_VERSION;

    if (send_byte(fd, EMBED_VFS_MSG_HELLO, region) < 0) {
        close(region);
        munmap(mem, sizeof(embed_vfs_shared));
        return NULL;
    }
    close(region);
    return mem;
}

/* ------------------------------------------------------------------
 * Serving
 * ------------------------------------------------------------------ */

static void
fill_stat_reply(embed_vfs_reply *reply, const struct stat *st)
{
    reply->size = (long long) st->st_size;
    reply->mtime = (long long) st->st_mtime;
    reply->atime = (long long) st->st_atime;
    reply->ctime = (long long) st->st_ctime;
    reply->dev = (long long) st->st_dev;
    reply->ino = (long long) st->st_ino;
    reply->blocks = (long long) st->st_blocks;
    reply->mode = (unsigned int) st->st_mode;
    reply->nlink = (unsigned int) st->st_nlink;
    reply->uid = (unsigned int) st->st_uid;
    reply->gid = (unsigned int) st->st_gid;
}

/* A missing callback is simply an operation this filesystem does not have */
#define NEED(cb) do { if (fs->cb == NULL) { rc = -ENOSYS; goto done; } } while (0)

/*
 * Answer the request waiting in the region by calling into the filesystem.
 *
 * This is the whole adapter: the wire format on one side, the embed_pg_fs
 * callbacks on the other.  Callbacks keep libfuse's conventions — 0 or a
 * negative errno, except read and write, which return a count — and that is
 * turned into the result/errno pair the client expects.  Reads and writes work
 * on the region directly, so the bytes are never copied on this side.
 */
static int
serve_request(struct channel *ch)
{
    const embed_pg_fs *fs = &ch->server->fs;
    embed_vfs_shared *sh = ch->shared;
    embed_vfs_request req;
    embed_vfs_reply reply;
    embed_pg_fs_file_info fi;
    const char *path = sh->path;
    const char *path2 = sh->path2;
    struct stat st;
    struct statvfs vfs;
    int rc = 0;

    /* The process is waiting, but nothing it wrote is trusted to be sane */
    req = sh->req;
    sh->path[EMBED_VFS_PATH_MAX - 1] = '\0';
    sh->path2[EMBED_VFS_PATH_MAX - 1] = '\0';

    memset(&reply, 0, sizeof(reply));
    memset(&fi, 0, sizeof(fi));
    fi.flags = req.flags;
    fi.fh = req.fh;

    switch (req.op) {
        case EMBED_VFS_OP_GETATTR:
            NEED(getattr);
            rc = fs->getattr(path, &st, req.fh ? &fi : NULL);
            if (rc == 0)
                fill_stat_reply(&reply, &st);
            break;

        case EMBED_VFS_OP_ACCESS:
            NEED(access);
            rc = fs->access(path, req.flags);
            break;

        case EMBED_VFS_OP_READLINK:
            NEED(readlink);
            rc = fs->readlink(path, sh->data, EMBED_VFS_PATH_MAX);
            if (rc == 0) {
                sh->data[EMBED_VFS_PATH_MAX - 1] = '\0';
                reply.length = (long long) strlen(sh->data);
            }
            break;

        case EMBED_VFS_OP_MKDIR:
            NEED(mkdir);
            rc = fs->mkdir(path, (mode_t) req.mode);
            break;

        case EMBED_VFS_OP_UNLINK:
            NEED(unlink);
            rc = fs->unlink(path);
            break;

        case EMBED_VFS_OP_RMDIR:
            NEED(rmdir);
            rc = fs->rmdir(path);
            break;

        case EMBED_VFS_OP_SYMLINK:
            NEED(symlink);
            rc = fs->symlink(path, path2);
            break;

        case EMBED_VFS_OP_RENAME:
            NEED(rename);
            rc = fs->rename(path, path2, 0);
            break;

        case EMBED_VFS_OP_LINK:
            NEED(link);
            rc = fs->link(path, path2);
            break;

        case EMBED_VFS_OP_CHMOD:
            NEED(chmod);
            rc = fs->chmod(path, (mode_t) req.mode, NULL);
            break;

        case EMBED_VFS_OP_UTIMENS: {
            struct timespec tv[2];

            NEED(utimens);
            tv[0].tv_sec = (time_t) req.offset;
            tv[0].tv_nsec = 0;
            tv[1].tv_sec = (time_t) req.length;
            tv[1].tv_nsec = 0;
            rc = fs->utimens(path, tv, NULL);
            break;
        }

        case EMBED_VFS_OP_STATFS:
            NEED(statfs);
            rc = fs->statfs(path, &vfs);
            if (rc == 0) {
                reply.f_bsize = vfs.f_bsize;
                reply.f_blocks = vfs.f_blocks;
                reply.f_bfree = vfs.f_bfree;
                reply.f_bavail = vfs.f_bavail;
                reply.f_files = vfs.f_files;
                reply.f_ffree = vfs.f_ffree;
            }
            break;

        case EMBED_VFS_OP_TRUNCATE:
            NEED(truncate);
            rc = fs->truncate(path, (off_t) req.length, req.fh ? &fi : NULL);
            break;

        case EMBED_VFS_OP_OPEN:
        case EMBED_VFS_OP_CREATE:
            /*
             * Hand over a descriptor when the filesystem has one: the server
             * then does its reads and writes without coming back here at all.
             */
            if (fs->open_fd != NULL) {
                int direct = fs->open_fd(path, req.flags, (mode_t) req.mode);

                if (direct >= 0) {
                    reply.has_fd = 1;
                    sh->reply = reply;
                    rc = send_byte(ch->fd, EMBED_VFS_MSG_REPLY, direct);
                    close(direct);
                    return rc;
                }
                if (direct != -ENOSYS && direct != -EOPNOTSUPP) {
                    rc = direct;
                    break;
                }
                /* Not for this path; fall through to the handle interface */
            }

            if (req.op == EMBED_VFS_OP_CREATE) {
                NEED(create);
                rc = fs->create(path, (mode_t) req.mode, &fi);
            } else {
                NEED(open);
                rc = fs->open(path, &fi);
            }
            if (rc != 0)
                break;
            reply.fh = fi.fh;
            channel_track(ch, fi.fh, path, 0);

            /* O_APPEND needs the size to start the position at the end */
            if ((req.flags & O_APPEND) && fs->getattr &&
                fs->getattr(path, &st, &fi) == 0)
                reply.size = (long long) st.st_size;
            break;

        case EMBED_VFS_OP_READ:
            NEED(read);
            if (req.length < 0 || req.length > EMBED_VFS_IO_MAX) {
                rc = -EINVAL;
                break;
            }
            rc = fs->read(path, sh->data, (size_t) req.length,
                          (off_t) req.offset, &fi);
            if (rc >= 0) {
                reply.length = (long long) rc;
                rc = 0;
            }
            break;

        case EMBED_VFS_OP_WRITE:
            NEED(write);
            if (req.length < 0 || req.length > EMBED_VFS_IO_MAX) {
                rc = -EINVAL;
                break;
            }
            rc = fs->write(path, sh->data, (size_t) req.length,
                           (off_t) req.offset, &fi);
            if (rc >= 0) {
                reply.length = (long long) rc;
                rc = 0;
            }
            break;

        case EMBED_VFS_OP_FSYNC:
            NEED(fsync);
            rc = fs->fsync(path, req.flags, &fi);
            break;

        case EMBED_VFS_OP_FALLOCATE:
            NEED(fallocate);
            rc = fs->fallocate(path, req.flags, (off_t) req.offset,
                               (off_t) req.length, &fi);
            break;

        case EMBED_VFS_OP_RELEASE:
            if (fs->flush)
                fs->flush(path, &fi);
            NEED(release);
            rc = fs->release(path, &fi);
            channel_forget(ch, req.fh);
            break;

        case EMBED_VFS_OP_OPENDIR: {
            struct dir_listing *listing;

            NEED(opendir);
            NEED(readdir);
            rc = fs->opendir(path, &fi);
            if (rc != 0)
                break;

            listing = calloc(1, sizeof(*listing));
            if (listing == NULL) {
                if (fs->releasedir)
                    fs->releasedir(path, &fi);
                rc = -ENOMEM;
                break;
            }
            listing->fh = fi.fh;
            rc = fs->readdir(path, listing, listing_add, &fi);
            if (rc != 0) {
                listing_free(listing);
                if (fs->releasedir)
                    fs->releasedir(path, &fi);
                break;
            }

            listing->next = ch->dirs;
            ch->dirs = listing;
            reply.fh = fi.fh;
            channel_track(ch, fi.fh, path, 1);
            break;
        }

        case EMBED_VFS_OP_READDIR: {
            struct dir_listing *listing = listing_find(ch, req.fh);

            if (listing == NULL) {
                rc = -EBADF;
                break;
            }
            reply.length = (long long) listing_pack(listing, req.index,
                                                    sh->data,
                                                    EMBED_VFS_DIR_BATCH,
                                                    &reply.index);
            break;
        }

        case EMBED_VFS_OP_RELEASEDIR:
            listing_drop(ch, req.fh);
            if (fs->releasedir)
                rc = fs->releasedir(path, &fi);
            channel_forget(ch, req.fh);
            break;

        default:
            rc = -ENOSYS;
            break;
    }

done:
    if (rc < 0) {
        reply.result = -1;
        reply.err = -rc;
        reply.length = 0;
    }
    sh->reply = reply;
    return send_byte(ch->fd, EMBED_VFS_MSG_REPLY, -1);
}

/* Serve one process until it goes away or the server is stopped */
static void *
channel_thread(void *arg)
{
    struct channel *ch = arg;
    struct embed_vfs_server *server = ch->server;

    embed_pg_fs_enter(server->private_data);

    ch->shared = region_offer(ch->fd);
    while (ch->shared != NULL) {
        char byte;
        ssize_t n;

        do {
            n = recv(ch->fd, &byte, 1, 0);
        } while (n < 0 && errno == EINTR);

        if (n != 1 || byte != EMBED_VFS_MSG_REQUEST ||
            serve_request(ch) < 0)
            break;
    }
    channel_release_all(ch);

    pthread_mutex_lock(&server->channels_mutex);
    ch->done = 1;
    pthread_mutex_unlock(&server->channels_mutex);
    return NULL;
}

/*
 * Join the threads whose processes have gone, or, with `all`, every thread —
 * after waking those still waiting by shutting their sockets.  A socket is only
 * closed once its thread is joined, so the descriptor cannot be reused while
 * anything here might still name it.
 */
static void
channels_reap(struct embed_vfs_server *server, int all)
{
    struct channel *reaped = NULL;
    struct channel **link;

    pthread_mutex_lock(&server->channels_mutex);
    link = &server->channels;
    while (*link != NULL) {
        struct channel *ch = *link;

        if (all || ch->done) {
            if (!ch->done)
                shutdown(ch->fd, SHUT_RDWR);
            *link = ch->next;
            ch->next = reaped;
            reaped = ch;
        } else {
            link = &ch->next;
        }
    }
    pthread_mutex_unlock(&server->channels_mutex);

    while (reaped != NULL) {
        struct channel *ch = reaped;

        reaped = ch->next;
        pthread_join(ch->thread, NULL);
        close(ch->fd);
        free(ch);
    }
}

/* Accept connections and give each a thread of its own */
static void *
vfs_server_thread(void *arg)
{
    struct embed_vfs_server *server = arg;

    for (;;) {
        struct pollfd pfds[2];
        struct channel *ch;
        int conn;

        pfds[0].fd = server->shutdown_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = server->listen_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        if (poll(pfds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfds[0].revents)            /* the server was asked to stop */
            break;
        if (!pfds[1].revents)
            continue;

        channels_reap(server, 0);

        conn = accept(server->listen_fd, NULL, NULL);
        if (conn < 0)
            continue;
        embed_set_nosigpipe(conn);

        ch = calloc(1, sizeof(*ch));
        if (ch == NULL) {
            close(conn);
            continue;
        }
        ch->server = server;
        ch->fd = conn;

        pthread_mutex_lock(&server->channels_mutex);
        if (pthread_create(&ch->thread, NULL, channel_thread, ch) != 0) {
            pthread_mutex_unlock(&server->channels_mutex);
            close(conn);
            free(ch);
            continue;
        }
        ch->next = server->channels;
        server->channels = ch;
        pthread_mutex_unlock(&server->channels_mutex);
    }
    return NULL;
}

struct embed_vfs_server *
embed_vfs_server_start(const embed_pg_fs *fs, void *private_data,
                       const char *sock_path)
{
    struct embed_vfs_server *server;
    struct sockaddr_un addr;
    int shutdown_pipe[2];

    if (sock_path == NULL || strlen(sock_path) >= sizeof(addr.sun_path))
        return NULL;

    server = calloc(1, sizeof(*server));
    if (server == NULL)
        return NULL;

    server->fs = fs ? *fs : embed_pg_fs_passthrough;
    server->private_data = private_data;
    server->listen_fd = -1;
    server->shutdown_fd = -1;
    server->shutdown_write_fd = -1;
    pthread_mutex_init(&server->channels_mutex, NULL);
    snprintf(server->sock_path, sizeof(server->sock_path), "%s", sock_path);

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        free(server);
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    unlink(sock_path);

    if (bind(server->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
        listen(server->listen_fd, 64) < 0) {
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    if (pipe(shutdown_pipe) < 0) {
        close(server->listen_fd);
        unlink(sock_path);
        free(server);
        return NULL;
    }
    server->shutdown_fd = shutdown_pipe[0];
    server->shutdown_write_fd = shutdown_pipe[1];

    if (pthread_create(&server->thread, NULL, vfs_server_thread, server) != 0) {
        close(server->listen_fd);
        close(shutdown_pipe[0]);
        close(shutdown_pipe[1]);
        unlink(sock_path);
        free(server);
        return NULL;
    }

    server->running = 1;
    return server;
}

void
embed_vfs_server_stop(struct embed_vfs_server *server)
{
    if (server == NULL)
        return;

    /* Waking the thread through its own descriptor keeps signals out of this */
    if (server->running && server->shutdown_write_fd >= 0) {
        char byte = 'q';

        if (write(server->shutdown_write_fd, &byte, 1) < 0) {
            /* The thread is already gone or the pipe is broken; join anyway */
        }
    }
    if (server->running)
        pthread_join(server->thread, NULL);

    /* No new connections now; finish the ones there are */
    channels_reap(server, 1);

    if (server->listen_fd >= 0)
        close(server->listen_fd);
    if (server->shutdown_fd >= 0)
        close(server->shutdown_fd);
    if (server->shutdown_write_fd >= 0)
        close(server->shutdown_write_fd);
    if (server->sock_path[0] != '\0')
        unlink(server->sock_path);
    pthread_mutex_destroy(&server->channels_mutex);
    free(server);
}
