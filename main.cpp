/* Demo: two independent PostgreSQL instances running side by side */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

extern "C" {
#include "embed_postgres.h"
}

/* Owns a server handle; embed_pg_destroy() stops it if it is still running */
class Server {
public:
    explicit Server(embed_pg_server *srv) : srv_(srv) {}
    ~Server() { if (srv_) embed_pg_destroy(srv_); }

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    embed_pg_server *get() const { return srv_; }
    bool valid() const { return srv_ != nullptr; }
    int stop() { return embed_pg_stop(srv_); }

private:
    embed_pg_server *srv_;
};

static const char *DATA_DIR_A = "/tmp/embed_pg_data_A";
static const char *DATA_DIR_B = "/tmp/embed_pg_data_B";

static int
init_data_dir(const char *data_dir, const char *pg_share)
{
    struct stat st;
    if (stat(data_dir, &st) == 0) {
        printf("[init] Data directory already exists: %s\n", data_dir);
        return 0;
    }

    printf("[init] Initializing: %s\n", data_dir);
    embed_pg_options opts = {};
    opts.data_dir     = data_dir;
    opts.pg_data_path = pg_share;
    opts.superuser    = "postgres";
    opts.encoding     = "UTF8";

    int rc = embed_pg_initdb(&opts);
    if (rc != 0)
        fprintf(stderr, "[init] initdb failed for %s (rc=%d)\n", data_dir, rc);
    return rc;
}

static embed_pg_server *
start_server(const char *data_dir, const char *pg_share, const char *label)
{
    embed_pg_server *srv = embed_pg_create();
    if (!srv) {
        fprintf(stderr, "[%s] embed_pg_create failed\n", label);
        return NULL;
    }

    embed_pg_options opts = {};
    opts.data_dir     = data_dir;
    opts.pg_data_path = pg_share;

    int rc = embed_pg_start(srv, &opts);
    if (rc != 0) {
        fprintf(stderr, "[%s] embed_pg_start failed (rc=%d)\n", label, rc);
        embed_pg_destroy(srv);
        return NULL;
    }

    printf("[%s] Server process forked (data_dir=%s)\n", label, data_dir);
    return srv;
}

