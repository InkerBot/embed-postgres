#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "embed_postgres.h"
#include "embed_internal.h"

#include <zstd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Emitted by embed_blob.S: the zstd-compressed programs */
extern const unsigned char embed_postgres_blob_start[];
extern const unsigned char embed_postgres_blob_end[];
extern const unsigned char embed_initdb_blob_start[];
extern const unsigned char embed_initdb_blob_end[];

static int
write_all(int fd, const void *buf, size_t len)
{
    size_t written = 0;

    while (written < len) {
        ssize_t n = write(fd, (const char *)buf + written, len - written);
        if (n <= 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

/* Decompress one embedded program into `path`, executable by us alone. */
static int
extract_program(const unsigned char *start, const unsigned char *end,
                const char *path)
{
    const size_t compressed_size = (size_t)(end - start);
    unsigned long long size = ZSTD_getFrameContentSize(start, compressed_size);
    void *buf;
    size_t result;
    int fd;

    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR) {
        fprintf(stderr, "embed_loader: invalid zstd frame header\n");
        return -1;
    }

    buf = malloc((size_t)size);
    if (!buf) {
        fprintf(stderr, "embed_loader: malloc failed (%llu bytes)\n", size);
        return -1;
    }

    result = ZSTD_decompress(buf, (size_t)size, start, compressed_size);
    if (ZSTD_isError(result)) {
        fprintf(stderr, "embed_loader: zstd decompress failed: %s\n",
                ZSTD_getErrorName(result));
        free(buf);
        return -1;
    }

    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    if (fd < 0) {
        fprintf(stderr, "embed_loader: cannot create %s: %s\n",
                path, strerror(errno));
        free(buf);
        return -1;
    }

    if (write_all(fd, buf, (size_t)size) < 0) {
        fprintf(stderr, "embed_loader: writing %s failed: %s\n",
                path, strerror(errno));
        close(fd);
        unlink(path);
        free(buf);
        return -1;
    }

    free(buf);

    /*
     * Close before anyone execs it: a descriptor still open for writing makes
     * execve() fail with ETXTBSY.
     */
    if (close(fd) < 0) {
        fprintf(stderr, "embed_loader: closing %s failed: %s\n",
                path, strerror(errno));
        unlink(path);
        return -1;
    }
    return 0;
}

static int
link_or_fail(const char *target, const char *linkpath)
{
    if (symlink(target, linkpath) < 0) {
        fprintf(stderr, "embed_loader: cannot link %s -> %s: %s\n",
                linkpath, target, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Materialise the embedded programs as a miniature PostgreSQL installation.
 *
 * PostgreSQL derives share and library paths from the location of its own
 * executable, so the programs cannot simply be dropped in a scratch directory:
 * they have to sit in a tree that looks like an install prefix.  bin/ holds the
 * real files, while lib/ and share/ are symlinks into the installation the
 * caller pointed us at.
 *
 *     <dir>/bin/postgres   <dir>/bin/initdb   <dir>/lib -> ...   <dir>/share -> ...
 *
 * Returns 0 on success, with the prefix written to `prefix_out` (PATH_MAX).
 */
int
embed_extract_runtime(const char *share_dir, char *prefix_out)
{
    const char *tmpdir = getenv("TMPDIR");
    char prefix[PATH_MAX];
    char path[PATH_MAX];
    char real_share[PATH_MAX];
    char real_lib[PATH_MAX];
    char *sep;

    if (!share_dir || !prefix_out)
        return -1;

    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";

    snprintf(prefix, sizeof(prefix), "%s/embed-pg-XXXXXX", tmpdir);
    if (mkdtemp(prefix) == NULL) {
        fprintf(stderr, "embed_loader: mkdtemp failed: %s\n", strerror(errno));
        return -1;
    }

    snprintf(path, sizeof(path), "%s/bin", prefix);
    if (mkdir(path, 0700) < 0) {
        fprintf(stderr, "embed_loader: mkdir %s failed: %s\n",
                path, strerror(errno));
        goto fail;
    }

    snprintf(path, sizeof(path), "%s/bin/postgres", prefix);
    if (extract_program(embed_postgres_blob_start, embed_postgres_blob_end,
                        path) < 0)
        goto fail;

    snprintf(path, sizeof(path), "%s/bin/initdb", prefix);
    if (extract_program(embed_initdb_blob_start, embed_initdb_blob_end,
                        path) < 0)
        goto fail;

    if (realpath(share_dir, real_share) == NULL)
        snprintf(real_share, sizeof(real_share), "%s", share_dir);

    snprintf(real_lib, sizeof(real_lib), "%s", real_share);
    sep = strrchr(real_lib, '/');
    if (sep && sep != real_lib)
        *sep = '\0';
    strncat(real_lib, "/lib", sizeof(real_lib) - strlen(real_lib) - 1);

    snprintf(path, sizeof(path), "%s/share", prefix);
    if (link_or_fail(real_share, path) < 0)
        goto fail;

    snprintf(path, sizeof(path), "%s/lib", prefix);
    if (link_or_fail(real_lib, path) < 0)
        goto fail;

    snprintf(prefix_out, PATH_MAX, "%s", prefix);
    return 0;

fail:
    embed_remove_runtime(prefix);
    return -1;
}

void
embed_remove_runtime(const char *prefix)
{
    char path[PATH_MAX];

    if (!prefix || !*prefix)
        return;

    snprintf(path, sizeof(path), "%s/bin/postgres", prefix);
    unlink(path);
    snprintf(path, sizeof(path), "%s/bin/initdb", prefix);
    unlink(path);
    snprintf(path, sizeof(path), "%s/bin", prefix);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/share", prefix);
    unlink(path);
    snprintf(path, sizeof(path), "%s/lib", prefix);
    unlink(path);
    rmdir(prefix);
}
