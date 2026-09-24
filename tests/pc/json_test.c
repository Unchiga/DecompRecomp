/* The manifest reader: what a mod.json is allowed to say, and what happens
 * to one that is malformed. */
#include "pc/mods/json.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char *manifest =
    "{\n"
    "    \"id\": \"card-tweaks\",\n"
    "    \"name\": \"Card tweaks\",\n"
    "    \"enabled\": true,\n"
    "    \"restart\": false,\n"
    "    \"weight\": 12,\n"
    "    \"note\": \"a \\\"quoted\\\" \\\\ word\\nand a line\",\n"
    "    \"data\": [\n"
    "        { \"file\": \"\\\\DATA\\\\CARD.MRG;1\", \"replace\": \"card.mrg\" },\n"
    "        { \"lba\": 2776, \"patch\": [ { \"at\": \"0x5D800\", \"bytes\": \"26 25\" } ] }\n"
    "    ]\n"
    "}\n";

int main(void)
{
    char error[128];
    JsonDocument *document = Json_Parse(manifest, error, sizeof(error));
    const JsonValue *root, *data, *entry, *patch;
    assert(document);
    root = Json_Root(document);
    assert(Json_TypeOf(root) == JSON_OBJECT);
    assert(!strcmp(Json_String(Json_Member(root, "id"), ""), "card-tweaks"));
    assert(!strcmp(Json_String(Json_Member(root, "name"), ""), "Card tweaks"));
    assert(Json_Bool(Json_Member(root, "enabled"), 0) == 1);
    assert(Json_Bool(Json_Member(root, "restart"), 1) == 0);
    assert(Json_Bool(Json_Member(root, "missing"), 7) == 7);
    assert(Json_Number(Json_Member(root, "weight"), 0) == 12);
    /* Escapes are undone in place, and the text still ends where it should. */
    assert(!strcmp(Json_String(Json_Member(root, "note"), ""), "a \"quoted\" \\ word\nand a line"));
    data = Json_Member(root, "data");
    assert(Json_TypeOf(data) == JSON_ARRAY && Json_Count(data) == 2);
    entry = Json_At(data, 0);
    assert(!strcmp(Json_String(Json_Member(entry, "file"), ""), "\\DATA\\CARD.MRG;1"));
    assert(!strcmp(Json_String(Json_Member(entry, "replace"), ""), "card.mrg"));
    entry = Json_At(data, 1);
    assert(Json_Number(Json_Member(entry, "lba"), -1) == 2776);
    patch = Json_Member(entry, "patch");
    assert(Json_Count(patch) == 1);
    /* An offset written the way a tutorial writes one. */
    assert(Json_Number(Json_Member(Json_At(patch, 0), "at"), -1) == 0x5D800);
    assert(!strcmp(Json_String(Json_Member(Json_At(patch, 0), "bytes"), ""), "26 25"));
    assert(!Json_Count(Json_Member(root, "id")));
    assert(!Json_At(data, 2) && !Json_At(data, -1));
    Json_Free(document);

    /* Malformed manifests fail with a reason rather than a crash. */
    assert(!Json_Parse("{ \"id\": }", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("{ \"id\" \"x\" }", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("{ \"id\": \"x\"", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("{} trailing", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("", error, sizeof(error)) && error[0]);
    assert(!Json_Parse("{ \"a\": \"\\q\" }", error, sizeof(error)) && error[0]);
    assert(!Json_ParseFile("/does/not/exist.json", error, sizeof(error)) && error[0]);
    /* Numbers are JSON's, in base 10, and whole. */
    {
        static const struct { const char *text; long value; } good[] = {
            {"[0]", 0}, {"[-0]", 0}, {"[10]", 10}, {"[-42]", -42}, {"[1e3]", 1000}, {"[2.50e1]", 25},
            {"[7.0]", 7}, {"[1000E-3]", 1}, {"[1E+2]", 100}, {"[2147483647]", 2147483647L},
        };
        static const char *bad[] = {
            "[010]", "[08]", "[1-2]", "[1.5]", "[1e-3]", "[1.]", "[.5]", "[-]", "[1e]", "[+1]",
            "[0x10]", "[99999999999999999999]", "[1e400]",
        };
        char text[64];
        unsigned i;
        for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            document = Json_Parse(good[i].text, error, sizeof(error));
            assert(document && Json_Number(Json_At(Json_Root(document), 0), -1) == good[i].value);
            Json_Free(document);
        }
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            assert(!Json_Parse(bad[i], error, sizeof(error)) && error[0]);
        }
        /* A long's own ends, whatever its size on this system. */
        snprintf(text, sizeof(text), "[%ld, %ld]", LONG_MAX, LONG_MIN);
        document = Json_Parse(text, error, sizeof(error));
        assert(document && Json_Number(Json_At(Json_Root(document), 0), 0) == LONG_MAX &&
               Json_Number(Json_At(Json_Root(document), 1), 0) == LONG_MIN);
        Json_Free(document);
        /* Strings that hold a number: hexadecimal with its 0x, else decimal. */
        document = Json_Parse("[\"0x5D800\", \"010\", \" -3 \", \"08\", \"1.5\"]", error, sizeof(error));
        assert(document);
        assert(Json_Number(Json_At(Json_Root(document), 0), -1) == 0x5D800);
        assert(Json_Number(Json_At(Json_Root(document), 1), -1) == 10);
        assert(Json_Number(Json_At(Json_Root(document), 2), -1) == -3);
        assert(Json_Number(Json_At(Json_Root(document), 3), -1) == 8);
        assert(Json_Number(Json_At(Json_Root(document), 4), -1) == -1);
        Json_Free(document);
    }
    /* Nesting has a limit, so a hostile manifest cannot run the stack out. */
    {
        static char deep[200001];
        memset(deep, '[', sizeof(deep) - 1);
        assert(!Json_Parse(deep, error, sizeof(error)) && strstr(error, "nested"));
        memset(deep, 0, sizeof(deep));
        memset(deep, '[', 64);
        memset(deep + 64, ']', 64);
        document = Json_Parse(deep, error, sizeof(error));
        assert(document);
        Json_Free(document);
        deep[128] = ']';
        memmove(deep + 1, deep, 128);
        deep[0] = '[';
        assert(!Json_Parse(deep, error, sizeof(error)) && strstr(error, "nested"));
    }
    /* A trailing comma is common enough in a hand-written manifest to allow. */
    document = Json_Parse("{ \"a\": [1, 2,], }", error, sizeof(error));
    assert(document && Json_Count(Json_Member(Json_Root(document), "a")) == 2);
    Json_Free(document);
    Json_Free(NULL);
    return 0;
}
