# zelr atlas, area 05: devices (input, USB, sound, video, clipboard, pins)

Source: the repository root (github.com/blavese/zelr main, 2026-09-22, v0.37.0+2).
Method: every file listed in section 1 was read in full. Cross-references were
traced with grep into main.c, timer.c, idt.c, sched.c, smp.c, paging.c, heap.c,
blockdev.c, diskfs.c, usbnet.c, fd.c, shell.c, syscall.c, sysfs.c, theme.c,
wm.c, selftest.c, signal.c, uefi/loader.c and the README. Nothing was built or
run; every claim below comes from reading the code. "file:line" always means the
current tree. Where I say "verified" I read the code path end to end. Where I
say "suspected" or "likely" I am reasoning from the code plus an outside spec
(xHCI, USB, HDA, AK4531), and I say which one.

---------------------------------------------------------------------------

## 1. Scope

| File | Lines | Role |
|---|---:|---|
| include/ps2.h | 47 | 8042 controller API. One owner for the one configuration byte |
| kernel/ps2.c | 201 | 8042 bring-up, bounded port I/O, byte router (status AUX bit), timer drain that rescues missed IRQ edges |
| include/keyboard.h | 63 | `KEY_*` codes for keys that are not characters, `KEY_MOD_*` modifier stamp bits, keyboard API |
| kernel/keyboard.c | 217 | PS/2 set-1 decoder (the 8042 translates set 2), the shared 256-entry key ring, USB injection, serial merged in at read time, Ctrl-C raises SIGINT |
| include/mouse.h | 37 | Mouse API, `mouse_edge_t` |
| kernel/mouse.c | 291 | PS/2 mouse (3- or 4-byte packets, IntelliMouse knock). Pointer state shared by PS/2, Synaptics and USB. Button-edge queue, wheel counter, console-mode pointer sprite |
| include/synaptics.h | 66 | Synaptics API, plus the reasoning behind the detection |
| kernel/synaptics.c | 336 | Knock, identify (0x47) and mode byte. Absolute-to-relative decoder, two-finger scroll, tap-to-click |
| include/xhci.h | 144 | xHCI API: speeds, `usb_setup_t`, `xhci_where_t`, endpoint kinds |
| kernel/xhci.c | 869 | xHCI host controller: rings, contexts, commands, control/bulk/interrupt transfers. Event ring is polled, no interrupts |
| include/usb.h | 43 | USB layer API |
| kernel/usb.c | 768 | Enumeration, hubs (5 tiers), HID boot keyboard/mouse decode, MSC and RNDIS classification, hotplug service task |
| include/usbdisk.h | 34 | USB mass-storage API |
| kernel/usbdisk.c | 296 | Bulk-Only Transport + SCSI (TUR, REQUEST SENSE, INQUIRY, READ CAPACITY(10), READ(10), WRITE(10)). Registers a `blkdev_t` |
| include/hda.h | 38 | Intel HD Audio API |
| kernel/hda.c | 504 | HDA controller (CORB/RIRB rings), codec graph walk, one output stream on a 4-entry BDL |
| include/ens.h | 27 | Ensoniq AudioPCI API (same shape as hda.h) |
| kernel/ens.c | 473 | ES1370 / ES1371 (+CT5880) driver: sample-rate converter, AC'97 / AK4531 mixers, DAC2 looping playback |
| include/sound.h | 65 | Sound API above the controllers |
| kernel/sound.c | 391 | Picks a device, 64 KiB looping ring, write-ahead, silence behind the play position on each tick, software volume, tone generator, clock fallback when the position register never moves |
| include/fb.h | 60 | Framebuffer API, `RGB()` macro |
| kernel/fb.c | 530 | Bochs VBE (BGA) mode set, VMware SVGA hookup, adopting the UEFI GOP framebuffer, back buffer, mirror + band diff, half the bands on a second CPU |
| include/svga.h | 33 | VMware SVGA II API |
| kernel/svga.c | 209 | SVGA II mode set and FIFO `UPDATE` commands |
| include/vga.h | 17 | VGA text API + 16 colour enum |
| kernel/vga.c | 72 | 80x25 text console at 0xB8000 |
| include/fbcon.h | 9 | Framebuffer text console API |
| kernel/fbcon.c | 132 | Text console drawn into the framebuffer with the 8x16 font |
| include/clipboard.h | 30 | Clipboard API, `CLIP_MAX` |
| kernel/clipboard.c | 35 | One static 64 KiB text buffer |
| include/pins.h | 45 | Taskbar pins API |
| kernel/pins.c | 158 | `/zelr.pins` read/write. Orphaned: only selftest calls it |
| tools/usbcheck.py | 331 | QEMU: USB keyboard/mouse, the same behind a hub, keyboard hotplug, stick read/write |
| tools/inputcheck.py | 77 | QEMU: PS/2 typing still works after keys and mouse moves during boot |
| tools/soundcheck.py | 241 | QEMU intel-hda: record two beeps to WAV, measure pitch and silence |
| tools/enscheck.py | 111 | The same on QEMU ES1370 |
| tools/volcheck.py | 143 | Drag the desktop volume slider, compare the loudness of the recorded blips |
| tools/framecheck.py | 92 | `-smp 2`: drive the pointer, read `/sys/screen` counters |
| tools/tearcheck.py | 103 | `halfdrawn` program: no unfinished window frame is ever composited (a window-server check) |
| tools/clipcheck.py | 124 | Desktop terminal copy/paste via PS/2 keys, result read from `/sys/clipboard` |

Total: 7,462 lines (40 files).

---------------------------------------------------------------------------

## 2. Big picture

### 2.1 What this area is

Every device a person touches except storage controllers and network cards:
keyboards, pointers (PS/2 mouse, Synaptics pad, USB HID), the USB stack (which
also carries USB sticks and RNDIS network adapters), sound (HDA and Ensoniq),
the screen (framebuffer, VMware SVGA, VGA text, and a text console on the
framebuffer), and two small kernel services, the clipboard and the taskbar
pins file.

### 2.2 Input pipeline, end to end

```
 8042 ports 0x60/0x64
   IRQ1  (vector 33) -> keyboard.c:on_irq  --\
   IRQ12 (vector 44) -> mouse.c:mouse_isr  ---+--> ps2_poll()  (ps2.c:113)
   PIT tick          -> ps2_poll_from_timer -/      reads STATUS, then DATA;
                                                    STATUS bit 5 (AUX) picks the device
        AUX=0 -> keyboard_byte(sc)  -> push(c|mods) -> buf[256]   (+ signal_interrupt() if code==3)
        AUX=1 -> mouse_byte(b) -> syn_present()? syn_byte(b) : 3/4-byte packet
                                  -> mouse_inject(dx,dy,btns) / mouse_inject_scroll(n)

 xHCI event ring (never interrupts)
   PIT tick -> usb_poll() -> xhci_poll() -> drain_events() -> report_cb = usb.c:on_report
        keyboard report -> on_keyboard -> keyboard_inject(key|mods) -> buf[256]
                           and keyboard_set_mods(alt,ctrl,shift)
        mouse report    -> on_mouse    -> mouse_inject(dx,-dy,btns) / mouse_inject_scroll(-dz)

 COM1 (IRQ4, serial.c) keeps its own buffer. kbd_trygetchar() reads it only when buf[] is empty.

 Consumers
   wm.c:wm_run          one kbd_trygetchar() per loop pass; mouse_take_edge() loop;
                        mouse_x/y/buttons; mouse_take_scroll()
   fd.c:console_read    ring-3 reads of fd 0 (the console)
   fd.c:fd_ready_now    poll() on the console: kbd_has_char()
   shell.c:shell_task   the kernel shell
```

Design decisions and the reasons the comments give:

* **One owner for the 8042** (ps2.h:4-14). There is one controller and one
  configuration byte. Two drivers each setting their own bit is how the
  other driver's bit gets lost. The byte is **written whole, not
  read-modify-written** (ps2.c:164-173), because what the firmware leaves
  differs between cold and warm boots and between machines. The translate bit
  matters most: keyboard.c decodes set 1, and with translation off every key
  decodes wrong.
* **The missed-edge rescue** (ps2.c:1-19, timer.c:25-30). The 8042 raises its
  IRQ on the *edge* of a byte arriving. A byte that is already waiting when the
  line is unmasked means no edge ever comes again, and keyboard and mouse are
  dead until power-off. The fix: every byte is read through `ps2_poll`, which
  the timer also calls, so a missed edge costs 10 ms instead of the session.
  `inputcheck.py` is the regression test.
* **Routing by STATUS read before DATA** (ps2.c:127-130, keyboard.c:97-101,
  mouse.c:234-238). Which device sent a byte is known only from the status
  register *before* the data read, so ps2.c does all reading and sorting.
* **Modifiers are stamped at press time** (keyboard.h:22-32). The measured
  failure: on a fast chord every arrow key arrived with alt already released,
  so reading `kbd_alt()` later "worked only by luck". The WM reads the
  modifier off the key (wm.c:3821-3825).
* **One key ring for PS/2 and USB** (keyboard.h:44-50, usb.c:19-21). Nothing
  above the driver knows which keyboard was used.
* **Ctrl-C is raised where the key arrives** (keyboard.c:36-48), not where the
  console is read, because the program being interrupted is usually in a loop
  that is not reading. The key is also queued, so a shell can clear its
  half-typed line. *This only happens on the PS/2 path*; see section 10.
* **Mouse button edges are queued** (mouse.c:52-67). The WM samples once per
  composite pass, and a press and release inside one pass were invisible
  ("a swatch clicked three times running that never changed the colour").
  Each edge carries the pointer position at the moment it happened.
* **Synaptics decoding happens in software after an explicit knock**
  (synaptics.h:4-25). There is no "are you a trackpad" command. The argument
  is smuggled two bits at a time through four set-resolution commands. The
  decoder is written to be testable without hardware
  (`syn_answer_is_pad`, `syn_reset_state`).

### 2.3 USB

* **xHCI only**, because it is what every machine from the last decade has
  (xhci.h:4-18). **Nothing uses an interrupt** (xhci.c:22-27): an interrupt
  would need MSI or a shared PCI line, while a keyboard reports at most every
  8 ms, so draining the event ring on the 100 Hz tick is enough and leaves one
  consumer and no locks.
* **Everything is synchronous and spin-waited** (xhci.c:279-294): enumeration
  first runs from `main` before interrupts are ever on, so `sleep_ms` and
  tick-based timeouts would hang the boot. Waits use `io_wait` (a write to port
  0x80, about 1 us).
* **Memory handed to the controller must be identity mapped.** Heap memory is,
  and xhci.c checks with `virt_to_phys` rather than trusting it
  (xhci.c:28-30, 480-484). **xHCI memory is never freed**: `alloc_aligned`
  returns a pointer into a larger kmalloc block that kfree cannot take.
  Rings and contexts are therefore kept per slot and reused
  (xhci.c:226-231, 629-631).
* **Boot protocol only** (usb.h:14-18): 8-byte keyboard reports, 3-4 byte
  mouse reports, no report-descriptor parser.
* **Hubs are walked** to 5 tiers (usb.c:326-339, 565-568), because a laptop's
  built-in keyboard is often behind a chipset hub.
* **Hotplug covers root ports only** (usb.c:663-676). A task wakes every
  300 ms and rescans if a Port Status Change event set a flag.
* **Mass storage is BOT/SCSI, one stick at a time** (usbdisk.c:257-258). It is
  registered with the block layer as a removable `blkdev_t` and mounted by
  `diskfs_mount_removable` at `/usb`.
* A CDC-data interface with bulk endpoints goes to **usbnet (RNDIS)**
  (usb.c:449-540). That driver belongs to the network area.

### 2.4 Sound

* Two controllers behind one four-call shape: `start(buffer, bytes, rate)`,
  `position()`, `rate()`, `channels()`, `frame_bytes()` (ens.h:11-13,
  sound.c:33-66). The hardware **reads one buffer in a loop forever**.
  Sound means writing ahead of the play position. Silence means zeroes.
  There is no start or stop per sound.
* **HDA first, Ensoniq second** (sound.c:363-367). VMware gives an unrecognised
  guest an ES1371, so without ens.c "the most likely way anybody runs zelr has
  a sound controller and no sound" (ens.h:6-9).
* **Silence is put back on every tick** (sound.c:1-14, 213-247). Otherwise a
  sound written once would repeat on every loop of the buffer.
* **Clock fallback** (sound.c:124-150): VMware's Ensoniq never updates its
  position register. If the position has not moved 0.25 s after start, the
  play position is derived from ticks times rate. `sound_clocked()` reports
  this and `/sys/devices` shows it.
* **Volume is software**, applied as samples are copied into the ring
  (sound.c:81-101). The default is 70 %: "a machine that comes up at full volume
  is a machine somebody turns down once and resents twice".

### 2.5 Video

* **Three ways to get a framebuffer** (fb.h:22-26, `fb_backend()`):
  1. Adopt the one the UEFI loader set up through GOP (`fb_adopt`, main.c:392-393).
  2. Bochs VBE dispatch ports 0x1CE/0x1CF on QEMU std-vga, VirtualBox or
     Bochs (`bga_mode`). "VBE" in this code always means the Bochs dispatch
     interface, never VESA BIOS calls.
  3. VMware SVGA II (`init_svga`). It is tried only when BGA is absent.

  With none of these (for example a real BIOS machine), the console stays in
  VGA text mode. Both BIOS paths leave `fb_base = 0` on purpose
  (main.c:294-296, bootloader/cdboot.S:657-658).
* **Back buffer in RAM** (fb.c:8-10): reading VRAM over PCI is too slow to
  composite in.
* **Band diff against a mirror** (fb.c:318-346): the mirror (`sent`) holds what
  the card was last given, compared 16 rows at a time. Only bands that differ
  are copied. Half the bands go to another CPU when `band_count >= 8` and
  `smp_helper()` finds an idle AP. The comment calls this "the first thing in
  this kernel a second processor actually does". Motivation: the pointer was
  costing a full 3 MiB copy per move ("a bit laggy").
* **Write combining through PAT** (paging.c:513-565): PAT slot 4 is
  reprogrammed to WC and selected by the PAT bit (`PTE_WC` 0x080) in a 4 KiB
  PTE. **Only the adopted GOP aperture uses it.** The VBE aperture is mapped
  with plain `PTE_PRESENT|PTE_RW` (fb.c:208-213) and the SVGA aperture through
  `paging_map_device`, which is uncached. The boot log nevertheless says
  "write combining" whenever PAT is present (main.c:403-406). See section 10.

### 2.6 Clipboard and pins

* **Clipboard**: text only (clipboard.h:12-14). It is in the kernel because
  two ring-3 programs have no other way to hand each other bytes. Anything
  longer than `CLIP_MAX-1` bytes is **refused rather than truncated**.
  Syscalls 38/39.
* **Pins**: a list of up to 8 `{label, path}` entries kept in `/zelr.pins`,
  deliberately separate from the theme file, which the settings program
  rewrites whole. **Nothing outside selftest calls pins.c.** The WM's pinned
  row was removed (README.md:843). pins.c survives only as the `[taskbar]`
  selftest section.

### 2.7 Polled or interrupt-driven, and what runs on the timer tick

| Device | Mechanism |
|---|---|
| PS/2 keyboard | IRQ1 (vector 33) plus a drain on every tick |
| PS/2 mouse / Synaptics | IRQ12 (vector 44) plus a drain on every tick. Synaptics tap release happens on the tick (`syn_tick`) |
| xHCI / USB HID | Polled. Event ring drained on every tick. Also drained inline while synchronous commands and transfers wait |
| USB hotplug | "usb" kernel task, `task_sleep(300)` loop. Rescans when a Port Status Change event has set `port_changed` |
| USB stick | Synchronous spin-waited bulk transfers |
| HDA | No interrupts (INTCTL never written; the RIRB IRQ bit is set only so RIRBSTS works). Position read from LPIB |
| Ensoniq | No interrupts (`SER_P2_INT_EN` cleared). Position from the DAC2 frame register, or from the clock |
| VMware SVGA | Synchronous: `SVGA_REG_SYNC=1` then busy-wait on `SVGA_REG_BUSY` after each update |
| Framebuffer | Flushed on demand. Half the band work goes to an AP through an IPI (`smp_run`) |

**Timer tick** (`timer.c:on_tick`, PIT IRQ0 at 100 Hz, BSP only, kernel lock
held, IF=0), in this order: `ticks++`, `usb_poll()`, `ps2_poll_from_timer()`,
`syn_tick()`, `sound_poll()`, `rng_tick()` (timer.c:16-44).

---------------------------------------------------------------------------

## 3. File by file

### 3.1 include/ps2.h + kernel/ps2.c (the 8042)

**Constants** (ps2.c:26-57)

| Group | Names and values |
|---|---|
| Ports | `PS2_DATA 0x60`, `PS2_CMD 0x64`, `PS2_STAT 0x64` |
| Status bits | `ST_OUTPUT 0x01` (byte waiting), `ST_INPUT 0x02` (controller busy), `ST_AUX 0x20` (byte is from the mouse) |
| Config bits | `CFG_KBD_IRQ 0x01`, `CFG_AUX_IRQ 0x02`, `CFG_KBD_CLOCK 0x10` and `CFG_AUX_CLOCK 0x20` (set means clock off), `CFG_TRANSLATE 0x40` |
| Controller commands | `CMD_READ_CONFIG 0x20`, `CMD_WRITE_CONFIG 0x60`, `CMD_AUX_OFF 0xA7`, `CMD_AUX_ON 0xA8`, `CMD_KBD_OFF 0xAD`, `CMD_KBD_ON 0xAE` |
| Keyboard bytes | `KBD_ENABLE_SCAN 0xF4`, `KBD_ECHO 0xEE`, `KBD_ACK 0xFA` |
| Timeout | `SPIN 100000` status reads per wait. An absent port reads 0xFF, so every wait must be bounded |

**Globals** (ps2.c:59-62): `present`, `first_status` (initialised 0xFF),
`first_config`, `rescued`.

**Functions**

* `wait_write()` (ps2.c:69-72): spins until `ST_INPUT` clears, at most SPIN
  times. Gives up silently.
* `wait_read()` (ps2.c:74-78): spins until `ST_OUTPUT` is set. Returns bool.
* `ps2_command(u8)` / `ps2_write_data(u8)` (ps2.c:80-81): `wait_write` then `outb`.
* `bool ps2_read(u8 *out)` (ps2.c:83-88): `wait_read`, then `inb(0x60)`.
  `out` may be NULL, which discards the byte. **It does not check `ST_AUX`**,
  so during init a keyboard byte can be consumed as a mouse reply.
* `u8 ps2_config()` (ps2.c:90-95): 0x20 then read. Returns 0 on no answer.
* `write_config(u8)` (ps2.c:97-100): 0x60 then the byte.
* `drain()` (ps2.c:105-111): discards up to 64 bytes. Stops when status is 0xFF
  or OBF is clear.
* `u32 ps2_poll()` (ps2.c:113-133): the single read path. Returns 0 if
  `!present`. For up to 64 bytes: read status; stop on 0xFF or !OBF; read
  data; `st & ST_AUX` sends it to `mouse_byte`, otherwise `keyboard_byte`.
  The 64 cap exists "because this runs inside an interrupt handler: a
  controller gone mad must not be able to hold the machine here". Returns
  the number of bytes taken.
* `ps2_poll_from_timer()` (ps2.c:135-140): `rescued += ps2_poll()`. See
  section 10: nothing ever reads `rescued`.
* `ps2_init()` (ps2.c:142-201): the sequence is in section 4.3.
* Accessors: `ps2_present`, `ps2_first_status`, `ps2_first_config`,
  `ps2_rescued` (ps2.c:64-67). Only `ps2_present` has a caller outside ps2.c
  (mouse.c:265).

What ps2.c does **not** do: controller self-test (0xAA), port tests (0xAB/0xA9),
keyboard reset (0xFF), scancode-set selection (0xF0; it relies on translation),
LEDs (0xED), typematic rate (0xF3).

### 3.2 include/keyboard.h + kernel/keyboard.c

**Key encoding** (keyboard.h:10-37)

| Symbol | Value | Note |
|---|---|---|
| `KEY_UP/DOWN/LEFT/RIGHT` | 0x100 / 0x101 / 0x102 / 0x103 | |
| `KEY_HOME/END` | 0x104 / 0x105 | |
| `KEY_PAGE_UP/PAGE_DOWN` | 0x106 / 0x107 | |
| `KEY_DELETE/INSERT` | 0x108 / 0x109 | |
| `KEY_F1` | 0x110 | F1..F12 are consecutive: 0x110..0x11B |
| `KEY_MOD_ALT/CTRL/SHIFT` | 0x10000 / 0x20000 / 0x40000 | Mask `KEY_MODS 0x70000` |
| `KEY_CODE(k)` | `k & 0xFFFF` | |
| `KEY_IS_SPECIAL(k)` | `KEY_CODE(k) >= 0x100` | |

