/* The mod library loader (modload.h). A `.mod` is an i386 ELF shared object
 * with no library of its own to need: it is read whole, its loadable
 * segments are copied into fresh pages, its relocations are applied against
 * its own definitions and the two tables of what a mod may use, and its
 * pages are then given the protection each segment asks for. Nothing here
 * depends on the host's own library format, which is what lets one file run
 * on Linux and on Windows: both are 32-bit x86 with the same C calling
 * convention, and guest RAM sits at the same addresses in both.
 *
 * The file is the mod's own, so everything read from it is bounds-checked:
 * a truncated or malformed library is refused with a reason, never followed
 * off the end of the buffer. */
#include "modload.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define PAGE 4096u
#define FILE_MAX (64u << 20)
#define SPAN_MAX (256u << 20)

/* The parts of the ELF format (the System V ABI, i386 supplement) a
 * relocatable shared object needs; declared here because Windows has no
 * <elf.h>. */
typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ElfHeader;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} ElfSegment;

typedef struct {
    int32_t tag;
    uint32_t value;
} ElfDynamic;

typedef struct {
    uint32_t name, value, size;
    uint8_t info, other;
    uint16_t shndx;
} ElfSymbol;

typedef struct {
    uint32_t offset, info;
} ElfRel;

enum { ET_DYN = 3, EM_386 = 3 };
enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3, PT_TLS = 7 };
enum { PF_X = 1, PF_W = 2, PF_R = 4 };
enum {
    DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
    DT_STRSZ = 10, DT_SYMENT = 11, DT_INIT = 12, DT_REL = 17, DT_RELSZ = 18, DT_RELENT = 19,
    DT_PLTREL = 20, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27
};
enum { STB_WEAK = 2, STT_TLS = 6, SHN_UNDEF = 0 };
enum {
    R_386_NONE = 0, R_386_32 = 1, R_386_PC32 = 2, R_386_GLOB_DAT = 6, R_386_JMP_SLOT = 7,
    R_386_RELATIVE = 8
};

typedef struct {
    uint8_t *memory;   /* the pages the segments were copied into */
    uint32_t span;
    uintptr_t bias;    /* memory minus the lowest segment's address */
    const ElfSymbol *symbols;
    uint32_t symbol_count;
    const char *strings;
    uint32_t strings_size;
} Library;

/* --- the tables a mod links against --------------------------------- */

static void *search(const ModSymbol *table, unsigned count, const char *name)
{
    unsigned low = 0, high = count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        int order = strcmp(name, table[middle].name);
        if (!order) return table[middle].address;
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return NULL;
}

void *ModLoad_Resolve(const char *name)
{
    void *address = search(Memories_ModLibc, Memories_ModLibcCount, name);
    return address ? address : search(Memories_ModSymbols, Memories_ModSymbolCount, name);
}

/* --- pages ------------------------------------------------------------ */

static uint8_t *pages_allocate(uint32_t size)
{
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
#endif
}

static void pages_free(uint8_t *memory, uint32_t size)
{
#ifdef _WIN32
    (void)size;
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, size);
#endif
}

static int pages_protect(uint8_t *memory, uint32_t size, unsigned flags)
{
#ifdef _WIN32
    DWORD old, protect;
    if (flags & PF_X) protect = (flags & PF_W) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & PF_W) protect = PAGE_READWRITE;
    else if (flags & PF_R) protect = PAGE_READONLY;
    else protect = PAGE_NOACCESS;
    if (!VirtualProtect(memory, size, protect, &old)) return 0;
    if (flags & PF_X) FlushInstructionCache(GetCurrentProcess(), memory, size);
    return 1;
#else
    int protect = ((flags & PF_R) ? PROT_READ : 0) | ((flags & PF_W) ? PROT_WRITE : 0) |
                  ((flags & PF_X) ? PROT_EXEC : 0);
    return mprotect(memory, size, protect) == 0;
#endif
}

/* --- loading ---------------------------------------------------------- */

typedef struct {
    const uint8_t *file;
    uint32_t file_size;
    Library *library;
    char *error;
    size_t error_size;
    char missing[256];
    int missing_count;
} Loading;

