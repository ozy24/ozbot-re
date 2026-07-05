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
        print(f"{path}: kills even={e} odd={o}  deaths even={de} odd={do}")
    if len(sys.argv) > 2:
        print(f"pooled: kills even={even} odd={odd}  deaths even={deven} odd={dodd}")


if __name__ == "__main__":
    main()
