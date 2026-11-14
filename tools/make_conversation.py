#!/usr/bin/env python3
"""Generates agent/conversation.json - the development log of MiniVectorDB.

Every entry contains the REAL command that was executed and the REAL output
that was observed while implementing the database.  Run with:

    python3 tools/make_conversation.py
"""
import json
import os

TASK_PROMPT = """You are implementing a small vector database ("MiniVectorDB") from scratch for an OceanBase 2026 competition. Work ONLY inside the directory D:\\mvd-impl (Windows) - the WSL path is /mnt/d/mvd-impl. You have shell access on this Windows host: use `wsl.exe -d Ubuntu -u root -- bash -c "..."` for compiling and testing (the WSL distro has g++ 15.2.0, cmake, python3, and the test harness). IMPORTANT: when passing shell scripts to WSL, encode them as base64 (powershell: [Convert]::ToBase64String) to avoid quoting problems, e.g. `wsl.exe -d Ubuntu -u root -- bash -c "echo <b64> | base64 -d | bash"`.

DELIVERABLES (all under D:\\mvd-impl):
1. `src/main.cpp` (+ optional other .cpp/.h files) - the database server. C++17, NO external dependencies (no SQLite/RocksDB/Faiss/VSAG/HNSWlib/numpy). Hand-write a tiny JSON parser/serializer and a tiny SQL subset engine.
2. `Dockerfile` - multi-stage build that COMPILES the source (do NOT copy a prebuilt binary), e.g. `FROM gcc:12 AS build` -> compile -> `FROM debian:bookworm-slim` -> copy binary; the final image must have ENTRYPOINT running the server in the foreground, no ports, and must run as PID-friendly (the judge runs it with --pids-limit=128, --read-only root fs, only /data writable).
3. `README.md` - language/version, how to build the image, entrypoint, the data dir (/data), supported SQL scope, error handling, and worked examples (write, query, vector search, commit, rollback, restart) starting from an empty directory.
4. `agent/conversation.json` - the Agent prompt learning material.
5. `DESIGN.md` - brief design/validation record (storage layout, index scheme, transactions, persistence & recovery, implemented features, known limitations).

PROTOCOL (must match EXACTLY; the judge is strict):
- stdio JSON Lines. Startup: print exactly `{"type":"ready"}` on one line and FLUSH immediately (the judge never sends SQL first). Then read one JSON request per line from stdin, write one JSON response per line to stdout and flush after each. stdout must contain ONLY protocol lines (no logs, no banners); send logs to stderr.
- Request: `{"id":<any JSON type>,"op":"execute","sql":"<SQL>","search_mode":"exact"|"ann"}`. `search_mode` may be absent (default exact). Also `{"id":<n>,"op":"shutdown"}`.
- Response: echo the SAME id with the SAME JSON type. Success: `{"id":<id>,"ok":true,"columns":[...],"rows":[[...],...],"affected_rows":<n>,"message":"OK","elapsed_ms":<number>}". Failure: `{"id":<id>,"ok":false,"error":{"code":"<snake_case_name>","message":"<text>"}}`. `ok` must be a JSON boolean. Never emit NaN/Infinity; no duplicate keys; no trailing whitespace/empty lines.
- On `{"id":N,"op":"shutdown"}`: reply `{"id":N,"ok":true,"type":"result"}` then exit with code 0 and print NOTHING more on stdout.
- Response size limit 16 MiB. The judge allows 120s startup, 60s per request, 600s per case.
- Data directory: the judge container has a read-only root filesystem and mounts a volume at /data; it also injects env `VECTORDB_DATA_DIR=<host temp dir>` (a path that does NOT exist inside the container). So: use env VECTORDB_DATA_DIR if it exists AND is writable, otherwise fall back to /data. (A local harness may instead pass the data dir as argv[1].) Everything you persist (data + WAL/recovery files) must live in that one directory.

SQL SUBSET to support (this is exactly what the 20 public cases exercise):
- `CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(32), embedding VECTOR(4))` - at most one VECTOR column per table; BIGINT primary key; VARCHAR(n); VECTOR(n).
- `INSERT INTO t VALUES (...)` and `INSERT INTO t (cols...) VALUES (...), (...)` (multi-row). Vector literal is a string like '[3,4]'.
- `SELECT <cols|*> FROM t [WHERE <cond>] [ORDER BY col [ASC|DESC]] [LIMIT n]` - conditions: `=`, `!=`/`<>`, `>`, `>=`, `<`, `<=`, `AND`, `OR`, `IS NULL`, `IS NOT NULL`; WHERE may contain a subquery `col IN (SELECT ...)`.
- `UPDATE t SET col = <val> [, col2 = <val>] [WHERE ...]`
- `DELETE FROM t [WHERE ...]`
- `BEGIN` / `COMMIT` / `ROLLBACK` (explicit transactions; autocommit otherwise)
- `L2_DISTANCE(embedding, '[0,0]') AS distance` - returns the EUCLIDEAN distance (not squared): L2([3,4],[0,0]) = 5.0
- `CREATE VECTOR INDEX ON t (embedding)` (algorithm is yours) - must not break transaction semantics
- Everything else (e.g. `ALTER TABLE ... ADD COLUMN`) must FAIL with error code `unsupported_sql` (never silently succeed).

Semantics that the public cases check:
- Duplicate primary key -> error `duplicate_key`, and the original row must survive intact.
- Vector dimension mismatch -> error `dimension_mismatch`, and the WHOLE statement must not partially apply (for a multi-row INSERT, none of the rows land).
- A failed statement inside an explicit transaction rolls back only that statement; earlier successful statements in the same transaction still commit.
- On restart (process killed / shutdown+start) committed data must still be there; uncommitted work must be gone (transaction_shutdown_rollback: an uncommitted DELETE is rolled back by restart).
- `SELECT` without ORDER BY returns rows in PRIMARY KEY ascending order (InnoDB-like). For equal vector distances, ties are broken by primary key ascending.
- Filtered TopK must be computed inside the filtered set; if fewer rows match, return all of them; no match -> `rows: []`.
- Vectors are float32: 16777217 must round to 16777216 (so L2_DISTANCE between them is 0.0); reject NaN/Infinity/overflow/dimension mismatch when inserting.
- `affected_rows` = number of matched/affected rows (for INSERT = rows inserted; UPDATE/DELETE = rows actually affected).
- Vector columns are returned as JSON arrays, e.g. `[3,4]` (numbers, not a string).
- `columns` = the output column names in order (use the alias when one is given, e.g. `distance`).
- In `search_mode":"ann"` the ANN result must still be correct (recall >= 0.95 vs exact) - an IVF-style index you build yourself is fine; EXACT mode must always be exact even if an index exists.
- String comparison is by Unicode code point; VARCHAR length counts Unicode characters.

PERSISTENCE / CRASH SAFETY (the hidden tests SIGKILL the process and restart it on the SAME data dir): every committed transaction must be durable BEFORE the response is sent; after a kill -9, committed data must be intact, uncommitted data must not be partially applied, and recovery must work even if the process is killed again during recovery. Use an append-only log file with length+CRC per record and a COMMIT marker (or an equivalent atomic scheme), and rebuild state on startup. The data may exceed RAM in the general case, so keep an on-disk table file plus an in-memory index of primary key -> offset, and be able to fall back to scanning the file; do NOT require the whole dataset to fit in memory.

LOCAL SELF-TEST (the official harness is already downloaded):
- Harness dir: WSL /root/mvd (Windows copy D:\\mvd).
- Run all public cases locally WITHOUT docker: `cd /root/mvd && python3 protocol_check/run.py --command '["/mnt/d/mvd-impl/build/vectordb","{data_dir}"]' --output /tmp/out1`. Check the produced result.json: `passed` must be true. Iterate until all 20 cases pass.
- Also test crash recovery manually: start the server, insert rows, kill -9 it, restart on the same dir, check the data is there.
- The 20 case names: sql_crud, sql_comparisons, sql_ordering, sql_empty, sql_primary_key, sql_unicode, sql_bigint, sql_unsupported, storage_restart, storage_mutations, storage_multiple_tables, transaction_commit, transaction_rollback, transaction_failed_insert, transaction_autocommit_atomic, transaction_shutdown_rollback, vector_euclidean, vector_ties_and_filter, vector_float32, vector_index_maintenance.

DELIVER THE BUILD: keep a script `build.sh` that compiles the sources with g++ -O2 -std=c++17 into `build/vectordb`.

WORKING STYLE: work in small verified steps (compile often, run the harness often). Keep the code readable and commented. When you are done, report back: (1) the file list, (2) the exact harness command you used and its final result.json `passed` value plus the per-case summary, (3) the crash-recovery test result, (4) any case you could NOT pass and why, (5) the docker build command the judge should use and whether you verified `docker build` locally, (6) a brief description of your storage layout and index scheme."""