static int fail(Loading *loading, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(loading->error, loading->error_size, format, arguments);
    va_end(arguments);
    return 0;
}

/* A dynamic-table address as a pointer into the loaded image, with `size`
 * bytes available there, or NULL. */
static void *image_at(const Library *library, uint32_t address, uint32_t size)
{
    uint32_t at = (uint32_t)((uintptr_t)address + library->bias - (uintptr_t)library->memory);
    if (at > library->span || size > library->span - at) return NULL;
    return library->memory + at;
}

static const char *symbol_name(const Library *library, const ElfSymbol *symbol)
{
    uint32_t at = symbol->name, end;
    if (at >= library->strings_size) return NULL;
    for (end = at; end < library->strings_size; end++) {
        if (!library->strings[end]) return library->strings + at;
    }
    return NULL;
}

/* Whether `name` is already one of the comma-separated names in `list`. */
static int listed(const char *list, const char *name)
{
    size_t length = strlen(name);
    while (*list) {
        size_t word = strcspn(list, ",");
        if (word == length && !strncmp(list, name, length)) return 1;
        list += word;
        while (*list == ',' || *list == ' ') list++;
    }
    return 0;
}

/* What symbol `index` stands for: the library's own definition, or what the
 * tables give for a name it leaves undefined. A strong name that nothing
 * gives is recorded for the error and the load goes on, so that every such
 * name is reported at once. */
static int symbol_value(Loading *loading, uint32_t index, uint32_t *value)
{
    const Library *library = loading->library;
    const ElfSymbol *symbol;
    const char *name;
    void *address;
    if (index >= library->symbol_count) return fail(loading, "a relocation names a symbol past the table");
    symbol = &library->symbols[index];
    if ((symbol->info & 0xf) == STT_TLS) return fail(loading, "thread-local storage is not supported");
    if (symbol->shndx != SHN_UNDEF) {
        *value = (uint32_t)(library->bias + symbol->value);
        return 1;
    }
    name = symbol_name(library, symbol);
    if (!name) return fail(loading, "a symbol name is outside the string table");
    address = ModLoad_Resolve(name);
    if (!address && (symbol->info >> 4) != STB_WEAK && !listed(loading->missing, name)) {
        if (strlen(loading->missing) + strlen(name) + 3 < sizeof(loading->missing)) {
            if (loading->missing[0]) strcat(loading->missing, ", ");
            strcat(loading->missing, name);
        }
        loading->missing_count++;
    }
    *value = (uint32_t)(uintptr_t)address;
    return 1;
}

static int relocate(Loading *loading, uint32_t address, uint32_t size)
{
    Library *library = loading->library;
    const ElfRel *relocations;
    uint32_t i, count = size / sizeof(ElfRel);
    if (!size) return 1;
    relocations = image_at(library, address, size);
    if (!relocations) return fail(loading, "relocations outside the image");
    for (i = 0; i < count; i++) {
        uint32_t type = relocations[i].info & 0xff, symbol = relocations[i].info >> 8, value = 0;
        uint32_t *place = image_at(library, relocations[i].offset, 4);
        uint32_t here;
        if (type == R_386_NONE) continue;
        if (!place) return fail(loading, "a relocation outside the image");
        here = (uint32_t)(uintptr_t)place;
        if (type != R_386_RELATIVE && !symbol_value(loading, symbol, &value)) return 0;
        switch (type) {
        case R_386_32: *place += value; break;
        case R_386_PC32: *place += value - here; break;
        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT: *place = value; break;
        case R_386_RELATIVE: *place += (uint32_t)library->bias; break;
        default:
            return fail(loading, "relocation type %u is not supported", (unsigned)type);
        }
    }
    return 1;
}

static int load(Loading *loading)
{
    const ElfHeader *header = (const ElfHeader *)loading->file;
    const ElfSegment *segments, *dynamic_segment = NULL;
    const ElfDynamic *dynamic;
    Library *library = loading->library;
    uint32_t low = UINT32_MAX, high = 0, i, dynamic_count;
    uint32_t rel = 0, rel_size = 0, jmprel = 0, jmprel_size = 0, hash = 0, symtab = 0, strtab = 0, strsz = 0;
    uint32_t init = 0, init_array = 0, init_array_size = 0;
    uint8_t *page_flags;

    if (sizeof(void *) != 4) return fail(loading, "mods run in the 32-bit x86 game only");
    if (loading->file_size < sizeof(ElfHeader) || memcmp(header->ident, "\177ELF", 4))
        return fail(loading, "not a mod library (no ELF header)");
    if (header->ident[4] != 1 || header->ident[5] != 1 || header->machine != EM_386)
        return fail(loading, "not built for 32-bit x86 (see notes/modding.md)");
    if (header->type != ET_DYN) return fail(loading, "not a shared library (link it with -shared)");
    if (header->phentsize != sizeof(ElfSegment) || header->phoff > loading->file_size ||
        (uint32_t)header->phnum * sizeof(ElfSegment) > loading->file_size - header->phoff)
        return fail(loading, "its program headers are malformed");
    segments = (const ElfSegment *)(loading->file + header->phoff);

    for (i = 0; i < header->phnum; i++) {
        const ElfSegment *segment = &segments[i];
        if (segment->type == PT_INTERP) return fail(loading, "it is a program, not a library");
        if (segment->type == PT_TLS) return fail(loading, "thread-local storage is not supported");
        if (segment->type == PT_DYNAMIC) dynamic_segment = segment;
        if (segment->type != PT_LOAD) continue;
        if (segment->filesz > segment->memsz || segment->offset > loading->file_size ||
            segment->filesz > loading->file_size - segment->offset || segment->memsz > SPAN_MAX ||
            segment->vaddr > SPAN_MAX)
            return fail(loading, "a segment is malformed");
        if (segment->vaddr < low) low = segment->vaddr;
        if (segment->vaddr + segment->memsz > high) high = segment->vaddr + segment->memsz;
    }
    if (!dynamic_segment || low >= high) return fail(loading, "it has no dynamic section");
    low &= ~(PAGE - 1);
    high = (high + PAGE - 1) & ~(PAGE - 1);
    if (high - low > SPAN_MAX) return fail(loading, "it is too large");

    library->span = high - low;
    library->memory = pages_allocate(library->span);
    if (!library->memory) return fail(loading, "no memory for it");
    memset(library->memory, 0, library->span);
    library->bias = (uintptr_t)library->memory - low;
    for (i = 0; i < header->phnum; i++) {
        const ElfSegment *segment = &segments[i];
        if (segment->type == PT_LOAD && segment->filesz)
            memcpy(library->memory + (segment->vaddr - low), loading->file + segment->offset, segment->filesz);
    }

    dynamic = image_at(library, dynamic_segment->vaddr, dynamic_segment->memsz);
    if (!dynamic) return fail(loading, "its dynamic section is outside the image");
    dynamic_count = dynamic_segment->memsz / sizeof(ElfDynamic);
    for (i = 0; i < dynamic_count && dynamic[i].tag != DT_NULL; i++) {
        uint32_t value = dynamic[i].value;
        switch (dynamic[i].tag) {
        case DT_NEEDED:
            return fail(loading, "it needs another library; a mod may only use the game and the C library "
                                 "the game gives it (link it with -nostdlib)");
        case DT_RELA: return fail(loading, "it uses RELA relocations, which i386 does not");
        case DT_PLTREL: if (value != DT_REL) return fail(loading, "its PLT relocations are not REL"); break;
        case DT_REL: rel = value; break;
        case DT_RELSZ: rel_size = value; break;
        case DT_JMPREL: jmprel = value; break;
        case DT_PLTRELSZ: jmprel_size = value; break;
        case DT_HASH: hash = value; break;
        case DT_SYMTAB: symtab = value; break;
        case DT_STRTAB: strtab = value; break;
        case DT_STRSZ: strsz = value; break;
        case DT_SYMENT: if (value != sizeof(ElfSymbol)) return fail(loading, "its symbol table is malformed"); break;
        case DT_INIT: init = value; break;
        case DT_INIT_ARRAY: init_array = value; break;
        case DT_INIT_ARRAYSZ: init_array_size = value; break;
        default: break;
        }
    }
    {
        const uint32_t *buckets = hash ? image_at(library, hash, 8) : NULL;
        if (!buckets || !symtab || !strtab)
            return fail(loading, "it has no symbol hash table (link with --hash-style=both)");
        library->symbol_count = buckets[1];
        library->symbols = image_at(library, symtab, library->symbol_count * (uint32_t)sizeof(ElfSymbol));
        library->strings = image_at(library, strtab, strsz);
        library->strings_size = strsz;
        if (!library->symbols || !library->strings) return fail(loading, "its symbol table is malformed");
    }

    if (!relocate(loading, rel, rel_size) || !relocate(loading, jmprel, jmprel_size)) return 0;
    if (loading->missing_count) {
        snprintf(loading->error, loading->error_size, "it uses %s, which %s not available to mods",
                 loading->missing, loading->missing_count > 1 ? "are" : "is");
        return 0;
    }

    /* Each page gets the union of what the segments on it ask for. */
    page_flags = calloc(library->span / PAGE, 1);
    if (!page_flags) return fail(loading, "no memory for it");
    for (i = 0; i < header->phnum; i++) {
        const ElfSegment *segment = &segments[i];
        uint32_t page, first, last;
        if (segment->type != PT_LOAD || !segment->memsz) continue;
        first = (segment->vaddr - low) / PAGE;
        last = (segment->vaddr - low + segment->memsz - 1) / PAGE;
        for (page = first; page <= last; page++) page_flags[page] |= (uint8_t)(segment->flags & 7);
    }
    for (i = 0; i < library->span / PAGE;) {
        uint32_t run = i;
        while (run < library->span / PAGE && page_flags[run] == page_flags[i]) run++;
        if (!pages_protect(library->memory + i * PAGE, (run - i) * PAGE, page_flags[i])) {
            free(page_flags);
            return fail(loading, "its pages could not be protected");
        }
        i = run;
    }
    free(page_flags);

    /* Constructors, as the system loader would run them. */
    if (init) {
        void (*function)(void) = (void (*)(void))(library->bias + init);
        function();
    }
    if (init_array && init_array_size) {
        const uint32_t *entries = image_at(library, init_array, init_array_size);
        if (!entries) return fail(loading, "its constructors are outside the image");
        for (i = 0; i < init_array_size / 4; i++) {
            if (entries[i] && entries[i] != UINT32_MAX) ((void (*)(void))(uintptr_t)entries[i])();
        }
    }
    return 1;
}

