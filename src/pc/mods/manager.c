/* Declarative configuration, dependency ordering and portable profiles. */
#define _POSIX_C_SOURCE 200809L
#include "../../types.h"
#include "json.h"
#include "mods.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include "pc/compat/posix.h" /* rename() that replaces, on Windows too */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const JsonValue *member(int mod, const char *key) { return Json_Member(Mods_Manifest(mod), key); }
static int find(const char *id)
{
    int i;
    for (i = 0; i < Mods_Count(); i++)
        if (!strcmp(Mods_Id(i), id))
            return i;
    return -1;
}
static const char *require_id(const JsonValue *value)
{
    return Json_String(value, Json_String(Json_Member(value, "id"), ""));
}
static int version_compare(const char *a, const char *b)
{
    /* Numeric dotted versions; bounds are inclusive. */
    int n;
    for (n = 0; n < 4; n++) {
        char *ae, *be;
        long av = strtol(a, &ae, 10), bv = strtol(b, &be, 10);
        if (av != bv)
            return av < bv ? -1 : 1;
        a = *ae == '.' ? ae + 1 : "";
        b = *be == '.' ? be + 1 : "";
    }
    return 0;
}
int Mods_Compatible(int mod, const int *enabled, char *error, size_t size)
{
    const JsonValue *list = member(mod, "requires");
    int i;
    long api = Json_Number(member(mod, "min_api"), 1);
    const char *game = Mods_Metadata(mod, "game");
    if (api > MEMORIES_MOD_API || (*game && strcmp(game, "slus_01411"))) {
        snprintf(error, size, "%s needs another game or mod API version", Mods_Name(mod));
        return 0;
    }
    for (i = 0; i < Json_Count(list); i++) {
        const JsonValue *req = Json_At(list, i);
        const char *id = require_id(req), *min = Json_String(Json_Member(req, "min_version"), ""),
                   *max = Json_String(Json_Member(req, "max_version"), "");
        int dep = find(id);
        if (dep < 0 || !enabled[dep] || (*min && version_compare(Mods_Metadata(dep, "version"), min) < 0) ||
            (*max && version_compare(Mods_Metadata(dep, "version"), max) > 0)) {
            snprintf(error, size, "%s requires %s%s%s%s%s", Mods_Name(mod), id, *min ? " >= " : "", min,
                     *max ? " <= " : "", max);
            return 0;
        }
    }
    list = member(mod, "conflicts");
    for (i = 0; i < Json_Count(list); i++) {
        int other = find(Json_String(Json_At(list, i), ""));
        if (other >= 0 && enabled[other]) {
            snprintf(error, size, "%s conflicts with %s", Mods_Name(mod), Mods_Name(other));
            return 0;
        }
    }
    return 1;
}
static int waits_for_restart(int mod, const int *enabled, int depth)
{
    const JsonValue *list = member(mod, "requires"), *req;
    if (depth > MODS_MAX)
        return -1; /* a cycle, which Mods_Order reports */
    for (req = Json_At(list, 0); req; req = Json_Next(req)) {
        int dep = find(require_id(req));
        if (dep >= 0 && enabled[dep] && !Mods_Active(dep) &&
            (Mods_RequiresRestart(dep) || waits_for_restart(dep, enabled, depth + 1) >= 0))
            return dep;
    }
    return -1;
}
int Mods_WaitsForRestart(int mod, const int *enabled) { return waits_for_restart(mod, enabled, 0); }
int Mods_Order(const int *enabled, int *order, char *error, size_t size)
{
    int done[MODS_MAX] = {0}, total = 0, wanted = 0, i;
    for (i = 0; i < Mods_Count(); i++)
        wanted += !!enabled[i];
    while (total < wanted) {
        int best = -1, priority = INT_MAX;
        for (i = 0; i < Mods_Count(); i++)
            if (enabled[i] && !done[i]) {
                const char *keys[] = {"requires", "after"};
                int k, blocked = 0, rank;
                char key[160];
                for (k = 0; k < 2; k++) {
                    const JsonValue *list = member(i, keys[k]);
                    int j;
                    for (j = 0; j < Json_Count(list); j++) {
                        int dep = find(require_id(Json_At(list, j)));
                        if (dep >= 0 && enabled[dep] && !done[dep])
                            blocked = 1;
                    }
                }
                snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
                rank = Settings_GetNamed(key, (int)Json_Number(member(i, "priority"), 0));
                if (!blocked && (best < 0 || rank < priority)) {
                    best = i;
                    priority = rank;
                }
            }
        if (best < 0) {
            /* order[] keeps the mods that could be placed, ended by -1. */
            order[total] = -1;
            snprintf(error, size, "Dependency/load-order cycle: check requires and after");
            return -1;
        }
        order[total++] = best;
        done[best] = 1;
    }
    return total;
}
int Mods_Validate(const int *enabled, char *error, size_t size)
{
    int i, order[MODS_MAX];
    for (i = 0; i < Mods_Count(); i++)
        if (enabled[i]) {
            if (Mods_Failed(i)) {
                snprintf(error, size, "%s: %s", Mods_Name(i), Mods_Status(i));
                return 0;
            }
            if (!Mods_Compatible(i, enabled, error, size))
                return 0;
        }
    return Mods_Order(enabled, order, error, size) >= 0;
}
int Mods_Apply(const int *enabled, char *error, size_t size)
{
    int old[MODS_MAX], order[MODS_MAX], i, n;
    if (!Mods_Validate(enabled, error, size))
        return 0;
    for (i = 0; i < Mods_Count(); i++) {
        char key[160];
        old[i] = Mods_Enabled(i);
        snprintf(key, sizeof(key), "mod.%s", Mods_Id(i));
        Settings_SetNamed(key, enabled[i]);
    }
    if (!Settings_Save()) {
        for (i = 0; i < Mods_Count(); i++) {
            char key[160];
            snprintf(key, sizeof(key), "mod.%s", Mods_Id(i));
            Settings_SetNamed(key, old[i]);
        }
        snprintf(error, size, "Could not save settings; changes cancelled");
        return 0;
    }
    for (i = Mods_Count() - 1; i >= 0; i--)
        if (!enabled[i])
            Mods_SetEnabled(i, 0);
    n = Mods_Order(enabled, order, error, size);
    for (i = 0; i < n; i++) {
        int mod = order[i];
        Mods_SetEnabled(mod, 1);
        if (Mods_Failed(mod))
            break;
    }
    for (i = 0; i < Mods_Count(); i++)
        if (enabled[i] && Mods_Failed(i)) {
            snprintf(error, size, "%s: %s", Mods_Name(i), Mods_Status(i));
            /* Back to the old set the way a launch builds it: everything off
             * in reverse, then the old mods in dependency order. */
            char ignored[128];
            for (int j = Mods_Count() - 1; j >= 0; j--)
                Mods_SetEnabled(j, 0);
            n = Mods_Order(old, order, ignored, sizeof(ignored));
            for (int j = 0; n >= 0 ? j < n : order[j] >= 0; j++)
                Mods_SetEnabled(order[j], 1);
            if (!Settings_Save())
                snprintf(error, size,
                         "Mod failed and preferences could not be restored; check settings before restarting");
            return 0;
        }
    return 1;
}
int Mods_OptionCount(int mod) { return Json_Count(member(mod, "settings")); }
const JsonValue *Mods_Option(int mod, int option) { return Json_At(member(mod, "settings"), option); }
int Mods_OptionValue(int mod, int option)
{
    const JsonValue *spec = Mods_Option(mod, option);
    return Mods_Setting(Mods_Id(mod), Json_String(Json_Member(spec, "key"), ""),
                        (int)Json_Number(Json_Member(spec, "default"), 0));
}
int Mods_SettingKeyValid(const char *key)
{
    /* "order" is the manager's own (mod.<id>.order, Mods_Order). */
    return key && *key && strcmp(key, "order") &&
           strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(key);
}
int Mods_OptionValid(int mod, int option, int value)
{
    const JsonValue *spec = Mods_Option(mod, option);
    const char *key = Json_String(Json_Member(spec, "key"), ""), *type = Json_String(Json_Member(spec, "type"), "int");
    int low = (int)Json_Number(Json_Member(spec, "min"), 0), high = (int)Json_Number(Json_Member(spec, "max"), 100);
    char setting[256];
    if (!strcmp(type, "bool")) {
        low = 0;
        high = 1;
    }
    if (!strcmp(type, "choice")) {
        low = 0;
        high = Json_Count(Json_Member(spec, "choices")) - 1;
    }
    if (!strcmp(type, "key")) {
        low = 0;
        high = 0xffff;
    }
    if (!Mods_SettingKeyValid(key) || value < low || value > high || snprintf(setting, sizeof(setting), "mod.%s.%s", Mods_Id(mod), key) >= (int)sizeof(setting))
        return 0;
    return 1;
}
int Mods_OptionSet(int mod, int option, int value)
{
    char key[256];
    if (!Mods_OptionValid(mod, option, value))
        return 0;
    snprintf(key, sizeof(key), "mod.%s.%s", Mods_Id(mod),
             Json_String(Json_Member(Mods_Option(mod, option), "key"), ""));
    Settings_SetNamed(key, value);
    return 1;
}

