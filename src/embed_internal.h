/*
 * embed_internal.h
 *    Internal definitions shared between embed source files.
 */
#ifndef EMBED_INTERNAL_H
#define EMBED_INTERNAL_H

#include <pthread.h>
#include <sys/types.h>

struct embed_pg_server {
    pid_t           child_pid;      /* PG subprocess PID */
    int             control_fd;     /* Unix socket to child for SCM_RIGHTS + commands */
    int             running;
    int             exit_code;
    char           *data_dir;
    char           *argv0;          /* argv[0] for pg_main: path to postgres binary */

    /* Copies of options for the postmaster */
    char           *listen_address;
    int             port;
    char           *unix_socket_dir;

    /* Mutex for embed_pg_connect serialization */
    pthread_mutex_t connect_mutex;
};

#endif /* EMBED_INTERNAL_H */
