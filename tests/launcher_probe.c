/* Offline launcher fixture. Uses the production console handler and PID lock;
 * deliberately has no packet, authentication or Npcap code. */
#include "win32_compat.h"
#include "config.h"
#include "pid_lock.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>

static PROG_CONFIG config;
PROG_CONFIG* get_program_config(void) { return &config; }

static void marker(const char* conf, const char* suffix, const char* content) {
    char path[2048];
    FILE* file;
    snprintf(path, sizeof(path), "%s.%s", conf, suffix);
    file = fopen(path, "w");
    if (!file) exit(90);
    fputs(content, file);
    fclose(file);
}

int main(int argc, char** argv) {
    char mode[128], cwd[2048], record[256], stop_path[2048];
    DWORD flags;
    UINT initial_cp = GetConsoleOutputCP();
    FILE* conf;
    int tick, result = 0, stopped;
    if (argc != 5 || strcmp(argv[1], "--conf-file") || strcmp(argv[3], "--pid-file")) return 91;
    conf = fopen(argv[2], "r");
    if (!conf) return 92;
    if (!fgets(mode, sizeof(mode), conf)) { fclose(conf); return 93; }
    fclose(conf);
    if (strncmp(mode, "SCNUNET-LAUNCHER-TEST:", 22)) return 94;
    if (win32_platform_init() != 0) return 95;
    start_log();
    if (IS_FAIL(pid_lock_init(argv[4])) || IS_FAIL(pid_lock_lock()) || IS_FAIL(pid_lock_save_pid())) return 96;
    setvbuf(stdout, NULL, _IONBF, 0);
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    printf("PROBE cwd=%s\n", cwd);
    fprintf(stderr, "PROBE stderr\n");
    puts("PROBE 探针认证成功");
    snprintf(record, sizeof(record), "stdin=%d stdout=%d stderr=%d cp=%u\n",
        !!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &flags),
        !!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &flags),
        !!GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &flags), GetConsoleOutputCP());
    marker(argv[2], "ready", record);
    snprintf(stop_path, sizeof(stop_path), "%s.stop", argv[2]);
    for (tick = 0; tick < (strstr(mode, "hold") ? 1200 : 3); ++tick) {
        if (win32_stop_requested() || GetFileAttributesA(stop_path) != INVALID_FILE_ATTRIBUTES) break;
        printf("PROBE Keep-Alive %d\n", tick);
        Sleep(100);
    }
    stopped = win32_stop_requested();
    if (strstr(mode, "abnormal")) ExitProcess(0xc0000409u); /* exit propagation, not a fake diagnosis */
    if (strstr(mode, "nonzero")) result = 7;
    pid_lock_destroy();
    close_log();
    win32_platform_cleanup();
    snprintf(record, sizeof(record), "stop=%d cp_restored=%d\n", stopped, GetConsoleOutputCP() == initial_cp);
    marker(argv[2], "stopped", record);
    printf("PROBE cleaned stop=%d\n", stopped);
    return result;
}
