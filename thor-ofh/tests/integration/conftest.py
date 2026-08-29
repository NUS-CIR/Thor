#!/usr/bin/env python3

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.

"""pytest fixtures for the veth-based integration tests."""

from __future__ import annotations

import os
import subprocess
import tempfile
import uuid
from pathlib import Path

import pytest

import thor_testenv as env
from thor_testenv import (  # noqa: F401  (re-exported for the test modules)
    DU0_MAC,
    DU0_VLAN,
    DU1_MAC,
    DU1_VLAN,
    DU2_MAC,
    DU2_VLAN,
    NUM_PRBS,
    RU_MAC,
    RU_VLAN,
    UNKNOWN_MAC,
)


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: takes more than a couple of seconds")


def _sweep_stray_proxies() -> int:
    """
    Kill any proxy left over from an earlier run.

    Every proxy busy-polls the same lcore, so a stray one starves each new
    proxy during EAL init and makes unrelated tests fail with "did not become
    ready". Returns how many were killed.
    """
    found = subprocess.run(["pgrep", "-f", str(env.PROXY_BIN)],
                           capture_output=True, text=True)
    pids = [p for p in found.stdout.split() if p]
    if pids:
        subprocess.run(["pkill", "-9", "-f", str(env.PROXY_BIN)], check=False)
    return len(pids)


@pytest.fixture(scope="session", autouse=True)
def _preflight():
    env.require_root()
    env.check_tools()
    if not env.PROXY_BIN.exists():
        pytest.exit(f"{env.PROXY_BIN} not built -- run tests/build.sh", returncode=1)

    killed = _sweep_stray_proxies()
    if killed:
        print(f"\nswept {killed} stray proxy process(es) before starting")

    yield

    _sweep_stray_proxies()


@pytest.fixture(autouse=True)
def _no_stray_proxies(_preflight):
    """Fail the test that leaks a proxy, rather than the innocent ones after it."""
    yield
    leaked = _sweep_stray_proxies()
    assert leaked == 0, (
        f"{leaked} proxy process(es) outlived this test; they would have "
        f"starved the lcore for every test that follows"
    )


@pytest.fixture(scope="session")
def veth(_preflight):
    """One veth pair for the whole session; the proxy is restarted per test."""
    pair = env.VethPair()
    pair.setup()
    yield pair
    pair.teardown()


@pytest.fixture
def middlebox_mac(veth) -> str:
    return veth.middlebox_mac


@pytest.fixture
def ctrl_sock_path(tmp_path) -> str:
    # Keep well inside sockaddr_un's 108-byte sun_path.
    return str(Path(tempfile.gettempdir()) / f"thor-{uuid.uuid4().hex[:8]}.sock")


@pytest.fixture
def proxy_factory(veth, ctrl_sock_path):
    """
    Starts thor_fhaul_proxy against the veth pair.

    Call it with the DU set the test needs; the process is stopped and the
    control socket removed on teardown.
    """
    started: list[env.ProxyProcess] = []

    def _start(dus=((DU0_MAC, DU0_VLAN), (DU1_MAC, DU1_VLAN)), *,
               num_dus=None, ctrl=True, num_prbs=NUM_PRBS, **kwargs):
        proc = env.ProxyProcess(
            iface=veth.mb_iface,
            dus=dus,
            num_dus=num_dus,
            num_prbs=num_prbs,
            ctrl_sock=ctrl_sock_path if ctrl else None,
            # A distinct EAL prefix per instance, so a proxy that outlives its
            # test cannot stop the next one from starting and turn one real
            # failure into a cascade of unrelated errors.
            file_prefix=f"thor_it_{os.getpid()}_{uuid.uuid4().hex[:8]}",
            **kwargs,
        )
        proc.start()
        started.append(proc)
        return proc

    yield _start

    for proc in started:
        proc.stop()
    Path(ctrl_sock_path).unlink(missing_ok=True)


@pytest.fixture
def proxy(proxy_factory):
    """The common case: two L1s, DU0 and DU1, both active."""
    return proxy_factory()


@pytest.fixture
def ctrl(proxy, ctrl_sock_path):
    """A connected control client for the running proxy."""
    client = env.CtrlClient(ctrl_sock_path)
    client.connect()
    yield client
    client.close()


@pytest.fixture
def capture(veth, middlebox_mac):
    """
    Captures frames the proxy emits.

    Used as `with capture() as cap:` -- the sniffer starts on entry, so open it
    before injecting traffic.
    """
    def _capture():
        return env.Capture(veth.net_iface, middlebox_mac)

    return _capture
