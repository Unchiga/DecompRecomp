/* A code mod's object file, loaded (object_loader.h). Everything read from
 * the file is checked before it is used: offsets and sizes against the file,
 * indexes against their tables, strings against their sections. Values are
 * copied out with memcpy, so nothing in the file needs to be aligned. */
#define _DEFAULT_SOURCE   /* MAP_ANONYMOUS */
#include "object_loader.h"
#include "pc/compat/mman.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096u
#define IMAGE_MAX (64u << 20)   /* far more than any mod; keeps the arithmetic small */

/* ELF32, as far as a relocatable i386 object needs it. */
#define ET_REL 1
#define EM_386 3
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_PREINIT_ARRAY 16
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHF_TLS 0x400
#define SHN_UNDEF 0
#define SHN_LORESERVE 0xff00
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define STB_LOCAL 0
#define STB_WEAK 2
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_TLS 6
#define R_386_NONE 0
#define R_386_32 1
#define R_386_PC32 2
#define R_386_GOT32 3
#define R_386_PLT32 4
#define R_386_GOTOFF 9
#define R_386_GOTPC 10
#define R_386_GOT32X 43

typedef struct {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
} Section;

typedef struct {
    const unsigned char *file;
    size_t file_size;
    Section *sections;
    unsigned section_count;
    uint32_t *place;          /* each section's offset in the image, or NOT_LOADED */
    const Section *symtab, *strtab;
    unsigned symtab_index;
    uintptr_t *values;        /* each symbol's address, once bound */
    unsigned char *bound;     /* whether it has one a relocation may use */
    char *error;
    size_t error_size;
} Loader;

#define NOT_LOADED 0xffffffffu

static int fail(Loader *loader, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(loader->error, loader->error_size, format, arguments);
    va_end(arguments);
    return -1;
}

static uint16_t u16(const unsigned char *at) { uint16_t v; memcpy(&v, at, 2); return v; }
static uint32_t u32(const unsigned char *at) { uint32_t v; memcpy(&v, at, 4); return v; }

/* Whether [offset, offset + length) lies inside [0, limit). */
static int inside(uint64_t offset, uint64_t length, uint64_t limit)
{
    return offset <= limit && length <= limit - offset;
}

/* A NUL-terminated string at `offset` in a string table, or NULL. */
static const char *string_at(const Loader *loader, const Section *table, uint32_t offset)
{
    const char *start;
    if (!table || table->type != SHT_STRTAB || offset >= table->size) return NULL;
    start = (const char *)loader->file + table->offset + offset;
    return memchr(start, '\0', table->size - offset) ? start : NULL;
}

static const char *section_name(const Loader *loader, const Section *section, const Section *names)
{
    const char *name = string_at(loader, names, section->name);
    return name ? name : "";
}

