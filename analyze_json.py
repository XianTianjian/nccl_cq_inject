import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

parser = argparse.ArgumentParser(description="Analyze nccl_ar_detail JSON and optionally verify resiliency logs")
parser.add_argument("json", nargs="?", default="/tmp/fb.json", help="nccl_ar_detail JSON output")
parser.add_argument("--log", help="combined injector/NCCL log")
parser.add_argument("--expect", choices=("data", "baseline", "failover", "failback"), default="data")
args = parser.parse_args()

with open(args.json) as f:
    d = json.load(f)

print(f"nccl_version: {d.get('nccl_version')}")
print(f"op: {d.get('op')}")
print(f"n_ranks: {d.get('n_ranks')}")
print(f"n_iters: {d.get('n_iters')}")
print(f"data_ok: {d.get('data_ok')}")

detail = d["detail"]
print(f"\nDetail entries: {len(detail)}")

by_size = defaultdict(lambda: {"oop": [], "ip": []})
for det in detail:
    sz = det["size"]
    by_size[sz]["oop"].append(det["oop_time_us"])
    by_size[sz]["ip"].append(det["ip_time_us"])

for sz in sorted(by_size.keys()):
    size_mb = sz // 1048576
    oop = by_size[sz]["oop"]
    ip  = by_size[sz]["ip"]
    avg_oop = sum(oop) / len(oop)
    avg_ip  = sum(ip) / len(ip)

    print(f"\nsize={size_mb}MB: {len(oop)} iters")
    print(f"  oop: avg={avg_oop:.1f}us, min={min(oop):.1f}us, max={max(oop):.1f}us")
    print(f"  ip:  avg={avg_ip:.1f}us, min={min(ip):.1f}us, max={max(ip):.1f}us")

    oop_out = [(i, v) for i, v in enumerate(oop) if v > avg_oop * 3]
    ip_out  = [(i, v) for i, v in enumerate(ip) if v > avg_ip * 3]
    if oop_out:
        print(f"  oop SPIKES: {len(oop_out)}")
        for i, v in oop_out[:5]:
            print(f"    iter {i}: {v:.1f}us ({v/avg_oop:.1f}x avg)")
    if ip_out:
        print(f"  ip SPIKES: {len(ip_out)}")
        for i, v in ip_out[:5]:
            print(f"    iter {i}: {v:.1f}us ({v/avg_ip:.1f}x avg)")

failures = []
if d.get("data_ok") is not True:
    failures.append("JSON data_ok is not true")
if len(detail) != d.get("n_iters", 0) * len(by_size):
    failures.append("detail entry count does not match n_iters * number of sizes")

if args.expect in ("failover", "failback") and not args.log:
    failures.append(f"--log is required for --expect {args.expect}")

if args.log:
    log = Path(args.log).read_text(errors="replace")

    def count(text):
        return log.count(text)

    injected = count("[inject] injected")
    replaced = count("ncclIbResiliencyReplaceQps: Replacing QP")
    lines = log.splitlines()
    recovered_send = sum("Port recovery succeeded for devIndex=" in line and "(send comm=" in line for line in lines)
    recovered_recv = sum("Port recovery succeeded for devIndex=" in line and "(recv comm=" in line for line in lines)
    restored_active = count("ncclIbResiliencyActiveQpsRestore: Restoring QP")
    completed_send = count("All resiliency operations are completed for resiliency context (send comm=")
    completed_recv = count("All resiliency operations are completed for resiliency context (recv comm=")

    print("\nLog verification:")
    print(f"  injected={injected} replaced_qps={replaced}")
    print(f"  recovery_succeeded: send={recovered_send} recv={recovered_recv}")
    print(f"  active_qps_restored={restored_active}")
    print(f"  resiliency_completed: send={completed_send} recv={completed_recv}")

    if args.expect == "baseline" and injected:
        failures.append(f"baseline unexpectedly injected {injected} completion(s)")
    if args.expect in ("failover", "failback"):
        if injected < 1:
            failures.append("no injected completion found")
        if replaced < 1:
            failures.append("no failover QP replacement found")
    if args.expect == "failback":
        if recovered_send < 1 or recovered_recv < 1:
            failures.append("port recovery did not succeed on both send and recv communicators")
        if restored_active < 2:
            failures.append("recovered QPs were not restored on both communicators")
        if completed_send < 1 or completed_recv < 1:
            failures.append("resiliency operations did not complete on both communicators")

if failures:
    print("\nverification: FAIL", file=sys.stderr)
    for failure in failures:
        print(f"  - {failure}", file=sys.stderr)
    sys.exit(2)

print("\nverification: PASS")
