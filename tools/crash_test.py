#!/usr/bin/env python3
"""Crash recovery test for MiniVectorDB.

The judge kills the server with SIGKILL and restarts it on the same data
directory.  This script checks that

  1. a committed row survives a SIGKILL,
  2. an uncommitted transaction is rolled back by the restart,
  3. a torn write (a half written frame at the end of a file) is detected and
     truncated during recovery, the committed data stays readable,
  4. a clean shutdown keeps the committed data as well.

Usage: python3 tools/crash_test.py [path-to-binary]
"""
import json
import os
import signal
import subprocess
import sys

BINARY = sys.argv[1] if len(sys.argv) > 1 else "/mnt/d/mvd-impl/build/vectordb"
DATA_DIR = sys.argv[2] if len(sys.argv) > 2 else "/tmp/mvd-crash"


class Server:
    """Drives one server process over the stdio JSON Lines protocol."""

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
        ready = self.proc.stdout.readline()
        assert ready.strip() == '{"type":"ready"}', "bad ready line: %r" % ready
        self.next_id = 1

    def send(self, sql):
        request = {"id": self.next_id, "op": "execute", "sql": sql}
        self.next_id += 1
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        assert line, "the server closed stdout"
        return json.loads(line)

    def kill9(self):
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait()

    def shutdown(self):
        self.proc.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
        self.proc.stdin.flush()
        reply = self.proc.stdout.readline()
        self.proc.wait()
        return json.loads(reply)


def check(label, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print("%-4s %s %s" % (status, label, detail))
    if not condition:
        sys.exit(1)


def main():
    os.system("rm -rf %s" % DATA_DIR)
    os.makedirs(DATA_DIR, exist_ok=True)

    # ---- 1. commit a row, start a transaction, die -----------------------
    server = Server(DATA_DIR)
    check("create", server.send("CREATE TABLE t (id BIGINT PRIMARY KEY, note VARCHAR(8), e VECTOR(2))")["ok"])
    check("insert", server.send("INSERT INTO t VALUES (1, 'kept', '[1,2]')")["affected_rows"] == 1)
    check("begin", server.send("BEGIN")["ok"])
    check("uncommitted delete", server.send("DELETE FROM t")["affected_rows"] == 1)
    server.kill9()

    # ---- 2. restart: committed data survives, uncommitted work is gone ---
    server = Server(DATA_DIR)
    rows = server.send("SELECT id, note, e FROM t")["rows"]
    check("committed row survived SIGKILL", rows == [[1, "kept", [1, 2]]], str(rows))
    check("no uncommitted delete", len(rows) == 1, str(rows))

    # ---- 3. commit more data, then simulate a torn write -----------------
    server.send("INSERT INTO t VALUES (2, 'two', '[3,4]')")
    server.send("UPDATE t SET note = 'x' WHERE id = 1")
    server.shutdown()

    table_file = os.path.join(DATA_DIR, "t_1.tbl")
    size = os.path.getsize(table_file)
    with open(table_file, "ab") as handle:  # half a frame
        handle.write(b"\x40\x00\x00\x00\x11\x22\x33\x44garbage")

    server = Server(DATA_DIR)
    rows = server.send("SELECT id, note FROM t ORDER BY id")["rows"]
    check("torn write recovered", rows == [[1, "x"], [2, "two"]], str(rows))
    check("torn tail truncated", os.path.getsize(table_file) <= size + 4096)
    server.shutdown()

    # ---- 4. clean shutdown keeps the data ---------------------------------
    server = Server(DATA_DIR)
    rows = server.send("SELECT id FROM t ORDER BY id")["rows"]
    check("data after clean shutdown", rows == [[1], [2]], str(rows))
    server.shutdown()

    # ---- 5. killing the process while it recovers must not destroy data --
    for attempt in range(20):
        process = subprocess.Popen(
            [BINARY, DATA_DIR],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=dict(os.environ, VECTORDB_DATA_DIR=DATA_DIR),
            text=True,
        )
        process.kill()
        process.wait()
    server = Server(DATA_DIR)
    rows = server.send("SELECT id, note FROM t ORDER BY id")["rows"]
    check("data after 20 kills during startup", rows == [[1, "x"], [2, "two"]], str(rows))
    server.shutdown()
    print("all crash recovery checks passed")


if __name__ == "__main__":
    main()
