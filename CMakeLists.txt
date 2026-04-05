cmake_minimum_required(VERSION 3.20)
project(embed_postgres C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

include(ExternalProject)

set(PG_SOURCE ${CMAKE_SOURCE_DIR}/works/core)
set(PG_BUILD ${CMAKE_BINARY_DIR}/pg-build)
set(PG_INSTALL ${CMAKE_BINARY_DIR}/pg-install)

# Detect platform
if(APPLE)
    set(SHARED_EXT "dylib")
else()
    set(SHARED_EXT "so")
endif()

# Find zstd: command-line tool (compression at build time) and library (decompression at runtime)
find_program(ZSTD_PROGRAM zstd REQUIRED)
find_library(ZSTD_LIBRARY zstd REQUIRED)
find_path(ZSTD_INCLUDE_DIR zstd.h)

# Find system libraries needed by the PG shared library
find_library(SSL_LIBRARY ssl)
find_library(CRYPTO_LIBRARY crypto)
find_library(LZ4_LIBRARY lz4)
find_library(CURL_LIBRARY curl)
find_library(XML2_LIBRARY xml2)
find_library(XSLT_LIBRARY xslt)
find_library(LDAP_LIBRARY ldap)
find_library(PAM_LIBRARY pam)
find_library(ICU_UC_LIBRARY icuuc)
find_library(ICU_I18N_LIBRARY icui18n)
find_library(ICU_DATA_LIBRARY icudata)

# Build system deps link flags for the PG shared library
set(PG_SYS_LINK_FLAGS "")
macro(append_pg_dep var)
    if(${var})
        # macOS frameworks must use -framework, not the path directly
        if("${${var}}" MATCHES "\\.framework")
            get_filename_component(_fw_name "${${var}}" NAME)
            string(REGEX REPLACE "\\.framework$" "" _fw_name "${_fw_name}")
            string(APPEND PG_SYS_LINK_FLAGS " -framework ${_fw_name}")
        else()
            string(APPEND PG_SYS_LINK_FLAGS " ${${var}}")
        endif()
    endif()
endmacro()
append_pg_dep(SSL_LIBRARY)
append_pg_dep(CRYPTO_LIBRARY)
append_pg_dep(ZSTD_LIBRARY)
append_pg_dep(LZ4_LIBRARY)
append_pg_dep(CURL_LIBRARY)
append_pg_dep(XML2_LIBRARY)
append_pg_dep(XSLT_LIBRARY)
append_pg_dep(LDAP_LIBRARY)
append_pg_dep(PAM_LIBRARY)
append_pg_dep(ICU_UC_LIBRARY)
append_pg_dep(ICU_I18N_LIBRARY)
append_pg_dep(ICU_DATA_LIBRARY)
string(APPEND PG_SYS_LINK_FLAGS " -lz -lpthread -ldl -lm")

if(APPLE)
    string(APPEND PG_SYS_LINK_FLAGS " -framework CoreFoundation -framework Security -framework Kerberos")
endif()

# Platform-specific shared library build command
if(APPLE)
    set(PG_SHARED_LIB_CMD
        "cd ${PG_BUILD} && ${CMAKE_C_COMPILER} -shared \
        -o libpg_server.dylib \
        $(find src/backend/postgres_lib.a.p -name '*.o') \
        -force_load src/backend/bootstrap/boot_parser.a \
        -force_load src/backend/nodes/nodefuncs.a \
        -force_load src/backend/utils/misc/guc-file.a \
        -force_load src/backend/utils/adt/jsonpath.a \
        -force_load src/backend/parser/parser.a \
        -force_load src/backend/replication/repl_parser.a \
        -force_load src/backend/utils/activity/wait_event_names.a \
        -force_load src/backend/utils/adt/numeric_backend_lib.a \
        -force_load src/backend/storage/page/checksum_backend_lib.a \
        -Lsrc/common -lpgcommon_srv -Lsrc/port -lpgport_srv \
        ${PG_SYS_LINK_FLAGS} \
        -install_name @rpath/libpg_server.dylib")
else()
    set(PG_SHARED_LIB_CMD
        "cd ${PG_BUILD} && ${CMAKE_C_COMPILER} -shared \
        -o libpg_server.so \
        $(find src/backend/postgres_lib.a.p -name '*.o') \
        -Wl,--whole-archive \
            src/backend/bootstrap/boot_parser.a \
            src/backend/nodes/nodefuncs.a \
            src/backend/utils/misc/guc-file.a \
            src/backend/utils/adt/jsonpath.a \
            src/backend/parser/parser.a \
            src/backend/replication/repl_parser.a \
            src/backend/utils/activity/wait_event_names.a \
            src/backend/utils/adt/numeric_backend_lib.a \
            src/backend/storage/page/checksum_backend_lib.a \
        -Wl,--no-whole-archive \
        -Lsrc/common -lpgcommon_srv -Lsrc/port -lpgport_srv \
        ${PG_SYS_LINK_FLAGS} \
        -Wl,-soname,libpg_server.so")
endif()

# Cross-compilation support
set(MESON_CROSS_FILE "" CACHE FILEPATH "Meson cross-file for cross-compilation")
set(MESON_SETUP_ARGS --prefix=${PG_INSTALL} --buildtype=release -Dreadline=disabled)
if(MESON_CROSS_FILE)
    list(APPEND MESON_SETUP_ARGS --cross-file ${MESON_CROSS_FILE})
endif()

# Build PG via meson, create shared library, and compress
ExternalProject_Add(pg_ext
    SOURCE_DIR ${PG_SOURCE}
    BINARY_DIR ${PG_BUILD}
    INSTALL_DIR ${PG_INSTALL}
    CONFIGURE_COMMAND meson setup ${PG_BUILD} ${PG_SOURCE} ${MESON_SETUP_ARGS}
    BUILD_COMMAND meson compile -C ${PG_BUILD}
    INSTALL_COMMAND meson install -C ${PG_BUILD}
        # Create shared library from backend objects
        COMMAND ${CMAKE_COMMAND} -E echo "Creating libpg_server.${SHARED_EXT}..."
        COMMAND sh -c "${PG_SHARED_LIB_CMD}"
        # Compress with zstd
        COMMAND ${CMAKE_COMMAND} -E echo "Compressing libpg_server with zstd..."
        COMMAND ${ZSTD_PROGRAM} -19 -f
            -o ${PG_BUILD}/libpg_server.zst
            ${PG_BUILD}/libpg_server.${SHARED_EXT}
)

# Generate embed_blob.S at configure time.
# The .incbin directive references the .zst file which is created at build time
# (assembly happens after pg_ext, ensured by add_dependencies below).
if(APPLE)
    file(WRITE ${CMAKE_BINARY_DIR}/embed_blob.S
".section __DATA,__pg_blob
.global _embed_pg_blob_start
.global _embed_pg_blob_end
.balign 16
_embed_pg_blob_start:
    .incbin \"${PG_BUILD}/libpg_server.zst\"
_embed_pg_blob_end:
")
else()
    file(WRITE ${CMAKE_BINARY_DIR}/embed_blob.S
".section .rodata,\"a\",@progbits
.global embed_pg_blob_start
.global embed_pg_blob_end
.balign 16
embed_pg_blob_start:
    .incbin \"${PG_BUILD}/libpg_server.zst\"
embed_pg_blob_end:
")
endif()

# Import frontend PG libraries (only these are needed by the host process)
add_library(libpq_static STATIC IMPORTED)
set_target_properties(libpq_static PROPERTIES
    IMPORTED_LOCATION ${PG_BUILD}/src/interfaces/libpq/libpq.a)

# Use pgcommon_shlib (not pgcommon) — libpq expects pg_char_to_encoding
# without the _private suffix, which only pgcommon_shlib provides.
add_library(pgcommon_shlib STATIC IMPORTED)
set_target_properties(pgcommon_shlib PROPERTIES
    IMPORTED_LOCATION ${PG_BUILD}/src/common/libpgcommon_shlib.a)

# Frontend pgcommon objects excluded from shlib — needed by embedded initdb
# for pg_malloc, pg_logging_init, pg_log_generic, etc.
# Using excluded_shlib avoids duplicate symbols with pgcommon_shlib.
add_library(pgcommon_excluded STATIC IMPORTED)
set_target_properties(pgcommon_excluded PROPERTIES
    IMPORTED_LOCATION ${PG_BUILD}/src/common/libpgcommon_excluded_shlib.a)

add_library(pgport STATIC IMPORTED)
set_target_properties(pgport PROPERTIES
    IMPORTED_LOCATION ${PG_INSTALL}/lib/libpgport.a)

add_library(libpq_oauth STATIC IMPORTED)
set_target_properties(libpq_oauth PROPERTIES
    IMPORTED_LOCATION ${PG_BUILD}/src/interfaces/libpq-oauth/libpq-oauth.a)

# Frontend utility library — needed by embedded initdb for
# appendShellString, option_parse_int, parse_sync_method.
add_library(pgfeutils STATIC IMPORTED)
set_target_properties(pgfeutils PROPERTIES
    IMPORTED_LOCATION ${PG_BUILD}/src/fe_utils/libpgfeutils.a)

# Embed library
add_library(embed_pg STATIC
    src/embed_postgres.c
    src/embed_connect.c
    src/embed_initdb.c
    src/embed_loader.c
    ${CMAKE_BINARY_DIR}/embed_blob.S
)
target_include_directories(embed_pg PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${PG_SOURCE}/src/interfaces/libpq
    ${PG_INSTALL}/include
)
if(ZSTD_INCLUDE_DIR)
    target_include_directories(embed_pg PRIVATE ${ZSTD_INCLUDE_DIR})
endif()

# Compile PG's initdb sources into the embed library (frontend code).
# This allows embed_pg_initdb() to call initdb_main() directly instead
# of shelling out to an external initdb binary.
add_library(embed_initdb_pg OBJECT
    ${PG_SOURCE}/src/bin/initdb/initdb.c
    ${PG_SOURCE}/src/bin/initdb/findtimezone.c
    ${PG_SOURCE}/src/timezone/localtime.c
)
target_include_directories(embed_initdb_pg PRIVATE
    ${PG_SOURCE}/src/include
    ${PG_BUILD}/src/include
    ${PG_SOURCE}/src/interfaces/libpq
    ${PG_SOURCE}/src/timezone
    ${PG_INSTALL}/include
)
target_compile_definitions(embed_initdb_pg PRIVATE
    FRONTEND
)
add_dependencies(embed_initdb_pg pg_ext)
target_link_libraries(embed_pg PUBLIC embed_initdb_pg)
target_link_libraries(embed_pg PUBLIC
    libpq_static
    libpq_oauth
    pgcommon_shlib pgcommon_excluded pgport pgfeutils
    pthread dl m z
    ${ZSTD_LIBRARY}
)

# Frontend system deps: libpq needs SSL+LDAP, libpq-oauth needs curl
if(SSL_LIBRARY AND CRYPTO_LIBRARY)
    target_link_libraries(embed_pg PUBLIC ${SSL_LIBRARY} ${CRYPTO_LIBRARY})
endif()
if(LDAP_LIBRARY)
    target_link_libraries(embed_pg PUBLIC ${LDAP_LIBRARY})
endif()
if(CURL_LIBRARY)
    target_link_libraries(embed_pg PUBLIC ${CURL_LIBRARY})
endif()
if(APPLE)
    target_link_libraries(embed_pg PUBLIC
        "-framework CoreFoundation"
        "-framework Security"
        "-framework Kerberos"
    )
endif()
add_dependencies(embed_pg pg_ext)

# Demo executable
add_executable(embed_postgres_demo main.cpp)
target_include_directories(embed_postgres_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PG_SOURCE}/src/interfaces/libpq
    ${PG_INSTALL}/include
)
target_link_libraries(embed_postgres_demo embed_pg)

# ============================================================
# Tests (Google Test)
# ============================================================
option(BUILD_TESTS "Build test executables" ON)

if(BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    FetchContent_MakeAvailable(googletest)

    macro(add_embed_test test_name)
        add_executable(${test_name}
            tests/${test_name}.cpp
            tests/test_helpers.cpp
        )
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/tests
            ${PG_SOURCE}/src/interfaces/libpq
            ${PG_INSTALL}/include
        )
        target_compile_definitions(${test_name} PRIVATE
            TEST_PG_SHARE_DIR="${PG_INSTALL}/share"
            TEST_PG_BIN_DIR="${PG_INSTALL}/bin"
        )
        target_link_libraries(${test_name} GTest::gtest_main embed_pg)
        add_dependencies(${test_name} pg_ext)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "PG_SHARE_DIR=${PG_INSTALL}/share;PG_BIN_DIR=${PG_INSTALL}/bin"
            TIMEOUT 120
        )
    endmacro()

    add_embed_test(test_lifecycle)
    add_embed_test(test_connection)
    add_embed_test(test_sql)
    add_embed_test(test_initdb)
    add_embed_test(test_stress)
    add_embed_test(test_edge_cases)
    add_embed_test(test_concurrency)
    add_embed_test(test_advanced_sql)
    set_tests_properties(test_stress PROPERTIES TIMEOUT 300)
    set_tests_properties(test_edge_cases PROPERTIES TIMEOUT 120)
    set_tests_properties(test_concurrency PROPERTIES TIMEOUT 180)
    set_tests_properties(test_advanced_sql PROPERTIES TIMEOUT 180)
endif()