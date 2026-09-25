# Yu-Gi-Oh! Forbidden Memories Re-Decomp

> [!WARNING]
> Very much a work in progress.

A byte-matching decompilation of the North American PlayStation release of
**Yu-Gi-Oh! Forbidden Memories** (`SLUS-01411`), rebuilt as a native PC game
for Linux and Windows.

No game data is included. Bring your own dump of the disc.

## Play

1. Put your disc image (the `.bin` of the USA disc, any file name) in `game/`.
2. Run `./play.sh` on Linux or `play.bat` on Windows.

The first run builds the game, fetching what it needs into `tmp/`; after that
it starts straight away. Linux needs `gcc` and `python3` installed (`play.sh`
says so if they are missing); Windows needs nothing. See
[PC build](notes/pc-build.md).

## Match the PS1 executable

```sh
make tools          # pinned toolchain, installed under tools/
make match          # rebuild the PS1 executable byte-for-byte (see notes/setup.md)
```

## Progress

<!-- BEGIN GENERATED PROGRESS -->

| Metric | Current |
|---|---:|
| Game C-decompilation targets matched | **1,134 / 1,134 (100.00%)** |
| Game C-decompilation target bytes matched | **356,080 (`0x56EF0`) / 356,080 (`0x56EF0`) (100.00%)** |
| Remaining game C-decompilation targets | 0 functions, 0 (`0x0`) |
| Evidence-backed handwritten game assembly | 61 functions, 40,116 (`0x9CB4`) |
| Total game-owned functions | 1,195 |
| Preserved Psy-Q CRT/SDK assembly | 591 functions, 117,348 (`0x1CA64`) |
| Total discovered functions | 1,786 |
| Embedded/unassigned resident text | 1,780 (`0x6F4`) |

Runtime overlay modules:

| Module | Matching C functions | Matching C bytes |
|---|---:|---:|
| `free_duel` | 9 / 9 (100.00%) | 4,140 (`0x102C`) / 4,140 (`0x102C`) (100.00%) |
| `main_menu` | 31 / 31 (100.00%) | 17,724 (`0x453C`) / 17,724 (`0x453C`) (100.00%) |
| `overworld_after_coup` | 15 / 15 (100.00%) | 6,184 (`0x1828`) / 6,184 (`0x1828`) (100.00%) |
| `overworld_before_coup` | 15 / 15 (100.00%) | 6,184 (`0x1828`) / 6,184 (`0x1828`) (100.00%) |
| `password` | 27 / 27 (100.00%) | 10,884 (`0x2A84`) / 10,884 (`0x2A84`) (100.00%) |

_Generated from `config/slus_01411/functions.csv` and `config/slus_01411/overlays/*_functions.csv` by `tools/project/progress.py`._

<!-- END GENERATED PROGRESS -->

## More

- [Fusion helper](notes/fusion-helper.md)
- [Setup](notes/setup.md) · [Build](notes/build.md) · [PC build](notes/pc-build.md) · [Modding](notes/modding.md)
