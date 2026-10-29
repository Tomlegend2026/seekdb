"""Verify production catalog SQL against a disposable local seekdb instance.

This verifies storage/history/rollback only, not AI COLUMN DDL acceptance.
Every catalog mutation is rolled back, and an existing fixture ID is refused.
"""
import argparse
import os
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="ai_column_schema_regression executable")
    parser.add_argument("--socket", required=True, help="disposable instance socket")
    parser.add_argument("--mysql-client", default="mysql")
    args = parser.parse_args()
    environment = dict(os.environ, AI_CATALOG_SQL_FIXTURE="1")
    result = subprocess.run(
        [args.binary, "--gtest_filter=AiColumnCatalog.WritesUseCallerClientAndPropagateFailure:"
         "AiColumnCatalog.FailedHistoricalReadKeepsSchema"],
        env=environment, text=True, capture_output=True, check=True,
    )
    fixtures = {}
    for line in result.stdout.splitlines():
        if line.startswith("AI_CATALOG_"):
            key, sql = line.split(" ", 1)
            fixtures.setdefault(key, []).append(sql)
    assert len(fixtures.get("AI_CATALOG_INSERT_SQL", [])) == 2, result.stdout
    assert len(fixtures.get("AI_CATALOG_DELETE_SQL", [])) == 3, result.stdout
    assert len(fixtures.get("AI_CATALOG_READ_SQL", [])) == 1, result.stdout
    command = [args.mysql_client, "--protocol=socket", "--socket=" + args.socket,
               "--user=root", "--database=oceanbase", "--batch", "--raw", "--skip-column-names"]
    tables = ("__all_ai_column", "__all_ai_column_history", "__all_ai_column_task")
    preflight = subprocess.run(command, text=True, capture_output=True, timeout=30,
        input=";\n".join(f"SELECT COUNT(*) FROM {table} WHERE table_id=2001" for table in tables) + ";")
    assert preflight.returncode == 0, preflight.stderr
    assert preflight.stdout.splitlines() == ["0", "0", "0"], "fixture ID already exists"
    read_sql = fixtures["AI_CATALOG_READ_SQL"][0]

    def count_snapshot(query, tag):
        return f"SELECT CONCAT('{tag}:',COUNT(*)) FROM ({query}) snapshot_rows"

    statements = ["START TRANSACTION", *fixtures["AI_CATALOG_INSERT_SQL"],
        "SELECT CONCAT('CURRENT:',column_name,':',max_retries,':',sync_mode) "
        "FROM __all_ai_column WHERE table_id=2001",
        count_snapshot(read_sql, "OLD"), *fixtures["AI_CATALOG_DELETE_SQL"],
        "SELECT CONCAT('DELETED:',COUNT(*)) FROM __all_ai_column WHERE table_id=2001",
        count_snapshot(read_sql, "OLD_AFTER_DELETE"),
        count_snapshot(read_sql.replace("schema_version<=7", "schema_version<=9"), "NEW"),
        "SELECT CONCAT('HISTORY:',schema_version,':',is_deleted) FROM __all_ai_column_history "
        "WHERE table_id=2001 ORDER BY schema_version", "ROLLBACK", "START TRANSACTION",
        fixtures["AI_CATALOG_INSERT_SQL"][0], fixtures["AI_CATALOG_INSERT_SQL"][0], "ROLLBACK",
        "SELECT CONCAT('ROLLED_BACK_CURRENT:',COUNT(*)) FROM __all_ai_column WHERE table_id=2001",
        "SELECT CONCAT('ROLLED_BACK_HISTORY:',COUNT(*)) FROM __all_ai_column_history WHERE table_id=2001"]
    # --force continues after the deliberately duplicated INSERT so ROLLBACK still executes.
    result = subprocess.run(command + ["--force"], input=";\n".join(statements) + ";",
                            text=True, capture_output=True, timeout=60)
    assert re.findall(r"ERROR (\d+)", result.stderr) == ["1062"], result.stderr
    assert result.stdout.splitlines() == ["CURRENT:tags:3:async", "OLD:1", "DELETED:0",
        "OLD_AFTER_DELETE:1", "NEW:0", "HISTORY:7:0", "HISTORY:9:1",
        "ROLLED_BACK_CURRENT:0", "ROLLED_BACK_HISTORY:0"], result.stdout
    print("PASS: production catalog SQL, history, tombstones and transaction rollback")


if __name__ == "__main__":
    main()
