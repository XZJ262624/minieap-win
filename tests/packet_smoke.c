/* Offline integration test: the network driver and adapter metadata are fake.
 * The actual config, state machine, packet builder and RJv3 plugin are linked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "conf_parser.h"
#include "if_impl.h"
#include "logging.h"
#include "net_util.h"
#include "packet_builder.h"
#include "packet_plugin.h"
#include "packet_plugin_rjv3_priv.h"
#include "packet_plugin_rjv3_prop.h"
#include "eap_state_machine.h"
#include "sched_alarm.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

static const unsigned char local_mac[6] = {2, 0, 0, 0, 0, 1};
static const unsigned char server_mac[6] = {2, 0, 0, 0, 0, 2};
static unsigned char sent_bytes[FRAME_BUF_SIZE];
static size_t sent_length;
static int send_count;

static RESULT fake_ifname(IF_IMPL* self, char* out, int length) {
    (void)self;
    if (length < (int)sizeof("offline-test")) return FAILURE;
    strcpy(out, "offline-test");
    return SUCCESS;
}

static RESULT record_frame(IF_IMPL* self, ETH_EAP_FRAME* frame) {
    (void)self;
    CHECK(frame && frame->content && frame->actual_len <= sizeof(sent_bytes));
    CHECK(frame->actual_len <= frame->buffer_len);
    sent_length = frame->actual_len;
    memcpy(sent_bytes, frame->content, sent_length);
    ++send_count;
    return SUCCESS;
}

static void fake_destroy(IF_IMPL* self) { free(self); }

IF_IMPL* libpcap_new(void) {
    IF_IMPL* result = (IF_IMPL*)calloc(1, sizeof(*result));
    CHECK(result);
    result->name = "libpcap";
    result->description = "offline memory recorder; no network access";
    result->get_ifname = fake_ifname;
    result->send_frame = record_frame;
    result->destroy = fake_destroy;
    return result;
}

RESULT obtain_iface_mac(const char* name, uint8_t* mac) {
    CHECK(strcmp(name, "offline-test") == 0);
    memcpy(mac, local_mac, 6);
    return SUCCESS;
}

RESULT obtain_iface_ip_mask(const char* name, LIST_ELEMENT** list) {
    CHECK(strcmp(name, "offline-test") == 0);
    /* Deliberately no addresses: exercise pre-auth/no-DHCP IPv6 handling. */
    *list = NULL;
    return SUCCESS;
}

void free_ip_list(LIST_ELEMENT** list) { list_destroy(list, TRUE); }
void free_dns_list(LIST_ELEMENT** list) { list_destroy(list, TRUE); }
IP_ADDR* find_ip_with_family(LIST_ELEMENT* list, short family) {
    (void)list; (void)family;
    return NULL;
}
RESULT obtain_dns_list(LIST_ELEMENT** list) {
    insert_data(list, strdup("192.0.2.53"));
    insert_data(list, strdup("192.0.2.54"));
    return SUCCESS;
}
RESULT obtain_iface_ipv4_gateway(const char* name, uint8_t* out) {
    static const uint8_t gateway[4] = {192, 0, 2, 1};
    CHECK(strcmp(name, "offline-test") == 0);
    memcpy(out, gateway, sizeof(gateway));
    return SUCCESS;
}
RESULT print_adapter_list(void) { return FAILURE; }

static unsigned read_be16(const unsigned char* p) { return (p[0] << 8) | p[1]; }

static void make_request(unsigned char* bytes, size_t length, int id, int type) {
    memset(bytes, 0, length);
    memcpy(bytes, local_mac, 6);
    memcpy(bytes + 6, server_mac, 6);
    bytes[12] = 0x88; bytes[13] = 0x8e;
    bytes[14] = 1; bytes[15] = EAP_PACKET;
    bytes[17] = (unsigned char)(length - 18);
    bytes[18] = EAP_REQUEST; bytes[19] = (unsigned char)id;
    bytes[21] = (unsigned char)(length - 18); bytes[22] = (unsigned char)type;
}

static void feed(unsigned char* bytes, size_t length) {
    ETH_EAP_FRAME frame;
    frame.content = bytes; frame.actual_len = frame.buffer_len = length;
    eap_state_machine_recv_handler(&frame);
}

static void check_service(void) {
    static const unsigned char prefix[] = {0x1a, 0x28, 0, 0, 0x13, 0x11, 0x39, 0x22};
    unsigned char expected[32] = "students";
    size_t offset;
    int count = 0;
    for (offset = 18; offset + sizeof(prefix) + sizeof(expected) <= sent_length; ++offset) {
        if (memcmp(sent_bytes + offset, prefix, sizeof(prefix)) == 0) {
            CHECK(memcmp(sent_bytes + offset + sizeof(prefix), expected, sizeof(expected)) == 0);
            ++count;
        }
    }
    CHECK(count == 1);
}

