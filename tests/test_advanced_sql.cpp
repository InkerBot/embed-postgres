/*
 * test_advanced_sql.cpp
 *    Advanced SQL features: COPY, LISTEN/NOTIFY, SAVEPOINTs,
 *    binary format, async queries, CTEs, window functions,
 *    multiple databases, JSON, and binary parameters.
 */
#include "test_helpers.hpp"
#include <cstring>
#include <string>
#include <sstream>
#include <stdint.h>
#include <arpa/inet.h>

class AdvancedSqlTest : public EmbedPgTest {};

// ---- SAVEPOINT / nested transactions ----

TEST_F(AdvancedSqlTest, SavepointRollback) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_sp (id int)");
    conn.exec("BEGIN");
    conn.exec("INSERT INTO test_sp VALUES (1)");

    PGresult *res = conn.exec("SAVEPOINT sp1");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    conn.exec("INSERT INTO test_sp VALUES (2)");
    res = conn.exec("ROLLBACK TO SAVEPOINT sp1");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    conn.exec("COMMIT");

    res = conn.exec("SELECT count(*) FROM test_sp");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    // Only row 1 survives — row 2 was rolled back to savepoint
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    PQclear(res);

    conn.exec("DROP TABLE test_sp");
}

TEST_F(AdvancedSqlTest, SavepointRelease) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_sp2 (id int)");
    conn.exec("BEGIN");
    conn.exec("INSERT INTO test_sp2 VALUES (10)");
    conn.exec("SAVEPOINT sp_a");
    conn.exec("INSERT INTO test_sp2 VALUES (20)");

    PGresult *res = conn.exec("RELEASE SAVEPOINT sp_a");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    conn.exec("COMMIT");

    res = conn.exec("SELECT count(*) FROM test_sp2");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "2");
    PQclear(res);

    conn.exec("DROP TABLE test_sp2");
}

TEST_F(AdvancedSqlTest, NestedSavepoints) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_nested (id int)");
    conn.exec("BEGIN");
    conn.exec("INSERT INTO test_nested VALUES (1)");
    conn.exec("SAVEPOINT s1");
    conn.exec("INSERT INTO test_nested VALUES (2)");
    conn.exec("SAVEPOINT s2");
    conn.exec("INSERT INTO test_nested VALUES (3)");
    conn.exec("ROLLBACK TO SAVEPOINT s2");
    conn.exec("INSERT INTO test_nested VALUES (4)");
    conn.exec("ROLLBACK TO SAVEPOINT s1");
    conn.exec("INSERT INTO test_nested VALUES (5)");
    conn.exec("COMMIT");

    PGresult *res = conn.exec("SELECT id FROM test_nested ORDER BY id");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    // Rows 1 and 5 survive; 2, 3, 4 were rolled back
    EXPECT_EQ(PQntuples(res), 2);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "1");
    EXPECT_STREQ(PQgetvalue(res, 1, 0), "5");
    PQclear(res);

    conn.exec("DROP TABLE test_nested");
}

// ---- COPY protocol ----

TEST_F(AdvancedSqlTest, CopyToStdout) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_copy_out (a int, b text)");
    conn.exec("INSERT INTO test_copy_out VALUES (1,'one'), (2,'two'), (3,'three')");

    PGresult *res = conn.exec("COPY test_copy_out TO STDOUT");
    ASSERT_EQ(PQresultStatus(res), PGRES_COPY_OUT);
    PQclear(res);

    int row_count = 0;
    char *buf;
    int bytes;
    while ((bytes = PQgetCopyData(conn.get(), &buf, 0)) > 0) {
        row_count++;
        PQfreemem(buf);
    }
    EXPECT_EQ(row_count, 3);

    // End of COPY
    res = PQgetResult(conn.get());
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    conn.exec("DROP TABLE test_copy_out");
}

