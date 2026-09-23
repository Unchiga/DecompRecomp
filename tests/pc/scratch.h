#ifndef MEMORIES_TESTS_SCRATCH_H
#define MEMORIES_TESTS_SCRATCH_H
#include <stdio.h>
#include <stdlib.h>

/* A mkstemp/mkdtemp template for a test's scratch files, in the system's
 * temporary folder: TMPDIR, else TEMP or TMP (Windows), else /tmp. A plain
 * "/tmp" is \tmp on the current drive on Windows, which a fresh machine or a
 * CI runner does not have (Wine maps it, which hid this). */
#define SCRATCH_MAX 512

static void scratch_template(char *out, size_t size, const char *name)
{
    const char *names[] = {"TMPDIR", "TEMP", "TMP"}, *base = NULL;
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]) && (!base || !*base); i++) base = getenv(names[i]);
    if (!base || !*base) base = "/tmp";
    snprintf(out, size, "%s/%s-XXXXXX", base, name);
}
#endif
