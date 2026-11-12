#include "embed_postgres.h"
#include "embed_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>
#include <spawn.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>

extern char **environ;

void
embed_set_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void) fd;      /* Linux relies on MSG_NOSIGNAL per send instead */
#endif
}

static int
ms_until(const struct timespec *deadline)
{
    struct timespec now;
    long ms;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (long)(deadline->tv_sec - now.tv_sec) * 1000
       + (deadline->tv_nsec - now.tv_nsec) / 1000000;

    return ms > 0 ? (int)ms : 0;
}

/*
 * Read one message from the control channel, absorbing the notices that arrive
 * on the postmaster's own schedule: EMBED_MSG_READY may repeat, and
 * EMBED_MSG_EXITING carries a status byte the caller may not be waiting for.
 * Taking the next byte as the answer to one's own request would desynchronize
 * the stream permanently.
 *
 * Caller must hold srv->control_mutex.  Returns the message byte, or -1 on
 * timeout or error; timeout_ms <= 0 waits indefinitely.
 */
int
embed_control_wait(struct embed_pg_server *srv, char want, int timeout_ms)
{
    struct timespec deadline;
    bool bounded = timeout_ms > 0;

    if (bounded) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }

    for (;;) {
        struct pollfd pfd;
        char msg;
        ssize_t n;
        int ret;
        int wait_ms = -1;

        if (srv->control_fd < 0)
            return -1;

        if (bounded) {
            wait_ms = ms_until(&deadline);
            if (wait_ms == 0)
                return -1;      /* timed out */
        }

        pfd.fd = srv->control_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        ret = poll(&pfd, 1, wait_ms);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ret == 0)
            return -1;          /* timed out */

        /* Read even on POLLHUP: a half-closed socket can still hold messages */
        n = recv(srv->control_fd, &msg, 1, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            /*
             * The postmaster is gone without having reported a status — it was
             * killed, or died before it could tell us.
             */
            srv->exited = 1;
            return -1;
        }

        if (msg == EMBED_MSG_READY) {
            srv->ready = 1;
            if (want == EMBED_MSG_READY)
                return msg;
            continue;           /* async notice; keep waiting for `want` */
        }

        if (msg == EMBED_MSG_EXITING) {
            unsigned char status = 0;

            if (recv(srv->control_fd, &status, 1, 0) == 1)
                srv->exit_code = (int) status;
            else
                srv->exit_code = -1;
            srv->exited = 1;
            return msg;         /* nothing else can follow; let callers fail */
        }

        return (unsigned char)msg;
    }
}

/*
 * Build the postmaster's argv.  The "-c name=value" settings are formatted into
 * `settings`, which therefore has to outlive argv_buf.
 */
static int
build_pg_argv(const embed_pg_options *opts, const char *argv0,
              const char **argv_buf,
              char settings[][EMBED_SETTING_MAX])
{
    int argc = 0;
    int nset = 0;

    argv_buf[argc++] = argv0;
    argv_buf[argc++] = "-D";
    argv_buf[argc++] = opts->data_dir;

    if (!opts->listen_address && opts->port <= 0 && !opts->unix_socket_dir) {
        argv_buf[argc++] = "-c";
        argv_buf[argc++] = "listen_addresses=";
        argv_buf[argc++] = "-c";
        argv_buf[argc++] = "unix_socket_directories=";
    } else {
        if (opts->listen_address) {
            snprintf(settings[nset], EMBED_SETTING_MAX,
                     "listen_addresses=%s", opts->listen_address);
            argv_buf[argc++] = "-c";
            argv_buf[argc++] = settings[nset++];
        }
        if (opts->port > 0) {
            snprintf(settings[nset], EMBED_SETTING_MAX,
                     "port=%d", opts->port);
            argv_buf[argc++] = "-c";
            argv_buf[argc++] = settings[nset++];
        }
        if (opts->unix_socket_dir) {
            snprintf(settings[nset], EMBED_SETTING_MAX,
                     "unix_socket_directories=%s", opts->unix_socket_dir);
            argv_buf[argc++] = "-c";
            argv_buf[argc++] = settings[nset++];
        }
    }

    argv_buf[argc++] = "-c";
    argv_buf[argc++] = "log_min_messages=warning";

    argv_buf[argc] = NULL;
    return argc;
}

/*
 * Keep only stdio and the control socket.  Everything else the host has open —
 * its jars, its logs, its listening sockets — must not survive into the
 * postmaster, or the host cannot release them for as long as PostgreSQL runs.
 */
