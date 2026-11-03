// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// AWDL front end implementation (see itlmt7925_awdl.h). Phase 2: the virtual interface exists and is registered; no RF work
// yet (phases 3-5 of docs/ROADMAP-AWDL.md: channel slicing, action frames, data path).
//
// Creation order (docs/ROADMAP-AWDL.md §9.5): init -> attach -> deferBSDAttach(true) -> start(ctl) -> register the ifnet ->
// registerService + setRunningState(false) -> setAwdlInterface -> deferBSDAttach(false). The start(ctl) call is mandatory:
// it is what stores the controller at ivars+0x30 (getController), which IO80211VirtualInterface::attachToBpf dereferences
// from the BSD client's start - skipping it panics on the first BSD attach (Kernel-2026-10-17-163303/165042).
#include "itlmt7925_awdl.h"
#include "itlmt7925_iface.h"

#include "../itlmt7925/io80211/IO80211AWDLProtocol.h"
#include "io80211_buffer_min.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSByteOrder.h>
#include <pexpert/pexpert.h>
#include <string.h>

// The PSF (sync frame) cadence. The peer manager logs "setSyncFramePeriod::Setting PSF period to 440" and hands the template
// over only when it changes, so the interface repeats the last one itself at that period. The ROC window only has to cover one
// management frame (a few ms); 30 ms leaves room for the channel switch without keeping the AP's channel away for long.
static const uint32_t kAwdlPsfRocMs = 100;

// The AWDL timeline (docs/ROADMAP-AWDL.md §10.28). On Apple hardware the firmware keeps it: it numbers the availability windows
// (AW, 16 TU), groups four of them into an extended window (EAW, 64 TU) that takes one entry of the 16-slot channel sequence,
// and posts APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START (0x29) to the Family at the start of every EAW it is present in. The
// peer manager's whole schedule (election checks, "Did not get AW start event", the timeline monitor, the MIF/PSF cadence)
// runs off those events, so without them it sat at "Self stuck at AW0". This chip has no AWDL firmware: the interface keeps
// the clock itself, free running while the Family says we are master and re-aligned to the master's sync parameters TLV while
// it says we are a slave. IO80211AWDLPeerManager::monitorTimeline expects the AW number to advance by 4 per EAW it hears
// about, skipping EAWs whose channel sequence entry is empty, exactly what awdlTlTick() posts.
static const uint32_t kAwdlTuUs = 1024;
static const uint32_t kAwdlAwUs = 16 * kAwdlTuUs;        // one availability window
static const uint32_t kAwdlAwPerEaw = 4;                  // presence mode 4: an EAW is four AWs
static const uint32_t kAwdlPsfEaws = 7;                   // PSF every 7 EAWs = 448 TU (the Family asks for 440)
static const uint32_t kAwdlMsgAwStart = 0x29;             // APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START

// apple80211_availability_window_start (0x24 bytes), as AppleBCMWLANProximityInterface::handleAvailabilityWindowStart builds
// it from the firmware event and IO80211AWDLPeerManager::reportDataPathEvents / monitorTimeline / logAWStart read it (25G241):
//   +0x00 the AW sequence number ("AwNumber", u16 in a u32)   +0x04 the sync role, in the Family's own SYNC_STATE numbering
//   +0x08 the master we are synced to (6 bytes)                (1 master, 2 slave, 3 non-election master)
//   +0x0e the top master (6 bytes)                             +0x14 the AW start time in microseconds, big endian
//   +0x18 an apple80211_channel {version 1, channel, flags}: the channel of this EAW
struct AwdlAwStart {
    uint32_t seq;
    uint32_t role;
    uint8_t  master[6];
    uint8_t  topMaster[6];
    uint32_t startUsBe;
    uint32_t chVersion;
    uint32_t channel;
    uint32_t chFlags;
};
static_assert(sizeof(AwdlAwStart) == 0x24, "apple80211_availability_window_start is 0x24 bytes in 25G241");

extern "C" IOReturn itlmtAwdlPostMessage(IO80211Controller *controller, IO80211SkywalkInterface *iface, uint32_t code,
                                         void *data, unsigned long length, bool flag)
    __asm__("__ZN17IO80211Controller11postMessageEP23IO80211SkywalkInterfacejPvmb");

static uint64_t awdlNowUs()
{
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000;                                   // the clock the core stamps received frames with
}

static uint32_t awdlChFlags(uint32_t ch) { return (ch <= 14 ? 0x8u : 0x10u) | 0x2u; }   // band | 20 MHz

// The 802.11 + AWDL action header that the driver has to put in front of the Family's TLV blob (docs/ROADMAP-AWDL.md §10.11).
// On Apple hardware the firmware writes it; here the interface builds it: 24-byte management/action header (broadcast DA, the
// AWDL address as SA, the AWDL BSSID) followed by the 16-byte awdl_action (category 0x7f, Apple OUI 00:17:f2, type 8, version
// 1.0, subtype PSF). Reference: OWL src/frame.h + src/tx.c, and the Family's own parser (actionFrameHasCorrectOUI compares the
// same 0x17/0xf2/8 bytes at the same offsets). The sequence number and the two timestamps are stamped by the core per TX.
static const uint32_t kAwdlAfHdrLen = 24 + 16;
static const uint8_t kAwdlBssidDefault[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };

// apple80211_act_frm_hdr: the receive descriptor the Family's AWDL peer manager wants next to the AWDL action body
// (docs/ROADMAP-AWDL.md §10.26). Its layout is not public; it was read out of 25G241: Apple's own driver builds it on the stack
// in AppleBCMWLANProximityInterface::handleActionFrame_rx (bzero of 0x48 bytes, then the fields below) and hands it to
// IO80211VirtualInterface::bpfTapInput(buf, 0xc, &hdr, 0x48); the Family reads it in these places:
//   +0x00  the sender's address (from the firmware event's addr) - findPeer(), addAWDLPeer() -> withAddressAndManager()
//   +0x0c  an apple80211_channel {version 1, channel, flags} (ChanSpecConvToApple80211Channel writes it there) - updateAFStats()
//          and determinePeerReachability() compare the channel at +0x10 with 14 / 15
//   +0x28  the receive rate (the firmware's rx_frame_data rate, byte-swapped)
//   +0x2c  the RSSI in dBm (rx_frame_data rssi, byte-swapped). actionFrameProcessInput() only creates a peer when it is at or
//          above the edge threshold (ivars+0x281c, -70 by default, setAwdlElectionRssiThresholds), and
//          determinePeerReachability() answers "unreachable" for an RSSI of 0 - so it must be a real, non-zero value
//   +0x30  the receive time (rx_frame_data mactime)
//   +0x40  1 for the v2 event (rx_frame_data version 2), +0x44 four more bytes of it; left zero here (v1 layout)
// The whole 0x48 bytes are copied: actionFrameInput() memmove()s exactly that many into its AF queue (and traps on more), and
// postAFToP2pd() hands them to wifip2pd in front of the body.
struct AwdlActFrmHdr {
    uint8_t  sa[6];             // +0x00  the sender (the peer)
    uint8_t  rsvd0[6];          // +0x06  not written by Apple's driver
    uint32_t chVersion;         // +0x0c  apple80211_channel.version (1)
    uint32_t channel;           // +0x10  apple80211_channel.channel
    uint32_t chFlags;           // +0x14  apple80211_channel.flags: band (0x8 2.4 GHz / 0x10 5 GHz) | 0x2 (20 MHz)
    uint8_t  rsvd1[0x10];       // +0x18
    uint32_t rate;              // +0x28
    int32_t  rssi;              // +0x2c  dBm
    uint32_t rxTime;            // +0x30  microseconds
    uint8_t  rsvd2[0x14];       // +0x34
};
static_assert(sizeof(AwdlActFrmHdr) == 0x48, "apple80211_act_frm_hdr is 0x48 bytes in 25G241");

// IO80211VirtualInterface::bpfTapInput(IO80211Buffer*, uint type, void *hdr, ulong hdrLen) - the Family's own receive entry
// for management frames on a virtual interface. With type 0xc it hands the frame to the interface's peer manager
// (actionFrameInput, vtable +0x238) and otherwise to BPF; either way the Family owns the buffer from then on and returns it to
// its pool itself (IO80211Buffer::returnBuffer -> pool->returnBuffer -> release). This is the call Apple's driver makes.
static const uint32_t kAwdlBpfTypeActionFrame = 0xc;

// The receive buffers. One fresh buffer per frame: the Family returns each buffer to its pool once it is done with it (right
// away, or after its AF worker thread processed the queued frame), so a buffer must never be written again after the call.
// The pool is shared by every AWDL interface instance and lives as long as the kext: a buffer the Family still holds when an
// interface goes away is returned to a pool that still exists.
static const uint32_t kAwdlRxPoolCount = 16;
static const uint32_t kAwdlRxBufCap = 2048;      // a MIF with all its service TLVs stays below 1600 bytes
static IO80211DynamicBufferPool *gAwdlRxPool = nullptr;

