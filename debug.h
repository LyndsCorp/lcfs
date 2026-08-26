#ifndef LCFS_DEBUG_H
#define LCFS_DEBUG_H

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>

#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...) \
fprintf(stderr, "[DEBUG] %s:%d:%s(): " fmt "\n", \
__FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define DEBUG_ERROR(fmt, ...) \
fprintf(stderr, "[ERROR] %s:%d:%s(): " fmt " (errno=%d: %s)\n", \
__FILE__, __LINE__, __func__, ##__VA_ARGS__, errno, strerror(errno))
#define DEBUG_ENTER() \
fprintf(stderr, "[DEBUG] --> %s()\n", __func__)
#define DEBUG_EXIT(ret) \
fprintf(stderr, "[DEBUG] <-- %s() returns %d\n", __func__, (ret))
#else
#define DEBUG_PRINT(fmt, ...) do {} while (0)
#define DEBUG_ERROR(fmt, ...) do {} while (0)
#define DEBUG_ENTER() do {} while (0)
#define DEBUG_EXIT(ret) do {} while (0)
#endif

#endif
