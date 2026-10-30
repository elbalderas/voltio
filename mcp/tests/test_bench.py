import asyncio
import json

import pytest

from voltio_mcp.backend import CliBackend, VoltioError
from voltio_mcp.bench import LimitError
from voltio_mcp.config import ConfigError, load_config
from voltio_mcp.server import create_server

from conftest import write_config


def commands(psu):
    return [c[0] for c in psu.calls()]


# --- limits ------------------------------------------------------------------

def test_set_within_limits(bench, psu):
    assert bench.set(12.0, 2.0) == {"voltage_set_V": 12.0, "current_set_A": 2.0}


@pytest.mark.parametrize("v,a", [(14.1, None), (None, 3.01), (-1.0, None)])
def test_set_outside_limits_never_reaches_psu(bench, psu, v, a):
    with pytest.raises(LimitError):
        bench.set(v, a)
    assert "set" not in commands(psu)


def test_protection_programmed_before_first_write(bench, psu):
    bench.set(12.0, 1.0)
    bench.set(12.5, None)
    assert psu.calls()[0] == ["limits", "--ovp", "15.000", "--ocp", "3.500"]
    assert commands(psu).count("limits") == 1


def test_output_on_refused_when_panel_setpoints_exceed_limits(bench, psu):
    psu.update(v_set=24.0, i_set=1.0)
    with pytest.raises(LimitError, match="call `set` first"):
        bench.output("on")
    assert psu.state()["on"] is False


def test_output_off_always_allowed(bench, psu):
    psu.update(v_set=24.0, i_set=1.0, on=True)  # out of limits, still must turn off
    bench.output("off")
    assert psu.state()["on"] is False
    assert commands(psu) == ["output"]


# --- power cycle ---------------------------------------------------------------

def test_power_cycle_order_and_timing(bench, psu, fake_time):
    psu.update(v_set=12.0, i_set=1.0, on=True)
    assert bench.power_cycle(750) == {"off_ms": 750, "output": "on"}
    outs = [c for c in psu.calls() if c[0] == "output"]
    assert outs == [["output", "off"], ["output", "on"]]
    assert fake_time.sleeps == [0.75]


def test_power_cycle_refused_before_switching_off(bench, psu):
    psu.update(v_set=24.0, i_set=1.0, on=True)
    with pytest.raises(LimitError):
        bench.power_cycle(500)
    assert psu.state()["on"] is True  # ECU left powered, not stranded off


@pytest.mark.parametrize("off_ms", [10, 120_000])
def test_power_cycle_off_time_bounds(bench, psu, off_ms):
    with pytest.raises(LimitError):
        bench.power_cycle(off_ms)


# --- measure ---------------------------------------------------------------------

def test_measure_summary(bench, psu):
    psu.update(v_set=12.0, i_set=1.0, on=True, load_A=0.25)
    r = bench.measure(duration_s=2.0, interval_ms=500)
    assert r["samples"] == 5  # t = 0, 0.5, 1.0, 1.5, 2.0
    assert r["current_A"] == {"min": 0.25, "max": 0.25, "avg": 0.25}
    assert r["voltage_V"]["avg"] == 12.0
    assert r["cc_samples"] == 0
    assert "data" not in r


def test_measure_flags_cc(bench, psu):
    psu.update(v_set=12.0, i_set=0.5, on=True, load_A=2.0)
    r = bench.measure(duration_s=1.0, interval_ms=500, include_samples=True)
    assert r["cc_samples"] == r["samples"]
    assert len(r["data"]) == r["samples"]


@pytest.mark.parametrize("dur,interval", [(0, 500), (301, 500), (1, 50)])
def test_measure_bounds(bench, dur, interval):
    with pytest.raises(LimitError):
        bench.measure(dur, interval)


# --- errors and audit ------------------------------------------------------------

def test_cli_error_message_surfaces(bench, psu):
    psu.update(fail="Timeout waiting for PSU response")
    with pytest.raises(VoltioError, match="Timeout waiting"):
        bench.status()


def test_missing_cli():
    backend = CliBackend(["/nonexistent/voltio"], "FAKE", 1, 1.0)
    with pytest.raises(VoltioError, match="not found"):
        backend.status()


def test_audit_log_records_success_and_refusal(bench, psu, tmp_path):
    bench.set(12.0, 1.0)
    with pytest.raises(LimitError):
        bench.set(30.0, None)
    entries = [json.loads(l) for l in (tmp_path / "audit.jsonl").read_text().splitlines()]
    assert [e["tool"] for e in entries] == ["set", "set"]
    assert entries[0]["result"]["voltage_set_V"] == 12.0
    assert "LimitError" in entries[1]["error"]


# --- config ----------------------------------------------------------------------

def test_config_rejects_ovp_below_max(tmp_path):
    cfg = write_config(tmp_path)
    cfg.write_text(cfg.read_text().replace("ovp_V = 15.0", "ovp_V = 10.0"))
    with pytest.raises(ConfigError, match="ovp_V"):
        load_config(cfg)


def test_config_requires_limits(tmp_path):
    cfg = tmp_path / "bench.toml"
    cfg.write_text('[psu]\nport = "COM4"\n')
    with pytest.raises(ConfigError, match="max_voltage_V"):
        load_config(cfg)


# --- MCP surface -------------------------------------------------------------------

def test_tools_registered(bench):
    server = create_server(bench)
    tools = {t.name: t for t in asyncio.run(server.list_tools())}
    assert set(tools) == {"status", "get_limits", "measure", "set", "output", "power_cycle"}
    assert tools["status"].annotations.read_only_hint is True
    assert tools["set"].annotations.read_only_hint is False


def test_limit_refusal_is_tool_error(bench, psu):
    server = create_server(bench)
    with pytest.raises(Exception, match="outside the bench limit"):
        asyncio.run(server.call_tool("set", {"voltage_V": 99.0}))
    assert "set" not in commands(psu)
