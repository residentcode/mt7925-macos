# Roadmap: AWDL / AirDrop on the native MT7925 driver

Status 2026-10-17: **phase 2 code landed** (virtual interface + ioctl, behind `itlmt_awdl=1`; test tool ready). Phase 0
(research) and phase 1 (observation) completed on 2026-10-17 — see "Round 2026-10-17" at the end for the 25G241
disassembly, the Family entry points and the wifip2pd finding. This file records what was found, what is unknown (marked
"chưa xác định"), and the phase plan. Nothing here is promised. Decision taken with the user on 2026-10-14: implement real
AWDL in the driver (not the `BrowseAllInterfaces` shortcut).

Branch note (2026-10-17): the work lives on `awdl-tahoe` (worktree `.claude/worktrees/awdl`), cut from `tahoe/wifi` — main
is 15 commits behind and does not carry the Wi-Fi fixes this code builds on. Same rules as before: new files, boot-arg
gated, merged by pull request, never on the Wi-Fi test branch.

## 0. How this work stays out of the way (agreed 2026-10-14)

The native Wi-Fi front end is merged into `main` and under test. AWDL is a **separate workstream** and must not disturb it:

* AWDL work lives on its own branch cut from `main` (`awdl-*`), never on the Wi-Fi test branch, and is merged by pull request.
* New code goes into **new files** (`kexts/itlmt7925Native/itlmt7925_awdl*.cpp`, `docs/awdl/`, `tools/awdl_*`). Changes to the
  existing core / front end files are limited to a hook and are kept small, so they merge cleanly with the Wi-Fi fixes.
* Everything AWDL is behind its own boot-arg (`itlmt_awdl=1`, off by default). With the boot-arg absent the code paths are not
  entered: the verified Wi-Fi behaviour (33-41 MB/s, A-MSDU off) is unchanged.
* Phases 0 and 1 do not touch the data path at all (read-only research, then log-only observation).
* The EFI / OpenCore is not rebuilt for this: kext changes ship in a kext-only release (`kext-v*`, see
  [WIFI-DRIVERS.md](WIFI-DRIVERS.md)).

## 1. Goal and definition of done

* AirDrop between this machine and an iPhone / Mac works in both directions with the Wi-Fi connection kept.
* `awdl0` exists, has an IPv6 link-local address, and `sharingd` browses and serves `_airdrop._tcp` over it.
* Clean-room: no OWL / OpenDrop / opendrop-awdl source is copied (GPL). Protocol facts come from the published papers and from
  read-only inspection of Apple's 24H32 collection, like the rest of the native driver.
  Amended 2026-10-26 (project owner): OWL and OpenDrop may be read for protocol facts only; nothing is copied and the facts are
  restated in our own words with their sources in `docs/AWDL-PROTOCOL-REFERENCE.md`.

## 2. What AirDrop needs (stack, top to bottom)

| Layer | Piece | State on this machine |
|---|---|---|
| UI / daemon | `sharingd`, `NetworkBrowser`; AirDrop over Bonjour `_airdrop._tcp`, HTTPS (port 8770: /Discover /Ask /Upload) | present, idle |
| Trigger | A BLE advertisement from the sender wakes AWDL on the receiver (an iPhone only starts AWDL after it) | Bluetooth works as a controller ([BLUETOOTH.md](https://github.com/thanhnndev/ga403wm-hackintosh/blob/main/docs/BLUETOOTH.md)); BLE scan filter offload `0x1812` fails; effect on AirDrop: chưa xác định |
| Link | AWDL: action frames (PSF, MIF) with Apple OUI TLVs, election, time synchronisation, availability windows, channel hopping over social channels 6 / 44 / 149 | **absent**: no `awdl0`, `getAWDL_RSDB_CAPS` answers Unsupported, no virtual interface |
| Data path | AWDL data frames carrying IPv6 (link-local) | absent |
| Radio | Two logical interfaces on one radio: infrastructure station on the AP channel plus time slices on the social channels | MT7925 firmware has no AWDL; ROC (remain on channel) is already used by the core for join |

## 3. Findings in the 24H32 collection (read-only)

* Interface name: `awdl0` appears in the family; interface is an `IO80211VirtualInterface` handed to the controller with
  `IO80211Controller::setAwdlInterface(IO80211VirtualInterface*)` / `getAwdlInterface()`.
* Host side is Apple's, not ours: `IO80211AWDLPeerManager` (945 symbols), `IO80211AWDLPeer`, `IO80211AWDLMulticastPeer`,
  `IO80211P2PSupervisor` (`requestAWDLEnable`, `setAWDLEnable/Disable`, `setConfiguredAWDLChanSequence`, `awdlReset`, NAN
  channel-sequence adoption), `IO80211VirtualInterface::createPeerManager` -> `IO80211AWDLPeerManager::withInterface(iface,
  ether_addr*)`. So the family already runs peer database, service requests, statistics and the peer-manager timers once it
  has a virtual interface with a MAC.
* Firmware side is the driver's: the family forwards about 100 `apple80211` ioctls `getAWDL_* / setAWDL_*` to the controller
  (static handlers `setAWDL_CTL`, `setAWDL_SYNC_PARAMS`, ...). Apple's reference driver (`AppleBCMWLANProximityInterface`,
  57 symbols) turns them into firmware commands: sync state and enable, election id / metric / params / counts / RSSI
  thresholds, master channel, secondary master channel, social time slots, synchronisation channel sequence
  (`buildAWDLChanSequence`, `buildChanSequenceCommand`), presence mode, availability window alignment with AP beacons, guard
  time, min rate, AF TX mode, encryption keys / type, peer traffic registration, BT-LE state / sync params, OOB requests,
  DFSP / UCSA config, statistics. Which of these a minimal peer needs: chưa xác định (Phase 1 measures it).
* Two implementation models exist in the family: Broadcom firmware executes the protocol (sync, election, hopping) and the
  host only configures it. On MT7925 the host would have to execute it: the ioctls then become configuration of our own state
  machine.
* The native driver today: `itlmt7925_iface_stubs.inc` only has `getAWDL_RSDB_CAPS` (slot 489, Unsupported); the AWDL ioctl
  numbers: the allowed set in `commandAllowed()` (`itlmt7925_wifi.cpp`) contains no AWDL command, so any that arrives is
  refused. The call recorder (`itlmt_native_calls`) logs every request including refused ones; the boot checked on 2026-10-14
  was not exercised with AirDrop open, so it holds no AWDL command yet. Interface roles in use: 1 (primary). The role /
  creation path of the virtual interface in Apple's driver (class, role value, registration call): chưa xác định.

## 4. Prior art and what it proves

* Seemoo Lab (MobiCom 2018 "One Billion Apples' Secret Sauce", USENIX Security 2019 "A Billion Open Interfaces for Eve and
  Mallory"): AWDL builds on 802.11 with election, synchronisation and channel hopping on top; IPv6 data path. These papers are
  the protocol reference (frame formats, TLV list, timing). Full text not yet read: the fetch timed out on this machine; fetching
  them is the first Phase 0 task. No numbers are quoted here until read.
* OWL (user space, Linux/macOS): static election values (slave or winner), fixed channel sequence, monitor mode + injection,
  no concurrent AP connection in its own design.
* A community project reports **MT7925 working in "shared" mode on Linux** (Wi-Fi stays connected, AP must be on channel 6,
  44 or 149, station handles ACKs, TX about 490 kB/s because injected frames leave at most about every 2.5 ms, RX about
  1.3 MB/s). That is evidence the radio can do it with host-driven frames; it also shows the throughput ceiling of a host-timed
  approach and the channel restriction.
* An Apple device only starts AWDL and the AirDrop server after a BLE advertisement from the peer; receiving from an iPhone
  that is "Everyone for 10 minutes" needs that wake-up path as well.

## 5. Constraints specific to this project

* The AP channel here is not a social channel (observed ch 40, 80 MHz). Concurrent operation means time-slicing the one radio
  between the AP channel and social channels (ROC / channel contexts in firmware): chưa xác định how much the firmware allows
  and what it does to the infrastructure link (TX stall, BA state, power save).
* Timing: AWDL windows are in TU (1024 us); host timers (IOTimerEventSource / thread sleep) jitter by milliseconds. The firmware
  TSF / ROC grant events are the only trustworthy clock available.
* Frames: the core can already build and send management and data frames through the TX path (`m3Hdr`, `m3Tx`) and parse raw
  RX; AWDL needs action frames to the broadcast address with a different BSSID and no association: chưa xác định whether the
  firmware accepts them without a station record (needs an own-MAC / BSS entry, probably a second OMAC).
* Privacy: AirDrop visibility is user-controlled (Contacts Only / Everyone). Nothing in the driver should advertise by default
  before the user enables it.
* Wi-Fi performance must not regress: slicing costs airtime; the AWDL cluster should be armed only while AirDrop is open
  (family `requestAWDLEnable`), not permanently.

## 6. Phases

Each phase ends with one reboot and a fixed list of things to read, per the one-check-per-boot rule.

### Phase 0 - complete the research (no reboot)

* [ ] Read the two Seemoo papers and record frame formats and timing here (numbers, TLV list, election rules, data header).
* [ ] From 24H32 disassembly: how the virtual interface is created and registered (class, role, who calls `setAwdlInterface`),
      what the family requires from the interface before `createPeerManager` runs, and the apple80211 structs of the ioctls that
      a minimal peer needs (layouts only from the binary, no guessing).
* [ ] Which Bluetooth events / properties wake AWDL on the Mac side and whether our BT path delivers them.
* [ ] Check what `sharingd` does when there is no `awdl0` (log lines) so Phase 1 knows what to look for.

### Phase 1 - observe (log only, one reboot)

Free first step, no reboot: open Finder > AirDrop (set "Everyone for 10 Minutes"), wait a minute, read `itlmt_native_calls` and
`itlmt_native_trace_tail` for commands that are not in the recorded set above and for `sharingd` errors. Anything new is an
AWDL-side request.

* [ ] Allow and record (`itlmt_native_calls`) every `getAWDL_* / setAWDL_*` ioctl number and size the family sends when AirDrop is
      opened in Finder; answer all with the current Unsupported / success-without-effect so nothing changes.
* [ ] Exit: a table of ioctls actually issued, in order, with payload sizes. Decides the minimal set for Phase 2.

### Phase 2 - virtual interface, no RF (one or two reboots)

* [ ] Create the AWDL virtual interface from the native kext with its own locally administered MAC; the family builds its
      peer manager; `awdl0` shows up with an IPv6 link-local address.
* [ ] Answer the minimal ioctl set with a state machine that is only bookkeeping (enable / disable, config echo).
* [ ] Exit: `ifconfig awdl0` present, no kernel panic on enable / disable cycles, Wi-Fi unaffected (`itlmt_rx_received` keeps
      growing, throughput equal to baseline).

### Phase 3 - passive receive (RF, receive only)

* [ ] Hop to a social channel with ROC while associated, capture action frames, parse headers and TLVs (read only, no
      transmission). Record whether an iPhone with AirDrop open is heard, and how long the infrastructure link is blind.
* [ ] Exit: PSF / MIF from a real device decoded in a property log; infrastructure link survives the slicing.

### Phase 4 - synchronise and announce

* [ ] Transmit PSF / MIF as a slave of a heard master (or as master with static election values), keep availability windows,
      return to the AP channel between slices.
* [ ] Exit: the peer lists this machine in its AWDL peer database / the Mac's `awdl0` neighbour table shows the peer.

### Phase 5 - data path and AirDrop

* [ ] AWDL data encapsulation both ways, IPv6 neighbour discovery, `_airdrop._tcp` over `awdl0`, TCP to the AirDrop port.
* [ ] Exit: send and receive one file with an iPhone; measure throughput (target at least the 0.5 MB/s class seen elsewhere).

### Phase 6 - hardening

* [ ] Arm AWDL only while AirDrop needs it; release the radio cleanly; sleep / wake; roam while AWDL is up; multi-peer.

## 7. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Firmware refuses frames outside an association / BSS, or slicing breaks the link | Phase 3 / 4 blocked | Phase 3 is receive-only and reversible; second OMAC / virtual BSS test before Phase 4 |
| Apple's private ABI of the AWDL interface is larger than the ioctl list suggests | Phase 2 cost grows | Phase 0 / 1 measure it before any code |
| Host timing too coarse for availability windows | Peers drop sync | Use firmware ROC / TSF events, keep our own windows slightly wider (OWL-style guard) |
| BLE wake-up does not reach the Mac side on a non-Apple controller | Receive from iPhone does not start | Test with a Mac peer first (no BLE trigger needed when AWDL is already up on the peer) |
| Reboot-per-test cost | Slow | Batch: Phase 1 logs everything in one boot |

## 8. Wi-Fi improvement backlog (measured 2026-10-14, level 6, ch 40 / 80 MHz, through Cloudflare WARP)

* [x] Remeasured without WARP: about 0.5 Mb/s against 180 Mb/s on a phone of the same network; cause found, the 64-frame RX
      ring dropped 9.8 % of frames (see "Speed measurements" and "Round 24" in [TEST-NATIVE-WIFI.md](TEST-NATIVE-WIFI.md)). Fix
      installed, result after the next reboot.
* [ ] Firmware retry ratio about 0.8 per TX header at level 6 (`txf=..` in `stat-log`): check rate adaptation (STA_REC_RA) inputs
      and whether the retries are probing or loss.
* [ ] RX reorder timeouts about 0.8 % (`reo=` timeout / late fields): holes that never fill; compare BA scoreboard handling.
* [ ] First join after boot can end `auth-fail` (4 authentication frames unanswered although frames were received).
* [ ] Wi-Fi BSD name changes between boots (`en2`, `en4`, `en5`); default route stays on the previous service until the user
      reorders services.
* [ ] Scan: more than one pass per channel at long dwell for 5 GHz DFS channels; hidden SSIDs.

## 9. Round 2026-10-17: phase 0/1 findings and the phase 2 code (all addresses 25G241)

### 9.1 The Family's virtual-interface path (disassembly)

* Userland (wifip2pd) sends `APPLE80211_IOC_VIRTUAL_IF_CREATE` (0x5e) with an `apple80211_virt_if_create_data`
  (`{u32 version; u8 mac[6]; u16 unk1; u32 role; u8 bsd_name[15]}`, 0x1f bytes packed; the request length is clamped to 0x20).
  It arrives through the IO80211 userclient at `apple80211RouteExternal` (0xffffff800236cd2e), dispatch table for code
  0x802869c8 at 0xffffff8002479060, entry `setVirtualInterfaceCreate` (0xffffff8002357f33) — which first offers the request
  to the WCL layer (`sendIOUCToWcl(0x802869c8, 0x5e, buf, 0x20, &flag)`; only `flag == 0` or the sentinel 0xe082280f falls
  through) — then calls `apple80211setVIRTUAL_IF_CREATE` (0xffffff8002268494): `iface->isCommandProhibited(0x5e)`
  (slot 0xcc8, `IO80211SkywalkInterface::isCommandProhibited` 0xffffff80022f9bce forwards to the controller's
  `isCommandProhibited`, i.e. **our gate**), then the protocol slot 558 `setVIRTUAL_IF_CREATE` (vptr + 0x1160).
* Apple's implementation (`AppleBCMWLANInfraProtocol` thunk 0xffffff80015be8fa -> core vtable +0x1328 ->
  `AppleBCMWLANCore::setVIRTUAL_IF_CREATE` 0xffffff80016788f4) dispatches by **role**: 6 = AWDL/proximity
  (`createProxInterface` 0xffffff800165dcbe), 7 = APSTA (`AppleBCMWLANIO80211APSTAInterface::withOptions`), 8/9/10 = NAN.
  For role 6 it instantiates `AppleBCMWLANProximityInterface` (an `IO80211AWDLProtocol : IO80211VirtualInterface`
  subclass; factory `withOptions` 0xffffff80017159e6, vtable `__ZTV30AppleBCMWLANProximityInterface`), fills a
  `RegistrationInfo` (0x130 bytes: +0xc = 3, +0x14 = 2, +0x24 = 0x8000000080, +0x30 = name pointer "awdl", +0x3c = 0x40
  headroom, +0x40 = 4|6, +0x108 = MAC) and calls its `start(core, &info)` (0xffffff8001729376), which registers the ifnet
  with `IOSkywalkEthernetInterface::registerEthernetInterface(info, queues, n, txPool, rxPool, flags)`
  (0xffffff8002b10a84) and wires the core's queue objects.
* The **generic** path the Family itself provides: `IO80211Controller::createVirtualInterface` (0xffffff800229612e, role 7
  only, allocates `IO80211VirtualInterface::gMetaClass` at 0xffffff800248e478), `attachVirtualInterface`
  (0xffffff80022960aa: create -> `attach(controller)` -> `registerService(0)`), `setAwdlInterface`/`getAwdlInterface`
  (0xffffff8002299c24 / 0xffffff8002293256, store/read `[this+0x120]+0x190`; **non-virtual**, added to the generated
  controller header). `IO80211VirtualInterface::createPeerManager` (slot 355) maps role 6 to
  `IO80211AWDLPeerManager::withInterface` (0xffffff80021c14bc), which creates the `IO80211P2PSupervisor`
  (0xffffff80023904ee) — the whole host-side AWDL stack. `isPeerToPeerInterface` (0xffffff800233040a) = roles 6, 8, 9, 10.
  The generic class takes the default BSD prefix ("en", `IOSkywalkEthernetInterface::getBSDNamePrefix` 0xffffff8002b1231a);
  only the Broadcom subclass returns "awdl" — hence the driver-side subclass in 9.2. Slot 490 is `___cxa_pure_virtual` in
  both `IO80211VirtualInterface` and `IO80211AWDLProtocol` (implemented by the Broadcom subclass).
* The Family builds the BSD name from the registry properties `IOInterfaceNamePrefix`/`IOInterfaceUnit`, which the
  registration path sets from `getBSDNamePrefix()` — visible on the primary interface today (`IOInterfaceNamePrefix = en`,
  `IOInterfaceUnit = 5`). A subclass returning "awdl" therefore yields `awdl0`.

### 9.2 Phase 2 code (this branch)

* `kexts/itlmt7925Native/itlmt7925_awdl.{h,cpp}`: `itlmt7925AwdlInterface : IO80211VirtualInterface` (overrides
  `getBSDNamePrefix` -> "awdl" and the pure slot 490) plus `itlmt7925WifiInterface::setVIRTUAL_IF_CREATE` (slot 558):
  alloc -> `init(ctl, mac, 6, "awdl")` -> `attach(ctl)` -> pool + [TxSubmission, TxCompletion, RxCompletion] queues ->
  `RegistrationInfo` filled like `createProxInterface` -> `registerEthernetInterface` -> `registerService` ->
  `ctl->setAwdlInterface(vif)`; answers the BSD name back. Role 6 only; 7-10 stay unsupported for now.
* Gated by the boot-arg `itlmt_awdl=1` (off by default; the ioctl is also refused at the gate without it).
* `tools/awdl/vif_create_test.m`: sends ioctl 0x5e itself (Apple80211Open/Bind/RawSet through the private framework) to
  exercise the handler without wifip2pd; see §9.5 for the 25G241 calling ABI it bakes in.
* Generated ABI additions (tools/gen_io80211_header.py + tools/io80211_abi.py): new slot-exact header
  `kexts/itlmt7925/io80211/IO80211VirtualInterface.h` (506 slots, checker-clean; the unnamed pure slot 490 gets a
  placeholder so the layout stays exact), `registerEthernetInterface` on IOSkywalkEthernetInterface, and
  `setAwdlInterface`/`getAwdlInterface` on IO80211Controller.

### 9.3 Phase 1 observation: wifip2pd never sends the ioctl (its own gate)

* On every boot wifip2pd (the P2P daemon, `/usr/libexec/wifip2pd`, imports `Apple80211RawGet/RawSet`) is asked by airportd
  (XPC, `com.apple.awdl:xpc`) for an AWDL interface and answers:
  `No AWDL interface found. Creating AWDL interface` -> `Cannot create AWDL interface because the device is missing the
  capability` -> `Failed to create interface AWDL on en5 with error <Apple80211Error Operation Not Supported>`. The ioctl
  never reaches the driver: `itlmt_native_calls` holds no 0x5e (and no AWDL-range ioctl 0x74-0xe1) on any boot.
* The capability model is built in userland (CoreP2P `WiFiDriverCapabilities`: supportsAWDL...); it is **chưa xác định**
  which query feeds it. This is why phase 2 is tested with the tool and why the driver-side handler matters: once `awdl0`
  exists, wifip2pd's "No AWDL interface found" path is not taken.

### 9.4 Next

1. Reboot with `itlmt_awdl=1` (config.plist boot-args updated 2026-10-17; NVRAM picks it up at boot through OpenCore
   WriteFlash) and run `vif_create_test`: expect `RawSet(0x5e ...) rc=0 bsd_name='awdl0'`, `ifconfig awdl0` present,
   `awdl:vif up bsd=awdl0` in `itlmt_native_trace`, no panic, Wi-Fi unaffected.
2. Then check whether wifip2pd adopts the existing interface (it should stop trying to create one) and start recording the
   AWDL ioctls it sends once the peer manager exists (phase 3 groundwork).
3. Phase 3+ per section 6: channel slicing (ROC), action frames (PSF/MIF), then the data path.

### 9.5 The 2026-10-17 panics: the virtual interface never ran `start()` (fixed 2026-10-18, HW test pending)

The first `vif_create_test` run with `itlmt_awdl=1` reached the handler, built and registered the interface, answered the
ioctl - and then the machine panicked twice on the first BSD attach
(`/Library/Logs/DiagnosticReports/Kernel-2026-10-17-163303.panic`, `…-165042.panic`):
`IO80211VirtualInterface::attachToBpf` (+0x37) dereferences `getController()->[0x120]` and the register held 0
(CR2 = 0x120). `getController()` (vtable slot 375) returns the controller stored at ivars+0x30 - and the only code that
stores it there is `IO80211SkywalkInterface::start` (0xffffff80022f5792, store at 0x22f583f). The phase 2 code went
straight from `attach` to `registerEthernetInterface`, so `attachToBpf` - which runs from the BSD client's start
(`registerNetworkInterface` -> nexus -> `prepareBSDInterface`) - found a NULL controller.

The fix mirrors what Apple's own interfaces do (all offsets 25G241; the same sequence is what the working infra
interface, `itlmt7925_iface.cpp`, already used):

1. `deferBSDAttach(true)` (slot 297): sets `IODeferBSDAttach`, so `IOSkywalkNetworkBSDClient::start` (0x2aff562, defer
   check at 0x2aff6ae) returns false and the BSD client re-probes later; this also closes the race with the config thread.
2. `start(ctl)`: stores the controller at ivars+0x30 (`IO80211VirtualInterface::start` ->
   `IO80211SkywalkInterface::start`; slot 375 = `getController` reads it back), registers the ifnet, and runs
   `IOService::start` along the way - it must be the single start call ("Calling start twice in a row" otherwise).
3. `registerAwdlInterface`: `RegistrationInfo` filled like `createProxInterface` (0x165dcbe) plus `+0x38 = 0` - the unit
   `registerNetworkInterface` turns into the `IOInterfaceUnit` property (it only sets it when the value is >= 0 and the
   BSD client needs it to build its name); unit 0 -> `awdl0`.
