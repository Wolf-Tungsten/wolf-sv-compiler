from __future__ import annotations

from .. import _wolvrix as _native
from ._registry import register_session_adapter
from .stats import StatsValue


def _adapt_reg_to_mem_reports(session, key: str, _storage: str, _kind: str, _payload):
    text = _native.session_export(session._capsule, key=key, view="text")
    return StatsValue(key=key, text=text)


register_session_adapter(storage="native", kind="reg-to-mem.reports", factory=_adapt_reg_to_mem_reports)
