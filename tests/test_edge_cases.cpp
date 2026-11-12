#include "test_helpers.hpp"
#include "embed_internal.h"
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

class EdgeCaseTest : public EmbedPgTest {};

TEST(EdgeCaseStandalone, DestroyNull) {
    embed_pg_destroy(nullptr);
}

// Destroying the same handle twice is a use-after-free and is not tested
TEST(EdgeCaseStandalone, DestroyUnstartedServer) {
    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    embed_pg_destroy(srv);
}

TEST(EdgeCaseStandalone, StopNullServer) {
    EXPECT_NE(embed_pg_stop(nullptr), 0);
}

TEST(EdgeCaseStandalone, StopNotStartedServer) {
    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    int rc = embed_pg_stop(srv);
    EXPECT_NE(rc, 0);
    embed_pg_destroy(srv);
}

TEST(EdgeCaseStandalone, DoubleStop) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());

    int rc1 = embed_pg_stop(server.server());
    EXPECT_EQ(rc1, 0);

    int rc2 = embed_pg_stop(server.server());
    EXPECT_NE(rc2, 0);
}

TEST(EdgeCaseStandalone, IsRunningNull) {
    EXPECT_EQ(embed_pg_is_running(nullptr), 0);
}

TEST(EdgeCaseStandalone, WaitReadyNull) {
    EXPECT_NE(embed_pg_wait_ready(nullptr, 1000), 0);
}

TEST(EdgeCaseStandalone, WaitReadyNotStarted) {
    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    EXPECT_NE(embed_pg_wait_ready(srv, 1000), 0);
    embed_pg_destroy(srv);
}

TEST(EdgeCaseStandalone, WaitReadyShortTimeout) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = dir.options();

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    ASSERT_EQ(embed_pg_start(srv, &opts), 0);

    // PG may well be ready within the millisecond, so only a crash or hang
    // would be a failure here
    int rc = embed_pg_wait_ready(srv, 1);
    if (rc != 0)
        embed_pg_wait_ready(srv, 30000);

    embed_pg_stop(srv);
    embed_pg_destroy(srv);
}

TEST_F(EdgeCaseTest, ConnectNullDbname) {
    PGconn *conn = embed_pg_connect(srv(), nullptr, "postgres");
    ASSERT_NE(conn, nullptr);
    EXPECT_EQ(PQstatus(conn), CONNECTION_OK);

    PGresult *res = PQexec(conn, "SELECT current_database()");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "postgres");
    PQclear(res);
    PQfinish(conn);
}

TEST_F(EdgeCaseTest, ConnectNullUsername) {
    PGconn *conn = embed_pg_connect(srv(), "postgres", nullptr);
    ASSERT_NE(conn, nullptr);
    EXPECT_EQ(PQstatus(conn), CONNECTION_OK);

    PGresult *res = PQexec(conn, "SELECT current_user");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "postgres");
    PQclear(res);
    PQfinish(conn);
}

TEST_F(EdgeCaseTest, ConnectBothNull) {
    PGconn *conn = embed_pg_connect(srv(), nullptr, nullptr);
    ASSERT_NE(conn, nullptr);
    EXPECT_EQ(PQstatus(conn), CONNECTION_OK);

    PGresult *res = PQexec(conn, "SELECT 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    PQclear(res);
    PQfinish(conn);
}

TEST_F(EdgeCaseTest, ConnectNonexistentDatabase) {
    // A NULL return and a failed PGconn are both acceptable
    PGconn *conn = embed_pg_connect(srv(), "no_such_database_xyz", "postgres");
    if (conn) {
        EXPECT_NE(PQstatus(conn), CONNECTION_OK);
        PQfinish(conn);
    }
}

TEST_F(EdgeCaseTest, ConnectNonexistentUser) {
    // Under trust authentication this may succeed or fail by PG version
    PGconn *conn = embed_pg_connect(srv(), "postgres", "no_such_user_xyz");
    if (conn && PQstatus(conn) == CONNECTION_OK) {
        PGresult *res = PQexec(conn, "SELECT current_user");
        ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
        PQclear(res);
    }
    if (conn) PQfinish(conn);
}

TEST(EdgeCaseStandalone, ConnectAfterStop) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());

    PGconn *conn = embed_pg_connect(server.server(), "postgres", "postgres");
    ASSERT_NE(conn, nullptr);
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK);
    PQfinish(conn);

    embed_pg_stop(server.server());

    conn = embed_pg_connect(server.server(), "postgres", "postgres");
    if (conn) {
        EXPECT_NE(PQstatus(conn), CONNECTION_OK);
        PQfinish(conn);
    }
}

