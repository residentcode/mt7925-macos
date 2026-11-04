// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
//
// BtLeDlFix: Lilu plugin that lets BLE devices finish connecting through the MediaTek MT7925 controller.
//
// bluetoothd (macOS 15) walks every new LE connection through
//   VERSION -> FEATURES -> LENGTH -> PHY -> ready
// and in the LENGTH step waits for an HCI "LE Data Length Change" event after it has seen that both sides support
// Extended Data Length. The MT7925 firmware (like any spec-compliant controller whose lengths already match) never
// emits that event, so the link sits in WAIT_FOR_LENGTH until the peer times out and a BLE mouse never connects.
// Apple has a quirk for one chipset id (cmp eax, 8) that skips the step; this plugin makes the quirk unconditional by
// turning "cmp eax, 8" into "cmp eax, eax". Same hook as BlueToolFixup: cs_validate_page, patching the page in memory.
//
// Pattern (bluetoothd x86_64, macOS 15.8.1 / 24H32, __text+0x1e4f69):
//   83 f8 08            cmp   eax, 8          -> 39 c0 90   cmp eax, eax ; nop
//   45 0f b6 f6         movzx r14d, r14b
//   b8 00 00 00 00      mov   eax, 0
//   44 0f 44 f0         cmove r14d, eax
// (This pattern does not occur in the macOS 26.7.1 / 25G241 bluetoothd; the patch is then a no-op.)
//
// Second patch, LE advertising (macOS 26.7.1 / 25G241, bluetoothd x86_64 __text+0x40fcb4):
// bluetoothd has two ways to advertise. The legacy multi-advertising path calls a per-chipset vendor (Broadcom VSC)
// table; a third-party USB controller is chipset 0 ("setting chipset to THIRD_PARTY_DONGLE"), the table is empty and
// LE_InitMultipleAdvertising fails with status 152, so the LE advertise module never leaves "stack:OFF" and every
// request ends in "Stack is not started, updateAdvertisement returning". The other path, BT 5.0 advertising sets
// (standard HCI LE Extended Advertising commands), is chosen from a chipset-number whitelist that leaves out chipset 0.
// The MT7925 supports those commands (supported-commands octets 36-37 = FF FF, LE feature bit 12 set), so the plugin
// makes that one whitelist call answer yes on a Mac:
//   48 8b 08            mov   rcx, [rax]
//   48 89 c7            mov   rdi, rax
//   ff 91 c0 09 00 00   call  [rcx+0x9c0]     -> b8 01 00 00 00 90   mov eax, 1 ; nop
//   41 89 c6            mov   r14d, eax
// The defaults override DisableSpecSets is still honoured (it is read after this call).
//
// Third patch, advertising-set random address (macOS 26.7.1 / 25G241, __text+0x510ea9):
// with advertising sets on, the stack enables each set with LE Set Extended Advertising Enable. Sets that advertise
// from a random address (Apple's Nearby / AirDrop instances) need LE Set Advertising Set Random Address first, but
// initCoreStack clears the "send that command" flag for chipsets <= 3 (chipset 0 = third-party controller). The
// stack then enables a set whose random address was never written; the MT7925 rejects it as the Core spec requires
// (Invalid HCI Command Parameters 0x12, logged "STATUS 718"), and the start/halt/restart loop flaps ~20x/s with a
// follow-on 0x42 (766). The plugin drops that one call so the flag keeps its default (1):
//   83 b8 34 03 00 00 03   cmp   dword [rax+0x334], 3
//   77 2a                  ja    +0x2a
//   31 ff                  xor   edi, edi
//   e8 3e 6b ce ff         call  setUseAdvSetRandomAddress   -> 0f 1f 44 00 00   nop

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_version.hpp>

#include <sys/vnode.h>

#define MODULE_SHORT "btledl"

static const uint8_t kLenOriginal[] = {
    0x83, 0xF8, 0x08, 0x45, 0x0F, 0xB6, 0xF6, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x44, 0x0F, 0x44, 0xF0,
};
static const uint8_t kLenPatched[] = {
    0x39, 0xC0, 0x90, 0x45, 0x0F, 0xB6, 0xF6, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x44, 0x0F, 0x44, 0xF0,
};

static const uint8_t kAdvOriginal[] = {
    0x48, 0x8B, 0x08, 0x48, 0x89, 0xC7, 0xFF, 0x91, 0xC0, 0x09, 0x00, 0x00, 0x41, 0x89, 0xC6,
};
static const uint8_t kAdvPatched[] = {
    0x48, 0x8B, 0x08, 0x48, 0x89, 0xC7, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x41, 0x89, 0xC6,
};

static const uint8_t kAdvAddrOriginal[] = {
    0x83, 0xB8, 0x34, 0x03, 0x00, 0x00, 0x03, 0x77, 0x2A, 0x31, 0xFF, 0xE8, 0x3E, 0x6B, 0xCE, 0xFF,
};
static const uint8_t kAdvAddrPatched[] = {
    0x83, 0xB8, 0x34, 0x03, 0x00, 0x00, 0x03, 0x77, 0x2A, 0x31, 0xFF, 0x0F, 0x1F, 0x44, 0x00, 0x00,
};

static mach_vm_address_t orig_cs_validate {};
static bool advPatchEnabled = true;

static void patched_cs_validate_page(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data,
                                     int *validated_p, int *tainted_p, int *nx_p) {
    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    FunctionCast(patched_cs_validate_page, orig_cs_validate)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);
    static constexpr size_t dirLength = sizeof("/usr/sbin/") - 1;
    if (vn_getpath(vp, path, &pathlen) == 0 && UNLIKELY(strncmp(path, "/usr/sbin/", dirLength) == 0) &&
        strcmp(path + dirLength, "bluetoothd") == 0) {
        if (KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, kLenOriginal, kLenPatched))
            SYSLOG(MODULE_SHORT, "patched LE data length wait in %s", path);
        if (advPatchEnabled && KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, kAdvOriginal, kAdvPatched))
            SYSLOG(MODULE_SHORT, "patched LE advertising sets check in %s", path);
        if (advPatchEnabled && KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, kAdvAddrOriginal, kAdvAddrPatched))
            SYSLOG(MODULE_SHORT, "patched advertising set random address flag in %s", path);
    }
}

static void pluginStart() {
    SYSLOG(MODULE_SHORT, "start");
    if (checkKernelArgument("-btledlfxnoadv")) {
        advPatchEnabled = false;
        SYSLOG(MODULE_SHORT, "LE advertising patch disabled by -btledlfxnoadv");
    }
    lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
        KernelPatcher::RouteRequest csRoute("_cs_validate_page", patched_cs_validate_page, orig_cs_validate);
        if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &csRoute, 1))
            SYSLOG(MODULE_SHORT, "failed to route cs validation pages");
    });
}

static const char *bootargOff[] {"-btledlfxoff"};
static const char *bootargDebug[] {"-btledlfxdbg"};
static const char *bootargBeta[] {"-btledlfxbeta"};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Monterey,
    KernelVersion::Tahoe,
    pluginStart
};
