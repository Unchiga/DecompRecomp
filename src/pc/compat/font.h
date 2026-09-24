#ifndef MEMORIES_PC_COMPAT_FONT_H
#define MEMORIES_PC_COMPAT_FONT_H
/* FreeType's filename entry point uses the host code page on Windows.
 * Feed it bytes read through our filesystem boundary instead. */
#ifdef _WIN32
#include "fs.h"
#include <ft2build.h>
#include FT_FREETYPE_H

static void memories_font_free(void *object)
{
    free(((FT_Face)object)->generic.data);
}

static FT_Error memories_font_open(FT_Library library, const char *path, FT_Long index, FT_Face *face)
{
    FILE *file = fopen(path, "rb");
    unsigned char *bytes;
    long size;
    FT_Error error;
    if (!file) return FT_Err_Cannot_Open_Resource;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file); return FT_Err_Cannot_Open_Resource;
    }
    bytes = malloc((size_t)size);
    if (!bytes) { fclose(file); return FT_Err_Out_Of_Memory; }
    if (fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes); fclose(file); return FT_Err_Cannot_Open_Resource;
    }
    fclose(file);
    error = FT_New_Memory_Face(library, bytes, size, index, face);
    if (error) free(bytes);
    else { (*face)->generic.data = bytes; (*face)->generic.finalizer = memories_font_free; }
    return error;
}
#define FT_New_Face memories_font_open
#endif
#endif
