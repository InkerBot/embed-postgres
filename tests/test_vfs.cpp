/*
 * The cluster lives wherever the host says, in whatever form it likes.
 *
 * The first fixture keeps the bytes on disk but under names PostgreSQL never
 * sees; the second keeps them in the host's own memory, so a server that runs
 * at all proves nothing reached a file system.
 */
#include "test_helpers.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool exists_on_disk(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// ---- A host that rewrites every name before it reaches the disk ----

struct RemapFs {
    std::string from;
    std::string to;
    std::atomic<int> opens{0};
    std::atomic<int> reads{0};
    std::atomic<int> writes{0};

    std::string map(const char *path) const {
        std::string p(path);
        if (p.rfind(from, 0) == 0)
            return to + p.substr(from.size());
        return p;
    }
};

RemapFs *remap() {
    return static_cast<RemapFs *>(embed_pg_fs_get_context()->private_data);
}

int rm_getattr(const char *path, struct stat *st, embed_pg_fs_file_info *fi) {
    if (fi && fi->fh) return ::fstat((int)fi->fh, st) == 0 ? 0 : -errno;
    return ::stat(remap()->map(path).c_str(), st) == 0 ? 0 : -errno;
}
int rm_access(const char *path, int mask) {
    return ::access(remap()->map(path).c_str(), mask) == 0 ? 0 : -errno;
}
int rm_mkdir(const char *path, mode_t mode) {
    return ::mkdir(remap()->map(path).c_str(), mode) == 0 ? 0 : -errno;
}
int rm_unlink(const char *path) {
    return ::unlink(remap()->map(path).c_str()) == 0 ? 0 : -errno;
}
int rm_rmdir(const char *path) {
    return ::rmdir(remap()->map(path).c_str()) == 0 ? 0 : -errno;
}
int rm_rename(const char *from, const char *to, unsigned int) {
    return ::rename(remap()->map(from).c_str(),
                    remap()->map(to).c_str()) == 0 ? 0 : -errno;
}
int rm_chmod(const char *path, mode_t mode, embed_pg_fs_file_info *) {
    return ::chmod(remap()->map(path).c_str(), mode) == 0 ? 0 : -errno;
}
int rm_truncate(const char *path, off_t size, embed_pg_fs_file_info *fi) {
    if (fi) return ::ftruncate((int)fi->fh, size) == 0 ? 0 : -errno;
    return ::truncate(remap()->map(path).c_str(), size) == 0 ? 0 : -errno;
}
int rm_open_common(const char *path, int flags, mode_t mode,
                   embed_pg_fs_file_info *fi) {
    remap()->opens++;
    int fd = ::open(remap()->map(path).c_str(), flags, mode);
    if (fd < 0) return -errno;
    fi->fh = (unsigned long long)fd;
    return 0;
}
int rm_open(const char *path, embed_pg_fs_file_info *fi) {
    return rm_open_common(path, fi->flags, 0, fi);
}
int rm_create(const char *path, mode_t mode, embed_pg_fs_file_info *fi) {
    return rm_open_common(path, fi->flags, mode, fi);
}
int rm_read(const char *, char *buf, size_t size, off_t off,
            embed_pg_fs_file_info *fi) {
    remap()->reads++;
    ssize_t n = ::pread((int)fi->fh, buf, size, off);
    return n < 0 ? -errno : (int)n;
}
int rm_write(const char *, const char *buf, size_t size, off_t off,
             embed_pg_fs_file_info *fi) {
    remap()->writes++;
    ssize_t n = ::pwrite((int)fi->fh, buf, size, off);
    return n < 0 ? -errno : (int)n;
}
int rm_fsync(const char *, int, embed_pg_fs_file_info *fi) {
    return ::fsync((int)fi->fh) == 0 ? 0 : -errno;
}
int rm_release(const char *, embed_pg_fs_file_info *fi) {
    return ::close((int)fi->fh) == 0 ? 0 : -errno;
}
int rm_opendir(const char *path, embed_pg_fs_file_info *fi) {
    DIR *d = ::opendir(remap()->map(path).c_str());
    if (!d) return -errno;
    fi->fh = (unsigned long long)(uintptr_t)d;
    return 0;
}
int rm_readdir(const char *, void *buf, embed_pg_fs_fill_dir filler,
               embed_pg_fs_file_info *fi) {
    DIR *d = (DIR *)(uintptr_t)fi->fh;
    ::rewinddir(d);
    while (struct dirent *e = ::readdir(d)) {
        struct stat st{};
        st.st_ino = e->d_ino;
        st.st_mode = (mode_t)(e->d_type << 12);
        if (filler(buf, e->d_name, &st) != 0) break;
    }
    return 0;
}
int rm_releasedir(const char *, embed_pg_fs_file_info *fi) {
    return ::closedir((DIR *)(uintptr_t)fi->fh) == 0 ? 0 : -errno;
}