// -----------------------------------------------------------------------------------------------------------------------
// The virtual interface: IO80211VirtualInterface with the AWDL BSD name prefix.
//
// Apple's Broadcom path instantiates AppleBCMWLANProximityInterface (IO80211AWDLProtocol : IO80211VirtualInterface) whose
// getBSDNamePrefix returns "awdl" and whose AWDL protocol slots talk to firmware. The Family's generic
// IO80211VirtualInterface would take the default prefix ("en", IOSkywalkEthernetInterface::getBSDNamePrefix) and could never
// show up as awdl0, so this subclass overrides the prefix and leaves everything else to the Family (init, attach, the ifnet
// registration and - later - createPeerManager -> IO80211AWDLPeerManager -> IO80211P2PSupervisor).
// -----------------------------------------------------------------------------------------------------------------------
// Base is the Family's AWDL protocol class (not plain IO80211VirtualInterface): the AWDL ioctl dispatchers cast the
// interface to IO80211AWDLProtocol (apple80211setCOMPANION_SKYWALK_IF_CREATE at 0xffffff800226a32f: safeMetaCast
// against IO80211AWDLProtocol::gMetaClass, then the slot-562 call) and the AWDL peer manager drives the protocol
// virtuals. Without the subclass link every AWDL ioctl answers 0xe082280e before reaching this driver.
class itlmt7925AwdlInterface : public IO80211AWDLProtocol {
    OSDeclareDefaultStructors(itlmt7925AwdlInterface)

public:
    // IOSkywalkNetworkInterface::getBSDNamePrefix (slot 319): the BSD name the Family builds is <prefix><unit>.
    const char *getBSDNamePrefix() override;
    // Slot 490 is ___cxa_pure_virtual in Apple's binary and in IO80211AWDLProtocol (no override anywhere in the Family - the
    // Broadcom subclass implements it). Nothing on this path calls it; implemented so the subclass is concrete.
    void _unnamedPure490() override;
    // ---------------------------------------------------------------------------------------------------------------
    // IO80211AWDLProtocol (slots 507..569): the Family's AWDL ioctls and the AWDL peer manager call these. Phase 3
    // groundwork: each logs its first calls and answers kIOReturnUnsupported, so the Family's ioctls fail cleanly
    // instead of hitting an unimplemented pure virtual. The companion create (562) is in the .cpp.
    IOReturn awdlProto(const char *name);
    // Phase 3 step 2 - the configure handshake. IO80211AWDLPeerManager::configure aborts on the first slot that answers
    // unsupported: the trace on 2026-10-19 21:27 shows "configure Unable to set BSSID ret e00002c7" -> "doEnable:2839
    // ERROR: cannot configure AWDL after enabling interface" -> AWDL disabled, i.e. slot 540 (setAWDL_BSSID) alone kept the
    // whole stack down. awdlAccept() answers success and traces the slot; the values whose layout is public (the four AWDL
    // structs in IOKit/80211/apple80211_ioctl.h) are stored/printed as well. The configure sequence is only a parameter
    // handshake - the RF work (channel slicing, PSF frames, peer table) is what follows it.
    IOReturn awdlAccept(const char *name);
    /* 507 */ IOReturn getSSID(apple80211_ssid_data*a1) override { return awdlProto("getSSID"); }
    /* 508 */ IOReturn getCHANNEL(apple80211_channel_data*a1) override { return awdlProto("getCHANNEL"); }
    /* 509 */ IOReturn getBSSID(apple80211_bssid_data*a1) override { return awdlProto("getBSSID"); }
    /* 510 */ IOReturn getSTATE(apple80211_state_data*a1) override { return awdlProto("getSTATE"); }
    /* 511 */ IOReturn getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*a1) override { return awdlProto("getAWDL_SYNC_PARAMS"); }
    /* 512 */ IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*a1) override;
    /* 513 */ IOReturn getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*a1) override { return awdlProto("getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS"); }
    /* 514 */ IOReturn getAWDL_ELECTION_ID(apple80211_awdl_election_id*a1) override { return awdlProto("getAWDL_ELECTION_ID"); }
    /* 515 */ IOReturn getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*a1) override { return awdlProto("getAWDL_ELECTION_METRIC"); }
    /* 516 */ IOReturn getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*a1) override;
    /* 517 */ IOReturn getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size*a1) override { return awdlProto("getPEER_CACHE_MAXIMUM_SIZE"); }
    /* 518 */ IOReturn getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*a1) override;
    /* 519 */ IOReturn getAWDL_SYNC_STATE(apple80211_awdl_sync_state*a1) override;
    /* 520 */ IOReturn getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*a1) override { return awdlProto("getAWDL_ELECTION_RSSI_THRESHOLDS"); }
    /* 521 */ IOReturn getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*a1) override { return awdlProto("getAWDL_PRESENCE_MODE"); }
    /* 522 */ IOReturn getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*a1) override { return awdlProto("getAWDL_ELECTION_PARAMS"); }
    /* 523 */ IOReturn getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities*a1) override { return awdlProto("getAWDL_DEVICE_CAPABILITIES"); }
    /* 524 */ IOReturn getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*a1) override { return awdlProto("getAWDL_SECONDARY_MASTER_CHANNEL"); }
    /* 525 */ IOReturn getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*a1) override { return awdlProto("getAWDL_AF_TX_MODE"); }
    /* 526 */ IOReturn getAWDL_MIN_RATE(apple80211_awdl_min_rate*a1) override { return awdlProto("getAWDL_MIN_RATE"); }
    /* 527 */ IOReturn getAWDL_ENCRYPTION_TYPE(apple80211_encrypt*a1) override { return awdlProto("getAWDL_ENCRYPTION_TYPE"); }
    /* 528 */ IOReturn getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*a1) override { return awdlProto("getAWDL_TXCAL_PERIOD"); }
    /* 529 */ IOReturn getAWDL_DEV_STATS(apple80211_awdl_dev_stats*a1) override { return awdlProto("getAWDL_DEV_STATS"); }
    /* 530 */ IOReturn getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*a1) override { return awdlProto("getAWDL_DFSP_CONFIG"); }
    /* 531 */ IOReturn getAWDL_CCA(apple80211_awdl_cca*a1) override { return awdlProto("getAWDL_CCA"); }
    /* 532 */ IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap*a1) override { return awdlProto("getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES"); }
    /* 533 */ IOReturn getLLW_PARAMS(apple80211_llw_params*a1) override { return awdlProto("getLLW_PARAMS"); }
    /* 534 */ IOReturn setSSID(apple80211_ssid_data*a1) override { return awdlAccept("setSSID"); }
    /* 535 */ IOReturn setCHANNEL(apple80211_channel_data*a1) override { return awdlAccept("setCHANNEL"); }
    /* 536 */ IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*a1) override;
    /* 537 */ IOReturn setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*a1) override;
    /* 538 */ IOReturn setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*a1) override { return awdlAccept("setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS"); }
    /* 539 */ IOReturn setAWDL_ELECTION_ID(apple80211_awdl_election_id*a1) override { return awdlAccept("setAWDL_ELECTION_ID"); }
    /* 540 */ IOReturn setAWDL_BSSID(apple80211_awdl_bssid*a1) override;
    /* 541 */ IOReturn setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*a1) override;
    /* 542 */ IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*a1) override;
    /* 543 */ IOReturn setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*a1) override;
    /* 544 */ IOReturn setAWDL_SYNC_STATE(apple80211_awdl_sync_state*a1) override;
    /* 545 */ IOReturn setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*a1) override { return awdlAccept("setAWDL_ELECTION_RSSI_THRESHOLDS"); }
    /* 546 */ IOReturn setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*a1) override;
    /* 547 */ IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control*a1) override { return awdlAccept("setPEER_CACHE_CONTROL"); }
    /* 548 */ IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*a1) override { return awdlOob((const uint8_t *)a1, false); }
    /* 549 */ IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template*a1) override;
    /* 550 */ IOReturn setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*a1) override;
    /* 551 */ IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration*a1) override;
    /* 552 */ IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*a1) override { return awdlAccept("setAWDL_SECONDARY_MASTER_CHANNEL"); }
    /* 553 */ IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*a1) override { return awdlAccept("setAWDL_AF_TX_MODE"); }
    /* 554 */ IOReturn setAWDL_MIN_RATE(apple80211_awdl_min_rate*a1) override { return awdlAccept("setAWDL_MIN_RATE"); }
    /* 555 */ IOReturn setAWDL_ENCRYPTION_KEYS(apple80211_key*a1) override { return awdlAccept("setAWDL_ENCRYPTION_KEYS"); }
    /* 556 */ IOReturn setAWDL_ENCRYPTION_TYPE(apple80211_encrypt*a1) override { return awdlAccept("setAWDL_ENCRYPTION_TYPE"); }
    /* 557 */ IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request*a1) override { return awdlOob((const uint8_t *)a1, true); }
    /* 558 */ IOReturn setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*a1) override { return awdlAccept("setAWDL_TXCAL_PERIOD"); }
    /* 559 */ IOReturn setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add*a1) override;
    /* 473 */ void setAwdlCurrentChannelSequenceIndex(uint32_t a1) override;
    /* 560 */ IOReturn setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*a1) override { return awdlAccept("setAWDL_DFSP_CONFIG"); }
    /* 561 */ IOReturn setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config*a1) override { return awdlAccept("setAWDL_DFSP_UCSA_CONFIG"); }
    /* 562 */ IOReturn setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create*a1) override;
    /* 563 */ IOReturn setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request*a1) override { return awdlProto("setAWDL_OOB_SEC_PAYLOAD_REQUEST"); }
    /* 564 */ IOReturn setLLW_PARAMS(apple80211_llw_params*a1) override { return awdlProto("setLLW_PARAMS"); }
    /* 565 */ IOReturn setTX_RATE(apple80211_tx_rate*a1) override { return awdlAccept("setTX_RATE"); }
    /* 566 */ IOReturn setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection*a1) override { return awdlAccept("setP2P_COEX_PROTECTION"); }
    /* 567 */ IOReturn setPSF_DWELL(apple80211_awdl_psf_dwell_params*a1) override { return awdlAccept("setPSF_DWELL"); }
    /* 568 */ IOReturn setAWDL_CONFIG(apple80211_awdl_config_request*a1) override { return awdlAccept("setAWDL_CONFIG"); }
    /* 569 */ IOReturn setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg*a1) override { return awdlAccept("setAWDL_RTG_PEER_STATS_CONFIG"); }

    // Phase 3, step 2: the core's link thread transmits the PSF frame (ROC on the AWDL channel + a management TX). The core
    // service pointer comes from the vif-create handler, which runs in the infrastructure interface that owns it.
    void setNativeCore(IOService *c) { coreDev = c; }
    // The PSF repeats once per sync period (440 ms), not once per template: the Family only hands the template over when it
    // changes. awdlPsfStart() arms the timer, awdlPsfTick() submits one frame per period while AWDL is enabled.
    void awdlPsfStart();
    void awdlPsfTick();
    void awdlTlTick(uint64_t now);                          // one EAW boundary: AW start event, PSF when due
    void awdlTlArm(uint64_t now);                           // timer to the next EAW boundary
    void awdlTlSync(const uint8_t *frame, const uint8_t *body, uint32_t bodyLen, uint32_t rxTimeUs);
    static void awdlPsfTimerFired(OSObject *owner, IOTimerEventSource *sender);
    // Out-of-band action frames (docs/ROADMAP-AWDL.md §10.29): the master indication frame (MIF) and one-shot requests the
    // Family hands over through setAWDL_OOB_AUTO_REQUEST / setAWDL_OOB_REQUEST. Apple's firmware transmits them itself
    // ("awdl_oob_af_auto"); here they ride in the same ROC window as the PSF.
    IOReturn awdlOob(const uint8_t *req, bool autoReq);
    // The frame the driver puts on the air: 802.11 + AWDL action header, the synchronisation parameters TLV (type 4), then
    // the Family's TLVs. Apple's firmware inserts TLV 4 into every PSF/MIF; the Family's templates do not carry it.
    uint32_t awdlBuildSyncTlv(uint8_t *out, uint8_t txCh);
    uint32_t awdlBuildFrame(uint8_t *out, uint32_t cap, uint8_t subtype, const uint8_t *da, const uint8_t *tlv,
                            uint32_t tlvLen, uint8_t txCh);
    void awdlPublishState();                                // ioreg "itlmt-awdl": the AWDL state at a glance
    // Phase 3 receive half: action frames the radio heard on the social channel are queued by the core and handed to the
    // Family's io80211 peer manager here, which is what turns them into peers / sync / service requests.
    void awdlPollRx();
    void awdlDeliverRx(const uint8_t *frame, uint32_t len, uint32_t channel, int32_t rssi, uint32_t rxTimeUs);
    // A fresh Family buffer with the AWDL action body written into it (docs/ROADMAP-AWDL.md §10.26).
    IO80211Buffer *awdlRxBuffer(const uint8_t *body, uint32_t len);
    void free() override;

    // Creates the pool and the (three) queues and registers the ifnet from a RegistrationInfo filled like
    // AppleBCMWLANCore::createProxInterface fills it. Keeps the objects for the interface's lifetime.
    IOReturn registerAwdlInterface(const uint8_t *mac);
    void freeAwdlRegistration();

