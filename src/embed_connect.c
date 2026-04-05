/*
 * embed_connect.c
 *    Create embedded connections via socketpair + SCM_RIGHTS + PQconnectSocket.
 *
 * Flow:
 * 1. socketpair() creates a pre-connected fd pair
 * 2. Server-side fd is sent to the child process via SCM_RIGHTS
 * 3. Postmaster receives it via the control channel
 * 4. Postmaster forks a backend for it (BackendStartup)
 * 5. Client-side fd is wrapped in a PGconn via PQconnectSocket()
 */
#include "embed_postgres.h"
#include "embed_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/*
 * Send a file descriptor to another process via SCM_RIGHTS,
 * along with a single-byte command.
 */
static int
send_fd(int socket, int fd_to_send, char cmd)
{
    struct iovec iov = { .iov_base = &cmd, .iov_len = 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;

    memset(cmsgbuf, 0, sizeof(cmsgbuf));
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cmsgbuf, .msg_controllen = sizeof(cmsgbuf)
    };

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    return (int)sendmsg(socket, &msg, 0);
}

PGconn *
embed_pg_connect(embed_pg_server *srv,
                 const char *dbname,
                 const char *username)
{
    if (!srv || !srv->running)
        return NULL;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return NULL;

    /*
     * Serialize embed connections: the postmaster has a single pending slot.
     */
    pthread_mutex_lock(&srv->connect_mutex);

    /* Send server-side fd to child via SCM_RIGHTS */
    if (send_fd(srv->control_fd, sv[0], 'C') < 0) {
        pthread_mutex_unlock(&srv->connect_mutex);
        close(sv[0]);
        close(sv[1]);
        return NULL;
    }
    close(sv[0]);

    /* Wait for ACK (child has submitted fd to postmaster) */
    char ack;
    if (recv(srv->control_fd, &ack, 1, 0) <= 0) {
        pthread_mutex_unlock(&srv->connect_mutex);
        close(sv[1]);
        return NULL;
    }

    pthread_mutex_unlock(&srv->connect_mutex);

    /*
     * Build conninfo string for PQconnectSocket.
     * This provides parameters the startup packet will carry.
     */
    char conninfo[1024];
    snprintf(conninfo, sizeof(conninfo),
             "dbname='%s' user='%s'",
             dbname ? dbname : "postgres",
             username ? username : "postgres");

    return PQconnectSocket(sv[1], conninfo);
}