A key value is `code | mods`. For characters, `code` is a byte 0..255.

**State** (keyboard.c:13-18): ring `static volatile int buf[256]`, indices
`head` and `tail` (`BUFSZ 256`, one slot always empty, so 255 keys fit).
Modifier bools `shift`, `caps`, `ctrl`, `alt`. `extended` is set after an
0xE0 prefix.

**Maps** (keyboard.c:20-34): `MAP[128]` and `MAP_SHIFT[128]`, US layout,
scancode set 1, filled only up to index 0x39 (space). 0x37 (keypad `*`) maps
to `'*'`. Everything above 0x39 is 0.

**Functions**

* `interrupting(c)` (44-48): if `c == 3`, calls `signal_interrupt()`.
* `push(int c)` (50-60): ORs in `KEY_MOD_ALT/CTRL/SHIFT` from the *current*
  statics, calls `interrupting(KEY_CODE(c))` (so it fires even if the ring is
  full), then enqueues. A full ring drops the new key.
* `extended_key(code)` (64-78): after 0xE0: 0x48 UP, 0x50 DOWN, 0x4B LEFT,
  0x4D RIGHT, 0x47 HOME, 0x4F END, 0x49 PGUP, 0x51 PGDN, 0x53 DEL, 0x52 INS.
  Anything else returns -1 (ignored). That includes keypad Enter (E0 1C),
  keypad `/` (E0 35) and the GUI/menu keys.
* `keypad_key(code)` (82-95): the same navigation keys without the prefix
  (keypad), except INSERT. NumLock is not tracked, so **keypad digits always
  act as navigation keys**.
* `keyboard_byte(u8 sc)` (102-158):
  - `0xE0` sets `extended` and returns.
  - Release (bit 7): clears shift (0x2A/0x36), ctrl (0x1D) or alt (0x38).
    After an E0 only ctrl and alt are cleared, which means right ctrl and
    AltGr. The fake shifts E0 2A / E0 AA are therefore ignored.
  - Extended make: E0 1D sets ctrl, E0 38 sets alt, else `extended_key`.
  - Plain make: 0x2A/0x36 shift, 0x1D ctrl, 0x38 alt, 0x3A toggles caps.
    0x3B..0x44 give F1..F10, 0x57 F11, 0x58 F12. Otherwise
    `c = shift ? MAP_SHIFT[sc] : MAP[sc]`. If `c == 0`, try `keypad_key`.
    Caps uppercases a..z. Caps with shift lowercases A..Z. Then
    `ctrl && 'a'..'z'` folds to 1..26 (keyboard.c:153-157). Finally `push(c)`.
* `on_irq` (160-163) calls `ps2_poll()`. `keyboard_init()` (165-168)
  registers vector 33 and `pic_unmask(1)`.
* `kbd_has_char()` (170): `head != tail`. **It ignores serial input**, which
  `kbd_trygetchar` does return.
* `keyboard_inject(int key)` (174-177): enqueues an already-stamped key.
  **It does not call `interrupting()`.**
* `keyboard_set_mods(a,c,s)` (179-181): overwrites the same statics the PS/2
  decoder uses.
* `kbd_alt/ctrl/shift()` (183-185): live state. No callers.
* `int kbd_trygetchar()` (187-209): ring empty → `serial_trygetc()`, with CR
  turned into LF and no modifier bits; returns -1 if nothing. Otherwise
  returns `mods | (special ? code : (u8)code)`. The `(u8)` stops bytes above
  127 from coming back negative.
* `char kbd_getchar()` (211-217): blocking `hlt` loop, drops mods. No callers.

### 3.3 include/mouse.h + kernel/mouse.c

**Types**: `mouse_edge_t { i32 x, y; u8 buttons; }` (mouse.h:12).

**State** (mouse.c)

* Sprite: `CUR_W 12`, `CUR_H 19`, `CURSOR[19][12]` with 0 transparent,
  1 outline `RGB(0x10,0x14,0x18)`, 2 fill `RGB(0xF2,0xF5,0xF7)`
  (mouse.c:22-46, 178-179).
* Pointer: `present`, `mx`, `my`, `buttons` (48-50).
* Edge queue: `MOUSE_EDGES 32`, `edges[32]`, `volatile edge_head/edge_tail`
  (68-70).
* Packet: `packet[4]`, `phase`, `packet_len` (3, 4 with wheel, 6 when
  Synaptics is present, though Synaptics bypasses it), `wheel` (pending
  steps), `moves` (87-91).
* Sprite save-under: `drawn`, `autodraw` (initially true; false while the WM
  owns the pointer), `saved[19][12]`, `saved_x/saved_y` (93-96).

**Functions**

* `edge_record()` (72-79): appends `{mx,my,buttons}`. When the queue is full
  the new edge is dropped, keeping the backlog.
* `bool mouse_take_edge(out)` (81-86): pops the oldest edge.
* `mouse_has_wheel()` (105): `packet_len == 4 || syn_present()`. True for any
  Synaptics pad, even one without W reporting, which cannot scroll.
* `i32 mouse_take_scroll()` (107-116): reads and clears `wheel` with
  interrupts off.
* `mouse_inject_scroll(steps)` (118-121): `wheel += steps; moves++`.
  Positive means down.
* `mouse_cmd(cmd)` (123-127): 0xD4 prefix, the byte, then `ps2_read(0)` for
  the ACK. The ACK is not checked.
