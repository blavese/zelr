# 06a -- Network drivers and the TCP/IP stack

Snapshot: zelr `main` of 2026-09-22 (two commits after v0.37.0; `KERNEL_VERSION "0.37.0"`, include/types.h:25).
All references are `path:line` relative to the repository root. "Host order" means the kernel's `ipv4_t` convention
(a `u32` in host byte order, include/net.h:6). TLS/X.509/crypto internals belong to another area; only the interface
that http.c and the syscall layer use is covered here.

Ground truth used in addition to static reading: a real selftest log (`selftest1.log`, QEMU user networking,
rtl8139 card, one CPU, 556 passed / 0 failed). Its boot lines 55-59 read
`net rtl8139 52:54:00:12:34:56` and `card realtek 10ec:8139 at 0:3.0, ethernet`; its `[network]` section
(lines 212-221) passed all 9 checks (DHCP lease, address, gateway, resolver, ICMP to gateway, DNS of example.com).
Its `[kernel stack]` check reports "the deepest this run went left 16352 bytes of 32768 spare" (the selftest task,
which is the task that ran the DHCP/ping/DNS code).

---

## 1. Scope

| File | Lines | Role |
|---|---|---|
| include/net.h | 60 | Public API of the IPv4 stack: init, RX entry, poll, service task, DHCP, ping, UDP send, resolver, IP helpers, counters |
| include/netdev.h | 18 | One-interface abstraction over whichever NIC was found; `netdev_undriven` |
| include/netpriv.h | 9 | Byte-order helpers and `np_ip_send`, shared between net.c and tcp.c |
| include/tcp.h | 53 | TCP client API; `TCP_MAX 6` |
| include/http.h | 16 | Kernel HTTP client API (`http_get`) and its negative error codes |
| include/wifi.h | 42 | Wireless-controller detection API (`wifi_state_t`) |
| include/e1000.h | 10 | e1000 driver entry points (7 functions) |
| include/pcnet.h | 10 | PCnet driver entry points (7 functions) |
| include/rtl8139.h | 10 | RTL8139 driver entry points (7 functions, `rtl_` prefix) |
| include/usbnet.h | 38 | RNDIS-over-USB driver API (attach/detach/present + the NIC-shaped functions) |
| include/tls.h | 49 | (interface only) TLS 1.3 client keyed by TCP handle |
| kernel/netdev.c | 138 | Probe order e1000 -> pcnet -> rtl8139, USB fallback, dispatch switches, undriven-card report |
| kernel/e1000.c | 299 | Intel 8254x/82574/I217 driver: MMIO, 32 RX / 16 TX legacy descriptors |
| kernel/pcnet.c | 377 | AMD Am79C970A (PCnet-PCI II): port I/O RAP/RDP/BDP, SWSTYLE 2 init block, 64 RX / 16 TX |
| kernel/rtl8139.c | 178 | Realtek 8139: 8 KiB circular RX buffer, 4 TX slots |
| kernel/usbnet.c | 263 | RNDIS driver over xHCI bulk endpoints and encapsulated control messages |
| kernel/net.c | 843 | Ethernet, ARP, IPv4, ICMP, UDP, DHCP client, DNS resolver, the RX queue, delivery, the "net" task |
| kernel/tcp.c | 557 | TCP client, 6 connections, one segment in flight each, retransmission with backoff |
| kernel/http.c | 172 | The kernel shell's `fetch`: HTTP/1.0 GET, optionally over TLS |
| kernel/wifi.c | 64 | Finds a PCI wireless controller and says whether it could be driven |
| tools/netcheck.py | 264 | QEMU harness: auto-DHCP, panel icon/popup/button, no-card machine, USB-only machine |
| tools/wirecheck.py | 89 | QEMU harness: runs `/bin/wiretest` against the host server, checks keep-alive and cookies server-side |
| tools/webserver.py | 633 | Host-side HTTP/1.1 test server (Python `http.server`), fixtures for all web checks |
| userland/wiretest.c | 185 | Ring-3 test: gzip, cookies, three concurrent sockets |

Consulted outside the area (not deep-read, only where they touch networking): kernel/syscall.c:699-914 (sockets),
kernel/shell.c:365-433 (commands), kernel/wm.c:2506-2604 and 2846-2867 (panel), kernel/sysfs.c:171-213,
kernel/main.c:190-215, 463-464, 495-507, 583-637, kernel/usb.c:55-56, 425-563, 677-721, kernel/xhci.c:138-374,
726-869, kernel/selftest.c:420-447, 1739-1769, kernel/idt.c:62-63, 147-289, include/sched.h:164-184, kernel/fd.c:192-212,
kernel/timer.c, kernel/ioapic.c:134-155, kernel/tls.c:95-172, kernel/vfs.c:206-254, kernel/fs.c, kernel/welcome.c:57-65,
sdk/zelr.h:417-473, include/syscall.h, userland/fetch.h:470-668, userland/term.c:1099-1159, tools/webcheck.py,
tools/tlscheck.py, tools/abicheck.py, pipeline/gate.sh:477-514.

---

## 2. Big picture

### 2.1 What the area is

```
 ring 3:  browser / terminal `get` / wiretest  (userland/fetch.h, term.c)
             |  int 0x80: SYS_CONNECT 25, SYS_SEND 26, SYS_RECV 27, SYS_DISCONNECT 28,
             |            SYS_RESOLVE 29, SYS_NETINFO 30, SYS_TLS_CONNECT 45, SYS_TLS_STATUS 46
 kernel:  kernel/syscall.c socket table (6 sockets, owner pid)      kernel shell: net/dhcp/ping/fetch/resolve
             |                                                        |            (kernel/shell.c, kernel/http.c)
          kernel/tls.c (per TCP handle)  <-- http.c `https://`       |
             |                                                        |
          kernel/tcp.c   (6 connections, client only)  <--------------+
             |  np_ip_send / tcp_input
          kernel/net.c   Ethernet, ARP(16), IPv4, ICMP, UDP, DHCP, DNS, RX queue (64), "net" task
             |  netdev_send / netdev_poll / net_receive
          kernel/netdev.c  e1000 | pcnet | rtl8139 | usbnet(fallback)
             |
          hardware: PCI NIC (DMA into heap buffers) or xHCI bulk endpoints (RNDIS)
```

There are no sockets inside the kernel stack itself: TCP connections are small integer handles (`int h` in 0..5),
UDP is used only internally (DHCP on port 68, DNS replies on ports >= 40000), and ring 3 sees a separate socket
table in kernel/syscall.c that maps socket numbers to TCP handles and records the owning pid.

### 2.2 Main design decisions and the reasons given in comments

* **Interrupt handler only copies; a task runs the stack.** Drivers call `net_receive`, which copies the frame into a
  64-slot ring and returns (net.c:407-425). The stack itself runs from `net_poll` callers. Reason (net.c:333-365): the
  stack used to run inside the NIC interrupt; answering a frame needs `ip_send` -> `resolve_mac` -> polls the card ->
  delivers more frames -> answers -> polls... unbounded recursion, ~5 KiB of stack per turn against a 32 KiB kernel
  stack; the seventh turn wrote into the heap and triple-faulted a VMware guest about ten seconds into boot.
* **Delivery never nests.** Only one task delivers at a time (`deliver_depth`/`deliver_owner`, net.c:396-397, 472-513).
  A send inside a delivery that must wait for ARP handles only the queued ARP frames (`arp_only`, net.c:444-464), because
  an address is the only thing such a send can be waiting for; this costs a few hundred bytes instead of another 5 KiB
  per level "on top of the sixteen the deepest task here already uses" (net.c:380-395). (The real selftest run reports
  ~16 KiB of the 32 KiB stack used at its deepest.)
* **A background task keeps the stack alive** ("net", net.c:523-544): ACKs for connections nobody is reading and ARP
  replies for our own address must go out even when no caller is blocked on the network. It polls every 10 ms; callers
  that are waiting spin on `net_poll` themselves.
* **Every wait is a loop around `net_poll()`** (net.c:515-517: "which is what makes a stack with no threads work at all").
  TCP waits also call `tcp_pump()` to drive retransmission of every connection (tcp.h:49-51, tcp.c:214-221).
* **Six TCP connections, all state per connection** (tcp.h:5-17, tcp.c:1-14): the stack was one connection in file-level
  variables; a page and its pictures fetched serially, two programs could not both be online. Six "because a page with a
  stylesheet and a handful of images is the case this is for, and because every one of them holds a receive buffer of
  sixty four kilobytes".
* **One segment in flight per connection** (tcp.c:15-18): slow on fat links, correct everywhere.
* **Sequence numbers are settled before a segment is sent, never adjusted after** (tcp.c:20-31, 169-173, 414-417,
  444-449): the receive side can run between the send and the next instruction (a server on the same host replies
  inside the call); a post-send increment made the connection "one byte ahead of itself for the rest of its life".
  webserver.py's `instant_args` (guestfwd, 10.0.2.100) exists only to reproduce this.
* **Only acknowledge what was stored; the window is the real free space** (tcp.c:139-145, 284-291).
* **A FIN counts only in order** (tcp.c:313-332): acting on an out-of-order FIN ended 200 KB bodies with a hole
  "about half the time".
* **Ports are walked and checked against live connections** (tcp.c:91-96, 378-395): clock-derived ports collided within
  one tick (a redirect closes and reopens in the same instant), and a wrapping counter alone can collide with a
  long-lived connection.
* **Probe order e1000 -> PCnet -> RTL8139; USB is a fallback, never chosen at boot** (netdev.c:1-14): e1000 is what
  VirtualBox and a recognising VMware present; PCnet is what VMware gives an unrecognised guest ("the one that decides
  whether this works on somebody's own laptop"); RTL8139 is QEMU's older default. A USB adapter cannot be chosen at boot
  because the USB bus has not been walked yet and may be plugged in later, so `current()` re-asks every time
  (netdev.c:25-30).
* **"No card" vs "a card nothing here drives"** (netdev.h:14-18, netdev.c:101-128, wifi.h:4-21): distinct messages because
  only one is fixed by plugging something in. The PCI walk is cached because a panel that redraws itself asks it
  repeatedly and "nothing appears on the PCI bus while the machine is running".
* **Wireless is reported, not driven** (wifi.h:4-21): most cards need a vendor firmware blob; Atheros is named as the
  exception "whose MAC is in hardware". There is no wireless driver at all.
* **Ethernet over USB (RNDIS) is the answer for laptops whose Wi-Fi needs firmware** (usbnet.h:4-19): a phone with
  tethering or an adapter; RNDIS is published and nothing has to be trusted from a binary.
* **DHCP is asked for automatically at boot, in a task, and never twice at once** (net.h:30-45, net.c:640-661,
  main.c:623-636): the panel button was the only asker and freshly booted machines sat with no address; the compositor
  must not freeze for a DHCP exchange; the boot ask and the button share one UDP "socket" and one xid, so a flag in
  net.c serialises them.
* **Retries for one-datagram protocols** (net.c:760-769): a resolver that sends once reports "does not exist" for a
  dropped packet; ARP retries for the same reason.
* **Kernel `fetch` defaults to http, the browser/terminal default to https** (http.c:1-7, http.h:14-16): `fetch` is a
  debugging tool usually pointed "at a machine on the same desk"; HTTP/1.0 + `Connection: close` so chunked encoding
  never has to be parsed (http.c:97-101).

### 2.3 How it fits into zelr

* Brought up in `kmain` after SMP and before input/USB (main.c:495-507); the "net" task is created just before the
  scheduler starts (main.c:611-617) and the boot DHCP task right after `init` (main.c:623-636, not in selftest mode).
* All kernel code runs under one big kernel lock; system calls enter through an interrupt gate and run with interrupts
  off; kernel tasks run with interrupts on and are preemptible (section 6). The network code's own `cli`/`sti` sections
  are written for that model, but the network *syscalls* wait by spinning, which interacts badly with it (section 10, D1/D2).
* Consumers: ring-3 sockets (syscall.c), the kernel shell, the desktop's network panel and icon (wm.c), `/sys/net` and
  `/sys/hardware` (sysfs.c), TLS (tls.c reads/writes through tcp.c), USB enumeration (usb.c attaches usbnet).

---

## 3. File-by-file detail

### 3.1 include/net.h (60)

| Line | Declaration | Notes |
|---|---|---|
| 4 | `#define ETH_ALEN 6` | |
| 6 | `typedef u32 ipv4_t;` | host byte order |
| 8 | `void net_init(void)` | allocates RX queue once, resets queue/ARP/addresses, copies MAC if a card is up |
| 9 | `void net_receive(const u8 *frame, u16 len)` | driver -> queue (ISR-safe copy) |
| 10 | `void net_poll(void)` | `netdev_poll(); net_deliver();` |
| 14 | `void net_start_service(void)` | creates the "net" task once |
| 19-21 | `net_rx_queued/net_rx_dropped/net_rx_deepest` | queue depth, frames dropped because the queue was full, high-water mark |
| 23-28 | `net_up, net_ip, net_gateway, net_netmask, net_dns, net_mac` | `net_up()` is **card up**, not "has an address" (net.c:836) |
| 33 | `bool net_dhcp(u32 timeout_ms)` | synchronous full exchange |
| 44-45 | `net_dhcp_start(void)`, `net_dhcp_busy(void)` | asynchronous ask in a task + busy flag |
| 49 | `int net_ping(ipv4_t dst, u32 timeout_ms)` | RTT ms or -1 |
| 51 | `bool net_udp_send(ipv4_t dst, u16 sport, u16 dport, const void*, u16)` | unicast only |
| 54 | `bool net_resolve(const char *host, ipv4_t *out, u32 timeout_ms)` | A record via DHCP-supplied DNS |
| 56-57 | `net_parse_ip`, `net_format_ip` | dotted quad <-> host order (format needs >= 16 bytes) |
| 59-60 | `net_rx_packets`, `net_tx_packets` | driver frame counters |

