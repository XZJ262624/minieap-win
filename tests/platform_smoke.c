/* Offline checks only: no capture handles, packets or authentication. */
#include "minieap_common.h"
#include "config.h"
#include "misc.h"
#include "logging.h"
#include "pid_lock.h"
#include "sched_alarm.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PROG_CONFIG test_config;
static int failures;

/* pid_lock.c needs only this part of the application configuration. */
PROG_CONFIG* get_program_config(void) { return &test_config; }

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); ++failures; } \
} while (0)

static void test_strings(void) {
    char gbk[] = { (char)0xd6, (char)0xd0, (char)0xce, (char)0xc4 };
    const char expected[] = "\xe4\xb8\xad\xe6\x96\x87";
    char utf8[32];
    char bounded[2] = { 'x', 'x' };
    const char raw[3] = { 'a', 'b', 'c' };
    char* copy = strndup(raw, 2);
    gbk2utf8(gbk, sizeof(gbk), utf8, sizeof(utf8));
    CHECK(strcmp(utf8, expected) == 0, "GBK converts to UTF-8");
    gbk2utf8(gbk, sizeof(gbk), bounded, 1);
    CHECK(bounded[0] == 0 && bounded[1] == 'x', "GBK output respects capacity");
    CHECK(copy != NULL && strcmp(copy, "ab") == 0, "strndup bounds and terminates");
    free(copy);
}

static void test_getopt_passes(void) {
    char* args[] = { "minieap", "--nic", "\\Device\\NPF_{TEST}",
                     "--module", "rjv3", "--service", "students", NULL };
    const struct option main_options[] = {
        { "nic", required_argument, NULL, 'n' },
        { "module", required_argument, NULL, 'm' },
        { NULL, 0, NULL, 0 }
    };
    const struct option plugin_options[] = {
        { "service", required_argument, NULL, 's' },
        { NULL, 0, NULL, 0 }
    };
    int opt, index, nic_found = 0, module_found = 0, service_found = 0;
    optind = 0;
    opterr = 0;
    while ((opt = getopt_long(7, args, "-:n:m:", main_options, &index)) != -1) {
        if (opt == 'n') nic_found = strcmp(optarg, "\\Device\\NPF_{TEST}") == 0;
        if (opt == 'm') module_found = strcmp(optarg, "rjv3") == 0;
    }
    optind = 0;
    while ((opt = getopt_long(7, args, "-:s:", plugin_options, &index)) != -1) {
        if (opt == 's') service_found = strcmp(optarg, "students") == 0;
    }
    CHECK(nic_found && module_found && service_found, "ANSI getopt resets between application and plugin passes");
}

typedef struct _timer_context {
    int first;
    int nested;
    int unexpected;
    int cancelled_id;
    DWORD owner_thread;
} TIMER_CONTEXT;

static void timer_unexpected(void* user) {
    ++((TIMER_CONTEXT*)user)->unexpected;
}

static void timer_nested(void* user) {
    TIMER_CONTEXT* context = (TIMER_CONTEXT*)user;
    CHECK(GetCurrentThreadId() == context->owner_thread, "nested timer runs on caller thread");
    ++context->nested;
}

static void timer_first(void* user) {
    TIMER_CONTEXT* context = (TIMER_CONTEXT*)user;
    CHECK(GetCurrentThreadId() == context->owner_thread, "timer runs on caller thread");
    ++context->first;
    unschedule_alarm(context->cancelled_id);
    CHECK(schedule_alarm(1, timer_nested, context) > 0, "timer callback can schedule another timer");
}

static void test_timers(void) {
    TIMER_CONTEXT context = { 0 };
    int cancelled;
    context.owner_thread = GetCurrentThreadId();
    CHECK(sched_alarm_init() == SUCCESS, "scheduler initializes");
    cancelled = schedule_alarm(1, timer_unexpected, &context);
    CHECK(cancelled > 0, "timer scheduling returns an ID");
    unschedule_alarm(cancelled);
    context.cancelled_id = schedule_alarm(2, timer_unexpected, &context);
    CHECK(schedule_alarm(1, timer_first, &context) > 0, "first callback schedules");
    sched_alarm_poll();
    CHECK(context.first == 0, "timer cannot run before its deadline");
    Sleep(1100);
    sched_alarm_poll();
    CHECK(context.first == 1 && context.nested == 0 && context.unexpected == 0,
          "expired timer runs once and cancellation is respected");
    Sleep(1100);
    sched_alarm_poll();
    CHECK(context.first == 1 && context.nested == 1 && context.unexpected == 0,
          "callback can mutate the timer queue safely");
    CHECK(schedule_alarm(1, timer_unexpected, &context) > 0, "cleanup timer schedules");
    sched_alarm_destroy();
    sched_alarm_poll();
    CHECK(context.unexpected == 0, "destroy removes queued timers");
}

static void test_pid_lock(const char* output_dir) {
    char path[MAX_PATH];
    HANDLE other;
    DWORD error;
    CHECK(snprintf(path, sizeof(path), "%s\\platform-%lu.pid", output_dir,
                   GetCurrentProcessId()) < (int)sizeof(path), "PID test path fits");
    test_config.kill_type = KILL_NONE;
    CHECK(pid_lock_init(path) == SUCCESS, "PID path initializes");
    /* Another open handle exercises the same sharing rules as another process. */
    other = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(other != INVALID_HANDLE_VALUE, "independent PID owner opens");
    if (other != INVALID_HANDLE_VALUE) {
        CHECK(pid_lock_lock() == FAILURE, "PID lock refuses an existing owner");
        CloseHandle(other);
    }
    CHECK(pid_lock_lock() == SUCCESS, "PID lock succeeds after owner closes");
    CHECK(pid_lock_save_pid() == SUCCESS, "PID is written and flushed");
    other = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(other == INVALID_HANDLE_VALUE, "held PID lock excludes another handle");
    if (other != INVALID_HANDLE_VALUE) CloseHandle(other);
    CHECK(pid_lock_destroy() == SUCCESS, "PID lock closes");
    error = GetFileAttributesA(path);
    CHECK(error == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
          "PID file deletes atomically on close");
    CHECK(pid_lock_init("none") == SUCCESS && pid_lock_lock() == SUCCESS &&
          pid_lock_save_pid() == SUCCESS, "disabled PID lock is valid");
    pid_lock_destroy();
    test_config.kill_type = KILL_ONLY;
    CHECK(pid_lock_init("none") == FAILURE, "Windows rejects POSIX kill mode");
    test_config.kill_type = KILL_NONE;
}

int main(int argc, char** argv) {
    start_log();
    if (argc != 2) {
        fprintf(stderr, "Usage: platform_smoke.exe OUTPUT_DIRECTORY\n");
        return 2;
    }
    test_strings();
    test_getopt_passes();
    test_timers();
    test_pid_lock(argv[1]);
    close_log();
    if (failures) {
        fprintf(stderr, "%d platform check(s) failed\n", failures);
        return 1;
    }
    puts("PASS: string conversion, getopt passes, timer queue, PID locking (offline)");
    return 0;
}
