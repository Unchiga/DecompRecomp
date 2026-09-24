/* The Ogg Vorbis decoder audio replacement uses (replace.c): Sean Barrett's
 * stb_vorbis, public domain (or MIT), vendored unchanged in
 * src/pc/third_party/stb_vorbis.c (see src/pc/third_party/README.md). Only
 * whole-file decoding from memory is compiled; the port opens files itself.
 * Its warnings are the upstream file's and are not ours to fix. */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wtautological-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "pc/third_party/stb_vorbis.c"
