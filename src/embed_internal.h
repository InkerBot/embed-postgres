#ifndef EMBED_INTERNAL_H
#define EMBED_INTERNAL_H

#include <limits.h>
#include <pthread.h>
#include <sys/types.h>

#include "embed_vfs_protocol.h"

/*
 * Control channel protocol, duplicated from the patched
 * src/include/postmaster/postmaster.h because backend headers cannot be
 * included here — keep both in sync.
 *
 * The channel carries everything the two sides need from each other, so the
 * host never signals the postmaster and never has to wait on it.
 *
 * EMBED_MSG_READY is asynchronous: it arrives whenever the postmaster starts
 * accepting connections and possibly more than once (PM_HOT_STANDBY, then
 * PM_RUN), so readers must dispatch on the message byte rather than assume the
 * next byte answers their last request — see embed_control_wait().
 */
#define EMBED_CMD_CONNECT   'C'     /* host -> pm, carries an fd via SCM_RIGHTS */
#define EMBED_CMD_SHUTDOWN  'S'     /* host -> pm, no fd */
#define EMBED_MSG_READY     'R'     /* pm -> host, async, may repeat */
#define EMBED_MSG_ACK       'A'     /* pm -> host, backend started */
#define EMBED_MSG_ERROR     'E'     /* pm -> host, backend not started */
#define EMBED_MSG_EXITING   'X'     /* pm -> host, followed by the exit status */

/*
 * The descriptor numbers the sockets appear under in the server processes,
 * named for them in EMBED_CONTROL_FD and EMBED_VFS_FD.  0-2 stay with the
 * host's stdio.
 */
#define EMBED_CONTROL_CHILD_FD 3
#define EMBED_VFS_CHILD_FD     4

/*
 * A dead postmaster is detected immediately via EOF on the control channel;
 * these bounds only cover one that is wedged rather than gone.
 */
#define EMBED_CONNECT_TIMEOUT_MS  30000
#define EMBED_SHUTDOWN_TIMEOUT_MS 30000

/* listen_addresses, port and unix_socket_directories, as "-c name=value" */
#define EMBED_MAX_SETTINGS 3
#define EMBED_SETTING_MAX  512

struct embed_pg_server {
    pid_t           child_pid;
    int             control_fd;
    int             running;
    int             ready;          /* EMBED_MSG_READY seen at least once */
    int             exited;         /* EMBED_MSG_EXITING seen, or channel EOF */
    int             exit_code;
    char            runtime[PATH_MAX];  /* extracted install prefix, or "" */

    /* The file system the server processes see; see embed_vfs_host.c */
    struct embed_vfs_server *vfs;

    /*
     * Covers every access to control_fd, not just connections: both
     * embed_pg_wait_ready() and embed_pg_connect() read from it, and
     * concurrent readers would steal each other's message bytes.
     */
    pthread_mutex_t control_mutex;
};

/*
 * Writing to the control channel after the postmaster is gone must fail with
 * EPIPE rather than kill the host, which is somebody else's program and may
 * have no SIGPIPE handler.  Linux suppresses the signal per call; macOS has no
 * such flag on sendmsg() and needs the socket option below instead.
 */
#ifdef MSG_NOSIGNAL
#define EMBED_SEND_FLAGS MSG_NOSIGNAL
#else
#define EMBED_SEND_FLAGS 0
#endif

extern void embed_set_nosigpipe(int fd);

/*
 * Unpack the embedded programs into a temporary tree shaped like an install
 * prefix — bin/postgres and bin/initdb are real files, lib and share are
 * symlinks into `share_dir`'s installation — because PostgreSQL locates both
 * from the path of its own executable.  `prefix_out` needs PATH_MAX bytes.
 */
extern int embed_extract_runtime(const char *share_dir, char *prefix_out);
extern void embed_remove_runtime(const char *prefix);

/*
 * Spawn one of the extracted programs with posix_spawn(), which never copies
 * the caller's address space and is therefore safe in a threaded host.  Only
 * stdio and, when `control_fd` is not -1, the control socket are inherited;
 * every other descriptor of the host is left behind.
 */
extern int embed_spawn(const char *path, const char **argv,
                       int control_fd, const char *vfs_sock, pid_t *pid_out);

/*
 * Serve `vfs` (NULL for the real file system) on a socket at `sock_path`, which
 * the server processes are told about through EMBED_VFS_SOCK and connect to
 * individually.
 */
extern struct embed_vfs_server *embed_vfs_server_start(const embed_pg_fs *fs,
                                                       void *private_data,
                                                       const char *sock_path);
extern void embed_vfs_server_stop(struct embed_vfs_server *server);

/* Set the context before calling a filesystem outside the serving thread */
extern void embed_pg_fs_enter(void *private_data);

/*
 * Reap `pid` without letting the result matter: the exit status comes from the
 * control channel, so a host runtime that reaped the child first changes
 * nothing here.
 */
extern void embed_reap(pid_t pid);

/*
 * Read one control message.  Asynchronous EMBED_MSG_READY notices are absorbed
 * into srv->ready, and EMBED_MSG_EXITING (plus channel EOF) into srv->exited
 * and srv->exit_code, whether or not the caller asked for them.
 *
 * Caller must hold srv->control_mutex; defined in embed_postgres.c.
 */
extern int embed_control_wait(struct embed_pg_server *srv, char want,
                              int timeout_ms);

#endif /* EMBED_INTERNAL_H */
