/* Exercise the actual save-state chunk reader and mod compatibility preflight. */
#include "../../src/pc/guest/state.c"
#include <assert.h>
static unsigned test_signature = 12345, test_version = 1;
static int state_registered = 1;
static int reordered;
static unsigned payload[2] = {7, 11};
static unsigned other_payload[2] = {13, 17};
unsigned Mods_Signature(void) { return test_signature; }
int Mods_Count(void) { return 3; }
int Mods_Active(int owner) { return owner == 0 || owner == 2; }
const char *Mods_Id(int owner) { return (owner == 0) != reordered ? "alpha" : "beta"; }
void Mods_VisitState(void (*visit)(int, void *, size_t, unsigned, void *), void *context)
{
    if (state_registered) {
        visit(reordered ? 2 : 0, payload, sizeof(payload), test_version, context);
        visit(reordered ? 0 : 2, other_payload, sizeof(other_payload), test_version, context);
    }
}
int main(int argc, char **argv)
{
    const char *path;
    assert(argc == 2);
    path = argv[1];
    MemoriesState state = {0, NULL, NULL, 0};
    unsigned signature = test_signature;
    MemoriesStateField fields = {&signature, sizeof(signature)};
    unsigned char image[256];
    size_t length;
    state.file = fopen(path, "wb");
    assert(state.file);
    assert(fwrite("YFMSTATE00000000", 1, 16, state.file) == 16);
    Memories_StateChunk(&state, "mod-set", &fields, 1);
    Mods_VisitState(mod_state_visit, &state);
    fclose(state.file);
    state.file = fopen(path, "rb");
    assert(state.file);
    length = fread(image, 1, sizeof(image), state.file);
    fclose(state.file);
    state.loading = 1;
    state.image = image;
    state.image_size = length;
    assert(compatible_mods(&state));
    payload[0] = 99;
    Mods_VisitState(mod_state_visit, &state);
    assert(payload[0] == 7);
    reordered = 1;
    payload[0] = other_payload[0] = 99;
    assert(compatible_mods(&state));
    Mods_VisitState(mod_state_visit, &state);
    assert(payload[0] == 7 && other_payload[0] == 13);
    test_signature++;
    payload[0] = 99;
    assert(!compatible_mods(&state));
    assert(payload[0] == 99);
    test_signature--;
    test_version++;
    assert(!compatible_mods(&state));
    test_version--;
    state.image_size = length - 1;
    assert(!compatible_mods(&state));
    state.image_size = 16;
    assert(!compatible_mods(&state));
    state_registered = 0;
    test_signature = 2166136261u;
    assert(compatible_mods(&state));
    remove(path);
    return 0;
}
