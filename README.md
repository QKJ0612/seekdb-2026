# MiniVectorDB

A small, self-contained vector database written from scratch in **C++17**.
It speaks a JSON-Lines protocol over **stdin/stdout**, stores its data in a
single directory, and implements a small SQL subset with explicit
transactions, crash-safe persistence and a hand-written IVF-Flat vector index.

* No external dependencies — no SQLite, no RocksDB, no Faiss/VSAG/HNSWlib, no
  numpy. The JSON parser, the SQL parser and the vector index are all part of
  this repository.
* Single binary, single process, foreground: ideal for a container that is
  started with `--pids-limit=128 --read-only` and a volume mounted at `/data`.

---

## 1. Language, build and run

| Item | Value |
| --- | --- |
| Language | C++17 (no compiler extensions, no third-party headers) |
| Local compiler | g++ ≥ 9 (tested with g++ 15.2.0 in WSL) |
| Docker build compiler | `gcc:12` (Debian) |
| Dependencies | none |
| Output | `build/vectordb` (single static binary) |

### Build inside the container image (what the judge does)

```bash
docker build -t minivectordb .
```

The `Dockerfile` is a **multi-stage** build: the `build` stage compiles the
sources that are copied into the image (no prebuilt binary is shipped), the
runtime stage only contains the resulting static binary:

```
FROM gcc:12 AS build          # compiles src/*.cpp with g++ -O2 -std=c++17 -static
FROM debian:bookworm-slim     # runtime image, ~29 MB of content
```

### Build locally (no Docker)

```bash
./build.sh                    # -> build/vectordb
# or explicitly:
g++ -O2 -std=c++17 -o build/vectordb src/*.cpp
```

### Run

```bash
./build/vectordb [DATA_DIR]
```

* **Entrypoint**: `ENTRYPOINT ["/usr/local/bin/vectordb"]` — the server runs in
  the foreground as PID 1, no port is opened, so `SIGKILL`/`SIGTERM` from the
  judge reach the database directly.
* `DATA_DIR` (optional argument) is the directory that holds the data files.
* The **effective data directory** is chosen in this order:
  1. `$VECTORDB_DATA_DIR` — used when the directory exists **and** is writable;
  2. the `DATA_DIR` command line argument (used by the local test harness);
  3. `/data` — the fallback used inside the container.

  Everything that is persisted (table data, catalog, commit log) lives in that
  single directory. The container image sets `ENV VECTORDB_DATA_DIR=/data`, so a
  plain `docker run` writes to `/data` as well.

### Running it with the judge's flags

```bash
docker run --rm -i \
  --network=none --cpus=2 --memory=2g --memory-swap=2g \
  --pids-limit=128 --cap-drop=ALL --security-opt=no-new-privileges \
  --read-only --mount type=bind,src="$PWD/data",dst=/data \
  minivectordb:latest
```

---

## 2. Protocol

One JSON object per line on stdin, one JSON object per line on stdout.

* On startup the server prints **exactly** `{"type":"ready"}` and flushes it
  immediately — the judge never sends SQL first.
* Request: `{"id":<any JSON type>,"op":"execute","sql":"<SQL>","search_mode":"exact"|"ann"}`
  (`search_mode` is optional and defaults to `exact`).
* Request: `{"id":<n>,"op":"shutdown"}` — answered with
  `{"id":<n>,"ok":true,"type":"result"}`, then the process exits with code 0
  and writes nothing else to stdout.
* Success response:

  ```json
  {"id":1,"ok":true,"columns":["id","content"],"rows":[[1,"hello"]],"affected_rows":1,"message":"OK","elapsed_ms":0.42}
  ```

* Failure response:

  ```json
  {"id":2,"ok":false,"error":{"code":"duplicate_key","message":"duplicate primary key 1"}}
  ```

The `id` is echoed with the **same JSON type** it was sent with. `ok` is
always a JSON boolean, no `NaN`/`Infinity` is ever produced, keys are never
duplicated and stdout carries protocol lines only — diagnostics go to stderr.
Responses are limited to 16 MiB (`response_too_large` beyond that).

---

## 3. Supported SQL scope

```sql
-- DDL ---------------------------------------------------------------
CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(32), embedding VECTOR(4));
CREATE VECTOR INDEX ON t (embedding);
CREATE VECTOR INDEX idx_name ON t (embedding);      -- optional index name

-- DML ---------------------------------------------------------------
INSERT INTO t VALUES (1, 'a', '[1,2,3,4]');
INSERT INTO t (id, name) VALUES (2, 'b'), (3, 'c');  -- multi row, all-or-nothing
UPDATE t SET name = 'x' [, note = 'y'] [WHERE ...];
DELETE FROM t [WHERE ...];

-- Queries -----------------------------------------------------------
SELECT * FROM t;
SELECT id, name FROM t [WHERE <cond>] [ORDER BY col [ASC|DESC]] [LIMIT n];
SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM t ORDER BY distance LIMIT 10;

-- Transactions ------------------------------------------------------
BEGIN;  ... ; COMMIT;      -- or ROLLBACK
```