### 3.2 include/netdev.h (18) and kernel/netdev.c (138)

Types/state: `typedef enum { NIC_NONE, NIC_E1000, NIC_PCNET, NIC_RTL8139, NIC_USB } nic_t;` (netdev.c:22),
`static nic_t nic` (23). `current()` (27-30) returns `nic` if a PCI card was chosen, else `NIC_USB` when
`usbnet_present()`, else `NIC_NONE` -- so a USB adapter is used only when no PCI card was driven.

Functions:
* `netdev_init()` (32-38): `e1000_init()`, then `pcnet_init()`, then `rtl_init()`; first success wins; else `NIC_NONE`.
* `netdev_up()` (40-48): driver `*_up()` (a flag set at the end of init); `NIC_USB` always true.
* `netdev_send()` (50-58), `netdev_poll()` (60-68), `netdev_mac()` (70-79, a static zero MAC when none),
  `netdev_rx_count()`/`netdev_tx_count()` (81-99), `netdev_name()` (130-138: "e1000", "pcnet", "rtl8139",
  `usbnet_name()` = "usb ethernet", or "none").
* `netdev_undriven(u16 *vendor, u16 *device)` (101-128): only when `current() == NIC_NONE`; on first call walks PCI once
  for class 0x02 subclass 0x00 (Ethernet controller, any prog-if) with `pci_list_class(0x02, 0x00, &found, 1)` and caches
  `looked/present/saw_vendor/saw_device` statics. Wireless (subclass 0x80) is not considered here (that is wifi.c).
  Note: a supported card whose init failed (e.g. MMIO map failure) is reported as "no driver".

Adding a NIC means adding an enum value and a case to all eight switches.

### 3.3 include/netpriv.h (9)

`np_hs(u16)` host->network 16-bit, `np_hl(u32)` host->network 32-bit, `np_nl(u32)` network->host 32-bit (same swap),
`np_ip_send(ipv4_t dst, u8 proto, const void*, u16)` -> `ip_send`. Implemented at net.c:205-210. Used only by tcp.c.

### 3.4 kernel/e1000.c (299) -- Intel 8254x family

Identification: `VENDOR_INTEL 0x8086`; `SUPPORTED[] = { 0x100E, 0x1015, 0x1004, 0x100F, 0x10D3, 0x153A }` (25),
probed in array order with `pci_find` (first match wins). The header comment says "Intel 82540EM"; the list also has
82540EP-LOM (0x1015), 82543GC (0x1004), 82545EM (0x100F, VMware), 82574L (0x10D3, QEMU/VMware e1000e) and I217-LM (0x153A).

Registers (27-48): CTRL 0x0000, STATUS 0x0008 (defined, never read), EERD 0x0014, ICR 0x00C0, IMS 0x00D0, IMC 0x00D8,
RCTL 0x0100, TCTL 0x0400, TIPG 0x0410, RDBAL/RDBAH/RDLEN/RDH/RDT 0x2800-0x2818, TDBAL..TDT 0x3800-0x3818,
MTA 0x5200, RAL 0x5400, RAH 0x5404. Bits: CTRL_SLU (1<<6, "set link up"), CTRL_ASDE (1<<5); RCTL_EN, SBP (unused),
UPE, MPE, BAM (1<<15), SECRC (1<<26), SZ_2048 = 0; TCTL_EN, PSP, CT = 0x0F<<4, COLD = 0x40<<12; ICR_RXT0 (1<<7).

Rings (68-89): `RX_DESCS 32`, `TX_DESCS 16`, `BUF_SIZE 2048`. `rx_desc_t {u64 addr; u16 length; u16 checksum; u8 status;
u8 errors; u16 special;}` and `tx_desc_t {u64 addr; u16 length; u8 cso; u8 cmd; u8 status; u8 css; u16 special;}`,
both packed, 16 bytes. Rings are `volatile` because the card writes them (94-96).

State (91-104): `dev`, `mmio`, `rx_ring`, `tx_ring`, `rx_buf[32]`, `tx_buf[16]`, `rx_cur`, `tx_cur`, `mac[6]`, `up`,
`rx_count`, `tx_count`.

Functions:
* `alloc_aligned(bytes, align, &raw)` (115-122): `kmalloc(bytes+align)`, rounds up, zeroes; never freed.
* `read_mac()` (124-151): RAL/RAH if non-zero (the card latches the EEPROM address at reset); otherwise EERD words 0..2
  with `start = 1`, address `<< 8`, done bit 4, 1,000,000-spin timeout each (82540-style EERD layout).
* `rx_init()` (153-175): 32 descriptors, `kmalloc(2048)` buffers; RDBAL/RDBAH = ring (64-bit), RDLEN, RDH 0,
  RDT 31 ("the card owns everything up to here"); RCTL = EN | BAM | SECRC | 2048 | UPE | MPE (promiscuous unicast and
  multicast; no reason given).
* `tx_init()` (177-201): 16 descriptors with status DD=1 (free); TDH = TDT = 0; TCTL = EN | PSP | CT | COLD;
  TIPG 0x0060200A.
* `handle_rx()` (203-216): while `status & DD`: if `0 < len <= 2048` -> `rx_count++`, `net_receive(buf, len)` (CRC
  already stripped by SECRC); clear status; advance; `RDT = old` (hand the processed descriptor back; one descriptor is
  always held by software). EOP and `errors` are not checked.
