/* With the stack protector this reads a canary from Linux thread storage. */
#include <string.h>
int run(const char *text)
{
    char buffer[64];
    strncpy(buffer, text, sizeof(buffer));
    return buffer[0];
}
