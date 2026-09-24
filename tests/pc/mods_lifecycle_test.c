#define main original_mods_test
#include "pc/compat/fs.h"
#include "mods_test.c"
#undef main
#include "pc/mods/events.h"
static void unexpected_state(int owner, void *data, size_t size, unsigned version, void *context)
{
    (void)owner;
    (void)data;
    (void)size;
    (void)version;
    (void)context;
    assert(0);
}
/* A fixture object, copied into a mod directory. */
static void install(const char *from, const char *to)
{
    FILE *file;
    long length;
    void *object;
    file = fopen(from, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    object = malloc((size_t)length);
    assert(object);
    assert(fread(object, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    write_file(to, object, (size_t)length);
    free(object);
}
int main(int argc, char **argv)
{
    char path[1024];
    int no_api;
    assert(argc == 3);
    scratch_template(root, sizeof(root), "memories-lifecycle");
    assert(mkdtemp(root));
    make_dir("mods");
    make_dir("mods/reject");
    install(argv[1], "mods/reject/reject.o");
    write_text("mods/reject/mod.json", "{\"id\":\"reject\",\"library\":\"reject\",\"enabled\":true}");
    make_dir("mods/zz-no-api");
    install(argv[2], "mods/zz-no-api/no-api.o");
    write_text("mods/zz-no-api/mod.json", "{\"id\":\"no-api\",\"library\":\"no-api\",\"enabled\":true}");
    snprintf(path, sizeof(path), "%s/mods", root);
    setenv("MEMORIES_MODS_DIR", path, 1);
    snprintf(path, sizeof(path), "%s/settings.txt", root);
    setenv("MEMORIES_SETTINGS", path, 1);
    Settings_Load();
    Mods_Load();
    assert(find("reject") == 0);
    assert(Mods_Failed(0) && !Mods_Active(0));
    /* A mod that leaves mod->api at 0 is refused with that reason. */
    no_api = find("no-api");
    assert(no_api == 1 && Mods_Failed(no_api) && !Mods_Active(no_api));
    assert(strstr(Mods_Status(no_api), "did not set mod->api"));
    Mods_DrawFrame();
    Mods_Reset();
    Mods_Notify(MEMORIES_EVENT_DAMAGE, 0, 100, 0);
    Mods_VisitState(unexpected_state, NULL);
    Mods_Shutdown();
    assert(!Settings_GetNamed("mod.reject.unexpected_callback", 0));
    Mods_SetEnabled(0, 0);
    Mods_SetEnabled(0, 1);
    Mods_DrawFrame();
    assert(!Settings_GetNamed("mod.reject.unexpected_callback", 0));
    return 0;
}
