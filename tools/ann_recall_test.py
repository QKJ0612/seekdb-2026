#!/usr/bin/env python3
"""Recall test of the approximate vector index.

The judge requires that the "ann" search mode returns (almost) the same rows
as the exact search.  This script fills a table with random vectors, builds a
vector index and compares the ANN top-k with the exact top-k.

Usage: python3 tools/ann_recall_test.py [path-to-binary]
"""
import json
import os
import random
import subprocess
import sys

BINARY = sys.argv[1] if len(sys.argv) > 1 else "/mnt/d/mvd-impl/build/vectordb"
DATA_DIR = "/tmp/mvd-ann"
ROWS = 3000
DIM = 16
TOPK = 10


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
        return json.loads(self.proc.stdout.readline())

    def close(self):
        self.proc.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
        self.proc.stdin.flush()
        self.proc.stdout.readline()
        self.proc.wait()


def main():
    os.system("rm -rf %s" % DATA_DIR)
    os.makedirs(DATA_DIR, exist_ok=True)
    random.seed(20261114)

    server = Server(DATA_DIR)
    server.send("CREATE TABLE docs (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR(%d))" % DIM)

    vectors = {}
    chunk = []
    for row_id in range(1, ROWS + 1):
        vec = [round(random.uniform(-10, 10), 3) for _ in range(DIM)]
        vectors[row_id] = vec
        chunk.append("(%d, 'c%d', '[%s]')" % (row_id, row_id % 3, ",".join(str(v) for v in vec)))
        if len(chunk) == 200:
            server.send("INSERT INTO docs VALUES " + ",".join(chunk))
            chunk = []
    if chunk:
        server.send("INSERT INTO docs VALUES " + ",".join(chunk))

    server.send("CREATE VECTOR INDEX docs_vec ON docs (e)")

    worst = 1.0
    for trial in range(20):
        query = [round(random.uniform(-10, 10), 3) for _ in range(DIM)]
        literal = "[%s]" % ",".join(str(v) for v in query)
        sql = ("SELECT id, L2_DISTANCE(e, '%s') AS distance FROM docs ORDER BY distance LIMIT %d"
               % (literal, TOPK))
        exact = server.send(sql, "exact")["rows"]
        ann = server.send(sql, "ann")["rows"]
        exact_ids = {row[0] for row in exact}
        ann_ids = {row[0] for row in ann}
        recall = len(exact_ids & ann_ids) / float(len(exact_ids))
        worst = min(worst, recall)
        if recall < 0.95:
            print("FAIL recall %.3f exact=%s ann=%s" % (recall, sorted(exact_ids), sorted(ann_ids)))
            sys.exit(1)

    # Filtered top-k: the filter has to be applied inside the filtered set.
    query = [0.0] * DIM
    literal = "[%s]" % ",".join(str(v) for v in query)
    sql = ("SELECT id, L2_DISTANCE(e, '%s') AS distance FROM docs WHERE cat = 'c1' "
           "ORDER BY distance LIMIT %d" % (literal, TOPK))
    exact = server.send(sql, "exact")["rows"]
    ann = server.send(sql, "ann")["rows"]
    exact_ids = {row[0] for row in exact}
    ann_ids = {row[0] for row in ann}
    filtered_recall = len(exact_ids & ann_ids) / float(max(1, len(exact_ids)))
    print("worst top-%d recall over 20 queries: %.3f" % (TOPK, worst))
    print("filtered recall: %.3f (exact %d rows, ann %d rows)" % (filtered_recall, len(exact), len(ann)))
    if worst < 0.95 or filtered_recall < 0.95:
        print("FAIL recall below 0.95")
        sys.exit(1)
    server.close()
    print("PASS ann recall")


if __name__ == "__main__":
    main()
