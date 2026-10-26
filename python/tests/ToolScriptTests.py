import subprocess
import sys
from pathlib import Path

import pytest

TOOLS_DIRECTORY = Path(__file__).resolve().parent.parent / "tools"
STARTUP_TIMEOUT_S = 30


@pytest.mark.parametrize("scriptName", ["FrameViewer.py", "LossEmulator.py", "TestSender.py"])
def testToolStartsAsScriptFromAnyDirectory(scriptName, tmp_path):
    result = subprocess.run(
        [sys.executable, str(TOOLS_DIRECTORY / scriptName), "--help"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=STARTUP_TIMEOUT_S,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.startswith(f"usage: {scriptName}")