embed_pg_fs make_remap_fs() {
    embed_pg_fs fs = {};
    fs.getattr = rm_getattr;
    fs.access = rm_access;
    fs.mkdir = rm_mkdir;
    fs.unlink = rm_unlink;
    fs.rmdir = rm_rmdir;
    fs.rename = rm_rename;
    fs.chmod = rm_chmod;
    fs.truncate = rm_truncate;
    fs.open = rm_open;
    fs.create = rm_create;
    fs.read = rm_read;
    fs.write = rm_write;
    fs.fsync = rm_fsync;
    fs.release = rm_release;
    fs.opendir = rm_opendir;
    fs.readdir = rm_readdir;
    fs.releasedir = rm_releasedir;
    return fs;
}

// ---- A host that keeps the whole cluster in its own memory ----

struct MemFs {
    struct Node {
        bool is_dir = false;
        mode_t mode = 0644;
        std::vector<char> data;
    };

    std::string root = "/pgdata";
    std::mutex mutex;
    std::map<std::string, Node> nodes;      // absolute path -> node
    std::map<unsigned long long, std::string> open_files;
    unsigned long long next_handle = 1;
    std::atomic<long long> bytes_written{0};

    MemFs() { nodes["/"] = Node{true, (mode_t)0755, {}}; }

    static std::string parent_of(const std::string &path) {
        auto slash = path.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return "/";
        return path.substr(0, slash);
    }

    // Entries directly inside `dir`
    std::vector<std::string> children(const std::string &dir) {
        std::vector<std::string> out;
        std::string prefix = (dir == "/") ? "/" : dir + "/";
        for (auto &kv : nodes) {
            if (kv.first == dir || kv.first.rfind(prefix, 0) != 0) continue;
            std::string rest = kv.first.substr(prefix.size());
            if (rest.empty() || rest.find('/') != std::string::npos) continue;
            out.push_back(rest);
        }
        return out;
    }
};

MemFs *mem() {
    return static_cast<MemFs *>(embed_pg_fs_get_context()->private_data);
}

// The cluster is ours; the binaries and share files the server also opens are
// not, and go to the real filesystem exactly as a host would leave them.
bool ours(const char *path);

#define DELEGATE(call)                                                      \
    do {                                                                    \
        const embed_pg_fs *d = &embed_pg_fs_passthrough;         \
        return d->call;                                                     \
    } while (0)


