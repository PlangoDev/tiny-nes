#pragma once
#include <stddef.h>
int access(const char* path, int mode);
int unlink(const char* path);
int usleep(unsigned us);
#define F_OK 0
#define R_OK 4