* `e1000_isr()` (218-222): read ICR (clears); `RXT0` -> `handle_rx()`.
* `e1000_send(data, len)` (224-248): rejects `!up`, 0, >2048; copies into `tx_buf[tx_cur]` and zero-pads to 60 bytes
  (comment 227-230: never read past the caller's frame, or the previous packet goes on the wire); cmd = EOP | IFCS | RS;
  status 0; advance TDT; spins up to 5,000,000 reads for `status & 0x0F`; always returns true (does not check that the
  descriptor it overwrote was done).
* `e1000_poll()` (250-260): `cli`, `handle_rx()`, restore IF -- the ISR walks the same ring, so they must not overlap.
* `e1000_init()` (262-299): find; `pci_enable_bus_master` (I/O + memory + bus master bits, pci.c:217-223);
  `phys = bar0 & ~0xF` (32-bit BAR only; `pci_dev_t.bar0` is a u32, include/pci.h:22-27);
  `mmio = paging_map_device(phys, 0x20000)` (identity, uncached); IMC = all; CTRL |= SLU | ASDE (no device reset);
  clear 128 MTA words; read MAC; rings; `register_interrupt_handler(32 + dev.irq, e1000_isr)`; `pic_unmask(irq)` and
  cascade 2 if irq >= 8; IMS = RXT0; read ICR; `up = true`.

DMA: descriptor rings and buffers are kernel heap pointers used directly as bus addresses (heap is identity-mapped,
e1000.c:9-11); the e1000 takes 64-bit addresses so no 4 GiB check. Link state is never read (STATUS.LU unused):
"up" means "initialised".

### 3.5 kernel/pcnet.c (377) -- AMD Am79C970A (PCnet-PCI II)

Identification: `VENDOR_AMD 0x1022`, `DEVICE_PCNET 0x2000` (40-41). I/O BAR0 (`bar0 & ~3`).

Ports (46-50, 16-bit word I/O mode): APROM 0x00 (MAC PROM, first 16 bytes), RDP 0x10 (CSR data), RAP 0x12 (register
address), RESET 0x14 (reading resets), BDP 0x16 (BCR data). `csr_read/csr_write/bcr_read/bcr_write` (137-152) write the
register number to RAP then access RDP/BDP.

CSR0 bits (52-64): INIT 0x0001, STRT 0x0002, STOP 0x0004, TDMD 0x0008, INEA 0x0040, IDON 0x0100, TINT 0x0200,
RINT 0x0400, `CSR0_ACK 0x7F00` (all write-one-to-clear flags). Descriptor status (66-70): OWN 0x8000, ERR 0x4000,
STP 0x0200, ENP 0x0100.

Rings (72-96): `RX_LOG 6` (64 RX), `TX_LOG 4` (16 TX), `BUF_SIZE 2048`. Reason for 64 RX (72-75): the ring must hold a
whole advertised window, "Sixty four buffers of two kilobytes is a hundred and twenty eight, against the sixty four this
stack advertises". SWSTYLE 2 descriptors: `rx_desc_t {u32 base; u16 buf_length (negated, top 4 bits ones); u16 status;
u32 msg_length (low 12 bits: bytes incl. CRC); u32 reserved;}`, `tx_desc_t {u32 base; u16 length (negated); u16 status;
u32 misc; u32 reserved;}`. `init_block_t {u16 mode; u16 tlen_rlen; u8 padr[6]; u16 reserved; u8 ladr[8]; u32 rdra; u32 tdra;}`
(101-109). All `volatile` (114-121: store order is the protocol; without it, at -O2, the card "quietly stops keeping up").

Functions:
* `neg_len(len)` (156): `((0 - len) & 0x0FFF) | 0xF000` (22-25: getting it wrong truncates every frame silently).
* `alloc_aligned(bytes, align)` (160-167): refuses allocations ending above 4 GiB (32-bit DMA addresses).
* `handle_rx()` (169-193): while `!(status & OWN)`: accept only STP && ENP && !ERR && 4 < len <= 2048, deliver
  `len - 4` (drop CRC); always hand back: buf_length, msg_length = 0, then status = OWN last.
* `pcnet_isr()` (195-208): read CSR0; write back `(csr0 & ACK) | INEA` (199-205: INEA is an ordinary bit, so an ack
  without it disables interrupts forever); RINT -> `handle_rx()`.
* `pcnet_send()` (210-237): pads to 60 itself (the card could pad but then descriptor length and wire length differ);
  returns false if the slot is still OWNed (ring full); base, negated length, misc 0, then status OWN | STP | ENP;
  CSR0 = INEA | TDMD; spins up to 5,000,000 for OWN to drop.
* `pcnet_poll()` (239-268): `cli`; **reads and acks CSR0 before walking** (248-262: a poll that only reads descriptors
  never "talks to the card"; the card decides at frame arrival whether it can receive and must be made to look again  -- 
  without this it missed answers "reliably on the first connection after boot"); `handle_rx()`.
* `rings_init()` (270-293): all RX descriptors OWNed by the card; TX all free.
* `pcnet_init()` (295-377): find; bus master; MAC from APROM before reset (a card that will not reset can still be
  named); reset by reading RESET; ~1000-iteration delay; expect CSR0 == STOP (else "not one of these"); BCR20 low byte = 2
  (SWSTYLE 2); BCR2 |= 0x0002 (ASEL, auto-select media); **CSR3 = 0x5B00** (mask everything but RINT) and
  **CSR4 |= 0x0115** (328-342: an unmasked, never-cleared source holds the level-triggered line forever -- TX start and
  missed-frame-counter overflow both fire in normal use); rings; init block (mode 0 = own address + broadcast,
  `tlen_rlen = (TX_LOG << 12) | (RX_LOG << 4)`, PADR = MAC, LADRF = 0, RDRA/TDRA); CSR1/CSR2 = block address; CSR0 = INIT;
  wait up to 1,000,000 reads for IDON; register ISR on `32 + irq`, unmask irq and cascade; CSR0 = IDON | INEA | STRT.

### 3.6 kernel/rtl8139.c (178) -- Realtek RTL8139

Identification: `0x10EC:0x8139` (16-17). Registers (20-32): MAC 0x00, MAR 0x08, TSD0 0x10, TSAD0 0x20, RBSTART 0x30,
CMD 0x37, CAPR 0x38, CBR 0x3A, IMR 0x3C, ISR 0x3E, TCR 0x40, RCR 0x44, CONFIG1 0x52. CMD bits: RESET 0x10, RX_EN 0x08,
TX_EN 0x04, BUFE 0x01. ISR bits: ROK 0x0001, TOK 0x0004. `RX_BUF_SIZE 8192`, `RX_PAD (16 + 1500)`.

State (45-53): `dev`, `io_base`, `rx_buf`, `rx_offset`, `tx_buf[4]`, `tx_slot`, `mac`, `up`, counters.

Functions:
* `handle_rx()` (60-86): while `!(CMD & BUFE)`: header at `rx_offset % 8192` = {u16 status, u16 len (incl. CRC)};
  `len < 4 || len > 1522` -> treat as "out of step": `rx_offset = 0; CAPR = 0xFFF0` and return (comment says resetting
  the receiver is the only way back; the code does not reset the receiver); if status ROK deliver `len - 4`;
  `rx_offset = (rx_offset + len + 4 + 3) & ~3` mod 8192; `CAPR = rx_offset - 16` (hardware quirk).
* `rtl_isr()` (88-93): read ISR, write it back (ack all), ROK -> `handle_rx()`.
* `rtl_send()` (95-119): rejects > 1792; zero the 1792-byte slot, copy, pad length to 60; TSAD = buffer, TSD = length;
  spin up to 10,000,000 for bit 15 (`0x8000`) of TSD (that is TOK in the datasheet; the comment calls it OWN and calls
  bit 13 TOK -- swapped); advance slot `& 3`.
* `rtl_init()` (121-168): find; bus master; CONFIG1 = 0 (wake); CMD = RESET and spin for it to clear; RX buffer
  `kmalloc(8192 + 1516)`, refused if above 4 GiB (137-144; TX buffers are not checked); RBSTART; 4 TX buffers; IMR =
  ROK | TOK; RCR = 0x0F | (1 << 7) (accept-all-physical, physical match, multicast, broadcast, WRAP); CMD = RX_EN | TX_EN;
  MAC from IDR0-5; ISR on `32 + irq`; `pic_unmask(irq)` (no cascade unmask, unlike e1000/pcnet); `up = true`.
* `rtl_poll()` (170-178): `cli`; if not BUFE, `handle_rx()`.

### 3.7 include/usbnet.h (38) and kernel/usbnet.c (263) -- RNDIS over USB

API (usbnet.h): `usbnet_attach(u8 slot, u8 in_dci, u8 out_dci, u8 ctrl_iface)` called by the enumerator after both bulk
endpoints of a CDC data interface are open; `usbnet_detach(u8 slot)`; `usbnet_present/mac/name`; the NIC-shaped
`usbnet_send/poll/rx_count/tx_count` ("the same shape as every other card here").

Constants (usbnet.c:26-56): messages `MSG_PACKET 0x1`, `MSG_INITIALIZE 0x2` / `MSG_INIT_CMPLT 0x80000002`,
`MSG_QUERY 0x4` / `0x80000004`, `MSG_SET 0x5` / `0x80000005`; `STATUS_SUCCESS 0`; `OID_PERMANENT_ADDRESS 0x01010101`
(802.3 permanent address), `OID_PACKET_FILTER 0x0001010E`; filter DIRECTED 0x01 | MULTICAST 0x02 | BROADCAST 0x08
(40-42: without it "the adapter is open and silent"); `PACKET_HEADER 44`, `CONTROL_MAX 256`, `FRAME_MAX 1536`,
`TRANSFER_MAX 1580`; class requests `SEND_ENCAPSULATED 0x00`, `GET_ENCAPSULATED 0x01`; `CMD_MAX 64`.

State (58-77): `present`, `slot_used`, `ep_in`, `ep_out`, `comm_iface`, `mac`, `request_id`, counters; heap buffers
`tx`, `rx` (TRANSFER_MAX each), `cmd` (64), `answer` (256). The control buffers are on the heap on purpose (67-75, the
first README pitfall): the xHCI is handed the pointer as a physical address, and `xhci_control`/`xhci_bulk` refuse a
buffer whose `virt_to_phys` differs (xhci.c:731, 841); a stack buffer "produces a refused transfer rather than a wrong one".

Functions:
* `put32/get32` (83-89): little-endian by hand.
* `post()` (91-99): control OUT, bmRequestType 0x21, SEND_ENCAPSULATED, wIndex = comm interface.
* `collect()` (101-109): control IN, 0xA1, GET_ENCAPSULATED.
* `ask()` (113-123): post, then up to 20 collects, each preceded by zeroing the reply and followed by `sleep_ms(5)` on a
  mismatch; success when the first word equals the expected completion type. **The RequestId is not compared** (despite
  the file comment 5-9).
* `initialize()` (125-135): INITIALIZE (24 bytes: type, length, id, major 1, minor 0, MaxTransferSize 1580); status at +12.
* `query(oid, value, want_len)` (137-164): QUERY (28 bytes) with InformationBufferLength = 0 **and offset = 0**
  (142-146, the second README pitfall: naming an offset for an absent buffer points one past the message and a strict
  device stalls it); reply length at +16, offset at +20 counted from byte 8 (157-158); bounds-checked against 256.
* `set_filter(filter)` (166-178): SET (32 bytes), buffer length 4, offset 20 (again from byte 8), value at +28.
* `usbnet_attach()` (182-216): refuses if already `present` ("one is enough"); records endpoints; allocates buffers
  once; initialize -> query MAC -> set filter, each failure logged to the black box (`bb_log`); `present = true`.
* `usbnet_detach(slot)` (218-220): clears `present` if the slot matches. **No caller exists** (section 10, D5).
* `usbnet_send(frame, len)` (226-240): <= 1536; 44-byte PACKET header (type 1, total length, DataOffset 36 = 44 - 8,
  DataLength) + frame; `xhci_bulk(OUT)`; `tx_count++`.
* `usbnet_poll()` (242-260): one synchronous `xhci_bulk(IN, rx, 1580)`; needs >= 44 bytes and type PACKET; offset =
  DataOffset + 8, length = DataLength, both bounds-checked against what arrived; delivers exactly one frame per transfer
  (a device that packs several packets into one transfer loses all but the first). `xhci_bulk` blocks until the
  transfer completes or ~50,000 x (poll + 100 us) ~= 5 s (xhci.c:849-853) -- see D6.

USB side (usb.c): `CLASS_CDC 2` / `CLASS_CDC_DATA 0x0A` (55-56); the comm interface number is remembered when a class-2
interface goes past (453), the data interface is class 0x0A (454); both bulk endpoints are opened lowest DCI first, then
`usbnet_attach(slot, bulk_in, bulk_out, comm_iface)`, `nnets++`, and `net_init()` ("a new link, so the old address is
forgotten along with the arp cache", 535-538). Interfaces of class 0xE0 (wireless-controller RNDIS used by some phones)
are not recognised as the comm half (open question Q3).

### 3.8 kernel/net.c (843) -- Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, the queue

**Wire types** (33-72, all packed): `eth_t {dst[6], src[6], u16 type}` (14 B); `arp_t {htype, ptype, hlen, plen, oper,
sha[6], u32 spa, tha[6], u32 tpa}` (28 B); `ip_t {ver_ihl, tos, len, id, frag, ttl, proto, csum, src, dst}` (20 B);
`icmp_t {type, code, csum, id, seq}` (8 B); `udp_t {sport, dport, len, csum}` (8 B); `dhcp_t {op, htype, hlen, hops,
xid, secs, flags, ciaddr, yiaddr, siaddr, giaddr, chaddr[16], sname[64], file[128], cookie, options[312]}` (552 B).
Constants: `ETH_P_IP 0x0800`, `ETH_P_ARP 0x0806`, `IP_ICMP 1`, `IP_UDP 17` (TCP is the literal 6 at 330).
`hs()`/`hl()` (27-31) are byte swaps.

**Global state** (74-78, 176, 214-224): `my_mac[6]`, `my_ip`, `my_mask`, `my_gw`, `my_dns` (host order), `bound`,
`BCAST`, `ip_id` (starts 1), and the per-protocol "reply arrived" globals: `ping_got/ping_seq`; `dhcp_offer_got,
dhcp_ack_got, dhcp_offer_ip, dhcp_server_ip, dhcp_mask, dhcp_gw, dhcp_dns, dhcp_xid`; `dns_got, dns_result, dns_id`.
Every protocol wait is "set a global, spin until a handler flips a volatile flag" -- one exchange of each kind at a time.

**ARP cache** (80-109): `ARP_N 16` entries `{ip, mac[6], used}`. `arp_store()` updates an existing entry, else takes a
free slot, else **overwrites slot 0**. `arp_get()` linear lookup. No aging, no expiry, no validation of the sender.

**Checksum** (111-120): RFC 1071 over big-endian 16-bit words with a seed; used for IP header and ICMP.

**Transmit**
* `txbuf[1600]` static (124). `eth_send(dst, type, payload, len)` (126-141): refuses > 1600; `cli` (if on), builds the
  header with `my_mac`, copies payload, `netdev_send()` (result ignored), restores IF. The static buffer plus `cli` make
  "filling the buffer and handing it over one step" (the comment's "See above" points at nothing).
* `arp_send(oper, target_mac, target_ip)` (143-154): request (1) is broadcast; reply (2) unicast.
* `resolve_mac(ip, out, timeout_ms)` (156-174): off-subnet destinations use the gateway when mask and gateway are
  known; cache hit returns immediately; else up to 4 requests, each followed by a 250 ms (`timer_hz()/4`) poll loop
  (`net_poll()` + cache check), bounded by the deadline (ip_send passes 2000 ms, so ~1 s is the real maximum).
* `ip_send(dst, proto, payload, len)` (178-203): size checked first (`20 + len <= 1500`); resolve MAC; header
  ver/ihl 0x45, tos 0, total length, `id = ip_id++`, frag 0 (DF clear), TTL 64, header checksum; `eth_send`.
  Sends are never fragmented; nothing larger than 1500 is accepted.

**Receive handlers**
* `handle_arp(p, len)` (226-235): stores the sender for **every** ARP frame; answers requests whose target is `my_ip`.
* `handle_icmp(ip, p, len)` (237-253): echo request (8) -> copy, type 0, recompute checksum, `ip_send` back to the source
  (inside the delivery, so ARP may be needed -- the `arp_only` case); echo reply (0) -> `ping_got` if `seq == ping_seq`
  (id and source not checked). Incoming ICMP checksums are not verified; other ICMP types are ignored.
* `dhcp_option(d, want, &len)` (255-268): walks `options[312]` skipping pads until 0xFF, bounds-checked against the
  struct (not against the received length).
* `handle_dhcp(p, len)` (270-296): needs >= 240 bytes and `xid == dhcp_xid` (raw compare; op, cookie and chaddr are not
  checked); option 53: OFFER (2) records yiaddr and server id (option 54, else siaddr); ACK (5) records yiaddr, mask (1),
  first router (3), first DNS (6). NAK (6) is ignored.
* `handle_udp(ip, p, len)` (300-309): dport 68 -> DHCP; **any dport >= 40000 -> DNS reply**; everything else dropped.
  UDP checksums are not verified.
* `handle_ip(p, len)` (311-331): version 4, `ihl >= 20`, `ihl <= total <= len` (Ethernet padding tolerated); accept
  filter (323): when bound, only `my_ip` or 255.255.255.255; while unbound, everything (so the DHCP reply is not thrown
  away). Header checksum not verified; `frag` ignored (no reassembly, fragments are parsed as whole datagrams);
  options skipped via ihl. Dispatch: ICMP, UDP, TCP (`tcp_input(hl(src), body, blen)`).

**The RX queue** (333-425): `RXQ_FRAMES 64`, `RXQ_FRAME_MAX 1536`, `rxq_slot_t {u16 len; u8 data[1536];}` array
allocated once in `net_init` (~98 KB); `rxq_head` (producer), `rxq_tail` (consumer), `rxq_dropped`, `rxq_deepest`.
`net_receive()` (409-425): rejects no queue, `len < 14` or `> 1536`; full queue -> `rxq_dropped++` (newest frame is
dropped); copy, `__sync_synchronize()`, publish head; track high-water mark. Called from ISRs and polls.

**Delivery** (427-513)
* `deliver(frame, len)` (432-442): EtherType dispatch to ARP/IP.
* `arp_only()` (450-464): one pass from tail to head, handles ARP frames in place and zeroes their `len` (the ring cannot
  remove from the middle; the drain skips empty slots; the producer never touches slots between tail and head).
* `net_deliver()` (472-513): with interrupts off, take ownership if `deliver_depth == 0` (`deliver_owner = task_current()`,
  depth 1). Not the owner: the owner re-entering (a send inside a delivery) gets `arp_only()`; any other task returns
  immediately ("it will get its turn"). Owner: up to 64 frames, each copied to a 1536-byte stack buffer before the slot is
  released, then `deliver()`; zero-length slots (taken by `arp_only`) are skipped; release ownership.
* `net_poll()` (518-521) = `netdev_poll(); net_deliver();`.
* `net_task()` (532-537): forever `net_poll(); task_sleep(10);`. `net_start_service()` (539-544) creates task "net" once.

**UDP** (547-582): `udp_send_to(dst, sport, dport, data, len, broadcast)`: staging `pkt[1400]` (so payload <= 1392);
checksum 0 ("optional in IPv4, left off"); broadcast builds its own IP header (src `my_ip`, possibly 0) and sends to
ff:ff:ff:ff:ff:ff without ARP; unicast goes through `ip_send`. `net_udp_send()` = unicast.

**DHCP** (584-661)
* `dhcp_build(d, type, req_ip, server)` (586-608): op 1, htype 1, hlen 6, xid, flags 0x8000 (broadcast reply), chaddr
  = MAC, cookie 0x63825363; options 53 (type), 50 (requested IP) if given, 54 (server id) if given, 55 = {1, 3, 6}, 255.
* `net_dhcp(timeout)` (610-638): requires a card; `xid = 0x5A4C5200 ^ ticks` ("ZLR\0"); clears flags and learned values
  **and sets `my_ip = 0, bound = false` immediately**; broadcast DISCOVER (68 -> 67); spin until OFFER or deadline;
  broadcast REQUEST (50 = offered IP, 54 = server); spin until ACK or the **same** deadline; bind `my_ip`, mask
  (default 255.255.255.0), gateway, DNS. No retransmission of either message, no lease time, no renewal, no DECLINE/RELEASE.
* `dhcp_asking` (647), `net_dhcp_busy()` (649), `dhcp_task()` (651-655: `net_dhcp(6000)`, clear flag, `task_exit()`),
  `net_dhcp_start()` (657-661: no-op if busy or no card; sets the flag and creates task "dhcp"; clears it if creation fails).

**Ping** `net_ping(dst, timeout)` (665-689): needs card and address; static seq++; 64-byte echo (id 0x4E59 = "NY", a
leftover of the nyx name; payload bytes = index); RTT = elapsed ticks x 1000 / hz (10 ms resolution at 100 Hz).

**DNS** (691-788)
* `handle_dns_reply(p, len)` (693-724): id must equal `dns_id`; ANCOUNT 0 -> done with result 0; skip questions (label
  walk + 4); walk answers: a name that starts with a compression pointer is skipped as 2 bytes, otherwise labels are
  walked (a label run ending in a pointer is mis-walked); first TYPE A (1) with RDLENGTH 4 wins; CNAMEs are stepped
  over; class, RCODE and TC are not checked.
* `net_resolve(host, out, timeout)` (726-788): needs card and `my_dns`; `dns_id = next_id++` (static, starts 0x1234);
  query in `q[512]`: header (RD set, QDCOUNT 1), labels (empty or > 63-byte labels refused), QTYPE A, QCLASS IN;
  source port `40000 + (ticks & 0x3FF)`; up to 4 sends, same id, gap 750 ms doubling (0.75, 1.5, 3, 6 s), clipped to the
  deadline. **No cache.** The server is the first option-6 address from the DHCP ACK.

**Helpers** (790-818): `net_parse_ip` (four decimal parts <= 255, exactly three dots; returns 0 on error, so
"0.0.0.0" is indistinguishable from failure; digit accumulation can wrap a u32); `net_format_ip`.

**Init/accessors** (820-843): `net_init()` allocates the queue once, zeroes head/tail/counters and delivery ownership,
clears the ARP cache and all addresses, copies the MAC if `netdev_up()`. `net_up()` = `netdev_up()`.

### 3.9 include/tcp.h (53) and kernel/tcp.c (557)

API (tcp.h): `TCP_MAX 6` (17); `int tcp_open(ipv4_t, u16 port, u32 timeout_ms)` (22, handle or -1);
`bool tcp_send(int, const void*, u16)` (24); `u32 tcp_recv(int, u8*, u32 cap, u32 timeout_ms)` (29, consumes);
`bool tcp_ended(int)` (33); `int tcp_state_code(int)` (34); `void tcp_close(int)` (36); `bool tcp_connected(int)` (37);
`tcp_resets/tcp_out_of_order/tcp_retransmits` (42-44, totals since boot); `tcp_open_count` (47); `tcp_pump` (51);
`tcp_input(ipv4_t src, const u8*, u16)` (53).

Constants (tcp.c:41-54): flags FIN 0x01, SYN 0x02, RST 0x04, PSH 0x08, ACK 0x10; `RTO_MIN_MS 400`, `RTO_MAX_MS 4000`,
`MAX_RETRIES 6`; `RXCAP 65536`.

Types: `tcp_t` 20-byte header, packed (56-61). `tstate_t { T_CLOSED, T_SYNSENT, T_OPEN, T_CLOSING, T_DONE }` (63;
`tcp_state_code` returns these as 0..4). `tcpc` (65-87):

| Field | Meaning |
|---|---|
| `bool used` | slot owned by a handle |
| `volatile tstate_t state` | |
| `ipv4_t peer_ip; u16 peer_port, local_port` | 3 of the 4-tuple (local IP is always `net_ip()`) |
| `volatile u32 snd_nxt, snd_una, rcv_nxt` | sequence state |
| `volatile bool got_fin` | peer's FIN accepted (in order) |
| `u8 rt_data[1400]; u16 rt_len; u32 rt_seq; u8 rt_flags` | the one segment in flight, for retransmission |
| `bool rt_pending; u64 rt_sent_at; u32 rt_timeout_ms; int rt_tries` | retransmission timer |
| `u8 *rxbuf` | 64 KiB receive buffer, allocated on first use of the slot and kept |
| `volatile u32 rxlen` | bytes buffered |

Globals: `conns[TCP_MAX]` (89), `next_port` (96), `rst_seen, ooo_seen, rt_total` (101).

Functions:
* `slot(h)` (117-121): range + `used` check (no generation counter).
* `tcp_checksum(src, dst, seg, len)` (123-137): pseudo-header + segment. Used only for sending.
* `window_now(c)` (142-145): `min(65535, RXCAP - rxlen)`.
* `emit(c, seq, flags, data, dlen)` (148-167): builds a 20-byte header (data offset 5, **no options -- no MSS**), ack =
  `rcv_nxt`, window, checksum; `np_ip_send(peer, 6, ...)`. Refuses `20 + dlen > 1500`.
* `send_reliable(...)` (174-187): refuses dlen > 1400; records the segment, `rt_tries = 0`, `rt_timeout_ms = 400`,
  timestamp; `emit`.
* `send_ack(c)` (191): bare ACK at `snd_nxt`, never retransmitted.
* `pump_one(c)` (193-212): if pending and elapsed >= timeout: after 6 retransmissions -> `T_DONE`; else retransmit, count
  `rt_total`, double timeout (cap 4000 ms).
* `tcp_pump()` (218-221): every used connection.
* `ack_arrived(c, ack)` (223-231): signed-difference advance of `snd_una`; clears `rt_pending` when the ack covers
  `rt_seq + rt_len` (+1 for SYN/FIN).
* `demux(src, t)` (240-251): first used, non-CLOSED connection with matching local port, peer IP and peer port.
* `tcp_input(src, p, len)` (253-346): no match -> dropped silently (no RST is ever generated); header length check;
  **RST -> `rst_seen++`, `T_DONE`, pending cleared** (no sequence check); in `T_SYNSENT` only SYN+ACK matters:
  `rcv_nxt = seq + 1`, `snd_nxt = snd_una = peer's ack` (not validated against ISS+1), `T_OPEN`, ACK; otherwise ACK
  processing; data: in-order (`seq == rcv_nxt`) -> copy what fits under `cli`, advance `rcv_nxt` by what was stored,
  ACK; anything else -> `ooo_seen++`, duplicate ACK (no reassembly queue); FIN only if `seq + dlen == rcv_nxt` and not
  already seen: `rcv_nxt++`, `got_fin`, ACK; if `T_OPEN` -> immediately send our FIN|ACK reliably and go `T_CLOSING`;
  else (we had already closed) -> `T_DONE`. The peer's advertised window and options are ignored. Checksums are not verified.
