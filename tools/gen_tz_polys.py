#!/usr/bin/env python3
"""Generate src/TimezonePolyData.h from timezone-boundary-builder GeoJSON.

Turns real IANA timezone boundaries into the coarse int16 polygon table the
firmware scans before its box table. The output is byte-compatible with what
tools/tz_polygon_editor.html exports, including the EDITOR-STATE comment, so
the generated file can be loaded into the GUI editor and refined by hand.

Data source (ODbL, built from OpenStreetMap):
  https://github.com/evansiroky/timezone-boundary-builder/releases
  -> timezones.geojson.zip -> combined.json

POSIX TZ strings are taken from the local tzdata (/usr/share/zoneinfo TZif
footers), so they match whatever tzdata release the host has installed.

Usage:
  python3 tools/gen_tz_polys.py combined.json -o src/TimezonePolyData.h \
      [--eps 0.20] [--max-parts 15] [--max-verts 48] [--min-area 0.15]

Tuning for flash budget:
  --eps        Douglas-Peucker tolerance in degrees. Bigger = fewer vertices.
  --max-parts  Max polygons kept per zone (largest first).
  --min-area   Min ring area in equal-area-ish deg^2; smaller islands are
               dropped (the firmware's box table still covers them).
  --max-verts  Hard cap per ring; eps is raised locally until it fits.
After a flash upgrade, rerun with e.g. --eps 0.08 --min-area 0.05 for a much
finer map. The script prints the estimated flash cost of the result.

Zones are emitted smallest-total-area first so enclaves (Lesotho, eSwatini,
Kaliningrad, ...) match before the larger zone that surrounds them — the
firmware takes the first hit. Antarctica/*, Etc/* and Asia/Urumqi are skipped
(the last one so all of China keeps official Beijing time, matching the box
table's deliberate choice).
"""

import argparse
import json
import math
import os
import sys

ZONEINFO = "/usr/share/zoneinfo"
SKIP_PREFIXES = ("Antarctica/", "Etc/")
SKIP_ZONES = {"Asia/Urumqi"}

# Hand-drawn zones absent from the source data (no IANA zone exists), added
# on every regeneration. Format matches the collected zones: (area deg^2,
# name, posix, [ring]) with rings as (lat, lon) centidegree pairs.
EXTRA_ZONES = [
    # Bouvetøya: uninhabited Norwegian dependency, UTC+0. Octagon around the
    # island (~49 km^2); the kZones box in Timezone.cpp still covers the
    # surrounding waters as fallback.
    (0.004, "Atlantic/Bouvet", "GMT0", [[
        (-5439, 338), (-5441, 344), (-5443, 346), (-5446, 344),
        (-5447, 338), (-5446, 332), (-5443, 330), (-5441, 332),
    ]]),
]
NAME_MAX = 19   # TzResult.name is char[20]
POSIX_MAX = 47  # TzResult.posix is char[48]


def posix_of(tzid):
    """POSIX TZ string from the TZif v2+ footer of the compiled zone file."""
    path = os.path.join(ZONEINFO, tzid)
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    if not data.endswith(b"\n"):
        return None
    i = data.rfind(b"\n", 0, len(data) - 1)
    if i < 0:
        return None
    s = data[i + 1:-1].decode("ascii", "replace")
    return s or None


def _fixed_posix(secs):
    """Fixed-offset POSIX string from a UTC offset in seconds (east-positive).
    POSIX sign is inverted (UTC+1 -> '<+01>-1')."""
    east = secs >= 0
    a = abs(secs)
    h, m = a // 3600, (a % 3600) // 60
    tag = str(h) if m == 0 else f"{h}:{m:02d}"
    nm = f"{'+' if east else '-'}{h:02d}" + ("" if m == 0 else f"{m:02d}")
    return f"<{nm}>{'-' if east else '+'}{tag}"