TEST_F(AdvancedSqlTest, CopyFromStdin) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_copy_in (id int, name text)");

    PGresult *res = conn.exec("COPY test_copy_in FROM STDIN");
    ASSERT_EQ(PQresultStatus(res), PGRES_COPY_IN);
    PQclear(res);

    const char *rows[] = {
        "1\talice\n",
        "2\tbob\n",
        "3\tcharlie\n",
    };
    for (const char *row : rows) {
        int rc = PQputCopyData(conn.get(), row, (int)strlen(row));
        ASSERT_EQ(rc, 1);
    }

    int rc = PQputCopyEnd(conn.get(), nullptr);
    ASSERT_EQ(rc, 1);

    res = PQgetResult(conn.get());
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    EXPECT_STREQ(PQcmdTuples(res), "3");
    PQclear(res);

    res = conn.exec("SELECT count(*) FROM test_copy_in");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "3");
    PQclear(res);

    conn.exec("DROP TABLE test_copy_in");
}

TEST_F(AdvancedSqlTest, CopyFromStdinError) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE test_copy_err (id int NOT NULL)");

    PGresult *res = conn.exec("COPY test_copy_err FROM STDIN");
    ASSERT_EQ(PQresultStatus(res), PGRES_COPY_IN);
    PQclear(res);

    // Send a null value for a NOT NULL column
    const char *bad_row = "\\N\n";
    PQputCopyData(conn.get(), bad_row, (int)strlen(bad_row));

    // Abort the COPY
    int rc = PQputCopyEnd(conn.get(), "test-induced error");
    EXPECT_GE(rc, 0);

    res = PQgetResult(conn.get());
    // Should be PGRES_FATAL_ERROR since we sent an error string
    PQclear(res);

    // Connection should still be usable after COPY error
    // (may need to rollback if in an aborted transaction)
    PQexec(conn.get(), "ROLLBACK");
    res = conn.exec("SELECT 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    PQclear(res);

    conn.exec("DROP TABLE test_copy_err");
}

// ---- LISTEN / NOTIFY ----

TEST_F(AdvancedSqlTest, ListenNotify) {
    TestConnection listener(srv());
    TestConnection notifier(srv());
    ASSERT_TRUE(listener.is_ok());
    ASSERT_TRUE(notifier.is_ok());

    PGresult *res = listener.exec("LISTEN test_channel");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    res = notifier.exec("NOTIFY test_channel, 'hello'");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    // Consume any pending input on the listener connection
    PQconsumeInput(listener.get());
    PGnotify *notify = PQnotifies(listener.get());
    ASSERT_NE(notify, nullptr);
    EXPECT_STREQ(notify->relname, "test_channel");
    EXPECT_STREQ(notify->extra, "hello");
    PQfreemem(notify);

    res = listener.exec("UNLISTEN test_channel");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);
}

TEST_F(AdvancedSqlTest, NotifyWithoutListeners) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    // NOTIFY with no listeners should still succeed
    PGresult *res = conn.exec("NOTIFY orphan_channel, 'payload'");
    EXPECT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);
}

// ---- Async queries ----

TEST_F(AdvancedSqlTest, AsyncQuery) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    int rc = PQsendQuery(conn.get(), "SELECT generate_series(1, 100)");
    ASSERT_EQ(rc, 1);

    int row_count = 0;
    PGresult *res;
    while ((res = PQgetResult(conn.get())) != nullptr) {
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            row_count = PQntuples(res);
        }
        PQclear(res);
    }
    EXPECT_EQ(row_count, 100);
}

TEST_F(AdvancedSqlTest, AsyncQueryParams) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    const char *param_values[] = {"42"};
    int rc = PQsendQueryParams(conn.get(),
                               "SELECT $1::int",
                               1, nullptr,
                               param_values, nullptr, nullptr, 0);
    ASSERT_EQ(rc, 1);

    PGresult *res;
    std::string result;
    while ((res = PQgetResult(conn.get())) != nullptr) {
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            result = PQgetvalue(res, 0, 0);
        }
        PQclear(res);
    }
    EXPECT_EQ(result, "42");
}