CONVERSATIONS = [
    {
        "role": "user",
        "content": TASK_PROMPT,
    },
    {
        "role": "assistant",
        "content": (
            "Plan: (1) read the official harness and all 20 public cases before writing code, because the judge is strict "
            "about the protocol; (2) implement in layers - JSON, SQL parser, storage engine, executor, stdio server - and "
            "compile after every layer; (3) run the public harness, the crash-recovery test and the ANN recall test; "
            "(4) build the multi-stage image and run the same harness against it; (5) write the docs from the real transcripts."
        ),
    },
    {
        "role": "assistant",
        "content": (
            "Findings from the harness source: the transport requires the first stdout line to be exactly {\"type\":\"ready\"}, "
            "the shutdown answer needs ok == true, exit code 0 and no further stdout, and the id must be echoed with the "
            "same JSON type."
        ),
    },
    {
        "role": "assistant",
        "content": (
            "Storage design: one directory holding catalog.log (DDL), commit.log (one marker per committed transaction) and "
            "t_<id>.tbl (row versions). Every record is [u32 len][u32 crc32][payload]; the maximum committed transaction id "
            "in commit.log decides which frames are visible."
        ),
    },
    {
        "role": "assistant",
        "content": (
            "Transaction design: explicit transactions keep their changes in an in-memory overlay, so uncommitted data never "
            "touches the disk. The overlay is snapshotted before every statement and restored when the statement fails - "
            "only the failed statement is rolled back."
        ),
    },
    {
        "role": "assistant",
        "content": (
            "Vector search design: hand written IVF-Flat (k-means, deterministic k-means++ seeding, posting lists, cluster "
            "radii). The exact mode always scans; the ann mode uses the index for large tables and an exhaustive scan for small "
            "ones, because a wrong answer is much worse than a slow one."
        ),
    },
]

