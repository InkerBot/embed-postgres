#include "test_helpers.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- TempDataDir ----

TempDataDir::TempDataDir() {
    char tmpl[] = "/tmp/embed_pg_test_XXXXXX";
    char *result = mkdtemp(tmpl);
    if (!result) {
        perror("mkdtemp");
        path_ = "";
    } else {
        path_ = result;
    }
}

static int remove_cb(const char *fpath, const struct stat * /*sb*/,
                     int /*typeflag*/, struct FTW * /*ftwbuf*/) {
    return remove(fpath);
}

TempDataDir::~TempDataDir() {
    if (!path_.empty()) {
        nftw(path_.c_str(), remove_cb, 64, FTW_DEPTH | FTW_PHYS);
    }
}

// ---- TestServer ----

TestServer::TestServer() {
    if (dir_.path().empty()) return;

    const char *share = get_pg_share_dir();
    if (!share) {
        fprintf(stderr, "TestServer: PG_SHARE_DIR not set\n");
        return;
    }

    embed_pg_options opts = {};
    opts.data_dir = dir_.path().c_str();
    opts.pg_data_path = share;
    opts.superuser = "postgres";
    opts.encoding = "UTF8";

    if (embed_pg_initdb(&opts) != 0) {
        fprintf(stderr, "TestServer: embed_pg_initdb failed\n");
        return;
    }

    srv_ = embed_pg_create();
    if (!srv_) {
        fprintf(stderr, "TestServer: create failed\n");
        return;
    }

    if (embed_pg_start(srv_, &opts) != 0) {
        fprintf(stderr, "TestServer: start failed\n");
        embed_pg_destroy(srv_);
        srv_ = nullptr;
        return;
    }

    if (embed_pg_wait_ready(srv_, 30000) != 0) {
        fprintf(stderr, "TestServer: wait_ready failed\n");
        embed_pg_stop(srv_);
        embed_pg_destroy(srv_);
        srv_ = nullptr;
        return;
    }

    valid_ = true;
}

TestServer::~TestServer() {
    if (srv_) {
        embed_pg_stop(srv_);
        embed_pg_destroy(srv_);
    }
}

// ---- TestConnection ----

TestConnection::TestConnection(embed_pg_server *srv,
                               const char *dbname,
                               const char *username)
    : conn_(embed_pg_connect(srv, dbname, username)) {}

TestConnection::~TestConnection() {
    if (conn_) PQfinish(conn_);
}

bool TestConnection::is_ok() const {
    return conn_ && PQstatus(conn_) == CONNECTION_OK;
}

PGresult *TestConnection::exec(const char *sql) {
    return PQexec(conn_, sql);
}

// ---- Utility ----

const char *get_pg_share_dir() {
    const char *env = getenv("PG_SHARE_DIR");
    if (env) return env;
#ifdef TEST_PG_SHARE_DIR
    return TEST_PG_SHARE_DIR;
#else
    return nullptr;
#endif
}

const char *get_pg_bin_dir() {
    const char *env = getenv("PG_BIN_DIR");
    if (env) return env;
#ifdef TEST_PG_BIN_DIR
    return TEST_PG_BIN_DIR;
#else
    return nullptr;
#endif
}

int run_external_initdb(const char *data_dir, const char *share_dir) {
    const char *bin_dir = get_pg_bin_dir();
    if (!bin_dir) return -1;

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "%s/initdb -D '%s' -L '%s' --no-locale -U postgres -E UTF8 "
             "-A trust -N --no-instructions >/dev/null 2>&1",
             bin_dir, data_dir, share_dir);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "run_external_initdb: command failed (rc=%d)\n", rc);
        return -1;
    }

    // Append embedded-mode optimizations to postgresql.conf
    char conf_path[1024];
    snprintf(conf_path, sizeof(conf_path), "%s/postgresql.conf", data_dir);
    FILE *f = fopen(conf_path, "a");
    if (f) {
        fprintf(f,
            "\n# Embedded PostgreSQL test optimizations\n"
            "max_wal_senders = 0\n"
            "wal_level = minimal\n"
            "fsync = off\n"
            "synchronous_commit = off\n"
            "full_page_writes = off\n"
        );
        fclose(f);
    }
    return 0;
}

// ---- EmbedPgTest fixture ----

TestServer *EmbedPgTest::server_ = nullptr;

void EmbedPgTest::SetUpTestSuite() {
    server_ = new TestServer();
    ASSERT_TRUE(server_->is_valid()) << "Failed to start embedded PG server";
}

void EmbedPgTest::TearDownTestSuite() {
    delete server_;
    server_ = nullptr;
}

embed_pg_server *EmbedPgTest::srv() {
    return server_ ? server_->server() : nullptr;
}
