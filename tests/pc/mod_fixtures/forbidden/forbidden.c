/* A mod library for tests/pc/modload_test.c that reaches for what mods are
 * not given: a file by its own path, deleting one, and the network. It
 * declares them itself, since the mod headers do not; the loader must refuse
 * it and name all three. */
typedef struct MemoriesModFile FILE;
FILE *fopen(const char *path, const char *mode);
int remove(const char *path);
int socket(int domain, int type, int protocol);

int fixture_forbidden(void)
{
    FILE *file = fopen("anything", "w");
    return remove("anything") + socket(2, 1, 0) + (file != 0);
}
