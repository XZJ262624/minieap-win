#include "linkedlist.h"
#include "packet_plugin.h"
#include "logging.h"
#include "conf_parser.h"

#include <string.h>
#include <stdlib.h>
#include <getopt.h>

#define TRUE 1
#define FALSE 0

/* content of this list is PACKET_PLUGIN_INFO* */
static LIST_ELEMENT* g_packet_plugin_list;
/* We need to specify the order of plugins */
static LIST_ELEMENT* g_active_packet_plugin_list;

#ifdef _WIN32
PACKET_PLUGIN* packet_plugin_rjv3_new(void);
PACKET_PLUGIN* packet_plugin_printer_new(void);
#endif

int init_packet_plugin_list() {
#ifdef _WIN32
    static PACKET_PLUGIN* (*const factories[])(void) = {
        packet_plugin_rjv3_new, packet_plugin_printer_new
    };
    PACKET_PLUGIN* (*const* func)(void) = factories;
    PACKET_PLUGIN* (*const* end)(void) = factories + sizeof(factories) / sizeof(factories[0]);
#else
#ifdef __linux__
    extern PACKET_PLUGIN* (*__PACKET_PLUGIN_LIST_START__)();
    extern PACKET_PLUGIN* (*__PACKET_PLUGIN_LIST_END__)();
#else
    // __asm("alias for this variable in assembly");
    extern PACKET_PLUGIN* (*__PACKET_PLUGIN_LIST_START__)() __asm("section$start$__DATA$__pktplugininit");
    extern PACKET_PLUGIN* (*__PACKET_PLUGIN_LIST_END__)() __asm("section$end$__DATA$__pktplugininit");
#endif
    PACKET_PLUGIN* (**func)() = &__PACKET_PLUGIN_LIST_START__;
    PACKET_PLUGIN* (**end)() = &__PACKET_PLUGIN_LIST_END__;
#endif
    int i = 0;
    for (; func < end; ++i, ++func) {
        PACKET_PLUGIN* plugin = (*func)();
        LIST_ELEMENT* node;
        if (plugin == NULL) {
            packet_plugin_destroy();
            return -1;
        }
        node = (LIST_ELEMENT*)malloc(sizeof(*node));
        if (node == NULL) {
            PR_ERRNO("无法为数据包插件注册表分配内存");
            plugin->destroy(plugin);
            packet_plugin_destroy();
            return -1;
        }
        node->content = plugin;
        node->next = NULL;
        list_concat(&g_packet_plugin_list, node);
    }
    return i;
}

static int plugin_name_cmp(void* to_find, void* curr) {
    return strcmp(to_find, ((PACKET_PLUGIN*)curr)->name);
}

RESULT select_packet_plugin(const char* name) {
    PACKET_PLUGIN* _info;
    _info = (PACKET_PLUGIN*)lookup_data(g_packet_plugin_list, (void*)name, plugin_name_cmp);
    if (_info != NULL) {
        insert_data(&g_active_packet_plugin_list, _info);
        return SUCCESS;
    }
    return FAILURE;
}

static void save_one_packet_plugin(void* plugin, void* unused) {
    conf_parser_add_value("module", ((PACKET_PLUGIN*)plugin)->name);
}

void save_active_packet_plugin_list() {
    list_traverse(g_active_packet_plugin_list, save_one_packet_plugin, NULL);
}

/*
 * I know this is silly, but is there better way to do it?
 * list_traverse can not handle varying number of params. I'd make
 * lots of useless structs to pass params if we do need to use list_traverse.
 */
#define PLUGIN ((PACKET_PLUGIN*)(plugin_info->content))
#define CHK_FUNC(func) \
    if (func == NULL) continue;

void packet_plugin_destroy() {
    LIST_ELEMENT *plugin_info = g_packet_plugin_list; // Destroy everything, not just active ones
    if (g_packet_plugin_list == NULL) return;
    do {
        CHK_FUNC(PLUGIN->destroy);
        PLUGIN->destroy(PLUGIN);
    } while ((plugin_info = plugin_info->next));
    list_destroy(&g_packet_plugin_list, FALSE);
    list_destroy(&g_active_packet_plugin_list, FALSE);
}

