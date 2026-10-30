import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

from freemonitor.Packets import DISCOVERY_PORT, HOST_PORT
from tools.FrameViewer import RECEIVE_BUFFER_SIZE
from tools.TestSender import SEND_BUFFER_SIZE

SCRIPT_PATH = Path(__file__).resolve().parents[2] / "scripts" / "SetupFedoraNetwork.sh"
DEFAULT_LIMIT = 212992
ACTIVE_ZONES = "FedoraWorkstation (default)\n  interfaces: wlp2s0\nhome\n  interfaces: enp3s0\n"
linuxOnly = pytest.mark.skipif(sys.platform != "linux", reason="needs a Linux shell")

FAKE_FIREWALL = r"""#!/usr/bin/env bash
arguments="$*"
echo "$arguments" >> "$FAKE_STATE/firewallCalls"
zone="${arguments#*--zone=}"
zone="${zone%% *}"
port="${arguments##*-port=}"
case "$arguments" in
    --state) [[ ! -e "$FAKE_STATE/firewallStopped" ]] ;;
    --get-active-zones) cat "$FAKE_STATE/activeZones" ;;
    --get-default-zone) echo public ;;
    --get-zones) echo FedoraWorkstation home public ;;
    *--query-port=*) grep -qxF "$zone $port" "$FAKE_STATE/permanentPorts" ;;
    --permanent*--add-port=*) echo "$zone $port" >> "$FAKE_STATE/permanentPorts" ;;
    --permanent*--remove-port=*) sed -i "\|^$zone $port\$|d" "$FAKE_STATE/permanentPorts" ;;
    *--add-port=*) echo "$zone $port" >> "$FAKE_STATE/runtimePorts" ;;
    *--remove-port=*) sed -i "\|^$zone $port\$|d" "$FAKE_STATE/runtimePorts" ;;
esac
"""
FAKE_SYSCTL = r"""#!/usr/bin/env bash
if [[ "$1" == "-n" ]]; then
    cat "$FAKE_STATE/$2"
else
    echo "$*" >> "$FAKE_STATE/sysctlCalls"
fi
"""
FAKE_ID = r"""#!/usr/bin/env bash
cat "$FAKE_STATE/userId"
"""


@dataclass
class FakeSystem:
    state: Path
    commands: Path

    @property
    def sysctlFile(self) -> Path:
        return self.state / "90-freemonitor.conf"

    def read(self, name: str) -> str:
        return (self.state / name).read_text()

    def write(self, name: str, text: str) -> None:
        (self.state / name).write_text(text)

    def lines(self, name: str) -> list[str]:
        return self.read(name).splitlines()

    def runScript(self, *arguments: str) -> subprocess.CompletedProcess:
        environment = {
            **os.environ,
            "PATH": f"{self.commands}:{os.environ['PATH']}",
            "FAKE_STATE": str(self.state),
            "FM_SYSCTL_FILE": str(self.sysctlFile),
        }
        return subprocess.run(["bash", str(SCRIPT_PATH), *arguments], env=environment, capture_output=True, text=True)


@pytest.fixture
def fakeSystem(tmp_path):
    system = FakeSystem(tmp_path / "state", tmp_path / "commands")
    system.state.mkdir()
    system.commands.mkdir()
    for name, text in (("firewall-cmd", FAKE_FIREWALL), ("sysctl", FAKE_SYSCTL), ("id", FAKE_ID)):
        path = system.commands / name
        path.write_text(text)
        path.chmod(0o755)
    for name in ("firewallCalls", "sysctlCalls", "permanentPorts", "runtimePorts"):
        system.write(name, "")
    system.write("activeZones", ACTIVE_ZONES)
    system.write("userId", "0\n")
    system.write("net.core.rmem_max", f"{DEFAULT_LIMIT}\n")
    system.write("net.core.wmem_max", f"{DEFAULT_LIMIT}\n")
    return system


def scriptConstant(name: str) -> int:
    return int(re.search(rf"^readonly {name}=(\d+)$", SCRIPT_PATH.read_text(), re.MULTILINE).group(1))


def expectedPorts(*zones: str) -> list[str]:
    return [f"{zone} {port}/udp" for zone in zones for port in (HOST_PORT, DISCOVERY_PORT)]


def testPortsMatchTheProtocol():
    assert scriptConstant("HOST_PORT") == HOST_PORT
    assert scriptConstant("DISCOVERY_PORT") == DISCOVERY_PORT