* `take_slot()` (354-366): first unused slot, allocating its 64 KiB buffer once.
* `tcp_open(ip, port, timeout)` (368-438): seed `next_port = 45000 + (ticks & 0xFFF)` once; take a slot; choose a local
  port by walking `next_port` in [45000, 61000) past ports used by live connections; **ISN = 0x5A4C5200 ^ (ticks x
  2654435761) ^ (h x 0x9E3779B9)**; reset state; `snd_nxt = iss + 1` **before** sending SYN; spin `net_poll(); tcp_pump();`
  until not SYNSENT or the deadline; failure frees the slot and returns -1.
* `tcp_send(h, data, len)` (440-459): only in `T_OPEN`; settle `snd_nxt += len` first; PSH|ACK reliably; wait up to
  8 s for the ACK (`!rt_pending`), returning false on timeout (the segment stays pending; the next send overwrites it).
* `tcp_recv(h, out, cap, timeout)` (472-514): one `net_poll()` always (478-487: on fast transfers the loop body never
  ran, leaving only the 10 ms task to move frames); spin while empty until data, FIN, DONE/CLOSED or deadline; take up
  to `cap` from the front and `memmove` the rest under `cli`; send a window-update ACK after any read in `T_OPEN`.
* `tcp_ended(h)` (518-522): buffer empty and (FIN or DONE or CLOSED); invalid handle -> true.
* `tcp_close(h)` (524-547): if `T_OPEN`, FIN|ACK reliably, `T_CLOSING`, wait up to 2 s for state to change (peer FIN,
  RST, or retries exhausted); then CLOSED, `used = false`, `rxlen = 0` (buffer kept). No TIME_WAIT.
* `tcp_connected` (549-552), `tcp_state_code` (554-557), `tcp_open_count` (107-112): no callers outside tcp.c.

### 3.10 include/http.h (16) and kernel/http.c (172)

Errors: `HTTP_ERR_RESOLVE -1`, `CONNECT -2`, `SEND -3`, `MEMORY -4`, `EMPTY -5`, `TOOLONG -6`, `TLS -7`.
`int http_get(const char *host, const char *path, const char *save_as)` returns the status code or an error.

* `append()` (24-30): all-or-nothing append (half a request line is worse than none).
* `split_port(in, host, cap, fallback)` (35-48): "name[:port]", port 1..65535 else fallback.
* `done(h, secure)` (52-55): `tls_close` before `tcp_close` (close_notify needs the connection).
* `starts_fold()` (57-64): case-insensitive prefix.
* `http_get()` (66-172): strip `https://` (secure, default 443) or `http://` (default 80); host <= 127 chars; dotted
  address or `net_resolve(host, 5000)`; prints "connecting to NAME (A.B.C.D) port N"; `tcp_open(ip, port, 6000)`; if
  secure, `tls_connect(h, host)` (certificate checked against the name), on failure print `tls: <reason>`, `tcp_close`,
  return `HTTP_ERR_TLS`, else print `secure: <suite>`; request in `req[512]`:
  `GET <path> HTTP/1.0\r\nHost: <spec as typed incl. :port>\r\nUser-Agent: zelr/0.37.0\r\nConnection: close\r\n\r\n`;
  send (`tls_send` or `tcp_send` -- plain requests > 1400 bytes cannot occur since req is 512); `kmalloc(256 KiB)`;
  read until the peer ends or a 10 s read returns nothing (`BODY_CAP` reached = silent truncation); `done()`;
  split headers at the first CRLFCRLF (none -> everything is "body"); status from bytes 9-11 when the reply starts with
  'H' (no digit validation); `fs_write(save_as, body)` when asked (**in-memory fs, see D3**); prints
  "status S, H bytes of headers, B bytes of body" (webcheck.py parses this line) and either "saved to X" or the first
  400 body bytes. No redirects, no chunked decoding, no gzip, no cookies -- those exist only in userland/fetch.h.

### 3.11 include/wifi.h (42) and kernel/wifi.c (64)

