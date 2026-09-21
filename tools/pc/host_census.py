#!/usr/bin/env python3
"""Syntax-check every game/overlay C unit with the host compiler.

A review aid for the memory-model decision: it reports which units survive an
ILP32 (-m32) or LP64 host front end, not whether they link or behave."""
import argparse, collections, concurrent.futures, glob, json, os, re, subprocess

# GCC 2.8.1-era leniency and the MIPS front end's language predefines.
NATIVE_FLAGS = ["-std=gnu11", "-fpermissive", "-DMEMORIES_PC", "-D_LANGUAGE_C", "-DLANGUAGE_C"]

def check(args):
    path, flags = args
    run = subprocess.run(["gcc", *flags, *NATIVE_FLAGS, "-fsyntax-only", "-w", "-Isrc", path],
                         capture_output=True, text=True)
    errors = [re.sub(r"[‘'][^’']*[’']", "X", line.split("error: ", 1)[1])
              for line in run.stderr.splitlines() if "error: " in line]
    return path, run.returncode == 0, errors

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="tmp/pc/host-census.json")
    options = parser.parse_args()
    units = sorted(glob.glob("src/game/*.c") + glob.glob("src/overlays/*/*.c"))
    report = {}
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for name, flags in (("ilp32", ["-m32"]), ("lp64", [])):
            results = list(pool.map(check, [(unit, flags) for unit in units]))
            kinds = collections.Counter(e for _, _, errors in results for e in errors)
            report[name] = {"passed": sum(ok for _, ok, _ in results), "units": len(units),
                            "failed": [p for p, ok, _ in results if not ok],
                            "error_kinds": dict(kinds.most_common())}
            print(f"{name}: {report[name]['passed']} / {len(units)} units pass")
    os.makedirs(os.path.dirname(options.output), exist_ok=True)
    with open(options.output, "w") as handle:
        json.dump(report, handle, indent=1)

if __name__ == "__main__":
    main()
