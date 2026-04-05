# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Is

embed-postgres is a C library that embeds a full PostgreSQL 19devel server into a host process. PostgreSQL is compiled as a shared library, zstd-compressed, and embedded in the binary's data section. At runtime, a child process is forked which decompresses and loads the library via `dlopen`. Connections are established via `socketpair(AF_UNIX)` with fd passing over SCM_RIGHTS, returning standard `PGconn*` handles usable with the full libpq API.

## Build Commands

```bash
# First-time setup: prepare the patched PG source
bin/update    # clone upstream PG to works/postgres/
bin/apply     # sync to works/core/ and apply patches

# Full build (compiles PostgreSQL from source via meson, then builds embed library + demo)
mkdir build && cd build && cmake .. && cmake --build .

# First build takes several minutes (PG compilation). Subsequent builds are incremental.
# Build artifacts: build/libembed_pg.a, build/embed_postgres_demo

# Run the demo
PG_SHARE_DIR=build/pg-install/share ./embed_postgres_demo
```

## Architecture

### Two codebases in one repo

1. **Embed library** (`src/`, `include/`) — the C API that manages PG lifecycle
2. **PostgreSQL source** — upstream checkout at `works/postgres/`, patched working copy at `works/core/`

### Patch workflow

Patches are stored per-file in `works/patch/` (one `.patch` per modified PG source file, mirroring the directory structure). Three scripts manage the workflow:

1. **`bin/update`** — Clone or pull the latest upstream PostgreSQL to `works/postgres/`, record the commit hash in `works/rev`.
2. **`bin/apply`** — Sync upstream to `works/core/` (as a fresh git repo) and apply all patches from `works/patch/`. Run this after `bin/update` or when setting up a fresh checkout.
3. **`bin/generate`** — Diff `works/core/` against `works/postgres/`, writing per-file patches to `works/patch/`. Run this after editing files in `works/core/`, then commit `works/patch/`.

To modify PG behavior: edit files in `works/core/`, then run `bin/generate` to update patches.

### How the embedding works

PostgreSQL is built as a shared library (`libpg_server.dylib`/`.so`), compressed with zstd, and embedded in the binary via `.incbin` assembly. At startup:

1. **`fork()`** creates a child process
2. **`embed_load_pg_library()`** decompresses the blob and loads it via `dlopen`:
   - Linux: `memfd_create` → `write` → `dlopen("/proc/self/fd/N")` — 100% in-memory
   - macOS: `mkstemps` → `write` → `dlopen` → `unlink` — tmpfile exists briefly
3. **`dlsym()`** resolves `pg_main`, `embed_set_postmaster_mode`, `embed_set_control_fd`
4. **`pg_main()`** starts the postmaster, which monitors the control fd via its WaitEventSet

**Connection flow:** parent creates `socketpair()` → sends server fd to postmaster via **SCM_RIGHTS** on control channel → postmaster's `embed_handle_control_event()` receives fd and calls `BackendStartup()` inline → sends `'A'` ACK → parent wraps client fd in `PGconn` via `PQconnectSocket()`.

**Shutdown flow:** parent sends `'S'` byte on control channel → postmaster's `embed_handle_control_event()` sets `pending_pm_shutdown_request` → postmaster does normal shutdown → `exit()` terminates child → parent `waitpid()` collects status.

### Source files

| File | Role |
|------|------|
| `src/embed_postgres.c` | Server lifecycle: fork, child_main, start/stop/wait |
| `src/embed_connect.c` | Connection via SCM_RIGHTS fd passing + PQconnectSocket |
| `src/embed_loader.c` | Zstd decompression + dlopen (memfd on Linux, tmpfile on macOS) |
| `src/embed_initdb.c` | Data directory initialization (calls `initdb_main` in forked child) |
| `include/embed_postgres.h` | Public C API |
| `src/embed_internal.h` | Internal struct definitions (pid_t child_pid, control_fd) |
| `build/embed_blob.S` | Generated assembly: `.incbin` of compressed libpg_server |

### Key PG patch points

- **`postmaster.c`** — `embed_mode` flag, control fd monitoring via WaitEventSet, `embed_recv_fd()`/`embed_handle_control_event()` for inline SCM_RIGHTS + BackendStartup, embed API functions (`embed_set_postmaster_mode`, `embed_set_control_fd`)
- **`main.c`** — `main()` renamed to `pg_main()` with `__attribute__((weak))` wrapper
- **`fe-connect.c`** — `PQconnectSocket()` creates a PGconn from a pre-connected fd
- **`ipc.c`** — `embed_exit_code` for observability
- **`initdb.c`** — `main()` renamed to `initdb_main()` with `__attribute__((weak))` wrapper; `embed_pg_main_hook` + tokenizer/fork logic for embedded subprocess spawning

### Build system

CMake uses `ExternalProject_Add` to compile PostgreSQL via meson, then:

1. Links backend `.o` files + sub-archives into `libpg_server.dylib`/`.so` (with all system deps baked in)
2. Compresses with `zstd -19` → `libpg_server.zst`
3. Generates `embed_blob.S` with `.incbin` referencing the compressed file
4. Compiles the embed library linking **frontend** libs (libpq, pgcommon_shlib, pgcommon_excluded_shlib, pgport, pgfeutils) + initdb sources + zstd

The host binary carries no PG backend symbols — they live entirely within the embedded shared library, loaded only in the child process. However, the initdb frontend code (compiled from PG's `src/bin/initdb/`) is linked directly into the embed library, allowing `embed_pg_initdb()` to call `initdb_main()` in a forked child without needing an external binary.

**Note on pgcommon:** The embed library links `pgcommon_shlib.a` (for `pg_char_to_encoding` without the `_private` suffix, as libpq requires) plus `pgcommon_excluded_shlib.a` (for `pg_malloc`, `pg_logging_init`, and other frontend functions needed by the embedded initdb). These two archives are complementary and produce no duplicate symbols.

## Known Constraints

- **One PG instance per process** — PG uses extensive global state
- **initdb still requires the postgres binary** — initdb internally runs `postgres --boot` via `popen()` for bootstrapping; the binary must exist at `opts->pg_data_path/../bin/postgres`
- **macOS only currently** — uses `-force_load`, macOS frameworks; Linux support is structured (memfd path exists) but untested
- **Embed connections serialized** — parent-side mutex ensures one connection request is in flight at a time
