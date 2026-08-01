# lane-aggregate

`lane-aggregate` re-vectorizes per-lane scalar registers that firtool flattened out of
register arrays. It merges a group of structurally isomorphic lane registers back into
one wide `kRegister` plus a single write port, and rewrites every read of a merged lane
register into a static slice of the wide register.

The pass is **opt-in**: it is not part of any production pipeline. Run it explicitly
after `hier-flatten` and before `reg-to-mem` / `comb-lane-pack`.

## Pre-pass Normalization

Before grouping, the pass rewrites `kReduce{Or,And,Xor}(kConcat(e_0, ..., e_m))`
into an explicit element-wise tree (`kOr`/`kAnd`/`kXor` over per-element
reductions; a reduce over a 1-bit element is the element itself). The two forms
are semantically identical — the packed form is how firtool emits wide
reductions — and the tree form is lane-pointwise, so it can be widened.

## Input Shape

The pass expects the shape produced by firtool flattening (verified on the full
XiangShan graph):

- every lane register has **exactly one** `kRegisterWritePort`;
- per-lane write cones are structurally identical except for constants (affine in
  the lane index, e.g. `enqPtr == i`) and lane-relative register reads.

### Name Grouping

A *numeric segment* of a register name is `_<digits>` followed by `_`, `$`, or end
of string. Grouping masks **every** numeric segment to `*` — the masked name is a
candidate key. The lane segment is the segment position with the most distinct
values (ties resolve to the leftmost). Members are then **sub-grouped by the
values of all other segments**, so every emitted lane group has exactly one
varying segment; its report key keeps the constant segments' values (e.g.
`data16_*$needCheck0Reg_*` splits into `data16_3$needCheck0Reg_*`-style slices).

Examples:

- `robEntries_0_wflags` .. `robEntries_351_wflags` → key `robEntries_*_wflags`,
  one varying segment → one lane group;
- `cpu$enqueue$enqPtrVec_7_value` (lanes 0..7, first segment = module instance,
  constant across the group) → key `cpu$enqueue$enqPtrVec_*_value` → lane index =
  the port segment;
- `robEntries_0_uopNum_T_2` → key `robEntries_*_uopNum_T_*`: the array index has
  the most distinct values, the Chisel temporary id `_T_2` is constant → the lane
  group slices on the temp id (a "first/last segment wins" rule would mis-group
  both of these);
- `data16_3$needCheck0Reg_7` (bank × entry) → one lane group per bank slice;
- a diagonal-only pattern (`arr_0_0, arr_1_1, ...`) decomposes into nothing and
  is reported as `multi_varying_segment`.

Lane indices must be dense (at most `max_index_holes` holes inside the span) and
the merge bucket must have at least `min_lanes` lanes.

## Merge Criteria

All of the following must hold for a group (or its majority bucket) to be merged:

1. the name group has at least `min_lanes` candidate lanes;
2. all members have the same width and signedness (group-level `width_mismatch`
   reject otherwise);
3. per lane: exactly one write port with at least one event, 1-bit `updateCond`,
   `nextValue` width equal to the lane width, an all-ones write mask, no
   `initValue`, no XMR reference (`xmrPath` last segment), and — when
   `keep_declared_symbols` is on — the register symbol must not be a declared
   symbol. Lanes failing these checks stay scalar (they do not poison the group);
4. signature bucketing (hash of the `(updateCond, data)` cones with constants
   abstracted and bare-value leaves unified, plus the write-port event set) must
   produce a **majority bucket of at least `min_lanes`** lanes. The hash is only
   an accelerator: the rewrite is gated by an exact comparison. Lane-group
   register reads hash as `(marker, group key, absolute index)`: `51` = own
   lane, `53` = sibling group at the same index (index not mixed in, so
   same-index families bucket together), `52` = any group at an absolute index
   (so all lanes reading the same dispatch port bucket together; a lane whose
   own index coincides with that port hashes as `53` and simply stays scalar);
5. the bucket's indices must be dense: at most `max_index_holes` missing indices
   inside `[minIdx, maxIdx]` (`not_dense` reject otherwise);
6. all bucket lanes have **identical event sets** (`eventEdge` strings and event
   `ValueId`s);
