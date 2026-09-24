#!/usr/bin/env python3
"""Report a Windows crash of memories-pc.exe from the minidump Windows wrote.

Some crashes never reach the game's own handler (src/pc/debug/crash.c): a
fail-fast, or a stack pointer outside any writable memory, where Windows
cannot deliver the exception and ends the process at once. The game's
monitor (src/pc/debug/monitor.c) still writes crash-<pid>.txt then, with
the exit code, the log and the console output, but no dump. Windows Error
Reporting writes one to %LOCALAPPDATA%\\CrashDumps when local dumps are on;
this reads it and writes tmp/pc/crash-<pid>-wer.txt beside the monitor's
report (and copies the dump to crash-<pid>-wer.dmp): the exception,
the registers and what they point into, the game thread's frame chain, and
the Windows clock's repair counters (src/pc/platform/win32.c).

    python tools/pc/crash_report.py            newest dump of memories-pc.exe
    python tools/pc/crash_report.py --since T  only one written after time T (epoch seconds);
                                               --wait S gives Windows S seconds to write it
    python tools/pc/crash_report.py DUMP       that dump

tools/pc/run_debug_windows.bat runs it when the game crashes.
"""
import argparse
import bisect
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE_NAME = "memories-pc.exe"
GAME_STACK = (0xB0000000, 0xB0800000)  # src/pc/guest/state.c
CODES = {
    0xC0000005: "access violation",
    0xC000001D: "illegal instruction",
    0xC00000FD: "stack overflow",
    0xC0000094: "integer divide by zero",
    0xC0000409: "stack buffer overrun / fail fast",
    0x80000003: "breakpoint",
}
CLOCK_COUNTERS = ("_lost_redirects", "_undone_faults", "_skipped_unreliable", "_in_tick", "_redirected", "_held",
                  "_last_loaded_slot")


class Dump:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = data = handle.read()
        signature, _, count, directory = struct.unpack_from("<IIII", data, 0)
        if signature != 0x504D444D:
            raise ValueError(f"{path} is not a minidump")
        self.streams = {}
        for index in range(count):
            kind, size, rva = struct.unpack_from("<III", data, directory + 12 * index)
            self.streams.setdefault(kind, (size, rva))
        self.modules = self._modules()
        self.memory = self._memory()
        self.threads = self._threads()
        self.exception = self._exception()

    def _string(self, rva):
        length = struct.unpack_from("<I", self.data, rva)[0]
        return self.data[rva + 4:rva + 4 + length].decode("utf-16le")

    def _modules(self):
        if 4 not in self.streams:
            return []
        _, rva = self.streams[4]
        count = struct.unpack_from("<I", self.data, rva)[0]
        modules = []
        for index in range(count):
            base, size, _, stamp, name = struct.unpack_from("<QIIII", self.data, rva + 4 + 108 * index)
            modules.append((base & 0xFFFFFFFF, size, stamp, self._string(name)))
        return sorted(modules)

    def _memory(self):
        ranges = []
        if 9 in self.streams:  # Memory64ListStream: one blob, ranges back to back
            _, rva = self.streams[9]
            count, offset = struct.unpack_from("<QQ", self.data, rva)
            for index in range(count):
                start, size = struct.unpack_from("<QQ", self.data, rva + 16 + 16 * index)
                ranges.append((start & 0xFFFFFFFF, size, offset))
                offset += size
        if 5 in self.streams:  # MemoryListStream
            _, rva = self.streams[5]
            count = struct.unpack_from("<I", self.data, rva)[0]
            for index in range(count):
                start, size, offset = struct.unpack_from("<QII", self.data, rva + 4 + 16 * index)
                ranges.append((start & 0xFFFFFFFF, size, offset))
        return sorted(ranges)

    def _threads(self):
        if 3 not in self.streams:
            return {}
        _, rva = self.streams[3]
        count = struct.unpack_from("<I", self.data, rva)[0]
        threads = {}
        for index in range(count):
            tid, _, _, _, teb, stack, stack_size, _, _, context = struct.unpack_from(
                "<IIIIQQIIII", self.data, rva + 4 + 48 * index)
            threads[tid] = (teb, stack & 0xFFFFFFFF, stack_size, context)
        return threads

    def _exception(self):
        if 6 not in self.streams:
            return None
        _, rva = self.streams[6]
        tid = struct.unpack_from("<I", self.data, rva)[0]
        code, _, _, address, count = struct.unpack_from("<IIQQI", self.data, rva + 8)
        parameters = struct.unpack_from("<15Q", self.data, rva + 8 + 32)[:count]
        _, context = struct.unpack_from("<II", self.data, rva + 8 + 152)
        return tid, code, address & 0xFFFFFFFF, [p & 0xFFFFFFFF for p in parameters], context

    def registers(self, context_rva):
        """An x86 CONTEXT: EDI ESI EBX EDX ECX EAX EBP EIP CS EFLAGS ESP from offset 156."""
        names = ("edi", "esi", "ebx", "edx", "ecx", "eax", "ebp", "eip", "cs", "eflags", "esp")
        return dict(zip(names, struct.unpack_from("<11I", self.data, context_rva + 156)))

    def read(self, address, size):
        for start, length, offset in self.memory:
            if start <= address and address + size <= start + length:
                return self.data[offset + address - start:offset + address - start + size]
        return None

    def word(self, address):
        raw = self.read(address, 4)
        return None if raw is None else struct.unpack("<I", raw)[0]

    def module_at(self, address):
        for base, size, stamp, name in self.modules:
            if base <= address < base + size:
                return base, size, stamp, name
        return None