* `enable_wheel()` (137-154): the IntelliMouse knock. Set sample rate
  (0xF3) to 200, 100, 80; then 0xD4 0xF2 (get ID), read the ACK, read the ID;
  then set the rate back to 100 ("the knock leaves it at 80 reports a
  second"). Returns `id == 3`. The 5-button knock (200/200/80 giving ID 4)
  is not tried.
* `mouse_hide()` (156-163): restores the saved pixels and
  `fb_flush_rect`s the sprite area.
* `mouse_set_autodraw(on)` (165-168). `mouse_show()` (170-184): only when
  `autodraw && !drawn && fb_active() && present`. Saves the pixels under the
  sprite, draws it, flushes the rect.
* `mouse_inject(dx, dy, btns)` (189-210): the shared door for PS/2,
  Synaptics and USB. `dy` is **up-positive** (PS/2 convention) and screen y
  is `my -= dy`. Hides the sprite, moves, clamps to
  `[0, fb_width()-1] x [0, fb_height()-1]`, `moves++`, shows the sprite.
  An edge is recorded after the move when the buttons changed ("where the
  button went down is where the pointer ended up"). Without a framebuffer
  `fb_width()` is 0, so `mx` clamps to -1.
* `on_packet()` (212-232): byte 0 bit 3 must be set (sync). Bits 0xC0
  (overflow) drop the packet. Bits 0x10/0x20 are the X/Y signs. Buttons are
  `flags & 0x07` (left, right, middle). With a 4-byte packet the Z value is
  the low nibble of byte 3, signed.
* `mouse_byte(b)` (239-249): goes to `syn_byte` if `syn_present()`.
  Otherwise accumulates bytes, resyncs if byte 0 lacks bit 3, and decodes at
  `packet_len`.
* `mouse_isr` (251-254) calls `ps2_poll()`.
* `bool mouse_init()` (256-291): see section 4.3. **It never checks any ACK,
  so it returns true whenever the 8042 exists**, whether or not a mouse is
  attached.

### 3.4 include/synaptics.h + kernel/synaptics.c

**Protocol constants** (synaptics.c:31-46)

| Group | Names and values |
|---|---|
| Commands | `CMD_SET_RESOLUTION 0xE8`, `CMD_STATUS_REQUEST 0xE9`, `CMD_SET_SAMPLE_RATE 0xF3` |
| Queries | `QUERY_IDENTIFY 0x00`, `QUERY_CAPABILITIES 0x02` |
| Magic | `SYNAPTICS_MAGIC 0x47`, the middle byte of every answer |
| Mode byte | `MODE_ABSOLUTE 0x80`, `MODE_HIGH_RATE 0x40`, `MODE_DISABLE_GESTURE 0x04`, `MODE_W 0x01` |
| Capabilities | `CAP_EXTENDED 0x800000`, bit 23 of the assembled capability word: the pad can report W |

**Detection state** (48-52): `present`, `major`, `minor`, `caps`, `packets`,
`w_reported`.

**Decoder tuning** (132-160)

| Constant | Value | Meaning |
|---|---|---|
| `Z_TOUCH` | 30 | Pressure below this means no finger |
| `MOVE_DIVISOR` | 6 | Pad units per pixel |
| `SCROLL_DIVISOR` | 120 | Pad units per wheel step |
| `JUMP_LIMIT` | 700 | Per-report delta treated as a contact switch. It was 200, which threw away real flicks, and the checks caught it |
| `TAP_TICKS` | 20 | 200 ms at 100 Hz |
| `TAP_SLOP` | 120 | Pad units of total travel |
| `TAP_HOLD_TICKS` | 8 | How long the tap click is held, so the WM sees it |

**Decoder state** (162-176): `touching`, `last_x/last_y`,
`rem_x/rem_y/rem_scroll`, `fingers`, `most_fingers`, `touch_began` (ticks),
`travelled`, `phys_buttons`, `tap_button`, `tap_release_at`, `buf[6]`,
`phase`.

**Functions**

* `knock(arg)` (63-68): four `E8 xx` pairs, two bits each, most significant
  first.
* `query(arg, out[3])` (70-78): knock, 0xD4 0xE9, read the ACK, read 3 bytes.
* `syn_answer_is_pad(a)` (80-82): `a[1] == 0x47`.
* `bool syn_detect()` (84-121):
  1. Reset; return false without an 8042.
  2. Identify: `minor = id[0]`, `major = id[2] & 0x0F`.
  3. Capabilities, if the answer also carries 0x47:
     `caps = cap[0]<<16 | cap[1]<<8 | cap[2]`. The middle byte is 0x47.
  4. `w_reported = caps & CAP_EXTENDED`.
  5. Mode = `ABSOLUTE|HIGH_RATE|DISABLE_GESTURE`, plus `W` if reported.
     Knock the mode, then `F3 0x14` commits the mode byte.

  It **must run before interrupts are on**: afterwards the timer drains the
  8042 and eats the 3-byte answer, so a real pad reads as absent
  (synaptics.h:31-35).
* `syn_reset_state(report_w)` (178-190): clears the decoder and sets
  `w_reported`. Used by selftest.
* `fingers_from_w(w)` (199-206): if W is not reported, 1. Otherwise
  w=0 means 2 fingers, w=1 means 3, anything else 1 (2 is a pen, 4 and up is
  one finger's width).
* `publish(dx,dy)` (208-210): `mouse_inject(dx, dy, phys_buttons | tap_button)`.
* `scaled(delta, div, &rem)` (214-219): `total = delta + rem`, output
  `total/div`, remainder kept.
* `contact_ended()` (221-236): a tap is `held < TAP_TICKS && travelled <
  TAP_SLOP && most_fingers > 0`. One finger taps button 0x01, two or more tap
  0x02. Sets `tap_release_at = now + 8`, resets the contact and remainders,
  publishes.
* `on_packet()` (238-312): see the state machine in section 4.6.
  Decoding (241-247): `x = (b3&0x10)<<8 | (b1&0x0F)<<8 | b4`;
  `y = (b3&0x20)<<7 | (b1&0xF0)<<4 | b5`; `z = b2`;
  `w = (b0&0x30)>>2 | (b0&0x04)>>1 | (b3&0x04)>>2`;
  `phys_buttons = b0 & 3`. There is no middle button.
* `syn_byte(b)` (314-327): two anchors. Byte 0 must have top bits `10`
  (otherwise skipped). Byte 3 must have top bits `11` (otherwise the report is
  dropped and the phase reset).
* `syn_tick()` (331-336): when `tap_button` is set and the release time has
  come, clears it and publishes (the release edge).

### 3.5 include/xhci.h + kernel/xhci.c (host controller)

**Public types** (xhci.h)

* `XHCI_MAX_PORTS 32`, `XHCI_MAX_SLOTS 16` (xhci.h:20-21).
* `xhci_speed_t`, as PORTSC numbers it: `NONE 0`, `FULL 1`, `LOW 2`, `HIGH 3`,
  `SUPER 4` (24-30).
* `usb_setup_t`, packed: `u8 type; u8 request; u16 value; u16 index;
  u16 length` (33-39).
* `xhci_where_t` (61-67): `u32 root_port` (0-based controller port);
  `u32 route` (4 bits per hub tier, lowest tier first, 0 on a root port);
  `xhci_speed_t speed`; `u8 tt_slot` and `u8 tt_port` (the high-speed hub
  translating for a LS/FS device, 0 for none).
* Endpoint kinds, as they go into the endpoint context's EP Type field:
  `XHCI_EP_BULK_OUT 2`, `XHCI_EP_BULK_IN 6`, `XHCI_EP_INT_IN 7` (112-114).

**Register map** (xhci.c:41-94)

| Block | Registers and bits |
|---|---|
| Capability | `CAPLENGTH 0x00` (read as one 32-bit word together with HCIVERSION, because a narrower read returned 0 and "xhci 0.0"), `HCSPARAMS1 0x04`, `HCSPARAMS2 0x08`, `HCCPARAMS1 0x10`, `DBOFF 0x14`, `RTSOFF 0x18` |
| Operational (base+CAPLENGTH) | `USBCMD 0x00` (RUN bit0, RESET bit1), `USBSTS 0x04` (HALTED bit0, HSE bit2, CNR bit11), `PAGESIZE 0x08` (unused), `DNCTRL 0x14` (unused), `CRCR 0x18`, `DCBAAP 0x30`, `CONFIG 0x38`, `PORTSC(p) = 0x400 + p*0x10` |
| PORTSC bits | `CCS` b0, `PED` b1, `PR` b4, `PP` b9, speed = bits 10-13; RW1C change bits `CSC` b17, `PEC` b18, `WRC` b19, `OCC` b20, `PRC` b21, `PLC` b22, `CEC` b23 |
| PORTSC write helper | `PORT_KEEP(v)` masks off the change bits and PED (PED is write-1-to-disable) so a read-modify-write does not clear events or turn the port off |
| Runtime (base+RTSOFF), interrupter 0 | `IMAN 0x20` (unused), `IMOD 0x24`, `ERSTSZ 0x28`, `ERSTBA 0x30`, `ERDP 0x38` with `ERDP_BUSY` (EHB, bit 3) |

**TRBs** (97-128)

* Types: `NORMAL 1`, `SETUP 2`, `DATA 3`, `STATUS 4`, `LINK 6`,
  `ENABLE_SLOT 9`, `DISABLE_SLOT 10`, `ADDRESS_DEVICE 11`,
  `CONFIGURE_EP 12`, `EVALUATE_CTX 13`, `TRANSFER_EVENT 32`,
  `CMD_COMPLETE 33`, `PORT_STATUS 34`.
* Flags: `CYCLE` b0, `TOGGLE` b1, `ISP` b2, `CHAIN` b4 (defined, unused),
  `IOC` b5, `IDT` b6, type at `<<10`, `DIR_IN` b16.
* Completion codes: `COMP_SUCCESS 1`, `COMP_SHORT_PACKET 13`.
* `trb_t {u64 param; u32 status; u32 control;}` packed, 16 bytes.
* `erst_entry_t {u64 base; u32 size; u32 reserved;}`.

**Rings**: `RING_TRBS 32` (entry 31 is a Link TRB with TOGGLE, so 31 are
usable), `ring_t {trb_t *trb; u32 at; u8 cycle;}`. `EVENT_TRBS 64`, one ERST
segment.

**Globals** (148-195): the register pointers `cap, op, rt, db`. `nports`,
`nslots` (capped at 15), `ctx_stride` (32 or 64 from HCCPARAMS1 bit 2),
`present`, `description[72]`. `dcbaa` (nslots+1 entries; `dcbaa[0]` is the
scratchpad array), `cmd_ring`, `event_ring`, `erst`, `event_at`,
`event_cycle`. `slots[16]` of `slot_t { u8 *device_ctx; u8 *input_ctx;
ring_t ep[MAX_DCI=8]; bool used; u32 port; }`. `port` is set and never read.
Command completion: `volatile cmd_done, cmd_code, cmd_slot`. Transfer
completion, per `[slot][dci]`: `xfer_done` (counter), `xfer_code`,
`xfer_left` (residual). `report_cb`. `volatile bool port_changed`.

**Functions**

* `alloc_aligned(bytes, align)` (209-215): `kmalloc(bytes+align)`, rounded
  up, zeroed. **Never freeable.**
* `ctx_at(base, index)` (221-223): `base + index*ctx_stride`. Every context
  access must go through it.
* `ring_ready(r)` (232-248): allocates the ring on first use (64-byte
  aligned), otherwise zeroes it. `at=0`, `cycle=1`, and the last TRB becomes a
  Link back to the start with TOGGLE and cycle 0. `ring_init` is an alias.
* `ring_push(r, param, status, control)` (255-271): writes param and status,
  `__sync_synchronize()`, then control with the cycle bit, which goes on last.
  When `at` reaches 31, it rewrites the Link's cycle bit, wraps, and flips
  `cycle`.
* `doorbell(slot, target)` (273-276): `db[slot] = target`, then a read back
  to post the write.
* `spin_us(us)` is `io_wait` in a loop. `spin_ms`. `wait_bit(base, off,
  mask, set, ms)` (303-310) polls every 100 us, `ms*10+2` times.
* `drain_events()` (318-367): consumes event TRBs while their cycle bit
  matches `event_cycle`.
  - `CMD_COMPLETE` stores the code and slot (control bits 31:24), then sets
    `cmd_done`.
  - `TRANSFER_EVENT`: `slot = control>>24`, `dci = (control>>16)&0x1F`.
    Events with `slot >= 16` or `dci >= 8` are ignored. Stores the code and
    the residual (`status & 0xFFFFFF`), then `xfer_done++`. **If `dci > 1`
    and the code is SUCCESS or SHORT, calls `report_cb(slot, dci,
    residual)`.** That includes bulk completions, which usb.c's `on_report`
    simply fails to match.
  - `PORT_STATUS` sets `port_changed = true`.

  Wraps at 64 and flips `event_cycle`. Finally writes
  `ERDP = &event_ring[event_at] | EHB`.
* `xhci_poll()` (369-374): saves RFLAGS, `cli`, `drain_events`, restores IF.
  It is the only consumer of the event ring.
* `xhci_on_report(fn)` (376). `xhci_took_port_change()` (378-382) tests and
  clears the flag.
* `command(param, status, control, *slot_out)` (387-402): clears `cmd_done`,
  pushes, rings doorbell 0 target 0, then polls in 100 us steps until
  `cmd_done` or 10,000 iterations (about 1 s). Returns the completion code,
  or 0 on timeout. One command at a time.
* `reset_controller()` (405-417): clear RUN, wait HALTED (500 ms); set RESET,
  wait for RESET to clear (1 s) **and** for CNR to clear (1 s). "The second
  one is the one that gets forgotten."
* `take_ownership()` (424-446): walks the extended capabilities from
  HCCPARAMS1 bits 31:16 (dword offset), at most 64 hops. On the USB Legacy
  Support capability (ID 1) it sets OS-owned (bit 24), waits up to
  100 x 10 ms for BIOS-owned (bit 16) to clear, then writes 0 to
  USBLEGCTLSTS (`p+4`) to stop SMIs. The `pci_dev_t` parameter is unused.
* `make_rings()` (448-485):
  - DCBAA: `(nslots+1)*8` bytes, 64-byte aligned.
  - Scratchpad count: `((HCSPARAMS2>>21)&0x1F)<<5 | (HCSPARAMS2>>27)&0x1F`.
    That many 4 KiB pages (page size assumed; PAGESIZE is not read), their
    array stored in `dcbaa[0]`.
  - Command ring, 64-entry event ring, 1-entry ERST.
  - Checks that DCBAA, command ring and event ring are identity mapped.
* `bool xhci_init()` (487-560): see section 4.7.
* `xhci_port_connected(p)` reads CCS. `xhci_speed(p)` returns PORTSC bits
  10-13, or NONE when CCS is clear (566-576).
* `default_packet(speed)` (582-588): SS 512, HS 64, otherwise 8.
* `reset_port(p)` (591-611): requires CCS. Writes `PORT_KEEP|PR`. Polls up to
  100 x 10 ms for PRC and acks it with `PORT_KEEP|PRC`. Returns PED. USB 3
  ports that are already enabled pass too.
* `u8 xhci_attach(where)` (618-687):
  - Enable Slot, which must return a slot in 1..15.
  - Reuse or allocate that slot's device context (32 contexts) and input
    context (33); reuse its rings; `ring_ready(&ep[1])`;
    `dcbaa[slot] = device_ctx`.
  - Input control context `ctrl[1] = 0x3` (add slot and EP0).
  - Slot context: `sc[0] = route&0xFFFFF | speed<<20 | 1<<27` (context
    entries = 1), `sc[1] = (root_port+1)<<16`,
    `sc[2] = tt_slot | tt_port<<8`.
  - EP0 context: `ep0[1] = 4<<3` (control) `| 3<<1` (CErr 3)
    `| default_packet<<16`; `ep0[2..3]` = ring address | DCS 1; `ep0[4] = 8`
    (average TRB length).
  - Address Device (BSR=0, so SET_ADDRESS is sent). Returns the slot, or 0.
    A slot is not disabled on the failure paths after Enable Slot.
* `xhci_mark_hub(slot, ports, think_time, multi_tt)` (698-716): Configure
  Endpoint with only the slot context (`ctrl[1]=1`). The slot context is
  copied from the output context, then Hub bit 26, MTT bit 25, Number of
  Ports = `sc[1]` bits 31:24, TTT = `sc[2]` bits 17:16. The comment admits
  nothing tests this: QEMU routes to devices behind a hub without it.
* `xhci_detach(slot)` (718-723): Disable Slot, `used=false`, `dcbaa[slot]=0`.
  Memory is kept for reuse.
* `bool xhci_control(slot, setup, data, len)` (726-759): requires
  `data` identity mapped. Setup TRB with IDT, length 8 and TRT (3 IN, 2 OUT,
  0 none) at bits 17:16. Data TRB with DIR_IN for IN when `len > 0`. Status
  TRB with IOC and the opposite direction (IN for OUT/no-data). Doorbell
  target 1. Waits until `xfer_done[slot][1]` changes (10,000 x 100 us).
  SUCCESS or SHORT counts as success. No halt recovery.
* `xhci_set_packet_size(slot, mps)` (761-780): Evaluate Context on EP0 only
  (`ctrl[1]=2`). The EP0 context is copied from device context index 1, then
  the max packet size (bits 31:16) is replaced.
* `xhci_open_endpoint(slot, dci, kind, mps, interval)` (783-819): requires
  `2 <= dci < 8`. `ring_ready(&ep[dci])`. Input control context
  `ctrl[1] = 1 | 1<<dci`. Slot context copied, Context Entries raised to
  `dci` but never lowered, because a device is configured one endpoint at a
  time and the higher one must win. `ep[0] = interval<<16` (**the raw value
  passed in**); `ep[1] = kind<<3 | 3<<1 | mps<<16`; `ep[2..3]` = ring | DCS;
  `ep[4] = mps | mps<<16` (average TRB length and max ESIT payload).
  Configure Endpoint. `xhci_open_interrupt_in` is a wrapper with kind 7.
* `int xhci_bulk(slot, dci, data, len, in)` (836-859): `in` is ignored.
  Requires data identity mapped. One Normal TRB with `IOC|ISP` and
  `status = len` (no 64 KiB split or check). Doorbell target `dci`. Waits
  50,000 x 100 us (about 5 s). Returns `len - residual`, or -1.
* `xhci_listen(slot, dci, buf, len)` (861-869): queues one Normal TRB with
  IOC on an interrupt endpoint and rings the doorbell. Called once per report.

### 3.6 include/usb.h + kernel/usb.c (enumeration and classes)

**Requests and classes** (usb.c:39-61)

* Requests: `GET_DESCRIPTOR 6`, `SET_CONFIG 9`, `SET_IDLE 0x0A`,
  `SET_PROTOCOL 0x0B`.
* Descriptor types: `DEVICE 1`, `CONFIG 2`, `INTERFACE 4`, `ENDPOINT 5`,
  `HUB 0x29`, `HUB_SS 0x2A`.
* Classes: `HID 3` (subclass `BOOT 1`, protocols `KEYBOARD 1`, `MOUSE 2`),
  `HUB 9`, `MSC 8` (subclass `SCSI 6`, protocol `BBB 0x50`), `CDC 2`,
  `CDC_DATA 0x0A`.

**Descriptor structs** (63-101): `device_desc_t` (only the first 12 bytes:
length, type, usb, class, subclass, protocol, max_packet0, vendor, product),
`config_desc_t` (length, type, total_length, interfaces, value; 6 of 9
bytes), `interface_desc_t` (8 bytes), `endpoint_desc_t` (7 bytes), and
`hub_desc_t` (length, type, ports, characteristics, power_on_delay in 2 ms
units, current_ma; 7 bytes).

**Device table** (104-122): `MAX_DEVICES 8`, `REPORT_MAX 16`.
`device_t { bool used; bool keyboard; u8 slot; u8 dci; u32 root;
u16 length; u8 *buf; u8 last[16]; }`. **Only HID devices get an entry.**
Counters `nkeyboards, nmice, nhubs, ndisks, nnets` and
`volatile nreports`, plus `started` and `description[128]`.

**HID decode**

* `PLAIN[]` / `SHIFTED[]` (131-151): usages 0x00..0x38. Letters 0x04-0x1D,
  digits 0x1E-0x27, then `\n` (0x28), ESC (0x29), `\b` (0x2A), `\t` (0x2B),
  space (0x2C), punctuation 0x2D-0x38. The non-US key 0x32 is 0.
* `special(usage)` (155-171): 0x4F RIGHT, 0x50 LEFT, 0x51 DOWN, 0x52 UP,
  0x4A HOME, 0x4D END, 0x4B PGUP, 0x4E PGDN, 0x4C DELETE, 0x49 INSERT,
  0x3A..0x45 F1..F12. **No Caps Lock (0x39), no keypad (0x53-0x63), no 0x64.**
* `on_keyboard(d, len)` (184-226): modifier byte masks are shift 0x22,
  ctrl 0x11, alt 0x44. A usage in bytes 2..len-1 that is >= 4 and not in the
  previous report counts as a press. It is translated; ctrl folds
  `a..z` **and** `A..Z` to 1..26 (unlike PS/2). Then
  `keyboard_inject(key|stamp)`. After the loop:
  `keyboard_set_mods(alt, ctrl, shift)`; `last` is cleared, then the report
  copied into it. There is **no auto-repeat**, because `SET_IDLE(0)` stops
  the device repeating reports.
* `on_mouse(d, len)` (228-248): buttons `buf[0]&7`, `dx=(i8)buf[1]`,
  `dy=(i8)buf[2]`, then `mouse_inject(dx, -dy, buttons)`. With `len >= 4`
  the wheel `dz=(i8)buf[3]` goes to `mouse_inject_scroll(-dz)`.
* `on_report(slot, dci, residual)` (250-267): finds the matching `device_t`,
  computes `len = length - residual`, `nreports++`, decodes, re-queues with
  `xhci_listen`.

**Requests** (278-324): a heap bounce buffer `bounce` (`BOUNCE_MAX 512`).
Every control transfer goes through `request(slot, type, req, value, index,
out, len)`. `get_descriptor` uses type 0x80. `set_configuration` is type
0x00. `set_boot_protocol` is 0x21 with value 0. `set_idle` is 0x21 with
value 0 (report only on change). `wait_ms(ms)` (307-310) calls
`task_sleep(ms)` if there is a current task, otherwise spins on `io_wait`
(the boot path).

**Hubs** (340-410): `hub_descriptor` (0xA0 GET_DESCRIPTOR, type 0x29 or
0x2A). `port_feature` (0x23, SET_FEATURE 3 or CLEAR_FEATURE 1). `port_status`
(0xA3 GET_STATUS, 4 bytes). Port status bits: `CONNECTED 0x1`,
`ENABLED 0x2`, `RESETTING 0x10`, `LOW_SPEED 0x200`, `HIGH_SPEED 0x400`
(USB 2 layout). Features: `PORT_RESET 4`, `PORT_POWER 8`,
`C_PORT_CONNECTION 16`, `C_PORT_RESET 20`. `reset_hub_port(slot, port)`
(384-410): requires CONNECTED; SET PORT_RESET; up to 20 x 10 ms for
`!RESETTING && ENABLED`; clears C_PORT_RESET and C_PORT_CONNECTION; speed
from the LOW/HIGH bits, defaulting to FULL.

**`claim_interface(slot, root_port, cfg, total)`** (425-563) walks the
configuration records by their own `bLength`.
* On each interface descriptor:
  - `want` = the interface if it is HID boot keyboard or mouse, else null.
  - `disk` = the interface if it is MSC/SCSI/BBB, else null.
  - `comm_iface` = interface number when the class is `CDC` (0x02).
  - `netdata` = the interface if the class is `CDC_DATA`, else null.
* Endpoint while `disk || netdata`: bulk endpoints (attributes & 3 == 2)
  record `bulk_in` or `bulk_out` as a DCI (`number*2 + in`) and `bulk_mps`.
* Endpoint while `want`: the first interrupt-IN endpoint (a) takes a free
  `device_t`; (b) `xhci_open_interrupt_in(slot, dci, mps, interval =
  bInterval ? bInterval : 1)`; (c) gets a `kmalloc(32)` buffer;
  `length = min(mps, 16)`; (d) sends SET_PROTOCOL(boot) and SET_IDLE(0)
  with results ignored; (e) counts it, logs, `xhci_listen`, and **returns
  true**. So only the first boot interface of a device is claimed.
* After the walk:
  - Both bulk directions found **and `netdata` non-null** (so the CDC-data
    interface must be the *last* interface seen): open the lower DCI first,
    then the higher. Require `comm_iface`. Call `usbnet_attach`, `nnets++`,
    `net_init()`.
  - Else, both bulk directions found: open both, `usbdisk_attach`,
    `ndisks++`, `diskfs_mount_removable(usbdisk_blk_id())`.

**Tree walk**
* `MAX_TIER 5`.
* `walk_hub(slot, hub, tier, hub_speed)` (573-613): hub descriptor (1..15
  ports); `xhci_mark_hub(slot, ports, TTT = (wHubCharacteristics>>5)&3,
  false)`; power every port; wait `power_on_delay*2 + 20` ms. For each port,
  `reset_hub_port`, then the child `where` is a copy of the hub's with the
  new speed and `route |= p << (4*tier)`. When the hub is HS and the child
  LS/FS, `tt_slot=slot`, `tt_port=p` (otherwise the parent's TT fields are
  inherited). Then `enumerate(child, tier+1)`.
* `enumerate(where, tier)` (616-655):
  1. `xhci_attach`.
  2. Get the device descriptor, 8 bytes. If `max_packet0` is neither 0 nor 8,
     `xhci_set_packet_size` (result ignored; see section 10 for SuperSpeed).
  3. Get the config descriptor header (6 bytes). Require
     `0 < total <= 512`.
  4. `kmalloc(total)`, get the full descriptor, SET_CONFIGURATION(value).
  5. Device class 9: `nhubs++` and `walk_hub`. Otherwise `claim_interface`.
  6. kfree.

  Failure paths leave the slot enabled.
* `setup_port(p)` (657-661): `xhci_reset_root_port`, then enumerate
  `{p, 0, speed, 0, 0}` at tier 0.

**Hotplug** (663-721): `claimed[32]`.
* `forget_root(port)` (681-694): for each **HID** `device_t` with that root:
  adjust the counts, `diskfs_unmount_removable()`, `usbdisk_detach(slot)`,
  `xhci_detach(slot)`, clear the entry. See section 10 for why this is wrong.
* `rescan()` (696-710): compares every root port's CCS against `claimed[]`
  and sets up or forgets.
* `service()` (712-717): `for(;;) { task_sleep(300); if
  (xhci_took_port_change()) rescan(); }`.
* `usb_start_service()` (719-721): creates task "usb" if `started`.

**Outside interface** (724-768):
* `describe()` builds `"<xhci desc>, N hub(s), N keyboard(s), N mouse,
  N disk(s)[, N network]"`.
* `usb_init()`: allocate `bounce`; `xhci_init` (on failure the description is
  "no controller"); `xhci_on_report(on_report)`; set up every connected root
  port and mark it claimed; `started = true`.
* `usb_poll()` calls `xhci_poll()` if started.
* Accessors. `usb_mice/hubs/disks` have no callers.

### 3.7 include/usbdisk.h + kernel/usbdisk.c (mass storage)

**Wire formats** (26-46)
* `cbw_t`, 31 bytes packed: signature `0x43425355` "USBC", tag, length,
  flags (0x80 = IN), lun 0, cmd_len, `cmd[16]`.
* `csw_t`, 13 bytes: signature `0x53425355` "USBS", tag, residue, status
  (0 good, 1 failed, 2 phase error).

**SCSI opcodes used** (49-54): TEST UNIT READY 0x00, REQUEST SENSE 0x03 (18
bytes), INQUIRY 0x12 (36 bytes), READ CAPACITY(10) 0x25, READ(10) 0x28,
WRITE(10) 0x2A. Not used: MODE SENSE, READ CAPACITY(16),
PREVENT/ALLOW, START STOP, SYNCHRONIZE CACHE, and the class requests Get
Max LUN and Bulk-Only Reset.

**State** (60-72): `attached`, `dev_slot`, `dev_in`, `dev_out`, `nsectors`,
`sector_bytes` (default 512), `model[40]`, `next_tag`. Heap buffers `cbw`,
`csw`, `stage` (`STAGE_MAX 4096`) are allocated once and kept.

**Functions**
* `settle_ms()` always spins on `io_wait`, even from the task.
* `transact(cmd, len, data, dlen, in)` (89-118): CBW out (must move exactly
  31 bytes), optional data stage (short is fine), CSW in (exactly 13 bytes).
  Checks signature, tag, and status == 0.
* `clear_sense()` reads and discards sense data. A failed command leaves a
  sense condition that blocks the next one.
* `wait_ready()` (140-147): up to 20 x (TUR, clear sense, 50 ms). A freshly
  plugged stick always refuses the first command (unit attention).
* `inquiry()` (160-178): vendor(8) + " " + product(16), spaces trimmed,
  falling back to "usb disk".
* `read_capacity()` (180-195): big-endian last LBA and block size. Rejects a
  size of 0 or over 4096, and a last LBA of 0xFFFFFFFF (that would need
  READ CAPACITY(16), so sticks over 2 TiB are refused).
* `rw10(lba, count, buf, write)` (202-219): `count <= 0xFFFF`. Data length
  is `count*sector_bytes`. On failure, `clear_sense`.
* `usbdisk_read/write` (228-238): bounds-check against `nsectors`.
* `usbdisk_max_run()` returns 8 sectors. `usbdisk_flush()` returns
  `attached`; there is no cache.
* `USB_DEV` (248-251): the `blkdev_t` `{"usb", read, write, flush, sectors,
  max_run, model, removable=true}`. `blk_id` starts as `BLK_NONE`.
* `usbdisk_attach(slot, in, out)` (257-284): refuses if already attached
  ("one stick is enough for now"). `wait_ready`; `inquiry` (optional);
  `read_capacity`; `blk_id = blk_register(&USB_DEV)`.
* `usbdisk_detach(slot)` (286-296): acts only when `slot == dev_slot`:
  `blk_unregister`, clears state.

The block layer (blockdev.c:78-112) advances its buffer pointer by
`SECTOR_SIZE` (512) per sector, and all FAT code thinks in 512-byte sectors.
usbdisk moves `count*sector_bytes` bytes. See section 10 for sticks whose
block size is not 512.

### 3.8 include/hda.h + kernel/hda.c (Intel HD Audio)

**Controller registers** (hda.c:34-82): `GCAP 0x00` (ISS = bits 11:8,
OSS = bits 15:12), `GCTL 0x08` (CRST bit 0), `STATESTS 0x0E`, `INTCTL 0x20`
(never written), `CORBLBASE 0x40`, `CORBUBASE 0x44`, `CORBWP 0x48`,
`CORBRP 0x4A` (RST bit 15), `CORBCTL 0x4C` (RUN bit 1), `CORBSIZE 0x4E`,
`RIRBLBASE 0x50`, `RIRBUBASE 0x54`, `RIRBWP 0x58` (write bit 15 to reset),
`RINTCNT 0x5A`, `RIRBCTL 0x5C` (RUN bit 1, IRQ bit 0), `RIRBSTS 0x5D`,
`RIRBSIZE 0x5E`, `DPLBASE 0x70` and `DPUBASE 0x74` (unused).
Stream descriptors start at `SD_BASE 0x80`, `SD_STRIDE 0x20`, with
`CTL 0x00` (SRST bit 0, RUN bit 1, stream tag bits 23:20), `STS 0x03`,
`LPIB 0x04`, `CBL 0x08`, `LVI 0x0C`, `FMT 0x12`, `BDPL 0x18`, `BDPU 0x1C`.

`RIRBCTL_IRQ` is set even though no interrupt is wanted (hda.c:58-65): the
controller raises RIRBSTS only with it set and only counts a response as
acknowledged when that flag is cleared. Without it the response counter
reaches RINTCNT and command fetching stops for good. "One command works and
every command after it times out."

**Codec verbs** (89-98): `GET_PARAM(p) 0xF00xx`, `SET_POWER 0x705xx`,
`GET_CONN 0xF02xx`, `SET_CONN_SEL 0x701xx`, `SET_PIN_CTL 0x707xx`,
`SET_EAPD 0x70Cxx`, `SET_STREAM_CHAN 0x706xx`, `GET_CONFIG_DEFAULT 0xF1C00`,
`SET_FORMAT 0x2xxxx`, `SET_AMP 0x3xxxx`. Parameters: `VENDOR 0x00`,
`NODE_COUNT 0x04`, `FUNC_TYPE 0x05`, `WIDGET_CAPS 0x09` (type = bits 23:20),
`PIN_CAPS 0x0C` (output bit 4), `CONN_LEN 0x0E`. Widget types: DAC 0,
MIXER 2, SELECTOR 3, PIN 4.

**Other constants**: `AMP_OUT_UNMUTE 0xB07F` (output amp, left and right,
unmuted, gain 0x7F, which asks for more than any widget has so it clamps).
`PIN_OUT_ENABLE 0xC0` (out plus headphone drive). `MAX_WIDGETS 64`.
`RING_ENTRIES 256`.

**State** (126-148): `present`, `regs`, `out_stream` (= ISS),
`rate` (48000), `channels` (2), `corb` (u32[256]), `rirb` (u64[256]),
`corb_wp`, `rirb_rp`, `codec_addr`, `dac_nid`, `pin_nid`,
`described[96]`, `bdl` (`bdl_entry_t {u64 addr; u32 len; u32 flags;}`).

**Functions**

* `codec(nid, payload, *out)` (181-202): builds the verb
  `addr<<28 | nid<<20 | payload`, advances CORBWP, then polls RIRBWP every
  10 us up to 20,000 times (200 ms). Takes the next RIRB entry and acks
  RIRBSTS with 0x05. Only one command is ever in flight, "which removes every
  question about which response belongs to which command". Unsolicited
  responses are not distinguished (none are enabled).
* `param()`, `tell()`, `widget_type()` (204-218).
* `pin_score(nid)` (227-242): -1 unless the widget is a pin with output
  capability. From the configuration default: connectivity bits 31:30 equal
  to 1 (no connection) scores 0; device bits 23:20: 1 speaker 100,
  0 line out 90, 2 headphone 80, anything else 10.
* `dac_behind(pin)` (250-279): for connection index i < min(count, 8),
  issues `GET_CONN(i)` and uses **only the low byte** of the response. A DAC
  there is returned directly. A mixer or selector there gets one more hop,
  the same way; if the middle widget is a selector it is told `SET_CONN_SEL(j)`,
  and the middle widget is powered up and unmuted. The pin's own connection
  select is never set. Long-form lists and range entries are not handled.
* `find_route()` (282-329): for each function group under node 0 with
  `FUNC_TYPE == 1` (audio): power D0, wait 1 ms, score every widget (count
  clamped to 64; a count of 0 also becomes 64), take the best pin with score
  above 0, and find its DAC. If none is found, take the first DAC in the
  group ("the simple one QEMU presents wires a single converter straight to
  the pin and does not describe it"). Then power pin and DAC,
  `PIN_CTL 0xC0`, `EAPD 0x02` ("laptops need this or silence"), and unmute
  both amps.
* `reset_controller()` (333-354): clear CRST, wait up to 100 ms, wait 1 ms,
  set CRST, wait up to 100 ms, then 2 ms more. The spec needs 521 us for
  codecs to report; "look sooner ... and decide the machine has no sound".
* `start_rings()` (356-388): CORB 1 KiB and RIRB 2 KiB, 128-byte aligned
  (never freed). Size 0x02 (256 entries). CORBRP reset by the set/verify/
  clear/verify handshake. RIRBWP reset. `RINTCNT = 1`. Start both, with the
  RIRB IRQ bit set.
* `hda_init()` (390-441): `pci_find_class(0x04, 0x03, 0x00)`, so the first
  HDA function in bus order. BAR0, with the high dword from 0x14 when it is a
  64-bit BAR. `paging_map_device(bar, 0x4000)`. `pci_enable_bus_master`
  (which also enables memory and I/O). Reset. `out_stream = ISS`, and it
  fails when OSS is 0. Start the rings. The codec is the lowest set bit of
  STATESTS. `find_route`. Every failure writes a specific `bb_log` line
  ("hda: no pci class 4/3/0", and so on).
* `hda_start(buffer, bytes, rate_hz)` (449-499): `rate = rate_hz` (**the
  hardware is always programmed for 48 kHz regardless**). Allocates a
  4-entry BDL (128-byte aligned, a fresh one on every call), each entry a
  quarter of the buffer with no IOC. Stops the stream, pulses SRST, clears
  STS with 0x1C, `CBL = quarter*4`, `LVI = 3`, BDL address. `FMT = (ch-1) |
  1<<4`: 16-bit, 48 kHz base, no multiplier or divider. The DAC gets
  `SET_FORMAT(fmt)` and `SET_STREAM_CHAN(0x10)` (stream 1, channel 0). Then
  `CTL = 1<<20 | RUN` (stream tag 1). The stream is never stopped again.
* `hda_position()` (501-504): reads LPIB.

### 3.9 include/ens.h + kernel/ens.c (Ensoniq AudioPCI)

**PCI IDs** (38-43): vendor 0x1274. `ES1370 0x5000`. The 1371 family:
`ES1371 0x1371`, `CT5880 0x5880`, `ES1373_A 0x8001`, `ES1373_B 0x8002`. I did
not verify the last two against any outside list.

**I/O registers** (47-62): `CONTROL 0x00`, `STATUS 0x04`, `MEM_PAGE 0x0C`,
`1370_CODEC 0x10` (AK4531, write only), `1371_SRC 0x10` (same address),
`1371_CODEC 0x14` (AC'97), `LEGACY 0x18`, `SERIAL 0x20`, `DAC2_COUNT 0x28`.
The last 16 bytes are a paged window: `PAGE_DAC 0x0C` holds
`DAC2_FRAME 0x38` (physical address) and `DAC2_SIZE 0x3C` (size in dwords
minus 1 in the low half, current dword count in the high half).

**Bit fields** (65-111)

* CONTROL: `PCLKDIV` bits 28:16 (ES1370 rate divider), `1371_SYNC_RES`
  bit 14, `DAC1_EN` b6, `DAC2_EN` b5, `ADC_EN` b4, `UART_EN` b3,
  `JYSTK_EN` b2, `1370_CDC_EN` / `1371_XTAL_EN` b1, `SERR_DIS` b0.
* SERIAL: `P2_END_INC` bits 21:19, `P2_ST_INC` bits 18:16, `P2_LOOP_SEL`
  b14 (set means stop at end, so it is cleared), `P2_PAUSE` b12,
  `P2_INT_EN` b9, `P2_DAC_SEN` b6, `P2_FMT` bits 3:2 (3 = 16-bit stereo).
* AC'97 port: `RDY` b31, `WIP` b30, `PIRD` b23.
* SRC port: `ADDR` bits 31:25, `WE` b24, `BUSY` b23, `DISABLE` b22,
  `DIS_P1` b21, `DIS_P2` b20, `DIS_R1` b19, data bits 15:0.
* SRC register file: `SMP_DAC1 0x70`, `SMP_DAC2 0x74`, with offsets
  `TRUNC_N 0x00`, `INT_REGS 0x01`, `VFREQ_FRAC 0x03`; volumes
  `VOL_ADC 0x6C`, `VOL_DAC1 0x7C`, `VOL_DAC2 0x7E`.
* `POLL 100000`.

**State** (115-130): `dev`, `io_base`, `present`, `is_1371`, `rate` (44100),
`channels` (2), `described[80]`, `ctl_shadow`, `ser_shadow`.

**Functions**

* `udelay(us)` (139-145): counts toggles of port 0x61 bit 4 (the refresh
  flip-flop, 15.085 us), which works with interrupts still off.
* 1371 front end:
  - `src_wait()` (149-156) polls until BUSY clears.
  - `src_write(reg, data)` (161-165) carries the four disable bits through
    every write, or the write would re-enable channels.
  - `src_read(reg)` (167-171).
  - `src_dac2_rate(hz)` and `src_dac1_rate(hz)` (175-207):
    `freq = ((hz<<15)+1500)/3000`, written as INT_REGS
    `(old & 0xFF) | (freq>>5)&0xFC00` and `VFREQ_FRAC = freq & 0x7FFF`,
    with that channel disabled while it changes. This matches the Linux
    driver's formula.
  - `src_init()` (216-242): disable the SRC, zero all 0x80 words,
    `TRUNC_N = 16<<4` and `INT_REGS = 16<<10` for both DACs, the six volumes
    to unity (`1<<12`), DAC1 at 22050, DAC2 at `rate`, then enable. The
    manual warns that enabling with unset parameters "locks the chip until
    the power goes off".
  - `ac97_write(reg, val)` (259-271): waits for !WIP, then waits for
    `SRC & 0x00870000` to be 0 and then 0x00010000, each bounded by
    `CODEC_SETTLE 4000` reads. Emulators never satisfy these; the bound
    keeps the boot from losing a second. Then writes `(reg&0x7F)<<16 | val`.
* 1370 front end: `ak4531_write(reg, val)` (277-282) waits for
  `STATUS & (1<<8)` (CSTAT) to clear, `outw(io+0x10, reg<<8 | val)`, then
  100 us.
* `DIV_FOR(hz) = (1411200 + hz/2)/hz - 2` (287).
* `ens_start(buffer, bytes, rate_hz)` (291-362): requires the buffer below
  4 GiB, dword aligned, and at least 64 bytes.
  - 1371: `rate = rate_hz`, `src_dac2_rate`.
  - 1370: clamp to 5512..44100, compute the divider (capped at 0x1FFF),
    `rate = 1411200/(div+2)` (the real rate is reported back), divider into
    `ctl_shadow`.
  - `count = frames-1` (capped at 0xFFFF). DAC2 off.
    `MEM_PAGE = 0x0C`, `FRAME = phys`, `SIZE = bytes/4 - 1`,
    `DAC2_COUNT = count<<16 | count`. `SERIAL`: END_INC 2, ST_INC 0,
    16-bit stereo, loop and pause and interrupt all cleared. DAC2 on.
  - Logs every register read back ("a register that did not take ...").
  - Sets `described = "ensoniq es137x, dac2 16 bit stereo"`.
* `ens_position()` (364-373): selects page 0x0C again (anything else may have
  moved it), returns `(SIZE >> 16) * 4`.
* `find_card()` (375-390): 1371 IDs first, then 1370.
* `ens_init()` (392-473): enable bus master (plus I/O and memory),
  `io_base = BAR0 & ~3`. Zero CONTROL, SERIAL and LEGACY (the legacy window
  is SB emulation that collides with real devices).
  - 1371: warm reset of the AC'97 link through `SYNC_RES`; wait up to POLL
    for codec RDY (not required: "QEMU's part of this family has no codec
    behind it at all", though QEMU only emulates the 1370); `src_init`; then
    AC'97 reset (0x00), power (0x26 = 0), master 0x02, headphone 0x04,
    mono 0x06 all 0 (loudest, unmuted), PCM 0x18 = 0x0808.
  - 1370: `CONTROL = CDC_EN | SERR_DIS`, then AK4531 master L/R (0x00/0x01)
    = 0, voice L/R (0x02/0x03) = 0, mic gain 0x0E = 0x01, output mixers
    0x10/0x11 = 0x03, and **0x16 = 0x00** "reset the mixer state" (see
    section 10).

### 3.10 include/sound.h + kernel/sound.c (above the controllers)

**Constants**: `RING_FRAMES 16384` (0.34 s at 48 kHz; with 4-byte frames the
ring is 64 KiB). `GUARD_FRAMES 256`: never fill completely, so the writer
cannot overwrite the sample being read.

**Device dispatch** (41-66): `snd_dev_t {SND_NONE, SND_HDA, SND_ENS}` and the
static `snd_rate/channels/frame_bytes/position/name/start` wrappers.

**State** (68-84, 145-149): `ready`, `ring`, `ring_bytes`, `frame_bytes`.
Monotonic byte counters `played`, `written`, `zeroed` that never wrap, and
`last_pos` (the last hardware position, which does wrap). `volume` (70).
Clock fallback: `pos_moves`, `clocked`, `clock_from_tick`,
`clock_from_played`, `started_at`.

**Functions**

* `copy_at(dst, src, samples)` (96-101): at 100 % a memcpy, at 0 a memset,
  otherwise `s * volume / 100`.
* `isin(u16 phase)` (117-122): no FPU. A parabola per half wave,
  `half*(32768-half)>>13`, clamped to 32767, sign from bit 15.
* `advance()` (171-211): **runs with interrupts off**, because the timer and
  a writer both call it. The comment describes the race it fixed: a position
  from before the tick with a mark from after it looked like a full wrap and
  freed the whole buffer, so long notes were written three or four times
  over and played only their last third.
  - Reads the hardware position (mod ring). A change sets `pos_moves`.
  - If the position has never moved and more than `timer_hz()/4` ticks have
    passed since start, switch to clocked mode.
  - Clocked: `played = max(played, clock_from_played + since * rate *
    frame_bytes / hz)`, and `last_pos = played % ring`.
  - Otherwise `played += (pos - last_pos) mod ring`.
  - Finally `written = max(written, played)`.
* `sound_poll()` (213-247), on every tick: `advance`, then zero from
  `max(written, played)` (and not below `zeroed`) forward to
  `played + ring_bytes`, with a budget of one ring. Nearly always that is
  just the bytes played since the last tick. It is the whole ring once, just
  after a sound ends. Zeroing only *behind* the play position had left stale
  sound ahead of it ("two notes running into one").
* `sound_write(frames, count)` (249-301): loop.
  - `advance`. `room = ring - (written - played) - GUARD*frame_bytes`,
    computed in u64 and tested as i64.
  - No room: count a stall unless `played` moved. After more than 50 stalls
    (each `sleep_ms(2)`, at least one 10 ms tick) give up and return what was
    written (about 0.5 s without movement).
  - Room: claim it (`written += run`) **before** copying, because
    `sound_poll` could land mid-copy. Copy with volume. Raise `zeroed` to
    `written`.

  Returns frames written.
* `sound_tone(hz, ms)` (303-349): `total = rate*ms/1000` frames, 512-frame
  blocks built on the stack (2 KiB). Phase step `(hz<<16)/rate`. A 5 ms
  linear ramp at each end (`rate/200` frames) removes the clicks. Each block
  goes through `sound_write`. It blocks for roughly the note's length.
* `sound_silence()` (351-357): memset the ring, advance,
  `written = zeroed = played`.
* `sound_init()` (359-391): `hda_init`, else `ens_init`. Frame bytes, ring,
  `snd_start(ring, ring_bytes, 48000)`, counters reset, `last_pos` from the
  hardware, `started_at = timer_ticks()`.

**Syscall exposure** (syscall.c:439-461, include/syscall.h:63-64, 203-208):
`SYS_SOUND_INFO 40` fills `sound_info_t {u32 present, rate, channels,
reserved}`. `SYS_SOUND_WRITE 41` takes (frames pointer, count), caps count at
4096 frames per call, checks the user range, and calls `sound_write`. There is
**no syscall for volume or tone**. Volume comes from the theme
(`theme_set_volume_live`, theme.c:462-469; `wm_run`, wm.c:3884). Tones come
from the kernel shell's `beep [hz] [ms]` (shell.c:517-531; 20..20000 Hz,
at most 10 s) and the WM's volume blip (`sound_tone(880, 70)`, wm.c:2618-2629,
3565). `/sys/devices` shows "sound <desc>, N Hz, N channels, position
timed here | from the controller" (sysfs.c:161-169).

### 3.11 include/fb.h + kernel/fb.c (framebuffer)

**Bochs VBE (BGA)** (fb.c:23-46): index port 0x01CE, data port 0x01CF.
Registers: `ID 0`, `XRES 1`, `YRES 2`, `BPP 3`, `ENABLE 4` (`DISABLED 0`,
`ENABLED 0x01`, `LFB 0x40`), `BANK 5`, `VWIDTH 6`, `VHEIGHT 7`, `XOFF 8`,
`YOFF 9`. Devices: QEMU/Bochs std VGA 1234:1111, VirtualBox 80EE:BEEF. The ID
must be in 0xB0C0..0xB0CF.

**State** (48-68): `active`, `via_svga`, `adopted`, `flush_cycles`,
`width`, `height`, `pitch` (bytes), `lfb` (the identity-mapped aperture),
`back` (the back buffer, or `lfb` itself when there was no memory),
`sent` (mirror of what the card holds), `sent_valid`.

**Band machinery** (350-381): `BAND_ROWS 16`, `MAX_BANDS 512` (8192 rows),
`band_dirty[512]`, `band_count`. `volatile helper_first, helper_last,
helper_done`. `volatile bool flushing`. Counters `frames`, `shared_frames`,
`last_sent`, `total_sent`, readable through accessors.

**Functions**

* `take_back_buffer(bytes)` (74-79): kmalloc; on failure `back = lfb` and
  return false.
* `take_sent(bytes)` (83-88): frees the old mirror, `sent_valid = false`,
  allocates a new one unless `back == lfb`. Failure only costs the
  comparison.
* `fb_backend()` (102-107): "none", "adopted from the loader", "set through
  the vmware adapter", or "set through vbe".
* `fb_adopt(base, w, h, pitch_pixels)` (119-143): `pitch = pitch_pixels*4`
  (32 bpp assumed; the handoff's `fb_bpp` and the GOP pixel format are
  ignored). `paging_map_wc(base, pitch*h)`: write-combining if PAT is
  available, otherwise uncached. Back buffer (the result is ignored, so it
  may draw straight to the screen), mirror, `active`, clear, flush.
* `init_svga(w, h)` (150-168): `svga_init`. Adopts its width, height, pitch
  and `fb_phys`. **Returns false if the back buffer kmalloc fails, leaving
  the SVGA adapter enabled.** Mirror, `via_svga`, clear, flush.
* `bga_present(out)` (171-176): the ID check plus a PCI match.
* `bga_mode(w, h)` (184-220): BAR0 & 0xFFFFFFF0. Disable, set X/Y/BPP 32,
  enable with LFB. Read X and Y back; a mismatch means the card refused (not
  enough memory), so disable and fail. Maps `w*4*h` bytes page by page with
  `map_page(.., PTE_PRESENT|PTE_RW)`, which is PAT slot 0 (write-back), not
  WC and not UC. Sets `pitch = w*4`. It deliberately does not touch the back
  buffer, so a failed mode change can go back.
* `fb_init(w, h)` (222-241): no BGA means `init_svga`. `bga_mode`. If the back
  buffer fails, **disable VBE and return false**, which contradicts the
  "tearing is better than nothing" comment at 70-73. Mirror, active, clear,
  flush.
* `fb_mode_settable()` (247): `active && !adopted && !via_svga`.
* `fb_set_mode(w, h)` (249-280): 640x480 to 4096x4096. Saves the old
  geometry. If `bga_mode` fails, restore and set the old mode again. A new
  back buffer is allocated **before** the old one is freed, so a failure
  leaves the screen as it was. New mirror, clear, flush. The only caller is
  `apply_screen_size` in wm.c:3179-3191.
* `fb_put`, `fb_get` (282-290): bounds-checked against the back buffer.
* `fb_clear(rgb)` (292-297): writes `width*height` u32s linearly, so it
  **assumes `pitch == width*4`**.
* `fb_rect` (299-308): clips with `x + w > width`, which can wrap in u32.
  `fb_frame` (310-316) draws four rects.
* `bands_copy(first, last)` (383-398): for each band, if `sent_valid` and
  memcmp(sent, back) is equal, mark it clean. Otherwise memcpy back to lfb
  and back to sent, and mark it dirty.
* `bands_helper(arg)` (400-408): runs on an AP. `bands_copy(helper_first,
  helper_last)`, `sfence` (WC stores cannot be flushed by the other CPU),
  then `helper_done = 1`.
* `whole_screen()` (410-416): memcpy the whole back buffer, sfence, account
  it, and `svga_update(full)` when on SVGA. **The mirror is not updated.**
* `fb_flush()` (418-506): see section 4.12.
* `fb_flush_rect(x, y, w, h)` (508-526): copies the rows to lfb **and** the
  mirror (so the mirror never lies about those rows), sfence, then
  `svga_update(rect)`.
* `fb_flush_cycles()` returns the TSC cycles of the first full flush.
  `fb_double_buffered()` is `active && back != lfb`.
* Accessors: `fb_active`, `fb_width`, `fb_height`, `fb_pitch`, `fb_pixels`
  (returns the **back** buffer, which the WM composites into), and the
  `fb_frames/shared_frames/last_sent/total_sent/screen_bytes` statistics.
  `RGB(r,g,b)` is `r<<16 | g<<8 | b`, which is BGRX byte order in memory.

### 3.12 include/svga.h + kernel/svga.c (VMware SVGA II)

**Types**: `svga_mode_t { u64 fb_phys; u32 fb_bytes; u32 pitch; u32 width,
height; }` (svga.h:18-23).

**Constants** (svga.c:23-60): device 15AD:0405. I/O index port at BAR0+0,
value port at BAR0+1. Registers: `ID 0`, `ENABLE 1`, `WIDTH 2`, `HEIGHT 3`,
`MAX_WIDTH 4`, `MAX_HEIGHT 5`, `BITS_PER_PIXEL 7`, `BYTES_PER_LINE 12`,
`FB_START 13`, `FB_OFFSET 14`, `VRAM_SIZE 15`, `FB_SIZE 16`,
`CAPABILITIES 17` (unused), `MEM_START 18`, `MEM_SIZE 19`, `CONFIG_DONE 20`,
`SYNC 21`, `BUSY 22`. `SVGA_ID(v) = 0x900000<<8 | v`. `SVGA_CMD_UPDATE 1`.
FIFO header words `MIN 0`, `MAX 1`, `NEXT_CMD 2`, `STOP 3`, header size
`FIFO_HEADER 16`.

**State**: `present`, `io_base`, `fifo`, `fifo_bytes`, `mode_w`, `mode_h`.

**Functions**

* `sync_fifo()` (82-86): `SYNC = 1`, then **an unbounded** busy-wait while
  `BUSY` reads non-zero.
* `fifo_put(v)` (88-110): if the next position would equal STOP (full),
  sync and re-read NEXT_CMD. Writes the word and advances NEXT_CMD, wrapping
  from MAX to MIN.
* `svga_update(x, y, w, h)` (112-124): clips, then pushes `UPDATE x y w h`
  and syncs. Fully synchronous.
* `svga_init(w, h, out)` (126-209):
  1. Find the device, `io_base = BAR0 & ~3`, enable bus master.
  2. Negotiate the ID: only version 2 is accepted.
  3. Check against MAX_WIDTH/HEIGHT.
  4. Disable, set width, height, 32 bpp.
  5. Read FB_START, FB_OFFSET, FB_SIZE, VRAM_SIZE, MEM_START, MEM_SIZE and
     BYTES_PER_LINE. Validate them, and verify that the width, height and
     depth took.
  6. Map the framebuffer (`paging_map_device`, uncached; the return value is
     only null-checked and the identity address is used) and the FIFO.
  7. Initialise the FIFO header (MIN = NEXT = STOP = 16, MAX = size),
     `CONFIG_DONE = 1`, `ENABLE = 1`.

  No FIFO capabilities, no cursor, no acceleration.

### 3.13 include/vga.h + kernel/vga.c (VGA text)

`enum vga_color` holds the 16 CGA colours (vga.h:4-8). vga.c:
`W 80`, `H 25`, `BUF = (volatile u16*)0xB8000`, `row`, `col`,
`color = 0x07`.

* `vga_set_color(fg, bg)` (17-20): sets `color = fg | bg<<4` and forwards to
  `fbcon_set_color` if the framebuffer is active. This is how printf colours
  reach fbcon. `vga_get_color()`.
* `move_hw_cursor()`: CRTC 0x3D4/0x3D5, registers 14 and 15.
* `vga_clear()`: goes to fbcon if active, otherwise fills with spaces.
  `vga_init()` sets light grey on black and clears; it is the first thing
  kmain does (main.c:308).
* `vga_putc(c)` (46-63): handles `\n`, `\r`, `\t` (to the next multiple of
  4), and `\b` (moves back, wrapping up a line, and erases). It always writes
  to 0xB8000. Routing is done by `kputc` (printf.c:16-24): fbcon when the
  framebuffer is active, otherwise vga. Scrolls at row 25.
* `vga_cursor(r, c)`, `vga_get_cursor(r, c)` (67-72).
* `cell(c, attr)` is `(u16)c | attr<<8` with a **signed** `char`, so bytes
  0x80-0xFF sign-extend and replace the attribute with 0xFF.

### 3.14 include/fbcon.h + kernel/fbcon.c (text console on the framebuffer)

`PALETTE[16]` (fbcon.c:12-29) is a softened CGA palette. Black is lifted to
`RGB(0x14,0x18,0x1D)`, white is `RGB(0xF2,0xF5,0xF7)`. State: `cols`, `rows`,
cursor `cx`, `cy`, `fg` (7), `bg` (0), `cursor_shown`.

* `fbcon_init()` (36-44): `cols = fb_width()/8`, `rows = fb_height()/16`
  (so 128x48 at 1024x768, as the selftest log shows), clear to the
  background colour, full flush. Called at boot (main.c:397) and after a mode
  change (wm.c:3189).
* `draw_cell(col, row, ch, f, b)` (51-65): glyphs 32..126 come from
  `font8x16`, drawn pixel by pixel with `fb_put`. Anything else (including
  signed bytes above 127) is a background block.
* `flush_cell` flushes one 8x16 rect. `hide_cursor` / `show_cursor` draw a
  2-pixel underline in fg or bg.
* `scroll()` (84-94): memmove the back buffer up by one text line
  (`16 * pitch` bytes), clear the last line, `fb_flush()` (the band diff
  sends only what changed).
* `fbcon_putc(c)` (96-122): `mouse_hide()` and `hide_cursor()` first,
  because the sprite saves pixels under itself. Handles
  `\n \r \t \b`, draws and flushes the cell, wraps, scrolls, then
  `show_cursor()` and `mouse_show()`.
* `fbcon_clear()`, `fbcon_set_color()`, `fbcon_cols()`, `fbcon_rows()`.

While the desktop runs, kernel prints still draw over the desktop through
fbcon until the WM composites the next frame. `kputc` does not know the WM
exists.

### 3.15 include/clipboard.h + kernel/clipboard.c

`CLIP_MAX 65536` (clipboard.h:16). State: `static char buffer[65536]`,
`length`, `generation`.

* `clip_init()` zeroes everything. It is called from main.c:609, and by
  selftest before and after its section.
* `clip_set(text, len)` fails on a NULL text or `len > CLIP_MAX-1`. Otherwise
  memcpy, NUL-terminate, set the length, `generation++`.
* `clip_get(out, cap)` copies `min(length, cap-1)` bytes and terminates.
  Returns 0 when `cap == 0`.
* `clip_len()`. `clip_generation()` has no runtime caller (only selftest).

Syscalls (syscall.c:423-437): `SYS_CLIP_SET 38 (ptr, len)` rejects
`len > CLIP_MAX`, checks the user range, returns len or -1.
`SYS_CLIP_GET 39 (ptr, cap)`: `cap == 0` returns the length (the SDK's
`clip_len()`), otherwise copies out. `/sys/clipboard` (sysfs.c:263-271) shows
the text or "(empty)". The text is copied straight from and to user memory,
with no lock beyond the kernel lock.

### 3.16 include/pins.h + kernel/pins.c

`PIN_MAX 8`, `PIN_LABEL 16`, `PIN_PATH 32`, `PIN_FILE "/zelr.pins"`,
`pin_t { char label[16]; char path[32]; }` (pins.h:14-22).

File format (pins.c:102-104, 141-158): a header line
`# zelr taskbar, one program a line`, then one `path label` pair per line.
The path cannot contain spaces. The label runs to the end of the line. Lines
starting with `#` are comments. A missing or empty file (`vfs_read <= 0`)
gives the six defaults: Terminal `/bin/term`, Files `/bin/files`, Notes
`/bin/notes`, Paint `/bin/paint`, Settings `/bin/settings`, Browser
`/bin/browser`. A file containing only the header gives an empty list,
because somebody emptied it. The file is read into a 512-byte buffer
(511 bytes used).

* `append()` (51-61): refuses a full list, an empty path, or a duplicate
  path. An empty label becomes the basename of the path. Truncates with
  `strncpy`.
* `pins_add()` writes the file. `pins_remove()` writes the file.
* `pins_move(from, to)` (78-88) clamps `to` and does **not** save: the drag
  saves once when it ends.
* `pins_reload()` (131-139) re-reads the file and returns whether anything
  changed.
* `pins_save()` (141-158): at most 35 + 8*48 = 419 bytes, so it fits in the
  512-byte buffer.

**Nothing outside `kernel/selftest.c:998-1055` includes pins.h or calls
pins_***. The desktop dropped its pinned row (README.md:843-848), yet
wm.c:91 ("The badge, then the pinned apps, then a chip for each remaining
window") and wm.c:2140-2148 (an empty "apps on the panel" section) still
describe it.

---------------------------------------------------------------------------

## 4. Control flow and lifecycles

### 4.1 Boot order (kernel/main.c)

| main.c line | Step |
|---|---|
| 308 | `vga_init()`, the text console, before anything else prints |
| 348 | `paging_init_pat()`, PAT slot 4 becomes WC, "before anything is mapped" |
| 391-412 | Video. `h->fb_base` set means `fb_adopt(fb_base, fb_width, fb_height, fb_pitch)` (UEFI). Otherwise `fb_init(1024, 768)` (multiboot and BIOS both zero `fb_base`). On success, `fbcon_init()` and the boot log line, e.g. `video 1024x768 32bpp, set through vbe, full flush 93877 kcycles, write combining` from the selftest log. On failure, "video none: no vbe and no vmware adapter, vga text only" |
| 459 | `timer_init(100)`: PIT programmed, IRQ0 unmasked, but IF is still 0 so there are **no ticks until `sched_start`** |
| 488 | `smp_init()`: APs come up and idle; the scheduler is not started yet |
| 511 | `ps2_init()`, before both drivers ("neither can be trusted to leave it in a state the other one needs") |
| 512 | `keyboard_init()` |
| 513 | `if (fb_active() && mouse_init())`. **No PS/2 mouse without a framebuffer.** `syn_detect()` runs inside, with interrupts still off as required |
| 523 | `usb_init()`, after keyboard and mouse "because what it finds is handed to them". This is also where xHCI ownership is taken from the firmware, so until here a firmware's USB-legacy PS/2 emulation may have been what `ps2_init` and `mouse_init` talked to |
| 527-546 | `sound_init()`. On failure, lists undriven class 04/01 and 04/03 devices |
| 548 | `serial_enable_irq()` |
| 555-563 | Pointer summary: "pointer synaptics M.m, caps ...", "pointer ps/2 mouse with a wheel", or "pointer none found" |
| 589-605 | IOAPIC routes each of IRQs 0..15 that has a handler to the BSP (`ioapic_route_irq` uses `lapic_id()` of the caller). The selftest log shows "5 routed" |
| 609 | `clip_init()` |
| 613 | `usb_start_service()`, the "usb" task, once the scheduler exists |
| 646 | `sched_start()`, interrupts on |

`pins_init()` is never called at runtime.

### 4.2 Timer tick (timer.c:16-44)

PIT IRQ0 arrives on the BSP only (IOAPIC routing), inside `isr_dispatch`
holding the kernel lock with IF=0: `ticks++`, then `usb_poll()`
(xhci_poll, drain_events, HID callbacks), `ps2_poll_from_timer()`,
`syn_tick()`, `sound_poll()` (advance, then zero ahead), `rng_tick()`. The
dispatcher then calls `scheduler_switch` (idt.c:256-258). An AP's LAPIC timer
(`VEC_LOCAL_TIMER`) does **not** run `on_tick`.

### 4.3 PS/2 bring-up

`ps2_init` (ps2.c:142-201):
1. `present = false`, `rescued = 0`.
2. `first_status = inb(0x64)`. 0xFF means "ps/2 no controller answers", and
   return.
3. `first_config = ps2_config()`, read *before* anything is switched off.
4. 0xAD, 0xA7, `drain()`.
5. `write_config((first_config | 0x01 | 0x02 | 0x40) & ~0x30)`. In the
   selftest log, 0x61 becomes 0x43.
6. 0xAE, 0xA8.
7. Keyboard `0xF4` (enable scanning), then drain the ACK.
8. Keyboard `0xEE` (echo), read the reply; "answers" if it is 0xEE or 0xFA;
   drain.
9. `present = true`. `bb_log("ps/2 status %x, config %x to %x, keyboard
   answers|silent")`. The selftest log line is
   `ps/2 status 1c, config 61 to 43, keyboard answers`.

`mouse_init` (mouse.c:256-291):
1. Reset state; return false if `!ps2_present()`.
2. `0xF6` (set defaults).
3. `syn_detect()`. If a pad is found, `packet_len = 6`. Otherwise
   `enable_wheel()` gives 4 or 3.
4. `0xF4` (enable reporting).
5. Centre the pointer on the framebuffer.
6. Vector 44, unmask IRQ2 (cascade) and IRQ12.
7. `present = true`, `mouse_show()`.

None of the ACKs is checked.

### 4.4 One PS/2 keystroke

1. The keyboard sends a set-2 make code. The 8042 translates it to set 1
   (CFG_TRANSLATE), sets OBF, and raises an IRQ1 edge.
2. The BSP takes vector 33. `isr_dispatch` acquires the kernel lock if this
   CPU does not already hold it (idt.c:166-188). `keyboard.c:on_irq` calls
   `ps2_poll`.
3. `ps2_poll` reads STATUS (AUX = 0), then DATA, and calls `keyboard_byte(sc)`.
4. `keyboard_byte` tracks modifiers, maps the code, and calls `push(c)`.
   `push` ORs in `KEY_MOD_*` from the current state, raises
   `signal_interrupt()` if the code is 3, and enqueues into `buf[]`.
5. EOI (LAPIC once the IOAPIC routes, otherwise the PIC).
6. A consumer calls `kbd_trygetchar()`:
   * **WM** (wm.c:3952-3989), one key per pass:
     - ESC closes the find bar, then the menu, then leaves the desktop.
     - `handle_shortcut`: ctrl+f is code 6; the alt chords are alt+tab,
       alt+d, alt+m, alt+f, alt+q and alt+arrows.
     - Then `find_key`, then `menu_key`.
     - Otherwise the topmost unminimised window gets `WM_EV_KEY` carrying
       `KEY_CODE | (c & KEY_MOD_CTRL)`. Shift is already folded into the
       character and alt belongs to the desktop, so ring-3 programs only ever
       see the CTRL bit. A kernel window gets `on_key((char)code)` instead.
   * **Console fd 0** (fd.c:214-266): special keys are skipped. It handles
     `\n`, `\b`, ^D (end of input) and ^C (prints "^C", returns an empty
     line). Printable ASCII is echoed. It waits in `task_idle_wait` with
     interrupts briefly enabled.
   * **Kernel shell** (shell.c:612-629): `KEY_CODE`, printable only.

   If the IRQ1 edge was lost, the next PIT tick's `ps2_poll_from_timer` runs
   the same path.

### 4.5 One USB keystroke

1. `xhci_listen` left one Normal TRB queued on the interrupt-IN ring. At the
   endpoint's interval the controller writes the report into `d->buf` and
   posts a Transfer Event.
2. On the next PIT tick: `usb_poll`, `xhci_poll` (cli), `drain_events`.
   The event is `TRANSFER_EVENT(slot, dci)`, so `xfer_code`, `xfer_left` and
   `xfer_done` are updated and `report_cb(slot, dci, residual)` is called.
3. `on_report` finds the `device_t` and computes `len = length - residual`.
   Then `on_keyboard`: stamp from the modifier byte; for each usage not seen
   in `last`: `translate`, the ctrl fold, `keyboard_inject(key|stamp)`. Then
   `keyboard_set_mods`, copy the report into `last`, and re-queue with
   `xhci_listen`.
4. The consumers are the same as 4.4.

Latency is up to one 10 ms tick plus the endpoint's polling period. See
section 10 for how the interval is programmed.

### 4.6 Pointer input

* **PS/2 mouse**: IRQ12 (vector 44), `ps2_poll` (AUX=1), `mouse_byte`. After
  3 or 4 bytes, `on_packet`, then `mouse_inject(dx, dy, btns & 7)` and
  optionally `mouse_inject_scroll(z)`.
* **USB mouse**: the tick, then `on_mouse`, then `mouse_inject(dx, -dy,
  btns)` and `mouse_inject_scroll(-dz)`.
* **Synaptics contact state machine** (synaptics.c:238-312), one step per
  6-byte report:
  - `z < 30`: if touching, `contact_ended()` (tap test, reset, publish);
    otherwise publish buttons only.
  - Not touching and `z >= 30`: start the contact. Origin = (x,y),
    `fingers = most_fingers = f(w)`, `touch_began = ticks`,
    `travelled = 0`. Publish buttons only; nothing moves.
  - Touching and the finger count changed: re-origin, clear the remainders,
    raise `most_fingers`, publish buttons only. The clock is **not** reset,
    so a resting finger plus a new one is not a tap.
  - Touching, same count, `|dx|` or `|dy|` above 700: the origin has already
    moved; publish buttons only.
  - Touching with two or more fingers: `travelled += |dx|+|dy|`;
    `steps = scaled(-dy, 120)` goes to `mouse_inject_scroll`; publish
    buttons only.
  - Touching with one finger: `travelled += ...`;
    `publish(scaled(dx,6), scaled(dy,6))`.
  - Tap: when the contact ends within 20 ticks, having travelled under 120
    units, `tap_button` becomes 0x01 (one finger) or 0x02 (two or more) and
    is held until `ticks >= tap_release_at` (8 ticks), when `syn_tick`
    releases it.
* **Consumers**: the WM drains `mouse_take_edge` first (each edge dispatched
  at its own position), then reads the level with `mouse_x/y/buttons`, then
  `mouse_take_scroll`. The wheel goes to the volume control if the pointer is
  over it (`theme_set_volume(volume - wheel*5)` plus a blip), otherwise
  `WM_EV_SCROLL` to the window under the pointer, not the focused one
  (wm.c:3897-3950). With the WM running, `mouse_set_autodraw(false)`
  (wm.c:3890) means the WM draws the pointer. In console mode the driver
  draws the sprite itself, from interrupt context.

### 4.7 xHCI bring-up (`xhci_init`, xhci.c:487-560)

1. `pci_find_class(0x0C, 0x03, 0x30)`: the first xHCI only.
2. BAR0 plus BAR1 as a 64-bit address. `pci_enable_bus_master`, and also
   `cmd |= 0x6`, which is redundant.
3. `cap = paging_map_device(bar, 64 KiB)`.
4. Read CAPLENGTH and HCIVERSION as one dword. `op = cap + caplen`,
   `rt = cap + (RTSOFF & ~0x1F)`, `db = cap + (DBOFF & ~3)`.
5. From HCSPARAMS1: `nslots` (capped at 15) and `nports` (capped at 32, must
   be non-zero). `ctx_stride = HCCPARAMS1 & 4 ? 64 : 32`.
6. `take_ownership`, then `reset_controller`, then `make_rings`.
7. `CONFIG = nslots`, `DCBAAP`, `CRCR = ring | 1`.
8. `ERSTSZ = 1`, `ERDP = ring | EHB`, `ERSTBA`, `IMOD = 0`. IMAN.IE and
   USBCMD.INTE are never set.
9. `USBCMD |= RUN`, then wait for !HALTED.
10. Power every unpowered port (`PORT_KEEP | PP`), then 20 ms.
11. `present = true`. The description is "xhci M.m, N port(s), N slot(s)",
    plus a `bb_log`.

### 4.8 USB enumeration of one device

The entry point is `setup_port(p)` for a root port, or `walk_hub` for a hub
port.

1. **Root port**: `reset_port`, which writes PR, waits for PRC, acks PRC,
   and needs PED. Then `where = {p, 0, PORTSC speed, 0, 0}`.
2. `xhci_attach`: Enable Slot, set up the contexts, Address Device (the
   controller sends SET_ADDRESS).
3. `GET_DESCRIPTOR(DEVICE, 8)`. If `bMaxPacketSize0` is neither 0 nor 8, an
   Evaluate Context updates EP0's max packet size.
4. `GET_DESCRIPTOR(CONFIG, 6)`, then `total_length`, which must be 1..512.
5. `GET_DESCRIPTOR(CONFIG, total)`, then `SET_CONFIGURATION(bConfigurationValue)`.
6. Device class 9 (hub) goes to `walk_hub`:
   - Hub descriptor (0x29, or 0x2A for SuperSpeed).
   - `xhci_mark_hub`.
   - SET_FEATURE(PORT_POWER) on every port, then wait
     `bPwrOn2PwrGood*2 + 20` ms.
   - Per port: `reset_hub_port` (SET_FEATURE(PORT_RESET), poll GET_STATUS,
     clear C_RESET and C_CONNECTION), build the child's route string and TT,
     then recurse (tier + 1, at most 5).
7. Anything else goes to `claim_interface`:
   - A HID boot interface: open the interrupt EP, SET_PROTOCOL(0),
     SET_IDLE(0), `xhci_listen`.
   - MSC BBB: open both bulk EPs, lower DCI first. `usbdisk_attach`, then
     `diskfs_mount_removable`.
   - CDC data after a CDC comm interface: open both bulk EPs, `usbnet_attach`,
     then `net_init()`.
   - Anything else is not claimed, and its slot stays enabled.

### 4.9 Hotplug and removal

1. The controller posts a Port Status Change event. The next tick's
   `drain_events` sets `port_changed`.
2. The "usb" task wakes every 300 ms. `xhci_took_port_change()` returns true
   once, so it calls `rescan()`.
3. For each root port: connected and not claimed means `claimed = true`,
   `setup_port`, `describe`. Disconnected and claimed means
   `claimed = false`, `forget_root(p)`, `describe`.
4. `forget_root(p)` walks only the HID `device_t` table. For each keyboard or
   mouse whose `root == p`: decrement the count, call
   `diskfs_unmount_removable()` unconditionally, call
   `usbdisk_detach(slot)` (a no-op unless the slot is the stick's), call
   `xhci_detach(slot)`, free the entry.

   **Sticks, hubs, network adapters and unclaimed devices are never
   detached** (section 10).

Hub ports are never watched; the hub's status-change interrupt endpoint is
never opened. The PORTSC change bits other than PRC are never acknowledged.

### 4.10 USB stick lifecycle

* **Attach** (from `claim_interface`):
  1. Open both bulk endpoints.
  2. `usbdisk_attach`: `wait_ready` (up to 20 rounds of TUR / REQUEST SENSE /
     50 ms), `inquiry`, `read_capacity`, `blk_id = blk_register(&USB_DEV)`,
     "usb disk ..., N sectors of N bytes, disk N".
  3. `ndisks++`.
  4. `diskfs_mount_removable(blk_id)` (diskfs.c:52-78): read sector 0 into a
     512-byte **stack** buffer. If there is an MBR signature, try each
     partition whose type is FAT (01, 04, 06, 0B, 0C, 0E, EF). Otherwise
     mount the whole device (`fat_mount_on(FAT_VOL_USB, ...)`). Files then
     appear under `/usb`.
* **Read or write** (for example `cp /usb/x .`): VFS, FAT, `blk_read_on(id,
  lba, count, buf)`, which splits into runs of `max_run() = 8` sectors, each
  going to `usbdisk_read`, `rw10(READ10)` and `transact`: `xhci_bulk(out,
  CBW)`, `xhci_bulk(in, data)`, `xhci_bulk(in, CSW)`. Each bulk transfer is
  spin-waited for up to about 5 s. On failure, `clear_sense`.
* **Shell**: `stick` prints model and size. `stick read <lba>` dumps the first
  16 bytes. `stick write <lba> <byte>` fills a sector (shell.c:532-570).
* **Removal**: not handled (section 10).

### 4.11 Sound lifecycle

* **Init** (`sound_init`): HDA, else Ensoniq. Allocate the 64 KiB ring.
  `snd_start(ring, 64 KiB, 48000)`. Hardware then loops over the ring forever.
* **Each tick** (`sound_poll`): `advance()` turns the hardware position into
  the monotonic `played` (or into clocked time), then zeroes everything from
  the end of queued audio to one full ring ahead of the play position.
* **Writer** (`sound_write`, reached from `SYS_SOUND_WRITE`, `sound_tone` in
  the shell `beep` and the WM blip): claim room, copy with volume. When full,
  `sleep_ms(2)` and retry, giving up after 50 stalls with no progress.
* **Clock fallback**: the first `advance()` more than 0.25 s after start that
  has never seen the position change sets `clocked`. From then on `played`
  is computed from ticks. The mode is permanent until reboot.

### 4.12 One frame (`fb_flush`, fb.c:418-506)

1. Inactive: return. `back == lfb` (no back buffer): only the
   `svga_update(full)` for SVGA, then return.
2. `t0 = rdtsc()`, `frames++`.
3. No mirror, or `flushing` already set (re-entered): `whole_screen()`, go to
   the end.
4. `band_count = ceil(height/16)`. Above 512: `whole_screen()`.
5. `flushing = true`.
6. If `band_count >= 8` and `smp_helper()` names an idle AP: the helper takes
   bands `0 .. band_count/2 - 1`, `helper_done = 0`, `smp_run(helper,
   bands_helper)` (an IPI wakes it; the AP runs the job **without the kernel
   lock**, smp.c:300-311). This CPU takes `band_count/2 .. end`. If
   `smp_run` fails, this CPU does everything.
7. `bands_copy(mine..end)`, then sfence.
8. With a helper: spin on `helper_done`, at most 20,000,000 `pause`es. On
   timeout, do the helper's half here (writing the same bytes to the same
   place is harmless). Otherwise `shared_frames++`.
9. `sent_valid = true`, `flushing = false`.
10. `last_sent` = the sum of the dirty band sizes. `total_sent += last_sent`.
11. SVGA: one `svga_update(0, y, width, h)` per run of adjacent dirty bands.
12. The first flush ever records `flush_cycles`.

### 4.13 Changing the mode at run time

The settings program writes `want_w` and `want_h` into `/zelr.cfg`. The WM
re-reads the theme four times a second (wm.c:3991-3994) and calls
`apply_screen_size()` (wm.c:3179-3191), which returns early unless
`fb_mode_settable()`. `fb_set_mode(w,h)`: `bga_mode` (X/Y read back), a new
back buffer before the old one is freed, a new mirror, clear, flush. Then
`fbcon_init()` (a new console geometry) and `screen_changed()`.
`/sys/screen` shows `settable 1` only for the VBE backend
(sysfs.c:290-302).

### 4.14 Copy and paste (as clipcheck.py exercises it)

1. In the desktop terminal the user presses ctrl+c.
2. PS/2 `push(3 | CTRL)`, which also raises `signal_interrupt()` (see
   section 11).
3. The WM delivers `WM_EV_KEY(3 | KEY_MOD_CTRL)` to the terminal.
4. The terminal calls `SYS_CLIP_SET` with its selection or input line, then
   `clip_set`.
5. ctrl+v: `SYS_CLIP_GET` (cap 0 for the length, then the copy).
6. The result can be checked from outside with `cat /sys/clipboard`.

---------------------------------------------------------------------------

## 5. Interfaces

### 5.1 What each module exports and who calls it (from grep)

| Export | Callers |
|---|---|
| `ps2_init` | main.c:511 |
| `ps2_poll` | keyboard.c:162 (IRQ1), mouse.c:253 (IRQ12), ps2.c:139 |
| `ps2_poll_from_timer` | timer.c:30 |
| `ps2_present` | mouse.c:265, synaptics.c:89 |
| `ps2_command`, `ps2_write_data`, `ps2_read` | mouse.c:123-149, synaptics.c:54-77 |
| `ps2_config` | ps2.c only |
| `ps2_first_status`, `ps2_first_config`, `ps2_rescued` | **none** |
| `keyboard_init` | main.c:512 |
| `keyboard_byte` | ps2.c:130 |
| `keyboard_inject`, `keyboard_set_mods` | usb.c:216, usb.c:219 |
| `kbd_trygetchar` | wm.c:3952, fd.c:226, shell.c:613 |
| `kbd_has_char` | fd.c:425 (poll) |
| `kbd_getchar`, `kbd_alt`, `kbd_ctrl`, `kbd_shift` | **none** |
| `KEY_*`, `KEY_MOD_*` | wm.c, fd.c, shell.c, apps; SDK mirrors for ring 3 |
| `mouse_init` | main.c:513 |
| `mouse_byte` | ps2.c:129 |
| `mouse_inject` | synaptics.c:209, usb.c:237, selftest |
| `mouse_inject_scroll` | synaptics.c:305, usb.c:246 |
| `mouse_take_edge` | wm.c:3909, selftest |
| `mouse_take_scroll` | wm.c:3935, selftest |
| `mouse_x`, `mouse_y`, `mouse_buttons` | wm.c:3891-3917, shell.c:341-345, main.c:516 |
| `mouse_set_autodraw` | wm.c:3890 (off), wm.c:4077 (on) |
| `mouse_hide`, `mouse_show` | fbcon.c:99-131 |
| `mouse_present` | main.c:559, shell.c:340, selftest |
| `mouse_has_wheel` | main.c:515, 558, 561 |
| `mouse_moves` | shell.c:346 |
| `syn_detect` | mouse.c:273 |
| `syn_byte` | mouse.c:244, selftest |
| `syn_tick` | timer.c:34, selftest |
| `syn_present` | mouse.c:105, 244; main.c:555; selftest |
| `syn_major`, `syn_minor`, `syn_capabilities` | main.c:557 |
| `syn_answer_is_pad`, `syn_reset_state` | selftest (and synaptics.c) |
| `syn_packets`, `syn_fingers` | **none** |
| `xhci_*` | usb.c (everything); usbdisk.c (`xhci_bulk`); usbnet.c:98, 108 (`xhci_control`), 236, 245 (`xhci_bulk`) |
| `usb_init` | main.c:523 |
| `usb_poll` | timer.c:23 |
| `usb_start_service` | main.c:613 |
| `usb_present`, `usb_describe` | main.c:524, sysfs.c:182-183 |
| `usb_reports` | sysfs.c:183 |
| `usb_keyboards` | sysfs.c:186 |
| `usb_mice`, `usb_hubs`, `usb_disks` | **none** |
| `usbdisk_attach`, `usbdisk_detach`, `usbdisk_blk_id` | usb.c:551, 689, 558 |
| `usbdisk_present/sectors/block_size/model` | sysfs.c:188-190, shell.c:540-545 |
| `usbdisk_read`, `usbdisk_write` | shell.c:555, 565; block layer through `USB_DEV` |
| `hda_*`, `ens_*` | sound.c only |
| `sound_init` | main.c:527 |
| `sound_poll` | timer.c:38 |
| `sound_write` | syscall.c:460 (`SYS_SOUND_WRITE`), sound_tone |
| `sound_tone` | shell.c:530 (beep), wm.c:2628, 3565 |
| `sound_set_volume` | theme.c:467, wm.c:3884 |
| `sound_present`, `sound_rate`, `sound_channels`, `sound_describe`, `sound_clocked`, `sound_played` | main.c, syscall.c:442-444, sysfs.c:164-167, wm.c, shell.c, selftest |
| `sound_volume`, `sound_silence` | **none** |
| `fb_init`, `fb_adopt` | main.c:393-394 |
| `fb_set_mode`, `fb_mode_settable` | wm.c:3186-3187; `fb_mode_settable` also sysfs.c:294 |
| drawing (`fb_put/get/rect/frame/clear/flush/flush_rect/pixels/width/height/pitch`) | wm.c (152 uses), gfx.c (47), fbcon.c, mouse.c, blackbox.c, winsrv.c:198, syscall.c:1007-1008 (sysinfo screen size), apps.c:44 |
| `fb_backend`, statistics | main.c:403-407, sysfs.c:290-301 |
| `svga_*` | fb.c only |
| `vga_*` | printf.c:20 (`kputc`), main.c, blackbox.c:196-205 (panic screen without fb) |
| `fbcon_*` | printf.c:19, main.c:397-402, wm.c:3189, vga.c:19, 30, selftest |
| `clip_*` | syscall.c:425-437 (38/39), sysfs.c:264 (`/sys/clipboard`), main.c:609, selftest |
| `pins_*`, `pin_at` | **selftest only** |

### 5.2 User-visible surfaces

* **Syscalls**: 38 `SYS_CLIP_SET`, 39 `SYS_CLIP_GET`, 40 `SYS_SOUND_INFO`, 41
  `SYS_SOUND_WRITE` (include/syscall.h:58-64, sdk/zelr.h:99-104, 773-802).
  Screen size is in the sysinfo struct (syscall.c:1007-1008). Keys and mouse
  reach ring 3 only as window-server events (`WM_EV_KEY` carrying the CTRL
  bit only, `WM_EV_SCROLL`) or as console reads.
* **/sys**:
  - `/sys/devices` (sysfs.c:153-192): sound, video, usb (with the report
    count), keyboard "ps/2[ and usb]", usbdisk.
  - `/sys/screen` (sysfs.c:290-302): width, height, settable, source, frames,
    shared, lastkib, fullkib, sentkib.
  - `/sys/clipboard` (sysfs.c:263-271).
* **Kernel shell**: `mouse`, `beep [hz] [ms]`, `stick [read|write] <lba>
  [byte]`.
* **Boot log (black box)**: "ps/2 status ...", "xhci up: ...", "usb keyboard on
  slot N ep N", "usb hub on slot N, tier N", "usb disk ...", "usb network ...",
  "usb device on port N unplugged", "audio hda codec ...", "hda: ..." and
  "ens: ..." failures, "video ...".

### 5.3 What this area depends on

* **io.h**: port I/O, `io_wait` (port 0x80, about 1 us), `cli`/`sti`,
  `interrupts_enabled`, `rdtsc`.
* **pci**: `pci_find_class`, `pci_find`, `pci_read32/write32`,
  `pci_enable_bus_master` (sets I/O, memory and bus-master, pci.c:217-223).
* **paging**:
  - `paging_map_device` identity-maps with `PTE_NOCACHE` (PCD|PWT = 0x018,
    so uncached) and skips pages already mapped (paging.c:567-575).
  - `paging_map_wc` unmaps and remaps with `PTE_WC` (0x080, the PAT bit in a
    4 KiB PTE, selecting PAT slot 4, which is WC after `paging_init_pat`), or
    falls back to `paging_map_device` (paging.c:551-565).
  - Also `map_page`, `virt_to_phys`.
* **heap**: `kmalloc` (8-byte alignment, heap.c:41) and `kfree`. Much device
  memory is never freed.
* **idt/pic**: `register_interrupt_handler`, `pic_unmask`. Final routing is
  done by the IOAPIC in main.c.
* **timer/sched**: `timer_ticks`, `timer_hz`, `sleep_ms` (a hlt loop),
  `task_sleep`, `task_current`, `task_create`, `task_idle_wait`.
* **smp**: `smp_helper`, `smp_run`.
* **signal**: `signal_interrupt`. **serial**: `serial_trygetc`.
* **blockdev**: `blk_register/unregister`. **diskfs**:
  `diskfs_mount_removable/unmount_removable`. **usbnet**: `usbnet_attach`.
  **net**: `net_init`.
* **vfs**: `vfs_read/vfs_write` (pins). **font.h**: `font8x16`, 8x16, glyphs
  32..126. **blackbox**: `bb_log`. **printf**: `kformat`, `kprintf`.
* **Boot handoff**: `h->fb_base`, `fb_width`, `fb_height`, `fb_pitch` (in
  pixels), `fb_bpp` (include/handoff.h:39-44; set only by uefi/loader.c:131-135).

---------------------------------------------------------------------------

## 6. Concurrency, locking, memory ownership, invariants

**The execution contexts that matter**

* **The PIT tick** (`on_tick`) runs on the BSP only, with IF=0 and the kernel
  lock held. The dispatcher acquires it on entry if this CPU does not already
  have it (idt.c:166-188). All device IRQs 0-15 are also routed to the BSP.
* **Kernel tasks** (the WM runs inside the kernel shell task; also the "usb"
  task, the net service, selftest) always run on the BSP (sched.c:423-427,
  464). On the BSP they hold the lock and are preemptible.
* **Ring-3 syscalls** can run on APs holding the lock (idt.c:150-158). An AP
  switching to another ring-3 task releases it (idt.c:287).
* **`bands_helper`** runs on an AP **without** the kernel lock, with
  interrupts on (smp.c:300-311). It touches only the back buffer, the mirror,
  the lfb, `band_dirty[0..half-1]` and `helper_done`.

Because the kernel lock serialises everything else, the device code has
almost no locks of its own. Specifically:

* **Key ring** (keyboard.c): single producer (IRQ1, the timer, or
  `xhci_poll` callbacks) and single consumer (tasks), with volatile indices,
  no lock. The kernel lock already keeps producer and consumer off different
  CPUs at the same time. A full ring drops new keys; `push` still raises
  SIGINT.
* **Mouse edges**: the same SPSC arrangement. A full queue drops new edges.
  `mouse_take_scroll` clears `wheel` with IF off; `mouse_inject_scroll`
  modifies it from interrupt context.
* **xHCI**: one consumer of the event ring, `drain_events`, always entered
  with IF=0 through `xhci_poll`. Synchronous waits (`command`,
  `xhci_control`, `xhci_bulk`) poll the ring themselves, so they complete
  even when no tick can arrive. **Invariant: one command outstanding at a
  time**, because `cmd_done` is a single flag. Transfer completion is
  detected by a per-`[slot][dci]` counter changing, not by matching TRB
  addresses, so a late completion from a timed-out transfer is credited to
  the next one.
* **Memory handed to devices must be identity mapped.** xhci.c checks this
  for rings (`make_rings`) and for data buffers (`xhci_control`, `xhci_bulk`,
  `xhci_listen`). hda.c and ens.c assume it (kmalloc'd ring); ens.c checks
  that the buffer is below 4 GiB. usb.c uses a heap bounce buffer for every
  control transfer. usbdisk's CBW, CSW and staging buffers are on the heap,
  but caller data (for example diskfs' 512-byte stack buffer) goes to the
  device directly.
* **Memory never freed**: xHCI rings, contexts, DCBAA, scratchpad pages and
  the event ring (`alloc_aligned`). Rings and contexts are reused per slot.
  The HDA CORB, RIRB and BDL (a new BDL on every `hda_start`). usbdisk
  cbw/csw/stage. usb.c HID report buffers (`kmalloc(32)` per claimed device,
  never freed on unplug). The sound ring. The framebuffer back buffer and
  mirror are freed on mode change.
* **Sound bookkeeping**: `advance()` runs with IF off, because the tick and a
  writer both call it. `sound_write` claims space (`written += run`) before
  copying, so the tick's zeroing cannot erase bytes being written.
  Invariant: `played <= written`, and `zeroed >= written` after writes
  (sound.c:208, 298).
* **fb flush**: guarded against re-entry by `volatile bool flushing`, which
  is a plain flag and not atomic. A re-entrant caller sends the whole screen.
  `fb_flush_rect` keeps the mirror consistent for the rows it writes.
  Invariant intended: after `sent_valid = true`, `sent == what the card
  holds`. `whole_screen()` breaks it (section 10).
* **PS/2 at boot**: all command/response exchanges (`ps2_init`,
  `mouse_init`, `syn_detect`) run with IF=0 before `sched_start`. Nothing
  else reads port 0x60 at that point. After boot, only `ps2_poll` reads it.
* **Console-mode pointer sprite**: `mouse_inject` from IRQ context calls
  `mouse_hide`/`mouse_show`, which read and write the back buffer and flush
  rects. `fbcon_putc` brackets its own drawing with hide/show, but an IRQ
  landing mid-glyph can save half-drawn pixels under the sprite. Cosmetic.
* **Waiting primitives**:
  - xhci.c and hda.c spin with `io_wait`. ens.c spins on port 0x61 bit 4.
  - usbdisk's `settle_ms` spins.
  - usb.c's `wait_ms` uses `task_sleep` when a task exists.
  - sound.c uses `sleep_ms`, which halts until `ticks` changes. `ticks`
    changes only on the BSP's PIT handler, which needs the kernel lock
    (section 10, item B1).

---------------------------------------------------------------------------

## 7. Limits and magic numbers

| What | Value | Where |
|---|---|---|
| 8042 wait bound | 100,000 status reads | ps2.c:57 |
| Bytes per `ps2_poll` / `drain` | 64 | ps2.c:106, 120 |
| Key ring | 256 ints (255 usable) | keyboard.c:13 |
| Special keys | 0x100-0x109, F1-F12 at 0x110-0x11B | keyboard.h:10-20 |
| Modifier stamp bits | ALT 0x10000, CTRL 0x20000, SHIFT 0x40000 | keyboard.h:29-32 |
| Mouse edge queue | 32 | mouse.c:68 |
| Pointer sprite | 12x19 | mouse.c:22-23 |
| IntelliMouse knock | rates 200, 100, 80, then ID 3; rate restored to 100 | mouse.c:138-153 |
| PS/2 mouse packet | 3 bytes, 4 with wheel (Z = low nibble, signed) | mouse.c:89, 224-231 |
| Synaptics magic | 0x47, middle byte | synaptics.c:37 |
| Synaptics mode byte | 0x80 abs, 0x40 rate, 0x04 no gestures, 0x01 W; committed with `F3 0x14` | synaptics.c:39-42, 112-117 |
| Synaptics `Z_TOUCH` / `MOVE_DIVISOR` / `SCROLL_DIVISOR` / `JUMP_LIMIT` | 30 / 6 / 120 / 700 | synaptics.c:132-151 |
| Tap | shorter than 20 ticks, travel under 120 units, held 8 ticks | synaptics.c:154-160 |
| xHCI ports / slots | 32 / 16 (slot IDs 1..15; `CONFIG` = min(HCSPARAMS1, 15)) | xhci.h:20-21, xhci.c:521-522 |
| xHCI MMIO mapping | 64 KiB | xhci.c:503 |
| Transfer/command ring | 32 TRBs (31 usable plus Link) | xhci.c:138 |
| Event ring | 64 TRBs, 1 segment | xhci.c:145 |
| DCIs per slot | 8 (endpoint numbers 1-3 only) | xhci.c:170 |
| Command timeout | 10,000 x 100 us (about 1 s) | xhci.c:395-399 |
| Control transfer timeout | 10,000 x 100 us | xhci.c:752-755 |
| Bulk timeout | 50,000 x 100 us (about 5 s) | xhci.c:849-852 |
| Root port reset wait | 100 x 10 ms | xhci.c:600-608 |
| BIOS handoff wait | 100 x 10 ms | xhci.c:433-436 |
| Controller halt / reset / CNR waits | 500 ms / 1 s / 1 s | xhci.c:408-415 |
| Port power settle | 20 ms | xhci.c:552 |
| EP0 default max packet | SS 512, HS 64, LS/FS 8 | xhci.c:582-588 |
| CErr | 3 | xhci.c:673, 810 |
| USB HID devices tracked | 8 | usb.c:104 |
| Report buffer | length min(mps, 16); `kmalloc(32)` | usb.c:105, 485-495 |
| Descriptor bounce / max config length | 512 bytes | usb.c:279, 636 |
| Hub tiers | 5 (20-bit route string) | usb.c:568 |
| Hub ports | 1..15 | usb.c:580 |
| Hub port reset poll | 20 x 10 ms | usb.c:395-399 |
| Hub power-on wait | `bPwrOn2PwrGood*2 + 20` ms | usb.c:592 |
| Hotplug scan period | 300 ms (task) | usb.c:714 |
| Sticks | 1 at a time | usbdisk.c:258 |
| Block size accepted | 1..4096 (block layer assumes 512) | usbdisk.c:189 |
| Capacity | below 2 TiB (READ CAPACITY(10) only) | usbdisk.c:190 |
| Unit-ready retries | 20 x (TUR + sense + 50 ms) | usbdisk.c:141-145 |
| Stick run size | 8 sectors per block-layer request | usbdisk.c:243 |
| Block devices | `BLK_MAX 4` | blockdev.h:19 |
| HDA codec response wait | 20,000 x 10 us (200 ms) per verb | hda.c:190-200 |
| HDA widgets scanned | 64 | hda.c:123, 296 |
| CORB / RIRB entries | 256 / 256 | hda.c:124, 364, 378 |
| HDA BAR mapping | 16 KiB | hda.c:408 |
| HDA format | 48 kHz, 16-bit, 2 channels, stream tag 1, 4 BDL entries | hda.c:456-497 |
| HDA pin scores | speaker 100, line out 90, headphone 80, other output 10, unconnected 0 | hda.c:236-241 |
| Ensoniq `POLL` / `CODEC_SETTLE` | 100,000 / 4,000 port reads | ens.c:113, 257 |
| ES1370 clock | 1,411,200 Hz; rate = 1411200/(div+2); clamp 5512..44100 | ens.c:287, 312-318 |
| ES1371 SRC rate word | ((hz<<15)+1500)/3000 | ens.c:176 |
| Sound ring | 16,384 frames (64 KiB at 4 bytes a frame) | sound.c:27 |
| Guard | 256 frames | sound.c:31 |
| Clock fallback trigger | position unmoved for `timer_hz()/4` ticks (0.25 s) | sound.c:184-189 |
| Writer stall give-up | 50 x `sleep_ms(2)` (at least 10 ms each) | sound.c:276-280 |
| Default volume | 70 % | sound.c:84 |
| Tone ramp | 5 ms (`rate/200`) | sound.c:326 |
| `SYS_SOUND_WRITE` cap | 4096 frames per call | syscall.c:456 |
| Shell `beep` | 20..20000 Hz, at most 10 s | shell.c:525-527 |
| Default mode | 1024x768x32 (VBE and SVGA) | main.c:394 |
| `fb_set_mode` range | 640x480 .. 4096x4096 | fb.c:251 |
| GOP mode ceiling (loader) | 3840x2160 | uefi/loader.c:111-112 |
| BGA ID range | 0xB0C0..0xB0CF | fb.c:173 |
| Band height / max bands | 16 rows / 512 (8192 rows) | fb.c:350-351 |
| Helper threshold / wait | 8 or more bands / 20,000,000 `pause` | fb.c:442, 462 |
| SVGA version | 2 only (`SVGA_ID(2)`) | svga.c:143-144 |
| VGA text | 80x25 at 0xB8000, tab stop 4 | vga.c:8-10, 50 |
| fbcon cell | 8x16, glyphs 32..126 | font.h:4-7 |
| Clipboard | 65,536-byte buffer, at most 65,535 bytes of text | clipboard.h:16, clipboard.c:17 |
| Pins | 8 entries; label 15 characters; path 31 characters; file read limit 511 bytes | pins.h:14-16, pins.c:91-94 |
| Timer | PIT 100 Hz | main.c:459 |

---------------------------------------------------------------------------

## 8. Tests

### 8.1 Kernel selftest (`-append selftest`, kernel/selftest.c:3548-3605)

The supplied log (a local `bash run.sh -T` log: `run.sh -T` style, `-m 64`,
single CPU, ATA disk, rtl8139, no USB, no sound) passes 556/556. Its
boot lines confirm the device paths:

```
video 1024x768 32bpp, set through vbe, full flush 93877 kcycles, write combining
ps/2 status 1c, config 61 to 43, keyboard answers
mouse   ps/2 with a wheel, pointer at 512,384
hda: no pci class 4/3/0 / ens: no ensoniq audiopci on the bus / sound none
```

| Section | Checks | What it proves (selftest.c) |
|---|---:|---|
| `[video]` | 7 | 450-470: the mode is 1024x768, pitch = width*4, text grid = width/8, a pixel round-trips through the back buffer, a rect fills and stops at its edge, the font has glyph data. Skipped without a framebuffer |
| `[mouse]` | 4 | 513-518 plus 479-511: the pointer starts on screen; a press and release with no read between are both queued, in order; a press carries the position where it happened. Drives `mouse_inject` directly |
| `[taskbar]` | 18 | 998-1055: pins.c. Defaults when there is no file, add, duplicate refused, move to front and back, clamping, save and reload with order and labels, emptied list stays empty, `PIN_MAX` limit |
| `[trackpad]` | 25 | 2685-2908: no pad was detected (**this fails on a real laptop with a Synaptics pad**); `syn_answer_is_pad` for pad, mouse and near-miss answers; then synthesised 6-byte reports (`syn_pack`/`syn_feed`, 2698-2718): first contact moves nothing, right, up (y inverted), remainder carry, jump rejection, two-finger scroll (240 units down = 2 steps), no pointer jump when a second finger lands or leaves, tap = left click held through one pass and released after 120 ms, two-finger tap = right, a long rest or a moved touch is not a tap, resync on bad first and fourth bytes |
| `[clipboard]` | 14 | 3339-3381: empty, set and get, length, short-buffer truncation plus terminator, `CLIP_MAX+16` refused and the old content kept, `CLIP_MAX-1` accepted, replace not append, generation increments |
| `[sound]` | 3 | 3506-3546: rate 8k..96k, channels 1..8, and the play position advances within a factor of 4 of `rate/5*frame` bytes over 200 ms. **Skipped** in the log (no sound device); README's table omits the section |
| `[live tree]` | 1 of 19 | "devices reports the video mode" (`/sys/devices`) |

The xHCI, USB, PS/2 bring-up, HDA/Ensoniq (except `[sound]`), SVGA, fbcon
and VGA code has no selftest coverage.

### 8.2 Python harnesses (need QEMU; I read them but did not run them)

All use `harness.Guest`: `-kernel build/zelr.bin` (multiboot, so the VBE
path), `-display none`, serial on stdio, a TCP monitor, `-append console`
(tools/harness.py:602-636).

* **usbcheck.py** (24 checks). Machine: `q35`, 128 MiB (256 for the stick),
  `qemu-xhci`.
  1. **Direct**: usb-kbd and usb-mouse. Checks "xhci" and "1 keyboard(s),
     1 mouse" in the boot log; `/sys/devices` has a usb line and
     "ps/2 and usb". Types `uname` with monitor `sendkey`. QEMU delivers keys
     only to the USB keyboard when one exists, so typing proves the xHCI path.
     The report count must rise. Starts the desktop from the USB keyboard;
     `mouse_move` must move the drawn pointer.
  2. **Hub**: usb-hub on port 1 with the keyboard and mouse on 1.1 and 1.2.
     Checks "1 hub(s)", enumeration, and typing.
  3. **Hotplug**: boot with only a mouse; `device_add usb-kbd`; within 25 s
     `/sys/devices` shows "1 keyboard(s)" and typing works; `device_del`
     brings it back to "0 keyboard(s)". **Only a keyboard is hot-plugged.**
     Stick removal is never tested.
  4. **Stick**: an 8 MiB raw image with DEADBEEF at LBA 100. Checks "1
     disk(s)"; `stick` reports "sectors of 512 bytes" and "16384 sectors";
     `stick read 100`; `stick write 200 171`; read back; and the host file has
     0xAB bytes at LBA 200.
* **inputcheck.py** (5 checks): `q35`. Boots while pressing 0, 8 or 25 keys
  and/or moving the mouse 8 times during boot, then types `uname` over PS/2.
  Regression test for the missed-edge dead keyboard.
* **soundcheck.py** (9 checks): `q35` with intel-hda + hda-output to a WAV
  file. Checks "sound   hda" and "converter ... into pin" in the log. `beep
  440 600` and `beep 880 600`, then decodes the WAV: exactly two loud runs,
  zero-crossing pitch within 6 % (the middle of each note is measured to
  avoid the ramps), the notes differ, and the second has ended at least
  0.25 s before the recording ends (proves silencing).
* **enscheck.py** (9 checks): default machine, `-device ES1370`. Checks
  "ensoniq es137" and "44100 Hz" (the divider result), then the same WAV
  checks. Cannot reach the ES1371 SRC, the AC'97 path or the clock fallback.
* **volcheck.py** (5 checks): intel-hda. Opens the desktop volume pop-up by
  pixel arithmetic that mirrors wm.c, drags the slider down then up, and
  requires the last blip's peak to exceed 1.5 times the first's (volume is
  applied in software).
* **framecheck.py** (5 checks): `-smp 2`, 512 MiB. Moves the pointer ten
  times on the desktop. From `/sys/screen`: more than 10 frames drawn,
  average KiB per frame below the full screen and below half of it,
  `shared > 0` (the AP took half some frames).
* **tearcheck.py** (3 checks): the `halfdrawn` program paints red,
  uncommitted, then green. 14 screenshots must never show red. A
  window-server commit test rather than fb.c.
* **clipcheck.py** (5 checks): desktop terminal. Types a marker with PS/2
  `sendkey`, ctrl-c, deletes it, `echo ` then ctrl-v and Return, copies a
  decoy, then ctrl-a ctrl-c over the scrollback, ESC. `cat /sys/clipboard`
  must contain the marker and more than 3 lines.
* **Related harnesses outside my file list**, found by grep:
  - deskcheck.py:525-590 tests the wheel through the WM (`mouse_move 0 0 ±1`)
    and a run-time mode change to 800x600 via `/zelr.cfg` (`fb_set_mode`).
  - appcheck.py:237-269 plays a WAV from a USB stick with the music app
    (`SYS_SOUND_WRITE`), single CPU.
  - iso_test.sh boots OVMF, the only coverage of `fb_adopt`.
  - Nothing tests the VMware SVGA path; `-vga vmware` appears nowhere under
    tools/.

---------------------------------------------------------------------------

## 9. How to extend

### 9.1 Input

* **A new key.** Pick a code in 0x10A-0x10F or above 0x11B (keyboard.h). Map
  it in `extended_key`/`keypad_key` or the main switch (keyboard.c) **and** in
  `special()` (usb.c:155-171) so both keyboards agree. Mirror it in the SDK
  header if ring 3 must see it; the WM passes only `KEY_CODE | CTRL`.
* **Keypad digits, NumLock, keypad Enter/`/`.** Track NumLock (0x45) in
  keyboard.c. At present keypad digits always become navigation keys.
* **Caps Lock and Ctrl consistency.** Fold ctrl *before* applying caps in
  keyboard.c:153-157, or fold `A..Z` too, as usb.c:214-215 does. Add usage
  0x39 to usb.c and drive the LED with SET_REPORT.
* **Ctrl-C from USB.** Call the same `interrupting()` logic in
  `keyboard_inject` (keyboard.c:174-177).
* **USB auto-repeat.** It needs a timer-driven repeat in usb.c. The boot
  protocol plus SET_IDLE(0) sends no repeats.
* **Pitfalls**
  - Never read port 0x60 anywhere except `ps2_poll`. The AUX bit is valid
    only before the data read (ps2.c:127-128).
  - Anything that talks to the mouse, such as a new knock, must run before
    `sched_start` (synaptics.h:31-35). Otherwise the timer drain eats the
    replies.
  - Stamp modifiers at press time. Do not add consumers of `kbd_alt()` for
    chords (keyboard.h:58-60).
  - Keep `mouse_inject` the single door for movement: edges, clamping and the
    sprite all live there.

### 9.2 Synaptics

* More gestures (tap-and-drag, edge scroll, three-finger) go into
  `on_packet`/`contact_ended`. Tests go into `test_trackpad` through
  `syn_feed`, which is how this decoder has been developed ("broken on
  purpose to see whether it would", selftest.c:2817-2820).
* Extended-W (W=2) packets and the middle button are not decoded.

### 9.3 USB

* **A new device class.** Extend `claim_interface` (usb.c:425-563). Collect
  endpoints under an "interface of interest" pointer the way `disk` and
  `netdata` do. **Record the device** in a table keyed by slot and root port
  so `forget_root` can detach it; today only HID devices are recorded.
  Open endpoints lower DCI first (Context Entries only ever rise).
* **Removal done properly.** Make `forget_root` iterate every attached slot:
  call the class detach (`usbdisk_detach`, `usbnet_detach`, which currently
  have no caller, and HID); unmount only if the stick itself went; then
  `xhci_detach` every slot, hubs included. Free the HID report buffers.
  Acknowledge PORTSC CSC/PEC in `rescan` (write `PORT_KEEP(sc) | PORT_CSC`)
  so the next change raises an event on real controllers.
* **Hotplug behind hubs.** Open the hub's status-change interrupt endpoint
  (usb.c:671-675 explains why it isn't) and re-walk the ports that change.
* **Endpoint numbers 4 and up.** Raise `MAX_DCI` (xhci.c:170). It sizes
  `slot_t.ep[]` and the `xfer_*[16][MAX_DCI]` arrays. `drain_events` ignores
  higher DCIs.
* **Interval conversion.** Convert `bInterval` per speed before
  `xhci_open_endpoint`: FS/LS gives roughly `log2(bInterval*8)`, clamped to
  3..10; HS/SS gives `bInterval-1`.
* **SuperSpeed.** Decode `bMaxPacketSize0` as `1 << n` when `bcdUSB >=
  0x0300`. For SS hubs, send SET_HUB_DEPTH and use the USB 3 port-status bit
  layout. Read the SS endpoint companion for Max Burst.
* **Halt recovery.** Handle STALL (completion code 6) with Reset Endpoint,
  Set TR Dequeue Pointer and CLEAR_FEATURE(ENDPOINT_HALT). Cancel timed-out
  TRBs with Stop Endpoint.
* **Interrupts instead of polling.** Enable MSI/MSI-X and set IMAN.IE and
  USBCMD.INTE. `drain_events` is already safe from interrupt context (it is
  called with IF=0).
* **Pitfalls the comments call out**
  - Every PORTSC read-modify-write must use `PORT_KEEP` (xhci.c:79-86).
  - Contexts are 32 or 64 bytes; always use `ctx_at` (xhci.c:217-220).
  - Read capability registers as 32-bit words (xhci.c:506-510).
  - Wait for CNR after reset (xhci.c:411-413).
  - Nothing from `alloc_aligned` can be freed, so reuse it (xhci.c:226-231).
  - Never pass a stack buffer to the controller; use the heap (usb.c:270-277,
    README usbnet notes).
  - During boot there are no ticks, so never use `sleep_ms` or tick timeouts
    in code reachable from `usb_init` (xhci.c:279-294, usb.c:299-310).
  - The HID buffer must be re-queued after every report, or the device goes
    silent (usb.c:261-264).
  - Set the ISP flag on bulk TRBs (xhci.c:832-835).

### 9.4 Mass storage

* **Block sizes other than 512.** Either refuse them in `read_capacity` or
  translate 512-byte LBAs. `blkdev_t` has no block-size field, and the block
  layer and FAT assume 512 (blockdev.h:4, blockdev.c:90).
* **Beyond 2 TiB**: READ CAPACITY(16) and READ/WRITE(16).
* **More than one stick**: `usbdisk.c` is single-instance statics
  (`attached`, `dev_slot`, `model`); it needs a per-device struct, a
  `blkdev_t` per device, and diskfs support for more than one removable
  volume (`FAT_VOL_USB`).
* **Robustness**: Bulk-Only Mass Storage Reset (class request 0xFF) plus
  clear-halt on both endpoints after a phase error.

### 9.5 Sound

* **A new controller.** Implement `xxx_init`, `xxx_start(buf, bytes, rate)`
  (loop forever), `xxx_position` (bytes into the buffer), `xxx_rate`,
  `xxx_channels`, `xxx_frame_bytes` and `xxx_describe`. Add it to
  `snd_dev_t` and the six dispatchers (sound.c:41-66) and to `sound_init`.
  If its position never moves, the clock fallback already covers it.
* **Volume or tone for ring 3.** There are no syscalls; add them next to
  `SYS_SOUND_INFO`/`WRITE` (syscall.c:439-461, syscall.h, sdk/zelr.h, and
  tools/abicheck.py's struct pairs).
* **HDA rates other than 48 kHz**: build the SD_FMT base, multiplier and
  divider, check the DAC's supported-rates parameter (0x0A), and report the
  rate actually programmed.
* **Pitfalls**
  - `advance()` must stay interrupt-safe.
  - Claim ring space before copying (sound.c:291-294).
  - From code that may run on an AP while holding the kernel lock, sleep with
    `task_sleep`, not `sleep_ms` (section 10, item B1).
  - The ES1371 SRC must be fully programmed before it is enabled
    (ens.c:209-215).
  - The HDA RIRB IRQ bit must stay set (hda.c:58-65).
  - HDA stream tag and converter stream number must match (hda.c:491-495).

### 9.6 Video

* **A new framebuffer source.** Set `width`, `height`, `pitch` and `lfb`.
  Map the aperture, WC through `paging_map_wc` where possible. Call
  `take_back_buffer` and `take_sent`, set a backend flag, update
  `fb_backend()` and `fb_mode_settable()`, then `active = true; fb_clear;
  fb_flush`. Add its caller in main.c:391-412.
* **WC for VBE and SVGA.** Map with `paging_map_wc` instead of
  `map_page(..PTE_PRESENT|PTE_RW)` (fb.c:208-213) or `paging_map_device`
  (svga.c:183). Then the boot-log claim (main.c:406) becomes true.
* **GOP pixel formats.** Pass the GOP `pixel_format` through the handoff and
  swap channels in fb.c (or restrict the loader to BGR modes,
  uefi/loader.c:102-103).
* **Pitfalls**
  - Every drawer writes only to `fb_pixels()` (the back buffer) and then
    flushes. Code that writes to the lfb directly bypasses the mirror.
    `fb_flush_rect` keeps the mirror in step; anything else that writes the
    card must invalidate it (`sent_valid = false`).
  - The AP helper must only touch its own bands. It runs without the kernel
    lock.
  - Mode changes must allocate the new buffer before freeing the old
    (fb.c:263-265).
  - On SVGA every change must be announced with `svga_update`, or the host
    never shows it (svga.h:13-16).

### 9.7 Clipboard and pins

* The clipboard is intentionally text only. A typed clipboard needs a MIME
  negotiation protocol (clipboard.h:12-14). `clip_generation` exists for a
  future watcher.
* Pins: either wire pins.c back into the WM's taskbar (`panel_*` in wm.c,
  with `pins_reload` on the same 4 Hz check as the theme) or delete pins.c,
  pins.h and `test_pins`, and fix the comments at wm.c:91 and 2140-2146.

---------------------------------------------------------------------------

## 10. Doc drift and suspicious code

"Verified" means I followed the code path completely. "Suspected" means the
conclusion also rests on an outside specification or on timing, and I say
which.

### A. USB removal and resource handling (verified)

A1. **An unplugged USB stick is never detached.** `forget_root`
   (usb.c:681-694) iterates only `devices[]`, and `claim_interface` adds an
   entry only for HID (usb.c:474-504). The stick branch (usb.c:542-560)
   records nothing. So after the stick is pulled:
   * `usbdisk` stays attached, the block device stays registered, `/usb`
     stays mounted, and `ndisks` and `/sys/devices` still show it.
   * Every access spins about 5 s per bulk transfer before failing (the slot
     is still "used"; xhci.c:849-852).
   * A newly inserted stick is refused, because
     `if (attached) return false;` (usbdisk.c:258) is still true.

   usbcheck.py hot-plugs only a keyboard (usbcheck.py:216-253), so nothing
   catches this. It contradicts README.md:213-214 ("pulling it out is
   noticed too") for sticks.
A2. **Unplugging any USB keyboard or mouse unmounts `/usb`.**
   `diskfs_unmount_removable()` is called unconditionally for each HID device
   removed (usb.c:688). The stick itself stays attached and registered
   (`usbdisk_detach` is a no-op for the keyboard's slot, usbdisk.c:287).
A3. **`usbnet_detach` has no caller** (it is defined at usbnet.c:218; grep
   finds nothing else). An unplugged RNDIS adapter stays `present`.
A4. **xHCI slots leak.** `xhci_detach` (Disable Slot) is only reached from
   `forget_root` for HID devices. Hub slots, stick and network slots, devices
   no class claimed (usb.c:651, 562), and every enumeration that failed after
   `xhci_attach` (usb.c:625-641; xhci.c:647-650, 680-685) keep their slot.
   With `CONFIG` at most 15 slots (xhci.c:521, 531), enough replugging
   exhausts Enable Slot until reboot. `nhubs`, `ndisks` and `nnets` are also
   never decremented, and HID report buffers (`kmalloc(32)`, usb.c:485) are
   never freed.

### B. Likely bugs outside USB removal

B1. **Suspected stall or deadlock on SMP: `sound_write` sleeps with
   `sleep_ms` while holding the kernel lock.**
   * sound.c:280 calls `sleep_ms(2)`, a `hlt` loop until `ticks` advances
     (timer.c:76-82).
   * `ticks` advances only in the PIT handler, which runs on the BSP
     (IOAPIC routing) and must `kernel_lock_acquire()` first
     (idt.c:185-187; only `VEC_LOCAL_TIMER` uses try-lock).
   * A ring-3 program calling `SYS_SOUND_WRITE` (userland/music.c:242, 283)
     can be running on an AP. User tasks migrate; kernel tasks are pinned to
     the BSP (sched.c:423-427, 464). Inside the syscall that AP holds the
     lock.
   * When the ring is full, the AP halts in `sleep_ms` holding the lock. The
     BSP's next PIT interrupt spins in `kernel_lock_acquire` with IF=0, so
     `ticks` stops. Keyboard and mouse IRQs, also routed to the BSP, stop
     too.
   * The AP escapes only if its local-timer `scheduler_switch` finds
     *another* runnable ring-3 task to switch to, which releases the lock
     (idt.c:287). With the music player as the only runnable user task, it
     never does.

   Nothing tests this: appcheck.py's music test is single-CPU (no `-smp`).
   zelr.bat runs with `-smp 2` and `-smp 4` (zelr.bat:100, 135). The fix is
   `task_sleep(2)` (sched.c:656-666), which blocks, yields and lets idle
   release the lock.
B2. **Ctrl-C on a USB keyboard does not interrupt anything.** PS/2 `push()`
   raises `signal_interrupt()` for code 3 (keyboard.c:44-58).
   `keyboard_inject()` does not (keyboard.c:174-177). The key is still
   queued, so a shell at its prompt sees ^C, but a looping program is not
   signalled. Verified.
B3. **Caps Lock breaks Ctrl+letter on PS/2.** keyboard.c:153-156 uppercases
   before the ctrl fold at 156, which only folds `a..z`. With Caps Lock on,
   Ctrl+C yields `'C'|KEY_MOD_CTRL`: no ^C, no SIGINT. The WM's ctrl+f test
   `KEY_CODE(key) == 6` (wm.c:3836) fails. Programs comparing against 3 for
   copy fail. Ctrl+Shift+letter is likewise unfolded on PS/2 but folded on
   USB (usb.c:214-215). Verified.
B4. **fb mirror can go stale after a re-entrant flush.** Suspected; it needs
   a preemption inside `fb_flush`. The re-entrant path `whole_screen()`
   (fb.c:410-416) writes the whole back buffer to the card **without
   updating `sent`**. The interrupted outer flush then sets
   `sent_valid = true` (fb.c:472). Later, if the back buffer returns to what
   the stale mirror holds (for example kernel-print text drawn by fbcon over
   the desktop, then the desktop redrawn), those bands compare equal and are
   never resent, so ghost pixels stay until something else in the band
   changes. Kernel tasks are preemptible, and ring-3 syscalls on an AP can be
   switched out mid-kernel, so the interleaving is possible. Fix:
   `sent_valid = false` in `whole_screen()`.
B5. **USB sector sizes other than 512 corrupt memory.** Verified.
   `read_capacity` accepts 1..4096-byte blocks (usbdisk.c:189). `rw10` moves
   `count*sector_bytes` (usbdisk.c:214). But `blk_read_on`/`blk_write_on`
   step 512 bytes per sector (blockdev.c:90, 108), and `diskfs_mount_removable`
   reads "1 sector" into `u8 sec[512]` on the stack (diskfs.c:55-56). A 4Kn
   stick would have 4096 bytes DMA'd into a 512-byte stack buffer.
B6. **Boot log claims write combining that is not in effect** (verified;
   confirmed by the selftest log line "set through vbe, ..., write
   combining"). main.c:403-406 prints "write combining" whenever
   `paging_wc_ready()`. Only `fb_adopt` maps with `paging_map_wc`. The VBE
   aperture uses `map_page(..PTE_PRESENT|PTE_RW)` (PAT slot 0, write-back,
   fb.c:208-213) and SVGA uses `paging_map_device` (uncached, svga.c:183).
B7. **GOP RGB modes get red and blue swapped.** Verified. The UEFI loader
   accepts both `PixelRedGreenBlueReserved8BitPerColor` and `...BlueGreenRed...`
   (uefi/loader.c:102-103). The handoff has no pixel-format field
   (handoff.h:39-44), and fb.c always writes `0x00RRGGBB` (BGRX in memory).
B8. **`fb_clear` ignores pitch.** Verified. It writes `width*height`
   consecutive u32s (fb.c:292-297). On an adopted GOP mode with
   `pixels_per_scan_line > width` (common on real panels), the bottom rows
   are not cleared (`fbcon_clear`, `fbcon_init`, the initial clear).
   `[video]` checks `pitch == width*4` only under QEMU VBE.
B9. **`fb_init` gives up entirely when the back buffer cannot be
   allocated.** Verified. fb.c:231-234 disables VBE and returns false,
   contradicting fb.c:70-73 ("drawing straight into video memory is still
   worth doing"; only `fb_adopt` honours that). `init_svga` returns false
   after `svga_init` has already enabled the adapter (fb.c:159-160), leaving
   VMware in SVGA mode with the console routed to VGA text nobody sees.
B10. **Possible silence on a real ES1370.** Suspected, based on the AK4531
   register map: `ak4531_write(0x16, 0x00)` "reset the mixer state" is the
   last mixer write (ens.c:465). In the AK4531, register 0x16 is the
   reset/power-down register, where 0 asserts reset and power-down; the Linux
   ak4531 driver writes 0x03 ("no RST, PD") there. QEMU's ES1370 ignores
   codec writes, so enscheck.py passes regardless.
B11. **`kbd_has_char()` ignores serial input.** Verified. It tests only the
   key ring (keyboard.c:170), while `kbd_trygetchar()` also returns serial
   bytes (keyboard.c:188-198). So `poll()` on the console (fd.c:425) will not
   report POLLIN for input that arrived on COM1. The harnesses drive the
   shell over serial.
B12. **`mouse_init` never checks an ACK.** Verified (mouse.c:256-291). On any
   machine whose 8042 status is not 0xFF it returns true and
   `mouse_present()` is true, with a pointer drawn in console mode, even with
   no mouse. The ordering makes this worse: `ps2_init` and `mouse_init` run
   before `usb_init` takes the xHCI from the firmware (main.c:511-523), so
   they may be talking to the firmware's USB-legacy PS/2 emulation, which
   disappears at `take_ownership`.
B13. **`ps2_read` does not filter by AUX.** Suspected; the window is narrow
   and at boot only. A key pressed during `mouse_init`/`syn_detect` can be
   consumed as a mouse ACK, ID or status byte (mouse.c:123-153,
   synaptics.c:70-78), shifting every later reply by one. For example the
   wheel ID read could be a scancode (the mouse then sends 4-byte packets
   while `packet_len = 3`), or a pad's 0x47 answer is missed.
B14. **`mouse_has_wheel()` is true for any Synaptics pad** (mouse.c:105),
   even without W reporting, where two-finger scroll cannot happen
   (synaptics.c:199-201). main.c:558 then prints ", two finger scroll".
   Verified.
B15. **Latent HDA rate mismatch.** Verified. `hda_start` stores
   `rate = rate_hz` (hda.c:451) but always programs 48 kHz (hda.c:486-489).
   Harmless while `sound_init` passes 48000; hda.h:33-35's "a codec is
   allowed to refuse a rate" is not implemented.
B16. **VGA text attribute corruption for bytes 0x80-0xFF.** `cell()`
   (vga.c:15) casts a signed `char` to u16, and sign extension overwrites the
   attribute with 0xFF. Verified.
B17. **Unchecked `x + w` in clipping.** Hazard. `fb_rect`, `fb_flush_rect`
   and `svga_update` clip with `x + w > width` (fb.c:302-303, 511-512;
   svga.c:115-116). A huge `w` (a negative int cast to u32) wraps past the
   check and writes far out of bounds. The callers I looked at pass sane
   values; gfx.c does its own clipping (not in my area).
B18. **`sync_fifo` busy-waits without bound** on `SVGA_REG_BUSY`
   (svga.c:84-85). A wedged host adapter hangs the kernel.

### C. xHCI and USB spec deviations (suspected; checked against the xHCI and USB specs, invisible to QEMU)

C1. **Interrupt endpoint Interval is passed raw.** `bInterval` goes straight
   into the endpoint context's Interval field (usb.c:480, xhci.c:808). xHCI
   defines the period as 2^Interval x 125 us. For FS/LS devices `bInterval`
   is milliseconds and must be converted (roughly `log2(bInterval*8)`, legal
   range 3..10); for HS it is `bInterval-1`. Effects on real hardware: a FS
   keyboard with `bInterval=10` polls every 128 ms instead of 10 ms; values
   1-2 are below the FS minimum; values above 15 are invalid and may fail
   Configure Endpoint (then the device is simply not claimed).
C2. **SuperSpeed `bMaxPacketSize0` is an exponent** (9 means 512).
   usb.c:629-630 programs it raw (9) through Evaluate Context, ignoring the
   result. If a controller accepts it, EP0 transfers to USB 3 devices on
   USB 3 ports break. The usbcheck stick lands on a USB 2 port
   (usbcheck.py:30-33), so this is untested.
C3. **SuperSpeed hubs.** `reset_hub_port` decodes port status with the USB 2
   bit layout (usb.c:353-357, 407-409). In a USB 3 hub's `wPortStatus`,
   bit 9 is PORT_POWER, so every powered SS port reads as LOW speed. Also
   SET_HUB_DEPTH is never sent, and the TTT field is meaningless for SS.
   SuperSpeed devices behind USB 3 hubs are therefore unlikely to work.
   LS/FS/HS devices go through the hub's USB 2 half and are unaffected.
C4. **Root-port change bits are never acknowledged**, except PRC in
   `reset_port` (xhci.c:598-605). Port Status Change events are raised when
   a change bit goes from 0 to 1. With CSC left set after the first connect,
   a later unplug or replug on the same port may raise no event, so
   `port_changed` stays false and nothing rescans until some *other* port
   changes. QEMU's xHCI model rewrites PORTSC on each attach and detach
   (clearing CSC before setting it again), so the hotplug test cannot see
   this.
C5. **No halt recovery, no TRB cancellation.** A STALL leaves the endpoint
   Halted; there is no Reset Endpoint, Set TR Dequeue or
   CLEAR_FEATURE(ENDPOINT_HALT) anywhere. A timeout leaves its TRBs on the
   ring (xhci.c:395-399, 752-755, 849-852), and completion is detected by
   the counter changing, so a late completion is credited to the next
   transfer. SET_IDLE is optional for mice and is sometimes STALLed; that is
   harmless for HID but fatal for a stick whose bulk endpoint stalls.
C6. **Bulk TRBs are neither split nor bounded.** One TRB with
   `status = len` (xhci.c:846): lengths over 64 KiB overflow the 17-bit
   length field, and buffers crossing a 64 KiB boundary violate the TRB
   rules. usbdisk keeps runs at 8 sectors through the block layer, but
   `rw10` itself allows `count <= 0xFFFF` (usbdisk.c:203) for direct callers.
C7. **`xhci_bulk` blocks about 5 s when no data comes**, and usbnet uses it
   for RX polling (usbnet.c:242-260, reached from netdev.c:65). Every idle
   poll spins about 5 s and leaves one more TRB queued (see C5). This is
   cross-area; the network atlas should confirm the calling pattern.
C8. **Only one boot interface per device is claimed.** `claim_interface`
   returns true after the first HID boot interface (usb.c:503-504). A combo
   receiver (keyboard on interface 0, mouse on interface 1) loses its mouse.
   Verified.
C9. **RNDIS detection is narrow.** The control interface is recognised only
   as class 0x02 (usb.c:453). Android tethering commonly presents RNDIS
   control as class 0xE0/0x01/0x03, which Linux's rndis_host matches. Such a
   phone would reach "comm_iface == 0xFF", return false, and leak its slot.
   Also the CDC-data interface must be the last interface seen (usb.c:454,
   515). A phone that also exposes ADB (a vendor interface after it) falls
   into the stick branch and fails `usbdisk_attach` after about a second of
   retries.
C10. **`MAX_DCI 8`** limits endpoints to numbers 1-3 (xhci.c:170, 786, 839,
   863). Devices using endpoint 4 or above cannot be opened, and their events
   are dropped (xhci.c:335).
C11. **Missing post-reset recovery delays.** None after a root or hub port
   reset before Address Device (the USB spec wants 10 ms). QEMU does not
   care; some real devices do.
C12. **HDA BDL buffer alignment.** The HDA BDL requires 128-byte-aligned
   buffer addresses. The sound ring is plain `kmalloc` (8-byte alignment,
   heap.c:41; hda.c:461). Suspected minor impact on real controllers.
C13. **HDA connection-list walk.** `dac_behind` sends `GET_CONN(i)` for
   i = 0..7 and uses only the low byte (hda.c:255-258, 266-268). A
   short-form response carries four entries. Linux queries only multiples of
   4 and unpacks them, so for i not a multiple of 4 it is codec-dependent
   whether entry i comes back. The pin's own connection select is never set,
   which matters only when the DAC is not entry 0.

### D. Documentation drift (verified)

D1. zelr.bat:111-112: "zelr has no USB stack yet".
D2. usb.h:4 "USB, as far as a keyboard and a mouse need it" and usb.h:14
   "Only the boot protocol is implemented". usb.c:1-21 lists only HID steps.
   xhci.h:14-18 and xhci.c:166-169 ("Only the endpoints a keyboard or a
   mouse uses are ever opened"). All of these predate MSC and RNDIS support.
D3. xhci.h:136-141 says `on_report` receives "how many bytes arrived". It
   receives the **residual** (xhci.c:346), which usb.c:255 correctly
   converts.
D4. usbdisk.h:13-15 says blockdev.c "picks one disk at boot and hides which
   it was". blockdev now holds numbered disks (blockdev.h:6-17), and usbdisk
   registers through it.
D5. README.md:1628-1630 "USB stops at keyboards, mice, hubs and storage. No
   other class is claimed" is contradicted by RNDIS (usb.c:449-540;
   README.md:1222-1228). README.md:1633-1634 "A second stick is a disk with a
   number and no way to mount it": a second stick is refused before it gets
   a number (usbdisk.c:258). README.md:1330 and its table say 552 checks and
   omit `[sound]`; the supplied log shows 556.
D6. fb.c:333 and README.md:575 say "two bands of forty eight". `BAND_ROWS`
   is 16 (fb.c:350), and the comment at fb.c:348-349 agrees with 16.
D7. ps2.c:135-138 and ps2.h:40-43 say the rescued count "says so in the boot
   log". `ps2_rescued()` (and `ps2_first_status/config`) have **no caller**,
   and the only log line (ps2.c:198-200) omits it. The count is also an upper
   bound: the timer can read a byte whose IRQ is already pending.
D8. timer.c:66-72 says the sound writer gives up after "two hundred and
   fifty" sleeps. sound.c:276 uses 50.
D9. sound.h:52-59 repeats the `sound_played` comment block with no
   declaration after it.
D10. pins.h (the whole header) plus wm.c:91 ("The badge, then the pinned
   apps ...") and wm.c:2140-2148 (an empty "apps on the panel" section)
   describe taskbar pins that the WM no longer has (README.md:843). pins.c
   has no runtime caller.
D11. smp.h:76 says a function handed over with `smp_run` "runs with
   interrupts off". smp.c:302 executes `sti` before calling it, so
   `bands_helper` runs with interrupts on. This is cross-area.
D12. usbcheck.py:190-191 says typing behind the hub proves "the transaction
   translator". walk_hub programs TT fields only for a **high-speed** hub
   (usb.c:606-610). If QEMU's usb-hub is a full-speed hub, as I believe
   (not verifiable here), the TT path is untested.
D13. `/sys/devices` prints "keyboard ps/2..." even when no 8042 exists
   (sysfs.c:186). fb.h:4-5 describes only the VBE source. fb.c:12 and 14
   include io.h twice. xhci.h:106-111 has a stale comment sitting above
   the endpoint-kind defines.
D14. selftest.c:2737 "this machine did not conclude it has a trackpad" fails
   **by design** on a real laptop with a Synaptics pad. The comment treats
   it as an emulator-only expectation.
D15. **Dead exports**: `kbd_getchar`, `kbd_alt/ctrl/shift`,
   `ps2_first_status/first_config/rescued`, `syn_packets`, `syn_fingers`,
   `usb_mice/hubs/disks`, `sound_volume`, `sound_silence`,
   `clip_generation` (selftest only), and all of pins.c (selftest only).
   xhci `slot_t.port` is written and never read. `TRB_CHAIN`, `OP_PAGESIZE`,
   `OP_DNCTRL`, `RT_IMAN`, `SVGA_REG_CAPABILITIES`, hda `INTCTL`/`DPLBASE`
   are defined and unused.

### E. Smaller observations

* USB ErrorRollOver reports (all 0x01) are skipped as usages, but they
  overwrite `last`. When rollover clears, every key still held registers as
  a new press (usb.c:200-225).
* A USB mouse moving without a framebuffer clamps `mx` to -1, because
  `fb_width()` is 0 (mouse.c:197-202).
* `hda_init` uses the first class 04/03 function in bus order
  (hda.c:396). On a machine where that is a GPU's HDMI audio, output may go
  to HDMI (digital pins score 10) and the onboard codec is never tried. I
  did not verify this on hardware.
* `usbdisk_attach`'s `wait_ready` spins up to 20 x 50 ms plus transfer
  timeouts in the usb task or at boot, holding the kernel lock
  (usbdisk.c:80-82, 140-147).

---------------------------------------------------------------------------

## 11. Open questions

1. **B1 (sleep_ms under the kernel lock on an AP).** The chain is complete on
   paper. Confirm it by playing `music` with `-smp 2` while nothing else
   runnable is in ring 3. If it reproduces, check the other `sleep_ms` users
   that could run inside syscalls (wait.c:35 falls back to `sleep_ms`; it is
   outside my area).
2. **Ctrl-C in the desktop.** PS/2 ctrl+c raises `signal_interrupt()` even
   while the desktop has the keyboard (the terminal's copy shortcut). Its
   target is the last ring-3 reader of the console (`signal_console_reader`,
   fd.c:223; signal.c:262-287) or that reader's children. I did not trace
   which task that is while the desktop runs, nor whether desktop apps can be
   its children. clipcheck.py passes, so in practice the terminal survives,
   but why was not established.
3. **The AK4531 register 0x16 write** (B10). Check the datasheet or real
   hardware. VMware provides an ES1371 (the AC'97 path), so the ES1370 path
   is exercised only under QEMU, which ignores mixer writes.
4. **ES1371 `src_read`** (ens.c:167-171) omits Linux's state-bit dance (setting
   0x10000, waiting for state 0x00010000, restoring the original value). Its
   result is used only to preserve the low byte of INT_REGS. Whether that is
   right on VMware or real ES1371/CT5880 hardware is unknown. The ES1373 IDs
   0x8001 and 0x8002 (ens.c:42-43) are unverified.
5. **The VMware position claim.** sound.c:124-137 says VMware's Ensoniq never
   updates DAC2's frame count. That cannot be verified statically, and no
   harness exercises clocked mode.
6. **Real xHCI behaviour** for C1-C4 (raw interval, SS `bMaxPacketSize0`, SS
   hubs, unacknowledged CSC) needs hardware or a stricter emulator. The
   README's laptop claims (README.md:203-214) depend on them.
7. **QEMU's usb-hub speed** (D12) decides whether the transaction-translator
   fields (`tt_slot`/`tt_port`, `xhci_mark_hub` TTT/MTT) have ever been
   exercised.
8. **`GET_CONN` with an index that is not a multiple of 4** (C13): which
   codecs return entry i, and which return the aligned group?
9. **The fb helper after a timeout.** `smp_run` clears `fn` before running
   it (smp.c:304), so `smp_helper()` can hand the same AP the next frame's
   job while it is still inside the previous one. If the BSP timed out
   waiting (fb.c:461-465), the old job's `helper_done = 1` could satisfy the
   next frame's wait before its own half has run. That half then runs late,
   concurrently with the BSP's next frame. It writes identical data, but the
   `band_dirty` and `sent` bookkeeping could mismatch. This needs a stalled
   AP to happen at all.
10. **Firmware USB-legacy emulation** (B12). On real machines, does anything
    PS/2-side (`ps2_present`, `mouse_present`, `packet_len`) go stale once
    `take_ownership` stops the SMI emulation? Nothing re-probes the 8042
    afterwards.
11. **Stack buffers handed to the xHCI.** `diskfs_mount_removable` uses a
    stack sector buffer (diskfs.c:55). It works only while the calling
    task's stack is identity mapped, as the boot stack and kmalloc'd task
    stacks are; otherwise `xhci_bulk` returns -1 (xhci.c:841) and mounting
    fails quietly. Worth confirming how task stacks are allocated (process
    area).