void *ModLoad_Open(const char *path, char *error, size_t error_size)
{
    Loading loading;
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *bytes;
    ModLibc_Start();
    memset(&loading, 0, sizeof(loading));
    loading.error = error;
    loading.error_size = error_size;
    if (!file) {
        snprintf(error, error_size, "cannot open %s", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || (unsigned long)size > FILE_MAX ||
        fseek(file, 0, SEEK_SET)) {
        fclose(file);
        snprintf(error, error_size, "cannot read %s", path);
        return NULL;
    }
    bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        snprintf(error, error_size, "cannot read %s", path);
        return NULL;
    }
    fclose(file);
    loading.file = bytes;
    loading.file_size = (uint32_t)size;
    loading.library = calloc(1, sizeof(Library));
    if (!loading.library || !load(&loading)) {
        if (loading.library && loading.library->memory) pages_free(loading.library->memory, loading.library->span);
        free(loading.library);
        free(bytes);
        if (!loading.library) snprintf(error, error_size, "no memory for it");
        return NULL;
    }
    /* The tables the symbol lookup reads are inside the loaded image; the
     * file itself is no longer needed. */
    free(bytes);
    return loading.library;
}

void *ModLoad_Symbol(void *handle, const char *name)
{
    const Library *library = handle;
    uint32_t i;
    for (i = 1; library && i < library->symbol_count; i++) {
        const ElfSymbol *symbol = &library->symbols[i];
        const char *own = symbol_name(library, symbol);
        if (symbol->shndx != SHN_UNDEF && (symbol->info >> 4) != 0 && own && !strcmp(own, name))
            return (void *)(library->bias + symbol->value);
    }
    return NULL;
}
