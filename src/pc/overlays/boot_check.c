/* The boot package's 0x80168000 module is the console-modification check
 * ("SOFTWARE TERMINATED ..."; see notes/boot-frontend-sequence.md). It reads
 * the BIOS region byte and drives the CD mechanism directly, neither of which
 * exists here, and it has no recovered source. The native build passes it. */
void func_801680F4(void)
{
}

int func_80168160(int step)
{
    (void)step;
    return 0; /* finished; Main_RunBootSequence continues to DsInit */
}
