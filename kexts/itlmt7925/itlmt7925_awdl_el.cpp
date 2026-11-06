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

void itlmt7925::awdlElStat()
{
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