static int profile_path(char *path, size_t size, const char *name)
{
    char relative[160];
    if (!name || !*name || strlen(name) > 64 ||
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456"
                     "789_- ") != strlen(name))
        return 0;
    snprintf(relative, sizeof(relative), "mod-profiles/%s.txt", name);
    return Paths_User(path, size, relative) == 0;
}
static void profile_write(const char *key, int value, void *context)
{
    if (!strncmp(key, "mod.", 4))
        fprintf(context, "%s=%d\n", key, value);
}
int Mods_ProfileSave(const char *name)
{
    char path[1024], temp[1040];
    FILE *file;
    int i, failed;
    if (!profile_path(path, sizeof(path), name))
        return 0;
    snprintf(temp, sizeof(temp), "%s", path);
    *strrchr(temp, '/') = 0;
    Paths_MakeDirs(temp);
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    file = fopen(temp, "w");
    if (!file)
        return 0;
    Settings_VisitNamed(profile_write, file);
    /* Include defaults, so a profile is complete even before any edit. */
    for (i = 0; i < Mods_Count(); i++) {
        int j;
        fprintf(file, "mod.%s=%d\n", Mods_Id(i), Mods_Enabled(i));
        for (j = 0; j < Mods_OptionCount(i); j++)
            fprintf(file, "mod.%s.%s=%d\n", Mods_Id(i), Json_String(Json_Member(Mods_Option(i, j), "key"), ""),
                    Mods_OptionValue(i, j));
    }
    failed = ferror(file);
    if (fclose(file))
        failed = 1;
    if (failed || rename(temp, path)) {
        remove(temp);
        return 0;
    }
    return 1;
}
int Mods_ProfileValue(const char *name, const char *key, int fallback)
{
    char path[1024], line[512], found[256];
    int value;
    FILE *file;
    if (!profile_path(path, sizeof(path), name) || !(file = fopen(path, "r")))
        return fallback;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "%255[^=]=%d", found, &value) == 2 && !strcmp(key, found))
            fallback = value;
    fclose(file);
    return fallback;
}
int Mods_ProfileRead(const char *name, int *enabled)
{
    char path[1024];
    FILE *file;
    int i;
    if (!profile_path(path, sizeof(path), name) || !(file = fopen(path, "r")))
        return 0;
    {
        char line[512], key[256];
        int on;
        while (fgets(line, sizeof(line), file)) {
            if (sscanf(line, "mod.%255[^=]=%d", key, &on) == 2 && !strchr(key, '.') && on && find(key) < 0) {
                fclose(file);
                return 0;
            }
        }
    }
    fclose(file);
    for (i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s", Mods_Id(i));
        enabled[i] = !!Mods_ProfileValue(name, key, 0);
    }
    return 1;
}
static unsigned hash_text(unsigned hash, const char *s)
{
    while (*s) {
        hash ^= (unsigned char)*s++;
        hash *= 16777619u;
    }
    return (hash ^ 255u) * 16777619u;
}
static unsigned hash_json(unsigned hash, const JsonValue *value)
{
    const JsonValue *child;
    char number[40];
    hash = (hash ^ (unsigned)Json_TypeOf(value)) * 16777619u;
    hash = hash_text(hash, Json_Name(value) ? Json_Name(value) : "");
    hash = hash_text(hash, Json_String(value, ""));
    snprintf(number, sizeof(number), "%ld", Json_Number(value, Json_Bool(value, 0)));
    hash = hash_text(hash, number);
    for (child = Json_At(value, 0); child; child = Json_Next(child))
        hash = hash_json(hash, child);
    return hash;
}
static void hash_setting(const char *key, int value, void *context)
{
    unsigned *hash = context;
    char number[40];
    if (strncmp(key, "mod.", 4))
        return;
    for (int i = 0; i < Mods_Count(); i++)
        if (Mods_Active(i)) {
            char prefix[80];
            snprintf(prefix, sizeof(prefix), "mod.%s.", Mods_Id(i));
            if (!strncmp(key, prefix, strlen(prefix))) {
                const char *suffix = key + strlen(prefix);
                if (!strcmp(suffix, "order"))
                    return;
                for (int j = 0; j < Mods_OptionCount(i); j++)
                    if (!strcmp(suffix, Json_String(Json_Member(Mods_Option(i, j), "key"), "")))
                        return;
                snprintf(number, sizeof(number), "%d", value);
                *hash ^= hash_text(hash_text(2166136261u, key), number);
                break;
            }
        }
}
unsigned Mods_Signature(void)
{
    unsigned hash = 2166136261u, previous = 0;
    for (;;) {
        int i = -1;
        for (int candidate = 0; candidate < Mods_Count(); candidate++)
            if (Mods_Active(candidate) && Mods_Sequence(candidate) > previous &&
                (i < 0 || Mods_Sequence(candidate) < Mods_Sequence(i)))
                i = candidate;
        if (i < 0)
            break;
        previous = Mods_Sequence(i);
        char number[40];
        hash = hash_text(hash, Mods_Id(i));
        hash = hash_json(hash, Mods_Manifest(i));
        hash = (hash ^ Mods_CodeHash(i)) * 16777619u;
        for (int j = 0; j < Mods_OptionCount(i); j++) {
            snprintf(number, sizeof(number), "%d", Mods_RuntimeOption(i, j));
            hash = hash_text(hash, number);
        }
    }
    {
        unsigned settings_hash = 0;
        Settings_VisitNamed(hash_setting, &settings_hash);
        hash ^= settings_hash;
    }
    return hash ^ Mods_CardSignature() ^ Mods_DiscSignature();
}