static int read_sections(Loader *loader)
{
    const unsigned char *file = loader->file;
    uint32_t section_offset;
    unsigned i, names_index, symtabs = 0;
    const Section *names;
    if (loader->file_size < 52 || memcmp(file, "\177ELF", 4)) return fail(loader, "is not an ELF object file");
    if (file[4] != 1 || file[5] != 1) return fail(loader, "is not a 32-bit little-endian ELF object");
    if (u16(file + 16) != ET_REL) return fail(loader, "is not a relocatable object (.o); build it with tools/pc/build_mod.py");
    if (u16(file + 18) != EM_386) return fail(loader, "is not 32-bit x86 code");
    section_offset = u32(file + 32);
    loader->section_count = u16(file + 48);
    names_index = u16(file + 50);
    if (u16(file + 46) != 40 || !loader->section_count) return fail(loader, "has no section table");
    if (!inside(section_offset, (uint64_t)loader->section_count * 40, loader->file_size)) {
        return fail(loader, "is cut short (the section table runs past the end)");
    }
    if (names_index >= loader->section_count) return fail(loader, "has a bad section name table");
    loader->sections = calloc(loader->section_count, sizeof(*loader->sections));
    loader->place = malloc(loader->section_count * sizeof(*loader->place));
    if (!loader->sections || !loader->place) return fail(loader, "out of memory");
    for (i = 0; i < loader->section_count; i++) {
        const unsigned char *at = file + section_offset + i * 40;
        Section *section = &loader->sections[i];
        section->name = u32(at);
        section->type = u32(at + 4);
        section->flags = u32(at + 8);
        section->addr = u32(at + 12);
        section->offset = u32(at + 16);
        section->size = u32(at + 20);
        section->link = u32(at + 24);
        section->info = u32(at + 28);
        section->addralign = u32(at + 32);
        section->entsize = u32(at + 36);
        loader->place[i] = NOT_LOADED;
        if (section->type != SHT_NOBITS && !inside(section->offset, section->size, loader->file_size)) {
            return fail(loader, "is cut short (section %u runs past the end)", i);
        }
    }
    names = &loader->sections[names_index];
    if (names->type != SHT_STRTAB) return fail(loader, "has a bad section name table");
    for (i = 0; i < loader->section_count; i++) {
        const Section *section = &loader->sections[i];
        const char *name = section_name(loader, section, names);
        if (section->type == SHT_INIT_ARRAY || section->type == SHT_FINI_ARRAY || section->type == SHT_PREINIT_ARRAY ||
            !strncmp(name, ".ctors", 6) || !strncmp(name, ".dtors", 6)) {
            return fail(loader, "has constructors or destructors (%s), which mods may not have; "
                                "do that work in MemoriesModInit", name);
        }
        if (section->flags & SHF_TLS) return fail(loader, "has thread-local storage (%s), which mods may not have", name);
        if (section->type == SHT_RELA && section->info < loader->section_count &&
            (loader->sections[section->info].flags & SHF_ALLOC)) {
            return fail(loader, "has RELA relocations, which 32-bit x86 objects do not use");
        }
        if (section->type == SHT_SYMTAB) {
            symtabs++;
            loader->symtab = section;
            loader->symtab_index = i;
        }
    }
    if (symtabs != 1) return fail(loader, "has %s symbol table", symtabs ? "more than one" : "no");
    if (loader->symtab->entsize != 16 || loader->symtab->size % 16 || loader->symtab->link >= loader->section_count ||
        loader->sections[loader->symtab->link].type != SHT_STRTAB) {
        return fail(loader, "has a bad symbol table");
    }
    loader->strtab = &loader->sections[loader->symtab->link];
    return 0;
}

/* Code first, then everything else, each part starting on a page, so the
 * code pages can be made executable and the rest left writable. */
static int lay_out(Loader *loader, size_t *code_size, size_t *total)
{
    int pass;
    uint64_t cursor = 0;
    for (pass = 0; pass < 2; pass++) {
        unsigned i;
        for (i = 0; i < loader->section_count; i++) {
            const Section *section = &loader->sections[i];
            uint32_t align = section->addralign ? section->addralign : 1;
            if (!(section->flags & SHF_ALLOC) || !!(section->flags & SHF_EXECINSTR) != !pass) continue;
            if (section->type != SHT_PROGBITS && section->type != SHT_NOBITS) continue;
            if (align & (align - 1) || align > PAGE) return fail(loader, "section %u has an alignment of %u", i, align);
            cursor = (cursor + align - 1) & ~(uint64_t)(align - 1);
            loader->place[i] = (uint32_t)cursor;
            cursor += section->size;
            if (cursor > IMAGE_MAX) return fail(loader, "is larger than %u MiB", IMAGE_MAX >> 20);
        }
        cursor = (cursor + PAGE - 1) & ~(uint64_t)(PAGE - 1);
        if (!pass) *code_size = (size_t)cursor;
    }
    *total = (size_t)cursor;
    return *total ? 0 : fail(loader, "has no code or data");
}