static int
restrict_inherited_fds(posix_spawn_file_actions_t *fa,
                       posix_spawnattr_t *attr, int control_fd)
{
    int rc;

    for (int fd = 0; fd <= 2; fd++) {
        rc = posix_spawn_file_actions_adddup2(fa, fd, fd);
        if (rc != 0)
            return rc;
    }

    if (control_fd >= 0) {
        rc = posix_spawn_file_actions_adddup2(fa, control_fd,
                                              EMBED_CONTROL_CHILD_FD);
        if (rc != 0)
            return rc;
    }

#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    /* Closes every descriptor the file actions did not name */
    return posix_spawnattr_setflags(attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    (void) attr;

    /*
     * No blanket flag here, so the descriptors have to be listed one by one.
     * Reading them from the process itself is the only way to know which exist.
     */
    DIR *d = opendir("/proc/self/fd");
    if (d != NULL) {
        struct dirent *e;
        int dirfd_no = dirfd(d);

        while ((e = readdir(d)) != NULL) {
            int fd = atoi(e->d_name);

            if (fd <= 2 || fd == control_fd || fd == dirfd_no)
                continue;
            posix_spawn_file_actions_addclose(fa, fd);
        }
        closedir(d);
    }
    return 0;
#endif
}

int
embed_spawn(const char *path, const char **argv, int control_fd,
            const char *vfs_sock, pid_t *pid_out)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    char ctlvar[64];
    char vfsvar[PATH_MAX + 32];
    char **envp = environ;
    char **extended = NULL;
    int nextra = 0;
    int rc;

    if (posix_spawn_file_actions_init(&fa) != 0)
        return -1;
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return -1;
    }

    rc = restrict_inherited_fds(&fa, &attr, control_fd);

    if (rc == 0 && (control_fd >= 0 || vfs_sock != NULL)) {
        char *extra[2];
        size_t n = 0;

        if (control_fd >= 0) {
            snprintf(ctlvar, sizeof(ctlvar), "EMBED_CONTROL_FD=%d",
                     EMBED_CONTROL_CHILD_FD);
            extra[nextra++] = ctlvar;
        }
        if (vfs_sock != NULL) {
            snprintf(vfsvar, sizeof(vfsvar), "EMBED_VFS_SOCK=%s", vfs_sock);
            extra[nextra++] = vfsvar;
        }

        while (environ[n] != NULL)
            n++;
        extended = malloc((n + (size_t) nextra + 1) * sizeof(char *));
        if (extended == NULL) {
            rc = -1;
        } else {
            memcpy(extended, environ, n * sizeof(char *));
            for (int i = 0; i < nextra; i++)
                extended[n + i] = extra[i];
            extended[n + nextra] = NULL;
            envp = extended;
        }
    }

    if (rc == 0)
        rc = posix_spawn(pid_out, path, &fa, &attr, (char *const *)argv, envp);

    free(extended);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

void
embed_reap(pid_t pid)
{
    if (pid <= 0)
        return;

    /*
     * The process was told to exit and has already reported its status, so this
     * only clears the zombie.  It may well have been cleared for us already.
     */
    for (int i = 0; i < 200; i++) {
        if (waitpid(pid, NULL, WNOHANG) != 0)
            return;
        usleep(10000);
    }
}

embed_pg_server *
embed_pg_create(void)
{
    embed_pg_server *srv = calloc(1, sizeof(embed_pg_server));
    if (!srv)
        return NULL;
    srv->control_fd = -1;
    srv->child_pid = -1;
    pthread_mutex_init(&srv->control_mutex, NULL);
    return srv;
}

int
embed_pg_start(embed_pg_server *srv, const embed_pg_options *opts)
{
    if (!srv || !opts || !opts->data_dir || !opts->pg_data_path)
        return -1;

    /*
     * Starting twice would overwrite child_pid and control_fd, orphaning the
     * first postmaster with no way left to reach it.
     */
    if (srv->running)
        return -1;

    if (srv->runtime[0] != '\0') {
        embed_remove_runtime(srv->runtime);
        srv->runtime[0] = '\0';
    }
    if (embed_extract_runtime(opts->pg_data_path, srv->runtime) < 0)
        return -1;

    char postgres_bin[PATH_MAX];
    snprintf(postgres_bin, sizeof(postgres_bin), "%s/bin/postgres",
             srv->runtime);

    const char *argv_buf[20];
    char settings[EMBED_MAX_SETTINGS][EMBED_SETTING_MAX];

    build_pg_argv(opts, postgres_bin, argv_buf, settings);

    int ctl[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctl) < 0) {
        embed_remove_runtime(srv->runtime);
        srv->runtime[0] = '\0';
        return -1;
    }

    /*
     * With no filesystem of the caller's, the server is better off reaching the
     * real one itself: routing it through this process would only add a round
     * trip to every file operation to arrive at the same place.
     */
    char vfs_sock[PATH_MAX];
    const char *vfs_sock_arg = NULL;

    if (opts->fs != NULL) {
        snprintf(vfs_sock, sizeof(vfs_sock), "%s/vfs.sock", srv->runtime);
        srv->vfs = embed_vfs_server_start(opts->fs, opts->fs_private_data,
                                          vfs_sock);
        if (srv->vfs == NULL) {
            close(ctl[0]);
            close(ctl[1]);
            embed_remove_runtime(srv->runtime);
            srv->runtime[0] = '\0';
            return -1;
        }
        vfs_sock_arg = vfs_sock;
    }

    pid_t child;
    if (embed_spawn(postgres_bin, argv_buf, ctl[1], vfs_sock_arg, &child) < 0) {
        close(ctl[0]);
        close(ctl[1]);
        embed_vfs_server_stop(srv->vfs);
        srv->vfs = NULL;
        embed_remove_runtime(srv->runtime);
        srv->runtime[0] = '\0';
        return -1;
    }

    close(ctl[1]);
    srv->child_pid = child;
    srv->control_fd = ctl[0];
    srv->running = 1;
    srv->exited = 0;
    srv->exit_code = 0;

    /* A dead postmaster leaves no reader; sends must fail, not signal us */
    embed_set_nosigpipe(srv->control_fd);

    return 0;
}