/* An "audio" id as replace.c reads it: 0x-prefixed hexadecimal, else decimal. */
static long audio_id(const char *text)
{
    char *end;
    long value;
    if (!text || !*text) return -1;
    value = text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? strtol(text + 2, &end, 16) : strtol(text, &end, 10);
    return *end ? -1 : value;
}

/* Whether two mods replace one sound: the one applied later is heard. */
static int audio_overlap(int a, int b)
{
    static const char *const kinds[] = {"music", "xa", "sfx"};
    for (int k = 0; k < 3; k++) {
        const JsonValue *x = Json_Member(member(a, "audio"), kinds[k]), *y = Json_Member(member(b, "audio"), kinds[k]);
        for (int i = 0; i < Json_Count(x); i++)
            for (int j = 0; j < Json_Count(y); j++) {
                long id = audio_id(Json_Name(Json_At(x, i)));
                if (id >= 0 && id == audio_id(Json_Name(Json_At(y, j)))) return 1;
            }
    }
    return 0;
}

int Mods_ConflictText(int mod, char *out, size_t size)
{
    const JsonValue *data = member(mod, "data");
    int i, j, k;
    for (i = 0; i < Mods_Count(); i++)
        if (i != mod && Mods_Enabled(i)) {
            const JsonValue *other = member(i, "data");
            for (j = 0; j < Json_Count(data); j++)
                for (k = 0; k < Json_Count(other); k++) {
                    const JsonValue *a = Json_At(data, j), *b = Json_At(other, k);
                    const char *af = Json_String(Json_Member(a, "file"), ""),
                               *bf = Json_String(Json_Member(b, "file"), "");
                    long al = Json_Number(Json_Member(a, "lba"), -1), bl = Json_Number(Json_Member(b, "lba"), -1);
                    if ((*af && !strcmp(af, bf)) || (al >= 0 && al == bl)) {
                        snprintf(out, size,
                                 "Potential data overlap with %s. Replacements run first, "
                                 "then patches; later entries win within each kind.",
                                 Mods_Name(i));
                        return 1;
                    }
                }
            if (audio_overlap(mod, i)) {
                snprintf(out, size, "Replaces some of the same sounds as %s; the mod applied later is heard.",
                         Mods_Name(i));
                return 1;
            }
            if (*Mods_Metadata(mod, "textures") && *Mods_Metadata(i, "textures")) {
                snprintf(out, size,
                         "Multiple texture packs enabled. Where two replace the same "
                         "image, the one later in load order is drawn.");
                return 1;
            }
        }
    return 0;
}