private:
    uint32_t awdlProtoLogged = 0;              // trace budget for the AWDL protocol stubs
    uint32_t awdlAccepted = 0;                 // trace budget for the accepted (configure-handshake) slots
    uint32_t awdlAdvertLogged = 0;             // trace budget for the advertise parameters (service request / MIF source)
    uint32_t awdlSyncEnabled = 0;              // last setAWDL_SYNC_ENABLED value, answered back by getAWDL_SYNC_ENABLED
    uint8_t awdlBssid[6] = {};                 // last setAWDL_BSSID bytes (traced only: the layout is not public)
    uint8_t awdlSelfAddr[6] = {};              // the AWDL interface address: SA of every frame sent on the AWDL channel
    uint32_t awdlElectionMetric = 0;           // last setAWDL_ELECTION_METRIC
    uint32_t awdlSyncFrameLen = 0;             // last setAWDL_SYNC_FRAME_TEMPLATE payload_len (phase 4: the PSF frame)
    uint32_t awdlSyncTxLogged = 0;             // trace budget for the core handoff
    IOTimerEventSource *awdlPsfTimer = nullptr; // repeats the PSF frame at the sync period
    // The last PSF template's TLVs (the Family's buffer is not ours to keep). The frame itself is built at every transmission,
    // because its sync parameters TLV names the channel it goes out on (docs/ROADMAP-AWDL.md §10.29).
    uint8_t awdlPsfTlv[1200] = {};
    uint32_t awdlPsfTlvLen = 0;
    uint32_t awdlPsfLen = 0;                   // nonzero while there is a PSF to send (the length of the last built frame)
    uint8_t awdlTxPsf[1600] = {};              // frames being handed to the core (copied there synchronously)
    uint8_t awdlTxMif[1600] = {};
    // The MIF / one-shot OOB frame (setAWDL_OOB_AUTO_REQUEST / setAWDL_OOB_REQUEST): its TLVs and destination.
    uint8_t awdlMifTlv[1400] = {};
    uint32_t awdlMifTlvLen = 0;
    uint8_t awdlMifDa[6] = {};
    bool awdlMifOneShot = false;
    uint32_t awdlOobAuto = 0, awdlOobOnce = 0, awdlOobStop = 0, awdlOobTooLong = 0, awdlOobLogged = 0;
    uint32_t awdlMifSubmitted = 0;
    uint8_t awdlOobHdr[0x30] = {};             // the last request's header (the payload pointer at +0x30 is not kept)
    uint8_t awdlSyncParamsRaw[0x18] = {};      // setAWDL_SYNC_PARAMS (0x18 bytes: AppleBCMWLANProximityInterface reads +4..+0x14)
    bool awdlSyncParamsSet = false;
    uint32_t awdlSyncVer = 3;                  // ItlNativeAwdlSync version the core accepts (3, 2 or 1 for an older core)
    uint32_t awdlTlv4Inserted = 0, awdlTlv4Present = 0;
    bool awdlSelfTlv4Published = false, awdlPeerTlv4Published = false;
    uint32_t awdlTlv4FromPeer = 0;
    uint32_t awdlPsfTicks = 0;                 // trace/metrics counter
    // The channels the sync frame is repeated on. Apple's AWDL spreads the PSF over a channel sequence (2.4 GHz social channel 6,
    // 5 GHz 44/149) and the Family hands that sequence down through setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE. Until it does - and
    // on this driver, which cannot yet report dual-band device capabilities, it may only ever hand ch6 over - the default set is
    // used, because a peer that sits on 44/149 is otherwise inaudible: the firmware only ever hands us frames from the channel we
    // park on (docs/ROADMAP-AWDL.md §10.13, §10.14).
    uint8_t awdlChans[8] = { 6, 44, 149, 0, 0, 0, 0, 0 };
    uint8_t awdlChanN = 3;
    uint8_t awdlChanIdx = 0;                   // round-robin cursor over awdlChans, advanced by every PSF tick
    uint32_t awdlSeqIndex = 0;                 // channel-sequence index the Family is currently on (slot 473)
    bool awdlSeqIndexSet = false;              // the Family has begun driving the index: follow it instead of rotating
    // The four AWDL state structs whose layout is not public are stored verbatim when the Family sets them and echoed back when
    // it reads them. Returning unsupported to the read-back is what kept configure from ever reaching the sync template.
    uint8_t awdlRawSeq[0xd0] = {};             // setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE / get...
    uint16_t awdlRawSeqLen = 0;
    uint8_t awdlRawMaster[16] = {};            // setAWDL_MASTER_CHANNEL / getAWDL_MASTER_CHANNEL
    uint8_t awdlRawMasterLen = 0;
    uint8_t awdlRawState[16] = {};             // setAWDL_SYNC_STATE / getAWDL_SYNC_STATE
    uint8_t awdlRawStateLen = 0;
    uint32_t awdlRxFrames = 0, awdlRxLogged = 0;   // action frames handed to the peer manager / trace budget
    uint32_t awdlRxBufFail = 0;                  // times the Family's pool could not give a buffer
    uint32_t awdlRxNoRssi = 0;                   // frames whose RX vector gave no signal (the core could not decode RCPI)
    uint32_t awdlRxWeak = 0;                     // frames below the peer manager's -70 dBm edge (no new peer from them)
    uint32_t awdlRxTrim = 0;                     // frames with bytes after the last TLV (cut to the TLV end)
    uint32_t awdlRxBadTlv = 0;                   // frames whose TLVs do not parse (dropped)
    uint32_t awdlRxVer = 2;                      // ItlNativeAwdlRx version the core accepts (2, or 1 for an older core)
    uint8_t awdlRxScratch[1600] = {};            // one received frame (MIFs are 600-1000+ bytes; a stack buffer of 512 cut them)
    // The AW timeline (see kAwdlAwUs). Window number awdlTlBaseSeq starts at host uptime awdlTlBaseUs.
    bool awdlTlRunning = false;
    uint64_t awdlTlBaseUs = 0;
    uint16_t awdlTlBaseSeq = 0;
    int32_t awdlTlLastPosted = -1;               // AW number of the last AW start event (one per EAW)
    uint32_t awdlTlEaws = 0, awdlTlPosted = 0, awdlTlPostFail = 0, awdlTlLate = 0;
    uint32_t awdlTlSyncs = 0, awdlTlSyncLogged = 0;
    int32_t awdlTlLastAdjUs = 0;                 // how far the last re-alignment moved the clock
    uint32_t awdlPsfDue = 0;                     // EAWs until the next PSF window
    uint16_t awdlTlFlags = 0;                    // bit 0: the sync TLV's sequence number names the next AW (itlmt_awdl_snnext=1)
    // The sync role the Family set (setAWDL_SYNC_STATE: +4 the state, +8 the master's address) and the top master it named in
    // setAWDL_ELECTION_PARAMS (+0x16); both go into every AW start event, which the timeline monitor checks against them.
    uint32_t awdlRole = 1;                       // 1 master, 2 slave, 3 non-election master
    uint8_t awdlMasterAddr[6] = {};
    uint8_t awdlTopMaster[6] = {};
    uint64_t awdlTlDirectUs = 0;                 // last time a frame from the master itself re-aligned the clock
    // The channel of each of the 16 sequence slots, from setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE (0 = not present).
    uint8_t awdlSlotCh[16] = { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 };
    bool awdlSlotMulti = false;                  // the sequence has more than one distinct channel
    IOService *coreDev = nullptr;              // the core service (itlmt-native-awdl-sync-v1)
    IOSkywalkPacketBufferPool *awdlPool = nullptr;
    IOSkywalkPacketQueue *awdlQueues[3] = {};
};

#undef super
#define super IO80211AWDLProtocol
OSDefineMetaClassAndStructors(itlmt7925AwdlInterface, IO80211AWDLProtocol)

const char *itlmt7925AwdlInterface::getBSDNamePrefix() { return "awdl"; }
void itlmt7925AwdlInterface::_unnamedPure490() {}

// The AWDL protocol slots (507..569) answer unsupported until the real implementation (channel sequence, PSF frames, peer
// table - phases 3/4). The trace goes through the controller's itlmt_native_trace ring (mirrored to NVRAM with
// itlmt_native_nvtrace=1): IOLog from this kext does not reach the unified log on this machine, so NVRAM/ioreg is the only
// way to see which slots the Family and the AWDL peer manager actually call, in what order.
IOReturn itlmt7925AwdlInterface::awdlProto(const char *name)
{
    // Called once a second by the Family while it waits for an answer it will never get; tracing it once was enough and the
    // repeats evicted every useful line from the NVRAM ring.
    if (awdlProtoLogged < 96) {
        awdlProtoLogged++;
        itlmtTrace(getController(), "awdl:proto %s", name);
    }
    return kIOReturnUnsupported;
}

// -----------------------------------------------------------------------------------------------------------------------
// Phase 3 step 2: the configure handshake (see the class comment). The peer manager hands the driver its parameters through
// these slots and aborts the whole configure sequence on the first unsupported answer, so they accept (and the four values
// whose layout is public are stored/printed). This is deliberately not RF work: no channel is switched and no frame is
// sent yet - it is what lets configure() finish so the next missing piece (the channel sequence / PSF path) becomes visible
// in the trace.
// -----------------------------------------------------------------------------------------------------------------------
IOReturn itlmt7925AwdlInterface::awdlAccept(const char *name)
{
    if (awdlAccepted < 96) {
        awdlAccepted++;
        itlmtTrace(getController(), "awdl:accept %s", name);
    }
    return kIOReturnSuccess;
}

// apple80211_awdl_sync_enabled { u32 version; u32 unk1; u32 enabled } (IOKit/80211/apple80211_ioctl.h, packed).
IOReturn itlmt7925AwdlInterface::setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *a1)
{
    awdlSyncEnabled = a1 ? *(const uint32_t *)((const uint8_t *)a1 + 8) : 1;
    itlmtTrace(getController(), "awdl:set SYNC_ENABLED=%u", (unsigned)awdlSyncEnabled);
    if (awdlSyncEnabled) awdlPsfStart();                    // AWDL on: start repeating the sync frame
    else {                                                   // AWDL off: nothing may leave the radio, no AW events
        if (awdlPsfTimer) awdlPsfTimer->cancelTimeout();
        awdlTlRunning = false;
    }
    return kIOReturnSuccess;
}

IOReturn itlmt7925AwdlInterface::getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *a1)
{
    if (a1) {
        *(uint32_t *)((uint8_t *)a1 + 0) = 1;               // version
        *(uint32_t *)((uint8_t *)a1 + 4) = 0;               // unk1
        *(uint32_t *)((uint8_t *)a1 + 8) = awdlSyncEnabled; // enabled
    }
    return kIOReturnSuccess;
}

// Slot 540 - the first call that failed ("configure Unable to set BSSID ret e00002c7"). The struct is u32 version + the
// BSSID the peer manager derived for this interface; the address part is read as six bytes at +4 (the layout is not in the
// public header) and only traced, so a wrong guess cannot corrupt anything.
IOReturn itlmt7925AwdlInterface::setAWDL_BSSID(apple80211_awdl_bssid *a1)
{
    if (a1) {
        memcpy(awdlBssid, (const uint8_t *)a1 + 4, sizeof(awdlBssid));
        itlmtTrace(getController(), "awdl:set BSSID %02x:%02x:%02x:%02x:%02x:%02x",
                   awdlBssid[0], awdlBssid[1], awdlBssid[2], awdlBssid[3], awdlBssid[4], awdlBssid[5]);
    } else {
        itlmtTrace(getController(), "awdl:set BSSID (null)");
    }
    return kIOReturnSuccess;
}

// apple80211_awdl_sync_channel_sequence - the channels the Family wants the sync frame repeated on. Layout as the Family's own
// AppleBCMWLANProximityInterface::buildAWDLChanSequence() reads it (25G241): {u32 version; u8 ; u8 count; u8 ; u8 ; u8 ;
// u8 masterChannel (0xff = none); u16 pad; entries[] at +0x0c}, where each entry is a plain apple80211_channel
// {u32 version; u32 channel; u32 flags} - the builder takes the low byte of entry+4 as the channel and tests entry+8 for the
// band bits (0x8 2.4 GHz, 0x10 5 GHz). Count is clamped and every read stays inside the struct, and the raw bytes are traced
// so a wrong guess shows up in the log instead of silently parking the radio somewhere odd.
IOReturn itlmt7925AwdlInterface::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *a1)
{
    const uint8_t *s = (const uint8_t *)a1;
    if (!s) return kIOReturnBadArgument;
    memcpy(awdlRawSeq, s, sizeof(awdlRawSeq));           // answered back verbatim by the getter below
    awdlRawSeqLen = sizeof(awdlRawSeq);
    uint32_t n = s[5];
    if (n > 16) n = 16;
    uint8_t master = s[9];
    uint8_t list[8];
    uint32_t len = 0;
    if (master >= 1 && master <= 196) list[len++] = master;   // the master channel first: it is where the sync tree lives
    for (uint32_t i = 0; i < n; i++) {
        uint8_t ch = s[0xc + 12 * i + 4];
        if (ch < 1 || ch > 196) continue;
        bool dup = false;
        for (uint32_t j = 0; j < len; j++) if (list[j] == ch) dup = true;
        if (!dup && len < 8) list[len++] = ch;
    }
    // Union with the social channels this driver falls back to. This is about discovery: a peer sitting on 44 or 149 cannot be
    // heard while the radio only parks on ch6, and the sequence the Family hands over may be built from capabilities this
    // driver still reports as unsupported (getAWDL_DEVICE_CAPABILITIES, slot 523).
    const uint8_t fallback[3] = { 6, 44, 149 };
    for (int i = 0; i < 3; i++) {
        bool dup = false;
        for (uint32_t j = 0; j < len; j++) if (list[j] == fallback[i]) dup = true;
        if (!dup && len < 8) list[len++] = fallback[i];
    }
    // The slot table the timeline walks: entry i of the sequence is slot i (a shorter sequence repeats).
    if (n) {
        uint8_t first = 0;
        bool multi = false;
        for (uint32_t i = 0; i < 16; i++) {
            uint8_t ch = s[0xc + 12 * (i % n) + 4];
            if (ch > 196) ch = 0;
            awdlSlotCh[i] = ch;
            if (ch && !first) first = ch;
            else if (ch && ch != first) multi = true;
        }
        awdlSlotMulti = multi;
    }
    if (len) {
        memcpy(awdlChans, list, len);
        awdlChanN = (uint8_t)len;
        if (awdlChanIdx >= awdlChanN) awdlChanIdx = 0;
    }
    if (awdlAccepted < 96) {
        awdlAccepted++;
        itlmtTrace(getController(),
                   "awdl:set CHANSEQ n=%u master=%u hop=%u,%u,%u,%u,%u e0=%u/%03x e1=%u/%03x e2=%u/%03x",
                   (unsigned)n, (unsigned)master, awdlChans[0], awdlChans[1], awdlChans[2], awdlChans[3], awdlChans[4],
                   (unsigned)s[0x10], (unsigned)(s[0x14] | (s[0x15] << 8) | (s[0x16] << 16)),
                   n > 1 ? (unsigned)s[0x1c] : 0, n > 1 ? (unsigned)(s[0x20] | (s[0x21] << 8) | (s[0x22] << 16)) : 0,
                   n > 2 ? (unsigned)s[0x28] : 0, n > 2 ? (unsigned)(s[0x2c] | (s[0x2d] << 8) | (s[0x2e] << 16)) : 0);
    }
    return kIOReturnSuccess;
}

