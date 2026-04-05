# 把 PostgreSQL 塞进你的进程：从零实现嵌入式 PostgreSQL 服务器

## 引言

SQLite 之所以无处不在，很大程度上归功于它的嵌入式架构——不需要独立的服务进程，直接链接到宿主程序就能用。那如果 PostgreSQL 也能这样呢？

这不是异想天开。PostgreSQL 的代码库其实已经具备了这个潜力：它的 meson 构建系统会生成一个 `postgres_lib` 静态库，包含了全部约 900 个后端 .c 文件编译出的目标文件。理论上，你只要链接这个库，就拥有了一个完整的 PostgreSQL 服务器内核。

本文记录了我从零实现 embed-postgres 库的完整过程——将 PostgreSQL 19devel 编译为共享库，zstd 压缩后嵌入宿主二进制的数据段，运行时 fork 子进程并通过 dlopen 加载，通过 SCM_RIGHTS 传递 socketpair fd 建立连接，返回标准的 `PGconn*`，让你可以用完整的 libpq API 操作一个不需要任何网络 socket 的 PostgreSQL 服务器。

最终效果：

```
=== Starting embedded PostgreSQL server ===
Server is ready!

=== Creating embedded connection ===
Connected! Backend PID: 56800

SELECT 1: num = 1, msg = hello embed
Version: PostgreSQL 19devel on aarch64-darwin, compiled by clang-16.0.0, 64-bit
CREATE TABLE: PGRES_COMMAND_OK
INSERT: id=1, name=embedded!

=== Testing second connection ===
Second connection OK! Backend PID: 56801
Rows visible from conn2: 1

Server stopped (exit code: 0)
Done!
```

## 架构设计

在开始写代码之前，需要想清楚几个核心问题：

**问题一：如何运行 PostgreSQL 而不污染宿主进程？**

最初的方案是把 postmaster 跑在宿主进程的一个 pthread 里。但这带来了一系列棘手的问题：PostgreSQL 使用大量全局变量，它的 `proc_exit()` 会调用 `exit()` 杀死整个进程，需要用 `pthread_exit()` 劫持退出路径；前端（libpq）和后端（postgres）链接在同一地址空间导致符号冲突；所有后端依赖（OpenSSL、ICU、zstd、lz4、curl、xml2 等）都被拖入宿主进程。

最终解决方案：**fork + dlopen 子进程模型**。将 PostgreSQL 编译为共享库，zstd 压缩后用 `.incbin` 嵌入宿主二进制的数据段。运行时 fork 子进程，在子进程中解压并 dlopen 加载。前端和后端运行在不同的进程中，彻底隔离符号和依赖。

**问题二：如何在不监听 socket 的情况下建立连接？**

正常的 PostgreSQL 通过 TCP 或 Unix domain socket 接受连接。但嵌入模式下，我们不想开任何端口。

解决方案：`socketpair(AF_UNIX, SOCK_STREAM, 0, sv)` 创建一对预连接的 fd。通过 **SCM_RIGHTS** 把 `sv[0]`（服务端）发送到子进程中的 postmaster，让它 fork 出一个 backend 来服务；把 `sv[1]`（客户端）包装成 `PGconn*` 返回给调用者。

**问题三：如何跨进程传递 fd 和控制命令？**

父子进程之间通过一个 Unix socket 控制通道通信。父进程通过 SCM_RIGHTS 发送 fd，子进程中的控制线程接收后交给 postmaster。关闭时发送 `'S'` 命令，控制线程调用 `embed_request_shutdown()`。

整体架构：

```
宿主进程（父）                          PostgreSQL 子进程
  |                                      |
  +-- embed_pg_initdb()                  |  (调用已安装的 initdb 二进制)
  |                                      |
  +-- embed_pg_start()                   |
  |     fork() ──────────────────────>   +-- embed_load_pg_library()
  |     控制通道 socketpair              |     zstd 解压 → dlopen
  |                                      +-- dlsym(pg_main, embed_*)
  |                                      +-- 启动控制线程
  |                                      +-- pg_main() → PostmasterMain
  |                                      |
  +-- embed_pg_connect()                 |
  |     socketpair()                     |
  |     SCM_RIGHTS 发送 sv[0] ───────>   +-- 控制线程: embed_submit_connection(sv[0])
  |     等待 ACK <────────────────────   |     postmaster fork backend
  |     PQconnectSocket(sv[1])           |
  |     返回 PGconn*                     |
  |                                      |
  +-- embed_pg_stop()                    |
        发送 'S' ─────────────────────>  +-- 控制线程: embed_request_shutdown()
        waitpid()                        +-- postmaster: proc_exit() → exit()
```

