/* The manifest reader: what a mod.json is allowed to say, and what happens
 * to one that is malformed. */
#include "pc/mods/json.h"
#include <assert.h>
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
    /* A trailing comma is common enough in a hand-written manifest to allow. */
    document = Json_Parse("{ \"a\": [1, 2,], }", error, sizeof(error));
    assert(document && Json_Count(Json_Member(Json_Root(document), "a")) == 2);
    Json_Free(document);
    Json_Free(NULL);
    return 0;
}
