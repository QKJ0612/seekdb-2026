# MiniVectorDB — design and validation record

MiniVectorDB is a single-process vector database written from scratch in C++17.
It speaks JSON Lines over stdio, stores everything in one directory and
implements the SQL subset of the competition.

---

## 1. Architecture

```
main.cpp   stdio JSON-Lines server: read request -> parse JSON -> Database::execute
           -> build response JSON -> write one line -> flush
exec.cpp   statement executor: DDL, DML, queries, transactions, expression evaluation
db.cpp     storage engine: files, frames, WAL, commit protocol, recovery
sql.cpp    tokenizer + recursive descent SQL parser
json.cpp   hand written JSON parser / serializer
ann.cpp    hand written IVF-Flat vector index
util.hpp   CRC32, UTF-8 helpers, number formatting, path helpers
```

No third-party code is used: the JSON parser, the SQL parser, the CRC32, the
k-means clustering and the index are all part of this repository.

---

## 2. Storage layout

Everything lives in the **data directory** (effective directory:
`$VECTORDB_DATA_DIR` if it exists and is writable, otherwise the `argv[1]`
argument, otherwise `/data`):

```
<data dir>/
  catalog.log   DDL frames (CREATE TABLE, CREATE VECTOR INDEX)
  commit.log    one commit marker per committed transaction
  t_<id>.tbl    row frames of table <id>
```

### 2.1 Frame format (all files)

Every file is a sequence of frames:

```
+--------------------+------------------+---------------------+
| u32 payloadLen     | u32 crc32(payload)| payload bytes       |
+--------------------+------------------+---------------------+
```

Little endian. `crc32` is the standard zlib polynomial (0xEDB88320). A frame
whose length exceeds the remaining bytes, or whose CRC does not match, is a
*torn write*: the reader stops there and the file is truncated to the end of
the last valid frame.

### 2.2 Row frames (`t_<id>.tbl`)

```
payload := kind:u8, txnId:u64, pk:i64, values...
kind    := 1 = row version (insert or update), 2 = delete
```

A row version contains the **complete** row (all columns), so a scan never
has to merge versions; the newest committed version of a key wins.

Values are encoded as

```
tag 0 = NULL
tag 1 = BIGINT   (i64, little endian)
tag 2 = VARCHAR  (u32 byte length + UTF-8 bytes)
tag 3 = VECTOR   (u32 element count + float32 bit patterns)
tag 4 = DOUBLE   (IEEE-754 double, 8 bytes)
```

Vectors are stored as **float32**, therefore `16777217` is stored as
`16777216` and the distance between the two is exactly `0.0`.

### 2.3 Catalog frames (`catalog.log`)

```
kind 1 = CREATE TABLE:      i64 tableId, name, u32 nCols, per column:
                              name, u8 type, u32 param (VARCHAR len / VECTOR dim), u8 flags
kind 2 = CREATE VECTOR INDEX: i64 tableId, index name, column name
```

### 2.4 Commit markers (`commit.log`)

One frame per committed transaction, payload = `u64 txnId`. The **maximum**
committed transaction id in the log is the only thing that decides which
frames of the other files are visible.

---

## 3. Index scheme

### 3.1 Primary key index

In memory, per table:

```cpp
std::map<int64_t, uint64_t> index;   // primary key -> offset of the newest row frame
```

* `std::map` iterates in **primary key ascending** order, which is exactly the
  order an `ORDER BY`-less `SELECT` must return.
* Only the *offset* of a row is kept — the row payload is read from the file
  with `pread` when it is needed. The data set therefore does **not** have to
  fit into memory; the index grows with the number of rows, not with the size
  of the rows.
* Fallback: the whole table file is scanned during `open()`; if the index is
  lost the file is simply rescanned (the scan is also what rebuilds the index
  after a restart).

### 3.2 Vector index (IVF-Flat, hand written)

`CREATE VECTOR INDEX ON t (embedding)` enables an in-memory IVF-Flat index:

* `k = min(256, sqrt(n))` clusters, deterministic **k-means++** seeding
  (farthest point traversal — no random number generator, so the index is
  reproducible),
* 25 k-means iterations on a deterministic sample (cap 20 000 points); the
  iteration count is reduced automatically for very large training sets so the
  build stays fast,
* every vector is then assigned to its nearest centroid → posting lists,
* the **cluster radius** (max distance of a member to its centroid) is stored
  per cluster,
* a query probes the clusters in the order of the lower bound
  `max(0, dist(query, centroid) - radius)` and stops when that bound exceeds
  the k-th best distance; at least 8 clusters are always probed.

