import json, sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fb.json"
d = json.load(open(path))

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
