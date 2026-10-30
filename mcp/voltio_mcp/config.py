"""Bench configuration: which PSU, how to reach it, and what the agent may do."""

from __future__ import annotations

import os
import shlex
import tomllib
from dataclasses import dataclass
from pathlib import Path


class ConfigError(Exception):
    pass


@dataclass(frozen=True)
class Config:
    # [psu]
    cli: list[str]
    port: str
    dp: int
    timeout_s: float
    # [limits] — enforced by the server on every write, regardless of what the agent asks
    max_voltage_V: float
    max_current_A: float
    ovp_V: float | None
    ocp_A: float | None
    # [power_cycle]
    min_off_ms: int
    max_off_ms: int
    # [measure]
    max_duration_s: float
    min_interval_ms: int
    # [audit]
    audit_log: Path | None


def _num(section: dict, key: str, default=None, *, required: bool = False) -> float | None:
    if key not in section:
        if required:
            raise ConfigError(f"missing required setting '{key}'")
        return default
    value = section[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigError(f"'{key}' must be a number")
    return float(value)


def load_config(path: str | Path) -> Config:
    path = Path(path)
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise ConfigError(f"config file not found: {path}") from None
    except tomllib.TOMLDecodeError as e:
        raise ConfigError(f"invalid TOML in {path}: {e}") from None

    psu = raw.get("psu", {})
    limits = raw.get("limits", {})
    cycle = raw.get("power_cycle", {})
    measure = raw.get("measure", {})
    audit = raw.get("audit", {})

    cli = psu.get("cli", "voltio")
    if isinstance(cli, str):
        cli = shlex.split(cli, posix=(os.name != "nt"))
    if not cli or not all(isinstance(c, str) for c in cli):
        raise ConfigError("'cli' must be a command string or a list of strings")

    port = psu.get("port")
    if not isinstance(port, str) or not port:
        raise ConfigError("missing required setting 'port' in [psu]")

    dp = psu.get("dp", 1)
    if not (isinstance(dp, int) and not isinstance(dp, bool) and dp in (1, 2)):
        raise ConfigError("'dp' must be 1 (most models) or 2 (1685B)")

    max_v = _num(limits, "max_voltage_V", required=True)
    max_a = _num(limits, "max_current_A", required=True)
    if max_v <= 0 or max_a <= 0:
        raise ConfigError("max_voltage_V and max_current_A must be > 0")

    ovp = _num(limits, "ovp_V")
    ocp = _num(limits, "ocp_A")
    # Protection below the allowed setpoints would trip on legal commands.
    if ovp is not None and ovp < max_v:
        raise ConfigError(f"ovp_V ({ovp}) must be >= max_voltage_V ({max_v})")
    if ocp is not None and ocp < max_a:
        raise ConfigError(f"ocp_A ({ocp}) must be >= max_current_A ({max_a})")

    min_off = int(_num(cycle, "min_off_ms", 100))
    max_off = int(_num(cycle, "max_off_ms", 60_000))
    if not 0 < min_off <= max_off:
        raise ConfigError("power_cycle: need 0 < min_off_ms <= max_off_ms")

    max_dur = _num(measure, "max_duration_s", 300.0)
    min_int = int(_num(measure, "min_interval_ms", 250))
    if max_dur <= 0 or min_int <= 0:
        raise ConfigError("measure: max_duration_s and min_interval_ms must be > 0")

    timeout_s = _num(psu, "timeout_s", 5.0)
    if timeout_s <= 0:
        raise ConfigError("'timeout_s' must be > 0")

    log = audit.get("log", "voltio-audit.jsonl")
    audit_log = None
    if log:
        audit_log = Path(log)
        if not audit_log.is_absolute():
            audit_log = path.parent / audit_log

    return Config(
        cli=list(cli),
        port=port,
        dp=dp,
        timeout_s=timeout_s,
        max_voltage_V=max_v,
        max_current_A=max_a,
        ovp_V=ovp,
        ocp_A=ocp,
        min_off_ms=min_off,
        max_off_ms=max_off,
        max_duration_s=max_dur,
        min_interval_ms=min_int,
        audit_log=audit_log,
    )
