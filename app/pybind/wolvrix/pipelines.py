from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from . import Session


def _has_error(diagnostics: list[dict]) -> bool:
    return any(
        str(diagnostic.get("kind", "")).lower() == "error"
        for diagnostic in diagnostics
    )


def cpu_single_thread(
    session: "Session",
    *,
    design: str,
    output: str,
    module: str = "grhsim.main",
    top: str | None = None,
    replace: bool = False,
    ops_per_source_file: int = 50000,
    fixed_point_iteration_limit: int = 100,
) -> list[dict]:
    diagnostics: list[dict] = []
    diagnostics.extend(
        session.lower_grhsim(
            design=design,
            module=module,
            top=top,
            replace=replace,
        )
    )
    if not _has_error(diagnostics):
        diagnostics.extend(session.run_sim_pass("schedule-topo", module=module))
    if not _has_error(diagnostics):
        diagnostics.extend(
            session.emit_grhsim(
                module=module,
                backend="cpu",
                output=output,
                ops_per_source_file=ops_per_source_file,
                fixed_point_iteration_limit=fixed_point_iteration_limit,
            )
        )
    return diagnostics


__all__ = ["cpu_single_thread"]
