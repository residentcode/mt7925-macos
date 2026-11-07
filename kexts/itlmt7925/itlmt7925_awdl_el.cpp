// SPDX-License-Identifier: BSD-3-Clause-Clear
// AWDL election fields on the air, as a working non-Apple AWDL node writes them (docs/ROADMAP-AWDL.md §10.65).
//
// Reference: Owly (github.com/t4t5/owly, MIT), which exchanges AirDrop both ways with iOS 26 iPhones on an MT7925 under
// Linux. Its captures (tests/captures/receive-ios26.x-mt7925e-ch44-1.pcap) show what the iPhone accepts from a node it then
// sends unicast to: the node follows the iPhone (dist 1), advertises the master's metric and counter exactly as the master
// itself advertises them in its Election Parameters v2 TLV (type 24), and keeps its own metric at 60 (AWDL-8), far below the
// master's. Ours, as the Family built them in test 4: a slave of the iPhone, but advertising the iPhone with metric 541 /
// counter 491 while the iPhone said 512 / 483 of itself, and our own metric 526 above our master's 512.
//
// itl_awdlel=1 rewrites, at TX time and only while our TLV 24 names another node as master: TLV 24 master counter and master
// metric (copied from the master's own last TLV 24, at most 5 s old), TLV 24 own metric, TLV 5 top master metric and TLV 5 own
// metric. Nothing else in the frame changes, and the Family's own election state is untouched. 0 (default) = frames as built.
#include "itlmt7925.hpp"

#include <string.h>
#include <libkern/libkern.h>

static inline uint32_t elLe32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void elPut32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint64_t elNowMs()
{
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000000;
}

// The TLV of `type` in an AWDL action frame (24-byte 802.11 header + 16-byte AWDL header, then TLVs), or nullptr.
static uint8_t *elFind(uint8_t *f, uint32_t len, uint8_t type, uint32_t minLen)
{
    uint32_t pos = 40;
    while (pos + 3 <= len) {
        uint32_t l = (uint32_t)f[pos + 1] | ((uint32_t)f[pos + 2] << 8);
        if (pos + 3 + l > len) return nullptr;
        if (f[pos] == type) return l >= minLen ? f + pos + 3 : nullptr;
        pos += 3 + l;
    }
    return nullptr;
}

// RX: remember what each master says of itself. Only a node that is its own master (TLV 24 master == sync == SA, distance 0)
// is learned from, so the values are the master's own and not a relay's view of it.
void itlmt7925::awdlElLearn(const uint8_t *f, uint32_t len)
{
    if (len < 40 + 3 + 40) return;
    const uint8_t *v = elFind((uint8_t *)f, len, 24, 40);
    if (!v) return;
    const uint8_t *sa = f + 10;
    if (memcmp(v, sa, 6) != 0 || memcmp(v + 6, sa, 6) != 0 || elLe32(v + 16) != 0) return;
    uint32_t slot = 0, oldest = 0;
    uint64_t oldestMs = ~0ull;
    for (; slot < kAwdlElPeers; slot++) {
        if (!memcmp(awdlElPeer[slot].addr, sa, 6)) break;
        if (awdlElPeer[slot].ms < oldestMs) { oldestMs = awdlElPeer[slot].ms; oldest = slot; }
    }
    if (slot == kAwdlElPeers) slot = oldest;
    AwdlElPeer &p = awdlElPeer[slot];
    p.ms = 0;                                                // invalid while it is rewritten (TX runs on another thread)
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(p.addr, sa, 6);
    p.metric = elLe32(v + 20);
    p.counter = elLe32(v + 12);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p.ms = elNowMs();
    awdlElLearned++;
}

// TX: the rewrite described at the top. Returns true when the frame was changed.
bool itlmt7925::awdlElFix(uint8_t *f, uint32_t len)
{
    if (len < 40) return false;
    uint8_t *v24 = elFind(f, len, 24, 40);
    if (!v24) { awdlElNo24++; return false; }
    memcpy(awdlElTxMaster, v24, 6);                          // what goes on the air with the switch off (stat line)
    awdlElTxMetric = elLe32(v24 + 20);
    awdlElTxCounter = elLe32(v24 + 12);
    awdlElTxOwn = elLe32(v24 + 24);
    if (!awdlElOn) return false;
    if (!memcmp(v24, f + 10, 6)) { awdlElSelf++; return false; }    // we are our own master: nothing to copy
    uint64_t now = elNowMs();
    const AwdlElPeer *m = nullptr;
    for (uint32_t i = 0; i < kAwdlElPeers; i++)
        if (awdlElPeer[i].ms && now - awdlElPeer[i].ms < 5000 && !memcmp(awdlElPeer[i].addr, v24, 6)) m = &awdlElPeer[i];
    if (!m) { awdlElStale++; return false; }
    uint32_t metric = m->metric, counter = m->counter;
    uint32_t own = awdlElMetric < metric ? awdlElMetric : (metric > 1 ? metric - 1 : 0);
    if (awdlElFixed < 3)
        m3log("awdl el: fix master %02x%02x%02x%02x%02x%02x metric %u->%u ctr %u->%u own %u->%u dist=%u",
              v24[0], v24[1], v24[2], v24[3], v24[4], v24[5], elLe32(v24 + 20), metric, elLe32(v24 + 12), counter,
              elLe32(v24 + 24), own, elLe32(v24 + 16));
    elPut32(v24 + 12, counter);
    elPut32(v24 + 20, metric);
    elPut32(v24 + 24, own);
    if (uint8_t *v5 = elFind(f, len, 5, 19)) {
        elPut32(v5 + 11, metric);
        elPut32(v5 + 15, own);
    }
    awdlElFixed++;
    return true;
}

