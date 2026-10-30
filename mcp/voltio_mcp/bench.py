"""PSU operations with the bench safety rules applied.

Every limit here is enforced in code, so it holds no matter what the agent
is prompted to do. Every call is written to the audit log.
"""

from __future__ import annotations

import json
import logging
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

from .backend import CliBackend
from .config import Config

log = logging.getLogger("voltio_mcp")

class LimitError(Exception):
    """A request was refused because it breaks the bench limits."""


class AuditLog:
    def __init__(self, path: Path | None):
        self._path = path
        self._lock = threading.Lock()
        self._degraded = False

    def write(self, tool: str, args: dict, *, result=None, error: str | None = None) -> None:
        """Append one entry. Never raises: a broken log must not change a tool's outcome."""
        if self._path is None:
            return
        try:
            entry = {
                "ts": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
                "tool": tool,
                "args": args,
            }
            if error is None:
                entry["result"] = result
            else:
                entry["error"] = error
            line = json.dumps(entry, separators=(",", ":")) + "\n"
            with self._lock:
                self._path.parent.mkdir(parents=True, exist_ok=True)
                with self._path.open("a", encoding="utf-8") as f:
                    f.write(line)
        except Exception as e:
            # Warn once per outage, then stay quiet until a write succeeds again.
            if not self._degraded:
                log.warning("audit log write to %s failed: %s", self._path, e)
            self._degraded = True
        else:
            self._degraded = False

    def verify(self) -> None:
        """Create the log's directory and file now, raising OSError if that's impossible."""
        if self._path is None:
            return
        self._path.parent.mkdir(parents=True, exist_ok=True)
        with self._path.open("a", encoding="utf-8"):
            pass


def _stats(values: list[float]) -> dict:
    return {
        "min": round(min(values), 3),
        "max": round(max(values), 3),
        "avg": round(sum(values) / len(values), 3),
    }


class Bench:
    def __init__(
        self,
        cfg: Config,
        backend: CliBackend,
        audit: AuditLog | None = None,
        *,
        sleep: Callable[[float], None] = time.sleep,
        clock: Callable[[], float] = time.monotonic,
    ):
        self.cfg = cfg
        self.psu = backend
        self.audit = audit or AuditLog(None)
        self._sleep = sleep
        self._clock = clock
        self._protection_applied = False

    # --- helpers -------------------------------------------------------------

    def _audited(self, tool: str, args: dict, fn: Callable[[], dict]) -> dict:
        try:
            result = fn()
        except Exception as e:
            self.audit.write(tool, args, error=f"{type(e).__name__}: {e}")
            raise
        self.audit.write(tool, args, result=result)
        return result

    def _ensure_protection(self) -> None:
        """Program OVP/OCP from the config before the first command that can energise the load.

        Done lazily rather than at startup so the server still starts (and
        `status` still works) when the PSU is unplugged.
        """
        if self._protection_applied:
            return
        if self.cfg.ovp_V is not None or self.cfg.ocp_A is not None:
            self.psu.limits(ovp=self.cfg.ovp_V, ocp=self.cfg.ocp_A)
        self._protection_applied = True

    def _check_setpoints(self, voltage: float | None, current: float | None) -> None:
        cfg = self.cfg
        if voltage is not None and not 0 <= voltage <= cfg.max_voltage_V:
            raise LimitError(f"voltage {voltage} V is outside the bench limit 0..{cfg.max_voltage_V} V")
        if current is not None and not 0 <= current <= cfg.max_current_A:
            raise LimitError(f"current {current} A is outside the bench limit 0..{cfg.max_current_A} A")

    def _check_present_setpoints(self) -> dict:
        """Refuse to energise if someone set the PSU beyond the limits from the front panel."""
        st = self.psu.status()
        try:
            self._check_setpoints(st["voltage_set_V"], st["current_set_A"])
        except LimitError as e:
            raise LimitError(f"PSU setpoints are outside the bench limits ({e}); call `set` first") from None
        return st

    # --- operations ----------------------------------------------------------

    def status(self) -> dict:
        return self._audited("status", {}, self.psu.status)

    def get_limits(self) -> dict:
        def run():
            hw = self.psu.limits()
            return {
                "hardware": hw,
                "bench": {
                    "max_voltage_V": self.cfg.max_voltage_V,
                    "max_current_A": self.cfg.max_current_A,
                    "ovp_V": self.cfg.ovp_V,
                    "ocp_A": self.cfg.ocp_A,
                },
            }

        return self._audited("get_limits", {}, run)

    def set(self, voltage_V: float | None = None, current_A: float | None = None) -> dict:
        args = {"voltage_V": voltage_V, "current_A": current_A}

        def run():
            if voltage_V is None and current_A is None:
                raise LimitError("give voltage_V and/or current_A")
            self._check_setpoints(voltage_V, current_A)
            self._ensure_protection()
            return self.psu.set(voltage_V, current_A)

        return self._audited("set", args, run)

    def output(self, state: str) -> dict:
        def run():
            if state == "off":
                # Turning off is always allowed and never blocked by other checks.
                return self.psu.output(False)
            if state != "on":
                raise LimitError("state must be 'on' or 'off'")
            self._ensure_protection()
            self._check_present_setpoints()
            return self.psu.output(True)

        return self._audited("output", {"state": state}, run)

    def power_cycle(self, off_ms: int = 1000) -> dict:
        def run():
            cfg = self.cfg
            if not cfg.min_off_ms <= off_ms <= cfg.max_off_ms:
                raise LimitError(f"off_ms must be {cfg.min_off_ms}..{cfg.max_off_ms}")
            # Check before switching off, so we never leave the ECU unpowered
            # because the "on" half would be refused.
            self._ensure_protection()
            self._check_present_setpoints()
            self.psu.output(False)
            self._sleep(off_ms / 1000)
            self.psu.output(True)
            return {"off_ms": off_ms, "output": "on"}

        return self._audited("power_cycle", {"off_ms": off_ms}, run)

    def measure(self, duration_s: float, interval_ms: int = 500, include_samples: bool = False) -> dict:
        args = {"duration_s": duration_s, "interval_ms": interval_ms, "include_samples": include_samples}

        def run():
            cfg = self.cfg
            if not 0 < duration_s <= cfg.max_duration_s:
                raise LimitError(f"duration_s must be > 0 and <= {cfg.max_duration_s}")
            if interval_ms < cfg.min_interval_ms:
                raise LimitError(f"interval_ms must be >= {cfg.min_interval_ms}")

            samples = []
            start = self._clock()
            next_t = start
            while True:
                st = self.psu.status()
                samples.append({
                    "t_s": round(self._clock() - start, 3),
                    "voltage_V": st["voltage_actual_V"],
                    "current_A": st["current_actual_A"],
                    "mode": st["mode"],
                })
                next_t += interval_ms / 1000
                if next_t - start > duration_s:
                    break
                delay = next_t - self._clock()
                if delay > 0:
                    self._sleep(delay)

            elapsed = samples[-1]["t_s"]
            result = {
                "samples": len(samples),
                "elapsed_s": elapsed,
                # Each sample is a full CLI round-trip, so the real spacing can exceed interval_ms.
                "actual_interval_ms": round(elapsed * 1000 / (len(samples) - 1)) if len(samples) > 1 else None,
                "voltage_V": _stats([s["voltage_V"] for s in samples]),
                "current_A": _stats([s["current_A"] for s in samples]),
                "cc_samples": sum(1 for s in samples if s["mode"] == "CC"),
            }
            if include_samples:
                result["data"] = samples
            return result

        return self._audited("measure", args, run)
