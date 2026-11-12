cmake_minimum_required(VERSION 3.20)
project(embed_postgres C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

include(ExternalProject)

set(PG_SOURCE ${CMAKE_SOURCE_DIR}/works/core)
set(PG_BUILD ${CMAKE_BINARY_DIR}/pg-build)
set(PG_INSTALL ${CMAKE_BINARY_DIR}/pg-install)

# The tool compresses at build time, the library decompresses at runtime
find_program(ZSTD_PROGRAM zstd REQUIRED)
find_library(ZSTD_LIBRARY zstd REQUIRED)
find_path(ZSTD_INCLUDE_DIR zstd.h)

# Only what the frontend libpq in the host process needs; the backend's own
# dependencies are resolved inside the postgres binary we embed.
find_library(SSL_LIBRARY ssl)
find_library(CRYPTO_LIBRARY crypto)
find_library(CURL_LIBRARY curl)
find_library(LDAP_LIBRARY ldap)

set(MESON_CROSS_FILE "" CACHE FILEPATH "Meson cross-file for cross-compilation")
set(MESON_SETUP_ARGS --prefix=${PG_INSTALL} --buildtype=release -Dreadline=disabled)
if(MESON_CROSS_FILE)
    list(APPEND MESON_SETUP_ARGS --cross-file ${MESON_CROSS_FILE})
endif()

# What the rest of this file takes from the PostgreSQL build
set(PG_PROGRAMS postgres initdb)
set(PG_OUTPUTS
    ${PG_INSTALL}/bin/postgres
    ${PG_INSTALL}/bin/initdb
    ${PG_BUILD}/src/interfaces/libpq/libpq.a
    ${PG_BUILD}/src/interfaces/libpq-oauth/libpq-oauth.a
    ${PG_BUILD}/src/common/libpgcommon_shlib.a
    ${PG_INSTALL}/lib/libpgport.a
)

# works/core is edited behind CMake's back, so only ninja can tell whether the
# server is out of date: ask it every build, which costs well under a second
# when nothing changed.  --only-changed then leaves untouched files alone, so
# their timestamps say truthfully that nothing downstream has to be redone.
ExternalProject_Add(pg_ext
    SOURCE_DIR ${PG_SOURCE}
    BINARY_DIR ${PG_BUILD}
    INSTALL_DIR ${PG_INSTALL}
    CONFIGURE_COMMAND meson setup ${PG_BUILD} ${PG_SOURCE} ${MESON_SETUP_ARGS}
    BUILD_COMMAND meson compile -C ${PG_BUILD}
    INSTALL_COMMAND meson install -C ${PG_BUILD} --only-changed --quiet
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS ${PG_OUTPUTS}
)

# Each program is compressed on its own, and only when it was rebuilt
set(PG_BLOBS "")
foreach(_prog ${PG_PROGRAMS})
    add_custom_command(
        OUTPUT ${PG_BUILD}/${_prog}.zst
        COMMAND ${ZSTD_PROGRAM} -19 -f -q
            -o ${PG_BUILD}/${_prog}.zst ${PG_INSTALL}/bin/${_prog}
        DEPENDS ${PG_INSTALL}/bin/${_prog}
        COMMENT "Compressing ${_prog} with zstd"
        VERBATIM
    )
    list(APPEND PG_BLOBS ${PG_BUILD}/${_prog}.zst)
endforeach()

# The compressed programs, .incbin'd into the library
if(APPLE)
    set(_sec ".section __DATA,__pg_blob")
    set(_pfx "_")
else()
    set(_sec ".section .rodata,\"a\",@progbits")
    set(_pfx "")
endif()

set(_blob_asm "${_sec}\n")
foreach(_prog ${PG_PROGRAMS})
    string(APPEND _blob_asm
".global ${_pfx}embed_${_prog}_blob_start
.global ${_pfx}embed_${_prog}_blob_end
.balign 16
${_pfx}embed_${_prog}_blob_start:
    .incbin \"${PG_BUILD}/${_prog}.zst\"
${_pfx}embed_${_prog}_blob_end:
")
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/embed_blob.S.in
     CONTENT "${_blob_asm}" @ONLY)

# The assembly is generated from the blobs, not just next to them: no scanner
# sees what an .incbin reads, so a new blob has to arrive as a new source file.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/embed_blob.S
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_BINARY_DIR}/embed_blob.S.in ${CMAKE_BINARY_DIR}/embed_blob.S
    DEPENDS ${CMAKE_BINARY_DIR}/embed_blob.S.in ${PG_BLOBS}
    VERBATIM
)

# The host process links frontend libraries only
add_library(libpq_static STATIC IMPORTED)
set_target_properties(libpq_static PROPERTIES
    IMPORTED_LOCATION ${PG_BUILD}/src/interfaces/libpq/libpq.a)

# Not pgcommon: libpq expects pg_char_to_encoding without the _private suffix
add_library(pgcommon_shlib STATIC IMPORTED)
set_target_properties(pgcommon_shlib PROPERTIES
    IMPORTED_LOCATION ${PG_BUILD}/src/common/libpgcommon_shlib.a)

add_library(pgport STATIC IMPORTED)
set_target_properties(pgport PROPERTIES
    IMPORTED_LOCATION ${PG_INSTALL}/lib/libpgport.a)

add_library(libpq_oauth STATIC IMPORTED)
set_target_properties(libpq_oauth PROPERTIES
    IMPORTED_LOCATION ${PG_BUILD}/src/interfaces/libpq-oauth/libpq-oauth.a)

add_library(embed_pg STATIC
    src/embed_postgres.c
    src/embed_connect.c
    src/embed_initdb.c
    src/embed_loader.c
    src/embed_vfs_host.c
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

target_link_libraries(embed_pg PUBLIC
    libpq_static
    libpq_oauth
    pgcommon_shlib pgport
    pthread dl m z
    ${ZSTD_LIBRARY}
)

# libpq needs SSL and LDAP, libpq-oauth needs curl
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

add_executable(embed_postgres_demo main.cpp)
target_include_directories(embed_postgres_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PG_SOURCE}/src/interfaces/libpq
    ${PG_INSTALL}/include
)
target_link_libraries(embed_postgres_demo embed_pg)

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
        )
        target_link_libraries(${test_name} GTest::gtest_main embed_pg)
        add_dependencies(${test_name} pg_ext)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "PG_SHARE_DIR=${PG_INSTALL}/share"
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
    add_embed_test(test_socket_semantics)
    add_embed_test(test_vfs)
    set_tests_properties(test_stress PROPERTIES TIMEOUT 300)
    set_tests_properties(test_socket_semantics PROPERTIES TIMEOUT 300)
    set_tests_properties(test_edge_cases PROPERTIES TIMEOUT 120)
    set_tests_properties(test_concurrency PROPERTIES TIMEOUT 180)
    set_tests_properties(test_advanced_sql PROPERTIES TIMEOUT 180)
    set_tests_properties(test_vfs PROPERTIES TIMEOUT 180)
endif()