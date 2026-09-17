# zelr

A 64-bit operating system written from scratch for x86. It boots itself off a
disc or a USB stick, through BIOS or UEFI, drives a framebuffer, manages its
own memory, preempts its own tasks, and starts every processor the firmware
describes.

It routes its interrupts through the IOAPIC, finds an NVMe, SATA or ATA disk,
reads the GPT on it, keeps files in directories on a FAT16 or FAT32 volume,
talks to the internet over its own TCP/IP stack, takes input from PS/2 and
USB, and runs a desktop whose programs are real ring 3 processes.

When it fails it says why.

![the zelr desktop](docs/desktop.png)

<table>
<tr>
<td width="50%"><img src="docs/launcher.png" alt="the launcher open over a terminal"></td>
<td width="50%"><img src="docs/files.png" alt="the file manager in front of the terminal"></td>
</tr>
<tr>
<td align="center"><sub>the launcher, animating open over a running terminal</sub></td>
<td align="center"><sub>two windows, and which of them has focus</sub></td>
</tr>
<tr>
<td width="50%"><img src="docs/paint.png" alt="the paint program with three strokes drawn"></td>
<td width="50%"><img src="docs/settings.png" alt="the settings program showing accent colours"></td>
</tr>
<tr>
<td align="center"><sub>paint, a ring 3 process like everything else</sub></td>
<td align="center"><sub>the theme, which changes as you touch it</sub></td>
</tr>
<tr>
<td width="50%"><img src="docs/wallpaper.png" alt="an animated wallpaper with the apps along the panel"></td>
<td width="50%"><img src="docs/maximised.png" alt="a terminal filling the whole screen with the panel gone"></td>
</tr>
<tr>
<td align="center"><sub>one of the six wallpapers that move, and the apps kept on the panel</sub></td>
<td align="center"><sub>and the panel tucked away for a window that wanted the screen</sub></td>
</tr>
<tr>
<td width="50%"><img src="docs/monitor.png" alt="the system monitor showing processor use and a task list"></td>
<td width="50%"><img src="docs/calc.png" alt="the calculator showing 19.5 over the system monitor"></td>
</tr>
<tr>
<td align="center"><sub>what the machine is doing, thirty seconds of it</sub></td>
<td align="center"><sub>and 78 / 4, worked out without a floating point unit</sub></td>
</tr>
</table>

![a web browser showing a page fetched over http](docs/browser.png)

<sub>a web browser: that page came off a real web server, over this system's
own TCP, and was parsed and laid out by the program showing it</sub>

Every one of those was photographed by `tools/shots.py`, which boots the
machine, drives it, and saves what came out. They are not mockups and they do
not go stale quietly.

It is not a clone of anything. About 33,500 lines in all, of which 3,700 are
generated font data: roughly 19,600 hand-written lines of kernel, bootloaders
and headers, 5,500 of ring 3 programs, 5,500 of build and test tooling. No
libc, no runtime dependencies, and nothing borrowed from another kernel:
every driver, the filesystem, the bootloader, the image writer and the font
are written here, from the specifications where there is one and from scratch
where there is not.

The one exception, since "from scratch" invites the question: `zelr.exe`, the
Windows launcher, is a C# program that bundles the .NET runtime, which is
most of its 162 MB. The kernel inside it is about 1.5 MB. Nothing third party
runs on the machine zelr boots.

## running it on Windows