7. the `(updateCond, data)` cones must compare **exactly** structurally isomorphic
   under an N-wise traversal (no hashing). Every cone position must classify as:

   | position | requirement | materialization |
   | --- | --- | --- |
   | shared leaf | all lanes reference the same `ValueId` | one `kReplicate(value, span)` |
   | constant leaf | per-lane `kConstant`, values affine in the lane index `c_i = a*i + b` (`a = 0` allowed), width ≤ 64, no unknown bits | one packed `kConstant` of width `span*w` (lane `i`'s value in segment `[i*w +: w]`, hole segments zero) |
   | self read | lane `i` reads this group's lane `i` register | the wide read |
   | sibling read | lane `i` reads a sibling group's lane `i` register | the sibling's wide read; the sibling group must merge with a lane set that is a superset of this bucket and the same span, otherwise this group is rejected (`sibling_not_merged` / `sibling_lane_mismatch`) |
   | shared register | every lane reads the same register `R` | one read of `R` (or a slice of `R`'s wide read when `R` is itself a merged lane), replicated by `span` |
   | lane parameter | per-lane distinct `kRegisterReadPort`s (any targets, including undeclared registers), or per-lane bare values with no defining op (`_GEN_N` wires, input ports) | one `kConcat` of the per-lane values: operand 0 = value of the highest segment, last operand = segment 0 (hole segments get a zero constant); `res[i*w +: w]` = lane `i`'s own value — exact, not a guess |
   | eq-onehot | per-lane `kEq(x, c_i)` with the same shared `x` and `c_i == i` exactly (both operand orders, no ambiguous both-constant case) | `kShl(kConstant(span'd1), x)` — result bit `i` = `(x == i)`, zero when `x >= span`; other affine slopes stay rejected |
   | internal node | lane-pointwise op (`kAnd`/`kOr`/`kXor`/`kXnor`/`kNot`/`kAssign`/`kMux`, plus `kLogicAnd`/`kLogicOr`/`kLogicNot` with all operands 1-bit), same kind/arity/attrs/result type across lanes | widened to `span*w`; `kMux` is rebuilt as `(t & m) \| (f & ~m)` with the merged select broadcast per lane; `kLogicAnd`/`kLogicOr`/`kLogicNot` materialize as `kAnd`/`kOr`/`kNot` (identical at 1-bit operands) |
   | replicate | `kReplicate(v_i, W)` with a per-lane 1-bit operand and `rep == lane width` | per-lane broadcast: the merged operand (span bits) is expanded lane-wise, `res[i*W + j] = mergedOperand[i]` (same broadcast used for mux masks) |
   | 1-bit reduce | `kReduceOr`/`kReduceAnd`/`kReduceXor` with a 1-bit operand | the identity: the merged operand is used directly (a 1-bit reduce is a no-op) |

   Anything else rejects the group: other non-pointwise ops (`kAdd`, `kConcat`,
   reductions with multi-bit operands, `kReplicate` with a multi-bit operand,
   slices, `kNe`/other comparators) as lane-varying positions
   (`unsupported_op`), mixed defined/undefined lane-varying leaves
   (`lane_varying_leaf`), reads of a *different* lane of the same group
   (`cross_lane_read`), non-affine constants (`non_affine_constant`), divergent
   shared subtrees (`shared_subtree_divergence`).

### initValue packing

Lanes whose `initValue` parses as a constant no longer block merging. Either
**every** bucket lane has a constant init or none does (`init_mixed` reject);
the per-lane init values must be affine in the lane index (`c_i = a*i + b`,
all-equal included, `non_affine_init` reject otherwise). The wide register's
`initValue` attr is then the packed constant table — a single
`"<span*W>'h<hex>"` literal with lane `i`'s init in segment `[i*W +: W]` and
holes zeroed, in the same literal form `ingest` produces.

Lanes outside the majority bucket (e.g. a lane 0 with a reset-specialized cone) keep
their scalar registers untouched.

## Merged Form (exact operand definitions)

For a bucket with lane width `W`, indices `i` in `0..span-1` (`span = maxIdx + 1`,
missing indices become dead hole segments):

- **Wide register**: `kRegister` named after the masked group key with every `_*`
  marker removed plus a `__laneagg` suffix (e.g. `robEntries_*_wflags` →
  `robEntries_wflags__laneagg`) with attrs
  `width = span*W`, `isSigned` = lane signedness. Bit order convention: **lane `i`
  occupies bits `[i*W +: W]`, lane 0 in the LSBs**. Hole segments (missing indices)
  are never read and hold their value forever.
- **Wide read**: one `kRegisterReadPort` with attr `regSymbol = <wide name>`;
  `res[0]` has width `span*W`.
- **Write port**: one `kRegisterWritePort` with attr `regSymbol = <wide name>` and
  `eventEdge` copied from the lanes; operands:
  - `oper[0]` (updateCond): `kReduceOr(kAnd(condVec, presentConst))`, where
    `condVec` (width `span`) is the merged cond cone and `presentConst` is the
    `span`-bit constant with bit `i` set exactly when lane `i` is in the bucket;
  - `oper[1]` (nextValue, width `span*W`):
    `kOr(kAnd(dataVec, mask), kAnd(wideRead, kNot(mask)))`, where `dataVec` is the
    merged data cone and `mask` is the per-lane broadcast of the masked cond —
    `mask[i*W + j] = condMasked[i]` (built as a `kConcat` of `kReplicate`d
    1-bit slices, or `condMasked` itself when `W == 1`);
  - `oper[2]` (mask): an all-ones `kConstant` of width `span*W`;
  - `oper[3]`.. (events): the shared event values, identical to the lane write ports.

  Semantically each lane segment performs `if (cond_i) wide[i*W +: W] <= data_i`,
  which is exactly the per-lane behavior of the original scalar registers. Reset is
  not special-cased: lane reset values live inside the data/cond cones (encoded as
  muxes and affine constants) and reset events are covered by the event-set check;
  a lane whose reset structure differs from the majority simply stays scalar.
- **Read side**: every `kRegisterReadPort` of a merged lane register is replaced by
  `kSliceStatic(wideRead, i*W, i*W + W - 1)` — `oper[0]` = the wide read value,
  attrs `sliceStart = i*W`, `sliceEnd = i*W + W - 1`, result width `W`. Value
  symbols and output-port bindings are moved to the slice.

The old lane registers, their write ports, and the replaced read ports are erased.
Dead per-lane cone ops are left in place for `dead-code-elim` / `simplify`.

## Phase 2: Read-Side Select Trees

After phase 1, every read of a merged lane register exists as
`kSliceStatic(wideRead, i*W, W)`. The array read-out structures that used to
select among the scalar lanes — mux chains or one-hot and/or trees — therefore
turn into trees over these slices. Phase 2 (option `-read-select`, on by
default) rewrites a whole tree into one dynamic read of the wide register:

```text
kSliceDynamic(wideRead, ptr * W, W)
```

- `oper[0]`: the wide read value of one merged group;
- `oper[1]` (start offset, unsigned): `ptr * W`, built as `ptr` when `W == 1`,
  `kShl(ptr, log2(W))` when `W` is a power of two, else `kMul(ptr, W)` (result
  widths are widened so the product cannot wrap);
- attr `sliceWidth` (int64): the lane width `W`; result width `W`.

### Matched forms

- **mux chain**: `kMux(kEq(ptr, C_i), slice_i, rest)`, recursing on the false
  branch. `C_i` are distinct constants, each equal to its slice's lane index,
  in any order; the chain must cover **every** span index `0..span-1`, and the
  final default must be a zero constant (see semantics below).
- **and/or one-hot tree**: `kOr` over mutually exclusive terms
  `kAnd(kReplicate(kEq(ptr, i), W), slice_i)` or `kMux(kEq(ptr, i), slice_i, 0)`
  (replicate in either operand position; `kAnd(kEq(ptr,i), slice_i)` at `W==1`).

Requirements for both forms: all `kEq` share the same `ptr` `ValueId` (either
operand position, never both-constant); every leaf slice has
`sliceStart == i*W`, `sliceEnd == i*W + W - 1` on the **same** merged group's
wide read; a tree that touches any other value (another group, a scalar
register read of an unmerged lane, arbitrary logic) is skipped.

### Semantics and out-of-range behavior

For `ptr < span`, the tree and `kSliceDynamic(wideRead, ptr*W, W)` agree
exactly. For `ptr >= span` the tree yields its default while the dynamic read
is out of range; the GRH reference interpreter and Verilator (2-state) both
yield `0` there, so the rewrite requires a zero default and is **2-state
equivalent**. 4-state simulation may propagate `X` where the tree yielded the
default — same caveat class as `onehot-to-mux`. Full span coverage is required
because a missing branch would read a hole segment of the wide register (hold
bits) instead of the default.

### Minority-lane rescue (phase 1 addition)

Signature bucketing deliberately over-splits lanes that read a dispatch port at
their own index (hash markers `53` vs `52`); their cones are usually identical
to the majority. After the majority bucket passes the exact check, every
remaining candidate lane is tried once: it joins the bucket when the exact
structural check still passes **and** its write-port event set matches the
majority. Rescue is what lets `robEntries`-style families reach full span,
which in turn makes their read-side select trees convertible.

### Example

Input (after phase 1, `W = 4`, 8 lanes):

```text
sel_out = mux(eq(ptr, 3'd0), slice(wide, 0, 3),
          mux(eq(ptr, 3'd1), slice(wide, 4, 7),
          ...
          mux(eq(ptr, 3'd7), slice(wide, 28, 31), 4'd0)))
```

Output:

```text
offset  = kShl(ptr, 3'd2)
sel_out = kSliceDynamic(wide, offset, sliceWidth = 4)
```

### Report

The JSON report's `summary.read_select` counts phase 2:
`{"trees": N, "ops_retired": R, "ops_created": C}` — trees replaced, tree ops
that become dead (mux/or/and/replicate/eq), and ops created
(sliceDynamic + offset computation).

## Example

Input (lane width `W = 4`, indices `0..7`, `en` shared):

```text
lane_0_q: if (en) q0 <= (enW & q0) ^ 4'd0
lane_1_q: if (en) q1 <= (enW & q1) ^ 4'd1
...
lane_7_q: if (en) q7 <= (enW & q7) ^ 4'd7
out_i = lane_i_q
```

Output (one 32-bit register, lane `i` in `[i*4 +: 4]`):

```text
lane_q__laneagg:                          // kRegister, width = 32
wide = kRegisterReadPort(lane_q__laneagg) // 32 bits
condVec = kReplicate(en, 8)               // 8 bits
cond    = kReduceOr(kAnd(condVec, 8'hFF))
table   = 32'h76543210                    // packed constant: segment i = i
dataVec = kXor(kAnd(kReplicate(enW, 8), wide), table)
mask    = kConcat(kReplicate(condVec[7], 4), ..., kReplicate(condVec[0], 4))
next    = kOr(kAnd(dataVec, mask), kAnd(wide, kNot(mask)))
write:  updateCond = cond, nextValue = next, mask = 32'hFFFFFFFF, events = [clk], eventEdge = [posedge]
out_i   = kSliceStatic(wide, i*4, i*4 + 3)
```

## Python Example

```python
with wolvrix.Session() as sess:
    sess.read_sv("top.sv", out_design="design.main")
    sess.run_pass("hier-flatten", design="design.main")
    sess.run_pass(
        "lane-aggregate",
        design="design.main",
        min_lanes=8,
        max_index_holes=2,
        out_lane_aggregate_report="laneagg.report",
        keep_declared_symbols=False,
    )
    sess.run_pass("simplify", design="design.main")
```

CLI equivalent:

```bash
wolvrix --pass=hier-flatten --pass=lane-aggregate:-min-lanes=8:-output-key=laneagg.report top.sv
```

## Key Options

| Option | Default | Meaning |
| --- | --- | --- |
| `-min-lanes` | `8` | minimum majority-bucket size required to merge (must be ≥ 2) |
| `-max-index-holes` | `2` | maximum missing indices allowed inside the bucket span |
| `-read-select` / `-no-read-select` | on | phase 2: rewrite read-side select trees to `kSliceDynamic` |
| `-output-key` | empty | session key for the per-group JSON report |

## Session Output

With `-output-key=<key>`, the pass stores a JSON string (kind
`lane-aggregate.reports`) shaped like the `reg-to-mem` group report:

```json
{"groups": [{
    "graph": "...", "group_id": 1, "discovery": "name_pattern",
    "group": "robEntries_*_wflags",
    "module": "Rob.sv", "element_width": 1, "element_count": 352,
    "lane_count": 351, "outcome": "merged",
    "reject_reason": "", "reject_detail": ""
  }],
 "summary": {"by_reason": {"...": {"groups": 3, "elements": 40}},
             "by_outcome": {"merged": {"groups": 1811, "elements": 633885}}}}
```

- `group`: the (possibly constant-segment-specialized) lane-group key, with the
  lane segment shown as `*`
- `element_width`: lane register width `W`
- `element_count`: registers in the name group
- `lane_count`: lanes in the merged bucket (0 when rejected before bucketing)
- `outcome`: `merged` / `rejected` / `skipped`
- `reject_reason`: e.g. `too_few_lanes`, `no_majority`, `multi_varying_segment`,
  `width_mismatch`, `not_dense`, `event_mismatch`, `structure_mismatch`,
  `unsupported_op`, `non_affine_constant`, `lane_varying_leaf`, `cross_lane_read`,
  `sibling_not_merged`, `sibling_lane_mismatch`, `shared_subtree_divergence`,
  `rewrite_failed`

## Notes

- Only lane-pointwise logic is widened. Cones containing lane-varying
  non-pointwise ops (`kAdd`, `kConcat`, reductions, slices, comparators other
  than the eq-onehot form) are rejected; the `enqPtr == i` decode merges via
  the eq-onehot branch, while wider patterns (e.g. `x < i`, range compares)
  still report `unsupported_op` and are left to follow-up pattern matchers.
- Per-lane distinct external values are no longer blockers: dispatch-port
  reads at a fixed absolute index are shared leaves, and per-lane distinct
  register reads / bare wires become lane-parameter `kConcat`s. This is what
  lets the XiangShan `robEntries` 1-bit field families and the `enqRob_req`
  families merge.
- Merged groups disappear from `reg-to-mem` discovery; unmerged groups are
  untouched and flow through it as before. Run `simplify` afterwards to clean up
  mask/replicate/slice artifacts and dead per-lane cones; `comb-lane-pack` can
  pack remaining sibling comb trees.
- With `keep_declared_symbols=True` (the default), groups whose register symbols
  are declared symbols are rejected (`declared_symbol` exclusions), mirroring how
  `memory-read-retime` treats declared registers.