`wifi_state_t { WIFI_NONE, WIFI_NEEDS_BLOB, WIFI_DRIVABLE }` (wifi.h:23-28). `wifi_init()` (wifi.c:23-43): lists up to
4 PCI functions of class 0x02 subclass 0x80 ("other network controller", where wireless lands); picks the first Atheros
(`VENDOR_ATHEROS 0x168C`) if any, else the first; DRIVABLE iff vendor is 0x168C, else NEEDS_BLOB. `maker_of()` (10-21)
names 0x8086 intel, 0x168C atheros, 0x10EC realtek, 0x14E4 broadcom, 0x1814 ralink, 0x14C3 mediatek, 0x1969 qualcomm.
`wifi_describe()` (53-64): "no wireless card" / "wireless, drivable without firmware" / "wireless, needs vendor firmware".
Nothing drives any wireless card; kernel/wpa.c (WPA2-PSK key derivation) is called only by the selftest. main.c's
`net_survey()` (190-215) prints every ethernet and "other network" controller at boot with the same Atheros note.

### 3.12 include/tls.h (49) -- the interface used here

Sessions are indexed by the TCP handle (`static tls_t sessions[TCP_MAX]`, tls.c:105; out-of-range handles map to a
`nowhere` session so `tls_error(-1)` can still say why). Used by http.c and syscall.c: `tls_connect(int tcp, const char
*host)`, `tls_send(int, const void*, u32)` (splits into <= 1400-byte `tcp_send` calls, tls.c:163-172),
`tls_recv(int, u8*, u32, u32 timeout)` (reads records via `tcp_recv(..., 1000)`, tls.c:151-161), `tls_ended`,
`tls_close`, `tls_active`, `tls_any`, `tls_error(int)` (last error is also copied to `nowhere`), `tls_describe(int)`.

### 3.13 The ring-3 socket API (kernel/syscall.c:699-914; numbers in include/syscall.h and sdk/zelr.h)

`SOCK_MAX = TCP_MAX` (712); `sock_t {bool open; bool secure; u32 owner; int tcp;}` (714-719); `socks[6]`.
Sockets are **not** file descriptors: `SYS_POLL` (63) goes to `fd_poll` and knows nothing about them (syscall.c:355-374).

| Call | No. | Args (rbx, rcx, rdx) | Behaviour | Returns |
|---|---|---|---|---|
| SYS_CONNECT | 25 | host str (<=128), port | port 0 refused; `net_up()` (card only) else NET_ERR_DOWN; free socket else NET_ERR_BUSY; dotted or `net_resolve(6000)` else NET_ERR_RESOLVE; `tcp_open(6000)` else NET_ERR_CONNECT | socket 0..5 |
| SYS_TLS_CONNECT | 45 | host, port (0 -> 443) | as above, but refuses (NET_ERR_BUSY) if **any** secure socket is open; `tls_connect` failure closes TCP -> NET_ERR_TLS | socket |
| SYS_TLS_STATUS | 46 | buf, cap (1..256), which | TLS_WHY 0 -> `tls_error(-1)`, TLS_WHAT 1 -> `tls_describe(-1)` (machine-wide last) | length |
| SYS_SEND | 26 | sock, buf, len | owner-checked; len 1..1400 plain (one segment), 1..8192 TLS | len or -1 |
| SYS_RECV | 27 | sock, buf, len (1..65536) | 4000 ms `tcp_recv`/`tls_recv` | bytes; 0 = nothing in time; **-2 = NET_EOF**; -1 bad |
| SYS_DISCONNECT | 28 | sock | `tls_close` then `tcp_close` | 0/-1 |
| SYS_RESOLVE | 29 | host, u32* | `net_resolve(6000)`; writes host-order address | 0/-1 |
| SYS_NETINFO | 30 | `zelr_netinfo_t*` | `{u32 up; u32 ip, gateway, netmask, dns; u8 mac[6]; u16 pad}`; `up` = card present | 0/-1 |

Error codes (include/syscall.h:185-198): NET_ERR_DOWN -2, RESOLVE -3, CONNECT -4, TLS -5, BUSY -6 (NET_EOF is also -2,
in the recv context). Ownership: `sock_of()` checks `owner == caller_pid()` (735-740); `syscall_release(pid)` closes a
dead task's sockets from `task_exit_with` (sched.c:674). Forked children do not inherit usable sockets (owner is the
parent's pid). Users: userland/fetch.h (browser; keep-alive socket `ka_sock`, sends in 1400-byte pieces, reads 32 KiB at
a time, stops at Content-Length or the chunked terminator), userland/term.c `get` (HTTP/1.0; ports fixed to 443/80  -- 
a "host:port" argument is passed whole to `connect` and fails to resolve), userland/wiretest.c.

### 3.14 Other in-kernel consumers

* **Kernel shell** (shell.c): `net` (365-391: card name, MAC, address/netmask/gateway/dns or "address none, run: dhcp",
  "packets N in, M out"; with no driven card either "card VVVV:DDDD on the bus, no driver for it" or "no network card");
  `dhcp` (392-398: **calls `net_dhcp(6000)` directly**, prints "got A.B.C.D" or "no answer"); `ping ADDR` (399-411:
  needs an address, resolves names with 4000 ms, 4 pings of 2000 ms); `fetch [https://]HOST [PATH] [SAVEAS]` (412-425);
  `resolve NAME` (426-433, 4000 ms). The shell runs in the kernel task "init" (main.c:233-237), interrupts on.
* **Panel** (wm.c): icon (2858-2866) -- "three states, not two": `reachable = net_ip() != 0` -> wired glyph in
  `t->text`; `link = netdev_up()` without address -> wired glyph in `t->text_dim`; no link -> the signal glyph at level 0
  in `t->text_mute`. "Link" is "a driver is initialised" (or a USB adapter is present), never carrier. Popup
  `draw_net_panel` (2506-2594): card name / "VVVV:DDDD, no driver" / "no wired card"; "address ..." + "router ..." or
  "asking for an address"/"no address" + "nothing can be reached without one"; `wifi_describe()` and "maker VVVV:DDDD";
  button "ask for an address" / "asking..." -> `start_dhcp()` -> `net_dhcp_start()` (2601-2604).
* **sysfs**: `/sys/hardware` "network NAME MAC" (sysfs.c:171-177); `/sys/net` (194-213, node at 360): address, netmask,
  gateway, dns, "state configured/no address yet" (see D11), packets, "queue N waiting, D deepest, X dropped",
  "tcp R resent, O out of order, S reset".

### 3.15 tools/webserver.py (633) -- the host test server

`HOST_IP = "10.0.2.2"` (27: slirp's host address; any port there reaches the host loopback -- guestfwd was rejected
because it "carries one connection and then stops answering", 24-26); `INSTANT_IP = "10.0.2.100"` (31).
`Handler(BaseHTTPRequestHandler)` with `protocol_version = "HTTP/1.1"` (376); `setup()` counts connections (372-374);
`do_GET` counts requests and records each request's Cookie header (432-434); logging suppressed.

Endpoints (do_GET 438-560, do_POST 406-413):

| Path | Response | Used by |
|---|---|---|
| `/`, `/index.html` | PAGE (heading, list, link) | browser checks |
| `/gz` | GZIPPED html, gzip-encoded **only if** Accept-Encoding contains gzip (415-430) | wiretest (twice) |
| `/one`, `/two` | "marker-alpha" / "marker-beta" | wiretest three-at-once |
| `/setcookie` | sets `sid=abc123; Path=/` and `pref=dark; Path=/` | wiretest |
| `/whoami` | echoes the Cookie header (or "nothing") | wiretest |
| `/bye` | deletes sid (`Max-Age=0`) | wiretest |
| `/form`, `/posts`, `/said` (GET query or POST body, recorded in RECEIVED) | forms | formcheck |
| `/second`, `/styled`, `/bare`, `/style.css`, `/scripted`, `/unscripted`, `/live`, `/live.js`, `/live.txt`, `/live-quiet`, `/logo.svg`, `/drawn`, `/logo.png` (160x90 PNG), `/picture`, `/missing-picture` | browser fixtures | browser/live checks |
| `/size/N` | N bytes of numbered 16-byte lines (`filler`) | webcheck |
| `/big` | 200,000 bytes | **unused** |
| `/framed` vs `/measured` | SAMPLE chunked in 97-byte pieces vs Content-Length | browsercheck |
| `/chunked` | 50,000 bytes chunked in 4000-byte pieces | **unused** |
| `/close` | 30,000 bytes, no length, `Connection: close` | **unused** |
| `/redirect` (302 -> /second) | | browsercheck |
| `/redirect-relative` (301 -> "second"), `/loop` (302 -> itself) | | **unused** |
| `/slow` | Content-Length 2000, 4 x 500 bytes 0.4 s apart | webcheck |
| other | 404 | webcheck `/nothing-here` |

`Server` (563-624): `ThreadingHTTPServer(("127.0.0.1", 0))` (ephemeral port) run in a daemon thread; `counts()`,
`cookies()`, `reset_counts()`, `received()`, `forget()`; `host` = "10.0.2.2:PORT"; `qemu_args(model="e1000")` =
`["-nic", "user,model=<model>"]`; `instant_args()` = guestfwd `tcp:10.0.2.100:80-tcp:127.0.0.1:PORT` (one connection
only, replies "in the same virtual instant" -- the only arrangement that catches post-send sequence arithmetic).
`reachable(port)` (627-633).

### 3.16 tools/netcheck.py (264), tools/wirecheck.py (89), userland/wiretest.c (185)

Assertions are listed in section 8.

---

## 4. Control flow and lifecycles

### 4.1 Boot order (kernel/main.c)

1. `timer_init(100)` (459) -- interrupts stay off until `sched_start` (463-464, 583-588).
2. `sched_init()`, `smp_init()` (486-494).
3. `netdev_init()` -> e1000 / pcnet / rtl8139 init (each registers its ISR on vector `32 + irq`) -> `net_init()`; prints
   "net NAME MAC" or "net no card found" (495-505).
4. `wifi_init()`, `net_survey()` (506-507).
5. `usb_init()` (523) -- enumerates devices present at power-on, including RNDIS adapters -> `usbnet_attach()` ->
   `net_init()` again (usb.c:538). Runs with interrupts off (matters for D4).
6. IOAPIC routing of every IRQ < 16 that has a handler, all to the BSP (590-605; ioapic.c:147-151).
7. `syscall_init`, `winsrv_init`, `usb_start_service`, **`net_start_service()`** (608-617).
8. Not selftest: task "init" (kernel shell), then **`net_dhcp_start()`** (618-637). Selftest: task "selftest", which runs
   its own `net_dhcp(8000)`.
9. `sched_start()` enables interrupts (sched.c:603).

### 4.2 Receive path

```
 NIC DMA into its ring/buffer
   |-- IRQ (to BSP) -> e1000_isr / pcnet_isr / rtl_isr -> handle_rx() --+
   |-- netdev_poll() -> e1000_poll / pcnet_poll / rtl_poll (cli) -------+--> net_receive(frame,len)
 usbnet: netdev_poll() -> usbnet_poll() -> xhci_bulk(IN) (blocking) ---+       copy into rxq[head] (64 x 1536),
                                                                               drop newest if full
 net_poll() = netdev_poll(); net_deliver();
 net_deliver(): take ownership (one task) -> <= 64 frames: copy to stack, advance tail, deliver()
 deliver(): 0x0806 -> handle_arp()      0x0800 -> handle_ip() -> 1 handle_icmp / 17 handle_udp / 6 tcp_input
 handle_udp(): dport 68 -> handle_dhcp()   dport >= 40000 -> handle_dns_reply()
```

### 4.3 Transmit path

```
 tcp_open/send/close, send_ack, pump_one -> emit() -> np_ip_send() -> ip_send()
 net_udp_send / DNS -> udp_send_to(unicast) -> ip_send()
 DHCP -> udp_send_to(broadcast) -> hand-built IP header -> eth_send(ff:..:ff)
 net_ping / echo reply -> ip_send();   arp_send() -> eth_send()
 ip_send(): 20+len <= 1500 -> resolve_mac(dst or gateway, 2000) [may poll: arp_only if inside a delivery]
            -> header + checksum -> eth_send()
 eth_send(): cli -> static txbuf[1600] -> netdev_send() -> driver copies, pads to 60, spins for completion -> sti
```

### 4.4 Who runs the stack

The stack runs only inside `net_poll()`. Callers: the "net" task every 10 ms (net.c:532-537); `resolve_mac` (167);
`net_dhcp` (623, 629); `net_ping` (684); `net_resolve` (778); `tcp_open` (428), `tcp_send` (455), `tcp_recv` (488, 491),
`tcp_close` (535). ISRs only fill the queue. Nothing runs from the timer tick (unlike USB HID, which is polled from
`on_tick`, timer.c:23). `tcp_pump()` (retransmission) is called only from the four TCP wait loops, never from the net
task, so an outstanding segment is retransmitted only while somebody is blocked in tcp.c.

### 4.5 ARP

