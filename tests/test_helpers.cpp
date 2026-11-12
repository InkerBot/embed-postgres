#include "test_helpers.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <ftw.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>

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

/*
 * Only a postmaster that died abruptly — as the kill tests make it — leaves its
 * System V segment behind, and nothing will look at this data directory again,
 * so the segment would leak until reboot.  macOS allows only kern.sysv.shmmni
 * (32) of them system-wide: leak enough and every later initdb fails with
 * "could not create shared memory segment: No space left on device".
 *
 * Line 7 of the lock file holds "<key> <id>" (LOCK_FILE_LINE_SHMEM_KEY).
 */
static void release_orphan_shmem(const std::string &data_dir) {
    std::string pidfile = data_dir + "/postmaster.pid";
    FILE *f = fopen(pidfile.c_str(), "r");
    if (!f) return;

    char line[256];
    unsigned long key = 0, id = 0;
    for (int lineno = 1; lineno <= 7; lineno++) {
        if (!fgets(line, sizeof(line), f)) break;
        if (lineno == 7) sscanf(line, "%lu %lu", &key, &id);
    }
    fclose(f);

    // shmid 0 is a valid id, so key is the "we found a line" signal.
    if (key != 0) shmctl(static_cast<int>(id), IPC_RMID, nullptr);
}

embed_pg_options TempDataDir::options() const {
    embed_pg_options opts = {};
    opts.data_dir = path_.c_str();
    opts.pg_data_path = get_pg_share_dir();
    opts.superuser = "postgres";
    opts.encoding = "UTF8";
    return opts;
}

TempDataDir::~TempDataDir() {
    if (!path_.empty()) {
        release_orphan_shmem(path_);
        nftw(path_.c_str(), remove_cb, 64, FTW_DEPTH | FTW_PHYS);
    }
}

TestServer::TestServer() {
    if (dir_.path().empty()) return;

    embed_pg_options opts = dir_.options();
    if (!opts.pg_data_path) {
        fprintf(stderr, "TestServer: PG_SHARE_DIR not set\n");
        return;
    }

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

PGnotify *wait_for_notify(PGconn *conn, int timeout_ms) {
    if (!conn) return nullptr;

    // A notification may already be buffered from an earlier read.
    if (PGnotify *n = PQnotifies(conn)) return n;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining = (deadline.tv_sec - now.tv_sec) * 1000
                       + (deadline.tv_nsec - now.tv_nsec) / 1000000;
        if (remaining <= 0) return nullptr;

        struct pollfd pfd = {PQsocket(conn), POLLIN, 0};
        int rc = poll(&pfd, 1, static_cast<int>(remaining));
        if (rc < 0) {
            if (errno == EINTR) continue;
            return nullptr;
        }
        if (rc == 0) return nullptr;

        if (!PQconsumeInput(conn)) return nullptr;
        if (PGnotify *n = PQnotifies(conn)) return n;
    }
}

const char *get_pg_share_dir() {
    const char *env = getenv("PG_SHARE_DIR");
    if (env) return env;
#ifdef TEST_PG_SHARE_DIR
    return TEST_PG_SHARE_DIR;
#else
    return nullptr;
#endif
}

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
