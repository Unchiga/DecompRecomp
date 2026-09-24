#!/usr/bin/env python3
"""Fetch an old 32-bit Linux system to build the Linux executable against.

A program built on this machine asks for this machine's glibc (2.43 on Arch
in 2026), which most people's Linux does not have yet. Built against Debian
11's (glibc 2.31) instead, it runs on Debian 11, Ubuntu 20.04 and anything
newer. This script fetches Debian 11's i386 packages into
tmp/pc/linux-sysroot, with no root and no container: the headers and
libraries for the game and for SDL3, checked against the archive's SHA-256
sums. It then lays them out so the host's gcc -m32 finds them with
--sysroot, and builds SDL3 (the pinned release the Windows build ships as
well) against them into tmp/pc/sdl-m32-portable.

build_game32.py builds every Linux executable against the result, so the one
a developer runs is the one that is shared. Debian's own 32-bit libgcc comes
with it, so the host needs gcc and binutils but not their 32-bit (multilib)
parts; CMake and Ninja come from tools/pc/fetch_tools.py when missing."""
import hashlib, lzma, os, re, shlex, shutil, subprocess, sys, tarfile, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SYSROOT = os.path.join(ROOT, "tmp", "pc", "linux-sysroot")
DOWNLOADS = os.path.join(ROOT, "tmp", "pc", "linux-sysroot-downloads")
SDL_BUILD = os.path.join(ROOT, "tmp", "pc", "sdl-m32-portable")
SDL_SOURCE = os.path.join(ROOT, "tmp", "pc", "sdl-source", "SDL3-3.4.16")
SDL_ARCHIVE = ("https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz",
               "7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68")
GCC_LIB = "usr/lib/gcc/i686-linux-gnu/10"   # crtbegin.o and libgcc.a, from libgcc-10-dev
MIRRORS = ["https://deb.debian.org/debian", "https://archive.debian.org/debian"]
SUITE = "bullseye"
# What the game links (libc, GL, FreeType, fontconfig, libpng) and the
# headers SDL3 compiles its backends against. SDL loads every backend's
# library at run time, so only the headers matter for those.
SEEDS = ["libc6-dev", "linux-libc-dev", "libfreetype-dev", "libfontconfig-dev", "libpng-dev", "zlib1g-dev",
         "libgl-dev", "libegl-dev", "libgles-dev", "libx11-dev", "libxext-dev", "libxrandr-dev",
         "libxcursor-dev", "libxi-dev", "libxtst-dev", "libxinerama-dev", "libxfixes-dev", "libxss-dev", "libxkbcommon-dev", "libwayland-dev",
         "libasound2-dev", "libpulse-dev", "libdbus-1-dev", "libudev-dev", "libdrm-dev", "libgbm-dev",
         "libusb-1.0-0-dev", "libbz2-dev", "uuid-dev", "libgcc-10-dev"]
# Packages a -dev package names that a build never reads.
SKIP = re.compile(r"^(perl.*|python.*|dpkg.*|debconf.*|install-info|libc-dev-bin|gcc-(?!10-base).*|gcc|cpp.*|"
                  r"binutils.*|libcrypt-dev|libnsl-dev|rpcsvc-proto|libtirpc-dev|pkg-config|pkgconf.*|"
                  r"libglib2\.0-dev.*|libsystemd-dev|x11-common|lsb-base|sensible-utils|init-system-helpers|"
                  r"adduser|passwd|login|systemd.*|udev|dbus|mount|util-linux|coreutils|tar|bash|libpam.*|"
                  r"ucf|libselinux1-dev|libsepol1-dev|libpcre.*-dev|libmount-dev|libblkid-dev|"
                  r"libffi-dev|libsndfile1-dev|libflac-dev|libogg-dev|libvorbis-dev|"
                  r"libopus-dev|libapparmor-dev|libasyncns.*|libxml2.*)$")


