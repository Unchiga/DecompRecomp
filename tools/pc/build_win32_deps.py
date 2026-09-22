#!/usr/bin/env python3
"""Fetch and build the Windows game executable's libraries (i686, llvm-mingw).

The Linux build takes FreeType and libpng from the system and builds SDL3
itself (tools/pc/build_sdl32.sh). On Windows this script provides them under
tmp/pc/win32-deps: static zlib, libpng and FreeType built from pinned release
archives, and SDL3's official MinGW development release (SDL3.dll, shipped
beside the executable). Needs cmake, ninja and llvm-mingw's
i686-w64-mingw32-clang on PATH (notes/pc-build.md, "Windows")."""
import hashlib, os, shutil, subprocess, sys, tarfile, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tmp", "pc", "win32-deps")
ARCHIVES = {
    "zlib": ("https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz",
             "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"),
    "libpng": ("https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz",
               "a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf"),
    "freetype": ("https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz",
                 "dc49de6b01a266eef4876a4dd34d9842c475d3e28ff2eff63bd2fb760ab56261"),
    "sdl": ("https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-devel-3.4.16-mingw.tar.gz",
            "c7ef65bd72eabac6e5b535411dbd8d5824d0aab24fd62ff8812666b336f18a9c"),
}
CC = "i686-w64-mingw32-clang"


def fetch(name):
    url, digest = ARCHIVES[name]
    path = os.path.join(OUT, "downloads", os.path.basename(url))
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print(f"fetch {url}")
        with urllib.request.urlopen(url) as response, open(path + ".part", "wb") as handle:
            shutil.copyfileobj(response, handle)
        os.replace(path + ".part", path)
    with open(path, "rb") as handle:
        actual = hashlib.sha256(handle.read()).hexdigest()
    if actual != digest:
        sys.exit(f"{path}: SHA-256 {actual}, expected {digest}")
    source = os.path.join(OUT, "src", name)
    if not os.path.isdir(source):
        with tarfile.open(path) as archive:
            top = archive.getnames()[0].split("/")[0]
            # The data filter exists from Python 3.12 (and the 3.8-3.11 point
            # releases that backported it); older ones take the plain call.
            if hasattr(tarfile, "data_filter"):
                archive.extractall(os.path.join(OUT, "src"), filter="data")
            else:
                archive.extractall(os.path.join(OUT, "src"))
        os.replace(os.path.join(OUT, "src", top), source)
    return source


def cmake(name, source, *options):
    build = os.path.join(OUT, "build", name)
    subprocess.run(["cmake", "-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_SYSTEM_NAME=Windows", f"-DCMAKE_C_COMPILER={CC}", "-DCMAKE_RC_COMPILER=llvm-windres",
                    f"-DCMAKE_INSTALL_PREFIX={OUT}", "-DCMAKE_PREFIX_PATH=" + OUT,
                    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", *options], check=True)
    subprocess.run(["cmake", "--build", build, "--target", "install"], check=True)


def main():
    if shutil.which(CC) is None:
        sys.exit(f"{CC} is not on PATH (llvm-mingw)")
    cmake("zlib", fetch("zlib"), "-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_TESTING=OFF", "-DBUILD_SHARED_LIBS=OFF")
    # zlib's static library is libzs.a, which FindZLIB does not look for.
    cmake("libpng", fetch("libpng"), "-DPNG_SHARED=OFF", "-DPNG_STATIC=ON", "-DPNG_TESTS=OFF", "-DPNG_TOOLS=OFF",
          "-DPNG_FRAMEWORK=OFF", f"-DZLIB_LIBRARY={OUT}/lib/libzs.a", f"-DZLIB_INCLUDE_DIR={OUT}/include")
    cmake("freetype", fetch("freetype"), "-DBUILD_SHARED_LIBS=OFF", "-DFT_DISABLE_ZLIB=ON", "-DFT_DISABLE_BZIP2=ON",
          "-DFT_DISABLE_PNG=ON", "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_BROTLI=ON")
    sdl = fetch("sdl")
    shutil.copytree(os.path.join(sdl, "i686-w64-mingw32"), os.path.join(OUT, "sdl"), dirs_exist_ok=True)
    print(f"win32 deps: {OUT}")


if __name__ == "__main__":
    main()
