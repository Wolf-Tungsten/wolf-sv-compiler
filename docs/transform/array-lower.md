# array-lower

`array-lower` expands the twelve array-value ops (see
[grh-ir.md §2.4](../grh/grh-ir.md)) back into plain wide-scalar form. It is the
semantic inverse of `lane-aggregate -output-mode=array`: every expansion is
bit-exact, so the lowered graph is interchangeable with the array form.

Use it:

1. as a **degradation step** in front of consumers that do not support the
   array ops (SystemVerilog emit, legacy toolchains) — run `array-lower`
   before `emit`;
2. as an **equivalence witness** for dual-shape difftests — the same design
   simulated in array form and in lowered form must produce identical
   results.

The pass takes no arguments. `kMemory` declarations and plain
`kMemoryReadPort` row reads are already standard form and pass through
untouched. Graphs without array ops report `unchanged`.

Notation: `W = elemWidth`, `row = rows`; lane `i` of a packed value occupies
`[i*W +: W]`, lane 0 in the LSBs.

## Expansion Rules

| Source op | Expansion |
| --- | --- |
| `kMemoryReadAllPort` | One `kMemoryReadPort` per row (constant address `i`, same `memSymbol`) + one `kConcat` with row `row-1` in the MSBs and row 0 in the LSBs. With `row == 1` the single read-port result replaces the array result directly (no concat). |
| `kMemoryWriteLanesPort` | One `kMemoryWritePort` per row: `updateCond = kSliceStatic(laneMask, i, i)`, `addr = kConstant(i)`, `data = kSliceStatic(data, i*W, i*W+W-1)`, `mask =` all-ones constant (`W` bits); `events` operands and the `eventEdge` attr are forwarded verbatim. |
| `kArrayMux` | Per-lane select broadcast `m = kConcat(kReplicate(kSliceStatic(sel, i, i), W) ...)`, then `res = kOr(kAnd(t, m), kAnd(f, kNot(m)))`. |
| `kArrayReduceOr/And/Xor` | Single `kReduceOr/kReduceAnd/kReduceXor` over the full packed operand. Per-lane-then-cross-lane reduction equals full-width reduction by associativity. |
| `kArrayReduceLanesOr/And/Xor` | Per lane `kReduceOr/kReduceAnd/kReduceXor` over `kSliceStatic(data, i*W, i*W+W-1)`, plus one `kConcat` of the per-lane bits with lane `row-1` in the MSBs and lane 0 in the LSBs. With `row == 1` the single per-lane bit replaces the result directly (no concat). |
| `kArrayBroadcast` | `kReplicate(scalar, rows)`. |
| `kArrayLaneConst` | One packed `kConstant` with `values[i]` in segment `[i*elemWidth +: elemWidth]`. |
| `kArrayOnehot` | `kShl(kConstant(rows'd1), x)` at result width `rows`; `x >= rows` naturally yields 0 under fixed-width shift, matching the out-of-range semantics. |

## Priority Attrs on `kMemoryWriteLanesPort`

`memoryWrite.priorityGroup` / `memoryWrite.priority` are **dropped** on
expansion, with a warning per source op and a counter in the pass summary.
Rationale:

- the expanded per-row ports write pairwise distinct constant addresses, so
  they can never collide with each other and intra-group ordering is
  meaningless for them;
- keeping one port's priority on `row` fresh ports would violate the
  priority-group invariant (priorities unique and contiguous in `[0, N)` per
  group);
- the producing pass (`lane-aggregate -output-mode=array`) never emits these
  attrs, so the drop path is defensive.

The one semantic gap: ordering of the expanded ports against *other* grouped
write ports with dynamic addresses becomes unordered. The warning makes that
visible.

## Notes

- Equal constants (row addresses, all-ones masks, the onehot `1`) are
  memoized per graph — expanded rows share one `kConstant` per value.
- New ops carry srcLoc `pass = "array-lower"` with `expand-*` notes; the
  array result's symbol moves onto the replacement value and output ports
  are rebound, so downstream names stay stable.
- Dead leftovers (replaced values) are left for `dead-code-elim`; the pass
  only erases the array ops themselves.
- Ops whose shape is inconsistent (e.g. `memSymbol` not resolving to a
  `kMemory`, or result width ≠ `row × width`) are skipped with a warning
  rather than failing the whole pass.

Python: `sess.run_pass("array_lower", design="design.main")` (no options).