def fetch(url, path):
    if os.path.exists(path):
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with urllib.request.urlopen(url, timeout=120) as response, open(path + ".part", "wb") as handle:
        shutil.copyfileobj(response, handle)
    os.replace(path + ".part", path)
    return path


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def index():
    """The suite's i386 package list, checked against its Release file."""
    for mirror in MIRRORS:
        try:
            release = fetch(f"{mirror}/dists/{SUITE}/Release", os.path.join(DOWNLOADS, "Release"))
            break
        except OSError:
            continue
    else:
        sys.exit("build_linux_sysroot: no Debian mirror answered")
    with open(release) as handle:
        text = handle.read()
    wanted = re.search(r"^ ([0-9a-f]{64})\s+\d+ main/binary-i386/Packages\.xz$", text, re.M)
    if not wanted:
        sys.exit("build_linux_sysroot: the Release file lists no main/binary-i386/Packages.xz")
    packages = fetch(f"{mirror}/dists/{SUITE}/main/binary-i386/Packages.xz", os.path.join(DOWNLOADS, "Packages.xz"))
    if sha256(packages) != wanted.group(1):
        os.remove(packages)
        sys.exit("build_linux_sysroot: Packages.xz does not match the Release file; run again")
    found = {}
    with lzma.open(packages, "rt", encoding="utf-8") as handle:
        for stanza in handle.read().split("\n\n"):
            fields = dict(re.findall(r"^([A-Za-z0-9-]+): (.*)$", stanza, re.M))
            if "Package" in fields:
                found[fields["Package"]] = fields
            for provided in re.findall(r"[a-z0-9.+-]+", fields.get("Provides", "")):
                found.setdefault(provided, fields)
    return mirror, found


def resolve(found):
    """The seeds and everything they depend on, first alternative of each."""
    chosen, queue = {}, list(SEEDS)
    while queue:
        name = queue.pop()
        if name in chosen or SKIP.match(name):
            continue
        fields = found.get(name)
        if not fields:
            print(f"build_linux_sysroot: {name} is not in {SUITE}; skipped")
            continue
        chosen[fields["Package"]] = fields
        for group in (fields.get("Depends", "") + "," + fields.get("Pre-Depends", "")).split(","):
            first = group.split("|")[0].strip()
            if first:
                queue.append(re.split(r"[\s(:]", first)[0])
    return chosen


def extract(deb, destination):
    """A .deb's files: an ar archive holding data.tar.*."""
    work = deb + ".x"
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    subprocess.run(["ar", "x", os.path.abspath(deb)], cwd=work, check=True)
    data = next(name for name in os.listdir(work) if name.startswith("data.tar"))
    with tarfile.open(os.path.join(work, data)) as archive:
        # Debian's absolute symbolic links are wanted (lay_out points them
        # back inside the sysroot), which the default filter refuses; the
        # files themselves must still land inside it.
        members = [m for m in archive.getmembers() if not m.isdev()]
        for member in members:
            if os.path.isabs(member.name) or ".." in member.name.split("/"):
                sys.exit(f"build_linux_sysroot: {deb} has an entry outside itself: {member.name}")
        archive.extractall(destination, members=members, **({"filter": "fully_trusted"} if hasattr(tarfile, "data_filter") else {}))
    shutil.rmtree(work)


