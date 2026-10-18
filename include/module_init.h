#ifndef _MINIEAP_MODULE_INIT
#define _MINIEAP_MODULE_INIT

/*
 * Registers a module factory function by placing a pointer to it into a
 * dedicated linker section, which if_impl.c / packet_plugin.c walk at startup.
 *
 * This relies on ELF/other formats that expose __start_<section> and
 * __stop_<section> symbols. Windows PE files do not, so on Windows the build
 * system generates an explicit registry instead and this macro expands to
 * nothing. See the g_*_registry arrays in if_impl/if_impl.c and
 * packet_plugin/packet_plugin.c.
 */
#if defined(__GNUC__) && !defined(_WIN32)
#define __define_in_section(func, sect) \
    static void* (*__init_##func)() __attribute__((used, __section__(sect))) = (void*(*)())func;
#else
#define __define_in_section(func, sect)
#endif

#endif