int mem_getattr(const char *path, struct stat *st, embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(getattr(path, st, fi));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    auto it = mem()->nodes.find(path);
    if (it == mem()->nodes.end()) return -ENOENT;
    std::memset(st, 0, sizeof(*st));
    st->st_mode = (it->second.is_dir ? S_IFDIR : S_IFREG) | it->second.mode;
    st->st_nlink = it->second.is_dir ? 2 : 1;
    st->st_size = (off_t)it->second.data.size();
    st->st_blocks = (blkcnt_t)((it->second.data.size() + 511) / 512);
    st->st_uid = ::getuid();
    st->st_gid = ::getgid();
    // Distinct inodes matter: the backend identifies loaded libraries by them
    st->st_ino = (ino_t)std::hash<std::string>{}(path);
    st->st_dev = 1;
    return 0;
}
int mem_access(const char *path, int mask) {
    if (!ours(path)) DELEGATE(access(path, mask));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    return mem()->nodes.count(path) ? 0 : -ENOENT;
}
int mem_mkdir(const char *path, mode_t mode) {
    if (!ours(path)) DELEGATE(mkdir(path, mode));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    if (mem()->nodes.count(path)) return -EEXIST;
    if (!mem()->nodes.count(MemFs::parent_of(path))) return -ENOENT;
    mem()->nodes[path] = MemFs::Node{true, (mode_t)mode, {}};
    return 0;
}
int mem_unlink(const char *path) {
    if (!ours(path)) DELEGATE(unlink(path));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    return mem()->nodes.erase(path) ? 0 : -ENOENT;
}
int mem_rmdir(const char *path) {
    if (!ours(path)) DELEGATE(rmdir(path));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    auto it = mem()->nodes.find(path);
    if (it == mem()->nodes.end()) return -ENOENT;
    if (!mem()->children(path).empty()) return -ENOTEMPTY;
    mem()->nodes.erase(it);
    return 0;
}
int mem_rename(const char *from, const char *to, unsigned int flags) {
    if (!ours(from)) DELEGATE(rename(from, to, flags));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    auto it = mem()->nodes.find(from);
    if (it == mem()->nodes.end()) return -ENOENT;
    MemFs::Node node = it->second;
    mem()->nodes.erase(it);
    mem()->nodes[to] = std::move(node);
    return 0;
}
int mem_chmod(const char *path, mode_t mode, embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(chmod(path, mode, fi));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    auto it = mem()->nodes.find(path);
    if (it == mem()->nodes.end()) return -ENOENT;
    it->second.mode = mode;
    return 0;
}
int mem_truncate(const char *path, off_t size, embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(truncate(path, size, fi));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    std::string name = path;
    if (fi) {
        auto h = mem()->open_files.find(fi->fh);
        if (h != mem()->open_files.end()) name = h->second;
    }
    auto it = mem()->nodes.find(name);
    if (it == mem()->nodes.end()) return -ENOENT;
    it->second.data.resize((size_t)size, 0);
    return 0;
}
int mem_open_common(const char *path, int flags, mode_t mode,
                    embed_pg_fs_file_info *fi) {
    std::lock_guard<std::mutex> lock(mem()->mutex);
    auto it = mem()->nodes.find(path);
    if (it == mem()->nodes.end()) {
        if (!(flags & O_CREAT)) return -ENOENT;
        if (!mem()->nodes.count(MemFs::parent_of(path))) return -ENOENT;
        mem()->nodes[path] = MemFs::Node{false, (mode_t)(mode ? mode : 0644), {}};
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    } else if (flags & O_TRUNC) {
        it->second.data.clear();
    }
    fi->fh = mem()->next_handle++;
    mem()->open_files[fi->fh] = path;
    return 0;
}
int mem_open(const char *path, embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(open(path, fi));
    return mem_open_common(path, fi->flags, 0, fi);
}
int mem_create(const char *path, mode_t mode, embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(create(path, mode, fi));
    return mem_open_common(path, fi->flags | O_CREAT, mode, fi);
}
int mem_read(const char *path, char *buf, size_t size, off_t off,
             embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(read(path, buf, size, off, fi));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    auto h = mem()->open_files.find(fi->fh);
    if (h == mem()->open_files.end()) return -EBADF;
    auto it = mem()->nodes.find(h->second);
    if (it == mem()->nodes.end()) return -ENOENT;
    const auto &data = it->second.data;
    if ((size_t)off >= data.size()) return 0;
    size_t n = std::min(size, data.size() - (size_t)off);
    std::memcpy(buf, data.data() + off, n);
    return (int)n;
}
int mem_write(const char *path, const char *buf, size_t size, off_t off,
              embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(write(path, buf, size, off, fi));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    auto h = mem()->open_files.find(fi->fh);
    if (h == mem()->open_files.end()) return -EBADF;
    auto &data = mem()->nodes[h->second].data;
    if (data.size() < (size_t)off + size) data.resize((size_t)off + size, 0);
    std::memcpy(data.data() + off, buf, size);
    mem()->bytes_written += (long long)size;
    return (int)size;
}
int mem_fsync(const char *path, int ds, embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(fsync(path, ds, fi));
    return 0;
}
int mem_release(const char *path, embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(release(path, fi));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    mem()->open_files.erase(fi->fh);
    return 0;
}
int mem_opendir(const char *path, embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(opendir(path, fi));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    auto it = mem()->nodes.find(path);
    if (it == mem()->nodes.end()) return -ENOENT;
    if (!it->second.is_dir) return -ENOTDIR;
    fi->fh = mem()->next_handle++;
    mem()->open_files[fi->fh] = path;
    return 0;
}
int mem_readdir(const char *path, void *buf, embed_pg_fs_fill_dir filler,
                embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(readdir(path, buf, filler, fi));
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(mem()->mutex);
        names = mem()->children(path);
    }
    struct stat st{};
    st.st_mode = S_IFREG;
    filler(buf, ".", &st);
    filler(buf, "..", &st);
    for (auto &n : names) filler(buf, n.c_str(), &st);
    return 0;
}
int mem_releasedir(const char *path, embed_pg_fs_file_info *fi) {
    if (!ours(path)) DELEGATE(releasedir(path, fi));
    std::lock_guard<std::mutex> lock(mem()->mutex);
    mem()->open_files.erase(fi->fh);
    return 0;
}

