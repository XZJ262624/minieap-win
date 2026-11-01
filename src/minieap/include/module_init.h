#ifndef _MINIEAP_MODULE_INIT
#define _MINIEAP_MODULE_INIT

#ifdef _WIN32
/* Windows uses explicit factory tables in if_impl.c and packet_plugin.c. */
#define __define_in_section(func, sect)
#else
#define __define_in_section(func, sect) \
    static void* (*__init_##func)() __attribute__((used, __section__(sect))) = (void*(*)())func;
#endif

#endif