static int bind_symbols(Loader *loader, unsigned char *image, ObjectResolver resolve, void *context)
{
    unsigned i, count = loader->symtab->size / 16;
    loader->values = calloc(count ? count : 1, sizeof(*loader->values));
    loader->bound = calloc(count ? count : 1, 1);
    if (!loader->values || !loader->bound) return fail(loader, "out of memory");
    for (i = 1; i < count; i++) {
        const unsigned char *at = loader->file + loader->symtab->offset + i * 16;
        uint32_t value = u32(at + 4);
        unsigned type = at[12] & 0xf, bind = at[12] >> 4, index = u16(at + 14);
        const char *name = string_at(loader, loader->strtab, u32(at));
        if (!name) return fail(loader, "symbol %u has a bad name", i);
        if (type == STT_TLS) return fail(loader, "%s is thread-local, which mods may not have", name);
        if (index == SHN_UNDEF) {
            void *address;
            if (!*name) return fail(loader, "symbol %u is undefined and has no name", i);
            if (!strcmp(name, "_GLOBAL_OFFSET_TABLE_")) {
                return fail(loader, "is position-independent code; build it with tools/pc/build_mod.py");
            }
            if (!strncmp(name, "__stack_chk_", 12)) {
                return fail(loader, "uses the stack protector, which reads Linux thread storage; "
                                    "build it with tools/pc/build_mod.py");
            }
            address = resolve ? resolve(name, context) : NULL;
            if (!address && bind != STB_WEAK) return fail(loader, "needs %s, which this game does not provide", name);
            loader->values[i] = (uintptr_t)address;
            loader->bound[i] = 1;
        } else if (index == SHN_ABS) {
            loader->values[i] = value;
            loader->bound[i] = 1;
        } else if (index == SHN_COMMON) {
            return fail(loader, "%s is a COMMON symbol; build with -fno-common (tools/pc/build_mod.py does)", name);
        } else if (index >= SHN_LORESERVE || index >= loader->section_count) {
            return fail(loader, "symbol %s is in section %u, which does not exist", name, index);
        } else if (loader->place[index] != NOT_LOADED) {
            if (value > loader->sections[index].size) return fail(loader, "symbol %s lies outside its section", name);
            loader->values[i] = (uintptr_t)(image + loader->place[index] + value);
            loader->bound[i] = 1;
        }
        /* A symbol in a section that is not loaded (debugging information)
         * stays unbound; only a relocation in loaded code could use it. */
    }
    return 0;
}

static int relocate(Loader *loader, unsigned char *image)
{
    unsigned i, count = loader->symtab->size / 16;
    for (i = 0; i < loader->section_count; i++) {
        const Section *table = &loader->sections[i], *target;
        unsigned r;
        if (table->type != SHT_REL) continue;
        if (table->info >= loader->section_count || loader->place[table->info] == NOT_LOADED) continue;
        target = &loader->sections[table->info];
        if (table->link != loader->symtab_index || table->entsize != 8 || table->size % 8) {
            return fail(loader, "has a bad relocation table (section %u)", i);
        }
        for (r = 0; r < table->size / 8; r++) {
            const unsigned char *at = loader->file + table->offset + r * 8;
            uint32_t offset = u32(at), info = u32(at + 4), symbol = info >> 8, type = info & 0xff, word;
            unsigned char *place;
            uintptr_t value;
            if (type == R_386_NONE) continue;
            if (!inside(offset, 4, target->size) || target->type == SHT_NOBITS) {
                return fail(loader, "has a relocation outside its section (section %u)", table->info);
            }
            if (symbol >= count || (symbol && !loader->bound[symbol])) {
                return fail(loader, "has a relocation against a symbol it does not load");
            }
            place = image + loader->place[table->info] + offset;
            value = symbol ? loader->values[symbol] : 0;
            memcpy(&word, place, 4);   /* REL: the addend is in place */
            switch (type) {
            case R_386_32:
                word += (uint32_t)value;
                break;
            case R_386_PC32:
            case R_386_PLT32:   /* nothing goes through a PLT here: a direct call */
                word += (uint32_t)value - (uint32_t)(uintptr_t)place;
                break;
            case R_386_GOT32:
            case R_386_GOT32X:
            case R_386_GOTOFF:
            case R_386_GOTPC:
                return fail(loader, "is position-independent code; build it with tools/pc/build_mod.py");
            default:
                return fail(loader, "has relocation type %u, which the mod loader does not handle", type);
            }
            memcpy(place, &word, 4);
        }
    }
    return 0;
}

