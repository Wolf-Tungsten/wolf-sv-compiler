# GrhSIM/GSim Statistics Definitions Deprecated

Date: 2026-07-08

All previous GrhSIM/GSim static and runtime statistics definitions are obsolete. Do not use them for comparison, regression gates, cost-model training, or performance claims.

The replacement JSON-only schema is documented in [grhsim-gsim-statistics.md](grhsim-gsim-statistics.md).

## Deprecated GrhSIM Static Outputs

The following GrhSIM static statistics outputs are removed and must not be regenerated:

- `activity_schedule_supernode_stats.json`
- `activity_schedule_stats.json`
- `<target>.activity_schedule.summary_stats`
- `grhsim_emit_stats.json`
- `packed_array_lane_emit` counters inside `grhsim_emit_stats.json`
- `wolvrix_xs_stats.json`
- `wolvrix_xs_post_stats_summary.json`
- `WOLVRIX_XS_GRHSIM_ENABLE_STATS`

`wolvrix_xs_post_reg_to_mem.json` is a design checkpoint for resume only. It is not a statistics file and must not be used as a metrics source.

## Deprecated GrhSIM Runtime Outputs

The following GrhSIM runtime profile interfaces and artifacts are removed:

- `emit_runtime_profile`
- `GRHSIM_EMIT_RUNTIME_PROFILE`
- `WOLVRIX_GRHSIM_SUPERNODE_TSV`
- `grhsim_supernode_static.tsv`
- `grhsim_supernode_fire.tsv`
- runtime fire-count TSV rows

The generated GrhSIM compatibility methods `set_runtime_profile_enabled`, `runtime_profile_enabled`, and `dump_runtime_profile` remain no-op compatibility APIs only. They do not produce statistics.

## Deprecated GSim Outputs

The following GSim static and runtime statistics outputs are removed:

- `--dump-stats-json`
- `<graph>_<stage>_Stats.json`
- `<top>_supernode_stats.json`
- `<top>_supernode_static.tsv`
- `<top>_supernode_fire.tsv`
- `GSIM_EMIT_RUNTIME_PROFILE`
- `EMU_RUNTIME_PROFILE`
- `GSIM_SUPERNODE_TSV`
- `[GSIM_RUNTIME_PROFILE]` log lines

Graph DOT/JSON dumps are still debug dumps. They are not part of a statistics schema.

## Deprecated Scripts And Targets

The following old aggregation and profiling entry points are obsolete and must not be used:

- `make xs_no0076_stats`
- `scripts/xs_no0076_stats.py`
- `scripts/no0087_collect_metrics.py`
- `scripts/grhsim_opt_metrics.py`
- `testcase/xs-components/scripts/model_stats.py`
- `testcase/xs-components/scripts/collect_matrix.py`
- `testcase/xs-components/scripts/objdump_stats.py`
- `testcase/xs-components/scripts/collect_runtime_profile_matrix.py`
- `testcase/xs-components/scripts/regress_runtime_cost_model.py`

The old xs-components outputs `stats/model_stats.json` and `build/matrix/results.csv` are also obsolete.
