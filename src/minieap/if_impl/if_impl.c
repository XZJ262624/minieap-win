#include "linkedlist.h"
#include "if_impl.h"
#include "logging.h"
#include <stdlib.h>
#include <string.h>

/* content of this list is IF_IMPL* */
static LIST_ELEMENT* g_if_impl_list;
static IF_IMPL* g_selected_impl;

IF_IMPL* libpcap_new(void);

int init_if_impl_list() {
#ifdef _WIN32
    static IF_IMPL* (*const factories[])(void) = { libpcap_new };
    IF_IMPL* (*const* func)(void) = factories;
    IF_IMPL* (*const* end)(void) = factories + sizeof(factories) / sizeof(factories[0]);
#else
#ifdef __linux__
    extern IF_IMPL* (*__IF_IMPL_LIST_START__)();
    extern IF_IMPL* (*__IF_IMPL_LIST_END__)(); // They are just location markers, do not care about their content
#else
    extern IF_IMPL* (*__IF_IMPL_LIST_START__)() __asm("section$start$__DATA$__ifimplinit");
    extern IF_IMPL* (*__IF_IMPL_LIST_END__)() __asm("section$end$__DATA$__ifimplinit");
#endif
    IF_IMPL* (**func)() = &__IF_IMPL_LIST_START__;
    IF_IMPL* (**end)() = &__IF_IMPL_LIST_END__;
#endif
    int i = 0;
    for (; func < end; ++i, ++func) {
        IF_IMPL* impl = (*func)();
        LIST_ELEMENT* node;
        if (impl == NULL) {
            free_if_impl();
            return -1;
        }
        node = (LIST_ELEMENT*)malloc(sizeof(*node));
        if (node == NULL) {
            PR_ERRNO("无法为网络模块注册表分配内存");
            impl->destroy(impl);
            free_if_impl();
            return -1;
        }
        node->content = impl;
        node->next = NULL;
        list_concat(&g_if_impl_list, node);
    }
    return i;
}

void print_if_impl_single(void* vimpl, void* unused) {
#define IMPL ((IF_IMPL*)vimpl)
    PR_RAW("    \033[1m%s\033[0m (%s)\n", IMPL->name, IMPL->description);
}

void print_if_impl_list() {
    PR_RAW("\n以下是可用的网络操作模块：\n\n");
    list_traverse(g_if_impl_list, print_if_impl_single, NULL);
}

/*
 * Returns "matched" if to_find is NULL:
 * this will select the first implementation
 * if no name is specified.
 */
static int impl_name_cmp(void* to_find, void* vimpl) {
    return to_find ? strcmp(to_find, IMPL->name) : 0;
}

RESULT select_if_impl(const char* name) {
    g_selected_impl = (IF_IMPL*)lookup_data(g_if_impl_list, (void*)name, impl_name_cmp);
    return g_selected_impl == NULL ? FAILURE : SUCCESS;
}

IF_IMPL* get_if_impl() { return g_selected_impl; }

static void free_one_impl(void* impl, void* unused) {
    ((IF_IMPL*)impl)->destroy((IF_IMPL*)impl);
}

/*
 * Destroy everything in the list, and the list itself
 */
void free_if_impl() {
    g_selected_impl = NULL;
    list_traverse(g_if_impl_list, free_one_impl, NULL);
    list_destroy(&g_if_impl_list, FALSE);
}
