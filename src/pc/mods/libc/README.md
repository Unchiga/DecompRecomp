# The C library a mod is given

A mod is compiled against these headers instead of its system's, because it
runs against the C library the game gives it (`src/pc/mods/modlibc.c`), not
against Linux's or Windows's. Everything declared here exists in the game on
both platforms and means the same thing on both; nothing else can be linked.
See `notes/modding.md`, "Native mods".

Files are opened by the host (`open_asset`, `open_data` in `modapi.h`); there
is no `fopen`, `remove` or `rename`, and no network, process or library call.
