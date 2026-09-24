/* Real ISO lookup, replacement composition and asynchronous drive delivery. */
#define MODS_REAL_DISC
#define main original_mods_test
#include "mods_test.c"
#undef main
#include "psyq/libds.h"
#include "pc/sdk/disc.h"
#include "pc/guest/state.h"
#include "pc/audio/spu.h"
#include "pc/render/texture_dump.h"
#include "pc/guest/image.h"
#include <stdint.h>

static char image_path[1024];
static int delivered_lba, callback_count, expected_lba, audio_blocks;
static unsigned char callback_data[5 * 2048];
const char *GameFiles_Disc(char *why, size_t size) { (void)why; (void)size; return image_path; }
void Crash_ReportFatal(const char *kind, const char *detail) { fprintf(stderr, "%s: %s\n", kind, detail); abort(); }
void Spu_CdFlush(void) {}
size_t Spu_CdWrite(const int16_t *frames, size_t count, unsigned rate) { (void)frames; (void)rate; audio_blocks++; return count; }
int AudioReplace_XaSectorMuted(int lba) { (void)lba; return 0; }
void Memories_GuestWritten(void *destination, size_t bytes) { (void)destination; (void)bytes; }
void TextureDump_SetDiscFiles(int (*info)(const char *, int *, unsigned *)) { (void)info; }
void TextureDump_Delivered(const void *destination, unsigned bytes, int lba, unsigned offset)
{ (void)destination; (void)bytes; (void)offset; delivered_lba = lba; }
void Log_Signal(int channel, const char *format, long a, long b, long c, long d, long e, long f)
{ (void)channel; (void)format; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{ (void)state; (void)tag; (void)fields; (void)count; return 0; }
extern int CdGetSector(void *, int);
extern int CdControlB(u8, u8 *, u8 *);
extern DslLOC *CdIntToPos_8007E600(int, DslLOC *);
extern int CdPosToInt_8007E710(const DslLOC *);

static void le(unsigned char *p, unsigned value)
{ for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (8 * i)); }
static int record(unsigned char *p, const char *name, unsigned lba, unsigned size)
{
    int length = (int)strlen(name), bytes = (33 + length + 1) & ~1;
    p[0] = (unsigned char)bytes; le(p + 2, lba); le(p + 10, size);
    p[32] = (unsigned char)length; memcpy(p + 33, name, (size_t)length);
    return bytes;
}
static void fixture(int sectors)
{
    unsigned char raw[2352];
    FILE *image;
    snprintf(image_path, sizeof(image_path), "%s/disc.bin", root);
    image = fopen(image_path, "wb"); assert(image);
    for (int lba = 0; lba < 40; lba++) {
        memset(raw, 0, sizeof(raw)); raw[15] = 2; raw[18] = raw[22] = 8;
        if (lba == 16) { le(raw + 24 + 156 + 2, 17); le(raw + 24 + 156 + 10, 2048); }
        if (lba == 17) record(raw + 24, "DATA", 18, 2048);
        if (lba == 18) {
            int at = record(raw + 24, "CARD.MRG;1", 20, 5000);
            at += record(raw + 24 + at, "NEXT.DAT;1", 23, 2048);
            record(raw + 24 + at, "SECOND.MRG;1", 24, 2048);
        }
        if (lba >= 20) memset(raw + 24, lba, 2048);
        if (lba == 30) { memset(raw + 24, 0, 2048); raw[18] = raw[22] = 0x64; }
        assert(fwrite(raw, 1, sizeof(raw), image) == sizeof(raw));
    }
    assert(!fseek(image, (long)sectors * 2352 - 1, SEEK_SET));
    assert(fputc(0, image) != EOF); assert(!fclose(image));
}
static void ready(u8 result, u8 *header, u32 *words)
{
    DslLOC loc;
    (void)words;
    assert(result == 1 && header[3] == 2 && header[6] == 8);
    memcpy(&loc, header, sizeof(loc));
    assert(CdPosToInt_8007E710(&loc) == expected_lba + callback_count);
    assert(callback_count < 5);
    assert(CdGetSector(callback_data + callback_count * 2048, 512));
    callback_count++;
    if (callback_count == 5) CdControlB(DslPause, NULL, NULL);
}
static void manifest(const char *id, const char *text)
{
    char path[256];
    snprintf(path, sizeof(path), "mods/%s", id); make_dir(path);
    snprintf(path, sizeof(path), "mods/%s/mod.json", id); write_text(path, text);
}
static int inspect_startup(const char *directory, unsigned rank, char *problems, size_t size)
{
    (void)directory; (void)rank; (void)problems; (void)size;
    /* Even an early mod's initialization sees later replacements. */
    assert(Memories_DiscFileStart("\\DATA\\CARD.MRG;1") == 40);
    return 1;
}
static void unload_pack(void) {}
int main(int argc, char **argv)
{
    unsigned char replacement[9000], lower[12000], got[5 * 2048], alias[2048];
    char path[1024];
    int lba, capacity = argc > 1 && !strcmp(argv[1], "capacity");
    unsigned bytes, signature;
    int failed_code = argc > 1 && !strcmp(argv[1], "failed-code");
    DslFILE file;
    DslLOC loc;
    scratch_template(root, sizeof(root), "memories-disc"); assert(mkdtemp(root)); make_dir("mods");
    fixture(capacity ? MEMORIES_DISC_MAX_LBA - 2 : 40);
    for (size_t i = 0; i < sizeof(replacement); i++) replacement[i] = (unsigned char)(i * 13 + 7);
    memset(lower, 0xCC, sizeof(lower));
    manifest("a-patch", "{\"enabled\":true,\"restart\":false,\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"patch\":[{\"at\":2047,\"bytes\":\"AABBCC\"},{\"at\":8999,\"bytes\":\"DD\"}]}]}");
    if (!capacity) {
        manifest("a-check", "{\"enabled\":true,\"textures\":\"images\"}");
        Mods_SetTexturePack(inspect_startup, unload_pack);
    }
    if (!failed_code) manifest("b-low", "{\"enabled\":true,\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"replace\":\"data.bin\"}]}");
    if (!failed_code) write_file("mods/b-low/data.bin", lower, sizeof(lower));
    manifest("c-high", "{\"enabled\":true,\"restart\":false,\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"replace\":\"data.bin\"}]}");
    if (failed_code) write_text("mods/c-high/mod.json",
        "{\"enabled\":true,\"library\":\"missing\",\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"replace\":\"data.bin\"}]}");
    write_file("mods/c-high/data.bin", replacement, sizeof(replacement));
    manifest("d-bad", "{\"enabled\":true,\"data\":[{\"file\":\"\\\\DATA\\\\CARD.MRG;1\",\"replace\":\"data.bin\",\"patch\":[{\"at\":0,\"bytes\":\"GG\"}]}]}");
    write_file("mods/d-bad/data.bin", lower, sizeof(lower));
    manifest("e-raw", "{\"enabled\":true,\"restart\":false,\"data\":[{\"lba\":20,\"patch\":[{\"at\":1,\"bytes\":\"FE\"},{\"at\":6143,\"bytes\":\"ABCD\"}]}]}");
    manifest("f-second", "{\"enabled\":true,\"data\":[{\"file\":\"\\\\DATA\\\\SECOND.MRG;1\",\"replace\":\"data.bin\"}]}");
    write_file("mods/f-second/data.bin", replacement, sizeof(replacement));
    manifest("g-live", "{\"enabled\":true,\"restart\":false,\"data\":[{\"lba\":32,\"sectors\":1,\"replace\":\"data.bin\"}]}");
    write_text("mods/g-live/data.bin", "first");
    manifest("h-overflow", "{\"enabled\":true,\"data\":[{\"lba\":\"0x100000014\",\"patch\":[{\"at\":0,\"bytes\":\"FF\"}]}]}");
    snprintf(path, sizeof(path), "%s/mods", root); assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    assert(!setenv("MEMORIES_USER_DIR", root, 1));
    snprintf(path, sizeof(path), "%s/settings", root); assert(!setenv("MEMORIES_SETTINGS", path, 1));
    Settings_Load(); Mods_Load();
    assert(!Memories_DiscOriginalFileInfo("\\DATA\\CARD.MRG;1", &lba, &bytes) && lba == 20 && bytes == 5000);
    if (capacity) {
        assert(!Mods_Active(find("c-high")) && strstr(Mods_Status(find("c-high")), "address space"));
        assert(!Mods_Active(find("a-patch")));
        assert(Memories_DiscFileStart("\\DATA\\CARD.MRG;1") == 20);
        Mods_Shutdown(); return 0;
    }
    assert(!Mods_Active(find("h-overflow")));
    if (failed_code) {
        assert(!Mods_Active(find("c-high")) && !Mods_Active(find("a-patch")));
        assert(strstr(Mods_Status(find("a-patch")), "final replacement"));
        assert(!Memories_DiscFileInfo("\\DATA\\CARD.MRG;1", &lba, &bytes) && lba == 40 && bytes == 5000);
        assert(Memories_DiscReadSectors(lba, 1, alias) == 1 && alias[0] == 20 && alias[1] == 0xFE);
        assert(Memories_DiscReadSectors(lba + 3, 1, alias) == 0);
        Mods_Shutdown(); return 0;
    }
    assert(Mods_Active(find("c-high")) && Mods_RequiresRestart(find("c-high")));
    assert(Mods_Active(find("a-patch")) && !Mods_Active(find("d-bad")));
    assert(!Memories_DiscFileInfo("\\DATA\\CARD.MRG;1", &lba, &bytes) && lba == 40 && bytes == sizeof(replacement));
    assert(DsSearchFile(&file, "\\DATA\\CARD.MRG") && file.size == bytes && CdPosToInt_8007E710(&file.pos) == lba);
    assert(Memories_DiscFileStart("\\DATA\\SECOND.MRG;1") == 47); /* lower candidate reserves six + guard */
    assert(Memories_DiscReadSectors(lba, 5, got) == 5 && delivered_lba == lba + 4);
    replacement[1] = 0xFE; replacement[2047] = 0xAA; replacement[2048] = 0xBB;
    replacement[2049] = 0xCC; replacement[6143] = 0xAB; replacement[8999] = 0xDD;
    assert(!memcmp(got, replacement, sizeof(replacement)));
    for (size_t i = sizeof(replacement); i < sizeof(got); i++) assert(got[i] == 0);
    for (int i = 0; i < 3; i++) {
        assert(Memories_DiscReadSectors(20 + i, 1, alias) == 1 && delivered_lba == lba + i);
        assert(!memcmp(alias, got + i * 2048, 2048));
    }
    assert(Memories_DiscReadSectors(23, 1, alias) == 1 && alias[0] == 0xCD && alias[1] == 23);
    assert(got[6144] != 0xCD); /* raw crossing never becomes an expanded-tail patch */
    assert(Memories_DiscReadSectors(lba + 5, 1, alias) == 0); /* unused reservation */
    assert(Memories_DiscReadSectors(lba + 6, 1, alias) == 0); /* guard */
    assert(Memories_DiscReadSectors(-1, 1, alias) == 0);
    assert(Memories_DiscReadSectors(INT32_MAX, 2, alias) == 0);
    expected_lba = lba;
    CdIntToPos_8007E600(lba, &loc);
    DsStartReadySystem(ready, 0); DsPacket(0, &loc, DslReadN, NULL, 0);
    for (int i = 0; i < 3; i++) Memories_DiscService((uint64_t)i * 1000);
    assert(callback_count == 5 && !memcmp(callback_data, got, sizeof(got)));
    signature = Mods_Signature();
    Mods_SetEnabled(find("a-patch"), 0);
    assert(Mods_Signature() != signature);
    assert(Memories_DiscReadSectors(lba + 4, 1, alias) == 1 && alias[807] != 0xDD);
    Mods_SetEnabled(find("a-patch"), 1);
    /* Reactivation changes the manager's activation order/signature. */
    assert(Memories_DiscReadSectors(lba, 5, callback_data) == 5 && !memcmp(callback_data, got, sizeof(got)));
    Mods_SetEnabled(find("c-high"), 0);
    assert(Mods_Active(find("c-high")) && Memories_DiscFileStart("\\DATA\\CARD.MRG;1") == lba);
    assert(Memories_DiscReadSectors(lba, 5, callback_data) == 5 && !memcmp(callback_data, got, sizeof(got)));
    signature = Mods_DiscSignature();
    Mods_SetEnabled(find("g-live"), 0);
    write_text("mods/g-live/data.bin", "other");
    Mods_SetEnabled(find("g-live"), 1);
    assert(Mods_DiscSignature() != signature); /* same name/length, changed backing bytes */
    assert(Memories_DiscReadSectors(32, 1, alias) == 1 && !memcmp(alias, "other", 5));
    DsEndReadySystem();
    CdIntToPos_8007E600(30, &loc);
    DsPacket(DslModeRT, &loc, DslReadS, NULL, 0);
    Memories_DiscService(1000000);
    assert(audio_blocks == 1); /* physical XA subheaders still reach the decoder */
    CdControlB(DslPause, NULL, NULL);
    CdIntToPos_8007E600(MEMORIES_DISC_MAX_LBA, &loc);
    assert(CdPosToInt_8007E710(&loc) == MEMORIES_DISC_MAX_LBA);
    Mods_Shutdown();
    return 0;
}
