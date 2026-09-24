#ifndef MEMORIES_PC_MODS_JSON_H
#define MEMORIES_PC_MODS_JSON_H
/* Enough JSON to read a mod manifest (mods.h): objects, arrays, strings,
 * whole numbers, true/false and null. Numbers keep their text as well, so a
 * manifest can write a disc offset as "0x5D800" and have it read either way.
 * Nothing here allocates while the game runs: a manifest is parsed once.
 * Numbers follow JSON's grammar in base 10 and must be whole and fit a long;
 * arrays and objects nest at most 64 deep. */
#include <stddef.h>

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
typedef struct JsonDocument JsonDocument;

/* Parses `text`; on failure returns NULL and writes why into `error`. */
JsonDocument *Json_Parse(const char *text, char *error, size_t error_size);
JsonDocument *Json_ParseFile(const char *path, char *error, size_t error_size);
void Json_Free(JsonDocument *document);
const JsonValue *Json_Root(const JsonDocument *document);

JsonType Json_TypeOf(const JsonValue *value);
/* Members of an object, elements of an array; both 0 for anything else. */
const JsonValue *Json_Member(const JsonValue *object, const char *name);
int Json_Count(const JsonValue *value);
const JsonValue *Json_At(const JsonValue *value, int index);
/* The element or member after this one, NULL after the last. Json_At walks
 * from the start every time, so a loop over a long array goes this way:
 * for (item = Json_At(list, 0); item; item = Json_Next(item)). */
const JsonValue *Json_Next(const JsonValue *value);
const char *Json_Name(const JsonValue *member);

const char *Json_String(const JsonValue *value, const char *fallback);
/* Whole numbers, and strings that hold one ("0x5D800", "-3"; decimal
 * unless written with "0x"). */
long Json_Number(const JsonValue *value, long fallback);
int Json_Bool(const JsonValue *value, int fallback);

#endif