def lay_out():
    """What Debian keeps under i386-linux-gnu, where Arch's gcc -m32 looks,
    and every absolute symbolic link pointed back inside the sysroot."""
    for parent, _, files in os.walk(SYSROOT):
        for name in files:
            path = os.path.join(parent, name)
            if os.path.islink(path) and os.readlink(path).startswith("/"):
                target = os.path.join(SYSROOT, os.readlink(path).lstrip("/"))
                os.remove(path)
                os.symlink(os.path.relpath(target, parent), path)
    for link, target in (("usr/lib32", "lib/i386-linux-gnu"), ("lib32", "lib/i386-linux-gnu")):
        path = os.path.join(SYSROOT, link)
        if not os.path.lexists(path):
            os.symlink(target, path)
    # Libraries Debian keeps in /lib/i386-linux-gnu are linked into
    # /usr/lib/i386-linux-gnu, so one directory holds them all.
    usr_lib, lib = os.path.join(SYSROOT, "usr/lib/i386-linux-gnu"), os.path.join(SYSROOT, "lib/i386-linux-gnu")
    for name in os.listdir(lib) if os.path.isdir(lib) else []:
        if not os.path.lexists(os.path.join(usr_lib, name)):
            os.symlink(os.path.relpath(os.path.join(lib, name), usr_lib), os.path.join(usr_lib, name))
    include, multiarch = os.path.join(SYSROOT, "usr/include"), os.path.join(SYSROOT, "usr/include/i386-linux-gnu")
    for name in os.listdir(multiarch):
        if not os.path.lexists(os.path.join(include, name)):
            os.symlink(os.path.join("i386-linux-gnu", name), os.path.join(include, name))


def flags():
    """Compiler and linker flags for building against the sysroot. The
    linker flags name the start-up objects themselves (-nostartfiles): gcc
    would otherwise look for 32-bit ones beside the host compiler, which a
    host without multilib does not have. Put startfiles() first in a link
    and endfiles() last."""
    lib, gcc = os.path.join(SYSROOT, "usr/lib/i386-linux-gnu"), os.path.join(SYSROOT, GCC_LIB)
    return (["--sysroot=" + SYSROOT, "-isystem", os.path.join(SYSROOT, "usr/include/i386-linux-gnu")],
            ["--sysroot=" + SYSROOT, "-no-pie", "-nostartfiles", "-L" + gcc, "-L" + lib,
             "-L" + os.path.join(SYSROOT, "lib/i386-linux-gnu"), "-Xlinker", "-rpath-link", "-Xlinker", lib, "-static-libgcc"])


def startfiles():
    lib, gcc = os.path.join(SYSROOT, "usr/lib/i386-linux-gnu"), os.path.join(SYSROOT, GCC_LIB)
    return [os.path.join(lib, "crt1.o"), os.path.join(lib, "crti.o"), os.path.join(gcc, "crtbegin.o")]


def endfiles():
    lib, gcc = os.path.join(SYSROOT, "usr/lib/i386-linux-gnu"), os.path.join(SYSROOT, GCC_LIB)
    return [os.path.join(gcc, "crtend.o"), os.path.join(lib, "crtn.o")]


def fetch_sdl():
    """SDL3's source, the release the Windows build ships as SDL3.dll."""
    if os.path.exists(os.path.join(SDL_SOURCE, "CMakeLists.txt")):
        return
    url, digest = SDL_ARCHIVE
    archive = fetch(url, os.path.join(DOWNLOADS, os.path.basename(url)))
    if sha256(archive) != digest:
        os.remove(archive)
        sys.exit(f"build_linux_sysroot: {archive} does not match its pinned SHA-256; run again")
    with tarfile.open(archive) as bundle:
        bundle.extractall(os.path.dirname(SDL_SOURCE), **({"filter": "data"} if hasattr(tarfile, "data_filter") else {}))


