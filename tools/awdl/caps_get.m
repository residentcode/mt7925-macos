// RawGet CARD_CAPABILITIES (0xc) on an interface the way wifip2pd does (len 21), print the bytes userland sees.
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
struct apple80211req { char n[16]; int32_t type; int32_t val; uint32_t len; void *data; };
typedef int (*fopen_t)(void **); typedef int (*fbind_t)(void *, io_service_t, CFStringRef);
typedef int (*fbind2_t)(void *, CFStringRef); typedef int (*fraw_t)(void *, struct apple80211req *);
int main(int argc, char **argv) { @autoreleasepool {
    const char *ifn = argc > 1 ? argv[1] : "en5"; int type = argc > 2 ? (int)strtol(argv[2], 0, 0) : 0xc;
    uint32_t len = argc > 3 ? (uint32_t)strtol(argv[3], 0, 0) : 21;
    void *fw = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/IO80211", RTLD_NOW);
    fopen_t o = (fopen_t)dlsym(fw, "Apple80211Open"); fbind_t b = (fbind_t)dlsym(fw, "Apple80211BindToInterfaceWithService");
    fbind2_t b2 = (fbind2_t)dlsym(fw, "Apple80211BindToInterface"); fraw_t g = (fraw_t)dlsym(fw, "Apple80211RawGet");
    void *h = 0; int rc = o(&h); if (rc) { printf("open rc=%d\n", rc); return 1; }
    CFStringRef name = CFStringCreateWithCString(NULL, ifn, kCFStringEncodingUTF8);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("itlmt7925WifiInterface"));
    @try { rc = b(h, svc, name); } @catch (NSException *e) { rc = b2(h, name); }
    printf("bind rc=%d\n", rc);
    uint8_t buf[256]; memset(buf, 0xee, sizeof(buf));
    struct apple80211req r; memset(&r, 0, sizeof(r)); strlcpy(r.n, ifn, 16); r.type = type; r.len = len; r.data = buf;
    rc = g(h, &r); printf("RawGet(0x%x len=%u) rc=%d (0x%x) len_out=%u\n", type, len, rc, rc, r.len);
    for (uint32_t i = 0; i < len + 4 && i < sizeof(buf); i++) printf("%02x%s", buf[i], (i % 16 == 15) ? "\n" : " ");
    printf("\n"); return 0; } }
