#include "embed_postgres.h"
#include "embed_internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

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

    return (int)sendmsg(socket, &msg, EMBED_SEND_FLAGS);
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

    /* One request in flight: concurrent readers would steal reply bytes */
    pthread_mutex_lock(&srv->control_mutex);

    if (send_fd(srv->control_fd, sv[0], EMBED_CMD_CONNECT) < 0) {
        pthread_mutex_unlock(&srv->control_mutex);
        close(sv[0]);
        close(sv[1]);
        return NULL;
    }

    int reply = embed_control_wait(srv, EMBED_MSG_ACK, EMBED_CONNECT_TIMEOUT_MS);

    pthread_mutex_unlock(&srv->control_mutex);

    /*
     * The reply byte is the barrier for dropping our reference: closing right
     * after sendmsg() leaves the descriptor in flight with the socket buffer as
     * its only reference, and macOS's AF_UNIX garbage collector can tear the
     * socket down before the postmaster internalizes it.  The backend is then
     * forked onto a socket whose peer is gone, reads EOF instead of a startup
     * packet, and exits silently — the host sees "server closed the connection
     * unexpectedly" milliseconds after a successful ACK.
     */
    close(sv[0]);

    /*
     * EMBED_MSG_ERROR means BackendStartup() failed, so nothing is listening on
     * sv[1]; handing back a PGconn would only defer the failure to the first
     * query.
     */
    if (reply != EMBED_MSG_ACK) {
        close(sv[1]);
        return NULL;
    }

    char conninfo[1024];
    snprintf(conninfo, sizeof(conninfo),
             "dbname='%s' user='%s'",
             dbname ? dbname : "postgres",
             username ? username : "postgres");

    return PQconnectSocket(sv[1], conninfo);
}