// The Family reads the AWDL state structs back after setting them; answering kIOReturnUnsupported there stopped the configure
// sequence right after setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE and the sync template was never handed over. The payloads are
// echoed verbatim (the layouts are not public, so a verbatim copy is the only truthful answer), with a valid default sequence
// when nothing has been set yet.
IOReturn itlmt7925AwdlInterface::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *a1)
{
    if (!a1) return kIOReturnBadArgument;
    uint8_t *b = (uint8_t *)a1;
    if (!awdlRawSeqLen) {
        memset(b, 0, sizeof(awdlRawSeq));
        b[0] = 1;                                        // version
        b[5] = 1;                                        // one channel
        b[9] = 0xff;                                     // no master channel
        b[0x0c] = 1;                                     // entry.version
        b[0x10] = 6;                                     // entry.channel
        b[0x14] = 0x0a;                                  // entry.flags: 2.4 GHz | 20 MHz
        return kIOReturnSuccess;
    }
    memcpy(b, awdlRawSeq, awdlRawSeqLen);
    return kIOReturnSuccess;
}

// apple80211_awdl_master_channel / apple80211_awdl_sync_state: stored and echoed, never guessed at.
IOReturn itlmt7925AwdlInterface::setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *a1)
{
    if (a1) {
        memcpy(awdlRawMaster, a1, sizeof(awdlRawMaster));
        awdlRawMasterLen = sizeof(awdlRawMaster);
    }
    if (awdlAccepted < 96) {
        awdlAccepted++;
        itlmtTrace(getController(), "awdl:set MASTER_CHANNEL %02x%02x%02x%02x %02x%02x%02x%02x",
                   awdlRawMaster[0], awdlRawMaster[1], awdlRawMaster[2], awdlRawMaster[3],
                   awdlRawMaster[4], awdlRawMaster[5], awdlRawMaster[6], awdlRawMaster[7]);
    }
    return kIOReturnSuccess;
}

IOReturn itlmt7925AwdlInterface::getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *a1)
{
    if (!a1) return kIOReturnBadArgument;
    uint8_t *b = (uint8_t *)a1;
    if (!awdlRawMasterLen) {
        memset(b, 0, sizeof(awdlRawMaster));
        *(uint32_t *)b = 1;                              // version
        b[4] = 6;                                        // channel
    } else {
        memcpy(b, awdlRawMaster, awdlRawMasterLen);
    }
    return kIOReturnSuccess;
}

IOReturn itlmt7925AwdlInterface::setAWDL_SYNC_STATE(apple80211_awdl_sync_state *a1)
{
    if (a1) {
        memcpy(awdlRawState, a1, sizeof(awdlRawState));
        awdlRawStateLen = sizeof(awdlRawState);
        // apple80211_awdl_sync_state, as AppleBCMWLANProximityInterface::setAWDL_SYNC_STATE reads it (25G241): +4 the state
        // (1 master, 2 slave, 3 non-election master), +8 the address of the master to follow (6 bytes).
        uint32_t role = *(const uint32_t *)(awdlRawState + 4);
        if (role >= 1 && role <= 3) {
            bool changed = role != awdlRole || memcmp(awdlMasterAddr, awdlRawState + 8, 6) != 0;
            awdlRole = role;
            memcpy(awdlMasterAddr, awdlRawState + 8, 6);
            if (changed) {
                awdlTlDirectUs = 0;
                const uint8_t *m = awdlMasterAddr;
                itlmtTrace(getController(), "awdl:tl role=%u master=%02x:%02x:%02x:%02x:%02x:%02x", (unsigned)role, m[0], m[1],
                           m[2], m[3], m[4], m[5]);
            }
        }
    }
    if (awdlAccepted < 96) {
        awdlAccepted++;
        itlmtTrace(getController(), "awdl:set SYNC_STATE %02x%02x%02x%02x %02x%02x%02x%02x",
                   awdlRawState[0], awdlRawState[1], awdlRawState[2], awdlRawState[3],
                   awdlRawState[4], awdlRawState[5], awdlRawState[6], awdlRawState[7]);
    }
    return kIOReturnSuccess;
}

IOReturn itlmt7925AwdlInterface::getAWDL_SYNC_STATE(apple80211_awdl_sync_state *a1)
{
    if (!a1) return kIOReturnBadArgument;
    if (!awdlRawStateLen) memset((void *)a1, 0, sizeof(awdlRawState));
    else memcpy((void *)a1, awdlRawState, awdlRawStateLen);
    return kIOReturnSuccess;
}

// IO80211VirtualInterface::setAwdlCurrentChannelSequenceIndex (slot 473) is an empty hook in the base class, i.e. it exists
// for the driver: the Family announces which entry of the synchronisation channel sequence the AWDL cluster is on, and the
// driver is expected to be there. Following it beats the free round-robin used for discovery, because it is the same index
// the peer manager schedules its availability windows around.
void itlmt7925AwdlInterface::setAwdlCurrentChannelSequenceIndex(uint32_t a1)
{
    awdlSeqIndex = a1;
    if (!awdlSeqIndexSet) {
        awdlSeqIndexSet = true;
        itlmtTrace(getController(), "awdl:seq index %u -> ch=%u (n=%u)", (unsigned)a1,
                   awdlChans[a1 % (awdlChanN ? awdlChanN : 1)], (unsigned)awdlChanN);
    }
}

// The advertise parameters. On Broadcom hardware the firmware builds the service-request / master-indication action frames
// from this payload, so this is where the driver has to take them from now that there is no firmware to do it. The struct
// (awdl_peer_advert_add) is not public: the first bytes are traced so the layout can be read off a real AirDrop-open boot
// instead of guessed at, and the call is accepted either way.
IOReturn itlmt7925AwdlInterface::setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add *a1)
{
    const uint8_t *p = (const uint8_t *)a1;
    if (!p) return kIOReturnBadArgument;
    if (awdlAdvertLogged < 4) {
        awdlAdvertLogged++;
        itlmtTrace(getController(), "awdl:set ADVERTISERS %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                   p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    }
    return kIOReturnSuccess;
}

IOReturn itlmt7925AwdlInterface::setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *a1)
{
    if (a1 && awdlAdvertLogged < 8) {
        awdlAdvertLogged++;
        itlmtTrace(getController(), "awdl:set PRESENCE_MODE %08x %08x", *(const uint32_t *)a1,
                   *(const uint32_t *)((const uint8_t *)a1 + 4));
    }
    return kIOReturnSuccess;
}

IOReturn itlmt7925AwdlInterface::setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *a1)
{
    // +0x16: six bytes AppleBCMWLANProximityInterface::setAWDL_ELECTION_PARAMS hands to the firmware's election tree next to
    // the metrics - the top master. Taken into the AW start events (zero = not named yet: the sync master is used).
    if (a1) memcpy(awdlTopMaster, (const uint8_t *)a1 + 0x16, 6);
    if (a1 && awdlAdvertLogged < 12) {
        awdlAdvertLogged++;
        itlmtTrace(getController(), "awdl:set ELECTION_PARAMS %08x %08x %08x %08x", *(const uint32_t *)a1,
                   *(const uint32_t *)((const uint8_t *)a1 + 4), *(const uint32_t *)((const uint8_t *)a1 + 8),
                   *(const uint32_t *)((const uint8_t *)a1 + 12));
    }
    return kIOReturnSuccess;
}

// apple80211_awdl_election_metric { u32 version; u32 metric }.
IOReturn itlmt7925AwdlInterface::setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *a1)
{
    awdlElectionMetric = a1 ? *(const uint32_t *)((const uint8_t *)a1 + 4) : 0;
    itlmtTrace(getController(), "awdl:set ELECTION_METRIC=%u", (unsigned)awdlElectionMetric);
    return kIOReturnSuccess;
}

// apple80211_awdl_sync_frame_template { u32 version; u32 payload_len; void *payload } - the PSF/action frame body the driver
// is expected to transmit on the AWDL channels. The Family hands it over when it changes, so the TLVs are copied here; the
// frame is built at every transmission by awdlBuildFrame() (phase 3 transmit half, docs/ROADMAP-AWDL.md §10.29).
IOReturn itlmt7925AwdlInterface::setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template *a1)
{
    awdlSyncFrameLen = a1 ? *(const uint32_t *)((const uint8_t *)a1 + 4) : 0;
    const void *payload = a1 ? *(void *const *)((const uint8_t *)a1 + 8) : nullptr;
    itlmtTrace(getController(), "awdl:set SYNC_FRAME_TEMPLATE len=%u", (unsigned)awdlSyncFrameLen);
    if (awdlSyncFrameLen >= 4 && awdlSyncFrameLen <= sizeof(awdlPsfTlv) && payload) {
        const uint8_t *pl = (const uint8_t *)payload;
        itlmtTrace(getController(), "awdl:tpl %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                   pl[0], pl[1], pl[2], pl[3], pl[4], pl[5], pl[6], pl[7], pl[8], pl[9], pl[10], pl[11], pl[12], pl[13], pl[14], pl[15]);
        // The payload is the action frame *body*: TLVs only (its first byte is a TLV type - buildServiceParamTlv writes 6),
        // never an 802.11 frame. The wrapper and the sync parameters TLV are what Apple's firmware would have added
        // (docs/ROADMAP-AWDL.md §10.11, §10.29).
        memcpy(awdlPsfTlv, payload, awdlSyncFrameLen);
        awdlPsfTlvLen = awdlSyncFrameLen;
        static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        awdlPsfLen = awdlBuildFrame(awdlTxPsf, sizeof(awdlTxPsf), 0, bcast, awdlPsfTlv, awdlPsfTlvLen, awdlChans[0]);
        if (awdlSyncTxLogged < 8) {
            awdlSyncTxLogged++;
            const uint8_t *f = awdlTxPsf;
            itlmtTrace(getController(), "awdl:psf armed tlv=%u frame=%u first=%02x sa=%02x%02x%02x%02x%02x%02x",
                       (unsigned)awdlSyncFrameLen, awdlPsfLen, f[40], f[10], f[11], f[12], f[13], f[14], f[15]);
        }
        if (awdlSyncEnabled) awdlPsfStart();                 // a new template: make sure the timeline runs (it is not reset)
    } else {
        awdlPsfLen = 0;                                      // empty template: no PSF (the AW timeline keeps running)
        awdlPsfTlvLen = 0;
        if (awdlSyncTxLogged < 8) {
            awdlSyncTxLogged++;
            itlmtTrace(getController(), "awdl:psf cleared core=%d len=%u", coreDev ? 1 : 0, (unsigned)awdlSyncFrameLen);
        }
    }
    return kIOReturnSuccess;
}

// apple80211_awdl_sync_params: 0x18 bytes (AppleBCMWLANProximityInterface::setAWDL_SYNC_PARAMS copies +4, +8, +0xc, +0x10 and
// +0x14 into its sync state and logs them as the AW period / AW length / extension length / advertisement period). Stored and
// published raw so the values can be matched against what the peers put in their TLV 4; the TLV this driver builds keeps the
// protocol defaults until that comparison is made.
IOReturn itlmt7925AwdlInterface::setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *a1)
{
    if (!a1) return kIOReturnBadArgument;
    memcpy(awdlSyncParamsRaw, a1, sizeof(awdlSyncParamsRaw));
    awdlSyncParamsSet = true;
    const uint32_t *w = (const uint32_t *)awdlSyncParamsRaw;
    itlmtTrace(getController(), "awdl:set SYNC_PARAMS %x %u %u %u %u %u", w[0], w[1], w[2], w[3], w[4], w[5]);
    OSData *d = OSData::withBytes(awdlSyncParamsRaw, sizeof(awdlSyncParamsRaw));
    if (d) { setProperty("itlmt-awdl-syncparams", d); d->release(); }
    return kIOReturnSuccess;
}