bool ours(const char *path) {
    return std::string(path).rfind(mem()->root, 0) == 0;
}

embed_pg_fs make_mem_fs() {
    embed_pg_fs fs = {};
    fs.getattr = mem_getattr;
    fs.access = mem_access;
    fs.mkdir = mem_mkdir;
    fs.unlink = mem_unlink;
    fs.rmdir = mem_rmdir;
    fs.rename = mem_rename;
    fs.chmod = mem_chmod;
    fs.truncate = mem_truncate;
    fs.open = mem_open;
    fs.create = mem_create;
    fs.read = mem_read;
    fs.write = mem_write;
    fs.fsync = mem_fsync;
    fs.release = mem_release;
    fs.opendir = mem_opendir;
    fs.readdir = mem_readdir;
    fs.releasedir = mem_releasedir;
    return fs;
}

}  // namespace

TEST(VfsTest, ClusterLivesWhereTheHostPutsIt) {
    TempDataDir real;
    ASSERT_FALSE(real.path().empty());

    // The name the server is given, which never reaches the file system
    const std::string claimed = "/embed-vfs-does-not-exist";

    RemapFs state;
    state.from = claimed;
    state.to = real.path();
    embed_pg_fs fs = make_remap_fs();

    embed_pg_options opts = {};
    opts.data_dir = claimed.c_str();
    opts.pg_data_path = get_pg_share_dir();
    opts.superuser = "postgres";
    opts.encoding = "UTF8";
    opts.fs = &fs;
    opts.fs_private_data = &state;

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    EXPECT_FALSE(exists_on_disk(claimed));
    EXPECT_TRUE(exists_on_disk(real.path() + "/PG_VERSION"));

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    ASSERT_EQ(embed_pg_start(srv, &opts), 0);
    ASSERT_EQ(embed_pg_wait_ready(srv, 30000), 0);

    PGconn *conn = embed_pg_connect(srv, "postgres", "postgres");
    ASSERT_NE(conn, nullptr);
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK) << PQerrorMessage(conn);

    PGresult *res = PQexec(conn, "CREATE TABLE via_vfs (id int, note text)");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK) << PQerrorMessage(conn);
    PQclear(res);
    res = PQexec(conn, "INSERT INTO via_vfs VALUES (1, 'through the host')");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK) << PQerrorMessage(conn);
    PQclear(res);
    res = PQexec(conn, "SELECT note FROM via_vfs WHERE id = 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "through the host");
    PQclear(res);

    PQfinish(conn);
    embed_pg_stop(srv);
    embed_pg_destroy(srv);

    EXPECT_FALSE(exists_on_disk(claimed));
    EXPECT_GT(state.opens.load(), 100);
    EXPECT_GT(state.reads.load(), 0) << "reads must come through the host";
    EXPECT_GT(state.writes.load(), 0) << "writes must come through the host";
}

/*
 * Nothing on disk at all: the cluster is built, queried and read back out of
 * the host's own memory.
 */
