"""MCP tool definitions and the `voltio-mcp` entry point."""

from __future__ import annotations

import argparse
import logging
import os
import sys
from typing import Literal

from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.types import ToolAnnotations

from .backend import CliBackend, VoltioError
from .bench import AuditLog, Bench, LimitError
from .config import ConfigError, load_config

log = logging.getLogger("voltio_mcp")

INSTRUCTIONS = """\
Controls a bench power supply that feeds the device under test (an ECU).
- `status` shows setpoints, measured V/I and mode. mode "CC" means the PSU is
  current-limiting: usually a short, a stuck ECU, or a current limit set too low.
- Voltage/current writes are capped by bench limits the server enforces; a refused
  request is not a bug to work around.
- Never switch the output off or power-cycle while a debugger is flashing the target.
- If anything looks wrong, `output` with state "off" is always allowed.
"""

READ_ONLY = ToolAnnotations(read_only_hint=True, destructive_hint=False, idempotent_hint=True, open_world_hint=False)
WRITE = ToolAnnotations(read_only_hint=False, destructive_hint=True, idempotent_hint=True, open_world_hint=False)


def _call(fn, *args, **kwargs) -> dict:
    try:
        return fn(*args, **kwargs)
    except (LimitError, VoltioError) as e:
        raise ToolError(str(e)) from None
    except KeyError as e:
        # The CLI's JSON is a contract; a missing field means it drifted.
        raise ToolError(f"unexpected CLI response: missing field {e}") from None


def create_server(bench: Bench) -> MCPServer:
    mcp = MCPServer("voltio", instructions=INSTRUCTIONS)

    @mcp.tool(name="status", annotations=READ_ONLY)
    def status() -> dict:
        """Read the PSU: voltage/current setpoints, measured voltage, current and power, and mode (CV or CC)."""
        return _call(bench.status)

    @mcp.tool(name="get_limits", annotations=READ_ONLY)
    def get_limits() -> dict:
        """Read the PSU's OVP/OCP protection limits and the bench limits this server enforces."""
        return _call(bench.get_limits)

    @mcp.tool(name="measure", annotations=READ_ONLY)
    def measure(duration_s: float, interval_ms: int = 500, include_samples: bool = False) -> dict:
        """Sample measured voltage and current over time and return min/max/avg.

        Use for sleep-current checks, inrush and load behaviour. Each sample is a
        serial round-trip, so the real spacing may be longer than interval_ms
        (see actual_interval_ms). Set include_samples to get every point.
        """
        return _call(bench.measure, duration_s, interval_ms, include_samples)

    @mcp.tool(name="set", annotations=WRITE)
    def set_(voltage_V: float | None = None, current_A: float | None = None) -> dict:
        """Set the output voltage (V) and/or current limit (A). Values beyond the bench limits are refused."""
        return _call(bench.set, voltage_V, current_A)

    @mcp.tool(name="output", annotations=WRITE)
    def output(state: Literal["on", "off"]) -> dict:
        """Switch the PSU output on or off. 'off' is always allowed; 'on' is refused if the setpoints exceed the bench limits."""
        return _call(bench.output, state)

    @mcp.tool(name="power_cycle", annotations=WRITE)
    def power_cycle(off_ms: int = 1000) -> dict:
        """Switch the output off, wait off_ms milliseconds, and switch it back on. Use to reset the ECU."""
        return _call(bench.power_cycle, off_ms)

    return mcp


def main(argv: list[str] | None = None) -> None:
    p = argparse.ArgumentParser(prog="voltio-mcp", description="MCP server for a BK Precision 1900B-series PSU")
    p.add_argument("--config", default=os.environ.get("VOLTIO_MCP_CONFIG"),
                   help="bench config TOML (default: $VOLTIO_MCP_CONFIG)")
    p.add_argument("--transport", choices=["stdio", "streamable-http"], default="stdio")
    p.add_argument("--host", default="127.0.0.1", help="HTTP bind address (streamable-http only)")
    p.add_argument("--http-port", type=int, default=8765, help="HTTP port (streamable-http only)")
    args = p.parse_args(argv)

    # stdout carries the MCP protocol on stdio; logs must go to stderr.
    logging.basicConfig(level=logging.INFO, stream=sys.stderr, format="voltio-mcp: %(message)s")

    if not args.config:
        p.error("no config: pass --config or set VOLTIO_MCP_CONFIG")
    try:
        cfg = load_config(args.config)
    except ConfigError as e:
        p.error(str(e))

    if args.transport == "streamable-http" and args.host not in ("127.0.0.1", "localhost", "::1"):
        log.warning("listening on %s without authentication: anyone who can reach this port can control the PSU",
                    args.host)

    backend = CliBackend(cfg.cli, cfg.port, cfg.dp, cfg.timeout_s)
    audit = AuditLog(cfg.audit_log)
    try:
        audit.verify()
    except OSError as e:
        p.error(f"audit log not writable: {e}")
    audit.write("server_start", {
        "port": cfg.port,
        "max_voltage_V": cfg.max_voltage_V,
        "max_current_A": cfg.max_current_A,
        "ovp_V": cfg.ovp_V,
        "ocp_A": cfg.ocp_A,
    })
    bench = Bench(cfg, backend, audit)
    log.info("PSU on %s, limits %.2f V / %.2f A, audit log %s",
             cfg.port, cfg.max_voltage_V, cfg.max_current_A, cfg.audit_log or "disabled")

    server = create_server(bench)
    if args.transport == "stdio":
        server.run()
    else:
        server.run("streamable-http", host=args.host, port=args.http_port)


if __name__ == "__main__":
    main()
