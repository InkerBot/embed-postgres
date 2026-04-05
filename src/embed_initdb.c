/*
 * embed_initdb.c
 *    Initialize a PostgreSQL data directory using the embedded initdb.
 *
 * Instead of calling an external initdb binary via system(), this forks
 * a child process and calls initdb_main() directly. The initdb code is
 * compiled into the embed library (linked from PG's initdb sources),
 * so it's available in-process after fork().
 *
 * The embedding library loads the PG shared library, resolves pg_main via
 * dlsym, and sets embed_pg_main_hook in initdb.c. The tokenizer and
 * fork+call logic for popen/pclose/system replacements live in initdb.c
 * itself, so the embed library just needs to provide the pg_main pointer.
 */
#include "embed_postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <limits.h>

/* From PG's initdb.c (renamed from main) */
extern int initdb_main(int argc, char *argv[]);

/* Embed pg_main hook declared in initdb.c */
typedef int (*pg_main_fn)(int argc, char **argv);
extern pg_main_fn embed_pg_main_hook;

/* From embed_loader.c */
extern void *embed_load_pg_library(void);

/* -----------------------------------------------------------------------
 * embed_pg_initdb — public API
 * ----------------------------------------------------------------------- */
int
embed_pg_initdb(const embed_pg_options *opts)
{
    if (!opts || !opts->data_dir || !opts->pg_data_path)
        return -1;

    const char *superuser = opts->superuser ? opts->superuser : "postgres";
    const char *encoding  = opts->encoding  ? opts->encoding  : "UTF8";

    /*
     * Derive an absolute path to the postgres binary from the share
     * directory so that bin_path in initdb is set to a sane value.
     * The binary does not need to exist; EMBED_POSTGRES_EXEC bypasses
     * version checking and the hook handles all actual invocations.
     */
    char real_share[PATH_MAX];
    char postgres_bin[PATH_MAX];
    const char *abs_share;

    if (realpath(opts->pg_data_path, real_share) != NULL)
    {
        abs_share = real_share;

        /* Derive prefix/bin/postgres: go up one level from the share dir */
        char prefix[PATH_MAX];
        strlcpy(prefix, real_share, sizeof(prefix));
        char *sep = strrchr(prefix, '/');
        if (sep) *sep = '\0';
        snprintf(postgres_bin, sizeof(postgres_bin),
                 "%s/bin/postgres", prefix);
    }
    else
    {
        /* realpath failed (path doesn't exist yet or is already absolute) */
        abs_share = opts->pg_data_path;
        snprintf(postgres_bin, sizeof(postgres_bin),
                 "%s/../bin/postgres", opts->pg_data_path);
    }

    fprintf(stderr, "embed_initdb: initializing data directory %s...\n",
            opts->data_dir);

    /*
     * Fork a child process for initdb.  Necessary because initdb uses
     * global state, atexit handlers, and signal handlers.
     */
    pid_t child = fork();
    if (child < 0)
    {
        fprintf(stderr, "embed_initdb: fork failed\n");
        return -1;
    }

    if (child == 0)
    {
        /*
         * Child process.
         *
         * Load the embedded PG library and set the pg_main hook so that
         * initdb's popen/pclose/system replacements can call pg_main
         * in forked grandchildren.
         */
        void *handle = embed_load_pg_library();
        if (!handle)
        {
            fprintf(stderr, "embed_initdb: failed to load embedded PG library\n");
            _exit(1);
        }
        pg_main_fn pg_main = (pg_main_fn) dlsym(handle, "pg_main");
        if (!pg_main)
        {
            fprintf(stderr, "embed_initdb: dlsym(pg_main) failed: %s\n",
                    dlerror());
            _exit(1);
        }

        embed_pg_main_hook = pg_main;

        /*
         * Tell setup_bin_paths() where "postgres" lives (bypasses
         * find_other_exec version check).  Also pass -L so initdb
         * reads BKI/SQL files from the correct share directory without
         * needing to derive it from the binary path.
         *
         * Set PGSHAREDIR so that the embedded postgres subprocesses
         * (--boot, --check, standalone) find timezone and other share
         * files from our embedded installation, not from any system PG.
         */
        setenv("EMBED_POSTGRES_EXEC", postgres_bin, 1);
        setenv("PGSHAREDIR", abs_share, 1);

        const char *initdb_argv[] = {
            postgres_bin,           /* argv[0]: used as program name */
            "-D", opts->data_dir,
            "-L", abs_share,        /* share directory (absolute path) */
            "--no-locale",
            "-U", superuser,
            "-E", encoding,
            "-A", "trust",
            "-N",                   /* --no-sync: skip fsync for speed */
            "--no-instructions",    /* suppress startup instructions */
            NULL
        };

        int argc = 0;
        while (initdb_argv[argc]) argc++;

        int rc = initdb_main(argc, (char **)initdb_argv);
        _exit(rc);
    }

    /* Parent: wait for initdb child */
    int status;
    waitpid(child, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        fprintf(stderr, "embed_initdb: initdb failed (status=%d)\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }

    /* Write optimized postgresql.conf for embedded mode */
    char conf_path[1024];
    snprintf(conf_path, sizeof(conf_path), "%s/postgresql.conf", opts->data_dir);
    FILE *f = fopen(conf_path, "a");
    if (f)
    {
        fprintf(f,
            "\n# Embedded PostgreSQL optimizations\n"
            "max_wal_senders = 0\n"
            "wal_level = minimal\n"
            "fsync = off\n"
            "synchronous_commit = off\n"
            "full_page_writes = off\n"
        );
        fclose(f);
    }

    /* Append pg_hba.conf rule for embed connections (socketpair) */
    char hba_path[1024];
    snprintf(hba_path, sizeof(hba_path), "%s/pg_hba.conf", opts->data_dir);
    f = fopen(hba_path, "a");
    if (f)
    {
        fprintf(f,
            "\n# Trust all connections (for embed mode socketpair)\n"
            "host    all    all    0.0.0.0/0    trust\n"
            "host    all    all    ::/0          trust\n"
        );
        fclose(f);
    }

    fprintf(stderr, "embed_initdb: done.\n");
    return 0;
}
