#include "win32_compat.h"
#include <stdio.h>
#include <stdlib.h>

static volatile LONG stop_requested;
static int winsock_started;
static UINT original_output_cp;
static HMODULE npcap_module;

char* win32_strndup(const char* text, size_t limit) {
    size_t len = strnlen(text, limit);
    char* copy = (char*)malloc(len + 1);
    if (copy) {
        memcpy(copy, text, len);
        copy[len] = '\0';
    }
    return copy;
}

static BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        InterlockedExchange(&stop_requested, 1);
        return TRUE;
    }
    return FALSE;
}

int win32_stop_requested(void) {
    return InterlockedCompareExchange(&stop_requested, 0, 0) != 0;
}

int win32_platform_init(void) {
    WSADATA data;
    int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", result);
        return -1;
    }
    winsock_started = 1;
    original_output_cp = GetConsoleOutputCP();
    if (original_output_cp) SetConsoleOutputCP(CP_UTF8);
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        fprintf(stderr, "SetConsoleCtrlHandler failed: %lu\n", GetLastError());
        win32_platform_cleanup();
        return -1;
    }
    return 0;
}

int win32_load_npcap(void) {
    wchar_t system_dir[MAX_PATH];
    wchar_t dll_path[MAX_PATH];
    UINT length;
    if (npcap_module) return 0;
    length = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (!length || length >= MAX_PATH) return -1;
    if (swprintf_s(dll_path, MAX_PATH, L"%ls\\Npcap\\wpcap.dll", system_dir) < 0)
        return -1;
    npcap_module = LoadLibraryExW(dll_path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!npcap_module) {
        /* Npcap's WinPcap-compatible installation uses System32. */
        npcap_module = LoadLibraryExW(L"wpcap.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!npcap_module) {
        fprintf(stderr, "Cannot load Npcap Runtime (Windows error %lu). Install Npcap x64.\n", GetLastError());
        return -1;
    }
    return 0;
}

void win32_platform_cleanup(void) {
    SetConsoleCtrlHandler(console_handler, FALSE);
    if (original_output_cp) SetConsoleOutputCP(original_output_cp);
    if (winsock_started) {
        WSACleanup();
        winsock_started = 0;
    }
    /* Keep the delay-loaded module alive until process teardown. */
}