TEST(VfsTest, ClusterInHostMemory) {
    MemFs store;
    embed_pg_fs fs = make_mem_fs();

    const std::string data_dir = "/pgdata";

    embed_pg_options opts = {};
    opts.data_dir = data_dir.c_str();
    opts.pg_data_path = get_pg_share_dir();
    opts.superuser = "postgres";
    opts.encoding = "UTF8";
    opts.fs = &fs;
    opts.fs_private_data = &store;

    ASSERT_EQ(embed_pg_initdb(&opts), 0);
    EXPECT_FALSE(exists_on_disk(data_dir));
    EXPECT_GT(store.bytes_written.load(), 0);

    {
        std::lock_guard<std::mutex> lock(store.mutex);
        EXPECT_TRUE(store.nodes.count("/pgdata/PG_VERSION"))
            << "initdb should have left a cluster in memory";
        EXPECT_TRUE(store.nodes.count("/pgdata/global"));
    }

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    ASSERT_EQ(embed_pg_start(srv, &opts), 0);
    ASSERT_EQ(embed_pg_wait_ready(srv, 30000), 0);

    PGconn *conn = embed_pg_connect(srv, "postgres", "postgres");
    ASSERT_NE(conn, nullptr);
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK) << PQerrorMessage(conn);

    PGresult *res = PQexec(conn, "CREATE TABLE in_memory (id int, note text)");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK) << PQerrorMessage(conn);
    PQclear(res);

    res = PQexec(conn, "INSERT INTO in_memory SELECT g, 'row ' || g "
                       "FROM generate_series(1, 500) g");
    ASSERT_EQ(PQresultStatus(res), PGRES_COMMAND_OK) << PQerrorMessage(conn);
    PQclear(res);

    res = PQexec(conn, "SELECT count(*), max(note) FROM in_memory");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK) << PQerrorMessage(conn);
    EXPECT_STREQ(PQgetvalue(res, 0, 0), "500");
    PQclear(res);

    // Force it out of shared buffers so the pages come back through the host
    res = PQexec(conn, "CHECKPOINT");
    PQclear(res);

    PQfinish(conn);
    embed_pg_stop(srv);
    embed_pg_destroy(srv);

    EXPECT_FALSE(exists_on_disk(data_dir));
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        EXPECT_GT(store.nodes.size(), 100u)
            << "the whole cluster should be in the map";
    }
}

/*
 * A directory far larger than one readdir batch: every entry has to come back
 * exactly once, across however many exchanges it takes.
 */
TEST(VfsTest, LargeDirectoryListsCompletely) {
    constexpr int kFiles = 5000;       // ~300kB of entries, several batches

    MemFs store;
    embed_pg_fs fs = make_mem_fs();
    const std::string data_dir = "/pgdata";

    embed_pg_options opts = {};
    opts.data_dir = data_dir.c_str();
    opts.pg_data_path = get_pg_share_dir();
    opts.superuser = "postgres";
    opts.encoding = "UTF8";
    opts.fs = &fs;
    opts.fs_private_data = &store;

    ASSERT_EQ(embed_pg_initdb(&opts), 0);
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        store.nodes["/pgdata/many"] = MemFs::Node{true, (mode_t)0700, {}};
        for (int i = 0; i < kFiles; i++) {
            char name[64];
            snprintf(name, sizeof(name),
                     "/pgdata/many/entry-with-a-longish-name-%05d", i);
            store.nodes[name] = MemFs::Node{};
        }
    }

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    ASSERT_EQ(embed_pg_start(srv, &opts), 0);
    ASSERT_EQ(embed_pg_wait_ready(srv, 30000), 0);

    PGconn *conn = embed_pg_connect(srv, "postgres", "postgres");
    ASSERT_NE(conn, nullptr);
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK) << PQerrorMessage(conn);

    PGresult *res = PQexec(conn,
        "SELECT count(*), count(DISTINCT f), min(f), max(f) "
        "FROM pg_ls_dir('many') f");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK) << PQerrorMessage(conn);
    EXPECT_EQ(std::stoi(PQgetvalue(res, 0, 0)), kFiles);
    EXPECT_EQ(std::stoi(PQgetvalue(res, 0, 1)), kFiles);
    EXPECT_STREQ(PQgetvalue(res, 0, 2), "entry-with-a-longish-name-00000");
    EXPECT_STREQ(PQgetvalue(res, 0, 3), "entry-with-a-longish-name-04999");
    PQclear(res);

    PQfinish(conn);
    embed_pg_stop(srv);
    embed_pg_destroy(srv);
}

/*
 * How fast the host channel is, not whether it works.  Disabled so ctest does
 * not pay for it; run with
 *   ./test_vfs --gtest_also_run_disabled_tests --gtest_filter='*Bench*'
 *
 * Everything lives in MemFs, which offers no open_fd, so every page read and
 * written crosses the channel.  The server is restarted before each scan so
 * that shared buffers are cold and the pages have to come from the host.
 */
namespace {

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start).count();
}

void exec_ok(PGconn *conn, const char *sql) {
    PGresult *res = PQexec(conn, sql);
    ExecStatusType st = PQresultStatus(res);
    EXPECT_TRUE(st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK)
        << sql << ": " << PQerrorMessage(conn);
    PQclear(res);
}

embed_pg_server *bench_start(const embed_pg_options *opts) {
    embed_pg_server *srv = embed_pg_create();
    EXPECT_EQ(embed_pg_start(srv, opts), 0);
    EXPECT_EQ(embed_pg_wait_ready(srv, 30000), 0);
    return srv;
}

