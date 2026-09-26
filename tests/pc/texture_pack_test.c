/* A texture pack's manifest entries and the settings that switch them
 * (src/pc/render/texture_pack.c): an entry whose setting is on is used, one
 * whose setting is off is left out, and one naming a setting the mod does
 * not declare is a problem and used. Also exercise field-thumbnail uploads
 * after a duplicate from a battle/effect's full-art record was delivered. */
#include "pc/compat/fs.h"
#include "pc/render/texture_pack.h"
#include "pc/render/texture_dump.h"
#include <assert.h>
#include <png.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"
#include "scratch.h"

static char root[SCRATCH_MAX];
static unsigned char disc[3 * 2048];
static int disc_ready;

int Memories_DiscReadSectors(int lba, int sectors, void *out)
{
    if (!disc_ready || lba < 0 || sectors < 0 || lba + sectors > 3) return 0;
    memcpy(out, disc + lba * 2048, (size_t)sectors * 2048);
    return sectors;
}

static int disc_file(const char *path, int *lba, unsigned *size)
{
    assert(!strcmp(path, "\\DATA\\WA_MRG.MRG;1"));
    *lba = 1;
    *size = 2048;
    return 0;
}

static void write_text(const char *relative, const char *text)
{
    char path[1024];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(!fclose(file));
}

static void make_dir(const char *relative)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    assert(!mkdir(path, 0777));
}

/* The mod's settings: "on" is on, "off" is off, nothing else is declared. */
static int part(const char *setting, void *context)
{
    assert(context == root);
    return !strcmp(setting, "on") ? 1 : !strcmp(setting, "off") ? 0 : -1;
}

#define ENTRY(file, offset, extra) \
    "{\"file\":\"" file "\",\"archive\":\"WA_MRG.MRG\",\"offset\":" #offset ",\"words\":1,\"rows\":1,\"bpp\":16" extra "}"

static void field_thumbnail(void)
{
    uint16_t original[704], duplicate[704], copied[704];
    const unsigned char green[] = {0, 255, 0, 255};
    png_image png = {0};
    char path[1024];
    unsigned char encoded[256];
    png_alloc_size_t size;
    FILE *file;
    uint32_t rgb;
    int i, entry, written;
    for (i = 0; i < 640; i++) original[i] = (uint16_t)(1 + i % 63) * 0x101;
    for (i = 0; i < 64; i++) original[640 + i] = (uint16_t)i;
    memcpy(disc + 2048, original, sizeof(original));
    memcpy(disc + 4096, original, sizeof(original));
    disc_ready = 1;
    TextureDump_SetDiscFiles(disc_file);
    make_dir("field");
    snprintf(path, sizeof(path), "%s/field/thumb.png", root);
    png.version = PNG_IMAGE_VERSION;
    png.width = png.height = 1;
    png.format = PNG_FORMAT_RGBA;
    /* Through fopen (Memories_Fopen, UTF-8) like the other files here, not
     * libpng's own fopen. */
    size = sizeof(encoded);
    written = png_image_write_to_memory(&png, encoded, &size, 0, green, 0, NULL);
    if (!written) fprintf(stderr, "png: %s\n", png.message);
    assert(written && size <= sizeof(encoded));
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(encoded, 1, size, file) == size);
    assert(!fclose(file));
    write_text("field/manifest.json", "[{\"file\":\"thumb.png\",\"archive\":\"WA_MRG.MRG\","
               "\"offset\":0,\"words\":20,\"rows\":32,\"bpp\":8,\"clut_offset\":1280,\"clut_entries\":64}]");
    snprintf(path, sizeof(path), "%s/field", root);
    assert(TexturePack_Load(path, 1, NULL, NULL, NULL, 0) == 1);
    TexturePack_Service();

    /* Initial hand/field upload from the thumbnail sector. */
    TextureDump_Delivered(original, sizeof(original), 1, 0);
    SoftGpu_Load(896, 0, 20, 32, original);
    SoftGpu_Load(896, 224, 64, 1, original + 640);
    TexturePack_Service();
    entry = TexturePack_EntryFor(896, 0, 1, 896, 224, 0, 0);
    assert(entry != 0);

    /* Battle/equip/fusion loads read the same bytes from a full-art record.
     * Field updates upload a copy from the deck table or a VRAM readback.
     * The newest delivery names the duplicate, absent from the manifest. */
    memcpy(duplicate, original, sizeof(original));
    TextureDump_Delivered(duplicate, sizeof(duplicate), 2, 0);
    memcpy(copied, duplicate, sizeof(copied));
    SoftGpu_Load(896, 0, 20, 32, copied);
    assert(TexturePack_EntryFor(896, 0, 1, 896, 224, 0, 0) == entry);
    SoftGpu_Load(896, 224, 64, 1, copied + 640);
    assert(TexturePack_EntryFor(896, 0, 1, 896, 224, 0, 0) == entry);
    assert(TextureDump_Sample(896, 0, 1, 10 << 16, 10 << 16, &rgb) == 1 && rgb == 0x00ff00);
    assert(*TextureDump_Cell(896, 0, 0) == (0x8000 | 0x03e0));
    assert(SoftGpu_Vram()[896] == original[0]); /* Native game pixels stay intact. */

    /* Card effects also upload that block as an 8x88 strip, read it back,
     * then reuse the bytes as a field thumbnail and its palette. */
    SoftGpu_Load(832, 256, 8, 88, duplicate);
    SoftGpu_Store(832, 256, 8, 88, copied);
    assert(!memcmp(copied, original, sizeof(copied)));
    SoftGpu_Load(896, 32, 20, 32, copied);
    SoftGpu_Load(896, 229, 64, 1, copied + 640);
    assert(TexturePack_EntryFor(896, 0, 1, 896, 229, 0, 32) == entry);

    /* A matching prefix is not enough, nor is a truncated block. */
    assert(TextureDump_Recall(copied, 640) == 2048);
    assert(TextureDump_Recall(copied, 16) == 0);
    assert(TextureDump_Recall(copied, 704) == 0);
    copied[639] ^= 1;
    assert(TextureDump_Recall(copied, 640) == 0);
    copied[703] ^= 1;
    assert(TextureDump_Recall(copied + 640, 64) == 0);
    TexturePack_Unload();
    assert(TexturePack_EntryFor(896, 0, 1, 896, 224, 0, 0) == 0);
}

