#!/usr/bin/env python3
"""Exercise native FileSQL on a dedicated local seekdb test instance.

The server and this script must share the local filesystem. The script temporarily
restricts secure_file_priv to its own fixture directory and restores it on exit.
"""
import argparse
import csv as csvlib
from datetime import date, datetime
import json
from pathlib import Path
import subprocess
import tempfile
import uuid


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

    def sql(statement, *, enabled=True, error=False, as_user=None, database="test"):
        prefix = "SET SESSION enable_file_sql=ON;" if enabled else ""
        client = ["--user=" + as_user if arg.startswith("--user=") and as_user else arg
                  for arg in command]
        if database is None:
            # Connect without a default schema: the temporary account must not
            # inherit any privilege on the schema it is tested against.
            client = client[:-1]
        result = subprocess.run(client, input=prefix + statement, text=True,
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

            expected = "\n".join(
                f"{name}\t{kind}\t{nullable}\tNULL\tNULL\tNULL"
                for name, kind, nullable in [
                    ("id", "BIGINT", "NO"), ("name", "VARCHAR", "NO"),
                    ("score", "DOUBLE", "NO"), ("active", "BOOLEAN", "NO"),
                    ("day", "DATE", "YES"), ("created", "DATETIME", "YES"),
                    ("note", "VARCHAR", "NO"), ("id__2", "BIGINT", "NO")])
            assert sql(f"DESCRIBE SELECT * FROM {csv};") == expected
            assert sql(f"DESC SELECT active AS `a'b`,id FROM {csv} WHERE 1=0;") == (
                "a'b\tBOOLEAN\tNO\tNULL\tNULL\tNULL\nid\tBIGINT\tNO\tNULL\tNULL\tNULL")
            assert sql(f"DESCRIBE SELECT id,active FROM {js};") == (
                "id\tBIGINT\tNO\tNULL\tNULL\tNULL\nactive\tBOOLEAN\tYES\tNULL\tNULL\tNULL")
            assert sql(f"DESC SELECT j.id FROM {csv} c LEFT JOIN {js} j ON c.id=j.id;") == (
                "id\tBIGINT\tYES\tNULL\tNULL\tNULL")
            # Derived tables and CTEs keep the file type name and nullability.
            assert sql(f"DESCRIBE SELECT * FROM (SELECT id,active FROM {csv}) t;") == (
                "id\tBIGINT\tNO\tNULL\tNULL\tNULL\nactive\tBOOLEAN\tNO\tNULL\tNULL\tNULL")
            assert sql(f"DESC SELECT t.id FROM (SELECT id FROM {csv}) t;") == (
                "id\tBIGINT\tNO\tNULL\tNULL\tNULL")
            assert sql(f"DESCRIBE WITH cte AS (SELECT id,active FROM {csv}) SELECT * FROM cte;") == (
                "id\tBIGINT\tNO\tNULL\tNULL\tNULL\nactive\tBOOLEAN\tNO\tNULL\tNULL\tNULL")
            assert sql(f"DESC SELECT c.id FROM (WITH cte AS (SELECT id FROM {csv}) "
                       f"SELECT * FROM cte) c;") == ("id\tBIGINT\tNO\tNULL\tNULL\tNULL")
            assert "column_name" not in sql("DESC SELECT 1;")
            # A query that also reads a protected table keeps the normal EXPLAIN
            # output, so the executor still checks the original query privileges.
            sql("CREATE TABLE file_desc_base(id BIGINT);")
            try:
                plan = sql(f"DESC SELECT f.id FROM {csv} f JOIN file_desc_base t ON f.id=t.id;")
                assert "JSON_TABLE" in plan and "column_name" not in plan, plan
            finally:
                sql("DROP TABLE file_desc_base;")
            identifier = "file_desc_" + uuid.uuid4().hex[:12]
            account = "'" + identifier + "'@'%'"
            created_user = False
            created_table = False
            try:
                sql(f"CREATE USER {account};")
                created_user = True
                sql(f"GRANT FILE ON *.* TO {account};")
                assert sql("SELECT 1;", as_user=identifier, database=None) == "1"
                sql(f"CREATE TABLE {identifier}(id BIGINT);")
                created_table = True
                sql(f"DESC SELECT f.id FROM {csv} f JOIN test.{identifier} t ON f.id=t.id;",
                    as_user=identifier, database=None, error=True)
                sql(f"GRANT SELECT ON test.{identifier} TO {account};")
                assert "JSON_TABLE" in sql(
                    f"DESC SELECT f.id FROM {csv} f JOIN test.{identifier} t ON f.id=t.id;",
                    as_user=identifier, database=None)
                sql(f"REVOKE FILE ON *.* FROM {account};")
                sql(f"DESC SELECT * FROM {csv};", as_user=identifier, database=None, error=True)
            finally:
                if created_table:
                    sql(f"DROP TABLE {identifier};")
                if created_user:
                    sql(f"DROP USER {account};")
            print("PASS DESCRIBE: inferred types, NULL, projection, aliases, outer joins, "
                  "derived tables, CTEs, empty results and privileges", flush=True)

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

            listing = "glob(" + literal(str(data / "*.csv")) + ")"
            sql(f"SELECT COUNT(*) FROM {listing};", enabled=False, error=True)
            assert sql(f"SELECT SUBSTRING_INDEX(file,'/',-1) FROM {listing} ORDER BY file;") == "bad.csv\nbasic.csv\ndynamic.csv\nno_header.csv"
            assert sql("SELECT COUNT(*) FROM glob('*.csv');") == "4"
            assert sql(f"SELECT COUNT(*) FROM {listing} g WHERE g.file LIKE '%/basic.csv';") == "1"
            assert sql("SELECT COUNT(*) FROM glob(" + literal(str(data / "*.absent")) + ");") == "0"
            sql("SELECT * FROM glob(" + literal(str(outside)) + ");", error=True)
            print("PASS native glob: sorted regular files, wildcard, empty matches, path boundary", flush=True)

            # COPY exports a query or a table to a local CSV file.
            def target(path):
                # the export target must be a plain string literal
                return "'" + str(path).replace("\\", "\\\\").replace("'", "''") + "'"

            export = data / "export.csv"
            sql(f"COPY (SELECT id,name,score,note FROM {csv} ORDER BY id) "
                f"TO {target(export)} (format csv, header);")
            assert export.exists()
            lines = export.read_text(encoding="utf-8").splitlines()
            assert lines[0] == "id,name,score,note", lines[:1]
            assert lines[1] == '1,"Alice",98.5,"hello,', lines[:2]
            special = data / "special-header.csv"
            names = ["a,b", 'a"b', "换\n行"]
            columns = ",".join(f"{i + 1} AS `{name}`" for i, name in enumerate(names))
            sql(f"COPY (SELECT {columns}) TO {target(special)} (format csv, header);")
            with special.open(encoding="utf-8", newline="") as handle:
                assert list(csvlib.reader(handle)) == [names, ["1", "2", "3"]]
            assert sql(f"SELECT * FROM read_csv({literal(str(special))});") == "1\t2\t3"
            empty = data / "empty-export.csv"
            sql(f"COPY (SELECT 1 AS id WHERE FALSE) TO {target(empty)} (header);")
            assert empty.read_text(encoding="utf-8") == "id\n"
            assert sql(f"SELECT id,name,score,REPLACE(note,CHAR(10),'<NL>') "
                       f"FROM read_csv({literal(str(export))}) ORDER BY id;") == (
                "1\tAlice\t98.5\thello,<NL>world\n"
                "2\tBob\t87\tplain\n"
                "3\tChen\t90.25\tsay \"hi\"\n"
                "4\tDora\t80\tlast")
            # an existing file is never overwritten
            sql(f"COPY (SELECT id FROM {csv}) TO {target(export)} (format csv, header);", error=True)
            assert export.read_text(encoding="utf-8").splitlines()[0] == "id,name,score,note"
            # This task requires no overwrite, even if an overwrite option is requested.
            original_export = export.read_bytes()
            sql(f"COPY (SELECT id FROM {csv}) TO {target(export)} (overwrite true);", error=True)
            assert export.read_bytes() == original_export
            # the null text is written as configured
            nulls = data / "nulls.csv"
            sql(f"COPY (SELECT id,day FROM {csv} ORDER BY id) TO {target(nulls)} "
                f"(format csv, header, nullstr 'NULL');")
            assert nulls.read_text(encoding="utf-8").splitlines() == [
                "id,day", '1,"2026-08-01"', "2,NULL", '3,"2026-08-03"', "4,NULL"]
            values = data / "null-and-empty.csv"
            sql(f"COPY (SELECT NULL AS n,'' AS e,'NULL' AS s) TO {target(values)} "
                "(header, nullstr 'NULL');")
            assert sql(f"SELECT n IS NULL,e='',s='NULL' FROM "
                       f"read_csv({literal(str(values))},nullstr='NULL');") == "1\t1\t1"
            custom = data / "custom.csv"
            sql(f"COPY (SELECT 'a;b' AS `x;y`,'q!z' AS z) TO {target(custom)} "
                "(header, delimiter ';', quote '|', escape '!');")
            assert sql(f"SELECT * FROM read_csv({literal(str(custom))},"
                       "delim=';',quote='|',escape='!');") == "a;b\tq!z"
            # a table can be exported as well
            sql("CREATE TABLE file_copy_base(id BIGINT, name VARCHAR(32));")
            try:
                sql("INSERT INTO file_copy_base VALUES (1,'a'),(2,NULL);")
                table_export = data / "table.csv"
                sql(f"COPY file_copy_base TO {target(table_export)} "
                    f"(format csv, header, nullstr 'NULL');")
                assert table_export.read_text(encoding="utf-8").splitlines() == [
                    "id,name", '1,"a"', "2,NULL"]
                failed = data / "failed-export.csv"
                sql(f"COPY (SELECT JSON_EXTRACT(IF(id=2,'invalid','{{}}'),'$') "
                    f"FROM file_copy_base) TO {target(failed)} (header);", error=True)
                assert not failed.exists(), "Failed COPY left a partial export"
            finally:
                sql("DROP TABLE file_copy_base;")
            # the export is part of the file SQL feature
            sql(f"COPY (SELECT id FROM {csv}) TO {target(data / 'off.csv')} (format csv);",
                enabled=False, error=True)
            for options in ["format json", "unknown_option 1", "delimiter ';;'",
                            "delimiter ',' , quote ','", "nullstr 'a,b'", "or_replace"]:
                sql(f"COPY (SELECT id FROM {csv}) TO {target(data / 'bad.csv')} ({options});", error=True)
            sql(f"COPY (SELECT 1) TO {target(outside)};", error=True)
            assert outside.read_text(encoding="utf-8") == "id\n999\n"
            sql("SET GLOBAL secure_file_priv='';", enabled=False)
            try:
                sql(f"COPY (SELECT 1) TO {target(data / 'empty-policy.csv')};", error=True)
                assert not (data / "empty-policy.csv").exists()
            finally:
                sql("SET GLOBAL secure_file_priv=" + literal(str(data)) + ";", enabled=False)
            print("PASS native COPY: query and table export, header, null text, "
                  "no overwrite, option checks", flush=True)
        finally:
            if changed_policy:
                sql("SET GLOBAL secure_file_priv=" + restore + ";", enabled=False)


if __name__ == "__main__":
    main()