## 第一步：重命名 main() 为 pg_main()

PostgreSQL 的入口在 `src/backend/main/main.c`。这个 `main()` 函数做了基本的初始化后，根据命令行参数分发到不同的子程序：

```c
switch (dispatch_option)
{
    case DISPATCH_BOOT:
        BootstrapModeMain(argc, argv, false);  // initdb 的 bootstrap 阶段
        break;
    case DISPATCH_SINGLE:
        PostgresSingleUserMain(argc, argv, ...);  // 单用户模式
        break;
    case DISPATCH_POSTMASTER:
        PostmasterMain(argc, argv);  // 正常的服务器模式
        break;
}
abort();  // 上面的函数都不应该返回
```

我们需要做两件事：

1. 把 `main()` 改名为 `pg_main()`，这样子进程可以通过 `dlsym` 找到它
2. 添加一个 weak 的 `main()` 包装，让独立的 postgres 二进制文件仍然能正常工作

```c
int
pg_main(int argc, char *argv[])
{
    // ... 原来 main() 的所有代码 ...
}

// weak main() 让宿主程序可以提供自己的 main()
int __attribute__((weak))
main(int argc, char *argv[])
{
    return pg_main(argc, argv);
}
```

这里的 `__attribute__((weak))` 至关重要。当共享库被构建时，这个 weak `main()` 存在但不会与宿主程序冲突。子进程通过 `dlsym(handle, "pg_main")` 直接获取入口点。

## 第二步：允许零 socket 运行 + suppress 多线程检查

正常的 PostmasterMain 在创建完监听 socket 后会检查：

```c
if (NumListenSockets == 0)
    ereport(FATAL,
            (errmsg("no socket created for listening")));
```

在 embed-only 模式下，我们不需要任何监听 socket，所以加个条件：

```c
if (NumListenSockets == 0 && !embed_mode)
    ereport(FATAL,
            (errmsg("no socket created for listening")));
```

macOS 上的 PostgreSQL 有一套 `pthread_is_threaded_np()` 检查，如果进程变成多线程就报 FATAL 错误。这是因为 postmaster 需要 `fork()` + `sigprocmask()`，在多线程环境下这两个操作行为未定义。

在 embed mode 下，子进程中有一个控制线程在运行，所以需要 suppress 这些检查：

```c
// PostmasterMain startup 中
if (pthread_is_threaded_np() != 0 && !embed_mode)
    ereport(FATAL, ...);

// ServerLoop 中的 Assert
Assert(embed_mode || pthread_is_threaded_np() == 0);

// ExitPostmaster 中
if (pthread_is_threaded_np() != 0 && !embed_mode)
    ereport(LOG, ...);
```

## 第三步：实现 embed connect 通道

这是整个方案中最精妙的部分。在子进程模型下，连接流程涉及跨进程 fd 传递：

```
embed_pg_connect(srv, "mydb", "postgres"):
  1. socketpair(AF_UNIX, SOCK_STREAM, 0, sv)
  2. SCM_RIGHTS 发送 sv[0] 到子进程控制通道
  3. 子进程控制线程收到 fd:
     └── embed_submit_connection(sv[0])
     └── 设置 embed_pending_client_fd = sv[0]
     └── pending_pm_pmsignal = true
     └── SetLatch(MyLatch) ← 唤醒 postmaster 的 ServerLoop
     └── 发送 ACK 给父进程
  4. Postmaster 醒来，process_pm_pmsignal():
     └── 构造 ClientSocket{sock=sv[0], raddr=AF_UNIX}
     └── BackendStartup(&cs) → fork() backend 子进程
  5. 父进程收到 ACK:
     └── PQconnectSocket(sv[1], "dbname=mydb user=postgres")
     └── 发送 StartupMessage，完成握手
     └── 返回 CONNECTION_OK 的 PGconn*
```

