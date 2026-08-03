# comb-lane-pack

`comb-lane-pack` packs groups of structurally identical combinational sibling
trees — N trees with the same op sequence and operand shapes (one "signature")
— into a single wide pointwise tree. It does not merge storage: each lane
root is rewritten to a static slice of the packed value, and dead per-lane
trees are removed by an embedded `dead-code-elim` run.

## Roots and Grouping

Candidate roots come from two sources (both on by default):

- `storage-data`: the data operand of `kRegisterWritePort` /
  `kMemoryWritePort` / `kMemoryFillPort`;
- `declared`: results of ops whose value is a declared symbol or an output
  port (with `-require-declared-roots`, on by default, only those).

Trees are analyzed bottom-up over the supported internal ops
(`kAnd`/`kOr`/`kXor`/`kXnor`/`kNot`/`kAssign`, plus `kMux` when `-enable-mux`
is on) and bucketed by structural signature. Within a bucket, candidates in
anchor (program) order are grouped under `-min-group-size` / `-max-group-size`
/ `-max-root-gap`, and a group is accepted only when its packed width is
within `-min-packed-width` / `-max-packed-width` and no lane tree depends on
another lane root of the same group.

## Wide Mode (default)

The historical shape, unchanged by the array-mode addition:

- leaf positions pack as one `kConcat` (`pack-leaf`); lane `i` occupies
  `[i*W +: W]`, lane 0 in the LSBs;
- `kMux` packs as `(t & m) | (f & ~m)` where the mask `m` is a per-lane
  `kReplicate(cond, W)` joined by one `kConcat` (`pack-mux-mask`,
  `pack-mux-mask-concat`, `pack-mux-not`, `pack-mux-true`, `pack-mux-false`,
  `pack-mux-or`);
- pointwise `kAnd`/`kOr`/`kXor`/`kXnor`/`kNot`/`kAssign` widen as-is
  (`pack-unary` / `pack-binary`);
- each lane root is replaced with `kSliceStatic(packed, i*W, W)`
  (`pack-result-slice`).

## Array Output Mode (`-output-mode=array`)

Off by default; `wide` output is bit-identical to previous behavior. Array
mode emits the array-value ops of [grh-ir.md §2.4](../grh/grh-ir.md) for the
packed products, moving them out of the plain compute shape (the
`pack-mux-mask` tree in particular). Differences from wide mode:

- `kMux` packs as one `kArrayMux` (`pack-array-mux`). Its `sel` operand is
  the per-lane guard vector (width = lane count, lane 0 at the LSB), built as
  one `kConcat` of the per-lane 1-bit conds (`pack-array-mux-sel`); when every
  lane selects on the same scalar cond, the shared cond is reused through one
  `kArrayBroadcast` instead (`pack-array-broadcast`). The
  replicate/concat/not/and/or mask tree is not emitted.
- A leaf position whose lanes are all the same shared scalar (a replicate
  shape) packs as one `kArrayBroadcast` (`pack-array-broadcast`) instead of a
  degenerate `kConcat`.
- Distinct leaves stay `kConcat` (that concat *is* the array packing);
  pointwise ops widen unchanged; per-lane result slices stay `kSliceStatic`
  (lane roots still feed scalar registers; no `kMemory` is introduced).

Example — four lanes of `out_i = sel_i ? t_i : f_i` with `W = 8`:

- wide: `4× kReplicate + kConcat(mask) + kNot + 2× kAnd + kOr +
  2× kConcat(leaves)` = 11 ops, plus 4 result slices;
- array: `kConcat(sel) + 2× kConcat(leaves) + kArrayMux` = 4 ops, plus 4
  result slices.

The packed value still follows the array layout convention: packed `Logic`,
lane `i` at `[i*W +: W]`, lane 0 in the LSBs; guard vectors are one bit per
lane. `array-lower` can expand the array ops back to the wide scalar form for
consumers that do not support them.

## Key Options

| Option | Default | Purpose |
| --- | --- | --- |
| `-min-group-size` / `-max-group-size` | 4 / 16 | lanes per packed group |
| `-min-packed-width` / `-max-packed-width` | 32 / 1024 | packed width window |
| `-max-tree-nodes` | 64 | per-tree analysis bound |
| `-max-root-gap` | 128 | anchor-order grouping window |
| `-require-declared-roots` | true | declared roots must be declared symbols / outputs |
| `-enable-declared-roots` | true | enable declared-symbol roots |
| `-enable-storage-data-roots` | true | enable storage data-operand roots |
| `-enable-mux` | true | allow `kMux` inside packed trees |
| `-output-mode` | `wide` | `wide`: historical wide shape; `array`: array-value shape (see above) |
| `-output-key` | empty | session key for the per-group report |

## Notes

- Every op the pass creates carries a `comb-lane-pack` srcLoc note tag
  (`pack-leaf`, `pack-unary`, `pack-binary`, the `pack-mux-*` family in wide
  mode, `pack-array-mux` / `pack-array-mux-sel` / `pack-array-broadcast` in
  array mode, and `pack-result-slice` on the per-lane slices), so statistics
  can attribute the packed products to this pass.
- Python: `sess.run_pass("comb-lane-pack", design=..., output_mode="array")`;
  all CLI options above are also available as snake_case kwargs.