TEST_F(AdvancedSqlTest, AsyncMultipleStatements) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    // PQsendQuery can send multiple statements separated by semicolons
    int rc = PQsendQuery(conn.get(), "SELECT 1; SELECT 2; SELECT 3");
    ASSERT_EQ(rc, 1);

    std::vector<std::string> results;
    PGresult *res;
    while ((res = PQgetResult(conn.get())) != nullptr) {
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            results.push_back(PQgetvalue(res, 0, 0));
        }
        PQclear(res);
    }

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], "1");
    EXPECT_EQ(results[1], "2");
    EXPECT_EQ(results[2], "3");
}

// ---- Binary format results ----

TEST_F(AdvancedSqlTest, BinaryIntResult) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    // Request binary format (resultFormat = 1)
    const char *param_values[] = {nullptr};
    const int param_lengths[] = {0};
    const int param_formats[] = {0};  // text params
    int result_format = 1;  // binary result

    PGresult *res = PQexecParams(conn.get(),
                                 "SELECT 12345::int4",
                                 0, nullptr,
                                 nullptr, nullptr, nullptr,
                                 result_format);
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);

    // Binary int4 is a 4-byte big-endian integer
    int len = PQgetlength(res, 0, 0);
    EXPECT_EQ(len, 4);

    const char *raw = PQgetvalue(res, 0, 0);
    uint32_t val_net;
    memcpy(&val_net, raw, 4);
    int32_t val = (int32_t)ntohl(val_net);
    EXPECT_EQ(val, 12345);

    PQclear(res);
}

TEST_F(AdvancedSqlTest, BinaryParams) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    // Send a binary int4 parameter
    int32_t param = htonl(9999);
    const char *param_values[] = { (char *)&param };
    const int param_lengths[] = { 4 };
    const int param_formats[] = { 1 };  // binary

    PGresult *res = PQexecParams(conn.get(),
                                 "SELECT $1::int4 + 1",
                                 1, nullptr,
                                 param_values, param_lengths, param_formats,
                                 0);  // text result
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "10000");
    PQclear(res);
}

// ---- CTEs (Common Table Expressions) ----

TEST_F(AdvancedSqlTest, SimpleCTE) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    PGresult *res = conn.exec(
        "WITH nums AS (SELECT generate_series(1, 5) AS n) "
        "SELECT sum(n) FROM nums");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "15");
    PQclear(res);
}

TEST_F(AdvancedSqlTest, RecursiveCTE) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    PGresult *res = conn.exec(
        "WITH RECURSIVE fib(a, b) AS ("
        "  SELECT 0, 1 "
        "  UNION ALL "
        "  SELECT b, a+b FROM fib WHERE b < 100"
        ") "
        "SELECT max(b) FROM fib");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    // Fibonacci numbers < 100: 1,1,2,3,5,8,13,21,34,55,89 → max 89
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "89");
    PQclear(res);
}

TEST_F(AdvancedSqlTest, WritableCTE) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE cte_target (val int)");
    conn.exec("CREATE TABLE cte_source (val int)");
    conn.exec("INSERT INTO cte_source VALUES (10), (20), (30)");

    PGresult *res = conn.exec(
        "WITH moved AS ("
        "  DELETE FROM cte_source RETURNING val"
        ") "
        "INSERT INTO cte_target SELECT val FROM moved");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    EXPECT_STREQ(PQcmdTuples(res), "3");
    PQclear(res);

    res = conn.exec("SELECT count(*) FROM cte_source");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "0");
    PQclear(res);

    res = conn.exec("SELECT count(*) FROM cte_target");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "3");
    PQclear(res);

    conn.exec("DROP TABLE cte_source");
    conn.exec("DROP TABLE cte_target");
}

// ---- Window functions ----