SCM_RIGHTS 是 Unix domain socket 的一个能力——它允许通过 socket 传递文件描述符。发送端把 fd 附在控制消息（ancillary data）中：

```c
static int
send_fd(int socket, int fd_to_send, char cmd)
{
    struct iovec iov = { .iov_base = &cmd, .iov_len = 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cmsgbuf, .msg_controllen = sizeof(cmsgbuf)
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));
    return sendmsg(socket, &msg, 0);
}
```

相比之前 pthread 模型中直接调用 `embed_submit_connection` 后 `usleep(50ms)` 等待 postmaster 处理，SCM_RIGHTS + ACK 机制更可靠——父进程会阻塞等待子进程明确确认 fd 已提交。

在 `process_pm_pmsignal` 中处理连接请求的代码没有变化：

```c
if (embed_connect_pending && embed_pending_client_fd >= 0)
{
    ClientSocket cs;
    cs.sock = embed_pending_client_fd;
    memset(&cs.raddr, 0, sizeof(cs.raddr));
    cs.raddr.addr.ss_family = AF_UNIX;
    cs.raddr.salen = sizeof(struct sockaddr_un);
    BackendStartup(&cs);
    embed_pending_client_fd = -1;
    embed_connect_pending = false;
}
```

这里 `cs.raddr.addr.ss_family = AF_UNIX` 很重要——它让 PostgreSQL 的 `pg_hba.conf` 检查把这个连接当作 "local" 连接，匹配 `local all all trust` 规则。

## 第四步：实现 PQconnectSocket

正常的 libpq 连接流程是：`PQconnectStart` → DNS 解析 → `connect()` → SSL/GSS 协商 → 发送 StartupMessage → 完成认证握手。对于 socketpair 连接，我们已经有了一个连接好的 fd，需要跳过前面的步骤，直接从发送 StartupMessage 开始。

在 `fe-connect.c` 末尾添加 `PQconnectSocket`：

```c
PGconn *
PQconnectSocket(int fd, const char *conninfo)
{
    PGconn     *conn;

    // 1. 创建空的 PGconn 并解析连接参数
    conn = pqMakeEmptyPGconn();
    connOptions = conninfo_parse(conninfo, &errorBuf, true);
    fillPGconn(conn, connOptions);
    pqConnectOptions2(conn);

    // 2. 安装已连接的 fd
    conn->sock = fd;
    conn->pversion = PG_PROTOCOL_LATEST;

    // 3. 设置状态为 CONNECTION_MADE，跳过 SSL/GSS 协商
    conn->status = CONNECTION_MADE;
    conn->current_enc_method = ENC_PLAINTEXT;

    // 4. 驱动 PQconnectPoll 完成握手
    for (;;)
    {
        poll_status = PQconnectPoll(conn);
        if (poll_status == PGRES_POLLING_OK) break;
        if (poll_status == PGRES_POLLING_FAILED) break;

        if (poll_status == PGRES_POLLING_READING)
            PQsocketPoll(conn->sock, 1, 0, -1);
        else if (poll_status == PGRES_POLLING_WRITING)
            PQsocketPoll(conn->sock, 0, 1, -1);
    }

    return conn;
}
```

这里有个容易踩坑的点：**必须设置 `conn->pversion = PG_PROTOCOL_LATEST`**。正常连接流程中，协议版本在 `PQconnectPoll` 的 `CONNECTION_NEEDED` 阶段设置。我们跳过了那个阶段直接进入 `CONNECTION_MADE`，如果不手动设置，`pversion` 为 0，发出的 StartupMessage 会声称使用协议 0.0，服务端直接拒绝：

```
FATAL: unsupported frontend protocol 0.0: server supports 3.0 to 3.2
```

还需要设置一个虚拟的 `connhost` 数组，否则 `PQconnectPoll` 在访问主机信息时会 crash。

## 第五步：构建共享库 + 压缩嵌入

这是新架构中最关键的构建系统改动。整个流程：

```
meson 编译 PG
    ↓
从 ~900 个 .o 文件 + 9 个子 archive → 链接为 libpg_server.dylib (~10MB)
    ↓
zstd -19 压缩 → libpg_server.zst (~3.3MB)
    ↓
.incbin 嵌入 → embed_blob.S → embed_blob.o → libembed_pg.a
```

