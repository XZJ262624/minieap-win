/* Offline integration test: real libpcap backend + Windows scheduler, fake pcap.
 * This executable does not link wpcap.lib and cannot send a network packet. */
#include "win32_compat.h"
#include "if_impl.h"
#include "sched_alarm.h"
#include "logging.h"
#include <pcap.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

IF_IMPL* libpcap_new(void);

enum { EMPTY_CAPTURE, FRAME_CAPTURE, ERROR_CAPTURE };
enum { NO_FAILURE, OPEN_FAILURE, LINK_FAILURE, COMPILE_FAILURE,
       FILTER_FAILURE, NONBLOCK_FAILURE, MINCOPY_FAILURE };
static char fake_handle;
static HANDLE capture_event;
static volatile LONG console_stop;
static int mode, failure, dispatch_calls, close_calls, freecode_calls;
static int send_calls, send_result, delivered, timer_calls, break_calls;
static DWORD callback_thread, main_thread;
static char opened_device[256];
static char compiled_filter[128];
static uint8_t sent_data[64];
static int sent_length;
static const char device_name[] = "\\Device\\NPF_{00000000-0000-0000-0000-000000000001}";

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

int win32_stop_requested(void) {
    return InterlockedCompareExchange(&console_stop, 0, 0) != 0;
}

pcap_t* pcap_open_live(const char* device, int snaplen, int promisc,
                     int timeout, char* error) {
    CHECK(snaplen == FRAME_BUF_SIZE && promisc == 0 && timeout > 0);
    strcpy(opened_device, device);
    if (failure == OPEN_FAILURE) {
        strcpy(error, "injected open failure");
        return NULL;
    }
    return (pcap_t*)&fake_handle;
}

int pcap_datalink(pcap_t* handle) {
    CHECK(handle == (pcap_t*)&fake_handle);
    return failure == LINK_FAILURE ? DLT_NULL : DLT_EN10MB;
}

int pcap_compile(pcap_t* handle, struct bpf_program* program,
                 const char* filter, int optimize, bpf_u_int32 netmask) {
    CHECK(handle == (pcap_t*)&fake_handle);
    CHECK(optimize == 1 && netmask == PCAP_NETMASK_UNKNOWN);
    strcpy(compiled_filter, filter);
    memset(program, 0, sizeof(*program));
    return failure == COMPILE_FAILURE ? -1 : 0;
}

int pcap_setfilter(pcap_t* handle, struct bpf_program* program) {
    (void)program;
    CHECK(handle == (pcap_t*)&fake_handle);
    return failure == FILTER_FAILURE ? -1 : 0;
}

void pcap_freecode(struct bpf_program* program) {
    (void)program;
    ++freecode_calls;
}

int pcap_setnonblock(pcap_t* handle, int enabled, char* error) {
    CHECK(handle == (pcap_t*)&fake_handle && enabled == 1);
    strcpy(error, "injected nonblock failure");
    return failure == NONBLOCK_FAILURE ? -1 : 0;
}

int pcap_setmintocopy(pcap_t* handle, int bytes) {
    CHECK(handle == (pcap_t*)&fake_handle && bytes == 1);
    return failure == MINCOPY_FAILURE ? -1 : 0;
}

HANDLE pcap_getevent(pcap_t* handle) {
    CHECK(handle == (pcap_t*)&fake_handle);
    return capture_event;
}

int pcap_dispatch(pcap_t* handle, int count, pcap_handler handler, u_char* user) {
    CHECK(handle == (pcap_t*)&fake_handle && count > 0);
    ++dispatch_calls;
    if (mode == ERROR_CAPTURE) return PCAP_ERROR;
    if (mode == FRAME_CAPTURE) {
        struct pcap_pkthdr header = {0};
        uint8_t packet[60] = {0x12};
        if (dispatch_calls > 1) return PCAP_ERROR_BREAK;
        /* Too short for the Ethernet/EAPOL header. */
        header.caplen = header.len = 17;
        handler(user, &header, packet);
        /* Truncated by the capture snapshot: must not reach the state machine. */
        header.caplen = 18;
        header.len = 60;
        handler(user, &header, packet);
        header.caplen = header.len = sizeof(packet);
        handler(user, &header, packet);
        return 3;
    }
    return 0;
}