class Symbols:
    def __init__(self, exe):
        self.names, self.addresses, self.text = [], [], (0, 0)
        try:
            output = subprocess.run(["llvm-nm", "-n", "--defined-only", exe], capture_output=True, text=True,
                                    check=True).stdout
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"crash_report: no symbols ({error})", file=sys.stderr)
            return
        low, high = None, 0
        for line in output.splitlines():
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                address = int(parts[0], 16)
            except ValueError:
                continue
            self.addresses.append(address)
            self.names.append(parts[2])
            if parts[1] in "Tt":
                low = address if low is None else min(low, address)
                high = max(high, address)
        self.text = (low or 0, high)

    def lookup(self, address):
        index = bisect.bisect_right(self.addresses, address) - 1
        if index < 0:
            return ""
        return f"{self.names[index]}+0x{address - self.addresses[index]:x}"

    def value(self, dump, name):
        if name in self.names:
            return dump.word(self.addresses[self.names.index(name)])
        return None

    def in_text(self, address):
        return self.text[0] <= address <= self.text[1]


def region(dump, symbols, address, stacks):
    if symbols.in_text(address):
        return f"game executable code ({symbols.lookup(address)})"
    if 0x80000000 <= address < 0x80200000 or 0xA0000000 <= address < 0xA0200000:
        return "guest RAM"
    if 0x1F800000 <= address < 0x1F801000:
        return "scratchpad"
    if GAME_STACK[0] <= address < GAME_STACK[1]:
        return "game stack"
    for low, high, tid in stacks:
        if low <= address < high:
            return f"stack of thread {tid}"
    module = dump.module_at(address)
    if module:
        return f"{os.path.basename(module[3])}+0x{address - module[0]:x}"
    return "unmapped / no module"


def frame_chain(dump, symbols, ebp, low, high, limit=40):
    frames = []
    while len(frames) < limit and low <= ebp and ebp + 8 <= high:
        saved, returned = dump.word(ebp), dump.word(ebp + 4)
        if saved is None or returned is None or not symbols.in_text(returned):
            break
        frames.append((ebp, returned))
        if saved <= ebp:
            break
        ebp = saved
    return frames


