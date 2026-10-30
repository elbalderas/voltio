"""Talks to the PSU by running the voltio CLI and parsing its JSON output.

This is the only module that touches hardware. Swapping to a ctypes binding
of the C library later means replacing this file and nothing else.
"""

from __future__ import annotations

import json
import subprocess
import threading


class VoltioError(Exception):
    """The PSU or the CLI reported a failure."""


class CliBackend:
    def __init__(self, cli: list[str], port: str, dp: int, timeout_s: float):
        self._base = [*cli, "--port", port, "--dp", str(dp)]
        self._timeout = timeout_s
        # One serial port, one command at a time.
        self._lock = threading.Lock()

    def _run(self, *args: str) -> dict:
        cmd = [*self._base, *args]
        with self._lock:
            try:
                proc = subprocess.run(cmd, capture_output=True, text=True, timeout=self._timeout)
            except FileNotFoundError:
                raise VoltioError(f"voltio CLI not found: {self._base[0]!r} (check [psu].cli)") from None
            except subprocess.TimeoutExpired:
                raise VoltioError(f"voltio CLI timed out after {self._timeout}s running {' '.join(args)!r}") from None

        # Success JSON goes to stdout, error JSON to stderr.
        text = proc.stdout if proc.returncode == 0 else proc.stderr
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            detail = (proc.stderr or proc.stdout).strip() or "no output"
            raise VoltioError(f"unexpected CLI output (exit {proc.returncode}): {detail}") from None

        if proc.returncode != 0 or data.get("status") != "ok":
            raise VoltioError(data.get("message", f"CLI failed with exit {proc.returncode}"))
        data.pop("status", None)
        return data

    # --- commands ------------------------------------------------------------

    def status(self) -> dict:
        return self._run("status")

    def set(self, voltage: float | None = None, current: float | None = None) -> dict:
        args = []
        if voltage is not None:
            args += ["--voltage", f"{voltage:.3f}"]
        if current is not None:
            args += ["--current", f"{current:.3f}"]
        return self._run("set", *args)

    def output(self, on: bool) -> dict:
        return self._run("output", "on" if on else "off")

    def limits(self, ovp: float | None = None, ocp: float | None = None) -> dict:
        args = []
        if ovp is not None:
            args += ["--ovp", f"{ovp:.3f}"]
        if ocp is not None:
            args += ["--ocp", f"{ocp:.3f}"]
        return self._run("limits", *args)
