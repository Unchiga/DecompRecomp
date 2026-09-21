/* Asset/window-independent SDK feasibility checks, not hardware equivalence. */
#include <psyz.h>
#include <libgpu.h>
#include <libgte.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)

int main(void)
{
    OT_TYPE ot[2];
    POLY_F3 triangle = {0};
    /* NCLIP of (0,0), (10,0), (0,10): twice the signed area is 100. */
    Psyz_GteDataWrite(12, 0);
    Psyz_GteDataWrite(13, 10);
    Psyz_GteDataWrite(14, 10u << 16);
    Psyz_GteCommand(0x06);
    CHECK((int32_t)Psyz_GteDataRead(24) == 100);
    Psyz_GteDataWrite(13, 10u << 16);
    Psyz_GteDataWrite(14, 10);
    Psyz_GteCommand(0x06);
    CHECK((int32_t)Psyz_GteDataRead(24) == -100);

    /* Installs the host GPU callback table before the OTC entrypoint is used. */
    ResetGraph(0);
    ClearOTagR(ot, 2);
    setPolyF3(&triangle);
    setRGB0(&triangle, 255, 0, 0);
    addPrim(&ot[1], &triangle);
    CHECK((uintptr_t)getaddr(&ot[1]) == (uintptr_t)&triangle);
    CHECK((uintptr_t)getaddr(&triangle) == (uintptr_t)&ot[0]);
    CHECK(getlen(&triangle) == 4);
    CHECK(sizeof(OT_TYPE) >= sizeof(void *));
    printf("PSY-Z probe: GTE winding and native packet links passed (%zu-bit host)\n",
           sizeof(void *) * 8);
    return 0;
}