// The 802.11 operating class of a 20 MHz channel (IEEE 802.11 Annex E, global table), as the opclass-encoded channel sequence
// carries it next to the channel number.
static uint8_t awdlOpClass(uint8_t ch)
{
    if (ch >= 1 && ch <= 13) return 81;
    if (ch == 14) return 82;
    if (ch >= 36 && ch <= 48) return 115;
    if (ch >= 52 && ch <= 64) return 118;
    if (ch >= 100 && ch <= 144) return 121;
    if (ch >= 149 && ch <= 165) return 124;
    return 0;
}

// The synchronisation parameters TLV (type 4, 73 value bytes) - what every PSF and MIF on the air starts with and what a peer
// takes our timeline and channel sequence from (IO80211AWDLPeer::parseAwdlSyncParamsTlv). Value layout (offsets inside the
// value, little endian), as the Family's parser reads it and as the open AWDL work documents it (docs/ROADMAP-AWDL.md §10.29):
//   +0 tx channel  +1 TU to the next AW (u16, stamped by the core at TX)  +3 master channel  +4 guard time
//   +5 AW period (TU)  +7 action frame period (TU)  +9 flags  +11 AW extension length  +13 AW common length
//   +15 remaining AW length  +17 min ext  +18 max ext multicast  +19 max ext unicast  +20 max ext action frame
//   +21 the master this node follows  +27 presence mode  +28 reserved  +29 AW sequence number (u16, stamped by the core)
//   +31 AP beacon alignment delta  +33 channel sequence: count-1, encoding, duplicates, step, fill (u16), 16 entries
//   then two bytes of padding (an iPhone's TLV 4 is 73 bytes long: 39 + 16 x 2 + 2).
uint32_t itlmt7925AwdlInterface::awdlBuildSyncTlv(uint8_t *out, uint8_t txCh)
{
    const uint32_t vlen = 39 + 16 * 2 + 2;
    uint8_t *v = out + 3;
    bzero(out, 3 + vlen);
    out[0] = 4;
    out[1] = (uint8_t)(vlen & 0xff);
    out[2] = (uint8_t)(vlen >> 8);
    uint8_t masterCh = awdlRawMasterLen && awdlRawMaster[4] >= 1 && awdlRawMaster[4] <= 196 ? awdlRawMaster[4] : 6;
    v[0] = txCh;
    v[1] = (uint8_t)(kAwdlAwUs / kAwdlTuUs);                 // placeholder; the core writes the real count at TX time
    v[3] = masterCh;
    v[4] = 0;                                                // guard time
    OSWriteLittleInt16(v, 5, 16);                            // AW period: 16 TU
    OSWriteLittleInt16(v, 7, 110);                           // action frame period (TU)
    OSWriteLittleInt16(v, 9, 0x1800);                        // AWDL flags
    OSWriteLittleInt16(v, 11, 16);                           // AW extension length
    OSWriteLittleInt16(v, 13, 16);                           // AW common length
    OSWriteLittleInt16(v, 15, 0);                            // remaining AW length
    v[17] = 3; v[18] = 3; v[19] = 3; v[20] = 3;              // extension counts
    memcpy(v + 21, awdlRole == 2 ? awdlMasterAddr : awdlSelfAddr, 6);
    v[27] = (uint8_t)kAwdlAwPerEaw;                          // presence mode: one EAW = 4 AWs
    // +29 sequence number: stamped by the core
    v[33] = 15;                                              // 16 entries
    v[34] = 3;                                               // encoding: channel + operating class
    v[35] = 0;                                               // duplicates
    v[36] = 3;                                               // step count
    v[37] = 0xff; v[38] = 0xff;                              // fill channel
    for (int i = 0; i < 16; i++) {
        uint8_t ch = awdlSlotCh[i];                          // an empty slot is 00 00, as an iPhone writes it (§10.30)
        v[39 + 2 * i] = ch;
        v[40 + 2 * i] = ch ? awdlOpClass(ch) : 0;
    }
    return 3 + vlen;
}

// One AWDL action frame: 24-byte management/action header (DA, the AWDL address as SA, the AWDL BSSID), the 16-byte AWDL
// action header (category 0x7f, Apple OUI, type 8, version 1.0, subtype: PSF 0 / MIF 3), the sync parameters TLV unless the
// Family's TLVs already carry one, and the Family's TLVs. seq_ctrl and the two timestamps are stamped by the core per TX.
uint32_t itlmt7925AwdlInterface::awdlBuildFrame(uint8_t *out, uint32_t cap, uint8_t subtype, const uint8_t *da,
                                                const uint8_t *tlv, uint32_t tlvLen, uint8_t txCh)
{
    bool has4 = false;
    for (uint32_t pos = 0; pos + 3 <= tlvLen;) {
        uint32_t l = (uint32_t)tlv[pos + 1] | ((uint32_t)tlv[pos + 2] << 8);
        if (tlv[pos] == 4) { has4 = true; break; }
        pos += 3 + l;
    }
    uint8_t sync[3 + 73];
    uint32_t syncLen = has4 ? 0 : awdlBuildSyncTlv(sync, txCh);
    if (kAwdlAfHdrLen + syncLen + tlvLen > cap) return 0;
    uint8_t *f = out;
    bzero(f, kAwdlAfHdrLen);
    f[0] = 0xd0;                                             // management, action
    memcpy(f + 4, da, 6);                                    // addr1 = DA
    memcpy(f + 10, awdlSelfAddr, 6);                         // addr2 = SA: the AWDL interface address
    bool haveBssid = false;
    for (int i = 0; i < 6; i++) haveBssid |= awdlBssid[i] != 0;
    memcpy(f + 16, haveBssid ? awdlBssid : kAwdlBssidDefault, 6);   // addr3 = BSSID
    f[24] = 0x7f;                                            // category: vendor specific
    f[25] = 0x00; f[26] = 0x17; f[27] = 0xf2;                // Apple OUI
    f[28] = 0x08;                                            // AWDL type
    f[29] = 0x10;                                            // version 1.0
    f[30] = subtype;
    f[31] = 0x00;
    uint32_t pos = kAwdlAfHdrLen;
    if (syncLen) { memcpy(f + pos, sync, syncLen); pos += syncLen; awdlTlv4Inserted++; }
    else awdlTlv4Present++;
    memcpy(f + pos, tlv, tlvLen);
    pos += tlvLen;
    if (syncLen && !awdlSelfTlv4Published) {
        awdlSelfTlv4Published = true;
        OSData *d = OSData::withBytes(sync, syncLen);
        if (d) { setProperty("itlmt-awdl-self-tlv4", d); d->release(); }
    }
    return pos;
}

// apple80211_awdl_oob_request, as AppleBCMWLANProximityInterface::setAWDL_OOB_AUTO_REQUEST reads it (25G241): +4 u32, +8 u16,
// +0xc u32, +0x10 u32, +0x14 u16, +0x16 a 6-byte address, +0x28 the payload length (u16), +0x30 the payload pointer. Apple's
// driver hands the header fields and the payload to the firmware ("awdl_oob_af_auto"), which repeats the frame on its own;
// IO80211AWDLPeerManager::updatePrimaryPayloadMI is the caller, with the master indication TLVs as the payload. Returning
// unsupported here (as before §10.29) meant our MIF never reached the air, so no peer ever saw our services.
IOReturn itlmt7925AwdlInterface::awdlOob(const uint8_t *r, bool autoReq)
{
    if (!r) return kIOReturnBadArgument;
    uint32_t len = OSReadLittleInt16(r, 0x28);
    const uint8_t *pl = *(const uint8_t *const *)(r + 0x30);
    memcpy(awdlOobHdr, r, sizeof(awdlOobHdr));
    if (awdlOobLogged < 12) {
        awdlOobLogged++;
        const uint8_t *a = r + 0x16;
        itlmtTrace(getController(), "awdl:oob %s len=%u w4=%x w8=%x wc=%u w10=%u w14=%x addr=%02x:%02x:%02x:%02x:%02x:%02x first=%02x%02x%02x",
                   autoReq ? "auto" : "once", len, OSReadLittleInt32(r, 4), OSReadLittleInt16(r, 8), OSReadLittleInt32(r, 0xc),
                   OSReadLittleInt32(r, 0x10), OSReadLittleInt16(r, 0x14), a[0], a[1], a[2], a[3], a[4], a[5],
                   pl && len > 0 ? pl[0] : 0, pl && len > 1 ? pl[1] : 0, pl && len > 2 ? pl[2] : 0);
    }
    if (!len || !pl) {                                       // nothing to send: stop the frame
        awdlMifTlvLen = 0;
        awdlOobStop++;
        return kIOReturnSuccess;
    }
    // A payload that starts with the AWDL action header (7f 00 17 f2 08 ...) carries its own 16 bytes in front of the TLVs.
    if (len > 16 && pl[0] == 0x7f && pl[1] == 0x00 && pl[2] == 0x17 && pl[3] == 0xf2 && pl[4] == 0x08) { pl += 16; len -= 16; }
    if (len > sizeof(awdlMifTlv)) {
        awdlOobTooLong++;
        if (awdlOobTooLong <= 3) itlmtTrace(getController(), "awdl:oob payload %u > %u bytes", len, (unsigned)sizeof(awdlMifTlv));
        return kIOReturnNoSpace;
    }
    memcpy(awdlMifTlv, pl, len);
    awdlMifTlvLen = len;
    const uint8_t *a = r + 0x16;
    bool zero = true;
    for (int i = 0; i < 6; i++) zero &= a[i] == 0;
    // The auto request is the master indication: a broadcast. A one-shot request to a unicast address goes to that peer.
    if (autoReq || zero || (a[0] & 1) || !memcmp(a, awdlSelfAddr, 6)) memset(awdlMifDa, 0xff, 6);
    else memcpy(awdlMifDa, a, 6);
    awdlMifOneShot = !autoReq;
    if (autoReq) awdlOobAuto++; else awdlOobOnce++;
    {
        uint8_t h[0x30];
        memcpy(h, awdlOobHdr, sizeof(h));
        OSData *d = OSData::withBytes(h, sizeof(h));
        if (d) { setProperty(autoReq ? "itlmt-awdl-oob-auto" : "itlmt-awdl-oob-once", d); d->release(); }
    }
    if (awdlSyncEnabled) awdlPsfStart();
    return kIOReturnSuccess;
}

// The AWDL state at a glance (ioreg -l | grep itlmt-awdl): the NVRAM trace ring is 2 KB and loses these lines within seconds.
void itlmt7925AwdlInterface::awdlPublishState()
{
    char b[400];
    snprintf(b, sizeof(b),
             "role=%u eaws=%u posted=%u fail=%u late=%u syncs=%u adj=%d psf=%u/%u mif=%u/%u oob=a%u/o%u/s%u/x%u "
             "tlv4=%u/%u peer4=%u rx=%u norssi=%u weak=%u slots=%u%s ver=%u",
             (unsigned)awdlRole, (unsigned)awdlTlEaws, (unsigned)awdlTlPosted, (unsigned)awdlTlPostFail, (unsigned)awdlTlLate,
             (unsigned)awdlTlSyncs, (int)awdlTlLastAdjUs, (unsigned)awdlPsfTicks, (unsigned)awdlPsfLen,
             (unsigned)awdlMifSubmitted, (unsigned)awdlMifTlvLen, (unsigned)awdlOobAuto,
             (unsigned)awdlOobOnce, (unsigned)awdlOobStop, (unsigned)awdlOobTooLong, (unsigned)awdlTlv4Inserted,
             (unsigned)awdlTlv4Present, (unsigned)awdlTlv4FromPeer, (unsigned)awdlRxFrames, (unsigned)awdlRxNoRssi,
             (unsigned)awdlRxWeak, (unsigned)awdlSlotCh[0], awdlSlotMulti ? "+multi" : "", (unsigned)awdlSyncVer);
    setProperty("itlmt-awdl", b);
}

