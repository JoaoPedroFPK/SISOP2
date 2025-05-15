#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <stdio.h>
#include "packet.h"
#include "socket_utils.h"

#define DEBUG_PRINTF(...) \
    fprintf(stderr, "[DEBUG] %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__);

#endif
