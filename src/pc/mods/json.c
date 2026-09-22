/* The manifest reader (json.h). One pass over a private copy of the text:
 * strings are unescaped in place, which only ever shortens them, and values
 * come from arena blocks so that a parsed document's pointers stay put. */
#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 32

struct JsonValue {
    JsonType type;
    const char *name;   /* the member name, inside an object */
    char *text;         /* strings and numbers: their text */
    long number;
    int boolean;
    JsonValue *first, *next; /* children, and the next value beside this one */
};

typedef struct Block { struct Block *next; int used; JsonValue values[BLOCK]; } Block;

struct JsonDocument {
    char *text;
    Block *blocks;
    JsonValue *root;
};

typedef struct {
    JsonDocument *document;
    char *at;
    char *error;
    size_t error_size;
} Parser;

static void fail(Parser *parser, const char *why)
{
    if (parser->error && parser->error_size && !parser->error[0]) {
        snprintf(parser->error, parser->error_size, "%s", why);
    }
}

static JsonValue *make(Parser *parser)
{
    Block *block = parser->document->blocks;
    if (!block || block->used == BLOCK) {
        block = calloc(1, sizeof(*block));
        if (!block) { fail(parser, "out of memory"); return NULL; }
        block->next = parser->document->blocks;
        parser->document->blocks = block;
    }
    return &block->values[block->used++];
}

static void skip_space(Parser *parser)
{
    while (isspace((unsigned char)*parser->at)) parser->at++;
}

/* A string, unescaped over itself and terminated where its quote was. */
static char *parse_string(Parser *parser)
{
    char *out, *start;
    if (*parser->at != '"') { fail(parser, "expected a string"); return NULL; }
    parser->at++;
    out = start = parser->at;
    while (*parser->at && *parser->at != '"') {
        if (*parser->at != '\\') { *out++ = *parser->at++; continue; }
        parser->at++;
        switch (*parser->at) {
        case 'n': *out++ = '\n'; break;
        case 't': *out++ = '\t'; break;
        case 'r': *out++ = '\r'; break;
        case 'b': *out++ = '\b'; break;
        case 'f': *out++ = '\f'; break;
        case '\\': case '/': case '"': *out++ = *parser->at; break;
        case 'u': {
            /* Only the characters a manifest can need: one byte of Latin-1,
             * which covers an author's name. Anything above stays as "?". */
            unsigned code = 0;
            int i;
            for (i = 1; i <= 4; i++) {
                char digit = parser->at[i];
                if (!isxdigit((unsigned char)digit)) { fail(parser, "bad \\u escape"); return NULL; }
                code = code * 16 + (unsigned)(isdigit((unsigned char)digit) ? digit - '0'
                                              : tolower((unsigned char)digit) - 'a' + 10);
            }
            *out++ = code < 0x100 ? (char)code : '?';
            parser->at += 4;
            break;
        }
        default: fail(parser, "bad escape"); return NULL;
        }
        parser->at++;
    }
    if (*parser->at != '"') { fail(parser, "unterminated string"); return NULL; }
    parser->at++;      /* past the closing quote, which the copy may not reach */
    *out = '\0';
    return start;
}

static JsonValue *parse_value(Parser *parser);

static JsonValue *parse_container(Parser *parser, JsonValue *value, char close)
{
    JsonValue *last = NULL;
    parser->at++;
    for (;;) {
        JsonValue *child;
        const char *name = NULL;
        skip_space(parser);
        if (*parser->at == close) { parser->at++; return value; }
        if (last) {
            if (*parser->at != ',') { fail(parser, "expected a comma"); return NULL; }
            parser->at++;
            skip_space(parser);
            if (*parser->at == close) { parser->at++; return value; } /* a trailing comma */
        }
        if (close == '}') {
            name = parse_string(parser);
            if (!name) return NULL;
            skip_space(parser);
            if (*parser->at != ':') { fail(parser, "expected a colon"); return NULL; }
            parser->at++;
        }
        child = parse_value(parser);
        if (!child) return NULL;
        child->name = name;
        if (last) last->next = child;
        else value->first = child;
        last = child;
    }
}