int Mods_CheckManifest(int mod, char *error, size_t size)
{
    const char *arrays[] = {"settings", "requires", "after", "conflicts"};
    for (int i = 0; i < 4; i++) {
        const JsonValue *v = member(mod, arrays[i]);
        if (v && Json_TypeOf(v) != JSON_ARRAY) {
            snprintf(error, size, "%s must be an array", arrays[i]);
            return 0;
        }
    }
    for (int i = 0; i < Mods_OptionCount(mod); i++) {
        const JsonValue *spec = Mods_Option(mod, i);
        const char *type = Json_String(Json_Member(spec, "type"), "int");
        const char *key = Json_String(Json_Member(spec, "key"), "");
        long low = Json_Number(Json_Member(spec, "min"), 0), high = Json_Number(Json_Member(spec, "max"), 100);
        long def = Json_Number(Json_Member(spec, "default"), 0), step = Json_Number(Json_Member(spec, "step"), 1);
        if ((strcmp(type, "int") && strcmp(type, "bool") && strcmp(type, "choice") && strcmp(type, "key")) ||
            low < INT_MIN || high > INT_MAX || low > high || step < 1 || step > INT_MAX || def < INT_MIN ||
            def > INT_MAX || !Mods_OptionValid(mod, i, (int)def)) {
            snprintf(error, size, "Invalid setting schema: %s", key);
            return 0;
        }
        for (int j = 0; j < i; j++)
            if (!strcmp(key, Json_String(Json_Member(Mods_Option(mod, j), "key"), ""))) {
                snprintf(error, size, "Duplicate setting: %s", key);
                return 0;
            }
    }
    return 1;
}
