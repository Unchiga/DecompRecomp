#!/usr/bin/env python3
"""Build tools the PC build needs, fetched into tmp/pc/tools when missing.

CMake and Ninja (for SDL3 and the Windows libraries) come from PATH when
they are installed, else from their pinned official releases. On Windows,
llvm-mingw (the compiler) does too; on Linux, build_win32_deps.py fetches
its Linux release for cross builds. Every archive is checked against its
SHA-256 before it is unpacked. ensure() puts the tool's directory first on
PATH, so later commands in the same process find it."""
import hashlib, os, shutil, stat, sys, tarfile, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tmp", "pc", "tools")
WINDOWS = sys.platform == "win32"
# name: (url, sha256, directory holding the program inside the archive)
RELEASES = {
    "cmake": (("https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip",
               "4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab", "cmake-4.4.3-windows-x86_64/bin")
              if WINDOWS else
              ("https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-linux-x86_64.tar.gz",
               "d6c83076c575bc00b823522ac974bda66d0af05d6ddc30e739c12385cf32c6cc", "cmake-4.4.3-linux-x86_64/bin")),
    "ninja": (("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
               "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65", ".")
              if WINDOWS else
              ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip",
               "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6", ".")),
    # Windows only: the same llvm-mingw release build_win32_deps.py uses on Linux.
    "llvm-mingw": ("https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip",
                   "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666", "llvm-mingw-20260922-ucrt-x86_64/bin"),
}
PROGRAMS = {"cmake": "cmake", "ninja": "ninja", "llvm-mingw": "i686-w64-mingw32-clang"}


def download(url, digest):
    path = os.path.join(TOOLS, "downloads", os.path.basename(url))
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"fetch {url}", flush=True)
        with urllib.request.urlopen(url, timeout=300) as response, open(path + ".part", "wb") as handle:
            shutil.copyfileobj(response, handle)
        os.replace(path + ".part", path)
    sha = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            sha.update(block)
    if sha.hexdigest() != digest:
        os.remove(path)
        sys.exit(f"{path}: SHA-256 {sha.hexdigest()}, expected {digest}; run again")
    return path


def unpack(archive, destination):
    os.makedirs(destination, exist_ok=True)
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.infolist():
                if os.path.isabs(member.filename) or ".." in member.filename.split("/"):
                    sys.exit(f"{archive}: an entry outside the archive: {member.filename}")
            bundle.extractall(destination)
            for member in bundle.infolist():   # zip keeps Unix modes in the high bits
                mode = member.external_attr >> 16
                if mode & 0o111:
                    path = os.path.join(destination, member.filename)
                    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    else:
        with tarfile.open(archive) as bundle:
            if hasattr(tarfile, "data_filter"):
                bundle.extractall(destination, filter="data")
            else:
                bundle.extractall(destination)


def ensure(name):
    """The tool on PATH: the installed one, else the pinned release."""
    program = PROGRAMS[name]
    url, digest, inner = RELEASES[name]
    fetched = os.path.join(TOOLS, name, inner)
    if os.path.isdir(fetched):
        add_to_path(fetched)
    if shutil.which(program):
        return shutil.which(program)
    if name == "llvm-mingw" and not WINDOWS:
        sys.exit("llvm-mingw for Linux comes from tools/pc/build_win32_deps.py")
    shutil.rmtree(os.path.join(TOOLS, name), ignore_errors=True)
    unpack(download(url, digest), os.path.join(TOOLS, name))
    add_to_path(fetched)
    if not shutil.which(program):
        sys.exit(f"{name}: {program} is not in {fetched} after unpacking")
    return shutil.which(program)


def add_to_path(directory):
    directory = os.path.abspath(directory)
    if directory not in os.environ["PATH"].split(os.pathsep):
        os.environ["PATH"] = directory + os.pathsep + os.environ["PATH"]


if __name__ == "__main__":
    for tool in sys.argv[1:] or ["cmake", "ninja"]:
        print(ensure(tool))