Column types: `BIGINT` (primary key, exactly one per table), `VARCHAR(n)`,
`VECTOR(n)` (at most one vector column per table).

Conditions: `=`, `!=`, `<>`, `>`, `>=`, `<`, `<=`, `AND`, `OR`, `IS NULL`,
`IS NOT NULL`, `col IN (SELECT ...)` / `col NOT IN (SELECT ...)`.

Everything outside this subset (`ALTER TABLE`, `DROP`, `JOIN`, `GROUP BY`,
`CREATE TABLE ... AS SELECT`, other functions, …) is rejected with the error
code `unsupported_sql` — the server never silently accepts a statement it did
not implement.

### Semantics

| Topic | Behaviour |
| --- | --- |
| Row order | `SELECT` without `ORDER BY` returns rows in **primary key ascending** order. |
| Vector search | `L2_DISTANCE(a,b)` is the **Euclidean** distance (not squared): `L2([3,4],[0,0]) = 5.0`. |
| Ties | Equal distances are broken by primary key ascending. |
| Filtered TopK | The top-K is computed **inside** the filtered set; if fewer rows match, all of them are returned; if nothing matches, `rows` is `[]`. |
| Vector literals | A vector is written as a string: `'[3,4]'`. The dimension must match the column. |
| Vector precision | Vectors are **float32**: `16777217` is stored as `16777216`, therefore the distance between them is `0.0`. `NaN`, `Infinity` and values that overflow float32 are rejected. |
| Duplicate key | `duplicate_key`; the row that was already stored stays untouched. |
| Dimension mismatch | `dimension_mismatch`; the **whole statement** is rejected — for a multi-row `INSERT` not a single row is applied. |
| Failed statement in a transaction | Only that statement is rolled back, the previous statements of the same transaction are kept and are committed by `COMMIT`. |
| `affected_rows` | `INSERT` → number of inserted rows, `UPDATE`/`DELETE` → number of matched rows, `SELECT` → number of returned rows. |
| Vector columns in results | Returned as a JSON array of numbers, e.g. `[3,4]`. |
| `search_mode` | `exact` always computes the exact answer, even when a vector index exists. `ann` uses the IVF index when the table has one and the recall is guaranteed to be ≥ 0.95 (see §5). |
| Strings | Compared by Unicode code point; the length of a `VARCHAR` is counted in Unicode characters. |

### Error codes

`duplicate_key`, `dimension_mismatch`, `unsupported_sql` (the three codes the
judge checks explicitly) plus `syntax_error`, `invalid_vector`,
`value_too_long`, `table_exists`, `table_not_found`, `column_not_found`,
`type_mismatch`, `column_count_mismatch`, `invalid_request`,
`response_too_large`, `internal_error`.

Every error is reported as `{"ok":false,"error":{"code":"...","message":"..."}}`;
a failing statement never terminates the process.

---

## 4. Worked examples

Start from an empty directory:

```bash
mkdir -p /tmp/mvdb-data && rm -f /tmp/mvdb-data/*
./build/vectordb /tmp/mvdb-data
```

The server prints `{"type":"ready"}` and waits for requests. The examples below
are the exact lines that are typed into stdin (each line is one request).

### 4.1 Write and query

```
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, title VARCHAR(32), embedding VECTOR(2))"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1, 'first', '[3,4]'), (2, 'second', '[0,5]'), (3, 'third', '[1,1]')"}
{"id":3,"op":"execute","sql":"SELECT id, title FROM docs"}
{"id":4,"op":"execute","sql":"UPDATE docs SET title = 'changed' WHERE id = 2"}
{"id":5,"op":"execute","sql":"DELETE FROM docs WHERE id = 3"}
{"id":6,"op":"execute","sql":"SELECT id, title, embedding FROM docs"}
{"id":7,"op":"shutdown"}
```

Responses:

```
{"id":1,"ok":true,"columns":[],"rows":[],"affected_rows":0,"message":"OK",...}
{"id":2,"ok":true,"columns":[],"rows":[],"affected_rows":3,"message":"OK",...}
{"id":3,"ok":true,"columns":["id","title"],"rows":[[1,"first"],[2,"second"],[3,"third"]],"affected_rows":3,"message":"OK",...}
{"id":4,"ok":true,"columns":[],"rows":[],"affected_rows":1,"message":"OK",...}
{"id":5,"ok":true,"columns":[],"rows":[],"affected_rows":1,"message":"OK",...}
{"id":6,"ok":true,"columns":["id","title","embedding"],"rows":[[1,"first",[3,4]],[2,"changed",[0,5]]],"affected_rows":2,"message":"OK",...}
{"id":7,"ok":true,"type":"result"}
```

