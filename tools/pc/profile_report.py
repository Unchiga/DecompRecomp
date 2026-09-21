#!/usr/bin/env python3
"""Summarize a MEMORIES_PROFILE sampling output file."""
import argparse
from collections import defaultdict


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("profile")
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args()
    rows = []
    inclusive = defaultdict(int)
    with open(args.profile, encoding="utf-8") as source:
        for line in source:
            address, count, symbol = line.split(maxsplit=2)
            count = int(count)
            symbol = symbol.strip()
            rows.append((count, address, symbol))
            inclusive[symbol.split("+0x", 1)[0]] += count
    total = sum(row[0] for row in rows) or 1
    print("Top sample buckets")
    for count, address, symbol in sorted(rows, reverse=True)[:args.top]:
        print(f"{count:8d} {count * 100 / total:6.2f}%  {address}  {symbol}")
    print("\nInclusive by symbol")
    for symbol, count in sorted(inclusive.items(), key=lambda item: (-item[1], item[0]))[:args.top]:
        print(f"{count:8d} {count * 100 / total:6.2f}%  {symbol}")


if __name__ == "__main__":
    main()
