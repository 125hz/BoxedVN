/* Exercise the production shim, replacing only the host syscall transport.
 * No Wine, Vulkan driver, JIT, or user executable is run by this fixture. */
#include <assert.h>
#include <stdio.h>
#include "boxedwine_x64_vulkan_bridge.h"

static int64_t test_abi, test_caps, test_result;
static unsigned command_calls, lookup_calls;
static int64_t test_call(uint64_t op, uint64_t *args, uint64_t count)
{
    (void)args;
    (void)count;
    if (op == BOXEDWINE_X64_VK_OP_ABI) return test_abi;
    if (op == BOXEDWINE_X64_VK_OP_PROBE) return test_caps;
    if (op == BOXEDWINE_X64_VK_OP_PROC_ADDR) {
        ++lookup_calls;
        return 0; /* An unavailable extension must still resolve to NULL. */
    }
    ++command_calls;
    return test_result;
}

#define boxedwine_x64_vulkan_call test_call
#include "vulkan.c"

static void check_globals(int64_t abi, int64_t caps, int64_t host_result,
                          VkResult expected, unsigned expected_calls)
{
    typedef VkResult (*enumerate_fn)(const char *, uint32_t *, void *);
    typedef VkResult (*create_fn)(const void *, const void *, VkInstance *);
    typedef VkResult (*layers_fn)(uint32_t *, void *);
    typedef VkResult (*version_fn)(uint32_t *);
    enumerate_fn enumerate;
    create_fn create;
    layers_fn layers;
    version_fn version;
    uint32_t value = 0;
    VkInstance instance = NULL;
    test_abi = abi;
    test_caps = caps;
    test_result = host_result;
    command_calls = lookup_calls = 0;
    bw_ready = -1;

    enumerate = (enumerate_fn)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
    create = (create_fn)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    layers = (layers_fn)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceLayerProperties");
    version = (version_fn)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    assert(enumerate && create && layers && version);
    assert(command_calls == 0 && lookup_calls == 0);
    /* Wine 11's initialization sequence, followed by another attempt: failure
     * must return to the caller, not fault or leave a once-lock stranded. */
    assert(enumerate(NULL, &value, NULL) == expected);
    assert(create(NULL, NULL, &instance) == expected);
    assert(enumerate(NULL, &value, NULL) == expected);
    assert(layers(&value, NULL) == expected);
    assert(version(&value) == expected);
    assert(command_calls == expected_calls);
    assert(!vkGetInstanceProcAddr(NULL, "vkUnknownExtensionEXT"));
    assert(!vkGetInstanceProcAddr(NULL, NULL));
}

int main(void)
{
    const int64_t abi = BOXEDWINE_X64_VK_ABI_VERSION;
    /* 0x5 is exactly the desktop/helper capability mask in device logs. */
    check_globals(abi, 5, 0, VK_ERROR_INITIALIZATION_FAILED, 0);
    check_globals(abi, 0, 0, VK_ERROR_INITIALIZATION_FAILED, 0);
    check_globals(abi - 1, 7, 0, VK_ERROR_INITIALIZATION_FAILED, 0);
    check_globals(abi, 7, 0, VK_SUCCESS, 5);
    check_globals(abi, 7, BOXEDWINE_X64_VK_E_NOHOST, VK_ERROR_INITIALIZATION_FAILED, 5);
    puts("Vulkan global lookup: sparse helpers, native dispatch, and refusal paths passed");
    return 0;
}
