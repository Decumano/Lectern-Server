# Two stages: vcpkg + compiler in the builder, only the runtime pieces and a
# Chromium for PDF export in the final image.
FROM debian:bookworm-slim AS builder

# build-essential/cmake/ninja compile our code; the rest are what vcpkg's own
# ports need to build from source. perl is not optional — OpenSSL's build is
# written in it, and without it the dependency stage fails partway through
# with a message that never mentions perl.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        curl \
        zip \
        unzip \
        tar \
        pkg-config \
        ca-certificates \
        perl \
        python3 \
        autoconf \
        automake \
        libtool \
        bison \
        flex \
    && rm -rf /var/lib/apt/lists/*

# Pinned, not `--depth 1` of whatever the default branch is today: the whole
# point of building in a container is that the same source produces the same
# image, and an unpinned registry quietly changes every dependency version
# between builds. This is the same commit the release workflow pins.
ARG VCPKG_COMMIT=4e39a8622e56a113c89228e4e13944dce8654da7
ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" checkout --quiet "$VCPKG_COMMIT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /src

# The manifest alone first, so the (long) dependency build is cached and only
# redone when vcpkg.json actually changes — not on every source edit.
COPY vcpkg.json ./
# --clean-after-build discards each port's buildtree once it is installed,
# so the builder stage never holds the debug and release trees for all 19
# dependencies at once. Keeps peak build-time disk use modest.
# vcpkg otherwise parallelises across every core, and Drogon/OpenSSL compiles
# are memory-hungry enough to matter on a small CI runner. Capping it keeps
# peak memory predictable across very different builders; raise VCPKG_JOBS
# where there is headroom (the GitHub Actions runners are the tight case).
ARG VCPKG_JOBS=8
ENV VCPKG_MAX_CONCURRENCY=${VCPKG_JOBS}
RUN "$VCPKG_ROOT/vcpkg" install --triplet x64-linux --clean-after-build

COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
COPY migrations ./migrations

# Tests run here so a broken build never reaches the runtime stage.
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=ON \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    && cmake --build build -j "${VCPKG_MAX_CONCURRENCY}" \
    && ./build/lectern-tests

FROM debian:bookworm-slim

# chromium is what /api/export/pdf drives over the DevTools protocol. The
# libraries below cover the parts of the build that link dynamically; vcpkg's
# x64-linux triplet is static, so most dependencies are already inside the
# binary.
RUN apt-get update && apt-get install -y --no-install-recommends \
        chromium \
        ca-certificates \
        curl \
        libstdc++6 \
        libssl3 \
        zlib1g \
        libbrotli1 \
        libcurl4 \
        libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/lectern-server /app/lectern-server
COPY --from=builder /src/migrations /app/migrations
# The frontend. It is the officesuite-frontend submodule; run
# `git submodule update --init` before building, or the image ships with no UI.
COPY web /app/web

# Runs as a normal user: nothing here needs root, and the PDF renderer is
# handed visitor-authored HTML.
RUN useradd --system --create-home --uid 10001 lectern \
    && mkdir -p /data \
    && chown -R lectern:lectern /data /app
USER lectern

ENV DATABASE_URL=sqlite:///data/officesuite.db \
    WORKSPACES_DIR=/data/workspaces \
    FONTS_DIR=/data/fonts \
    MIGRATIONS_DIR=/app/migrations \
    WEB_DIR=/app/web \
    CHROME_PATH=/usr/bin/chromium \
    PORT=8080

VOLUME ["/data"]
EXPOSE 8080

# The frontend is served from the same process, so a 200 on / means both the
# HTTP layer and the static root are healthy.
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD ["/bin/sh", "-c", "curl -fsS http://127.0.0.1:${PORT}/ > /dev/null || exit 1"]

CMD ["/app/lectern-server"]
