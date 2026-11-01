#ifndef MINIEAP_WIN32_COMPAT_H
#define MINIEAP_WIN32_COMPAT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stddef.h>
#include <string.h>

#define strdup _strdup
#define strndup win32_strndup
char* win32_strndup(const char* text, size_t limit);
int win32_platform_init(void);
int win32_load_npcap(void);
void win32_platform_cleanup(void);
int win32_stop_requested(void);

#endif