// -----------------------------------------------------------------------------------------------------------------------
// The sync period. The Family's AWDL state machine expects the PSF to be on the air every period (440 ms here); it hands the
// frame over once, so the interface owns the cadence. Every tick is one handoff to the core, which does the real work: ROC on
// the social channel, one management frame, back to the AP's channel (itlmt7925::awdlSyncTx).
// -----------------------------------------------------------------------------------------------------------------------
void itlmt7925AwdlInterface::awdlPsfTimerFired(OSObject *owner, IOTimerEventSource *)
{
    if (owner) ((itlmt7925AwdlInterface *)owner)->awdlPsfTick();
}

void itlmt7925AwdlInterface::awdlPsfStart()
{
    if (!awdlPsfTimer) {
        IOWorkLoop *wl = getWorkLoop();
        if (!wl) { itlmtTrace(getController(), "awdl:psf no workloop"); return; }
        awdlPsfTimer = IOTimerEventSource::timerEventSource(this, awdlPsfTimerFired);
        if (!awdlPsfTimer) return;
        if (wl->addEventSource(awdlPsfTimer) != kIOReturnSuccess) {
            awdlPsfTimer->release();
            awdlPsfTimer = nullptr;
            return;
        }
    }
    uint64_t now = awdlNowUs();
    if (!awdlTlRunning) {
        // AWDL just came up: we are our own master until the Family elects someone else, so window 0 starts now.
        uint32_t v = 0;
        awdlTlFlags = (PE_parse_boot_argn("itlmt_awdl_snnext", &v, sizeof(v)) && v) ? 1 : 0;
        awdlTlBaseUs = now;
        awdlTlBaseSeq = 0;
        awdlTlLastPosted = -1;
        awdlPsfDue = 0;
        awdlTlRunning = true;
        itlmtTrace(getController(), "awdl:tl start role=%u snnext=%u", (unsigned)awdlRole, (unsigned)awdlTlFlags);
    }
    awdlTlArm(now);
}

// The timer is always aimed at the next EAW boundary of the current timeline, so a re-alignment (awdlTlSync) takes effect
// on the following tick without any bookkeeping.
void itlmt7925AwdlInterface::awdlTlArm(uint64_t now)
{
    if (!awdlPsfTimer || !awdlSyncEnabled || !awdlTlRunning) return;
    int64_t el = (int64_t)(now - awdlTlBaseUs);
    int64_t n = el >= 0 ? el / kAwdlAwUs : -((-el + kAwdlAwUs - 1) / kAwdlAwUs);     // floor
    int64_t k = n + 1;
    while (((awdlTlBaseSeq + k) % kAwdlAwPerEaw) != 0) k++;                            // next AW that starts an EAW
    uint64_t at = awdlTlBaseUs + (uint64_t)(k * (int64_t)kAwdlAwUs);
    uint64_t d = at > now ? at - now : 0;
    if (d < 200) d = 200;
    awdlPsfTimer->cancelTimeout();
    awdlPsfTimer->setTimeoutUS((uint32_t)d + 100);                                     // land just inside the window
}

void itlmt7925AwdlInterface::awdlPsfTick()
{
    uint64_t now = awdlNowUs();
    if (coreDev && awdlSyncEnabled) awdlPollRx();            // frames heard so far go up first (they may re-align the clock)
    if (awdlSyncEnabled && awdlTlRunning) awdlTlTick(now);
    awdlTlArm(awdlNowUs());
}

void itlmt7925AwdlInterface::awdlTlTick(uint64_t now)
{
    int64_t el = (int64_t)(now - awdlTlBaseUs);
    int64_t n = el >= 0 ? el / kAwdlAwUs : -((-el + kAwdlAwUs - 1) / kAwdlAwUs);
    uint16_t aw = (uint16_t)(awdlTlBaseSeq + n);
    uint64_t awStart = awdlTlBaseUs + (uint64_t)(n * (int64_t)kAwdlAwUs);
    if ((aw % kAwdlAwPerEaw) != 0 || now - awStart > kAwdlAwUs / 2) {
        // Not at an EAW start: the clock was just re-aligned under the pending timer, or the work loop ran late. The next
        // boundary is armed by the caller; nothing is posted for a window that is already half over.
        awdlTlLate++;
        return;
    }
    if (awdlTlLastPosted == (int32_t)aw) return;
    awdlTlLastPosted = aw;
    awdlTlEaws++;
    uint32_t slot = ((uint32_t)aw / kAwdlAwPerEaw) & 15;
    uint8_t ch = awdlSlotCh[slot];

    if (ch) {
        AwdlAwStart ev;
        bzero(&ev, sizeof(ev));
        ev.seq = aw;
        ev.role = awdlRole;
        bool slave = awdlRole == 2;
        bool haveTop = false;
        for (int i = 0; i < 6; i++) haveTop |= awdlTopMaster[i] != 0;
        memcpy(ev.master, slave ? awdlMasterAddr : awdlSelfAddr, 6);
        memcpy(ev.topMaster, haveTop ? awdlTopMaster : ev.master, 6);
        ev.startUsBe = OSSwapHostToBigInt32((uint32_t)awStart);
        ev.chVersion = 1;
        ev.channel = ch;
        ev.chFlags = awdlChFlags(ch);
        IO80211Controller *ctl = getController();
        IOReturn r = ctl ? itlmtAwdlPostMessage(ctl, (IO80211SkywalkInterface *)this, kAwdlMsgAwStart, &ev, sizeof(ev), true)
                         : kIOReturnNotReady;
        if (r == kIOReturnSuccess) awdlTlPosted++;
        else awdlTlPostFail++;
        if (awdlTlPosted <= 3 || (awdlTlEaws % 500) == 0 || (r && awdlTlPostFail <= 3))
            itlmtTrace(ctl, "awdl:tl aw=%u slot=%u ch=%u role=%u posted=%u fail=%u late=%u syncs=%u adj=%d rc=%x",
                       (unsigned)aw, (unsigned)slot, ch, (unsigned)awdlRole, (unsigned)awdlTlPosted, (unsigned)awdlTlPostFail,
                       (unsigned)awdlTlLate, (unsigned)awdlTlSyncs, (int)awdlTlLastAdjUs, r);
    }

    // The PSF goes out at the start of an EAW, on that EAW's channel. While the sequence names a single channel (the Family
    // builds a 2.4 GHz-only sequence until it believes the card is dual band) the window walks the social channels instead,
    // or a peer that sits on 44/149 would never be heard (docs/ROADMAP-AWDL.md §10.13).
    if ((awdlTlEaws % 64) == 1) awdlPublishState();
    if (awdlPsfDue) { awdlPsfDue--; return; }
    if (!coreDev || (!awdlPsfTlvLen && !awdlMifTlvLen)) return;
    uint8_t txCh = ch;
    if (!awdlSlotMulti || !txCh) {
        txCh = awdlChans[awdlChanIdx];
        awdlChanIdx = (uint8_t)((awdlChanIdx + 1) % (awdlChanN ? awdlChanN : 1));
    }
    if (!txCh) return;
    awdlPsfDue = kAwdlPsfEaws - 1;
    // Both frames are built for this window's channel (the sync parameters TLV names it) and handed over together: the core
    // sends the PSF and then the MIF right after the ROC grant (docs/ROADMAP-AWDL.md §10.29).
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint32_t psfLen = awdlPsfTlvLen ? awdlBuildFrame(awdlTxPsf, sizeof(awdlTxPsf), 0, bcast, awdlPsfTlv, awdlPsfTlvLen, txCh) : 0;
    uint32_t mifLen = awdlMifTlvLen ? awdlBuildFrame(awdlTxMif, sizeof(awdlTxMif), 3, awdlMifDa, awdlMifTlv, awdlMifTlvLen, txCh)
                                    : 0;
    if (psfLen) awdlPsfLen = psfLen;
    ItlNativeAwdlSync req = {};
    req.version = awdlSyncVer;
    req.frame = psfLen ? awdlTxPsf : awdlTxMif;
    req.len = psfLen ? psfLen : mifLen;
    req.channel = txCh;
    req.dwellMs = kAwdlPsfRocMs;
    req.awBaseUs = awdlTlBaseUs;
    req.awBaseSeq = awdlTlBaseSeq;
    req.flags = awdlTlFlags;
    req.awPeriodUs = kAwdlAwUs;
    if (psfLen && mifLen) { req.frame2 = awdlTxMif; req.len2 = mifLen; }
    if (!req.len) return;
    IOReturn r = kIOReturnBadArgument;
    while (true) {
        if (awdlSyncVer < 3) { req.frame2 = nullptr; req.len2 = 0; }
        if (awdlSyncVer < 3 && req.len > 256) break;               // an older core takes 256-byte frames only
        req.version = awdlSyncVer;
        r = coreDev->callPlatformFunction(ITL_NATIVE_AWDL_SYNC, false, &req, nullptr, nullptr, nullptr);
        if (r != kIOReturnBadArgument || awdlSyncVer <= 1) break;
        awdlSyncVer--;                                              // a core from before §10.29 / §10.28
        itlmtTrace(getController(), "awdl:psf core only speaks ItlNativeAwdlSync v%u", (unsigned)awdlSyncVer);
    }
    if (r == kIOReturnSuccess && req.len2) awdlMifSubmitted++;
    if (r == kIOReturnSuccess && !psfLen && mifLen) awdlMifSubmitted++;
    if (r == kIOReturnSuccess && mifLen && awdlMifOneShot) { awdlMifTlvLen = 0; awdlMifOneShot = false; }
    awdlPsfTicks++;
    if (awdlPsfTicks <= 5 || (awdlPsfTicks % 100) == 0)
        itlmtTrace(getController(), "awdl:psf tick %u psf=%u mif=%u ch=%u aw=%u v=%u rc=%x", (unsigned)awdlPsfTicks, psfLen, mifLen,
                   txCh, (unsigned)aw, (unsigned)awdlSyncVer, r);
}

// Re-align the clock to the master (docs/ROADMAP-AWDL.md §10.28). The sync parameters TLV (type 4) of the master's PSF/MIF
// says, at the moment it was built for transmission (the target TX time of the action header), how many TU were left to the
// next AW (+1, the down counter) and which AW was running (+29, the sequence number; IO80211AWDLPeer::parseAwdlSyncParamsTlv
// reads the same offsets as "dc" and "sn"); +21 is the master that node itself follows. The action header's PHY TX time
// minus its target TX time is how late the frame really left, which the counter does not include.
void itlmt7925AwdlInterface::awdlTlSync(const uint8_t *frame, const uint8_t *body, uint32_t bodyLen, uint32_t rxTimeUs)
{
    if (awdlRole != 2 || !awdlTlRunning || !rxTimeUs) return;     // a master keeps its own clock
    const uint8_t *sa = frame + 10;
    uint32_t pos = 16;
    const uint8_t *v = nullptr;
    while (pos + 3 <= bodyLen) {
        uint32_t l = (uint32_t)body[pos + 1] | ((uint32_t)body[pos + 2] << 8);
        if (pos + 3 + l > bodyLen) return;
        if (body[pos] == 4 && l >= 31) { v = body + pos + 3; break; }
        pos += 3 + l;
    }
    if (!v) return;
    uint64_t now = awdlNowUs();
    bool direct = memcmp(sa, awdlMasterAddr, 6) == 0;
    bool sameTree = memcmp(v + 21, awdlMasterAddr, 6) == 0;
    // Prefer the master's own frames; a node of the same tree only counts when the master has not been heard for a second.
    if (!direct && !(sameTree && (!awdlTlDirectUs || now - awdlTlDirectUs > 1000000))) return;
    if (direct) awdlTlDirectUs = now;

    uint32_t phyTx = OSReadLittleInt32(body, 8), targetTx = OSReadLittleInt32(body, 12);
    uint32_t late = phyTx - targetTx;
    if (late > 50000) late = 0;                                  // not a plausible queueing delay: ignore it
    uint32_t dc = (uint32_t)v[1] | ((uint32_t)v[2] << 8);
    uint16_t sn = (uint16_t)(v[29] | (v[30] << 8));
    if (dc > 64) return;                                         // longer than any AW period: not a timeline we understand
    uint64_t rx = now - (uint32_t)((uint32_t)now - rxTimeUs);    // the 32-bit RX stamp back on the 64-bit clock
    uint64_t next = rx + (uint64_t)dc * kAwdlTuUs - late;        // start of the AW after the one the frame was sent in
    uint64_t base;
    uint16_t baseSeq;
    if (awdlTlFlags & 1) { base = next; baseSeq = sn; }              // sn names that next AW
    else { base = next - kAwdlAwUs; baseSeq = sn; }                  // sn names the AW the frame was sent in (default)

    // How far this moves our clock: compare where AW baseSeq starts on the current timeline.
    int64_t old = (int64_t)awdlTlBaseUs + (int64_t)(int16_t)(uint16_t)(baseSeq - awdlTlBaseSeq) * (int64_t)kAwdlAwUs;
    awdlTlLastAdjUs = (int32_t)((int64_t)base - old);
    awdlTlBaseUs = base;
    awdlTlBaseSeq = baseSeq;
    awdlTlSyncs++;
    if (awdlTlSyncLogged < 6 || (awdlTlSyncs % 500) == 0) {
        if (awdlTlSyncLogged < 6) awdlTlSyncLogged++;
        itlmtTrace(getController(), "awdl:tl sync %s sa=%02x:%02x:%02x:%02x:%02x:%02x sn=%u dc=%u late=%u adj=%d n=%u",
                   direct ? "master" : "tree", sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], (unsigned)sn, (unsigned)dc,
                   (unsigned)late, (int)awdlTlLastAdjUs, (unsigned)awdlTlSyncs);
    }
}

