#!/usr/bin/env python3

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.

"""
Local veth test environment for thor_fhaul_proxy.

The proxy is a single-port middlebox: the RU and every DU sit on the same wire,
distinguished only by MAC address. That maps exactly onto one veth pair --

    thor-mb  <---->  thor-net
    (proxy, via       (test harness, via
     DPDK af_packet)   raw sockets/scapy)

-- with the proxy bound to thor-mb through the af_packet PMD, and the harness
injecting DU/RU traffic on thor-net and capturing whatever the proxy sends back.

Because the two ends share a wire, the harness separates its own injected
traffic from the proxy's output by source MAC: everything the proxy emits is
rewritten to the middlebox MAC, which the harness never uses as a source.
"""

from __future__ import annotations

import os
import shutil
import signal
import socket
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
PROXY_BIN = REPO_ROOT / "build" / "thor_fhaul_proxy_dpdk" / "thor_fhaul_proxy_dpdk"

from scapy.all import AsyncSniffer, Ether, conf as scapy_conf, sendp  # noqa: E402

# ---------------------------------------------------------------------------
# topology constants
# ---------------------------------------------------------------------------

MB_IFACE = "thor-mb"    # proxy side of the veth pair
NET_IFACE = "thor-net"  # harness side

RU_MAC = "02:00:00:00:00:01"
DU0_MAC = "02:00:00:00:00:10"
DU1_MAC = "02:00:00:00:00:11"
DU2_MAC = "02:00:00:00:00:12"
UNKNOWN_MAC = "02:00:00:00:00:ff"

RU_VLAN = 5
DU0_VLAN = 10
DU1_VLAN = 11
DU2_VLAN = 12

NUM_PRBS = 32   # fits inside the 1500-byte MTU the af_packet PMD forces

# The af_packet PMD hardcodes max_rx_pktlen to RTE_ETHER_MAX_LEN (1518), so the
# proxy cannot run at its production 9600-byte MTU over veth; the tests pass
# --mtu 1500 instead. Frames larger than that are covered by the unit tests,
# which build mbufs directly and are not bound by any PMD.
#
# blocksz must be a multiple of the page size and of framesz, framecnt must be
# frames_per_block * blockcnt, and framesz - TPACKET2_HDRLEN must fit in an
# mbuf's data room. 2048/4096 satisfies all of these for a 1500-byte MTU.
AF_PACKET_FRAMESZ = 2048
AF_PACKET_BLOCKSZ = 4096
AF_PACKET_FRAMECNT = 512

PROXY_MTU = 1500
# Frame budget: 14 (Ethernet) + MTU, minus the 30-byte O-RAN header stack,
# divided by the 28-byte compressed PRB.
PRB_9_SIZE = 28
MAX_TEST_PRBS = (14 + PROXY_MTU - 30) // PRB_9_SIZE  # 52


def _run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=check, capture_output=True, text=True)


def require_root() -> None:
    if os.geteuid() != 0:
        raise RuntimeError(
            "the integration tests need root: they create veth interfaces and "
            "open AF_PACKET sockets. Run tests/run_tests.sh integration, or "
            "sudo venv/bin/python -m pytest tests/integration"
        )


# ---------------------------------------------------------------------------
# veth pair
# ---------------------------------------------------------------------------