Two extra rules make the recall requirement (`>= 0.95`) hold in practice:

* tables with at most **65 536** vectors are searched **exhaustively** in
  `ann` mode — the result is then bit-identical to the exact result,
* if the index returns fewer candidates than `LIMIT` requires, the query falls
  back to an exact scan.

The index is **not persisted**: it is rebuilt on demand and invalidated by the
table version counter, so `CREATE VECTOR INDEX` never breaks the transaction
semantics (it is a normal, transactional DDL statement).

`exact` mode never touches the index.

---

## 4. Transactions

* **Autocommit** (default): every statement is its own transaction.
* `BEGIN` starts an explicit transaction, `COMMIT` ends it, `ROLLBACK` undoes
  it. `START TRANSACTION` is accepted as a synonym of `BEGIN`.
* An explicit transaction collects its changes in an **in-memory overlay**

  ```cpp
  std::map<int64_t, std::map<int64_t, TxnChange>> changes;  // tableId -> pk -> change
  ```

  where `TxnChange` is either a delete or the complete new row. Uncommitted
  data never reaches the disk.
* **Statement granularity**: before every statement the overlay is snapshotted;
  if the statement fails, the snapshot is restored. A failed statement
  therefore rolls back **only itself** — the previous statements of the same
  transaction survive and are committed by `COMMIT`.
* `INSERT` validates **all** rows before applying any of them, so a
  multi-row insert is atomic: a `duplicate_key` or `dimension_mismatch` in the
  third row leaves the first two rows unwritten.
* `CREATE TABLE` inside an explicit transaction is deferred until `COMMIT`;
  a `ROLLBACK` discards it.

---

## 5. Persistence and recovery

### 5.1 Commit protocol

```
1. append the row/delete frames to the table files      (no fsync yet)
2. append the DDL frames to catalog.log                 (only for DDL)
3. fsync every touched table file (+ catalog.log)
4. append the commit marker (txnId) to commit.log
5. fsync commit.log          <-- the transaction becomes visible here
6. publish the new offsets in the in-memory index
7. answer the client
```

The response is only sent **after** step 5, therefore a committed transaction
is durable before the client learns about it. A crash between steps 1 and 5
leaves frames that are not covered by a commit marker — they are invisible
after recovery (their `txnId` is greater than the maximum committed id) and
are simply overwritten by the next write to the same key.

### 5.2 Recovery (`Database::open()`)

```
1. read commit.log  -> maxCommittedTxn (the highest committed transaction id)
2. read catalog.log -> apply every DDL frame whose txnId <= maxCommittedTxn
3. for every t_<id>.tbl:
       scan the frames; stop at the first torn frame and truncate the file
       to the end of the last valid frame
       for every frame with txnId <= maxCommittedTxn:
            row frame    -> index[pk] = offset of the frame
            delete frame -> erase index[pk]
```

Because the recovery only reads frames whose transaction id is covered by a
commit marker, an uncommitted transaction can never be applied partially. The
recovery itself only reads and truncates — it never appends — so being killed
*during* the recovery is harmless: the next start repeats the same steps and
finds the same state. `commit.log` is compacted (rewritten into a temporary
file and renamed) when it contains more entries than the number of tables
plus a margin.

### 5.3 Verified behaviour

`tools/crash_test.py` checks exactly the scenarios the hidden tests use:

| Check | Result |
| --- | --- |
| committed row survives `kill -9` | PASS — `[[1, 'kept', [1, 2]]]` |
| uncommitted `DELETE` is rolled back by the restart | PASS — `[[1, 'kept', [1, 2]]]` |
| torn write in the middle of a table file | PASS — `[[1, 'x'], [2, 'two']]`, tail truncated |
| clean shutdown keeps the data | PASS — `[[1], [2]]` |
| 20 kills during startup leave the data intact | PASS — `[[1, 'x'], [2, 'two']]` |

---

## 6. Implemented features

* **Protocol**: `{"type":"ready"}` on startup, one JSON request per line, one
  JSON response per line, the id is echoed with the same JSON type, `ok` is
  always a boolean, no `NaN`/`Infinity`, no duplicate keys, stdout carries
  protocol lines only (diagnostics go to stderr), `shutdown` answers
  `{"id":N,"ok":true,"type":"result"}` and exits with code 0.
* **SQL**: `CREATE TABLE`, `CREATE VECTOR INDEX [name]`, `INSERT` (single and
  multi row, with or without a column list), `SELECT` (projection, `*`,
  `L2_DISTANCE(...) AS alias`, `WHERE`, `ORDER BY`, `LIMIT`), `UPDATE`,
  `DELETE`, `BEGIN` / `COMMIT` / `ROLLBACK`.
