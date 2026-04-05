/*
 * embed_postgres.c
 *    Server lifecycle: start/stop/wait for embedded PostgreSQL.
 *
 * PostgreSQL runs as a subprocess. At startup, the process forks a child
 * that loads the PostgreSQL shared library (embedded in the binary as a
 * zstd-compressed blob), then calls pg_main via dlsym. Communication
 * between parent and child uses a Unix socket control channel with
 * SCM_RIGHTS for passing file descriptors.
 */
#include "embed_postgres.h"
#include "embed_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>
#include <dlfcn.h>
#include <stdbool.h>

/* From embed_loader.c */
extern void *embed_load_pg_library(void);

/* Function pointer types for dlsym'd PG symbols */
typedef int  (*pg_main_fn)(int, char **);
typedef void (*embed_set_mode_fn)(bool);
typedef void (*embed_set_ctl_fd_fn)(int);

/*
 * Build pg_main argv from server configuration.
 * Returns argc.
 */
static int
build_pg_argv(const embed_pg_server *srv, const char **argv_buf)
{
    int argc = 0;

    argv_buf[argc++] = srv->argv0;
    argv_buf[argc++] = "-D";
    argv_buf[argc++] = srv->data_dir;

    /* Embed-only mode: disable all network listening */
    if (!srv->listen_address && srv->port == 0 && !srv->unix_socket_dir) {
        argv_buf[argc++] = "-c";
        argv_buf[argc++] = "listen_addresses=";
        argv_buf[argc++] = "-c";
        argv_buf[argc++] = "unix_socket_directories=";
    } else {
        if (srv->listen_address) {
            char buf[256];
            snprintf(buf, sizeof(buf), "listen_addresses=%s", srv->listen_address);
            argv_buf[argc++] = "-c";
            argv_buf[argc++] = strdup(buf);
        }
        if (srv->port > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "port=%d", srv->port);
            argv_buf[argc++] = "-c";
            argv_buf[argc++] = strdup(buf);
        }
        if (srv->unix_socket_dir) {
            char buf[512];
            snprintf(buf, sizeof(buf), "unix_socket_directories=%s", srv->unix_socket_dir);
            argv_buf[argc++] = "-c";
            argv_buf[argc++] = strdup(buf);
        }
    }

    argv_buf[argc++] = "-c";
    argv_buf[argc++] = "log_min_messages=warning";

    argv_buf[argc] = NULL;
    return argc;
}

/*
 * Child process entry point. Loads the embedded PG shared library
 * and runs pg_main. The postmaster event loop handles the control
 * channel directly — no control thread needed.
 */
static void
child_main(int ctl_fd, const embed_pg_server *srv)
{
    /* 1. Load PostgreSQL from embedded blob */
    void *handle = embed_load_pg_library();
    if (!handle) {
        fprintf(stderr, "embed: failed to load pg library\n");
        _exit(1);
    }

    /* 2. Resolve symbols */
    pg_main_fn fn_pg_main =
        (pg_main_fn)dlsym(handle, "pg_main");
    embed_set_mode_fn fn_set_mode =
        (embed_set_mode_fn)dlsym(handle, "embed_set_postmaster_mode");
    embed_set_ctl_fd_fn fn_set_ctl_fd =
        (embed_set_ctl_fd_fn)dlsym(handle, "embed_set_control_fd");

    if (!fn_pg_main || !fn_set_mode || !fn_set_ctl_fd) {
        fprintf(stderr, "embed: dlsym failed: %s\n", dlerror());
        _exit(1);
    }

    /* 3. Build argv for pg_main */
    const char *argv_buf[20];
    int argc = build_pg_argv(srv, argv_buf);

    /* 4. Enable embed mode, set control fd, and run PostgreSQL */
    fn_set_mode(true);
    fn_set_ctl_fd(ctl_fd);
    fn_pg_main(argc, (char **)argv_buf);

    _exit(0);
}

