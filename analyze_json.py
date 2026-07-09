import json, sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/nccl_detail_failback_112.json"
d = json.load(open(path))

print(f"nccl_version: {d.get('nccl_version')}")
print(f"n_ranks: {d.get('n_ranks')}")
print(f"n_iters: {d.get('n_iters')}")
print(f"wall_time_us: {d.get('wall_time_us', 0):.1f}")
print(f"data_ok: {d.get('data_ok')}")

detail = d.get("detail", [])
print(f"\nDetail entries: {len(detail)}")

if not detail:
    # Old format fallback
    print("(old format, using results)")
    results = d.get("results", [])
    for r in results:
        size_mb = r["size"] // 1048576
        oop = r.get("out_of_place_us", [])
        if oop:
            avg = sum(oop)/len(oop)
            outliers = [(i, v) for i, v in enumerate(oop) if v > avg * 3]
            print(f"\nsize={size_mb}MB: oop {len(oop)} iters, avg={avg:.1f}us")
            if outliers:
                print(f"  OUTLIERS (>3x avg): {len(outliers)}")
                for i, v in outliers[:5]:
                    print(f"    iter {i}: {v:.1f} us ({v/avg:.1f}x)")
    sys.exit(0)

# New detail format: group by size
by_size = defaultdict(lambda: {"oop": [], "ip": []})
for det in detail:
    sz = det["size"]
    by_size[sz]["oop"].append(det["oop_time_us"])
    by_size[sz]["ip"].append(det["ip_time_us"])

for sz in sorted(by_size.keys()):
    size_mb = sz // 1048576
    oop = by_size[sz]["oop"]
    ip  = by_size[sz]["ip"]
    avg_oop = sum(oop)/len(oop) if oop else 0
    avg_ip  = sum(ip)/len(ip) if ip else 0

    print(f"\nsize={size_mb}MB: {len(oop)} iters")
    print(f"  oop: avg={avg_oop:.1f}us, min={min(oop):.1f}us, max={max(oop):.1f}us")
    print(f"  ip:  avg={avg_ip:.1f}us, min={min(ip):.1f}us, max={max(ip):.1f}us")

    oop_out = [(i, v) for i, v in enumerate(oop) if v > avg_oop * 3]
    ip_out  = [(i, v) for i, v in enumerate(ip) if v > avg_ip * 3]
    if oop_out:
        print(f"  oop OUTLIERS: {len(oop_out)}")
        for i, v in oop_out[:5]:
            print(f"    iter {i}: {v:.1f} us ({v/avg_oop:.1f}x avg)")
    if ip_out:
        print(f"  ip OUTLIERS: {len(ip_out)}")
        for i, v in ip_out[:5]:
            print(f"    iter {i}: {v:.1f} us ({v/avg_ip:.1f}x avg)")
