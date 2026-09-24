#define main original_mods_test
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
int main(int argc, char **argv)
{
    char path[1024];
    FILE *file;
    long length;
    void *object;
    assert(argc == 2);
    file = fopen(argv[1], "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    object = malloc((size_t)length);
    assert(object);
    assert(fread(object, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    scratch_template(root, sizeof(root), "memories-lifecycle");
    assert(mkdtemp(root));
    make_dir("mods");
    make_dir("mods/reject");
    write_file("mods/reject/reject.o", object, (size_t)length);
    free(object);
    write_text("mods/reject/mod.json", "{\"id\":\"reject\",\"library\":\"reject\",\"enabled\":true}");
    snprintf(path, sizeof(path), "%s/mods", root);
    setenv("MEMORIES_MODS_DIR", path, 1);
    snprintf(path, sizeof(path), "%s/settings.txt", root);
    setenv("MEMORIES_SETTINGS", path, 1);
    Settings_Load();
    Mods_Load();
    assert(find("reject") == 0);
    assert(Mods_Failed(0) && !Mods_Active(0));
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