// embed_pg_connect() quotes dbname into a conninfo string, so a quote in the
// name must not break out of it
TEST_F(EdgeCaseTest, ConnectDbnameSingleQuote) {
    PGconn *conn = embed_pg_connect(srv(), "doesn't_exist", "postgres");
    if (conn) PQfinish(conn);
}

TEST_F(EdgeCaseTest, ConnectEmptyDbname) {
    PGconn *conn = embed_pg_connect(srv(), "", "postgres");
    if (conn) {
        // An empty dbname may fall back to the user name, or fail
        if (PQstatus(conn) == CONNECTION_OK) {
            PGresult *res = PQexec(conn, "SELECT 1");
            ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
            PQclear(res);
        }
        PQfinish(conn);
    }
}

TEST(EdgeCaseStandalone, InitdbOnExistingDirectory) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = dir.options();

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    int rc = embed_pg_initdb(&opts);
    EXPECT_NE(rc, 0) << "initdb should fail on an already-initialized directory";
}

TEST(EdgeCaseStandalone, InitdbInvalidEncoding) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = dir.options();
    opts.encoding = "INVALID_ENCODING_XYZ";

    int rc = embed_pg_initdb(&opts);
    EXPECT_NE(rc, 0) << "initdb should fail with an invalid encoding";
}

TEST(EdgeCaseStandalone, InitdbInvalidDataPath) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = dir.options();
    opts.pg_data_path = "/nonexistent/path/to/share";

    int rc = embed_pg_initdb(&opts);
    EXPECT_NE(rc, 0) << "initdb should fail with a nonexistent share dir";
}

TEST(EdgeCaseStandalone, InitdbEmptySuperuser) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = dir.options();
    opts.superuser = "";

    // Accepting or rejecting an empty superuser name are both fine; crashing
    // is not
    (void) embed_pg_initdb(&opts);
}

TEST(EdgeCaseStandalone, StartWithoutInitdb) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = dir.options();

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);

    // The start only forks, so it may well succeed; the server must never
    // become ready
    int rc = embed_pg_start(srv, &opts);
    if (rc == 0) {
        int ready = embed_pg_wait_ready(srv, 5000);
        EXPECT_NE(ready, 0) << "Server should not become ready without initdb";
    }

    if (rc == 0)
        embed_pg_stop(srv);
    embed_pg_destroy(srv);
}

/*
 * A second start on a live handle would overwrite child_pid and control_fd,
 * leaving the first postmaster running with nothing able to reach or reap it.
 */
TEST(EdgeCaseStandalone, StartTwiceIsRejected) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());

    TempDataDir other;
    ASSERT_FALSE(other.path().empty());

    embed_pg_options opts = other.options();

    EXPECT_NE(embed_pg_start(server.server(), &opts), 0);
    EXPECT_NE(embed_pg_is_running(server.server()), 0)
        << "the original server must still be running";
}

TEST(EdgeCaseStandalone, StartMissingDataDir) {
    embed_pg_options opts = {};   // data_dir left NULL
    opts.pg_data_path = get_pg_share_dir();

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);

    int rc = embed_pg_start(srv, &opts);
    EXPECT_NE(rc, 0);

    embed_pg_destroy(srv);
}

TEST_F(EdgeCaseTest, QueryAfterOtherConnectionFinished) {
    // Raw PGconn*, so that the explicit PQfinish() below is not a double free
    PGconn *raw1 = embed_pg_connect(srv(), "postgres", "postgres");
    ASSERT_NE(raw1, nullptr);
    ASSERT_EQ(PQstatus(raw1), CONNECTION_OK);

    TestConnection conn2(srv());
    ASSERT_TRUE(conn2.is_ok());

    PGresult *res = PQexec(raw1, "CREATE TABLE test_other_finish (id int)");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    PQfinish(raw1);

    res = PQexec(conn2.get(), "SELECT 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    PQclear(res);

    res = PQexec(conn2.get(), "DROP TABLE test_other_finish");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);
}

TEST_F(EdgeCaseTest, ManyErrorsStability) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    for (int i = 0; i < 50; i++) {
        PGresult *res = conn.exec("SELECT * FROM table_that_does_not_exist_xyz");
        EXPECT_EQ(PQresultStatus(res), PGRES_FATAL_ERROR);
        PQclear(res);
    }

    PGresult *res = conn.exec("SELECT 42");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "42");
    PQclear(res);
}

TEST(EdgeCaseStandalone, IsRunningAfterChildKill) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());
    embed_pg_server *srv = server.server();

    EXPECT_NE(embed_pg_is_running(srv), 0);

    pid_t child_pid = srv->child_pid;
    ASSERT_GT(child_pid, 0);

    kill(child_pid, SIGKILL);
    usleep(100000);

    EXPECT_EQ(embed_pg_is_running(srv), 0);
}