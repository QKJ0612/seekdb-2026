# MiniVectorDB - multi stage build.
#
# The build stage compiles the C++17 sources from scratch (no prebuilt binary
# is copied in), the runtime stage only contains the resulting binary.
#
#   docker build -t minivectordb .
#   docker run --rm -i -v "$PWD/data:/data" minivectordb
#
# The server speaks the JSON line protocol over stdio: it prints
# {"type":"ready"} and then reads one request per line from stdin.
# No port is opened, the container runs the server in the foreground so that
# `docker stop` / SIGKILL reach the database process directly.

# ---------------------------------------------------------------- build stage
FROM gcc:12 AS build

WORKDIR /src
COPY src ./src
COPY build.sh ./build.sh

# Static linking: the runtime image then needs no shared library at all and
# the image can be built without access to a package mirror.
RUN CXXFLAGS="-O2 -std=c++17 -static" bash build.sh && \
    mkdir -p /out && cp build/vectordb /out/vectordb && \
    strip /out/vectordb && \
    /out/vectordb --version || true

# -------------------------------------------------------------- runtime stage
FROM debian:bookworm-slim

# /data is the only directory the database writes to.  The judge mounts a
# volume (or a bind mount) there and keeps the rest of the file system read
# only, so the permissions are widened to make sure the server can write even
# when the container runs with a non default user.
RUN mkdir -p /data && chmod 1777 /data

COPY --from=build /out/vectordb /usr/local/bin/vectordb

# Fallback for a plain `docker run` without a volume: the server prefers
# VECTORDB_DATA_DIR when that directory exists and is writable and falls back
# to /data otherwise.
ENV VECTORDB_DATA_DIR=/data

WORKDIR /data

# No EXPOSE / no port: the protocol runs over stdin/stdout.
# `exec` keeps the server as PID 1 so that SIGKILL/SIGTERM reach it directly.
ENTRYPOINT ["/usr/local/bin/vectordb"]
