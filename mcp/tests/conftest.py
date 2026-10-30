import json
import sys
from pathlib import Path

import pytest

from voltio_mcp.backend import CliBackend
from voltio_mcp.bench import AuditLog, Bench
from voltio_mcp.config import load_config

FAKE_CLI = Path(__file__).with_name("fake_voltio.py")


class FakePSU:
    """Handle on the fake CLI's state file."""

    def __init__(self, path: Path):
        self.path = path

    def state(self) -> dict:
        return json.loads(self.path.read_text()) if self.path.exists() else {}

    def update(self, **kv) -> None:
        self.path.write_text(json.dumps({**self.state(), **kv}))

    def calls(self) -> list[list[str]]:
        return self.state().get("calls", [])


class FakeTime:
    def __init__(self):
        self.now = 0.0
        self.sleeps = []
        self.waits = []

    def clock(self) -> float:
        return self.now

    def sleep(self, s: float) -> None:
        self.sleeps.append(s)
        self.now += s

    def wait(self, ev, t: float) -> bool:
        """Stand-in for Event.wait: time passes, returns whether the event is set."""
        self.waits.append(t)
        self.now += t
        return ev.is_set()


@pytest.fixture
def psu(tmp_path, monkeypatch) -> FakePSU:
    path = tmp_path / "psu.json"
    monkeypatch.setenv("FAKE_VOLTIO_STATE", str(path))
    return FakePSU(path)


def write_config(tmp_path: Path, extra: str = "") -> Path:
    cli = json.dumps([sys.executable, str(FAKE_CLI)])
    cfg = tmp_path / "bench.toml"
    cfg.write_text(f"""
[psu]
cli = {cli}
port = "FAKE"
dp = 1

[limits]
max_voltage_V = 14.0
max_current_A = 3.0
ovp_V = 15.0
ocp_A = 3.5

[audit]
log = "audit.jsonl"
{extra}
""")
    return cfg


@pytest.fixture
def fake_time() -> FakeTime:
    return FakeTime()


@pytest.fixture
def bench(tmp_path, psu, fake_time) -> Bench:
    cfg = load_config(write_config(tmp_path))
    backend = CliBackend(cfg.cli, cfg.port, cfg.dp, cfg.timeout_s)
    return Bench(cfg, backend, AuditLog(cfg.audit_log), sleep=fake_time.sleep, clock=fake_time.clock,
                 wait=fake_time.wait)
