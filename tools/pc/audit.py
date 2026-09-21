#!/usr/bin/env python3
"""Generate a conservative source inventory, not a C call graph or ABI proof."""
from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LEXICAL_NONCODE = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
TOKEN = re.compile(r"\b[A-Za-z_]\w*\b")
ADDRESS = re.compile(r"\b0[xX]([0-9a-fA-F]+)\b")


def mask_noncode(text: str) -> str:
    return LEXICAL_NONCODE.sub(lambda m: re.sub(r"[^\n]", " ", m[0]), text)


def inventory() -> dict:
    with (ROOT / "config/slus_01411/functions.csv").open() as stream:
        functions = list(csv.DictReader(stream))
    sdk = {row["name"]: row for row in functions if row["status"] == "sdk_asm"}
    refs: dict[str, list[dict]] = defaultdict(list)
    hazards: list[dict] = []
    paths = sorted(
        p for base in (ROOT / "src/game", ROOT / "src/overlays")
        for p in base.rglob("*") if p.suffix in (".c", ".h")
    )
    for path in paths:
        source = path.relative_to(ROOT).as_posix()
        for lineno, line in enumerate(mask_noncode(path.read_text()).splitlines(), 1):
            tokens = set(TOKEN.findall(line))
            for name in sorted(tokens & sdk.keys()):
                refs[name].append({"source": source, "line": lineno})
            kinds = set()
            if any(name.startswith("gte_") for name in tokens):
                kinds.add("gte_macro")
            if tokens & {"asm", "__asm", "__asm__"}:
                kinds.add("asm_or_symbol_alias")
            if re.search(r"\(\s*(?:u32|s32|unsigned\s+int|int)\s*\)\s*&", line):
                kinds.add("address_to_32bit_cast")
            for match in ADDRESS.finditer(line):
                value = int(match[1], 16)
                if 0x1F800000 <= value < 0x1F800400:
                    kinds.add("scratchpad_literal")
                elif 0x1F801000 <= value < 0x1F803000:
                    kinds.add("mmio_literal")
                elif 0x80000000 <= value < 0x80200000:
                    kinds.add("ram_literal_or_integer_constant")
            for kind in sorted(kinds):
                hazards.append({"kind": kind, "source": source, "line": lineno,
                                "code": line.strip()})
    overlays = []
    metadata = json.loads((ROOT / "config/slus_01411/overlays.json").read_text())
    for module in metadata["modules"]:
        manifest = ROOT / f"config/slus_01411/overlays/{module['name']}_functions.csv"
        with manifest.open() as stream:
            rows = list(csv.DictReader(stream))
        overlays.append({**module, "function_status": dict(Counter(r["status"] for r in rows))})
    return {
        "schema": 1,
        "limitations": [
            "SDK references are identifier occurrences, including declarations and address tables; not resolved calls.",
            "Source scanning ignores comments/strings but does not preprocess or expand macros.",
            "Hazards are review candidates, not a complete pointer/layout audit; integer constants can resemble addresses.",
            "Configured overlays do not prove coverage of all executable disc payloads.",
        ],
        "resident_status": dict(Counter(row["status"] for row in functions)),
        "scanned_source_files": len(paths),
        "handwritten_assembly": [row for row in functions if row["status"] == "handwritten_asm"],
        "sdk_references": [
            {"symbol": name, "module": sdk[name]["module"], "address": sdk[name]["address"],
             "references": refs[name], "backend_status": "needs_contract_review"}
            for name in sorted(refs)
        ],
        "hazard_counts": dict(sorted(Counter(h["kind"] for h in hazards).items())),
        "hazards": hazards,
        "configured_overlays": overlays,
        "runtime_banks_to_audit": [
            {"archive": "MODEL.MRG", "start": "0x8013A000", "end": "0x80146000"},
            {"archive": "WA_MRG.MRG", "start": "0x80146000", "end": "0x80168000"},
            {"archive": "WA_MRG.MRG", "start": "0x80168000", "end": "0x8017A000"},
            {"archive": "MODEL.MRG", "start": "0x8017A000", "end": "0x80180000"},
            {"archive": "SU.MRG", "start": "0x80180000", "end": "0x801AC000"},
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "tmp/reports/pc-port-audit.json")
    args = parser.parse_args()
    report = inventory()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PC audit: {report['scanned_source_files']} source files, "
          f"{len(report['sdk_references'])} referenced resident SDK symbols, "
          f"{len(report['handwritten_assembly'])} ASM targets")
    print(f"Review candidates: {report['hazard_counts']}")
    print(f"Report: {args.output}")


if __name__ == "__main__":
    main()
