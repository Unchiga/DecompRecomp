/* A constructor: code the loader would have to run before the mod's init.
 * It reaches the host, so the compiler cannot work it out ahead of time. */
extern int host_add(int a, int b);
static int ready;
__attribute__((constructor)) static void start(void) { ready = host_add(1, 2); }
int run(void) { return ready; }