void pcap_breakloop(pcap_t* handle) {
    CHECK(handle == (pcap_t*)&fake_handle);
    ++break_calls;
}

char* pcap_geterr(pcap_t* handle) {
    CHECK(handle == (pcap_t*)&fake_handle);
    return "injected pcap failure";
}

void pcap_close(pcap_t* handle) {
    CHECK(handle == (pcap_t*)&fake_handle);
    ++close_calls;
}

int pcap_sendpacket(pcap_t* handle, const u_char* data, int length) {
    CHECK(handle == (pcap_t*)&fake_handle);
    CHECK(length >= 0 && length <= sizeof(sent_data));
    ++send_calls;
    sent_length = length;
    memcpy(sent_data, data, (size_t)length);
    return send_result;
}

static void receive_frame(ETH_EAP_FRAME* frame) {
    CHECK(frame->actual_len == 60 && frame->buffer_len == 60);
    CHECK(frame->content[0] == 0x12);
    callback_thread = GetCurrentThreadId();
    ++delivered;
}

static void reset_test(void) {
    sched_alarm_destroy();
    CHECK(sched_alarm_init() == SUCCESS);
    InterlockedExchange(&console_stop, 0);
    mode = EMPTY_CAPTURE;
    failure = NO_FAILURE;
    dispatch_calls = close_calls = freecode_calls = 0;
    send_calls = send_result = delivered = timer_calls = break_calls = 0;
    callback_thread = 0;
    if (capture_event) ResetEvent(capture_event);
}

static IF_IMPL* new_capture(void) {
    IF_IMPL* impl = libpcap_new();
    CHECK(impl != NULL);
    CHECK(impl->set_ifname(impl, device_name) == SUCCESS);
    CHECK(impl->setup_capture_params(impl, 0x888e, 0) == SUCCESS);
    impl->set_frame_handler(impl, receive_frame);
    return impl;
}

static void stop_from_timer(void* user) {
    IF_IMPL* impl = (IF_IMPL*)user;
    callback_thread = GetCurrentThreadId();
    ++timer_calls;
    CHECK(impl->stop_capture(impl) == SUCCESS);
}

static DWORD WINAPI simulate_console_stop(void* unused) {
    (void)unused;
    Sleep(120);
    InterlockedExchange(&console_stop, 1);
    return 0;
}

static void test_idle_timer(void) {
    IF_IMPL* impl;
    ULONGLONG start, elapsed;
    reset_test();
    impl = new_capture();
    CHECK(impl->prepare_interface(impl) == SUCCESS);
    CHECK(strcmp(opened_device, device_name) == 0);
    CHECK(strcmp(compiled_filter, "ether proto 0x888e") == 0);
    CHECK(freecode_calls == 1);
    CHECK(schedule_alarm(1, stop_from_timer, impl) > 0);
    start = GetTickCount64();
    CHECK(impl->start_capture(impl) == SUCCESS);
    elapsed = GetTickCount64() - start;
    CHECK(timer_calls == 1 && delivered == 0 && break_calls == 1);
    CHECK(callback_thread == main_thread);
    CHECK(elapsed >= 900 && elapsed < 5000);
    CHECK(dispatch_calls > 0 && dispatch_calls < 100);
    CHECK(send_calls == 0);
    impl->destroy(impl);
    CHECK(close_calls == 1);
    puts("PASS: idle capture runs the one-second timer on the capture thread; timer stop exits.");
}

