/* Exercise manager transactions and dependency ordering against real manifests. */
#define main original_mods_test
#include "mods_test.c"
#undef main
#include "pc/mods/events.h"

static int calls[8], call_count;
static void low(MemoriesModEvent *e)
{
    calls[call_count++] = 1;
    e->b += 2;
}
static void high(MemoriesModEvent *e)
{
    calls[call_count++] = 2;
    e->b *= 3;
}
static void replace(MemoriesModEvent *e)
{
    calls[call_count++] = 3;
    e->handled = 1;
    e->result = 17;
}
int main(void)
{
    char path[1024], error[256];
    int enabled[MODS_MAX] = {0}, order[MODS_MAX], a, b, partial;
    unsigned char sector[2048] = {0};
    MemoriesModEvent event = {MEMORIES_EVENT_DAMAGE, MEMORIES_BEFORE, 0, 4, 0, 0, 0};
    scratch_template(root, sizeof(root), "memories-manager");
    assert(mkdtemp(root));
    make_dir("mods");
    make_dir("mods/a");
    write_text(
        "mods/a/mod.json",
        "{\"id\":\"a\",\"version\":\"1.2\",\"settings\":[{\"key\":\"speed\",\"default\":5,\"min\":1,\"max\":10}]}");
    make_dir("mods/b");
    write_text("mods/b/mod.json",
               "{\"id\":\"b\",\"requires\":[{\"id\":\"a\",\"min_version\":\"1.1\"}],\"after\":[\"a\"]}");
    make_dir("mods/partial");
    write_text("mods/partial/mod.json", "{\"id\":\"partial\",\"enabled\":true,\"data\":[{\"lba\":5000,\"patch\":[{"
                                        "\"at\":0,\"bytes\":\"AA\"},{\"at\":1,\"bytes\":\"ZZ\"}]}]}");
    make_dir("mods/cycle-a");
    make_dir("mods/invalid-data");
    write_text("mods/invalid-data/mod.json",
               "{\"id\":\"invalid-data\",\"enabled\":true,\"data\":[{\"lba\":5001,\"patch\":{}}]}");
    write_text("mods/cycle-a/mod.json", "{\"id\":\"cycle-a\",\"after\":[\"cycle-b\"]}");
    make_dir("mods/cycle-b");
    write_text("mods/cycle-b/mod.json", "{\"id\":\"cycle-b\",\"after\":[\"cycle-a\"]}");
    /* A data mod, so applied at the next launch, and a live mod needing it. */
    make_dir("mods/later");
    write_text("mods/later/mod.json", "{\"id\":\"later\",\"data\":[{\"lba\":6000,\"patch\":[{\"at\":0,\"bytes\":\"11\"}]}]}");
    make_dir("mods/needs-later");
    write_text("mods/needs-later/mod.json", "{\"id\":\"needs-later\",\"requires\":[\"later\"]}");
    /* Two folders of one directory with one id: the first, sorted, stays. */
    make_dir("mods/dup-b");
    write_text("mods/dup-b/mod.json", "{\"id\":\"dup\",\"name\":\"second\"}");
    make_dir("mods/dup-a");
    write_text("mods/dup-a/mod.json", "{\"id\":\"dup\",\"name\":\"first\"}");
    /* A live data mod whose replacement file is missing until later. */
    make_dir("mods/missing");
    write_text("mods/missing/mod.json",
               "{\"id\":\"missing\",\"restart\":false,\"data\":[{\"lba\":7000,\"sectors\":1,\"replace\":\"late.bin\"}]}");
    make_dir("mods/invalid-schema");
    write_text("mods/invalid-schema/mod.json",
               "{\"id\":\"invalid-schema\",\"settings\":[{\"key\":\"oops\",\"default\":99,\"max\":10}]}");
    snprintf(path, sizeof(path), "%s/mods", root);
    assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    snprintf(path, sizeof(path), "%s/settings.txt", root);
    assert(!setenv("MEMORIES_SETTINGS", path, 1));
    assert(!setenv("MEMORIES_USER_DIR", root, 1));
    Settings_Load();
    Mods_Load();
    a = find("a");
    b = find("b");
    partial = find("partial");
    assert(a >= 0 && b >= 0 && partial >= 0);
    assert(Mods_Failed(partial) && !Mods_Active(partial));
    assert(!Mods_DiscSector(5000, sector) && sector[0] == 0);
    assert(Mods_Failed(find("invalid-schema")));
    {
        /* A failed mod is tried again once the player removes and reapplies
         * it, and goes in place when what it lacked is there. */
        int missing = find("missing"), wanted[MODS_MAX] = {0};
        Mods_SetEnabled(missing, 1);
        assert(Mods_Failed(missing) && !Mods_Active(missing) && strstr(Mods_Status(missing), "late.bin"));
        wanted[missing] = 1;
        assert(!Mods_Validate(wanted, error, sizeof(error)));
        Mods_SetEnabled(missing, 0);
        assert(!Mods_Failed(missing) && Mods_Validate(wanted, error, sizeof(error)));
        write_text("mods/missing/late.bin", "late");
        Mods_SetEnabled(missing, 1);
        assert(!Mods_Failed(missing) && Mods_Active(missing));
        memset(sector, 0xEE, sizeof(sector));
        assert(Mods_DiscSector(7000, sector) && !memcmp(sector, "late", 4) && !sector[4]);
        Mods_SetEnabled(missing, 0);
        assert(!Mods_DiscSector(7000, sector));
        /* A replacement bigger than what it replaces is cut, and says so. */
        {
            static char big[2050];
            memset(big, 'x', sizeof(big) - 1);
            write_text("mods/missing/late.bin", big);
        }
        Mods_SetEnabled(missing, 1);
        assert(Mods_Active(missing) && !Mods_Failed(missing) && strstr(Mods_Status(missing), "larger than"));
        Mods_SetEnabled(missing, 0);
    }
    {
        int dup = find("dup"), copies = 0;
        for (int i = 0; i < Mods_Count(); i++) copies += !strcmp(Mods_Id(i), "dup");
        assert(dup >= 0 && copies == 1 && !strcmp(Mods_Name(dup), "first") && !Mods_Failed(dup));
        assert(strstr(Mods_Status(dup), "dup-b was left out: same id as") && strstr(Mods_Status(dup), "dup-a"));
        /* The warning outlasts applying the mod, which resets its status. */
        Mods_SetEnabled(dup, 1);
        assert(Mods_Active(dup) && strstr(Mods_Status(dup), "same id as"));
        Mods_SetEnabled(dup, 0);
    }
    assert(Mods_Failed(find("invalid-data")) && !Mods_Active(find("invalid-data")));
    enabled[find("cycle-a")] = enabled[find("cycle-b")] = enabled[a] = 1;
    assert(Mods_Order(enabled, order, error, sizeof(error)) < 0);
    assert(order[0] == a && order[1] == -1); /* the mods outside the cycle still load */
    enabled[find("cycle-a")] = enabled[find("cycle-b")] = enabled[a] = 0;
    enabled[b] = 1;
    assert(!Mods_Validate(enabled, error, sizeof(error)));
    assert(strstr(error, "requires a"));
    enabled[a] = 1;
    assert(Mods_Validate(enabled, error, sizeof(error)));
    Settings_SetNamed("mod.b.order", -100);
    assert(Mods_Order(enabled, order, error, sizeof(error)) == 2 && order[0] == a && order[1] == b);
    /* The keys a mod may write with host->set_setting, as a declared one. */
    assert(Mods_SettingKeyValid("speed") && Mods_SettingKeyValid("turn_left-2"));
    assert(!Mods_SettingKeyValid("order") && !Mods_SettingKeyValid("") && !Mods_SettingKeyValid(NULL));
    assert(!Mods_SettingKeyValid("a.b") && !Mods_SettingKeyValid("a b") && !Mods_SettingKeyValid("x=1"));
    assert(!Mods_OptionSet(a, 0, 99));
    assert(Mods_OptionSet(a, 0, 7));
    assert(Mods_OptionValue(a, 0) == 7);
    assert(Mods_Apply(enabled, error, sizeof(error)));
    assert(Mods_Active(a) && Mods_Active(b));
    assert(Mods_ProfileSave("Test profile"));
    enabled[a] = enabled[b] = 0;
    assert(Mods_ProfileRead("Test profile", enabled));
    assert(enabled[a] && enabled[b]);
    assert(Mods_ProfileValue("Test profile", "mod.a.speed", 0) == 7);
    assert(!Mods_ProfileSave("../escape"));
    int h1 = Mods_Subscribe(a, MEMORIES_EVENT_DAMAGE, 0, low), h2 = Mods_Subscribe(b, MEMORIES_EVENT_DAMAGE, 10, high);
    assert(h1 && h2);
    Mods_Dispatch(&event);
    assert(call_count == 2 && calls[0] == 2 && calls[1] == 1 && event.b == 14);
    call_count = 0;
    event.phase = MEMORIES_AFTER;
    Mods_Dispatch(&event);
    assert(event.b == 14); /* observers cannot change result */
    int h3 = Mods_Subscribe(b, MEMORIES_EVENT_DAMAGE, 20, replace);
    assert(h3);
    call_count = 0;
    event.phase = MEMORIES_BEFORE;
    Mods_Dispatch(&event);
    assert(call_count == 1 && event.result == 17 && event.handled);
    Mods_Unsubscribe(b, h3);
    Mods_SetEnabled(b, 0);
    call_count = 0;
    event.handled = 0;
    Mods_Dispatch(&event);
    assert(call_count == 1 && calls[0] == 1);
    Mods_ClearHooks(a);
    call_count = 0;
    Mods_Dispatch(&event);
    assert(call_count == 0);

    /* Reload settings records a restart-only mod without putting it in
     * place, and a live mod requiring it waits for the same restart. */
    {
        int later = find("later"), needs = find("needs-later"), all[MODS_MAX] = {0};
        assert(later >= 0 && needs >= 0 && Mods_RequiresRestart(later) && !Mods_RequiresRestart(needs));
        Settings_SetNamed("mod.later", 1);
        Mods_Load();
        assert(Mods_Enabled(later) && !Mods_Active(later));
        memset(sector, 0, sizeof(sector));
        assert(!Mods_DiscSector(6000, sector) && sector[0] == 0);
        all[later] = all[needs] = 1;
        assert(Mods_Validate(all, error, sizeof(error)));
        assert(Mods_WaitsForRestart(needs, all) == later);
        Mods_SetEnabled(needs, 1);
        assert(Mods_Enabled(needs) && !Mods_Active(needs) && strstr(Mods_Status(needs), "restart"));
        Settings_SetNamed("mod.needs-later", 1);
        Mods_Load();
        assert(!Mods_Active(needs) && strstr(Mods_Status(needs), "restart"));
        Mods_SetEnabled(needs, 0);
        assert(!Mods_Status(needs)[0]);
        Mods_SetEnabled(later, 0);
        assert(!Mods_Active(later));
        all[later] = 0;
        assert(Mods_WaitsForRestart(needs, all) < 0);
    }
    assert(!setenv("MEMORIES_SETTINGS", "/dev/null/settings", 1));
    enabled[b] = 1;
    assert(!Mods_Apply(enabled, error, sizeof(error)));
    assert(!Mods_Enabled(b));
    /* Once the mods are shut down no event reaches them: VBlank input could
     * otherwise call into a mod whose shutdown hook freed its memory. */
    assert(Mods_Active(a) && Mods_Subscribe(a, MEMORIES_EVENT_DAMAGE, 0, low));
    call_count = 0;
    event.handled = 0;
    event.phase = MEMORIES_BEFORE;
    Mods_Dispatch(&event);
    assert(call_count == 1);
    Mods_Shutdown();
    call_count = 0;
    Mods_Dispatch(&event);
    assert(call_count == 0);
    return 0;
}
