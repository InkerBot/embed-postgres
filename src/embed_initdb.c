#include "embed_postgres.h"
#include "embed_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int
embed_pg_initdb(const embed_pg_options *opts)
{
    if (!opts || !opts->data_dir || !opts->pg_data_path)
        return -1;

    const char *superuser = opts->superuser ? opts->superuser : "postgres";
    const char *encoding  = opts->encoding  ? opts->encoding  : "UTF8";

    /*
     * Every path below goes through the caller's resolver too: the data
     * directory exists in whatever namespace it defines, and this process
     * cannot reach it any other way.
     */
    const embed_pg_fs *fs = opts->fs ? opts->fs
        : &embed_pg_fs_passthrough;
    struct stat st;

    embed_pg_fs_enter(opts->fs_private_data);

    /*
     * Refuse a directory that already holds a cluster, the same way initdb
     * itself would.  Checking up front is what lets the outcome be read off the
     * file system afterwards: a version stamp left by an earlier run would
     * otherwise be indistinguishable from one this run produced.
     */
    char version_path[PATH_MAX];

    snprintf(version_path, sizeof(version_path), "%s/PG_VERSION",
             opts->data_dir);
    if (fs->getattr(version_path, &st, NULL) == 0) {
        fprintf(stderr, "embed_initdb: %s already contains a database cluster\n",
                opts->data_dir);
        return -1;
    }

    /*
     * initdb finds the backend it bootstraps with next to itself, so it runs
     * out of the same extracted prefix the postmaster does.
     */
    char runtime[PATH_MAX];

    if (embed_extract_runtime(opts->pg_data_path, runtime) < 0)
        return -1;

    char initdb_bin[PATH_MAX];
    snprintf(initdb_bin, sizeof(initdb_bin), "%s/bin/initdb", runtime);

    const char *argv[] = {
        initdb_bin,
        "-D", opts->data_dir,
        "--no-locale",
        "-U", superuser,
        "-E", encoding,
        "-A", "trust",
        "-N",                   /* --no-sync */
        "--no-instructions",
        NULL
    };

    fprintf(stderr, "embed_initdb: initializing data directory %s...\n",
            opts->data_dir);

    /*
     * initdb speaks no control protocol, so completion is observed instead of
     * reported: it inherits the write end of this pipe, which the kernel closes
     * for it — and for anything it spawned — on exit, and the read below then
     * sees end of file.  waitpid() would be the obvious way to wait, and is
     * exactly what a host's own child reaping can take away from us.
     */
    int done[2];

    if (pipe(done) < 0) {
        fprintf(stderr, "embed_initdb: pipe failed: %s\n", strerror(errno));
        embed_remove_runtime(runtime);
        return -1;
    }

    /* Nothing to serve when the caller did not bring a filesystem */
    char vfs_sock[PATH_MAX];
    const char *vfs_sock_arg = NULL;
    struct embed_vfs_server *vfs_server = NULL;

    if (opts->fs != NULL) {
        snprintf(vfs_sock, sizeof(vfs_sock), "%s/vfs.sock", runtime);
        vfs_server = embed_vfs_server_start(opts->fs, opts->fs_private_data,
                                            vfs_sock);
        if (vfs_server == NULL) {
            close(done[0]);
            close(done[1]);
            embed_remove_runtime(runtime);
            return -1;
        }
        vfs_sock_arg = vfs_sock;
    }

    pid_t child;
    if (embed_spawn(initdb_bin, argv, done[1], vfs_sock_arg, &child) < 0) {
        fprintf(stderr, "embed_initdb: cannot run initdb: %s\n",
                strerror(errno));
        close(done[0]);
        close(done[1]);
        embed_vfs_server_stop(vfs_server);
        embed_remove_runtime(runtime);
        return -1;
    }
    close(done[1]);

    for (;;) {
        char byte;
        ssize_t n = read(done[0], &byte, 1);

        if (n == 0)
            break;                      /* every writer is gone: it finished */
        if (n < 0 && errno != EINTR) {
            fprintf(stderr, "embed_initdb: waiting for initdb failed: %s\n",
                    strerror(errno));
            break;
        }
    }
    close(done[0]);

    embed_reap(child);
    embed_vfs_server_stop(vfs_server);
    embed_remove_runtime(runtime);

    /*
     * And the outcome is read off the result, not off an exit status: initdb
     * tears the directory down again when it fails, so a data directory that
     * carries a version stamp is one that was built.
     */
    if (fs->getattr(version_path, &st, NULL) != 0) {
        fprintf(stderr, "embed_initdb: initdb did not produce a data directory\n");
        return -1;
    }

    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/postgresql.conf", opts->data_dir);

    embed_pg_fs_file_info fi;

    memset(&fi, 0, sizeof(fi));
    fi.flags = O_WRONLY | O_APPEND;
    if (fs->open && fs->open(conf_path, &fi) == 0)
    {
        static const char settings[] =
            "\n# Embedded PostgreSQL optimizations\n"
            "max_wal_senders = 0\n"
            "wal_level = minimal\n"
            "fsync = off\n"
            "synchronous_commit = off\n"
            "full_page_writes = off\n";
        size_t remaining = sizeof(settings) - 1;
        const char *p = settings;
        off_t offset = st.st_size;

        if (fs->getattr(conf_path, &st, &fi) == 0)
            offset = st.st_size;

        while (remaining > 0 && fs->write)
        {
            int n = fs->write(conf_path, p, remaining, offset, &fi);

            if (n <= 0)
                break;
            p += n;
            offset += n;
            remaining -= (size_t) n;
        }
        if (fs->release)
            fs->release(conf_path, &fi);
    }

    /*
     * pg_hba.conf is deliberately left as initdb generated it: embed
     * connections look like AF_UNIX peers, so they already match its "local all
     * all trust" line.  A blanket "host all all 0.0.0.0/0 trust" would add
     * nothing here while exposing the cluster the moment a caller sets
     * listen_address/port.
     */

    fprintf(stderr, "embed_initdb: done.\n");
    return 0;
}