// -----------------------------------------------------------------------------------------------------------------------
// Phase 3, receive half (docs/ROADMAP-AWDL.md §10.12). The core's RX path recognises an AWDL action frame (0x7f / 00:17:f2 /
// type 8) and queues it; this drains the queue and hands each frame to the Family's AWDL peer manager, which is the only
// component that can turn frames into peers, sync state and service requests. Without it the peer list stays empty and
// AirDrop never advertises anything, however well the PSF transmit half works.
// -----------------------------------------------------------------------------------------------------------------------
void itlmt7925AwdlInterface::awdlPollRx()
{
    for (int i = 0; i < 4; i++) {
        uint8_t *buf = awdlRxScratch;
        ItlNativeAwdlRx req = {};
        req.version = awdlRxVer;
        req.buf = buf;
        req.cap = sizeof(awdlRxScratch);
        IOReturn r = coreDev->callPlatformFunction(ITL_NATIVE_AWDL_RX, false, &req, nullptr, nullptr, nullptr);
        if (r == kIOReturnBadArgument && awdlRxVer == 2) {      // a core from before §10.26: no signal / time fields
            awdlRxVer = 1;
            itlmtTrace(getController(), "awdl:rx core only speaks ItlNativeAwdlRx v1 (no RSSI)");
            continue;
        }
        if (r != kIOReturnSuccess || !req.len) return;
        awdlDeliverRx(buf, req.len, req.channel, awdlRxVer >= 2 ? req.rssi : 0, awdlRxVer >= 2 ? req.rxTimeUs : 0);
    }
}

// A fresh buffer from the Family's dynamic pool with the AWDL action body written into it (docs/ROADMAP-AWDL.md §10.26).
// The buffer is a real IO80211Buffer - the peer manager calls getLength()/getCursor() on it and returns it to the pool when it
// is done - so it is used for exactly one frame and never touched again after it was handed over.
IO80211Buffer *itlmt7925AwdlInterface::awdlRxBuffer(const uint8_t *body, uint32_t len)
{
    if (len > kAwdlRxBufCap) return nullptr;
    IO80211DynamicBufferPool *pool = gAwdlRxPool;
    if (!pool) {
        // The pool allocates its slabs itself (IO80211BufferSlab::withOptions) and builds plain IO80211Buffers through
        // IO80211Buffer::allocBuffer(), so unlike allocBufferSingle() it needs nothing from the controller. See
        // io80211_buffer_min.h for why the static pool cannot be used from here.
        IO80211BufferPoolOptions opts = {};
        opts.count = kAwdlRxPoolCount;
        opts.size = kAwdlRxBufCap;
        opts.name = "itlmt-awdl-rx";
        pool = IO80211DynamicBufferPool::withOptions(&opts);
        if (!pool) {
            awdlRxBufFail++;
            if (awdlRxBufFail <= 4) itlmtTrace(getController(), "awdl:rx the Family's dynamic buffer pool failed");
            return nullptr;
        }
        if (!OSCompareAndSwapPtr(nullptr, pool, (void * volatile *)&gAwdlRxPool)) {
            pool->free();                              // another interface won the race; use its pool
            pool = gAwdlRxPool;
        } else {
            itlmtTrace(getController(), "awdl:rx buffer pool ready (%u x %u)", (unsigned)kAwdlRxPoolCount,
                       (unsigned)kAwdlRxBufCap);
        }
    }
    IO80211Buffer *b = pool->getBuffer();
    if (!b) {
        awdlRxBufFail++;
        if (awdlRxBufFail <= 4 || (awdlRxBufFail % 100) == 0)
            itlmtTrace(getController(), "awdl:rx no buffer from the Family's pool (%u)", (unsigned)awdlRxBufFail);
        return nullptr;
    }
    b->writeBytes(0, body, len);
    b->setLength(len);
    return b;
}

void itlmt7925AwdlInterface::awdlDeliverRx(const uint8_t *frame, uint32_t len, uint32_t channel, int32_t rssi,
                                           uint32_t rxTimeUs)
{
    // IO80211SkywalkInterface::getPeerManager() is [[this+0x120]+0x28] in this KernelCollection; bpfTapInput() reads the same
    // field and falls back to BPF without a peer manager. The check here only keeps the frame (and a buffer) from being spent
    // before the peer manager exists.
    void *pm = *(void **)((uint8_t *)this + 0x120);
    if (pm) pm = *(void **)((uint8_t *)pm + 0x28);
    if (!pm) {
        if (awdlRxLogged < 4) { awdlRxLogged++; itlmtTrace(getController(), "awdl:rx no peer manager yet"); }
        return;
    }

    // The peer manager wants the AWDL action *body* (the 16-byte action header - category 0x7f, Apple's OUI, type 8, version,
    // subtype, two timestamps - plus the TLVs), not the 24-byte 802.11 header: actionFrameHasCorrectOUI() reads the OUI at
    // body offsets 1..5. Apple's firmware event carries exactly that.
    if (len < 24 + 16) return;
    const uint8_t *body = frame + 24;
    uint32_t bodyLen = len - 24;

    // The RX buffer can carry bytes after the frame (the FCS when the sniffer passes it, alignment padding). The TLVs (1-byte
    // type, 2-byte little-endian length) define where the frame ends: cut a short tail after the last complete TLV so the
    // Family's TLV parser does not read it as a truncated element. A longer mismatch is passed on unchanged and counted.
    {
        uint32_t pos = 16;
        while (pos + 3 <= bodyLen) {
            uint32_t l = (uint32_t)body[pos + 1] | ((uint32_t)body[pos + 2] << 8);
            if (pos + 3 + l > bodyLen) break;
            pos += 3 + l;
        }
        if (pos != bodyLen) {
            uint32_t rest = bodyLen - pos;
            if (rest <= 4) {
                awdlRxTrim++;
                if (awdlRxTrim <= 3) itlmtTrace(getController(), "awdl:rx tail cut %u bytes after the last TLV (len %u)", rest, bodyLen);
                bodyLen = pos;
            } else {
                awdlRxBadTlv++;
                if (awdlRxBadTlv <= 3)
                    itlmtTrace(getController(), "awdl:rx TLVs end at %u of %u (passed on unchanged)", pos, bodyLen);
            }
        }
    }

    // RSSI: the core decodes it from the RX vector (RCPI of the chains, rxRssi()). A frame whose vector the core could not
    // read gets the edge-of-range value the peer manager itself defaults to (-70 dBm, initParamsToDefaults): the frame was
    // received and decoded, so the link is at least that good, and 0 would make determinePeerReachability() drop every peer.
    // The counter says how often that happened; the core's "awdl af rx" line shows the vector for the first frames.
    if (rssi >= 0 || rssi < -110) {
        awdlRxNoRssi++;
        rssi = -70;
    }
    if (rssi < -70) awdlRxWeak++;

    // One peer's sync parameters TLV, published raw, to compare with the one this driver builds (itlmt-awdl-self-tlv4).
    for (uint32_t pos = 16; pos + 3 <= bodyLen;) {
        uint32_t l = (uint32_t)body[pos + 1] | ((uint32_t)body[pos + 2] << 8);
        if (pos + 3 + l > bodyLen) break;
        if (body[pos] == 4) {
            awdlTlv4FromPeer++;
            if (!awdlPeerTlv4Published) {
                awdlPeerTlv4Published = true;
                OSData *d = OSData::withBytes(body + pos, 3 + l);
                if (d) { setProperty("itlmt-awdl-peer-tlv4", d); d->release(); }
                OSData *h = OSData::withBytes(body, pos < 64 ? pos : 64);   // the action header and what precedes TLV 4
                if (h) { setProperty("itlmt-awdl-peer-hdr", h); h->release(); }
            }
            break;
        }
        pos += 3 + l;
    }
    awdlTlSync(frame, body, bodyLen, rxTimeUs);            // a slave follows its master's clock

    IO80211Buffer *buf = awdlRxBuffer(body, bodyLen);
    if (!buf) return;

    AwdlActFrmHdr hdr;
    bzero(&hdr, sizeof(hdr));
    memcpy(hdr.sa, frame + 10, 6);                     // addr2: the sender, i.e. the peer
    hdr.chVersion = 1;
    hdr.channel = channel;                             // the social channel the frame was heard on
    hdr.chFlags = (channel <= 14 ? 0x8 : 0x10) | 0x2;  // band | 20 MHz, as the scan results report it
    hdr.rssi = rssi;
    hdr.rxTime = rxTimeUs;

    awdlRxFrames++;
    // The Family's own receive entry, as Apple's driver calls it: type 0xc routes to the peer manager. The buffer belongs to
    // the Family after this call.
    IO80211VirtualInterface_bpfTapInput(this, buf, kAwdlBpfTypeActionFrame, &hdr, sizeof(hdr));
    if (awdlRxLogged < 8 || (awdlRxFrames % 200) == 0) {
        if (awdlRxLogged < 8) awdlRxLogged++;
        itlmtTrace(getController(),
                   "awdl:rx af n=%u len=%u body=%u ch=%u sa=%02x:%02x:%02x:%02x:%02x:%02x sub=%02x rssi=%d norssi=%u weak=%u "
                   "nobuf=%u trim=%u badtlv=%u",
                   (unsigned)awdlRxFrames, len, bodyLen, hdr.channel, hdr.sa[0], hdr.sa[1], hdr.sa[2], hdr.sa[3], hdr.sa[4],
                   hdr.sa[5], body[6], (int)rssi, (unsigned)awdlRxNoRssi, (unsigned)awdlRxWeak, (unsigned)awdlRxBufFail,
                   (unsigned)awdlRxTrim, (unsigned)awdlRxBadTlv);
    }
}

void itlmt7925AwdlInterface::free()
{
    if (awdlPsfTimer) {
        awdlPsfTimer->cancelTimeout();
        IOWorkLoop *wl = getWorkLoop();
        if (wl) wl->removeEventSource(awdlPsfTimer);
        awdlPsfTimer->release();
        awdlPsfTimer = nullptr;
    }
    IO80211AWDLProtocol::free();
}

// apple80211_awdl_peer_traffic_registration { u32 version; void *addr; u32 name_len; char name[152]; u32 active } packed -
// the real-time group registration ("Can't send peer traffic registration to the driver" in the 21:27 log came from here).
IOReturn itlmt7925AwdlInterface::setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration *a1)
{
    if (a1) {
        const uint8_t *p = (const uint8_t *)a1;
        uint32_t nameLen = *(const uint32_t *)(p + 12);
        uint32_t active = *(const uint32_t *)(p + 168);
        char name[17];
        size_t n = nameLen < sizeof(name) - 1 ? nameLen : sizeof(name) - 1;
        memcpy(name, p + 16, n);
        name[n] = 0;
        for (size_t i = 0; i < n; i++)
            if (name[i] < 0x20 || name[i] > 0x7e) name[i] = '.';
        itlmtTrace(getController(), "awdl:set PEER_TRAFFIC_REG active=%u len=%u name=%s",
                   (unsigned)active, (unsigned)nameLen, name);
    } else {
        itlmtTrace(getController(), "awdl:set PEER_TRAFFIC_REG (null)");
    }
    return kIOReturnSuccess;
}

