# The zelr atlas

A subsystem-by-subsystem map of zelr, written so that whoever works on it next can find anything without
re-reading 120,000 lines. It describes the tree at **commit 6048716** (`main`, 2026-09-22, two commits
after v0.37.0). It was written on 2026-09-26 by reading every file in the repository (generated data
skimmed) and checking the results against a real build and test run.

- Each numbered file covers one area, in the same eleven sections: scope, big picture, file-by-file
  detail, control flow, interfaces, concurrency and ownership, limits, tests, how to extend, **doc drift
  and suspicious code**, and open questions.
- File:line references are to 6048716. Once the tree moves on, trust the code over this atlas and fix
  the atlas.
- "Verified" in a §10 means the claim was checked against the source by reading, not by running a
  reproduction, unless it says so.

## Files

| File | Area | Covers |
|---|---|---|
| [01-boot-build.md](01-boot-build.md) | Boot, build, imaging | `boot/`, `bootloader/`, `uefi/`, `linker.ld`, `build.sh`, `run.sh`, `zelr.bat`, `handoff.h`, `builtin.S`, the image writers `mkiso/mkgpt/mkfat/readfat`, the build checks |
| [02-cpu-memory.md](02-cpu-memory.md) | CPU, interrupts, memory, SMP, platform | GDT/TSS, IDT/ISR, PIC/LAPIC/IOAPIC, ACPI, SMP, paging, PMM, heap, FPU, PIT, RTC, RNG, serial, power, PCI |
| [03-processes-syscalls.md](03-processes-syscalls.md) | Tasks, processes, system calls | scheduler, the big kernel lock, wait queues, signals, ELF64 loader, fork/COW/exec, fds, pipes, poll, **the full syscall table** |
| [04-storage-fs.md](04-storage-fs.md) | Storage and filesystems | ATA/AHCI/NVMe, the block layer, GPT/MBR, FAT16/32 + LFN, crash-safe writes, VFS, `/sys`, layout, the black box boot log |
| [05-devices.md](05-devices.md) | Devices | PS/2, keyboard, mouse, Synaptics, xHCI/USB/HID/mass storage, HD Audio, Ensoniq, sound, framebuffer/VBE/SVGA/GOP, fbcon, VGA, clipboard, pins |
| [06a-network.md](06a-network.md) | Network | netdev, e1000, PCnet, RTL8139, RNDIS, ARP/IPv4/ICMP/UDP/DHCP/DNS, TCP, the kernel HTTP client, wifi detection, the test web server |
| [06b-tls-crypto.md](06b-tls-crypto.md) | TLS and cryptography | TLS 1.3, X.509 chains, RSA, ECDSA P-256/384, X25519, SHA-1/256/384/512, HMAC/HKDF/PBKDF2, AES/GCM, WPA2, the root store |
| [07-wm-desktop.md](07-wm-desktop.md) | Desktop | the compositing window manager, window server, theme and its 31 knobs, gfx primitives |
| [08-kmain-shell-selftest.md](08-kmain-shell-selftest.md) | Kernel entry and self test | `kmain` phases, boot tasks, the kernel shell and its commands, first run, **the selftest sections** |
| [09a-sdk-libc-toolkit.md](09a-sdk-libc-toolkit.md) | SDK, libc, toolkit | `sdk/zelr.h` ABI, `zelr.ld`, `sdk/libc`, `alloc.h`, `args.h`, `draw.h`, `ui.h` widgets |
| [09b-term-sh-tests.md](09b-term-sh-tests.md) | Terminal and shell | `term.c` and its built-ins, `/bin/sh`, coreutils, the ring 3 test programs |
| [10-apps-games.md](10-apps-games.md) | Desktop programs | files, settings, notes, paint, music, calc, monitor, blackjack, poker |
| [11-browser.md](11-browser.md) | Web browser | URL/fetch/cookies/keep-alive, HTML, DOM, CSS cascade, layout, painting, forms, events |
| [12-js-engine.md](12-js-engine.md) | JavaScript | values, parser, interpreter, built-ins, regex engine, DOM bindings, timers, XHR |
| [13-media-fonts.md](13-media-fonts.md) | Pictures and type | inflate, PNG, JPEG, SVG, the outline typeface generator, the 8x16 font |
| [14-tests-pipeline-launcher.md](14-tests-pipeline-launcher.md) | Verification and delivery | `harness.py`, **the test catalogue**, `gate.sh` tiers, the two-model pipeline, `release.sh`, the Windows launcher |
| [15-history-website.md](15-history-website.md) | History and public face | every release and commit, decisions and their reasons, the bug catalogue, commit/release style, zelr.org |
| [16-design-system.md](16-design-system.md) | Design system | principles, colour tokens, the two looks, shape, type, spacing, icons, motion, wallpapers, components, voice |

## System map

```
  firmware / emulator                     loaders (all build one handoff_t, magic "ZELR64HF")
  ---------------------------------------------------------------------------------------------
  BIOS, disc (El Torito entry 1) --\
  BIOS, stick (MBR)  ---------------+--> bootloader/cdboot.S  (real mode, unreal mode copies up) --\
  UEFI, disc (El Torito 0xEF)  ----\                                                               |
  UEFI, stick (ESP)  ---------------+--> uefi/loader.c = \EFI\BOOT\BOOTX64.EFI (GOP, ACPI, map)  ---+--> kernel
  QEMU -kernel (development)  -------> boot/boot.S multiboot a.out kludge, 32 -> 64 bit  ---------/    at 16 MiB

  kmain (kernel/main.c), boot log phases in order:
    serial,vga > gdt > idt > pic > memory > paging > heap > acpi,pcie > video > filesystem > disk >
    clock > timer > entropy > smp > network > input > sound > interrupt routing >
    syscalls, window server, scheduler > handing over to the scheduler  [> selftest phase]

  kernel (ring 0, one big lock held whenever a CPU is not in ring 3)
  +-- CPU & memory ........ gdt idt isr pic lapic ioapic acpi smp paging pmm heap fpu timer    (02)
  +-- tasks & processes ... sched wait signal user elf syscall fd pipe apps                    (03)
  +-- storage ............. ata ahci nvme usbdisk > blockdev > parts > fat/diskfs > vfs        (04)
  |                         vfs also serves /bin (builtin.S), /sys (sysfs.c), RAM fs (fs.c)
  +-- devices ............. ps2 keyboard mouse synaptics | xhci usb | hda ens sound |         (05)
  |                         fb svga vga fbcon | clipboard pins
  +-- network ............. e1000 pcnet rtl8139 usbnet > netdev > net > tcp > http            (06a)
  +-- crypto & tls ........ sha* gcm crypto x25519 rsa ec x509 roots tls wpa rng              (06b)
  +-- desktop ............. wm (runs inside the shell task) winsrv gfx theme                  (07)
  +-- console ............. shell welcome selftest blackbox                                  (08, 04)
  ========================= int 0x80, 66 system calls (include/syscall.h = sdk/zelr.h) =========
  ring 3 (userland/, built only with sdk/)
    term sh coreutils | files notes paint settings monitor music calc | blackjack poker |       (09b, 10)
    browser = web fetch html dom css layout + js jsparse jsrun jsdom jsregex +                (11, 12)
              png jpeg svg inflate + facetext                                                  (13)
    toolkit = ui.h draw.h alloc.h args.h face.h font.h, optional sdk/libc                      (09a)
  host side: build.sh (zig cc) | tools/ image writers, generators, harnesses | pipeline/ gate |
             launcher/ (C#/WPF zelr.exe)                                                        (01, 14)
```

## Verified by running (2026-09-26, on a Windows 11 host)

- **Toolchain.** Git for Windows 2.55 (Git Bash), Python 3.11.8, Zig 0.16.0, and QEMU 11.1.0 installed per
  user in `%LOCALAPPDATA%\Programs\qemu`.
- **`bash build.sh`** succeeded in 55 s. It built 46 ring 3 programs, `cdboot.bin` (1,592 B), `mbr.bin`
  (131 B), `trampoline.bin` (200 B), `BOOTX64.EFI` (11,776 B), `zelr.elf` (11.3 MB) and `zelr.bin`
  (9.97 MB). The only warnings are unused static functions in `jsrun.h` and `jsdom.h`.
- **`bash run.sh -T`** (i440fx, `-m 64`, one CPU, rtl8139, IDE): **556 passed, 0 failed**. It skipped the SMP
  checks, MCFG and sound, which is expected on that machine.
- **`bash pipeline/gate.sh full`**: **every one of its 47 steps passed, in 8 min 44 s** (GATE_EXIT=0). The
  selftest reached 560 on the gate machine (`-m 256`) and 568 each on q35 and on NVMe. All four ISO boot
  paths passed, as did every desktop, USB, sound, network, https, browser, SMP, crash-safety, SDK and libc
  harness. QEMU 11.1 accepts the harness's `server,nowait` monitor syntax.
- **Reproductions** (scripts in [repro/](repro/)):
  - the network freeze (finding 12);
  - JS `call`/`apply`/`bind` (finding 13).

## Since 6048716

What has changed in the tree since the atlas was written, newest first. File:line references in the
numbered files are still to 6048716; where they disagree with this list, this list and the code win.

### 0.76.0 (in progress)

- **The document and window layer (11, 12; jsdom.h and the new jsnet.h, jsobs.h, jsurl.h, jswalk.h,
  jswin.h).** A page's nodes as the standard's objects (Node, Element, the interface prototypes, custom
  elements), events that capture and bubble, navigator as this browser is (user agent "zelr"), cookies
  through the browser's jar (HttpOnly hidden), URL and URLSearchParams, location and history, the
  window's clock (performance, requestAnimationFrame), sizes, scroll, matchMedia and getComputedStyle,
  localStorage and sessionStorage (in memory, per origin), fetch with Headers, Request, Response,
  FormData and AbortController (XMLHttpRequest in jsnet.h, replies to 4 MB), the Mutation, Intersection
  and Resize observers, crypto.getRandomValues and randomUUID on the new system call 67
  (`SYS_RANDOM`, `random_bytes`, capped at 64 KiB a call; a self test through a ring 3 program), TreeWalker
  and NodeIterator, DOMParser for HTML, data: and blob: addresses, Blob and File. Scripts see the page's own
  `<html>` as the document element (the parser keeps a root of its own above it, which the layout styles
  once). Script files to 4 MB (mapped), 64 a page, 6 MB in all, run only when the machine has room.
  pagetest 211 (was 87). Not there: XML documents, SubtleCrypto, attachShadow, request headers a page
  sets, methods other than GET and POST, FileReader. The page's 24 MB script memory with nothing freed is
  now the most common stop on big sites (js.h, `JS_MEM_CAP`).
- **Nothing lost to a missing stick (04 S22, vfs.c, fd.c).** With no stick mounted, every operation on a path
  under /usb fails (`on_usb`, `vfs_volume_missing`, and `fd_open` refusing it up front) instead of going to
  the files kept in memory, where a file copied to the stick was gone at the next start. [userspace]'s live
  tree section: four checks.
- **A stick's EFI partition left alone (04 S21, diskfs.c).** Partition type 0xEF is no longer one a stick's
  volume is mounted from. mountcheck: a stick whose only partition is an EFI System Partition is found and
  not mounted.
- **One task in the filesystem at a time (04 S25, fat.c).** A sleeping, re-entrant lock (`fat_enter`,
  `fat_leave`): every public fat.c call takes it, vfs.c holds it for the whole of an operation including
  its choice of volume, and `syscall_abandon` gives back what a killed task held (`fat_abandon`). The USB
  task mounting a stick could be preempted half way and the disk selected under it, writing the stick's
  geometry into the disk's record. [fat]: six checks (a rival waits and does not change the selection, a
  killed holder gives it back).
- **An image a small machine can hold (userland/build.sh, tools/check_image.py).** Programs are built
  without the debug information the compiler added by default, which was two thirds of the kernel image
  (27 MB, now 9 MB): on the 64 MiB machine the heap came out 20 MiB instead of 24 and programs had almost
  no memory. A gate step fails on any program carrying a .debug section or an image past 16 MiB.