def testBufferLimitFitsThePythonTools():
    assert scriptConstant("BUFFER_BYTES") >= RECEIVE_BUFFER_SIZE
    assert scriptConstant("BUFFER_BYTES") >= SEND_BUFFER_SIZE


@linuxOnly
def testScriptIsExecutable():
    assert os.access(SCRIPT_PATH, os.X_OK)


@linuxOnly
def testOpensPortsInEveryActiveZone(fakeSystem):
    assert fakeSystem.runScript().returncode == 0
    assert fakeSystem.lines("permanentPorts") == expectedPorts("FedoraWorkstation", "home")
    assert fakeSystem.lines("runtimePorts") == expectedPorts("FedoraWorkstation", "home")


@linuxOnly
def testUsesTheDefaultZoneWithoutActiveZones(fakeSystem):
    fakeSystem.write("activeZones", "")
    assert fakeSystem.runScript().returncode == 0
    assert fakeSystem.lines("permanentPorts") == expectedPorts("public")


@linuxOnly
def testRaisesBothBufferLimits(fakeSystem):
    assert fakeSystem.runScript().returncode == 0
    limit = scriptConstant("BUFFER_BYTES")
    assert fakeSystem.sysctlFile.read_text() == f"net.core.rmem_max = {limit}\nnet.core.wmem_max = {limit}\n"
    assert fakeSystem.lines("sysctlCalls") == [f"-q -p {fakeSystem.sysctlFile}"]


@linuxOnly
def testKeepsAHigherBufferLimit(fakeSystem):
    higher = 2 * scriptConstant("BUFFER_BYTES")
    fakeSystem.write("net.core.rmem_max", f"{higher}\n")
    assert fakeSystem.runScript().returncode == 0
    assert f"net.core.rmem_max = {higher}\n" in fakeSystem.sysctlFile.read_text()


@linuxOnly
def testSecondRunOpensNothingNew(fakeSystem):
    fakeSystem.runScript()
    result = fakeSystem.runScript()
    assert result.returncode == 0
    assert "already open" in result.stdout
    assert fakeSystem.lines("permanentPorts") == expectedPorts("FedoraWorkstation", "home")
    assert fakeSystem.lines("runtimePorts") == expectedPorts("FedoraWorkstation", "home")


@linuxOnly
def testStoppedFirewallStillRaisesBufferLimits(fakeSystem):
    fakeSystem.write("firewallStopped", "")
    result = fakeSystem.runScript()
    assert result.returncode == 0
    assert "firewalld is not running" in result.stdout
    assert fakeSystem.lines("permanentPorts") == []
    assert fakeSystem.sysctlFile.exists()


@linuxOnly
@pytest.mark.parametrize("arguments", [(), ("--remove",)])
def testRefusesToRunWithoutRoot(fakeSystem, arguments):
    fakeSystem.write("userId", "1000\n")
    fakeSystem.write("permanentPorts", "\n".join(expectedPorts("home")) + "\n")
    fakeSystem.sysctlFile.write_text("existing\n")
    result = fakeSystem.runScript(*arguments)
    assert result.returncode == 1
    assert "as root" in result.stderr
    assert fakeSystem.lines("firewallCalls") == []
    assert fakeSystem.sysctlFile.read_text() == "existing\n"


@linuxOnly
@pytest.mark.parametrize("arguments", [("--unknown",), ("--remove", "--remove")])
def testRejectsBadArguments(fakeSystem, arguments):
    result = fakeSystem.runScript(*arguments)
    assert result.returncode == 2
    assert "Usage:" in result.stderr
    assert fakeSystem.lines("firewallCalls") == []


@linuxOnly
def testRemoveUndoesEverythingEvenInInactiveZones(fakeSystem):
    fakeSystem.runScript()
    fakeSystem.write("activeZones", "public (default)\n  interfaces: wlp2s0\n")
    result = fakeSystem.runScript("--remove")
    assert result.returncode == 0
    assert fakeSystem.lines("permanentPorts") == []
    assert fakeSystem.lines("runtimePorts") == []
    assert not fakeSystem.sysctlFile.exists()
    assert "after a reboot" in result.stdout


@linuxOnly
def testRemoveWithNothingToUndoSucceeds(fakeSystem):
    result = fakeSystem.runScript("--remove")
    assert result.returncode == 0
    assert result.stdout == ""
