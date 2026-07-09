# GrhSIM/GSim Supernode Statistics

Date: 2026-07-08

This is the only supported GrhSIM/GSim supernode statistics schema. All previous static and runtime statistics definitions are deprecated in `grhsim-gsim-statistics-deprecated.md`.

## Files

The outputs are JSON only:

- `gsim_static_stats.json`
- `grhsim_static_stats.json`
- `gsim_runtime_stats.json`
- `grhsim_runtime_stats.json`

No TSV output is part of this schema.

Rows in `supernodes` are joinable by:

```text
sim, top, supernode_id
```

Each file also contains a `summary` object. Summary values are aggregate values only and are not part of the join key.

## Static Stats

Static stats are generated at emit time and are always enabled.

Common fields:

- `format`: `wolvrix.sim-supernode-static-stats.v1`
- `sim`: `gsim` or `grhsim`
- `top`: emitted top name
- `summary`: aggregate counters
- `supernodes`: per-supernode rows

GSim static rows:

- `supernode_id`: GSim emitted supernode id.
- `kind`: `supernode`.
- `activation_edges.total`: number of unique outgoing directed activation edges from this supernode.
- `activation_edges.self`: outgoing activation edges whose target is the same supernode.
- `activation_checks`: number of unique member nodes in this supernode whose value is checked to update runtime active flags.

GrhSIM static rows:

- `supernode_id`: GrhSIM activity-schedule supernode id.
- `kind`: `compute` or `commit`.
- `activation_edges.compute_compute`
- `activation_edges.compute_commit`
- `activation_edges.commit_compute`
- `activation_edges.commit_commit`
- `activation_edges.total`
- `activation_edges.self`
- `activation_checks`: number of unique detected values for this supernode. Commit state-write detection points are counted once per write operation that can activate at least one reader supernode.

Activation edges are outgoing edges only. A directed edge `s1 -> s2` exists when `s1` can set or otherwise trigger the runtime active flag for `s2`. Edges are unique; repeated sources for the same `(s1, s2)` pair do not add more edges. Topological dependencies that do not trigger runtime active flags are not counted.

Activation checks measure detection cost. A detected node or value is counted once even when it can activate multiple target supernodes; multi-target fanout is represented by `activation_edges`.

## Runtime Stats

Runtime stats are compiled into the generated model only when explicitly enabled at emit time.

Enable switches:

- GSim generator: `GSIM_EMIT_RUNTIME_STATS=1`
- Top-level XiangShan GSim make flow: `XS_GSIM_EMIT_RUNTIME_STATS=1`
- GrhSIM emit attribute: `emit_runtime_stats=true`
- Top-level XiangShan GrhSIM make flow: `XS_WOLF_GRHSIM_EMIT_RUNTIME_STATS=1`
- xs-components GrhSIM make flow: `GRHSIM_EMIT_RUNTIME_STATS=1`

Runtime file fields:

- `format`: `wolvrix.sim-supernode-runtime-stats.v1`
- `sim`: `gsim` or `grhsim`
- `top`: emitted top name
- `summary.activation_count`: total actual supernode executions.
- `supernodes[].activation_count`: actual execution count for that supernode.

Only actual supernode execution is counted. Attempted activation, already-set active flags, and static reachability do not increment runtime `activation_count`.
