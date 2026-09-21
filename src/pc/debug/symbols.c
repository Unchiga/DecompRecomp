#include "symbols.h"
#include "pc/guest/state.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct RuntimeSymbol {
    uintptr_t address, size;
    char name[72];
} RuntimeSymbol;

static RuntimeSymbol *symbols;
static size_t symbol_count;

int Symbols_Load(void)
{
    char path[640];
    FILE *file;
    size_t room = 0;
    if (Memories_SymbolTablePath(path, sizeof(path))) return -1;
    file = fopen(path, "r");
    if (!file) return -1;
    while (!feof(file)) {
        RuntimeSymbol symbol;
        unsigned address, size;
        if (fscanf(file, "%x %x %71s", &address, &size, symbol.name) != 3) break;
        symbol.address = address;
        symbol.size = size;
        if (symbol_count == room) {
            RuntimeSymbol *grown;
            room = room ? room * 2 : 2048;
            grown = realloc(symbols, room * sizeof(*symbols));
            if (!grown) {
                fclose(file);
                free(symbols);
                symbols = NULL;
                symbol_count = 0;
                return -1;
            }
            symbols = grown;
        }
        symbols[symbol_count++] = symbol;
    }
    fclose(file);
    return symbol_count ? 0 : -1;
}

const char *Symbols_Lookup(uintptr_t address, uintptr_t *offset)
{
    size_t low = 0, high = symbol_count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (symbols[middle].address <= address) low = middle + 1;
        else high = middle;
    }
    if (!low) return NULL;
    low--;
    if (address >= symbols[low].address + (symbols[low].size ? symbols[low].size : 1)) return NULL;
    if (offset) *offset = address - symbols[low].address;
    return symbols[low].name;
}
