#include "pc/compat/pgxp.h"
#include "pc/compat/gte.h"
#include <assert.h>

int main(void)
{
    uint32_t packet[] = {0x04ffffff, 0x20000000, 0x0014000a, 0x00140014, 0x001e000a};
    uint32_t address = ((uint32_t)(uintptr_t)packet & 0x00ffffffu) + 8;
    float a[] = {10.25f, 20.5f, 100.0f}, b[] = {10.75f, 20.5f, 120.0f};
    float x, y, w;
    Pgxp_Active = 1;
    Pgxp_Project(packet[2], a[0], a[1], a[2]);
    Pgxp_Project(packet[2], b[0], b[1], b[2]);
    assert(!Pgxp_Find(packet[2], &x, &y, &w)); /* ambiguous by value */
    Pgxp_Stored(packet[2], a);
    Pgxp_AddPrim(packet);
    assert(Pgxp_FindAt(address, packet[2], &x, &y, &w) == 1 && x == a[0]);

    /* The same packet buffer is reused for a primitive without GTE stores.
     * An unchanged rounded word must not inherit the previous projection. */
    Pgxp_AddPrim(packet);
    assert(Pgxp_FindAt(address, packet[2], &x, &y, &w) == 0);
    Pgxp_Stored(packet[2], a);
    Pgxp_Stored(packet[2], b);
    Pgxp_AddPrim(packet);
    assert(Pgxp_FindAt(address, packet[2], &x, &y, &w) == -1);
    Pgxp_Stored(packet[2], a);
    Pgxp_AddPrim(packet);
    Pgxp_NextFrame();
    assert(Pgxp_FindAt(address, packet[2], &x, &y, &w) == 1);
    Pgxp_NextFrame();
    assert(Pgxp_FindAt(address, packet[2], &x, &y, &w) == 0);
    return 0;
}
