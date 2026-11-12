#ifndef TEST_HELPERS_HPP
#define TEST_HELPERS_HPP

#include <string>
#include <gtest/gtest.h>

extern "C" {
#include "embed_postgres.h"
}

class TempDataDir {
public:
    TempDataDir();
    ~TempDataDir();

    TempDataDir(const TempDataDir &) = delete;
    TempDataDir &operator=(const TempDataDir &) = delete;

    const std::string &path() const { return path_; }

    // Defaults pointing at this directory; valid while the object lives
    embed_pg_options options() const;

private:
    std::string path_;
};

class TestServer {
public:
    TestServer();
    ~TestServer();

    TestServer(const TestServer &) = delete;
    TestServer &operator=(const TestServer &) = delete;

    embed_pg_server *server() const { return srv_; }
    bool is_valid() const { return valid_; }

private:
    TempDataDir dir_;
    embed_pg_server *srv_ = nullptr;
    bool valid_ = false;
};

class TestConnection {
public:
    TestConnection(embed_pg_server *srv,
                   const char *dbname = "postgres",
                   const char *username = "postgres");
    ~TestConnection();

    TestConnection(const TestConnection &) = delete;
    TestConnection &operator=(const TestConnection &) = delete;

    PGconn *get() const { return conn_; }
    bool is_ok() const;
    PGresult *exec(const char *sql);

private:
    PGconn *conn_;
};

// NOTIFY delivery is asynchronous — the notifying backend commits, signals the
// listening backend, and only then does that backend write to its socket — so a
// single PQconsumeInput() after the NOTIFY usually finds nothing yet.
// Returns NULL on timeout; the caller must PQfreemem() the result.
PGnotify *wait_for_notify(PGconn *conn, int timeout_ms);

const char *get_pg_share_dir();

// One server shared by every test in a file
class EmbedPgTest : public ::testing::Test {
protected:
    static void SetUpTestSuite();
    static void TearDownTestSuite();
    static embed_pg_server *srv();

    static TestServer *server_;
};

#endif // TEST_HELPERS_HPP