// Slot 562. AppleBCMWLANProximityInterface::setCOMPANION_SKYWALK_IF_CREATE (0xffffff8001722ef2) reads action at +4
// (0 = destroy the companion, 1 = create one), builds apple80211SkywalkIfCreateParams_t {class, MAC, 9, 2}, calls
// AppleBCMWLANCore::createSkywalkInterface(&vif->companion, params, vif), then setCompanionInterfaceId(parent) and copies
// the companion's BSD name into the request at +8 (apple80211_companion_skywalk_if_create: version, action, bsd_name[16]).
// The LLW skywalk interface does not exist yet, so answer unsupported: wifip2pd logs it as a missing optional companion and
// continues (16:36:03 log: the failure is followed by the normal power-state / eligibility bring-up).
IOReturn itlmt7925AwdlInterface::setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create *a1)
{
    uint32_t action = a1 ? *(const uint32_t *)((const uint8_t *)a1 + 4) : 0xffffffffu;
    itlmtTrace(getController(), "awdl:companion action=%u (unsupported)", action);
    return kIOReturnUnsupported;
}

namespace {

// The registration needs a queue set; the datapath arrives in phase 5, so the actions consume nothing yet.
uint32_t awdlTxSubmissionAction(OSObject *, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket *const *, uint32_t, void *) { return 0; }
uint32_t awdlTxCompletionAction(OSObject *, IOSkywalkTxCompletionQueue *, IOSkywalkPacket **, uint32_t, void *) { return 0; }
uint32_t awdlRxCompletionAction(OSObject *, IOSkywalkRxCompletionQueue *, IOSkywalkPacket **, uint32_t, void *) { return 0; }

template <typename T> T &field(void *base, size_t off) { return *(T *)((uint8_t *)base + off); }

const uint32_t kAwdlPoolPackets = 0x100;      // enough for a phase-2 registration; the real datapath sizes this later
const uint32_t kAwdlPoolBufferSize = 0x800;
const uint32_t kAwdlQueueCapacity = 0x100;

} // namespace

void itlmt7925AwdlInterface::freeAwdlRegistration()
{
    for (uint32_t i = 0; i < 3; i++) {
        if (awdlQueues[i]) {
            awdlQueues[i]->release();
            awdlQueues[i] = nullptr;
        }
    }
    if (awdlPool) {
        awdlPool->release();
        awdlPool = nullptr;
    }
}

IOReturn itlmt7925AwdlInterface::registerAwdlInterface(const uint8_t *mac)
{
    if (mac) memcpy(awdlSelfAddr, mac, sizeof(awdlSelfAddr));   // SA of every frame this interface puts on the AWDL channel
    IOSkywalkPacketBufferPool::PoolOptions po;
    bzero(&po, sizeof(po));
    po.packets0 = po.packets1 = kAwdlPoolPackets;
    po.bufferSize = kAwdlPoolBufferSize;
    po.one = 1;
    po.zero = 0;
    po.flags = 1;
    awdlPool = IOSkywalkPacketBufferPool::withName("itlmt7925AwdlPool", this, 1, &po);
    if (!awdlPool) return kIOReturnNoMemory;

    awdlQueues[0] = IOSkywalkTxSubmissionQueue::withPool(awdlPool, kAwdlQueueCapacity, 0, this, awdlTxSubmissionAction, this, 0);
    awdlQueues[1] = IOSkywalkTxCompletionQueue::withPool(awdlPool, kAwdlQueueCapacity, 1, this, awdlTxCompletionAction, this, 0);
    awdlQueues[2] = IOSkywalkRxCompletionQueue::withPool(awdlPool, kAwdlQueueCapacity, 2, this, awdlRxCompletionAction, this, 0);
    if (!awdlQueues[0] || !awdlQueues[1] || !awdlQueues[2]) {
        freeAwdlRegistration();
        return kIOReturnNoMemory;
    }

    // RegistrationInfo (opaque 0x130 bytes): the fields AppleBCMWLANCore::createProxInterface sets before calling the
    // proximity interface's start (0xffffff800165dcbe): +0xc = 3, +0x14 = 2, +0x24 = 0x8000000080, +0x30 = the name pointer,
    // +0x38 = 0 (BSD unit: registerNetworkInterface sets the IOInterfaceUnit property only when this is >= 0; the BSD client
    // needs that property to build its name), +0x3c = 0x40 (headroom), +0x40 = 4|6 (flags), +0x108 = the MAC.
    uint8_t info[0x130];
    bzero(info, sizeof(info));
    initRegistrationInfo((IOSkywalkEthernetInterface::RegistrationInfo *)info, 1, 0x130);
    field<uint32_t>(info, 0xc) = 3;
    field<uint32_t>(info, 0x14) = 2;
    field<uint64_t>(info, 0x24) = 0x8000000080ULL;
    field<const char *>(info, 0x30) = "awdl";
    field<uint32_t>(info, 0x38) = 0;             // unit 0 -> awdl0
    field<uint16_t>(info, 0x3c) = 0x40;
    field<uint32_t>(info, 0x40) = 6;
    memcpy((uint8_t *)info + 0x108, mac, 6);

    IOReturn rr = registerEthernetInterface((IOSkywalkEthernetInterface::RegistrationInfo *)info, awdlQueues, 3,
                                            awdlPool, awdlPool, 0);
    if (rr != kIOReturnSuccess) freeAwdlRegistration();
    return rr;
}

// -----------------------------------------------------------------------------------------------------------------------
// Boot-arg
// -----------------------------------------------------------------------------------------------------------------------
bool itlmtAwdlEnabled()
{
    static int cached = -1;
    if (cached < 0) {
        uint32_t v = 0;
        cached = (PE_parse_boot_argn("itlmt_awdl", &v, sizeof(v)) && v) ? 1 : 0;
    }
    return cached == 1;
}

// -----------------------------------------------------------------------------------------------------------------------
// APPLE80211_IOC_VIRTUAL_IF_CREATE (0x5e), protocol slot 558.
// -----------------------------------------------------------------------------------------------------------------------
// Layout from IOKit/80211/apple80211_ioctl.h (apple80211_virt_if_create_data): u32 version; u8 mac[6]; u16 unk1; u32 role;
// u8 bsd_name[15]. The SDK header cannot be included in a kext (it wants __IO80211_TARGET), so the fields are read through
// this packed copy of it.
struct VirtIfCreateData {
    uint32_t version;
    uint8_t mac[6];
    uint16_t unk1;
    uint32_t role;
    uint8_t bsd_name[15];
} __attribute__((packed));

IOReturn itlmt7925WifiInterface::setVIRTUAL_IF_CREATE(apple80211_virt_if_create_data *d)
{
    itlmtCall(ctl, 'S', 558);
    VirtIfCreateData *data = (VirtIfCreateData *)d;
    if (!data) return kIOReturnBadArgument;
    if (!itlmtAwdlEnabled()) {
        itlmtTrace(ctl, "awdl:vif create refused (itlmt_awdl=0)");
        return kIOReturnUnsupported;
    }
    if (data->role != 6) {                       // only the AWDL (proximity) interface for now; 7 = APSTA, 8-10 = NAN
        itlmtTrace(ctl, "awdl:vif create role=%u unsupported", (unsigned)data->role);
        return kIOReturnUnsupported;
    }
    if (awdlVif) {                               // already created: answer with the existing BSD name
        const char *n = awdlVif->getBSDName();
        strlcpy((char *)data->bsd_name, n ? n : "", 15);
        return kIOReturnSuccess;
    }

    itlmt7925AwdlInterface *vif = (itlmt7925AwdlInterface *)OSMetaClass::allocClassWithName("itlmt7925AwdlInterface");
    if (!vif) {
        itlmtTrace(ctl, "awdl:vif alloc failed");
        return kIOReturnUnsupported;
    }

    // MAC: userland may hand one in; otherwise derive the AWDL MAC from the primary one (locally administered, different
    // last byte). Apple keeps a separate "awdl MAC" derived from the Wi-Fi MAC the same way. ether_addr stays incomplete in
    // a kext (the SDK header wants __IO80211_TARGET), so the address travels as bytes and is cast at the call sites.
    uint8_t mac[6];
    memcpy(mac, data->mac, 6);
    bool zero = true;
    for (uint32_t i = 0; i < 6; i++) if (mac[i]) zero = false;
    if (zero) {
        bzero(mac, 6);
        getHardwareAddress((ether_addr *)mac);
        mac[0] |= 0x02;
        mac[5] ^= 0x01;
    }

    if (!vif->init(ctl, (ether_addr *)mac, 6, "awdl")) {
        itlmtTrace(ctl, "awdl:vif init failed");
        vif->release();
        return kIOReturnError;
    }
    if (!vif->attach(ctl)) {
        itlmtTrace(ctl, "awdl:vif attach failed");
        vif->release();
        return kIOReturnError;
    }

    // Order (docs/ROADMAP-AWDL.md §9.5). AppleBCMWLANCore::createProxInterface (0xffffff800165dcbe) does withOptions ->
    // attach -> start(core, info) [which registers the ifnet and then registerService(kIOServiceSynchronous) +
    // setRunningState(false)], and the Family's own interfaces additionally bracket the whole setup with deferBSDAttach:
    // AppleBCMWLANSkywalkInterface::start calls deferBSDAttach(true) first, AppleBCMWLANCore::setupDriver calls
    // deferBSDAttach(false) once everything is up.
    //
    //   start(ctl)            IO80211VirtualInterface::start -> IO80211SkywalkInterface::start. That function is the only
    //                         place that stores the controller at ivars+0x30, which is exactly what getController()
    //                         (vtable slot 375) returns - and IO80211VirtualInterface::attachToBpf dereferences
    //                         getController()->[0x120] unconditionally. attachToBpf runs from the BSD client's start
    //                         (registerEthernetInterface -> registerNetworkInterface -> IOSkywalkNetworkBSDClient::
    //                         start -> nexus -> prepareBSDInterface), so without this call the first BSD attach panics
    //                         on a NULL controller (Kernel-2026-10-17-163303/165042: attachToBpf+0x37, CR2 0x120).
    //                         start() also runs IOService::start (IOSkywalkNetworkInterface::start, slot 188), so the
    //                         framework will not call start again (that would panic: "Calling start twice in a row").
    //   registerAwdlInterface the ifnet + BSD client; the client must NOT attach yet, hence the deferral below.
    //   deferBSDAttach(false) removeProperty(IODeferBSDAttach) + registerService(0): the BSD client re-probes only now,
    //                         with the controller in place. IOSkywalkNetworkBSDClient::start fails (defers) while the
    //                         property is true, so the deferral also closes the race with the config thread.
    vif->deferBSDAttach(true);
    if (!vif->start(ctl)) {
        itlmtTrace(ctl, "awdl:vif start failed");
        vif->release();
        return kIOReturnError;
    }
    itlmtTrace(ctl, "awdl:vif started");

    IOReturn rr = vif->registerAwdlInterface(mac);
    if (rr != kIOReturnSuccess) {
        itlmtTrace(ctl, "awdl:vif register rc=%x", rr);
        vif->release();
        return rr;
    }

    vif->registerService(kIOServiceAsynchronous);
    vif->setRunningState(false);
    vif->setNativeCore(core);
    ctl->setAwdlInterface(vif);
    awdlVif = vif;
    vif->deferBSDAttach(false);

    // The BSD client builds the BSD name during its (now asynchronous) re-probe, so getBSDName() may still be empty here;
    // the prefix is ours and the unit is the one registerAwdlInterface handed to the RegistrationInfo.
    const char *n = vif->getBSDName();
    char fallback[16];
    if (!n || !*n) {
        snprintf(fallback, sizeof(fallback), "awdl%u", 0);
        n = fallback;
    }
    strlcpy((char *)data->bsd_name, n, 15);
    itlmtTrace(ctl, "awdl:vif up bsd=%s", n);
    return kIOReturnSuccess;
}