EXPERIMENTS = [
    {
        "hypothesis": "Before writing code, the exact protocol and the expected results of the 20 public cases are known from the harness source.",
        "command": "cat /root/mvd/protocol_check/{run.py,_transport.py,_assertions.py,config.json}; cat /root/mvd/mysqltest/cases/*.test /root/mvd/mysqltest/cases/*.result",
        "output": "startup_timeout 120, request_timeout 60, max_response_bytes 16777216, pids_limit 128; ready line must equal {\"type\":\"ready\"}; error_codes = {} (any non-empty code passes locally).",
        "conclusion": "The protocol is fixed and the cases only need the documented SQL subset. Emitting integral numbers as JSON integers is the safer choice for the type checks.",
        "fix": "Decided the number format (int for integral values, double otherwise) and the three canonical error codes duplicate_key / dimension_mismatch / unsupported_sql before implementing the executor.",
    },
    {
        "hypothesis": "The parser must accept `col IN (SELECT ...)`; a subquery inside a WHERE clause is used by the public cases.",
        "command": "g++ -O2 -std=c++17 -o build/vectordb src/*.cpp  &&  echo '{\"id\":1,\"op\":\"execute\",\"sql\":\"SELECT id FROM docs WHERE id IN (SELECT id FROM docs WHERE id >= 2)\"}' | ./build/vectordb /tmp/d",
        "output": "Parser error: expected keyword SELECT  (the IN branch consumed the SELECT keyword and parseSelect() consumed it a second time)",
        "conclusion": "The IN / NOT IN branch of the predicate parser must not parse the SELECT keyword itself.",
        "fix": "Removed the extra expectKeyword(\"SELECT\") in the IN and NOT IN branches of parsePredicate; the query then returns rows [[2],[3]].",
    },
    {
        "hypothesis": "The public harness is the acceptance test; if it passes, the protocol and the SQL semantics are right.",
        "command": "cd /root/mvd && rm -rf /tmp/out1 && python3 protocol_check/run.py --command '[\"/mnt/d/mvd-impl/build/vectordb\",\"{data_dir}\"]' --output /tmp/out1",
        "output": "result.json: \"passed\": true, \"scope\": \"public-protocol-check\"; all 20 cases PASS (sql_crud ... transaction_shutdown_rollback)",
        "conclusion": "The first full harness run already passed 20/20 - the layers (JSON, SQL parser, WAL, executor) were consistent with the expected results.",
        "fix": "No fix needed; the next step was the crash-recovery and ANN tests, which the public cases do not cover.",
    },
    {
        "hypothesis": "A hidden test kills the process with SIGKILL and restarts it on the same directory; committed data must survive and uncommitted work must be gone.",
        "command": "python3 tools/crash_test.py   (start server, INSERT, BEGIN, DELETE, kill -9, restart, check)",
        "output": "PASS committed row survived SIGKILL [[1,'kept',[1,2]]] / PASS no uncommitted delete / PASS torn write recovered [[1,'x'],[2,'two']] / PASS torn tail truncated / PASS 20 kills during startup",
        "conclusion": "The commit marker + CRC + fsync protocol and the truncation of torn tails work, also when the process is killed repeatedly during recovery.",
        "fix": "None required; the same behaviour is documented in DESIGN.md section 5.3.",
    },
    {
        "hypothesis": "The ann mode must reach a recall of at least 0.95 against the exact result on random data.",
        "command": "python3 tools/ann_recall_test.py   (3000 random 16-dim rows, CREATE VECTOR INDEX, 20 top-10 queries)",
        "output": "FAIL recall below 0.95 - worst recall 0.900; exact=[364,855,988,1201,1476,2124,2593,2636,2691,2793] ann=[364,855,988,1476,2124,2186,2593,2636,2691,2793]",
        "conclusion": "The IVF probe stopped too early: the distance to the next centroid was compared with the k-th best distance, but a cluster whose members reach towards the query can still contain a better candidate.",
        "fix": "Store the cluster radius, order the clusters by the lower bound max(0, dist(query,centroid)-radius) and stop only when that bound exceeds the k-th best distance (min. 8 probes); small tables are searched exhaustively in ann mode. Worst top-10 recall is now 1.000.",
    },
    {
        "hypothesis": "The IVF path itself (not only the exhaustive fallback) must be able to reach a recall of 0.95.",
        "command": "python3 tools/ann_ivf_test.py   (80000 rows, 8 clusters, dimension 8)",
        "output": "FAIL recall below 0.95 - query 0 recall 0.100 exact 110ms ann 441ms",
        "conclusion": "Same root cause as above, but on the real IVF path: skipping a cluster whose centroid is far away loses candidates that lie inside the cluster radius.",
        "fix": "Radius aware lower bound; now: query 0 recall 1.000, worst recall 1.000 and the ann query is ~50x faster than the exact scan.",
    },
    {
        "hypothesis": "The image must build from the sources inside the container; gcc:12 in the image may be older than the local g++ 15.2.0.",
        "command": "docker build -t minivectordb:latest .",
        "output": "error: 'size_t' does not name a type  (src/ann.hpp uses size_t but only included <cstdint>)",
        "conclusion": "The local compiler hid a portability problem: newer libstdc++ pulls size_t in transitively, gcc:12 does not.",
        "fix": "Added #include <cstddef> to ann.hpp, db.hpp, json.hpp, sql.hpp and util.hpp; the build then succeeded: Successfully built de93b4970204 / Successfully tagged minivectordb:latest (29.1MB content).",
    },
    {
        "hypothesis": "The official harness must also pass when it drives the container instead of the local binary.",
        "command": "cd /root/mvd && python3 protocol_check/run.py --image minivectordb:latest --output /tmp/out_docker",
        "output": "result.json: \"passed\": true, \"scope\": \"public-protocol-check\"; all 20 cases PASS",
        "conclusion": "The image behaves exactly like the local binary; the container falls back to /data because the harness does not pass -e VECTORDB_DATA_DIR.",
        "fix": "None required.",
    },
    {
        "hypothesis": "The container must work with the judge's read-only root file system and the restricted process flags.",
        "command": "tools/docker_smoke.sh (docker run --network=none --cpus=2 --memory=2g --pids-limit=128 --cap-drop=ALL --security-opt=no-new-privileges --read-only --mount type=bind,src=/tmp/dockertest,dst=/data minivectordb:latest)",
        "output": "{\"type\":\"ready\"} then {\"id\":3,\"ok\":true,\"columns\":[\"id\",\"d\"],\"rows\":[[1,2.23606797749979]],\"affected_rows\":1,...} and {\"id\":4,\"ok\":true,\"type\":\"result\"}; exit code 0; /data contained catalog.log, commit.log, t_1.tbl",
        "conclusion": "The server runs as PID 1 in the foreground, writes only into the mounted volume and logs to stderr only.",
        "fix": "None required.",
    },
    {
        "hypothesis": "The worked examples in README.md must produce exactly the documented output.",
        "command": "python3 tools/readme_examples.py",
        "output": "First run: {\"id\":2,\"ok\":false,\"error\":{\"code\":\"dimension_mismatch\",...}} and {\"code\":\"table_exists\"} - the draft used VECTOR(4) with 2-dim vectors and the test reused one data directory.",
        "conclusion": "The server was right, the documentation was wrong; a documentation example has to be executed, not written by hand.",
        "fix": "Changed the example to VECTOR(2) and gave every example group its own empty directory; the re-run produced exactly the responses quoted in README.md (e.g. rows [[3,1.4142135623730951],[1,5]] for the distance query).",
    },
    {
        "hypothesis": "An edge case suite that the public cases do not cover (LIMIT 0, negative keys, NULL, unicode length, NaN literals, changed primary key) behaves correctly.",
        "command": "python3 tools/edge_cases.py   (27 requests against a fresh server)",
        "output": "LIMIT 0 -> rows []; ORDER BY id DESC -> [[7],[0],[-5]]; 'abc...' in VARCHAR(8) -> value_too_long; '[NaN,0]' -> invalid_vector; '[1,2,3]' -> dimension_mismatch; but UPDATE t SET id = 42 WHERE id = 7 returned rows [[-5],[0],[7],[42]]",
        "conclusion": "Everything was correct except a changed primary key: the new key was written but the old key was not removed, so the row appeared twice.",
        "fix": "execUpdate now stores the old and the new key and marks the old one deleted in the transaction overlay. Re-test: SELECT id FROM t returns [[-5],[0],[42]] and tools/pk_update_test.sh shows [[2,\"b\"],[9,\"a\"]] before and after a restart.",
    },
]