def stack_frames(dump, symbols, low, high, limit=40):
    """Every linked [saved EBP, return address] pair left on a stack whose
    registers are lost, nearest the top last: the frames of the code that
    was running, and below them older frames it had not yet overwritten."""
    raw = dump.read(low, high - low)
    if raw is None:
        return []
    words = struct.unpack("<%dI" % ((high - low) // 4), raw)

    def linked(index):
        saved, returned = words[index], words[index + 1]
        return low + 4 * index < saved < high and symbols.in_text(returned)

    frames = []
    for index in range(len(words) - 2):
        if not linked(index):
            continue
        target = (words[index] - low) // 4
        if target + 1 < len(words) and (linked(target) or words[target + 1] == 0):
            frames.append((low + 4 * index, words[index + 1]))
    return frames[-limit:]


def find_dump(since):
    folder = os.path.join(os.environ.get("LOCALAPPDATA", ""), "CrashDumps")
    dumps = [path for path in glob.glob(os.path.join(folder, EXE_NAME + ".*.dmp"))
             if os.path.getmtime(path) >= since]
    return max(dumps, key=os.path.getmtime) if dumps else None


def report(path, out):
    dump = Dump(path)
    exe_module = next((m for m in dump.modules if os.path.basename(m[3]).lower() == EXE_NAME), None)
    lines = []
    exe = exe_module[3] if exe_module else os.path.join(ROOT, "tmp", "pc", "game32", EXE_NAME)
    symbols = Symbols(exe)
    if exe_module and os.path.exists(exe):
        with open(exe, "rb") as handle:
            head = handle.read(4096)
        pe = struct.unpack_from("<I", head, 0x3C)[0]
        if struct.unpack_from("<I", head, pe + 8)[0] != exe_module[2]:
            lines.append(f"warning: {exe} was rebuilt after the crash; symbol names below may be wrong")
    stacks = [(stack, stack + size, tid) for tid, (_, stack, size, _) in dump.threads.items()]
    if dump.exception is None:
        lines.append("no exception record in the dump")
    else:
        tid, code, address, parameters, context = dump.exception
        regs = dump.registers(context)
        what = CODES.get(code, "exception")
        detail = ""
        if code == 0xC0000005 and len(parameters) >= 2:
            detail = {0: "reading", 1: "writing", 8: "executing"}.get(parameters[0], "accessing")
            detail = f" {detail} 0x{parameters[1]:08x} ({region(dump, symbols, parameters[1], stacks)})"
        lines.append(f"memories-pc: fatal exception 0x{code:08x} ({what}){detail}")
        lines.append(f"at EIP 0x{address:08x}: {region(dump, symbols, address, stacks)}; thread {tid}"
                     + (" (game thread)" if GAME_STACK[0] <= dump.threads.get(tid, (0, 0))[1] < GAME_STACK[1]
                        else ""))
        lines.append("registers: " + " ".join(f"{name.upper()}=0x{value:08x}" for name, value in regs.items()
                                              if name != "cs"))
        for name in ("esp", "ebp"):
            lines.append(f"  {name.upper()} points into {region(dump, symbols, regs[name], stacks)}")
        _, low, size, _ = dump.threads.get(tid, (0, 0, 0, 0))
        high = low + size
        if GAME_STACK[0] <= low < GAME_STACK[1]:
            low, high = GAME_STACK
        stack_ok = low <= regs["esp"] < high
        if not stack_ok:
            lines.append("  the stack pointer is outside the thread's stack: Windows could not deliver the exception,"
                         " so the game's own crash handler never ran")
        chain = frame_chain(dump, symbols, regs["ebp"], low, high)
        if chain:
            lines.append("frame chain:")
        else:
            chain = stack_frames(dump, symbols, low, high)
            lines.append("frames left on the stack (the registers are lost; outermost last, the deepest may be"
                         " stale ones below where the thread really was):")
        lines.append(f"  #0 0x{regs['eip']:08x} {symbols.lookup(regs['eip']) if symbols.in_text(regs['eip']) else ''}")
        for index, (frame, returned) in enumerate(chain, 1):
            lines.append(f"  #{index} 0x{returned:08x} {symbols.lookup(returned)}  (frame 0x{frame:08x})")
    counters = [(name.lstrip("_"), symbols.value(dump, name)) for name in CLOCK_COUNTERS]
    known = [f"{name}={value}" for name, value in counters if value is not None]
    if known:
        lines.append("windows clock: " + " ".join(known))
    lines.append(f"dump: {path}")
    text = "\n".join(lines) + "\n"
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(text)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", nargs="?")
    parser.add_argument("--since", type=float, default=0.0)
    parser.add_argument("--wait", type=float, default=0.0, help="seconds to wait for Windows to finish the dump")
    args = parser.parse_args()
    path = args.dump or find_dump(args.since)
    deadline = time.time() + args.wait
    while not path and time.time() < deadline:
        time.sleep(0.5)
        path = find_dump(args.since)
    if path and not args.dump:
        size = -1
        while os.path.getsize(path) != size and time.time() < deadline + 5:  # still being written
            size = os.path.getsize(path)
            time.sleep(0.5)
    if not path:
        print("crash_report: Windows wrote no crash dump. To have it keep them, create the registry key\n"
              "  HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\n"
              "(as administrator; dumps then go to %LOCALAPPDATA%\\CrashDumps).", file=sys.stderr)
        return 1
    numbers = re.findall(r"\d+", os.path.basename(path))
    pid = numbers[-1] if numbers else "unknown"
    folder = os.path.join(ROOT, "tmp", "pc")
    os.makedirs(folder, exist_ok=True)
    kept = os.path.join(folder, f"crash-{pid}-wer.dmp")
    if os.path.abspath(path) != os.path.abspath(kept):
        shutil.copyfile(path, kept)
    out = os.path.join(folder, f"crash-{pid}-wer.txt")
    sys.stdout.write(report(kept, out))
    print(f"crash_report: wrote {os.path.relpath(out, ROOT)} and {os.path.relpath(kept, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
