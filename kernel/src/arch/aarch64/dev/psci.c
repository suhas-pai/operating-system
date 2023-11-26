/*
 * kernel/src/arch/aarch64/dev/psci.c
 * © suhas pai
 */

#include "dev/driver.h"
#include "dev/printk.h"

#include "psci.h"

static enum psci_invoke_method g_invoke_method = PSCI_INVOKE_METHOD_NONE;
static uint32_t g_func_to_cmd[] = {
    [PSCI_FUNC_VERSION] = 0x84000000,
    [PSCI_FUNC_CPU_OFF] = 0x84000002,
    [PSCI_FUNC_MIGRATE_INFO_TYPE] = 0x84000006,
    [PSCI_FUNC_SYSTEM_OFF] = 0x84000008,
    [PSCI_FUNC_SYSTEM_RESET] = 0x84000009,
    [PSCI_FUNC_CPU_SUSPEND] = 0xC4000001,
    [PSCI_FUNC_CPU_ON] = 0xC4000003,
    [PSCI_FUNC_AFFINITY_INFO] = 0xC4000004,
    [PSCI_FUNC_MIGRATE] = 0xC4000005,
    [PSCI_FUNC_MIGRATE_INFO_UP_CPU] = 0xC4000007,
};

enum psci_return_value
psci_invoke_function(const enum psci_function func,
                     const uint64_t arg1,
                     const uint64_t arg2,
                     const uint64_t arg3)
{
    register int32_t result asm("w0");

    register uint32_t cmd asm("w0") = g_func_to_cmd[func];
    register uint64_t cpu asm("x1") = arg1;
    register uint64_t addr asm("x2") = arg2;
    register uint64_t ctx asm("x3") = arg3;

    switch (g_invoke_method) {
        case PSCI_INVOKE_METHOD_NONE:
            verify_not_reached();
        case PSCI_INVOKE_METHOD_HVC:
            asm volatile ("hvc #0"
                          : "=r"(result)
                          : "r"(cmd), "r"(cpu), "r"(addr), "r"(ctx));
            return (enum psci_return_value)result;
        case PSCI_INVOKE_METHOD_SMC:
            asm volatile ("smc #0"
                          : "=r"(result)
                          : "r"(cmd), "r"(cpu), "r"(addr), "r"(ctx));
            return (enum psci_return_value)result;
    }

    verify_not_reached();
}

static bool init_common() {
    const int32_t version =
        psci_invoke_function(PSCI_FUNC_VERSION,
                             /*arg1=*/0,
                             /*arg2=*/0,
                             /*arg3=*/0);

    if (version < 0) {
        return false;
    }

    const uint32_t major = (uint32_t)version & (uint32_t)~0xffff;
    const uint32_t minor = (uint32_t)version & 0xffff;

    printk(LOGLEVEL_INFO,
           "psci: version is %" PRId32 ".%" PRId32 "\n",
           major,
           minor);

    return true;
}

#if 0
static bool init_from_dtb(const void *const dtb, const int nodeoff) {
    struct string_view method_sv = SV_EMPTY();
    if (!dtb_get_string_prop(dtb, nodeoff, "method", &method_sv)) {
        printk(LOGLEVEL_WARN, "psci: failed to get \"method\" key in dtb\n");
        return false;
    }

    enum psci_invoke_method method = PSCI_INVOKE_METHOD_NONE;
    if (sv_equals(method_sv, SV_STATIC("hvc"))) {
        method = PSCI_INVOKE_METHOD_HVC;
    } else if (sv_equals(method_sv, SV_STATIC("smc"))) {
        method = PSCI_INVOKE_METHOD_SMC;
    } else {
        printk(LOGLEVEL_WARN,
               "psci: unrecognized method " SV_FMT " in dtb\n",
               SV_FMT_ARGS(method_sv));

        return false;
    }

    static const char *const key_list[] = {
        "migrate", "cpu_on", "cpu_off", "cpu_suspend"
    };

    static const enum psci_function key_index_to_func[] = {
        PSCI_FUNC_MIGRATE, PSCI_FUNC_CPU_ON, PSCI_FUNC_CPU_OFF,
        PSCI_FUNC_CPU_SUSPEND
    };

    carr_foreach(key_list, key_iter) {
        fdt32_t cmd = 0;
        const bool result = dtb_get_integer_prop(dtb, nodeoff, *key_iter, &cmd);

        if (!result) {
            continue;
        }

        g_func_to_cmd[key_index_to_func[key_iter - key_list]] = cmd;
    }

    if (g_invoke_method != PSCI_INVOKE_METHOD_NONE) {
        if (g_invoke_method != method) {
            g_invoke_method = PSCI_INVOKE_METHOD_NONE;
            printk(LOGLEVEL_WARN,
                   "psci: dtb's psci-invoke method doesn't match method from "
                   "either other dtb node or from acpi\n");

            return false;
        }
    } else {
        g_invoke_method = method;
    }

    init_common();
    printk(LOGLEVEL_INFO, "psci: successfully initialized from dtb\n");

    return true;
}
#endif

void psci_init_from_acpi(const bool use_hvc) {
    const enum psci_invoke_method method =
        use_hvc ? PSCI_INVOKE_METHOD_HVC : PSCI_INVOKE_METHOD_SMC;

    if (g_invoke_method != PSCI_INVOKE_METHOD_NONE) {
        if (g_invoke_method != method) {
            g_invoke_method = PSCI_INVOKE_METHOD_NONE;
            printk(LOGLEVEL_WARN,
                   "psci: fadt's psci-invoke method doesn't match method from "
                   "dtb\n");

            return;
        }
    } else {
        g_invoke_method = method;
    }

    init_common();
    printk(LOGLEVEL_INFO, "psci: successfully initialized from acpi\n");
}

enum psci_return_value psci_reboot() {
    return psci_invoke_function(PSCI_FUNC_SYSTEM_RESET,
                                /*arg1=*/0,
                                /*arg2=*/0,
                                /*arg3=*/0);
}

enum psci_return_value psci_shutdown() {
    return psci_invoke_function(PSCI_FUNC_SYSTEM_OFF,
                                /*arg1=*/0,
                                /*arg2=*/0,
                                /*arg3=*/0);
}

#if 0
static const char *const compat_list[] = {
    "arm,psci", "arm,psci-1.0", "arm,psci-0.2"
};

static const struct dtb_driver dtb_driver = {
    .compat_list = compat_list,
    .compat_count = countof(compat_list),
    .init = init_from_dtb
};

__driver static const struct driver driver = {
    .dtb = &dtb_driver,
    .pci = NULL
};
#endif