TEST_F(AdvancedSqlTest, WindowFunctionRowNumber) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE wf_data (grp text, val int)");
    conn.exec("INSERT INTO wf_data VALUES "
              "('a',3),('a',1),('a',2),('b',5),('b',4)");

    PGresult *res = conn.exec(
        "SELECT grp, val, "
        "  row_number() OVER (PARTITION BY grp ORDER BY val) AS rn "
        "FROM wf_data "
        "ORDER BY grp, val");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 5);

    // First row in group 'a' (val=1) should have rn=1
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "a");
    EXPECT_STREQ(PQgetvalue(res, 0, 1), "1");
    EXPECT_STREQ(PQgetvalue(res, 0, 2), "1");

    // First row in group 'b' (val=4) should have rn=1
    EXPECT_STREQ(PQgetvalue(res, 3, 0), "b");
    EXPECT_STREQ(PQgetvalue(res, 3, 1), "4");
    EXPECT_STREQ(PQgetvalue(res, 3, 2), "1");

    PQclear(res);
    conn.exec("DROP TABLE wf_data");
}

TEST_F(AdvancedSqlTest, WindowFunctionRunningSum) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    PGresult *res = conn.exec(
        "SELECT n, sum(n) OVER (ORDER BY n ROWS UNBOUNDED PRECEDING) "
        "FROM generate_series(1, 5) AS n");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 5);

    // Running sums: 1, 3, 6, 10, 15
    const char *expected[] = {"1","3","6","10","15"};
    for (int i = 0; i < 5; i++) {
        EXPECT_STREQ(PQgetvalue(res, i, 1), expected[i]) << "Row " << i;
    }
    PQclear(res);
}

// ---- JSON ----

TEST_F(AdvancedSqlTest, JsonbQuery) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE json_test (id int, data jsonb)");
    conn.exec("INSERT INTO json_test VALUES "
              "(1, '{\"name\":\"alice\",\"age\":30}'), "
              "(2, '{\"name\":\"bob\",\"age\":25}')");

    PGresult *res = conn.exec(
        "SELECT data->>'name' FROM json_test "
        "WHERE (data->>'age')::int > 26 "
        "ORDER BY id");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 1);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "alice");
    PQclear(res);

    res = conn.exec("SELECT jsonb_agg(data->'name') FROM json_test");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 1);
    PQclear(res);

    conn.exec("DROP TABLE json_test");
}

TEST_F(AdvancedSqlTest, JsonPath) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    PGresult *res = conn.exec(
        "SELECT jsonb_path_query_first("
        "  '{\"items\": [1, 2, 3]}'::jsonb, "
        "  '$.items[*] ? (@ > 1)'"
        ")");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "2");
    PQclear(res);
}

// ---- CREATE DATABASE / multi-database ----

TEST_F(AdvancedSqlTest, CreateAndConnectToDatabase) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    // Create a new database
    PGresult *res = conn.exec("CREATE DATABASE test_newdb");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK)
        << PQerrorMessage(conn.get());
    PQclear(res);

    // Connect to the new database
    PGconn *newconn = embed_pg_connect(srv(), "test_newdb", "postgres");
    ASSERT_NE(newconn, nullptr);
    ASSERT_EQ(PQstatus(newconn), CONNECTION_OK)
        << PQerrorMessage(newconn);

    PGresult *res2 = PQexec(newconn, "SELECT current_database()");
    ASSERT_EQ(PQresultStatus(res2), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res2, 0, 0), "test_newdb");
    PQclear(res2);

    // Create a table in the new database
    res2 = PQexec(newconn, "CREATE TABLE only_here (val int)");
    ASSERT_EQ(PQresultStatus(res2), PGRES_COMMAND_OK);
    PQclear(res2);

    res2 = PQexec(newconn, "INSERT INTO only_here VALUES (42)");
    ASSERT_EQ(PQresultStatus(res2), PGRES_COMMAND_OK);
    PQclear(res2);

    PQfinish(newconn);

    // The table should not exist in the original postgres database
    res = conn.exec("SELECT to_regclass('public.only_here')");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQgetisnull(res, 0, 0), 1)
        << "Table 'only_here' should not exist in postgres database";
    PQclear(res);

    conn.exec("DROP DATABASE test_newdb");
}

