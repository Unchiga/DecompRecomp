/* A texture pack's manifest entries and the settings that switch them
 * (src/pc/render/texture_pack.c): an entry whose setting is on is used, one
 * whose setting is off is left out, and one naming a setting the mod does
 * not declare is a problem and used. The images are only checked for a PNG
 * signature at load, so that is all the files hold. */
#include "pc/compat/fs.h"
#include "pc/render/texture_pack.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"
#include "scratch.h"

static char root[SCRATCH_MAX];

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
    puts("texture pack tests passed");
    return 0;
}
