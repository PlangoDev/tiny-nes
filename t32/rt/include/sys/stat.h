#pragma once
#include <sys/types.h>
struct stat { long st_size; unsigned st_mode; };
int stat(const char* path, struct stat* st);
int mkdir(const char* path, mode_t mode);
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