`ip_send` -> `resolve_mac`: gateway substitution for off-subnet; cache hit; else up to 4 broadcast requests, 250 ms
apart, polling in between; the reply is learned by `handle_arp` (every ARP frame's sender is stored, and requests for
`my_ip` are answered). Cache: 16 entries, no expiry, slot 0 evicted when full; cleared only by `net_init()`.

### 4.6 DHCP and its askers

```
 net_dhcp(T): card? -> xid = 0x5A4C5200 ^ ticks; my_ip = 0; bound = false
   DISCOVER (bcast, 53=1, 55={1,3,6}) --> wait OFFER (xid match)      [deadline T, no resend]
   REQUEST  (bcast, 53=3, 50=offer, 54=server) --> wait ACK            [same deadline, NAK ignored]
   ACK: my_ip = yiaddr, mask (default /24), gw = first router, dns = first DNS, bound = true
```

Askers: boot task (main.c:636 -> `net_dhcp_start` -> task "dhcp" -> `net_dhcp(6000)`), panel button (wm.c:2602 ->
`net_dhcp_start`), kernel shell `dhcp` (shell.c:395 -> `net_dhcp(6000)` **synchronously, ignoring the busy flag**),
selftest (selftest.c:438 -> `net_dhcp(8000)`; boot DHCP is suppressed in selftest mode). A USB adapter plugged in after
boot gets `net_init()` but **no** DHCP (usb.c:535-538); the user must press the button or type `dhcp`.

### 4.7 DNS

`net_resolve`: build query, pick port 40000-41023, send to `my_dns:53`, wait 0.75 s, resend with the same id, 1.5 s,
3 s, 6 s, bounded by the caller's timeout (shell 4000, http.c 5000, syscalls 6000, selftest 6000). The first A record
of a matching-id reply wins; ANCOUNT 0 means "no such name".

### 4.8 ICMP

Outbound: `net_ping` (shell `ping`, selftest "gateway answers icmp"). Inbound: echo requests to our address (or to the
broadcast address, or to anything while unbound) are answered from inside the delivery.

### 4.9 TCP

States: `T_CLOSED(0)`, `T_SYNSENT(1)`, `T_OPEN(2)`, `T_CLOSING(3)` (covers FIN-WAIT-1/2 and LAST-ACK), `T_DONE(4)`
(covers TIME-WAIT/CLOSED; no timer). No LISTEN, no SYN-RECEIVED, client only.

```
 tcp_open:  CLOSED --SYN(iss; snd_nxt=iss+1)--> SYNSENT --SYN+ACK--> OPEN (send ACK)
            SYNSENT --RST or deadline--> slot freed (CLOSED), return -1
 OPEN --peer FIN in order--> CLOSING (we ACK, then send FIN|ACK reliably at once)
 OPEN --tcp_close--> CLOSING (FIN|ACK reliably) --peer FIN--> DONE
 any demuxed state --RST--> DONE;  any with a pending segment --6 retransmissions--> DONE
 tcp_close: (wait <= 2 s if it sent the FIN) -> CLOSED, used = false
```

Retransmission timeline for one segment (if pumped and the clock runs): send at 0; resend at 0.4, 1.2, 2.8, 6.0, 10.0,
14.0 s; declared dead at 18.0 s. `tcp_open` with 6 s sends the SYN at 0, 0.4, 1.2, 2.8 s. `tcp_send` gives up waiting at
8 s. `tcp_close` resends the FIN at 0.4 and 1.2 s within its 2 s.

Receive: in-order bytes up to the free space are appended to the 64 KiB buffer; the ACK/window reflect exactly what was
stored; out-of-order and duplicate segments are dropped with a duplicate ACK; `tcp_recv` removes bytes from the front
and sends a window update. A peer that closes: its FIN (in order) sets `got_fin`, zelr answers with its own FIN at once;
`tcp_recv` then drains the buffer and `tcp_ended` becomes true; `sys_recv` returns -2 (NET_EOF).

### 4.10 Kernel `fetch`

`http_get` -> resolve (5 s) -> `tcp_open` (6 s) -> optional `tls_connect` -> one request -> read loop (10 s quiet
timeout, 256 KiB cap) -> `tls_close`/`tcp_close` -> parse -> print/save.

### 4.11 USB adapter lifecycle

Plug-in (boot or the "usb" task's 300 ms rescan, usb.c:696-717) -> `claim_interface` -> open bulk endpoints ->
`usbnet_attach` (INITIALIZE, QUERY MAC, SET FILTER) -> `present = true` -> `net_init()` (clears address/ARP/queue even
if a PCI card is the active NIC) -> used only if no PCI card was driven. Unplug: `forget_root` (usb.c:681-694) never calls
`usbnet_detach`, so the adapter stays "present" (D5).

---

## 5. Interfaces

### 5.1 Exports and their users

| Export | Users |
|---|---|
| `netdev_init` | main.c:496 |
| `netdev_up/send/poll/mac/rx_count/tx_count` | net.c; sysfs.c:171-174, 196; wm.c:2531, 2861 |
| `netdev_name` | main.c:500-501, shell.c:381, wm.c:2531/2537, sysfs.c:173 |
| `netdev_undriven` | shell.c:371, wm.c:2543, selftest.c:427 |
| `net_init` | main.c:497, usb.c:538 |
| `net_receive` | e1000.c:208, pcnet.c:181, rtl8139.c:79, usbnet.c:259 |
| `net_poll` | net.c, tcp.c (only) |
| `net_start_service` | main.c:617 |
| `net_dhcp` | shell.c:395, selftest.c:438, net.c:652 |
| `net_dhcp_start/busy` | main.c:636, wm.c:2565/2587-2590/2602 |
| `net_ping` | shell.c:408, selftest.c:443 |
| `net_resolve` | http.c:75, shell.c:404/429, syscall.c:758/793/895, selftest.c:445 |
| `net_parse_ip/format_ip` | http.c, shell.c, syscall.c, wm.c, sysfs.c |
| `net_up/ip/gateway/netmask/dns/mac` | shell.c, syscall.c:752/784/892/904-910, wm.c, sysfs.c, selftest.c, main.c:498, tcp.c:165 (`net_ip`) |
| `net_rx_queued/dropped/deepest` | sysfs.c:205-206 |
| `net_rx_packets/tx_packets` | shell.c:391, sysfs.c:204 |
| `net_udp_send` | net.c (DNS) only |
| `np_*` | tcp.c only |
| `tcp_open/send/recv/close/ended` | http.c, syscall.c, tls.c (send/recv/ended) |
| `tcp_input` | net.c:330 |
| `tcp_retransmits/out_of_order/resets` | sysfs.c:210-211 |
| `tcp_pump` | tcp.c only |
| `tcp_state_code/connected/open_count` | **none** |
| `http_get` | shell.c:417 |
| `wifi_*` | main.c:506, wm.c:2575-2580 |
| `usbnet_attach` | usb.c:529 |
| `usbnet_detach` | **none** |
| `usbnet_present/mac/name/send/poll/rx_count/tx_count` | netdev.c |
| `TCP_MAX` | syscall.c:712 (SOCK_MAX), tls.c:105/109/145, selftest.c:1766-1768 |

### 5.2 Dependencies

pci (`pci_find`, `pci_list_class`, `pci_enable_bus_master`; `pci_dev_t` has only `bar0` (u32) and `irq` = config 0x3C
low byte), idt (`register_interrupt_handler(u8, ...)`), pic (`pic_unmask`), ioapic (routing in main.c), paging
(`paging_map_device`, identity-mapped heap, `virt_to_phys` in xhci), heap (`kmalloc`, never freed by drivers), timer
(`timer_ticks`, `timer_hz` = 100, `sleep_ms` in usbnet), sched (`task_create`, `task_sleep`, `task_exit`,
`task_current`), xhci (`xhci_control`, `xhci_bulk`), blackbox (`bb_log` in usbnet), tls, fs (`fs_write` in http.c),
printf.

---

## 6. Concurrency, locking, memory ownership, invariants

**Execution model** (from outside this area, verified): one big kernel lock held by a processor whenever it is not in
ring 3 (include/sched.h:164-184, idt.c:147-188, 287). System calls come through an interrupt gate (`0xEE`, idt.c:62-63),
so "no timer lands in the middle of one" (sched.h:180-183); fd.c:192-212 shows the author's own fix for waiting inside a
syscall (`sti` around the wait). Kernel tasks start with `rflags = 0x202` (interrupts on, preemptible; sched.c:207) and
stay on the boot processor (README:1599-1600). All IOAPIC interrupts go to the BSP (ioapic.c:147-151), and the BSP must
take the kernel lock to handle IRQ0 (only the local-APIC timer vector uses try-lock, idt.c:179-187). The tick counter
advances only in the IRQ0 handler (timer.c:16-18, 52).

Consequences for this area:
* Two kernel code paths never run truly in parallel; interleaving happens only by preemption of kernel tasks
  (net task, dhcp task, kernel shell, usb task) or by an interrupt on the processor that holds the lock.
* The `cli` sections are the real mutual exclusion: `eth_send` (static `txbuf`), driver polls vs ISRs (ring cursors),
  `tcp_input` vs `tcp_recv` (rxbuf/rxlen, tcp.c:292-303, 496-508), `net_deliver`'s ownership test-and-set (475-484),
  `net_receive`'s publish order (`__sync_synchronize`, 418-421).
* A system call that waits on the network spins with interrupts off, holding the lock: ticks stop, TCP timers stop, and
  a delivery owned by a preempted kernel task can never complete (D1, D2).

**Single-flight globals** (one exchange at a time, no locking): DHCP state and xid, `dns_id/dns_got/dns_result`,
`ping_seq/ping_got`, `ip_id`, `next_port`, `take_slot` (check-then-set with a `kmalloc` in between, 354-366).

**Memory ownership**: all driver rings/buffers, the RX queue (~98 KB), usbnet buffers and each TCP slot's 64 KiB buffer
are allocated once and never freed; TCP buffers are kept per slot across connections (348-353). `http_get` allocates and
frees 256 KiB per fetch. Stack use: `net_deliver` frame 1536 + `handle_icmp` reply 1480 + `ip_send` pkt 1500 (or
`emit` seg 1500 + `ip_send` pkt 1500) per delivery path; broadcast UDP uses 1400 + 1500; the real selftest run peaked at
~16 KiB of the 32 KiB task stack (`TASK_STACK_SIZE 32768u`, include/sched.h:32).

**Invariants worth keeping**
* Nothing above `net_receive` runs in interrupt context.
* Only one task delivers at a time; a nested wait may only process ARP.
* The producer never touches slots between tail and head; `len` is written before `head` is published.
* A TCP segment's sequence number is fixed before it is sent (`snd_nxt` advanced first).
* The advertised window equals the free buffer space; only stored bytes are acknowledged; a FIN is accepted only in order.
* At most one reliable segment per connection (callers must wait, which `tcp_send` does).
* Local ports are unique among live connections.
* Driver descriptors are handed back with the ownership bit written last (volatile rings).
* Buffers handed to xHCI must be identity-mapped heap memory.
* DHCP exchanges must not overlap (enforced only for `net_dhcp_start` callers).

---

## 7. Limits and magic numbers

| Name / value | Where | Meaning |
|---|---|---|
| ARP_N 16 | net.c:82 | ARP entries; no aging; slot 0 evicted |
| 4 x 250 ms | net.c:163-172 | ARP attempts; ip_send deadline 2000 ms |
| txbuf 1600 | net.c:124 | Ethernet TX staging |
| 1500 | net.c:179 | max IP datagram; no fragmentation either way |
| TTL 64, frag 0, id from 1 | net.c:188-193, 176 | IPv4 header |
| 1400 | net.c:551 | UDP staging (payload <= 1392) |
| RXQ_FRAMES 64, RXQ_FRAME_MAX 1536 | net.c:366-367 | RX queue |
| 10 ms | net.c:535 | net task period |
| 0x5A4C5200 ("ZLR\0") | net.c:613, tcp.c:406 | DHCP xid and TCP ISN base |
| 6000 / 8000 ms | net.c:652, shell.c:395 / selftest.c:438 | DHCP exchange deadline |
| 255.255.255.0 | net.c:633 | default netmask |
| 552 bytes, options 312 | net.c:63-72 | DHCP message |
| 0x4E59 ("NY"), 64 bytes | net.c:673-676 | ping id and size |
| 0x1234.. | net.c:729 | DNS ids (sequential) |
| 40000 + (ticks & 0x3FF) | net.c:758 | DNS source port |
| dport >= 40000 | net.c:308 | "is a DNS reply" |
| 4 tries, 750 ms doubling | net.c:771-782 | DNS retries |
| 512, label <= 63 | net.c:734, 749 | DNS query buffer |
| TCP_MAX 6 | tcp.h:17 | connections (= SOCK_MAX) |
| RXCAP 65536 | tcp.c:54 | per-connection receive buffer |
| window min(65535, free) | tcp.c:142-145 | advertised window, no scaling |
| 1400 | tcp.c:76, syscall.c:840 | max segment payload / plain send |
| RTO 400..4000 ms, 6 retries | tcp.c:47-49 | ~18 s to give up |
| 45000..60999 | tcp.c:369, 397 | local ports, seeded 45000 + (ticks & 0xFFF) |
| 2654435761, 0x9E3779B9 | tcp.c:406 | ISN mixing |
| 8 s / 2 s | tcp.c:453 / 533 | send-ACK wait / close wait |
| 20-byte header, no options | tcp.c:157 | no MSS, no window scale, no SACK |
| e1000 32 RX / 16 TX x 2048 | e1000.c:68-70 | rings |
| 0x20000 | e1000.c:278 | e1000 MMIO window mapped |
| 0x0060200A | e1000.c:199 | TIPG |
| 1e6 / 5e6 spins | e1000.c:141, 243 | EERD / TX done |
| pcnet 64 RX / 16 TX x 2048 | pcnet.c:76-80 | rings (log2 6 / 4) |
| CSR3 0x5B00, CSR4 or 0x0115 | pcnet.c:341-342 | interrupt masks |
| BCR20 = 2, BCR2 or 0x0002 | pcnet.c:321, 326 | SWSTYLE 2, ASEL |
| rtl RX 8192 + 16 + 1500 | rtl8139.c:42-43 | RX ring with WRAP pad |
| rtl TX 4 x 1792 | rtl8139.c:49, 148 | TX slots |
| RCR 0x0F + 0x80 | rtl8139.c:158 | promiscuous + WRAP |
| 1e7 spins | rtl8139.c:111, 131 | TX / reset |
| 44 / 256 / 1536 / 1580 / 64 | usbnet.c:48-51, 79 | RNDIS header, control reply, frame, transfer, command |
| 20 x sleep_ms(5) | usbnet.c:116-121 | control reply retries |
| 50000 x (poll + 100 us) | xhci.c:851 | bulk transfer timeout (~5 s) |
| 32 TRBs | xhci.c:138 | transfer ring (31 usable, no full check) |
| BODY_CAP 256 KiB | http.c:19 | kernel fetch body |
| 512 / 128 | http.c:102, 71 | request / host buffers |
| 5000 / 6000 / 10000 ms | http.c:75, 81, 129 | resolve / connect / read-quiet |
| 1..8192 | syscall.c:840 | TLS send per call |
| 1..65536, 4000 ms | syscall.c:856, 860, 869 | recv size and timeout |
| 128 | syscall.c:748 | host string in connect/resolve |
| 4 | wifi.c:31 | wireless controllers examined |

---

## 8. Tests

**Kernel selftest `[network]`** (selftest.c:420-447; run list 3565): "a card that is driven is not also reported as
undriven", "an undriven card is named by its ids" (both before the skip), "card has a mac address", "dhcp obtained a
lease" (`net_dhcp(8000)`), then if an address: "address is not zero", "gateway was supplied", "resolver was supplied",
"gateway answers icmp" (`net_ping(gw, 3000)`), "dns resolves a name" (`net_resolve("example.com", 6000)`, needs the host
to resolve names through slirp). **Verified in the real log (selftest1.log:212-221): all 9 passed on rtl8139.** The run
also shows the "net" task existed (boot line 74 "sched 2 task(s)" = net + selftest) and IRQ routing through the IOAPIC
(line 71-72). The selftest runs in a kernel task, so its timeouts work (unlike syscalls, D1). `[tls 1.3]` test_tls
(1739-1769) checks invalid-handle behaviour for all `TCP_MAX` sessions (TLS area).

**tools/netcheck.py** (gate `nettest`): machine 1 (default QEMU NIC, e1000 on the pc machine): "an address arrives
without anything being asked for" (poll `net` every 2 s for 25 s, expect 10.0.2.15); "the icon is drawn on the panel";
"clicking it opens the network panel" (>4000 pixels near the overlay colour in the popup rectangle); "which is drawn
over what was there"; after pressing the button and closing the panel: "the address came from the dhcp server"
(10.0.2.15), "and a router to send everything else to" (`net` contains 10.0.2.2), "and pressing the button anyway does
not lose it". Machine 2 (`-nic none`): "a machine with no card does not invent one", icon drawn, panel opens and draws,
"a machine with no card gets no address by asking", "and its icon did not change", "and says something different in the
icon than one with a card". Machine 3 (q35, `-nic none`, qemu-xhci + usb-net): "a usb adapter is found and is the card"
("usb ethernet" in `net`), "and an address arrives over usb" (with a `dhcp` retry), "with frames counted going out as
well as coming in" (last "packets" line must not contain "0 in, 0 out").

**tools/wirecheck.py + userland/wiretest.c** (gate `wiretest`): `dhcp`, reset counts, `exec /bin/wiretest
http://10.0.2.2:PORT`. In-guest checks (through userland/fetch.h): "a page the server compressed comes back at all",
"and is the words rather than the packed bytes", "and the whole of them" (/gz); "a cookie the server set is sent back to
it", "and so is a second one from the same answer"; "one the server deleted is not sent again", "and deleting it left the
others alone"; raw sockets: "three connections open at once", "and they are three different sockets", "a request goes
out on each of them" (HTTP/1.1, Connection: close, paths /one, /two, /gz), "all three answers arrive" (round-robin
`recv`, up to 400 rounds), "and each one is the answer to its own request" (markers; the raw /gz has no Accept-Encoding so
it arrives plain). Server-side: "the browser's own fetching runs against a real server", "the server was asked for several
pages" (>= 5), "and did not have to accept a connection for each one" (`conns * 2 <= reqs`), "a request before anything
was set carried no cookie", "and one after it carried what was set".