4. `registerService(kIOServiceAsynchronous)` + `setRunningState(false)` (slot 297's neighbours; the pattern at the tail of
   the prox interface's start, 0x1729825 / 0x1729838, and of `setupDriver`).
5. `ctl->setAwdlInterface(vif)`, then `deferBSDAttach(false)`: the BSD client re-probes now that the controller is in
   place and produces `awdl0`.
6. The BSD name is built during that asynchronous re-probe, so `getBSDName()` may still be empty when the ioctl answers;
   the handler falls back to `awdl0` (prefix "awdl", unit 0).

`tools/awdl/vif_create_test.m` (the Obj-C rewrite; 25G241 calling ABI: `Apple80211BindToInterfaceWithService` wants
(ref, service, name) and `Apple80211RawSet` takes (ref, apple80211req*), not the classic (ref, cmd, data, len)) is the
tool that exercises this. After the next boot: `rc=0 bsd_name='awdl0'`, no panic, `ifconfig awdl0` present,
`awdl:vif started` then `awdl:vif up bsd=awdl0` in `itlmt_native_trace`.

## 10. Round 2026-10-19: phase 2 verified on hardware, wifip2pd adopts the interface, the AWDL protocol class

### 10.1 The phase 2 test passed (no panic)

`./vif_create_test en5` on the boot with `itlmt_awdl=1`: `BindToInterfaceWithService(service=0x1803, en5) rc=0` +
`RawSet(0x5e VIRTUAL_IF_CREATE, role=6) rc=0 bsd_name='awdl0'`, no administrator rights needed. `ifconfig awdl0`:
`flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST> mtu 1484`, MAC `de:56:7b:34:81:d4` (primary + locally-administered
bit + last byte flipped), `inet6 fe80::dc56:7bff:fe34:81d4%awdl0`. `itlmt_native_trace`: `awdl:vif started`,
`awdl:vif up bsd=awdl0`. No panic, Wi-Fi unaffected (ping 2.9 ms, `wifi-state=connected`). The 2026-10-17 panics are
gone.

### 10.2 wifip2pd adopts the interface and maps the rings

Right after the create (log 16:36:03): `(IO80211) Apple80211BindToInterfaceWithService: Sanity test passed` ->
`Found new interface awdl0 with role AWDL` -> `Created AWDLInterface<P2PController<AppleIO80211Driver>>[awdl0]<de:56:7b:34:81:d4>`
-> `Successfully updated awdl0 link state to Link Up` -> `(IO80211) _createMemoryMappings: ringBuffer[0x102f80000]
ringSize[0x4b000], ringState[0x102ac0000] ringSize[0x1000]` -> then the normal power/eligibility bring-up
(`Registering for Power State changes from wifid`, `OS Eligibility: 5G/160 answer eligible`). The one failure is
`APPLE80211_IOC_COMPANION_SKYWALK_IF_CREATE` (0x125, len 24) -> `return 58/0x3a` ->
`Failed to create the companion low latency interface for AWDL: <WiFiP2PError Unavailable>` - and it is **non-fatal**:
the daemon continues and the interface stays up.

### 10.3 What the AWDL ioctls and the peer manager need (25G241 disassembly)

* `apple80211setCOMPANION_SKYWALK_IF_CREATE(controller, iface, uc, req)` (0xffffff800228b4b8) first offers the request to
  the userland WCL (`IO80211Glue::sendIOUCToWcl` when `[controller+0xd8] != NULL`); on this Intel/kext path that is NULL
  and it tail-calls `apple80211setCOMPANION_SKYWALK_IF_CREATE(iface, req)` (0xffffff800226a32f), which calls slot
  `[vptr+0xcc8]` with the ioctl number, then `safeMetaCast(iface, IO80211AWDLProtocol::gMetaClass)` and calls slot 562
  (`0x1180` from the vptr = dump slot 562). So **the vif must be an `IO80211AWDLProtocol`** or every AWDL ioctl answers
  `0xe082280e` before reaching the driver.
* The `apple80211{get,set}AWDL_*` dispatchers (e.g. `getAWDL_SYNC_ENABLED` 0xffffff800227bc2f) do NOT cast the vif: they
  read the peer manager from `[iface+0x120]+0x28`, cast THAT to `IO80211AWDLPeerManager` and call its methods. The peer
  manager is created by the Family's `IO80211VirtualInterface::createPeerManager` (0xffffff80023308c2, virtual slot 355):
  for role 6 it runs `IO80211AWDLPeerManager::withInterface(vif, mac)` (role read from `[vif+0x120]+0x58`, MAC from
  `getMacAddress`). That is the Family's own code path, so it runs for this driver too once the vif exists.
* `AppleBCMWLANProximityInterface::setCOMPANION_SKYWALK_IF_CREATE` (0xffffff8001722ef2) reads `action` at req+4
  (0 = destroy, 1 = create), builds `apple80211SkywalkIfCreateParams_t {class ptr, MAC, 9, 2}`, calls
  `AppleBCMWLANCore::createSkywalkInterface(&vif->companion, params, vif)`, then
  `companion->setCompanionInterfaceId(vif->getInterfaceId())` and `strncpy(req+8, companion->getBSDName(), 0x10)`.
  `apple80211_companion_skywalk_if_create` is 24 bytes: `u32 version; u32 action; char bsd_name[16]`.
* The AWDL protocol class: `IO80211AWDLProtocol : IO80211VirtualInterface`, 571 slots, 0x130 bytes (same ivars), 63 new
  pure virtuals (slots 507..569) named from `AppleBCMWLANProximityInterface`: getSSID/CHANNEL/BSSID/STATE, the
  AWDL_SYNC_* / AWDL_ELECTION_* / AWDL_MASTER_CHANNEL / AWDL_SYNC_FRAME_TEMPLATE / PSF_DWELL / LLW_PARAMS families, and
  `setCOMPANION_SKYWALK_IF_CREATE` at slot 562. Slot 490 (pure in both classes) is the Broadcom `setMacAddress(ether_addr&)`.

### 10.4 Code in this round

* `tools/io80211_abi.py`: `IO80211AWDLProtocol` added (parent IO80211VirtualInterface, size 0x130,
  children AppleBCMWLANProximityInterface) + the IOReturn rule for its methods; dumps for 25G241
  (`IO80211AWDLProtocol`, `AppleBCMWLANProximityInterface`, MetaClass) added; `tools/kc_extract.py` extracts one fileset
  entry from the KC for disassemblers.
* `kexts/itlmt7925/io80211/IO80211AWDLProtocol.h`: generated, layout-checked (`io80211check: ok`).
* `itlmt7925AwdlInterface` now derives from `IO80211AWDLProtocol`; all 63 protocol slots are implemented as tracing stubs
  (`IOLog("itlmt7925: awdl proto %s (stub)")` + `kIOReturnUnsupported`, 48-line budget) and
  `setCOMPANION_SKYWALK_IF_CREATE` logs the action and answers unsupported (non-fatal for wifip2pd). The kernel log after
  the reboot will show which slots the Family and the peer manager actually call - that is the phase 3 to-do list.

### 10.5 Next

1. Reboot, `./vif_create_test en5`, then `log show --last 2m | grep "awdl proto"`: the calls that appear name the slots
   the peer manager drives (expect `setAWDL_SYNC_ENABLED`, `setAWDL_SYNC_PARAMS`, `setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE`,
   `setPSF_DWELL` once AirDrop/peer discovery starts).
2. Phase 3: implement the channel sequence (ROC slicing) and the PSF/action-frame TX those slots need; the LLW companion
   interface (slot 562) afterwards.

### 10.6 Round 2026-10-19 (evening): the AWDL ioctls never left the driver - `ret=58` is this driver's own gate

AirDrop-off trace on the boot with the AWDL protocol class (`./vif_create_test en5`, then the kernel log):

* `(IO80211Family) com.apple.p2p.awdl0:` lines prove the **Family's AWDL peer manager runs for this vif**:
  `IO80211AWDLPeerManager::updateLocalRequests`, `::interfaceStateChange`, `isScanDisallowedByAwdl`,
  `IO80211PeerManager::setScanningState` (source ScanManagerFamily / DriverInternalScanDone). So
  `IO80211VirtualInterface::createPeerManager` (slot 355) did run for role 6.
* `IO80211AWDLPeerManager::getAwdlChannelSequence[28260] : unable to get AWDL channel sequence ret=58` - the peer
  manager asks the driver through `apple80211RouteInternal(0xc02869c9, 0x81, iface, out)`, which indexes the Family's
  route table at `0xffffff800247cea0` (get) / `0xffffff800247e0f0` (set) by `ioctl - 1`; entry 0x81 is
  `apple80211getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(iface, out)` (0xffffff8002263a45), whose first step is
  `call [vptr + 0xcc8]` = **slot 411 `IO80211SkywalkInterface::isCommandProhibited(int)`** (0xffffff80022f9bce), which
  tail-calls the **controller's** `isCommandProhibited`. Returning non-zero there returns that value to the caller -
  and `ret=58` is exactly `itlmt7925Wifi::isCommandProhibited`'s `kProhibited` (0x3a, itlmt7925_wifi.cpp:127).
* So the whole AWDL ioctl family (channel sequence, sync params, election, PSF dwell, LLW params, companion create
  0x125, OS eligibility 0x23f) was refused by this driver's own allowlist before reaching either the Family's handlers
  or the interface's `IO80211AWDLProtocol` slots. `wifip2pd`'s `Apple80211IOCTLSetWrapper ... return 58/0x0000003a`
  lines are the same gate.
* Fix in this round: `commandAllowed()` now lets the AWDL family through when `itlmt_awdl=1` (the ioctl numbers are the
  route-table indices above: 0x74-0x8b, 0x8f, 0x94-0x97, 0x9b, 0x9e, 0xa2-0xa7, 0xc9-0xe9, 0xf4-0x107, 0x125,
  0x152-0x158, 0x15f, 0x172, 0x1a0, 0x1a1, 0x1b0, 0x1d3, 0x23f). With the boot-arg absent nothing changes.
* The AWDL stub trace now goes through `itlmtTrace(getController(), ...)` (the `itlmt_native_trace` ring, mirrored to
  NVRAM): IOLog from this kext does not reach the unified log on this machine. `nvram -p | grep itlmt-native-trace`
  after the reboot shows `awdl:proto <slot>` / `awdl:companion action=<n>` - i.e. which protocol slots the peer manager
  drives once the gate is open.

### 10.7 Round 2026-10-19 late: the gate was the whole AWDL blocker - the peer manager now drives the protocol slots

Reboot with the 10.6 gate fix (`com.itlmt7925.native` UUID `56A5CD9C-0E15-3C07-8C55-BA309D3B9DA4`), AirDrop on,
`./vif_create_test en5`. The driver's own trace (`nvram -p | grep itlmt-native-trace`) now shows the whole AWDL ioctl
family **allowed** and the interface's protocol slots being called for the first time:

```
ioctl:cmd=75 allowed   ioctl:cmd=7d allowed   ioctl:cmd=97 allowed   ioctl:cmd=7e allowed
ioctl:cmd=a4 allowed   ioctl:cmd=125 allowed  ioctl:cmd=23f allowed  ioctl:cmd=81 allowed
awdl:vif started; awdl:vif up bsd=awdl0
awdl:proto getAWDL_SYNC_ENABLED
awdl:proto setAWDL_BSSID
awdl:proto setAWDL_SYNC_FRAME_TEMPLATE
awdl:proto setAWDL_ELECTION_METRIC
awdl:proto setAWDL_PEER_TRAFFIC_REGISTRATION
awdl:companion action=1 (unsupported)
awdl:proto getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE
```

and wifip2pd brings the interface up on its own (`No AWDL interface found. Creating AWDL interface`, link up, IPv6,
mDNS agent, `OS Eligibility: 5G/160 = ENABLED`). `setCOMPANION_SKYWALK_IF_CREATE` now answers the stub's
`kIOReturnUnsupported` instead of the old gate's `0x3a` - proof the ioctl reached the driver.

**The remaining blocker is one slot.** `IO80211AWDLPeerManager`'s enable path (`doEnable` -> `configure`) aborts on the
first unsupported answer, and the first parameter it hands over is the BSSID:

```
IO80211AWDLPeerManager::enable: <awdl0> enable 1 ...
doEnable:2829 Enabling AWDL Interface because of setLinkState... configured = 0
IO80211AWDLPeerManager::configure Configuring AWDL interface
IO80211AWDLPeerManager::initParamsToDefaults RTGConcurrency:1 RTScan:1 RTGSteering:1 ...
IO80211AWDLPeerManager::configure  Unable to set BSSID ret e00002c7
IO80211AWDLPeerManager::configure configure error!! :(
doEnable:2839 ERROR: cannot configure AWDL after enabling interface!! ret=e00002c7 _enabling 1
<awdl0> disable 1 ont 0   -> AWDL DISABLE SET last state 0
```

So this round accepts the configure handshake: `awdlAccept()` (slot 540 `setAWDL_BSSID`, 541 `setAWDL_ELECTION_METRIC`,
549 `setAWDL_SYNC_FRAME_TEMPLATE`, 551 `setAWDL_PEER_TRAFFIC_REGISTRATION` and the rest of the parameter setters
536-569) answers `kIOReturnSuccess` and traces the slot, and the four AWDL structs that are public in
`IOKit/80211/apple80211_ioctl.h` are stored/printed (`apple80211_awdl_sync_enabled` {version, unk1, enabled},
`apple80211_awdl_election_metric` {version, metric}, `apple80211_awdl_sync_frame_template` {version, payload_len,
payload}, `apple80211_awdl_peer_traffic_registration` {version, addr, name_len, name[152], active} packed). The
out-of-band request slots (548/557/563) and the LLW/companion path (562/564) still answer unsupported - they expect a
response or a second interface, not just a parameter handshake. No RF work is done by these slots: the point is to let
`configure()` finish so the next missing piece (the channel sequence, then the PSF/ROC path) shows up in the trace.

### 10.8 Round 2026-10-20: the PSF frame reaches the driver - first AWDL transmit path

With the configure handshake in place the Family's AWDL state machine runs end to end: the trace shows the sync frame
template being handed to the driver over and over, and the state dump reports AWDL up with no peers because the radio
never transmits anything:

```
awdl:set SYNC_FRAME_TEMPLATE len=110
awdl:accept setAWDL_SYNC_PARAMS / setAWDL_SYNC_STATE / setAWDL_ELECTION_PARAMS
setSyncFramePeriod::Setting PSF period to 440
AWDLStateDump: AWDL up time: 29 Secs, DE:56:7B:34:81:D4 peers:0, cached:0(0), rtg(0)
AWDLStateDump: PSFDwell Support/Active: 1/0 {0ms 0ms}
IO80211AWDLPeerManager::monitorAWEvents WARNING: Did not get AW start event ...
```

`netstat -I awdl0` = 0 TX / 0 RX: nothing ever leaves the box, which is why a second Mac/iPhone with AirDrop open cannot
see it. The firmware has no AWDL channel sequencing of its own, so the driver has to provide the off-channel slot.

What this round adds (phase 3 transmit half):

* `ItlNativeAwdlSync` (`ITL_NATIVE_AWDL_SYNC`, itl_native_bridge.h) - the native interface hands one complete 802.11 frame
  plus the channel and dwell to the core. `itlmt7925::awdlSubmit()` only copies it into a mailbox under `cmdLock` and sets
  `CMD_AWDL_SYNC`; nothing blocks on the chip in the caller's context (the Family's work queue).
* `itlmt7925::awdlSyncTx()` runs on the link thread next to the scan command: `m3Roc(true, channel)` (UNI_ROC on the AWDL
  channel, bounded to 80 ms of pumping so the AP keeps being serviced), `m3Tx(frame, len, 0, 13, ...)` (management, action,
  fixed OFDM 6 Mb/s, ALT X0 queue), then `m3Roc(false, channel)` back to the infrastructure channel.
* ROC grants are now parsed after link up as well (`rocWant`, set by m3Roc and cleared by the grant or the abort). The old
  early return assumed no ROC event can arrive while associated; off-channel AWDL needs exactly that.
* `setAWDL_SYNC_FRAME_TEMPLATE` (slot 549) queues the frame the Family builds every PSF period (110 bytes on this build,
  social channel 6, 60 ms ROC window). The core service pointer is handed to the vif by the vif-create handler
  (`setNativeCore`).

What to expect on the next boot: `awdl:sync tx queued len=110 rc=0` in the interface trace and `awdl sync: tx ch=6
len=110 ok=1` in the driver log once the link is up (the AWDL TX only runs while associated - the link thread is what
services the command). A second AirDrop device should then start seeing this Mac, even though reception (peer table) still
needs the RX half: routing the AWDL action frames the firmware delivers during the ROC window up to
`IO80211AWDLPeerManager`, plus the missing "AW start event" the peer manager keeps warning about.

### 10.9 Round 2026-10-20 (same boot): the PSF leaves the radio - ROC works while associated

Test after the 10.8 build (core `b155d936`, native `618de933`), `awdl0` created with `~/vif_create_test en5`, then the
AirDrop window closed and reopened (the order matters: the vif must exist *before* the AirDrop session asks for AWDL,
otherwise wifip2pd initialises the interface but never re-runs `doEnable` - the 10:13 boot stalled exactly there, trace
ending at `getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE` with `set SYNC_ENABLED` never called).

With that order the configure handshake completes and the new transmit path runs:

```
awdl:set SYNC_FRAME_TEMPLATE len=110
awdl:sync tx queued len=110 rc=0
awdl:accept setAWDL_SYNC_PARAMS / setAWDL_SYNC_STATE / setAWDL_ELECTION_PARAMS
```

and in `m3-log` (the core's own ring, `ioreg -l | grep m3-log`):

```
roc_acquire rc=0 st=0
roc event bss=0 tok=10 status=0
awdl sync: tx ch=6 len=110 ok=1
roc_abort rc=0 st=0
```

That answers the 10.8 open question: **the firmware grants UNI_ROC while the station is associated** (tok 10, 11, 12, ...,
status 0), so the off-channel slot works as implemented - relax-on-`rocWant` was the right change and no infra
disconnect is needed. Nine frames went out this way.

**The finding: the Family does not hand the template over once per period.** It hands it over when it changes (four
times in five minutes here), so the 10.8 note "The Family re-issues this template every sync period, so every call is one
PSF transmission" was wrong: 9 frames in ~5 minutes is nowhere near the 440 ms PSF cadence the peer manager logs
(`setSyncFramePeriod::Setting PSF period to 440`). The other device never saw the Mac because the sync frames were
essentially absent, not because they were malformed.

So the interface now owns the cadence: `setAWDL_SYNC_FRAME_TEMPLATE` copies the bytes (`awdlPsfFrame`, the Family's buffer
is not ours to keep) and `awdlPsfStart()` arms an `IOTimerEventSource` on the interface's work loop; `awdlPsfTick()` hands
one frame to the core every 440 ms with a 30 ms ROC window, and `setAWDL_SYNC_ENABLED=0` cancels the timer (AWDL off must
mean nothing leaves the radio). The immediate on-template transmit is gone: the timer's first tick is the transmit.

Still open after this round: the peer manager's `Did not get AW start event` warning (it wants
`APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START` posted per window; the numeric id has not been confirmed yet - the name table
in the kernel points at 0x4f3 but the spacing of neighbouring ids makes that unverified), and the receive half - routing
received AWDL action frames up to `IO80211AWDLPeerManager` so `peers:` leaves 0 and election can run.

### 10.10 Round 2026-10-20 evening: a real root cause - the template is a TLV blob, not a frame

The 10.9 build runs (timer armed, `awdl:psf armed len=110`, ticks reaching the core), but the trace of the template's first
byte exposed a defect that is worse than the cadence one: the 110 bytes the Family hands over are **not an 802.11 frame**.

```
awdl:psf armed len=110 type=06 sub=0
```

`0x06` is not a frame-control byte - an AWDL action frame would start with `0xd0`. The kernel confirms where it comes from:
`IO80211AWDLPeerManager::updateSyncFrameTemplate` (0xffffff80021dc5b2) builds the buffer with `buildServiceParamTlv`,
`buildAWDLElectionParamsVer2`, `buildAwdlDataPathStateTlv`, `buildEnhDataRateTlv` ... and `buildServiceParamTlv`
(0xffffff80022118a6) starts with `mov byte ptr [rsi], 6` - TLV type 6, little-endian length, then the value. So the payload is
a **TLV blob**, exactly what the Family also feeds into the master indication template (`buildMasterIndicationTemplate`, no
`0xd0`/OUI anywhere in it). On Apple hardware the firmware wraps it: the driver only hands the bytes to the chip.

This driver's firmware has no AWDL support, so the wrapper has to be built by the host before `m3Tx()` - and it was not: the
raw TLV blob went out as an MPDU whose first 24 bytes (TLV types/lengths) were interpreted as addresses. That is why nothing
ever appeared on the other device's AirDrop, while the counters looked healthy (`awdl sync: tx ... ok=1` only means the
firmware accepted the descriptor).

What this round adds:

* The transmit wrapper is **not** guessed. A capture probe in the RX path (`rxParseFrame`, `itlmt7925_m2.cpp`) now looks for
  management Action frames (`fc 0xd0`) in the received buffer, notes whether the Apple OUI `00:25:00` sits at +25 (category
  at +24), and hex-dumps the first 40 bytes of the first four into `m3-log`:
  `awdl af rx len=.. cat=.. apple=1 <hex>`. The frame is only heard while the radio is on the social channel, i.e. inside the
  ROC window, so that window grew from 30 ms to 60 ms for these runs.
* Reading those bytes from a real iPhone/Mac gives the exact header (FC, DA, SA/BSSID, category, OUI, and where the TLV blob
  begins), which is also the format the receive half has to parse. Both directions need the same layout, so the probe is the
  cheapest way to obtain it: it either captures a frame (ground truth immediately) or proves the firmware does not forward
  broadcast action frames at all (then the RX filter, not the parser, is the next suspect).

Once the layout is known the transmit path becomes three lines of wrapping in `awdlSyncTx()` - 802.11 header with the AWDL MAC
(`de:56:7b:34:81:d4`) and the peer manager's BSSID (`00:25:00:ff:94:73`), category, OUI, then `awdlFrame` - and the same
parser routes received frames to the peer manager.

First capture attempt (d8a7d57, core `b614d747`, native `b14c06b4`): four dumps arrived, all `apple=0`, and none of them is
an AWDL frame - the probe accepted any `0xd0` byte inside the received buffer, and data frames (IP payloads, IPv6 multicast
DA `33:33:00:00:00:fb` in one of them) contain that byte too. It also ran with no `awdl0` at all (the vif is recreated per
boot and had not been created), i.e. on the infrastructure channel, so nothing was even looking at the social channel.

The probe is now strict: duration 0, DA broadcast or unicast, TA and BSSID unicast and non-zero (any real 802.11 header
satisfies these, data frames found at a stray `0xd0` almost never do), the dump budget is 10 frames plus every frame that
carries the Apple OUI at +25, and every 25th hit logs a one-line summary so a long run shows whether anything is arriving at
all. Core `e756604c`.

### 10.11 The transmit wrapper, from ground truth (not a guess)

Two facts settled the format, and they agree with each other:

1. The kernel. `IO80211AWDLPeerManager::actionFrameHasCorrectOUI` (0xffffff8002210200) compares three bytes of the frame
   body at fixed offsets: `0x17`, `0xf2`, then `8` - the Apple OUI and the AWDL type, exactly where a vendor-specific action
   frame puts them. `AppleBCMWLANNetAdapter::sendActionFrame` (0xffffff80015c511a) never writes an 802.11 header at all: it
   builds a descriptor (`{addr, addr, len, ...}`) and the firmware frames it. So on Apple hardware the host never sees the
   header, and no amount of kernel reading can produce it.