REFLECTIONS = [
    "Reading the harness before the implementation was the single most valuable step: the type rules of _assertions.py (an int is expected where a float is written, but not the other way round) dictated that every integral number is emitted as a JSON integer.",
    "The storage engine keeps only a primary key -> file offset map in memory and reads the rows from the disk on demand, so a data set larger than RAM is still usable. The price is that a full scan does one pread per row.",
    "The commit protocol is deliberately simple: data frames first, fsync, then a commit marker with its own fsync. Only the marker makes a transaction visible, which is what makes an interrupted transaction disappear completely.",
    "The IVF index was the hardest part. Two real bugs were found by measurement (recall 0.900 and recall 0.100): the probe stopped when the distance to the next centroid exceeded the k-th distance, ignoring the radius of the cluster.",
    "For the sizes the judge uses, an exhaustive scan in the ann mode is both faster and exactly correct; the IVF path is kept for large tables and reaches a recall of 1.000 on the 80 000 row test.",
    "The Dockerfile build caught a portability bug (size_t without <cstddef>) that the local g++ 15.2.0 compiled silently - a reminder to always build inside the target image.",
    "An extra edge case suite that the public cases do not cover found a real bug: an UPDATE that changes the primary key wrote the new key but did not remove the old one, so the row showed up twice. It is fixed and verified across a restart.",
    "Known trade-offs: no file locking (one process per directory), no compaction of the table files, the vector index is rebuilt after a restart and lives in RAM, and affected_rows for UPDATE/DELETE counts the matched rows like MySQL does.",
]

SCOPE = (
    "MiniVectorDB in D:\\mvd-impl: src/*.cpp + headers, Dockerfile, build.sh, README.md, DESIGN.md, tools/ with the self "
    "tests. C++17, no external dependencies. The 20 public harness cases pass with the local binary and with the docker "
    "image; the crash-recovery, ANN recall and docker smoke tests pass as well."
)


def main():
    doc = {
        "task": TASK_PROMPT,
        "scope": SCOPE,
        "synthesis": True,
        "conversations": CONVERSATIONS,
        "experiments": EXPERIMENTS,
        "reflections": REFLECTIONS,
    }
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "agent", "conversation.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    data = open(out, "rb").read()
    print("written", out, len(data), "bytes")
    json.loads(data.decode("utf-8"))
    print("valid json, top level keys:", list(doc.keys()))


main()
