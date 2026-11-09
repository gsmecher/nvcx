import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_hello(nvc, vivado, tmp_path):
    """hello/, as a newcomer would run it (but on this host or container, DOCKER=0): nvcx built
    and installed from the tree into a prefix, found there through pkg-config, and the example
    simulated.

    Only hello/ itself is copied, for a clean and private place to build in; nvcx is built from
    the tree where it is (NVCX_ROOT). A copy of the tree would need a list of what a build reads,
    and that list has gone stale before."""
    # hello's own .gitignore names what it generates; the tree's ignores build/ too
    generated = [
        line.strip().rstrip("/")
        for line in (ROOT / "hello" / ".gitignore").read_text().splitlines()
        if line.strip()
    ]
    hello = tmp_path / "hello"
    shutil.copytree(
        ROOT / "hello", hello, ignore=shutil.ignore_patterns("build", *generated)
    )
    env = dict(
        os.environ,
        PATH=os.pathsep.join([str(Path(nvc).parent), os.environ["PATH"]]),
        XILINX_VIVADO=str(vivado),
    )
    cmd = ["make", "-C", str(hello), f"NVCX_ROOT={ROOT}", "DOCKER=0", "all"]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    out = r.stdout + r.stderr  # NVC reports on stderr
    assert r.returncode == 0, out[-3000:]
    assert "PASS: 256" in out, out[-3000:]
