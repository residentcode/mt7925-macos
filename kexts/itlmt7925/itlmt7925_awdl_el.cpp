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
static inline uint16_t elLe16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void elPut16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static uint64_t elNowUs()
{
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000;
}

static const uint32_t kElTuUs = 1024, kElAwTu = 16, kElSlotTu = 64;   // AW 16 TU, 4 AWs a slot

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
    // Its slot clock (itl_awdlfollow): the next slot boundary is next_tu TU from now and holds AW aw (Owly: Clock::update).
    if (const uint8_t *v4 = elFind((uint8_t *)f, len, 4, 31)) {
        uint32_t next = elLe16(v4 + 1);
        if (next > kElSlotTu) next = kElSlotTu;
        p.clkUs = elNowUs() - (uint64_t)(kElSlotTu - next) * kElTuUs;
        p.clkAw = (uint16_t)(elLe16(v4 + 29) & ~3u);
    }
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

// TX (itl_awdlfollow): the frame follows the master heard last, as Owly's slave does (node.rs action_frame, tlv.rs
// put_sync_params / put_election_v1 / v2). Runs after the timeline stamp, so these values are what goes on the air.
bool itlmt7925::awdlFollowFix(uint8_t *f, uint32_t len)
{
    if (!awdlFollow || len < 40) return false;
    uint64_t nowMs = elNowMs();
    const AwdlElPeer *m = nullptr;
    for (uint32_t i = 0; i < kAwdlElPeers; i++) {
        const AwdlElPeer &p = awdlElPeer[i];
        if (!p.ms || !p.clkUs || nowMs - p.ms > 60000 || !memcmp(p.addr, f + 10, 6)) continue;
        if (!m || p.ms > m->ms) m = &p;
    }
    if (!m) { awdlFollowNone++; return false; }
    uint8_t *v4 = elFind(f, len, 4, 33);
    uint8_t *v24 = elFind(f, len, 24, 40);
    if (!v4 || !v24) return false;
    uint64_t now = elNowUs();
    uint64_t tu = now > m->clkUs ? (now - m->clkUs) / kElTuUs : 0;
    uint16_t next = (uint16_t)(kElSlotTu - tu % kElSlotTu);
    uint16_t aw = (uint16_t)(m->clkAw + (tu % kElSlotTu) / kElAwTu + 4 * (tu / kElSlotTu));
    uint32_t elapsed = kElSlotTu - next;                     // TU into the slot
    elPut16(v4 + 1, next);                                   // TX down-counter: TU to the next slot boundary
    elPut16(v4 + 15, (uint16_t)(elapsed < kElAwTu ? kElAwTu - elapsed : 0));   // remaining AW length
    memcpy(v4 + 21, m->addr, 6);                             // master address
    elPut16(v4 + 29, aw);                                    // AW sequence now
    elPut16(v4 + 31, aw);                                    // AP alignment, as Owly
    memcpy(v24, m->addr, 6);                                 // TLV 24: master, sync node, its counter, distance 1,
    memcpy(v24 + 6, m->addr, 6);                             // its metric, ours 60
    elPut32(v24 + 12, m->counter);
    elPut32(v24 + 16, 1);
    elPut32(v24 + 20, m->metric);
    elPut32(v24 + 24, 60);
    if (uint8_t *v5 = elFind(f, len, 5, 19)) {               // TLV 5: distance 1, top master, its metric, ours
        v5[3] = 1;
        memcpy(v5 + 5, m->addr, 6);
        elPut32(v5 + 11, m->metric);
        elPut32(v5 + 15, 60);
    }
    if (awdlFollowFixed < 3)
        m3log("awdl follow: master %02x%02x%02x%02x%02x%02x metric %u ctr %u aw %u next %u", m->addr[0], m->addr[1],
              m->addr[2], m->addr[3], m->addr[4], m->addr[5], m->metric, m->counter, aw, next);
    awdlFollowFixed++;
    return true;
}

