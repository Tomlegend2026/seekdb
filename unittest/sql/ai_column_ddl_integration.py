#!/usr/bin/env python3
"""Run the shipped DDL acceptance SQL with actual error/result assertions.

Uses a dedicated disposable seekdb instance; no external model is contacted.
"""
import argparse
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    fixture = root / 'tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_ddl.test'
    commands = ['mysql', '--protocol=tcp', '--host=127.0.0.1', f'--port={args.port}',
                '--user=root', '--batch', '--raw', '--skip-column-names']
    pending, expected, number = [], None, 0
    selections = [
        '1', 'emb\t3\ntags\t2', '1', '1', '1', '1',
    ]
    select_count = 0
    for line in fixture.read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith('--error '):
            expected = {int(code) for code in stripped[8:].split(',')}
        elif not stripped or stripped.startswith(('#', '--')) or stripped == 'connection default;':
            continue
        else:
            pending.append(line)
            if stripped.endswith(';'):
                query = '\n'.join(pending)
                pending.clear()
                number += 1
                result = subprocess.run(commands, input=query, capture_output=True, text=True, timeout=60)
                match = re.search(r'ERROR (\d+)\b', result.stderr)
                code = int(match.group(1)) if match else 0
                if expected is not None:
                    assert code in expected, f'Statement {number}: {query}\n{result.stderr}'
                else:
                    assert result.returncode == 0 and not match, f'Statement {number}: {query}\n{result.stderr}'
                if stripped.lower().startswith('select ') or query.lstrip().lower().startswith('select '):
                    actual = result.stdout.strip()
                    assert actual == selections[select_count], (query, actual, selections[select_count])
                    select_count += 1
                print(f'PASS DDL statement {number}: {query.splitlines()[0]}', flush=True)
                expected = None
    assert not pending and select_count == len(selections)
    print(f'PASS: {number} shipped DDL statements, {select_count} asserted metadata queries')


if __name__ == '__main__':
    main()
