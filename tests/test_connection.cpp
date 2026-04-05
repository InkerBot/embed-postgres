#include "test_helpers.hpp"

class ConnectionTest : public EmbedPgTest {};

TEST_F(ConnectionTest, ConnectReturnsValid) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());
}

TEST_F(ConnectionTest, BackendPidPositive) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());
    EXPECT_GT(PQbackendPID(conn.get()), 0);
}

TEST_F(ConnectionTest, ConnectNullServerReturnsNull) {
    PGconn *conn = embed_pg_connect(nullptr, "postgres", "postgres");
    EXPECT_EQ(conn, nullptr);
}

TEST_F(ConnectionTest, TwoSequentialConnections) {
    {
        TestConnection conn1(srv());
        ASSERT_TRUE(conn1.is_ok());
        int pid1 = PQbackendPID(conn1.get());
        EXPECT_GT(pid1, 0);
    }
    {
        TestConnection conn2(srv());
        ASSERT_TRUE(conn2.is_ok());
        int pid2 = PQbackendPID(conn2.get());
        EXPECT_GT(pid2, 0);
    }
}

TEST_F(ConnectionTest, TwoConcurrentConnections) {
    TestConnection conn1(srv());
    TestConnection conn2(srv());
    ASSERT_TRUE(conn1.is_ok());
    ASSERT_TRUE(conn2.is_ok());
    EXPECT_NE(PQbackendPID(conn1.get()), PQbackendPID(conn2.get()));

    // Both should work independently
    PGresult *res1 = conn1.exec("SELECT 1");
    PGresult *res2 = conn2.exec("SELECT 2");
    ASSERT_EQ(PQresultStatus(res1), PGRES_TUPLES_OK);
    ASSERT_EQ(PQresultStatus(res2), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res1, 0, 0), "1");
    EXPECT_STREQ(PQgetvalue(res2, 0, 0), "2");
    PQclear(res1);
    PQclear(res2);
}

TEST_F(ConnectionTest, ManySequentialConnections) {
    for (int i = 0; i < 20; i++) {
        TestConnection conn(srv());
        ASSERT_TRUE(conn.is_ok()) << "Connection " << i << " failed";
        PGresult *res = conn.exec("SELECT 1");
        ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
        PQclear(res);
    }
}