// RX, the copy handed to the Family (itl_awdlel=1): a master's own metric is raised above ours, so the Family's election
// follows that master the way Owly does (it starts at 60 and follows the iPhone), instead of making us master because the
// Family's metric happens to be higher (533 vs the iPhone's 522 on 2026-11-06). Only the copy changes; awdlElLearn has
// already taken the master's real values, which are what goes back on the air.
void itlmt7925::awdlElRxFix(uint8_t *f, uint32_t len)
{
    if (!awdlElOn || len < 40 + 3 + 40) return;
    uint8_t *v24 = elFind(f, len, 24, 40);
    if (!v24) return;
    const uint8_t *sa = f + 10;
    if (memcmp(v24, sa, 6) != 0 || memcmp(v24 + 6, sa, 6) != 0 || elLe32(v24 + 16) != 0) return;
    uint32_t target = awdlElTxOwn ? awdlElTxOwn + 100 : 1000;
    if (elLe32(v24 + 20) >= target) return;
    elPut32(v24 + 20, target);
    elPut32(v24 + 24, target);
    if (uint8_t *v5 = elFind(f, len, 5, 19)) {
        elPut32(v5 + 11, target);
        elPut32(v5 + 15, target);
    }
    awdlElRxFixed++;
}

// TLVs 5 and 18 (§10.65). Owly's spec (docs/SPEC.md AWDL-6) marks Election Parameters (5) and Channel Sequence (18)
// required, and every PSF/MIF of the iPhone and of Owly carries them right after TLV 4: "A PSF carries types 4, 5, 18, 24, 6,
// 7, 12 and 21, in that order". On Apple hardware the firmware adds them; the Family's TLVs here have neither, and this driver
// only added TLV 4 - the 2026-11-06 snapshots show our PSF as 4, 6, 24, 12, 7, 21. itl_awdltlv=1 (default) inserts, when
// missing: TLV 5 from TLV 24 (distance, top master, its metric, our metric), and TLV 18 from TLV 4's own channel sequence
// (value +33: count-1, encoding, duplicates, step, fill, 16 slots), both right after TLV 4. Done once per submitted frame.
uint32_t itlmt7925::awdlAddTlvs(uint8_t *f, uint32_t len, uint32_t cap)
{
    if (!awdlTlvOn || len < 40 || f[0] != 0xd0) return len;
    uint8_t *v4 = elFind(f, len, 4, 33 + 6 + 32);
    uint8_t *v24 = elFind(f, len, 24, 40);
    if (!v4) { awdlTlvNo4++; return len; }
    bool has5 = elFind(f, len, 5, 0) != nullptr, has18 = elFind(f, len, 18, 0) != nullptr;
    uint8_t add[3 + 21 + 3 + 41];
    uint32_t n = 0;
    if (!has5 && v24) {
        uint8_t *t = add + n;
        bzero(t, 3 + 21);
        t[0] = 5; t[1] = 21; t[2] = 0;
        uint8_t *v = t + 3;
        v[3] = (uint8_t)elLe32(v24 + 16);                    // distance to the top master
        memcpy(v + 5, v24, 6);                               // top master
        elPut32(v + 11, elLe32(v24 + 20));                   // its metric
        elPut32(v + 15, elLe32(v24 + 24));                   // our metric
        n += 3 + 21;
    }
    if (!has18) {
        uint8_t *t = add + n;
        bzero(t, 3 + 41);
        t[0] = 18; t[1] = 41; t[2] = 0;
        memcpy(t + 3, v4 + 33, 6 + 32);                      // the sequence TLV 4 carries, as the iPhone's TLV 18 repeats it
        n += 3 + 41;
    }
    if (!n) return len;
    if (len + n > cap) { awdlTlvNoRoom++; return len; }
    uint32_t at = (uint32_t)(v4 - f) + (uint32_t)(f[v4 - f - 2] | (f[v4 - f - 1] << 8));   // end of TLV 4
    memmove(f + at + n, f + at, len - at);
    memcpy(f + at, add, n);
    awdlTlvAdded++;
    return len + n;
}

