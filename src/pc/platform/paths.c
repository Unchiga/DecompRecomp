/* The two roots the port reads and writes through (paths.h). Both are
 * resolved once and cached: the program directory from /proc/self/exe, the
 * user directory from the platform's own convention for a game's files. */
#define _POSIX_C_SOURCE 200809L
#include "paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <shlobj.h> /* SHGetFolderPathA; ahead of posix.h, which then skips its own declarations */
#endif
#include "pc/compat/posix.h" /* mkdir, and readlink of /proc/self/exe, on Windows */
#include <sys/stat.h>

#define PATH_MAX_ 1024
#define APP_NAME "YFM Re-Decomp"
#define OLD_APP_NAME "YFM ReDecomp" /* what builds before 2026-09-22 called it */

static char user_dir[PATH_MAX_];
static char program_dir[PATH_MAX_];

int Paths_MakeDirs(const char *path)
{
    char work[PATH_MAX_];
    size_t length = strlen(path), i;
    if (length >= sizeof(work)) return -1;
    memcpy(work, path, length + 1);
    while (length > 1 && work[length - 1] == '/') work[--length] = '\0';
    for (i = 1; i < length; i++) {
        if (work[i] != '/') continue;
        work[i] = '\0';
        if (mkdir(work, 0777) && access(work, X_OK)) return -1;
        work[i] = '/';
    }
    if (mkdir(work, 0777) && access(work, X_OK)) return -1;
    return 0;
}

/* Everything above the last separator of a path. */
static void directory_of(char *path)
{
    char *slash = strrchr(path, '/');
    if (slash) *slash = '\0';
    else path[0] = '\0';
}

const char *Paths_ProgramDir(void)
{
    ssize_t length;
    if (program_dir[0]) return program_dir;
    length = readlink("/proc/self/exe", program_dir, sizeof(program_dir) - 1);
    if (length > 0 && (size_t)length < sizeof(program_dir)) {
        program_dir[length] = '\0';
        directory_of(program_dir);
    }
    if (!program_dir[0]) snprintf(program_dir, sizeof(program_dir), ".");
    return program_dir;
}

const char *Paths_UserDir(void)
{
    const char *named = getenv("MEMORIES_USER_DIR");
    if (user_dir[0]) return user_dir;
    if (named && *named) {
        snprintf(user_dir, sizeof(user_dir), "%s", named);
    } else {
        char root[PATH_MAX_ - 32] = ""; /* room for the folder name after it */
#ifdef _WIN32
        /* Documents\My Games is where Windows games have put their files
         * since the Games for Windows era; the player can find and back it
         * up without being told where to look. Ask the shell where Documents
         * is, since OneDrive and the folder's Location tab both move it. */
        char documents[MAX_PATH];
        const char *profile = getenv("USERPROFILE");
        if (SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents) == S_OK)
            snprintf(root, sizeof(root), "%s/My Games", documents);
        else
            snprintf(root, sizeof(root), "%s/Documents/My Games", profile && *profile ? profile : ".");
#else
        const char *home = getenv("HOME"), *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg == '/') snprintf(root, sizeof(root), "%s", xdg);
        else if (home && *home) snprintf(root, sizeof(root), "%s/.local/share", home);
#endif
        if (root[0]) {
            char old[PATH_MAX_];
            snprintf(user_dir, sizeof(user_dir), "%s/" APP_NAME, root);
            snprintf(old, sizeof(old), "%s/" OLD_APP_NAME, root);
            /* Bring the old folder along under the new name, once. */
            if (access(user_dir, F_OK) && !access(old, F_OK) && !rename(old, user_dir))
                fprintf(stderr, "memories-pc: moved %s to %s\n", old, user_dir);
        } else {
            snprintf(user_dir, sizeof(user_dir), "saves");
        }
    }
    /* A root that cannot be made is not worth carrying: fall back beside the
     * game, which is where the port kept everything before. */
    if (Paths_MakeDirs(user_dir)) {
        fprintf(stderr, "memories-pc: cannot create %s; using ./saves\n", user_dir);
        snprintf(user_dir, sizeof(user_dir), "saves");
        Paths_MakeDirs(user_dir);
    }
    return user_dir;
}

static int join(char *out, size_t size, const char *root, const char *relative, int create)
{
    int n = snprintf(out, size, "%s/%s", root, relative);
    if (n < 0 || (size_t)n >= size) return -1;
    if (create) {
        char parent[PATH_MAX_];
        if ((size_t)n >= sizeof(parent)) return -1;
        memcpy(parent, out, (size_t)n + 1);
        directory_of(parent);
        if (parent[0] && Paths_MakeDirs(parent)) return -1;
    }
    return 0;
}

int Paths_User(char *out, size_t size, const char *relative)
{
    return join(out, size, Paths_UserDir(), relative, 1);
}

int Paths_Program(char *out, size_t size, const char *relative)
{
    return join(out, size, Paths_ProgramDir(), relative, 0);
}

int Paths_Contained(const char *relative)
{
    const char *at = relative;
    if (!relative || !*relative || *relative == '/' || *relative == '\\') return 0;
    if (relative[0] && relative[1] == ':') return 0; /* a drive letter */
    for (;;) {
        size_t length = strcspn(at, "/");
        if (strchr(at, '\\')) return 0;
        if (!length) return 0; /* an empty component: "a//b" or a trailing slash */
        if (length == 1 && at[0] == '.') return 0;
        if (length == 2 && at[0] == '.' && at[1] == '.') return 0;
        if (!at[length]) return 1;
        at += length + 1;
    }
}

static void carry(const char *from, const char *relative)
{
    char destination[PATH_MAX_];
    char buffer[65536];
    FILE *in, *out;
    size_t got;
    if (access(from, R_OK)) return;
    if (Paths_User(destination, sizeof(destination), relative)) return;
    if (!access(destination, F_OK)) return; /* already carried, or newer */
    in = fopen(from, "rb");
    if (!in) return;
    out = fopen(destination, "wb");
    if (!out) { fclose(in); return; }
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) break;
    }
    fclose(in);
    if (fclose(out)) remove(destination);
    else fprintf(stderr, "memories-pc: carried %s to %s\n", from, destination);
}

void Paths_MigrateLegacySaves(void)
{
    static const char *files[] = {"settings.txt", "controls.txt", "memcard1.mcd", "memcard2.mcd"};
    char legacy[PATH_MAX_];
    unsigned i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(legacy, sizeof(legacy), "saves/%s", files[i]);
        carry(legacy, files[i]);
    }
}