def build_sdl(sysroot_stamp):
    """SDL3 against the sysroot. Rebuilt when the SDL release or the sysroot
    changes; the source is fetched first either way, since the game's
    sdl.c compiles against its headers."""
    fetch_sdl()
    stamp = os.path.join(SDL_BUILD, ".complete")
    wanted = f"{SDL_ARCHIVE[0]}\n{sysroot_stamp}"
    if os.path.exists(os.path.join(SDL_BUILD, "libSDL3.a")) and os.path.exists(stamp):
        with open(stamp) as handle:
            if handle.read() == wanted:
                return
    shutil.rmtree(SDL_BUILD, ignore_errors=True)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import fetch_tools
    fetch_tools.ensure("cmake")
    fetch_tools.ensure("ninja")
    compile_flags, link_flags = flags()
    pkgconfig = ":".join(os.path.join(SYSROOT, d) for d in ("usr/lib/i386-linux-gnu/pkgconfig", "usr/share/pkgconfig"))
    environment = dict(os.environ, PKG_CONFIG_LIBDIR=pkgconfig, PKG_CONFIG_SYSROOT_DIR=SYSROOT,
                       PKG_CONFIG_PATH="")
    # A toolchain file, because CMake reads it for its own test programs as
    # well: they link with the sysroot's start-up objects like everything else.
    os.makedirs(SDL_BUILD, exist_ok=True)
    toolchain = os.path.join(SDL_BUILD, "sysroot-toolchain.cmake")
    # First quote for the shell that executes CMake's command; then use a
    # CMake bracket argument so $, quotes and backslashes stay literal.
    quote = shlex.join
    bracket = lambda text: "[====[" + text + "]====]"
    with open(toolchain, "w", encoding="utf-8") as handle:
        handle.write(f"""set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_SYSROOT {bracket(SYSROOT)})
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
set(CMAKE_C_COMPILER gcc)
set(CMAKE_C_FLAGS_INIT {bracket("-m32 " + quote(compile_flags))})
set(CMAKE_ASM_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT {bracket("-m32 " + quote(link_flags))})
set(CMAKE_C_LINK_EXECUTABLE
    {bracket("<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> " + quote(startfiles()) + " <OBJECTS> -o <TARGET> <LINK_LIBRARIES> " + quote(endfiles()))})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
""")
    subprocess.run(["cmake", "-S", SDL_SOURCE, "-B", SDL_BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                    f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                    "-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TESTS=OFF", "-DSDL_TEST_LIBRARY=OFF",
                    "-DSDL_EXAMPLES=OFF", "-DSDL_INSTALL=OFF", "-DSDL_PIPEWIRE=OFF", "-DSDL_JACK=OFF",
                    "-DSDL_IBUS=OFF", "-DSDL_WAYLAND_LIBDECOR=OFF", "-DSDL_SNDIO=OFF"],
                   env=environment, check=True)
    subprocess.run(["cmake", "--build", SDL_BUILD], env=environment, check=True)
    with open(stamp, "w") as handle:
        handle.write(wanted)


def sysroot_stamp():
    """The .complete marker's first line: the packages asked for. A sysroot
    laid out for another list (an older checkout's, before libgcc-10-dev
    joined it) is laid out again; the .debs stay in DOWNLOADS."""
    return f"{SUITE} {' '.join(SEEDS)}\n"


def main():
    stamp = os.path.join(SYSROOT, ".complete")
    wanted = sysroot_stamp()
    current = None
    if os.path.exists(stamp):
        with open(stamp) as handle:
            current = handle.readline()
    if current != wanted:
        mirror, found = index()
        chosen = resolve(found)
        shutil.rmtree(SYSROOT, ignore_errors=True)
        os.makedirs(SYSROOT)
        for name, fields in sorted(chosen.items()):
            deb = fetch(f"{mirror}/{fields['Filename']}", os.path.join(DOWNLOADS, os.path.basename(fields["Filename"])))
            if sha256(deb) != fields["SHA256"]:
                os.remove(deb)
                sys.exit(f"build_linux_sysroot: {deb} does not match the package list; run again")
            extract(deb, SYSROOT)
        lay_out()
        with open(stamp, "w") as handle:
            handle.write(wanted)
            handle.writelines(f"{name} {fields['Version']}\n" for name, fields in sorted(chosen.items()))
        print(f"{SYSROOT}: {len(chosen)} packages from Debian {SUITE}")
    build_sdl(wanted)
    print(f"{SDL_BUILD}/libSDL3.a")


if __name__ == "__main__":
    main()
