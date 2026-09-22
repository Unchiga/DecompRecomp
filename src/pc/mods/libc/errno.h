#ifndef MEMORIES_MOD_ERRNO_H
#define MEMORIES_MOD_ERRNO_H
/* A mod's <errno.h> (README.md), with the numbers both platforms share. */
int *__errno_location(void);
#define errno (*__errno_location())

#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EINVAL 22
#define ERANGE 34
#endif
