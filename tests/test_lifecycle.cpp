#include "test_helpers.hpp"
#include <unistd.h>

TEST(LifecycleTest, CreateReturnsValidServer) {
    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    embed_pg_destroy(srv);
}

TEST(LifecycleTest, StartNullServerFails) {
    embed_pg_options opts = {};
    opts.data_dir = "/tmp/nonexistent";
    opts.pg_data_path = get_pg_share_dir();
    EXPECT_NE(embed_pg_start(nullptr, &opts), 0);
}

TEST(LifecycleTest, StartNullOptsFails) {
    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    EXPECT_NE(embed_pg_start(srv, nullptr), 0);
    embed_pg_destroy(srv);
}

TEST(LifecycleTest, IsRunningBeforeStart) {
    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    EXPECT_EQ(embed_pg_is_running(srv), 0);
    embed_pg_destroy(srv);
}

TEST(LifecycleTest, FullLifecycle) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = dir.options();

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);

    ASSERT_EQ(embed_pg_start(srv, &opts), 0);
    ASSERT_EQ(embed_pg_wait_ready(srv, 30000), 0);
    EXPECT_NE(embed_pg_is_running(srv), 0);

    PGconn *conn = embed_pg_connect(srv, "postgres", "postgres");
    ASSERT_NE(conn, nullptr);
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK);

    PGresult *res = PQexec(conn, "SELECT 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    PQclear(res);
    PQfinish(conn);

    int exit_code = embed_pg_stop(srv);
    EXPECT_EQ(exit_code, 0);
    EXPECT_EQ(embed_pg_is_running(srv), 0);

    embed_pg_destroy(srv);
}

TEST(LifecycleTest, StopReturnsZero) {
    TestServer server;
    ASSERT_TRUE(server.is_valid());
    EXPECT_EQ(embed_pg_stop(server.server()), 0);
}

TEST(LifecycleTest, DestroyRunningServer) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = dir.options();

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    ASSERT_EQ(embed_pg_start(srv, &opts), 0);
    ASSERT_EQ(embed_pg_wait_ready(srv, 30000), 0);

    // embed_pg_destroy() stops a running server itself
    embed_pg_stop(srv);
    embed_pg_destroy(srv);
}

/*
 * The embed-only default suppresses every listener, so the settings that turn
 * TCP back on are otherwise never exercised.
 */
TEST(LifecycleTest, ListenAddressAndPortReachTheServer) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    embed_pg_options opts = dir.options();
    opts.listen_address = "127.0.0.1";
    opts.port = 15432;

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    ASSERT_EQ(embed_pg_start(srv, &opts), 0);
    ASSERT_EQ(embed_pg_wait_ready(srv, 30000), 0);

    PGconn *tcp = PQconnectdb("host=127.0.0.1 port=15432 "
                              "dbname=postgres user=postgres");
    ASSERT_NE(tcp, nullptr);
    EXPECT_EQ(PQstatus(tcp), CONNECTION_OK) << PQerrorMessage(tcp);

    if (PQstatus(tcp) == CONNECTION_OK) {
        PGresult *res = PQexec(tcp, "SELECT 1");
        EXPECT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
        PQclear(res);
    }
    PQfinish(tcp);

    embed_pg_stop(srv);
    embed_pg_destroy(srv);
}
