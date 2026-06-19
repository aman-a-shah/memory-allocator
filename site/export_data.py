#!/usr/bin/env python3
"""Generate site/data.json from a real allocator run.

Builds the metrics target (if needed), runs it to replay the synthetic HFT
trace through the custom allocator and the system allocator, then augments the
emitted JSON with run metadata. No numbers are mocked: every value in
data.json comes from bench/metrics.cpp timing real allocations.
"""

import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from datetime import date

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build", "release")
METRICS = os.path.join(BUILD, "bench", "memalloc_metrics")
OUT = os.path.join(HERE, "data.json")


def run(cmd, **kw):
    print("  $", " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True, **kw)


def ensure_metrics_binary():
    if os.path.exists(METRICS):
        return
    print("[export] metrics binary missing — configuring + building", file=sys.stderr)
    if not os.path.exists(os.path.join(BUILD, "CMakeCache.txt")):
        run(["cmake", "--preset", "release"], cwd=ROOT)
    run(["cmake", "--build", "--preset", "release", "--target", "memalloc_metrics"],
        cwd=ROOT)


def host_string():
    sysname = platform.system()
    if sysname == "Darwin":
        try:
            cpu = subprocess.check_output(
                ["sysctl", "-n", "machdep.cpu.brand_string"]).decode().strip()
        except Exception:
            cpu = platform.processor() or "Apple silicon"
        return f"{cpu} · macOS {platform.mac_ver()[0]}"
    return f"{platform.processor() or platform.machine()} · {sysname}"


def compiler_string():
    cache = os.path.join(BUILD, "CMakeCache.txt")
    cxx = ""
    try:
        with open(cache) as f:
            for line in f:
                if line.startswith("CMAKE_CXX_COMPILER:"):
                    cxx = line.split("=", 1)[1].strip()
                    break
    except OSError:
        pass
    if cxx and os.path.exists(cxx):
        try:
            out = subprocess.check_output([cxx, "--version"]).decode().splitlines()[0]
            return out.strip()
        except Exception:
            pass
    return cxx or "clang++ (C++17)"


def main():
    ensure_metrics_binary()
    with tempfile.NamedTemporaryFile("r", suffix=".json", delete=False) as tmp:
        tmp_path = tmp.name
    print("[export] running metrics (replaying HFT trace, best-of-N)…", file=sys.stderr)
    run([METRICS, tmp_path])
    with open(tmp_path) as f:
        data = json.load(f)
    os.unlink(tmp_path)

    data["meta"] = {
        "host": host_string(),
        "compiler": compiler_string(),
        "date": date.today().isoformat(),
        "baseline": "system libc malloc",
    }

    with open(OUT, "w") as f:
        json.dump(data, f, indent=1)
    # Also emit an inlined copy so the page can render synchronously (no async
    # fetch) — this is what makes the headless-Chrome print capture fully-drawn
    # charts. The on-screen page still falls back to fetching data.json.
    with open(os.path.join(HERE, "data.js"), "w") as f:
        f.write("window.__DATA__=" + json.dumps(data) + ";\n")
    print(f"[export] wrote {OUT} + data.js", file=sys.stderr)
    h = data["headline"]
    print(f"[export] speedup mixed={h['speedup_mixed']:.2f}× "
          f"tail={h['tail_ratio']:.1f}× frag={h['fragmentation_pct']:.3f}%",
          file=sys.stderr)


if __name__ == "__main__":
    main()
