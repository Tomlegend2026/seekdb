#!/usr/bin/env python3
"""Exercise the actual Linux server with restricted interface discovery.

Use a dedicated, stopped instance directory. No instance data is removed.
Only the server process launched by this script is terminated.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seekdb", required=True)
    parser.add_argument("--base-dir", required=True)
    parser.add_argument("--port", type=int, default=2882)
    parser.add_argument("--mysql-client", default="mysql")
    parser.add_argument("--fault", choices=["normal", "deny", "empty", "entropy-denied", "entropy-eof", "entropy-partial"], default="deny")
    parser.add_argument("--expect-failure", action="store_true")
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    base = Path(args.base_dir).resolve()
    base.mkdir(parents=True, exist_ok=True)
    client = [args.mysql_client, "--protocol=socket", "--socket=" + str(base / "run/sql.sock"),
              "--user=root", "--batch", "--skip-column-names", "--connect-timeout=2"]

    def query(sql):
        return subprocess.run(client, input=sql, text=True, capture_output=True, timeout=5)

    if query("SELECT 1;").returncode == 0:
        raise RuntimeError("Dedicated test instance is already running; stop it first")
    log_path = base / "log/seekdb.log"
    old_log = log_path.stat() if log_path.exists() else None
    fixture = Path(__file__).with_name("uuid_node_fault_injection.c")
    with tempfile.TemporaryDirectory(prefix="seekdb-uuid-test-") as temporary:
        shim = Path(temporary) / "uuid_node_fault.so"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        str(fixture), "-ldl", "-o", str(shim)], check=True)
        env = os.environ.copy()
        env["LD_PRELOAD"] = str(shim)
        env["SEEKDB_UUID_TEST_MODE"] = args.fault
        command = [str(Path(args.seekdb).resolve()), "--nodaemon", "--base-dir=" + str(base),
                   "--port=" + str(args.port), "--parameter", "memory_budget=2G",
                   "--parameter", "log_disk_size=2G", "--parameter", "datafile_size=512M",
                   "--parameter", "datafile_maxsize=2G"]
        with (Path(temporary) / "startup.log").open("wb") as output:
            server = subprocess.Popen(command, env=env, stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + args.timeout
                ready = False
                while time.monotonic() < deadline:
                    if server.poll() is not None:
                        break
                    if query("SELECT 1;").returncode == 0:
                        ready = True
                        break
                    time.sleep(0.2)
                if args.expect_failure:
                    if ready or server.poll() in (None, 0):
                        raise AssertionError("Expected UUID initialization failure")
                    current = log_path.stat()
                    offset = old_log.st_size if old_log and old_log.st_ino == current.st_ino else 0
                    with log_path.open("rb") as log:
                        log.seek(max(offset, current.st_size - 1024 * 1024))
                        diagnostic = log.read().decode("utf-8", "replace")
                    startup = (Path(temporary) / "startup.log").read_text(errors="replace")
                    uuid_failure = "init uuid node failed" in diagnostic or (
                        "init sql expr static var" in diagnostic and "errcode=-4016" in diagnostic)
                    if "UUID_TEST: forced interface discovery" not in startup or not uuid_failure:
                        raise AssertionError("Server failed outside UUID initialization; exit="
                                             + str(server.returncode) + "\n" + startup[-2000:]
                                             + "\n" + diagnostic[-4000:])
                    print("PASS expected UUID startup failure: " + args.fault, flush=True)
                else:
                    if not ready:
                        raise AssertionError("Server did not become SQL ready; exit=" + str(server.poll()))
                    digits = "(" + " UNION ALL ".join("SELECT " + str(n) + " AS n" for n in range(10)) + ")"
                    result = query("SELECT UUID() FROM " + digits + " a CROSS JOIN " + digits + " b CROSS JOIN " + digits + " c;")
                    if result.returncode:
                        raise AssertionError(result.stderr)
                    values = result.stdout.splitlines()
                    assert len(values) == 1000 and len(set(values)) == 1000
                    parsed = [uuid.UUID(value) for value in values]
                    assert all(value.version == 1 and value.variant == uuid.RFC_4122 for value in parsed)
                    if args.fault != "normal":
                        assert all(((value.node >> 40) & 1) == 1 for value in parsed)
                    print("PASS SQL ready; 1000 unique RFC4122 UUIDs: " + args.fault, flush=True)
            finally:
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()


if __name__ == "__main__":
    main()
