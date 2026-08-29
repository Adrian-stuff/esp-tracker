#!/usr/bin/env python3
"""Turn a survey log into a Phase 0 go/no-go.

    python3 analyze.py survey.csv                 # coverage report
    python3 analyze.py survey.csv --geolocate KEY # also test Wi-Fi resolvability

The second form is what actually closes gate 2. Counting access points only
tells you they exist; it does not tell you the geolocation provider's database
knows where they are. In a newly built subdivision it very often does not.
"""
import csv, sys, statistics, argparse, json, urllib.request

REGISTERED = {1, 5}          # CREG 1 = home, 5 = roaming

def load(path):
    with open(path, newline="") as f:
        return [r for r in csv.DictReader(f) if r.get("sample")]

def parse_aps(field):
    out = []
    for part in (field or "").split(";"):
        if "/" in part:
            bssid, rssi = part.rsplit("/", 1)
            try:
                out.append({"bssid": bssid.strip(), "rssi": int(rssi)})
            except ValueError:
                pass
    return out

def pct(n, d):
    return 0.0 if not d else 100.0 * n / d

def report_group(name, rows):
    n = len(rows)
    csqs = [int(r["csq"]) for r in rows if r["csq"] not in ("", "-1") and int(r["csq"]) != 99]
    reg  = sum(1 for r in rows if int(r["creg"] or -1) in REGISTERED)
    fix  = sum(1 for r in rows if r["gps_fix"] == "1")
    aps  = [len(parse_aps(r["aps"])) for r in rows]
    ops  = {r["operator"] for r in rows if r["operator"]}

    reg_pct = pct(reg, n)
    med_csq = statistics.median(csqs) if csqs else 0
    med_aps = statistics.median(aps) if aps else 0

    print(f"\n  {name}   ({n} samples)")
    print(f"    operator        {', '.join(sorted(ops)) or '—'}")
    print(f"    registered      {reg_pct:5.1f}%   {_verdict_reg(reg_pct)}")
    print(f"    CSQ median      {med_csq:5.0f}    min {min(csqs) if csqs else 0:>3}   {_verdict_csq(med_csq)}")
    print(f"    GNSS fix        {pct(fix, n):5.1f}%")
    print(f"    Wi-Fi APs       {med_aps:5.0f}    max {max(aps) if aps else 0:>3}   {_verdict_aps(med_aps)}")

def _verdict_reg(p):
    if p >= 98: return "OK"
    if p >= 90: return "MARGINAL — gaps on this stretch"
    return "*** FAIL — 2G does not hold here ***"

def _verdict_csq(c):
    # CSQ 0-31. <10 is roughly -95 dBm or worse: attach may hold but data will
    # be slow and TX bursts will brown out a weak power rail.
    if c >= 15: return "OK"
    if c >= 10: return "MARGINAL"
    return "*** WEAK — expect failed sends and brownouts ***"

def _verdict_aps(a):
    if a >= 5:  return "OK for Wi-Fi positioning"
    if a >= 2:  return "MARGINAL — resolvable but imprecise"
    return "*** TOO FEW — no indoor position here ***"

def geolocate(aps, key, endpoint="https://us1.unwiredlabs.com/v2/process.php"):
    """Test whether the provider can actually resolve a scan. Unwired Labs
    format; adapt the body for Combain or Google."""
    body = json.dumps({"token": key, "wifi": [{"bssid": a["bssid"], "signal": a["rssi"]} for a in aps]}).encode()
    req = urllib.request.Request(endpoint, data=body, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            d = json.load(r)
        if d.get("status") == "ok":
            return d.get("accuracy"), None
        return None, d.get("message", "not resolved")
    except Exception as e:
        return None, str(e)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--geolocate", metavar="API_KEY",
                    help="test the strongest scan at each waypoint against the provider")
    args = ap.parse_args()

    rows = load(args.csv)
    if not rows:
        print("no samples in that file"); return 1

    print(f"\n{'='*64}\n  PHASE 0 COVERAGE SURVEY — {len(rows)} samples\n{'='*64}")
    report_group("WHOLE ROUTE", rows)

    waypoints = sorted({int(r["waypoint"]) for r in rows})
    if len(waypoints) > 1:
        print(f"\n{'-'*64}\n  BY WAYPOINT   (gate, classroom, home — wherever you pressed)\n{'-'*64}")
        for w in waypoints:
            report_group(f"waypoint {w}", [r for r in rows if int(r["waypoint"]) == w])

    if args.geolocate:
        print(f"\n{'-'*64}\n  WI-FI RESOLVABILITY   (does the provider know these APs?)\n{'-'*64}")
        for w in waypoints:
            grp = [r for r in rows if int(r["waypoint"]) == w]
            best = max(grp, key=lambda r: len(parse_aps(r["aps"])), default=None)
            aps = parse_aps(best["aps"]) if best else []
            if len(aps) < 2:
                print(f"  waypoint {w}: only {len(aps)} AP(s) — not enough to query")
                continue
            acc, err = geolocate(aps, args.geolocate)
            if acc is not None:
                mark = "OK" if acc <= 50 else "MARGINAL" if acc <= 150 else "POOR"
                print(f"  waypoint {w}: resolved to +/-{acc:.0f} m from {len(aps)} APs   {mark}")
            else:
                print(f"  waypoint {w}: *** NOT RESOLVED *** ({err})")

    print(f"\n{'='*64}")
    print("  GATE 1  2G registered >=98% and CSQ median >=15 along the route")
    print("  GATE 2  >=5 APs and provider resolves to <=50 m INSIDE the school")
    print("  Run this once per carrier. Globe and Smart differ a lot by area.")
    print(f"{'='*64}\n")
    return 0

if __name__ == "__main__":
    sys.exit(main())
