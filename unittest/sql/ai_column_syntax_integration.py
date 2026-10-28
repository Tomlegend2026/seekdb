#!/usr/bin/env python3
"""Verify AI COLUMN syntax reaches the explicit, fail-closed DDL guard.

This is a parser/resolver milestone test, not AI column acceptance. The guard
must reject unsupported execution without creating tables or ordinary columns.
"""
import argparse
import re
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--mysql-client", default="mysql")
    args = parser.parse_args()
    command = [args.mysql_client, "--protocol=socket", "--socket=" + args.socket,
               "--user=root", "--batch", "--raw", "--skip-column-names"]
    database = "ai_syntax_" + uuid.uuid4().hex[:12]

    def sql(statement, error=None):
        result = subprocess.run(command, input=statement, text=True,
                                capture_output=True, timeout=45)
        if error is None:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0, "Unexpected success: " + statement
            assert re.search(r"ERROR (" + error + r")\b", result.stderr), result.stderr
        return result

    sql(f"CREATE DATABASE {database};")
    try:
        sql(f"USE {database}; CREATE TABLE base(id INT PRIMARY KEY, content VARCHAR(20));")
        definitions = [
            "a int AI COLUMN (1)",
            "a json AI COLUMN (1)",
            "a varchar(10) AI COLUMN ('x')",
            "content varchar(20), tags json AI COLUMN (AI_COMPLETE('mock',content))",
            "content varchar(20), emb vector(4) AI COLUMN (AI_EMBED('mock',content)) "
            "WITH (sync_mode='async',max_retries=2)",
            "content varchar(20), tags json AI COLUMN (AI_COMPLETE('mock',content)) "
            "WITH (max_retries=0,sync_mode='async')",
        ]
        for definition in definitions:
            result = sql(f"USE {database}; CREATE TABLE candidate({definition});", "1235")
            assert "AI COLUMN execution (syntax recognized)" in result.stderr, result.stderr
        result = sql(f"USE {database}; ALTER TABLE base ADD COLUMN tags json "
                     "AI COLUMN (AI_COMPLETE('mock',content)) "
                     "WITH (sync_mode='async',max_retries=2);", "1235")
        assert "syntax recognized" in result.stderr, result.stderr
        for definition in ["a int AI COLUMN ()", "a int AI COLUMN (1) WITH ()",
                           "a int AI COLUMN (1) WITH (max_retries=)"]:
            sql(f"USE {database}; CREATE TABLE candidate({definition});", "1064|1149")
        assert sql(f"SHOW TABLES FROM {database};").stdout.strip() == "base"
        assert sql(f"SELECT COUNT(*) FROM information_schema.columns WHERE "
                   f"table_schema='{database}' AND table_name='base';").stdout.strip() == "2"
        print("PASS 6 CREATE and 1 ALTER syntaxes reach the DDL guard; "
              "3 malformed forms rejected; no partial table/column created")
    finally:
        sql(f"DROP DATABASE {database};")


if __name__ == "__main__":
    main()