- **Frames from a USB network adapter kept (05 C7, usbnet.c, xhci.c).** A receive left queued when a
  five second wait ran out swallowed the next frame unseen, and a DHCP answer lost that way was an address
  never had, about one start in five (the gate's usb address check). One receive is now kept queued and
  asked about without waiting, and its completion wakes the network task, which had spun while idle.
  netcheck: the address, and the task asleep while nothing arrives (6 wakes in five seconds, was 178).
- **/tmp emptied of folders (04 S19, layout.c).** One folder with anything in it at the front of /tmp kept
  itself and everything after it; each entry now goes as a tree (`remove_tree`, eight levels) and one that
  will not go is stepped past. [userspace]: two checks.
- **A disk port that will not start (04, ahci.c).** `start_port` waited for the engine with no bound, and
  `stop_port`'s bounded wait said nothing when it ran out; both now say (`bits_clear`), and `ahci_init`
  refuses a port that will not stop or start. [disk]: one check (the refusal itself is not exercised:
  QEMU's port always stops).
- **A slow server's answer read to its end (06b 10.6, tls.c).** A read waited twenty seconds for each half
  of a record whatever the caller asked (four seconds from a program), threw away what had come when that
  ran out, and ended the session, so a server quiet mid answer looked finished. Records are now read as
  they arrive and kept between reads (`fill_record`); only an ended connection ends the session.
  [tls 1.3]: two checks.
- **The page's own background (11, layout.h, browser.c).** The html element's background, or else the
  body's, fills the whole window behind the page (`ldoc.canvas`), as CSS carries it to the canvas: a dark
  page was dark for the height of its words and white below. layouttest: three checks; browsercheck: a
  short dark page dark all the way down.

### 0.75.0

- **Auto margins, and columns lined up (11).** In a row, auto margins take the room left after growing and
  before justify-content, shared evenly (`lay_flex_line`, `amarg`): `margin-left: auto` pushes an item to
  the far end, `margin: auto` centres one. In a column, an item lined up by align-items or its align-self
  (start, centre, end) or with an auto margin is as wide as what is in it (its width, else measured) and
  placed across the column; stretch still fills it. layouttest: four checks, each seen failing.
- **Background pictures and masks (11, css.h, browser.c).** `background-image`, the `background`
  shorthand's picture, `background-size` (lengths, percentages, cover, contain), `-position` and `-repeat`
  are read (`cstyle.bg_*`, `css_bg_layer`, from the layer the picture is in), and `mask`/`mask-image` with
  their size, position and repeat as a picture the box's colour is painted through. A box with one keeps
  it in `ldoc.bgs` (`lay_bg`, the url as written), and the browser fetches each once the page is laid out
  (`gather_backgrounds`, 32 a page, 16 MB), decodes it twice, over black and over white, keeping colour
  times alpha and the alpha (`pic_merge`: what differs between the two is what shows through), keeps a
  drawing as markup made at the size it is drawn, and draws it under the box's contents
  (`draw_background`, nearest neighbour). A linked sheet's url()s are written out against the sheet's own
  address before it is read (`css_urls_from`, `css_parse_sheet`, imports too). Not drawn: several
  pictures in one box, a picture clipped to a rounded box, `background-attachment`, `mask-mode:
  luminance`. layouttest: five checks; browsercheck: a sheet-relative picture, a mask's shape, a clear
  part showing the box behind.
- **Linear gradients (11, css.h, browser.c).** `linear-gradient()` in any layer, over or under the picture
  as the layers order them (`cstyle.bg_grad`, `bg_grad_under`): its direction (`to` a side or a corner, an
  angle in deg, turn, rad or grad, the old prefixed form naming where it starts) and up to six stops with
  their places, unplaced ones spread as CSS spreads them (`lay_gradient`, `lbg.stop_*`), drawn along the
  gradient line with colour and alpha interpolated together (`draw_gradient`, `sin65536`). `css_last_raw`
  is a colour before it is laid over white. Not drawn: radial and conic gradients, repeating ones as
  repeating. layouttest: three checks; browsercheck: a two-colour gradient's halves.
- **Pictures with clear parts, fitted and rounded (browser.c, 11).** An `<img>` whose file says it may
  have clear parts (PNG colour types 4 and 6 or tRNS, WebP's alpha flags, any GIF or drawing:
  `pic_may_be_clear`) is decoded over black and over white and drawn over what is behind it
  (`shown.alpha`): flattened onto white, a logo on a dark header sat in a white box. `object-fit`
  (contain, cover, none, scale-down; `litem.ofit`) centres the picture in its box at its own shape instead
  of stretching it, and `border-radius` rounds its corners. layouttest: one check; browsercheck: a clear
  half showing green, a picture fitted whole, one with round corners.
- **A style sheet asked for twice (browser.c).** A linked sheet whose fetch gets no answer, 429 or a 5xx is
  asked for once more; Wikipedia's lost its sheet this way after a burst of fetching. browsercheck: a
  sheet that answers 503 the first time.

### 0.74.0

- **Pictures sized by the page's rules (11).** An `<img>` takes the CSS width and height (a percentage left
  out while measured, `cstyle.width_pct`), the other side from the attributes' proportions when both are
  given, else the file's, and `max-width`; attributes and then the file's size only when the rules say
  nothing. layouttest: a picture 100% wide, a height alone, max-width.
- **Shadow trees written into the page (11, dom.h, css.h).** `dom_shadows` (called after every parse)
  rearranges `<template shadowrootmode>` into what would be drawn: its contents become the host's children
  and each `<slot>` takes the children assigned to it by name (its own contents only when none were); the
  tree's elements are marked `data-zs`, the host `data-zh`, slotted children `data-zl`. `css_parse_style`
  parses a style element inside a tree through `css_scope`, which writes every compound of every selector
  again to ask for the tree's mark, `:host` as the host and `::slotted(x)` as x among the slotted, leaving
  fonts and keyframes as they are; `slot` is `display: contents`. The page's own sheets still reach into a
  tree. `:not()` takes a list of compounds (`csel.nneg` of them side by side in `negs`), which MDN's
  `:host(:not([loaded],:focus-within))` needs. layouttest: the tree drawn, slots by name, fallback
  contents, the sheet reaching the tree and not outside it, `:host`, `:not()` with a list; browsercheck 31
  (a page whose only band is inside a tree).
- **Six properties (11).** `text-transform` (inherited; ASCII and Latin-1 letters, in `lay_word`); `inset`
  (the four offsets in one), and an absolute box with a left and a right and no width as wide as lies
  between them; `order` (flex items sorted in `lay_items`, stably); `align-self` (per item in
  `lay_flex_line`); the `flex` shorthand read whole (`P_FLEX`: `flex: 1` is 1 1 0, so columns come out of
  one width) with `flex-basis` and `flex-shrink` (0 holds its width); `aspect-ratio` (a box with no height
  as tall as its width says). layouttest: each.
- **Flex items sized as CSS sizes them (11).** A basis is where an item starts, not a floor or a ceiling: an
  item stops at its contents' width however small its basis (`flex: 1 1 0` beside a long menu had left
  Nature's logo a stripe), and whether a line grows or shrinks is decided by where the items would stop
  (`lay_flex_line`: one whose share of the room leaves it below its floor is held there and the rest shared
  again). A row being measured reports its items and gaps, not the room it would justify them across (a
  row pushed to its end measured as wide as whatever held it). A box without a width measured as narrow as
  it goes keeps its padding round what is in it, past the room it was measured in. A percentage of a width
  still being worked out is auto (`lay_pct_cyclic`, CSS's cyclic percentages): a width while measuring,
  unless it is the element asked about (`lctx.measure_root`, set by `lay_measure` only when the room is
  known) or its parent has a width of its own; a percentage basis likewise (`cstyle.basis_pct`). MDN's
  sidebar button (width: 100% in a host as wide as its contents) had taken half of every breadcrumb bar,
  and Spotify's cards (a column of basis 100%) the whole page each. layouttest: each, seen failing.
- **Words straight inside a flex row or a grid (11).** An item of their own, as CSS has them
  (`lay_items_next` returns a text node that is not only space; `lay_block` lays one out as a line;
  `lay_style` gives a text node its parent's inherited style). They were dropped: `<a style="display:
  flex">Sign in</a>` drew nothing, and W3C's, Mozilla's and Microsoft's menus had no words (atlas 11 §10
  12). layouttest: a row, a row of words alone, a grid.
- **Fields and buttons (11).** A field that is a flex item or a grid item is a field (`lay_block_placed`
  sends controls other than `<button>` to the inline code): it was an empty box, so what was typed into
  NHS's search box, or any search box in a flex row, was never drawn. A field takes the CSS width
  (auto while measuring, as a percentage of a width being worked out), height, min-width and max-width,
  and the width its row gave it (`flex_sized`). A `<button>` with elements in it is laid out as the box it
  is with them in it (`lay_button_box`), not as its words: GOV.UK's and Bloomberg's magnifying glasses and
  Ars Technica's menu icons are drawn; a click on anything inside a button is the button's (browser.c).
  layouttest: a field in a row, one that grew, a field's width and height, a button round an icon;
  formcheck (a search bar: a field 100% of a flex row, typed into, sent by a click on the button's icon).
- **Links, words and buttons that can be used (11, css.h, browser.c).** A link that is a block or a flex or
  grid item is a link (`lay_link_open` from `lay_block_placed` as well as `lay_inline`; each box and
  picture inside it carries it, and the link in force outside is put back on the way out, `stack_link`,
  `entry_link`): only links inside a line were, so no menu of block or flex links could be followed
  (python.org had 48 links and has 139). A picture or a box in a link can be clicked (`lay_link_at` takes
  LK_IMAGE and LK_BOX, words with their 2 pixels of slack): a site's logo could not be. Words belong to
  the element they are in (`lctx.word_node` into each text item's node), so a click on them reaches a
  handler on it. `:hover` matches the element under the pointer and each one it is inside, as CSS has it,
  and the browser lays the page out again only when what the :hover rules reach changes
  (`css_hover_reach`), not whenever the element under the pointer does. A `<button type="button">` no
  longer sends its form. layouttest: nine checks; formcheck 27 (the way to the search bar is a link in a
  flex row, and the bar's type="button" button sends nothing).
- **`@layer` (11, css.h).** Rules in a layer are read (a layer's block was skipped whole as an at-rule not
  drawn: Primer's components on GitHub and everything Tailwind 4 writes, and weather.com and cloudflare.com
  came out unstyled). A rule in a layer is weaker than one in none, and a layer named earlier than one named
  later (`crule.layer`, the rank from `css_layer` in the order names are first seen, in `@layer a, b;` or at
  a block; nested ones named "a.b"; one with no name a layer of its own), which `css_hit_after` puts before
  specificity. Not kept: a layer's own rules beating its sublayers', and the reversal for `!important`,
  which is not read. layouttest: four checks.
- **Hex colours with an alpha (css.h).** Four digits are the short form with an alpha (`#0000` is
  transparent: normalize.css's `a { background-color: #0000 }` had put every link on GitHub on a black
  box), and four and eight digits are composited over white as rgba() is. layouttest: three.
- **Host tools.** cssq takes a node number (`@n`, from laydump) and prints each element's attributes.

### 0.73.0

- **JavaScript (12).** The engine is JavaScript of 2015 to 2021: prototypes and lexical scopes (`let`/`const`
  with a fresh binding per loop turn, hoisting: B22), arrows, template and tagged template strings,
  defaults, rest, spread, destructuring, getters and setters, `?.` `??` `??=` `**`, `this` in a plain call
  (B23), no 24-argument limit (B20), a parser depth guard (B9), direct eval; classes with fields, private
  names, static blocks, `super`, `new.target`; Symbol, iterators, for-of, Map/Set/WeakMap/WeakSet and the
  2015-2021 library; generators, Promise with a microtask queue, async/await and async generators, on
  coroutines with their own stacks (System V in zelr, Windows x64 on the host); patterns with lookbehind,
  backreferences, named groups and `$<name>`, the s/y/u/d flags and `\p{}` (B11, B16), reading whole UTF-8
  characters; ArrayBuffer, the nine typed arrays and DataView; atob, btoa, TextEncoder, TextDecoder, `self`,
  DOMException. New files: `jsarr.h`, `jsco.h`, `jslib.h`, `jsprom.h`, `jstyped.h`. jstest 384 (was 248),
  pagetest 87, each new check seen failing on a broken build. Not there: modules, Proxy, Intl, BigInt
  arithmetic; strings are indexed by byte; no garbage collector and a 24 MB cap a page; WeakMap holds its
  keys. The image grew from 14.3 MB to 20.2 MB (jstest 2.4 MB, the browser 4.8 MB).
- **Templates and noscript (11).** `<template>` is hidden by the browser's sheet and its style elements,
  linked sheets and pictures are not gathered (`in_template`); a meta refresh inside `<noscript>` is not
  followed (`in_noscript`), since scripts run here. GitHub's `{{ message }}` placeholder and stray dialog
  were template contents. layouttest 179, browsercheck 29 (a refresh, one in `<noscript>`, a template).
- **display: contents (11).** `D_CONTENTS`: an element with no box (`lay_style` clears its margins, padding,
  borders, background and size); a flex row or a grid takes its children as items (`lay_items_start`,
  `lay_items_next`, `lay_items`, and each item's parent style from `lay_item_parent`, so what the wrapper
  passes down still reaches them); elsewhere it is laid out as inline. `@supports` says yes to it now.
  layouttest 184.
- **The space after the colon (11).** `css_declare` kept it, so on any sheet not minified every keyword
  read by its first letters missed: `display: none`, `position`, `float`, `clear`, `text-align`,
  `font-weight`, `list-style` and the rest were ignored, while colours and lengths (whose readers skip
  spaces) applied. Closed menus, fixed bars, floats and centring on Python's docs and home page, W3C,
  arXiv, WordPress, Wikipedia and Ars Technica now come out as written. Every layouttest check used
  compact CSS, which is why none saw it; four now use a sheet written by hand. layouttest 188.
- **Grid placement (11).** Line names in `grid-template-columns` (`[content-start] ... [content-end]`,
  `lgline`, `lay_grid_tracks`); a track read whole, brackets and all, and worked out with `css_len_at`
  (MDN's `max(1rem, calc(50vw - 720px))` was four tracks); capped tracks, `minmax(least, length)` (`GT_CAP`),
  grown to their caps before `fr` shares. Items placed by line along both axes (`lay_grid_parts`: numeric
  `grid-area`, `grid-row`/`grid-column` and their longhands; `lay_grid_axis`, `lay_grid_line`: numbers,
  negative numbers, spans, names and `name-start`/`name-end`); a grid with an item on a named row is placed
  the way the rules place it (`lay_grid_placed`: named rows first, then the rest from a cursor over an
  occupancy map, rows sized from measured items, row spans), and one without keeps the row by row pass.
  MDN's home page (sections placed on `content`) and the BBC's lead story (picture on row 1 at column 9,
  words beside it) now come out as other browsers draw them. Not done: `grid-template-rows`, dense
  packing, named rows. layouttest 198.
- **Pages to eight megabytes (11).** `SRC_MAX` 8 MB (was 3; mapped, paid for as used): Netflix's front
  page is 3.2 MB unpacked. It then overflows the document's text store (`DOM_ARENA`, 3 MB), which is left
  as it is because `svg_render` and the test programs hold a whole `ddoc` each. browsercheck 30 (a page
  with 3.3 MB of comment before its band).

### 0.72.0

- **WebP (13).** `webp.h`: lossless and lossy WebP, transparency, the first frame of an animation, from RFC
  9649 and RFC 6386, tables taken from the RFC texts (`tools/genwebptab.py`). webptest (136) decodes twelve
  pictures zelr draws (`tools/genwebp.html`) that Edge's encoder made, against Windows' decoder. The browser
  asks for image/webp first and knows it by its bytes.
- **Drawings in the page (11, 13).** An `<svg>` in the markup is a picture (`lay_drawing`, `drawing_of`,
  `svg_render_tree`), sized by the page's rules, then its attributes (in any unit), then its viewBox, drawn
  on the backdrop behind it with currentColor the text's colour; the browser's sheet no longer hides them.
  Logos and icons appear on GOV.UK, the BBC, Ars Technica, MDN, GitHub and The Verge; GOV.UK's header no
  longer squeezes its buttons. A percentage height resolves against a parent with a height of its own
  (the BBC's wordmark was drawn the width of the page); a percentage width is auto while measured, and a
  drawing with no size is 300 wide there (The Verge's). A picture that is a flex item is drawn.
- **Checks.** layouttest 176 (ten for drawings and pictures in rows), svgtest 51 (four for drawings in a
  page), webptest 136 in ring3check (47), browsercheck 25 (a drawing in the page, a WebP picture); each
  seen failing on a broken build.
- **Rows and sheets (11).** A flex row's items are its children that are drawn and in the flow: one not
  drawn (display: none, `hidden`) is no item, and one positioned absolutely is laid out on its own from the
  row's top, once (`lay_flex`). GOV.UK's closed menu, `hidden` with width: 100%, took a line of the row and
  pushed the search button under the logo. Up to 96 linked sheets (`SHEETS_MAX`, was 40; 128 addresses
  remembered): The Verge links 66 and the rule that keeps its drawer shut was past the fortieth.
  layouttest 178, browsercheck 26 (a rule in the sixtieth sheet), each seen failing on a broken build.
- **Grid tracks (11).** A flexible track (fr, auto) is never narrower than the least its items can be
  drawn in (`lay_grid_least`: the width an item asks for, else its min-width, else its narrowest content;
  nothing for one that clips), and one held at that leaves the sharing to the others (`lay_grid_share`);
  `grid-auto-flow: column` makes a column per item along one row, at `grid-auto-columns`
  (`lay_grid_tracks`); a grid that would scroll sideways wraps to the columns that fit, as a scrolling
  flex row already did. The Guardian's strip of 280 pixel cards in six 1fr columns was six slivers; it is
  two cards a row. A box being measured is as wide as it asks to be, up to the page (laid out it is still
  kept to its room), and a flex item's floor is measured with its own width set aside, as the rules have
  it for flex and not for grid.
- **Checks.** layouttest 166 (five new: a 1fr column as wide as its item, the other column taking what that
  leaves, a sideways grid wrapping, auto-flow column, auto-columns), each seen failing on its own broken
  build.
- **Host tools (14).** `laydump` prints where every item of a real page was laid out and, through
  layout.h's `LAY_TRACE` hook (compiled only there), every block's box, drawn or not.

### 0.71.0: custom properties, calc(), every style sheet a page links, and GitHub

- **Style sheets (11).** Up to 40 linked sheets (`SHEETS_MAX`, was 12), each address read once
  (`sheet_seen`), and each sheet's `@import`s read before it (`css_next_import`, `gather_imports`, one
  level, with their media queries). GitHub links 31, the same one five times, and the rule that hides its
  menu was in the 27th: its repository pages now show as its header, tabs and file list.
- **Custom properties (11).** `--name: value` is kept (`P_CUSTOM`, as the text "--name:value"); each element's
  are gathered before anything else about it is applied into an inherited chain (`cstyle.vars`,
  `lay_var_push`/`lay_var_find`, cached per element and parent chain); a declaration using var()
  (`CSS_HAS_VAR`) has it substituted (`lay_var_subst`, fallbacks, nesting to eight, cycles to nothing) into
  `lay_arena`, which lasts the layout; a shorthand with var() in it is split once known (`P_DEFER`,
  `lay_apply_short`). The `<html>` element's style is now computed and is the body's parent, so `:root`'s
  properties, colours and size reach the page.
- **calc() (11).** `css_len_at`: calc() of sums and products of lengths, percentages and numbers, and min(),
  max() and clamp(), in lengths, font sizes and gaps. Grid templates and areas are kept as pointers now
  (they can come from a resolved var()).
- **Masks and missing pictures (11).** A box with a `mask`/`mask-image` draws no background (an icon made of
  a square of the text colour and a mask was a black square); a picture that did not arrive but has a
  size keeps its room as an empty frame instead of its alt text poured into a narrow column. Pictures per
  page: 48 (was 24).
- **Colours and hiding (11).** `css_last_alpha`: a background under 5% opaque is none, text under 5% is laid
  out and not drawn (`cstyle.ink_none`); `.5` alphas are read (they were lost, and a faint colour drawn
  solid); hsl()/hsla(), `currentColor` backgrounds, `#rrggbbaa`. `font-size: 0` lays out no words;
  `text-indent` keeps its sign and moves the first line only (it moved every line).
- **Memory (11).** The page's and a sheet's source, the tree, the style sheet and the laid out page are
  mapped at start (`map`, paid for a page at a time) instead of declared: the browser's loaded size went
  from 26.8 MB to 5.9 MB, after it had stopped starting on termcheck's 64 MB machine with other programs
  open (the gate failed twice on that).
- **Promised pages (03).** A system call given a buffer in a page the program was promised but has not
  touched -- a mapping, or the stack below where it has grown to -- is handed the page (`user_range_ok` calls
  `user_fault_fill`), as touching it would have; it was refused as a bad pointer, and with the browser's
  buffers mapped every network read into a fresh page failed (five browser steps of the gate). A signal
  frame that reaches below the last touched stack page is filled the same way (`stack_is_there`); the
  program was ended instead.
- **Checks.** maptest and sigtest one more each (a call writing into an untouched mapped page; a signal
  raised with the stack at the top of an untouched mapping), each seen failing with its fix taken out.
  layouttest 161 (six more for the colours and hiding). layouttest 155 (16 new: :root variables, the nearest one inherited, fallback, a length from a
  variable, a variable made of another, calc less, min, clamp, calc products, masks, @import past @charset,
  print imports skipped, an import's media, the missing picture's frame and its alt text, one without a
  size), each seen failing on its own broken build; the :root check needed a page with a real `<body>` and
  the min() check its smaller value second before their breaks showed.

### 0.70.0: grids, GIF pictures, and pages that ask what the browser can do

- **Grid (11).** `display:grid` is laid out (`lay_grid`): columns from `grid-template-columns` (lengths,
  percentages, `fr`, `auto` as 1fr, `minmax()`, `fit-content()`, `repeat(N, ...)` and
  `repeat(auto-fill|auto-fit, ...)`, `LAY_GRID_COLS` 32), items poured in a row at a time in document order,
  `grid-column: span N` and `1 / -1`, rows as tall as their tallest item, `align-items`, the one gap for both.
  Named areas (`lay_grid_named`): `grid-template-areas` read into rectangles, items placed by `grid-area`
  whatever order they were written in, items naming no area on rows of their own below. Not done: placement
  by line number, `grid-template-rows`, dense packing.
- **@supports (11).** `css_supports` answers as this browser would: no for grid (so a page's no-grid fallback
  is what it gets -- an encyclopaedia's grid put its contents list first and its article after), `display:
  contents`, container queries and `:has()`; `not` turns it round.
- **Rows that would scroll (11).** A flex row with `overflow: auto` or `scroll` (`cstyle.clip` 2) wraps: it
  cannot be scrolled inside a page here, and one line of cards drew them over each other. `max-height` still
  only cuts a box that hides its overflow (`clip` 1).
- **Icon buttons (11).** A button's label leaves out the text of any `<svg>` inside it (`lay_words_of`): an
  icon's `<title>` ("Chevron Left") is for a screen reader, not the button's words.
- **GIF (13).** `gif.h`: the first frame, variable-width LZW with clears and the one-past-the-table case,
  local and global colour tables, the transparent index over `bg`, interlaced rows put back in order, and
  pixels a short file never delivered left as `bg`. The browser decodes GIFs by their bytes. giftest
  checks every pixel of four GIFs made by Windows' GDI+ encoder (`tools/gengif.ps1`), so the decoder is
  not checked against itself.
- **Checks.** layouttest 139 (17 new: @supports three ways, the scrolling row, the icon's title, grid tracks,
  gaps, spans, auto-fill and fr, named areas), each targeted one seen failing on a broken build; giftest 16,
  each decoding feature seen failing on its own break (the one-past case, code widths, clears,
  interlacing, transparency, reading past the end, pixels that never arrived). The first cut-off check
  found that missing pixels were drawn as colour 0, which was fixed.

### 0.69.0: pages laid out the ways pages are laid out, and readers that survive a change of names

- **Layout (11).** Tables are laid out as tables (`lay_table_plan`, `lay_table_rows`): each cell measured
  narrowest and widest, columns sized from them, the table as wide as its columns or as told (`width`),
  colspan and rowspan, rows as tall as their tallest cell, cells middle-aligned unless `valign` or
  `vertical-align` says otherwise, cell backgrounds stretched to the row, captions first. They were blocks
  with the cells run together as words, so a table in a cell ran its rows into a paragraph. Presentational
  attributes (`lay_hints`: `bgcolor`, `width`, `height`, `align`, `valign`, `cellpadding`, `cellspacing`,
  `border`, `nowrap`, `<font color size face>`, `<body text>`, `<img align>`) sit between the browser's own
  rules and the page's in the cascade, which now applies the browser's rules first whatever their
  specificity (`csheet.ua_rules`). Without a doctype (`ddoc.standards`) a table resets `text-align`, the
  quirk a `<center>`ed page of tables relies on; `<center>` and `align=center` round a table centre it.
- **Inline-blocks and blocks in inline (11).** An inline-block (and inline-flex, and a stray table cell) is
  laid out as a block and placed on its line as one piece (`lay_inline_piece`, `lctx grp_*`); something
  absolutely positioned in a run of text is laid out where it is and takes no room. A block inside an inline
  element ends the line and is laid out as a block. A measurement is one pass: a box measured is as wide
  as its content and frame (`shrunk`), not the room it was measured in; lines are not centred and auto
  margins not applied while measuring. Measurements are kept for the layout (`lay_mcache`).
- **Flex (11).** `lay_flex_line`: items shrink in proportion but never below their narrowest
  (`lay_measure` at 1), `flex-wrap` wraps, alignment uses the heights the items came out at, and past
  `LAY_FLEX_MAX` (128, was 32) the rest are laid out below instead of dropped.
- **Floats (11).** `lay_float` puts a float at its side, past the floats there, down past them when there is
  no room; `lay_line_start` shortens a line beside a float (`lay_float_room`) or moves it below when too
  little is left; `clear` and `<br clear>` go below (`lay_cleared`); a block always grows to hold its floats.
  Negative margins are kept (auto is `CSS_AUTO_OFF` for margins too), so a float pulled back with
  `margin-left:-100%` goes where it was meant to.
- **CSS (11).** Media queries are evaluated (`css_mq`): widths kept per rule (`mq_lo`, `mq_hi`) and compared
  with `css_view_w` in `css_collect_chain`; print, dark schemes, more than one pixel a pixel, portrait and
  touch never apply; a `<link>` or `<style>` `media` attribute is read the same way (`sheet_media`,
  `css_parse_in`). Selectors: attribute values (`= ~= |= ^= $= *=`, with `i`), `+` and `~`, `:last-child`,
  `:only-child`, `:*-of-type`, `:empty`, `:nth-child`/`-of-type`/`-last-child(An+B)`, `:checked`,
  `:disabled`, `:enabled`, `:not(one compound)`, `:is`/`:where(one compound)`, and elements with no `T_`
  by name (`csel.tname`); any other state (`:focus`, `:target` ...) never matches, where it used to be read
  past and match always. `vertical-align`, `border-spacing`, `border-collapse`, `overflow`, `clip`,
  `clip-path: inset(50%)`, `float`, `clear` are read; a box clipped to nothing, or a pixel or two across
  with its overflow hidden, is not drawn (`lay_unseen`); `max-height` only cuts a box that hides its
  overflow; `[hidden]` hides. A transform's translation (`translate`, `translateX/Y`, `translate3d`, with
  `calc()` of a percentage and pixels) moves a box once it is laid out (`lay_translate`), which is how a
  dialog is centred and an off-canvas menu kept off. Limits: 16000 rules, 40000 selectors, 64000
  declarations, 2 MiB of text.
- **Browser (11).** 12 style sheets (was 6), 3 MiB pages (was 1), 40000 elements; a picture's address
  from `data-src` and its kin when `src` is a placeholder, from `srcset` when there is no `src`, from
  `photo.jpg` for `photo.jpg.webp`, and from the address itself for a `data:` picture; pictures are asked
  for as PNG, JPEG, GIF or SVG (`web_accept`), not anything. An icon button is named by `aria-label` or
  `title`, not "Button".
- **Sites (11, sites.h).** Every reader falls back when the name it keys on is changed: videos found by shape
  (`yt_videos_any`), details by the watched id (`yt_details_any`) or the head tags (`yt_meta_video`),
  storyboards by address, the comments token in any comments section, a channel's uploads from its feed,
  Twitch's Client-Id learned from its page, a minimal question when a field is renamed, and a channel
  card from its page when the API gives nothing.
- **Host tools (14).** `tools/host/`: any ring 3 program built for Windows on a system-call shim, TLS by a
  local proxy, pages rendered and tiled by `render.py`. Not part of the gate.
- **Checks.** layouttest 122 (60 new), each targeted check seen failing on a broken build (tables; table
  parts one at a time; inline-blocks, blocks in inline, measuring, the cascade, `<font>`, `[hidden]`;
  media queries; selectors; flex; floats, negative margins, unseen text, max-height, block pictures,
  button names; translation and calc). sitetest 144 (40 new), each seen failing.

### 0.68.1: the line that says a Google search was answered by DuckDuckGo, shown

- **Browser (11).** `build_noted` put its line in front of the whole page, before DuckDuckGo's `<html>`, and the
  reader put it in the head, which is never drawn: the status said so and the page did not. It goes just inside
  the page's `<body>` now. sitecheck 25: the note's colour covers more than 2000 pixels of the page after a Google
  search (41,100 when it ran); 0 with the old placement.

### 0.68.0: arrays that cannot be made to write past themselves

- **JavaScript (12 §10 B7).** An element index is kept among the elements only up to `JS_ARR_MAX` (4M), with the
  arithmetic in 64 bits; one further out is kept as a named property (read back the same, no length), where it
  used to wrap the size to nothing and write past it, or loop for ever doubling to zero. `js_index_of` refuses a
  leading zero and anything past 4294967294 (they are names), where "4294967296" wrapped to 0. Setting `length`
  to something that is not a whole number up to 2^32-1 throws RangeError, and a longer length becomes the
  length, filled with undefined. The numeric index fast path falls through to a property past the elements.
- **Checks.** jstest 248 (6 new): failed six with the index, length and fast-path changes undone; with only the
  size guards undone, jstest itself was killed by a write to an unmapped page (the corruption the atlas
  described), so the test program is the check there.

### 0.67.0: a video's comments, and a page kept out of the toolbar

- **Comments (11, sites.h).** A watch page carries a token for its comments in the section named
  `comment-item-section` (`yt_comments_token`, with the page's `INNERTUBE_CLIENT_VERSION`); `site_youtube_comments`
  posts it to `https://www.youtube.com/youtubei/v1/next` as YouTube's own page does, into 768 KiB of its own, and
  `yt_comments_write` writes the first twenty `commentEntityPayload`s: who, when, likes, replies and the words.
  The browser adds them to the end of the page (`site_append`). Only a token of token characters is sent.
- **Browser (11).** The page was drawn into a surface starting at the window's top row, with the bevel put back
  afterwards, so a line or box half scrolled off the top (always drawn, since items within 8 pixels above are)
  landed on the toolbar. It is drawn into a surface of only the well's rows now.
- **Checks.** sitetest 104 (7 new): the token of the comments section and not another section's first (that
  section is listed first in the test), none when comments are off, none for a token that would close its quotes,
  the answer's comments with nothing for one with no words, the counts said right, and the end of the page. They
  failed with the first token in the page taken, comments with no words kept, "likes" for one, the token taken
  whole, and the addition put after the end. sitecheck 24: at least five comments on a real video. browsercheck 23:
  a long page scrolled down six times under the toolbar leaves the toolbar's pixels as they were; failed with the
  old surface.

### 0.66.0: a page that uses up its memory no longer takes the browser with it

- **JavaScript (12 §10 B8).** At the 24 MiB cap `js_alloc` returned NULL, and the places that use a string or a
  property without asking (js_find's `key->hash` first) took the browser down. It still stops the script at the
  cap, but lets the C already in a statement draw on a 1 MiB spare (`JS_MEM_SPARE`) to get back out, and
  `js_find` takes a missing key as not found. jsdom then runs none of that page's handlers, timers or requests
  (`jd_spent`), and does not wake the browser for them. Not covered: a single allocation after the cap larger
  than the spare still returns NULL (the big ones, js_concat and friends, already check).
- **Checks.** pagetest 85 (3 new): a page whose script fills an object until the cap stops with the memory message
  and the program is still there, its click handler does not run, and it asks for no timer. With the spare, the
  key guard and `jd_spent` taken out, pagetest itself was killed: "page fault at 0x4".

### 0.65.0: YouTube playlists and Shorts

- **Sites (11).** `yt_videos` reads `shortsLockupViewModel` (the id from its `reelWatchEndpoint`, the words from
  its `accessibilityText` with " - play Short" and its dash taken off) as rows linked to the ordinary watch page,
  and a playlist `lockupViewModel` as a row linked to `/playlist?list=<id>` with its first video's picture; a
  playlist's own page is headed from `playlistMetadataRenderer`, as a channel's is. `yt_row` takes the address
  and the picture's video separately.
- **Checks.** sitetest 97 (5 new, and the watch page now wants its playlist as a third row): failed with Shorts
  left out, playlists left out, the playlist heading not read, a list id of anything accepted, and the first
  trimming of a Short's words, which also took an accented letter before the dash. sitecheck 23: a YouTube chart
  playlist read from the real site.

### 0.64.0: page scripts that wait for the window, set handlers as properties, and break out of loops

- **jsdom (12 §10 B17).** `el.onclick = f`, `document.onclick = f` and `window.onload = f` are called
  (`jd_run_prop`; they were stored and never read). The window has `addEventListener` and
  `removeEventListener`, and a bare `addEventListener(...)` is the window's (`JD_WINDOW`, `jd_listener_host`);
  it had none, so the first line of many scripts threw and the rest of it never ran. Events reach the window
  after the document, and `load` also runs the body's `onload` attribute. `return false` from an attribute or a
  property handler refuses the default. A listener on the document or the window has it as `this`. Script
  elements run only with no type or one of JavaScript's names (`jd_script_type_runs`): JSON-LD, templates and
  modules are left alone, where their parse error had become the page's only reported error.
- **JavaScript (B12, B4).** `+` with any object is text (`[1,2] + 3` is "1,23", `[] + []` is ""); `==` between
  an object and a primitive compares the object's text by the primitive's rules (`[1] == true`, `[] == false`).
  A label goes only to a loop (a label on a block no longer lands on the first loop inside it); for-in takes its
  label and passes other labels' break and continue on; a switch passes a labelled break on.
- **Checks.** jstest 242 (8 new), pagetest 82 (11 new): they failed with property handlers never run, the window's
  listeners not declared, the body's onload skipped, `return false` ignored, every script type run, `this` on
  the document undefined, arrays added as numbers, loose equality as text, labels handed to any statement,
  for-in's old break and continue, and the switch's old break (16 checks).

### 0.63.0: Twitch's past broadcasts, their frames, and its search

- **Twitch (11, sites.h).** A channel's page lists its last ten past broadcasts (`videos(first:10)`), each linked
  to `/videos/<id>`, which is now a page of its own (`TW_VIDEO`): title, channel, category, views, length, day,
  picture, and frames from it. Twitch's answer names a list of storyboard sheets (`seekPreviewsURL`, on
  `*.cloudfront.net`); `site_twitch` fetches it into a buffer of its own and `twitch_frames` shows the smaller
  quality's first, middle and last sheets, captioned. Every Twitch page carries a search box (`/search?term=`,
  `TW_SEARCH`, `searchFor` channels): live ones as rows with what they stream, the rest as offline, with
  followers. Only numbers for a broadcast id, only login characters for a login, only file-name characters for a
  sheet, and only Twitch's servers for a list or a picture.
- **Checks.** sitetest 92 (16 new): failed with the list's host check, the sheet-name check (alone: with the
  quality choice also broken the high list, which has no bad name, was used), the choice of the smaller frames,
  the broadcast id and login checks, a broadcast number made of anything, and a search term's quotes kept. The
  base address's off-by-one (a character past its last slash) was caught by the first run. Not in sitecheck: a
  broadcast's number does not last, and a search may find nobody live; both were read from the real Twitch by
  hand (10 past broadcasts for a channel, 7 channels for "chess", a broadcast page with its sheet).

### 0.62.0: frames from a video, a start page, and JavaScript's strings and JSON

- **Frames (11, sites.h `yt_frames`).** A watch page shows frames from the video from YouTube's storyboards
  (`playerStoryboardSpecRenderer`): the level whose frames are the largest no wider than 160, and its first,
  middle and last sheets, each captioned with the stretch it covers. Only from `https://i.ytimg.com/sb/`;
  none for a stream that is live. sitetest live prints `frames N`; sitecheck wants at least one for a video.
- **Start page (11, browser.c `show_start`).** The browser opens on `about:start` (typed or started with no
  address): a search box and links to YouTube, Twitch, Wikipedia, Hacker News, DuckDuckGo and example.com, each
  said plainly. It needs no network, where example.com was an error on a machine without one.
- **Status bar (09a, ui.h `ui_status_fit`, `ui_fit_text`).** A long status and a long title were drawn through
  each other. The left keeps its room and the right gives way, down to a third of the bar (or its 128-pixel
  panel in the classic look), cut with "..." and never through a character.
- **JavaScript (12 §10).** B11: `substring` and `substr` are no longer `slice`; `indexOf`, `includes` and
  `startsWith` take their position; `lastIndexOf`, `endsWith`, `padStart`, `padEnd`, `trimStart`, `trimEnd`,
  `at` and `concat` are there; `replaceAll` replaces them all; a string pattern's replacement knows `$&`, `$$`,
  `` $` `` and `$'` and a function replacement is called. B22: `var x;` leaves a value x already has (`let x;` still
  starts again). B23: top-level `this` is the global object (a plain call's is still undefined). B9: an array
  inside itself becomes text empty where it comes round again, and each element is made text once (it was
  twice, doubling per level of nesting); `JSON.stringify` of an object inside itself is a TypeError. B15:
  `JSON.stringify` writes into a growing buffer (it concatenated a character at a time and ran a page out of
  memory at about 7 KB), escapes control characters, writes NaN and the infinities as null and returns
  undefined for undefined; `JSON.parse` undoes `\u` escapes and surrogate pairs and throws SyntaxError on
  anything that is not JSON.
- **Checks.** jstest 234 (33 new): failed with substring and substr as slice, indexOf's position ignored,
  replaceAll as replace, `$&` ignored, `var x;` resetting, top-level this undefined, the join's cycle guard
  removed, JSON's cycle check, control escapes and NaN rule removed, and trailing text accepted by parse (17
  checks). Not run against the old code: the twice-made join (a 200-deep array would not end) and the old
  concatenating JSON writer, which the rewrite replaced. sitetest 76: the frames checks failed with the level
  choice, the host check and the live skip removed; the status bar checks with `ui_status_fit` doing nothing
  and the cut going through a character (24 of 101 widths). sitecheck 20: the start page check failed with the
  start page replaced by a message.

### 0.61.0: YouTube and Twitch, read another way

- **Sites (11, new `userland/sites.h`).** YouTube and Twitch send an application rather than a page, and the
  browser showed nothing (YouTube: "shown as far as it fits", a script stopped) or Google's "please click here if
  you are not redirected". YouTube's pages carry what they would show as data (`ytInitialData`,
  `ytInitialPlayerResponse`); `site_youtube` reads it where it lies (`sj_skip`/`sj_find`/`sj_str`/`sj_text`, no tree)
  into a plain page: searches, watch pages (title, channel, views, length, description, the videos beside it from
  `videoRenderer`, `compactVideoRenderer`, `gridVideoRenderer` and video `lockupViewModel`s), channels, each row a
  thumbnail from `i.ytimg.com`, links back to YouTube's own addresses, the channel linked by its canonical address
  or `/channel/<id>`. Twitch's pages come from its public GraphQL API (`gql.twitch.tv`, the Client-Id its own page
  sends): who is live, the categories, a category's streams, a channel live or offline. A Google search
  (`google.<country>/search?q=`) is sent to DuckDuckGo's lite page with a line at the top saying so. Video is not
  played (H.264 in pieces chosen by the site's script; no decoder here), and each page says so. The browser fetches a
  YouTube page into 4 MiB of its own (`SITE_SRC_MAX`, freed after), since the data is 1.5-2.3 MB; `SRC_MAX` is
  unchanged. fetch.h gained `web_body_type` and `web_extra` for the API request. The browser prints
  `browser: <address> -- <title> -- <status>` on the console after each page, for the harness.
- **Layout (11).** Two faults found drawing those pages. A form field or a hidden element that was the last thing
  in its parent climbed out of it when skipped (`lay_inline`), so the rest of the document was laid out twice, once
  flowed on to the field's line: every search form ending in a button did it (`lay_past`). And a flex item was
  laid out at its own `width` rather than the width its row gave it, so `flex:1` with a width never grew and a
  percentage was of its share (`flex_sized`). Pictures in text (emoji, dingbats, flags) and the marks that join
  or colour them are left out rather than drawn as `?` (`html_fold_cp`).
- **Checks.** `sitetest` (ring3check, 66 checks on pages of the sites' shapes, no network) failed with: escaped
  quotes not counted in `sj_find`, surrogate halves not paired, the duplicate video kept, a login not limited to
  login characters, a quote left in a category name, `google.example.org` taken for Google, `<` not escaped, and a
  channel address accepted part way. layouttest: the words after a form ending in a button and after a hidden last
  child are laid out once (both 2 with the old climb), a growing flex item takes the rest of its row (250 of 500
  with the width taken again), pictures left out of the text (failed with the fold removed); the shrink check passed
  with the width taken again (a block never grows past what it is given) and stays as a guard. `tools/sitecheck.py`
  (gate full, outward, like tlscheck): the reader on a real YouTube search and watch page and Twitch's front page
  and directory, and the browser on YouTube, Twitch and a Google search; it failed with the YouTube hook, the
  Google redirect and the Twitch stream rows broken. `/bin` holds 64 programs now (49 used).

### 0.60.0: progressive photographs

- **JPEG (13).** Progressive files were refused, and a large share of the photographs on the web are sent that way.
  They are decoded now: every block's coefficients are kept, each scan is decoded into them (DC first and
  refined, AC bands with end-of-band runs, AC refined by successive approximation), and they are multiplied out
  and transformed at the end. A file that stops part way is shown from the scans that arrived.
- **Test pictures.** Nothing on the host writes a progressive JPEG, so `tools/genjpegprog.py` is an encoder of its
  own from the specification, writing each picture baseline and progressive from the same quantised coefficients
  (`userland/jpegprog.h`). Windows' decoder gave the same pixels for every pair when it was written. jpegtest wants
  the same pixels from both, byte for byte, for six pictures between them carrying every kind of scan, odd sizes,
  three samplings, one component and restart markers; it failed with the refinement's sign swapped, an end-of-band
  run one too long, a one-component scan walking the MCU grid, the DC refinement ignored, correction bits
  ignored, and a cut file refused. Not checked: resetting `eobrun` at a restart, which passed broken because a
  valid file never carries a run across a restart marker (the encoder ends every run before one); it matters only
  to a damaged file.
- **framecheck, under load.** Two of its checks were tried with three harnesses at once: the stars check wanted 20
  frames in four seconds, which a loaded host does not draw, and now wants 4; and "no commit drawn late" is now
  an average (`lag_ticks` over `lag_count`, at most 3 ticks a commit), because a count of late ones swung with
  where the blinks fell -- the loop that nothing wakes drew 2 of 5 late in one run, which a looser count passed.
  With the average it failed at 21 ticks over 4 commits; the fixed build waited 1 tick over 5. deskcheck's snap and
  screen size checks also failed once each under that load and pass alone, as the gate retries them.

### 0.59.0: more than one secure connection at a time

- **TLS (06b §10.2, §10.3).** `sys_connect_tls` refused a secure socket while any other was open, a limit left
  from when tls.c had one session; so every program got `NET_ERR_BUSY` while the browser sat on an https page. The
  loop is gone. x509.c's chain was the last static a second handshake could share (a kernel task such as the
  shell's fetch can be preempted mid-check); each check allocates its own now. tlscheck: a program opens two secure
  sockets at once and both answer (failed with the loop back). Not checked: the preemption race itself.
- Not done: the browser still fetches a page's pictures one at a time; doing them at once needs sockets a program
  can wait on together.

### 0.58.0: a machine that sleeps when nothing is happening

- **poll (03 §4).** `fd_poll` slept a tick and looked again, so a program waiting in poll ran a hundred times a
  second and heard of a write up to a tick late. It sleeps on one channel now, which `fd_poll_wake` wakes from
  every pipe read, write and close and from `input_wake`. polltest: a quarter second waiting on a pipe is 1 slice
  (26 with the tick sleep back), and the write, not the writer's leaving, is what wakes it (1250 ms with the
  write's wake removed). Not checked: the console's wake, which no program polls for.
- **The network's task (06a).** It went round every 10 ms with nothing arriving. A card that interrupts already
  wakes `net_arrived` for each frame, so the task sleeps on that now, a second at most (USB networking is still
  polled). netcheck: 5 runs in five idle seconds, 576 with the tick sleep back.
- **The desktop's loop (07 §4.2).** It went round every tick. It sleeps until the next look at the theme or the
  clock, or a tick while anything moves, and whatever asks it for a frame from outside wakes it. framecheck: 52
  passes in eight idle seconds (failed with the tick wait back), and no commit drawn more than five ticks late
  (failed with commits not waking it).
- **The theme (07 §3.6).** Its four looks a second each read `/zelr.cfg` off the disk. It reads only when
  `vfs_changes()` has moved. framecheck: no disk reads in eight idle seconds (failed with the check removed).
- **framecheck** enters the desktop through `enter_desktop`, which waits 90 seconds for the terminal and types the
  command again once: the 0.57.0 gate's first framecheck on the busy host measured a shell prompt for a desktop
  that never started. Its first check says whether the desktop came up (failed with the shell's `desktop`
  renamed).

Counts after 0.58.0: selftest unchanged from 0.57.0; framecheck 24 checks, netcheck 18.

### 0.57.0: hover that draws only what it touches, and partial frames under a moving wallpaper

- **Hover zones (07 §3.9.20).** A pointer move that is not quiet but changes nothing else damages only where it
  was and where it is: a title bar (its buttons), the icon column, the dock band, an open menu
  (`hover_zone_at`, /sys/screen `hover`). framecheck: the moves along the dock all partial, 72 of 86 as hover
  (failed with the zones off: 16 partial, none as hover); the close button lights down to its lower corner
  (failed with the title bar's zone half as tall).
- **The dock over a maximised window (07 §10 33, fixed).** From 0.54.0 its hover was never drawn there, because
  `pointer_is_quiet_at` found the window's contents under the dock before the dock. framecheck lights the badge
  from one point of the contents under the dock to another (failed with the dock tested after the windows; the
  first version started on the bottom border and passed broken).
- **Moving wallpapers.** Painted at `wall_now`, the last whole frame's tick, so partial frames happen under them
  too. `[windows]` compares clipped and whole frames under all six (failed with the stars painted from the live
  clock); framecheck counts partial frames under the stars (failed with moving wallpapers excluded again).
- framecheck's first session now makes its whole frames by clicking bare desktop, since moves along the dock are
  no longer whole frames.

Counts after 0.57.0: selftest 679 (pc, 64 MiB), 685 (256 MiB), 692 (q35), 706 (`-smp 4`); framecheck 20
checks; gate full 53 steps.

### 0.56.0: commits that copy only what changed, and a music player that sleeps

- **Commits (07 §3.7).** Every program draws its whole surface and commits the whole of it. The kernel now
  compares each committed row with what the desktop shows, copies only the run that differs and marks only the
  box round it (`publish_diff`); a commit that changed nothing draws no frame. framecheck: a blink copies 144
  bytes where a commit was 1428 KiB, a typed character 412 bytes (failed with everything copied: 608000 and
  1459200); `[window server]`: nothing, one pixel, the far corners and a rectangle's commit (failed with
  everything copied, and with the box a row short). The terminal's row commit is now checked by the bytes
  compared (`compared_bytes`: failed at 7125 KiB with the blink committing the whole window).
- **The music player (10).** It went round every 30 ms whether or not it was playing, drawing and committing its
  window each time. Playing nothing, it waits in `ui_wait` now. termcheck: 4 wakes in five idle seconds, 138
  with the old loop.

Counts after 0.56.0: selftest 678 (pc, 64 MiB), 684 (256 MiB), 691 (q35), 705 (`-smp 4`); framecheck 16 checks,
termcheck 12; gate full 53 steps.

### 0.55.0: a desktop that draws only the part of the screen that changed

- **Partial frames (07 §3.9.20, 05 §3.11).** Every frame drew the whole desktop and then sent only what
  differed. Now fb.c has a clip (`fb_clip`) that every writer there and every drawer in gfx.c keeps to, and a
  frame with damage (and no moving wallpaper) puts the pointer's patch back, clips to the damage, draws, and
  sends the damage and the two pointer patches; `fb_flush_rect` leaves out rows the card already has. A clip
  inside one window's contents starts the frame from that window; the icon column, each window with its
  shadow and the dock are skipped whole when the clip misses them. /sys/screen `partial` counts these frames.
- **Commits (07 §3.7).** `winsrv_commit` marks the window's contents rather than asking for the whole screen,
  and `win_commit_rect` (syscall 66) copies and damages only a rectangle of a surface. The terminal's blink
  draws and commits only its bottom row (09b).
- **The drawers (07 §3.8).** Shadow spans, glyphs and corner pixels go through `fb_row` rather than
  `fb_get`/`fb_put` a pixel at a time, which is most of why a whole frame costs a third of what it did.
- **Damage kept to the screen (07 §10 32, fixed).** A menu at the left edge stayed half faded.
- Checked: `[windows]` draws a scene whole and then through 110 rectangles over a pattern, from the kept
  wallpaper and painted afresh, and wants the whole frame's pixels inside and the pattern outside; it failed
  with a span, a corner or the wallpaper restore ignoring the clip, a window's or the dock's box drawn without
  its shadow, the held window found by overlap rather than containment, a glyph left out a row early, and the
  damage clamp gone. framecheck: every frame of a blinking terminal is partial (failed with frames never
  partial), a commit copies under 256 KiB (failed with the blink committing the whole window: 1428 KiB each),
  and the cursor still visibly blinks (failed with the rectangle committed in the wrong place). The `[video]`
  colour order check failed once the rectangle flush skipped rows, since it sends one pixel three ways; its hook
  now clears `sent_valid` while it does.
- Measured with the kernel's counters over eight seconds of a blinking terminal, against v0.54.0 on the same
  host: 51.5 to 53.5 million cycles a frame drawing before, 0.4 now; 1428 KiB copied a commit before, 118 now.
  Ten moves along the dock, which are whole frames: 52 million cycles a frame before, 16 now.
- Not done: the toolkit, the browser and the other programs still commit whole windows; a moving wallpaper
  still means whole frames; hover over a title bar, the dock or a menu is still a whole frame.

Counts after 0.55.0: selftest 672 (pc, 64 MiB), 678 (256 MiB), 685 (q35), 699 (`-smp 4`), in 51 sections;
framecheck 14 checks; 66 live system calls; gate full 53 steps.

### 0.54.0: a pointer that moves without drawing the desktop, and a desktop left alone that draws nothing

- **The pointer (07 §3.9.20).** Every move drew the whole desktop, wallpaper to dock, about 60 million
  cycles, to put a twelve pixel arrow somewhere else. Over a window's contents or bare desktop, with nothing
  open, held or already wanting a frame, a move now puts back the patch the arrow covered, keeps the new one
  and draws the arrow there (`move_pointer_only`, /sys/screen `pointeronly`). Title bars, borders, the icons
  and the dock still take whole frames, because the desktop draws hover there. framecheck: 51 moves redrew
  only the pointer (0 with every move a whole frame); after six steps over bare desktop the arrow is where it
  was sent and nowhere it has been, with no frame drawn across the walk (failed with the patch never put
  back: 414 pixels of arrow left behind); the first icon lights up under the pointer (failed with the icons
  counted as quiet).
- **A desktop left alone (07 §4.2).** It drew the whole screen every second for the dock's clock, which moves
  once a minute, and a window put away still drew one whenever it redrew -- a terminal does twice a second, to
  blink its cursor. The once a second look draws only when `dock_changed()` (clock text, link, address) says
  so or the network panel is open. A minimised window's commit only marks it (`winsrv_commit` called
  `wm_invalidate`, which asks for a whole frame itself, so the loop's own minimised test never mattered), and
  the loop marks it clean. framecheck, with the kernel's `draws` read through the monitor: nothing in eight
  seconds with every window put away (at most 1 allowed, for the clock's minute); failed with 11 with the
  loop drawing minimised windows, 11 with their commits through `wm_invalidate`, and 5 with the clock's frame
  every second. That half-second and one-second frame is also what had hidden the trail and the hover: the
  trail break passed framecheck until the frames were gone.
- **The harness (14 §3.1).** `kernel_symbol(name, source)` reads an address out of build/zelr.elf's symbol table
  and `Monitor.read_u32` reads it through `xp`, so a check can read a kernel counter while the desktop is up.
  framecheck also waits properly for the prompt after leaving the desktop (14 §10 P).
- Measured with `draws` over eight seconds against v0.53.0: a terminal showing, 14 frames before and 10 now;
  every window put away, 17 before and 0 now.
- Not checked: hover on a title bar's buttons under a zone drawn too wide there (framecheck passed with title
  bars counted as quiet). An open terminal draws a whole frame twice a second, and that draws the hover anyway.
- Found and not fixed: damage off the top or left edge sends nothing (07 §10 32).

Counts after 0.54.0: unchanged from 0.53.0 below; framecheck has 11 checks.

### 0.53.0: photographs three times quicker to decode

- **The JPEG transform (13).** Terms whose coefficient is zero -- most of every block -- are left out of
  both passes, exactly: jpegtest holds it to the whole formula byte for byte on 400 random blocks (it failed
  with negative coefficients dropped). The colour conversion looks its samples up rather than dividing six
  times a pixel (jpegtest's colour pictures failed with every plane given the brightness plane's columns).
- Measured in a scratch jpegtest: fifty decodes of the gradient picture, 109 to 126 million cycles before,
  37 to 38 after.
- Tried and not kept, with the numbers: a lookup table for inflate's codes and eight-byte back-reference
  copies, neither faster than the run-to-run spread on 179 KiB of text (13).
- **Pipes (03 §5).** 64 KiB rather than 4, copied in one or two pieces rather than a byte at a time.
  `[open files]`: 256 KiB between two tasks, whole, with the writer stopping for room at most 16 times
  (failed at 4 KiB); and bytes written and read across the end of the ring in one task, in order (failed
  with either half of the wrap's copy misplaced -- the two-task version could not see that, because on one
  processor it keeps every copy lined up with the end of the ring).
- **maptest (03 §4).** "The pages that arrived come back" waits up to two seconds for the collector rather
  than reading free memory once: it failed the first run after every boot (v0.49.1 too), which failed the
  0.53.0 gate on its first try. Still fails with unmap's pages never freed.

Counts after 0.53.0:
- selftest 666 (pc, 64 MiB), 672 (256 MiB), 679 (q35), 693 (`-smp 4`), in 51 sections;
- gate full 53 steps.

### 0.52.0: the rest of the programs that polled

- **The browser (11 §5).** It drew only when something changed but looked sixty times a second, for the
  page's timers and requests. `browser_wait` sleeps until an event, the page's next timer or a waiting
  request (`jsdom_next_due`), a tick at the least. termcheck: 4 or 5 slices in five idle seconds, 206 with
  the old loop.
- **The card games, monitor and paint (10, 09a).** Blackjack draws a frame a tick while a card is still
  sliding in and sleeps once they are down; poker the same, and wakes when an opponent has finished
  thinking; monitor wakes for its samples (four a second, where it drew sixteen frames); paint waits in
  `win_wait` for the pointer or a key, where it looked a hundred times a second.
- Checked: the browser, which failed termcheck with its old sleep put back. Not checked: that the games draw
  while cards move. gamecheck passed with blackjack's frames taken out, because every wait ends within a
  second, so the cards still arrive -- in jumps rather than sliding. The same is true of poker's thinking
  time and the monitor's samples.

### 0.51.0: programs that sleep until something happens

Each change has a check that was run against a build broken for it alone and seen to fail there; the
gain was measured by the scheduler's slices for each program over five idle seconds.

- **win_wait, system call 65 (03 §4, 07).** Sleeps on the window record until `wm_push_event` wakes it
  or the timeout passes, a second at most a call (a program being ended wakes on its own task, not its
  window). `[windows]`: a task asleep on a window is woken by a pushed key, not its three-second clock
  (failed with the wake taken out of `wm_push_event`).
- **The terminal (09b §3.1.14).** Blinks by the clock (half a second) and sleeps until a key or the next
  blink: 27 slices in five idle seconds and a reading, 207 with the old poll (termcheck, which allows 120).
- **The toolkit (09a).** `ui_wait` replaces `sleep_ms(16)` in calc, notes, files and settings: once more
  after a pass with input, then asleep until an event or a declared `ui_due` (a caret blink, a message's
  timeout, settings' four reloads a second). The calculator behind the terminal: 4 to 6 slices in five
  seconds, 206 with the old loop, each of which had been a whole frame committed. A missing extra pass
  after input failed appcheck (the calculator showed the key before last).
- Not converted: the card games, the browser, monitor and paint, which animate or poll the network.
- Found on the way and flagged, not fixed: maptest's "the pages that arrived come back" fails on the first
  run in every boot (v0.49.1 too), which failed ring3check twice.

Counts after 0.51.0:
- selftest 663 (pc, 64 MiB), 669 (256 MiB), 676 (q35), 690 (`-smp 4`), in 51 sections; 65 live system calls;
- gate full 53 steps.

### 0.50.0: a layout seven times cheaper, and nesting that no longer doubles it

Each change has a check that was run against a build broken for it alone and seen to fail there; the
gains were measured in a scratch build of layouttest on a page of 2500 elements under 300 rules, 40 of
them descendant rules sharing a last part.

- **Nested flex (11 §2).** A row measured each child by laying it out and then laid it out again, and
  a row inside did the same inside both. A row being measured now only measures and reports its reach.
  Twelve rows deep: 4096 boxes laid out, now 79 (`ldoc.laid`).
- **Matching once (11 §3).** Each element's matched rules are kept for the length of one layout; it
  was matched about 2.4 times a layout, and matching was nine tenths of a relayout (`ldoc.matched`).
- **Ancestor bits (11 §3).** A rule wanting an ancestor the element does not have is skipped without
  being walked (`cbloom`, 128 bits; `cindex.need`).
- Checks in layouttest: a row inside a row placed where its content ends, twelve rows deep within 400
  boxes, each element matched once, and descendant and child rules through ids, classes and names
  still matching while one wanting an absent ancestor does not.
- Measured, one relayout of the page: 708 million cycles, 642 of them matching; with matching kept,
  371 and 301; with the ancestor bits, about 105 and 35.

### 0.49.1: a power-cut check that tossed a coin

- **crashcheck (14 §10 U, 04 §8).** "Both A and B turn up across six power cuts" was six coin tosses
  once a write took milliseconds, all alike about one run in 32, and it failed the 0.49.0 gate once.
  crashwrite fills each copy with a generation number, carried on across boots, and says which it
  finished; every survivor must be no older than the last one it said. Three runs on the working
  build left the last reported copy or the one after it every time; with rewrites that skip the
  commit, every survivor was generation 0 against up to 106 reported. The usage line and gate.sh's
  "Six boots" (it is fifteen) are corrected.

### 0.49.0: the console that cost a third of the self test

Each change has a check that was run against a build broken for it alone and seen to fail there; the
cost was measured by counting the cycles `kputc` spends drawing, over a whole selftest run, in a
scratch build.

- **Scrolling (05 §3.14).** Once the text reached the bottom, every line moved the 3 MB back buffer
  up and sent the whole screen. It jumps by a quarter of the rows now.
- **Characters.** Glyphs are drawn a back-buffer row at a time rather than through 128 `fb_put`
  calls, and the cells a character touches go to the card as one rect rather than three.
- **The pointer.** It came off and went back for every character, wherever it was; now only when it
  overlaps the cursor's rows or the character scrolls (`mouse_over`).
- Checks in `[video]` (`test_console`, 4): whole-screen sends over two screens of newlines
  (`fb_frames`), one rect a character (`fb_rect_flushes`), the pointer left alone by text far from it
  (`mouse_hides`), and a drawn 'A' against the font. The first version of the pointer check read its
  count after a PASS line had been printed, which can scroll, so it depended on where the cursor
  happened to be; it reads both counts before reporting now.
- Measured: the console took 12.4 billion of a selftest run's 36.8 billion cycles, a third; now 1.5
  billion of 26.3 billion.

Counts after 0.49.0:
- selftest 662 (pc, 64 MiB), 668 (256 MiB), 675 (q35), 689 (`-smp 4`), in 51 sections;
- gate full 53 steps.

### 0.48.0: the disk asked for a sector at a time

Each change has a check that was run against a build broken for it alone and seen to fail there; the
gains were measured by counting the commands each operation hands the drivers (`blk_io`, shown in
`/sys/devices`) and the cycles it takes, on the ATA machine (pc) and the AHCI one (q35).

- **ATA flushed after every write (04 §3).** `ata_write` ended in FLUSH CACHE, a host fsync each on
  QEMU; it waits for the drive and checks ERR/DF now, as AHCI always did. Callers flush where the
  order matters. `[fat]` compares the drive's flushes (`ata_flushes`) with the ones asked for.
- **Runs of clusters (04 §4.3).** File data is read and written a run of neighbouring clusters to a
  request through a 4 KiB-aligned 32 KiB staging buffer, never the caller's (a program's memory, which
  the xHCI refuses). `fat_format` zeroes its areas the same way.
- **The table (04 §2).** Allocating a file's chain and freeing one hold their changes in the cached
  sector and write it once a sector (`fat_defer`); the order of writes is unchanged.
- **Directory sectors (04 §2).** Eight kept (`dcache`); fat.c's own writes are followed exactly, and
  anyone else's write (`blk_writes` moving) drops them all. Not checked: that `vol_write` drops a kept
  sector it overwrites, which cannot be seen through the calls (a directory must be empty to go).
- Measured, before and after: writing 256 KiB, 1023 writes and 3761 Mcycles on ATA (116 on AHCI), now
  11 writes and 120 Mcycles (16 on AHCI, 67 writes); reading it, 513 reads, now 8 (64 on AHCI);
  listing forty files, 2045 reads and 559 Mcycles, now none and 2; writing forty small files, 2740
  reads and 1775 Mcycles, now 2 reads and 334; deleting them, 1151 reads, now none.

Counts after 0.48.0:
- selftest 658 (pc, 64 MiB), 664 (256 MiB), 671 (q35), 685 (`-smp 4`), in 51 sections;
- gate full 53 steps.

### 0.47.0: a download fifty times faster

Each change has a check that was run against a build broken for it alone and seen to fail there; the
gains were measured on the guest's own clock and on QEMU's capture of the card's traffic.

- **A measurement (06a §3.10).** `fetch` prints how long the exchange took, timed with the guest's
  ticks from `tcp_open` to the close: timing it from the host had measured the harness typing.
- **The e1000 ring (06a §10 M2).** 128 descriptors rather than 32. slirp sends a whole 64 KiB window,
  45 frames, in 0.37 ms, and 31 usable descriptors lost the last 13 of every burst; each loss then cost
  slirp's retransmission timer, 1.5 s. 250,000 bytes took 800-1000 ms and take 10-20 ms (a 10 ms tick),
  with nothing resent and nothing out of order. webcheck counts the server's resends in the capture: 73
  with the old ring.
- **Bytes past a hole are kept (06a §3.9).** `take`/`hold`/`absorb`: in place in the receive buffer, up
  to 8 stretches, and a FIN past a hole waits for it. Measured honestly: with the old ring it made no
  difference (780-990 ms), because the losses came 13 at a time and a sender without selective
  acknowledgements repairs one a round; it is kept because a single loss then costs one resend rather
  than a timer. New self test section `[tcp]`, 8 checks, fed by hand across the sequence wrap.
- **Waiting for the network (06a §10 D1).** `net_wait` sleeps on a channel `net_receive` wakes rather
  than for a whole tick. On the wire: with the tick wait two of five connections took 18.3 and 27.9 ms,
  sitting out ticks; with the wake none of ten took more than 13.4 ms (4.5 ms of it the server).
- **The send side (06a §10 M1, 06b).** A SYN offers MSS 1460 (webcheck reads it from the capture), and
  a TLS record goes to TCP in one write, header and body together (two `[tls 1.3]` checks).

Counts after 0.47.0:
- selftest 652 (pc, 64 MiB), 658 (256 MiB), 666 (q35), 679 (`-smp 4`), in 51 sections;
- gate full 53 steps.

### 0.46.0: drawing a frame ten times cheaper

Each change has a check that was run against a build broken for it alone and seen to fail there; the
gains were measured by switching each off in turn.

- **A measurement (07).** `composite()` counts the cycles it spends drawing (`rdtsc`, not the send):
  /sys/screen `draws` and `drawmc`, printed by framecheck.
- **The wallpaper (07 §3.9.12).** A still wallpaper is drawn once into a copy, keyed by everything it is
  drawn from, and frames start from the copy; the copy is taken only with four times its size free.
  536 million cycles a frame without it, 51.5 million with it, for framecheck's pointer moves, and the
  desktop drew 111 frames in the moves rather than 56.
- **Window frames (07 §10 16).** The hairline and refill touch only the ring (`fb_round_ring_aa`); solid
  runs of rounded rectangles are written through the row (`fb_row`). 70.8 million cycles a frame with the
  old three passes, 51.5 with the ring.
- Checks in `[graphics]`: the ring equals the old passes pixel for pixel; solid runs equal a per-pixel
  reference; the copy equals a fresh draw; the copy follows a desktop colour change (first written
  against bloom, which does not use that colour, where it could not fail: now against the gradient).

Counts after 0.46.0:
- selftest 642 (pc, 64 MiB; the wallpaper checks SKIP there), 648 (256 MiB), 656 (q35), 669 (`-smp 4`);
- gate full 53 steps.

### 0.45.0: the file manager's harness, keys that arrive together, and checks that waited for time

Each change has a check that was run against a build broken for it alone and seen to fail there.

- **Files (10).** ctrl+c, ctrl+x, ctrl+v and F2, sharing `take`/`begin_rename` with the menus, and every
  key of a frame rather than the first. `tools/filescheck.py` (gate `filestest`, 11 checks) copies,
  renames and moves from the keyboard and reads the result off the disk image while the guest runs.
  Failed with ctrl+c and F2 made to do nothing, and with Files taking only the first key of a frame.
- **Keys to programs (07).** The desktop's loop takes every waiting key each pass rather than one, so
  typing reaches a program at once rather than at the frame rate: eighteen keys typed quickly were acted
  on 1.4 s after the first, against 8.9 s with one key a pass. That made the first-key-only programs
  (Notes, the calculator, Settings, blackjack, poker) drop keys, so they walk `in.keys` too.
- **Harnesses (14 §10 N, V).** iso_test.sh checks the line `cat` printed rather than the echoed command
  (failed on all four paths with the file not read back). livecheck and gamecheck wait for what they
  check. volcheck compares the quietest burst with the loudest. `Guest` reads the serial line in blocks.
  The harness still types at 50 ms a character, deliberately: bursts would cut its tolerance for a
  guest stall from 800 ms to tens of milliseconds.

Counts after 0.45.0:
- selftest unchanged (640, 644, 652, 665);
- gate full 53 steps.

### 0.44.0: the window being used, alt+tab, and Escape

Each change has a check that was run against a build broken for it alone and seen to fail there.

- **The window being used (07 §10 6).** `focus_index()`/`focused_window()`, the frontmost non-minimised
  window, is what is drawn focused, highlighted on the dock, typed into and acted on by alt+m/f/q/arrows
  and by a click on its own chip. alt+m on a minimised top window used to minimise it again.
- **alt+tab (7).** A walk over a snapshot of the stack while alt is held, ended by letting go
  (`kbd_alt()` every loop pass), a button, another key, or the window closing; the one reached ends in
  front with the previous front behind it. It raised the window behind the front one, so three windows
  could never all be reached.
- **Escape (14).** `wm_key`: Escape closes find, the launcher, the context menu, the volume or network
  panel; then goes to the focused window if it asked (`SYS_WIN_ESCAPE` 64, `win_want_escape`: the
  browser, Files, the calculator, blackjack); else leaves. alt+Escape always leaves. gamecheck and
  netcheck leave with alt+Escape.
- **The dock (5).** Drawing and hit test share `chip_width`. The disagreement was latent: the regular and
  bold 15 px faces have identical advances.
- **Leaving (13, partly).** The find bar and network panel are closed on the way out.

Counts after 0.44.0:
- selftest 640 (pc, 64 MiB), 644 (256 MiB), 652 (q35), 665 (`-smp 4`); 64 live system calls;
- gate full 52 steps.

### 0.43.0: the JavaScript engine's numbers, `new`, key order, errors and memory

Each change has a check that was run against a build broken for it alone and seen to fail there
(jstest, 202 cases, 58 new).

- **Numbers (12 §10 B5).** `userland/jsnum.h`: correctly rounded reading (one exact operation for
  fifteen digits and a small power of ten, exact big integers otherwise) and shortest round-trip
  printing. The lexer, `Number()`, `parseFloat` (now the longest decimal at the front) and JSON read
  through it. 0.1 + 0.2 prints 0.30000000000000004; 0.3 reads as the double nearest it.
- **`new X().y` (B3).** `js_parse_new`: the constructor is a member expression and its arguments are
  the first parentheses. The parser's `J->nodes[n].x = parse(...)` stores now go through a local
  (B19), because parsing can move the node array.
- **Key order and hidden properties (B13, B14).** An insertion-order list on every object;
  `js_own_keys` gives indices ascending then insertion order, enumerable only, to for-in,
  `Object.keys`/`values` and JSON. The engine's `__ctor__` and the like are non-enumerable
  (`js_set_hidden`). JSON leaves out properties whose value is undefined or a function.
- **Errors (B21).** Error, TypeError, RangeError, ReferenceError, SyntaxError and EvalError; the
  engine throws them rather than strings; `instanceof Error` holds for all six. Found on the way:
  `return f()` replaced a throw from f with undefined (B26).
- **Memory.** A finished call's scope that no function value captured goes back to size-class free
  lists; `arguments` is built only for a function whose text names it; string and array methods
  called where they are fetched share one native each; the names looked up on every call are
  interned. Before, each call kept about 600 bytes and a page ran out at about 40,000 calls; jstest
  now makes 200,000 calls, string method calls and pushes.

Counts after 0.43.0:
- selftest unchanged (626, 630, 638, 651); jstest 202;
- gate full 52 steps.

### 0.42.0: other systems' disks, names past ASCII, TLS manners, SVG paint, the colour order

Each change has a check that was run against a build broken for it alone and seen to fail there.

- **Foreign FAT volumes (04 S20).** `fat_made_here()` (the OEM field says "ZELR"). On a volume without it
  `finish_mount` skips the reclaim and `layout_init` makes, seeds and empties nothing; the shell starts in
  `/` when there is no `/home`. Check: `gpt_test.sh`, a GPT partition and a whole disk made as "MSDOS5.0"
  with a lost chain and `TMP/KEEP.TXT` (`mkfat.py --foreign`, `mkgpt.py foreign`): every byte outside the
  written file unchanged.
- **FAT S15, S16, S17.** `fat_free_bytes` is u64; `fat_list` copies the terminator; long names are UTF-8
  above and UTF-16 on the disk both ways, any byte past ASCII makes a name long, invalid UTF-8 is refused.
  Checks: `[fat]` mounts a 16 GiB FAT32 volume that exists only as answers to reads, lists a 63-byte name
  into a filled buffer, writes and reads "café au lait.txt" and "大.txt" (first byte 0xE5).
- **TLS (06b §10.4, 10.7, 10.8).** Fatal alerts on failure, in the clear before there is a key and sealed
  after, never in answer to the server's own; KeyUpdate read, and answered when asked, under the old key;
  a served copy of a store root is set aside before its dates are looked at. Checks: `[tls 1.3]` byte for
  byte against traced keys and independently worked next secrets; `[x509]` an expired root copy passes and
  an expired intermediate still fails.
- **libc (09a §10.2 #3, #4).** `fseek(SEEK_CUR)` from the logical position; `ftell` counts buffered writes.
  Checks: libccheck, two new.
- **SVG (13 §10 3-6, 11).** Paint is inherited (fill, stroke, their widths, opacities and rule, up to the
  root), opacity multiplies down; `rect` and `polygon` strokes close; `<defs>`, `<clipPath>`, `<mask>`,
  `<symbol>`, `<pattern>`, `<marker>` are not drawn; `transparent` is no paint. Checks: svgtest, ten new.
  The closing bug was found by one of the inheritance checks failing on the fixed build.
- **DOM (11 §10 #34).** The end tag of an element the tag table does not know closes it
  (`dp_pop_to_name`). It closed nothing, so after a custom element or an SVG `<g>` every sibling nested
  inside; with inheritance in, a group's fill would have leaked into everything after it. Found by the
  `<defs>` check failing on the fixed build. Checks: layouttest and svgtest.
- **UEFI colour order (01 §10 #2, 05 B7).** `handoff_t.fb_format` at 3296 (the structure is now 3304 bytes),
  set by the loader; fb.c swaps red and blue on the copy to the card only. Check: `[video]` through
  `fb_test_orders`. Not run on a real RGB panel: QEMU's OVMF offers BGR modes only.

Counts after 0.42.0:
- selftest 626 (pc, 64 MiB), 630 (256 MiB), 638 (q35), 651 (`-smp 4`);
- gate full 52 steps.

### 0.41.0: memory and waiting under preemption, the clocks, FP state, and the checks themselves

Each change has a check that was run against a build broken for it alone and seen to fail there.

- **Heap, PMM, task ring (02 §10 B8, B9, B10, B28).**
  - `spin_lock_irqsave` (smp.h) around every heap and PMM change: IF off for this processor's own ticks
    (the collector frees memory inside the timer interrupt), a spinlock for processors without the kernel
    lock (boot, `smp_run` work). `heap_check` walks and adds up the heap.
  - `ring_insert` (sched.c) joins a task with IF off, `next` written before the link.
  - `kmalloc` refuses sizes above 0xFFFFFF00 (B8). `can_collect` requires `on_cpu < 0` (B28).
  - Checks: probes (`heap_test_probe`, `pmm_test_probe`, `sched_test_probe`) yield where a tick could land
    while a rival task asks for the same thing; a two-processor heap hammer with one side off the kernel
    lock; "the heap still adds up after all of that" at the end of the self test.
- **Waiting for input (03, console_pause).** `input_wait`/`input_wake` (keyboard.c) block the waiting task
  until a key, a serial byte or the mouse; console reads, the kernel shell and the desktop loop use it.
  Check: `[userspace]` starts a flat ring 3 program that reads the console and wants it blocked and nearly
  never scheduled until a key is injected.
- **Clocks (02 §10 B5, B6, B27).** PIT mode 2 for the calibration, checked against one CMOS second; a gate
  for vector 0xFF (`isr255`, counted, no EOI); the PIT tick counted before the lock and taking the try-lock
  path.
- **FP state (09a §10.2, 18 and 21).** The signal frame carries the FXSAVE image (MXCSR masked by
  `mxcsr_mask` before FXRSTOR) and the handler starts clean; exec calls `task_fpu_reset`; fork copies the
  parent's live state into the child's aligned area. Checks: sigtest, five new.
- **Serial.** FCR 0xC7: a fourteen-byte trigger, because QEMU accepts only as many bytes as the trigger and
  its stdio backend drops the rest. Check: `tools/serialcheck.py`, a gate step.
- **The checks (14 §10 B, N, S, AB).** The gate's build step reads build.sh's exit status and build.sh runs
  `set -eo pipefail` and deletes the old image first; check_sse is a gate step again with its false alarms
  fixed; defaultcheck compares what is still duplicated and found Settings deleting `text_dim`, so Settings
  keeps lines it does not manage (`keep_unmanaged`, and setcheck checks it); shell_test matches whole lines,
  which exposed a path in it that had never worked; the gate runs the self test on four processors.

Counts after 0.41.0:
- selftest 609 (pc, 64 MiB), 613 (256 MiB), 621 (q35), 634 (`-smp 4`);
- gate full 52 steps.

### 0.40.0: multiprocessor memory, window lifetimes, JavaScript, and data loss

Each change has a check that was run against a deliberately broken build (or, for the ring 3 fixes, the
committed old sources) and seen to fail there.

- **Multiprocessor memory (finding 14 and 02 §10 B1-B4).**
  - `paging_current_directory()` reads CR3; the global `current_pml4` is gone. `paging_init` builds the
    kernel tables by name, since CR3 is still the loader's then.
  - Resolved copy-on-write and demand faults leave through `leave:` in `isr_dispatch`, which releases the
    lock and delivers signals. A lock still held on entry from ring 3 is counted as `kept` in `/sys/cpu`.
  - `paging_protect_writes()` sets CR0.WP on the boot processor (end of `paging_init`) and every AP
    (`ap_main`). A ring 0 fault at a user address that copy on write cannot resolve ends the calling program
    (`on_behalf_of_program`).
  - The ragged ends of usable regions above 64 MiB are `pmm_reserve`d; there were 480 such frames at 256 MiB.
  - Checks:
    - `/bin/cputest` in ring3check and in smpcheck (four processors);
    - `[physical memory]` "every frame that can be handed out is mapped".
- **Frames that waited for a busy processor (02 §10 B12).** Found when smpcheck's new cputest step lost its
  typing under a loaded gate. `smp_helper` now also requires the AP to be in its idle task
  (`sched_cpu_idle`); the AP claims a job with a compare-and-swap; `fb_flush` takes back a half nobody
  claimed (`smp_take_back`) and does it itself instead of spinning up to 20,000,000 `pause`s with IF=0 and
  the lock held. The slot also has `running`, from the claim until the job returns, because claiming cleared
  the only thing the scheduler looked at: a flusher preempted mid-wait freed the lock and the helper was given
  a program halfway through its half. `smp_work_pending`, `smp_run`, `smp_busy` and `smp_wait` include it,
  so `smp_wait` means finished. `/sys/screen` gains `stalled`.
  - Checks: smpcheck, with a spinner on every processor, "no flush waited out a processor that was running
    a program"; and in `[processors]`, with a held job and more ring 3 spinners than processors, "a processor running
    a program is not offered as a helper", "work taken back before it started never runs" and "a processor in
    the middle of handed work is not given a program". Each of the three `[processors]` checks failed on a build
    broken for it alone. smpcheck's counter caught the idle-test break only before `running` existed: with
    it, a preempted flusher lets the helper reach idle and finish, so that break showed as a cputest failure
    instead.
  - The gate now runs the self test with `-smp 4` too ("the same checks on four processors"); before, the
    multiprocessor half of `[processors]` ran only by hand.
  - smpcheck also waits for cputest's whole verdict (it had read `CPUTEST_PA`), and its `ps` check looks at
    what `ps` printed (it matched the earlier `bg /bin/spin` lines and could not fail).
  - New finding by reading: 02 §10 B28, the collector does not check `on_cpu`.
- **Window lifetimes (finding 6).**
  - `wm_close` clears `resizing`.
  - `held_button` records which button started a hold, and releasing that button ends it.
  - Surfaces are mapped `PTE_BORROWED`: `copy_table` leaves them out of a fork and `free_table` never frees
    their frames. `sys_exec` calls `winsrv_release` before freeing the old space.
  - `apply_snap` sets `maximized` only when the resize went through.
  - Surfaces go up to 2560x1600 with a 16 MiB step (`WINSRV_MAX_W/H`).
  - Checks: `[window server]` `test_window_lifetimes`, 12 checks, through hooks `wm_test_holds`,
    `wm_test_pointer`, `wm_test_begin_resize` and `wm_test_maximize`. The existing window-server checks now
    run in a scratch address space, answering open question 2 in 07.
- **JavaScript (finding 13, 12 §10 B1, B2, B5).**
  - call/apply/bind take the target from `this`. Natives can read `J->callee`, which is how a bound function
    finds its own record; bound arguments are kept.
  - The for clause parses with `no_in`, and the target form sets `d = 0`.
  - `js_num_text` prints 15 significant digits, rounded (17 for whole numbers of 2^53 or more below 10^21).
  - jstest now counts every case: 162.
- **Data loss in programs (09b, 10 §10).**
  - The terminal's cp copies in chunks and checks the size; mv renames when it can.
  - Files copies and renames the same way.
  - Notes' buffer grows, and a file it could not read whole cannot be saved.
  - Check: `tools/bigfilecheck.py`.
- **libc (finding 9).**
  - atan uses y = x^2/(1+x^2).
  - fputc flushes a full buffer before storing.
  - stdin, stdout and stderr are descriptors 0, 1 and 2.
  - `clock()` scales ticks by the rate in `/sys/uptime`.
  - Check: libccheck, 22 checks.
- **PNG (finding 10).** The chunk bound is checked in two steps. Check: pngtest.

Counts after 0.40.0:
- selftest 595 (pc, 64 MiB), 599 (256 MiB), 607 (q35), 614 (`-smp 4`) -- as 0.40.0 shipped;
- 48 ring 3 programs, which is `SYSFS_MAX_PROGRAMS`;
- gate full 50 steps.

### 0.39.0: six of the findings below, fixed

Each change has a check that was run against a deliberately broken build and seen to fail there.

- **The network freeze (finding 12).**
  - `net_wait()` in `net.c` sleeps a tick (`task_sleep(1)`) where every wait in `net.c` and `tcp.c` used
    to spin, so ticks advance inside a system call and other tasks run.
  - Because a network call can now be interleaved with another:
    - sockets are reserved before the wait (`sock_take` in `syscall.c`);
    - DNS is taken one asker at a time (`dns_take`);
    - `take_slot` in `tcp.c` claims atomically;
    - the chain and leaf in `tls.c` are per session (they were statics).
  - A task ended from outside -- `sys_kill`, or a signal's default action, which runs inside the scheduler
    -- goes through `syscall_abandon`. It drops sockets, TLS sessions, half-open connections (`tcp_abandon`
    by owner pid) and its turn at delivery or DNS (`net_abandon`), all without waiting.
  - Retransmission inside calls works now, since `pump_one` sees time pass.
  - Check: `tools/hangcheck.py` (gate `hangtest`), with `/bin/hangtest`. `repro/nethang.py` now reports
    NOT FROZEN.
- **TLS CertificateVerify (finding 1).**
  - `tls_flight_step` enforces extensions, certificate, signature, finished, each once and in that order.
  - A finished message with no signature before it fails as "the server never proved it holds the
    certificate's key".
  - Check: `[tls 1.3]` `test_tls_order`, 8 checks.
- **Auto-format (finding 2).**
  - `diskfs_format` formats only a blank disk (the first 128 KiB and the last sector all zero) or the volume
    already mounted across the whole disk.
  - A refused table is left alone and the refusal is logged.
  - Check: `tools/gpt_test.sh` now compares the whole image before and after, and has a
    Linux-style unpartitioned disk case.
- **FAT (finding 3).**
  - `dir_find` compares 8.3 names only for names that fit 8.3.
  - A rename to a file's own alias keeps the file.
  - `vfs_rename` refuses two volumes.
  - The volume selection lasts one VFS call (`unroute`).
  - `fat_format` always means the boot disk.
  - Delete and rmdir write the entry and flush before freeing, and rmdir now drops its long name entries
    too (S14).
  - `fat_reclaim` frees nothing unless the walk read every directory and every table sector, never frees a
    bad-cluster mark, and queues each directory once.
  - The mount bound uses the volume's own disk size (S10).
  - Check: `[fat]` `test_fat_names`, 18 checks, using two self-test fault hooks: `fat_test_writes_left`
    and `fat_test_subdirs_unreadable`.
- **Flushes (finding 4).**
  - `ahci_flush` issues FLUSH CACHE EXT (a command with no PRDT).
  - `usbdisk_flush` issues SYNCHRONIZE CACHE(10), and accepts only ILLEGAL REQUEST as "no cache".
  - `vol_flush` in fat.c flushes the volume's own disk.
  - `diskfs_flush` flushes the stick as well.
  - Check: `tools/mountcheck.py` watches QEMU's own trace of `ide_bus_exec_cmd` (cmd 0xea) and
    `scsi_req_parsed` (command 53).
- **Kill and signal (finding 5).**
  - `may_end` in `syscall.c`: only tasks with `user` set, and never idle.
  - Monitor offers Stop only for programs.
  - Check: in `hangcheck.py`.

Counts after 0.39.0:
- selftest 582 (pc, 64 MiB), 586 (256 MiB), 594 (q35), 597 (`-smp 4`), still 50 sections;
- 47 ring 3 programs;
- gate full 48 steps.

Findings 1-5 and 12 below are kept as they were written, for the reasoning.

## Most important findings, across all areas

These are the verified-by-reading defects that matter most, most severe first. The details and the
reasoning are in each file's §10.

1. **FIXED in 0.39.0. TLS server authentication can be bypassed.** CertificateVerify is never required
   (`kernel/tls.c:681-758`), so a man in the middle holding a real site's public chain can intercept any
   https connection. [06b §10.1]
2. **FIXED in 0.39.0. The first-boot auto-format can wipe a disk that is not blank.** Any disk whose partition table the
   kernel rejects (a bad GPT CRC, an unreadable header, a protective MBR only) is formatted as disk 0. On a
   laptop booted from a stick, disk 0 is the internal drive. [04 §10 S1]
3. **FIXED in 0.39.0. FAT name handling can hit the wrong file.**
   - The 8.3 alias compare makes `chapter10.txt` resolve to `chapter1.txt` (fat.c:1202-1211).
   - Renaming a file to its own alias deletes it.
   - A cross-volume rename is not rejected; this is reachable from ring 3.
   - `format` after any `/usb` access writes onto the stick.
   - Deletes are not crash-ordered.
   - Reclaim can free live data under memory pressure.
   [04 §10 S2-S7]
4. **FIXED in 0.39.0. The crash-safety promise is weaker on real hardware than stated.** The AHCI and USB-stick "flush"
   are no-ops, and every flush goes to disk 0. [04 §10 S8, S9]
5. **FIXED in 0.39.0. Any program can kill or signal any task, including kernel services** (`sys_kill`, `sys_sigsend`
   have no ownership check). [03 §10.5]
6. **FIXED in 0.40.0. Window manager lifetime bugs.**
   - Closing a window while it is being resized leaves a dangling `resizing` pointer.
   - Right-button capture sticks until a left release.
   - fork and exec treat window surface pages as user pages.
   [07 §10]
7. **FIXED in 0.41.0. FP state is not isolated.** Signal handlers can clobber FP/SSE state, and exec keeps the FPU state
   (ring 3 is built with SSE on). [09a §10.2 #18, #21]
8. **FIXED in 0.41.0 (defaultcheck and the build step; the others as each was touched). Tests that cannot fail.**
   - `tools/defaultcheck.py` matches nothing in settings.c and passes on three vacuous checks (confirmed
     by reading its output in the gate).
   - The gate's build step only fails on the literal word "error" [01 §10 #5].
   - Several checks pass for other reasons than their comments say [04 D11, 06b §10.10].
9. **FIXED in 0.40.0. libc correctness.** `atan` is wrong, `fputc` can overflow into the next FILE, `stdin` is always EOF,
   and `clock()` is ten times too small. [09a §10.2]
10. **Untrusted input in the browser's decoders.** The PNG chunk bounds check wraps (png.h:146; FIXED in 0.40.0), and
    there are several SVG and JPEG conformance gaps (the SVG paint ones FIXED in 0.42.0). [13 §10]
11. **UEFI.** The GOP pixel format (RGB vs BGR) is not carried in the handoff (FIXED in 0.42.0), and the handoff
    address is fixed at 0x70000 with no fallback. [01 §10 #2, #8]
12. **FIXED in 0.39.0. A network syscall whose peer never answers freezes the machine. REPRODUCED 2026-09-26**
    (`repro/nethang.py`, prints FROZEN and exits 1 while the bug stands).
    - **Cause.**
      - int 0x80 is an interrupt gate (`idt.c:63`, flags 0xEE), so interrupts are off for the whole
        syscall, and `ticks` only advances in the IRQ0 handler (`timer.c:18`).
      - Every network wait keyed on `timer_ticks()` therefore never times out inside a syscall: ARP
        (`net.c:162-172`), TCP open, send and receive (`tcp.c:426-494`), DNS (`net.c:778`), DHCP and
        ping. Frames still arrive because `net_poll()` polls the card, which is why the ordinary path
        works.
    - **Reproduction.**
      - Setup: QEMU `-netdev user,restrict=on` (DHCP gives no gateway, and nothing outside answers);
        `exec /bin/wiretest http://93.184.216.34:80`.
      - The machine stops for good: `ticks` frozen at 945, and every monitor sample shows CPL 0 with
        IF clear. The shell never answers again.
      - Control: the kernel shell's own `fetch` to the same address (a kernel task, interrupts on)
        gives up after 2.6 s with "could not connect".
    - **Real-world trigger.** On a real network, a firewalled port, a silent DNS server or a dead
      neighbour is enough. Any ring 3 program, the browser included, can then freeze zelr.
    - **Reproduced again on an ordinary network** (with the gateway set):
      - Setup: a host server accepted the connection and never sent a byte, like a hung web server.
      - `wiretest` then waited in `tcp_recv`.
      - Result: ticks frozen (877 -> 877), CPL 0 with IF clear, and the shell dead.
    - Atlas 06a also notes that a kernel task preempted while it owns frame delivery can leave a
      syscall spinning. [06a §10]
13. **FIXED in 0.40.0. JavaScript `call`, `apply` and `bind` do not work. REPRODUCED 2026-09-26** (`repro/callbind.py`, prints BROKEN and exits 1 while the bug stands).
    - `jsprobe` against a page with one script per method: the control script ran, and all three stopped
      with "this is not a function".
    - Cause: the wrappers look up `__fn__` on the receiver `t` (the target function) instead of on the
      wrapper itself (`jsrun.h:450-460, 573-622`).
    - This is the feature commit 3f4cdb5b says it added for Google's front page, and nothing tests it.
      [12 §10]
14. **FIXED in 0.40.0 (with 02 B1, B3, B4). SMP: the big lock stays held into ring 3 after a copy-on-write or demand fault** (verified by
    reading).
    - `isr_dispatch` returns early at `idt.c:202-203` and `220-221`, skipping `kernel_lock_release()`
      at `:287`, and it skips signal delivery on those returns too.
    - This is not a deadlock: the next kernel entry on that CPU sees `kernel_lock_held_here()` and
      releases on exit. But every other CPU is kept out of the kernel until then.
    - Atlas 02 lists further SMP defects, not yet verified:
      - a single global `current_pml4` while CR3 is per CPU;
      - CR0.WP never set, so kernel writes bypass copy-on-write;
      - partially mapped 2 MiB chunks above 64 MiB handed out as frames.
      [02 §10]
15. **Network odds and ends.**
    - DHCP sends one DISCOVER and one REQUEST with no retry.
    - TCP never sends RST or MSS.
    - The ARP cache never expires.
    - `fetch ... SAVEAS` writes to the RAM filesystem.
    - A USB RNDIS adapter can hang boot and is never detached.
    [06a §10]

Documentation drift is catalogued in 15 §9.4 (by document) and in each file's §10.

## Glossary
- **handoff_t**: the one structure every loader hands the kernel (memory map, framebuffer, ACPI pointer,
  command line). Magic "ZELR64HF". `include/handoff.h`.
- **The big lock**: the single kernel lock held whenever a CPU is not executing ring 3. Whether it is
  released on the way out is decided by the interrupt frame being returned through (`kernel/idt.c`).
- **Knob**: one entry of `KNOBS[]` in `kernel/theme.c`, a numeric or switch setting stored in `/zelr.cfg`.
- **Face**: one size and weight of the generated typeface (a coverage table plus metrics).
- **Surface**: a window's pixel buffer. The program draws into `pixels`; `win_commit` copies them to
  `shown`, which the compositor reads.
- **Black box**: the boot log kept in sectors reserved at the front of a volume zelr formatted itself,
  readable as `/sys/boot` and `/sys/lastboot`.
- **Gate**: `pipeline/gate.sh`, the mechanical verification tiers `fast`, `screen` and `full`.
