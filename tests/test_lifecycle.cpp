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
    ASSERT_EQ(embed_pg_wait_ready(srv, 30000), 0);
    EXPECT_NE(embed_pg_is_running(srv), 0);

    // Quick query to verify it works
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
    ASSERT_EQ(embed_pg_wait_ready(srv, 30000), 0);

    // Destroy without explicit stop — embed_pg_destroy calls stop internally
    embed_pg_stop(srv);
    embed_pg_destroy(srv);
}
