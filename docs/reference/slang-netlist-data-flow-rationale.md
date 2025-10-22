# Slang Netlist Data Flow Analysis Rationale

This note captures how the vendored `slang-netlist` codebase performs data flow
analysis when building its connectivity graph. The implementation lives under
`docs/reference/slang-netlist` and layers custom logic on top of upstream
slang's `AbstractFlowAnalysis`.

## High-Level Flow

1. `NetlistVisitor` creates a `DataFlowAnalysis` for each procedural construct
   (procedural blocks, continuous assignments) and runs it over the relevant
   AST subtree (`source/NetlistVisitor.cpp`).
2. `DataFlowAnalysis` walks the statements / expressions, maintains per-symbol
   driver intervals in its `AnalysisState`, and creates temporary graph nodes
   via `NetlistBuilder`.
3. After the analysis finishes, `NetlistBuilder::mergeProcDrivers` folds the
   collected driver intervals into the global graph, optionally inserting
   state nodes for sequential logic (`source/NetlistBuilder.cpp`).
4. Deferred work—pending non-blocking assignments and R-value dependencies—is
   processed during `DataFlowAnalysis::finalize` and `NetlistBuilder::finalize`.

## Analysis State & Symbol Tracking

`AnalysisState` (`include/netlist/DataFlowAnalysis.hpp`) snapshots the
analysis lattice:

- `symbolDrivers` holds a `DriverMap` per symbol, recording which bit ranges
  are driven and by which netlist nodes.
- `node` tracks the current procedural node under construction so that new
  edges can be attached to it.
- `condition` remembers the most recent branching node, allowing new nodes to
  depend on the predicate that guarded their execution.
- `reachable` mirrors upstream slang and avoids attributing usage to paths the
  heuristics deem unreachable.

`SymbolTracker` (`include/netlist/SymbolTracker.hpp`, `source/SymbolTracker.cpp`)
assigns stable slots to symbols and manages the interval maps. Its interval
logic splits, merges, and de-duplicates ranges so that overlapping drivers are
represented exactly once per bit slice—critical to avoid combinational fan-in
errors when the same symbol is updated by multiple statements.

## Handling L-Values vs R-Values

`DataFlowAnalysis::noteReference` dispatches to either `handleLvalue` or
`handleRvalue` (`source/DataFlowAnalysis.cpp`) based on the `isLValue` flag,
which is guarded while visiting the left-hand side of assignments.

- **Blocking assignments**: `handleLvalue` immediately records the driver in
  `symbolDrivers`, associating the current netlist node with the bit range.
- **Non-blocking assignments**: the driver is deferred via
  `addNonBlockingLvalue`, because updates become visible only after the event
  region. `finalize` replays the pending list.
- **R-values**: `handleRvalue` first checks for in-block definitions of the
  referenced bits. If a matching interval exists, the builder connects the
  defining node(s) directly to the current node. For uncovered slices, the
  logic adds a pending R-value so that `NetlistBuilder::processPendingRvalues`
  can later connect them to upstream drivers (including interface-variable
  resolutions).

The analysis relies on `ast::LSPUtilities` to decompose composite expressions
into longest static prefixes (LSPs), enabling bit-precise ranges even through
struct/array selections.

## Control-Flow Awareness

`DataFlowAnalysis::updateNode` wires newly created assignment / case /
conditional nodes back to the predicate that guarded them, so the resulting
graph retains the fact that the assignments are conditionally executed.
Conditional constructs create dedicated netlist nodes (`builder.createConditional`,
`builder.createCase`), while sequential statements reuse the assignment nodes
the builder already emits.

Loops and complex control structures are handled by the upstream flow analysis;
this layer focuses on integrating the resulting control-flow lattice with
netlist nodes and driver intervals.

## State Merging

`mergeStates`, `joinState`, and `meetState` clone and merge the per-symbol
driver maps whenever the control-flow lattice requires it. When branches
reconverge, their driver maps are unioned range by range, using
`SymbolTracker::mergeDrivers` to accumulate contributing nodes. Any divergence
in the current or condition nodes results in a synthetic merge node, ensuring
the graph maintains a single point of control-flow convergence.

## Integration with the Netlist Graph

Once `DataFlowAnalysis` finishes, `NetlistVisitor` hands the accumulated
drivers to `NetlistBuilder::mergeProcDrivers`. This step:

- Transfers each bit-range driver list into the builder's global tracker.
- Creates `State` nodes for sequential edges (e.g. `always_ff`) so that the
  graph explicitly models registers.
- Hooks procedural drivers back to matching interface ports or interface
  member nodes, leveraging `resolveInterfaceRef` to follow modport connections.

`NetlistBuilder` also batches R-value dependencies until all drivers are known,
ensuring the graph edges point from the actual source nodes to the consuming
operation.

## Key Design Choices

- **Separation of concerns**: `DataFlowAnalysis` focuses on collecting precise,
  per-bit driver information without mutating the global graph directly; the
  builder owns graph lifetime and cross-procedural merging.
- **Deferred processing**: both non-blocking L-values and unmapped R-values
  are queued and replayed later, aligning the static analysis with simulation
  semantics.
- **Interface awareness**: Modport drivers are resolved back to interface
  variables so that downstream queries can trace through hierarchical port
  bindings.
- **Compatibility with slang**: by inheriting from `AbstractFlowAnalysis`, the
  implementation benefits from upstream reachability pruning, loop unrolling,
  and expression evaluation while exercising fine-grained control over driver
  bookkeeping.

These choices produce a netlist where nodes and edges mirror the SystemVerilog
execution model, while retaining sufficient metadata (symbols, LSPs, bit
ranges) to support detailed dependency queries.
