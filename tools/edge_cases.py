#!/usr/bin/env python3
"""Edge cases that the public cases do not cover but a hidden test might."""
import json
import os
import subprocess
import tempfile

BIN = "/mnt/d/mvd-impl/build/vectordb"

REQUESTS = [
    # LIMIT 0 must return no rows (and must not be read as "no limit")
    {"id": 1, "op": "execute", "sql": "CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(8), v VECTOR(2))"},
    {"id": 2, "op": "execute", "sql": "INSERT INTO t VALUES (-5, 'neg', '[1,2]'), (0, 'zero', '[0,0]'), (7, 'seven', '[-1,-1]')"},
    {"id": 3, "op": "execute", "sql": "SELECT id FROM t LIMIT 0"},
    {"id": 4, "op": "execute", "sql": "SELECT id FROM t ORDER BY id DESC"},
    {"id": 5, "op": "execute", "sql": "SELECT * FROM t WHERE id = 0"},
    {"id": 6, "op": "execute", "sql": "SELECT id FROM t ORDER BY name DESC"},
    {"id": 7, "op": "execute", "sql": ""},
    {"id": 8, "op": "execute", "sql": "SELECT FROM t"},
    {"id": 9, "op": "execute", "sql": "DROP TABLE t"},
    {"id": 10, "op": "execute", "sql": "SELECT id FROM t WHERE id = 999"},
    # ann mode without a vector index must still work (exact fallback)
    {"id": 11, "op": "execute", "sql": "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t ORDER BY d LIMIT 2", "search_mode": "ann"},
    # update the primary key
    {"id": 12, "op": "execute", "sql": "UPDATE t SET id = 42 WHERE id = 7"},
    {"id": 13, "op": "execute", "sql": "SELECT id FROM t"},
    # varchar that is too long -> error, not a silent truncation
    {"id": 14, "op": "execute", "sql": "INSERT INTO t VALUES (100, 'abcdefghij', '[0,0]')"},
    # unicode: the length is counted in characters, not bytes
    {"id": 15, "op": "execute", "sql": "INSERT INTO t VALUES (101, '中文', '[0,0]')"},
    {"id": 16, "op": "execute", "sql": "SELECT id, name FROM t WHERE id = 101"},
    # NULL handling
    {"id": 17, "op": "execute", "sql": "INSERT INTO t (id, name) VALUES (200, NULL)"},
    {"id": 18, "op": "execute", "sql": "SELECT id FROM t WHERE name IS NULL"},
    {"id": 19, "op": "execute", "sql": "SELECT id FROM t WHERE name IS NOT NULL"},
    # vector literal with spaces / negative numbers / exponent
    {"id": 20, "op": "execute", "sql": "INSERT INTO t VALUES (300, 'vec', '[1e1, -2.5]')"},
    {"id": 21, "op": "execute", "sql": "SELECT id, v FROM t WHERE id = 300"},
    # invalid vector values
    {"id": 22, "op": "execute", "sql": "INSERT INTO t VALUES (400, 'nan', '[NaN,0]')"},
    {"id": 23, "op": "execute", "sql": "INSERT INTO t VALUES (401, 'inf', '[Infinity,0]')"},
    # wrong dimension
    {"id": 24, "op": "execute", "sql": "INSERT INTO t VALUES (402, 'dim', '[1,2,3]')"},
    # multi statement in one request
    {"id": 25, "op": "execute", "sql": "INSERT INTO t VALUES (500, 'a', '[0,0]'); INSERT INTO t VALUES (501, 'b', '[0,0]')"},
    {"id": 26, "op": "execute", "sql": "SELECT id FROM t WHERE id >= 500"},
    {"id": 27, "op": "shutdown"},
]


def main():
    with tempfile.TemporaryDirectory() as d:
        env = dict(os.environ)
        env.pop("VECTORDB_DATA_DIR", None)
        payload = "\n".join(json.dumps(r) for r in REQUESTS) + "\n"
        proc = subprocess.run([BIN, d], input=payload, capture_output=True, text=True, timeout=60, env=env)
        for line in proc.stdout.splitlines():
            if line.strip():
                print(line)
        print("exit code:", proc.returncode)
        if proc.stderr.strip():
            print("stderr:", proc.stderr.strip()[:400])


main()