// RX (itl_awdlrssi): the signal handed to the Family for a peer's AWDL frame, never weaker than -awdlRssiFloor dBm.
int8_t itlmt7925::awdlRssiAdj(int8_t r)
{
    if (!r) awdlRssiZero++;
    else {
        if (r < awdlRssiMin || !awdlRssiMin) awdlRssiMin = r;
        if (r > awdlRssiMax) awdlRssiMax = r;
    }
    if (!awdlRssiFloor) return r;
    int8_t floor = (int8_t)-(int)awdlRssiFloor;
    if (!r || r < floor) { awdlRssiRaised++; return floor; }
    return r;
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

// The operating class of a 20 MHz channel (IEEE 802.11 Annex E, global table as AWDL's opclass encoding uses it: 81, 115, 118,
// 121, 124, 125), 0 for a channel it does not cover.
static uint8_t elOpClass(uint8_t ch)
{
    if (ch >= 1 && ch <= 13) return 81;
    if (ch >= 36 && ch <= 48) return 115;
    if (ch >= 52 && ch <= 64) return 118;
    if (ch >= 100 && ch <= 144) return 121;
    if (ch >= 149 && ch <= 161) return 124;
    if (ch >= 165 && ch <= 177) return 125;
    return 0;
}

// One channel sequence (header: count-1, encoding, duplicate count, step count, fill channel u16; then 16 entries): every
// slot that names a channel names `ch` instead. Encodings 0 (1 byte: channel) and 3 (channel, operating class); the legacy
// encoding 1 (flags, channel) carries a width the AP's channel may not have, so it is left alone (false).
static bool elSeqToCh(uint8_t *s, uint32_t avail, uint8_t ch)
{
    if (avail < 6) return false;
    uint32_t n = (uint32_t)s[0] + 1, enc = s[1], es = enc == 0 ? 1 : 2;
    if ((enc != 0 && enc != 3) || n > 16 || 6 + n * es > avail) return false;
    uint8_t oc = elOpClass(ch);
    if (enc == 3 && !oc) return false;
    uint8_t *e = s + 6;
    bool any = false;
    for (uint32_t i = 0; i < n; i++, e += es) {
        if (!e[0]) continue;                                 // a slot we do not serve stays 0
        e[0] = ch;
        if (enc == 3) e[1] = oc;
        any = true;
    }
    if (!any) { e = s + 6; e[0] = ch; if (enc == 3) e[1] = oc; }   // never no slot at all: slot 0
    return true;
}

// itl_awdlonech bit 0 (§10.67): our PSF/MIF name only the AP's channel - TLV 4's channel of the next AW (+0), master
// channel (+3) and sequence (+33), and TLV 18. AWDL-12 in Owly's SPEC: a node advertises in each slot only the channel it
// will actually be on; with every window moved to the AP's channel (awdlSyncTx) that is the AP's channel in every slot the
// Family serves.
void itlmt7925::awdlSeqOneCh(uint8_t *f, uint32_t len)
{
    uint8_t ch = awdlParked ? awdlParkOn : tgtCh;           // itl_awdlpark: the park channel (§10.68)
    if (!(awdlParked || ((awdlOneCh & 1) && m3linkUp)) || !ch || len < 40 || f[0] != 0xd0) return;
    bool ok = true;
    if (uint8_t *v4 = elFind(f, len, 4, 33 + 6)) {
        uint32_t l4 = (uint32_t)v4[-2] | ((uint32_t)v4[-1] << 8);
        if (v4[0]) v4[0] = ch;
        if (v4[3]) v4[3] = ch;
        ok = elSeqToCh(v4 + 33, l4 - 33, ch) && ok;
    }
    if (uint8_t *v18 = elFind(f, len, 18, 6)) {
        uint32_t l18 = (uint32_t)v18[-2] | ((uint32_t)v18[-1] << 8);
        ok = elSeqToCh(v18, l18, ch) && ok;
    }
    if (ok) awdlOneChSeq++;
    else awdlOneChSeqFail++;
}

// §10.81 clock check (see itlmt7925.hpp). The master's own frames only (TLV 4 +21 = the sender). The frame's PHY TX time
// minus its target TX time (action header +8 / +12) is how late it left; the RX latency of our ring is not taken out, so
// the phase carries a few ms of noise, the AW number difference does not.
static uint64_t ckUptimeUs()
{
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000;
}

void itlmt7925::awdlClockCheck(const uint8_t *f, uint32_t len)
{
    if (!awdlAwPeriodUs || len < 40 + 3 + 31) return;
    const uint8_t *v = elFind((uint8_t *)f, len, 4, 31);
    if (!v || memcmp(v + 21, f + 10, 6) != 0) return;            // not a master speaking about itself
    uint64_t now = ckUptimeUs();
    uint32_t phy = (uint32_t)f[32] | ((uint32_t)f[33] << 8) | ((uint32_t)f[34] << 16) | ((uint32_t)f[35] << 24);
    uint32_t tgt = (uint32_t)f[36] | ((uint32_t)f[37] << 8) | ((uint32_t)f[38] << 16) | ((uint32_t)f[39] << 24);
    int32_t late = (int32_t)(phy - tgt);
    if (late < 0 || late > 20000) late = 0;
    uint32_t dc = (uint32_t)v[1] | ((uint32_t)v[2] << 8);
    uint16_t sn = (uint16_t)(v[29] | (v[30] << 8));
    uint32_t pm = (v[27] >= 1 && v[27] <= 16) ? v[27] : 4;
    uint16_t m = (uint16_t)(sn + 1);
    while (m % pm) m++;                                          // the AW number of its next extended window
    int64_t T = (int64_t)now + (int64_t)dc * 1024 - late;       // ... which starts then, on our clock
    int64_t per = (int64_t)awdlAwPeriodUs;
    int64_t el = T - (int64_t)awdlAwBaseUs;
    int64_t n = el >= 0 ? el / per : -((-el + per - 1) / per);
    int64_t frac = el - n * per;                                 // 0..per: how far into our AW n that instant is
    if (frac > per / 2) { n++; frac -= per; }                    // nearest boundary
    uint16_t ours = (uint16_t)(awdlAwBaseSeq + n);
    int32_t diff = (int16_t)(uint16_t)(m - ours);
    awdlCkN++;
    awdlCkHist[diff < -4 ? 0 : diff < 0 ? 1 : diff == 0 ? 2 : diff <= 4 ? 3 : 4]++;
    awdlCkLastDiff = diff;
    awdlCkLastPhase = (int32_t)frac;
    awdlCkLastDc = (int32_t)dc;
    awdlCkPhaseSum += frac < 0 ? -frac : frac;
    if (awdlCkN <= 4 || (awdlCkN % 200) == 0)
        m3log("awdl clock: master sn=%u dc=%u pm=%u -> its AW %u, ours %u (diff %d) phase %dus late=%dus n=%u", sn, dc, pm,
              m, ours, diff, (int)frac, late, awdlCkN);
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
    m3log("awdl park: on=%u parked=%u starts=%u fail=%u windows=%u renew=%u/%u refused joins=%u af=%u/%u | mc=%u/%u n=%u "
          "rc=%d/%d grp rx=%u", awdlParkOn, awdlParked, awdlParkN, awdlParkFail, awdlParkWin, awdlParkRenew, awdlParkRenewFail,
          awdlParkJoinRefused, awdlParkAfOk, awdlParkAfTx, awdlMc, awdlMcApplied, awdlMcN, awdlMcRc[0], awdlMcRc[1], awdlDataRxGrp);
    m3log("awdl clock: n=%u diff <-4/-4..-1/0/1..4/>4 = %u/%u/%u/%u/%u last diff=%d phase=%dus dc=%d avg|phase|=%dus", awdlCkN,
          awdlCkHist[0], awdlCkHist[1], awdlCkHist[2], awdlCkHist[3], awdlCkHist[4], awdlCkLastDiff, awdlCkLastPhase,
          awdlCkLastDc, awdlCkN ? (int)(awdlCkPhaseSum / awdlCkN) : 0);
    m3log("awdl follow: on=%u fixed=%u none=%u | txbcm=%u n=%u | rssi min=%d max=%d zero=%u floor=-%u raised=%u | scan cached=%u",
          awdlFollow, awdlFollowFixed, awdlFollowNone, txBcm, txBcmN, awdlRssiMin, awdlRssiMax, awdlRssiZero, awdlRssiFloor,
          awdlRssiRaised, awdlParkScanCached);
    m3log("awdl onech: on=%u ch=%u remap=%u seq=%u/%u uc wait=%u (flush calls) aged=%u snif=%u/%u", awdlOneCh, tgtCh,
          awdlOneChRemap, awdlOneChSeq, awdlOneChSeqFail, awdlUcGated, awdlUcAged, awdlSnifPersist, awdlSnifPersistOn);
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