int main(void)
{
    char path[1024], problems[256];
    scratch_template(root, sizeof(root), "memories-texture-pack");
    assert(mkdtemp(root));
    make_dir("pack");
    write_text("pack/a.png", "\x89PNG\r\n\x1a\n");
    write_text("pack/manifest.json",
               "[" ENTRY("a.png", 0, ",\"setting\":\"on\"") "," ENTRY("a.png", 2, ",\"setting\":\"off\"") ","
               ENTRY("a.png", 4, ",\"setting\":\"nope\"") "," ENTRY("a.png", 6, "") "]");
    snprintf(path, sizeof(path), "%s/pack", root);
    /* on, the undeclared one and the plain one; off is left out */
    assert(TexturePack_Load(path, 1, part, root, problems, sizeof(problems)) == 3);
    assert(!strcmp(problems, "1 image names a setting the mod does not declare (first: nope)"));
    TexturePack_Unload();
    /* Without the mod's settings every "setting" is undeclared: all used. */
    assert(TexturePack_Load(path, 1, NULL, NULL, problems, sizeof(problems)) == 4);
    assert(!strcmp(problems, "3 images name a setting the mod does not declare (first: on)"));
    TexturePack_Unload();
    /* Every part switched off is nothing wrong with the pack: 0, not -1. */
    make_dir("off");
    write_text("off/a.png", "\x89PNG\r\n\x1a\n");
    write_text("off/manifest.json", "[" ENTRY("a.png", 0, ",\"setting\":\"off\"") "]");
    snprintf(path, sizeof(path), "%s/off", root);
    assert(TexturePack_Load(path, 1, part, root, problems, sizeof(problems)) == 0 && !problems[0]);
    write_text("off/manifest.json", "[]");
    assert(TexturePack_Load(path, 1, part, root, problems, sizeof(problems)) == -1);
    TexturePack_Unload();
    field_thumbnail();
    puts("texture pack tests passed");
    return 0;
}
