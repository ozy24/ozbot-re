"""Sum final frag scores by bot-id parity from a telemetry JSONL.

Usage:
    py tools/parity_frags.py <telemetry.jsonl> [more.jsonl ...]

Companion to the id-parity head-to-head A/Bs (bot_skilltest, bot_leadtest):
bots are split into treatment/control by bot-id parity within one match, so
comparing the two parities' pooled kills isolates the treatment effect.
run_parallel.py merges workers with an even per-instance id offset, so parity
survives the merge.
"""
import json
import sys

# Minimum pooled frag count for a kill-SHARE to mean anything.  Below this the
# denominator is dominated by suicide penalties (see the guard in main()).
MIN_FRAG_POOL = 80


def main():
    even = odd = deven = dodd = 0
    for path in sys.argv[1:]:
        last = {}
        deaths = {}
        with open(path, "r", encoding="utf-8") as fp:
            for line in fp:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                if rec.get("type") == "tick" and "score" in rec:
                    last[rec["bot"]] = rec["score"]
                elif rec.get("type") == "event" and rec.get("event") == "death":
                    deaths[rec["bot"]] = deaths.get(rec["bot"], 0) + 1
        e = sum(v for k, v in last.items() if k % 2 == 0)
        o = sum(v for k, v in last.items() if k % 2 == 1)
        de = sum(v for k, v in deaths.items() if k % 2 == 0)
        do = sum(v for k, v in deaths.items() if k % 2 == 1)
        even, odd, deven, dodd = even + e, odd + o, deven + de, dodd + do
        # `score` is the Quake 2 frag count, which DECREMENTS on a suicide.  On
        # hazard-heavy maps (q2dm3/4/6/7) most bots finish negative, so the
        # even/(even+odd) kill SHARE divides by ~zero and returns nonsense --
        # observed shifts of +420pt and -216pt on a scale bounded to +-100.
        # Refuse to report a share from a degenerate pool rather than emit a
        # number that looks usable.  Deaths are event counts and always valid.
        nneg = sum(1 for v in last.values() if v < 0)
        warn = ""
        if e + o < MIN_FRAG_POOL:
            warn = (f"  <-- UNUSABLE: frag pool {e + o} < {MIN_FRAG_POOL}"
                    f" ({nneg} bots below zero); kill-share is meaningless here,"
                    f" use deaths or a non-hazard map")
        elif nneg:
            warn = f"  (note: {nneg} bots finished with a negative score)"
        print(f"{path}: kills even={e} odd={o}  deaths even={de} odd={do}{warn}")
    if len(sys.argv) > 2:
        tag = "" if even + odd >= MIN_FRAG_POOL else "  <-- UNUSABLE pooled frag pool"
        print(f"pooled: kills even={even} odd={odd}  deaths even={deven} odd={dodd}{tag}")


if __name__ == "__main__":
    main()