### 构建共享库

meson 的 `postgres_lib` 目标不会生成独立的 `.a` 文件，而是在 `postgres_lib.a.p/` 目录下存放 `.o` 文件，另外还有 9 个独立的子 archive（`boot_parser.a`、`nodefuncs.a` 等）。我们需要把这些全部链接为一个共享库：

```cmake
# macOS
set(PG_SHARED_LIB_CMD
    "cd ${PG_BUILD} && ${CMAKE_C_COMPILER} -shared \
    -o libpg_server.dylib \
    $(find src/backend/postgres_lib.a.p -name '*.o') \
    -force_load src/backend/bootstrap/boot_parser.a \
    -force_load src/backend/nodes/nodefuncs.a \
    -force_load src/backend/utils/misc/guc-file.a \
    ... (9 个子 archive) \
    -Lsrc/common -lpgcommon_srv -Lsrc/port -lpgport_srv \
    ${PG_SYS_LINK_FLAGS} \
    -install_name @rpath/libpg_server.dylib")
```

**所有后端系统依赖**（OpenSSL、ICU、zstd、lz4、curl、xml2、xslt、LDAP、PAM 等）和 macOS 框架都链接进这个共享库。宿主进程不再需要这些依赖。

### 压缩 + 嵌入

zstd level 19 把 9.8MB 的 dylib 压缩到 3.3MB（67% 压缩率）。然后用汇编的 `.incbin` 指令把压缩数据嵌入二进制的数据段：

```cmake
# 在 CMake configure 时生成 embed_blob.S
file(WRITE ${CMAKE_BINARY_DIR}/embed_blob.S
".section __DATA,__pg_blob
.global _embed_pg_blob_start
.global _embed_pg_blob_end
.balign 16
_embed_pg_blob_start:
    .incbin \"${PG_BUILD}/libpg_server.zst\"
_embed_pg_blob_end:
")
```

在 C 代码中通过 `extern` 符号引用这些数据：

```c
extern const unsigned char embed_pg_blob_start[];
extern const unsigned char embed_pg_blob_end[];
```

### 运行时加载

在子进程中，`embed_load_pg_library()` 解压并加载：

```c
void *embed_load_pg_library(void)
{
    const size_t compressed_size = embed_pg_blob_end - embed_pg_blob_start;

    // 从 zstd frame header 获取解压大小
    unsigned long long decompressed_size =
        ZSTD_getFrameContentSize(embed_pg_blob_start, compressed_size);

    // 解压
    void *buf = malloc(decompressed_size);
    ZSTD_decompress(buf, decompressed_size, embed_pg_blob_start, compressed_size);

#ifdef __linux__
    // Linux: memfd_create — 100% 内存操作，无磁盘 I/O
    int fd = memfd_create("libpg_server", MFD_CLOEXEC);
    write(fd, buf, decompressed_size);
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    close(fd);
#else
    // macOS: tmpfile → write → dlopen → unlink（文件仅存在毫秒级）
    int fd = mkstemps(tmppath, 6);  // .dylib 后缀
    write(fd, buf, decompressed_size);
    close(fd);
    handle = dlopen(tmppath, RTLD_NOW | RTLD_GLOBAL);
    unlink(tmppath);  // 立即删除——mmap 映射保持有效
#endif

    free(buf);
    return handle;
}
```

关键点：
- **只在子进程中运行**——fork 之后解压，避免在父进程中浪费内存
- `RTLD_GLOBAL` 让所有后端符号全局可见，PostgreSQL 的 extension 动态加载（如 plpgsql.so）需要这些符号
- macOS 上临时文件存在时间不到 1ms——mkstemps 到 unlink 之间只有 write 和 dlopen
- Linux 上通过 memfd_create 完全不涉及磁盘

## 第六步：子进程生命周期

子进程的入口函数 `child_main`：