/* The object's own functions and variables, kept for ObjectLoader_Symbol
 * and for crash reports. Their names point into a copy of the string table,
 * whose entries bind_symbols has already found terminated. */
static int keep_symbols(Loader *loader, LoadedObject *object)
{
    unsigned i, count = loader->symtab->size / 16;
    object->strings = malloc(loader->strtab->size ? loader->strtab->size : 1);
    object->symbols = calloc(count ? count : 1, sizeof(*object->symbols));
    if (!object->strings || !object->symbols) return fail(loader, "out of memory");
    memcpy(object->strings, loader->file + loader->strtab->offset, loader->strtab->size);
    for (i = 1; i < count; i++) {
        const unsigned char *at = loader->file + loader->symtab->offset + i * 16;
        unsigned type = at[12] & 0xf, bind = at[12] >> 4, index = u16(at + 14);
        uint32_t name = u32(at);
        struct ObjectSymbol *kept;
        if ((type != STT_FUNC && type != STT_OBJECT) || index == SHN_UNDEF || index >= loader->section_count ||
            loader->place[index] == NOT_LOADED || !object->strings[name]) {
            continue;
        }
        kept = &object->symbols[object->symbol_count++];
        kept->name = object->strings + name;
        kept->address = loader->values[i];
        kept->size = u32(at + 8);
        kept->function = type == STT_FUNC;
        kept->global = bind != STB_LOCAL;
    }
    return 0;
}

int ObjectLoader_Load(const void *data, size_t size, ObjectResolver resolve, void *context,
                      LoadedObject *object, char *error, size_t error_size)
{
    Loader loader;
    unsigned char *image = NULL;
    size_t code_size = 0, total = 0;
    unsigned i;
    int result = -1;
    memset(object, 0, sizeof(*object));
    memset(&loader, 0, sizeof(loader));
    loader.file = data;
    loader.file_size = size;
    loader.error = error;
    loader.error_size = error_size;
    if (error_size) error[0] = '\0';
    if (!data) {
        fail(&loader, "is empty");
        goto done;
    }
    if (read_sections(&loader) || lay_out(&loader, &code_size, &total)) goto done;
    image = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (image == MAP_FAILED) {
        image = NULL;
        fail(&loader, "does not fit in memory (%u bytes)", (unsigned)total);
        goto done;
    }
    for (i = 0; i < loader.section_count; i++) {
        const Section *section = &loader.sections[i];
        if (loader.place[i] != NOT_LOADED && section->type == SHT_PROGBITS && section->size) {
            memcpy(image + loader.place[i], loader.file + section->offset, section->size);
        }
    }
    object->image = image;
    object->size = total;
    object->code_size = code_size;
    if (bind_symbols(&loader, image, resolve, context) || relocate(&loader, image) || keep_symbols(&loader, object)) {
        goto done;
    }
    if (code_size && mprotect(image, code_size, PROT_READ | PROT_EXEC)) {
        fail(&loader, "could not make its code executable");
        goto done;
    }
    result = 0;
done:
    if (result) ObjectLoader_Free(object);
    free(loader.sections);
    free(loader.place);
    free(loader.values);
    free(loader.bound);
    return result;
}

void *ObjectLoader_Symbol(const LoadedObject *object, const char *name)
{
    size_t i;
    for (i = 0; object && name && i < object->symbol_count; i++) {
        if (object->symbols[i].global && !strcmp(object->symbols[i].name, name)) {
            return (void *)object->symbols[i].address;
        }
    }
    return NULL;
}

void ObjectLoader_Free(LoadedObject *object)
{
    if (object->image) munmap(object->image, object->size);
    free(object->symbols);
    free(object->strings);
    memset(object, 0, sizeof(*object));
}
