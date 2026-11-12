#include "test_helpers.hpp"
#include "embed_internal.h"

#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <unistd.h>

class SocketSemanticsTest : public EmbedPgTest {};

static int count_open_fds() {
    DIR *d = opendir("/dev/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d) != nullptr) n++;
    closedir(d);
    return n;
}

/*
 * libpq assumes throughout that the socket is non-blocking; on a blocking one
 * PQconsumeInput() waits for data that may never come, turning a poll for
 * notifications into a hang.
 */
TEST_F(SocketSemanticsTest, ConnectionSocketIsNonBlocking) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    int flags = fcntl(PQsocket(conn.get()), F_GETFL, 0);
    ASSERT_NE(flags, -1) << strerror(errno);
    EXPECT_TRUE(flags & O_NONBLOCK)
        << "connection socket must be non-blocking, like libpq's own";
}

/*
 * Without FD_CLOEXEC every exec() the host performs leaks a live backend
 * connection into the new image, and the backend never sees a disconnect.
 */
TEST_F(SocketSemanticsTest, ConnectionSocketIsCloseOnExec) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    int flags = fcntl(PQsocket(conn.get()), F_GETFD, 0);
    ASSERT_NE(flags, -1) << strerror(errno);
    EXPECT_TRUE(flags & FD_CLOEXEC)
        << "connection socket must not leak across exec()";
}

/* The asynchronous API is what actually depends on non-blocking I/O */
TEST_F(SocketSemanticsTest, AsyncQueryRoundtrip) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    ASSERT_EQ(PQsendQuery(conn.get(), "SELECT 4242"), 1)
        << PQerrorMessage(conn.get());

    bool got_row = false;
    for (int spins = 0; spins < 1000; spins++) {
        while (PQisBusy(conn.get())) {
            struct pollfd pfd = {PQsocket(conn.get()), POLLIN, 0};
            ASSERT_GE(poll(&pfd, 1, 10000), 0) << strerror(errno);
            ASSERT_EQ(PQconsumeInput(conn.get()), 1) << PQerrorMessage(conn.get());
        }
        PGresult *res = PQgetResult(conn.get());
        if (!res) break;
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            EXPECT_STREQ(PQgetvalue(res, 0, 0), "4242");
            got_row = true;
        }
        PQclear(res);
    }
    EXPECT_TRUE(got_row);
}

/*
 * A statement far larger than the socket buffer cannot be written in one go on
 * a non-blocking socket, so this drives libpq's partial-write path against a
 * backend that is writing its reply back at the same time.
 */
TEST_F(SocketSemanticsTest, QueryLargerThanSocketBuffer) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    std::string sql = "SELECT length($x$";
    sql.append(2 * 1024 * 1024, 'q');
    sql += "$x$)";

    PGresult *res = conn.exec(sql.c_str());
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK) << PQerrorMessage(conn.get());
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "2097152");
    PQclear(res);
}

/*
 * Regression guard for closing the server-side fd while it is still in flight,
 * where macOS's AF_UNIX garbage collector could tear the socket down before the
 * postmaster internalized it: the host saw "server closed the connection
 * unexpectedly" right after a successful ACK, within ~30 iterations in roughly
 * 40% of runs.
 */
TEST_F(SocketSemanticsTest, TwoHundredRapidConnections) {
    for (int i = 0; i < 200; i++) {
        PGconn *conn = embed_pg_connect(srv(), "postgres", "postgres");
        ASSERT_NE(conn, nullptr) << "iteration " << i;
        ASSERT_EQ(PQstatus(conn), CONNECTION_OK)
            << "iteration " << i << ": " << PQerrorMessage(conn);
        PQfinish(conn);
    }
}

/*
 * Every connection allocates a socketpair and passes one half away; if either
 * half outlives the connection, a long-lived host runs out of descriptors.
 */