RESULT packet_plugin_process_cmdline_opts(int argc, char* argv[]) {
    LIST_ELEMENT *plugin_info = g_active_packet_plugin_list;
    if (g_active_packet_plugin_list == NULL) return SUCCESS;
    do {
        CHK_FUNC(PLUGIN->process_cmdline_opts);
#ifdef _WIN32
        optind = 0; /* Reset getopt's internal scan state as well. */
#else
        optind = 1;
#endif
        opterr = 0; /* Do not warn about unsupported options */
        if (PLUGIN->process_cmdline_opts(PLUGIN, argc, argv) == FAILURE)
            return FAILURE;
    } while ((plugin_info = plugin_info->next));
    return SUCCESS;
}

RESULT packet_plugin_process_config_file(const char* filepath) {
    LIST_ELEMENT *plugin_info = g_active_packet_plugin_list;
    if (g_active_packet_plugin_list == NULL) return SUCCESS;
    do {
        CHK_FUNC(PLUGIN->process_config_file);
        if (PLUGIN->process_config_file(PLUGIN, filepath) == FAILURE)
            return FAILURE;
    } while ((plugin_info = plugin_info->next));
    return SUCCESS;
}

RESULT packet_plugin_validate_params() {
    LIST_ELEMENT *plugin_info = g_active_packet_plugin_list;
    if (g_active_packet_plugin_list == NULL) return SUCCESS;
    do {
        CHK_FUNC(PLUGIN->validate_params);
        if (PLUGIN->validate_params(PLUGIN) == FAILURE)
            return FAILURE;
    } while ((plugin_info = plugin_info->next));
    return SUCCESS;
}

void packet_plugin_load_default_params() {
    LIST_ELEMENT *plugin_info = g_active_packet_plugin_list;
    if (g_active_packet_plugin_list == NULL) return;
    do {
        CHK_FUNC(PLUGIN->load_default_params);
        PLUGIN->load_default_params(PLUGIN);
    } while ((plugin_info = plugin_info->next));
}

void packet_plugin_print_cmdline_help() {
    LIST_ELEMENT *plugin_info = g_packet_plugin_list;
    if (g_packet_plugin_list == NULL) return;
    PR_RAW("\n以下是可用的数据包修改插件及其选项：\n");
    do {
        PR_RAW("\n    插件名称： \033[1m%s\033[0m (%s)\n", PLUGIN->name, PLUGIN->description);
        if (PLUGIN->print_cmdline_help) {
            PLUGIN->print_cmdline_help(PLUGIN);
        } else {
            PR_RAW("\t此插件无选项可用\n");
        }
    } while ((plugin_info = plugin_info->next));
}

RESULT packet_plugin_prepare_frame(ETH_EAP_FRAME* frame) {
    LIST_ELEMENT *plugin_info = g_active_packet_plugin_list;
    if (g_active_packet_plugin_list == NULL) return SUCCESS;
    do {
        CHK_FUNC(PLUGIN->prepare_frame);
        if (PLUGIN->prepare_frame(PLUGIN, frame) == FAILURE)
            return FAILURE;
    } while ((plugin_info = plugin_info->next));
    return SUCCESS;
}

RESULT packet_plugin_on_frame_received(ETH_EAP_FRAME* frame) {
    LIST_ELEMENT *plugin_info = g_active_packet_plugin_list;
    if (g_active_packet_plugin_list == NULL) return SUCCESS;
    do {
        CHK_FUNC(PLUGIN->on_frame_received);
        if (PLUGIN->on_frame_received(PLUGIN, frame) == FAILURE)
            return FAILURE;
    } while ((plugin_info = plugin_info->next));
    return SUCCESS;
}

void packet_plugin_set_auth_round(int round) {
    LIST_ELEMENT *plugin_info = g_active_packet_plugin_list;
    if (g_active_packet_plugin_list == NULL) return;
    do {
        CHK_FUNC(PLUGIN->set_auth_round);
        PLUGIN->set_auth_round(PLUGIN, round);
    } while ((plugin_info = plugin_info->next));
}

void packet_plugin_print_banner() {
    LIST_ELEMENT *plugin_info = g_active_packet_plugin_list;
    if (g_active_packet_plugin_list == NULL) return;
    do {
        CHK_FUNC(PLUGIN->print_banner);
        PLUGIN->print_banner(PLUGIN);
    } while ((plugin_info = plugin_info->next));
}

void packet_plugin_save_config() {
    LIST_ELEMENT *plugin_info = g_active_packet_plugin_list;
    if (g_active_packet_plugin_list == NULL) return;
    do {
        CHK_FUNC(PLUGIN->save_config);
        PLUGIN->save_config(PLUGIN);
    } while ((plugin_info = plugin_info->next));
}