static JsonValue *parse_value(Parser *parser)
{
    JsonValue *value;
    skip_space(parser);
    value = make(parser);
    if (!value) return NULL;
    memset(value, 0, sizeof(*value));
    switch (*parser->at) {
    case '{': value->type = JSON_OBJECT; return parse_container(parser, value, '}');
    case '[': value->type = JSON_ARRAY; return parse_container(parser, value, ']');
    case '"':
        value->type = JSON_STRING;
        value->text = parse_string(parser);
        return value->text ? value : NULL;
    default:
        if (!strncmp(parser->at, "true", 4)) { parser->at += 4; value->type = JSON_BOOL; value->boolean = 1; return value; }
        if (!strncmp(parser->at, "false", 5)) { parser->at += 5; value->type = JSON_BOOL; return value; }
        if (!strncmp(parser->at, "null", 4)) { parser->at += 4; value->type = JSON_NULL; return value; }
        if (*parser->at == '-' || isdigit((unsigned char)*parser->at)) {
            char *end;
            value->type = JSON_NUMBER;
            value->text = parser->at;
            value->number = strtol(parser->at, &end, 0);
            if (end == parser->at) { fail(parser, "bad number"); return NULL; }
            /* A fraction or an exponent is read past but not kept: nothing a
             * manifest holds is fractional. */
            while (*end == '.' || *end == 'e' || *end == 'E' || *end == '+' ||
                   *end == '-' || isdigit((unsigned char)*end)) {
                end++;
            }
            parser->at = end;
            return value;
        }
        fail(parser, *parser->at ? "unexpected character" : "unexpected end of file");
        return NULL;
    }
}

JsonDocument *Json_Parse(const char *text, char *error, size_t error_size)
{
    JsonDocument *document;
    Parser parser;
    if (error && error_size) error[0] = '\0';
    if (!text) return NULL;
    document = calloc(1, sizeof(*document));
    if (!document) return NULL;
    document->text = malloc(strlen(text) + 1);
    if (!document->text) { free(document); return NULL; }
    memcpy(document->text, text, strlen(text) + 1);
    parser.document = document;
    parser.at = document->text;
    parser.error = error;
    parser.error_size = error_size;
    document->root = parse_value(&parser);
    if (document->root) {
        skip_space(&parser);
        if (*parser.at) { fail(&parser, "trailing text"); document->root = NULL; }
    }
    if (!document->root) { Json_Free(document); return NULL; }
    return document;
}

JsonDocument *Json_ParseFile(const char *path, char *error, size_t error_size)
{
    JsonDocument *document;
    char *text;
    long size;
    FILE *file = fopen(path, "rb");
    if (error && error_size) error[0] = '\0';
    if (!file) {
        if (error && error_size) snprintf(error, error_size, "cannot be opened");
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        if (error && error_size) snprintf(error, error_size, "cannot be read");
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (!text) { fclose(file); return NULL; }
    if (fread(text, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(text);
        if (error && error_size) snprintf(error, error_size, "cannot be read");
        return NULL;
    }
    fclose(file);
    text[size] = '\0';
    document = Json_Parse(text, error, error_size);
    free(text);
    return document;
}

void Json_Free(JsonDocument *document)
{
    Block *block;
    if (!document) return;
    block = document->blocks;
    while (block) {
        Block *next = block->next;
        free(block);
        block = next;
    }
    free(document->text);
    free(document);
}

const JsonValue *Json_Root(const JsonDocument *document) { return document ? document->root : NULL; }
JsonType Json_TypeOf(const JsonValue *value) { return value ? value->type : JSON_NULL; }
const char *Json_Name(const JsonValue *member) { return member ? member->name : NULL; }

const JsonValue *Json_Member(const JsonValue *object, const char *name)
{
    const JsonValue *child;
    if (!object || object->type != JSON_OBJECT || !name) return NULL;
    for (child = object->first; child; child = child->next) {
        if (child->name && !strcmp(child->name, name)) return child;
    }
    return NULL;
}

int Json_Count(const JsonValue *value)
{
    const JsonValue *child;
    int count = 0;
    if (!value || (value->type != JSON_ARRAY && value->type != JSON_OBJECT)) return 0;
    for (child = value->first; child; child = child->next) count++;
    return count;
}

const JsonValue *Json_At(const JsonValue *value, int index)
{
    const JsonValue *child;
    if (!value || index < 0) return NULL;
    for (child = value->first; child; child = child->next) {
        if (!index--) return child;
    }
    return NULL;
}

const char *Json_String(const JsonValue *value, const char *fallback)
{
    return value && value->type == JSON_STRING ? value->text : fallback;
}

long Json_Number(const JsonValue *value, long fallback)
{
    if (!value) return fallback;
    if (value->type == JSON_NUMBER) return value->number;
    if (value->type == JSON_BOOL) return value->boolean;
    if (value->type == JSON_STRING && value->text) {
        char *end;
        long parsed = strtol(value->text, &end, 0);
        while (isspace((unsigned char)*end)) end++;
        if (end != value->text && !*end) return parsed;
    }
    return fallback;
}

int Json_Bool(const JsonValue *value, int fallback)
{
    if (!value) return fallback;
    if (value->type == JSON_BOOL) return value->boolean;
    if (value->type == JSON_NUMBER) return value->number != 0;
    return fallback;
}
