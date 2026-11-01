# Build-path optimization (contest V5 baseline)

This note documents the two build-path changes on top of the V5 baseline
(`634888d9b`) and how to reproduce the comparison on a Radeon host.

## 1. Shorten the NN-Descent refinement schedule

The V5 bridge copied the hipVS large-dataset test configuration verbatim:

```text
graph_degree              = 128
intermediate_graph_degree = 256
nn_descent_niter          = 100
```

The degree configuration is what keeps 256-dimensional recall above the
contest gate, so it is left unchanged.  The 100-iteration schedule, however,
is a stress-test schedule: for a 250K-point graph the NN-Descent neighbour
lists have effectively converged well before that, and every extra iteration
is a full sweep over the intermediate graph.  The optimized bridge keeps the
same graph shape and uses:

```text
dim <= 128 : nn_descent_niter = 40
dim >  128 : nn_descent_niter = 64
```

The 256-dimensional path intentionally keeps the longer schedule because its
Recall@100 is the tighter gate.  No search parameters are changed, so query
latency/throughput behaviour is unchanged.

For local tuning the bridge accepts `SEEKDB_CUVS_NITER`; set it to `100` to
reproduce the frozen baseline's refinement schedule with the same binary.  The
graph shape is also overridable so a Radeon host can sweep recall against
build time without rebuilding anything:

| Variable | Default | Range | Meaning |
| --- | --- | --- | --- |
| `SEEKDB_CUVS_NITER` | `40` (`dim<=128`), `64` (`dim>128`) | 1..1000 | NN-Descent iterations |
| `SEEKDB_CUVS_GRAPH_DEGREE` | `128` | 16..512 | Final CAGRA graph degree |
| `SEEKDB_CUVS_INTERMEDIATE_DEGREE` | `256` | >= graph degree, <=1024 | Intermediate kNN graph degree |

Invalid values are logged and ignored; the defaults above are the values used
for the scored build.  Reducing the degrees is a much riskier lever than
reducing the iteration count: the frozen baseline's 256-dimensional
Recall@100 is 0.914, and the relative gate allows only a 0.005 drop (0.909),
so a small graph-quality regression can fail the dataset even though the
absolute 0.85 floor still looks comfortable.  Sweep the iteration count first
and keep the degree defaults unless a candidate clearly holds both datasets
inside the gates.

## 2. Remove the duplicated dataset from the GPU snapshot

V5 serialized the CAGRA graph with `include_dataset=true` and then appended a
second copy of every FP32 vector to the seekdb snapshot stream.  At 250K x
256 that is a duplicate 256 MiB write (128 MiB for SIFT) on every offline DDL.

The new `OBCUVS03` snapshot stores only the graph blob and the id mapping.  The
graph blob already contains the dataset because cuVS is called with
`include_dataset=true`.  On reload, `seekdb_cuvs_copy_dataset` copies the
dataset view owned by the CAGRA index back into seekdb's update/fallback
buffer.  `OBCUVS02` snapshots remain readable, so an already-created baseline
index can still be reopened.

## Reproduction

Build the bridge and the server with the contest commands:

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so \
  --make -j64
```

The bridge now prints one line per GPU build, for example:

```text
[seekdb_cuvs] build n=250000 dim=128 degree=128 intermediate=256 niter=40 <elapsed>s
```

Run the existing lifecycle smoke test to verify GPU build, snapshot reload,
and CPU fallback isolation:

```bash
LD_LIBRARY_PATH=/work/bridge:/opt/hipvs/lib:/opt/rocm/lib \
  docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh \
    --observer /path/to/release/seekdb \
    --bridge /work/bridge/libseekdb_cuvs_bridge.so \
    --base-dir /tmp/seekdb-cuvs-smoke \
    --port 2981 \
    --render /dev/dri/renderD135 \
    --evidence /tmp/seekdb-cuvs-evidence
```

For the scored 250K comparison, run the frozen baseline and this branch on the
same host with the contest SQL.  Measure from immediately before
`CREATE VECTOR INDEX ... lib=cuvs` until the first approximate query returns,
and report `baseline_seconds / candidate_seconds`.  Use the same sequence with
`lib=vsag` only as a diagnostic CPU reference; the official score compares the
candidate against the frozen V5 `lib=cuvs` build, not against VSAG.

The repository includes a small runner for that measurement:

```bash
MYSQL='mysql --local-infile=1 -h127.0.0.1 -P2881 -uroot -ANB' \
  docs/gpu-vector-index-hipvs-cuvs/validation/run_build_compare.sh \
    /data/sift250k.tsv 128 3
```

It prints `backend`, `run`, `create_s`, `first_query_s` and `total_s`.
`total_s` spans `CREATE VECTOR INDEX` through the first returned approximate
query, which is the contest interval; use the median of that column for the
paired baseline/candidate ratio.  Run it once on the frozen `634888d9b` binary
and once on the candidate binary.

## Expected effect and what to report

Only the iteration cut changes the amount of GPU graph work; it does not change
the graph shape, the search parameters, or the SQL contract.  The build cost is
dominated by full NN-Descent sweeps, so the iteration ratio is the first-order
estimate of the GPU build-time ratio:

| Dataset | Baseline iterations | Candidate iterations | Iteration ratio |
| --- | ---: | ---: | ---: |
| SIFT250K (128-d, 40% weight) | 100 | 40 | 2.50x |
| Hidden250K (256-d, 60% weight) | 100 | 64 | 1.56x |

End-to-end build time also contains the DDL scan, the graph prune/sort, and the
snapshot write, so the measured acceleration is lower than these ratios.  The
snapshot change removes a 128-256 MiB duplicate write from every offline DDL.

For the scored comparison report, per dataset and per run:

1. `build_seconds` - wall clock from `CREATE VECTOR INDEX ... lib=cuvs` until
   the first `APPROXIMATE` query returns, using the contest SQL and a cleared
   index/cache.
2. `recall@10`, `recall@100` - against exact L2 ground truth for the same
   query set.
3. `qps_top10`, `qps_top100` - `nq=4096` single-row approximate queries.
4. The bridge's `[seekdb_cuvs] build ...` line, which separates GPU build time
   from the surrounding DDL work.

Run the frozen baseline and the candidate on the same host and in the same
session, alternate their order, and report the paired ratios.  Do not publish
a speedup measured against the `lib=vsag` CPU path: the contest compares
against the frozen V5 `lib=cuvs` build.

## Correctness invariants

- `graph_degree` (128), `intermediate_graph_degree` (256), metric, FP32 input
  and the public SQL contract are unchanged at their defaults; the environment
  overrides exist only for local experiments and are unset during scoring.
- Search parameters are unchanged.
- `lib=vsag` code paths are untouched.
- v3 snapshots are self-contained: graph + ids.  The host vector buffer is
  reconstructed from the graph's attached dataset on load.
- v2 snapshots are accepted with their original payload layout.
