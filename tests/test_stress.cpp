#include "test_helpers.hpp"
#include <vector>
#include <string>
#include <sstream>

class StressTest : public EmbedPgTest {};

TEST_F(StressTest, TwentySequentialConnections) {
    for (int i = 0; i < 20; i++) {
        TestConnection conn(srv());
        ASSERT_TRUE(conn.is_ok()) << "Connection " << i << " failed";
        PGresult *res = conn.exec("SELECT 1");
        ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
        PQclear(res);
    }
}

TEST_F(StressTest, TenConcurrentConnections) {
    std::vector<TestConnection *> conns;
    for (int i = 0; i < 10; i++) {
        auto *c = new TestConnection(srv());
        ASSERT_TRUE(c->is_ok()) << "Connection " << i << " failed";
        conns.push_back(c);
    }

    for (int i = 0; i < 10; i++) {
        PGresult *res = conns[i]->exec("SELECT 1");
        ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
        PQclear(res);
    }

    for (auto *c : conns) delete c;
}

TEST_F(StressTest, RapidConnectDisconnect) {
    for (int i = 0; i < 30; i++) {
        PGconn *conn = embed_pg_connect(srv(), "postgres", "postgres");
        ASSERT_NE(conn, nullptr) << "Iteration " << i;
        ASSERT_EQ(PQstatus(conn), CONNECTION_OK) << "Iteration " << i << ": " << PQerrorMessage(conn);
        PQfinish(conn);
    }
}

TEST_F(StressTest, LargeQueryResult) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    PGresult *res = conn.exec(
        "SELECT repeat('x', 1000) FROM generate_series(1, 1000)");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 1000);
    EXPECT_EQ(strlen(PQgetvalue(res, 0, 0)), 1000u);
    PQclear(res);
}

TEST_F(StressTest, LargeInsertBatch) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    PGresult *res = conn.exec("CREATE TABLE stress_batch (id int, data text)");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    res = conn.exec("BEGIN");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    for (int i = 0; i < 1000; i++) {
        std::ostringstream sql;
        sql << "INSERT INTO stress_batch VALUES (" << i << ", 'data_" << i << "')";
        res = conn.exec(sql.str().c_str());
        ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK) << "Insert " << i << " failed";
        PQclear(res);
    }

    res = conn.exec("COMMIT");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    res = conn.exec("SELECT count(*) FROM stress_batch");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1000");
    PQclear(res);

    conn.exec("DROP TABLE stress_batch");
}