void bench_stop(embed_pg_server *srv) {
    embed_pg_stop(srv);
    embed_pg_destroy(srv);
}

}  // namespace

/*
 * The same workload wherever the cluster lives.  fsync is turned off throughout:
 * MemFs has nothing to sync, and a disk would otherwise be measured for its
 * flushes rather than for the path the bytes take.
 */
static void run_bench(const char *label, const embed_pg_options &opts) {
    constexpr int kTables = 4;
    constexpr int kRows = 80000;       // ~18MB per table

    auto t = std::chrono::steady_clock::now();
    ASSERT_EQ(embed_pg_initdb(&opts), 0);
    double t_initdb = seconds_since(t);

    embed_pg_server *srv = bench_start(&opts);
    PGconn *conn = embed_pg_connect(srv, "postgres", "postgres");
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK);
    exec_ok(conn, "ALTER SYSTEM SET fsync = off");
    exec_ok(conn, "SELECT pg_reload_conf()");

    t = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; i++) {
        exec_ok(conn, "CREATE TABLE churn (id int)");
        exec_ok(conn, "DROP TABLE churn");
    }
    double t_ddl = seconds_since(t);

    t = std::chrono::steady_clock::now();
    for (int i = 0; i < kTables; i++) {
        std::string sql = "CREATE TABLE bench" + std::to_string(i) +
                          " AS SELECT g AS id, repeat('x', 200) AS pad "
                          "FROM generate_series(1, " + std::to_string(kRows) +
                          ") g";
        exec_ok(conn, sql.c_str());
    }
    exec_ok(conn, "CHECKPOINT");
    double t_load = seconds_since(t);
    PQfinish(conn);
    bench_stop(srv);

    // One scanner, cold
    srv = bench_start(&opts);
    conn = embed_pg_connect(srv, "postgres", "postgres");
    t = std::chrono::steady_clock::now();
    exec_ok(conn, "SELECT sum(length(pad)) FROM bench0");
    double t_scan1 = seconds_since(t);
    PQfinish(conn);
    bench_stop(srv);

    // Several scanners at once, each on its own table, cold
    srv = bench_start(&opts);
    std::vector<PGconn *> conns;
    for (int i = 0; i < kTables; i++)
        conns.push_back(embed_pg_connect(srv, "postgres", "postgres"));
    t = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < kTables; i++) {
        threads.emplace_back([&, i] {
            std::string sql = "SELECT sum(length(pad)) FROM bench" +
                              std::to_string(i);
            exec_ok(conns[i], sql.c_str());
        });
    }
    for (auto &th : threads) th.join();
    double t_scanN = seconds_since(t);
    for (auto *c : conns) PQfinish(c);
    bench_stop(srv);

    const double table_mb = 18.0;
    printf("\n  [%s]\n", label);
    printf("  initdb                     %7.3f s\n", t_initdb);
    printf("  200x CREATE+DROP TABLE     %7.3f s\n", t_ddl);
    printf("  load %d tables + CHECKPOINT %7.3f s\n", kTables, t_load);
    printf("  cold scan, 1 conn          %7.3f s  (%.0f MB/s)\n",
           t_scan1, table_mb / t_scan1);
    printf("  cold scan, %d conns         %7.3f s  (%.0f MB/s)\n\n",
           kTables, t_scanN, table_mb * kTables / t_scanN);
}

TEST(VfsTest, DISABLED_BenchInHostMemory) {
    MemFs store;
    embed_pg_fs fs = make_mem_fs();

    embed_pg_options opts = {};
    opts.data_dir = "/pgdata";
    opts.pg_data_path = get_pg_share_dir();
    opts.superuser = "postgres";
    opts.encoding = "UTF8";
    opts.fs = &fs;
    opts.fs_private_data = &store;
    run_bench("MemFs: everything through the host", opts);
}

TEST(VfsTest, DISABLED_BenchPassthrough) {
    TempDataDir dir;
    embed_pg_options opts = dir.options();
    opts.fs = &embed_pg_fs_passthrough;
    run_bench("passthrough: names through the host, bytes by descriptor", opts);
}

TEST(VfsTest, DISABLED_BenchKernel) {
    TempDataDir dir;
    embed_pg_options opts = dir.options();
    opts.fs = nullptr;
    run_bench("no host: the kernel alone", opts);
}
