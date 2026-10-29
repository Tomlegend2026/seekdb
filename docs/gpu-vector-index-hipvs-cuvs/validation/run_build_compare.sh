#!/usr/bin/env bash
# Measure the offline-DDL build path for the two seekdb vector backends.
# The input TSV must contain "<id>\t<vector-literal>" rows, one per line.
#
# Usage:
#   MYSQL='mysql --local-infile=1 -h127.0.0.1 -P2881 -uroot -ANB' \
#     docs/gpu-vector-index-hipvs-cuvs/validation/run_build_compare.sh \
#       /data/sift250k.tsv 128 3
#
# The GPU build number is the one that matters for the contest.  The VSAG
# number is a CPU reference only and must not be compared with the V5 baseline.

set -Eeuo pipefail

BASE_TSV=${1:?input TSV required}
DIM=${2:?vector dimension required}
REPEAT=${3:-3}
DATABASE=${DATABASE:-seekdb_build_compare}
MYSQL=${MYSQL:-mysql --local-infile=1 -h127.0.0.1 -P2881 -uroot -ANB}
[[ "$DIM" =~ ^[0-9]+$ ]] || exit 2
[[ "$REPEAT" =~ ^[0-9]+$ ]] || exit 2
[[ -r "$BASE_TSV" ]] || { echo "cannot read $BASE_TSV" >&2; exit 2; }

probe=$(head -n 1 "$BASE_TSV" | cut -f2)
[[ -n "$probe" ]] || { echo "empty probe vector" >&2; exit 2; }

sql()
{
  # shellcheck disable=SC2086
  $MYSQL -e "set ob_query_timeout=300000000; $1"
}

load_table()
{
  sql "create database if not exists $DATABASE; use $DATABASE;
       drop table if exists items;
       create table items(id bigint primary key, v vector($DIM));
       load data local infile '$BASE_TSV' into table items
       fields terminated by '\\t' lines terminated by '\\n' (id,v);"
}

measure_backend()
{
  local backend=$1
  local run=$2
  local ef=64
  if [[ "$backend" == "vsag" ]]; then
    ef=200
  fi
  load_table
  local start end build_ns query_start query_end query_ns
  start=$(date +%s%N)
  sql "use $DATABASE; create vector index idx on items(v)
       with (distance=l2,type=hnsw,lib=$backend,m=16,ef_construction=200,ef_search=$ef);"
  end=$(date +%s%N)
  query_start=$(date +%s%N)
  sql "select id from $DATABASE.items
       order by l2_distance(v,$probe) approximate limit 10" >/dev/null
  query_end=$(date +%s%N)
  build_ns=$((end - start))
  query_ns=$((query_end - query_start))
  printf '%s\t%s\t%s\t%s\n' "$backend" "$run" \
    "$(awk -v ns="$build_ns" 'BEGIN { printf "%.3f", ns/1e9 }')" \
    "$(awk -v ns="$query_ns" 'BEGIN { printf "%.3f", ns/1e9 }')"
}

printf 'backend\trun\tbuild_s\tfirst_query_s\n'
for run in $(seq 1 "$REPEAT"); do
  measure_backend cuvs "$run"
  measure_backend vsag "$run"
done