// ---- Transaction isolation levels ----

TEST_F(AdvancedSqlTest, RepeatableRead) {
    TestConnection conn1(srv());
    TestConnection conn2(srv());
    ASSERT_TRUE(conn1.is_ok());
    ASSERT_TRUE(conn2.is_ok());

    conn1.exec("CREATE TABLE iso_test (val int)");
    conn1.exec("INSERT INTO iso_test VALUES (100)");

    // Start a REPEATABLE READ transaction on conn1
    conn1.exec("BEGIN ISOLATION LEVEL REPEATABLE READ");

    // Read initial value in conn1
    PGresult *res = conn1.exec("SELECT val FROM iso_test");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "100");
    PQclear(res);

    // conn2 updates the value and commits
    conn2.exec("UPDATE iso_test SET val = 200");

    // conn1 should still see 100 under REPEATABLE READ
    res = conn1.exec("SELECT val FROM iso_test");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "100")
        << "REPEATABLE READ should not see conn2's committed change";
    PQclear(res);

    conn1.exec("COMMIT");

    conn1.exec("DROP TABLE iso_test");
}

// ---- Prepared statements across connections ----

TEST_F(AdvancedSqlTest, PreparedStatementsAreConnectionLocal) {
    TestConnection conn1(srv());
    TestConnection conn2(srv());
    ASSERT_TRUE(conn1.is_ok());
    ASSERT_TRUE(conn2.is_ok());

    // Prepare on conn1
    PGresult *res = PQprepare(conn1.get(), "my_stmt", "SELECT $1::int * 2",
                              1, nullptr);
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    // Execute on conn1 — should work
    const char *p1[] = {"21"};
    res = PQexecPrepared(conn1.get(), "my_stmt", 1, p1, nullptr, nullptr, 0);
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "42");
    PQclear(res);

    // Execute on conn2 — should fail (prepared statements are session-local)
    res = PQexecPrepared(conn2.get(), "my_stmt", 1, p1, nullptr, nullptr, 0);
    EXPECT_EQ(PQresultStatus(res), PGRES_FATAL_ERROR)
        << "Prepared statement should not be visible on another connection";
    PQclear(res);
}

// ---- RETURNING clause ----

TEST_F(AdvancedSqlTest, UpdateReturning) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE ret_test (id serial, val int, updated_at timestamptz)");
    conn.exec("INSERT INTO ret_test (val) VALUES (1),(2),(3)");

    PGresult *res = conn.exec(
        "UPDATE ret_test SET val = val * 10, updated_at = now() "
        "RETURNING id, val");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 3);
    EXPECT_STREQ(PQgetvalue(res, 0, 1), "10");
    EXPECT_STREQ(PQgetvalue(res, 1, 1), "20");
    EXPECT_STREQ(PQgetvalue(res, 2, 1), "30");
    PQclear(res);

    conn.exec("DROP TABLE ret_test");
}

TEST_F(AdvancedSqlTest, DeleteReturning) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE del_ret (id int, name text)");
    conn.exec("INSERT INTO del_ret VALUES (1,'a'),(2,'b'),(3,'c')");

    PGresult *res = conn.exec(
        "DELETE FROM del_ret WHERE id > 1 RETURNING name");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 2);
    PQclear(res);

    conn.exec("DROP TABLE del_ret");
}

// ---- Error handling in transactions (aborted state) ----