* **Conditions**: `=`, `!=`, `<>`, `>`, `>=`, `<`, `<=`, `AND`, `OR`, `IS NULL`,
  `IS NOT NULL`, `IN (SELECT ...)`, `NOT IN (SELECT ...)`.
* **Error handling**: every failure is reported as
  `{"ok":false,"error":{"code":"...","message":"..."}}`; the process stays
  alive; unknown SQL is rejected with `unsupported_sql` and never silently
  accepted.
* **Types**: `BIGINT` (64 bit), `VARCHAR(n)` (length counted in Unicode
  characters), `VECTOR(n)` (float32, dimension checked, `NaN`/`Infinity`/
  overflow rejected).
* **Ordering**: primary key ascending without `ORDER BY`, ties in a vector
  search broken by primary key ascending, strings compared by Unicode code
  point.
* **Search modes**: `exact` (always exact) and `ann` (IVF-Flat index, recall
  1.0 in the measured tests).

---

## 7. Validation record

| Test | Command | Result |
| --- | --- | --- |
| Public protocol check (local binary) | `cd /root/mvd && python3 protocol_check/run.py --command '["/mnt/d/mvd-impl/build/vectordb","{data_dir}"]' --output /tmp/out1` | `passed: true`, 20/20 cases PASS |
| Public protocol check (docker image) | `cd /root/mvd && python3 protocol_check/run.py --image minivectordb:latest --output /tmp/out_docker` | `passed: true`, 20/20 cases PASS |
| Crash recovery | `python3 tools/crash_test.py` | all checks PASS |
| ANN recall (3 000 rows) | `python3 tools/ann_recall_test.py` | worst top-10 recall **1.000**, filtered recall **1.000** |
| ANN IVF recall (80 000 rows) | `python3 tools/ann_ivf_test.py` | worst recall **1.000**, query ~50x faster than the exact scan |
| Edge cases beyond the public cases | `python3 tools/edge_cases.py` | 27 requests: `LIMIT 0` → `rows: []`, negative keys, `NULL` filters, `value_too_long`, `invalid_vector`, `dimension_mismatch`, changed primary key — all correct |
| Docker image with the judge flags | `tools/docker_smoke.sh` | `{"type":"ready"}` → responses → `{"id":4,"ok":true,"type":"result"}`, exit code 0 |

All 20 public cases pass: `sql_crud`, `sql_comparisons`, `sql_ordering`,
`sql_empty`, `sql_primary_key`, `sql_unicode`, `sql_bigint`, `sql_unsupported`,
`storage_restart`, `storage_mutations`, `storage_multiple_tables`,
`transaction_commit`, `transaction_rollback`, `transaction_failed_insert`,
`transaction_autocommit_atomic`, `transaction_shutdown_rollback`,
`vector_euclidean`, `vector_ties_and_filter`, `vector_float32`,
`vector_index_maintenance`.

---

## 8. Known limitations

* **SQL surface**: only the documented subset. `ALTER TABLE`, `DROP`,
  `JOIN`, `GROUP BY`, `HAVING`, aggregates, other functions and
  `CREATE TABLE ... AS SELECT` are rejected with `unsupported_sql`.
* **No file locking**: the database assumes a single process on the data
  directory. Two servers on the same directory would corrupt each other.
* **No compaction of table files**: an update appends a new row version; the
  old version stays on disk. The file grows with the number of updates, the
  visible data set does not.
* **The vector index lives in RAM**: the IVF-Flat index keeps the vectors of
  the indexed table in memory (O(n·dim)); the exact path streams the rows
  from the disk. For a table with hundreds of millions of vectors the ANN
  path would need to be made disk based.
* **The index is rebuilt after a restart** (it is not persisted); the first
  `ann` query after the restart pays the build cost.
* **`affected_rows` for `UPDATE`/`DELETE` counts the matched rows** (MySQL
  "rows matched" semantics), not the rows whose value actually changed.
* **A `VARCHAR` that is too long is an error** (`value_too_long`) instead of
  being truncated silently.
* **Only one vector column per table** and exactly one `BIGINT` primary key
  are accepted; a table without a primary key is rejected.
* **`IN (SELECT ...)`** must select a single plain column; correlated
  subqueries are not supported.
* **Single threaded**: the requests are processed strictly one after the
  other, which keeps the protocol deterministic but limits the throughput.
