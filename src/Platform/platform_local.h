#ifndef OFFS_PLATFORM_LOCAL_H
#define OFFS_PLATFORM_LOCAL_H

#include "platform_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

platform_socket_t* platform_local_listen(const char* path);
platform_socket_t* platform_local_accept(platform_socket_t* listener);
platform_socket_t* platform_local_connect(const char* path);
void platform_local_cleanup(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_LOCAL_H */