def sanitize_posix(tzid, posix):
    """The MCU's POSIX evaluator only understands Mm.w.d DST rules. A few zones
    (e.g. Morocco / W. Sahara, whose Ramadan model uses Julian Jn / n rules)
    ship rules the firmware can't parse, so tz_offset_at would fall back to
    UTC. Replace those with the zone's current fixed offset (no DST)."""
    rules = posix.split(",")[1:]
    if rules and any(not r.split("/")[0].startswith("M") for r in rules):
        try:
            import datetime
            from zoneinfo import ZoneInfo
            off = datetime.datetime.now(ZoneInfo(tzid)).utcoffset()
            fixed = _fixed_posix(int(off.total_seconds()))
        except Exception:
            fixed = posix.split(",")[0]  # at least drop the unparseable DST
        print(f"  ~ {tzid}: non-M DST rule, using fixed {fixed} (was {posix})",
              file=sys.stderr)
        return fixed
    return posix


def eff_area(ring):
    """Shoelace area in deg^2, lon scaled by cos(mid-lat) => ~equal-area."""
    lat0 = sum(p[1] for p in ring) / len(ring)
    c = math.cos(math.radians(max(-85.0, min(85.0, lat0))))
    a = 0.0
    for i in range(len(ring)):
        x1, y1 = ring[i - 1][0] * c, ring[i - 1][1]
        x2, y2 = ring[i][0] * c, ring[i][1]
        a += x1 * y2 - x2 * y1
    return abs(a) / 2.0


