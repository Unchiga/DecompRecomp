#define _POSIX_C_SOURCE 200809L
#include "pc/compat/posix.h"
#include "pc/platform/paths.h"
#include "pc/saves/save_slots.h"
#include "scratch.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* Accents, decomposed accents, Cyrillic, CJK, a non-BMP character, spaces
 * and legal Windows shell metacharacters. Keep the spelling exact on disk. */
#define NAME "Jos\u00e9-e\u0301-\u042f-\u6771\u4eac-\U0001f600 & (100%) ! ^"
#define LEAF "\u00e9-\u6771\u4eac.txt"
static int sound(unsigned char *state) { return state[SAVE_SLOT_STATE_SIZE - 1] == 0x5A; }

static void exact_file(const char *path)
{
#ifdef _WIN32
    /* Independent OS-level check, not a round trip through the adapter:
     * narrow fopen/mkdir can succeed while making a mojibake directory. */
    wchar_t wide[2048];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 2048));
    assert(GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES);
#else
    assert(!access(path, F_OK));
#endif
}

int main(int argc, char **argv)
{
    char root[SCRATCH_MAX], directory[1024], path[1200], other[1200], temp[1200];
    const char *kept;
    FILE *file;
    DIR *dir;
    struct dirent *entry;
    struct stat info;
    int fd, found = 0;
    unsigned char image[SAVE_SLOT_FILE_SIZE] = {0}, state[SAVE_SLOT_STATE_SIZE];
    SaveSlotInfo slots[SAVE_SLOT_COUNT];
#ifdef _WIN32
    argv = Memories_Argv(&argc);
    assert(argv);
#endif
    if (argc > 1) assert(!strcmp(argv[1], NAME));
    {
        char executable[2048];
        long bytes = readlink("/proc/self/exe", executable, sizeof(executable));
        assert(bytes > 0 && (size_t)bytes < sizeof(executable));
        executable[bytes] = 0;
        exact_file(executable);
    }
    scratch_template(root, sizeof(root), "memories-fs");
    assert(mkdtemp(root));
    snprintf(directory, sizeof(directory), "%s/%s", root, NAME);
    assert(!Paths_MakeDirs(directory));
    exact_file(directory);
    assert(!setenv("MEMORIES_USER_DIR", directory, 1));
    kept = getenv("MEMORIES_USER_DIR");
    assert(kept && !strcmp(kept, directory));
    assert(!setenv("MEMORIES_FS_OTHER", NAME, 1));
    assert(!strcmp(getenv("MEMORIES_FS_OTHER"), NAME));
    assert(!strcmp(kept, directory)); /* another getenv must not invalidate it */
    assert(!setenv("MEMORIES_FS_OTHER", "ignored", 0));
    assert(!strcmp(getenv("MEMORIES_FS_OTHER"), NAME));
    assert(!unsetenv("MEMORIES_FS_OTHER") && !getenv("MEMORIES_FS_OTHER"));

    snprintf(path, sizeof(path), "%s/%s", directory, LEAF);
    snprintf(other, sizeof(other), "%s/\u03bb.partial", directory);
    file = fopen(path, "wb"); assert(file); assert(fputs("old", file) >= 0); assert(!fclose(file));
    file = fopen(other, "wb"); assert(file); assert(fputs("new", file) >= 0); assert(!fclose(file));
    exact_file(path); exact_file(other);
    assert(!rename(other, path)); /* overwrite, as saves/settings/states do */
    assert(!stat(path, &info) && info.st_size == 3);
    fd = open(path, O_RDONLY); assert(fd >= 0);
    assert(read(fd, temp, 3) == 3 && !memcmp(temp, "new", 3)); assert(!close(fd));
    dir = opendir(directory); assert(dir);
    while ((entry = readdir(dir))) if (!strcmp(entry->d_name, LEAF)) found++;
    assert(!closedir(dir) && found == 1);

    snprintf(temp, sizeof(temp), "%s/\u03bb-XXXXXX", directory);
    fd = mkstemp(temp); assert(fd >= 0); assert(!close(fd)); exact_file(temp); assert(!unlink(temp));
    snprintf(temp, sizeof(temp), "%s/\u03bb-XXXXXX", directory);
    assert(mkdtemp(temp)); exact_file(temp); assert(!rmdir(temp));

    /* The real slot backend, including replacement and directory scans. */
    image[SAVE_SLOT_HEADER_SIZE + SAVE_SLOT_STATE_SIZE - 1] = 0x5A;
    image[SAVE_SLOT_DUPLICATE_OFFSET + SAVE_SLOT_STATE_SIZE - 1] = 0x5A;
    assert(!SaveSlots_WriteFile(0, image, sizeof(image)));
    image[SAVE_SLOT_HEADER_SIZE + 0x50] = 7;
    assert(!SaveSlots_WriteFile(0, image, sizeof(image)));
    assert(!SaveSlots_Path(0, other, sizeof(other))); exact_file(other);
    assert(!SaveSlots_ReadState(0, state, sound) && state[0x50] == 7);
    SaveSlots_Scan(slots, sound); assert(slots[0].status == SAVE_SLOT_USED && slots[0].cards == 7);
    assert(!remove(other));
    snprintf(other, sizeof(other), "%s/saves", directory); assert(!rmdir(other));
#ifdef _WIN32
    errno = 0;
    assert(!fopen("invalid-\xff", "wb") && errno == EILSEQ);
    assert(!Memories_Utf8ToWide("\xed\xa0\x80") && errno == EILSEQ); /* unpaired surrogate */
#endif
    assert(!remove(path)); assert(!rmdir(directory)); assert(!rmdir(root));
    puts("UTF-8 paths: ok");
    return 0;
}