**tools/webcheck.py** (gate `webtest`, kernel `fetch`): /size/1000 status 200 and length, again, /size/200000 three times
whole, /size/40000, /slow (2000 bytes), 404; the same on `model=pcnet` (1000 and 200000 bytes -- the ring-sizing check);
`model=ne2k_pci` reported as "10ec:8029" and not "no network card" (netdev_undriven); guestfwd instant server (/size/1000).

**tools/tlscheck.py** (kernel `fetch https://`, `model=rtl8139`, live internet): handshakes and statuses for real
sites, a non-TLS server on :80 refused with a `tls:` message, plain http still works.

**tools/abicheck.py**: `zelr_netinfo_t` vs `zelr_netinfo` layout and syscall numbers match between include/syscall.h
and sdk/zelr.h.

Browser harnesses (browsercheck, formcheck, livecheck, findcheck, shots) exercise the socket path indirectly.

**Not covered by any test**: TCP retransmission/timeout/RST paths, zero window, out-of-order handling (only incidentally),
DHCP loss/NAK, DNS loss (slirp never drops), inbound pings, ARP eviction, USB TCP traffic (netcheck's USB machine only
does DHCP), USB unplug/replug, `fetch ... SAVEAS`, pcnet beyond webcheck, rtl8139 outside tlscheck/selftest,
the unused webserver endpoints, and anything that depends on network syscalls timing out (slirp answers instantly, so
D1/D2 never show).

**QEMU user-networking assumptions**: none are hard-coded in the stack itself (no 10.0.2.x constants in kernel net code).
Tests and text assume guest 10.0.2.15, gateway/host 10.0.2.2 (netcheck, webserver `HOST_IP`, wiretest usage comment,
welcome.c:61 "ping 10.0.2.2"), guestfwd 10.0.2.100, a slirp DNS that can resolve example.com (selftest, tlscheck),
slirp's instant DHCP/TCP replies, slirp's default MSS of 1460 when no MSS option is offered (hides M1), and no ICMP
forwarding to the internet (README:1623-1627). run.sh uses rtl8139, zelr.bat uses e1000, the harness uses QEMU's default.

---

## 9. How to extend

* **New PCI NIC driver**: provide `*_init/up/send/poll/mac/rx_count/tx_count` with the existing signatures; add a
  `nic_t` value and a case to all eight switches in netdev.c; decide its place in the probe order. In the driver: allocate
  rings and buffers from the heap (identity-mapped; add a 4 GiB check for 32-bit DMA engines, like pcnet.c:160-167);
  keep descriptors `volatile` and write the ownership bit last; pad TX to 60 bytes by zeroing; call only `net_receive`
  from both the ISR and the poll, and wrap the poll's ring walk in `cli`/restore; register the ISR on `32 + irq`, unmask the
  IRQ **and the cascade** (IRQ 2) for irq >= 8; mask interrupt sources you do not acknowledge (pcnet.c:328-342). Routing
  is done later by main.c only for IRQs < 16 that have a handler.
* **New L4 protocol / UDP sockets**: dispatch in `handle_ip`/`handle_udp`; note that every dport >= 40000 is currently
  swallowed by the DNS handler; add a port table rather than more globals.
* **Server-side TCP**: needs LISTEN/SYN-RECEIVED states, a demux for unsolicited SYNs, and RST generation (none exists).
* **Performance**: send an MSS option in the SYN (`emit` always uses data offset 5); a multi-segment send window;
  delayed ACKs; a ring buffer instead of `memmove` in `tcp_recv`.
* **Robustness**: verify IP/TCP/UDP/ICMP checksums on receive (`tcp_checksum` already exists); validate the SYN-ACK's ack
  and RST sequence numbers; retransmit DHCP messages, handle NAK, keep the old address until a new lease is bound, honour
  lease times; add DNS source/port checks and a small cache; call `net_dhcp_start()` after a USB attach.
* **Waiting from syscalls**: follow fd.c:192-212 (enable interrupts only for the wait) or block on a wait queue and let the
  net task do the work -- this is the fix direction for D1 and D2.
* **Pitfalls the comments warn about**: sequence numbers after sending (tcp.c:20-31); acknowledging discarded bytes
  (284-291); out-of-order FIN (313-332); a window that is not the free space (139-145); port reuse inside one tick
  (378-395); running the stack in the ISR (net.c:333-365); nested delivery (380-395); PCnet INEA on ack (pcnet.c:199-205),
  poll must touch CSR0 (248-262), negated lengths (22-25), unmasked level-triggered sources (328-342); padding past the
  caller's frame (e1000.c:227-230); non-volatile descriptors (e1000.c:94-96, pcnet.c:114-121); xHCI buffers on the stack
  (usbnet.c:67-75); RNDIS offsets counted from byte 8 (157-158, 172) and zero offset for an absent buffer (142-146); the
  packet filter (40-42); DHCP overlap (net.h:35-43, main.c:631-635); expensive PCI walks (netdev.c:103-107); tick-based
  waits and `sleep_ms` before `sched_start` (xhci.c:278-289 -- usbnet.c currently violates it).

---

## 10. Doc drift and suspicious code

Severity: H = can hang or corrupt, M = wrong behaviour, L = minor. All items are from static reading unless noted.

