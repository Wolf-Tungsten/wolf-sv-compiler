# wolvrix

Wolvrix parses SystemVerilog into GRH (Graph RTL Hierarchy), runs transform passes on that IR, and writes results back out through emitters and storage backends.

Today, the main way to use Wolvrix is the Python API.

The Python API is organized around a single `Session` object. Instead of passing standalone `Design` objects around in Python, you keep your working state inside the session:

- read or load a design into a session key
- run passes against that design key
- write extra results into other session keys when needed
- emit or store files from the same session

This model keeps design data, intermediate pass results, diagnostics, and debugging state in one place.

## Build

```bash
git submodule update --init --recursive
cmake -S wolvrix -B wolvrix/build
cmake --build wolvrix/build -j$(nproc)
```

Install the Python package:

```bash
python3 -m pip install --no-build-isolation -e .
```

## The Basic Python Workflow

```python
import wolvrix

with wolvrix.Session() as sess:
    sess.log_level = "info"
    sess.diagnostics_print_min_level = "warning"
    sess.diagnostics_raise_min_level = "error"

    sess.read_sv(
        "path/to/top.sv",
        out_design="design.main",
        slang_args=["--top", "top"],
    )

    sess.run_pass("xmr-resolve", design="design.main")
    sess.run_pass("simplify", design="design.main")
    sess.run_pass("stats", design="design.main", out_stats="stats.main")

    sess.store_json(design="design.main", output="build/main.json")
    sess.emit_sv(design="design.main", output="build/main.sv")
```

What happens in this example:

1. `read_sv(...)` creates a design and stores it under `design.main`.
2. Each `run_pass(...)` call modifies `design.main` in place.
3. `out_stats="stats.main"` stores a separate result in the same session.
4. `store_json(...)` and `emit_sv(...)` write files using the design stored in the session.

## Naming Rules

The naming rules are intentionally simple:

- use `out_design=...` when an action creates or loads a design
- use `design=...` for the design being operated on
- use `in_*` and `out_*` for session-based data flow
- use regular keyword arguments for ordinary pass or emit configuration

Example:

```python
with wolvrix.Session() as sess:
    sess.read_sv("top.sv", out_design="design.main")
    sess.run_pass("stats", design="design.main", out_stats="stats.main")
```

This reads naturally:

- `design="design.main"` tells you which design is being worked on
- `out_stats=...` tells you which additional session value will be produced

## Diagnostics And Logging

Every action returns diagnostics as `list[dict]`.

```python
diags = sess.run_pass("stats", design="design.main", out_stats="stats.main")
```

Logging is separate from diagnostics:

- diagnostics are structured results returned to Python
- logs are C++ runtime messages printed directly from native code

Configure them at the session level:

```python
with wolvrix.Session() as sess:
    sess.diagnostics_print_min_level = "warning"  # none | warning | error
    sess.diagnostics_raise_min_level = "error"   # none | warning | error
    sess.log_level = "warn"                      # trace | debug | info | warn | error | off
```

Default behavior is usually enough:

- warning and error diagnostics are printed automatically
- error diagnostics raise automatically
- every action still returns the diagnostics list for explicit inspection

Useful helpers:

```python
text = wolvrix.format_diagnostics(diags, min_level="info")
wolvrix.print_diagnostics(diags, min_level="warning")
```

## Working With Session Values

The session is a key-value store.

Common keys look like this:

- `design.main`
- `design.flat`
- `stats.main`
- `loops.main`

You can inspect the session with:

```python
value = sess.get("stats.main")
kind = sess.kind("stats.main")
keys = sess.keys(prefix="stats.")
```

Not every session value needs a rich Python representation.

- if a `kind` has a Python adapter, `sess.get(...)` returns that adapter object
- otherwise it returns `OpaqueValue`

This is deliberate. Session values are not only for Python debugging; they are also how C++ passes and emitters pass data to each other.

## Emit And Store APIs

Currently implemented:

```python
sess.store_json(*, design: str, output: str, mode="pretty-compact", top=None)
sess.emit_sv(*, design: str, output: str, top=None, split_modules=False)
sess.emit_verilator_repcut_package(*, design: str, output: str, top=None)
sess.emit_grhsim_cpp(*, design: str, output: str, top=None, waveform="off", perf="off")
```

The JSON, SV, and Verilator RepCut emitters do not yet consume extra session values, so they
currently expose no `in_*` parameters. GrhSIM is different: it requires `activity-schedule`
results from the same session. If you pass an unsupported `in_*`, the API raises an error instead
of silently ignoring it.

## Important Documents

If you are new to Wolvrix, read these in order:

1. [docs/grh/grh-ir.md](docs/grh/grh-ir.md)
2. [docs/transform/README.md](docs/transform/README.md)
3. [docs/python/python-binding-architecture.md](docs/python/python-binding-architecture.md)
4. [docs/emit/grhsim-model.md](docs/emit/grhsim-model.md) (GrhSIM 仿真模型总览)

## Tests

```bash
ctest --test-dir wolvrix/build --output-on-failure
```