```c
static void child_main(int ctl_fd, const embed_pg_server *srv)
{
    // 1. 加载 PostgreSQL 共享库
    void *handle = embed_load_pg_library();

    // 2. 通过 dlsym 解析入口点
    pg_main_fn fn_pg_main = dlsym(handle, "pg_main");
    embed_set_mode_fn fn_set_mode = dlsym(handle, "embed_set_postmaster_mode");
    embed_submit_fn fn_submit = dlsym(handle, "embed_submit_connection");
    embed_shutdown_fn fn_shutdown = dlsym(handle, "embed_request_shutdown");

    // 3. 启动控制线程（接收父进程的 fd 传递和关闭命令）
    pthread_create(&ctl_thread, NULL, control_thread, &args);

    // 4. 启用 embed 模式并运行 PostgreSQL
    fn_set_mode(true);
    fn_pg_main(argc, argv);

    _exit(0);
}
```

**关闭机制极其简洁**：在之前的 pthread 模型中，需要用 `pthread_exit()` 劫持 `proc_exit()`，因为 `exit()` 会杀死整个宿主进程。在子进程模型中，`proc_exit()` → `exit()` 只终止子进程，父进程通过 `waitpid()` 收集退出状态。不需要任何退出路径的 hack。

## 第七步：前端/后端符号隔离

这是子进程模型带来的最大红利之一。在之前的 pthread + 静态链接模型中，`postgres_lib.a`（后端）和 `libpq.a`（前端）链接在同一地址空间，产生符号冲突。

一个典型例子是 `pg_char_to_encoding` 函数。后端通过宏把它重命名为 `pg_char_to_encoding_private`，但 libpq 引用原始名称。之前需要写一个 `embed_compat.c` 桥接层：

```c
// 之前需要的兼容层（现在已删除）
int pg_char_to_encoding(const char *name)
{
    return pg_char_to_encoding_private(name);
}
```

在子进程模型中，前端（libpq）在父进程，后端（libpg_server.so）在子进程——完全不同的地址空间，根本不会冲突。`embed_compat.c` 直接删除。

不过有一个细微之处：libpq.a 需要链接 `pgcommon_shlib.a`（为共享库编译的版本），而不是 `pgcommon.a`（为静态二进制编译的版本）。因为只有 shlib 版本提供没有 `_private` 后缀的编码函数名。

## 第八步：initdb 的务实选择

initdb 仍然通过调用已安装的 `initdb` 二进制来完成：

```c
int embed_pg_initdb(const embed_pg_options *opts)
{
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "%s/../bin/initdb -D '%s' --no-locale -U '%s' --encoding='%s' -A trust",
        opts->pg_data_path, opts->data_dir,
        opts->superuser, opts->encoding);
    return system(cmd);
}
```

虽然理论上可以在子进程中 dlopen PG 库后调用 `pg_main("--boot", ...)`，但 initdb 的逻辑远比单纯的 bootstrap 复杂——它需要执行几十个 SQL 文件来创建系统视图和类型。直接调用 initdb 二进制是最可靠的选择。

## 最终的 C API

公共 API 完全没有变化——从使用者的角度看，内部是 pthread 还是 fork+dlopen 是透明的：

```c
typedef struct embed_pg_options {
    const char *data_dir;        // PGDATA 路径
    const char *pg_data_path;    // PG share 目录（initdb 需要的 BKI 文件）
    const char *superuser;       // 超级用户名，默认 "postgres"
    const char *encoding;        // 数据库编码，默认 "UTF8"
    const char *listen_address;  // 网络地址（NULL = 纯嵌入模式）
    int         port;            // 端口（0 = 纯嵌入模式）
    const char *unix_socket_dir; // Unix socket 路径（NULL = 不启用）
} embed_pg_options;

int embed_pg_initdb(const embed_pg_options *opts);
embed_pg_server *embed_pg_create(void);
int embed_pg_start(embed_pg_server *srv, const embed_pg_options *opts);
int embed_pg_wait_ready(embed_pg_server *srv, int timeout_ms);
PGconn *embed_pg_connect(embed_pg_server *srv, const char *dbname, const char *username);
int embed_pg_stop(embed_pg_server *srv);
void embed_pg_destroy(embed_pg_server *srv);
```

使用示例：