int main(int argc, char *argv[])
{
    const char *pg_share = getenv("PG_SHARE_DIR");
    if (!pg_share) {
        fprintf(stderr, "Set PG_SHARE_DIR to the PostgreSQL share directory\n");
        fprintf(stderr, "  e.g. export PG_SHARE_DIR=build/pg-install/share/postgresql\n");
        return 1;
    }

    printf("=== Step 1: Initialize two independent data directories ===\n");
    if (init_data_dir(DATA_DIR_A, pg_share) != 0) return 1;
    if (init_data_dir(DATA_DIR_B, pg_share) != 0) return 1;
    printf("\n");

    printf("=== Step 2: Fork both server processes (parallel startup) ===\n");
    Server srv_a(start_server(DATA_DIR_A, pg_share, "A"));
    Server srv_b(start_server(DATA_DIR_B, pg_share, "B"));
    if (!srv_a.valid() || !srv_b.valid())
        return 1;
    printf("\n");

    printf("=== Step 3: Wait for both servers to become ready ===\n");
    if (embed_pg_wait_ready(srv_a.get(), 30000) != 0) {
        fprintf(stderr, "[A] Server did not become ready\n");
        return 1;
    }
    printf("[A] Ready!\n");

    if (embed_pg_wait_ready(srv_b.get(), 30000) != 0) {
        fprintf(stderr, "[B] Server did not become ready\n");
        return 1;
    }
    printf("[B] Ready!\n\n");

    printf("=== Step 4: Connect to both instances ===\n");
    PGconn *conn_a = embed_pg_connect(srv_a.get(), "postgres", "postgres");
    if (!conn_a || PQstatus(conn_a) != CONNECTION_OK) {
        fprintf(stderr, "[A] Connection failed: %s\n",
                conn_a ? PQerrorMessage(conn_a) : "NULL");
        if (conn_a) PQfinish(conn_a);
        return 1;
    }
    printf("[A] Connected! Backend PID: %d\n", PQbackendPID(conn_a));

    PGconn *conn_b = embed_pg_connect(srv_b.get(), "postgres", "postgres");
    if (!conn_b || PQstatus(conn_b) != CONNECTION_OK) {
        fprintf(stderr, "[B] Connection failed: %s\n",
                conn_b ? PQerrorMessage(conn_b) : "NULL");
        if (conn_b) PQfinish(conn_b);
        PQfinish(conn_a);
        return 1;
    }
    printf("[B] Connected! Backend PID: %d\n\n", PQbackendPID(conn_b));

    printf("=== Step 5: Verify both instances report their version ===\n");
    PGresult *res;

    res = PQexec(conn_a, "SELECT version()");
    if (PQresultStatus(res) == PGRES_TUPLES_OK)
        printf("[A] %s\n", PQgetvalue(res, 0, 0));
    PQclear(res);

    res = PQexec(conn_b, "SELECT version()");
    if (PQresultStatus(res) == PGRES_TUPLES_OK)
        printf("[B] %s\n", PQgetvalue(res, 0, 0));
    PQclear(res);
    printf("\n");

    printf("=== Step 6: Write different data to each instance ===\n");

    res = PQexec(conn_a,
        "CREATE TABLE IF NOT EXISTS items (id serial PRIMARY KEY, value text)");
    printf("[A] CREATE TABLE: %s\n", PQresStatus(PQresultStatus(res)));
    PQclear(res);

    res = PQexec(conn_b,
        "CREATE TABLE IF NOT EXISTS items (id serial PRIMARY KEY, value text)");
    printf("[B] CREATE TABLE: %s\n", PQresStatus(PQresultStatus(res)));
    PQclear(res);

    res = PQexec(conn_a, "INSERT INTO items (value) VALUES ('only-in-A') RETURNING id");
    printf("[A] INSERT id=%s\n",
           PQresultStatus(res) == PGRES_TUPLES_OK ? PQgetvalue(res, 0, 0) : "ERROR");
    PQclear(res);

    res = PQexec(conn_b, "INSERT INTO items (value) VALUES ('only-in-B') RETURNING id");
    printf("[B] INSERT id=%s\n",
           PQresultStatus(res) == PGRES_TUPLES_OK ? PQgetvalue(res, 0, 0) : "ERROR");
    PQclear(res);
    printf("\n");

    printf("=== Step 7: Verify data isolation (cross-check) ===\n");

    res = PQexec(conn_a, "SELECT value FROM items");
    printf("[A] rows in items: ");
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); i++)
            printf("'%s' ", PQgetvalue(res, i, 0));
    }
    printf("\n");
    PQclear(res);

    res = PQexec(conn_b, "SELECT value FROM items");
    printf("[B] rows in items: ");
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); i++)
            printf("'%s' ", PQgetvalue(res, i, 0));
    }
    printf("\n\n");
    PQclear(res);

    printf("=== Step 8: Multiple connections per instance ===\n");

    PGconn *conn_a2 = embed_pg_connect(srv_a.get(), "postgres", "postgres");
    PGconn *conn_b2 = embed_pg_connect(srv_b.get(), "postgres", "postgres");

    if (conn_a2 && PQstatus(conn_a2) == CONNECTION_OK) {
        res = PQexec(conn_a2, "SELECT count(*) FROM items");
        printf("[A conn2] count=%s (pid=%d)\n",
               PQresultStatus(res) == PGRES_TUPLES_OK ? PQgetvalue(res, 0, 0) : "ERROR",
               PQbackendPID(conn_a2));
        PQclear(res);
        PQfinish(conn_a2);
    }
    if (conn_b2 && PQstatus(conn_b2) == CONNECTION_OK) {
        res = PQexec(conn_b2, "SELECT count(*) FROM items");
        printf("[B conn2] count=%s (pid=%d)\n",
               PQresultStatus(res) == PGRES_TUPLES_OK ? PQgetvalue(res, 0, 0) : "ERROR",
               PQbackendPID(conn_b2));
        PQclear(res);
        PQfinish(conn_b2);
    }
    printf("\n");

    printf("=== Step 9: Shutdown both instances ===\n");
    PQfinish(conn_a);
    PQfinish(conn_b);

    printf("[A] Server stopped (exit code: %d)\n", srv_a.stop());
    printf("[B] Server stopped (exit code: %d)\n", srv_b.stop());

    printf("\nDone! Two independent PostgreSQL instances ran concurrently.\n");
    return 0;
}