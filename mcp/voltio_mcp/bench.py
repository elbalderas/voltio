"""PSU operations with the bench safety rules applied.

Every limit here is enforced in code, so it holds no matter what the agent
is prompted to do. Every call is written to the audit log.
"""

from __future__ import annotations

import json
import logging
import math
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

from .backend import CliBackend, VoltioError
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


def _quantize(value: float, scale: int) -> float:
    """Round to the PSU's resolution the way the C encoder does (round half up)."""
    return math.floor(value * scale + 0.5) / scale


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
        wait: Callable[[threading.Event, float], bool] = lambda ev, t: ev.wait(t),
    ):
        self.cfg = cfg
        self.psu = backend
        self.audit = audit or AuditLog(None)
        self._sleep = sleep
        self._clock = clock
        self._wait = wait
        self._protection_applied = False
        # Serializes writes that can energise the load. `output off` never takes
        # it: it sets _abort instead, so it can't wait behind a power cycle.
        self._write_lock = threading.Lock()
        self._abort = threading.Event()

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
        # Every caller (set, output on, power_cycle) holds _write_lock, so this check-then-act can't race.
        if self._protection_applied:
            return
        if self.cfg.ovp_V is not None or self.cfg.ocp_A is not None:
            self.psu.limits(ovp=self.cfg.ovp_V, ocp=self.cfg.ocp_A)
        self._protection_applied = True

    def _verify_protection(self) -> None:
        """Re-program OVP/OCP if they no longer match the config (e.g. changed from the front panel)."""
        cfg = self.cfg
        if cfg.ovp_V is None and cfg.ocp_A is None:
            return
        hw = self.psu.limits()
        drifted = (
            (cfg.ovp_V is not None and abs(hw["ovp_V"] - cfg.ovp_V) > 0.001)
            or (cfg.ocp_A is not None and abs(hw["ocp_A"] - cfg.ocp_A) > 0.001)
        )
        if drifted:
            self.psu.limits(ovp=cfg.ovp_V, ocp=cfg.ocp_A)
            self.audit.write("protection_reasserted", {"was": hw})

    def _check_setpoints(self, voltage: float | None, current: float | None,
                         requested: tuple[float | None, float | None] = (None, None)) -> None:
        """Check setpoints against the bench caps.

        `requested` holds the values before quantization, so a refusal can say
        when rounding to device resolution is what pushed a value over the cap.
        """
        cfg = self.cfg

        def show(value, req, unit):
            if req is None or req == value:
                return f"{value} {unit}"
            return f"{req} {unit} ({value} at device resolution)"

        if voltage is not None and not 0 <= voltage <= cfg.max_voltage_V:
            raise LimitError(f"voltage {show(voltage, requested[0], 'V')} is outside the bench limit "
                             f"0..{cfg.max_voltage_V} V")
        if current is not None and not 0 <= current <= cfg.max_current_A:
            raise LimitError(f"current {show(current, requested[1], 'A')} is outside the bench limit "
                             f"0..{cfg.max_current_A} A")

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
            with self._write_lock:
                if voltage_V is None and current_A is None:
                    raise LimitError("give voltage_V and/or current_A")
                # Check what the PSU will actually be programmed to, not the raw request:
                # with a 13.95 V cap, 13.95 would otherwise pass and be encoded as 14.0.
                v = None if voltage_V is None else _quantize(voltage_V, 10)
                a = None if current_A is None else _quantize(current_A, 100 if self.cfg.dp == 2 else 10)
                self._check_setpoints(v, a, requested=(voltage_V, current_A))
                self._ensure_protection()
                return self.psu.set(v, a)

        return self._audited("set", args, run)

    def output(self, state: str) -> dict:
        def run():
            if state == "off":
                # Turning off is always allowed and never blocked by other checks or
                # by the write lock; it also aborts an in-flight power cycle.
                self._abort.set()
                return self.psu.output(False)
            if state != "on":
                raise LimitError("state must be 'on' or 'off'")
            with self._write_lock:
                self._ensure_protection()
                self._verify_protection()
                self._check_present_setpoints()
                return self.psu.output(True)

        return self._audited("output", {"state": state}, run)

    def power_cycle(self, off_ms: int = 1000) -> dict:
        def run():
            cfg = self.cfg
            if not cfg.min_off_ms <= off_ms <= cfg.max_off_ms:
                raise LimitError(f"off_ms must be {cfg.min_off_ms}..{cfg.max_off_ms}")
            with self._write_lock:
                # Only an `output off` issued during this cycle may abort it.
                self._abort.clear()
                # Check before switching off, so we never leave the ECU unpowered
                # because the "on" half would be refused.
                self._ensure_protection()
                self._verify_protection()
                self._check_present_setpoints()
                self.psu.output(False)
                aborted = {"off_ms": off_ms, "output": "off", "aborted": True}
                if self._wait(self._abort, off_ms / 1000):
                    return aborted
                try:
                    self.psu.output(True)
                except VoltioError as e:
                    # A serial hiccup must not strand the ECU unpowered: retry once.
                    self._sleep(0.2)
                    if self._abort.is_set():  # `output off` arrived meanwhile; honour it
                        return aborted
                    try:
                        self.psu.output(True)
                    except VoltioError:
                        raise VoltioError(f"{e}; PSU output is now OFF — the power cycle did not complete") from None
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
