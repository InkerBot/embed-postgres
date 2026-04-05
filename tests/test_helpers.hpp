#ifndef TEST_HELPERS_HPP
#define TEST_HELPERS_HPP

#include <string>
#include <gtest/gtest.h>

extern "C" {
#include "embed_postgres.h"
}

// RAII wrapper for temporary data directory
class TempDataDir {
public:
    TempDataDir();
    ~TempDataDir();

    TempDataDir(const TempDataDir &) = delete;
    TempDataDir &operator=(const TempDataDir &) = delete;

    const std::string &path() const { return path_; }

private:
    std::string path_;
};

// RAII wrapper for full server lifecycle
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

// RAII wrapper for PGconn
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

// Get the PG share directory from env or compile-time define
const char *get_pg_share_dir();

// Get the PG bin directory from env or compile-time define
const char *get_pg_bin_dir();

// Run external initdb binary to initialize a data directory. Returns 0 on success.
int run_external_initdb(const char *data_dir, const char *share_dir);

// gtest fixture sharing one server across all tests in a file
class EmbedPgTest : public ::testing::Test {
protected:
    static void SetUpTestSuite();
    static void TearDownTestSuite();
    static embed_pg_server *srv();

    static TestServer *server_;
};

#endif // TEST_HELPERS_HPP