static void check_response(int id, int type, unsigned length) {
    CHECK(memcmp(sent_bytes, server_mac, 6) == 0);
    CHECK(memcmp(sent_bytes + 6, local_mac, 6) == 0);
    CHECK(sent_bytes[12] == 0x88 && sent_bytes[13] == 0x8e);
    CHECK(sent_bytes[14] == 1 && sent_bytes[15] == EAP_PACKET);
    CHECK(read_be16(sent_bytes + 16) == length);
    CHECK(sent_bytes[18] == EAP_RESPONSE && sent_bytes[19] == id);
    CHECK(read_be16(sent_bytes + 20) == length && sent_bytes[22] == type);
    CHECK(sent_length > 18 + length);
    check_service();
}

static void malformed_rj_properties(void) {
    unsigned char truncated[] = {0, 0, 0x13, 0x11, 0x39, 40, 'x'};
    unsigned char underflow[] = {0x1a, 8, 0, 0, 0x13, 0x11, 0x39, 1};
    unsigned char success[29] = {0};
    ETH_EAP_FRAME frame;
    LIST_ELEMENT* list = NULL;
    CHECK(IS_FAIL(parse_rjv3_buf_to_prop_list(&list, truncated, sizeof(truncated), TRUE)));
    destroy_rjv3_prop_list(&list);
    CHECK(IS_FAIL(parse_rjv3_buf_to_prop_list(&list, underflow, sizeof(underflow), FALSE)));
    destroy_rjv3_prop_list(&list);
    success[15] = EAP_PACKET; success[18] = EAP_SUCCESS;
    memcpy(success + 22, "\0\0\x13\x11\x01\x03x", 7);
    frame.content = success; frame.actual_len = frame.buffer_len = sizeof(success);
    CHECK(IS_FAIL(rjv3_process_result_prop(&frame))); /* A one-byte echo key is invalid. */
    frame.actual_len = 21;
    CHECK(IS_FAIL(rjv3_process_result_prop(&frame)));
    puts("PASS: truncated RJ properties and short echo keys rejected");
}

static unsigned char reverse_byte(unsigned char value) {
    unsigned char reversed = 0;
    int i;
    for (i = 0; i < 8; ++i) { reversed = (unsigned char)((reversed << 1) | (value & 1)); value >>= 1; }
    return reversed;
}

static void cleanup(void) {
    sched_alarm_destroy();
    free_if_impl();
    packet_plugin_destroy();
    eap_state_machine_destroy();
    free_config();
    conf_parser_free();
    close_log();
}