2. OWL (the open AWDL implementation) has the wire format: `src/frame.h` / `src/tx.c`. The action frame is
   `ieee80211_hdr{fc=0x00d0, dur=0, addr1=dst, addr2=src, addr3=AWDL_BSSID 00:25:00:ff:94:73, seq<<4}` followed by
   `awdl_action{category=0x7f, oui=00:17:f2, type=8, version=0x10 (1.0), subtype=0 (PSF) / 3 (MIF), reserved,
   phy_tx u32, target_tx u32}` and then the TLV blob. PSF is broadcast (`state->dst = ETHER_BROADCAST`), sent every 440 TU
   when following a master - the same 440 ms the Family's log reports.

That is now what the driver builds. `setAWDL_SYNC_FRAME_TEMPLATE` sees the Family's bytes for what they are (TLVs, first
byte a TLV type) and prepends the 40-byte wrapper into `awdlPsfFrame` (broadcast DA, the AWDL interface address as SA, the
AWDL BSSID - `setAWDL_BSSID`'s value when the Family provides one, the standard `00:25:00:ff:94:73` otherwise). The core
stamps the per-transmission fields just before `m3Tx()`: `seq_ctrl` at +22 (incrementing, 12 bits) and the two microsecond
timestamps at +32/+36.

The earlier trace line proves how necessary this was: `awdl:psf armed len=110 type=06` - the first byte was TLV type 6
(`buildServiceParamTlv`), so the previously transmitted MPDUs began with TLV bytes where the receiver expects addresses.
`ok=1` only ever meant "the firmware accepted the descriptor". With the wrapper the frame is 153 bytes
(24 + 16 + 113) and starts with `d0 00 00 00 ff ff ff ff ff ff de 56 7b 34 81 d4 00 25 00 ff 94 73 .. .. 7f 00 17 f2 08 10 00 00`.
The new `awdl:psf armed` line prints the first bytes of exactly that, and the RX probe now looks for `0x7f / 00:17:f2 / 8`
instead of the wrong `00:25:00` it checked before (which is the BSSID OUI, not the action OUI).

Also observed in the same run: the Family re-sends `setAWDL_SYNC_FRAME_TEMPLATE` every few seconds with updated TLVs
(110 -> 112 -> 113 bytes) - i.e. its state machine is alive and the sync parameters are changing (channel sequence, counters),
so the cadence and the content both come from it.

### 10.12 Receive half: action frames go to the Family's peer manager

The transmit half is now correct (`awdl:psf armed tlv=110 frame=150 fc=d0 cat=7f oui=0017f2 type=08 sa=de567b3481d4`,
`awdl sync: tx ch=6 len=150 ok=1`, ROC granted every time), and the other device still shows nothing. That is expected once
the discovery chain is looked at: AirDrop lists a device only after that device's **service** is learned, and services are
exchanged in AWDL action frames (service request / response, MIF). The Family builds them in `IO80211AWDLPeerManager`
(`buildServiceResponseTlvs`, `buildMasterIndicationTemplate`) in reply to frames it receives. With no receive path the peer
list stays empty, so nothing is advertised and nothing shows up - transmit alone can never be enough.

So the frames the radio hears now go where the Family expects them:

* `rxParseFrame` already recognises an AWDL action frame by its header (24-byte 802.11 action frame with `addr3` a unicast
  BSSID, then `0x7f / 00:17:f2 / 8`). Those frames are copied into a four-entry ring (`awdlRxRing`) - single producer, the RX
  thread - and nothing else in the RX path changes.
* The AWDL interface drains the ring once per PSF tick through a new platform function, `itlmt-native-awdl-rx-v1`
  (`ITL_NATIVE_AWDL_RX`), and hands each frame to `IO80211AWDLPeerManager::actionFrameInput`.
* Reaching that method needed two offsets from the KernelCollection, and both are checked at run time: the peer manager is
  `IO80211SkywalkInterface::getPeerManager()`, i.e. `[[this+0x120]+0x28]` (`0xffffff80022f5fa8`), and `actionFrameInput` is
  **virtual slot 73** - 0x238 bytes into the object's vptr once the two vtable header words are accounted for. Before the
  first call the driver verifies that slot 74 is `doEnable` by comparing the distance between the two function pointers
  (`0xffffff80021c5216 - 0xffffff800220e1ae`); a KASLR slide cancels out, and a mismatch means the table is not what this
  code assumes, in which case the frame is dropped and `awdl:rx slot check failed` is traced instead of calling an unknown
  pointer.

The trace tells the two possible outcomes apart on the next run:

* `awdl:rx af len=.. n=.. pm=.. fn=.. sub=..` - frames arrive on the social channel, the peer manager is being fed, and the
  next observable is `IO80211AWDLPeerManager` state in the unified log (peers, election, sync).
* nothing at all - the firmware is not delivering other-BSS management frames to the host while parked on the social channel.
  Then the RX filter (chip side), not the parser, is the next thing to change.

### 10.13 Why nothing was ever received: the RX filter only passed our own BSS

The receive half was wired up (10.12) and still nothing arrived - not one `awdl:rx`, not one `awdl af rx` - while the transmit
half kept running (`psf tick 200 len=153`, `awdl sync: tx ... ok=1`). That rules out the parser and the AWDL channel: the
frames were never handed to the host in the first place.

The vendor driver programs the station's RX filter explicitly. `mt7925_configure_filter` (mt7925/main.c) maps mac80211's
filter flags onto `MT7925_FILTER_ENABLE` (BIT(31)), `MT7925_FILTER_OTHER_BSS` (BIT(6)), `MT7925_FILTER_CONTROL` (BIT(5)) and
sends them with `mt7925_mcu_set_rxfilter()`, i.e. a `MCU_UNI_CMD(BAND_CONFIG)` message whose TLV is
`UNI_BAND_CONFIG_SET_MAC80211_RX_FILTER` (0x0c), value layout `{mode, rsv[3], fif, bit_map, bit_op, pad[51]}`; `mode = 0`
replaces the whole filter with `fif`. This driver never sent that message, so the firmware kept its default - our own BSS
only. Frames from another device (an AWDL action frame carries `addr3 = 00:25:00:ff:94:73`, not our AP's BSSID) were dropped
in the firmware, on the infrastructure channel as much as inside a ROC window. That is also why the probe saw four *data*
frames earlier and never a management one.

`awdlSyncTx()` now programs `fif = BIT(31) | BIT(6)` once per link, right before the first ROC window, and the trace gained a
line that separates the two possible failures from now on:

```
awdl stat: try=.. tx=.. rocfail=.. rocframes=.. bcn=.. af=..
```

`rocframes` counts every frame parsed while a ROC window is open and `bcn` the beacons among them. If `rocframes` grows but
`af` stays 0, the radio is healthy and simply no AWDL peer is on the air; if `rocframes` stays 0, the filter is still not
letting frames through and the next step is the chip-side promiscuous path, not the AWDL code.

### 10.14 The receive half works - but only on the channel we park on

With the RX filter in place (10.13) the counters finally answer the question that had been open for days:

```
rx_filter rc=0 st=0
awdl stat: try=50 tx=49 rocfail=1 rocframes=90 bcn=9 af=0
```

`rocframes` counts every frame parsed while a ROC window is open, `bcn` the beacons among them. Ninety frames in fifty
windows of 60 ms prove the radio really is listening off-channel, the RX ring is delivering, and the parser runs. `af = 0`
then means exactly one thing: **no AWDL action frame was on the air on channel 6** while we were there.

That is not a receive bug. The interface was transmitting (and therefore listening) on channel 6 unconditionally -
`ItlNativeAwdlSync req = { 1, awdlPsfFrame, awdlPsfLen, 6, kAwdlPsfRocMs }` - while `setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE`
(slot 542) was answered by `awdlAccept()` and its contents thrown away. Apple's AWDL spreads the sync frame over a channel
sequence (2.4 GHz social channel 6, 5 GHz 44/149), and the Family's `AppleBCMWLANProximityInterface::buildAWDLChanSequence()`
builds that sequence from the struct we were ignoring. A peer whose sync tree sits on 44 or 149 is simply inaudible to a
radio that only ever parks on 6, in both directions.

The setter is now parsed (layout taken from `buildAWDLChanSequence`, 25G241, plus a raw hex trace so a wrong guess is
visible instead of silently parking the radio somewhere odd): master channel first, then the sequence entries at +0x10
(12 bytes each, channel in byte 0), then the default social set {6, 44, 149} as a union. The PSF cadence walks that list
round-robin, one channel per 440 ms period, so every channel is visited about once a second - which is what discovery
needs until the sync tree we hear tells us where the master actually is. `awdl stat` and the AF log now carry the channel,
so the next run attributes every frame to the channel it was heard on:

```
awdl:psf tick 100 len=153 ch=44 rc=0
awdl stat: try=50 tx=49 rocfail=1 rocframes=90 bcn=9 af=0 ch=149
```

Still open: `getAWDL_DEVICE_CAPABILITIES` (slot 523) answers unsupported, so the Family may keep building a 2.4 GHz-only
sequence - the union above is what makes discovery possible in the meantime, and reporting dual-band capabilities properly
is the clean fix once the struct layout is confirmed.

### 10.15 The channel sequence read-back stopped configure before the sync template

The hop change worked as far as it could - `awdl:set CHANSEQ n=8 master=255 hop=6,44,149,0,0` - but nothing followed it:
no `SYNC_FRAME_TEMPLATE`, no `SYNC_ENABLED`, no PSF tick. The trace ring held the whole boot (46 lines), and it ended on

```
awdl:proto getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE
```

i.e. the Family sets the sequence and then *reads it back*, and `awdlProto()` answered `kIOReturnUnsupported`. That answer
aborted the configure sequence before the step that hands the driver the sync frame, so the interface had nothing to
transmit and nothing to listen for - the hop list was never used.

The read-back is not a query the driver may fail: the payloads whose layout is not public are now stored verbatim when the
Family sets them and echoed back when it reads them (`set/get AWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE`, `MASTER_CHANNEL`,
`SYNC_STATE`), with a valid default sequence (version 1, one entry, 2.4 GHz ch6) when nothing has been set yet. Echoing is
also the only truthful answer: guessing a layout would hand the caller numbers the radio never used.

The entry layout is confirmed by the raw bytes of that first set - `01 00 00 00 | 00 10 00 03 | 00 ff 00 00 | 01 00 00 00`:
version 1, count 0x10, master 0xff (none), and entry 0 starting at +0x0c with `version = 1`, i.e. `apple80211_channel`
entries at +0x0c (channel in the low byte of +4, band flags at +8), exactly what `buildAWDLChanSequence` reads. The parse
now uses that offset and traces the first three entries with their flags.

### 10.16 What the firmware hands the host inside a ROC window

With the hop in place the trace shows the whole transmit half working on every channel:

```
awdl:psf tick 1 len=150 ch=6 rc=0
awdl:psf tick 2 len=150 ch=44 rc=0
awdl:psf tick 3 len=150 ch=149 rc=0
awdl sync: tx ch=6 len=150 ok=1 / ch=44 ok=1 / ch=149 ok=1
awdl stat: try=100 tx=97 rocfail=3 rocframes=86 bcn=7 af=0 ch=6
```

Still `af = 0`: over 100 periods, ~5 s of listening spread over the three social channels, not one action frame - not
even a non-AWDL one, which a busy 2.4 GHz band normally supplies (802.11v BSS transition frames are everywhere). Two
explanations remain, and they need different fixes:

* the firmware only forwards beacons and data to the host in station mode, so management frames from other devices never
  reach the RX ring at all - the AWDL code cannot fix that, the chip's RX path has to be opened up;
* the ROC grant is real but the radio never actually leaves the AP's channel, so every "off-channel" window is spent
  listening to the infrastructure channel.

`awdl stat` now separates them without another guess: `onch` counts the frames whose RXD3 channel field (p[13]) really is
the parked channel, and `fc` is a class histogram of everything heard inside a window (beacons / probe / action / other
management / data):

```
awdl stat: try=100 tx=97 rocfail=3 rocframes=86 onch=.. bcn=.. af=0 ch=.. fc=../../../../..
```

`onch ≈ rocframes` and `fc` beacons+data only → the radio is switching and the firmware is filtering management frames:
the next step is the firmware sniffer path (`MCU_UNI_CMD(SNIFFER)` / `UNI_SNIFFER_ENABLE`), the only documented way to make
an mt7925 hand every frame to the host.
`onch = 0` → ROC is a no-op on this firmware and the AWDL channel has to be changed the hard way.

### 10.17 Following the reference implementation: the sniffer, not the station RX filter

Everything so far assumed the station path could be made to deliver another device's action frame. It cannot: `10.13` and
`10.16` show beacons and data arriving inside the ROC window and never a single management frame from a foreign BSS, on
three different channels. The one public MT7925 AWDL implementation (§4) does not work that way either - it runs the radio
as a monitor/sniffer, which is exactly why it needs the AP link on a social channel (6/44/149) in its "shared" mode.

The receive half therefore now opens the firmware sniffer for the duration of the ROC window, and only for it:

```
ROC on the social channel (grant, status 0)
  MCU_UNI_CMD(SNIFFER) / UNI_SNIFFER_ENABLE  = 1
  MCU_UNI_CMD(SNIFFER) / UNI_SNIFFER_CONFIG  (band, 20 MHz, control = centre = that channel, drop_err)
  ...frames arrive, the RX ring is drained, awdlPollRx hands them to the peer manager...
  MCU_UNI_CMD(SNIFFER) / UNI_SNIFFER_ENABLE  = 0
ROC back to the AP channel
```

This keeps the infrastructure link: the radio is already parked off-channel for the window, so nothing is taken away from
it that the ROC had not already taken, and the station is back on the AP channel 60 ms later. `awdl stat` (`onch`, `fc`,
`af`) stays as the verdict: if `fc` now shows action frames and `af` moves, the receive path is open and the remaining work
is the Family's own advertise path; if the histogram does not change, the firmware ignores the sniffer while the interface
is a station and the next attempt is a real monitor vif.

Second documented blocker that this does not solve: an iPhone only starts AWDL after a BLE advertisement from the sender
(§2), and this machine's BT scan-filter offload `0x1812` fails. A test against an iPhone therefore measures the BLE wake-up
path too; two Macs (or an iPad whose AirDrop sheet is open) are the cleaner RX test.

### 10.18 What the Family actually needs from the driver (and what it cannot send itself)

A full symbol scan of IO80211Family settles a question that was still open in `10.12`: the Family never transmits an AWDL
action frame itself. There is no `outputActionFrame` and no `sendActionFrame` on `IO80211Controller` or on the virtual
interface - the only transmit-shaped symbol on the AWDL path is `IO80211VirtualInterface::sendToBpfTap`, and the AF thread
(`IO80211AWDLPeerManager::actionFrameProcessThread`) does nothing but tap the frame to BPF, account for it and release its
buffer. On Broadcom hardware the service-request / master-indication frames are built *by the firmware* from the parameters
the Family hands over: `setAWDL_AWDL_ADVERTISERS` (`awdl_peer_advert_add`), `setAWDL_PRESENCE_MODE`,
`setAWDL_ELECTION_PARAMS`, `setAWDL_AF_TX_MODE`, `setAWDL_SYNC_PARAMS`, `setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE`.
Those are the frames we still have to build ourselves, and their layouts are exactly what the driver was ignoring with
`awdlAccept()`.

Confirmed while reading the same vtable: `IO80211VirtualInterface::setAwdlCurrentChannelSequenceIndex` (slot 473) is an
*empty* function in the base class - a hook the driver is expected to fill. It is the Family telling the driver which entry
of the synchronisation channel sequence the cluster is on. The interface now follows that index instead of rotating on its
own, and only free-runs the sequence when the Family never drives one (discovery).

`setAWDL_AWDL_ADVERTISERS`, `setAWDL_PRESENCE_MODE` and `setAWDL_ELECTION_PARAMS` are now traced (first bytes / first four
words, bounded) instead of silently accepted, so the next AirDrop-open boot yields the layout of the advertise payload that
the service-request frame has to be built from.

### 10.19 The sniffer works - the frame is not at offset 32

With the sniffer on, the ROC window delivers 1686 frames per 100 periods instead of 86: the receive path is open. But the
`af` hits it produced were false positives, and the class histogram filled with "other management" (1681 of 1686) - the RX
buffer layout changed under the parser.

`mt7925_mac_fill_rx()` says why: after the 32 bytes of RXD the firmware appends optional groups depending on RXD1/RXD2, and
the frame starts after them (`mt76_connac2_mac.h`):

```
off = 32
    + 16 if RXD1.GROUP_4 (BIT 14)   fc/seq of the frame
    + 16 if RXD1.GROUP_1 (BIT 11)   IV
    + 16 if RXD1.GROUP_2 (BIT 12)   timestamp
    + 16 if RXD1.GROUP_3 (BIT 13)   P-RXV  (+24 more if GROUP_5, C-RXV)
    + RXD2.HDR_OFFSET (bits 15:14)
```

Station-mode frames carry no group, which is why the fixed 32 was right for the data and beacon paths since the first RX
work - but the sniffer sets GROUP_4, so offset 32 lands on `d0 00 00 00 ...`, the group, not the action frame. The AWDL
paths (action-frame probe, beacon probe, class histogram) now compute the offset with that formula; the group bits are
dumped once per boot (`awdl rx hdr off=.. rxd=..`) so the layout can be verified against real traffic instead of assumed.

### 10.20 Regression: the offset formula broke the scan path (and the HDR_OFFSET unit)

The first boot with §10.19 came up with no Wi-Fi at all: `scan:delivered 0 of 0`, no beacons, no association. The cause was
not the sniffer - it was applying `rxHdrOff()` to the *scan* probe. That probe feeds `rxBeacons`/`m3Capture`/`apSeen`, i.e.
the live link's scan and associate path, and it had been correct at a fixed offset of 32 since the first RX work.

Two lessons, both now encoded:

1. **`HDR_OFFSET` is in 2-byte units.** `mt7925_mac_fill_rx()` computes
   `hdr_gap = (u8 *)rxd - skb->data + 2 * remove_pad;` with `remove_pad = FIELD_GET(MT_RXD2_NORMAL_HDR_OFFSET, rxd2)`.
   Adding the raw field (what §10.19 did) leaves the offset 2-6 bytes short, which is enough to miss every beacon. Group
   sizes confirmed against the same function: GROUP_4/GROUP_1/GROUP_2/GROUP_3 are 16 bytes each and GROUP_5 is 24.
2. **Only the AWDL probes may use the computed offset.** The scan/associate path keeps the plain station-mode offset of 32,
   and the data/control/deauth paths keep it too. A frame missed on the scan path costs the link; a frame missed on the AWDL
   probe costs a diagnostic counter.

The descriptor dump (`awdl rx hdr off=.. rxd=..`) stays, and its read is bounds-checked.

### 10.21 RX probe: search by signature instead of by offset

The computed offset is still not trusted (the descriptor dump that would pin it down is evicted from the m3-log ring before it can be read), and every narrow window built on it either missed real frames or, worse, counted group words as action frames. The probe now scans 32..160 and requires the full AWDL signature (`0xd0 00` action header, protocol version 0, sane addresses, then `7f 00 17 f2 08` at +24) before anything is counted or queued. Group words and data frames cannot satisfy that, so `af` now means "real AWDL action frames heard" and the peer manager only ever receives genuine frames. The descriptor dump (`awdl rx hdr off=.. rxd=..`) stays for the first three frames of the boot.

### 10.22 The ROC window was 90% dead, and "no AWDL frame" was never proven

Two boot-long mysteries (`af=0`, "the firmware does not deliver AWDL frames") turned out to rest on a measurement bug, and
the measurement itself was never sound. Both are fixed here.

**1. The sniffer was on for ~8 ms out of every ~110 ms window.** `awdlSyncTx()` enabled the promiscuous mode *after* the
ROC grant and turned it off again a handful of milliseconds later, right before our own transmission:

```
m3Roc(true, ch);                    // park on the social channel
for (i < 80 && !rocGrant) m3Pump(1);// wait for the grant
m3Sniffer(true, ch);                // <- promiscuous mode starts HERE
for (i < 8) m3Pump(1);              // <- 8 ms of receiving
...TX...
m3Sniffer(false, ch);               // <- and it ends here
```

In station mode the chip only hands the host its own BSS; the promiscuous mode is the *only* reason a peer's PSF/MIF can
reach the host at all. A peer's frame arrives at an arbitrary point inside its availability window, so listening for 8 ms
out of a 440 ms period throws away ~98% of the window - and the "no AWDL frames on the air" conclusion was drawn from it.
`awdlDwellMs` (the dwell the native interface asks for, currently 100 ms) was stored and never used at all. The sniffer is
now enabled *before* the ROC and stays on for the whole dwell, which is also the only way the `awdl stat` numbers mean
anything.

**2. The RX filter was programmed once per link.** `m3RxFilter()` (the "pass frames from other BSS" band-config TLV) ran
only when the link came up; the firmware resets it when the channel changes, so by the time the radio parked on the next
social channel it was gone. It is now reprogrammed in every window, with all four bits the chip defines
(`0x80000064` = ENABLE | OTHER_BSS | CONTROL | FCSFAIL).

**3. The RX classifier could not tell an AWDL frame from a coincidence.** The histogram and the probe matched the first
byte of the buffer against `0xd0`, which a data payload satisfies easily; the result was a histogram full of phantom
"action frames" (`1164` other-management frames over 2574 frames is not a real air). Three unambiguous patterns are now
counted per buffer instead:

| counter | pattern | meaning |
|---|---|---|
| `oui`  | `00 17 f2`        | Apple's OUI is in the buffer somewhere |
| `sig`  | `7f 00 17 f2 08`  | the AWDL action header is in the buffer |
| `bss`  | `00 25 00 ff 94 73` | AWDL's BSSID is in the buffer |

`sig > 0` means the firmware does deliver AWDL frames and the probe's offset is wrong. `oui > 0, sig == 0` means the frames
are there but not where the probe looks. `oui == 0` means the firmware delivers no AWDL frame on this interface at all, and
the next step is a different interface type (the only public mt7925 AWDL implementation listens through a *monitor* vif, not
through an AWDL one - see the risk below).

The probe now scans the whole buffer (not just the first 160 bytes: the sniffer's RXD groups push the frame further out),
and a `fc == 0xd0` frame that fails the signature check is dumped as a *near miss* instead of being dropped silently, so a
frame that is nearly AWDL can be recognised.

**4. The diagnostics were evicted before they could be read.** The ROC fires five logged commands per period; the m3-log
ring holds ~258 lines, so the descriptor dump was gone within seconds. The ROC/sniffer command logging is now suppressed
while an AWDL window is open (failures are still reported), and the forensics dump is emitted for the first four frames of
a window and then every 250th frame.

The reference layout this all follows is the one the public AWDL implementation uses (`t4t5/owly`, `crates/owly-awdl/src/frame.rs`):
24-byte 802.11 header (`fc = 0x00d0`, broadcast DA, peer SA, BSSID `00:25:00:ff:94:73`) + 16-byte action header
(`7f` category, `00:17:f2` OUI, type `08`, version `0x10`, subtype `0` = PSF / `3` = MIF, two microsecond timestamps) + TLVs.

**5. The sniffer's channel configuration was sent before the radio moved.** `UNI_SNIFFER_CONFIG` carries the channel the
promiscuous mode listens on, and the reference implementation updates it when the channel context changes
(`mt7925_change_chanctx` -> `mt7925_mcu_config_sniffer`), i.e. *after* the radio is on the new channel. The enable and the
config are now separate commands (`m3SnifferEnable` / `m3SnifferConfig`): the mode is switched on before the ROC, and the
config is re-sent with the social channel once the ROC grant came in.

For the record, the channel rotation is not the problem it looks like. AWDL's availability window is 16 TU (16.4 ms) and the
peer sends a PSF in every window, on the channel of the slot it is in; with three social channels that is one AW per
82.5 ms, which a 100 ms dwell always covers. The phase between our 1320 ms rotation and the peer's 440 ms period is
therefore harmless: a window is always caught.

### 10.23 The AWDL receive path works - and panics the kernel in the last step

The four panics of 2026-10-22 (10:35, 11:33, 12:20, 12:44) have the same backtrace, and it is the best news in this whole
project:

```
0xffffff801693112a com.apple.iokit.IO80211Family : IO80211VirtualInterface::sendToBpfTap(IO80211Buffer*, uint, void*, ulong) + 0x2a
0xffffff801680e87d com.apple.iokit.IO80211Family : IO80211AWDLPeerManager::actionFrameInput(IO80211Buffer*, uint, void*, ulong) + 0x6cf
0xffffff8018bcc2de com.itlmt7925.native          : itlmt7925AwdlInterface::awdlDeliverRx(unsigned char const*, unsigned int) + 0xde
0xffffff8018bcc08e com.itlmt7925.native          : itlmt7925AwdlInterface::awdlPollRx() + 0x8e
0xffffff8018bcbe7b com.itlmt7925.native          : itlmt7925AwdlInterface::awdlPsfTick() + 0x2b
```

The driver reached `IO80211AWDLPeerManager::actionFrameInput` with a **real AWDL action frame**. The receive half works: the
firmware does deliver AWDL frames, the signature probe recognises them and they are queued and handed up. The `af=0` in the
statistics was read before the first frame arrived.

**The panic was our own fault.** `actionFrameInput` takes an `IO80211Buffer*`, and the driver passed a
`{const uint8_t *data; uint64_t len;}` pair pretending to be one. The Family calls virtual methods on it:

```c
// IO80211VirtualInterface::sendToBpfTap, IO80211Family 25G241
mov      rax, qword ptr [r13]        ; r13 = the "buffer" -> our data pointer
mov      rdi, r13
call     qword ptr [rax + 0x130]     ; IO80211Buffer::getLength() - through the frame bytes!
```

`0x130` is slot 38 of `IO80211Buffer`'s vtable (`getLength`). Calling through the *frame bytes* jumps into garbage: a general
protection fault, and with it the machine. The same call exists in `IO80211AWDLPeerManager::actionFrameProcessInput` and in
`isValidAF`, which is where the peer manager reads the frame through `getCursor()`.

**What the peer manager actually wants** (all of this read out of IO80211Family 25G241):

| argument | what it is |
|---|---|
| 1. `IO80211Buffer*` | the **AWDL action body**: category `0x7f`, Apple's OUI `00:17:f2`, type `8`, version, subtype, the two timestamps and the TLVs - i.e. the frame *without* the 24-byte 802.11 header. `getCursor()` starts at the buffer's first byte and `actionFrameHasCorrectOUI` compares `00:17:f2 / 08 / 00` at offsets 1..5, i.e. the category is at offset 0 |
| 2. `uint len` | the length of that body (**wrong**: it is the frame type `0xc`, see §10.26) |
| 3. `void* param` | a pointer to `apple80211_act_frm_hdr`, the 802.11 header of the frame. `findPeer()` looks the peer up with it and `addAWDLPeer()` copies its first six bytes into `IO80211AWDLPeer::withAddressAndManager()` - so **the sender's address is at offset 0**. Offset `0x10` is the channel (`updateAFStats` compares it with 14 to tell 2.4 GHz from 5 GHz), offset `0x2c` a timestamp (**wrong**: `+0x2c` is the RSSI and the struct is 0x48 bytes, see §10.26) |
| 4. `uint64 category` | the expected category byte, `0x7f` (`isValidAF` rejects anything `<= 0x47` and hands the low byte to `actionFrameHasCorrectOUI`) (**wrong**: it is the header length and must be `0x48`, see §10.26) |

**The fix.** The buffer is now a real `IO80211Buffer`, taken from the Family's own pool machinery
(`IO80211Buffer::allocBufferSingle(type, size, &pool, name)`, exported by IO80211Family - the argument order was wrong in
this round and is what panicked next, see §10.24), with the body written into it
(`writeBytes`/`setLength`). The buffers are kept in a ring of eight and reused round-robin instead of being freed right after
the call: the Family copies the payload out, but its peer bookkeeping may still look at the buffer, and freeing it would turn
that into a use-after-free.

The header the peer manager wants next to it is built as `apple80211_act_frm_hdr` with the sender's address at offset 0, the
channel at `0x10` and the sender's clock (the AWDL `phy_tx` timestamp) at `0x2c`; the fields the Family never reads are zero.

The core now also hands the channel the frame was heard on up to the interface (`ItlNativeAwdlRx::channel`), so the peer
manager gets the right channel instead of a guess.

### 10.24 The panic in 10.23: `allocBufferSingle` was called with its arguments swapped

The next round (Kernel-2026-10-22-131442) panicked on the first AWDL frame that reached `awdlRxBuffer`:

```
IO80211StaticBufferPool::flushPool+0x2b          CR2=0x8, RAX=0
IO80211StaticBufferPool::free+0x1c
IO80211StaticBufferPool::withOptions+0x6b
IO80211BufferPool::withOptions(IO80211BufferPoolOptions*, bool)+0x94
IO80211Buffer::allocBufferSingle(uint, ulong, IO80211BufferPool**, char const*)+0x7a
itlmt7925AwdlInterface::awdlRxBuffer+0x6b
```

This is not a race and not a Family bug: **we passed the two first arguments the wrong way round.** The 25G241
IO80211Family (extracted from `BootKernelExtensions.kc`, the same `xnu-12377.161.15.700.19~2` build the panic came from):

`IO80211Buffer::allocBufferSingle` (0xffffff800233b71e) builds `IO80211BufferPoolOptions` on the stack like this:

```
mov  qword ptr [rdi], 1              ; options+0x00 = 1
mov  qword ptr [rdi+8], rsi          ; options+0x08 = SECOND argument  <-- the buffer size
mov  qword ptr [rdi+0x18], rcx       ; options+0x18 = name
mov  dword ptr [rdi+0x28], r13d      ; options+0x28 = FIRST argument   <-- the allocator type
mov  word  ptr [rdi+0x38], 0
xor  esi, esi
call IO80211BufferPool::withOptions ; bool = false -> the static pool path
```

`IO80211StaticBufferPool::initWithOptions` (0xffffff80021889ee) validates `options+0x08` **before** it links anything:

```
mov  rax, qword ptr [r14+8]          ; the size
mov  qword ptr [rcx+0x38], rax       ; ivars+0x38
test rax, rax
je   0xffffff8002188d99              ; size == 0 -> return false
...
mov  qword ptr [rcx], rcx            ; (only now) list anchors get linked
```

The driver called `allocBufferSingle(kAwdlRxBufCap /*2048*/, 0, &pool, "itlmt-awdl-rx")`, so the Family got
**size = 0** (the second argument) and 2048 as the allocator type (the first). `initWithOptions` returned false with the
0x110-byte `kalloc_type` IVars still zeroed. `IO80211StaticBufferPool::withOptions` then takes its failure path
(`call qword ptr [rax+0x28]` = `free()`), `free()` calls `flushPool()` through vtable slot 0x128, and `flushPool` does:

```
mov  rcx, qword ptr [rbx+0x10]       ; ivars
mov  rdi, qword ptr [rcx+0x48]       ; 0, nothing to kfree
mov  rax, qword ptr [rcx]            ; list anchor = 0 (zeroed memory)
cmp  rcx, rax                        ; not the anchor -> treat it as a buffer
mov  rdx, qword ptr [rax+8]          ; <-- fault, CR2=0x8
```

The registers in the panic are exactly that state: `RBX` = the new pool, `RCX` = the zeroed IVars, `RAX`/`RDI`/`R14` = 0,
`R13` = 0x800 (our first argument, preserved in `allocBufferSingle`), `R12` = 0 (our second argument: the size).
`R14` in `flushPool` is zeroed just before the fault, which is why it is 0 and not the loop counter the pre-25G241 code had.

**The first fix (superseded by 10.25).** Call it as the Family does: `allocBufferSingle(0, kAwdlRxBufCap, &pool, name)`. Apple's own callers confirm the
order in the same build: `IO80211LQMCrashTracer::initLQMRingBuffer` passes `(0, size)`, `IO80211IOUCEventPipe::initWithClient`
passes `(0x10003, pageRoundedSize)` and `(0x10003, 0xc)`. The allocator type goes into the slab options at `+0x20` and from
there to the kalloc call, so 0 (what the LQM ring buffer uses) is the plain case.

That fixed the panic (the 22 Oct 17:45 boot is clean), but it is not enough for the static pool: `options+0x08` is the
controller's DMA segment (see 10.25), which this driver does not own. The receive path therefore takes its buffers from an
`IO80211DynamicBufferPool` instead.

`allocBufferSingle` also already frees and releases the pool on its own failure paths and only writes `*pool` on success, so
the `if (pool) pool->free()` in our error branch was removed: it could only ever have been a second free.

The lesson for the rest of the Family API: the mangled name fixes the *types* but says nothing about the *meaning* of the
arguments. `j`/`m` told us `uint` and `ulong`, and the natural reading - `(size, flags)` - was wrong. Every Family entry point
used from here on has to be read out of the KernelCollection, not guessed from its signature.

The entry a kernel collection hands a disassembler is not a standalone Mach-O: its load commands keep the file offsets of the
`.kc` file, so Ghidra (and lldb) reject or misread it. `tools/kc_extract.py` now rebases them (segments, sections, symtab,
dysymtab, dyld info, linkedit data, encryption info) while keeping the vmaddrs. The extracted 25G241 IO80211Family is then a
normal Mach-O and can be read with REA/Ghidra (`GHIDRA_INSTALL_DIR`, `rea decompile <macho> <mangled symbol>`), e.g.
`__ZN13IO80211Buffer17allocBufferSingleEjmPP17IO80211BufferPoolPKc`.
### 10.25 Which buffer pool the driver may use (Kernel-2026-10-22-131442)

The first attempt at 10.23 asked the Family for a buffer through `IO80211Buffer::allocBufferSingle(size, segment, &pool, name)`
and the machine panicked in the Family's own error path:

```
IO80211StaticBufferPool::flushPool + 0x2b          page fault, CR2 = 0x8
IO80211StaticBufferPool::free + 0x1c
IO80211StaticBufferPool::withOptions + 0x6b
IO80211BufferPool::withOptions + 0x94
IO80211Buffer::allocBufferSingle + 0x7a
itlmt7925AwdlInterface::awdlRxBuffer + 0x6b
```

`allocBufferSingle` is not a general-purpose entry point. It fills `IO80211BufferPoolOptions` with `count = 1`, the name, a
size at `+0x28` and **its second argument at `+0x08`** - and for the static pool that field is the **DMA segment** Apple's
driver owns (`initWithOptions` does `rivars[0x38] = opts[8]; test rax, rax; je <fail>`). With a zero there the pool fails, and
`IO80211StaticBufferPool::withOptions` then calls the object's release slot on a half-built object, whose `flushPool` walks a
null list. That is a Family bug, but it is reachable only by calling that entry point without a segment.

The pools, and what each needs:

| pool | needs | usable here |
|---|---|---|
| `IO80211StaticBufferPool` | `opts[8]` = Apple's mapped DMA segment | no |
| `IO80211MallocBufferPool` | nothing, but its buffers only override `getLength`/`setLength`/`readBytes` - the base `writeBytes` would read the slab at `+0x10`, which a `IO80211MallocBuffer` does not have (its state is at `+0x18`) | no |
| `IO80211DynamicBufferPool` | `opts[0]` = count, `opts[8]` = bytes per buffer, `opts[0x18]` = name | **yes** |

The dynamic pool allocates its own memory: `getBuffer()` -> `allocateSlab()` -> `IO80211BufferSlab::withOptions()` ->
`IOBufferMemoryDescriptor::inTaskWithGuardPages()`, then `allocateBufferFromSlab()` -> `IO80211Buffer::allocBuffer()`, which
builds a plain `IO80211Buffer` (state at `+0x10`, `getLength` at `slab[0x20]`). Those buffers support the base
`writeBytes(offset, data, len)` and `setLength(len)`, which is exactly what the receive path needs.

So the receive path creates one `IO80211DynamicBufferPool` (8 buffers x 2048 bytes) on the first AWDL frame and takes the
ring's buffers from it; the pool stays alive for the lifetime of the interface, so nothing the peer manager still looks at is
ever freed.

`IO80211BufferPool::withOptions(opts, bool)` is not used: apart from the `bool` (which selects the malloc pool) it decides
between static and dynamic on a boot-arg (`dynamic_buffer`), not on the caller's wish, and it panics when the chosen pool
fails to build.

### 10.26 Round 2026-11-03: the receive hand-off had four ABI errors, read out of 25G241 (no reboot yet)

State at the start of this round: the installed kexts are the f7058a9 build (dynamic pool, §10.25). The roadmap has no
hardware result for it, and on the 2026-11-03 21:24 boot `awdl0` did not exist (it is only created by
`tools/awdl/awdl-vif-test.applescript` / `~/vif_create_test en5`, §9.3), so the receive path never ran. The two panics of
that morning (08:39, 09:03) are in `com.nullmoth.NMXGPU` (`amdgpu_cs_ioctl`), not in this driver.

Before spending a boot on it, the whole hand-off was read again in the 25G241 KernelCollection (`tools/kc.py`, the
vtables with `tools/kc_vtable.py`; vtable offsets below are from the vptr). It had four errors, each of which alone would
have kept peers from ever appearing, and one of which is a use-after-free.

**1. The Family consumes the buffer - the round-robin ring was a use-after-free.** `IO80211AWDLPeerManager::actionFrameInput`
ends every path with `call [buf->vptr + 0x118]`, which is `IO80211Buffer::returnBuffer()` (slot 37). `returnBuffer` calls
the owning pool's `returnBuffer(buf)` (`[pool->vptr + 0x120]`), and `IO80211DynamicBufferPool::returnBuffer` sets the slab's
free bit and **releases the buffer object** (`call [buf->vptr + 0x28]`), freeing the slab too when it is empty and above the
pre-allocated count. When the AF queue is enabled (`ivars+0x41f0` bit 0) the buffer pointer is stored in the queue
(entry `+0x00`, 0x68-byte entries) and returned later by the AF worker. The ring of eight therefore kept pointers to freed
objects and wrote the ninth frame into one of them. Apple's driver does the same as the fix: one fresh buffer per frame,
never touched again after the hand-off (`AppleBCMWLANProximityInterface::handleActionFrame_rx` takes a buffer from the pool,
and returns it itself only on its own error path).

**2. The fourth argument is the length of the receive header, and it must be 0x48.** `isValidAF` rejects anything
`<= 0x47`, and the queued path of `actionFrameInput` does `cmp r15, 0x49; jae ud2` before
`memmove(entry + 0x10, hdr, r15)`: the only value both accept is `0x48`. The driver passed the category `0x7f` there (the
§10.23 reading of the argument was wrong; `actionFrameHasCorrectOUI` only prints it). With the AF queue enabled that is a
`ud2` - a kernel panic - on the first frame; in the direct path it went through only because nothing else checks it.

**3. The second argument is a frame type (0xc), not the body length.** Apple's driver does not call the peer manager at all:
it calls `IO80211VirtualInterface::bpfTapInput(buf, 0xc, &hdr, 0x48)`, and `bpfTapInput` forwards to
`peerManager->actionFrameInput(buf, 0xc, hdr, 0x48)` (vtable `+0x238`) only when the type is `0xc`, otherwise it goes to
BPF and returns the buffer. The type is stored in the AF queue entry (`+0x08`) and handed to `sendToBpfTap`. The driver now
calls the exported `bpfTapInput` exactly like that, which also removes the vtable-slot-73 call and its pointer-distance
check.

**4. `apple80211_act_frm_hdr` is 0x48 bytes and `+0x2c` is the RSSI, not a timestamp.** `handleActionFrame_rx` bzeroes
0x48 bytes on its stack and fills:

| offset | field | source in Apple's driver | read by the Family |
|---|---|---|---|
| `+0x00` | sender address (6) | firmware event `addr` | `findPeer`, `addAWDLPeer` -> `IO80211AWDLPeer::withAddressAndManager` |
| `+0x0c` | `apple80211_channel` {version, channel, flags} | `ChanSpecConvToApple80211Channel` | `updateAFStats` (`+0x10` vs 14), `determinePeerReachability` (`+0x10` vs 15) |
| `+0x28` | rate | `rx_frame_data` rate (byte-swapped) | (wifip2pd) |
| `+0x2c` | **RSSI, dBm** | `rx_frame_data` rssi (byte-swapped) | `actionFrameProcessInput`: a new peer only if `rssi >= ivars+0x281c` (-70, `initParamsToDefaults`, `setAwdlElectionRssiThresholds`); `determinePeerReachability`: `rssi == 0` -> 3 = unreachable, `addAWDLPeer` then creates no peer; `enqueuePeerInOrder(peer, rssi)` |
| `+0x30` | receive time | `rx_frame_data` mactime | (wifip2pd) |
| `+0x40`, `+0x44` | v2 event fields | `rx_frame_data` v2 | - |

`postAFToP2pd` copies exactly 0x48 header bytes plus the body to wifip2pd. The driver had a 0x38-byte struct with the
sender's clock at `+0x2c`: as a signed RSSI that is a random number, so a peer was created only when the clock happened to
look like a value above -70 and the queued path read 16 bytes past the struct.

**The fix (native `itlmt7925_awdl.cpp`, `io80211_buffer_min.h`; core `itlmt7925_m2/m3/eth.cpp`, `itl_native_bridge.h`):**

* `AwdlActFrmHdr` is the 0x48-byte layout above (`static_assert`), with `chVersion = 1`, `chFlags` = band (0x8 / 0x10) |
  0x2, the RSSI and the receive time.
* One buffer per frame from a dynamic pool (16 x 2048) that is shared by every AWDL interface and lives as long as the kext,
  so a buffer the Family returns late always has its pool; handed over with `bpfTapInput(buf, 0xc, &hdr, 0x48)`.
* The core decodes the RSSI of every queued AWDL frame with the beacon decoder (`rxRssi()`, RCPI of the RX vector) and stamps
  the host uptime in microseconds; `ItlNativeAwdlRx` is version 2 with `rssi` / `rxTimeUs` (the core still answers v1, the
  native side falls back to v1 against an older core). The `awdl af rx` line now prints `rssi=`, RXD1 and the header offset,
  so the next boot shows whether the vector is found in the sniffer layout. A frame whose vector cannot be decoded gets -70
  (the peer manager's own default edge) and is counted as `norssi` - interim, until that line pins the layout down.
* The TLVs of the body (1-byte type, 2-byte LE length) are walked; a tail of at most 4 bytes after the last complete TLV
  (an FCS or padding from the RX buffer) is cut, a longer mismatch is passed on unchanged and counted (`badtlv`).
* `awdl:rx af` traces the first 8 frames and every 200th, with the counters
  `norssi / weak (< -70) / nobuf / trim / badtlv`. The `ok=` value is gone: `actionFrameInput` always returns 0.

Built at the round's commit, link-checked against this KC (`tools/check_kext_links.py`, `bpfTapInput` is exported), and the
call checked in the binary (`edx = 0xc`, `r8d = 0x48`, `rcx = &hdr`). **Staged on the ESP 2026-11-03 21:41**; the previous
kexts are `EFI/OC/Kexts-backup/itlmt7925.kext.20261103-214158` and `itlmt7925Native.kext.20261103-214158` (Info.plist
identical, no credentials to carry). Nothing in this batch runs before `awdl0` exists, so station Wi-Fi is the same code as
before.

#### Test for the next boot (one boot, the order matters - §10.9)

1. Boot normally. Check Wi-Fi first: `ping -c3 1.1.1.1`; `kmutil showloaded | grep itlmt7925` must show new UUIDs
   (not `76DA2B9F...` / `550DFC0C...`).
2. Create the interface: `osascript tools/awdl/awdl-vif-test.applescript` (asks for the administrator password). Expect
   `rc=0 bsd_name='awdl0'`.
3. Finder > AirDrop, "Allow me to be discovered by: Everyone for 10 Minutes"; close the AirDrop window and open it again.
4. Put an iPhone (or a Mac) next to it with its AirDrop share sheet open (screen on, Wi-Fi + Bluetooth on). Leave both for
   two minutes; more than 8 frames have to be received, because the ninth is where the old ring wrote into a freed buffer.
5. Collect, in one go:
   ```
   bash tools/awdl/collect.sh > ~/awdl-10.26.txt 2>&1
   log show --last 15m --predicate 'sender == "IO80211Family" OR process == "wifip2pd"' --style compact 2>/dev/null | grep -i -E 'peer|awdl' | tail -200 >> ~/awdl-10.26.txt
   ```
6. Read:
   * native trace: `awdl:rx buffer pool ready (16 x 2048)` once, then `awdl:rx af n=...` with `n` past 8 and no panic.
     `rssi=` should be negative and `norssi=0`; `nobuf` / `badtlv` should stay 0; `trim` says whether the RX buffer carries
     a tail.
   * core ring: `awdl af rx ... rssi=<dBm> d1=<RXD1> off=<header offset>` - `rssi=0` means `rxRssi()` does not find the
     vector in the sniffer layout (then `d1`/`off` are what fixes it).
   * Family / wifip2pd log: a peer being added for the phone's AWDL address (the `sa=` of the `awdl:rx af` lines).
7. If it panics: keep `/Library/Logs/DiagnosticReports/Kernel-*.panic`. Undo (the ESP is mounted user-owned):
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad && mv Kexts-backup/itlmt7925.kext.20261103-214158 Kexts/itlmt7925.kext && mv Kexts-backup/itlmt7925Native.kext.20261103-214158 Kexts/itlmt7925Native.kext
   ```
   Not running step 2 is enough to keep the new receive code from running at all.

Next, depending on the result: with peers in the Family's table, phase 4 starts - check that the peer lists us in its sync
tree (our PSF accepted) and read `IO80211AWDLPeerManager`'s TX of the MIF / service response; without peers but with
`rssi` / TLVs clean, the next suspect is the body itself (`actionFrameProcessInput`'s TLV parse, `isValidAF`).

### 10.27 Hardware result of 10.26 (boot 2026-11-03 21:45): the peer manager has peers - next blocker is the AW timeline

Test as in §10.26: new kexts loaded (core `CD0BF2A1`, native `6A5546E5`), Wi-Fi up, `vif_create_test` -> `awdl0`, AirDrop
"Everyone" reopened. **No panic** with well over 8 frames received (the old ring would have written into a freed buffer at
the ninth).

What the traces show:

* `awdl:rx buffer pool ready`, then `awdl:rx af` on all three social channels from five AWDL addresses; the RX-vector RSSI
  decodes in the sniffer layout (`d1=80970fff`, header at 184): -55 .. -86 dBm, one frame of ~20 without a vector
  (`norssi=1`). About half the frames are below the -70 edge (`weak`), as expected for devices in other rooms.
* **The Family accepts them.** `com.apple.p2p.awdl0` logs `pppDone: Posted Peer Presence for DE:18:B3:E8:23:30` /
  `86:B4:13:80:4C:AF`, `actionFrameReport: no overlap slots with [ 2A:0C:B5:75:EC:C7 ]`, and election runs against the peers'
  trees (`isPeerSafeForElection: Peer DE:18:... seems to be stuck at AW0 18240, current AWSeqNum 18243`). §10.26 was the
  receive blocker; receive is done.
* `awdl:rx tail cut 2 bytes` - the RX buffer carries 2 bytes after the frame; cutting them is right.

What blocks next (all from the same log, in order of importance):

1. **No availability-window events.** `monitorAWEvents WARNING: Did not get AW start event for (70705 ms)` and
   `isPeerSafeForElection: ALERT: Self ... seems to be stuck at AW0 0, current AWSeqNum 0`. On Apple hardware the firmware
   runs the AW schedule and the driver forwards every AW start: `AppleBCMWLANProximityInterface::handleAvailabilityWindowStart`
   copies the firmware event into an `apple80211_availability_window_start` (0x24 bytes) and calls
   `IO80211Controller::postMessage(iface, 0x29, &aw, 0x24, true)`; the Family consumes it in
   `IO80211AWDLPeerManager::reportDataPathEvents` (-> `monitorTimeline`, `logAWStart`). Layout as Apple's driver fills it
   (firmware event offsets in brackets): `+0x00` u32 [fw+0x0a, u16], `+0x04` u32 role (fw+0x0d: 0 -> 2, 1 -> 3, 2 -> 1),
   `+0x08` u32 [fw+0x04], `+0x0c` u16 [fw+0x08], `+0x0e` u32 [fw+0x16], `+0x12` u16 [fw+0x1a], `+0x14` u32 [fw+0x00],
   `+0x18` `apple80211_channel` (from the chanspec at fw+0x0f). The meaning of each field (AW sequence number, TSF, slot,
   ...) is next to read out of `monitorTimeline` / `logAWStart` / `sanityCheckAWStart`. This is the phase-4 core: the driver
   has to run the AW timeline itself (sync to the master's TX down-counter from the sync-parameters TLV, AW = 16 TU, one
   channel-sequence slot per EAW) and post 0x29 (and the AW-end / extension events, `handleAvailabilityWindowEnd` /
   `...Extension`) on that schedule - which is also what the ROC windows must follow instead of the free-running 440 ms timer.
2. **MIF frames are cut at 512 bytes.** MIFs on the air are 644-1015 bytes (`awdl af rx len=1015 ... sub=03`); the core's
   queue slot (`AwdlRxSlot.data[512]`) and the native poll buffer are 512, so `TLVs end at 466 of 488`. Both need ~1600.
3. **Single band.** `configureValidAWDLChannels Dual band not supported ... 5G: 0` and the channel sequence the Family sends
   is channel 6 only (`CHANSEQ ... e0=6/008`), while the peers sit on 149/44 slots (`no overlap slots`). The AWDL device
   capabilities / RSDB answer has to report 5 GHz.
4. `buildMasterIndicationTemplate: Invalid _htCapabilityLength 0, skipping buildEnhDataRateTlv` - the MIF goes out without
   HT capabilities; the driver has to hand the Family its HT capability IE.

Items 2-4 are small and go into the same batch as 1; that batch is the next reboot.

### 10.28 Round 2026-11-03 (2): the driver keeps the AW timeline, MIFs fit, dual band, HT caps (no reboot yet)

All four blockers of §10.27 in one batch, each read out of 25G241 before it was written.

**1. The AW timeline and the AW start event.** On Apple hardware the firmware numbers the availability windows (AW, 16 TU),
puts four in an extended window (EAW, 64 TU) that takes one of the 16 channel-sequence slots, and at the start of every EAW
it is present in hands `AppleBCMWLANProximityInterface::handleAvailabilityWindowStart` an event, which calls
`IO80211Controller::postMessage(iface, 0x29, &aw, 0x24, true)` (`APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START`). The peer
manager consumes it in `reportDataPathEvents` case 0x29 and `monitorTimeline`. `apple80211_availability_window_start`:

| Offset | Field | Read by the Family as |
|---|---|---|
| +0x00 | AW number (u16 in a u32) | `AwNumber`; slot = `(AwNumber >> 2) & 15`; the next one is expected 4 AWs later per present slot |
| +0x04 | sync role, the Family's own numbering (1 master, 2 slave, 3 non-election master) | `ivars+0x4284` |
| +0x08 | master (6 bytes) | compared with the master it asked for in `SYNC_STATE` ("Waiting for Master=") |
| +0x0e | top master (6 bytes) | "Detected TopMaster change" |
| +0x14 | AW start time, microseconds, big endian | `FwAwStartTime` (byte-swapped) |
| +0x18 | `apple80211_channel` {1, channel, flags} | the EAW's channel |

The driver now keeps that clock (`itlmt7925_awdl.cpp`, `awdlTlTick`): a timer aimed at every EAW boundary posts 0x29 for each
slot whose channel (from `setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE`) is non-zero. While the Family says we are master
(`apple80211_awdl_sync_state` +4 = 1, read the way `setAWDL_SYNC_STATE` reads it: +4 state, +8 master address) the clock
free-runs from the moment AWDL is enabled. As a slave (+4 = 2) it is re-aligned on every PSF/MIF from that master (or, when
the master has not been heard for 1 s, from a node whose TLV names the same master) using the synchronisation parameters
TLV: value +1 = TX down counter in TU ("dc"), +29 = AW sequence number ("sn"), +21 = the sender's master, the offsets
`IO80211AWDLPeer::parseAwdlSyncParamsTlv` uses; the action header's PHY TX minus target TX time is subtracted. Top master is
`apple80211_awdl_election_params` +0x16 (what Apple hands its firmware's election tree), the sync master when zero.

The PSF now goes out on that timeline: every 7 EAWs (448 TU, the Family asks for 440), at an EAW start, on that slot's
channel (walking 6/44/149 while the sequence has a single channel). The core transmits right after the ROC grant instead of
at the end of the 100 ms dwell, and stamps the sync TLV's "dc" and "sn" at the real TX moment (`ItlNativeAwdlSync` v2 carries
the timeline; `awdlStampSync`). Open point: whether "sn" names the AW in progress (default) or the next one. Both directions
use the same convention, and `itlmt_awdl_snnext=1` in the boot args flips it without a rebuild.

Not done: the AW end / extension end event (0x2b, 0x28 bytes, from `handleAvailabilityWindowEnd`); the radio is still on the
AWDL channel only during the PSF window, not for every AW of the sequence (that is the data-path phase).

**2. MIF truncation.** The core's AWDL RX slot (`AwdlRxSlot.data`) and the interface's receive buffer were 512 bytes; MIFs on
the air were 644-1015. Both are 1600 now (the interface's is a member, not on the stack).

**3. Dual band.** `IO80211AWDLPeerManager::configureValidAWDLChannels` tests the controller's loaded card capabilities
(`ivars+0xb37 & 0x20`, i.e. `apple80211_capability_data` byte +7 bit 5, capability 29; Apple sets it from BCM feature flag
0x1c) and otherwise logs "Dual band not supported" and zeroes every 5 GHz AWDL channel - which is why the sequence was 16 x
ch 6. `getCARD_CAPABILITIES` now sets it: the MT7925 is a 2.4/5/6 GHz radio. This is the one bit there that the infra side
can see change; watch station Wi-Fi in step 1.

**4. HT capabilities.** The peer manager asks the infra interface for ioctl 0x9a (`getHT_CAPABILITY`, slot 481) through
`IO80211PeerManager::getHtCapabilityIE`; the old "unsupported" left `_htCapabilityLength` 0. Answered now with the MT7925's
HT Capabilities element (28 bytes with its header, the layout `AppleBCMWLANCore::getHT_CAPABILITY` returns): LDPC, 20/40,
SM PS off, SGI 20/40, STBC, 7935-byte A-MSDU, A-MPDU 64 KiB / 8 us, MCS 0-15. The generator's CUSTOM set has it
(`tools/gen_native_iface.py`).

Staged 2026-11-03 22:16: core `70ED9B79-F037-3E9C-A9D5-30E0C2415E4C`, native `FE13D2AF-E38A-3EA3-B8C4-DE64F13E6DA2`; the
previous pair (round 1, §10.26) is in `Kexts-backup/*.kext.20261103-221645`. Links checked against the KC (`check_kext_links`);
the `postMessage` call has the same registers as Apple's (controller, interface, 0x29, &ev, 0x24, 1).

#### Test for the next boot

1. Boot normally. `ping -c3 1.1.1.1`; `kmutil showloaded | grep itlmt7925` must show `70ED9B79...` / `FE13D2AF...`.
   Station Wi-Fi must behave as before (the dual-band bit and the HT answer are visible to the infra side).
2. `osascript tools/awdl/awdl-vif-test.applescript` (administrator password) -> `rc=0 bsd_name='awdl0'`.
3. Finder > AirDrop, "Everyone for 10 Minutes"; an iPhone/Mac with its share sheet open next to it, two minutes.
4. Collect:
   ```
   bash tools/awdl/collect.sh > ~/awdl-10.28.txt 2>&1
   /usr/bin/log show --last 15m --predicate 'subsystem == "com.apple.p2p.awdl0" OR sender == "IO80211Family" OR process == "wifip2pd"' --style compact 2>/dev/null | grep -i -E 'AW start|AwNumber|stuck|Master|Dual band|CHANSEQ|htCapability|sn=|election|peer' | tail -400 >> ~/awdl-10.28.txt
   ```
5. Read:
   * native trace: `awdl:tl start role=1`, `awdl:tl aw=... posted=N fail=0` with N growing; `awdl:set CHANSEQ` with 44/149 in
     the entries now (`e0..e2` not all `6/008`); after the Family elects a master, `awdl:tl role=2 master=...` and
     `awdl:tl sync master ... adj=` with small `adj` (a few ms) after the first one.
   * core: `awdl sync: tx ... stamped=N miss=0`; `awdl af rx` lengths above 512 for MIFs.
   * Family: no "Did not get AW start event", no "Self ... stuck at AW0"; `monitorTimeline` "AwNumber=a->b" stepping by 4
     (or 4 x the skipped slots); no "Dual band not supported"; no "Invalid _htCapabilityLength".
   * If, as a slave, "AW Number discontinuity" or the peers' `sn=` disagree with our `aw=` by one: set
     `itlmt_awdl_snnext=1` (boot args) for the following boot.
6. Undo if needed (ESP is user-owned):
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad && mv Kexts-backup/itlmt7925.kext.20261103-221645 Kexts/itlmt7925.kext && mv Kexts-backup/itlmt7925Native.kext.20261103-221645 Kexts/itlmt7925Native.kext
   ```

### 10.29 Hardware result of 10.28 (boot 2026-11-03 22:36) and round 3: TLV 4 built by the driver, the MIF goes out

**What 10.28 fixed (seen on hardware).** Both kexts loaded, station Wi-Fi unchanged, `awdl0` up through the applescript. The AW
start events are accepted: no "Did not get AW start event", no "stuck at AW0", and the PSF ticks now run on the AW timeline.
The dual-band bit took: `awdl:set CHANSEQ` has 149 in it. MIFs arrive whole (938 bytes; 512 used to cut them). The Family
holds one peer with 4 valid AirDrop services.

**What is still broken.** AirDrop finds nothing, in either direction (the owner's report: this Mac does not see the other
device and the other does not see this Mac). We found four causes, and fixed three of them this round:

1. **Our frames had no sync parameters TLV (type 4).** Both Family templates carry only the Family's own TLVs:
   * the PSF template starts with TLV 6;
   * the MIF payload starts with the election parameters TLV.

   Every PSF/MIF on the air starts with TLV 4. Apple's firmware inserts it, and so does every iPhone frame we capture (73
   value bytes). A peer takes our timeline, master and channel sequence from that TLV. Without it nobody could sync to us,
   and the core's TX stamping had nothing to stamp (`miss` counted up).
2. **The MIF never left the radio.** `IO80211AWDLPeerManager::updatePrimaryPayloadMI` hands the master indication frame to
   the driver. It uses `setAWDL_OOB_AUTO_REQUEST` (ioctl 0xe1), or `setAWDL_OOB_REQUEST` (0x95) for a one-shot. Both
   answered unsupported, and the auto one was even exempted from the trace. The MIF is the frame that carries our services,
   so no peer could see our AirDrop.
3. **HT capabilities were still refused.** `getHT_CAPABILITY` existed (10.28), but ioctl 0x9a is not in `commandAllowed`,
   so the gate answered ret 58 before the slot was ever reached.
4. **There is no data path yet.** The radio is on an AWDL channel only during the PSF windows (100 ms of every 448 ms), and
   awdl0 has no data path. AirDrop also needs mDNS/TCP over awdl0 (phase 5), so even with discovery working, a transfer
   cannot finish yet. Not addressed this round.

**Fixes (all in one boot).**

* `itlmt7925_wifi.cpp`: `case 0x9a` (HT_CAPABILITY) is allowed.
* `itlmt7925_awdl.cpp`, TLV 4: `awdlBuildSyncTlv()` builds the synchronisation parameters TLV (type 4, 73 bytes). The value
  layout is the one `parseAwdlSyncParamsTlv` reads:

  | Field | Value |
  |---|---|
  | tx channel | the window's channel |
  | master channel | from `setAWDL_MASTER_CHANNEL`, default 6 |
  | AW period | 16 |
  | AF period | 110 |
  | flags | 0x1800 |
  | ext / cmn | 16 / 16 |
  | ext counts | 3 |
  | master address | self while master, the sync master while slave |
  | presence mode | 4 |
  | channel sequence | 16 entries, encoding 3 (channel + 20 MHz operating class: 81, 115, 118, 121, 124), step 3, fill 0xffff, 2 bytes of padding |

  `awdlBuildFrame()` builds the 40-byte 802.11 + AWDL action header, then TLV 4 (unless the Family's TLVs already carry one),
  then the Family's TLVs. The frame is built at every transmission because TLV 4 names that window's channel. The core
  still stamps `dc`/`sn` and the timestamps at TX time.
* `itlmt7925_awdl.cpp`, OOB requests: `awdlOob()` implements both OOB slots. The layout was confirmed this round from
  `AppleBCMWLANProximityInterface::setAWDL_OOB_AUTO_REQUEST`:

  | Offset | Field |
  |---|---|
  | +4 | u32 |
  | +8 | u16 |
  | +0xc | u32 |
  | +0x10 | u32 |
  | +0x14 | u16 |
  | +0x16 | address |
  | +0x28 | length (u16) |
  | +0x30 | payload pointer |

  Apple's driver hands these to the firmware as the iovar `awdl_oob_af_auto`. Ours handles them like this:
  * the payload (≤ 1400 bytes) becomes a MIF, subtype 3;
  * the auto request is broadcast; a one-shot to a unicast address goes to that address and is sent once;
  * length 0 stops the MIF.
* `setAWDL_SYNC_PARAMS` (0x18 bytes) is stored, traced and published raw, so the Family's values can be checked against our
  TLV 4 defaults.
* Core, `ItlNativeAwdlSync` v3:
  * frames up to 1600 bytes (was 256);
  * a second frame (`frame2`/`len2`), sent and stamped right after the PSF in the same ROC window;
  * both frames are snapshotted into member buffers (too big for the stack);
  * the eth bridge accepts v3 and keeps the 256-byte limit for v1/v2;
  * the native side falls back from v3 to v2 to v1 against an older core.
* Diagnostics on the AWDL interface's ioreg node (`ioreg -r -c itlmt7925AwdlInterface`). The NVRAM ring is 2 KB and lost
  the `awdl:tl` lines within seconds. `tools/awdl/collect.sh` prints these properties:

  | Property | Contents |
  |---|---|
  | `itlmt-awdl` | counter line, refreshed every 64 EAWs: role, eaws, posted/fail/late, syncs/adj, psf ticks/len, mif submitted/len, oob auto/once/stop/too-long, tlv4 inserted/present, peer TLV 4s seen, rx/norssi/weak, slot 0, multi, sync ABI version |
  | `itlmt-awdl-self-tlv4` | our first TLV 4 (raw) |
  | `itlmt-awdl-peer-tlv4` | the first TLV 4 heard from a peer (raw) |
  | `itlmt-awdl-peer-hdr` | the bytes in front of that peer TLV 4 (raw) |
  | `itlmt-awdl-syncparams` | `setAWDL_SYNC_PARAMS` (raw) |
  | `itlmt-awdl-oob-auto` / `-oob-once` | OOB request header without the pointer (raw) |

  The core's `awdl sync: tx` line adds `len=psf/mif` and `mif=ok/try`; `awdl rxstat` adds `stamp=` and `mif=`.

Staged 2026-11-03 22:56: core `FEB66BA5-B20E-326D-B244-92301D817471`, native `563B3169-5523-3DAC-9975-DCA7E4713155`. The
previous pair (10.28) is in `Kexts-backup/*.kext.20261103-225614`. Links checked against the KC; both plists unchanged.

#### Test for the next boot

1. Boot normally. `ping -c3 1.1.1.1`; `kmutil showloaded | grep itlmt7925` must show `FEB66BA5...` / `563B3169...`.
2. `osascript tools/awdl/awdl-vif-test.applescript` (administrator password) -> `rc=0 bsd_name='awdl0'`.
3. Finder > AirDrop, "Everyone for 10 Minutes"; the iPhone with its share sheet open (AirDrop row) next to the Mac, two
   minutes. Note whether the Mac appears on the iPhone, and the iPhone in Finder.
4. Collect:
   ```
   bash tools/awdl/collect.sh > ~/awdl-10.29.txt 2>&1
   /usr/bin/log show --last 15m --predicate 'subsystem == "com.apple.p2p.awdl0" OR sender == "IO80211Family" OR process == "wifip2pd" OR process == "sharingd"' --style compact 2>/dev/null | grep -i -E 'AW start|AwNumber|stuck|Master|CHANSEQ|htCapability|OOB|MI |election|peer|service|airdrop' | tail -500 >> ~/awdl-10.29.txt
   ```
5. Read:
   * `itlmt-awdl`:
     * `oob=a<N>` with N > 0 and `mif=<n>/<len>`, both growing: the Family hands over the MIF and it is submitted;
     * `tlv4=<n>/0`: TLV 4 is inserted, not duplicated.
   * Core: `awdl sync: tx ... len=<psf>/<mif> ok=1 stamped=N miss=0 mif=K/K`.
   * Compare `itlmt-awdl-self-tlv4` with `itlmt-awdl-peer-tlv4`. Same length (0x49 value bytes)? Same AW/AF period, flags,
     presence mode, encoding? Check the iPhone's per-entry bytes against our channel/opclass pairs. If any of them differ,
     take the iPhone's values next round.
   * Family: no "Invalid _htCapabilityLength"; the iPhone should now list this Mac as a peer (on the iPhone side only the
     UI shows it).
   * `itlmt-awdl-syncparams`: the five values, for the TLV 4 comparison.
6. Undo if needed (ESP is user-owned):
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad3 && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad3 && mv Kexts-backup/itlmt7925.kext.20261103-225614 Kexts/itlmt7925.kext && mv Kexts-backup/itlmt7925Native.kext.20261103-225614 Kexts/itlmt7925Native.kext
   ```

Even if discovery works after this boot, a transfer will not complete yet: that needs the data path over awdl0 (phase 5:
mDNS/TCP, and the radio staying on the AWDL channel for the windows in the sequence).

### 10.30 Hardware result of 10.29 (boot 2026-11-03 23:14): AWDL frames go both ways; AirDrop now waits on election + data path

**Transmit works.** In every window both frames went out with the sync parameters TLV in place:
* `awdl sync: tx ch=149 len=249/249 ok=1`;
* after ~650 windows: `stamp=1294/0`, `mif=644/647`, `rocfail=3`;
* `itlmt-awdl` showed `tlv4=77/0` (TLV 4 inserted every time, never duplicated) and `oob=a6`: the Family handed over its MIF
  six times (133-byte payloads), and we sent it.

The `setAWDL_SYNC_PARAMS` values are `0 16 16 16 110 0`, the same as the TLV 4 defaults we chose.

**Receive needs the iPhone's share sheet open.** For the first 6 minutes the share sheet was closed: zero AWDL frames were
heard (`sig=0`). Once it was open, frames arrived: `af` 66 -> 98, `rx` 74 -> 108. The Family then:
* logged `actionFrameReport SUI from 7E:59:... has changed` and `Posted Peer Presence`;
* kept `peers:1` with 4 AirDrop services.

**The iPhone's TLV 4 next to ours** (`itlmt-awdl-peer-tlv4` / `-self-tlv4`):

| Field | iPhone | Ours |
|---|---|---|
| AW / AF period, flags, ext/cmn, ext counts, presence mode | 16 / 110, 0x1800, 16 / 16, 3, 4 | identical |
| dc | 0x20 (32 TU) | — |
| master address | its own (7e:59:...): it is master of its own tree | ours (we are master of ours) |
| channel sequence encoding | 1 (legacy: flags byte, then the *center* channel) | 3 (opclass) |

The iPhone's sequence entries:

| Slots | Bytes | Meaning |
|---|---|---|
| 0 | `1e 7e` | 40 MHz, center 126, flags & 3 = 2 (primary = center + 2) |
| 2, 10 | `1d 97` | 40 MHz, center 151, primary 149 |
| 8 | `2b 06` | 2.4 GHz ch 6, 20 MHz |
| every other slot | `00 00` | absent |

From these values, the legacy flags look like: bits 0-1 = primary position (1 lower, 2 upper, 3 = 20 MHz), 0x08/0x04 =
width, 0x10 = 5 GHz, 0x20 = 2.4 GHz. This is an inference; we have not checked it against a parser yet. Change made from
this comparison: an empty slot in our TLV is now `00 00` (it used to be the master channel). That change is built but not
staged; it goes with the next batch.

**Why neither side shows the other in the AirDrop UI.**
1. **No election.** On Apple hardware the firmware runs the master election: the Family only hands down
   `AWDL_ELECTION_PARAMS` ("Initialize FW Election Params") and waits for sync-state events. Our driver has no election, so
   both devices stay masters of their own trees: we stay `role=1, syncs=0`, and the iPhone names itself as master. Two
   unsynchronised trees never line up their AWs, so neither side can schedule anything with the other.
2. **No data path.** AirDrop does not show a device just because its MIF service records arrive. sharingd first runs the
   AirDrop discovery exchange over TCP/TLS on awdl0's IPv6 link-local addresses. awdl0 has no data path yet (phase 5),
   and the radio is on an AWDL channel only for 100 ms of every 448 ms.

**Next work (a phase of its own, larger than a batch):**
* **Election in the driver.** Compare the peers' election TLV (metric/counter, as the Family's `ELECTION_PARAMS` defines it)
  with our own. When a peer wins, call `setAWDL_SYNC_STATE`-equivalent events up to the Family (slave, master = peer, top
  master), so the existing `awdlTlSync` follows its clock. That needs the sync-state-changed message number and its layout
  from `handleSyncStateChanged` in the BCM driver.
* **On-channel scheduling.** Instead of one 100 ms ROC per 7 EAWs, stay on the AWDL channel for the AWs of the sequence
  while the AP link tolerates it (or drop the AP channel while AirDrop is active).
* **Data path.** 802.11 data frames with Apple's AWDL LLC/SNAP header in both directions on awdl0 (the Skywalk queues that
  `registerAwdlInterface` already creates).

### 10.31 Round 2026-11-03 (4): the election runs in the Family, triggered by the AW end event (no reboot yet)

**Finding (25G241).** The master election is not done in firmware. The Broadcom firmware only reports the outcome: its
events end up in `handleMasterEvent` / `handleSlaveEvent` / `handleNonElectionMasterEvent`, which post message 0x2e (16
bytes, +0 the state: 1 master, 2 slave, 3 non-election master). But the decision itself comes from the Family:
`IO80211AWDLPeerManager::runElection` → `programChipWithNewMaster` → ioctls down to the driver (`setAWDL_SYNC_STATE`,
`setAWDL_ELECTION_PARAMS`, sync frame period).

`runElection` has exactly one caller: `reportDataPathEvents`, case **0x2b** (jump-table index 2 at `0xffffff80021e0b80`).
0x2b is `APPLE80211_M_AWDL_AVAILABILITY_WINDOW_END`. It is gated by `shouldRunElection`, which needs:
* `ivars+0x2843` bit 0 (set by `configure` unless `+0x2841`);
* `ivars+0x3ee0` bit 1 (AWDL on).

The driver posted AW start (0x29) since §10.28 but never AW end, so the Family never ran an election. Both nodes stayed
masters, which explains `role=1 syncs=0` in §10.30.

**AW end layout** (`AppleBCMWLANProximityInterface::handleAvailabilityWindowEnd`, 0x28 bytes):

| Offset | Field |
|---|---|
| +0 | event byte +0xc |
| +4 | event word +0 |
| +8 | event byte +0x11 |
| +0xc | event word +0x12 (the Family keeps it at `ivars+0xa6b4`) |
| +0x10 | u16 version 1 + per-AW statistics, written only when the firmware event is ≥ 0x34 bytes |

The election path reads none of these fields. The statistics are read only when +0x10 == 1, so a zero tail means "none".

**The election tree read-back.** `getAWDL_ELECTION_PARAMS` answered unsupported. Apple's driver fills it from the firmware
`awdl_election_tree`:

| Offset | Field |
|---|---|
| +0x10 | flags |
| +0x12 | election id |
| +0x14 | tree depth |
| +0x16 | top master |
| +0x1c | top master metric |
| +0x20 | self metric |

**Fixes (native only; the core is unchanged from §10.29).**
* `awdlTlTick` posts AW end (0x2b) for the previous present EAW, just before the next AW start. It carries the channel
  and the end time of the 16-TU AW.
* `setAWDL_ELECTION_PARAMS` stores the first 0x24 bytes. `getAWDL_ELECTION_PARAMS` echoes them back; before the first set
  it answers self as top master with our metric.
* The `itlmt-awdl` line gains these fields:
  * `chg=` (role/master changes from `setAWDL_SYNC_STATE`);
  * `master=` / `top=`;
  * `end=posted/fail`.
* From §10.30: empty slots of our TLV 4 channel sequence are `00 00`.

Staged 2026-11-03 23:31: native `0600CBA6-BD06-375E-9D34-142F38A7BD8B`, core unchanged (`FEB66BA5...`). The previous native
(§10.29) is in `Kexts-backup/itlmt7925Native.kext.20261103-233153`.

#### Test for the next boot

1. Boot. `ping -c3 1.1.1.1`; `kmutil showloaded | grep itlmt7925` must show `FEB66BA5...` / `0600CBA6...`.
2. `osascript tools/awdl/awdl-vif-test.applescript` → awdl0. Open Finder > AirDrop (Everyone). **Open the iPhone's share
   sheet** (without it the iPhone sends no AWDL frames at all, §10.30).
3. After two minutes: `bash tools/awdl/collect.sh > ~/awdl-10.31.txt 2>&1`, then this, which adds the Family's election
   logs:
   ```
   /usr/bin/log show --last 10m --predicate 'sender == "IO80211Family"' --style compact 2>/dev/null | grep -iE "elect|master|syncState|SYNC_STATE|AW end|stuck" | tail -200 >> ~/awdl-10.31.txt
   ```
4. Read `itlmt-awdl`:
   * `end=N/0` with N growing (the AW end events are accepted);
   * then `role=2 chg>=1 master=7e59...` (the iPhone's address) and `syncs>0` with small `adj` (our clock follows it);
   * or `role=1` with a winning metric: then the iPhone's next TLV 4 should name *us* as master (`itlmt-awdl-peer-tlv4`
     +21).
5. Undo (native only):
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad4 && mv Kexts-backup/itlmt7925Native.kext.20261103-233153 Kexts/itlmt7925Native.kext
   ```

Even with the election working, the AirDrop UI still needs the data path (phase 5). This round only checks that the two
trees merge.

### 10.32 Hardware result of 10.31 (cold boot 2026-11-04 09:05) and round 5: listen where the peer is

**What 10.31 did on hardware.**
* **Bluetooth.** A warm reboot lost Bluetooth (MtkBtFw "section 2: block 49 failed", see `docs/BLUETOOTH.md`). A cold boot
  brought it back.
* **AWDL enable ordering.** After the cold boot AWDL never came up. sharingd had started its AirDrop browse before awdl0
  existed, mDNSResponder deduplicated the later one, and wifip2pd never got an `AWDLBrowse`, so `SYNC_ENABLED` never came.
  Test workaround: create awdl0 first, then run `dns-sd -includeAWDL -B _airdrop._tcp local.` in the background. That
  triggers `AWDLBrowse ... started` → "enabled AWDL". The real fix (awdl0 present before sharingd browses) is open.
* **AW end is accepted.** `end=975/0`. The Family runs its election about every 3 s
  (`runElection: ElectionParamsChanged`, `programChipWithNewMaster`).
* **The election did pick the iPhone, once.** At 09:12:50 the log has:
  ```
  programChipWithNewMaster: Programming new Mst: AE:E5:E4:83:39:FB ... HC 1 met 538 my met 522
  runElection: ... New P:AE:E5:E4:83:39:FB ... [RLFC:621 HC:1 RMet:538 SLFC:52 SMet:522]
  topMasterChanged: Top master changed to AE:E5:E4:83:39:FB
  ```
  So the merge logic works: the iPhone's metric 538 beats our 522.
* **Then the peer aged out.** Within 5 s:
  ```
  shouldRemovePeerFromNDP: No Action frames for(2397mSec) from AE:E5:E4:83:39:FB
  Posted Peer Absence
  ```
  The election then went back to self, and `SYNC_INFO: AWDLSequenceNumberNotUpdated for_9_AW_Start_events` was logged
  while we were nominally a slave.
* **Counters.** Over ~10 min the counters were `rx=63` (`weak=45` below −70 dBm). The `peer4=61` TLVs came in bursts
  separated by more than 2.4 s.

**Why the iPhone is heard so rarely.** Its channel sequence (`itlmt-awdl-peer-tlv4`, legacy encoding) is:

| Entry | Slots | Primary |
|---|---|---|
| `1e 26` | 0, 1, 7, 9, 11, 15 | 40 |
| `1d 97` | 2, 10 | 149 |
| `2b 06` | 8 | ch 6 |

So it sits on 2.4 GHz for 1 EAW in 16 (~65 ms a second) and on 5 GHz 40/149 the rest. Our only receive time was the
100 ms ROC per ~459 ms PSF window. That window was placed on *our* clock, on our slot channel or the 6/44/149 rotation, and
44 is not even one of its channels. It overlapped the iPhone's presence only by chance.

**dc counts to the next EAW, not the next AW.** The iPhone's TLV 4 down counter is 29 TU here (32 TU in §10.30), with
16-TU AWs. With sn=6 that is "AW 8 starts in 29 TU": the counter runs to the sender's next *extended* window, where AW % 4 == 0
(presence mode, +27). Our stamping (core `awdlStampSync`) and our slave alignment (`awdlTlSync`) both assumed the next AW,
so a peer following us was off by up to 3 AWs, and so was our alignment to it.

**Fixes (round 5, core + native).**
* **Core, `ItlNativeAwdlSync` v4.** A listen-only window (`frame == nullptr, len == 0`): ROC on the channel, sniffer on,
  dwell, back. Counted in the `awdl stat:` line as `listen=`.
* **Core, `awdlStampSync`.** dc runs to the next AW whose number is a multiple of the presence mode (TLV +27).
* **Native, peer timelines.** Every received TLV 4 updates a 4-entry table `AwdlPeerTl`:
  * the peer's next EAW start on our clock;
  * its AW number;
  * AWs per entry;
  * the primary channel of each of its 16 entries. Legacy encoding: center ∓ 2 by flags & 3.
* **Native, listen timer.** A second timer follows one peer:
  * the master while we are a slave, else the peer heard most recently (within 8 s);
  * it fires 2 ms before the peer's next present EAW;
  * it opens a 40 ms window on that EAW's channel;
  * it re-arms for the next present EAW.

  While a peer is followed, a due PSF/MIF is not sent on our own slot. It rides in the next listen window, on the peer's
  channel, where the peer can hear it.
* **Native, instant slave alignment.** `setAWDL_SYNC_STATE` → slave takes the new master's clock at once, from its cached
  timeline, instead of waiting for its next frame. Counted as `align=`.
* **Native, PSF period.** The PSF period follows `setAWDL_SYNC_PARAMS` +0x10 ("Setting PSF period to 110" → every 2 EAWs),
  instead of a fixed 7.
* **Native, `itlmt-awdl` line.** Gains these fields:
  * `lsn=windows/with-tx/fail`;
  * `ch=` (the last listen channel);
  * `ptl=` (peer timeline updates);
  * `align=`.

**Air-time cost.** While a peer is followed, the radio spends ~40 ms of each of the peer's present EAWs (9 of 16 for this
iPhone) off the AP's channel, about 35 % of the time. Infrastructure throughput drops during AirDrop. This is the same
time-sharing Apple's AWDL does.

Staged 2026-11-04 09:23:

| Kext | UUID | Backup |
|---|---|---|
| core | `9DA98E35-5229-3B43-A040-53C7206ACE48` | `Kexts-backup/itlmt7925.kext.20261104-092327` (= `FEB66BA5...`) |
| native | `F7B9FFEE-4099-3F82-AC70-FC5CAEC864DA` | `Kexts-backup/itlmt7925Native.kext.20261104-092327` (= `0600CBA6...`) |

#### Test for the next boot (cold boot, so Bluetooth comes up)

1. **Check the boot.** `ping -c3 1.1.1.1`. `kmutil showloaded | grep itlmt7925` must show `9DA98E35...` / `F7B9FFEE...`.
2. **Bring AWDL up, in this order.**
   1. `osascript tools/awdl/awdl-vif-test.applescript` → awdl0.
   2. `dns-sd -includeAWDL -B _airdrop._tcp local.` in a terminal; leave it running.
   3. Finder > AirDrop (Everyone). Open the iPhone's share sheet and keep it open.
3. **Collect.** After two minutes:
   ```
   ioreg -l -w0 | grep '"itlmt-awdl' > ~/awdl-10.32.txt
   /usr/bin/log show --last 5m --predicate 'sender == "IO80211Family" AND eventMessage CONTAINS "awdl0"' --style compact 2>/dev/null | grep -iE "programChip|topMaster|No Action frames|Peer Presence|Peer Absence|SequenceNumber" >> ~/awdl-10.32.txt
   ```
4. **Read `itlmt-awdl`.**
   * `ptl>0` and `lsn=N/M/0` with N growing: the listen windows run. `ch=` should be 40, 149 or 6.
   * `rx` should grow by several frames a second, not a few a minute.
   * The log should show `Programming new Mst: AE:E5...` (the iPhone, its random MAC may differ) and stay there, with no
     `No Action frames for` lines.
   * `role=2 align>=1 syncs>0` with small `adj`.
5. **If Wi-Fi (infra) drops** while AirDrop is open, the 40 ms dwell is too long for this AP. Note it; the dwell is
   `awdlLsnFire`'s 40.
6. **Undo (both kexts).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad5 && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad5 && mv Kexts-backup/itlmt7925.kext.20261104-092327 Kexts/itlmt7925.kext && mv Kexts-backup/itlmt7925Native.kext.20261104-092327 Kexts/itlmt7925Native.kext
   ```

Open after this round:
* awdl0 must exist before sharingd browses;
* the weak RSSI (45 of 63 frames below −70 dBm with the iPhone next to the Mac: check the RCPI decode);
* the data path (phase 5).

### 10.33 Hardware result of 10.32 (cold boot 2026-11-04 10:10): AWDL discovery works end to end; round 6 is the data path

**What 10.32 did on hardware** (core `9DA98E35`, native `F7B9FFEE`; iPhone share sheet open from ~10:17:50).
* **The listen windows follow the iPhone.**
  * `lsn=473/335/0 ch=149 ptl=163` means the windows run on its channels (40/149/6).
  * `rx` climbed from 25 to 164 in about a minute, where earlier boots got a few frames a minute.
  * From 10:17:53 the Family kept the iPhone (`AE:E5:E4:83:39:FB`) as a peer continuously. There was no
    `No Action frames for ...` line and no `Peer Absence`.
* **Election.**
  * At 10:14:17 the Family elected the iPhone (`HC 1 met 516`, while our metric was still 65).
  * 0.4 s later our metric was 537 and it re-elected us. Since then we are master (`role=1`), which is correct: 537 > 516.
  * Whether the iPhone follows us (its TLV 4 +21 naming our address) is not yet checked: `itlmt-awdl-peer-tlv4` is
    published once only.
* **Bonjour over AWDL works.** `dns-sd -includeAWDL -B _airdrop._tcp local.` on awdl0 (if 15) listed:
  ```
  10:18:04.058  Add  15 local. _airdrop._tcp. 366bfda0f544
  10:18:04.593  Add  15 local. _airdrop._tcp. 6bfb34049458
  ```
  These are the iPhone and a second Apple device (`2E:4D:39:A2:2D:4D`, also a Family peer). `ndp -an` has
  `fe80::ace5:e4ff:fe83:39fb%awdl0` / `fe80::2c4d:39ff:fea2:2d4d%awdl0` as permanent entries.
* **The next blocker is the data path.**
  * sharingd sends AirDrop `DISCOVER` requests to those services: `Sending DISCOVER request` →
    `DISCOVER request timeout` → `Bonjour service ... failed discovery`.
  * awdl0 shows `Opkts 0`, `Ipkts 0`. The awdl0 Skywalk queues were created and registered, but they were never put on the
    interface's work queue or enabled, and the TX submission action consumed nothing. No packet ever left the stack.
* **Side issue: radio time.** With no peer being followed, the PSF fallback now runs every 2 EAWs, with a 100 ms dwell each
  time. Infra still worked (ping fine), but this is too much time off the AP's channel.

**Fixes (round 6, phase 5: the data path).**

| Part | Change |
|---|---|
| Frame format | AWDL data frame = 802.11 data, no DS bits, addr1 = DA, addr2 = our awdl0 address, addr3 = `00:25:00:ff:94:73`; LLC/SNAP `aa aa 03 00 17 f2 08 00`; AWDL data header `03 04`, sequence (LE16), `00 00`, ethertype (BE); payload. Protocol facts per the OWL paper/implementation, read as a reference only. |
| Core RX (`awdlRxData`) | Inside an AWDL window, data frames matching the BSSID + SNAP go into the AWDL RX ring, which grows from 4 to 16 slots. `awdl rxstat:` gains `full=` and `data rx=`. |
| Core TX | `ITL_NATIVE_AWDL_DATA_TX`: a 16-slot ring. `awdlDataFlush()` sends it, fixed 6 Mb/s on ALTX0 like the PSF, every ms of every AWDL window (listen or PSF). `data ... tx=queued/sent/fail/full`. |
| Native registration | The three awdl0 queues are added to the interface's work queue (`awdl:dp queues on the work queue 3/3`). |
| Native enable | `enable()` override and `SYNC_ENABLED`: enable the completion queues, then TX submission, and start a 5 ms data path timer (`awdl:dp enabled`). |
| Native TX | Ethernet → AWDL data frame → core. The packet is completed on the next tick. |
| Native RX | AWDL data frame → Ethernet (IPv6 length trims an FCS tail) → RX completion queue, with broadcast/multicast flags. |
| `itlmt-awdl` | Gains `dp=0/1 dtx=in/queued/full/bad/done drx=in/up/notus/fail`. |

Not in this round: the dwell of the no-peer fallback (above), and the RSSI of 5 GHz frames.

Staged 2026-11-04 10:23:

| Kext | UUID | Backup |
|---|---|---|
| core | `AB264730-940A-3287-ADA9-7D4DDF9F3BBC` | `Kexts-backup/itlmt7925.kext.20261104-102307` (= `9DA98E35`, §10.32, works) |
| native | `AE068461-4938-3BA9-AE8D-DFFD989CE09C` | `Kexts-backup/itlmt7925Native.kext.20261104-102307` (= `F7B9FFEE`) |

#### Test for the next boot (cold boot)

1. **Check the boot.** `kmutil showloaded | grep itlmt7925` must show `AB264730...` / `AE068461...`. Ping the AP.
2. **Bring AWDL up, in this order.**
   1. `osascript tools/awdl/awdl-vif-test.applescript`.
   2. `dns-sd -includeAWDL -B _airdrop._tcp local.` (leave it running).
   3. Finder > AirDrop. Open the iPhone's share sheet and keep it open.
3. **Read the state.**
   ```
   ioreg -l -w0 | grep '"itlmt-awdl"'
   netstat -I awdl0
   ioreg -l -w0 | grep '"m3-log"' | sed 's/.*"m3-log" = "//' | tr ';' '\n' | grep -E "awdl (data|rxstat)" | tail -5
   ```
   Expected:
   * `dp=1`;
   * `dtx` growing, with `queued` equal to `in`;
   * `Opkts` > 0 on awdl0;
   * `data rx` > 0 in the core;
   * `drx up` > 0 and `Ipkts` > 0;
   * in `log show --predicate 'process == "sharingd"'`, the DISCOVER requests stop timing out and the iPhone shows up in
     Finder > AirDrop.
4. **If it panics,** or awdl0 breaks Wi-Fi, undo both kexts to §10.32:
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad6 && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad6 && mv Kexts-backup/itlmt7925.kext.20261104-102307 Kexts/itlmt7925.kext && mv Kexts-backup/itlmt7925Native.kext.20261104-102307 Kexts/itlmt7925Native.kext
   ```

### 10.34 Hardware result of 10.33 (cold boot 2026-11-04 11:53) and round 7: a state-1 master that is not us makes us a slave

**What 10.33 did on hardware** (core `AB264730`, native `AE068461`).
* **The data path transmits.**
  * `dp=1` (the queues are on the work queue and enabled).
  * awdl0 shows `Opkts 25` for the first time.
  * Native `dtx=25/25/0/0/25` (in/queued/full/bad/done); core `data ... tx=24/24/0/0` (queued/sent/fail/full).
  * Infra Wi-Fi kept working.
* **Nothing received yet.** `data rx=0`, `Ipkts 0`. No AirDrop `DISCOVER` happened in this boot: sharingd removed its AirDrop
  client at 12:25:52. So the TCP exchange was never attempted, and the receive half is untested.
* **Bonjour.** `_airdrop._tcp` over awdl0 again: `366bfda0f544` at 12:26:11.
* **The election now picks the iPhone** (new random MAC `82:F6:FC:2E:AB:90`, metric 515 vs our 510):
  ```
  programChipWithNewMaster: Programming new Mst: 82:F6:FC:2E:AB:90 ... HC 1 met 515 my met 510
  ```
  It holds for minutes. But the driver stayed `role=1 align=0`: `setAWDL_SYNC_STATE` carried **state 1** with the iPhone's
  address, and the driver only treated state 2 as "slave". Our clock never followed, and the Family flagged it:
  ```
  SYNC_INFO: (v10.0:iOS)(82:F6:FC:2E:AB:90) MI AF has sequence number:(21953), current window:(6248)
  ```
  It also still logs `No Action frames for(2446mSec)` now and then.

**Fix (round 7, native only).**
* `setAWDL_SYNC_STATE`: state 1 with a non-zero master address that is not this interface → our role is 2 (slave). Counted
  as the second number of `align=a/b`.
* The slave path then runs: immediate alignment from the master's cached timeline, `awdlTlSync` on every master frame, and
  the listen windows following the master.
* The AW start events still carry the state the Family itself set (`awdlFamilyState`), not the derived role.

Staged 2026-11-04 12:27: native `DBDD093B-7B5C-3FF6-A172-EC7641E7327E`. The core is unchanged (`AB264730`). The previous
native (§10.33) is in `Kexts-backup/itlmt7925Native.kext.20261104-122723`.

#### Test for the next boot (cold boot)

1. **Check the boot.** `kmutil showloaded | grep itlmt7925` must show `AB264730...` / `DBDD093B...`.
2. **Bring AWDL up.** Run awdl0 + `dns-sd -includeAWDL -B _airdrop._tcp local.` as in §10.33. Open Finder > AirDrop and
   keep that window open, then open the iPhone's share sheet.
3. **Read `itlmt-awdl`.**
   * `role=2 master=<iPhone>` and `align=N/M` with N ≥ 1, M ≥ 1;
   * `syncs` growing with small `adj`;
   * no more `MI AF has sequence number ... current window` mismatches in the Family log.
4. **Read the data path.** With the Finder window open, sharingd should log `Sending DISCOVER request`. Then:
   * `drx` / `Ipkts` > 0;
   * core `data rx` > 0;
   * no `DISCOVER request timeout`.
5. **Undo (native only).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad7 && mv Kexts-backup/itlmt7925Native.kext.20261104-122723 Kexts/itlmt7925Native.kext
   ```

### 10.35 Boot 2026-11-04 12:29: infrastructure Wi-Fi died before AWDL started; the core now resets a dead chip

This boot never reached the §10.34 test. The first join came up (`RESULT rc=0 link up`), then:
1. DHCP got no reply (`pulse ... rxd=2 eth=0`).
2. 4 s after link-up the firmware went silent (`postmortem silent 4s`, the known issue in `docs/ROADMAP.md`).
3. The Family disconnected before the 15 s beacon watch fired.
4. From then on every MCU command timed out: `bss_info(del) rc=-110`, `edca rc=-110`, and every scan came back empty
   (`no configured network seen in the scan`).

Only the beacon-watch path requested a chip reset, so nothing reset it, and Wi-Fi stayed down. The owner fell back to USB
networking. AWDL had not been started (no awdl0); round 7's native change only touches `SYNC_STATE`.

**Fix (core).** `m3Cmd`: two MCU command timeouts in a row set `recoverWanted`. The supervisor then runs the same
`chipRecover()` (wfsys reset, rings, firmware reload) after the current attempt. This shows in the log as
`mcu: ... chip reset requested` → `recover #1`. The root cause of the silence is still open.

Staged 2026-11-04 12:35: core `0C89AEB3-EC59-3BA8-8DE7-B2FBF0E41694`. The previous core (`AB264730`) is in
`Kexts-backup/itlmt7925.kext.20261104-123537`. The native stays `DBDD093B` (§10.34).

**Next boot: cold boot.** Then run the §10.34 test. If Wi-Fi drops again, the m3-log should show `chip reset requested` and
`recover #1`, followed by a fresh join instead of a dead radio. Undo (core only):
```
cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad8 && mv Kexts-backup/itlmt7925.kext.20261104-123537 Kexts/itlmt7925.kext
```

### 10.36 Hardware result of 10.34/10.35 (cold boot 2026-11-04 12:46) and round 8: AWDL data frames were never found in RX

**Verified on this boot.**
* Infrastructure Wi-Fi and Bluetooth came up normally. The §10.35 recovery was not exercised: no MCU timeouts.
* **Slave sync works (§10.34).** `role=2 master=aee5e48339fb` (the iPhone), `align=2/59`, `syncs=465`, `adj` a few
  hundred µs. The Family's sequence numbers sit next to the iPhone's (`MI AF has sequence number:(13302), current
  window:(13292)`). Election metric: iPhone 537, ours 511.
* **Data TX works (§10.33).** Native `dtx` 96 queued / 96 done; core `tx=95/95/0`; awdl0 `Opkts 96`.
* **wifip2pd.**
  * `AWDLBrowse _airdrop._tcp.local was started`.
  * Discovery results for the iPhone's `_applicationservicepairing` / `_appsvcprepair` at −42 dBm.
  * `Posted peer presence for ae:e5:e4:83:39:fb on awdl0`.
  * `AWDLAdvertise 366bfda0f544._airdrop._tcp.local was started` (our instance).
  * sharingd: `Device in everyone mode`.
* **No data RX.** `drx=0`, core `data rx=0`, awdl0 `Ipkts 0`. sharingd never logged a DISCOVER from the iPhone.

**Enable ordering (workaround, not fixed).** At boot, wifip2pd tries to create AWDL on en5 three times (12:47:06–12:47:59)
and gets `Operation Not Supported`. awdl0 appears at 12:49:05, once Wi-Fi is powered and joined. AWDL was enabled only at
12:55:29, after this sequence:
1. Close Finder > AirDrop.
2. Kill the `dns-sd -includeAWDL` processes, by PID.
3. Start `dns-sd -includeAWDL -B _airdrop._tcp local.` again.

A browse started before awdl0 existed keeps mDNSResponder's AWDL session to wifip2pd stale, so `SYNC_ENABLED` never
comes. The root fix is to answer wifip2pd's early create; that is open.

**Bug 1: RX header offset in sniffer mode.**
* The forensics lines show it: `awdl rx fx len=1143 hdr=52 ... sig=1@208 bss=1@200`. rxHdrOff() returns 52, but the
  802.11 header really starts at 184 (addr3 = AWDL BSSID at 200). The sniffer adds RX vector groups that rxHdrOff()
  does not count.
* Action frames are found anyway, because their path scans the whole buffer for the AWDL signature.
* `awdlRxData()` only looked at `rxHdrOff()+0..7`, so no AWDL data frame could ever match.
* **Fix:** `awdlRxData()` locates the header by addr3 == 00:25:00:ff:94:73 anywhere in the buffer, then checks: data
  frame, no DS bits, AWDL SNAP. Null data is consumed. QoS +HTC (+4) is handled in the core and the native.
* New counters: `rxstat ... data rx=N hdr=A/B` (A = at rxHdrOff, B = elsewhere).

**Bug 2: TX retry storm while AWDL is not on the air.**
* Before the enable, the stack pushed into a full core ring, and `dtx` full reached 61892 retries.
* **Fix:** the core records the uptime of the last AWDL radio window. When the ring is full and no window ran in the last
  second, the frame is refused as stale (`kIOReturnNotReady`). The native drops it (new 6th `dtx` field) instead of
  retrying. A full ring while windows are running is still backpressure.
* New counter: `rxstat ... stale=N`.

Staged 2026-11-04 13:01:

| Kext | UUID | Backup |
|---|---|---|
| core | `8525CBFF-7B4F-3175-A87F-D7E84DCD4974` | `Kexts-backup/itlmt7925.kext.20261104-130156` = `0C89AEB3` |
| native | `529071AB-BDA2-3619-B195-167A7F7EB913` | `Kexts-backup/itlmt7925Native.kext.20261104-130156` = `DBDD093B` |

**Next boot (cold).**
1. **Check the boot.** `kmutil showloaded | grep itlmt7925` must show `8525CBFF...` / `529071AB...`.
2. **Bring AWDL up.** Do not open Finder > AirDrop and do not start dns-sd until `ifconfig awdl0` exists. Then start
   `dns-sd -includeAWDL -B _airdrop._tcp local.`, then open Finder > AirDrop. If `itlmt-awdl` shows no `dp=1` within a
   minute, use the restart sequence above.
3. **Check sync.** Same as §10.34: `role=2`, `align` growing.
4. **Check data RX** (the point of this round). Open the iPhone share sheet.
   * m3-log `awdl data rx len=... sa=aee5e48339fb` lines appear; `rxstat data rx` > 0 with `hdr=0/B`, B > 0.
   * Native `drx=in/up/...` with up > 0; awdl0 `Ipkts` > 0.
   * Then sharingd `Received DISCOVER` / the Mac appears on the iPhone.
5. **If `data rx` stays 0** but the iPhone's MIFs keep arriving, the iPhone is not sending data to us. Look next at
   whether our MIF carries the `_airdrop` service response.
6. **Undo.**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad9 && mv Kexts-backup/itlmt7925.kext.20261104-130156 Kexts/itlmt7925.kext
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad9 && mv Kexts-backup/itlmt7925Native.kext.20261104-130156 Kexts/itlmt7925Native.kext
   ```

### 10.37 Hardware result of 10.36 (cold boot 2026-11-04 13:03) and round 9: does the iPhone ever take our frames into its election?

**Verified on this boot.**
* Core `8525CBFF` and native `529071AB` loaded; infrastructure Wi-Fi joined normally.
* **Data RX fix works.** The first AWDL data frame was found ~130 bytes past rxHdrOff (`hdr=0/1`) and reached the stack:
  `drx=1/1`, awdl0 `Ipkts 1`. It was mDNS (`da=3333000000fb`) from a third Apple device, `62:ed:76:a2:79:0d`, not
  from the iPhone.
* **Stale-drop works.** `stale=0`, `dtx` 105/105: no retry storm.

**Bring-up needed three manual steps** (the ordering problem from §10.36):
1. awdl0 does not come up by itself. wifip2pd logs `Failed to create interface AWDL on en5 ... Operation Not
   Supported`, and that create never reaches the driver: no `cmd=5e` in the native trace, no IOUC log line. So it is
   refused in userland, before the ioctl. awdl0 was created with `osascript tools/awdl/awdl-vif-test.applescript`, the
   same way as the earlier boots.
2. Close Finder > AirDrop, then restart `dns-sd -includeAWDL -B _airdrop._tcp local.`. This rebuilt mDNSResponder's
   AWDL session and gave `enabled AWDL`.
3. **New this boot:** no `AWDLAdvertise ..._airdrop` until AirDrop was switched to No One and back to Everyone.
   sharingd had registered its service before AWDL was up. After the toggle we got `AWDLAdvertise
   366bfda0f544._airdrop._tcp.local was started`, a listener on awdl0:8770, and the MIF TLV payload grew from 133 to
   940 bytes.

**Still not shown on the iPhone.**
* The iPhone stayed present (`Posted Peer Presence`), but sent no data frame to us and no DISCOVER.
* The election on this boot:
  * The iPhone was master at first (`met 518`), then dropped out once (`Peer Absence`).
  * After that we elected ourselves (`met 529`, `role=1`) for the rest of the boot.
* The Family's own dump says `AWDL on for more than 30 seconds with no TX (...) or RX (...)`. Its counters do not see
  our data path.
* The only peer TLV 4 we have, published once at the first frame, names the iPhone itself as master (`+21 =
  aee5e48339fb`). Ours names us. That looks like two masters, but the copy is stale.

**Hypothesis.** The iPhone does not take our PSF/MIF into its election or its service discovery. That would explain both
boots:
* §10.36: we were slave and the iPhone never discovered us.
* This boot: we were master with a higher metric, and the iPhone kept itself master.

**Round 9 (native only, instrumentation).**
* Every peer TLV 4 is decoded for the master it names: `pm=self/us/other last=<master>@<AW seq>` on the `itlmt-awdl`
  line.
* `itlmt-awdl-peer-tlv4` and `itlmt-awdl-self-tlv4` are republished every 200 frames instead of once, so each can be
  compared with the current role.

Staged 2026-11-04 13:19: native `30B898E6-C430-3F4F-A2DD-D08089E3245D`. The previous native (`529071AB`) is in
`Kexts-backup/itlmt7925Native.kext.20261104-131905`. The core stays `8525CBFF` (§10.36).

**Next boot (cold).**
1. **Check the boot.** `kmutil showloaded | grep itlmt7925` must show `8525CBFF...` / `30B898E6...`.
2. **Bring AWDL up.** Do not open Finder > AirDrop yet. Then, in order:
   1. Run `osascript tools/awdl/awdl-vif-test.applescript` and wait for `ifconfig awdl0`.
   2. Start `dns-sd -includeAWDL -B _airdrop._tcp local.` and wait for `enabled AWDL` in the wifip2pd log.
   3. Switch AirDrop to No One and back to Everyone, and wait for `AWDLAdvertise ..._airdrop`.
   4. Open Finder > AirDrop and the iPhone share sheet.
3. **Read `pm=`.**
   * `pm=0/N/0`, the iPhone names us: it follows us. If the Mac still does not show, look at the discovery and TCP
     path next.
   * `pm=N/0/0` while we are `role=1`: the iPhone ignores our frames. Next, check what the iPhone can be missing:
     * our frame timing against its AW (`last=...@seq` against our own AW number);
     * the TLV 4 fields: compare `self-tlv4` and `peer-tlv4` byte by byte;
     * the TX rate and channel of our PSF/MIF.
   * `role=2`, with the iPhone named on both sides: same as §10.36. The slave is synced, so the question becomes
     service discovery.
4. **Undo (native only).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad10 && mv Kexts-backup/itlmt7925Native.kext.20261104-131905 Kexts/itlmt7925Native.kext
   ```

**Open, for a later round.** The root fix for the bring-up order: find why wifip2pd refuses its own AWDL create on en5 in
userland, so awdl0 exists before sharingd and mDNSResponder start.

### 10.38 Hardware result of 10.37 (cold boot 2026-11-04 13:21): TCP runs over AWDL; unicast to awdl0 is never ACKed

**Bring-up.**
1. vif tool, then `dns-sd -includeAWDL -B _airdrop._tcp local.`. This time `enabled AWDL` came at once (13:30:53),
   because Finder > AirDrop was not open before awdl0 existed.
2. AirDrop No One → Everyone toggle, then `AWDLAdvertise 366bfda0f544._airdrop._tcp.local was started`.

**Results.**
* **pm=271/0/1.** In every frame the iPhone names itself as master. We are master too (`role=1`, metric 537 vs the
  iPhone's ~518), so the trees are split. Even so, the iPhone sends data to us.
* **Data RX.** `data rx=163 hdr=0/163`, `drx=163/163`, awdl0 `Ipkts 171`. Included is unicast to our awdl0 MAC
  (`fc=8808`, i.e. QoS data with the **retry** bit, `da=de567b3481d4`, ch 149).
* **Data TX.** `dtx=274/274`, core `tx=273/273`.
* **TCP over AWDL works.** A 40 s `tcpdump -i awdl0` (351 packets) shows:
  * The Mac connects to the iPhone's `_appsvcprepair` service (iPhone ports 57707/57710), with SYN → SYN-ACK → data
    in both directions.
  * The iPhone answers mDNS on awdl0 (PTR `_applicationServicePairing`/`_appSvcPrePair`).
  * We answer with our `_airdrop` PTR / TXT.
* **But every unicast from the iPhone arrives many times.** One SYN-ACK came 22 times, in bursts a few ms apart with an
  identical TCP timestamp, so these are 802.11 retries, not TCP retransmits. The iPhone's 1412-byte segment is resent
  again and again even though we ACK it.
  * **Cause:** the chip's MAC only acknowledges addr1 == an active OMAC, and only OMAC 0 (the station address) is
    programmed. The iPhone never gets an 802.11 ACK, treats every unicast as lost, and backs off.
  * Our own TX also needed up to 8 SYNs. Those go out only inside our windows, so that part is expected.
* **No AirDrop connection.** Nothing to port 8770 in the capture. The Mac still does not show on the iPhone.

**Round 10.**
* **Core: awdl0 as a second OMAC.** The first AWDL frame handed to the core gives the interface address (addr2). The
  link thread then sends `DEV_INFO_UPDATE` with omac_idx 1, band auto, active, link_idx 1 and that address. This is the
  same layout attach uses for OMAC 0. After a chip recover the command is sent again.
  * Log line: `awdl own mac (omac 1) rc=… st=…` / `awdl own mac <addr> rc=…`.
* **Native: 802.11 duplicate filter.** For AWDL data, keyed on sender + sequence control, with the retry bit set and an
  8-sender table. A retry of the frame just delivered is dropped. Shows as the new 5th `drx` field (`drx=in/up/notus/
  fail/dup`).

Staged 2026-11-04 13:38:

| Kext | UUID | Backup |
|---|---|---|
| core | `A03A05D7-DD1C-3343-976C-5FDEF705313D` | `Kexts-backup/itlmt7925.kext.20261104-133809` = `8525CBFF` |
| native | `DC024A86-7633-361E-A26F-DD9ADDD0A56D` | `Kexts-backup/itlmt7925Native.kext.20261104-133809` = `30B898E6` |

**Next boot (cold).**
1. **Check the boot.** `kmutil showloaded` must show `A03A05D7` / `DC024A86`. Infrastructure Wi-Fi must still join:
   the second OMAC must not disturb the station.
2. **Bring AWDL up.** Same order as §10.37: vif tool, dns-sd, wait for `enabled AWDL`, AirDrop toggle, then Finder >
   AirDrop and the iPhone share sheet.
3. **m3-log.** `awdl own mac de567b3481d4 rc=0`.
4. **Capture.** Record 40 s of `tcpdump -i awdl0` (as root), then check:
   * No more bursts of identical SYN-ACKs. If some remain, compare `drx ... /dup` with the bursts.
   * Port 8770 traffic, and the Mac shown on the iPhone.
5. **If rc ≠ 0, or the retries stay the same,** the RMAC also wants a BSS bound to OMAC 1. Next step:
   `BSS_INFO_UPDATE` basic for bss_idx 1 / omac 1, as an "AWDL"/P2P-device connection type.
6. **Undo.**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad11 && mv Kexts-backup/itlmt7925.kext.20261104-133809 Kexts/itlmt7925.kext
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad11 && mv Kexts-backup/itlmt7925Native.kext.20261104-133809 Kexts/itlmt7925Native.kext
   ```

### 10.39 Hardware result of 10.38 (cold boot 2026-11-04 14:37): unicast is ACKed, Continuity TCP runs over AWDL; AirDrop stops before AWDL

**Verified.**
* Core `A03A05D7` and native `DC024A86` loaded. Infrastructure Wi-Fi joined normally, so the second OMAC does not
  disturb the station.
* **OMAC 1 accepted.** `awdl own mac (omac 1) rc=0 st=0` / `awdl own mac de567b3481d4 rc=0`.
* **Bring-up.** vif tool, then dns-sd: `enabled AWDL` immediately (14:41:35). After the AirDrop toggle, `AWDLAdvertise
  366bfda0f544._airdrop._tcp.local was started` (14:43:27).
* **Unicast retries are gone.** `drx=49/34/0/0/15`: 15 retries were filtered. The 45 s awdl0 capture has 57 packets and
  only 2 repeated segments; before, one SYN-ACK arrived 22 times.
* **The iPhone opens TCP to the Mac over AWDL.** rapportd (Continuity) holds `fe80::dc56…:61351 ↔ fe80::ace5…:56199`,
  ESTABLISHED: the iPhone connected and we accepted with a SYN-ACK. This is the first Apple service running over this
  driver's AWDL.
* **Election.** `pm=678/5/6`: for the first time, the iPhone named us as master (5 frames). It is mostly still its own
  master, and we stay `role=1`.

**AirDrop fails in both directions at the same point, before AWDL.**
* **iPhone → Mac.** The Mac is not shown on the iPhone. The iPhone sends no mDNS for `_airdrop` on awdl0 and makes no
  connection to :8770.
* **Mac → iPhone.** With the iPhone's AirDrop set to Everyone for 10 Minutes, it is not shown in Finder. wifip2pd gets
  no `_airdrop` discovery result from the iPhone (its `_applicationservicepairing` / `_appsvcprepair` do arrive), and
  dns-sd lists only our own instance.

AWDL itself (service TLVs, mDNS, TCP) works; only the AirDrop instances never appear. AirDrop discovery is gated on
Bluetooth LE:
* the sender's BLE AirDrop advertisement wakes receivers;
* receivers advertise `_airdrop` on AWDL only after they see it.

sharingd logs `Starting to advertise for <private>`, but whether the MT7925 Bluetooth controller really puts that
advertisement on the air has never been checked. Scanning works: sharingd sees NearbyInfo devices. That is the next
suspect, and it belongs to the Bluetooth work, not to AWDL.

**Next checks (no reboot needed).**
1. **Mac → air.** On the iPhone, a BLE scanner (e.g. nRF Connect) should show the Mac advertising Apple manufacturer
   data (company 0x004C) while Finder > AirDrop is open. If no Apple advertisement from the Mac shows up, BLE advertising
   on the controller is the blocker.
2. **iPhone → Mac.** While the iPhone's share sheet is open, sharingd should log the iPhone's AirDrop BLE advertisement
   (AirDropNW.BLE). None was seen in this boot.

**Reference check (OpenDrop README, read for facts).** Under "Current Limitations/TODOs", OpenDrop says Apple receivers start
their AWDL interface and AirDrop server only after they receive a custom Bluetooth LE advertisement. Without that trigger
they may not be discovered, even when set to Everyone. OpenDrop never implemented the trigger, which is why it can miss
Apple receivers. Our stack is different: macOS's own sharingd sends the trigger, so what has to work is the BLE
advertising path of the MT7925 Bluetooth controller (HCI LE advertising commands accepted, and the advertisement
actually on the air).

No kext change this round. Core `A03A05D7` and native `DC024A86` stay staged.

### 10.40 Same boot (14:37), ~15 min of AWDL: the firmware went silent and took infrastructure Wi-Fi with it

**What happened** (m3-log):
1. After ~3100 AWDL windows (`rocfail=133`, i.e. 133 never granted), `sniffer off` and `roc_abort` timed out
   (`rc=-110`). The log then shows `mcu: roc_abort and the command before it timed out: chip reset requested` and
   `postmortem silent 4s`.
2. The link thread kept scheduling AWDL windows: `rx_filter`, `sniffer on`, `roc_acquire`, ... each waited 3 s for the
   dead MCU.
3. The infrastructure link was declared lost only 15 s later (`no beacon or data from the AP for 15 s`). Only then did
   the supervisor run `recover #2` (so this was the second time this boot).
4. Wi-Fi rejoined (`RESULT rc=0 link up`) and AWDL came back (OMAC 1 again). Then the link left twice "for a command",
   and the owner switched Wi-Fi off and fell back to USB tethering.

**Cause (likely).** Every AWDL window asked the firmware for a **JOIN-type ROC with a 5000 ms maxinterval**, then
aborted it after 20–100 ms, about 10 times a second. Linux mt7925 (read for facts) uses `MT7925_ROC_REQ_JOIN` = 0 only for
association, and `MT7925_ROC_REQ_ROC` = 1 for a remain-on-channel, with `maxinterval` = the requested duration. Hundreds of
5-second join reservations cancelled immediately is a plausible way to wedge the firmware's channel manager. §10.35's
silence (4 s after link-up, without AWDL) is a separate trigger.

**Fix (core).**
* AWDL windows request `reqtype 1` (ROC) with `maxinterval = dwell + 30 ms`. The join path keeps JOIN / 5000 ms.
* As soon as `recoverWanted` is set, the link loop declares the link lost ("the firmware stopped answering MCU
  commands") and the supervisor resets at once, not after 15 s.
* No AWDL window is scheduled while a reset is pending.

Staged 2026-11-04 15:00: core `88160DDE-6965-3E99-9991-E36849B247BC`. The previous core (`A03A05D7`) is in
`Kexts-backup/itlmt7925.kext.20261104-150020`. The native stays `DC024A86` (§10.38).

**Next boot.**
1. Run AWDL for at least 20 minutes, with the §10.38 bring-up order. Watch `awdl stat ... rocfail=`: it should stay
   near 0.
2. Infrastructure Wi-Fi must keep working throughout (`pulse` lines advancing, no `chip reset requested`).
3. If the firmware still goes silent, the log should show `recover #N` within ~6 s of the first timeout, then a rejoin.
4. **Undo (core).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad12 && mv Kexts-backup/itlmt7925.kext.20261104-150020 Kexts/itlmt7925.kext
   ```

### 10.41 The AirDrop blocker is Bluetooth: bluetoothd's LE advertising never becomes ready (boot 14:37, Android scan 15:08)

* **The Mac asks for advertising.** sharingd: `AirDropNW.BLE Started advertising` (15:05:52) and `CBAdvertiser Activated
  ... CA DC:56:7B:34:81:D6`. bluetoothd receives `start advertising` requests for Nearby (`0B FF 4C 00 10 …`), type 9
  (`0D FF 4C 00 09 …`) and AirDrop.
* **bluetoothd sends nothing.** Every request ends in `[Server.LE.Advertise] Stack is not started, updateAdvertisement
  returning`. Since the core stack started (14:38:25, `STACK_READY/StackStarted`), that was 81× `Stack is not ready,
  waiting for the stack to start` and 79× `not started`, and never a success. WirelessProximity: `Trying to update
  advertiser but peripheral manager isn't powered on` (WPErrorDomain 26).
* **Capabilities bluetoothd detected at start:** `enableBTSpecAdvertisingSets=0 enableExtendedAdvertising=0 ...
  maxAdvHandles=4`, and the chip is reported as `BCM_4350C2` / `THIRD_PARTY_DONGLE`. With BT 5.0 advertising sets off,
  bluetoothd uses its legacy multi-advertising path, and the LE advertise module never reports the stack as started on
  this controller.
* **On air.** nRF Connect on Android (RSSI ≥ −60, filter `0x4C00`) showed Apple Nearby/Find My devices, but nothing
  attributable to the Mac and no `4C 00 05` AirDrop packet.

**Conclusion.** AirDrop receivers wake only on the sender's BLE AirDrop advertisement (OpenDrop, §10.39). The Mac emits
no BLE advertisement at all, so the iPhone never starts its AirDrop side. The Mac also never sees the iPhone's AirDrop
advertisement as a trigger. Scanning works; advertising does not. The AWDL side (sync, data, TCP, OMAC ACKs) is not the
blocker any more.

**Next (Bluetooth work, outside the AWDL kexts).** Find what the LE advertise module waits for. Candidates:
* the HCI LE advertising commands the controller rejects at start: `LE Set Advertising Parameters` / `LE Read Advertising
  Channel Tx Power`, or the vendor multi-advertising command bluetoothd sends for `maxAdvHandles=4`;
* the controller features bluetoothd reads to decide that LE advertising is available.

PacketLogger (Additional Tools for Xcode) on the HCI trace at Bluetooth start is the direct way to see the failing command.

**Found and patched (2026-11-04, docs/BLUETOOTH.md "LE advertising never starts").** No HCI command fails: bluetoothd sets
a third-party USB controller to chipset 0, its legacy multi-advertising path needs a per-chipset vendor table
(`LE_InitMultipleAdvertising failed with status STATUS 152` right after `Stack Did Start`), and BT 5.0 advertising sets are
gated by a chipset whitelist that excludes chipset 0. `BtLeDlFix` now forces that one whitelist answer to yes (the MT7925
supports the LE extended advertising commands). Staged on the ESP, untested; next-boot test in docs/BLUETOOTH.md.


**Hardware 15:52 + second patch.** Advertising sets came up but flapped (718 = HCI 0x12 on LE Set Extended Advertising
Enable, 766 = 0x42 follow-on): bluetoothd skips LE Set Advertising Set Random Address for chipset ≤ 3, so the random-address
sets never get an address. `BtLeDlFix` now keeps that command on. Staged 16:07, untested (docs/BLUETOOTH.md).

**16:52 boot: still 718 (~9/s), and the BLE mouse stopped working (connected, no input).** Advertising patches reverted on
the ESP at 17:07 (pre-59db42b build); open lead: `primaryPHY:0` in the extended advertising parameters (docs/BLUETOOTH.md).

**Fourth patch (23:43, HCI trace).** The PHY lead was wrong. Real cause of the remaining 718: bluetoothd caches each set's
random address and sends LE Set Advertising Set Random Address only when it changed; at stack start it sends it for sets 0
and 2 before they exist (0x42), caches it anyway, so the AirDrop set (2) is later enabled without an address (0x12) and the
stack loops ~2300×/min. `BtLeDlFix` now always resends the address before Enable. Verified
live with a swapped bluetoothd: every advertising command st=00, AirDrop data `4C 00 05` enabled, no 718/766. Staged on the
ESP (patches 1-4).

Next boot: (1) `sudo dmesg | grep btledl` shows three advertising patches; (2) 0× `result 718|766` in
`log show --last 5m`; (3) the BLE mouse moves the pointer with Finder > AirDrop open; (4) nRF Connect shows `4C 00 05`
from the Mac; (5) retry iPhone ↔ Mac AirDrop. Undo: `-btledlfxnoadv`, or copy
`Kexts-backup/BtLeDlFix.kext.20261104-234307-preadv` back (docs/BLUETOOTH.md).

**2026-11-05 10:24 boot: advertising OK, BLE mouse dead.** Not the advertising patches: the builds from
fix/bt-le-advertising lacked the macOS 26 LE data length wait patch that the uncommitted v1.1.0 build had, so the mouse
link never reaches LE Link Ready (no GATT discovery, HID notifications dropped). v1.2.0 restores it (docs/BLUETOOTH.md
"BLE mouse connected but dead"), staged on the ESP. Next boot: mouse moves, `LinkReady took` in the log, 0× 718/766, nRF
Connect shows `4C 00 05`, then iPhone ↔ Mac AirDrop.

**10:42 boot (v1.2.0): BLE mouse works and advertising is clean** (LinkReady 180 ms, 0× 718/766). Next: nRF Connect
`4C 00 05` from the Mac, then iPhone ↔ Mac AirDrop.

**10:54: Mac's AirDrop BLE advert seen on air** (nRF Connect: `4C 00 05 12 40 F5 …` from 57:8F:05:5D:57:87, matching the
HCI trace). The §10.41 blocker is closed. Next: iPhone ↔ Mac AirDrop over AWDL.

### 10.42 Boot 2026-11-04 16:52 (core 88160DDE): fast recovery works, but the firmware now hangs every 1-3 minutes of AWDL

Recorded live (`ioreg` m3-log + state every 5 s) from AWDL enable at 17:05:19:

| Time | Event |
|---|---|
| 17:06:42 | `sniffer off rc=-110`, then `roc_abort rc=-110` → `chip reset requested` |
| 17:06:50 | `link lost: the firmware stopped answering MCU commands`, `recover #1` (the §10.40 fast path: ~8 s instead of 15) |
| 17:07:11 | rejoined (`RESULT rc=0 link up`), AWDL resumed |
| ~17:08 | `recover #2` |
| 17:09:20 → 17:09:56 | `rocfail` 39 → 81 (42 ungranted ROCs in 36 s) |
| 17:10:11 | `chip reset requested`, `recover #3`; 17:10:46 link up |

* Three hangs in 7 minutes, against one in ~15 minutes with the old JOIN/5000 ms ROC (§10.40). The ROC type change did
  not fix the hang and made it come sooner.
* Every hang follows a run of `roc fail ... grant=0 status=255`: requests the firmware never granted within 80 ms, which
  we then aborted, and the next window asked again 20-60 ms later. Linux mt7925 waits up to 4 s for a grant and never
  aborts a ROC that has not been granted.
* Infrastructure Wi-Fi comes back on its own each time (about 20-35 s without connectivity). Between hangs the AP link
  keeps working (`pulse` advancing).

**Round 12 (core).**
* **Strict ROC lifecycle.** The grant is waited for up to 300 ms, with m3Pump servicing the AP meanwhile.
  * A grant after 80 ms is accepted, and the ROC released at once, with nothing sent in it.
  * A ROC that is never granted is aborted, and AWDL windows pause for 2 s, so ungranted requests no longer pile up.
* **Trace.**
  * `awdl roc latency <10/<40/<80/late/never` goes out with each `awdl stat`.
  * On `chip reset requested`, `mcu hist ...` dumps the last 24 MCU commands (`name:rc@ms`), so the sequence the firmware
    died in is on record.

Staged 2026-11-04 17:12: core `29E5C6A5-2307-380C-A592-D12D9E37B13C`. The previous core (`88160DDE`) is in
`Kexts-backup/itlmt7925.kext.20261104-171219`. The native stays `DC024A86`.

**Next boot.**
1. Bring AWDL up as in §10.38. Run the recorder (5 s `ioreg` snapshots) for 20 minutes.
2. Expect no `chip reset requested`, and a `never=` count that stays low.
3. If the firmware still dies, read `mcu hist`:
   * dies after `roc_acquire` with no grant: the windows themselves are the problem. Next step: fewer windows, a longer
     minimum gap between them, or the sniffer left on for the whole AWDL session instead of toggled per window;
   * dies after `sniffer off` / `rx_filter`: the per-window sniffer toggling is the problem.
4. **Undo (core).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad13 && mv Kexts-backup/itlmt7925.kext.20261104-171219 Kexts/itlmt7925.kext
   ```

**Also this boot.** The BLE mouse is "connected" but does not work after BtLeDlFix `96742446`
(`fix/bt-le-advertising`). Reported to the Bluetooth session.

### 10.43 Boot 2026-11-04 17:29 (core 29E5C6A5): no firmware hang. Wi-Fi drops because AWDL starves the AP link

AWDL was enabled at 17:34:09. At 17:38:12 en5 went down. It rejoined at 17:38:25 (RESULT rc=0 link up).

* **The firmware did not hang.**
  * No `chip reset requested`, no `link lost`, no `rc=-110`.
  * ROC latency: `<40=946 late=6 never=1`. The §10.42 strict ROC lifecycle held.
* **airportd left the network itself.**
  * symptomsd measured `TCP progress ... problem ratio: 1.00` from 17:36:58 on.
  * airportd went `not broken -> tcp friction (strong) -> confirmed (quick probe)` (broken backhaul).
  * At 17:38:12 it sent `WCL_LEAVE_NETWORK`, which the core logs as `supervisor: disconnect`. Auto-join rejoined 12 s later.
* **Why TCP broke.** The radio was away from the AP's channel most of the time.
  * The `pulse` beacon count (beacons from our AP only) fell from ~10/s before AWDL to 1-6/s with AWDL.
  * From 17:36:39 to 17:36:54 neither `rxd` nor `tx` moved.
  * Each window is 100 ms (`kAwdlPsfRocMs`), plus the ROC grant and 5-6 MCU commands. Windows came back to back at
    ~3.3/s.
  * Nothing told the AP that we were away, so its downlink frames were retried into the void.
* **The LQM summary the native reports is wrong** (`txFail ≈ txFrames`, `beaconRecv` tiny) already before AWDL. It
  played no part here (bbh works from TCP metrics), but it is a separate bug. Follow-up.

**Round 13 (core).**
* **802.11 power save toward the AP around every AWDL window.**
  * A null-data frame with PM=1 (`fc 48 11`, ToDS, to the BSSID) goes out before the sniffer, RX filter and ROC commands.
  * A null-data frame with PM=0 goes out as soon as `roc_abort` returns. Every way out of the window does this
    (`winEnd()`).
  * The AP buffers our downlink while we are away. Any later uplink data frame (PM=0) also wakes us, so a lost PM=0 null
    cannot leave the link asleep.
* **Off-channel duty cap.**
  * The time from the PM=1 null to the PM=0 null is measured.
  * The next window is not taken before the same time has passed again on the AP's channel (at most half the airtime
    away). Windows refused this way are counted as `skip`.
* **Trace.** `awdl duty: off=<ms> in <n> windows (avg) skip=<n> ps=<pm1>/<pm0> apbcn=<AP beacons>`, with every
  `awdl stat`.

Staged 2026-11-04 17:43: core `525C76B6-B8D5-385D-AD56-587618D32DD6`. The previous core (`29E5C6A5`) is in
`Kexts-backup/itlmt7925.kext.20261104-174343`. The native stays `DC024A86`.

**Next boot.**
1. Bring AWDL up as in §10.38, and leave it on for 20 minutes with the recorder running. Use Wi-Fi normally: browse,
   play a video.
2. **Expected.**
   * en5 stays up.
   * No `tcp friction` / `broken backhaul` in `log show --predicate 'process == "airportd"'`.
   * `awdl duty` shows `ps` counts equal to the window count, an avg near 130-150 ms, and `skip` > 0.
   * `apbcn` keeps rising at ≥5/s.
3. **If TCP still stalls.**
   * `ps` stays 0: no link up at window time, so check `m3linkUp`.
   * `ps` counts rise but downlink still dies: the AP ignores our PM bit (check with a sniffer that the AP sets our TIM
     bit), or the firmware overrides it. Next lever: shorten the dwell (`kAwdlPsfRocMs` 100 → 48 ms, native).
4. **Also check.** AWDL itself still works with fewer windows:
   * `awdl data rx` / peer `drx` still rising;
   * the iPhone still listed by `dns-sd -includeAWDL`.
5. **Undo (core).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad14 && mv Kexts-backup/itlmt7925.kext.20261104-174343 Kexts/itlmt7925.kext
   ```

### 10.44 Boot 2026-11-04 21:38 (core 525C76B6): Wi-Fi drops without AWDL. The AP's group key rekey was never answered

The owner reported unstable Wi-Fi and went back to USB tethering. AWDL was not up this boot (no awdl0), so the §10.43
code never ran.

* **The network is a different AP:** `9c:5d:12:89:1f:24`, ch 157, VHT80, SSID length 12.
* **The AP drops us.** Disassociation reason 2, then deauthentication reason 1, every 30-80 s (21:46:59, 21:48:43,
  21:49:40 ...). airportd rejoined each time, 10 s per join, and the core ran a full `recover #N: wfsys reset` for every
  drop.
* **Cause.** Before each drop, `pulse rxe` (EAPOL frames received) rises by 1-3 long after the 4-way handshake: 24→27,
  29→31→32. These are the AP's group key handshake (message 1/2, the GTK rekey).
  * After `RESULT link up`, `m3wantEapol` is false, so the RX path counted and dropped them.
  * The AP got no message 2/2, resent message 1/2, and then removed the station. An AP with a short group rekey interval
    makes this look like random instability.

**Round 14 (core).**
* EAPOL-Key frames on the link are queued and handled in the link loop by `m3EapolOnLink()`:
  * group message 1/2 (ver 2, MIC|ACK|SECURE, non-pairwise);
  * the replay counter must be newer than the last accepted one (M3, or the previous rekey);
  * the MIC is checked with the KCK;
  * the GTK KDE is unwrapped with the KEK;
  * message 2/2 (`0x0302`, same replay counter) is sent encrypted with the PTK;
  * the new GTK is installed on the broadcast wcid.
* **Trace.** `eapol group rekey #n id= len= msg2= key rc=` (also in ev-log). Rejections are logged as `eapol on link: ...`.
* A PTK rekey (a pairwise M1 on the link) is logged and ignored. Follow-up if an AP ever does it.

Staged 2026-11-04 21:53: core `B1CA2849-0759-38A1-B6A9-2C5061E571BF`. It also carries round 13 (§10.43). The previous core
(`525C76B6`) is in `Kexts-backup/itlmt7925.kext.20261104-215326`. The native stays `DC024A86`.

**Next boot.**
1. Join the same AP and use Wi-Fi only (unplug USB tethering) for 15 minutes.
2. **Expected.**
   * `eapol group rekey #1, #2 ... key rc=0` in m3-log at the AP's rekey interval.
   * No `link lost: disassociation/deauthentication`.
   * No `recover #`.
3. If the drops continue, look for `eapol on link:` lines:
   * `MIC rejected` / `key data not usable`: the key derivation or KDE parsing differs for this AP;
   * `pairwise ... ignored`: the AP does PTK rekeys, so the 4-way has to run on the link too.
4. Then the AWDL test of §10.43: power save + duty cap.
5. **Undo (core).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad15 && mv Kexts-backup/itlmt7925.kext.20261104-215326 Kexts/itlmt7925.kext
   ```

### 10.45 Same boot (22:57, core B1CA2849): Wi-Fi is stable; why wifip2pd never creates awdl0 (capability 32)

**Wi-Fi (§10.44 verified).**
* The AP rekeys its group key about once a minute. `eapol group rekey #1..#8 ... key rc=0`, with no `link lost` and no
  `recover`.
* With USB tethering unplugged, all traffic over en5: 74/74 HTTPS probes OK in 5 min, a 10 MB download at ~3 MB/s.
* One 25 s gap right after unplugging USB (23:05:45-23:06:10). The core's TX counter stood still while beacons kept
  coming: the stack sent nothing. That is consistent with Cloudflare WARP blocking all traffic while it rebuilds its tunnel
  on the new default route, not with a driver fault.

**awdl0 auto-create: root cause.**
* wifip2pd still logs `Cannot create AWDL interface because the device is missing the capability` after Wi-Fi joined
  (22:59:10). So this is not boot ordering.
* Disassembly of `/usr/libexec/wifip2pd` (x86_64, the function that logs the message at 0x100048450):
  * For each interface type (`NAN` 0x4e414e, `AWDL` 0x4c445741) it calls `Apple80211RawGet` with type 0xc
    (CARD_CAPABILITIES), len 21, into a stack buffer.
  * It then tests one bit: AWDL = buffer[4] bit 0, NAN = buffer[9] bit 0. Clear bit: the error, and no ioctl.
* `tools/awdl/caps_get.m` (RawGet 0xc as a normal user) shows the buffer userland gets: `6f e6 73 24 00 41 04 00 01 02 ...`.
  * That is our capability array **without the version word**, so buffer[4] is our `d[0x08]`, which was 0.
  * Capability 32 = AWDL. Apple's driver sets it from a firmware feature flag. We left it clear under the "claim nothing
    we cannot back" policy.

**Round 15 (native).** `getCARD_CAPABILITIES` sets `d[0x08] = 0x01`, capability 32, AWDL. The driver implements the
AWDL interface, so the bit is backed. NAN stays clear.

Staged 2026-11-04 23:12: native `E1DE91CA-5A6B-30A6-81BB-B00E60FCEB10`. The previous native (`DC024A86`) is in
`Kexts-backup/itlmt7925Native.kext.20261104-231225`. The core stays `B1CA2849`.

**Next boot.**
1. Do **not** run the vif tool. Check `ifconfig awdl0` and the log
   `log show --predicate 'process == "wifip2pd"' | grep -E 'AWDL interface|capability'`:
   * **expected:** `Creating AWDL interface` followed by no `missing the capability`, then awdl0 present;
   * `cmd=5e` in the native trace.
2. Then check that AirDrop comes up without the manual steps of §10.38: Finder > AirDrop "Everyone" →
   `AWDLAdvertise ..._airdrop._tcp.local was started`, `dns-sd -includeAWDL -B _airdrop._tcp local.` → `enabled AWDL`.
3. Wi-Fi with AWDL on: the §10.43 power-save and duty-cap test (20 min, normal use, no `broken backhaul`).
4. **If something regresses.** The Family itself also reads the capability array, and may now start its own AWDL paths at
   boot. Collect `itlmt_native_trace` and the wifip2pd log first.
5. **Undo (native).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad16 && mv Kexts-backup/itlmt7925Native.kext.20261104-231225 Kexts/itlmt7925Native.kext
   ```

### 10.46 Boot 2026-11-04 23:32 (core B1CA2849, native E1DE91CA): awdl0 comes up by itself; a new AP drops us because M4 is lost

**§10.45 verified.** At 23:32:57, 2 s after Wi-Fi came up and with no tool run, wifip2pd logged
`Found new interface awdl0 with role AWDL`. awdl0 is up with MAC `c6:91:a2:50:20:ba`. There is no
`missing the capability` this boot.

**New AP, new failure.**
* The AP is `02:f0:7b:79:7c:56` (ch 52, SSID length 14, CCMP/PSK, no MFP). The owner sees "Weak Security" in the Wi-Fi
  menu and the network keeps dropping.
  * The label comes from ControlCenter's recommendation UI (`DISPLAY WIFI RECOMMENDATION UI`, 23:35:09). It describes the
    AP's configuration. airportd's records all say `NetworkHasWeakSecurity = 0`.
* airportd logs a link down every 15-30 s (23:33:27, 23:33:53, 23:34:20, 23:35:21, 23:35:52 ...).
  * After each link up the core logs `eapol on link: pairwise info=13ca ignored`. 0x13ca is message 3 again: the AP never
    received M4.
  * The handshake summaries show `txFree=0`. The M4 had not left the queue when the PTK was installed (the old fixed 30 ms
    wait).
  * §10.44 only answered group messages, so the AP retried M3, gave up and the link went down.

**Round 16 (core).**
* **The first M4 is freed before the keys are installed.** The link thread pumps until TXFREE for M4 is seen, at most
  300 ms and at least 30 ms. It logs `eapol m4 freed|NOT freed after wait`.
* **A retransmitted M3 on the link is answered.**
  * The replay counter must be newer, and the MIC is checked with the KCK.
  * M4 is sent again in the clear (the AP installs its PTK only once it has M4).
  * Keys are not installed again (no PN reset, no KRACK pattern).
  * Logged as `eapol on link: m3 again #n, m4 resent=`.

Staged 2026-11-04 23:43: core `7579D98A-B4BF-3F86-BBA0-7AA27BB1BF10`. The previous core (`B1CA2849`) is in
`Kexts-backup/itlmt7925.kext.20261104-234340`. The native stays `E1DE91CA`.

**Next boot.**
1. Join the ch 52 AP and use Wi-Fi alone for 10 minutes.
2. **Expected.**
   * `eapol m4 freed after wait`;
   * at most a few `m3 again` (each answered);
   * no airportd `Link down event` every 15-30 s.
   * On the earlier AP (`9c:5d:...`), the group rekeys still work.
3. awdl0 should again appear by itself (§10.45). Then the AWDL + Wi-Fi test of §10.43.
4. **Undo (core).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925.kext Kexts-backup/itlmt7925.kext.bad17 && mv Kexts-backup/itlmt7925.kext.20261104-234340 Kexts/itlmt7925.kext
   ```

### 10.47 Boot 2026-11-05 08:46 (core 7579D98A, native E1DE91CA, BtLeDlFix F715BA43): §10.46 verified on the ch 52 AP

* `eapol m4 freed after wait` on the first handshake. The AP never resent M3 (`rxe=2` for the whole run), so no
  `m3 again` was needed.
* 12 minutes on en5 alone:
  * one join, zero airportd `Link down event`, no `recover`;
  * 134/137 HTTPS probes OK (the misses: one moment without a default route at 08:51:00, one 5 s timeout at 08:56:26);
  * 10 MB at ~2.7 MB/s.
* awdl0 came up by itself again (§10.45).
* **Open.**
  * BLE mouse and AirDrop with the Bluetooth session's fourth BtLeDlFix (`bbac9e0`, branch `fix/bt-le-advertising`).
  * The AWDL + Wi-Fi test of §10.43.

### 10.48 Sleep took 17.5 s longer because en5 advertised keepalive offload it does not have (sleep test 2026-11-05 08:50)

From the owner's sleep test (`~/ga403-logs/syssleep-20261105-084946`, 08:50:15-08:50:33):

| Time | Event |
|---|---|
| 08:50:15.517 | powerd: `Entering Sleep state`; apsd `prepareForSleep`, `Waiting to acknowledge power change event` |
| 08:50:15.525 | airportd: `WOW_ENABLED` on en5 → -3900, `WoW is not active on en5`; airportd itself acknowledges at once |
| 08:50:15.544 | apsd: `Unable to obtain sleep proxy keep alives, reason = -65542` |
| 08:50:16.607 | apsd: `_interfaceSupportsKeepAliveProxy {interfaceHasSupport: YES, usedRecursiveSearch: YES}`, so it asks again |
| 08:50:30.555 | powerd: `apsd TimedOut ... com.apple.apsd-requestkeepaliveproxy-push.apple.com` (15 s) |
| 08:50:33.036 | apsd: `Acknowledging power change event after waiting` |

**Cause.** `itlmt7925WifiInterface` carries `"mDNS_KEY" = "2009-07-30"` and `"mDNS_Keepalive" = Yes` in the IORegistry.
* These are the keys mDNSResponder and apsd search for (recursively, from the BSD interface up) to decide that the NIC
  can hold Bonjour sleep-proxy records and TCP keepalives while the host sleeps.
* Our code never sets them; the Family publishes them on the interface.
* The driver implements neither offload, and `WOW_ENABLED` answers unsupported. So apsd waited for keepalive records the
  NIC could never take.

**Round 17 (native).**
* `itlmt7925WifiInterface` overrides the `setProperty` overloads the Family can use (OSSymbol / OSString / C string with
  OSObject, string, bool) and never stores those two keys.
* A real WoW + TCP keepalive offload would be the other way to fix this; it is follow-up work.

Staged 2026-11-05 09:09: native `59639683-12FE-3989-9689-9C44E28D4536`. The previous native (`E1DE91CA`) is in
`Kexts-backup/itlmt7925Native.kext.20261105-090941`. The core stays `7579D98A`.

**Next boot.**
1. `ioreg -l -w0 | grep -E '"mDNS_(KEY|Keepalive)"'` prints nothing.
2. Sleep test:
   * no `Unable to obtain sleep proxy keep alives`;
   * no `apsd TimedOut ... requestkeepaliveproxy`;
   * apsd acknowledges within ~1 s of `prepareForSleep`.
3. Wi-Fi after wake: link back, no airportd link-down loop. awdl0 still auto-created (§10.45).
4. **Undo (native).**
   ```
   cd /Volumes/ESP/EFI/OC && mv Kexts/itlmt7925Native.kext Kexts-backup/itlmt7925Native.kext.bad18 && mv Kexts-backup/itlmt7925Native.kext.20261105-090941 Kexts/itlmt7925Native.kext
   ```
