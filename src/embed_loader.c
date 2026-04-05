/*
 * embed_loader.c
 *    In-memory decompression and loading of the embedded PostgreSQL shared library.
 *
 * The compressed shared library is embedded in the binary's data section via
 * .incbin. This module decompresses it and loads it via dlopen:
 *   - Linux: memfd_create (100% in-memory, no disk I/O)
 *   - macOS: tmpfile + dlopen + unlink (tmpfile exists briefly)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <zstd.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

/* Symbols defined by embed_blob.S */
extern const unsigned char embed_pg_blob_start[];
extern const unsigned char embed_pg_blob_end[];

/*
 * Decompress the embedded zstd blob and load it as a shared library.
 * Called in child process after fork().
 * Returns dlopen handle, or NULL on failure.
 */
void *
embed_load_pg_library(void)
{
    const size_t compressed_size = (size_t)(embed_pg_blob_end - embed_pg_blob_start);

    /* Get decompressed size from zstd frame header */
    unsigned long long decompressed_size =
        ZSTD_getFrameContentSize(embed_pg_blob_start, compressed_size);

    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN ||
        decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        fprintf(stderr, "embed_loader: invalid zstd frame header\n");
        return NULL;
    }

    /* Decompress */
    void *buf = malloc((size_t)decompressed_size);
    if (!buf) {
        fprintf(stderr, "embed_loader: malloc failed (%llu bytes)\n",
                decompressed_size);
        return NULL;
    }

    size_t result = ZSTD_decompress(buf, (size_t)decompressed_size,
                                     embed_pg_blob_start, compressed_size);
    if (ZSTD_isError(result)) {
        fprintf(stderr, "embed_loader: zstd decompress failed: %s\n",
                ZSTD_getErrorName(result));
        free(buf);
        return NULL;
    }

    void *handle = NULL;
    int fd;
    size_t written;

#ifdef __linux__
    /* Linux: memfd_create — purely in-memory, no disk I/O */
    fd = memfd_create("libpg_server", MFD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "embed_loader: memfd_create failed\n");
        free(buf);
        return NULL;
    }

    written = 0;
    while (written < (size_t)decompressed_size) {
        ssize_t n = write(fd, (char *)buf + written,
                          (size_t)decompressed_size - written);
        if (n <= 0) {
            fprintf(stderr, "embed_loader: write to memfd failed\n");
            close(fd);
            free(buf);
            return NULL;
        }
        written += (size_t)n;
    }

    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
    handle = dlopen(fdpath, RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
        fprintf(stderr, "embed_loader: dlopen failed: %s\n", dlerror());
    close(fd);
#else
    /* macOS: tmpfile, write, dlopen, immediately unlink */
    {
        const char *tmpdir = getenv("TMPDIR");
        if (!tmpdir) tmpdir = "/tmp";
        char tmppath[PATH_MAX];
        snprintf(tmppath, sizeof(tmppath), "%s/libpg_XXXXXX.dylib", tmpdir);
        fd = mkstemps(tmppath, 6);  /* .dylib suffix required for dlopen */
        if (fd < 0) {
            fprintf(stderr, "embed_loader: mkstemps failed\n");
            free(buf);
            return NULL;
        }

        written = 0;
        while (written < (size_t)decompressed_size) {
            ssize_t n = write(fd, (char *)buf + written,
                              (size_t)decompressed_size - written);
            if (n <= 0) {
                fprintf(stderr, "embed_loader: write to tmpfile failed\n");
                close(fd);
                unlink(tmppath);
                free(buf);
                return NULL;
            }
            written += (size_t)n;
        }

        close(fd);
        handle = dlopen(tmppath, RTLD_NOW | RTLD_GLOBAL);
        if (!handle)
            fprintf(stderr, "embed_loader: dlopen failed: %s\n", dlerror());
        unlink(tmppath);  /* Delete immediately — mapping persists in memory */
    }
#endif

    free(buf);
    return handle;
}