int main(void) {
    char* argv[] = {"packet-smoke", "--username", "student", "--password", "password",
        "--nic", "offline-test", "--module", "rjv3", "--service=overridden", "--service", "students",
        "--fake-serial", "OFFLINE", "--dhcp-type", "0", "--stage-timeout", "1",
        "--max-retries", "10"};
    int argc = sizeof(argv) / sizeof(argv[0]);
    unsigned char request[40], saved[FRAME_BUF_SIZE], success[38] = {0}, noise[23];
    size_t saved_length;
    int i, sends, echo_number;
    uint32_t echo_key = 0x11223344, echo_sum;
    static const unsigned char expected_md5[16] = {
        0xf0, 0x41, 0x2f, 0x2f, 0x1c, 0x91, 0xe0, 0xe6,
        0x0b, 0x69, 0x6c, 0xa7, 0xa9, 0x11, 0x60, 0x9b
    }; /* Independently computed: MD5(0x09 || "password" || bytes 00..0f). */
    atexit(cleanup);
    set_log_destination(LOG_TO_CONSOLE); start_log();
    CHECK(init_if_impl_list() == 1 && init_packet_plugin_list() == 2);
    load_default_params();
    CHECK(!IS_FAIL(parse_cmdline_conf_file(argc, argv)));
    CHECK(!IS_FAIL(parse_cmdline_opts(argc, argv)));
    CHECK(!IS_FAIL(parse_cmdline_opts(argc, argv))); /* Repeat the same scan from scratch. */
    CHECK(!IS_FAIL(validate_params()));
    CHECK(get_program_config()->packet_plugin_list && !get_program_config()->packet_plugin_list->next);
    CHECK(!IS_FAIL(select_if_impl("libpcap")) && !IS_FAIL(select_packet_plugin("rjv3")));
    packet_plugin_load_default_params();
    CHECK(!IS_FAIL(conf_parser_add_value("service", "staff")));
    CHECK(!IS_FAIL(packet_plugin_process_config_file("unused")));
    CHECK(!IS_FAIL(packet_plugin_process_cmdline_opts(argc, argv)));
    CHECK(!IS_FAIL(packet_plugin_process_cmdline_opts(argc, argv)));
    conf_parser_free();
    CHECK(!IS_FAIL(packet_plugin_validate_params()));
    CHECK(!IS_FAIL(sched_alarm_init()) && !IS_FAIL(eap_state_machine_init()));

    make_request(request, 23, 7, IDENTITY);
    feed(request, 17); CHECK(send_count == 0);
    feed(request, 22); CHECK(send_count == 0);
    feed(request, 23); CHECK(send_count == 1);
    check_response(7, IDENTITY, 5 + 7);
    CHECK(memcmp(sent_bytes + 23, "student", 7) == 0);
    puts("PASS: Identity response, Ethernet/EAP lengths, service students and option rescans");

    make_request(request, sizeof(request), 9, MD5_CHALLENGE);
    request[23] = 16;
    for (i = 0; i < 16; ++i) request[24 + i] = (unsigned char)i;
    feed(request, 39); CHECK(send_count == 1);
    feed(request, sizeof(request)); CHECK(send_count == 2);
    check_response(9, MD5_CHALLENGE, 5 + 1 + 16 + 7);
    CHECK(sent_bytes[23] == 16 && memcmp(sent_bytes + 24, expected_md5, 16) == 0);
    CHECK(memcmp(sent_bytes + 40, "student", 7) == 0);
    puts("PASS: MD5 response digest and RJv3 trailer");

    saved_length = sent_length; memcpy(saved, sent_bytes, saved_length);
    feed(saved, saved_length); /* Npcap can report our own transmitted response. */
    CHECK(send_count == 2);
    Sleep(1100); sched_alarm_poll();
    CHECK(send_count == 3 && sent_length == saved_length);
    CHECK(memcmp(saved, sent_bytes, saved_length) == 0);
    puts("PASS: self-captured response ignored; watchdog retransmits the original request response");

    for (i = 0; i < 4; ++i) {
        size_t noise_length = sizeof(noise);
        make_request(noise, sizeof(noise), 33, IDENTITY);
        if (i < 2) {
            noise[15] = (unsigned char)(i == 0 ? EAPOL_START : EAPOL_LOGOFF);
            noise[17] = 0; noise_length = 18;
        } else if (i == 2) {
            noise[18] = EAP_RESPONSE; /* A different supplicant's response. */
        } else {
            noise[22] = 13; /* Unsupported EAP request type. */
        }
        sends = send_count;
        feed(noise, noise_length);
        CHECK(send_count == sends);
        Sleep(1100); sched_alarm_poll();
        CHECK(send_count == sends + 1 && sent_length == saved_length);
        CHECK(memcmp(saved, sent_bytes, saved_length) == 0);
    }
    puts("PASS: foreign Start, Logoff, Response and unsupported Request do not replace the watchdog request");

    malformed_rj_properties();
    memcpy(success, local_mac, 6); memcpy(success + 6, server_mac, 6);
    success[12] = 0x88; success[13] = 0x8e; success[14] = 1;
    success[15] = EAP_PACKET; success[17] = 4; success[18] = EAP_SUCCESS;
    success[19] = 9; success[21] = 4;
    memcpy(success + 22, "\0\0\x13\x11\x01\x0c", 6);
    for (i = 0; i < 4; ++i) success[34 + i] = (unsigned char)~reverse_byte((unsigned char)(echo_key >> (24 - i * 8)));
    srand(1); echo_number = rand() & 0xffff; srand(1);
    feed(success, sizeof(success));
    sends = send_count;
    Sleep(1100); sched_alarm_poll();
    CHECK(send_count == sends + 1 && sent_bytes[15] == EAPOL_RJ_PROPRIETARY_KEEPALIVE);
    CHECK(sent_length == 18 + 27 && memcmp(sent_bytes, server_mac, 6) == 0);
    echo_sum = echo_key + (uint32_t)echo_number;
    for (i = 0; i < 4; ++i) {
        CHECK(sent_bytes[24 + i] == (unsigned char)~reverse_byte((unsigned char)(echo_sum >> (24 - i * 8))));
        CHECK(sent_bytes[34 + i] == (unsigned char)~reverse_byte((unsigned char)((uint32_t)echo_number >> (24 - i * 8))));
    }
    puts("PASS: RJv3 success trailer, echo key and heartbeat encoding; watchdog cancelled");
    puts("PASS: no Npcap library, socket, adapter enumeration or network transmission used");
    return 0;
}