static void test_console_stop(void) {
    IF_IMPL* impl;
    HANDLE thread;
    ULONGLONG start;
    reset_test();
    impl = new_capture();
    CHECK(impl->prepare_interface(impl) == SUCCESS);
    thread = CreateThread(NULL, 0, simulate_console_stop, NULL, 0, NULL);
    CHECK(thread != NULL);
    start = GetTickCount64();
    CHECK(impl->start_capture(impl) == SUCCESS);
    CHECK(GetTickCount64() - start < 2000);
    CHECK(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    CHECK(timer_calls == 0 && delivered == 0 && send_calls == 0);
    impl->destroy(impl);
    puts("PASS: the simulated console stop flag exits idle capture without incoming traffic.");
}

static void test_receive_and_errors(void) {
    IF_IMPL* impl;
    reset_test();
    impl = new_capture();
    CHECK(impl->prepare_interface(impl) == SUCCESS);
    mode = FRAME_CAPTURE;
    CHECK(impl->start_capture(impl) == SUCCESS);
    CHECK(delivered == 1 && dispatch_calls == 2 && callback_thread == main_thread);
    mode = ERROR_CAPTURE;
    CHECK(impl->start_capture(impl) == FAILURE);
    impl->destroy(impl);
    puts("PASS: short/truncated captures are discarded; complete frames arrive; capture errors propagate.");
}

static void test_sending_and_names(void) {
    IF_IMPL* impl;
    char copied[256], overlong[257];
    uint8_t bytes[60] = {0x33, 0x44};
    ETH_EAP_FRAME frame;
    reset_test();
    impl = new_capture();
    CHECK(impl->get_ifname(impl, copied, (int)strlen(device_name)) == FAILURE);
    CHECK(impl->get_ifname(impl, copied, (int)strlen(device_name) + 1) == SUCCESS);
    CHECK(strcmp(copied, device_name) == 0);
    memset(overlong, 'x', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = 0;
    CHECK(impl->set_ifname(impl, overlong) == FAILURE);
    CHECK(impl->prepare_interface(impl) == SUCCESS);
    frame.content = bytes;
    frame.buffer_len = frame.actual_len = sizeof(bytes);
    CHECK(impl->send_frame(impl, &frame) == SUCCESS);
    CHECK(send_calls == 1 && sent_length == sizeof(bytes));
    CHECK(memcmp(sent_data, bytes, sizeof(bytes)) == 0);
    frame.actual_len = sizeof(bytes) + 1;
    CHECK(impl->send_frame(impl, &frame) == FAILURE && send_calls == 1);
    frame.actual_len = frame.buffer_len = (size_t)INT_MAX + 1;
    CHECK(impl->send_frame(impl, &frame) == FAILURE && send_calls == 1);
    frame.actual_len = frame.buffer_len = sizeof(bytes);
    send_result = -1;
    CHECK(impl->send_frame(impl, &frame) == FAILURE && send_calls == 2);
    impl->destroy(impl);
    puts("PASS: complete NPF names round-trip; send lengths are checked; pcap send failures propagate.");
}

static void test_prepare_failures(void) {
    int injected;
    for (injected = OPEN_FAILURE; injected <= MINCOPY_FAILURE; ++injected) {
        IF_IMPL* impl;
        reset_test();
        impl = new_capture();
        failure = injected;
        CHECK(impl->prepare_interface(impl) == FAILURE);
        CHECK(close_calls == (injected == OPEN_FAILURE ? 0 : 1));
        CHECK(freecode_calls == (injected >= FILTER_FAILURE ? 1 : 0));
        CHECK(impl->start_capture(impl) == FAILURE);
        impl->destroy(impl);
        CHECK(close_calls == (injected == OPEN_FAILURE ? 0 : 1));
    }
    puts("PASS: open/datalink/filter/nonblocking failures release handles and compiled filters exactly once.");
}

int main(void) {
    main_thread = GetCurrentThreadId();
    capture_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(capture_event != NULL);
    start_log();
    test_idle_timer();
    test_console_stop();
    test_receive_and_errors();
    test_sending_and_names();
    test_prepare_failures();
    sched_alarm_destroy();
    CloseHandle(capture_event);
    close_log();
    puts("PASS: offline capture integration completed; no Npcap runtime or network frames used.");
    return 0;
}