Download **zelr.exe** from the
[latest release](https://github.com/blavese/zelr/releases/latest) and run it.
The kernel is inside that file; nothing needs to be built.

The launcher checks for QEMU, the emulator zelr boots on, and offers to install
it from Microsoft's package manager if it is missing. Then press Start and a
black window opens with the operating system running in it. Type `guide` when
you get there.

It runs as a normal user, needs no administrator rights, and cannot affect
Windows: the kernel only ever sees the pretend machine QEMU gives it. Your
files live in a disk image under `%LocalAppData%\zelr`.

It opens the desktop by itself, with a terminal on it. Escape leaves that
for the console, and `desktop` at the console goes back. Both directions are
there because both are useful: the desktop is what a machine with a screen
should do when you switch it on, and the console is what you want when you
are driving it down a serial line or something has gone wrong.

A terminal opens, over a grey desktop with icons down the left of it. Click
the badge in the corner, or the wallpaper, for the launcher: a file manager,
an editor, paint, settings, a system monitor, a music player, a calculator, a
web browser, and what the machine is made of. Drag a title bar to move a window; the three
buttons at its right put it away, fill the screen, or close it.
Drag the bottom right corner to resize, or drag a title bar to an edge to
snap. Alt and tab changes window, alt and an arrow snaps, alt and d clears
the desktop, and shaking a window sends the others away. Escape returns to
the shell.

The wheel scrolls whatever is under the pointer, the speaker by the clock
sets the volume, and the launcher can switch the machine off.

The apps along the panel are kept there. Drag one to move it, right click it
to take it off, and right click anything in the launcher to put it on. A
program that is running is the same icon with a line under it, so the panel
reads as what you keep and then what you happen to have open. All of that is
in the settings window too, along with the screen size, whether the desktop
opens at startup, and one button that puts every setting back.

Everything on that desktop except the system info window is a separate
program running in ring 3. Settings cannot reach into the window manager at
all; it writes a file, and the window manager reads it, which is why the
desktop changes colour while the settings window is still open.

In the terminal, which is itself one of those ring 3 programs:

    tree /                 the whole filesystem
    cat /sys/memory        what is allocated, worked out as you read it
    ps                     what is running
    run hello              start another program and wait for it
    theme amber            and it is remembered next time

Tab completes commands and paths, up and down walk through history, and
PageUp scrolls back.

    mkdir work
    cd work
    write notes hello
    cat notes

    dhcp
    fetch example.com / page.html
    cat page.html

That gets an address from the network, downloads a live web page over TCP, and
saves it to a disk that survives closing the window. `browser example.com`,
or the browser in the launcher, shows the same page laid out rather than as
markup.

## running it on a real machine

Download **zelr.iso**, write it to a USB stick or burn it to a disc, and boot
from it. One file, four ways in, and it picks the right one itself:

![the console at startup](docs/boot.png)

<sub>what it says on the way up, before anything graphical happens</sub>


| | from a disc | from a USB stick |
|---|---|---|
| **BIOS** | El Torito, first catalog entry | the MBR at the front of the image |
| **UEFI** | El Torito, second entry, platform 0xEF | the partition marked as an ESP |

Both bootloaders are ours. Nothing else is involved: no GRUB, no syslinux, no
isohybrid, and the image is written by `tools/mkiso.py` rather than by
xorriso. The EFI system partition inside it is a FAT16 filesystem built by
`tools/mkfat.py`, not by mtools.

It runs entirely from the disc and writes nothing unless there is a hard disk
attached, in which case it will use it. **Be careful with that on a machine
whose disk you care about**: a blank or unformatted one gets formatted on
first boot.

### what to expect on a laptop made this decade

It will boot and you will get a screen: UEFI hands over a framebuffer and the
desktop draws on it at whatever resolution the firmware picked.

Whether you can then *use* it depends on the machine, and this is the honest
boundary.

**Input** goes through PS/2 where a machine still has it, and through USB
where it does not. There is an xHCI driver and the HID boot protocol, so a
keyboard or mouse plugged into a USB port is found at boot, addressed, and
read; both feed the same buffer the PS/2 drivers fill, so nothing above the
driver knows which one was typed on.

Hubs are walked as well, down the five tiers USB allows, so a keyboard behind
one is found the same way as a keyboard on a port of the machine itself. That
matters more than it sounds on a laptop, where the built-in keyboard is often
behind a hub inside the chipset rather than on a port anybody can see.
Something plugged in after boot is noticed and enumerated from a kernel task,
and pulling it out is noticed too.

A USB stick works too. Mass storage is SCSI posted through two bulk
endpoints, so a stick is enumerated, asked how big it is, read and written a
sector at a time, and mounted: its files are under `/usb`.

What is not there is any other class. Only the machine's own ports are
watched for a change: plug a keyboard into a hub that is already connected
and it is not found until the next boot.

**A USB stick mounts.** Plug one in and its files are under `/usb`, read and
written like anything else, and `cp` moves files between it and the disk the
machine booted from. The block layer holds disks by number rather than
holding one picked at boot, which is what made that possible: a stick
registers itself when it arrives and the volume is mounted from its partition
table, or from the whole device when it has no table, which is the two shapes
a stick comes in.

**Files keep their names.** A FAT directory entry holds eight characters and
three more, and everything since 1995 has carried the real name in extra
entries in front of that one. Both are written and read, so
`a-rather-long-file-name.txt` is what it is called rather than
`A-RATHE~1.TXT`, and a name that always fitted is still stored the way it
always was. tools/readfat.py reads them too, so the two implementations
disagree out loud rather than quietly.

**It turns off.** There is no instruction for that: a machine is switched off
by asking the chipset for sleep state five, and what has to be written to ask
is two numbers the firmware chose and left in its own bytecode. The kernel
finds them by reading it. `shutdown` in the shell.

**Sound** is Intel HD Audio, which is the controller every machine made this
century has. The controller half is small: a list of buffer descriptors and a
run bit, and once it is running whatever is in the buffer is what comes out.
The work is the codec behind it, a graph of converters, mixers and physical
sockets where nothing says which of them you can actually hear. The driver
reads the configuration the manufacturer left on each socket, picks the one
most likely to be the speaker, and walks backwards along the connection lists
until it reaches a converter. Tested by playing notes and measuring the
recording, because every step of an audio driver can report success while
producing silence.

**Storage** is NVMe, AHCI and ATA. NVMe is what a laptop bought this decade
has instead of the other two, and it is reached the way the specification
describes: queues in ordinary memory, a doorbell whose spacing the controller
reports rather than one that is assumed, and completions recognised by their
phase bit.

**Partition tables** are read, GPT and MBR, with both of GPT's checksums
verified, so a filesystem is found on a disk somebody else partitioned rather
than only on an image. Neither is written. An existing partition is never
formatted, and the EFI System Partition is never touched: it is FAT, so it
passes every other test, and it is also how the machine starts.

**Filesystems** are FAT16 and FAT32. Which one a volume is gets decided by
counting its clusters, which is the only thing the specification says decides
it; the string "FAT32" in a boot sector is a label and some formatters get it
wrong.

**Interrupts** are routed through the IOAPIC, with the firmware's list of
which legacy line really arrives where applied. Almost every machine moves
the timer from line 0 to line 2, and a kernel that assumes otherwise waits
forever for a tick that never comes.

**The machine describes itself** through ACPI: the tables are taken from the
pointer UEFI hands over rather than by searching memory a UEFI machine need
not have filled in, the XSDT is preferred where there is one, PCIe
configuration space is reached through the mapping MCFG describes, and every
processor the firmware lists is started. Until that pointer was wired through,
zelr had no ACPI on any UEFI machine at all, which is to say on every laptop,
and reported one processor whatever the machine had.

So: it boots, draws, finds every core, finds an NVMe disk, reads its GPT,
mounts a FAT32 partition without disturbing the one the firmware boots from,
takes a keystroke from a USB keyboard, and says what happened if any of that
fails.

### when it does not boot

A failure on a laptop used to be a black screen and nothing else. Under QEMU
the serial line carries the whole story out; the first boot on real hardware
is the one run where that is not available, and it is also the run most
likely to fail.

So the kernel narrates itself as it comes up, and there are three ways to
read it back.

**On the screen.** Any panic paints the tail of the log where a camera can
see it: the phase marks in blue, the fault and its registers in red. The last
mark is the thing that did not finish. Photograph it.

**From inside a running system.**

    cat /sys/boot        what this boot did
    cat /sys/lastboot    what the boot before it did

`/sys/lastboot` is the one that matters after a machine has failed to come
up: boot it again, read that, and the last mark names where it died. The
record is lifted off the disk early, before this boot writes its own, so
rebooting to ask the question does not destroy the answer.

**Over serial**, if the machine has a port. Every line is mirrored as it is
written, so nothing has to survive for it to be useful.

The log names what each phase found, not just that it ran, which is usually
the answer on its own:

    [    0] == disk
    [    0] disk none: no controller this kernel can drive
    [    0] == network
    [    0] net no card this kernel can drive

The disk copy lives in sectors reserved at the front of the volume, and is
written **only to a volume zelr formatted itself**. Four things about the boot
sector have to agree before a byte is written, and the sectors themselves
have to be blank or already hold a log. A disk that fails any of those gets
nothing and keeps working; the screen and serial routes are unaffected.

## running it from source

You need QEMU and Zig. Zig is used only as a cross compiler, so there is no
x86_64-elf toolchain to build first.

    ./run.sh          boot in a window
    ./run.sh -t       boot headless, console on stdout
    ./run.sh -T       run the built-in self test, exit code is the result
    ./run.sh -i       build a bootable image and boot it through our own
                      bootloader, the way a real machine would

`run.sh` creates a disk image and attaches a network card automatically. The
first three hand the kernel to QEMU with `-kernel`, which means QEMU is doing
the bootloader's job; `-i` is the one that does not.

The kernel handed to `-kernel` is `build/zelr.bin` rather than the ELF, and
that is not a detail: no multiboot loader will accept a 64-bit ELF, because
multiboot predates long mode. The multiboot header carries the a.out kludge,
which tells a loader to stop reading ELF headers and copy the flat image
instead.

    python tools/mkiso.py       write build/zelr.iso
    bash tools/iso_test.sh      boot it as a disc and as a stick
    bash tools/shell_test.sh    type at the shell over the serial line
    python tools/shotcheck.py   use the desktop and look at the screen
    python tools/termcheck.py   type into the terminal and check the result
    python tools/deskcheck.py   move the windows and check where they went
    python tools/usbcheck.py    boot with usb keyboard, mouse and stick, use them
    python tools/inputcheck.py  type on machines touched while they booted
    python tools/soundcheck.py  play notes and measure what came out
    python tools/mountcheck.py  mount a usb stick and copy files off it
    python tools/namecheck.py   save long names and read them back
    python tools/powercheck.py  tell it to shut down, see if it does
    python tools/appcheck.py    make the calculator divide, play a file
    python tools/abicheck.py    the structs the kernel writes and programs read
    python tools/netcheck.py    click for an address, see if one arrives
    python tools/shots.py       retake the screenshots in this readme

The Windows launcher lives in `launcher/` and is built with
`dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true`.
It embeds `build/zelr.elf` and a starter disk, so build the kernel and run
`userland/build.sh` first.

## what it actually does

**Boot.** Three ways in, all agreeing on one structure.

`bootloader/cdboot.S` is the BIOS one: the firmware drops it at 0x7C00 in
16-bit real mode and it opens the A20 gate, asks for the memory map, reads the
kernel off the boot device in 32 KiB chunks and copies each above 1 MiB
through unreal mode, which is the only way to write there without giving up
the BIOS calls it still needs. It handles both sector sizes, because a disc
reports 2048 bytes and a stick reports 512.

`uefi/loader.c` is the other. A UEFI machine never enters real mode and never
runs a boot sector, so none of that applies: it is an EFI application, a PE
executable the firmware loads and calls. The interface is transcribed from the
UEFI specification in `uefi/efi.h` rather than taken from gnu-efi. It locates
the graphics protocol and picks a mode, takes the ACPI pointer from the
configuration table, reads the kernel off the volume it booted from, and calls
ExitBootServices in the documented loop, because asking for the memory map
allocates, allocating changes the map, and that invalidates the key the call
wants.

Both build the same `handoff_t`, so the kernel has one entry contract and
never finds out which one started it. Multiboot could not have been that
contract: it cannot describe a framebuffer the firmware chose, has no room for
an ACPI pointer, and is 32-bit.

**Long mode.** `boot/boot.S` gets there from 32-bit protected mode, and the
order is fixed by the processor rather than by preference: long mode cannot be
entered without paging already on, and paging in long mode needs four levels
of tables that have to exist first. So it builds tables identity mapping the
first 4 GiB with 2 MiB pages, turns on PAE, asks for long mode, turns on
paging, and only then jumps through a 64-bit descriptor. It refuses to
continue on a processor without long mode rather than faulting somewhere less
explicable a few instructions later. The UEFI path skips all of this: the
firmware is already there.

**Descriptor tables.** A flat GDT (kernel and user code/data) plus a TSS, which
is how an interrupt taken in ring 3 finds a kernel stack to switch to. A 256-entry IDT with
stubs for all 32 CPU exceptions and 16 hardware IRQs, generated rather than
hand-written.

**Interrupts.** The 8259 PICs are remapped off the exception vectors to 32..47.
Exceptions that nothing handles print a register dump and halt, instead of
silently triple faulting.

**Physical memory.** A bitmap allocator, one bit per 4 KiB frame, built from
the firmware memory map. The kernel image and the bitmap itself are marked in
use so they can never be handed out.

**Virtual memory.** Two-level paging. The low 16 MiB is identity mapped so that
enabling paging does not move the ground out from under the kernel, and
map_page / unmap_page / virt_to_phys work for anything above that. Page faults
report the faulting address and whether it was a read or a write.

**Heap.** First-fit free list with boundary tags, coalescing neighbours on
free. kmalloc, kcalloc, kfree.

**Tasks.** Round-robin preemptive multitasking. Each task owns a kernel stack
holding a complete interrupt frame, so switching is a matter of telling the
interrupt return path to unwind a different one. Tasks sleep, yield and exit
with a status somebody can collect, and dead ones are reaped once it has
been, or after a grace period if nobody asks.

A task can also block on an address and cost nothing while it waits: the
scheduler skips it entirely, and whoever changes that thing wakes everyone
waiting on it. The channel is just an address, so nothing has to be declared
waitable, and a wait can carry a deadline and tell a timeout apart from an
actual wakeup.

**Disk.** Two drivers behind one block layer. AHCI is tried first, because it
is what a real machine and every modern virtual machine present: the driver
builds a command list and a scatter-gather table in memory and lets the
controller do the transfer itself. If there is no AHCI controller it falls back
to ATA PIO on the primary bus, which moves every word through the CPU and needs
no bus mastering setup. Whichever answers, the rest of the kernel sees the same
four calls.

**Filesystem.** FAT16, so the disk is not a sealed box: other tools can open
the image and files move in both directions. Files are worked on in memory and
written through on every change. Writes are ordered so that losing power part
way through cannot destroy what was already there: the new cluster chain is
written and flushed first, the directory entry is committed as a single sector,
and only then is the old chain released. Anything an interruption stranded is
found and reclaimed at the next mount. A blank disk is formatted automatically
on first boot. `tools/readfat.py` parses the image straight from the
specification, sharing no code with the kernel, and can copy a file in from the
host.

**Network.** PCI enumeration to find the card, then one of two drivers behind
a common interface. The Intel e1000 is tried first, since it is what VirtualBox
and VMware present by default; it is driven through memory-mapped registers and
descriptor rings the card DMAs into by itself. A Realtek RTL8139 driver covers
the other common case with a circular receive buffer and four transmit
descriptors. On top of either: ethernet, ARP with a cache, IPv4 with checksums,
ICMP (it answers pings and sends them), UDP, a DHCP client, a DNS resolver, and
a single-connection TCP client with a three way handshake, orderly close, and
retransmission with exponential backoff. `fetch` uses all of it to do an
HTTP GET.

**The font.** Ninety-five glyphs on an 8 by 16 cell, drawn by hand in
`tools/genfont.py` as pictures made of dots and hashes, which is also how they
are edited. It used to be traced from a system typeface, which made the shapes
somebody else's and put an imaging library in the way of building a font.
Capitals are nine rows, x-height is six, stems are one pixel, and the whole
thing is emitted twice: once as a C array for the kernel and once as a header,
because ring 3 cannot link against the kernel's copy.

**Graphics.** Mode setting through the Bochs VBE dispatch ports rather than a
BIOS call, so it works from protected mode with no real mode trampoline and no
help from the bootloader. The aperture is found through the VGA device's PCI
BAR and mapped explicitly. Drawing goes to a back buffer and is pushed to the
card in one go, because compositing directly in video memory over PCI is
visibly slow. The console is redrawn on top of that with a bitmap font, so
everything that already printed kept working.

**Other processors.** A PC boots with one CPU running and does not say the
others exist, so `acpi.c` goes and reads the firmware tables to find them and
`smp.c` starts each one with an INIT signal followed by a startup signal
carrying a page number. It begins executing there in real mode with no stack
and no paging, which is what `bootloader/trampoline.S` is for. What they do
afterwards is a decision rather than a requirement: sharing the scheduler
would mean a lock on the heap, the task list, the filesystem and every driver,
so instead each one waits for a function to be handed to it. The boot
processor still owns the kernel; the others own nothing until they are given
something.

**Programs.** Ring 3, its own address space per process, and thirty-eight
system calls through int 0x80. A program can start another program, block
until it finishes and read what it returned from `main`, so the terminal
starting `paint` is one ring 3 process starting another with the kernel only
lending a hand. An ELF32 loader maps each PT_LOAD segment where the
file asks and refuses anything that would land in kernel memory. `userland/`
holds programs built entirely separately: the only thing they share with the
kernel is the syscall numbers. They are then pasted whole into the kernel
image, so a fresh install already has something to run. They are deliberately
never saved to the disk: if they were, the first boot would write them out and
every later boot would run the written copies, so rebuilding the kernel would
appear to change nothing.

**Windows.** A compositing window manager: windows are off-screen surfaces,
the manager owns the chrome, the stacking order and the pointer, and the whole
screen is assembled into the back buffer and pushed once per frame so a window
moving over another leaves no trail. Title bars drag, clicking raises, the
close box closes, and a taskbar shows what is open.

**The panel.** A bar welded to the bottom of the screen means a maximised
window is not maximised: it stops short, and the last thirty pixels of the
display are spent on something that is looked at occasionally. A bar that is
always hidden means reaching for it every time, which is worse. So it is
neither. Nothing wants the room and it is out, floating clear of the edge
with the wallpaper showing around it; something does and it slides away and
the window has the whole screen; put the pointer at the bottom and it comes
back over the window, and stays as long as the pointer is on it. There is no
setting for any of that. It is a consequence of what is on the screen.

A window covering the whole screen also means the wallpaper and everything
under that window are being drawn and then painted over, twelve times a
second if the wallpaper is one that moves. Neither is drawn at all now.

The apps on it are a list in a file, `/zelr.pins`, one program to a line.
Dragging an icon reorders the list as the pointer crosses each slot rather
than when the button comes up, so the icons move out of the way while it is
happening. There are no icon files anywhere in this project: an app's icon is
a rounded square in a colour worked out from its own path, with the first
letter of its name in it, which tells five of them apart at a glance and
costs nothing to carry.

**The screen, at whatever size.** Nothing on this desktop knows a
resolution: every window, the panel, the launcher and each wallpaper is laid
out from the width and height of the framebuffer, every frame. So the size
can change while the machine is running. Pick one in the settings window and
it is written into the same file the colours live in; the window manager
reads that four times a second, asks the card for the mode, and refits the
windows around what came back. A size the card will not take leaves the
screen exactly as it was, because the new back buffer is allocated before
the old one is let go and the card is asked what mode it is actually in
rather than told.

Where the mode came from decides whether it can be changed at all, and the
settings window says which it is rather than offering a list that does
nothing: a mode this kernel set through the dispatch interface can be set
again, and a framebuffer a UEFI machine handed over is the size the firmware
chose for as long as the machine is on.

**The window server.** A program in ring 3 cannot touch the framebuffer and
cannot be handed a kernel pointer, so a window's pixels are allocated on a page
boundary and mapped into the calling process with the user bit set. The program
draws into that memory directly and asks for a repaint; the kernel keeps the
window and hands back only a small integer handle, checked against the caller
on every call. Input travels the other way through a per-window event queue.
`paint` is an ordinary ELF executable that uses nothing else: sixteen colours,
four brush sizes, and strokes interpolated with Bresenham, because the mouse
reports in jumps and without it a quick stroke is a row of dots.

**Drivers.** Framebuffer and VGA text consoles, PS/2 keyboard with
shift/caps/ctrl/alt, arrows and function keys, PS/2 mouse with a drawn
pointer, PIT at 100 Hz, and a 16550 serial port driven by IRQ4. A key carries
the modifiers that were held when it was pressed rather than when it is read,
because by then a chord has usually been let go.

**The filesystem.** Six directories. `/home` is yours and where the shell
starts, `/doc` is what shipped, `/cfg` is what programs remember, `/tmp` is
emptied at boot. The other two are not stored anywhere: `/bin` holds the
programs that live inside the kernel image, and reading a file in `/sys` runs
the code that works out the answer, so it is never stale. Files that ship
with the system are offered once, recorded by generation on the disk, so one
you delete stays deleted.

**The desktop's programs.** The terminal, paint and settings are ordinary ELF
executables in ring 3. The terminal is a shell that is not part of the kernel:
listing a directory, reading a file, writing one, starting another program and
fetching a page over TCP all go through int 0x80, and its `get` command does
an HTTP GET from user space. It has a line editor, history kept in `/cfg`, tab
completion over both commands and paths, and thirty-six commands of its own.
Settings is the interesting one, because it changes how the desktop
looks without being able to reach the window manager at all. It writes
`/zelr.cfg`, a plain "key value" file, and the window manager re-reads that four
times a second. Anything the window can do can also be done with the shell's
`write` command.

Eleven wallpapers, six of which move: drifting stars, travelling waves,
aurora, rain, wandering orbs and rings going out from the middle. They are
all cheap on purpose, because this has to stay smooth on a machine with no
graphics acceleration of any kind: a column fill or a few thousand points a
frame, never a calculation per pixel of the screen. The curves come from a
seventeen entry table, since the kernel is built with no floating point in it
at all.

**Eight programs.** The terminal, the file manager, the editor, paint,
settings, a system monitor, a music player and a calculator, all ring 3 and
all using nothing the kernel does not offer everybody.

The monitor is the one that says most about the machine, and the number it
puts at the top took two goes. The scheduler counts slices, one for whichever
task it picked on each tick, and the share of ticks handed out looks like the
answer. It is not: this is a round robin kernel, so a loop waiting for a key
is picked every tick and spends the slice halted. Measured that way an idle
machine reads a hundred per cent, and it did.

So a loop that is only waiting says so, the kernel counts the ticks it slept
through, and slices minus those is work. Two things came out of that. The
processor figure means something, and the desktop stopped spinning its loop
as fast as the processor would go when there was nothing on it to draw, which
on a laptop is the difference between a warm machine and a cool one.

The calculator is integer arithmetic, because there is no floating point
anywhere in this system: the kernel is built with the vector registers turned
off and a program that used a double would fault on the first instruction
that touched one. Everything in it is a sixty four bit count of millionths.

The music player reads WAV files, which is a header and then the samples.
The hardware plays at one rate and in stereo and will not be argued with, so
a file recorded at some other rate is stepped through at a ratio held in
sixteen fixed point bits and a mono file has each sample written twice. Put
one on a USB stick, plug it in, and it is under `/usb`.

**Opening a file.** A program could be started and could not be told
anything, so a file manager could offer to open a file in the editor and had
no way to say which file. A task now carries one string, which for everything
that uses it is a path: the file manager decides from the name which program
should have it, and that program is told which file. The terminal passes one
too, so `music /usb/tone.wav` and `notes readme` do what they look like.

**Scrolling.** A PS/2 mouse reports three byte packets because that is what a
mouse reported in 1987, and only starts sending a fourth after it is asked in
a way no ordinary sequence of commands would produce by accident: three
sample rates in a fixed order, and then asking the device who it is. One with
a wheel answers 3. Above the driver it is one number, steps since somebody
last looked, and the window manager hands it to the window under the pointer
rather than the focused one.

**A desktop that is built rather than tinted.**

The whole of the look is one idea: a surface is not a colour, it is a plane
with a light above and to the left of it. Everything raised has a bright top
and left and a dark bottom and right, everything sunk has it the other way
round, and a control says what it is by which way its edges go. A button is
raised, so it can be pressed; a list is sunk, so it cannot. Pressing one does
not tint it, it turns the bevel over and moves the label a pixel down and
right, so the thing is genuinely depressed. That reads as a press at any size
and in any palette, which a colour change does not.

Two pixels rather than one, because one is a line and two is a bevel: the
outer pair carry the strong colours and the inner pair the soft ones, and
together they read as a chamfer.

It is a grey and not an off-white for a reason that took a rebuild to see.
Chrome built out of edges needs a surface with room above it and below it,
and a near-white one has nowhere to put a highlight: every bevel collapses to
a single grey line and the whole desktop goes flat. So the ground is a warm
neutral a few steps down, which is the only part of this that is a matter of
taste rather than mechanics.

Windows are square, because a bevel has to turn a corner to read as one and a
rounded corner has nowhere to put four edges. Nothing casts a shadow, because
a bevel already says which way is up and a drop shadow on top of one is two
answers to the same question. The panel is flush to the bottom edge and the
full width of it, because a panel is part of the machine rather than a card
lying on the desktop.

All of it comes out of the theme, so the six accents and the dark ground
still work: the four edge colours are derived from the surface the same way
every other colour here is, and a program gets them in the same struct it
already reads. One header changed and every window in the system changed
with it, which is the test of whether a toolkit is one.

**A menu bar, at last.** File, Edit, View, Help. The thing most missing from
this desktop: every window had its commands behind whichever button its
author found room for, so no two programs put anything in the same place and
nothing was discoverable. The file manager has one now, and everything in it
does what the toolbar button or the context menu does. A command reachable
two ways is not duplication, it is the difference between a program you can
learn and one you have to be told about.

**Desktop icons, drawn rather than stored.** A bitmap for each would mean a
file format, a loader for it and a directory to keep them in before anything
appeared on the screen at all, and at thirty two pixels a drawn shape and a
stored one are the same picture. Single click selects, a second within half a
second opens, because dragging one has to start with putting the pointer on
it. A program's first window opens to the right of them rather than on top,
which is the only reason to have put them on the left.

**A click that was never delivered.** A window's events arrive through a ring
of thirty two, and a pointer crossing the screen fills it with nothing but
positions. So the press was dropped for want of room, the release turned up
once the backlog had drained, and a release with no press before it sets
nothing: the program sees the button go up from a state where it was already
up. Nothing in the path reports a dropped event, so what this looked like was
a button that did not work, three times over, with the pointer sitting on it
in the photograph.

Positions are folded into one another now, so the ring cannot fill with them.
Which events count as positions took a second attempt: the release carries no
buttons and so does the move after it, and folding one into the other on that
alone puts the release wherever the pointer went next, which for a button
means it was let go somewhere else and the click is lost again. An event is
overwritten only when it carries the same buttons as the one before it and
the one after it.

**A web browser.** It opens a TCP connection with this system's own stack,
asks a server for a page, reads the HTML, lays it out against the width of
its own window and draws it with the letterforms in `face.h`. There is an
address bar, a history with back and forward, a scrollbar, and links you can
click.

There is no CSS in it. A page is drawn the way a page was drawn before there
was any: headings bigger, paragraphs with air around them, lists indented and
bulleted, preformatted text in the fixed pitch font, links blue and
underlined, and everything in the order the markup puts it. That is an answer
rather than a stopgap. A page whose meaning is in its markup reads properly,
and a page whose meaning is entirely in a style sheet reads as one long
column, which is what it is.

The work is not in the request. It is in the three ways a server can say
where a body ends, all of which are common and only one of which is obvious:
a content length, chunks each with their own size, or nothing at all and the
body ends when the connection does. A client that knows one of them loses the
end of about half the web without ever reporting a problem. It is also in the
markup: script that looks exactly like prose to anything reading for angle
brackets, tags that are never closed, entities that are not the characters
they spell, and a page full of curly quotes and dashes on a machine whose
font is the printable half of ASCII. Each of those is written back the way it
was written before there was anything but ASCII, so a reader loses the shape
of a mark rather than the sense of it.

What it cannot do is https, and that is not a footnote: it is most of the
web. TLS means a certificate parser, big integer arithmetic and a key
exchange or two, all of which have to be written here as well. It is the next
piece of work rather than a limitation being papered over, and until it is
done the browser says so on the page instead of failing quietly.

**The network, and being straight about it.** There is an icon on the panel
next to the speaker, and it says three things apart rather than two: no
link, a link with no address, and a link that can reach something. The
middle one is the state people actually get stuck in, and an icon with two
states cannot show it, which is why a cable plugged into a router with no
DHCP behind it looks, on most machines, exactly like no cable at all.
Clicking it opens a panel with the card, the address, the router, and a
button that asks for one. Asking happens in a task of its own, because a
compositor that stops for the length of a DHCP exchange is a desktop that
freezes whenever somebody plugs a cable in.

The panel also says what wireless hardware is present and whether it can be
used, which needs explaining. Most wireless cards run the 802.11 MAC as
firmware on a processor of their own, and that firmware is a binary from the
vendor. A kernel written from scratch cannot make such a card transmit at
all: the logic is not missing, it is on the other side of a chip that will
not start without its own software, and no amount of further code changes
that. Atheros parts are the exception, because their MAC is in hardware.
So a machine with wireless it cannot use says which card and why, which is
more useful than an empty list of networks and a good deal more honest.

**Ethernet over USB**, which is the answer to a laptop whose wireless will
not start without a vendor binary. There is still a socket on the side of
it, and a phone with tethering turned on presents itself as a network
adapter over the same protocol an adapter does. Nothing in it has to be
taken on trust from a binary: RNDIS is published, it is two bulk endpoints
and a handful of messages posted through the control endpoint, and it runs
over the xHCI stack that was already there for the keyboard.

Two things in it cost an afternoon each and are worth writing down. The
buffers a control message is built in cannot be on the stack: the controller
is handed a physical address and this kernel hands it the pointer it has, so
anything it is pointed at has to live where the two are the same, which the
heap is and a kernel stack is not always. And a query carries no information
buffer, so the offset of that buffer is zero: naming an offset for a buffer
that is not there puts it one past the end of the message, and a device that
checks stalls the whole transfer. What that looks like from this side is one
message sending and the next one not.

**WPA2, written out.** Joining a protected network is not sending the
password: the password never crosses the air. Both ends grind it into the
same key and then prove to each other that they did. All of that is
arithmetic and none of it needs a radio to be right, so it is written and
checked first: SHA-1, HMAC-SHA1, PBKDF2 and AES, and on top of them the key
a password and a network name turn into, the session keys, the signature on
a handshake message, and unwrapping the group key.

Every number it is checked against is somebody else's: FIPS 180-1, RFC 2202,
RFC 6070, IEEE 802.11i annex H, FIPS-197, RFC 3394. That is the point.
Cryptography that has only been made to agree with itself is cryptography
nobody should trust, including whoever wrote it.

Two details worth the space. The addresses and nonces go into the derivation
smallest first rather than in the order they arrived, because neither end is
in charge and both have to sort the same pair the same way; getting it wrong
gives two keys that are each perfectly well formed and are not the same, and
it surfaces later as traffic that cannot be read. And a signature is
compared a byte at a time with no early exit, because a comparison that
stops at the first difference tells anyone who can time it how much of a
guess was right.

**A trackpad.** The pad in a laptop answers as that same 1987 mouse unless
it is asked otherwise, and the asking is a knock of the same kind: there is
no command that takes an argument, so the argument goes through four
set-resolution commands two bits at a time, and a status request reads three
bytes back. A mouse answers with its resolution. A Synaptics answers with
0x47 in the middle byte, and that is the whole identification.

It has to be done before interrupts are on, which is not a detail. After
that the timer drains the 8042 on every tick and hands what it finds to the
packet decoder, so the three bytes of an answer are taken before the read
sees them, and a pad that is present and answering correctly reports as
absent.

What comes back is where the finger is, and a pointer needs how far it
moved, so the work is subtraction and the bugs are all ways subtraction goes
wrong. A finger that lifts and lands elsewhere must not drag the pointer, so
the first report of a contact sets an origin and moves nothing. A pad is
four thousand units across and a screen is a thousand pixels, so the
difference is divided down, and the remainder is kept, because a pointer
that cannot be moved slowly cannot be aimed. A finger arriving or leaving
changes which contact is being reported, and the jump that causes is caught
by noticing the count change rather than by the distance: the first attempt
used distance alone, with a limit smaller than an ordinary flick, and threw
away real movement.

Two fingers scroll. A tap is a contact that ended quickly and went nowhere,
one finger for the left button and two for the right, held for a few ticks
afterwards because the window manager reads the buttons once a pass and a
click released before the next one never happened.

No emulator has one of these, so none of that can be checked against
hardware here. What is checked is the decoder, by assembling reports and
feeding them to it, and the checks were themselves checked by breaking the
decoder six ways to see that each break is caught.

**Shell.** Reads from the keyboard or the serial line, whichever produces a
character first, so a person can type at it and a script can pipe into it. It
is still the kernel's own, on the console; the one in a window is a program.

## testing

The kernel tests itself. `./run.sh -T` boots with selftest on the command line,
runs 390 checks across every subsystem, then writes to QEMU's debug-exit port
so the host gets a real exit status.

    [string]              8 checks   [graphics]           13 checks
    [the identity map]    6 checks   [windows]             7 checks
    [physical memory]     4 checks   [window server]      16 checks
    [paging]              4 checks   [built-in programs]   6 checks
    [user access]         5 checks   [theme]              16 checks
    [heap]                5 checks   [taskbar]            18 checks
    [filesystem]          7 checks   [live tree]          19 checks
    [paths]              11 checks   [layout]              9 checks
    [directories]        12 checks   [waiting]            16 checks
    [open files]         12 checks   [trackpad]           25 checks
    [timer]               2 checks   [crypto]             22 checks
    [interrupts]          2 checks   [wpa]                19 checks
    [disk]               12 checks   [wait timeouts]       3 checks
    [fat]                14 checks   [processors]          2 checks
    [network]             7 checks   [black box]          21 checks
    [elf]                 7 checks   [acpi and pcie]       4 checks
    [userspace]           4 checks   [interrupt routing]   9 checks
    [video]               7 checks   [clipboard]          14 checks
    [mouse]               4 checks   [clock]              18 checks

    390 passed, 0 failed
    SELFTEST_PASS

The processor section is two checks on a machine with one CPU and eleven on
a machine with several, where it hands work to each of them and requires the
count they share to come back exact. `qemu-system-x86_64 -smp 4` reaches 399.

The same checks run again on `-machine q35`, which has PCIe and an AHCI
controller rather than a 1996 chipset and a PIO disk, and reach 398 there.
Two bugs found the day that was added were invisible on the older machine:
the block layer would not split a request past the eight sectors AHCI
accepts, and the ACPI tables were never read on a UEFI machine at all.

The tests are written to fail for the right reasons. The disk test writes a
pattern to a spare sector, reads it back, and restores the original. The FAT
test writes a file spanning several clusters, so it exercises chain following
rather than a single sector. The network test performs a real DHCP handshake,
pings the gateway and resolves a live hostname. The ELF test feeds the loader
six malformed images, including one asking to be mapped over the kernel, and
requires each to be refused. The user access test maps a kernel page and a user
page into the same page table and requires the kernel one to stay out of reach,
because that is the distinction a system call has to make about a pointer it is
handed. The userspace test watches the system call counter rather than the task
list, because a program can finish before a count is taken.

`tools/iso_test.sh` is the fourth, and the only one that does not use QEMU's
`-kernel`. It builds the image and boots it all four ways a real machine
might, typing at the shell each time rather than trusting the banner.

`tools/shell_test.sh` is the second half. It boots the OS, types commands at
the shell over the serial line, and checks what comes back, including running a
program that reports what it can see of its own window from ring 3.

`tools/shotcheck.py` is the third. Neither of the others can prove anything
reaches the screen, so this one boots headless, opens the desktop, drives the
real mouse through QEMU's monitor to draw a stroke in paint, takes a real
screenshot and counts pixels. It caught a bug the other two could not: the
surface was being mapped one page before the window's pixels, which drew a
recognisable but wrong toolbar.

## things that went wrong

The interesting part of writing a kernel is that nothing catches you. Every one
of these presented as a machine that stopped, with no message.

**memset called itself.** At -O2 clang recognises the byte-fill loop inside
memset as a memset, and replaces the body with a call to memset. It recursed
until the stack was gone. -fno-builtin does not stop the loop idiom pass; the
fix was volatile pointers in the three byte movers.

**The compiler emitted SSE.** Zig's default x86 target has SSE2 on, so clang
used movd xmm0 for a 64-bit integer move. The CPU had never been told the FPU
exists, so the first one raised an invalid opcode fault, far from anything that
looked related. Fixed with -mcpu=i686, and tools/check_sse.py now fails the
build if an SSE opcode reaches an executable section.

**The TSS descriptor got an address where it wanted a length.** Passing
base + size - 1 instead of size - 1 as the limit made ltr fault, which triple
faulted the machine one instruction into gdt_init.

**One timer tick, then silence.** The PIC will not deliver another interrupt at
the same or lower priority until it is acknowledged. The end-of-interrupt write
was missing, so exactly one IRQ0 arrived and the kernel waited forever for the
second.

**Serial input arrived shredded.** Piping a command into the shell produced
"notes.txshell" instead of "notes.txt shell". Instrumenting both ends settled
it: for a 26 byte burst the kernel reported irqs=3 got=4 read=4 dropped=0. The
receive path had lost nothing, because it was never given the bytes. QEMU's
-serial stdio backend does not apply back pressure to a pipe. The kernel now
takes IRQ4 and buffers into a ring, and the harness types at human speed, which
the guest keeps up with exactly (irqs=104 got=104 read=104 dropped=0).

**The tests waited by sleeping, and lied about it.** The three harnesses that
drive the desktop each did the same thing: press a key, sleep three seconds,
take a screenshot, decide. On an idle machine that is enough. Under the full
gate, which runs four machines at once, it is not, so about one run in ten
failed on a different check each time. Every one of those reads as a broken
window manager, and one of them cost most of a day before it turned out that
tripling every sleep made the failure vanish on the exact commit that appeared
to have caused it. They wait for what they are waiting for now, which is both
reliable and two to four times faster, because a check that passes in 300 ms
no longer costs three seconds.

Three real faults were underneath that. Screenshots were read as soon as the
file existed rather than when QEMU had finished writing it, so the bottom of a
short one read as black and whatever was being counted was not there. QEMU's
serial output went to a pipe nobody drained, which stops the guest dead once
it fills. And the window manager reads the mouse once per pass of its own
loop, so a press and release that both land inside one pass is a click it
never sees; the harness holds the button down for longer now, and checks that
the click did something rather than assuming.

**A directory that ring 3 saw as empty.** `ls /` in the terminal on the
desktop listed nothing, and so did the file manager, while the same call from
the kernel's own shell listed six entries. vfs_list fills a name with strncpy,
strncpy pads to the full width, and the struct the readdir system call filled
had a 32 byte name in it while the width became 64 when long filenames
landed. Every readdir wrote 32 bytes past the end of a struct on the kernel
stack, upward, into the saved registers the call returns through: the listing
worked, and what the program got back was a zero the padding had written. The
field is as wide as what the kernel writes into it now, and a static assert
says so.

**A calculator that could not divide.** 78 / 4 came out as 0.099489, which is
78 / 784. Drawing the pending operation formatted the first number by writing
it into the display, keeping a copy of what was there and putting it back
afterwards; it put the string back and left the flag that says whether the
display is being typed into, so the digit after an operator was appended to
the first number instead of starting the second. Found by a check that makes
the calculator work out a sum and then types the answer in by hand: if the
arithmetic is right the two pictures of the display are identical, and it
reads no text off the screen at all.

**And three of them passed for the wrong reason.** The wallpaper checks were
the worst: one compared the length of a screenshot against zero, which is true
of any picture. The other two compared two pictures of the desktop and called
them different, and they were, because a window was still closing in the first
one and the mouse pointer had moved between them. Deliberately breaking the
config parser so that setting a wallpaper did nothing at all left all three
still reporting PASS. They wait for the windows to be gone and park the
pointer somewhere outside the comparison now, and all three fail on that
break.

## what it does not do

Being explicit about the boundary, because "operating system" covers a very
large range:

- **No fork or exec in the Unix sense.** A program is loaded and run; it
  cannot start another or replace itself. The launcher and the shell start
  programs because they are the kernel, not because a program can.
- **Forty system calls.** Enough to print, walk directories, read and write
  files, open one TCP connection, sleep, exit, wait on a child and own a
  window. There is no signal, no pipe and no memory mapping.
- **No shared libraries**, no dynamic linking, no relocation: programs are
  static and loaded at a fixed address.
- **Eight windows at once**, which is a fixed array and not a limit anybody
  reached. Resizing works, by the corner grip, by maximising and by snapping
  to an edge: the surface is reallocated and the program is told its new
  size. This entry used to say resizing did not exist, long after it did.
- **The system info window is still kernel code**, because it reports on the
  allocator, the scheduler and the clock, and no system call exposes those.
  Every other window on the desktop belongs to a ring 3 process.
- **The other processors do not run tasks.** They are started, they execute
  work handed to them and they share a lock, but the scheduler runs on the
  boot processor alone. Spreading it would mean a lock on the heap, the task
  list, the filesystem and every driver.
- **TCP handles one connection at a time.** It retransmits with exponential
  backoff and gives up after six tries, but there is no congestion control, no
  window scaling and no selective acknowledgement.
- **No TLS**, so `fetch` is plain HTTP only.
- **Ping only reaches the local network.** ICMP is implemented in both
  directions and pinging the gateway works. QEMU's user mode networking does
  not forward ICMP to the wider internet without elevated privileges, so
  pinging an outside address times out even though DNS and TCP to that same
  address work.
- **USB stops at keyboards, mice, hubs and storage.** No other class is
  claimed. Only the machine's own ports are watched for a change, so
  something plugged into a hub after boot is not found until the next one.
  The controller is polled on the timer tick rather than wired to an
  interrupt, which costs up to ten milliseconds of latency on a key press.
- **Two volumes at a time.** The disk the machine booted from, and one
  removable. A second stick is a disk with a number and no way to mount it.
- **Names are ASCII.** The entries that carry a long name hold sixteen bit
  characters, and anything above 127 comes back as a question mark rather
  than as half of something nobody can type.
- **Memory is capped at 64 GiB**, and by how much bitmap fits between the
  kernel and the heap, whichever is lower. What the machine actually has is
  what gets mapped: the bottom 64 MiB a page at a time, and everything the
  firmware called usable above that in 2 MiB pages. The gaps between are
  left alone, because that is where devices keep their registers and they
  have to be mapped uncached rather than as ordinary memory.

It is a real kernel in that it boots itself on a bare machine, drives its own
hardware, and can fetch a file from a real server and keep it on a real disk.
It is not something you would run anything important on, and it is several
orders of magnitude away from Linux, which is roughly 30 million lines.

## layout

    boot/boot.S        multiboot header and entry point
    kernel/gdt.c       segments and the task state segment
    kernel/idt.c       interrupt descriptor table and dispatch
    kernel/isr.S       the 49 interrupt stubs (generated)
    kernel/pic.c       8259 remapping
    kernel/lapic.c     each processor's own interrupt controller
    kernel/ioapic.c    interrupt routing, and the firmware's overrides
    kernel/pmm.c       physical frame allocator
    kernel/paging.c    four-level paging
    kernel/heap.c      kmalloc
    kernel/sched.c     preemptive round-robin tasks
    kernel/wait.c      blocking on an address instead of spinning
    kernel/blockdev.c  picks a disk driver and hides which one
    kernel/ahci.c      sata through ahci
    kernel/nvme.c      nvme, the disk a modern laptop has
    kernel/ata.c       ata pio disk driver
    kernel/parts.c     gpt and mbr partition tables
    kernel/diskfs.c    reading and writing the filesystem image
    kernel/fs.c        the in-memory filesystem, for a machine with no disk
    kernel/pci.c       pci configuration space, ports and the pcie mapping
    kernel/netdev.c    picks a network driver and hides which one
    kernel/e1000.c     intel e1000 driver
    kernel/rtl8139.c   rtl8139 driver
    kernel/net.c       ethernet, arp, ip, icmp, udp, dhcp, dns
    kernel/tcp.c       tcp client
    kernel/http.c      http get
    kernel/fb.c        linear framebuffer via the bochs vbe ports
    kernel/fbcon.c     the text console drawn into it
    kernel/font.c      the 8x16 font (generated from the drawings)
    kernel/mouse.c     ps/2 mouse and the drawn pointer
    kernel/fat.c       fat16 and fat32
    kernel/elf.c       elf32 loader
    kernel/syscall.c   the system call table
    kernel/user.c      building and launching ring 3 processes
    kernel/wm.c        the window manager and the launcher
    kernel/winsrv.c    handing window surfaces across to ring 3
    kernel/theme.c     the desktop's appearance, and the file it lives in
    kernel/pins.c      the apps kept on the taskbar, and their file
    kernel/synaptics.c a trackpad, and turning a position into a pointer
    kernel/crypto.c    sha-1, hmac, pbkdf2 and aes, written out
    kernel/wpa.c       what a wireless password turns into
    kernel/wifi.c      what wireless hardware is here, and whether it is usable
    kernel/usbnet.c    ethernet over usb, for a phone or an adapter
    userland/monitor.c what the machine is doing, while it does it
    userland/music.c   wav files, resampled to whatever the card wants
    userland/calc.c    arithmetic in millionths, because there is no fpu
    kernel/builtin.S   the user programs, pasted into the kernel image
    kernel/apps.c      the system info window
    kernel/vfs.c       one namespace over the live tree, the disk and memory
    kernel/sysfs.c     /sys and /bin: files that are generated when read
    kernel/layout.c    the directory layout, and what ships in it
    kernel/acpi.c      the firmware tables: processors, and where pcie is
    kernel/blackbox.c  what the machine was doing when it stopped
    kernel/smp.c       starting them and handing them work
    bootloader/        the BIOS bootloader, and where a second cpu starts
    uefi/              the UEFI bootloader, and the firmware interface
                       it is written against
    kernel/gfx.c       drawing into off-screen surfaces
    kernel/vga.c       text console
    kernel/serial.c    16550 uart, interrupt driven
    kernel/keyboard.c  ps/2 keyboard
    kernel/xhci.c      the usb host controller every laptop has
    kernel/usb.c       enumeration, and the hid boot protocol
    kernel/usbdisk.c   usb sticks, which are scsi through bulk endpoints
    kernel/hda.c       the sound controller, and walking its codec
    kernel/sound.c     what is in the buffer when the hardware reads it
    kernel/power.c     turning the machine off, which means reading aml
    kernel/timer.c     programmable interval timer
    kernel/shell.c     the shell
    kernel/welcome.c   the first-run text and the guided tour
    kernel/selftest.c  the boot-time test suite
    kernel/divide.c    64-bit division helpers libgcc would normally provide
    userland/          programs, built separately from the kernel:
                       a terminal, paint, settings and three small tests
    tools/             build checks, the font generator, a FAT reader and
                       a FAT writer, the image builder, the four test
                       harnesses, and harness.py, which is how they drive a
                       running machine and wait for it
    launcher/          the Windows launcher (C#/WPF)

## license

GPLv3, see [LICENSE](LICENSE).

Free to read, run, change and share. The condition is that anything built from
it stays that way: distribute a modified version and it carries the same
license, with the source. Selling it is allowed, and always has been under
this license. Closing it is not.

Versions up to and including 0.16.1 went out under MIT and stay under MIT.
Nothing here is retroactive.
