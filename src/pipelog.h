#ifndef PIPELOG_H
#define PIPELOG_H
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIPELOG_VERSION_MAJOR 0
#define PIPELOG_VERSION_MINOR 9
#define PIPELOG_VERSION_PATCH 0

struct Pipelog_Output {
    const char *filename;
    const char *link;
    int fd;
};

enum {
    PIPELOG_NONE                =  0,
    PIPELOG_QUIET               =  1,
    PIPELOG_EXIT_ON_WRITE_ERROR =  2,
    PIPELOG_NO_SPLICE           =  4,
    // internal use only:
    PIPELOG_FORCE_ROTATE        =  8,
    PIPELOG_BLOCK_SIGHUP        = 16,
    PIPELOG_SPLICE              = 32,
};

int pipelog(int fd, const struct Pipelog_Output output[], size_t count, const char *pidfile, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif
