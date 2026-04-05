# syntax=docker/dockerfile:1
#
# Multi-platform build for embed-postgres.
# Produces: libembed_pg.a + PG shared library blob + PG share/bin files.
#
# Linux:  docker buildx build --platform linux/amd64 --target output -o out/linux-amd64 .
# macOS:  docker buildx build --build-arg TARGET_OS=darwin --build-arg DARWIN_ARCH=arm64 \
#           --target output -o out/darwin-arm64 .

# Global ARG — must be before all FROM to be usable in FROM
ARG TARGET_OS=linux

# ============================================================
# Common base: build tools
# ============================================================
FROM debian:bookworm-slim AS base

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config python3 \
    meson flex bison perl \
    git ca-certificates curl wget xz-utils \
    && rm -rf /var/lib/apt/lists/*

# ============================================================
# Linux dependencies (native, multi-arch via buildx --platform)
# ============================================================
FROM base AS deps-linux

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl-dev libicu-dev libzstd-dev liblz4-dev \
    libcurl4-openssl-dev libxml2-dev libxslt1-dev \
    libldap2-dev libpam0g-dev zlib1g-dev \
    zstd \
    && rm -rf /var/lib/apt/lists/*

# ============================================================
# osxcross toolchain builder
# ============================================================
FROM base AS osxcross-builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang lld libbz2-dev libxml2-dev libssl-dev \
    zlib1g-dev patch cpio \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/tpoechtrager/osxcross /opt/osxcross-src

ARG MACOS_SDK_TARBALL=MacOSX14.0.sdk.tar.xz
COPY sdk/${MACOS_SDK_TARBALL} /opt/osxcross-src/tarballs/

RUN cd /opt/osxcross-src && UNATTENDED=1 TARGET_DIR=/opt/osxcross ./build.sh

# ============================================================
# Darwin dependencies: cross-compile from Linux
# ============================================================
FROM base AS deps-darwin

ARG DARWIN_ARCH=arm64
ARG OSX_VERSION_MIN=13.0

COPY --from=osxcross-builder /opt/osxcross /opt/osxcross
ENV PATH="/opt/osxcross/bin:${PATH}"

RUN apt-get update && apt-get install -y --no-install-recommends \
    zstd autoconf automake libtool \
    && rm -rf /var/lib/apt/lists/*

# Set cross-compiler environment
ENV CROSS_PREFIX=/opt/darwin-deps

# Map arch names
RUN if [ "$DARWIN_ARCH" = "arm64" ]; then \
      echo "aarch64-apple-darwin23" > /tmp/osxcross_host; \
    else \
      echo "x86_64-apple-darwin23" > /tmp/osxcross_host; \
    fi
# Read it back for later stages
ENV OSXCROSS_HOST_FILE=/tmp/osxcross_host

# Find SDK path
RUN ls /opt/osxcross/SDK/ | head -1 > /tmp/sdk_name
ENV SDK_NAME_FILE=/tmp/sdk_name

# Cross-compile zlib
RUN OSXCROSS_HOST=$(cat /tmp/osxcross_host) && \
    SDK_NAME=$(cat /tmp/sdk_name) && \
    SDK_PATH=/opt/osxcross/SDK/$SDK_NAME && \
    cd /tmp && wget -q https://zlib.net/fossils/zlib-1.3.1.tar.gz && \
    tar xf zlib-1.3.1.tar.gz && cd zlib-1.3.1 && \
    CC=${OSXCROSS_HOST}-clang \
    CFLAGS="-isysroot $SDK_PATH" \
    LDFLAGS="-isysroot $SDK_PATH" \
    ./configure --prefix=$CROSS_PREFIX --static && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/zlib*

# Cross-compile zstd
RUN OSXCROSS_HOST=$(cat /tmp/osxcross_host) && \
    SDK_NAME=$(cat /tmp/sdk_name) && \
    SDK_PATH=/opt/osxcross/SDK/$SDK_NAME && \
    cd /tmp && wget -q https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz && \
    tar xf zstd-1.5.6.tar.gz && cd zstd-1.5.6 && \
    cmake -S build/cmake -B build -G Ninja \
      -DCMAKE_SYSTEM_NAME=Darwin \
      -DCMAKE_C_COMPILER=${OSXCROSS_HOST}-clang \
      -DCMAKE_CXX_COMPILER=${OSXCROSS_HOST}-clang++ \
      -DCMAKE_AR=$(which ${OSXCROSS_HOST}-ar) \
      -DCMAKE_RANLIB=$(which ${OSXCROSS_HOST}-ranlib) \
      -DCMAKE_OSX_SYSROOT=$SDK_PATH \
      -DCMAKE_INSTALL_PREFIX=$CROSS_PREFIX \
      -DZSTD_BUILD_PROGRAMS=OFF \
      -DZSTD_BUILD_SHARED=OFF && \
    cmake --build build -j$(nproc) && cmake --install build && \
    rm -rf /tmp/zstd*

# Cross-compile OpenSSL
RUN OSXCROSS_HOST=$(cat /tmp/osxcross_host) && \
    DARWIN_ARCH_LOCAL=${DARWIN_ARCH} && \
    cd /tmp && wget -q https://www.openssl.org/source/openssl-3.3.1.tar.gz && \
    tar xf openssl-3.3.1.tar.gz && cd openssl-3.3.1 && \
    if [ "$DARWIN_ARCH_LOCAL" = "arm64" ]; then \
      OPENSSL_TARGET=darwin64-arm64-cc; \
    else \
      OPENSSL_TARGET=darwin64-x86_64-cc; \
    fi && \
    CC=${OSXCROSS_HOST}-clang \
    ./Configure $OPENSSL_TARGET no-shared \
      --prefix=$CROSS_PREFIX \
      --openssldir=$CROSS_PREFIX/ssl && \
    make -j$(nproc) && make install_sw && \
    rm -rf /tmp/openssl*

# Cross-compile ICU (two-step: native build for tools, then cross build)
RUN cd /tmp && wget -q https://github.com/unicode-org/icu/releases/download/release-74-2/icu4c-74_2-src.tgz && \
    tar xf icu4c-74_2-src.tgz && \
    mkdir icu-native && cd icu-native && \
    CC=gcc CXX=g++ ../icu/source/configure --disable-shared --enable-static && \
    make -j$(nproc)

RUN OSXCROSS_HOST=$(cat /tmp/osxcross_host) && \
    SDK_NAME=$(cat /tmp/sdk_name) && \
    SDK_PATH=/opt/osxcross/SDK/$SDK_NAME && \
    cd /tmp && mkdir icu-cross && cd icu-cross && \
    CC=${OSXCROSS_HOST}-clang \
    CXX=${OSXCROSS_HOST}-clang++ \
    CFLAGS="-isysroot $SDK_PATH" \
    CXXFLAGS="-isysroot $SDK_PATH" \
    LDFLAGS="-isysroot $SDK_PATH" \
    ../icu/source/configure \
      --host=$(cat /tmp/osxcross_host) \
      --prefix=$CROSS_PREFIX \
      --enable-static --disable-shared \
      --with-cross-build=/tmp/icu-native && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/icu*

# Cross-compile lz4
RUN OSXCROSS_HOST=$(cat /tmp/osxcross_host) && \
    SDK_NAME=$(cat /tmp/sdk_name) && \
    SDK_PATH=/opt/osxcross/SDK/$SDK_NAME && \
    cd /tmp && wget -q https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz && \
    tar xf lz4-1.10.0.tar.gz && cd lz4-1.10.0 && \
    make CC="${OSXCROSS_HOST}-clang" AR="${OSXCROSS_HOST}-ar" \
      CFLAGS="-isysroot $SDK_PATH" \
      PREFIX=$CROSS_PREFIX lib-release && \
    make PREFIX=$CROSS_PREFIX install && \
    rm -rf /tmp/lz4*

# Cross-compile libxml2
RUN OSXCROSS_HOST=$(cat /tmp/osxcross_host) && \
    SDK_NAME=$(cat /tmp/sdk_name) && \
    SDK_PATH=/opt/osxcross/SDK/$SDK_NAME && \
    cd /tmp && wget -q https://download.gnome.org/sources/libxml2/2.12/libxml2-2.12.9.tar.xz && \
    tar xf libxml2-2.12.9.tar.xz && cd libxml2-2.12.9 && \
    CC=${OSXCROSS_HOST}-clang \
    CFLAGS="-isysroot $SDK_PATH" \
    LDFLAGS="-isysroot $SDK_PATH" \
    ./configure --host=$(cat /tmp/osxcross_host) \
      --prefix=$CROSS_PREFIX \
      --enable-static --disable-shared \
      --without-python --without-lzma && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/libxml2*

# Cross-compile curl (minimal, for libpq-oauth)
RUN OSXCROSS_HOST=$(cat /tmp/osxcross_host) && \
    SDK_NAME=$(cat /tmp/sdk_name) && \
    SDK_PATH=/opt/osxcross/SDK/$SDK_NAME && \
    cd /tmp && wget -q https://curl.se/download/curl-8.9.1.tar.xz && \
    tar xf curl-8.9.1.tar.xz && cd curl-8.9.1 && \
    CC=${OSXCROSS_HOST}-clang \
    CFLAGS="-isysroot $SDK_PATH" \
    LDFLAGS="-isysroot $SDK_PATH -L$CROSS_PREFIX/lib" \
    CPPFLAGS="-I$CROSS_PREFIX/include" \
    PKG_CONFIG_PATH=$CROSS_PREFIX/lib/pkgconfig \
    ./configure --host=$(cat /tmp/osxcross_host) \
      --prefix=$CROSS_PREFIX \
      --enable-static --disable-shared \
      --with-ssl=$CROSS_PREFIX \
      --without-libpsl --without-brotli --without-nghttp2 && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/curl*

# Install zstd CLI tool (native, for compression at build time)
RUN apt-get update && apt-get install -y --no-install-recommends zstd && \
    rm -rf /var/lib/apt/lists/*

# ============================================================
# Build stage: select deps based on TARGET_OS
# ============================================================
ARG TARGET_OS=linux
FROM deps-${TARGET_OS} AS build

ARG TARGET_OS=linux
ARG DARWIN_ARCH=arm64

WORKDIR /src
COPY . .

# Generate meson cross-file and cmake toolchain for darwin
RUN if [ "$TARGET_OS" = "darwin" ]; then \
      OSXCROSS_HOST=$(cat /tmp/osxcross_host) && \
      SDK_NAME=$(cat /tmp/sdk_name) && \
      SDK_PATH=/opt/osxcross/SDK/$SDK_NAME && \
      if [ "$DARWIN_ARCH" = "arm64" ]; then MESON_CPU=aarch64; else MESON_CPU=x86_64; fi && \
      printf "[binaries]\nc = '%s-clang'\ncpp = '%s-clang++'\nar = '%s-ar'\nstrip = '%s-strip'\npkg-config = '%s-pkg-config'\n\n[host_machine]\nsystem = 'darwin'\ncpu_family = '%s'\ncpu = '%s'\nendian = 'little'\n\n[built-in options]\nc_args = ['-isysroot', '%s', '-I/opt/darwin-deps/include']\nc_link_args = ['-isysroot', '%s', '-L/opt/darwin-deps/lib']\ncpp_args = ['-isysroot', '%s', '-I/opt/darwin-deps/include']\ncpp_link_args = ['-isysroot', '%s', '-L/opt/darwin-deps/lib']\npkg_config_path = '/opt/darwin-deps/lib/pkgconfig'\n\n[properties]\ndarwin_sysroot = '%s'\n" \
        "$OSXCROSS_HOST" "$OSXCROSS_HOST" "$OSXCROSS_HOST" "$OSXCROSS_HOST" "$OSXCROSS_HOST" \
        "$MESON_CPU" "$MESON_CPU" \
        "$SDK_PATH" "$SDK_PATH" "$SDK_PATH" "$SDK_PATH" "$SDK_PATH" \
        > /tmp/meson-cross.ini && \
      printf "set(CMAKE_SYSTEM_NAME Darwin)\nset(CMAKE_SYSTEM_PROCESSOR %s)\nset(CMAKE_C_COMPILER %s-clang)\nset(CMAKE_CXX_COMPILER %s-clang++)\nset(CMAKE_ASM_COMPILER %s-clang)\nset(CMAKE_AR %s CACHE FILEPATH \"\" FORCE)\nset(CMAKE_RANLIB %s CACHE FILEPATH \"\" FORCE)\nset(CMAKE_OSX_SYSROOT %s)\nset(CMAKE_FIND_ROOT_PATH /opt/darwin-deps %s)\nset(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\nset(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\nset(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n" \
        "$DARWIN_ARCH" "$OSXCROSS_HOST" "$OSXCROSS_HOST" "$OSXCROSS_HOST" \
        "$(which ${OSXCROSS_HOST}-ar)" "$(which ${OSXCROSS_HOST}-ranlib)" \
        "$SDK_PATH" "$SDK_PATH" \
        > /tmp/darwin-toolchain.cmake; \
    fi

# Build
RUN mkdir build && cd build && \
    if [ "$TARGET_OS" = "darwin" ]; then \
      cmake .. \
        -DCMAKE_TOOLCHAIN_FILE=/tmp/darwin-toolchain.cmake \
        -DMESON_CROSS_FILE=/tmp/meson-cross.ini \
        -DCMAKE_BUILD_TYPE=Release && \
      cmake --build . -j$(nproc); \
    else \
      cmake .. -DCMAKE_BUILD_TYPE=Release && \
      cmake --build . -j$(nproc); \
    fi

# ============================================================
# Output: artifacts only
# ============================================================
FROM scratch AS output

COPY --from=build /src/build/libembed_pg.a                                   /lib/
COPY --from=build /src/build/pg-build/src/interfaces/libpq/libpq.a           /lib/
COPY --from=build /src/build/pg-build/src/interfaces/libpq-oauth/libpq-oauth.a /lib/
COPY --from=build /src/build/pg-install/lib/libpgcommon.a                    /lib/
COPY --from=build /src/build/pg-install/lib/libpgport.a                      /lib/
COPY --from=build /src/build/pg-build/libpg_server.zst                       /blob/
COPY --from=build /src/build/pg-install/share/                               /share/
COPY --from=build /src/build/pg-install/bin/initdb                           /bin/
COPY --from=build /src/include/embed_postgres.h                              /include/
COPY --from=build /src/build/pg-install/include/                             /include/pg/