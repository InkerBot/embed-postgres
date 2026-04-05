/*
 * test_concurrency.cpp
 *    Multi-threaded concurrency tests for embed_postgres.
 *
 * Tests thread-safety of embed_pg_connect (serialized by connect_mutex),
 * concurrent query execution, and data consistency under parallel access.
 */
#include "test_helpers.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstring>

class ConcurrencyTest : public EmbedPgTest {};

// ---- Multi-threaded connection creation ----

TEST_F(ConcurrencyTest, ConcurrentConnectFromThreads) {
    constexpr int NUM_THREADS = 8;
    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};

    auto connect_and_query = [&]() {
        PGconn *conn = embed_pg_connect(srv(), "postgres", "postgres");
        if (!conn || PQstatus(conn) != CONNECTION_OK) {
            failure_count++;
            if (conn) PQfinish(conn);
            return;
        }

        PGresult *res = PQexec(conn, "SELECT 1");
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            success_count++;
        } else {
            failure_count++;
        }
        PQclear(res);
        PQfinish(conn);
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(connect_and_query);
    }
    for (auto &t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), NUM_THREADS)
        << "All threads should successfully connect and query. "
        << "Failures: " << failure_count.load();
}

// ---- Concurrent queries on separate connections ----

TEST_F(ConcurrencyTest, ConcurrentQueriesOnSeparateConnections) {
    constexpr int NUM_THREADS = 4;
    constexpr int QUERIES_PER_THREAD = 20;

    // Each thread gets its own connection
    std::vector<PGconn *> conns(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++) {
        conns[i] = embed_pg_connect(srv(), "postgres", "postgres");
        ASSERT_NE(conns[i], nullptr);
        ASSERT_EQ(PQstatus(conns[i]), CONNECTION_OK);
    }

    std::atomic<int> total_success{0};

    auto run_queries = [&](int thread_id) {
        PGconn *conn = conns[thread_id];
        for (int i = 0; i < QUERIES_PER_THREAD; i++) {
            std::ostringstream sql;
            sql << "SELECT " << (thread_id * 1000 + i);
            PGresult *res = PQexec(conn, sql.str().c_str());
            if (PQresultStatus(res) == PGRES_TUPLES_OK) {
                std::string expected = std::to_string(thread_id * 1000 + i);
                if (expected == PQgetvalue(res, 0, 0)) {
                    total_success++;
                }
            }
            PQclear(res);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(run_queries, i);
    }
    for (auto &t : threads) {
        t.join();
    }

    for (auto *conn : conns) {
        PQfinish(conn);
    }

    EXPECT_EQ(total_success.load(), NUM_THREADS * QUERIES_PER_THREAD);
}

// ---- Concurrent writes with isolation ----

TEST_F(ConcurrencyTest, ConcurrentWritesIsolation) {
    constexpr int NUM_THREADS = 4;
    constexpr int ROWS_PER_THREAD = 50;

    // Setup: create shared table
    TestConnection setup_conn(srv());
    ASSERT_TRUE(setup_conn.is_ok());
    PGresult *res = setup_conn.exec(
        "CREATE TABLE IF NOT EXISTS concurrent_writes "
        "(thread_id int, row_num int, data text)");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    std::atomic<int> errors{0};

    auto writer = [&](int thread_id) {
        PGconn *conn = embed_pg_connect(srv(), "postgres", "postgres");
        if (!conn || PQstatus(conn) != CONNECTION_OK) {
            errors++;
            if (conn) PQfinish(conn);
            return;
        }

        PGresult *r = PQexec(conn, "BEGIN");
        PQclear(r);

        for (int i = 0; i < ROWS_PER_THREAD; i++) {
            std::ostringstream sql;
            sql << "INSERT INTO concurrent_writes VALUES ("
                << thread_id << ", " << i << ", 'thread_" << thread_id << "')";
            r = PQexec(conn, sql.str().c_str());
            if (PQresultStatus(r) != PGRES_COMMAND_OK) {
                errors++;
                PQclear(r);
                PQexec(conn, "ROLLBACK");
                PQfinish(conn);
                return;
            }
            PQclear(r);
        }

        r = PQexec(conn, "COMMIT");
        if (PQresultStatus(r) != PGRES_COMMAND_OK) {
            errors++;
        }
        PQclear(r);
        PQfinish(conn);
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(writer, i);
    }
    for (auto &t : threads) {
        t.join();
    }

    EXPECT_EQ(errors.load(), 0);

    // Verify total row count
    res = setup_conn.exec("SELECT count(*) FROM concurrent_writes");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    int expected_total = NUM_THREADS * ROWS_PER_THREAD;
    EXPECT_STREQ(PQgetvalue(res, 0, 0), std::to_string(expected_total).c_str());
    PQclear(res);

    // Verify each thread's rows
    for (int t = 0; t < NUM_THREADS; t++) {
        std::ostringstream sql;
        sql << "SELECT count(*) FROM concurrent_writes WHERE thread_id = " << t;
        res = setup_conn.exec(sql.str().c_str());
        ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
        EXPECT_STREQ(PQgetvalue(res, 0, 0), std::to_string(ROWS_PER_THREAD).c_str())
            << "Thread " << t << " row count mismatch";
        PQclear(res);
    }

    setup_conn.exec("DROP TABLE concurrent_writes");
}

// ---- Concurrent connect and disconnect from many threads ----

TEST_F(ConcurrencyTest, ConcurrentConnectDisconnect) {
    constexpr int NUM_THREADS = 10;
    constexpr int ITERATIONS = 5;
    std::atomic<int> success{0};
    std::atomic<int> failures{0};

    auto worker = [&]() {
        for (int i = 0; i < ITERATIONS; i++) {
            PGconn *conn = embed_pg_connect(srv(), "postgres", "postgres");
            if (!conn || PQstatus(conn) != CONNECTION_OK) {
                failures++;
                if (conn) PQfinish(conn);
                continue;
            }
            PGresult *res = PQexec(conn, "SELECT pg_backend_pid()");
            if (PQresultStatus(res) == PGRES_TUPLES_OK) {
                success++;
            } else {
                failures++;
            }
            PQclear(res);
            PQfinish(conn);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(worker);
    }
    for (auto &t : threads) {
        t.join();
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(success.load(), NUM_THREADS * ITERATIONS);
}

// ---- Reader-writer concurrency: one writer, multiple readers ----

TEST_F(ConcurrencyTest, ReaderWriterConcurrency) {
    TestConnection setup(srv());
    ASSERT_TRUE(setup.is_ok());
    PGresult *res = setup.exec(
        "CREATE TABLE rw_test (id serial PRIMARY KEY, val int)");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK);
    PQclear(res);

    std::atomic<bool> stop{false};
    std::atomic<int> write_count{0};
    std::atomic<int> read_count{0};

    // Writer thread
    auto writer = [&]() {
        PGconn *conn = embed_pg_connect(srv(), "postgres", "postgres");
        if (!conn || PQstatus(conn) != CONNECTION_OK) {
            if (conn) PQfinish(conn);
            return;
        }

        for (int i = 0; i < 100 && !stop; i++) {
            std::ostringstream sql;
            sql << "INSERT INTO rw_test (val) VALUES (" << i << ")";
            PGresult *r = PQexec(conn, sql.str().c_str());
            if (PQresultStatus(r) == PGRES_COMMAND_OK) {
                write_count++;
            }
            PQclear(r);
        }
        stop = true;
        PQfinish(conn);
    };

    // Reader threads
    auto reader = [&]() {
        PGconn *conn = embed_pg_connect(srv(), "postgres", "postgres");
        if (!conn || PQstatus(conn) != CONNECTION_OK) {
            if (conn) PQfinish(conn);
            return;
        }

        while (!stop) {
            PGresult *r = PQexec(conn, "SELECT count(*) FROM rw_test");
            if (PQresultStatus(r) == PGRES_TUPLES_OK) {
                int count = atoi(PQgetvalue(r, 0, 0));
                (void)count;  // Just verify the query works
                read_count++;
            }
            PQclear(r);
        }
        PQfinish(conn);
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; i++) {
        readers.emplace_back(reader);
    }

    w.join();
    for (auto &r : readers) {
        r.join();
    }

    EXPECT_EQ(write_count.load(), 100);
    EXPECT_GT(read_count.load(), 0);

    setup.exec("DROP TABLE rw_test");
}