**D1 (H) Network system calls wait with interrupts off, so their timeouts and TCP retransmission cannot work, and the
whole kernel stalls while they wait.** Chain: `int 0x80` is an interrupt gate (idt.c:62-63), so IF = 0 for the whole call
(sched.h:180-183; fd.c:192-212 is the author's explicit workaround for console reads, and nothing equivalent exists for
the network). `sys_connect/sys_connect_tls/sys_send/sys_recv/sys_disconnect/sys_resolve` (syscall.c:747-898) call
`net_resolve`, `tcp_open`, `tcp_send`, `tcp_recv`, `tcp_close`, `resolve_mac`, all of which loop on
`timer_ticks() < deadline` (net.c:166-171, 778; tcp.c:427, 454, 490, 534) and restore rather than enable interrupts.
`ticks` advances only in the IRQ0 handler (timer.c:16-18), IRQ0 goes to the BSP (ioapic.c:147-151), and the BSP must take
the kernel lock to run it (idt.c:166-188), which the spinning CPU holds. xhci.c:280-285 states the same fact for boot code
("timer_ticks never advances, so a timeout counted in ticks never expires"). Manifestations: `connect` to a silent
host, a lost DNS reply, a lost data segment or ACK (`tcp_pump` never fires), `recv` on a quiet connection, and
`disconnect`/process exit (`syscall_release` -> `tcp_close`'s 2 s wait) with a vanished peer all spin forever with the
lock held (desktop and other CPUs frozen). The documented 4 s `recv` timeout and fetch.h's "three quiet reads" logic
cannot trigger. Hidden in tests because slirp always answers. Kernel-task callers (shell `fetch`, selftest, dhcp task)
are unaffected.

**D2 (H) Delivery-ownership deadlock between a preempted kernel task and a syscall.** `net_deliver` turns away every
non-owner (net.c:486-489), assuming the owner will run again. Kernel tasks (net task, dhcp task, kernel shell) run with
interrupts on and can be preempted inside the delivery loop (ownership is held with interrupts on, 491-512). If a ring-3
program then makes a network syscall, its wait loop calls `net_poll`, gets turned away, and spins with IF = 0 (D1): the
owner can never be scheduled and the frame it waits for sits in the queue forever. Likely rare, fatal when it happens.

**D3 (H, user-visible) Kernel `fetch ... SAVEAS` saves where nothing can read it.** http.c:158 calls `fs_write(save_as,
...)`, the in-memory fallback filesystem (include/fs.h:4-9) that stores names verbatim (fs.c:22-34). The VFS resolves
paths to absolute form and uses FAT whenever a disk is mounted (vfs.c:230-254), and the kernel shell's `cat` goes through
the VFS (shell.c:145-156). So welcome.c:63-65 ("fetch example.com / page.html ... Then: cat page.html") cannot work, with
or without a disk. http.c:161 also prints "saved to" without checking `fs_write`'s result.

**D4 (M/H) usbnet can hang boot.** `ask()` calls `sleep_ms(5)` when a reply is not ready (usbnet.c:120). An adapter
present at power-on is attached from `usb_init()` in `kmain`, before interrupts are enabled (main.c:463-464, 523); `sleep_ms`
halts waiting for ticks (timer.c:76-82), which never come. xhci.c:278-289 warns about exactly this. QEMU's usb-net answers
the first GET_ENCAPSULATED_RESPONSE, so netcheck does not see it.

**D5 (M) USB adapter unplug is never handled.** `usbnet_detach` has no caller (only usbnet.c:218); `forget_root`
(usb.c:681-694) walks only the HID `devices[]` table. The adapter stays `present`, netdev keeps sending to a dead slot,
`nnets` never decreases, and a replug is refused ("one is enough", usbnet.c:183). Plugging in after boot also starts no
DHCP (usb.c:535-538).

**D6 (M) usbnet polling misuses a blocking bulk transfer.** `usbnet_poll` issues a synchronous `xhci_bulk(IN)` that waits
~5 s when nothing arrives (xhci.c:849-853) and returns -1 **without cancelling the TRB**; `ring_push` has no full check and
the ring has 31 usable TRBs (xhci.c:138, 255-271). On an idle link each poll leaves one more pending TRB, so after about 31
idle polls the producer laps the controller's dequeue pointer. Meanwhile the net task busy-spins in `xhci_bulk`; a
completion processed by the timer-tick `usb_poll()` while nobody waits is lost; two concurrent pollers (net task and a
waiting task) can both return the same completion and deliver the same `rx` buffer twice.

**D7 (M) `net_init()` on USB attach wipes a working PCI configuration.** usb.c:538 calls it unconditionally, clearing
address, gateway, DNS, ARP cache and RX queue even when a PCI NIC is active and the adapter will not be used
(netdev.c:27-30); it also resets `deliver_depth/owner` and queue indices while another task may be mid-delivery.

**D8 (M) DHCP is fragile.** One DISCOVER and one REQUEST, no retransmission within the deadline (net.c:619-630); NAK
ignored; no lease/renewal; `my_ip` is zeroed when an exchange starts (616), so a failed re-ask from the panel or shell
drops a working address and breaks open TCP connections; replies are accepted on xid alone (273).

**D9 (M) The shell's `dhcp` bypasses the overlap guard.** shell.c:395 calls `net_dhcp` directly, the case net.h:35-43 says
must go through `net_dhcp_start`. Harnesses type `dhcp` right after boot while the boot task may still be asking
(wirecheck.py:43, webcheck.py:53, netcheck.py:243 and others); harmless with slirp's instant answer.

**D10 (M) DNS is single-flight and spoofable.** One `dns_id/dns_got/dns_result` (net.c:222-224): a kernel task and a
syscall resolving at overlapping times can return each other's answer. Any UDP datagram to a port >= 40000 is treated as
a reply (308) and only the sequential 16-bit id is checked (695-696, 729-730); source address and port are ignored. No
cache; RCODE/TC ignored; a name that is labels followed by a compression pointer fails.

**D11 (M) `/sys/net` always says "state configured".** sysfs.c:196 returns early unless `netdev_up()`, and line 203 then
tests `net_up()`, which is the same function (net.c:836); the "no address yet" branch is dead. It should test `net_ip()`.

**D12 (M) `NET_ERR_DOWN` is documented as "no card, or no address on it"** (include/syscall.h:194) but only the card is
checked (syscall.c:752, 784); with no address a program gets NET_ERR_RESOLVE or NET_ERR_CONNECT. `SYS_NETINFO.up` is also
card presence.

**M1 (M/L) No MSS option.** Every SYN has a bare 20-byte header (tcp.c:157). RFC-conforming peers (Linux, for example)
then assume 536-byte segments, so a full 64 KiB window is about 122 segments, more than the 64-slot RX queue and the
64-descriptor PCnet ring whose sizing comment (pcnet.c:72-75) assumes large segments. slirp uses 1460, so tests never see it.

**D13 (L) No receive-side verification.** IP header, ICMP, UDP and TCP checksums are never checked (net.c:311-331,
tcp.c:253-346) although README:517 says "IPv4 with checksums"; IP fragments are not reassembled or even recognised.

**D14 (L) TCP hardening gaps.** RST accepted without a sequence check (tcp.c:266); SYN-ACK ack not validated (269-276);
predictable ISN and ports (406, 369); the peer's window is ignored; no RST is ever sent; a retransmitted SYN-ACK is not
re-acknowledged; a stale handle aliases a reused slot, contrary to the comment at 542-544 (no generation counter;
syscall and TLS owners make this mostly harmless). Possible check-then-set race in `take_slot` between a preempted kernel
shell and a syscall (354-366).

**D15 (L) rtl8139 details.** Comment swaps TSD bits (rtl8139.c:109-110: bit 15 is TOK, bit 13 is OWN; the code waits on
bit 15); "resetting the receiver" comment but the code only zeroes the software offset and CAPR (67-73) without
re-enabling RX, so the card and driver can stay out of step; no cascade unmask for irq >= 8 (163-164, unlike e1000.c:292
and pcnet.c:370; moot when the IOAPIC routes, as in the real log); TX buffers not checked against 4 GiB; the WRAP pad of
1500 + 16 (43) is smaller than Linux's 2048, so a maximum-size frame whose header lands at offset 8188 would be written
a few bytes past the allocation (possible, depends on the card's wrap behaviour).

**D16 (L) e1000 details.** Link status never read (`REG_STATUS` unused); unicast/multicast promiscuous (173), as is
rtl8139 (RCR accept-all-physical), which loads the RX queue on busy LANs; `e1000_send` does not check the descriptor it
reuses is done (224-248); the EERD fallback uses the 82540 layout (done bit 4, address << 8), which differs on 82541+,
82574 and I217 (only reached when RAL/RAH are empty); no device reset.

**D17 (L) wifi classification by vendor only.** Every 0x168C device is WIFI_DRIVABLE (wifi.c:42) and shown as
"wireless, drivable without firmware" (60), but Qualcomm Atheros ath10k-class parts under 0x168C (for example
168c:003e, QCA6174) need firmware, and no wireless driver of any kind exists. `maker_of(0x1969)` "qualcomm" is the
Atheros/Attansic Ethernet vendor id.

**D18 (L) ARP.** Learns from every ARP frame without validation (net.c:231; trivially poisoned), never expires entries,
always evicts slot 0 (98), and caches probes with sender 0.0.0.0.

**D19 (L) Small parsing issues.** `net_parse_ip` digit accumulation wraps (net.c:797; "4294967296.1.1.1" parses as
0.1.1.1); `net_resolve`'s final five bytes could pass `q[512]` if a name reached 511 bytes (749-756; unreachable with
128-byte host limits); `dhcp_option` walks the full 312-byte struct rather than the received length (255-268; reads stale
bytes from the stack frame buffer, not out of bounds); ICMP echo replies matched by sequence only (251); echo requests to
the broadcast address are answered; `eth_send` ignores `netdev_send` failure (PCnet returns false when its ring is full,
pcnet.c:219).

**Doc drift**
* README:519 "a single-connection TCP client" vs `TCP_MAX 6` (README:1602 is current).
* README:1606-1609, syscall.c:775-778 and sdk/zelr.h:439-442 say the kernel's TLS session state is single; tls.c:98-111
  and tls.h:4-15 now keep one session per TCP handle. The syscall layer still refuses a second secure socket
  (syscall.c:786-787), so the ring-3 behaviour is unchanged but the stated reason is stale; the kernel `fetch` path has no
  such limit.
* sdk/zelr.h:430 "Zero means the connection is open" -- `connect_tls` returns the socket number (0..5).
* include/syscall.h:36 ("One TCP connection at a time"), 83 ("The same one socket"), 198 ("the one connection is already
  in use"), 254 ("such as the one TCP socket") -- all predate six connections.
* README:1628 "USB stops at keyboards, mice, hubs and storage. No other class is claimed." -- CDC/RNDIS is claimed
  (usb.c:449-454, 515-539).
* README:393 describes netcheck as "click for an address"; it now mainly checks automatic DHCP and adds a USB machine.
  netcheck.py:9-11 says "Two machines" and gate.sh:479-480 "Run twice"; there are three.
* wirecheck.py:59-63 reasons about "five pages"; wiretest now makes eight requests (five through fetch.h, three raw), so
  the minimum is four connections and `conns * 2 <= reqs` passes only at 4 == 4 -- one server-side recycle of the
  keep-alive connection fails it.
* netcheck.py:250-252 "frames counted going out as well as coming in" only rejects lines containing "0 in, 0 out"
  ("5 in, 0 out" passes; an address arriving already implies frames went out).
* net.c:362-364 says the recursion "still exists ... bounded, counted"; the current design (380-395) never nests and
  `deliver_depth` is only ever 0 or 1. net.c:129 "See above." points at nothing.
* usbnet.c:5-9 says replies are matched to requests by id; `ask()` compares only the message type (118).
* net.h:35-43 says both askers go through `net_dhcp_start`; the shell does not (D9).
* rtl8139.c:109-110 and 67-73 (D15). e1000.c:1 names only the 82540EM.
* webserver.py serves `/big`, `/chunked`, `/close`, `/redirect-relative` and `/loop`, which no check requests.
* The kernel `fetch` (http.c) follows no redirects and decodes neither chunked nor gzip; those features, and the webserver
  fixtures for them, belong to userland/fetch.h. (fetch.h:525-528's own comment still says "identity" and "Close" while the
  code sends `Accept-Encoding: gzip` and `Connection: keep-alive` -- outside this area.)
* tcp.h:46-47 `tcp_open_count` "for anything that reports on the machine" has no caller; nor do `tcp_state_code` and
  `tcp_connected`.

---

## 11. Open questions

* Q1: Is the interrupts-off wait in network syscalls (D1) a known, accepted limitation? The author fixed the same problem
  for console reads (fd.c:192-212), and xhci.c:278-289 shows awareness of frozen ticks, so it looks unintended.
* Q2: Do 82543GC (0x1004), 82574L (0x10D3) and I217-LM (0x153A) actually work with this minimal init (no reset, 82540-style
  EERD, no PHY/ULP handling on the I217)? Only QEMU/VMware/VirtualBox models appear in comments and tests.
* Q3: Many phones present RNDIS as interface class 0xE0 subclass 1 protocol 3; usb.c:453 only remembers class-2 comm
  interfaces, so such a phone would fail `comm_iface == 0xFF` (usb.c:528). Untested on a real phone.
* Q4: On real hardware the PCI interrupt line can be 0xFF or above 15; `register_interrupt_handler(32 + irq)` takes a u8
  (idt.c:49), `pic_unmask` would shift by >= 8, and main.c routes only IRQs < 16. How should such machines be handled (MSI)?
* Q5: RNDIS devices that aggregate several packets per bulk transfer, or report a smaller MaxTransferSize in INIT_CMPLT,
  are not handled (only the first packet is delivered; the reply's fields are ignored). Do target phones do this?
* Q6: xHCI bulk buffers come from `kmalloc` without a 64 KiB-boundary check; real controllers require a TRB buffer not to
  cross one. Not seen in QEMU.
* Q7: How often does the RX queue drop frames on a busy physical LAN given e1000/rtl8139 promiscuity, the 64-slot queue and
  10 ms polling? `/sys/net` exposes "dropped" but no test looks at it.
* Q8: Is the rtl8139 WRAP overrun (D15) real on hardware or in QEMU's model (QEMU assumes about 1.5 KiB of pad)?