embed_pg_server *
embed_pg_create(void)
{
    embed_pg_server *srv = calloc(1, sizeof(embed_pg_server));
    if (!srv)
        return NULL;
    srv->control_fd = -1;
    pthread_mutex_init(&srv->connect_mutex, NULL);
    return srv;
}

int
embed_pg_start(embed_pg_server *srv, const embed_pg_options *opts)
{
    if (!srv || !opts || !opts->data_dir)
        return -1;

    srv->data_dir = strdup(opts->data_dir);

    /*
     * argv[0] must point to the installed postgres binary. We derive it
     * from pg_data_path (share dir) by going up one level to the install
     * prefix and appending bin/postgres.
     */
    if (opts->pg_data_path) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s/../bin/postgres", opts->pg_data_path);
        srv->argv0 = strdup(buf);
    } else {
        srv->argv0 = strdup("postgres");
    }

    if (opts->listen_address)
        srv->listen_address = strdup(opts->listen_address);
    if (opts->port > 0)
        srv->port = opts->port;
    if (opts->unix_socket_dir)
        srv->unix_socket_dir = strdup(opts->unix_socket_dir);

    /* Create control channel */
    int ctl[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctl) < 0)
        return -1;

    pid_t child = fork();
    if (child < 0) {
        close(ctl[0]);
        close(ctl[1]);
        return -1;
    }

    if (child == 0) {
        /* Child process */
        close(ctl[0]);
        child_main(ctl[1], srv);
        _exit(1); /* unreachable */
    }

    /* Parent */
    close(ctl[1]);
    srv->child_pid = child;
    srv->control_fd = ctl[0];
    srv->running = 1;

    return 0;
}

int
embed_pg_wait_ready(embed_pg_server *srv, int timeout_ms)
{
    if (!srv || srv->control_fd < 0)
        return -1;

    /*
     * Block on the control channel waiting for the 'R' (ready) byte
     * that the postmaster sends when it reaches PM_STATUS_READY.
     * Uses poll() for timeout support — no busy-waiting.
     */
    struct pollfd pfd;
    pfd.fd = srv->control_fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : -1);
    if (ret <= 0)
        return -1;  /* timeout or error */

    if (pfd.revents & (POLLERR | POLLHUP))
        return -1;  /* child died */

    char byte;
    ssize_t n = recv(srv->control_fd, &byte, 1, 0);
    if (n != 1 || byte != 'R')
        return -1;

    return 0;
}

int
embed_pg_stop(embed_pg_server *srv)
{
    if (!srv || !srv->running)
        return -1;

    /* Send shutdown command via control channel */
    if (srv->control_fd >= 0) {
        char cmd = 'S';
        send(srv->control_fd, &cmd, 1, 0);
        close(srv->control_fd);
        srv->control_fd = -1;
    }

    /* Wait for child */
    int status;
    waitpid(srv->child_pid, &status, 0);
    srv->running = 0;
    srv->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return srv->exit_code;
}

int
embed_pg_is_running(const embed_pg_server *srv)
{
    if (!srv || !srv->running)
        return 0;

    /* Check if child is still alive */
    int status;
    pid_t result = waitpid(srv->child_pid, &status, WNOHANG);
    if (result > 0) {
        /* Child exited unexpectedly */
        ((embed_pg_server *)srv)->running = 0;
        ((embed_pg_server *)srv)->exit_code =
            WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return 0;
    }
    return 1;
}

void
embed_pg_destroy(embed_pg_server *srv)
{
    if (!srv)
        return;
    if (srv->running)
        embed_pg_stop(srv);
    if (srv->control_fd >= 0)
        close(srv->control_fd);
    pthread_mutex_destroy(&srv->connect_mutex);
    free(srv->data_dir);
    free(srv->argv0);
    free(srv->listen_address);
    free(srv->unix_socket_dir);
    free(srv);
}