### 4.2 Vector search

```
{"id":1,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs ORDER BY distance LIMIT 2"}
{"id":2,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs WHERE id > 1 ORDER BY distance"}
{"id":3,"op":"execute","sql":"SELECT id FROM docs WHERE id IN (SELECT id FROM docs WHERE id >= 2)"}
```

```
{"id":1,"ok":true,"columns":["id","distance"],"rows":[[3,1.4142135623730951],[1,5]],"affected_rows":2,...}
{"id":2,"ok":true,"columns":["id","distance"],"rows":[[3,1.4142135623730951],[2,5]],"affected_rows":2,...}
{"id":3,"ok":true,"columns":["id"],"rows":[[2],[3]],"affected_rows":2,...}
```

(`[1,1]` is `sqrt(2)` away from the origin, `[3,4]` and `[0,5]` are both at
distance 5 — such a tie is broken by the primary key.)

### 4.3 Commit and rollback

```
{"id":1,"op":"execute","sql":"BEGIN"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (10, 'temp', '[0,0]')"}
{"id":3,"op":"execute","sql":"SELECT id FROM docs"}
{"id":4,"op":"execute","sql":"ROLLBACK"}
{"id":5,"op":"execute","sql":"SELECT id FROM docs"}
```

```
{"id":2,"ok":true,...,"affected_rows":1,...}
{"id":3,"ok":true,"columns":["id"],"rows":[[1],[2],[10]],"affected_rows":3,...}   <- inside the transaction
{"id":5,"ok":true,"columns":["id"],"rows":[[1],[2]],"affected_rows":2,...}        <- rolled back
```

A statement that fails inside a transaction only rolls **itself** back:

```
{"id":1,"op":"execute","sql":"BEGIN"}
{"id":2,"op":"execute","sql":"UPDATE docs SET title = 'kept' WHERE id = 1"}
{"id":3,"op":"execute","sql":"INSERT INTO docs VALUES (2, 'dup', '[0,0]')"}   -> duplicate_key
{"id":4,"op":"execute","sql":"SELECT id, title FROM docs"}                      -> shows the UPDATE
{"id":5,"op":"execute","sql":"COMMIT"}
```

### 4.4 Restart

Stop the server with `{"id":N,"op":"shutdown"}` (or kill it) and start it again
with the same directory:

```bash
./build/vectordb /tmp/mvdb-data
{"id":1,"op":"execute","sql":"SELECT id, title FROM docs"}
```

All committed rows are still there, uncommitted work is gone. After the
`shutdown` above the directory contains:

```
catalog.log   commit.log   t_1.tbl
```

---

## 5. Vector index

`CREATE VECTOR INDEX ON t (embedding)` builds a **hand-written IVF-Flat**
index (k-means with k-means++ seeding, 25 iterations, deterministic — no
random number generator):

* up to `min(256, sqrt(n))` clusters, one posting list per cluster,
* the k-th best distance is compared against the lower bound
  `max(0, dist(query, centroid) - cluster_radius)` of the next cluster, so a
  cluster that reaches towards the query is never skipped,
* at least 8 clusters are probed, which keeps the recall high.

Tables with at most 65 536 vectors are searched **exhaustively** inside the
`ann` mode — the answer is then identical to the exact answer (recall 1.0).
The IVF path takes over for larger tables. `exact` mode never uses the index
and always computes the exact answer.

Measured recall (see `tools/ann_recall_test.py` and `tools/ann_ivf_test.py`):
1.000 for the 3 000-row random test and 1.000 for the 80 000-row clustered
test.

---

## 6. Durability

* Every committed transaction is written to disk **and fsynced before the
  response is sent**.
* The write-ahead log is an append-only file with a length + CRC32 per record
  and an explicit commit marker, so a torn write is detected and the
  incomplete tail is discarded during recovery.
* After `kill -9` the committed data is intact, uncommitted data is not
  applied and the server can be killed again while it is recovering.

See `DESIGN.md` for the file formats and the recovery algorithm.

---

## 7. Repository layout

```
build.sh                  g++ -O2 -std=c++17 -> build/vectordb
Dockerfile                multi stage build (gcc:12 -> debian:bookworm-slim)
src/main.cpp              stdio JSON-Lines server
src/db.hpp, src/db.cpp    storage engine: files, WAL, recovery, transactions
src/exec.cpp              statement executor (DDL/DML/queries/transactions)
src/sql.hpp, src/sql.cpp  tokenizer + SQL parser
src/json.hpp, src/json.cpp hand written JSON parser / serializer
src/ann.hpp, src/ann.cpp  hand written IVF-Flat vector index
src/util.hpp              CRC32, UTF-8 helpers, formatting, path helpers
tools/                    self tests (harness runner, crash recovery, ANN recall,
                          edge cases, docker smoke test)
agent/conversation.json   development log of this implementation
DESIGN.md                 design and validation record
```