```cpp
// 初始化数据目录
embed_pg_options opts = {};
opts.data_dir = "/tmp/embed_pg_data";
opts.pg_data_path = "/path/to/pg/share";
embed_pg_initdb(&opts);

// 启动服务器（纯嵌入模式，无网络 socket）
embed_pg_server *srv = embed_pg_create();
embed_pg_start(srv, &opts);
embed_pg_wait_ready(srv, 30000);

// 创建连接，使用标准 libpq API
PGconn *conn = embed_pg_connect(srv, "postgres", "postgres");
PGresult *res = PQexec(conn, "SELECT version()");
printf("%s\n", PQgetvalue(res, 0, 0));

// 关闭
PQfinish(conn);
embed_pg_stop(srv);
embed_pg_destroy(srv);
```

## 修改统计

对 PostgreSQL 源码的总改动量：

```
 src/backend/main/main.c             |  12 ++-
 src/backend/postmaster/postmaster.c  |  64 ++++++++++++--
 src/backend/storage/ipc/ipc.c       |   8 ++
 src/include/postmaster/postmaster.h  |   5 +
 src/include/storage/ipc.h           |   1 +
 src/include/storage/pmsignal.h      |   3 +-
 src/interfaces/libpq/fe-connect.c   | 141 +++++++++++++++++++++++++++++
 src/interfaces/libpq/libpq-fe.h     |   3 +
 8 files changed, 227 insertions(+), 10 deletions(-)
```

8 个文件，约 230 行改动。相比之前的 pthread 模型，补丁更精简了——不再需要 `ipc.c` 中的 `pthread_exit()` 劫持路径。

宿主侧的源文件：

| 文件 | 行数 | 作用 |
|------|------|------|
| `embed_postgres.c` | ~300 | fork + child_main + 控制线程 + 生命周期 |
| `embed_connect.c` | ~95 | SCM_RIGHTS fd 传递 + PQconnectSocket |
| `embed_loader.c` | ~120 | zstd 解压 + memfd/tmpfile + dlopen |
| `embed_initdb.c` | ~65 | 调用已安装的 initdb 二进制 |
| `embed_blob.S` | 8 | .incbin 嵌入压缩的共享库 |

## 对比：pthread 模型 vs fork+dlopen 模型

| 维度 | pthread 模型 | fork+dlopen 模型 |
|------|------------|-----------------|
| PG 运行方式 | 宿主进程的线程 | 独立子进程 |
| 退出处理 | `pthread_exit()` 劫持 `proc_exit()` | 正常 `exit()`，父进程 `waitpid()` |
| 符号隔离 | 需要 `embed_compat.c` 桥接 | 完全隔离，无冲突 |
| 依赖污染 | 所有 PG 依赖进入宿主 | 宿主只链接 libpq + zstd |
| 连接同步 | `usleep(50ms)` | SCM_RIGHTS + ACK |
| 二进制大小 | ~20MB+ | ~3.7MB |
| fork-safety | 宿主线程需注意 | 无影响（PG 在独立进程） |

## 限制与未来

当前实现的已知限制：

1. **每个进程只能有一个 PostgreSQL 实例**：PostgreSQL 使用了大量全局变量，无法在同一进程中运行多个实例
2. **initdb 依赖外部二进制**：目前 initdb 阶段需要调用已安装的 initdb 程序
3. **embed 连接序列化**：`embed_submit_connection` 使用单个 pending slot，并发的 `embed_pg_connect` 调用需要通过 mutex 序列化
4. **macOS only**：Linux 支持的代码路径已就绪（memfd_create）但尚未测试

未来可以探索的方向：
- Linux 平台测试和支持
- 将 initdb 也嵌入子进程执行
- 支持 WAL-less 模式，让嵌入式场景获得更极致的性能
- 将 share 数据（BKI 文件等）也嵌入二进制，实现完全自包含

## 结语

这个项目经历了两代架构。第一代（pthread + 静态链接）验证了核心思路的可行性，但暴露了符号污染和依赖膨胀的问题。第二代（fork + dlopen + zstd 嵌入）彻底解决了这些问题，同时让宿主二进制从 20MB+ 缩小到 3.7MB，依赖从 15+ 个库减少到只有 libpq 和 zstd。

最大的收获是：进程隔离是个强大的抽象。通过 fork + dlopen 把 PostgreSQL 放在自己的地址空间中，所有前端/后端符号冲突的问题一夜之间消失了，退出处理变成了一行 `waitpid()`，构建系统也大幅简化。

完整代码：所有源文件和 patch 都在 `embed-postgres/` 项目中，欢迎探索和改进。