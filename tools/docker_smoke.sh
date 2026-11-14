#!/usr/bin/env bash
# Smoke test of the built image with the exact flags the judge uses.
set -uo pipefail

IMAGE="${1:-minivectordb:latest}"
DIR=/tmp/dockertest
rm -rf "$DIR"
mkdir -p "$DIR"

cat > /tmp/req.jsonl <<'JSON'
{"id":1,"op":"execute","sql":"CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2))"}
{"id":2,"op":"execute","sql":"INSERT INTO t VALUES (1, '[1,2]')"}
{"id":3,"op":"execute","sql":"SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t"}
{"id":4,"op":"shutdown"}
JSON

echo "--- run with the judge flags ---"
timeout 60 docker run --name mvdb-smoke --interactive --network=none --cpus=2 --memory=2g \
  --memory-swap=2g --pids-limit=128 --cap-drop=ALL --security-opt=no-new-privileges --read-only \
  --mount "type=bind,src=$DIR,dst=/data" "$IMAGE" < /tmp/req.jsonl
echo "exit code: $?"

echo "--- files written into the mounted /data ---"
ls -l "$DIR"

docker rm -f mvdb-smoke >/dev/null 2>&1
exit 0
