/*
 * embed_postgres.h
 *    Public API for embedded PostgreSQL server library.
 *
 * Embeds a full PostgreSQL postmaster + workers into the host process.
 * Provides lifecycle management and embedded connections via socketpair
 * (no listening sockets required).
 */
#ifndef EMBED_POSTGRES_H
#define EMBED_POSTGRES_H

#include <libpq-fe.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct embed_pg_options {
    const char *data_dir;           /* PGDATA path (required) */
    const char *pg_data_path;       /* PG share dir for BKI/SQL files (required for initdb) */
    const char *superuser;          /* Superuser name (default "postgres") */
    const char *encoding;           /* Database encoding (default "UTF8") */

    /* Optional: enable network listening (NULL/0 = embed-only, no sockets) */
    const char *listen_address;     /* e.g. "127.0.0.1" (NULL = no TCP) */
    int         port;               /* e.g. 5432 (0 = no TCP) */
    const char *unix_socket_dir;    /* e.g. "/tmp" (NULL = no Unix socket) */
} embed_pg_options;

typedef struct embed_pg_server embed_pg_server;

/* Initialize a data directory (blocking). Returns 0 on success. */
int embed_pg_initdb(const embed_pg_options *opts);

/* Create server instance. */
embed_pg_server *embed_pg_create(void);

/* Start server (non-blocking, spawns thread). Returns 0 on success. */
int embed_pg_start(embed_pg_server *srv, const embed_pg_options *opts);

/* Block until server is ready to accept connections. Returns 0 on success. */
int embed_pg_wait_ready(embed_pg_server *srv, int timeout_ms);

/*
 * Create an embedded connection (no sockets). Returns a standard PGconn*.
 * Uses socketpair internally; the PGconn is fully functional with all libpq APIs.
 */
PGconn *embed_pg_connect(embed_pg_server *srv,
                         const char *dbname,
                         const char *username);

/* Stop server (blocks until shutdown complete). Returns exit code. */
int embed_pg_stop(embed_pg_server *srv);

/* Check if server is running. */
int embed_pg_is_running(const embed_pg_server *srv);

/* Destroy server instance. */
void embed_pg_destroy(embed_pg_server *srv);

#ifdef __cplusplus
}
#endif

#endif /* EMBED_POSTGRES_H */
