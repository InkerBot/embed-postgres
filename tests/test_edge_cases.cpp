/*
 * test_edge_cases.cpp
 *    Edge cases and boundary conditions for the embed_postgres API.
 *
 * Tests double-stop, connect after stop, NULL/invalid arguments,
 * initdb on existing directory, invalid dbname/user, destroy semantics, etc.
 */
#include "test_helpers.hpp"
#include "embed_internal.h"
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

class EdgeCaseTest : public EmbedPgTest {};

// ---- embed_pg_destroy edge cases ----

TEST(EdgeCaseStandalone, DestroyNull) {
    // Should not crash
    embed_pg_destroy(nullptr);
}

TEST(EdgeCaseStandalone, DestroyTwice) {
    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    embed_pg_destroy(srv);
    // Note: calling destroy twice is UB (use-after-free), but we test
    // that destroying a non-started server works at all.
}

// ---- embed_pg_stop edge cases ----

TEST(EdgeCaseStandalone, StopNullServer) {
    EXPECT_NE(embed_pg_stop(nullptr), 0);
}

TEST(EdgeCaseStandalone, StopNotStartedServer) {
    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    // Server was never started: stop should return error, not crash
    int rc = embed_pg_stop(srv);
    EXPECT_NE(rc, 0);
    embed_pg_destroy(srv);
}

TEST(EdgeCaseStandalone, DoubleStop) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());

    int rc1 = embed_pg_stop(server.server());
    EXPECT_EQ(rc1, 0);

    // Second stop should not crash; it should fail gracefully
    int rc2 = embed_pg_stop(server.server());
    EXPECT_NE(rc2, 0);
}

// ---- embed_pg_is_running edge cases ----

TEST(EdgeCaseStandalone, IsRunningNull) {
    EXPECT_EQ(embed_pg_is_running(nullptr), 0);
}

// ---- embed_pg_wait_ready edge cases ----

TEST(EdgeCaseStandalone, WaitReadyNull) {
    EXPECT_NE(embed_pg_wait_ready(nullptr, 1000), 0);
}

TEST(EdgeCaseStandalone, WaitReadyNotStarted) {
    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    // control_fd is -1 before start, should fail
    EXPECT_NE(embed_pg_wait_ready(srv, 1000), 0);
    embed_pg_destroy(srv);
}

TEST(EdgeCaseStandalone, WaitReadyShortTimeout) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    const char *share = get_pg_share_dir();
    ASSERT_NE(share, nullptr);

    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    opts.pg_data_path = share;
    opts.superuser = "postgres";
    opts.encoding = "UTF8";

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    ASSERT_EQ(embed_pg_start(srv, &opts), 0);

    // 1ms timeout — might or might not succeed, but should not crash
    int rc = embed_pg_wait_ready(srv, 1);
    // We don't assert the result since PG might be fast enough to start,
    // but we verify no crash/hang occurs.

    if (rc != 0) {
        // If timeout, wait properly before cleanup
        embed_pg_wait_ready(srv, 30000);
    }

    embed_pg_stop(srv);
    embed_pg_destroy(srv);
}

// ---- embed_pg_connect edge cases ----

TEST_F(EdgeCaseTest, ConnectNullDbname) {
    // NULL dbname should default to "postgres" per the implementation
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
    // NULL username should default to "postgres" per the implementation
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
    // Connecting to a database that doesn't exist
    PGconn *conn = embed_pg_connect(srv(), "no_such_database_xyz", "postgres");
    // PG should either reject during startup or return an error status
    if (conn) {
        EXPECT_NE(PQstatus(conn), CONNECTION_OK);
        PQfinish(conn);
    }
    // If NULL, that's also acceptable
}

TEST_F(EdgeCaseTest, ConnectNonexistentUser) {
    // Connecting with a non-existent user; pg_hba.conf uses trust mode,
    // so this may succeed or fail depending on PG version behavior
    PGconn *conn = embed_pg_connect(srv(), "postgres", "no_such_user_xyz");
    if (conn && PQstatus(conn) == CONNECTION_OK) {
        // If it succeeded (some PG configs allow this), verify it works
        PGresult *res = PQexec(conn, "SELECT current_user");
        ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
        PQclear(res);
    }
    if (conn) PQfinish(conn);
}

TEST(EdgeCaseStandalone, ConnectAfterStop) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());

    // Verify it works first
    PGconn *conn = embed_pg_connect(server.server(), "postgres", "postgres");
    ASSERT_NE(conn, nullptr);
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK);
    PQfinish(conn);

    embed_pg_stop(server.server());

    // Connect after stop should return NULL or a failed connection
    conn = embed_pg_connect(server.server(), "postgres", "postgres");
    if (conn) {
        EXPECT_NE(PQstatus(conn), CONNECTION_OK);
        PQfinish(conn);
    }
}

// ---- Connection with special characters ----

TEST_F(EdgeCaseTest, ConnectDbnameSingleQuote) {
    // The connect function uses snprintf with single quotes around dbname.
    // A dbname containing a single quote could break the conninfo string.
    // This tests whether it's handled safely.
    PGconn *conn = embed_pg_connect(srv(), "doesn't_exist", "postgres");
    // We don't care about the result database — just that it doesn't crash
    // or cause a security issue (SQL injection in conninfo).
    if (conn) PQfinish(conn);
}

