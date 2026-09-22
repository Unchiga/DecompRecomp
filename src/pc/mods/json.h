#ifndef MEMORIES_PC_MODS_JSON_H
#define MEMORIES_PC_MODS_JSON_H
/* Enough JSON to read a mod manifest (mods.h): objects, arrays, strings,
 * whole numbers, true/false and null. Numbers keep their text as well, so a
 * manifest can write a disc offset as "0x5D800" and have it read either way.
 * Nothing here allocates while the game runs: a manifest is parsed once. */
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
const char *Json_Name(const JsonValue *member);

const char *Json_String(const JsonValue *value, const char *fallback);
/* Whole numbers, and strings that hold one ("0x5D800", "-3"). */
long Json_Number(const JsonValue *value, long fallback);
int Json_Bool(const JsonValue *value, int fallback);

#endif
