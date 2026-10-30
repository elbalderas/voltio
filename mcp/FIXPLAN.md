# voltio-mcp Fix Plan

Fixes from a code review of the MCP server (`mcp/`). Implement the numbered
items in the order given in §13. This file is the spec; delete it in the
final commit.

## 0. Scope and ground rules

- All work is in `mcp/`, on this branch (`fix/mcp-review`, off `feat/mcp-server`).
- **Run tests:** `cd mcp && .venv/bin/pytest` (create the venv per `mcp/README.md`
  if missing). No PSU needed; tests use `tests/fake_voltio.py`.
- **One commit per numbered item**, tests included, suite green after each.
  Fake-CLI knobs from §12 go in the commit of the item that first needs them.
  Do not move on while the suite is red.
- **Do not change:** the C code (`src/`), the CLI's JSON field names (contract
  with `backend.py`/`bench.py`/`fake_voltio.py`), or the semantics of existing
  tests except where an item explicitly says to update one.
- **No new runtime dependencies.** `anyio` and `starlette` already ship with
  the `mcp` SDK. Python >= 3.11.
- **The design decisions below are settled.** If something turns out to be
  impossible as specified, stop and ask rather than substituting your own design.
- Line numbers refer to the files at commit `c7a88bc` (this branch's base).

## 1. Bench-level write serialization + abortable `power_cycle`

**Problem.** Only individual CLI calls are locked (`backend.py` `_lock`).
`Bench.power_cycle` (`bench.py:157-168`) holds nothing during its sleep (up to
60 s). MCP sync tools run on worker threads, so a concurrent `output off`
during the off-window is silently overridden when the cycle's "on" half runs.
Writes can also interleave generally.

**Change (`bench.py`).**
- Add to `Bench.__init__`: `self._write_lock = threading.Lock()` and
  `self._abort = threading.Event()`.
- Add an injectable wait seam: constructor keyword
  `wait: Callable[[threading.Event, float], bool] = lambda ev, t: ev.wait(t)`,
  stored as `self._wait`. (Needed so tests control time, like the existing
  `sleep`/`clock` seams.)
- `set()` and `output("on")`: run their `run()` bodies under
  `with self._write_lock:`.
- `output("off")`: must **never** block on `_write_lock`. It calls
  `self._abort.set()` first, then `self.psu.output(False)` directly (the
  backend lock still serializes the serial port, so the worst-case wait is one
  CLI round-trip).
- `power_cycle()`: body under `_write_lock`. At the top (inside the lock),
  `self._abort.clear()`. Replace `self._sleep(off_ms / 1000)` with
  `aborted = self._wait(self._abort, off_ms / 1000)`. If `aborted`, do **not**
  switch back on; return `{"off_ms": off_ms, "output": "off", "aborted": True}`.
  Otherwise proceed as today and return `{"off_ms": off_ms, "output": "on"}`
  (unchanged shape; do not add `"aborted": False`).
- `measure()` stays lock-free (read-only by design — an emergency off must not
  wait behind it).

**Tests (`tests/test_bench.py`, `tests/conftest.py`).**
- Extend `FakeTime` (or add a fixture) with a fake `wait(ev, t)` that records
  `t` and returns `ev.is_set()`; pass it into the `bench` fixture. Update
  `test_power_cycle_order_and_timing` to assert the recorded wait is `[0.75]`
  instead of `fake_time.sleeps`.
- New: abort test — inject a `wait` that calls `bench.output("off")` itself and
  then returns `True`. Assert: result has `"aborted": True` and
  `"output": "off"`; PSU state `on is False`; the fake's call list contains no
  `["output", "on"]` after the cycle's `["output", "off"]`. (This also proves
  `output("off")` doesn't deadlock while `power_cycle` holds the write lock.)
- New: stale-abort test — call `bench.output("off")`, then a full
  `power_cycle` with a wait that returns `ev.is_set()`; assert it completes
  with `"output": "on"` (the event was cleared at cycle start).

**Docs.** README bullet under safety rules: "`output off` aborts an in-flight
`power_cycle`; the output then stays off."

## 2. Retry + explicit state when the `power_cycle` "on" half fails

**Problem.** A `VoltioError` at `bench.py:168` (CLI timeout, serial hiccup)
leaves the ECU unpowered with a generic error, despite the design goal "never
leave the ECU unpowered".

**Change (`bench.py`, inside `power_cycle.run`).**
- Wrap the final `self.psu.output(True)` in `try/except VoltioError`. On
  failure: `self._sleep(0.2)`, retry once. If the retry also fails, raise
  `VoltioError(f"{original error}; PSU output is now OFF — the power cycle did
  not complete")` so the agent knows the bench state without probing.

**Tests.** Needs per-command failure in the fake — see §12 (`fail_cmd`,
`fail_times`).
- Fail `output on` once (`fail_cmd="output on"`, `fail_times=1`):
  `power_cycle` succeeds, result `{"output": "on", ...}`, fake state
  `on is True`.
- Fail `output on` persistently: `pytest.raises(VoltioError,
  match="output is now OFF")`, fake state `on is False`.

## 3. Quantize setpoints to device resolution before the limit check

**Problem.** The bench check compares raw floats, but the C encoder rounds to
the nearest LSB (`src/voltio.c:321`, `floor(val*scale + 0.5)`). With
`max_voltage_V = 13.95`, a request of 13.95 passes the check and the PSU is
programmed to **14.0 V** — above the cap.

**Change (`bench.py`).**
- Resolutions: voltage scale **10** always; current scale **10** for `dp == 1`,
  **100** for `dp == 2` (`cfg.dp` exists).
- Helper mirroring the C encoder exactly (round-half-up, not Python banker's
  rounding): `counts = math.floor(value * scale + 0.5); return counts / scale`.
- In `Bench.set()`: quantize `voltage_V`/`current_A` first, run
  `_check_setpoints` on the **quantized** values, and pass the quantized values
  to `self.psu.set(...)`. Refusal messages must mention quantization when it
  changed the value, e.g. `voltage 13.95 V (14.0 at device resolution) is
  outside ...` — exact wording free.
- `_check_present_setpoints` needs no change (the PSU reports
  already-quantized values).

**Tests.**
- Config with `max_voltage_V = 13.95`: `bench.set(13.95, None)` →
  `LimitError`, and `"set"` never reaches the fake; `bench.set(13.9, None)` →
  ok. Use `write_config(tmp_path, ...)` with a replaced limits block (same
  pattern as `test_config_rejects_ovp_below_max`).
- dp=2 config: `bench.set(None, 1.004)` sends `--current 1.000` (assert via
  the fake's call list).

## 4. Audit-log failure policy: fail-open at call time, verify at startup

**Problem.** `AuditLog.write` (`bench.py:28-45`) does `mkdir` + `open` on every
call. If it raises: in `_audited`'s success path a *succeeded* PSU action is
reported as an error; in the except path (`bench.py:79`) the audit exception
**replaces** the real `LimitError`/`VoltioError`.

**Change.**
- `AuditLog.write`: wrap the whole body in `try/except Exception`. On failure,
  log a warning to the `voltio_mcp` logger (stderr) on the **first** failure
  only; set a `self._degraded` flag and stay quiet until a write succeeds again
  (then clear the flag). Never raise.
- `AuditLog.verify()` (new): creates the parent directory and opens the file
  for append (touch), **raising** `OSError` on failure. No-op when path is
  `None`.
- `server.py:main`: after constructing `AuditLog`, call `verify()`; on
  `OSError`, `p.error(f"audit log not writable: {e}")` (fail-fast at startup,
  where it's fixable). Then write one startup entry:
  `audit.write("server_start", {"port": cfg.port, "max_voltage_V": ...,
  "max_current_A": ..., "ovp_V": ..., "ocp_A": ...})`.
- `_audited` itself stays unchanged.

**Tests.**
- Unwritable log (point `audit_log` at a path whose parent is an existing
  *file*): `bench.set(12.0, 1.0)` still succeeds and raises nothing; a refused
  `bench.set(30.0, None)` still raises `LimitError` (not `OSError`).
- `AuditLog.verify()` raises `OSError` for that same path; succeeds for a
  normal tmp path and creates parent dirs.

## 5. streamable-http security

**Problem.** `server.py:102-103` only *warns* on non-loopback binds. No auth at
all; and unless the SDK validates `Origin`, a web page in a browser on the
bench PC can drive the PSU via `fetch("http://localhost:8765/...")`
(DNS-rebinding/CSRF class).

**Change (`server.py`).**
- Non-loopback bind now **requires** a new `--allow-remote` flag; without it,
  `p.error("refusing to bind {host} without --allow-remote")`. Keep a warning
  when `--allow-remote` is used without a token.
- Token auth: `--http-token` argument, defaulting to
  `os.environ.get("VOLTIO_MCP_TOKEN")`. When set, every HTTP request must carry
  `Authorization: Bearer <token>`; compare with `hmac.compare_digest`. Reject
  with 401 otherwise.
- Origin/Host validation: **investigate the installed `mcp` 2.x SDK first**
  (`python -c "import mcp; print(mcp.__version__)"`, then read the installed
  sources under `.venv`). The 1.x SDK had `TransportSecuritySettings`
  (`allowed_hosts`, `allowed_origins`) — if the 2.x `MCPServer` exposes an
  equivalent, enable it with loopback hosts/origins. For the token (and as
  fallback for origin checks), get the underlying Starlette/ASGI app (1.x:
  `streamable_http_app()`) and add a small pure-ASGI middleware. Adapt to
  whatever the 2.x API actually is — do not guess; read the SDK.
- stdio transport: untouched by all of this (the token/origin logic must only
  run for streamable-http).

**Tests.**
- `main(["--config", cfg, "--transport", "streamable-http", "--host",
  "0.0.0.0"])` → `SystemExit` (argparse `error` exits 2) before any server
  starts. (Backend construction is side-effect-free, so this is safe to
  invoke.)
- If the middleware route is taken: unit-test it with
  `starlette.testclient.TestClient` — 401 without/with wrong token,
  pass-through with the right token. If the SDK's own transport-security
  settings are used instead, a test asserting they're configured (object
  inspection) is acceptable.

**Docs.** README "Configure" section: document `VOLTIO_MCP_TOKEN`,
`--allow-remote`, and keep the SSH-tunnel recommendation.

## 6. `measure`: cooperative cancellation + audit truncation

**Problem.** `bench.measure` (`bench.py:173`) can run 300 s as a *sync* tool:
the SDK can't cancel it, and after a client timeout the loop keeps hammering
the serial port. Separately, with `include_samples=True` the audit log stores
the full sample array (up to ~1200 entries).

**Change.**
- `Bench.measure` gains `stop: threading.Event | None = None`. Check
  `stop.is_set()` at the top of each loop iteration **and** use it for the
  inter-sample delay (via the same injectable `wait` seam from §1, instead of
  `self._sleep(delay)` — keep `self._sleep` for callers that pass no event).
  When stopped early with >= 1 sample, return the normal stats plus
  `"stopped_early": True`; with 0 samples collected, raise
  `VoltioError("measurement cancelled")`.
- `server.py`: make the `measure` tool `async`. Create a `threading.Event`,
  run `bench.measure(..., stop=ev)` via `anyio.to_thread.run_sync`, and wrap in
  `try/except anyio.get_cancelled_exc_class(): ev.set(); raise` so client
  cancellation stops the serial polling promptly. Check how the 2.x SDK
  dispatches sync vs async tools before assuming `to_thread` is needed — if the
  SDK already runs sync tools in cancellable threads with a documented hook,
  use that instead.
- Progress notifications (`ctx.report_progress`) are a **stretch goal**: only
  if the 2.x SDK exposes a tool `Context` that can be bridged from the worker
  thread without contortions. Skip otherwise.
- Audit truncation: give `_audited` an optional
  `redact: Callable[[dict], dict] | None` applied only to what's written to
  the audit log (never to the returned result). `measure` passes a redactor
  that caps `"data"` at the first 10 samples and adds
  `"data_truncated": <total>` when it cut anything.

**Tests.**
- Stop mid-run: inject a `wait` that sets the event after the 2nd sample;
  assert `stopped_early is True`, `samples == 2`, stats computed over 2
  samples.
- Audit truncation: `measure(duration_s=10, interval_ms=500,
  include_samples=True)` with fake time → returned `data` has all samples; the
  audit entry's `data` has 10 and `data_truncated` equals the total.
- Async cancellation wiring: optional; if tested, use `anyio` directly to
  cancel the tool coroutine and assert the event got set.

## 7. `shlex` mangles Windows paths

**Problem.** `config.py:65` uses `shlex.split(cli, posix=True)`:
`cli = 'C:\tools\voltio.exe'` becomes `C:toolsvoltio.exe`. Bench PCs are
Windows.

**Change (`config.py`).** `import os`;
`shlex.split(cli, posix=(os.name != "nt"))`. In `bench.example.toml`, add a
comment: on Windows prefer the list form for paths with backslashes or spaces,
e.g. `cli = ['C:\tools\voltio.exe']` (TOML literal string).

**Test.** `monkeypatch.setattr("voltio_mcp.config.os.name", "nt")` (patch
however `os` is referenced), load a config with a backslash path and assert
`cfg.cli` keeps the backslashes intact.

## 8. Config validation gaps

**Problem.** `timeout_s` (`config.py:111`) accepts `0`/negative (breaks
`subprocess.run`); `dp = 1.0` passes `dp in (1, 2)` via float equality and
later reaches the CLI as `--dp 1.0`, which its integer parser rejects — at
runtime instead of config load.

**Change (`config.py`).**
- Compute `timeout_s` before constructing `Config`; `ConfigError` unless `> 0`.
- `dp`: `ConfigError` unless `isinstance(dp, int) and not isinstance(dp, bool)
  and dp in (1, 2)`.

**Tests.** `timeout_s = 0` → `ConfigError`; `dp = 1.0` → `ConfigError`;
`dp = true` → `ConfigError`. (Pattern: write a minimal TOML per case,
`pytest.raises(ConfigError, match=...)`.)

## 9. Translate CLI-contract drift into `ToolError`

**Problem.** `_check_present_setpoints` (`bench.py:110`) and `measure` index
`st["voltage_set_V"]` etc. A missing field raises `KeyError`, which `_call`
(`server.py:36-40`) doesn't catch — the agent sees a raw internal error.

**Change (`server.py`).** In `_call`, also catch `KeyError` and raise
`ToolError(f"unexpected CLI response: missing field {e}")`.

**Test.** Using the fake's new `omit` knob (§12): set
`omit=["voltage_set_V"]`, call the `output` tool with `state="on"` through
`create_server(bench)` / `server.call_tool` (same pattern as
`test_limit_refusal_is_tool_error`); assert the raised error message mentions
the missing field and is not a bare `KeyError`.

## 10. OVP/OCP drift reconciliation

**Problem.** `_ensure_protection` (`bench.py:88-94`) programs OVP/OCP once per
process. A later front-panel change is never noticed or corrected.

**Change (`bench.py`).**
- New `_verify_protection()`: no-op when neither `cfg.ovp_V` nor `cfg.ocp_A`
  is configured. Otherwise read `hw = self.psu.limits()` and compare each
  configured value against `hw["ovp_V"]`/`hw["ocp_A"]` with tolerance `0.001`;
  on drift, re-program via `self.psu.limits(ovp=cfg.ovp_V, ocp=cfg.ocp_A)` and
  write an audit entry `("protection_reasserted", {"was": hw})` (use
  `self.audit.write` directly).
- Call it from the two energize paths — `output("on")` and `power_cycle` —
  right after `_ensure_protection()`. `set()` keeps only the cheap
  `_ensure_protection()` (don't add a serial round-trip to every setpoint
  change).
- While here: `_ensure_protection`'s check-then-act is now under the §1 write
  lock for all callers, which fixes its benign race for free — verify that's
  true and leave a one-line comment.

**Tests.** `bench.set(12.0, 1.0)` (programs 15.0/3.5) →
`psu.update(ovp=25.0)` (front-panel drift) → `bench.output("on")` → assert
fake state `ovp == 15.0` again and the audit log contains a
`protection_reasserted` entry. Also: a second `output("on")` without drift
issues no extra `limits` **write** (reads are fine — assert no `limits` call
with `--ovp` args).

## 11. Minor items

- **`power_cycle` annotation** (`server.py:33`): it is not idempotent (two
  calls = two resets). Give it its own
  `ToolAnnotations(read_only_hint=False, destructive_hint=True,
  idempotent_hint=False, open_world_hint=False)`; keep the shared `WRITE` for
  `set`/`output`. Update `test_tools_registered` to assert it.
- **Device max in `get_limits`** *(optional)*: add `CliBackend.max()` →
  `self._run("max")` (the real CLI's `max` command returns
  `max_voltage_V`/`max_current_A` — verified in `src/voltio_cli.c:205-211`);
  `get_limits` adds a `"device_max"` key. Requires the fake to implement `max`
  (§12). Update the README tool table.
- **README**: update for `--allow-remote`, `VOLTIO_MCP_TOKEN`,
  aborted-power-cycle semantics, `measure` `stopped_early`.

## 12. Fake CLI test-harness extensions (`tests/fake_voltio.py`)

Test knobs, not contract changes — the real CLI is untouched. Keep the
existing `fail` behavior. New optional state keys:

| Key | Behavior |
|---|---|
| `fail_cmd` | Fail only when `" ".join([cmd, *args])` starts with this string (e.g. `"output on"`). |
| `fail_times` | Int; with `fail_cmd`/`fail`, fail this many times then succeed (decrement in the state file). Absent = always fail. |
| `omit` | List of field names to delete from the `status` command's JSON output. |
| `max` command | New command branch returning `ok(max_voltage_V=st["max_V"], max_current_A=st["max_A"])`; add `"max_V": 60.0, "max_A": 5.0` to `DEFAULTS`. Only needed if the optional `get_limits` extension in §11 is done — field names must match `cmd_max` in the real CLI. |

The module docstring lists the state keys — keep it updated.

## 13. Order, commits, acceptance

**Order** (dependencies first): §8 → §7 → §9 → §4 → §3 → §1 → §2 (builds on
§1's seam) → §10 (uses §1's lock) → §6 → §5 → §11. Fake-CLI knobs from §12 as
each item needs them.

**Acceptance checklist** (go through it line by line when done and report
each):
- [ ] `cd mcp && .venv/bin/pytest` fully green; every new behavior above has a test.
- [ ] No changes under `src/`, no CLI JSON field renames, no new runtime deps in `pyproject.toml`.
- [ ] `output off` is never blocked by, and always aborts, an in-flight `power_cycle`.
- [ ] A setpoint that quantizes above a bench cap is refused before any CLI call.
- [ ] An unwritable audit log fails the server at startup but never corrupts a tool result at runtime.
- [ ] streamable-http refuses non-loopback binds without `--allow-remote`; stdio behavior unchanged.
- [ ] README and `bench.example.toml` reflect the new flags and semantics.
- [ ] This file (`mcp/FIXPLAN.md`) deleted in the final commit.