TEST_F(EdgeCaseTest, ConnectEmptyDbname) {
    PGconn *conn = embed_pg_connect(srv(), "", "postgres");
    if (conn) {
        // Empty dbname might succeed (PG may default to user's name) or fail
        if (PQstatus(conn) == CONNECTION_OK) {
            PGresult *res = PQexec(conn, "SELECT 1");
            ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
            PQclear(res);
        }
        PQfinish(conn);
    }
}

// ---- embed_pg_initdb edge cases ----

TEST(EdgeCaseStandalone, InitdbOnExistingDirectory) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    const char *share = get_pg_share_dir();
    ASSERT_NE(share, nullptr);

    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    opts.pg_data_path = share;
    opts.superuser = "postgres";
    opts.encoding = "UTF8";

    // First initdb should succeed
    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    // Second initdb on same directory — initdb should detect existing data
    // and refuse (non-zero return)
    int rc = embed_pg_initdb(&opts);
    EXPECT_NE(rc, 0) << "initdb should fail on an already-initialized directory";
}

TEST(EdgeCaseStandalone, InitdbInvalidEncoding) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    const char *share = get_pg_share_dir();
    ASSERT_NE(share, nullptr);

    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    opts.pg_data_path = share;
    opts.superuser = "postgres";
    opts.encoding = "INVALID_ENCODING_XYZ";

    int rc = embed_pg_initdb(&opts);
    EXPECT_NE(rc, 0) << "initdb should fail with an invalid encoding";
}

TEST(EdgeCaseStandalone, InitdbInvalidDataPath) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    opts.pg_data_path = "/nonexistent/path/to/share";
    opts.superuser = "postgres";
    opts.encoding = "UTF8";

    int rc = embed_pg_initdb(&opts);
    EXPECT_NE(rc, 0) << "initdb should fail with a nonexistent share dir";
}

TEST(EdgeCaseStandalone, InitdbEmptySuperuser) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    const char *share = get_pg_share_dir();
    ASSERT_NE(share, nullptr);

    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    opts.pg_data_path = share;
    opts.superuser = "";
    opts.encoding = "UTF8";

    // Empty superuser name — PG may reject or accept
    int rc = embed_pg_initdb(&opts);
    // Either outcome is valid; just verify no crash
    (void)rc;
}

// ---- embed_pg_start edge cases ----

TEST(EdgeCaseStandalone, StartWithoutInitdb) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    const char *share = get_pg_share_dir();
    ASSERT_NE(share, nullptr);

    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    opts.pg_data_path = share;

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);

    // Start without initializing data dir — fork succeeds but PG should fail
    int rc = embed_pg_start(srv, &opts);
    if (rc == 0) {
        // Start itself may succeed (it just forks), but wait_ready should fail
        int ready = embed_pg_wait_ready(srv, 5000);
        EXPECT_NE(ready, 0) << "Server should not become ready without initdb";
    }

    // Cleanup
    if (rc == 0)
        embed_pg_stop(srv);
    embed_pg_destroy(srv);
}

TEST(EdgeCaseStandalone, StartMissingDataDir) {
    embed_pg_options opts = {};
    // data_dir is NULL — should be caught by input validation
    opts.pg_data_path = get_pg_share_dir();

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);

    int rc = embed_pg_start(srv, &opts);
    EXPECT_NE(rc, 0);

    embed_pg_destroy(srv);
}

// ---- Query after PQfinish of another connection ----

TEST_F(EdgeCaseTest, QueryAfterOtherConnectionFinished) {
    // Use raw PGconn* to avoid double-free with TestConnection RAII
    PGconn *raw1 = embed_pg_connect(srv(), "postgres", "postgres");
    ASSERT_NE(raw1, nullptr);
    ASSERT_EQ(PQstatus(raw1), CONNECTION_OK);

    TestConnection conn2(srv());
    ASSERT_TRUE(conn2.is_ok());

    // Create a table via conn1
    PGresult *res = PQexec(raw1, "CREATE TABLE test_other_finish (id int)");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    // Finish conn1 explicitly
    PQfinish(raw1);

    // conn2 should still work after conn1 is finished
    res = PQexec(conn2.get(), "SELECT 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    PQclear(res);

    res = PQexec(conn2.get(), "DROP TABLE test_other_finish");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);
}

// ---- Server remains stable after many errors ----

TEST_F(EdgeCaseTest, ManyErrorsStability) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    // Fire 50 error queries in a row
    for (int i = 0; i < 50; i++) {
        PGresult *res = conn.exec("SELECT * FROM table_that_does_not_exist_xyz");
        EXPECT_EQ(PQresultStatus(res), PGRES_FATAL_ERROR);
        PQclear(res);
    }

    // Connection should still be functional
    PGresult *res = conn.exec("SELECT 42");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "42");
    PQclear(res);
}

// ---- is_running reflects child death ----

TEST(EdgeCaseStandalone, IsRunningAfterChildKill) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());
    embed_pg_server *srv = server.server();

    EXPECT_NE(embed_pg_is_running(srv), 0);

    // Kill child process (embed_internal.h gives us child_pid directly)
    pid_t child_pid = srv->child_pid;
    ASSERT_GT(child_pid, 0);

    kill(child_pid, SIGKILL);

    // Give it a moment to die
    usleep(100000);  // 100ms

    // is_running should now detect the child has died
    EXPECT_EQ(embed_pg_is_running(srv), 0);
}