# voltio-mcp

An MCP server that lets a coding agent (opencode, Claude Code, anything that
speaks MCP) control a BK Precision 1900B-series PSU through the `voltio` CLI.

The safety rules live in the server, not in the agent's prompt:

- **Bench limits.** Setpoints above `max_voltage_V` / `max_current_A` are refused
  before anything reaches the PSU. Values are first rounded to the PSU's resolution
  (0.1 V; 0.1 A, or 0.01 A on the 1685B), so a request that would be programmed above
  a cap is refused too.
- **Hardware protection.** OVP/OCP from the config are programmed before the first
  command that can energise the load.
- **No surprise energising.** `output on` and `power_cycle` are refused if the PSU's
  current setpoints (for example, set from the front panel) exceed the limits.
  `power_cycle` checks *before* switching off, so it never leaves the ECU unpowered.
- **Off is always allowed.** `output off` aborts an in-flight `power_cycle`; the
  output then stays off.
- **Audit log.** Every call, refusal and error is written as JSON Lines (`measure`
  samples are capped at the first 10, with `data_truncated` giving the total).

## Tools

| Tool | Kind | What it does |
|---|---|---|
| `status` | read | Setpoints, measured V/I/W, mode (CV or CC) |
| `get_limits` | read | PSU OVP/OCP, the model's max voltage/current (`device_max`), and the bench limits |
| `measure(duration_s, interval_ms=500, include_samples=false)` | read | Samples V/I over time; returns min/max/avg and a count of CC samples. Cancelling the call stops the sampling; a run cut short returns `stopped_early: true` |
| `set(voltage_V?, current_A?)` | write | Changes setpoints within the bench limits |
| `output(state)` | write | `"on"` / `"off"` |
| `power_cycle(off_ms=1000)` | write | Off, wait, on. An `output off` during the wait aborts it (`aborted: true`, output stays off) |

Read tools carry `readOnlyHint: true` and write tools `destructiveHint: true`, so
clients that understand MCP annotations can treat them differently. `power_cycle`
is also marked `idempotentHint: false`: two calls are two resets.

## Install

You need Python 3.11+ and a built `voltio` CLI (see the top-level README).

```sh
# from the repo root
pipx install ./mcp          # or: uv tool install ./mcp
```

With no internet access on the bench PC, build a wheel somewhere else and copy it
over. The only runtime dependency is `mcp`.

```sh
cd mcp && python -m build --wheel     # or: uv build
pipx install dist/voltio_mcp-0.1.0-py3-none-any.whl
```

## Configure

Copy `bench.example.toml` outside the repo (one per bench) and edit the port, `dp`
and limits for your ECU. Then:

```sh
voltio-mcp --config C:/bench/voltio.toml
# or set VOLTIO_MCP_CONFIG and run plain `voltio-mcp`
```

The server speaks MCP on stdio by default. If the agent runs on another machine
than the bench, use `--transport streamable-http --host 127.0.0.1 --http-port 8765`
and reach it through an SSH tunnel (recommended):

```sh
ssh -L 8765:127.0.0.1:8765 bench-pc     # then point the agent at http://127.0.0.1:8765/mcp
```

HTTP security:

- **Loopback only by default.** A non-loopback `--host` (e.g. `0.0.0.0`) is refused
  unless you also pass `--allow-remote`.
- **Host/Origin checks.** On a loopback bind, requests with a foreign `Host` or
  `Origin` header are rejected, so a web page open on the bench PC can't drive the PSU.
- **Bearer token.** Set `VOLTIO_MCP_TOKEN` (or pass `--http-token`) and every HTTP
  request must carry `Authorization: Bearer <token>`. Strongly recommended with
  `--allow-remote`; the server warns if it's missing.

None of this applies to stdio.

## opencode

See `examples/opencode/`:

- `opencode.json` registers the server and **turns `voltio_*` tools off by default**.
- `agent/bench.md` is a `bench` agent that turns them back on, plus the bench rules.
  Copy it into `.opencode/agent/` (project) or `~/.config/opencode/agent/` (global).

That way your everyday coding agent can't touch the PSU; you switch to the `bench`
agent on purpose. Check your opencode version's docs to see whether MCP tools can
also be set to ask before each call. If they can, set the write tools (`voltio_set`,
`voltio_output`, `voltio_power_cycle`) to ask.

## Claude Code

```sh
claude mcp add voltio -- voltio-mcp --config /path/to/bench.toml
```

## Serial port ownership

Only one process can have the COM port open. While CANoe holds it through
`voltio.dll`, this server's calls fail with a port error (and the reverse). Close
the port in CANoe, or let CANoe drive the PSU during test runs.

## Develop

```sh
cd mcp
python -m venv .venv && .venv/bin/pip install -e '.[test]'
.venv/bin/pytest
```

The tests use `tests/fake_voltio.py`, a stand-in CLI with the same arguments and
JSON output, so no PSU is needed. To try the server by hand:

```sh
npx @modelcontextprotocol/inspector .venv/bin/voltio-mcp --config bench.toml
```