def douglas_peucker(pts, eps):
    """Iterative DP on (lon, lat) point list; keeps endpoints."""
    n = len(pts)
    keep = [False] * n
    keep[0] = keep[n - 1] = True
    stack = [(0, n - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        ax, ay = pts[a]
        bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy)
        dmax, imax = -1.0, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            if norm == 0.0:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs(dx * (py - ay) - dy * (px - ax)) / norm
            if d > dmax:
                dmax, imax = d, i
        if dmax > eps:
            keep[imax] = True
            stack.append((a, imax))
            stack.append((imax, b))
    return [p for p, k in zip(pts, keep) if k]


def simplify_ring(ring, eps, max_verts):
    """Ring of (lon, lat), closed or not -> open cdeg (lat, lon) list."""
    pts = ring[:-1] if ring[0] == ring[-1] else ring[:]
    if len(pts) > 6000:  # cheap pre-thin before O(n^2)-ish DP
        step = len(pts) // 6000 + 1
        pts = pts[::step]
    if len(pts) < 3:
        return None
    pts = pts + [pts[0]]  # close so DP keeps the wrap-around shape
    e = eps
    while True:
        simp = douglas_peucker(pts, e)
        if simp[0] == simp[-1]:
            simp = simp[:-1]
        cdeg, seen_prev = [], None
        for lon, lat in simp:
            p = (round(lat * 100), round(lon * 100))
            if p != seen_prev:
                cdeg.append(p)
                seen_prev = p
        if len(cdeg) > 1 and cdeg[0] == cdeg[-1]:
            cdeg.pop()
        if len(cdeg) <= max_verts:
            break
        e *= 1.4
    if len(cdeg) < 3:
        return None
    lons = [p[1] for p in cdeg]
    if max(lons) - min(lons) > 18000:
        return None  # crosses the antimeridian; source data splits these anyway
    return cdeg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("combined_json")
    ap.add_argument("-o", "--out", default="src/TimezonePolyData.h")
    ap.add_argument("--eps", type=float, default=0.20)
    ap.add_argument("--max-parts", type=int, default=15)
    ap.add_argument("--max-verts", type=int, default=48)
    ap.add_argument("--min-area", type=float, default=0.15)
    args = ap.parse_args()

    print("loading", args.combined_json, "...", file=sys.stderr)
    gj = json.load(open(args.combined_json))

    zones = []       # (total_area, tzid, posix, [cdeg_ring, ...])
    dropped_zones = []
    for feat in gj["features"]:
        tzid = feat["properties"].get("tzid", "")
        if not tzid or tzid.startswith(SKIP_PREFIXES) or tzid in SKIP_ZONES:
            continue
        posix = posix_of(tzid)
        if not posix:
            print("  ! no POSIX string for", tzid, "- skipped", file=sys.stderr)
            continue
        posix = sanitize_posix(tzid, posix)
        if len(posix) > POSIX_MAX:
            print("  ! POSIX too long for", tzid, "- skipped:", posix, file=sys.stderr)
            continue
        geom = feat["geometry"]
        polys = geom["coordinates"]
        if geom["type"] == "Polygon":
            polys = [polys]
        rings = [(eff_area(p[0]), p[0]) for p in polys]  # outer rings only
        rings.sort(key=lambda r: -r[0])
        kept = []
        for area, ring in rings[: args.max_parts * 3]:
            if area < args.min_area or len(kept) >= args.max_parts:
                break
            simp = simplify_ring(ring, args.eps, args.max_verts)
            if simp:
                kept.append(simp)
        if kept:
            zones.append((sum(a for a, _ in rings), tzid, posix, kept))
        else:
            dropped_zones.append(tzid)

    zones.extend(EXTRA_ZONES)
    zones.sort(key=lambda z: (z[0], z[1]))  # smallest first: enclaves win

    # ---- emit ---------------------------------------------------------------
    n_polys = sum(len(z[3]) for z in zones)
    n_verts = sum(len(r) for z in zones for r in z[3])
    if n_verts > 65535:
        sys.exit(f"error: {n_verts} vertex pairs exceed uint16 'first' range")

    names = {z[1][:NAME_MAX] for z in zones}
    posixes = {z[2] for z in zones}
    est = (n_verts * 4 + n_polys * 20 + sum(len(n) + 1 for n in names)
           + sum(len(p) + 1 for p in posixes))

    editor_state = {"version": 1, "zones": []}
    vert_lines, zone_lines = [], []
    first = 0
    for _, tzid, posix, rings in zones:
        name = tzid[:NAME_MAX]
        for ring in rings:
            vert_lines.append(f"    // {name} ({len(ring)} verts)")
            flat = [str(v) for p in ring for v in p]
            for i in range(0, len(flat), 12):
                vert_lines.append("    " + ", ".join(flat[i:i + 12]) + ",")
            lats = [p[0] for p in ring]
            lons = [p[1] for p in ring]
            zone_lines.append(
                f"    {{{min(lats)}, {max(lats)}, {min(lons)}, {max(lons)}, "
                f"{first}, {len(ring)}, \"{name}\", \"{posix}\"}},")
            first += len(ring)
            editor_state["zones"].append({
                "name": name, "posix": posix,
                "verts": [[p[0] / 100.0, p[1] / 100.0] for p in ring]})

    hdr = f"""// TimezonePolyData.h — polygon timezone regions, checked before the box table.
// GENERATED by tools/gen_tz_polys.py from timezone-boundary-builder data
// (ODbL, © OpenStreetMap contributors) — regenerate, don't hand-edit.
// Load this file into tools/tz_polygon_editor.html (Import) to refine zones:
// the full editor state is embedded in the EDITOR-STATE comment at the bottom.
//
// Format: kPolyVerts holds flattened [lat, lon] pairs in centi-degrees
// (deg * 100, int16, 0.01 deg ~ 1.1 km). Each TzPolyZone references a slice
// of it plus a precomputed bounding box used as a cheap prefilter. Zones are
// matched top-down, first hit wins, before the kZones boxes in Timezone.cpp;
// they are ordered smallest-first so enclaves beat their surrounding zone.
// Polygons must not cross the antimeridian — split them in the editor.
//
// {len(zones)} zones, {n_polys} polygons, {n_verts} vertices, ~{est} bytes of flash.

static const int16_t kPolyVerts[] = {{
{chr(10).join(vert_lines)}
}};

static const TzPolyZone kPolyZones[] = {{
{chr(10).join(zone_lines)}
}};

static const unsigned kPolyZoneCount = sizeof(kPolyZones) / sizeof(kPolyZones[0]);

// EDITOR-STATE: {json.dumps(editor_state, separators=(',', ':'))}
"""
    open(args.out, "w").write(hdr)

    print(f"wrote {args.out}:", file=sys.stderr)
    print(f"  {len(zones)} zones, {n_polys} polygons, {n_verts} vertices",
          file=sys.stderr)
    print(f"  estimated flash cost ~{est} bytes "
          f"(verts {n_verts * 4}, structs {n_polys * 20}, strings "
          f"{est - n_verts * 4 - n_polys * 20})", file=sys.stderr)
    print(f"  {len(dropped_zones)} zones below --min-area (box table covers "
          f"them): {', '.join(sorted(dropped_zones)[:12])}"
          f"{' ...' if len(dropped_zones) > 12 else ''}", file=sys.stderr)


if __name__ == "__main__":
    main()
