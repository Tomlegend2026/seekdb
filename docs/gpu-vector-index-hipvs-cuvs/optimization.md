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
reproduce the frozen baseline's refinement schedule with the same binary.

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

It prints `backend`, `run`, `build_s` and `first_query_s`.  Run it once on the
frozen `634888d9b` binary and once on the candidate binary; the ratio of the
median `cuvs` build numbers is the build speedup used by the contest.

## Correctness invariants

- `graph_degree`, `intermediate_graph_degree`, metric, FP32 input and the
  public SQL contract are unchanged.
- Search parameters are unchanged.
- `lib=vsag` code paths are untouched.
- v3 snapshots are self-contained: graph + ids.  The host vector buffer is
  reconstructed from the graph's attached dataset on load.
- v2 snapshots are accepted with their original payload layout.