TEST_F(AdvancedSqlTest, AbortedTransactionRecovery) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("BEGIN");

    // This causes an error, putting the transaction in aborted state
    PGresult *res = conn.exec("SELECT 1/0");
    EXPECT_EQ(PQresultStatus(res), PGRES_FATAL_ERROR);
    PQclear(res);

    // All subsequent commands fail with "current transaction is aborted"
    res = conn.exec("SELECT 1");
    EXPECT_EQ(PQresultStatus(res), PGRES_FATAL_ERROR);
    PQclear(res);

    // ROLLBACK clears the error state
    res = conn.exec("ROLLBACK");
    EXPECT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    // Now normal queries should work again
    res = conn.exec("SELECT 99");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "99");
    PQclear(res);
}

// ---- Large objects / bytea ----

TEST_F(AdvancedSqlTest, ByteaRoundtrip) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    // Insert binary data via PQexecParams with bytea
    const char binary_data[] = "\x00\x01\x02\xfe\xff\x00hello";
    int data_len = 10;

    const char *params[] = { binary_data };
    int param_lengths[] = { data_len };
    int param_formats[] = { 1 };  // binary

    PGresult *res = PQexecParams(conn.get(),
                                 "SELECT $1::bytea",
                                 1, nullptr,
                                 params, param_lengths, param_formats,
                                 1);  // binary result
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQgetlength(res, 0, 0), data_len);
    EXPECT_EQ(memcmp(PQgetvalue(res, 0, 0), binary_data, data_len), 0);
    PQclear(res);
}

// ---- pg_catalog system queries ----

TEST_F(AdvancedSqlTest, SystemCatalogQuery) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    // pg_tables
    PGresult *res = conn.exec(
        "SELECT count(*) FROM pg_catalog.pg_tables "
        "WHERE schemaname = 'pg_catalog'");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    int count = atoi(PQgetvalue(res, 0, 0));
    EXPECT_GT(count, 0);
    PQclear(res);
}

TEST_F(AdvancedSqlTest, InformationSchema) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE info_schema_test (id int, name text)");

    PGresult *res = conn.exec(
        "SELECT column_name, data_type "
        "FROM information_schema.columns "
        "WHERE table_name = 'info_schema_test' "
        "ORDER BY ordinal_position");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_EQ(PQntuples(res), 2);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "id");
    EXPECT_STREQ(PQgetvalue(res, 0, 1), "integer");
    EXPECT_STREQ(PQgetvalue(res, 1, 0), "name");
    EXPECT_STREQ(PQgetvalue(res, 1, 1), "text");
    PQclear(res);

    conn.exec("DROP TABLE info_schema_test");
}

// ---- Multiple data types ----

TEST_F(AdvancedSqlTest, DataTypes) {
    TestConnection conn(srv());
    ASSERT_TRUE(conn.is_ok());

    conn.exec("CREATE TABLE types_test ("
              "  a_bool bool, a_int2 int2, a_int4 int4, a_int8 int8, "
              "  a_float4 float4, a_float8 float8, a_numeric numeric(10,2), "
              "  a_text text, a_varchar varchar(50), a_char char(5), "
              "  a_bytea bytea, a_uuid uuid, "
              "  a_date date, a_time time, a_timestamp timestamp, "
              "  a_interval interval, a_array int[]"
              ")");

    PGresult *res = conn.exec(
        "INSERT INTO types_test VALUES ("
        "  true, 32767, 2147483647, 9223372036854775807, "
        "  1.5, 2.5, 123.45, "
        "  'hello', 'world', 'abcde', "
        "  '\\xdeadbeef', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11', "
        "  '2024-01-15', '14:30:00', '2024-01-15 14:30:00', "
        "  '1 day 2 hours', ARRAY[1,2,3]"
        ")");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    res = conn.exec("SELECT a_bool, a_int8, a_text, a_uuid FROM types_test");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "t");
    EXPECT_STREQ(PQgetvalue(res, 0, 1), "9223372036854775807");
    EXPECT_STREQ(PQgetvalue(res, 0, 2), "hello");
    EXPECT_STREQ(PQgetvalue(res, 0, 3), "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
    PQclear(res);

    conn.exec("DROP TABLE types_test");
}
