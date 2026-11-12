/*
 * Public API of embed-postgres: runs a PostgreSQL server out of the host
 * process and connects to it over socketpairs, with no listening sockets.
 */
#ifndef EMBED_POSTGRES_H
#define EMBED_POSTGRES_H

#include <libpq-fe.h>

#include "embed_pg_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct embed_pg_options {
    const char *data_dir; /* PGDATA path (required) */
    const char *pg_data_path; /* PG share dir for BKI/SQL files (required for initdb) */
    const char *superuser; /* Superuser name (default "postgres") */
    const char *encoding; /* Database encoding (default "UTF8") */

    /* Optional: enable network listening (NULL/0 = embed-only, no sockets) */
    const char *listen_address; /* e.g. "127.0.0.1" (NULL = no TCP) */
    int         port; /* e.g. 5432 (0 = no TCP) */
    const char *unix_socket_dir; /* e.g. "/tmp" (NULL = no Unix socket) */

    /*
     * The filesystem the cluster lives in; NULL means the real one.  initdb and
     * the server must be given the same one, or the server will not find what
     * initdb built.  See below, and embed_pg_fuse.h.
     */
    const embed_pg_fs *fs;

    /* What the callbacks find in embed_pg_fs_get_context()->private_data */
    void *fs_private_data;
} embed_pg_options;

typedef struct embed_pg_server embed_pg_server;

/* Blocking. Returns 0 on success. */
int embed_pg_initdb(const embed_pg_options *opts);

embed_pg_server *embed_pg_create(void);

/*
 * Returns 0 as soon as the postmaster subprocess is forked, which is before
 * the server is usable — call embed_pg_wait_ready() next.
 */
int embed_pg_start(embed_pg_server *srv, const embed_pg_options *opts);

/* Returns 0 once the server accepts connections. */
int embed_pg_wait_ready(embed_pg_server *srv, int timeout_ms);

/* Returns a PGconn usable with the full libpq API. */
PGconn *embed_pg_connect(embed_pg_server *srv,
                         const char *dbname,
                         const char *username);

/* Blocks until shutdown completes. Returns the server's exit code. */
int embed_pg_stop(embed_pg_server *srv);

int embed_pg_is_running(const embed_pg_server *srv);

void embed_pg_destroy(embed_pg_server *srv);

#ifdef __cplusplus
}
#endif

#endif /* EMBED_POSTGRES_H */