class VethPair:
    """A veth pair configured to carry O-RAN fronthaul frames."""

    def __init__(self, mb_iface: str = MB_IFACE, net_iface: str = NET_IFACE):
        self.mb_iface = mb_iface
        self.net_iface = net_iface

    def setup(self) -> None:
        self.teardown()  # clear anything a crashed run left behind

        _run(["ip", "link", "add", self.mb_iface, "type", "veth",
              "peer", "name", self.net_iface])

        for iface in (self.mb_iface, self.net_iface):
            _run(["ip", "link", "set", iface, "mtu", str(PROXY_MTU)])
            _run(["ip", "link", "set", iface, "up"])
            _run(["ip", "link", "set", iface, "promisc", "on"])
            # Segmentation offloads would coalesce or split the frames under
            # test; keep what the harness sends byte-identical on the wire.
            _run(["ethtool", "-K", iface, "tx", "off", "rx", "off",
                  "tso", "off", "gso", "off", "gro", "off", "lro", "off"],
                 check=False)
            # The kernel would otherwise emit IPv6 MLD/RS frames from these
            # interfaces -- including from the proxy's own MAC, which the
            # capture filter selects on.
            _run(["sysctl", "-qw", f"net.ipv6.conf.{iface}.disable_ipv6=1"],
                 check=False)

        # veth reports carrier only once both ends are up; give the kernel a
        # moment so the PMD sees a live link.
        self._wait_for_link()

        # scapy snapshots the interface list when it is imported, which happens
        # before these interfaces exist.
        scapy_conf.ifaces.reload()

    def _wait_for_link(self, timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = Path(f"/sys/class/net/{self.mb_iface}/operstate").read_text().strip()
            if state == "up":
                return
            time.sleep(0.05)
        raise RuntimeError(f"{self.mb_iface} did not come up")

    def teardown(self) -> None:
        _run(["ip", "link", "del", self.mb_iface], check=False)

    def mac(self, iface: str) -> str:
        return Path(f"/sys/class/net/{iface}/address").read_text().strip().lower()

    @property
    def middlebox_mac(self) -> str:
        """The MAC the proxy will adopt: that of the port it binds to."""
        return self.mac(self.mb_iface)

    def __enter__(self) -> "VethPair":
        self.setup()
        return self

    def __exit__(self, *exc) -> None:
        self.teardown()


# ---------------------------------------------------------------------------
# control socket client
# ---------------------------------------------------------------------------


@dataclass
class CtrlReply:
    ok: bool
    status: str            # the text after OK/ERR on the first line
    lines: list[str] = field(default_factory=list)  # body lines, terminator excluded

    @property
    def fields(self) -> dict[str, str]:
        """key=value pairs parsed out of the status line."""
        out = {}
        for token in self.status.split():
            if "=" in token:
                k, v = token.split("=", 1)
                out[k] = v
        return out

    def __str__(self) -> str:
        head = "OK" if self.ok else "ERR"
        return f"{head} {self.status}" + ("\n" + "\n".join(self.lines) if self.lines else "")


class CtrlClient:
    """
    Client for the proxy's add/remove-L1 socket.

    Every reply opens with OK/ERR and closes with a lone "." line, so replies
    are framed without the client having to know the command.
    """

    def __init__(self, sock_path: str, timeout: float = 5.0):
        self.sock_path = sock_path
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._buf = b""

    def connect(self, retries: int = 100) -> "CtrlClient":
        last = None
        for _ in range(retries):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(self.timeout)
                s.connect(self.sock_path)
                self._sock = s
                return self
            except OSError as exc:  # socket not bound yet
                last = exc
                time.sleep(0.05)
        raise RuntimeError(f"cannot connect to {self.sock_path}: {last}")

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def _readline(self) -> str:
        while b"\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise RuntimeError("control connection closed by the proxy")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode(errors="replace")

    def command(self, line: str) -> CtrlReply:
        if self._sock is None:
            raise RuntimeError("not connected")
        self._sock.sendall(line.encode() + b"\n")

        head = self._readline()
        if head.startswith("OK"):
            ok, status = True, head[2:].strip()
        elif head.startswith("ERR"):
            ok, status = False, head[3:].strip()
        else:
            raise RuntimeError(f"malformed control reply: {head!r}")

        body = []
        while True:
            nxt = self._readline()
            if nxt == ".":
                break
            body.append(nxt)
        return CtrlReply(ok=ok, status=status, lines=body)

    # convenience wrappers
    def add(self, mac: str, vlan: int) -> CtrlReply:
        return self.command(f"add {mac} {vlan}")

    def delete(self, mac: str) -> CtrlReply:
        return self.command(f"del {mac}")

    def list(self) -> CtrlReply:
        return self.command("list")

    def stats(self) -> dict[str, int]:
        reply = self.command("stats")
        out: dict[str, int] = {}
        for token in reply.status.split():
            if "=" in token:
                k, v = token.split("=", 1)
                out[k] = int(v)
        for line in reply.lines:
            if line.startswith("du["):
                slot = line[3:line.index("]")]
                for token in line.split()[1:]:
                    k, v = token.split("=", 1)
                    out[f"du{slot}_{k}"] = int(v)
            elif "=" in line:
                k, v = line.split("=", 1)
                out[k] = int(v)
        return out

    def active_dus(self) -> list[dict[str, str]]:
        reply = self.list()
        dus = []
        for line in reply.lines:
            if not line.startswith("du["):
                continue
            entry = {"slot": line[3:line.index("]")]}
            for token in line.split()[1:]:
                k, v = token.split("=", 1)
                entry[k] = v
            dus.append(entry)
        return dus

    def __enter__(self) -> "CtrlClient":
        return self.connect()

    def __exit__(self, *exc) -> None:
        self.close()


# ---------------------------------------------------------------------------
# the proxy under test
# ---------------------------------------------------------------------------


class ProxyProcess:
    """
    Runs thor_fhaul_proxy_dpdk against the veth pair.

    EAL is given --no-huge so the tests neither need hugepage setup nor disturb
    another DPDK application already holding them, and --no-pci so it never
    touches a real NIC on this host.
    """

    READY_MARKER = "Middlebox ready"

    def __init__(
        self,
        iface: str,
        ru_mac: str = RU_MAC,
        ru_vlan: int = RU_VLAN,
        num_prbs: int = NUM_PRBS,
        dus: Iterable[tuple[str, int]] = ((DU0_MAC, DU0_VLAN), (DU1_MAC, DU1_VLAN)),
        num_dus: Optional[int] = None,
        ctrl_sock: Optional[str] = None,
        file_prefix: str = "thor_it",
        lcore: str = "1",
    ):
        self.iface = iface
        self.ru_mac = ru_mac
        self.ru_vlan = ru_vlan
        self.num_prbs = num_prbs
        self.dus = list(dus)
        self.num_dus = num_dus
        self.ctrl_sock = ctrl_sock
        self.file_prefix = file_prefix
        self.lcore = lcore

        self.proc: Optional[subprocess.Popen] = None
        self.log_lines: list[str] = []
        self._ready = threading.Event()
        self._log_thread: Optional[threading.Thread] = None

    def argv(self) -> list[str]:
        vdev = (
            f"net_af_packet0,iface={self.iface},"
            f"blocksz={AF_PACKET_BLOCKSZ},framesz={AF_PACKET_FRAMESZ},"
            f"framecnt={AF_PACKET_FRAMECNT},qpairs=1"
        )
        argv = [
            str(PROXY_BIN),
            "-l", self.lcore,
            "--no-huge", "-m", "512",
            "--no-pci", "--no-telemetry",
            "--file-prefix", self.file_prefix,
            "--vdev", vdev,
            "--",
            "net_af_packet0",
            self.ru_mac, str(self.ru_vlan), str(self.num_prbs),
        ]
        for mac, vlan in self.dus:
            argv += [mac, str(vlan)]
        if self.num_dus is not None:
            argv.append(str(self.num_dus))
        argv += ["--mtu", str(PROXY_MTU)]
        # Leave the datapath at normal priority: an RT busy-poll loop would
        # starve the harness sharing this machine.
        argv.append("--no-rt")
        if self.ctrl_sock:
            argv += ["--ctrl-sock", self.ctrl_sock]
        return argv

    def start(self, timeout: float = 30.0) -> "ProxyProcess":
        if not PROXY_BIN.exists():
            raise RuntimeError(f"{PROXY_BIN} not built; run tests/build.sh first")

        self.proc = subprocess.Popen(
            self.argv(),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        # Drain stdout from a thread from the outset. Reading inline would block
        # in readline() with no way to honour the deadline, and it also keeps
        # the pipe from filling up and stalling the datapath later on.
        self._ready = threading.Event()
        self._start_log_drain()

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._ready.wait(0.05):
                return self
            if self.proc.poll() is not None:
                break  # exited before signalling readiness

        exit_code = self.proc.poll()
        self.stop()
        raise RuntimeError(
            f"the proxy did not become ready (exit code {exit_code}). Output:\n  "
            + "\n  ".join(self.log_lines[-40:])
        )

    def _start_log_drain(self) -> None:
        def drain():
            try:
                for line in self.proc.stdout:
                    self.log_lines.append(line.rstrip())
                    if self.READY_MARKER in line:
                        self._ready.set()
            except (ValueError, OSError):
                pass

        self._log_thread = threading.Thread(target=drain, daemon=True)
        self._log_thread.start()

    def stop(self, timeout: float = 5.0) -> None:
        """
        Stop the proxy, escalating to SIGKILL if it does not go quietly.

        Every proxy busy-polls the same lcore, so one that outlives its test
        starves each later one during EAL init and turns a single failure into a
        run-wide cascade of "did not become ready" errors. This must never
        return with the process still alive.
        """
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                try:
                    self.proc.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    raise RuntimeError(f"proxy pid {self.proc.pid} survived SIGKILL")
        if self.proc.stdout:
            self.proc.stdout.close()
        self.proc = None

    @property
    def log(self) -> str:
        return "\n".join(self.log_lines)

    def __enter__(self) -> "ProxyProcess":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()


# ---------------------------------------------------------------------------
# capture
# ---------------------------------------------------------------------------


class Capture:
    """
    Captures only what the proxy emits.

    A packet socket on a veth also sees the frames the harness itself sends, so
    the capture is filtered to frames sourced from the middlebox MAC -- which is
    every frame the proxy forwards, and nothing the harness injects.
    """

    def __init__(self, iface: str, middlebox_mac: str):
        self.iface = iface
        self.middlebox_mac = middlebox_mac.lower()
        self._sniffer: Optional[AsyncSniffer] = None
        self.packets: list = []

    def __enter__(self) -> "Capture":
        # Match the eCPRI ethertype at its untagged offset and at its offset
        # behind a VLAN tag. A tag the kernel has moved into auxdata is not
        # inline when BPF runs, so both positions have to be accepted.
        self._sniffer = AsyncSniffer(
            iface=self.iface,
            filter=(f"ether src {self.middlebox_mac} and "
                    f"(ether[12:2] = 0xaefe or ether[16:2] = 0xaefe)"),
            store=True,
        )
        self._sniffer.start()
        # Give libpcap a moment to attach before any traffic is injected.
        time.sleep(0.25)

        # A sniffer that died inside its own thread -- an uncompilable filter
        # because libpcap is absent, a missing interface -- otherwise surfaces
        # only as "captured 0 packets", which reads like a proxy bug and sends
        # you debugging the wrong component. Fail here, with the real reason.
        if not self._sniffer.running:
            raise RuntimeError(
                f"packet capture failed to start on {self.iface}: "
                f"{self._sniffer.exception or 'unknown error'}"
            )
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def stop(self) -> list:
        if self._sniffer is None:
            return self.packets

        sniffer, self._sniffer = self._sniffer, None
        if sniffer.running:
            self.packets = list(sniffer.stop() or [])
        elif sniffer.exception is not None:
            raise RuntimeError(
                f"packet capture on {self.iface} died: {sniffer.exception}"
            ) from sniffer.exception
        else:
            self.packets = list(sniffer.results or [])
        return self.packets

    def wait_for(self, count: int, timeout: float = 2.0) -> list:
        """Block until `count` packets have been captured, or the timeout expires."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._sniffer is None or not self._sniffer.running:
                break
            got = list(self._sniffer.results or [])
            if len(got) >= count:
                break
            time.sleep(0.02)
        # A short settle so an unexpected extra packet still shows up and can be
        # asserted against.
        time.sleep(0.15)
        return self.stop()


def send(packets, iface: str = NET_IFACE) -> None:
    """Inject frames on the harness side of the veth pair."""
    sendp(packets, iface=iface, verbose=False)


def check_tools() -> None:
    for tool in ("ip", "ethtool"):
        if shutil.which(tool) is None:
            raise RuntimeError(f"{tool} is required by the integration tests")
