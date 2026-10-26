#!/usr/bin/env python3
"""Exercise native FileSQL on a dedicated local seekdb test instance.

The server and this script must share the local filesystem. The script temporarily
restricts secure_file_priv to its own fixture directory and restores it on exit.
"""
import argparse
from datetime import date, datetime
import json
from pathlib import Path
import subprocess
import tempfile


def literal(text):
    return "CONVERT(X'" + text.encode("utf-8").hex() + "' USING utf8mb4)"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysql-client", default="mysql")
    parser.add_argument("--port", type=int, default=2882)
    parser.add_argument("--user", default="root")
    parser.add_argument("--socket", help="Local server socket, required to configure secure_file_priv")
    args = parser.parse_args()
    command = [args.mysql_client, "--protocol=tcp", "--host=127.0.0.1",
               "--port=" + str(args.port), "--user=" + args.user,
               "--default-character-set=utf8mb4", "--batch", "--raw", "--skip-column-names", "test"]
    if args.socket:
        command = [args.mysql_client, "--protocol=socket", "--socket=" + args.socket,
                   "--user=" + args.user, "--default-character-set=utf8mb4", "--batch", "--raw",
                   "--skip-column-names", "test"]

    def sql(statement, *, enabled=True, error=False):
        prefix = "SET SESSION enable_file_sql=ON;" if enabled else ""
        result = subprocess.run(command, input=prefix + statement, text=True,
                                capture_output=True, timeout=90)
        if error:
            assert result.returncode != 0, "Invalid file SQL unexpectedly succeeded: " + statement
            return result.stderr
        elif result.returncode:
            raise RuntimeError(result.stderr.strip() + "\nSQL: " + statement)
        return result.stdout.rstrip("\n")

    original = sql("SELECT HEX(@@global.secure_file_priv);", enabled=False)
    restore = "NULL" if original == "NULL" else "CONVERT(X'" + original + "' USING utf8mb4)"
    with tempfile.TemporaryDirectory(prefix="seekdb-native-file-sql-") as directory:
        root = Path(directory)
        data = root / "allowed"
        data.mkdir()
        (data / "basic.csv").write_text(
            'id,name,score,active,day,created,note,id\r\n'
            '1,Alice,98.5,true,2026-08-01,2026-08-01 10:11:12,"hello,\nworld",101\r\n'
            '2,Bob,87,false,,,plain,102\r\n'
            '3,Chen,90.25,true,2026-08-03,2026-08-03 12:13:14,"say ""hi""",103\r\n'
            '4,Dora,80,false,,,last,104\r\n', encoding="utf-8", newline="")
        documents = [{"id": 1, "name": "Alice", "active": True},
                     {"id": 2, "name": "Bob", "day": "2026-08-02"},
                     {"id": 3, "name": "Chen", "note": "ok"}]
        (data / "basic.jsonl").write_text("\n".join(json.dumps(row) for row in documents), encoding="utf-8")
        (data / "no_header.csv").write_text("1;one\n2;NULL\n", encoding="utf-8")
        (data / "bad.csv").write_text("id,name\n1,ok\n2,too,many\n", encoding="utf-8")
        (data / "bad.jsonl").write_text('{"id":1}\n{"nested":[]}\n', encoding="utf-8")
        outside = root / "outside.csv"
        outside.write_text("id\n999\n", encoding="utf-8")
        (data / "link.csv").symlink_to(outside)
        csv = "read_csv(" + literal(str(data / "basic.csv")) + ")"
        js = "read_json(" + literal(str(data / "basic.jsonl")) + ",format='newline_delimited')"
        changed_policy = False
        try:
            assert sql("SELECT @@session.enable_file_sql;", enabled=False) == "0"
            sql(f"SELECT COUNT(*) FROM {csv};", enabled=False, error=True)
            sql("SET GLOBAL secure_file_priv=" + literal(str(data)) + ";", enabled=False)
            changed_policy = True
            assert sql(f"SELECT COUNT(*),SUM(id),SUM(active),MAX(id__2) FROM {csv};") == "4\t10\t2\t104"
            assert sql(f"SELECT name FROM {csv} c WHERE score>85 ORDER BY id DESC LIMIT 2;") == "Chen\nBob"
            assert sql(f"SELECT active,COUNT(*) FROM {csv} GROUP BY active HAVING COUNT(*)>=2 ORDER BY active;") == "0\t2\n1\t2"
            assert sql(f"SELECT REPLACE(note,CHAR(10),'<NL>') FROM {csv} WHERE id=1;") == "hello,<NL>world"
            dates = sql(f"SELECT day,created FROM {csv} WHERE id=1;")
            day, created = dates.split("\t")
            assert date.fromisoformat(day) == date(2026, 8, 1), repr(dates)
            assert datetime.fromisoformat(created) == datetime(2026, 8, 1, 10, 11, 12), repr(dates)
            assert sql(f"SELECT day IS NULL,created IS NULL FROM {csv} WHERE id=2;") == "1\t1"
            assert len(sql(f"SELECT * FROM {csv} LIMIT 1;").split("\t")) == 8
            print("PASS native CSV: types, NULL, quotes, aliases, filters, sort, limit, aggregate, having", flush=True)

            assert sql(f"SELECT id,note IS NULL FROM {js} ORDER BY id;") == "1\t1\n2\t1\n3\t0"
            assert sql(f"SELECT c.id,j.name FROM {csv} c INNER JOIN {js} j ON c.id=j.id ORDER BY c.id;") == "1\tAlice\n2\tBob\n3\tChen"
            assert sql(f"SELECT c.id,j.name FROM {csv} c LEFT JOIN {js} j ON c.id=j.id ORDER BY c.id;") == "1\tAlice\n2\tBob\n3\tChen\n4\tNULL"
            assert sql(f"SELECT COUNT(*) FROM {csv} a JOIN {csv} b ON a.id=b.id;") == "4"
            plan = sql(f"EXPLAIN SELECT c.id FROM {csv} c JOIN {js} j ON c.id=j.id;")
            assert "JSON_TABLE" in plan or "JSON TABLE" in plan, plan
            print("PASS native JSONL union schema, missing NULL, inner/left/cross-format join and plan", flush=True)

            options = "read_csv(" + literal(str(data / "no_header.csv")) + ",header=false,delim=';',nullstr='NULL')"
            assert sql(f"SELECT column1,column2 FROM {options} ORDER BY column1;") == "1\tone\n2\tNULL"
            for expression in [
                "read_csv(" + literal(str(data / "bad.csv")) + ")",
                "read_json(" + literal(str(data / "bad.jsonl")) + ")",
                "read_csv(" + literal(str(outside)) + ")",
                "read_csv(" + literal(str(data / "../outside.csv")) + ")",
                "read_csv(" + literal(str(data / "link.csv")) + ")",
                "read_csv(" + literal(str(data / "basic.csv")) + ",sample_size=10)",
                "read_csv(" + literal(str(data / "basic.csv")) + ",header=true,header=false)",
                "read_csv(" + literal(str(data / "basic.csv")) + ",unknown_option=1)",
            ]:
                sql(f"SELECT COUNT(*) FROM {expression};", error=True)
            assert sql("SELECT 1;") == "1"
            message = sql("SELECT * FROM read_csv(" + literal(str(data / "bad.csv")) + ");", error=True)
            assert "line 3" in message, message
            print("PASS file options, malformed records, sampling rejection, path boundary and symlink guard", flush=True)

            dynamic = data / "dynamic.csv"
            query = "SELECT value FROM read_csv(" + literal(str(dynamic)) + ");"
            dynamic.write_text("value\n7\n", encoding="utf-8")
            assert sql(query) == "7"
            dynamic.write_text("value\nchanged-type\n", encoding="utf-8")
            assert sql(query) == "changed-type"
            print("PASS dynamic schema does not reuse a stale cached projection", flush=True)
        finally:
            if changed_policy:
                sql("SET GLOBAL secure_file_priv=" + restore + ";", enabled=False)


if __name__ == "__main__":
    main()
