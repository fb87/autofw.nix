#ifndef SHELL_PLUGIN_ABI_H
#define SHELL_PLUGIN_ABI_H

#include <stdint.h>

#define SHELL_PLUGIN_ABI_VERSION 1U

#ifdef __cplusplus
extern "C"
{
#endif

    struct shell_host_api;

    struct shell_plugin_descriptor
    {
        uint32_t abi_version;

        const char* id;
        const char* name;

        void* (*open)(const struct shell_host_api* host);
        void (*close)(void* plugin);
        int (*dispatch)(void* plugin);
    };

    const struct shell_plugin_descriptor* shell_plugin_query(uint32_t abi_version);

#ifdef __cplusplus
}
#endif

#endif
