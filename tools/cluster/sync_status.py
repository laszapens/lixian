#!/usr/bin/env python3
"""Mirror cluster training state into the local tools/nnue directory.

The monitor dashboard reads local files, so this script keeps a small,
high-signal subset in sync: the datagen log tail, the trainer log tail, and a
JSON summary of how much data the cluster has produced. It never copies the
raw shards (they are gigabytes); only the numbers the dashboard needs.

Runs until killed. Usage:
    python3 tools/cluster/sync_status.py [--interval 20]
"""
import argparse
import json
import re
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
NNUE_DIR = os.path.join(ROOT, "tools", "nnue")

SSH_HOST = os.environ.get("LX_SSH_HOST", "ac0kv0924k@eshell111.hpccube.com")
SSH_PORT = os.environ.get("LX_SSH_PORT", "65082")
SSH_KEY = os.environ.get("LX_SSH_KEY", os.path.expanduser("~/.ssh/scnet_nw"))
REMOTE_DIR = os.environ.get("LX_REMOTE_DIR", "~/lixian-cloud")

REMOTE_SCRIPT = """cd ~/lixian-cloud || exit 1
echo '===STATUS==='
echo "jobid=$(squeue -u $(whoami) -h -o %i 2>/dev/null | head -1)"
echo "state=$(squeue -u $(whoami) -h -o %t 2>/dev/null | head -1)"
echo "node=$(squeue -u $(whoami) -h -o %N 2>/dev/null | head -1)"
echo "shards=$(ls data/*.bin 2>/dev/null | wc -l)"
echo "bytes=$(du -sb data 2>/dev/null | cut -f1)"
echo "checkpoints=$(ls weights/*.bin 2>/dev/null | wc -l)"
echo "cores=$(grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 0)"
echo "memtotal=$(awk '/MemTotal/{print $2*1024}' /proc/meminfo 2>/dev/null)"
echo "memavail=$(awk '/MemAvailable/{print $2*1024}' /proc/meminfo 2>/dev/null)"
echo "load1=$(awk '{print $1}' /proc/loadavg 2>/dev/null)"
echo "load5=$(awk '{print $2}' /proc/loadavg 2>/dev/null)"
echo '===GENLOG==='
ls -t gen-*.log 2>/dev/null | head -1 | xargs -r tail -n 200
echo '===TRAINLOG==='
tail -n 300 weights/train.log 2>/dev/null
exit 0
"""


def ssh_command():
    return [
        "ssh", "-p", SSH_PORT, "-i", SSH_KEY,
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=15",
        SSH_HOST, REMOTE_SCRIPT,
    ]


def poll():
    try:
        out = subprocess.run(ssh_command(), capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return None
    if out.returncode != 0:
        return None
    return out.stdout


def parse(raw):
    status = {}
    gen_lines, train_lines = [], []
    section = None
    for line in raw.splitlines():
        if line.startswith("==="):
            section = line.strip("=").strip()
            continue
        if section == "STATUS":
            if "=" in line:
                k, _, v = line.partition("=")
                status[k.strip()] = v.strip()
        elif section == "GENLOG":
            gen_lines.append(line)
        elif section == "TRAINLOG":
            train_lines.append(line)
    return status, gen_lines, train_lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval", type=float, default=20.0)
    ap.add_argument("--once", action="store_true")
    args = ap.parse_args()

    os.makedirs(NNUE_DIR, exist_ok=True)
    while True:
        raw = poll()
        if raw is None:
            print("sync: cluster unreachable, retrying", flush=True)
            if args.once:
                return 1
            time.sleep(args.interval)
            continue

        status, gen_lines, train_lines = parse(raw)
        bytes_total = int(status.get("bytes") or 0)
        shards = int(status.get("shards") or 0)

        # The datagen log is the authority on how many positions exist: the
        # shard files are buffered and their directory size lags behind.
        reported = 0
        for line in reversed(gen_lines):
            m = re.search(r"pos=(\d+)", line)
            if m:
                reported = int(m.group(1))
                break

        with open(os.path.join(NNUE_DIR, "cloud.json"), "w", encoding="utf-8") as f:
            json.dump({
                "jobid": status.get("jobid", ""),
                "state": status.get("state", ""),
                "node": status.get("node", ""),
                "shards": shards,
                "dataBytes": bytes_total,
                "positions": reported or (bytes_total // 120),
                "checkpoints": int(status.get("checkpoints") or 0),
                "cores": int(status.get("cores") or 0),
                "memTotal": int(status.get("memtotal") or 0),
                "memAvail": int(status.get("memavail") or 0),
                "load1": float(status.get("load1") or 0),
                "load5": float(status.get("load5") or 0) or float(status.get("load1") or 0),
                "syncedAt": int(time.time()),
            }, f, ensure_ascii=False, indent=2)

        # The dashboard tails these two files; writing them here makes the
        # cluster look local without copying the shards themselves.
        if gen_lines:
            with open(os.path.join(NNUE_DIR, "gen.log"), "w", encoding="utf-8") as f:
                f.write("\n".join(gen_lines) + "\n")
        if train_lines:
            os.makedirs(os.path.join(NNUE_DIR, "weights"), exist_ok=True)
            with open(os.path.join(NNUE_DIR, "train.log"), "w", encoding="utf-8") as f:
                f.write("\n".join(train_lines) + "\n")

        print(f"sync: state={status.get('state','?')} node={status.get('node','?')} "
              f"positions={reported or bytes_total//120} shards={shards}", flush=True)
        if args.once:
            return 0
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