// itl_awdlafall: a kept PSF/MIF re-sent in another window names that window's channel in TLV 4 (+0, the channel of this AW).
void itlmt7925::awdlAfSetCh(uint8_t *f, uint32_t len, uint8_t ch)
{
    if (uint8_t *v4 = elFind(f, len, 4, 1)) v4[0] = ch;
}

// itl_awdlapch (§10.66): tells the AWDL interface which channel it may put in its sequence although it is DFS - the AP's,
// where the radio already is - through the property "itlmt-awdl-apch" (0 = none).
void itlmt7925::awdlApChPublish()
{
    uint32_t v = (awdlApCh && m3linkUp) ? tgtCh : 0;
    OSNumber *n = OSNumber::withNumber(v, 32);
    if (n) { setProperty("itlmt-awdl-apch", n); n->release(); }
}

// Frame snapshots (§10.65): whole frames as they go out and come in, published as ioreg properties itlmt-awdl-snap-<kind>
// so they can be diffed byte by byte against Owly's working iOS 26 captures. At most one copy per kind every 2 s.
static const char *const kSnapName[8] = {
    "itlmt-awdl-snap-our-psf", "itlmt-awdl-snap-our-mif", "itlmt-awdl-snap-peer-psf", "itlmt-awdl-snap-peer-mif",
    "itlmt-awdl-snap-our-uc", "itlmt-awdl-snap-our-grp", "itlmt-awdl-snap-peer-grp", "itlmt-awdl-snap-peer-uc",
};

void itlmt7925::awdlSnap(uint32_t kind, const uint8_t *f, uint32_t len)
{
    if (kind >= kAwdlSnapKinds || !f || !len) return;
    uint64_t now = elNowMs();
    AwdlSnapBuf &s = awdlSnapBuf[kind];
    if (s.busy || (s.ms && now - s.ms < 2000)) return;
    s.busy = 1;
    uint32_t n = len < sizeof(s.data) ? len : (uint32_t)sizeof(s.data);
    memcpy(s.data, f, n);
    s.len = n;
    s.ms = now;
    s.dirty = 1;
    s.busy = 0;
}

void itlmt7925::awdlSnapPublish()
{
    for (uint32_t k = 0; k < kAwdlSnapKinds; k++) {
        AwdlSnapBuf &s = awdlSnapBuf[k];
        if (!s.dirty || s.busy) continue;
        s.busy = 1;
        OSData *d = OSData::withBytes(s.data, s.len);
        s.dirty = 0;
        s.busy = 0;
        if (d) { setProperty(kSnapName[k], d); d->release(); }
    }
}

void itlmt7925::awdlElStat()
{
    awdlSnapPublish();
    awdlApChPublish();
    m3log("awdl apch: on=%u ch=%u win=%u", awdlApCh, tgtCh, awdlApChWin);
    m3log("awdl tlv: on=%u added=%u no4=%u noroom=%u | afall=%u tx=%u ok=%u", awdlTlvOn, awdlTlvAdded, awdlTlvNo4,
          awdlTlvNoRoom, awdlAfAll, awdlAfAllTx, awdlAfAllOk);
    m3log("awdl el: on=%u metric=%u fixed=%u self=%u stale=%u no24=%u learned=%u rxfix=%u | tx built: master=%02x%02x%02x%02x%02x%02x "
          "metric=%u ctr=%u own=%u", awdlElOn, awdlElMetric, awdlElFixed, awdlElSelf, awdlElStale, awdlElNo24, awdlElLearned, awdlElRxFixed,
          awdlElTxMaster[0], awdlElTxMaster[1], awdlElTxMaster[2], awdlElTxMaster[3], awdlElTxMaster[4], awdlElTxMaster[5],
          awdlElTxMetric, awdlElTxCounter, awdlElTxOwn);
    for (uint32_t i = 0; i < kAwdlElPeers; i++)
        if (awdlElPeer[i].ms)
            m3log("awdl el: master %02x%02x%02x%02x%02x%02x says metric=%u ctr=%u age=%llums", awdlElPeer[i].addr[0],
                  awdlElPeer[i].addr[1], awdlElPeer[i].addr[2], awdlElPeer[i].addr[3], awdlElPeer[i].addr[4],
                  awdlElPeer[i].addr[5], awdlElPeer[i].metric, awdlElPeer[i].counter, elNowMs() - awdlElPeer[i].ms);
}