TEST_F(SocketSemanticsTest, NoFdLeakOverManyConnections) {
    // The first connection may populate caches that open fds
    { TestConnection warmup(srv()); ASSERT_TRUE(warmup.is_ok()); }

    int before = count_open_fds();
    ASSERT_GT(before, 0);

    for (int i = 0; i < 100; i++) {
        TestConnection conn(srv());
        ASSERT_TRUE(conn.is_ok()) << "iteration " << i;
        PGresult *res = conn.exec("SELECT 1");
        ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
        PQclear(res);
    }

    int after = count_open_fds();
    EXPECT_LE(after, before + 2)
        << "descriptor count grew from " << before << " to " << after
        << " over 100 connections";
}

/*
 * Writing the shutdown byte to a control channel whose postmaster is gone gets
 * EPIPE, and without SO_NOSIGPIPE / MSG_NOSIGNAL the accompanying SIGPIPE kills
 * the host outright.
 */
TEST(SocketSemanticsStandalone, StopAfterPostmasterKilledDoesNotSignalHost) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());
    embed_pg_server *srv = server.server();

    pid_t child = srv->child_pid;
    ASSERT_GT(child, 0);
    ASSERT_EQ(kill(child, SIGKILL), 0) << strerror(errno);

    // Let the kernel tear down the child's end of the control socket
    usleep(200000);

    int rc = embed_pg_stop(srv);
    EXPECT_NE(rc, 0) << "a SIGKILLed postmaster is not a clean shutdown";
}

/* Same requirement for the connect path, which writes via sendmsg() */
TEST(SocketSemanticsStandalone, ConnectAfterPostmasterKilledFails) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());
    embed_pg_server *srv = server.server();

    pid_t child = srv->child_pid;
    ASSERT_GT(child, 0);
    ASSERT_EQ(kill(child, SIGKILL), 0) << strerror(errno);
    usleep(200000);

    // A NULL return or a failed PGconn is fine; hanging or dying is not
    PGconn *conn = embed_pg_connect(srv, "postgres", "postgres");
    if (conn) {
        EXPECT_NE(PQstatus(conn), CONNECTION_OK);
        PQfinish(conn);
    }
}

/*
 * Installing SIG_IGN for SIGPIPE globally would be an easy way to pass the two
 * tests above, and an unacceptable one: it changes the behaviour of every
 * unrelated write in the host program.
 */
TEST(SocketSemanticsStandalone, HostSigpipeDispositionUntouched) {
    struct sigaction before;
    ASSERT_EQ(sigaction(SIGPIPE, nullptr, &before), 0);

    {
        TestServer server;
        ASSERT_TRUE(server.is_valid());
        PGconn *conn = embed_pg_connect(server.server(), "postgres", "postgres");
        ASSERT_NE(conn, nullptr);
        PQfinish(conn);
    }

    struct sigaction after;
    ASSERT_EQ(sigaction(SIGPIPE, nullptr, &after), 0);
    EXPECT_EQ(before.sa_handler, after.sa_handler)
        << "the embed library must not change the host's SIGPIPE disposition";
}

/*
 * A backend killed under an idle connection must surface as an error on the
 * next query, not take the host down when libpq writes to the dead socket.
 */
TEST_F(SocketSemanticsTest, QueryAfterBackendTerminated) {
    PGconn *victim = embed_pg_connect(srv(), "postgres", "postgres");
    ASSERT_NE(victim, nullptr);
    ASSERT_EQ(PQstatus(victim), CONNECTION_OK);
    int victim_pid = PQbackendPID(victim);
    ASSERT_GT(victim_pid, 0);

    {
        TestConnection killer(srv());
        ASSERT_TRUE(killer.is_ok());
        std::string sql =
            "SELECT pg_terminate_backend(" + std::to_string(victim_pid) + ")";
        PGresult *res = killer.exec(sql.c_str());
        ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK) << PQerrorMessage(killer.get());
        PQclear(res);
    }

    usleep(200000);

    // The first attempt may only consume the FATAL message
    for (int i = 0; i < 5; i++) {
        PGresult *res = PQexec(victim, "SELECT 1");
        if (res) PQclear(res);
    }
    EXPECT_NE(PQstatus(victim), CONNECTION_OK)
        << "connection to a terminated backend should be reported as bad";

    PQfinish(victim);

    TestConnection fresh(srv());
    ASSERT_TRUE(fresh.is_ok());
    PGresult *res = fresh.exec("SELECT 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    PQclear(res);
}
