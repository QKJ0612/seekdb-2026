#!/usr/bin/env python3
"""Exercises the real IVF (k-means) path of the vector index.

The recall test (tools/ann_recall_test.py) uses a small table which is served
by the exhaustive path of the index.  This script fills a table with more rows
than the exhaustive threshold so that the IVF clusters are really used, and
compares the ANN top-k against the exact top-k.

Usage: python3 tools/ann_ivf_test.py [path-to-binary]
"""
import json
import os
import random
import subprocess
import sys
import time

BINARY = sys.argv[1] if len(sys.argv) > 1 else "/mnt/d/mvd-impl/build/vectordb"
DATA_DIR = "/tmp/mvd-ivf"
ROWS = 80000
DIM = 8
TOPK = 10
CLUSTERS = 8


class Server:
    def __init__(self, data_dir):
        env = dict(os.environ, VECTORDB_DATA_DIR=data_dir)
        self.proc = subprocess.Popen(
            [BINARY, data_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
            text=True,
            bufsize=1,
        )
        assert self.proc.stdout.readline().strip() == '{"type":"ready"}'
        self.next_id = 1

    def send(self, sql, mode=None):
        request = {"id": self.next_id, "op": "execute", "sql": sql}
        if mode:
            request["search_mode"] = mode
        self.next_id += 1
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed the connection")
        return json.loads(line)

    def close(self):
        self.proc.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
        self.proc.stdin.flush()
        self.proc.stdout.readline()
        self.proc.wait()


def main():
    os.system("rm -rf %s" % DATA_DIR)
    os.makedirs(DATA_DIR, exist_ok=True)
    random.seed(4242)

    server = Server(DATA_DIR)
    server.send("CREATE TABLE docs (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR(%d))" % DIM)

    centers = [[random.uniform(-50, 50) for _ in range(DIM)] for _ in range(CLUSTERS)]
    chunk = []
    started = time.time()
    for row_id in range(1, ROWS + 1):
        center = centers[row_id % CLUSTERS]
        vec = [round(center[d] + random.gauss(0, 3), 3) for d in range(DIM)]
        chunk.append("(%d, 'c%d', '[%s]')" % (row_id, row_id % CLUSTERS, ",".join(str(v) for v in vec)))
        if len(chunk) == 500:
            server.send("INSERT INTO docs VALUES " + ",".join(chunk))
            chunk = []
    if chunk:
        server.send("INSERT INTO docs VALUES " + ",".join(chunk))
    print("inserted %d rows in %.1fs" % (ROWS, time.time() - started))

    server.send("CREATE VECTOR INDEX docs_vec ON docs (e)")

    worst = 1.0
    for trial in range(5):
        query = [round(random.uniform(-50, 50), 3) for _ in range(DIM)]
        literal = "[%s]" % ",".join(str(v) for v in query)
        sql = ("SELECT id, L2_DISTANCE(e, '%s') AS distance FROM docs ORDER BY distance LIMIT %d"
               % (literal, TOPK))
        started = time.time()
        exact = server.send(sql, "exact")["rows"]
        exact_ms = (time.time() - started) * 1000
        started = time.time()
        ann = server.send(sql, "ann")["rows"]
        ann_ms = (time.time() - started) * 1000
        exact_ids = {row[0] for row in exact}
        ann_ids = {row[0] for row in ann}
        recall = len(exact_ids & ann_ids) / float(len(exact_ids))
        worst = min(worst, recall)
        print("query %d recall %.3f exact %.0fms ann %.0fms" % (trial, recall, exact_ms, ann_ms))
        if recall < 0.95:
            print("FAIL recall below 0.95: exact=%s ann=%s" % (sorted(exact_ids), sorted(ann_ids)))
            sys.exit(1)

    server.close()
    print("worst recall: %.3f" % worst)
    print("PASS ann ivf recall")


if __name__ == "__main__":
    main()
