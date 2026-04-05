#include "test_helpers.hpp"
#include <cstring>

class SqlTest : public EmbedPgTest {};

TEST_F(SqlTest, SelectLiteral) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());
    PGresult *res = conn.exec("SELECT 42");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "42");
    PQclear(res);
}

TEST_F(SqlTest, SelectString) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());
    PGresult *res = conn.exec("SELECT 'hello world'");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "hello world");
    PQclear(res);
}

TEST_F(SqlTest, SelectVersion) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());
    PGresult *res = conn.exec("SELECT version()");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    std::string ver = PQgetvalue(res, 0, 0);
    EXPECT_NE(ver.find("PostgreSQL"), std::string::npos);
    PQclear(res);
}

TEST_F(SqlTest, SelectMultipleRows) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());
    PGresult *res = conn.exec("SELECT generate_series(1, 5)");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 5);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    EXPECT_STREQ(PQgetvalue(res, 4, 0), "5");
    PQclear(res);
}

TEST_F(SqlTest, SelectNull) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());
    PGresult *res = conn.exec("SELECT NULL");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQgetisnull(res, 0, 0), 1);
    PQclear(res);
}

TEST_F(SqlTest, CreateTable) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());
    PGresult *res = conn.exec(
        "CREATE TABLE test_create (id serial PRIMARY KEY, name text)");
    EXPECT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    res = conn.exec("DROP TABLE test_create");
    EXPECT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);
}

TEST_F(SqlTest, InsertReturning) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    PGresult *res = conn.exec(
        "CREATE TABLE test_insert (id serial PRIMARY KEY, val text)");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    res = conn.exec("INSERT INTO test_insert (val) VALUES ('foo') RETURNING id, val");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    EXPECT_STREQ(PQgetvalue(res, 0, 1), "foo");
    PQclear(res);

    res = conn.exec("DROP TABLE test_insert");
    PQclear(res);
}

TEST_F(SqlTest, Update) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_update (id int, val text)");
    conn.exec("INSERT INTO test_update VALUES (1, 'old')");

    PGresult *res = conn.exec("UPDATE test_update SET val = 'new' WHERE id = 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    EXPECT_STREQ(PQcmdTuples(res), "1");
    PQclear(res);

    res = conn.exec("SELECT val FROM test_update WHERE id = 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "new");
    PQclear(res);

    conn.exec("DROP TABLE test_update");
}

TEST_F(SqlTest, Delete) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_delete (id int)");
    conn.exec("INSERT INTO test_delete VALUES (1), (2), (3)");

    PGresult *res = conn.exec("DELETE FROM test_delete WHERE id = 2");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    EXPECT_STREQ(PQcmdTuples(res), "1");
    PQclear(res);

    res = conn.exec("SELECT count(*) FROM test_delete");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "2");
    PQclear(res);

    conn.exec("DROP TABLE test_delete");
}

TEST_F(SqlTest, TransactionCommit) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_txn (id int)");

    PGresult *res = conn.exec("BEGIN");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    conn.exec("INSERT INTO test_txn VALUES (1)");
    conn.exec("INSERT INTO test_txn VALUES (2)");

    res = conn.exec("COMMIT");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    res = conn.exec("SELECT count(*) FROM test_txn");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "2");
    PQclear(res);

    conn.exec("DROP TABLE test_txn");
}

TEST_F(SqlTest, TransactionRollback) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_rollback (id int)");
    conn.exec("INSERT INTO test_rollback VALUES (1)");

    conn.exec("BEGIN");
    conn.exec("INSERT INTO test_rollback VALUES (2)");
    conn.exec("ROLLBACK");

    PGresult *res = conn.exec("SELECT count(*) FROM test_rollback");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    PQclear(res);

    conn.exec("DROP TABLE test_rollback");
}

TEST_F(SqlTest, PreparedStatement) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_prepared (id int, name text)");
    conn.exec("INSERT INTO test_prepared VALUES (1, 'alice'), (2, 'bob')");

    PGresult *res = PQprepare(conn.get(), "find_by_id",
                              "SELECT name FROM test_prepared WHERE id = $1",
                              1, nullptr);
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    const char *params[] = {"2"};
    res = PQexecPrepared(conn.get(), "find_by_id", 1, params, nullptr, nullptr, 0);
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "bob");
    PQclear(res);

    conn.exec("DROP TABLE test_prepared");
}

TEST_F(SqlTest, ErrorQuery) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    PGresult *res = conn.exec("SELECT * FROM nonexistent_table_xyz");
    EXPECT_EQ(PQresultStatus(res), PGRES_FATAL_ERROR);
    PQclear(res);

    // Connection should still work after error
    res = conn.exec("SELECT 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    PQclear(res);
}

TEST_F(SqlTest, LargeResult) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    PGresult *res = conn.exec("SELECT generate_series(1, 10000)");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 10000);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    EXPECT_STREQ(PQgetvalue(res, 9999, 0), "10000");
    PQclear(res);
}

TEST_F(SqlTest, CrossConnectionVisibility) {
    TestConnection conn1(srv());
    TestConnection conn2(srv());
    ASSERT_TRUE(conn1.is_ok());
    ASSERT_TRUE(conn2.is_ok());

    conn1.exec("CREATE TABLE test_visibility (val text)");
    conn1.exec("INSERT INTO test_visibility VALUES ('visible')");

    // conn2 should see committed data
    PGresult *res = conn2.exec("SELECT val FROM test_visibility");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 1);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "visible");
    PQclear(res);

    conn1.exec("DROP TABLE test_visibility");
}