int
embed_pg_wait_ready(embed_pg_server *srv, int timeout_ms)
{
    if (!srv || srv->control_fd < 0)
        return -1;

    /*
     * srv->ready is sticky, so a second call — or one made after
     * embed_pg_connect() already absorbed the notice — returns immediately
     * instead of blocking for a message that will never come again.
     */
    int rc = 0;

    pthread_mutex_lock(&srv->control_mutex);
    if (!srv->ready &&
        embed_control_wait(srv, EMBED_MSG_READY, timeout_ms) != EMBED_MSG_READY)
        rc = -1;
    pthread_mutex_unlock(&srv->control_mutex);

    return rc;
}

int
embed_pg_stop(embed_pg_server *srv)
{
    if (!srv || !srv->running)
        return -1;

    pthread_mutex_lock(&srv->control_mutex);
    if (srv->control_fd >= 0 && !srv->exited) {
        char cmd = EMBED_CMD_SHUTDOWN;

        if (send(srv->control_fd, &cmd, 1, EMBED_SEND_FLAGS) == 1) {
            /*
             * The postmaster reports its status here rather than through
             * waitpid(), so nothing in this path depends on SIGCHLD reaching
             * us — the host's own runtime is free to reap whatever it likes.
             */
            embed_control_wait(srv, EMBED_MSG_EXITING,
                               EMBED_SHUTDOWN_TIMEOUT_MS);
        }
    }
    if (srv->control_fd >= 0) {
        close(srv->control_fd);
        srv->control_fd = -1;
    }
    pthread_mutex_unlock(&srv->control_mutex);

    embed_reap(srv->child_pid);
    srv->child_pid = -1;
    srv->ready = 0;
    srv->running = 0;

    embed_vfs_server_stop(srv->vfs);
    srv->vfs = NULL;

    if (srv->runtime[0] != '\0') {
        embed_remove_runtime(srv->runtime);
        srv->runtime[0] = '\0';
    }

    return srv->exited ? srv->exit_code : -1;
}

int
embed_pg_is_running(const embed_pg_server *srv)
{
    if (!srv || !srv->running)
        return 0;

    /* Draining the channel is what updates the handle, so the const is a lie */
    embed_pg_server *mutable_srv = (embed_pg_server *) srv;
    int running;

    pthread_mutex_lock(&mutable_srv->control_mutex);

    /*
     * Consume whatever the postmaster has sent but nobody has read yet; an exit
     * notice or an EOF in there is how its death becomes visible.  Waiting on
     * the child would be the other way to ask, and that is exactly what this
     * design avoids.
     *
     * Every pending message has to be drained, not just the first: a stale
     * EMBED_MSG_READY ahead of the exit notice would otherwise hide it, and the
     * postmaster would still look alive.  The bound is only a guard against
     * spinning on a descriptor that reports readable without ever yielding.
     */
    for (int i = 0; i < 64; i++) {
        struct pollfd pfd = { mutable_srv->control_fd, POLLIN, 0 };

        if (mutable_srv->exited || mutable_srv->control_fd < 0)
            break;
        if (poll(&pfd, 1, 0) <= 0)
            break;                  /* nothing pending */
        embed_control_wait(mutable_srv, EMBED_MSG_EXITING, 1);
    }

    running = !mutable_srv->exited;
    pthread_mutex_unlock(&mutable_srv->control_mutex);

    return running;
}

void
embed_pg_destroy(embed_pg_server *srv)
{
    if (!srv)
        return;
    if (srv->running)
        embed_pg_stop(srv);
    embed_vfs_server_stop(srv->vfs);
    srv->vfs = NULL;
    if (srv->control_fd >= 0)
        close(srv->control_fd);
    if (srv->runtime[0] != '\0')
        embed_remove_runtime(srv->runtime);
    pthread_mutex_destroy(&srv->control_mutex);
    free(srv);
}
