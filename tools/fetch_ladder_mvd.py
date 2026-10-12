#!/usr/bin/env python3
"""
Fetch the DM Duel Ladder's match demos (.mvd2) into demos/mvd/.

The ladder site is the only long-term copy of each recording (the game
server's ladder_ship deletes its local copy after upload), so this pulls:

    GET /api/duel/matches?limit=100&before=<start_ms>   (paged match list)
    GET /api/duel/matches/<id>                          -> <id>.json
    GET /api/duel/matches/<id>/recording                -> <id>.mvd2.gz

and appends one row per new match to demos/mvd/manifest.jsonl.  Incremental:
ids already on disk are skipped, so re-running only fetches new matches.
Recordings are stored as served (gzip); tools/mvd2parse.py reads them as is.

Usage:
    py fetch_ladder_mvd.py [--base URL] [--limit N] [--dry-run]
"""

import json
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "demos", "mvd")
BASE = "https://ladder.darkmatter-quake.com"
PAGE = 100


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "ozbot-re fetch_ladder_mvd"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def list_matches(base):
    out, before = [], None
    while True:
        url = f"{base}/api/duel/matches?limit={PAGE}"
        if before is not None:
            url += f"&before={before}"
        page = json.loads(get(url))
        if not page:
            break
        out.extend(page)
        oldest = min(m["start_ms"] for m in page)
        if len(page) < PAGE or oldest == before:
            break
        before = oldest
    seen, uniq = set(), []
    for m in out:
        if m["match_id"] not in seen:
            seen.add(m["match_id"])
            uniq.append(m)
    return uniq


def main():
    args = sys.argv[1:]

    def opt(name, default=None, cast=str):
        if name in args:
            return cast(args[args.index(name) + 1])
        return default

    base = opt("--base", BASE).rstrip("/")
    limit = opt("--limit", None, int)
    dry = "--dry-run" in args

    os.makedirs(OUT, exist_ok=True)
    matches = [m for m in list_matches(base) if m.get("has_demo")]
    matches.sort(key=lambda m: m["start_ms"])
    todo = [m for m in matches
            if not os.path.exists(os.path.join(OUT, m["match_id"] + ".mvd2.gz"))]
    print(f"[fetch] {len(matches)} matches with a demo, {len(todo)} new")
    if limit:
        todo = todo[:limit]
    if dry:
        for m in todo:
            print(f"  {m['match_id']}  {m['map_name']}")
        return

    manifest = os.path.join(OUT, "manifest.jsonl")
    total = 0
    for i, m in enumerate(todo, 1):
        mid = m["match_id"]
        detail = get(f"{base}/api/duel/matches/{mid}")
        demo = get(f"{base}/api/duel/matches/{mid}/recording")
        if demo[:2] != b"\x1f\x8b":
            print(f"  [skip] {mid}: recording is not gzip ({demo[:8]!r})")
            continue
        with open(os.path.join(OUT, mid + ".json"), "wb") as f:
            f.write(detail)
        # write the demo last: its presence is the "already fetched" marker
        tmp = os.path.join(OUT, mid + ".mvd2.gz.part")
        with open(tmp, "wb") as f:
            f.write(demo)
        os.replace(tmp, os.path.join(OUT, mid + ".mvd2.gz"))
        row = {"id": mid, "map": m["map_name"], "rule_set": m.get("rule_set"),
               "start_ms": m["start_ms"], "duration_ms": m["duration_ms"],
               "end_reason": m.get("end_reason"),
               "players": [{"name": p["name"], "identity_id": p.get("identity_id"),
                            "slot": p.get("slot"), "score": p.get("score"),
                            "outcome": p.get("outcome"),
                            "rating_before": p.get("rating_before")}
                           for p in m["players"]],
               "bytes": len(demo)}
        with open(manifest, "a", encoding="utf-8") as f:
            f.write(json.dumps(row) + "\n")
        total += len(demo)
        print(f"  [{i}/{len(todo)}] {mid}  {m['map_name']}  {len(demo) / 1e6:.1f} MB")
    print(f"[fetch] done, {total / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
