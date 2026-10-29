# FTS performance optimization

This change targets the hot setup path shared by FTS index build, `TOKENIZE()`
and `MATCH` query analysis.

## Bottleneck

Every call to `ObIKFTParser::init_dict()` loaded the three IK range
dictionaries from `ObFTDictHub`, allocated a fresh `ObFTRangeDict` for each,
and rebuilt its range list.  For the contest workload this happens once per
indexed document (and once per tokenize/query call), even though the
dictionaries are immutable for the lifetime of the process.

A second avoidable cost sits in `ObAddWord::casedown_word()`:
`ObCharset::tolower()` always copies the token, including the Chinese tokens
that make up most of the benchmark text and have no case at all.

## Changes

1. `ObFTParseData` now owns a lazily initialized, process-wide IK dictionary
   set (`main`, `quantifier`, `stopword`).  `ObIKFTParser` borrows those
   read-only dictionaries instead of rebuilding them for every document.
   The fallback path is retained for cases where the global cache cannot be
   initialized.
2. `ObAddWord::casedown_word()` accepts the original token pointer when the
   token is ASCII without uppercase letters, or consists only of uncased
   CJK/Hangul/Kana code points.  Other scripts still use
   `ObCharset::tolower()`, preserving the existing case-folding semantics.
3. The parser's shared dictionaries are owned by `ObFTParseData`, so parser
   destruction only releases the per-parser context/processors.

## Expected effect

- `build_ik_all_sec` and `build_ik_content_sec`: dictionary setup is removed
  from the per-row path.
- `tokenize_ik_avg_ms`: same dictionary setup removal, plus fewer token
  allocations for Chinese text.
- `query_*`: query analysis uses the same cached dictionaries; the scan and
  merge path is unchanged.

## Reproduction

Use the repository's fixed benchmark on a dedicated instance:

```bash
LABEL=after OUTPUT=/tmp/fts-after.txt \
  ./tools/benchmark/fts_large_bench.sh
```

Compare these fields against `tools/benchmark/fts_large_bench_baseline.json`:

```text
build_ik_all_sec, build_ik_content_sec, build_beng_en_sec
tokenize_ik_avg_ms, tokenize_beng_avg_ms
query_cn_avg_ms, query_beng_avg_ms, query_mixed_avg_ms, query_limit_avg_ms
```

Run the existing `fts_index` mysqltest suite through the normal seekdb test
driver before submitting.

No benchmark data, SQL, timing logic, tokenizer rules or scoring inputs were
modified.
