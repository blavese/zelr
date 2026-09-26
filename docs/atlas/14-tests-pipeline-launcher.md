# Atlas 14: test infrastructure, the AI development pipeline, releases, and the Windows launcher

Source tree: the repository root (github.com/blavese/zelr, main, 2026-09-22, two commits after v0.37.0).
`KERNEL_VERSION` is `"0.37.0"` (include/types.h). `.git/packed-refs` has 40 tags, and the newest is `v0.37.0`.

Conventions used below:
- `file:line` refers to this copy.
- `G(pc,256)` means a `harness.Guest` on QEMU's default `pc` machine with `-m 256`, and `G(q35,128)` means `machine="q35"`, `-m 128`. The full command line a Guest builds is in 3.1.
- "Tier" is the `gate.sh` mode that first includes a step. Every step in `fast` also runs in `screen` and `full`, and every step in `screen` also runs in `full`.
- Durations are **static estimates** worked out from boots, fixed sleeps and timeouts. Only one thing was actually run on this host: the coordinator's `bash run.sh -T` (section 8).

---

## 1. Scope

### 1.1 In scope, read completely

| File | Lines | Role |
|---|---|---|
| tools/harness.py | 825 | Shared library for every QEMU-driving Python harness. It contains `Guest` (boot plus serial), `Monitor` (HMP over TCP: mouse, keys, screendump), `Checks` (reporting and exit code), fast colour counting, `face_width`, `read_ppm` and `build_once`. |
| tools/shell_test.sh | 123 | Types 20 commands at the kernel console shell over serial and greps the transcript (fast tier). |
| pipeline/README.md | 219 | Design document for the two-agent pipeline (partly stale, see section 10). |
| pipeline/backlog.md | 65 | The markdown task table the scripts consume (`id / author / title / state`), plus a list of ideas. |
| pipeline/batch.sh | 259 | N tasks at once in git worktrees, then one full gate for the merged batch, falling back to one gate per task. |
| pipeline/cycle.sh | 223 | One task end to end in the main checkout: author, fast gate, review, answer, full gate, merge. |
| pipeline/gate.sh | 632 | **The gate**, the only arbiter. It has three modes: `fast`, `screen` and `full`. |
| pipeline/land.sh | 57 | Merges an already written branch onto a try branch, runs the full gate and fast-forwards main. |
| pipeline/lib.sh | 141 | Finds the `codex`/`claude` CLIs, runs them, provides the fake-agent seam, logging, and backlog parsing and rewriting (awk). |
| pipeline/loop.sh | 66 | Runs `cycle.sh` repeatedly with a budget (`MAX_CYCLES`) and stops after 2 consecutive failures. |
| pipeline/models.sh | 91 | `PROFILE` (`thrifty`, `balanced` (the default), `max`) selects models, reasoning effort and role overrides. |
| pipeline/parse_review.py | 78 | Pulls the first balanced JSON object out of a reviewer's reply and normalises severities. |
| pipeline/preflight.sh | 85 | Checks before a run: both agents answer, qemu, python, a clean tree, on main, `todo` tasks exist. |
| pipeline/release.sh | 67 | Builds `build/zelr.iso` and `build/launcher/zelr.exe`, checks the version against the tag, then runs `gh release create/edit/upload`. |
| pipeline/selftest.sh | 155 | Tests the pipeline itself: 6 scenarios and 17 checks of `cycle.sh`, with scripted agents and a stub gate. |
| pipeline/prompts/implement.md | 60 | The author's prompt (`{{TASK}}`). |
| pipeline/prompts/review.md | 73 | The reviewer's prompt (`{{TASK}}`, `{{REPORT}}`) and the JSON answer schema. |
| pipeline/prompts/address.md | 40 | The author's answer-the-findings prompt (`{{TASK}}`, `{{FINDINGS}}`). |
| launcher/App.xaml | 72 | WPF application resources: 7 brushes and the `Primary` and `Ghost` button styles. `StartupUri=MainWindow.xaml`. |
| launcher/App.xaml.cs | 7 | Empty `partial class App : Application`. |
| launcher/Emulator.cs | 185 | `FindQemu`, `ExtractKernel`, `DiskPath`, `EnsureDisk`, `Start` (the QEMU command line) and `InstallQemu` (winget). |
| launcher/MainWindow.xaml | 115 | The single window: status panel, "type these" command list, Start / Set up / Check again, GitHub link. |
| launcher/MainWindow.xaml.cs | 122 | Window logic: `Refresh`, `Start_Click`, `Setup_Click`, `Recheck_Click`, `Link_Click`. |
| launcher/ZelrLauncher.csproj | 36 | .NET 8 WPF, win-x64 self-contained single-file build. **Embeds `..\build\zelr.bin`** and nothing else. |
| launcher/app.manifest | 22 | `asInvoker`, Windows 10/11 supportedOS GUID, `dpiAware=true`. |

### 1.2 The other tools/ scripts

For each of these I read the header and the assertion logic. Almost all were read in full.

| File | Lines | Role |
|---|---|---|
| tools/abicheck.py | 174 | Static. Compares `include/syscall.h` with `sdk/zelr.h`: `SYS_*` numbers (63 on each side) and 5 struct pairs. |
| tools/appcheck.py | 281 | Calculator (78/4 compared against a typed 19.5 by pixel comparison), the monitor app, and the music player played from a USB stick and recorded. |
| tools/blackbox_test.sh | 110 | Boot log survives a reboot. Two boots of one disk; reads `/sys/lastboot` fenced between two echoes. |
| tools/bootcheck.py | 207 | The disk the kernel formats must boot from a plain BIOS, including repair of a disk formatted by the previous release. |
| tools/browsercheck.py | 404 | The browser against tools/webserver.py: render comparisons, links, back, chunked, CSS, PNG, SVG, JS, redirect, 404, https refusal. |
| tools/check_loader.py | 133 | Static, run by bootloader/build.sh:25. Checks the cdboot signature position, the handoff magic halves, and that the load address is the same in both loaders. |
| tools/check_sse.py | 157 | Byte scanner for SSE/MMX opcodes in ELF executable sections. **Not wired into any build script** (section 10). |
| tools/check_version.py | 81 | Static. `KERNEL_VERSION` must not be behind the newest `vN.N.N` git tag. |
| tools/clipcheck.py | 124 | Copy and paste inside the desktop terminal via sendkey ctrl-c/ctrl-v/ctrl-a, then reads `/sys/clipboard` over serial. |
| tools/crashcheck.py | 154 | Power cut in the middle of a FAT write. Six killed rounds; each must leave the file whole as either A or B. |
| tools/defaultcheck.py | 138 | Static. Kernel theme defaults against settings.c defaults. **Currently vacuous** (section 10). |
| tools/deskcheck.py | 799 | The window manager: minimise, restore, maximise, panel hide, resize, snap, wallpapers, alt-tab, wheel, resolution change, launcher, context menu, rubber band, and auto-desktop at 1920x1080. |
| tools/enscheck.py | 111 | Two notes from the ES1370 (Ensoniq), recorded with `-audiodev wav` and pitch-checked. |
| tools/fat32_test.sh | 127 | A FAT32 volume built by mkfat (one file past cluster 65535): read, write, survive a reboot. FAT16 still works. |
| tools/findcheck.py | 166 | Ctrl+F find bar over a browser page (highlight colour `#FFE58F`). |
| tools/formcheck.py | 341 | HTML forms: GET and POST contents checked on the server side, and rounded corners. |
| tools/framecheck.py | 92 | `/sys/screen` counters: a frame sends only the changed bands, and some comparisons are shared with the second CPU. |
| tools/gamecheck.py | 276 | Blackjack and poker windows, counted by white card pixels. |
| tools/gpt_test.sh | 101 | GPT from mkgpt: a good table, a bad header CRC, a bad entry CRC, and a bare disk. |
| tools/inputcheck.py | 77 | Five boots with keys or mouse moves injected during boot. The PS/2 keyboard must still type. |
| tools/iso_test.sh | 109 | `build/zelr.iso` booted 4 ways: BIOS disc, BIOS stick, UEFI disc, UEFI stick. |
| tools/libccheck.py | 145 | A standard C program built outside the tree against `sdk/libc`, put on a mkfat volume and run by name. |
| tools/livecheck.py | 154 | JS in the browser after load: click listener, setTimeout, external script, XHR. |
| tools/mountcheck.py | 124 | A USB stick carrying a mkfat FAT volume: mount, ls, cat, write, cp. Verified on the host with readfat. |
| tools/namecheck.py | 99 | Long file names: write, persistence across a reboot, delete. Verified with readfat. |
| tools/netcheck.py | 264 | The network panel on the dock and DHCP on e1000, with no NIC, and over usb-net. |
| tools/nvme_test.sh | 110 | NVMe as a disk: write, reboot, boot log recovery, GPT on NVMe. |
| tools/piccheck.py | 79 | Runs pngtest, jpegtest, svgtest, layouttest and pagetest in ring 3 (all five are also in ring3check). |
| tools/powercheck.py | 65 | `shutdown` must make QEMU exit with code 0 (ACPI S5 from the `_S5` AML object). |
| tools/progcheck.py | 98 | A program copied to /home runs by typing its name, and not-a-program files are refused. |
| tools/ring3check.py | 107 | 19 ring-3 test programs, run by their `*_PASS` markers. The JS suite must have at least 86 cases. |
| tools/sdkcheck.py | 143 | `sdk/hello.c` built outside the tree with sdk/build.sh, put on a mkfat volume and run with arguments. |
| tools/setcheck.py | 292 | `/sys/settings` table, a hand-written `dock_h`, and a settings-window toggle that rewrites `/zelr.cfg`. |
| tools/shcheck.py | 233 | The ring-3 `/bin/sh`: redirection, pipes, ^C, builtins, exit. |
| tools/shotcheck.py | 300 | The desktop comes up, the launcher opens Settings, and clicking an accent recolours the window manager chrome. |
| tools/shots.py | 436 | Not a test. Retakes the 12 `docs/*.png` screenshots used by README.md. |
| tools/smpcheck.py | 134 | `-smp 4`: programs are scheduled on more than one CPU and every AP has its own timer. |
| tools/soundcheck.py | 241 | Two notes on HD Audio, recorded and pitch-checked. It also exports the WAV helpers other harnesses import. |
| tools/tearcheck.py | 103 | `halfdrawn`: no uncommitted (red) frame is ever composited. |
| tools/termcheck.py | 150 | The ring-3 terminal driven with sendkey: typing, tab completion, history, cursor editing, verified by theme colours. |
| tools/tlscheck.py | 173 | HTTPS against 5 real internet sites (needs internet access), a non-TLS port refused, plain http. |
| tools/usbcheck.py | 331 | xHCI keyboard and mouse, a hub, hot-plug, and a USB stick sector read and write verified on the host. |
| tools/volcheck.py | 143 | Drags the dock volume slider; the loud note must be louder than the quiet one (WAV). |
| tools/webcheck.py | 170 | TCP/HTTP against the host server: large bodies, keep-alive, slow bodies, 404, pcnet, ne2k_pci, and an instant guestfwd reply. |
| tools/webserver.py | 633 | Host HTTP/1.1 server (`ThreadingHTTPServer` on 127.0.0.1:0) with all the test pages. The guest reaches it at `10.0.2.2:<port>`. |
| tools/whereis.py | 49 | Maps an address to a symbol from ELF symtab. **Only accepts a 32-bit ELF**, so it is broken on the 64-bit kernel. |
| tools/wirecheck.py | 89 | `/bin/wiretest` against the server: gzip, keep-alive (counted on the server side), cookies. |

### 1.3 Generators and utilities (other agents cover them; one line each)

| File | Lines | Role |
|---|---|---|
| tools/genface.py | 1174 | Draws the anti-aliased interface typeface from outlines and writes `include/face.h`, `userland/face.h` and `userland/facetext.h`. |
| tools/genfont.py | 556 | Hand-drawn 8x16 bitmap console font, written to `kernel/font.c` and `userland/font.h`. |
| tools/genpng.py | 170 | Builds `userland/pngdata.h` (PNG test images produced with the host's zlib). |
| tools/genjpeg.py | 112 | Builds `userland/jpegdata.h` using **Windows PowerShell + System.Drawing** as the encoder (Windows only). |
| tools/mkroots.py | 260 | Generates `kernel/roots.c` from a PEM bundle, or from **`certifi`** (third party, imported lazily at line 243) when no bundle is given. `--check` verifies the recorded offsets. |
| tools/mkfat.py | 504 | FAT16/FAT32 image builder (`[--fat32] [--at-cluster=N] OUT SIZE_KB FILE[:PATH]...`). `Fat16Builder` is imported by mkgpt. |
| tools/mkgpt.py | 180 | GPT image: protective MBR, ESP and data FAT16 partitions. Default 65536 KB. Accepts `--bad-header-crc` and `--bad-entry-crc`. |
| tools/mkiso.py | 399 | Hybrid ISO: El Torito BIOS and EFI entries, MBR, and the ESP. **Re-flattens `build/zelr.bin` on every run** (section 10). |
| tools/readfat.py | 323 | Independent host-side FAT16 reader and writer (LFN aware; refuses FAT12 and FAT32). Modes: list, print, `--put`. |
| tools/flatten.py | 82 | ELF (32- or 64-bit) to a flat image at BASE, with zero-filled gaps and BSS. Prints the entry point. |
| tools/loadaddr.py | 43 | Reads the link address from linker.ld (`. = 16M;` gives `0x1000000`). |

### 1.4 Context read outside scope, for accuracy

build.sh (64 lines), run.sh (47), zelr.bat (190), .gitignore (17), bootloader/build.sh, sdk/build.sh, sdk/libc/build.sh, kernel/shell.c (the console shell and its echo), kernel/printf.c (`kputc` mirrors output to serial), kernel/main.c (command-line words, `machine_exit`), kernel/builtin.c (/bin names), kernel/theme.c (KNOBS, `theme_init`), userland/settings.c, userland/term.c (the terminal's command table), include/face.h, include/gfx.h, kernel/wm.c, kernel/winsrv.c, README.md, and the coordinator's selftest log.

---

## 2. Big picture

zelr's quality control has five layers, and this area owns the last four.

1. **The kernel selftest.** It runs inside the kernel. `-append selftest` makes `kmain` start `selftest_task` (kernel/main.c:313-316, 618-619). It prints `N passed, M failed` and then `SELFTEST_PASS` or `SELFTEST_FAIL` (kernel/selftest.c:3602-3603). `machine_exit()` writes to QEMU's `isa-debug-exit` port 0xF4, so QEMU exits with `(code<<1)|1` (kernel/main.c:156-161, 228-231). The gate runs it 2 times in `fast` (pc/IDE and q35/AHCI) and a 3rd time in `full` (q35/NVMe).
2. **About 45 host harnesses** in tools/. Each boots QEMU and drives it through:
   - the **serial line** (stdin/stdout, typed one character every 50 ms);
   - **QEMU's HMP monitor** over TCP (`sendkey`, `mouse_move`, `mouse_button`, `screendump`, `device_add`/`device_del`, `quit`);
   - **pixel counting** on screendump PPMs;
   - **audio capture** through `-audiodev wav`;
   - **host-side re-reading of disk images** with readfat.py;
   - a **host web server** (webserver.py).
3. **pipeline/gate.sh**, which runs them in three tiers (`fast`, `screen`, `full`). Up to `PAR_MAX=4` machines run at once. Output is held and reported in a fixed order. Each failed step is re-run once alone, and the second result is the one that counts.
4. **The AI pipeline** (`cycle.sh`, `batch.sh`, `loop.sh`, `land.sh`). One agent (Claude Code or Codex CLI) writes the code, the other reviews it as JSON, the author answers the findings, and only the gate decides whether anything lands on `main`.
5. **Releasing.** `release.sh` builds the ISO and the launcher exe and uploads them with `gh`. **The launcher** (C#/WPF, the project's only non-from-scratch code) finds or installs QEMU through winget and boots the embedded flat kernel image.

### Design decisions and the reasons the comments give

- **Wait for the condition, never for a time** (harness.py:1-16). Fixed sleeps failed "the moment the gate runs four of them at once", the failures moved between runs, and they pointed at kernel code that was fine ("a whole day went into chasing three of those").
- **Free ports** (harness.py:46-53). Per-harness constant monitor ports meant overlapping runs drove each other's monitors.
- **The monitor is framed on its prompt** (harness.py:286-294). A fixed sleep after each command left replies in the socket, and every later read was one reply behind.
- **Serial is drained continuously by a thread** (harness.py:594-600). A full pipe blocks QEMU and looks exactly like a kernel hang.
- **One character every 50 ms** (harness.py:726-733, shell_test.sh:4-9). QEMU's stdio serial applies no back-pressure. Measured: a 26-byte burst reached the ISR as 4 bytes. formcheck and findcheck slow this to `gap=0.18` when the browser is running, because it spins instead of sleeping and reads the console less often (formcheck.py:94-108).
- **Pointer moves are walked** to the corner and then out in steps of at most 100 px with 50 ms gaps (harness.py:341-374). PS/2 carries 8-bit deltas and the driver discards overflow packets. The queue is short, so a burst loses steps.
- **Clicks are held** for 0.4 s (harness.py:400-408). The window manager samples the buttons once per compositing pass.
- **`click_for`/`drag_for` retry only when the screen shows the gesture had no effect** (harness.py:410-469). Many buttons are toggles, so blind retries would undo a click that worked.
- **The pointer is read off the screen** using the cursor's two colours, and only two agreeing readings count (harness.py:471-530). The screen lags the mouse.
- **Colours are counted with `bytes.find`**, i.e. in C (harness.py:94-104). The Python loop took more than 1 s per picture.
- **Geometry is derived from the kernel's own arithmetic.** Face advance widths are parsed out of `include/face.h` (harness.py:211-255). Remembered coordinates went stale every time the chrome changed.
- **Artifacts are named by pid** (harness.py:492-499; gate.sh:88-93; blackbox_test.sh:25-30). On Windows, one process deleting or rewriting a file another holds open is a permission error that masquerades as a feature failure.
- **`ZELR_PREBUILT=1`** (gate.sh:237-243; harness.py:817-825; shell_test.sh:46-49; mkiso.py:301-310). Build once, because concurrent builds rewrite the image while other QEMUs are reading it.
- **The exit status decides, not a phrase** (gate.sh:193-211). Harnesses reworded to "all 9 checks passed" were once called failures.
- **The build must pass or nothing runs** (gate.sh:222-235). Otherwise the tests would measure the previous binary.
- **A fresh disk every time** (gate.sh:280-281).
- **q35 selftest** (gate.sh:305-315). It found an AHCI 8-sector split bug and a missing UEFI ACPI pointer, both invisible on `pc`.
- **`PAR_MAX=4`** (gate.sh:105-116). Thirteen machines at once starved the real-time checks.
- **One retry, alone** (gate.sh:155-172). Measured: 1-2 random failures per run, never the same check, each passing alone. "This forgives a busy host and nothing else."
- **A global lock** (gate.sh:41-56). Two gates at once "get half as much done twice".
- **A VM warning** (gate.sh:58-65). An open VM made screen harnesses miss their timing.
- **The pipeline** (pipeline/README.md:12-25):
  - One model writes and the other reviews. "Every serious bug found here so far was found by someone other than the author, or by a test failing."
  - The gate is the only judge (gate.sh:2-7).
  - There is an explicit tool allowlist that includes Bash (README.md:165-179, lib.sh:86-92). An author without Bash wrote code it could never build.
  - Worktrees let batch tasks run concurrently (batch.sh:10-20).
  - `PUSH` is off by default because landing locally is reversible and pushing is not (README.md:74-76; cycle.sh:26).
- **Release** (release.sh:1-15). For 15 releases neither the exe nor the ISO was attached. The tag and the compiled version must agree.
- **Launcher** (csproj:14-33). It is a single-file, self-contained WPF app with native libraries extracted. Without extraction, WPF throws `DllNotFoundException` before any code runs. It carries the flat image because multiboot loaders do not accept a 64-bit ELF.

---

## 3. File-by-file detail

### 3.1 tools/harness.py: the harness API

37 files import it, with 38 import statements (gamecheck imports twice). That is all 36 `*check.py` files except check_loader, check_sse and check_version, plus shots.py. abicheck and defaultcheck import only `Checks` and `ROOT`; the other 35 boot a `Guest`.

**Module constants and helpers**

| Name | Where | Meaning |
|---|---|---|
| `ROOT`, `BUILD` | 34-35 | The repo root (the parent of tools/) and `ROOT/build`. |
| `qemu_path()` | 38-43 | Returns `$QEMU`, else `"C:/Program Files/qemu/qemu-system-x86_64.exe"`. If that path does not exist it falls back to `shutil.which("qemu-system-x86_64")`, so a QEMU on PATH is found. |
| `free_port()` | 46-58 | Binds `127.0.0.1:0`, reads the port and closes the socket. There is a TOCTOU window until QEMU binds the port. |
| `class Timeout(Exception)` | 61-62 | Raised by `wait_boot` and the monitor. |
| `PROMPT` | 67 | `re.compile(r"zelr(?::\S*)?> ")`. Matches `zelr> ` at the root and `zelr:/home> ` elsewhere. The shell starts in /home. |
| `MOUSE_GAP` | 71 | 0.05 s between pointer steps. |
| `CURSOR_FILL`, `CURSOR_EDGE` | 77-78 | `(0xF4,0xF7,0xF9)` and `(0x08,0x0C,0x10)`. These are the same colours as the WM cursor glyph (kernel/wm.c:2890). |
| `CURSOR_FILL_AT` | 79 | `(1,2)`: offset of the first fill pixel from the hot spot. |
| `CURSOR_FILL_MIN`, `CURSOR_EDGE_MIN` | 84-85 | 20 and 10: pixel counts needed inside the 12x19 box to accept a match as the cursor. |
| `MOUSE_STEP` | 91 | 100: the largest `mouse_move` delta per step. |
| `_count(buf, want, base=0)` | 106-116 | Counts occurrences of `want` found by `bytes.find` at 3-byte-aligned offsets. |
| `count_all(px, rgb)` | 119-121 | Exact-colour count over the whole frame. |
| `count_in(px, w, rect, rgb)` | 124-132 | Exact count in `rect=(left, top, right, bottom)`, half-open, row by row. |
| `count_near(px, w, rect, rgb, tol=8)` | 135-155 | Pure-Python loop: per-channel `abs(delta) <= tol`. Used for translucent (alpha 248-250) panels. |
| `row_mean(px, w, y, l, r)` | 158-168 | Mean RGB of one row segment. |
| `colour_gap(a, b)` | 171-173 | L1 distance between two colours. |
| `centre_of(px, w, h, rgb, min_pixels=200, within=None)` | 176-208 | Centroid of the exact-colour pixels, or `None` if fewer than `min_pixels`. `h` is unused. |
| `FACE_BODY..FACE_HEAD_BOLD` | 222 | 1..5, matching include/gfx.h:82-87. 0 (`FACE_SMALL`, 13 px) is not named here. |
| `face_width(text, which=FACE_BODY)` | 227-255 | Lazily parses include/face.h tables in `face_faces` order: `face_g_13, _15, _20, _26, _15b, _20b, _15m, _15bm`. It takes field 4 (`advance`) of each `face_glyph {w,h,left,top,advance,at}` and maps characters outside 32..126 to a space. Used by deskcheck, netcheck and volcheck for dock and clock geometry. |
| `read_ppm(path)` | 258-281 | P6 only, skips `#` comments, returns `(w, h, px)`. Raises `ValueError("the picture is N bytes short")` if the file is truncated. |

**`class Monitor`** (286-589) wraps QEMU HMP on `tcp:127.0.0.1:<port>`.

- `__init__(port, timeout=180)`
  - Retries the connect for 20 s (sleeping 0.2 s between tries), then re-raises `OSError`.
  - Sets the socket timeout.
  - Eats the banner with `_to_prompt()`.
- `_to_prompt(timeout=180)` (314-328)
  - Reads until `b"(qemu)"`.
  - On a socket timeout it raises `Timeout("the monitor stopped answering")`; on EOF, `Timeout("the monitor closed")`.
  - Returns the text before the prompt and keeps the remainder buffered.
- `send(command, settle=0.0)` (330-338): `sendall(command+"\n")`, wait for the prompt, then an optional sleep for the guest to act.
- `move_to(x, y)` (341-374)
  - 12 × `mouse_move -200 -200` (settle 0.05) to pin the pointer at (0,0).
  - Then steps of `mouse_move dx dy`, each with dx and dy ≤ 100, until the accumulated position equals (x, y); then sleeps 0.2.
  - Accepts non-negative absolute targets only.
- `click(x, y)` (376-408)
  - `move_to`, then up to 2 re-walks when `pointer()` is more than 3 px off.
  - Then `mouse_button 1` (settle 0.4) and `mouse_button 0` (settle 0.5).
- `click_for(x, y, name, want, timeout=20.0, tries=3)` (410-437)
  - Loops: click, then `wait_screen(name, want, timeout/tries)`.
  - Returns the last `(w,h,px,ppm,ok)`.
  - After all tries fail, prints the pointer position.
- `drag_for(frm, to, name, want, timeout=24.0, tries=3)` (439-469): the same pattern with `drag`.
- `pointer(tries=8)` (471-490): calls `_pointer_once` every 0.3 s until two consecutive readings agree, and returns the last reading or `None`.
- `_pointer_once()` (492-530)
  - Takes `screen("pointer-<pid>")` and deletes the file in a `finally`.
  - Finds each 3-aligned `CURSOR_FILL` pixel and checks the 12x19 box at (x-1, y-2) for at least 20 fill and 10 edge pixels.
  - Returns `(x, y)`, or `None` on `RuntimeError`/`ValueError` (i.e. before any desktop exists).
- `drag(frm, to, steps=8)` (532-550)
  - `move_to(frm)`, then `mouse_button 1` (0.3), then 8 **relative** moves (settle 0.08 each), then `mouse_button 0` (0.5).
  - It cannot use `move_to` mid-drag, because walking to the corner with the button held would drag the window there.
- `screen(name)` (553-568)
  - Deletes any old `build/<name>.ppm` and sends `screendump <path with forward slashes>`.
  - Relies on the monitor prompt returning only after the dump is written.
  - Raises `RuntimeError("no screenshot from the monitor")` if the file is missing.
  - Returns `(w,h,px,ppm)`.
- `wait_screen(name, want, timeout=30.0, interval=0.25)` (570-583)
  - Screenshots until `want(w,h,px)` is true or time runs out.
  - Returns `(w,h,px,ppm,ok)`, so a failing check still has its picture.
- `close()`.

**`class Guest`** (594-770) is one booted machine.

`__init__(disk, size_mb=32, memory=64, machine=None, kernel="zelr.bin", args=None, extra=None, keep=False, reuse=False)`:
- Unless `reuse` is set, (re)creates `disk` as a zero file of `size_mb` MiB.
- `keep` leaves the disk behind at `stop()`. It is a separate flag from `reuse` (see 604-610).
- The command line it builds (618-636) is:
  ```
  <qemu_path()> [-machine M] -kernel build/<kernel> -m <memory> -no-reboot -display none
    -serial stdio -drive file=<disk>,format=raw,if=ide,index=0
    -monitor tcp:127.0.0.1:<free_port>,server,nowait
    [-append console | -append <args> | (nothing when args=="")] [extra...]
  ```
- `args=None` means `-append console`. `console` is the kernel command-line word that suppresses the auto-desktop (kernel/main.c:315, kernel/shell.c:598-608). `args=""` means no command line at all, i.e. the "booted from a disc" path.
- On `q35`, an `if=ide` drive lands on the ICH9 AHCI controller.
- Launches `Popen(cwd=ROOT, stdin=PIPE, stdout=PIPE, stderr=STDOUT)` and a daemon `_drain` thread that does `read(1)` into `_chunks` under `_lock`.

Methods:

| Method | Lines | Behaviour |
|---|---|---|
| `serial()` | 656-658 | The whole transcript since start, including QEMU's own stderr warnings. |
| `alive()` | 660-661 | Whether the QEMU process is still running. |
| `wait_serial(text, timeout=60.0)` | 663-675 | Polls every 0.1 s and returns early if QEMU died. |
| `prompts()` | 677-683 | Number of `PROMPT` matches in the whole transcript. |
| `wait_prompt(count=1, timeout=60.0)` | 685-693 | Waits until the **total** prompt count is at least `count`. Calling it with the default after boot is a no-op; see the pitfalls in section 9. |
| `wait_boot(timeout=120.0)` | 695-702 | Waits for the first prompt, else raises `Timeout` with `tail(25)`. |
| `run(line, timeout=30.0)` | 704-710 | Types the line and waits for `prompts()+1`. Returns the **whole** transcript and does not raise on timeout. |
| `fresh(line, timeout=30.0)` | 712-724 | Only the transcript produced since the call. This still includes the echo of the typed line (kernel/shell.c:626-628 → kputc → serial, kernel/printf.c:16-24). |
| `type(text, gap=0.05)` | 726-737 | Writes one character at a time, flushing after each. |
| `monitor()` | 739-742 | A lazy `Monitor(self.port)`. |
| `tail(lines=25)` | 744-745 | The last lines of the transcript. |
| `stop()` | 747-770 | Closes the monitor and stdin, then `terminate`, wait 5 s, `kill`, wait 5 s. Deletes the disk unless `keep`. |

**`class Checks`** (775-814):
- `add(name, passed, shot=None)` returns a bool.
- `report(keep=False, note=None)`
  - Prints `=== title ===` and then `  PASS  name` / `  FAIL  name` lines.
  - On failure it prints `title: N failed` and `screenshots: ...`, and keeps the shots.
  - Otherwise it prints `title: all N checks passed` and deletes the shots unless `keep`.
  - Returns 1 or 0, and the scripts pass that to `sys.exit`. setcheck.py:279-288 and piccheck.py:66-75 record that this was once used as a boolean by mistake.

**`build_once()`** (817-825) returns immediately when `ZELR_PREBUILT == "1"`. Otherwise it runs `bash build.sh` with `check=True` and stdout discarded.

**What the harness does not provide:**
- There is **no sendkey helper**. Each harness carries its own `NAMED` map from characters to QEMU key names:
  - termcheck.py:44-47 and clipcheck.py:28-31: `" "→spc, "\n"→ret, "\t"→tab, "/"→slash, "."→dot, "-"→minus, "_"→shift-minus, ","→comma`
  - deskcheck.py:241: `spc ret slash dot`
  - usbcheck.py:70: `spc ret slash dot minus`
  - inputcheck.py:25: `spc ret`
- Letters and digits are sent as themselves. Upper-case letters would need `shift-x` and nothing handles that.
- Chords used across the harnesses: `alt-q`, `alt-d`, `alt-left`, `alt-right`, `alt-tab`, `alt-up`, `ctrl-c`, `ctrl-v`, `ctrl-a`, `esc`, `backspace`, `delete`, `home`, `left`, `up`, `tab`.
- The mouse wheel is `mouse_move 0 0 ±1` (deskcheck.py:544-552). The right button is `mouse_button 2` (deskcheck.py:218-223).

### 3.2 pipeline/gate.sh: helpers and mechanics

The ordered step lists are in section 4.1. Setup and helpers:

- **Setup** (30-39): `set -uo pipefail` (no `-e`), then `cd` to the directory above the script. `MODE` must be one of `fast|screen|full`; anything else exits 2. `QEMU` defaults to `/c/Program Files/qemu/qemu-system-x86_64.exe` and falls back to `command -v qemu-system-x86_64`.
- **Lock** (49-56): `LOCK=${TMPDIR:-/tmp}/zelr-gate.lock` is created with `mkdir`, which is atomic. If it already exists the gate prints the holder's pid and **exits 2**. The pid is written to `$LOCK/pid` and `trap 'rm -rf "$LOCK"' EXIT INT TERM` removes it.
- **VM warning** (61-65): `ps -W | grep -ciE 'qemu-system|vmware-vmx|VirtualBox'`. `ps -W` is MSYS/Cygwin-only. This only prints a note.
- **`report name rc [secs]`** (69-74): prints `  PASS|FAIL  name  secs` and increments `failures` on a non-zero rc.
- **`run_step name cmd...`** (76-84): runs the command synchronously, captures its output, prints the **last 3 lines** indented, then reports. Used for the three static checks.
- **`par_start name fn`** (118-129)
  - Creates `PARALLEL_DIR=$(mktemp -d)` if needed.
  - Blocks while `jobs -rp | wc -l` is at least `PAR_MAX` (default 4), checking every second.
  - Starts `( fn > stepN.txt 2>&1; echo rc=$?; echo secs=... ) &`.
  - Records the name, file, pid and `"$*"` (the function **name**, kept for the retry).
- **`par_wait`** (131-191)
  - Waits for all jobs, then reports them **in start order**.
  - A passing step prints its last 3 lines and PASS with its duration.
  - A failing step prints its last 12 lines and `....  <name> (busy host; again, alone)` **without** counting a failure.
  - Then each failed step is re-run **serially, alone**, as `${again_cmds[$i]}` (an unquoted function name). The result is reported as `<name>, alone`, and **only this second result counts**.
  - Finally clears the arrays and removes `PARALLEL_DIR`.
- **`keep cmd...`** (212-218): captures the output. On rc 0 it prints nothing. Otherwise it prints the output minus the `PASS` lines, `tail -10`, and returns 1. Every harness step is wrapped as `keep timeout <N> <harness>`.
- **Build** (227-235)
  - Runs `build_out="$(bash build.sh 2>&1)"`.
  - **Failure is decided by `grep -qE '\berror\b'`, not by the exit status.** On a match it prints up to 5 error lines, `FAIL it builds`, then `gate (MODE): it does not build, so nothing else was run`, and **exits 1**.
  - Warnings are counted with `grep -c 'warning:'` and printed as `(N build warnings)` (247-248).
  - Then `export ZELR_PREBUILT=1` (243).
- **Tidy** (614-623)
  - `stale PATTERN` deletes top-level files matching the pattern that are **not newer than the gate's start time** (`find . -maxdepth 1 -name P ! -newermt "@$started_at" -delete`, GNU find).
  - Explicitly removes `gate.img gateq.img gatenv.img deskcheck.img termcheck.img shotcheck.img sel.img blackbox.img` and `build/*.ppm`.
  - The stale patterns are: `gpttest.*.img fat32test.*.img nvmetest.*.img clipcheck.*.img shotcheck.*.img termcheck.*.img deskcheck.*.img usbcheck.*.img fat32probe.*.txt fat32high.*.txt`.
- **Summary** (625-632): `gate (MODE): everything passed in Xm Ys` and exit 0, or `gate (MODE): N failed, after Xm Ys` and exit 1.

The kernel selftest step functions:

| Function | Lines | QEMU command | Disk |
|---|---|---|---|
| `selftest` | 282-294 | `timeout 300 $QEMU -kernel build/zelr.bin -m 256 -no-reboot -display none -serial stdio -append selftest -drive file=gate.img,format=raw,if=ide,index=0 -device isa-debug-exit,iobase=0xf4,iosize=0x04` | fresh 32 MiB `gate.img` |
| `selftest_q35` | 316-329 | `-machine q35` … `-drive file=gateq.img,format=raw,if=none,id=d0 -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0` + debug-exit | fresh 32 MiB `gateq.img` |
| `selftest_nvme` | 378-390 | `-machine q35` … `-drive file=gatenv.img,format=raw,if=none,id=nv0 -device nvme,drive=nv0,serial=zelr0001` + debug-exit | fresh 64 MiB `gatenv.img` |

Each selftest step prints `grep -E 'FAIL|passed,' | tail -3` and **passes iff `SELFTEST_PASS` appears** in the output. The QEMU exit code is not used. None of them passes `-smp` or `-nic`, so QEMU's default user-mode NIC is present: `e1000` on pc and `e1000e` on q35. `kernel/e1000.c:25` supports device 0x10D3, which is e1000e.

### 3.3 Bash harnesses

All of them:
- `cd` to the repo root;
- resolve `QEMU` from `$QEMU`, else `command -v qemu-system-x86_64`, else `/c/Program Files/qemu/qemu-system-x86_64.exe`;
- type through a pipe into `-serial stdio` with `type_line`: one character every 50 ms, then 0.45-0.55 s after each line;
- stop the machine with a typed `reboot` plus `-no-reboot`, instead of waiting for `timeout`. It is a reboot rather than a poweroff, so nothing is flushed on the way out.
- decide with `grep -qF` over the transcript, and exit with the failure count (0 means pass).

**tools/shell_test.sh**

| Aspect | Detail |
|---|---|
| Version | Taken from `include/types.h` (15). |
| Timeout helper | `run_with_timeout` (21-44) uses `timeout`, else `gtimeout`, else a `python3` shim that exits 124 on timeout. |
| Build | Only when `ZELR_PREBUILT` is not 1 (49). |
| QEMU | `-kernel build/zelr.bin -m 64 -no-reboot -display none -serial stdio -append console`, under a 90 s timeout. **There is no disk**, so files live in the in-memory filesystem (94). |
| Input | `feed()` (63-92) sleeps 2.5 s, then types `uname, ls, cat /doc/readme, write notes.txt shell wrote this, cat notes.txt, rm notes.txt, cat notes.txt, mkdir docs, cd docs, pwd, write inner.txt nested file, cd /, cat docs/inner.txt, ls docs, ps, mem, spawn, exec /bin/hello, exec /bin/wintest, echo done testing, reboot`. |
| Checks | 17 checks (103-119). These include `zelr <VERSION> x86_64`, `no such file`, `/home/docs` (the prompt carries the cwd), `PID`, `running `, `physical:`, `spawned pid`, `hello from a program`, `wintest: surface at 0x0000008060000000` (`WINSRV_SURFACE_BASE` = `USER_SPACE_BASE + 0x60000000`, include/winsrv.h:19), `wintest: wrote and read back 3072 pixels`, `closed, handle is dead`, `wintest: ok` and `done testing`. |
| Output | Prints `shell test: all checks passed` or `N failed`, the transcript path, and exits with `$fails`. |
| Weakness | 4 of the 17 checks cannot fail. See section 10 (N). |

**tools/blackbox_test.sh**

| Aspect | Detail |
|---|---|
| Disk | `blackbox.$$.img`, 32 MiB of zeros, removed by a trap. |
| QEMU | `timeout 90 … -m 128 -append console -drive …if=ide`. |
| Boot 1 | `sleep 6; echo first boot done; reboot`. |
| Boot 2 | `echo BEGIN_RECORD; cat /sys/lastboot; echo END_RECORD; reboot`. The record is cut out with `sed -n '/BEGIN_RECORD/,/END_RECORD/p'`. |
| Checks | 11. Boot 1: `== handing over to the scheduler`, `first boot done`, `fs new disk prepared`. Fenced region non-empty. Fenced region: not `no record`; contains `== gdt`, `== disk`, `== handing over…`, `boot log`, `fs new disk prepared` (it must be the earlier boot's log); and not `fs fat16 mounted`. |
| Why fenced | Every log line is mirrored to serial, so an unfenced check "passed with the record-keeping entirely broken (4 of 7)" (11-17). |
| Build | Does **not** build. When run alone it tests whatever `build/zelr.bin` is already there. |

**tools/gpt_test.sh**

| Aspect | Detail |
|---|---|
| Disk | `gpttest.$$.img`. |
| Boots | 4, each under `timeout 90`, `-m 256 -append console`, IDE. Input: `sleep 6; cat /HELLO.TXT; reboot`. |
| (a) Good GPT | `mkgpt.py IMG` (64 MiB). Checks: `parts gpt, 2 partition(s)`, `efi system partition`, not `mounted partition 1`, `fs mounted partition 2`, `read from a gpt partition`. |
| (b) Bad header CRC | `--bad-header-crc`. Checks: `gpt rejected: header checksum`, and neither `fs mounted partition` nor the file. |
| (c) Bad entry CRC | `--bad-entry-crc`. Checks: `gpt rejected: entry array checksum`, and no mount. |
| (d) Bare disk | 32 MiB of zeros. Checks: `parts none` and `new disk prepared`. |
| Total | 12 checks. Does not build. |

**tools/fat32_test.sh**

| Aspect | Detail |
|---|---|
| Image | `mkfat.py --fat32 IMG 40960 SRC:HELLO32.TXT SRC:SUB/INNER.TXT --at-cluster=70000 HIGH:HIGH.TXT`. The file past cluster 65535 catches a reader that ignores the high 16 bits (66-76). |
| Boots | 3, each under `timeout 120`, `-m 256 -append console`, IDE. |
| Boot 1 | Reads the three files, then writes `made32.txt` and `newdir/deep.txt`. 5 checks: `fat32 mounted`, not `fat16 mounted`, both read strings, not `reclaimed`. |
| Boot 2 | Reads everything back. 6 checks, including not `no such file`. |
| Boot 3 | A FAT16 image built with `mkfat.py IMG 32768`. 3 checks. |
| Total | 14 checks. Temporary files are `fat32test.$$.img`, `fat32probe.$$.txt` and `fat32high.$$.txt`. Does not build. |

**tools/nvme_test.sh**

| Aspect | Detail |
|---|---|
| QEMU | `timeout 150 … -machine q35 … -drive file=IMG,format=raw,if=none,id=nv0 -device nvme,drive=nv0,serial=zelr0001`. |
| Boot 1 | 64 MiB of zeros. Checks: `via nvme`, not `disk    none`, `written through nvme`. |
| Boot 2 | Checks the files survived, not `no such file`, and `== handing over to the scheduler` in `/sys/lastboot`. The boot log is written in one request larger than the NVMe per-command limit, so this exercises the block layer's request splitting. |
| Boot 3 | A mkgpt 64 MiB image on NVMe. Checks: `via nvme`, `parts gpt`, `efi system partition`, not `mounted partition 1`, `read from a gpt partition`. |
| Total | 12 checks. |

**tools/iso_test.sh**

| Aspect | Detail |
|---|---|
| Version | From include/types.h. |
| Firmware | `FW=${ZELR_UEFI_FW:-/c/Program Files/qemu/share/edk2-x86_64-code.fd}`. |
| Build | Runs `python tools/mkiso.py` first, writing `build/zelr.iso`. |
| Disk | `build/isotest.img`, 32 MiB, a fixed name, recreated for each path. |
| Input | `sleep 9`, send ESC (a disc boot has no command line, so the desktop is up), `write booted.txt <what>`, `cat booted.txt`, `reboot`. |
| QEMU | `timeout 90 $QEMU <args> -m 256 -no-reboot -display none -serial stdio`, on the default `pc` machine. |
| Paths | BIOS disc: `-cdrom ISO -boot d -drive file=DISK,if=ide,index=1`. BIOS stick: `-drive file=ISO,format=raw,if=ide,index=0 -boot c` plus DISK at index 1. UEFI disc: `-drive if=pflash,format=raw,readonly=on,file=$FW` plus the cdrom. UEFI stick: pflash plus the ISO as IDE index 0. |
| Checks | 5 per path: `zelr $VERSION`, `long mode`, `progs`, `zelr:/home>` and `$what`. |
| Missing firmware | The UEFI paths are **skipped**, not failed. It prints `SKIP`, then `boot test: the bios paths passed, uefi skipped`, and exits 0. |

### 3.4 Python harness notes beyond the catalogue in section 8

- **ring3check.py**
  - `SUITES` (30-50) is a list of (program, marker, label, timeout). The programs are `fptest 60, alloctest 90, jstest 180, forktest 120, fdtest 120, pagetest 120, pngtest 120, jpegtest 180, svgtest 180, layouttest 120, cardtest 120, sleeptest 120, cowtest 180, argvtest 120, sigtest 180, faulttest 240, maptest 180, durtest 180, polltest 180`.
  - Each suite gets two checks: "runs at all" (the `_PASS` or `_FAIL` marker appears) and "every one of its checks passed" (the `_PASS` marker).
  - Afterwards `jstest` runs again, and the last `" of "`+`"passed"` line must report a total of at least 86 cases.
  - All run on one `G(pc,256)`. The disk is `ring3.<pid>.img`.
- **shotcheck.py**
  - Geometry for a 1024x768 screen: dock height 44, gap 14, side 16. The launcher has two columns: `MENU_RAIL 132`, `MENU_PANE 152`, `MENU_ITEM_H 32`, `MENU_PAD 10`.
  - The menu kinds are in `MENU_KINDS` and `MENU_IN` (73-82). `menu_top()` finds the menu edge by colour `(0xF3,0xF3,0xF6)` inside `MENU_RECT`.
  - It waits for the terminal's `SLATE` page (more than 100000 px in `PAGE`), clicks the dock badge, moves onto the "System" kind and clicks "Settings". That gesture is retried up to 3 times as a whole.
  - It waits for all 6 accent swatches, then clicks the teal swatch found by `centre_of`. Pass means teal pixels rose by more than 600 (`SWING`) **and** indigo pixels fell by more than 600.
  - It imports `count_all` but never calls it.
- **termcheck.py**: every step ends in a colour (`theme paper/amber/phosphor` repaint the terminal). The sequence is: tab completion (`them` + Tab + `amber`), history (up, up, ret), left-arrow insert (`theme phosphr`), backspace, home+delete. The final `cat /cfg/term` step only proves the program survived (section 10).
- **deskcheck.py**
  - Constants are written the way wm.c computes them: `WM_BORDER 1`, `WM_TITLE_H 32`, `ICON_LEFT 18`, `ICON_CELL_W 92`. The first window is at `WIN_X = 18+92+14 = 124`, `WIN_Y = 36`, with content 760x480. Title buttons are 30x24 with a gap of 2.
  - Taskbar chips are sized with `face_width(title)+22` capped at 160, with a 6 px gap.
  - `panel_showing()` compares the rows inside and above the panel (`colour_gap > 60`).
  - Wallpaper checks compare `desktop_bytes` (every third row). Stars must differ between two frames; weave must differ from the rest.
  - It changes the resolution to 800x600 and back with `write /zelr.cfg width …` plus `append … height …`.
  - The last part boots a second machine, `deskauto.<pid>.img`, with `memory=512, args=""`. It checks the desktop comes up on its own, that ESC reaches the shell, and that 1920x1080 works. 37 checks in total.
- **usbcheck.py**: its `reports()` parser reads the `usb …` line of `/sys/devices`. The hot-plug part uses `device_add usb-kbd,bus=xhci.0,id=latecomer` and `device_del latecomer`. The stick is 8 MiB with pattern `DEADBEEF×4` at LBA 100; the test writes 0xAB to LBA 200 and verifies it on the host.
- **soundcheck.py** also exports the audio helpers other harnesses import:
  - `read_wav` (35-78) is a hand parser that tolerates the zero length fields QEMU leaves when it did not exit cleanly.
  - `loud_runs(samples, rate, floor=2000, min_ms=120)` works in 5 ms blocks.
  - `frequency_of` counts zero crossings with hysteresis over the middle 3/4 of the note.
  - `close_to(tol=0.06)`, `LOW_HZ 440`, `HIGH_HZ 880`, `NOTE_MS 600`.
  - enscheck, volcheck and appcheck import these. It ends QEMU with monitor `quit` so the WAV header gets written.
- **webserver.py**
  - `HOST_IP="10.0.2.2"`, i.e. slirp's gateway, which is the host's loopback. `INSTANT_IP="10.0.2.100"`.
  - `Server` runs `ThreadingHTTPServer(("127.0.0.1",0), Handler)` in a daemon thread and is used as a context manager.
  - Methods: `host` (`10.0.2.2:<port>`), `qemu_args(model="e1000")` (`-nic user,model=…`) and `instant_args()`. The latter uses `guestfwd=tcp:10.0.2.100:80-tcp:127.0.0.1:<port>`, which is only usable for one connection.
  - Counters and records: `counts()` (connections and requests), `cookies()`, `received()` (form submissions), `reset_counts()`, `forget()`.
  - Routes: `/`, `/gz` (gzip only if the client asked for it), `/one`, `/two`, `/setcookie`, `/whoami`, `/bye`, `/form`, `/posts`, `/said` (GET/POST echo), `/second`, `/styled`, `/bare`, `/logo.svg`, `/drawn`, `/logo.png` (160x90 rose `(225,29,72)`), `/picture`, `/missing-picture`, `/scripted`, `/unscripted`, `/live`, `/live.js`, `/live.txt` ("teal"), `/live-quiet`, `/style.css`, `/big` (200000), `/size/N`, `/framed` (chunked `SAMPLE` in 97-byte pieces), `/measured`, `/chunked`, `/close`, `/redirect`, `/redirect-relative`, `/loop`, `/slow` (4 × 500 bytes, 0.4 s apart), and 404 for anything else.
- **bootcheck.py**
  - Formats a 64 MiB disk through `G(pc,256)`: `ls /`, `write /keepme.txt this-file-must-survive`, `sync`.
  - Then checks sector 0: `55 AA`, a `FAT16`/`FAT32` label, the jump `EB 3C 90` (FAT16) or `EB 58 90` (FAT32), and a non-zero byte at the jump target.
  - `bare_bios()` runs `qemu -drive file=DISK,format=raw,if=ide,index=0 -m 256 -display none -monitor stdio` with **no -kernel**. It sleeps 10 s, screendumps to `build/bootcheck.ppm` (a fixed name), and quits.
  - `lit_band(118,150)` must find more than 150 pixels with R>40, i.e. the boot sector's message.
  - Then it re-breaks the sector (old jump, zeroed code), boots the kernel with `reuse`, and checks that the sector was repaired and the file kept, then runs the BIOS test again. 11 checks.
- **crashcheck.py**
  - Rounds: 6. The argument must be spelled `--rounds=N` (57-59).
  - `/bin/crashwrite A` first. `crashwrite check` prints one of `CRASH_WHOLE A`, `CRASH_WHOLE B`, `CRASH_TORN`, `CRASH_SHORT`, `CRASH_MISSING` or `CRASH_WRONG`.
  - Each round waits for `CRASH_TURN`, sleeps `random.uniform(0.15, 2.5)`, then `vm.stop()` (terminate, i.e. a power cut), reboots and asks.
  - Total boots: 2 + 2×6 + 1 = 15.
- **sdkcheck.py / libccheck.py**
  - They copy `sdk/{zelr.h,zelr.ld,build.sh,hello.c}` (sdkcheck) or the whole of `sdk/` (libccheck) into a `tempfile.mkdtemp`.
  - Then run `bash build.sh hello.c outside` (or `foreign` in `sdk/libc`). **This needs zig on PATH or `$ZIG`.** sdk/build.sh has no WinGet fallback.
  - Then `mkfat.py IMAGE 32768 prog:home/<name>`, boot with `reuse=True`, and type the program name with arguments.
- **setcheck.py**: reads the `/sys/settings` table (at least 25 rows of `key value lo hi def …`) and requires keys such as `dock_h, dock_clock, title_h, border, icon_size, wallpaper, anim_ms, volume`. It writes `dock_h 96` and back again. It opens Settings by typing `sett` into the launcher, toggles `dock_brand` (row index taken from the table order), and checks `/zelr.cfg` contains `dock_brand 1`, `dock_h 44`, `wallpaper 11` and `anim_ms 120`.
- **netcheck.py**: dock geometry uses `CLOCK_W = face_width("12:54", FACE_HEAD)`, `VOL_W 30`, `NET_W 26`. The network popover is 268x150, with its button at `(POP_X+134, POP_Y+123)`. It expects the address `10.0.2.15` and the router `10.0.2.2`.
- **tlscheck.py**: sites are `www.google.com` (marker `<title>Google</title>`), `example.com` (`Example Domain`), `wikipedia.org`, `www.bbc.co.uk` and `archive.org`. Names are resolved up to 4 times, and a fetch is retried once only if no status line came back. Handshake success is the string `TLS 1.3, x25519, aes-128-gcm`. It also fetches `https://example.com:80/` (must be refused with `tls:`) and `http://example.com/` (a status and no `TLS 1.3`).

### 3.5 Static checkers

| Script | Run by | What it checks |
|---|---|---|
| check_version.py | gate, every mode | Parses `#define KERNEL_VERSION "x"` and `git tag --sort=-v:refname`, taking the first `v\d+(\.\d+)*`. Being behind is exit 1; no tags is a pass; being ahead is a pass. It knows it reads only local tags (16-20). |
| abicheck.py | gate, every mode | `SYS_*` names on both sides, equal values, no duplicate numbers on the kernel side. For the 5 `PAIRS` (`zelr_task_t/zelr_task`, `zelr_stat_t/zelr_stat`, `zelr_sysinfo_t/zelr_sysinfo`, `zelr_netinfo_t/zelr_netinfo`, `sound_info_t/zelr_sound`): the same field count, types after the `SAME` mapping (`unsigned int→u32`, `unsigned long long→u64`, `int→i32`) and array sizes. Field names are not compared. |
| defaultcheck.py | gate, every mode | The kernel's `theme_init` assignments against settings.c `static int k = N;` initialisers and the keys `save()` writes via `put_kv`. Now vacuous (section 10). |
| check_loader.py | bootloader/build.sh:25 | Signature `ZLR1` at an offset of at least 512 and within 2048 bytes. `HANDOFF_MAGIC` (include/handoff.h) equals the two `movl` halves in cdboot.S. `KERNEL_PHYS` (uefi/loader.c) and cdboot `patch_load` equal `loadaddr.load_address()`. |
| check_sse.py | nobody | `66 0F {6E,7E,D6,6F,7F,28,29}` always counts. Bare `0F {10,11,28,29,58,59,6E,7E}` is filtered when it looks like an address immediate, a RIP displacement or a rel32. |

### 3.6 The pipeline scripts

**lib.sh**
- `set -uo pipefail`. `ROOT` is the absolute repo root, `PIPE=$ROOT/pipeline`, `STATE=$PIPE/state` (created with `mkdir -p`; gitignored), `BACKLOG=$PIPE/backlog.md`.
- `find_codex` (22-27):
  - `command -v codex`;
  - else `find $LOCALAPPDATA_UNIX/OpenAI/Codex/bin -name codex.exe` (the directory name contains a build hash that changes with each update);
  - else `/c/Users/$USER/AppData/Local/OpenAI/Codex/bin`.
  - `LOCALAPPDATA` is converted from `C:\…` to `/c/…` with sed (29-30).
- `CODEX=${CODEX:-$(find_codex)}`. `CLAUDE=${CLAUDE:-$(command -v claude || echo $HOME/.local/bin/claude)}`.
- `log`, `warn` and `die` tee to `${CYCLE_LOG:-/dev/null}`.
- `fake_agent role out` (53-63): copies `$FAKE_AGENTS/<role>.txt` to `out`, then runs `<role>.sh` in `$ROOT` if the file exists. It tests with `-f`, not `-x`, because the exec bit does not survive on Windows filesystems.
- `run_codex prompt out [args]` (66-78):
  ```
  timeout ${AGENT_TIMEOUT:-3600} "$CODEX" exec --cd "$ROOT" --sandbox workspace-write --skip-git-repo-check -o "$out" [args] - < prompt
  ```
- `run_claude prompt out [args]` (81-99):
  ```
  (cd "$ROOT" && timeout ${AGENT_TIMEOUT:-3600} "$CLAUDE" -p --permission-mode acceptEdits --allowedTools "Bash Read Write Edit Glob Grep" --add-dir "$ROOT" [args] < prompt) > "$out" 2>&1
  ```
  The prompt goes on stdin because `--allowedTools` takes a list and would swallow a positional prompt.
- `backlog_next` (107-114): awk over lines matching `^\| *[a-z0-9-]+ *\|`, with fields 2-5 trimmed. Prints `id|author|title` of the first `todo`. The header and separator rows also match the regex but never have state `todo`.
- `backlog_set id state` (121-137): rebuilds the matching row as `| id | author | title | state |` through a `mktemp` file and `mv`. Titles containing `|` would break this.
- `backlog_count_todo` (139-141).

**models.sh**

| `PROFILE` | `CLAUDE_MODEL` | `CODEX_MODEL` | Codex effort: implement / review / address | Forced author → reviewer |
|---|---|---|---|---|
| `thrifty` | sonnet | gpt-5.6-luna | medium / medium / low | codex → codex |
| `max` | opus | gpt-5.6-sol | high / high / medium | none (backlog author; the other model reviews) |
| `balanced` (**default**) | sonnet | gpt-5.6-sol | medium / medium / low | **codex → claude** |

- Every value can be overridden through the environment (`: "${VAR:=…}"`).
- `codex_args_for implement|review|address` prints `-m MODEL -c model_reasoning_effort="EFFORT"`. `claude_args_for` prints `--model MODEL`. Claude has no effort flag.
- `author_for` and `reviewer_for` apply `FORCE_*`. Without an override, the reviewer is the other model.
- `ANSWER_FINDINGS=1` (74) is **never read** anywhere.

**parse_review.py**
- `first_object(text)` finds the first `{` and scans for the matching brace, skipping strings and escapes, then tries `json.loads` on that span. It returns `None` on failure. It never tries a later `{`.
- `main(raw, out)`:
  - A reply that is not a dict with `findings` becomes `{"verdict":"unreadable","findings":[]}`.
  - `findings` that is not a list is treated the same way.
  - A missing or unknown severity becomes `high`.
  - Non-dict findings are dropped.
  - Writes indented JSON and prints `<verdict>, N finding(s), M serious`. Always exits 0.

**cycle.sh** (the step sequence is in 4.3). Tunables: `MAX_REPAIRS=2` (effectively 1) and `PUSH=0`.
- Files written to `pipeline/state/<id>/`: `log.txt`, `implement.prompt`, `author-report.txt`, `author-stdout.txt` (codex only), `gate-fast.txt`, `review.prompt`, `review.json`, `review-stdout.txt` (codex reviewer), `findings.json`, `address-1.prompt`, `address-1.txt`, `gate-fast-1.txt`, `gate-full.txt`.
- `prompt_from tpl out k=v…` (82-95) copies the template and substitutes each `{{KEY}}` through an inline Python heredoc, so any characters are safe.
- The commit message on landing is the title, the first 1200 bytes of the author's report, and the text "Written by X, reviewed by Y, N finding(s) answered. Passed the full gate: build, 234 kernel checks, …three harnesses…" (200-210). That text is stale.

**batch.sh** (4.4). Tunables: `BATCH=1` (sic), `PUSH=0`, and `WORKTREES=$ROOT/../zelr-worktrees`.
- Per-task files: `log.txt`, `implement.prompt` (filled by **sed**), `author-report.txt`, `author-stdout.txt`, `gate-fast.txt`, `review.prompt`, `review.json`, `review-stdout.txt`, `findings.json`, `verdict.txt`, `address.prompt`, `address.txt`, `gate-fast-2.txt`.
- Batch-level files: `pipeline/state/gate-full-batch.txt` and `pipeline/state/gate-full-<id>.txt`.

**land.sh** (4.5): `bash pipeline/land.sh pipeline/<id>`, optionally with `PUSH=1`.

**loop.sh** (4.5): `MAX_CYCLES=6`. It runs `preflight.sh` first and does not start if that fails.

**preflight.sh**
- Codex probe: `timeout 180 $CODEX exec --cd ROOT --sandbox read-only --skip-git-repo-check -o <mktemp> "Reply with exactly: ready"`. It writes to a real file because a Windows binary cannot open `/dev/stdout`.
- Claude probe: `timeout 180 claude -p "Reply with exactly: ready" </dev/null | tail -3`, which calls the **PATH** `claude` even when only `$CLAUDE` exists. If the reply mentions `authenticate`, `401` or `expired`, it prints a hint.
- It also checks: `qemu is there`, `python is there` (`command -v python`), a clean tracked tree, being on `main`, and that at least one `todo` exists.
- `ok` and `BAD` lines are counted as problems. Exits 0 with `preflight: ready`, else 1.

**selftest.sh**
- Builds a sandbox repo with `git init`. It copies `lib.sh, cycle.sh, models.sh, parse_review.py` and the prompts, and writes a **stub `gate.sh`** that exits `${GATE_RESULT:-0}`.
- The backlog has one row: `| demo | claude | A demonstration task | todo |`.
- Canned replies go in `$FAKES/<role>.txt`, and optional edits in `<role>.sh`. Roles are `author`, `review` and `address`.
- Scenarios and checks:

| # | Scenario | Checks |
|---|---|---|
| 1 | Happy path | rc 0, marked done, on main, branch gone, change on main (5) |
| 2 | Gate fails | rc 1, blocked, 1 commit only, branch gone, tree clean (5) |
| 3 | Author does nothing | rc 1, blocked (2) |
| 4 | High-severity finding answered | lands, answer in HEAD, "answering findings" logged once (3) |
| 5 | Non-JSON review | "unreadable" appears in the output (1) |
| 6 | JSON wrapped in prose | task ends `done` (1) |

- 17 checks in total. The last line is `pipeline self test: all N checks passed` or `N failed`. Only cycle.sh is exercised.

**release.sh** (4.7), with `set -euo pipefail`.

**Prompts**
- `implement.md`: nothing third-party; add tests; break a test once on purpose to prove it can fail; comment style (no em-dashes); never claim something passed without running it; run `bash pipeline/gate.sh fast`; leave changes **uncommitted**; the report has 4 bullets.
- `review.md`: run `git diff`; focus on memory lifetime, two tasks touching one thing, QEMU-only assumptions, arithmetic, and tests that cannot fail; do not restyle or suggest libraries. Answer with **JSON only**: `{"verdict":"sound"|"needs-work","findings":[{"severity":"high"|"medium"|"low","where":"file.c:123","what","why","fix"}]}`.
- `address.md`: every finding is fixed (with a check added) or rejected with a specific reason; run the fast gate; leave changes uncommitted; report one line per finding.

**backlog.md**

| id | author | state |
|---|---|---|
| `rtc-clock` | claude | blocked |
| `pipes` | codex | blocked |
| `lfn-read` | claude | blocked |
| `ap-tasks` | codex | done |
| `editor` | claude | **todo** |
| `nvme` | codex | **todo** |
| `demand-pages` | claude | **todo** |
| `xhci-hid` | codex | **todo** |

It is followed by an "Ideas not yet tasks" list: page cache and swap, self-hosting, a service model, a packet filter, multiple users, a finer lock, parallel TLS sessions, and some entries struck through as done.

### 3.7 The launcher (launcher/)

- **ZelrLauncher.csproj**
  - `OutputType WinExe`, `TargetFramework net8.0-windows`, `UseWPF`, `Nullable enable`, `ImplicitUsings enable`, `RootNamespace ZelrLauncher`, `AssemblyName zelr` (so the output is `zelr.exe`), `Platforms x64`, `ApplicationManifest app.manifest`.
  - `RuntimeIdentifier win-x64`, `SelfContained true`, `PublishSingleFile true`, `IncludeNativeLibrariesForSelfExtract true`.
  - The single `EmbeddedResource` is `..\build\zelr.bin`, `Link="zelr.bin"`. **No `starter.img.gz` is embedded** and there is no build step that produces one.
  - Single-file compression is not enabled, so the kernel bytes are stored raw in the exe. release.sh's `grep "$VER"` relies on this.
- **app.manifest**: assemblyIdentity `Zelr.Launcher` 1.0.0.0, `requestedExecutionLevel asInvoker`, supportedOS `{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}` (Windows 10/11), `dpiAware true`.
- **App.xaml**
  - Brushes: `Bg #0B0F0E`, `Panel #131917`, `Border #243029`, `Text #DCE6E1`, `Dim #8A9B93`, `Accent #4FD6A0`, `Warn #E0A64A`.
  - Style `Primary`: accent background, foreground `#06120D`, 15 pt semibold, padding 26,13, corner radius 6, opacity 0.9 on hover and 0.35 when disabled.
  - Style `Ghost`: transparent with a border, Dim text that turns Text on hover, opacity 0.35 when disabled.
- **MainWindow.xaml**
  - `Title="zelr"`, 620x760, minimum 560x680, centred on screen, Segoe UI.
  - Rows: a heading ("zelr" plus a description); a status border with `StatusDot` (a 10 px ellipse), `StatusTitle` ("Checking...") and `StatusDetail`; a "Once the black window opens, type these" list of `desktop`, `guide`, `exec hello.elf`, `bg count.elf`, `dhcp`, `fetch example.com / p.html` and `help`; a note "Your files live on a FAT16 disk image in your AppData folder"; the buttons `StartButton` ("Start zelr", disabled initially), `SetupButton` ("Set up the emulator", collapsed) and `RecheckButton` ("Check again"); and a hyperlink to https://github.com/blavese/zelr.
- **MainWindow.xaml.cs**
  - `Refresh()` (21-43) calls `Emulator.FindQemu()`. Found: the dot becomes Accent, "Ready", Start enabled, Setup hidden. Missing: the dot becomes Warn, "One thing is missing" plus an explanation, Start disabled, Setup visible.
  - `Start_Click` (45-72): `ExtractKernel()`, then `Start(qemu, kernel)`. On `Exited` it returns to "Ready / zelr closed". Any exception is shown as "It did not start" with `ex.Message`.
  - `Setup_Click` (74-113): a Yes/No `MessageBox`, then `InstallQemu()`. On `Exited` it calls `Refresh()`, and if QEMU is still missing it shows "Still not found".
  - `Link_Click` opens the URL through the shell.
- **Emulator.cs**
  - `LikelyPaths` (15-20): `C:\Program Files\qemu\…`, `C:\Program Files (x86)\qemu\…`, `C:\qemu\…`.
  - `FindQemu()` (23-55): the likely paths, then every entry of the **process** `PATH`, then the registry value `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\QEMU` → `InstallLocation`. Returns `null` if none match.
  - `ExtractKernel()` (58-74): writes the first manifest resource ending in `zelr.bin` to `%TEMP%\zelr\zelr.bin`, every time.
  - `DiskPath` (77-108): `%LocalAppData%\zelr\disk.img`. It moves `%LocalAppData%\nyx\disk.img` there if only the old file exists, ignoring IO and permission errors.
  - `DiskBytes` = 32 MiB (110).
  - `EnsureDisk()` (119-142): returns if the disk exists and is at least 32 MiB. Otherwise it gunzips a `starter.img.gz` resource if one exists (none does), or else creates a blank 32 MiB file that the kernel formats on first boot.
  - `Start(qemu, kernel)` (145-169): `UseShellExecute=false`, `CreateNoWindow=true`, and the arguments are:
    ```
    -kernel %TEMP%\zelr\zelr.bin -m 64 -no-reboot -name zelr
    -drive file=%LocalAppData%\zelr\disk.img,format=raw,if=ide,index=0
    -netdev user,id=n0 -device rtl8139,netdev=n0
    ```
    - The machine is the default `pc`.
    - There is **no `-accel`** (zelr.bat uses `accel=whpx:tcg`).
    - There is no `-serial`, no sound device, no USB device, and no `-append`, so the kernel boots to the desktop.
  - `InstallQemu()` (172-184): `winget install --id SoftwareFreedomConservancy.QEMU --accept-source-agreements --accept-package-agreements`, with `UseShellExecute=true` so the console window is visible.

---

## 4. Control flow and lifecycles

### 4.1 Exactly what `gate.sh fast`, `screen` and `full` run

Common prologue, in this order, for every mode:
1. Validate `MODE`; exit 2 if invalid.
2. Resolve `QEMU`.
3. Take the lock; exit 2 if it is held.
4. Print the VM note.
5. Print `=== gate (MODE) ===`.
6. **Build**: `bash build.sh`. If the output matches `\berror\b`, print `FAIL it builds` and **exit 1**; nothing else runs. Otherwise print `PASS it builds` and `export ZELR_PREBUILT=1`.
7. `run_step "the version is not behind the newest tag"` → `python tools/check_version.py` (serial).
8. `run_step "the kernel and its programs agree on the structs"` → `python tools/abicheck.py` (serial).
9. `run_step "and on what a new machine looks like"` → `python tools/defaultcheck.py` (serial).

A failing static step counts as a failure, but the gate continues.

**Group A**, parallel, in every mode:

| # | Label | Command | Timeout | Machine |
|---|---|---|---|---|
| A1 | the kernel's own checks | `selftest` | 300 | pc, IDE `gate.img` 32 MiB, `-m 256`, `-append selftest`, debug-exit |
| A2 | what a program can do that it could not | `keep timeout 600 python tools/ring3check.py` | 600 | G(pc,256) |
| A3 | the same checks on q35, with pcie and ahci | `selftest_q35` | 300 | q35 + ahci + ide-hd, `gateq.img` 32 MiB |
| A4 | the shell answers over serial | `keep timeout 400 bash tools/shell_test.sh` | 400 (90 inner) | pc `-m 64`, no disk |
| A5 | the boot log survives a reboot | `keep timeout 400 bash tools/blackbox_test.sh` | 400 (2×90 inner) | pc `-m 128`, IDE, 2 boots |

A5 waits because A1-A4 already fill `PAR_MAX=4`. Then `par_wait`: failed steps are re-run alone, serially.

**`fast` ends here**, followed by tidy and the summary. That is 9 report lines, plus any "alone" re-runs.

**Group B**, `full` only:

| # | Label | Command | Timeout |
|---|---|---|---|
| B1 | gpt is read, and refused when it does not add up | `tools/gpt_test.sh` | 600 |
| B2 | fat32 is read and written, and survives a reboot | `tools/fat32_test.sh` | 600 |
| B3 | nvme is a disk, partitioned and not | `tools/nvme_test.sh` | 600 |
| B4 | copy and paste moves text out of a program | `python tools/clipcheck.py` | 400 |
| B5 | the same checks again, on an nvme disk | `selftest_nvme` (q35, NVMe, 64 MiB) | 300 |

Then `par_wait`.

**Group C**, `screen` and `full`. It is started in this exact order, at most 4 at a time, and reported in the same order after one `par_wait`:

| # | Label | Harness | Timeout |
|---|---|---|---|
| C0 (full only) | all four boot paths | `tools/iso_test.sh` (defined as `boottest` at 401) | 900 |
| C1 | the desktop reaches the screen | shotcheck.py | 600 |
| C2 | typing reaches the terminal | termcheck.py | 900 |
| C3 | the windows go where they are told | deskcheck.py | 900 |
| C4 | a usb keyboard and mouse are found and used | usbcheck.py | 600 |
| C5 | input survives being touched during boot | inputcheck.py | 600 |
| C6 | notes come out at the pitch they were asked for | soundcheck.py | 600 |
| C7 | and out of the ensoniq as well | enscheck.py | 600 |
| C8 | the volume slider can be heard changing the volume | volcheck.py | 600 |
| C9 | a frame sends the part of the screen that changed | framecheck.py | 600 |
| C10 | a usb stick mounts, and files copy off it | mountcheck.py | 600 |
| C11 | files keep the names they were given | namecheck.py | 600 |
| C12 | the machine turns itself off | powercheck.py | 600 |
| C13 | an address is asked for and arrives | netcheck.py | 900 |
| C14 | pages come back off a real web server | webcheck.py | 900 |
| C15 | https works against the real web | tlscheck.py (**internet required**) | 900 |
| C16 | the browser shows a page and follows a link | browsercheck.py | 600 |
| C17 | a page does its work on a click, a timer and an answer | livecheck.py | 700 |
| C18 | a form is filled in and arrives as it was filled in | formcheck.py | 600 |
| C19 | bodies arrive compressed and connections are kept | wirecheck.py | 600 |
| C20 | a machine still starts once it has formatted its disk | bootcheck.py (**redefines `boottest`** at 519) | 900 |
| C21 | ctrl+f finds a word that is on the screen | findcheck.py | 600 |
| C22 | a deal reaches the cloth and the dealer plays it out | gamecheck.py | 900 |
| C23 | what is on the screen is a frame that was finished | tearcheck.py | 600 |
| C24 | a program on the disk runs by typing its name | progcheck.py | 600 |
| C25 | a file survives the power going out mid-write | crashcheck.py | 900 |
| C26 | a program built outside the tree runs on the machine | sdkcheck.py | 600 |
| C27 | and one written in nothing but standard C | libccheck.py | 600 |
| C28 | programs run on more than one processor | smpcheck.py | 600 |
| C29 | the programs it ships with do what they say | appcheck.py | 600 |
| C30 | a shell, with pipes and redirection | shcheck.py | 600 |
| C31 | a setting written by hand reaches the screen | setcheck.py | 600 |
| C32 | the decoders and the layout, with no screen at all | piccheck.py | 600 |

Group C has 32 steps in `screen` and 33 in `full`.

Totals:
- `screen`: 9 + 32 = 41 report lines.
- `full`: 9 + 5 + 33 = 47 report lines.

The header claims `fast ~4 min`, `screen ~5 min` and `full ~17 min` (gate.sh:9-14). Group C is 32 QEMU harnesses at 4 wide, several of them multi-boot with 10-30 s fixed waits (webcheck says of itself "this one is eight minutes in"). So the `screen` and `full` figures are very likely stale.

**Tidy and exit** (see 3.2): exit 0 if nothing failed, else exit 1.

How failures show up in the log:
- A failed parallel step prints its last 12 lines plus `....  label (busy host; again, alone)  Ns`.
- The re-run prints `PASS|FAIL  label, alone  Ns`.
- A failed static step prints its last 3 lines plus `FAIL label`.
- `keep` shows only the last 10 non-PASS lines of a failing harness.

### 4.2 Harness lifecycle (Python)

1. `build_once()`. This is a no-op under the gate.
2. `vm = Guest(disk_with_pid, memory=…, machine=…, extra=[…])`. This creates the disk, picks a free monitor port and starts QEMU with a serial drain thread.
3. `vm.wait_boot()` waits for the first `zelr…> ` prompt (up to 120 s). With `args=""` there is no prompt; for example deskcheck's second machine waits on the screen instead.
4. Drive the machine:
   - `vm.run` or `vm.fresh` for console commands;
   - `vm.type("desktop\n")` to open the desktop;
   - `mon = vm.monitor()`, then `mon.move_to`, `click`, `click_for`, `drag_for`, `send("sendkey …")`;
   - `mon.wait_screen(name, predicate)` for pixel conditions;
   - `vm.wait_serial(text)` for serial conditions.
5. `c.add(name, bool, shot)` for each check.
6. `finally: vm.stop()` (and, for audio, monitor `quit` first so the WAV header is written).
7. Host-side verification where it applies: readfat.py on disk images, WAV analysis, webserver records.
8. `sys.exit(c.report(keep=--keep))`.

### 4.3 `cycle.sh`: one task, in the main checkout

1. Source lib.sh and models.sh.
2. `backlog_next`. If there is nothing to do, print `backlog: nothing to do` and exit 0.
3. Split `id|author|title`. The author must be `claude` or `codex`, else `die`. Apply `author_for`/`reviewer_for` (default profile: author codex, reviewer claude).
4. `mkdir pipeline/state/<id>` and truncate `log.txt`.
5. `cd $ROOT`. If `git diff --quiet` or `git diff --cached --quiet` fails, `die` with "uncommitted changes". **Untracked files are not checked.**
6. `BRANCH=pipeline/<id>`: delete it if it exists, then `git checkout -q -b $BRANCH`. `backlog_set <id> doing` modifies backlog.md on the branch.
7. `FAKE_ROLE=author`. Fill `implement.prompt` with TASK. Run the author: codex with `codex_args_for implement` (stdout to author-stdout.txt), or claude with `claude_args_for`.
8. Non-zero rc: `abandon`. Empty report: `abandon`.
9. `CHANGED=$(git status --porcelain -- . ':!pipeline/backlog.md' ':!pipeline/state')`. If empty, `abandon "changed no files"`.
10. `bash pipeline/gate.sh fast > gate-fast.txt`. On failure, print tail -12 and `abandon`.
11. `FAKE_ROLE=review`. Fill `review.prompt` with TASK and REPORT. Run the reviewer into `review.json`.
12. `parse_review.py review.json findings.json`. Read `VERDICT` and `N_FIND` (**all** severities).
13. While `N_FIND > 0` and `repairs < MAX_REPAIRS`:
    - `repairs++`, `FAKE_ROLE=address`;
    - fill `address-N.prompt` with TASK and FINDINGS (the whole findings.json);
    - run the author **without model or effort args** (167-171);
    - fast gate into `gate-fast-N.txt`; on failure, `abandon`;
    - `N_FIND` = number of high findings in the **same old** findings.json;
    - `break` unconditionally after the first round (186).
14. `bash pipeline/gate.sh full > gate-full.txt`. On failure, print tail -16 and `abandon`.
15. `git add -A` and commit with the message described in 3.6.
16. `git checkout -q main`, then `git merge -q --no-ff $BRANCH -m "$TITLE"` (on failure, `abandon`), then `git branch -d`. `backlog_set <id> done` leaves backlog.md **modified and uncommitted on main**.
17. If `PUSH=1`: `git push -q origin main`.

`abandon(reason)` (68-78) does, in order: log; `cd ROOT`; `git checkout -q -- .`; `git clean -qfd`; `git checkout -q main`; `git branch -D`; `backlog_set <id> blocked` (again uncommitted on main); exit 1.

### 4.4 `batch.sh`: several tasks, worktrees, one full gate

1. Source lib and models. The tree must be clean (tracked files) and on `main`.
2. **Claim**: up to `BATCH` (default **1**) times, run `backlog_next` and `backlog_set <id> doing`.
3. If nothing was claimed, print `backlog: nothing to do` and exit 0.
4. Print the batch. The printed author and reviewer come from the backlog column and **ignore `PROFILE` overrides** (57).
5. `git add pipeline/backlog.md` and `git commit -qm "pipeline: claim <ids>"`.
6. For each task, run `one_task id author title &` in the background:
   1. `wt=$ROOT/../zelr-worktrees/<id>`. Run `git worktree remove --force`, `git branch -D pipeline/<id>`, then `git worktree add -q -b pipeline/<id> $wt main`.
   2. Set `ROOT=$wt` so the agents run in the worktree. Fill `implement.prompt` with **sed** `s|{{TASK}}|$title|`. Run the author, then restore `ROOT`.
   3. If rc is non-zero, fail. `(cd $wt && git diff --quiet) && fail "changed nothing"`. This **misses new untracked files**.
   4. **Fast gate:** `( cd "$wt" && bash "$PIPE/gate.sh" fast ) > gate-fast.txt`. `PIPE` is the main repo's pipeline directory, and gate.sh `cd`s to its own parent, so **this gates the main checkout, not the worktree**. It also takes the global gate lock (section 10, D).
   5. Review in the worktree. Fill `review.prompt` through a Python heredoc. Write `review.json`, run `parse_review.py` into findings.json and verdict.txt, and count the high findings into `n_high`.
   6. If `n_high > 0`: fill `address.prompt` and run the author with `codex_args_for address` or `claude_args_for`, then run the fast gate again into `gate-fast-2.txt` (the same flaw applies).
   7. `(cd $wt && git add -A && git commit -q -F -)` with the title, the first 1000 bytes of the report, and "Written by X, reviewed by Y."
   8. Return 0.
7. `wait` for each job. For every failure, `backlog_set <id> blocked` (uncommitted on main).
8. If none is ready: `git add -A`, commit "pipeline: nothing landed", exit 1.
9. **Integrate**: `git branch -D pipeline/integration`, then `git checkout -q -b pipeline/integration main`. For each ready id, `git merge -q --no-ff pipeline/<id> -m <id>`. A merge that conflicts is aborted and the task goes back to `todo`.
10. **Full gate once**: `bash $PIPE/gate.sh full > pipeline/state/gate-full-batch.txt`. This runs in the main checkout, which now has the integration branch, so it is correct here.
    - **Pass**: `git checkout main`, `git merge --ff-only pipeline/integration`, mark the landed tasks `done`, `git add -A && git commit -m "pipeline: <ids> landed"`, and push if `PUSH=1`.
    - **Fail** (fallback): print tail -14, `git checkout main`. For each landed id:
      - `git checkout -q -b pipeline/solo-<id> main`;
      - `git merge -q --no-ff pipeline/<id>` (the result is not checked);
      - `gate.sh full > gate-full-<id>.txt`;
      - pass: `checkout main` and `merge --ff-only solo`, then mark `done`;
      - fail: `checkout main`, delete the solo branch, mark `blocked`.
      - Finally `git add -A && git commit -m "pipeline: after splitting the batch"`.
11. **Tidy**: `git worktree remove --force` for each worktree, then `git worktree prune`. Print "left to do: N".

The branches `pipeline/<id>`, `pipeline/integration` and a passing `pipeline/solo-<id>` are left in place.

### 4.5 `land.sh` and `loop.sh`

**land.sh**:
1. A `BRANCH` argument is required and must exist.
2. The tree must be clean and on main.
3. `n = git log main..BRANCH | wc -l` must be greater than 0.
4. Print the diff stat.
5. Create `TRY=pipeline/try-<basename>` from main and merge BRANCH into it with `--no-ff`. If that fails: abort, go back to main, delete TRY, `die`.
6. `bash gate.sh full`, output to the terminal.
   - Pass: checkout main, `merge --ff-only TRY`, delete TRY, `backlog_set <basename> done`, `git add -A && commit "pipeline: <id> landed"`, push if `PUSH=1`.
   - Fail: checkout main, delete TRY, exit 1 ("main is untouched").

**loop.sh**:
1. Run `preflight.sh`; if it fails, print "not starting" and exit 1.
2. For `i` in `1..MAX_CYCLES` (6):
   - stop if `backlog_count_todo == 0`;
   - run `bash cycle.sh`;
   - success: `landed++`, reset the failure streak;
   - failure: `blocked++`, `consecutive_failures++`, and stop at 2.
3. Print the landed, blocked and left counts. Always exits 0.

Because cycle.sh leaves backlog.md dirty (4.3, steps 16 and abandon), **the second cycle dies at step 5**. loop.sh therefore lands at most one task per run (section 10, F).

### 4.6 The backlog state machine

- `todo` → `doing`: cycle.sh step 6, or batch.sh's claim.
- `doing` → `done`: the full gate passed and the task was merged.
- `doing` → `blocked`: any abandon or failed gate.
- `doing` → `todo`: only in batch.sh, when the task conflicts with the batch.
- `blocked` → `todo`: a person edits the row (README.md:161-163).

The scripts write only the `state` column. Rows are rebuilt with single spaces, so any column alignment a person made is lost.

### 4.7 `release.sh <tag> [notes.md]`

1. `VER=${TAG#v}`. `have=$(grep KERNEL_VERSION include/types.h | cut -d'"' -f2)`. If they differ, print "the kernel says X and the tag says Y" and exit 1.
2. `bash build.sh > /dev/null`, then `python tools/mkiso.py > /dev/null`, which produces `build/zelr.iso`.
3. `dotnet publish launcher/ZelrLauncher.csproj -c Release -o build/launcher > /dev/null`, which produces `build/launcher/zelr.exe`.
4. For each of the ISO and the exe: `grep -q "$VER" file`, otherwise print "was not built from VER" and exit 1. `VER` is used as a regex, so its dots match any character.
5. If `gh release view $TAG` succeeds, run `gh release edit $TAG -F notes` when notes were given. Otherwise run `gh release create $TAG --title "zelr VER"` with `-F notes` or `--generate-notes`.
6. `gh release upload $TAG build/zelr.iso build/launcher/zelr.exe --clobber`.
7. `gh release view $TAG --json assets --jq '.assets[] | "  \(.name)  \(.size) bytes"'`.

It does **not**:
- run the gate;
- check the tree is clean;
- check that the tag exists locally or points at HEAD. `gh release create` creates a missing tag from the remote's default branch.
- run check_version.py.

### 4.8 Launcher flows

- **Startup**: `MainWindow` is constructed, and `Loaded` calls `Refresh()` → `FindQemu()`.
- **Start**:
  1. Disable the button and show "Starting".
  2. `ExtractKernel()` copies the resource to `%TEMP%\zelr\zelr.bin`.
  3. `Start()` calls `EnsureDisk()` (32 MiB blank on first run) and then `Process.Start(qemu …)`.
  4. On `Exited`, show "zelr closed. Press Start to boot it again" and re-enable Start. QEMU's own error output is never captured (`CreateNoWindow`, no redirect).
  5. Any exception is shown as "It did not start: <message>" and Start is re-enabled. The dot stays Warn even after a later successful start.
- **Set up**:
  1. Yes/No dialog.
  2. `winget install --id SoftwareFreedomConservancy.QEMU …` in a visible console.
  3. On `Exited`, call `Refresh()`. If QEMU is still missing, show "Still not found … Try Check again, or install QEMU yourself from qemu.org". The winget exit code is ignored.
- **Check again**: `Refresh()`.

---

## 5. Interfaces

### 5.1 What this area exports, and who uses it

| Export | Users |
|---|---|
| `harness.Guest`, `Checks`, `build_once`, `ROOT` | 34 harnesses plus shots.py. Only `Checks`/`ROOT` for abicheck and defaultcheck. |
| `harness.Monitor` (through `Guest.monitor()`) | Every screen, keyboard or mouse harness. |
| `count_in`, `count_near`, `centre_of`, `row_mean`, `colour_gap`, `face_width`, `_count`, `qemu_path`, `FACE_*` | deskcheck, shotcheck, termcheck, clipcheck, livecheck, gamecheck, tearcheck (`count_all`), netcheck, volcheck, setcheck, browsercheck, formcheck, shots, bootcheck (`qemu_path`). |
| `webserver.Server`, `INSTANT_IP` | webcheck, browsercheck, livecheck, formcheck, wirecheck, findcheck, shots. |
| `soundcheck.read_wav`, `loud_runs`, `frequency_of`, `close_to`, `LOW_HZ`, `HIGH_HZ`, `NOTE_MS` | enscheck, volcheck, appcheck. |
| `mkfat.Fat16Builder` | mkgpt.py. mkfat.py itself is run as a subprocess by mountcheck, appcheck, sdkcheck, libccheck, fat32_test.sh and mkiso.py. |
| `loadaddr.load_address` | check_loader.py, mkiso.py, build.sh (`python tools/loadaddr.py`). |
| `readfat.py` (subprocess) | mountcheck, namecheck. |
| `gate.sh` | cycle.sh, batch.sh, land.sh, humans, and agents (prompts tell them to run `bash pipeline/gate.sh fast`). |
| `lib.sh` functions | cycle, batch, land, loop, preflight. selftest.sh copies lib.sh. |
| `build/zelr.bin` → the launcher | csproj `EmbeddedResource`. release.sh runs `dotnet publish`. |

### 5.2 Environment variables

| Variable | Read by | Effect |
|---|---|---|
| `QEMU` | harness.qemu_path, gate.sh, preflight.sh, every tools/*.sh, run.sh | Path to qemu-system-x86_64. There is a PATH fallback everywhere except the launcher and zelr.bat, which have their own searches. |
| `ZELR_PREBUILT=1` | harness.build_once, shell_test.sh, mkiso.py | Skip rebuilding. gate.sh exports it after its build (243). |
| `ZELR_UEFI_FW` | iso_test.sh | Path of the edk2 code firmware. Without it: `/c/Program Files/qemu/share/edk2-x86_64-code.fd`, else the UEFI paths are skipped. |
| `PAR_MAX` (4), `TMPDIR` | gate.sh | Parallel width, and where the lock lives. |
| `BATCH` (1), `PUSH` (0) | batch.sh | Number of tasks; whether to push. |
| `PUSH` | cycle.sh, land.sh, loop.sh (via cycle) | Whether to push. |
| `MAX_REPAIRS` (2, effectively 1), `MAX_CYCLES` (6) | cycle.sh, loop.sh | Repair rounds; cycle budget. |
| `PROFILE`, `CLAUDE_MODEL`, `CODEX_MODEL`, `CODEX_EFFORT_IMPLEMENT/REVIEW/ADDRESS`, `ANSWER_FINDINGS` (unused) | models.sh | See 3.6. |
| `AGENT_TIMEOUT` (3600) | lib.sh | Timeout for each agent run. |
| `CODEX`, `CLAUDE`, `LOCALAPPDATA`, `USER`, `HOME` | lib.sh | Agent discovery. |
| `FAKE_AGENTS`, `FAKE_ROLE`, `GATE_RESULT` | lib.sh, selftest.sh, the stub gate | Test seam. |
| `ZIG` | build.sh (+ sub-builds), sdk/build.sh, sdk/libc/build.sh | Zig path. **The gate does not export it to harnesses**, so sdkcheck and libccheck need zig on PATH. |

### 5.3 Contracts with the kernel and userland that the tests depend on

If you change any of these, the named harnesses break.

- **Kernel command-line words**, matched as substrings (kernel/main.c:313-316): `selftest` (gate, run.sh -T, zelr.bat test) and `console` (every Guest by default, and the bash harnesses).
- **Selftest output**: `N passed, M failed` and `SELFTEST_PASS`/`SELFTEST_FAIL` (kernel/selftest.c:3602-3603). The debug-exit port 0xF4 gives a QEMU exit code of 1 on pass and 5 on fail.
- **Prompts**: `zelr> ` and `zelr:<cwd>> ` (harness PROMPT). The ring-3 `/bin/sh` prompt ends in `$ ` (shcheck).
- **Echo**: the console echoes typed characters to serial (kernel/shell.c:626-628; kernel/printf.c:16-24). Several checks are only valid because they use `fresh()` or fencing.
- **Boot-log strings**: `== <phase>`, `fs new disk prepared`, `fs fat%d mounted`, `parts gpt, %d partition(s)`, `parts gpt rejected: …`, `parts skipping partition … efi system partition`, `fs mounted partition %d …`, `usb volume mounted …`, `disk %s via %s`, `progs %d built in`, `long mode`, `back at the shell`, `%d hub(s), %d keyboard(s), %d mouse, %d disk(s)`, `powering off, sleep type %d/%d`, `sound   hda`, `ensoniq es137`, `44100 Hz`, `converter`, `into pin`, `usb ethernet`.
- **Shell and terminal command output**: `cannot power off`, `not a command, and no program of that name here…`, `wrote sector N`, `size N sectors of 512 bytes`, `status N, H bytes of headers, B bytes of body`, `X is A.B.C.D`, `TLS 1.3, x25519, aes-128-gcm`, `tls:`, `secure:`, `address …`, `packets N in, M out`, `no network card`, and the ids `10ec:8029`.
- **/sys files**:
  - `/sys/lastboot` (blackbox, nvme)
  - `/sys/devices` (usbcheck `usb …` line; `keyboard  ps/2 and usb`, sysfs.c:186)
  - `/sys/screen` (`fullkib`, `frames`, `sentkib`, `shared`)
  - `/sys/cpu` (regex `cpu(\d+)\s+apic \d+, \w+\s+(\d+) slices, (\d+) ticks, (\d+) busy`)
  - `/sys/settings` (rows of `key value lo hi def …`)
  - `/sys/theme` (`accent 0x…`, `surface 0x…`)
  - `/sys/clipboard`
- **/bin programs and their markers**:
  - Programs: `hello` (prints `5050`), `wintest`, `spin` (`spinning`), `halfdrawn` (RED `C81E1E`, GREEN `1E9632`, MINT `1EC864`), `crashwrite`, `wiretest` (`WIRETEST_PASS/FAIL`, `PASS  ` lines), `sh` (`zelr shell`).
  - Suites `*test` print `<NAME>_PASS`/`_FAIL`. jstest prints `… of N passed`.
  - `spawntest` and `jsprobe` are run by no harness.
- **Colours**: cursor `F4F7F9`/`080C10` (wm.c:2890); menu and overlay surface `F4F4F7` (`MODERN_L_SURFACE`, theme.c:52); shotcheck's exact `F3F3F6`; terminal pages `SLATE 10141A`, `PAPER F2EEE4`, `AMBER 140E04`, `PHOSPHOR 020A02`; link and accent `6E8AE8`; find highlight `FFE58F`.
- **Geometry**: 1024x768 default; dock height 44, gap 14, side 16, badge 76; launcher rail 132, pane 152, item 32, pad 10, 6 kinds (Productivity, Internet, Media, Games, System, Session); title bar 32, border 1; icon column `ICON_LEFT 18`, cell width `32 + 2×30 = 92` (theme defaults `icon_size 32`, `icon_gap 30`); window cascade `x = wm_icons_right() + 14 + step*48`, `y = 36 + step*38` (winsrv.c:195-196).

### 5.4 Host requirements, with the ground truth for this machine

**Present on this host now** (per the coordinator):
- Git for Windows 2.55, with bash at `C:\Program Files\Git\bin\bash.exe`;
- Python 3.11.8;
- Zig 0.16.0 (winget);
- QEMU 11.1.0, installed **per user** at `%LOCALAPPDATA%\Programs\qemu` and on the user PATH;
- `ZELR_UEFI_FW` pointing at `…\qemu\share\edk2-x86_64-code.fd`. I checked that this file exists, and that `C:\Program Files\qemu\qemu-system-x86_64.exe` does **not** exist.
- `bash build.sh` succeeded. `bash run.sh -T` reported **556 passed, 0 failed** (section 8).

| Requirement | Why and where | Notes |
|---|---|---|
| bash + GNU userland (Git for Windows / MSYS2) | Every .sh file. Uses `timeout`, `mktemp`, `head -c`, `date +%s`, GNU `find -newermt @epoch` (gate tidy), `awk`, `sed` with `\L` (lib.sh:30), `cksum`, `stat -c`, `ps -W` (gate.sh:61; MSYS-only, errors are ignored elsewhere), `tee`, `wc`. | Present (Git for Windows 2.55). |
| Python ≥ 3.7, runnable as **`python`** | gate.sh, build.sh, pipeline scripts. `http.server.ThreadingHTTPServer` and `capture_output=` need 3.7; f-strings need 3.6. shell_test.sh only needs `python3` when there is no `timeout` command. | 3.11.8 present, and `python` works in Git Bash since `bash build.sh` passed. **Standard library only** for every test. `mkroots.py` needs third-party **`certifi`** when no PEM bundle is passed (mkroots.py:242-247). |
| QEMU qemu-system-x86_64 | Every boot. The machines, devices and backends used are listed below this table. | 11.1.0 present, per user. See 5.5 for which scripts find it. |
| UEFI firmware `edk2-x86_64-code.fd` | iso_test.sh UEFI paths; zelr.bat `modern`. | Present, per user. iso_test.sh needs `ZELR_UEFI_FW`. |
| Zig (any version that builds) | build.sh, userland, bootloader and uefi builds; sdk/build.sh and sdk/libc/build.sh (sdkcheck, libccheck). | 0.16.0 present. The sdk builds need `zig` on PATH or `$ZIG`. |
| git | check_version.py (`git tag`), the whole pipeline. | Present. |
| Internet access and DNS | tlscheck.py (5 public sites). webcheck and the others use the local server. | Without it, `gate.sh screen` and `full` fail by design. |
| Windows PowerShell + System.Drawing | genjpeg.py only. | Only for regenerating test data. |
| .NET 8 SDK (`dotnet`) with the Windows desktop workload | The launcher and release.sh. | Presence unknown. |
| GitHub CLI `gh`, authenticated | release.sh. | Presence unknown. |
| Claude Code CLI `claude` and Codex CLI `codex`, signed in | The pipeline (`preflight.sh` checks both). | Presence unknown. |
| Disk space and RAM | Each harness disk is 32-64 MiB. Up to 4 machines at 64-512 MiB, plus audio WAVs and roughly 2-3 MB per screenshot. | |

QEMU features the tests need:
- Machines: `pc`, `q35`.
- Devices: `ahci`, `ide-hd`, `nvme`, `isa-debug-exit`, `qemu-xhci`, `usb-kbd`, `usb-mouse`, `usb-hub`, `usb-storage`, `usb-net`, `intel-hda`, `hda-output`, `ES1370`, NIC models `e1000`/`e1000e`/`pcnet`/`rtl8139`/`ne2k_pci`.
- Backends and options: `-audiodev wav`, `-nic user[,guestfwd=…]`, `-netdev user`, `-drive if=pflash,readonly=on`, `-smp 2/4`.
- HMP commands: `screendump`, `mouse_move` (with dz), `mouse_button`, `sendkey`, `device_add`, `device_del`, `quit`.

### 5.5 Windows-specific assumptions, and the per-user QEMU install

How each launcher of QEMU behaves with QEMU only at `%LocalAppData%\Programs\qemu` (on the user PATH) and firmware there too:

| Script or program | Finds QEMU? | Finds firmware? | Why |
|---|---|---|---|
| harness.py (all Python harnesses, bootcheck) | Yes | n/a | The default `C:/Program Files/…` is missing, so it uses `shutil.which` (harness.py:38-43). This needs the user PATH in the process environment. |
| gate.sh, preflight.sh | Yes | n/a | `[ -x "$QEMU" ] || QEMU=$(command -v qemu-system-x86_64 …)` (gate.sh:38-39; preflight.sh:60-61). |
| shell_test, blackbox, gpt, fat32, nvme, iso_test, run.sh | Yes | -- | `command -v` before the Program Files default. |
| iso_test.sh firmware | -- | **Only via `ZELR_UEFI_FW`** | The default is `/c/Program Files/qemu/share/…` (iso_test.sh:25-26). If the variable is unset, both UEFI paths are **silently skipped** and the gate still passes (`boot test: the bios paths passed, uefi skipped`). |
| zelr.bat | Yes | **No** | QEMU is found with `%%~$PATH:I` (zelr.bat:23-25). `modern` hard-codes `%ProgramFiles%\qemu\share\edk2-x86_64-code.fd` (113) with no env override, so it prints "Falling back to BIOS" and boots `plain`. |
| launcher `FindQemu()` | **Only if the per-user directory was on PATH when the launcher started** | n/a | `LikelyPaths` are all Program Files or `C:\qemu`. The **process** PATH is read once at process start. The registry fallback reads **HKLM** `…\Uninstall\QEMU`, and a per-user install registers under HKCU, so it misses. **After the launcher's own "Set up the emulator" runs winget and winget installs per user (as happened on this machine), `Refresh()` still reports "Still not found" until the launcher is restarted** (Emulator.cs:23-55; MainWindow.xaml.cs:93-104). |

Other Windows assumptions:
- **Hard-coded user name.** The zig fallback `/c/Users/admin/AppData/Local/Microsoft/WinGet/Packages` is repeated in build.sh:16, userland/build.sh:17, bootloader/build.sh:9 and uefi/build.sh:10.
- lib.sh uses `/c/Users/$USER/AppData/Local/OpenAI/Codex/bin` and `codex.exe`.
- Windows file locking is the stated reason for:
  - PPMs named by pid (harness.py:492-499);
  - `ZELR_PREBUILT` (harness.py:817-821; gate.sh:237-243; mkiso.py:301-305);
  - disk images named by pid.
- Paths passed to QEMU are converted with `.replace("\\", "/")` (harness screen, soundcheck/appcheck/mountcheck audio and drive arguments).
- preflight writes codex output to a temp file because the Windows binary cannot open `/dev/stdout` (preflight.sh:28-29).
- fake_agent tests with `-f` because the exec bit is lost on Windows filesystems (lib.sh:58-61).
- `ps -W` (gate.sh:61).
- genjpeg uses PowerShell and System.Drawing.
- The launcher, zelr.bat and run.sh all keep a persistent disk and migrate an old `nyx` one: `%LocalAppData%\zelr\disk.img` (migrated from `…\nyx\disk.img`), or `zelr.img` in the repo root (migrated from `nyx.img`).

A note on the verified run: `bash run.sh -T` created `zelr.img` in the repository root (16 MiB, 10:19 today). run.sh keeps that disk and reuses it (run.sh:16-29). So the selftest through run.sh runs on a persistent disk, unlike the gate's fresh one. The file is gitignored by `*.img`.

---

## 6. Concurrency, locking, memory ownership, invariants

- **Only one gate per machine.** `mkdir ${TMPDIR:-/tmp}/zelr-gate.lock` is atomic, and a second gate exits 2. The lock is shared by every checkout and worktree using the same TMPDIR. That includes gates an agent starts inside a worktree, and the per-task gates in batch.sh. A lock left by SIGKILL has to be removed by hand. The trap handles INT and TERM but does **not exit** (section 10, AA).
- **Parallel steps inside a gate**: at most `PAR_MAX=4` background subshells, counted with `jobs -rp`. Each writes to its own `stepN.txt`, and results are read back in start order. A retry runs serially after the whole group finishes.
- **Shared files inside a gate**:
  - Everything a Guest creates is named by pid: `<name>.<pid>.img` and `.wav`, `mountseed.<pid>.txt`, and `build/pointer-<pid>.ppm`.
  - Screenshot names otherwise use a per-harness prefix: `desk-*`, `term-*`, `br-*`, `fm-*`, `fn-*`, `live-*`, `bj-*`/`pk-*`/`gm-menu`, `tear-*`, `fr-*`, `set-*`, `vol-*`, `net-*`, `calc-*`/`mon-*`, `clip-start`, and shotcheck's `desktop`/`launcher`/`menu`/`settings`/`recoloured`/`launcher-again`.
  - Fixed names that rely on one instance per gate: `gate.img`, `gateq.img`, `gatenv.img`, `build/isotest.img`, `build/zelr.iso`, `build/esp.img`, `build/bootcheck.ppm`.
  - `build/zelr.bin` must not be rewritten while machines are booting. **mkiso.py rewrites it anyway** (it flattens even under `ZELR_PREBUILT`), and in `full` it does so concurrently with the first screen harnesses (section 10, C).
- **The Guest serial buffer**: the `_drain` thread appends one byte at a time to `_chunks` under `_lock`, and readers join the whole list. Memory grows for the life of the machine, which is fine for minutes-long runs.
- **Monitor**: one socket per Guest. It is not thread-safe and does not need to be (single-threaded use). Each `send` blocks until the prompt comes back. `quit` makes the monitor close mid-reply, which raises `Timeout` and is caught (soundcheck.py:184-187).
- **The pipeline in the main checkout**: cycle.sh, batch.sh's integration phase and land.sh all switch branches in `$ROOT`. Nothing else may use that checkout while they run. The agents (and batch's fast gate, wrongly) run in the same place.
- **The pipeline in worktrees**: each batch task has `../zelr-worktrees/<id>` on branch `pipeline/<id>`. Worktrees are created in parallel in the background.
- **Invariants the scripts try to keep**:
  1. Nothing reaches `main` without passing `gate.sh full`. The exception is bookkeeping commits: claim, landed, nothing-landed and after-splitting use `git add -A` on main.
  2. Only the backlog `state` column is written by scripts.
  3. `pipeline/state/<id>/` keeps every prompt, report and gate log, and is gitignored.
  4. Agents leave their changes uncommitted; the scripts commit.
  5. `PUSH` is only ever explicit.
- **Ownership of files a harness creates**: the harness deletes its own files in `finally` blocks. When the gate's `timeout` kills a Python harness, `finally` blocks do **not** run (the default SIGTERM action, or TerminateProcess). The disk image, any WAV, and the QEMU child (orphaned) are then left behind. The gate's `stale()` sweep covers only 10 patterns.
- **The launcher** runs single-threaded on the WPF UI thread. The `Exited` handlers marshal back with `Dispatcher.Invoke`. A per-user disk image is shared by all launcher instances. Two machines on the same `disk.img` would conflict on QEMU's image lock; the Start button prevents this only within one instance.

---

## 7. Limits and magic numbers

| Constant | Value | Where | Meaning |
|---|---|---|---|
| `PAR_MAX` | 4 | gate.sh:116 | Machines at once in the gate. |
| Step timeouts | 300 (selftests), 400 (shell, blackbox, clip), 600 (most), 700 (live), 900 (term, desk, net, web, tls, boot, game, crash, iso) | gate.sh | `timeout` per step. |
| Inner QEMU timeouts | 90 (shell, blackbox, gpt, iso), 120 (fat32), 150 (nvme) | tools/*.sh | Per boot. |
| Gate disks | 32 MiB (`gate.img`, `gateq.img`), 64 MiB (`gatenv.img`) | gate.sh:284, 318, 380 | Fresh each run. |
| Guest defaults | `size_mb=32`, `memory=64`, `kernel=zelr.bin` | harness.py:602 | |
| `wait_boot` | 120 s | harness.py:695 | First prompt. |
| `run`/`fresh` | 30 s | harness.py:704, 712 | Next prompt. |
| Monitor connect | 20 s | harness.py:299 | Retries every 0.2 s. |
| Monitor reply | 180 s | harness.py:298, 314 | Socket timeout. |
| Typing gap | 0.05 s (0.18 s with the browser) | harness.py:726; formcheck, findcheck | Serial drops. |
| `MOUSE_GAP`, `MOUSE_STEP` | 0.05 s, 100 px | harness.py:71, 91 | PS/2 limits. |
| Corner walk | 12 × (-200,-200) | harness.py:365-366 | Pin the pointer at (0,0). |
| Click hold | 0.4 s down, 0.5 s after release | harness.py:407-408 | WM sampling. |
| `click_for` | 20 s / 3 tries | harness.py:410 | |
| `drag_for` | 24 s / 3 tries, 8 steps | harness.py:439, 532 | |
| `pointer` | 8 tries × 0.3 s, tolerance 3 px | harness.py:392-397, 471 | |
| Cursor detection | box 12×19, fill ≥ 20, edge ≥ 10 | harness.py:79-85, 521-529 | |
| `wait_screen` | 30 s, every 0.25 s | harness.py:570 | |
| `count_near` default tolerance | 8 (callers use 6-8) | harness.py:135 | Translucent panels. |
| `centre_of` `min_pixels` | 200 | harness.py:176 | formcheck uses 60 for link text. |
| `AGENT_TIMEOUT` | 3600 s | lib.sh:71, 93 | Each agent run. |
| Preflight probe timeout | 180 s | preflight.sh:31, 46 | |
| `MAX_REPAIRS`, `MAX_CYCLES`, `BATCH` | 2 (effectively 1), 6, 1 | cycle.sh:25, loop.sh:17, batch.sh:29 | |
| Commit report excerpt | 1200 bytes (cycle), 1000 bytes (batch) | cycle.sh:204, batch.sh:169 | |
| ring3check JS cases | ≥ 86 | ring3check.py:96-97 | |
| crashcheck | 6 rounds, kill delay 0.15-2.5 s, 15 boots | crashcheck.py:56, 105 | |
| soundcheck | 440 and 880 Hz, 600 ms, pitch ±6%, floor 2000, ≥120 ms runs | soundcheck.py:26-28, 81, 150 | appcheck allows ±8%. |
| volcheck | floor 400, ≥30 ms, loud > 1.5 × quiet | volcheck.py:119, 133 | |
| tearcheck | 14 samples, 0.13 + 0.01·i s apart | tearcheck.py:33, 69 | |
| usbcheck stick | 8 MiB, pattern at LBA 100, write 0xAB at LBA 200 | usbcheck.py:53-57 | |
| Launcher disk | 32 MiB | Emulator.cs:110 | Blank; the kernel formats it. |
| Launcher VM | `-m 64`, rtl8139, `-name zelr`, no accel | Emulator.cs:154-165 | |
| Launcher window | 760×620, minimum 680×560 | MainWindow.xaml:4-5 | |
| Selftest exit codes | QEMU 1 on pass, 5 on fail (`(code<<1)|1`) | kernel/main.c:156-161, 230; run.sh:42-43 | |

---

## 8. Tests

### 8.1 The complete test catalogue

Machine notation is as in 3.1. Every Guest adds `-kernel build/zelr.bin -no-reboot -display none -serial stdio`, an IDE disk named by pid (32 MiB unless noted), a free-port monitor, and `-append console` unless noted.

Drive methods:
- **S** = typing on serial, reading the serial transcript
- **K** = monitor `sendkey`
- **M** = monitor mouse
- **P** = screendump pixel analysis
- **A** = WAV recording
- **H** = host-side verification (readfat, image bytes, web server records)

"Checks" is the number of `Checks.add` or grep checks when everything is reachable. Times are static estimates.

| Tool | Tier | Machine(s) and QEMU arguments | Drive | What it asserts | Checks | Est. time |
|---|---|---|---|---|---|---|
| build (build.sh) | every | n/a | n/a | The output contains no lowercase word `error`. Otherwise the gate stops. | 1 | 1-3 min |
| check_version.py | every (serial) | none | n/a | KERNEL_VERSION is not behind the newest `vX.Y.Z` tag. | exit code | <2 s |
| abicheck.py | every (serial) | none | n/a | `SYS_*` names match, values match, no duplicates; 5 struct shapes match. | 4 + 5 | <2 s |
| defaultcheck.py | every (serial) | none | n/a | Wallpaper enum parses; kernel and settings defaults and saved keys agree. **Currently vacuous.** | 3 | <2 s |
| selftest (gate.sh:282) | fast | pc `-m 256 -append selftest`, IDE 32 MiB, isa-debug-exit | S | `SELFTEST_PASS` appears. | ≈556 (in-kernel) | ~1 min |
| ring3check.py | fast | G(pc,256) | S | 19 suites: each runs and prints `_PASS`; jstest has at least 86 cases. | 39 | 3-5 min |
| selftest_q35 | fast | q35, `-device ahci` + `ide-hd`, 32 MiB | S | `SELFTEST_PASS`. | in-kernel | ~1 min |
| shell_test.sh | fast | pc `-m 64 -append console`, **no disk** | S | uname and version, file operations, cd/pwd, ps, mem, spawn, exec hello, wintest surface at 0x8060000000, echo. | 17 | ~40 s |
| blackbox_test.sh | fast | pc `-m 128`, IDE 32 MiB, 2 boots | S | The second boot's `/sys/lastboot` (fenced) holds the first boot's marks and "prepared", and not "fat16 mounted". | 11 | ~40 s |
| gpt_test.sh | full | pc `-m 256`, IDE, 4 boots | S | Good GPT mounts partition 2, not the ESP; bad header CRC and bad entry CRC are rejected and nothing is mounted; a bare disk is formatted. | 12 | 1-1.5 min |
| fat32_test.sh | full | pc `-m 256`, IDE, 3 boots, 40 MiB FAT32 plus 32 MiB FAT16 | S | FAT32 detected; a file past cluster 65535 is read; writes survive a reboot; nothing is reclaimed; FAT16 still works. | 14 | 1-1.5 min |
| nvme_test.sh | full | q35 `-m 256`, `-device nvme,serial=zelr0001`, 64 MiB, 3 boots | S | `via nvme`; writes survive; boot log recovered (request splitting); GPT on NVMe. | 12 | 1-1.5 min |
| clipcheck.py | full | G(pc,128) | S,K,P | Terminal up; ctrl-c/ctrl-v paste reaches echo; ctrl-a copies the scrollback; `/sys/clipboard` has the marker and many lines. | 5 | ~1 min |
| selftest_nvme | full | q35 NVMe 64 MiB | S | `SELFTEST_PASS`. | in-kernel | ~1 min |
| iso_test.sh | full (in group C) | pc `-m 256`, no `-kernel`: `-cdrom` or ISO-as-IDE, BIOS or pflash edk2, plus a 32 MiB IDE index 1 | S | For each of 4 paths: `zelr VERSION`, `long mode`, `progs`, `zelr:/home>`, and the echoed tag. UEFI is skipped without firmware. | 20 | 1.5-2.5 min |
| shotcheck.py | screen | G(pc,64) | S,M,P | Terminal drawn at 1024x768; launcher opens; Settings starts; 6 swatches; clicking teal moves more than 600 px of accent from indigo to teal. | 10 | 1-2 min |
| termcheck.py | screen | G(pc,64) | S,K,P | Typed `theme paper`; tab completion; history; left+insert; backspace; home+delete; still alive. Everything is verified by the page colour. | 8 | 1-2 min |
| deskcheck.py | screen | G(pc,64), then G(pc,512, `args=""`) | S,K,M,P | Min, restore, max (the panel tucks away and returns), unmax, corner resize, alt-left/right snap, 3 wallpapers, launcher → Paint, alt-tab, alt-d, wheel up and down, 800x600 and back, wallpaper-click launcher, right-click menu, close all, rubber band selects and releases icons, launcher search "ain" runs Paint, auto-desktop without a command line, ESC to shell, 1920x1080 layout. | 37 | 4-7 min |
| usbcheck.py | screen | G(q35,128) with qemu-xhci, usb-kbd, usb-mouse; G(q35,128) with a usb-hub on port 1 and devices on 1.1/1.2; G(q35,128) with a mouse only, then `device_add`/`device_del` usb-kbd; G(q35,256) with usb-storage (8 MiB) | S,K,M,H | Enumeration; keys arrive only through the USB keyboard; report counter; desktop starts; USB mouse moves the pointer; hub works; hot-plug in and out; stick size, read, write, and the host sees 0xAB at LBA 200. | 24 | 3-5 min |
| inputcheck.py | screen | 5 × G(q35,128) | K | Typing works after 0/8/25 keys, 8 mouse moves, or both, sent during boot. | 5 | 2-4 min |
| soundcheck.py | screen | G(q35,256) `-audiodev wav -device intel-hda -device hda-output` | S,A | hda found with a codec route; `beep 440 600` and `beep 880 600`; exactly 2 notes at ±6%; different; silent afterwards. | 9 | ~1 min |
| enscheck.py | screen | G(pc,256) `-device ES1370,audiodev=a0` | S,A | `ensoniq es137`, `44100 Hz`, then the same 2-note checks. | 9 | ~1 min |
| volcheck.py | screen | G(pc,256) + intel-hda | S,M,P,A | Speaker popover opens; slider drags produce at least 2 bursts; the loud one's peak is more than 1.5× the quiet one's. | 5 | 1-1.5 min |
| framecheck.py | screen | G(pc,512) `-smp 2` | S,M | `/sys/screen`: fullkib > 1000; more than 10 frames drawn; average frame KiB less than half the full frame; `shared > 0`. | 5 | 1-2 min |
| mountcheck.py | screen | G(q35,256) with xhci and usb-storage (mkfat 8 MiB FAT16) | S,H | Mounted; ls/cat on the stick; write joins the existing file; copy to /home; `usb/` in `/`; readfat confirms file and contents. | 8 | ~1 min |
| namecheck.py | screen | G(q35,256), then a second boot of the same disk | S,H | Long and multi-dot names listed and opened; short name works; the name persists across reboots; delete removes the long entries; readfat agrees. | 11 | 1-1.5 min |
| powercheck.py | screen | G(q35,256) | S | `shutdown`: QEMU exits within 20 s with code 0; `sleep type` logged; no "cannot power off". | 4 | <1 min |
| netcheck.py | screen | G(pc,128) default NIC; G(pc,128) `-nic none`; G(q35,192) `-nic none -device qemu-xhci -device usb-net,netdev=u1 -netdev user,id=u1` | S,M,P | DHCP gets 10.0.2.15 by itself and router 10.0.2.2; the dock icon and popover work; no card means no address and a different icon; usb-net gets an address with packets counted both ways. | 17 | 2-3 min |
| webcheck.py | screen | G(pc,192) `-nic user,model=e1000`, `…pcnet`, `…ne2k_pci`, `…e1000,guestfwd=tcp:10.0.2.100:80-…` | S,H | 1000 and 40000 byte bodies; a repeat fetch; 200000 bytes intact 3 times; slow body; 404; pcnet small and large; an undriven card named `10ec:8029`; the instant-reply server. | 12 | 3-8 min |
| tlscheck.py | screen | G(pc,256) `-nic user,model=rtl8139` (**internet**) | S | For 5 sites: handshake `TLS 1.3, x25519, aes-128-gcm`, a status and a body (markers for 2 sites); a non-TLS port refused with `tls:`; plain http works and is not TLS. | ~21 | 2-6 min |
| browsercheck.py | screen | G(pc,256) e1000 + Server | S,M,P | Page drawn on white; link in the accent colour clicked; back returns to identical pixels; measured and chunked render identically; styled vs bare; PNG; missing image; SVG; script vs unscripted; redirect equals the second page; 404 differs; https is refused visibly. | 22 | 3-5 min |
| livecheck.py | screen | G(pc,256) e1000 + Server | S,M,P | The script-free page stays blue. With scripts: timer (orange), external script (violet), XHR (teal) and click listener (green). | 8 | 1.5-2 min |
| formcheck.py | screen | G(pc,256) e1000 + Server | S,M,P,H | Fields are drawn, the hidden one is not, rounded corners; the server receives `q=hello+world`, `deep=yes`, `from=zelr`, no `off=`, no unnamed field; POST carries the body. | 19 | 2-3 min |
| wirecheck.py | screen | G(pc,256) e1000 + Server | S,H | `/bin/wiretest` passes each of its own checks; the server sees at least 5 requests on connections × 2 ≤ requests; cookie absent first, `sid=abc123` later. | 5 + N | 1-2 min |
| bootcheck.py | screen | G(pc,256) with a 64 MiB disk; a bare BIOS `qemu -drive … -m 256 -display none -monitor stdio` with no `-kernel`; a second G reusing the disk | S,P,H | Formatted boot sector has a valid jump and code; BIOS shows the sector's message; a sector broken the old way is repaired on mount; the file survives; BIOS boots again. | 11 | 2-3 min |
| findcheck.py | screen | G(pc,256) e1000 + Server | S,P | Dock middle is bare; ctrl+f bar appears; `connection` is highlighted with `#FFE58F`; a miss lights nothing; ESC closes the bar. | 8 | 1.5-2 min |
| gamecheck.py | screen | G(pc,256) | S,M,P | Games row in the launcher; blackjack deals, stands (the dealer plays), chips come back; poker deals 2 cards and a flop after calls. | 8 | 2-3 min |
| tearcheck.py | screen | G(pc,256) | S,P | `halfdrawn` is on screen; red never appears in 14 samples; both green and mint are seen. | 3 | ~1 min |
| progcheck.py | screen | G(pc,256), 64 MiB | S | `cp /bin/hello /home/mine` runs by name (prints 5050); unknown names are refused listing /bin; a non-ELF file is refused by name. | 8 | ~1 min |
| crashcheck.py | screen | G(pc,256), 64 MiB, 15 boots | S | Every killed write leaves `CRASH_WHOLE A` or `B`; both appear across the rounds; the volume still lists `crash.dat` and is FAT. | 6 | 5-8 min |
| sdkcheck.py | screen | Host zig build, then mkfat 32 MiB, then G(pc,256) booting that image | S,H | Builds from 4 SDK files; size 4 KiB-200 KiB; `outside alpha beta` prints its pid, `1..100 5050` and `[outside] [alpha] [beta]`. | 7 | 1-2 min |
| libccheck.py | screen | The same, with sdk/libc | S,H | printf padding and bases, argv, qsort, strtol, sqrt, pow, sin, fprintf/fgets round trip. | 16 | 1-2 min |
| smpcheck.py | screen | G(pc,256) `-smp 4` | S | 4 CPUs in `/sys/cpu`; the BSP and at least one AP ran programs; 3 APs tick more than 200; `ps` and `exec /bin/hello` still work. | 7 | 1-2 min |
| appcheck.py | screen | G(q35,256) with xhci, usb-storage (TONE.WAV) and intel-hda | S,K,M,P,A | Calculator 78/4 display equals the display after typing 19.5; the monitor app notices `run count &`; the music player plays 440 Hz (±8%). | 8 | 2-3 min |
| shcheck.py | screen | G(pc,256) | S | `/bin/sh`: echo, not found, `>`, `>>` (wc 2 2), `<`, 2- and 3-stage pipes, ^C stops `spin` while the shell survives, pwd/cd, exit back to the kernel shell. | 19 | 1-2 min |
| setcheck.py | screen | G(pc,256) | S,K,M,P | `/sys/settings` (≥ 25 sane rows); `/sys/theme`; `dock_h 96` moves the dock; Settings opens from the launcher; the dock_brand toggle works both ways; `/zelr.cfg` contents. | 10 | 1.5-2 min |
| piccheck.py | screen | G(pc,256) | S | `PNGTEST_PASS`, `JPEGTEST_PASS`, `SVGTEST_PASS`, `LAYOUTTEST_PASS`, `PAGETEST_PASS` (all duplicates of ring3check). | 5 | 1-2 min |
| check_loader.py | build (bootloader/build.sh:25) | none | n/a | cdboot signature position, handoff magic, load address. | exit code | <1 s |
| check_sse.py | **none** | none | n/a | No SSE opcodes in executable sections. Not run by anything. | exit code | -- |
| shots.py | **none** (manual) | G(pc,192) e1000 + Server | S,K,M,P | Not a test. Writes 12 `docs/*.png`. | -- | 3-5 min |
| whereis.py | none (manual) | -- | -- | Address-to-symbol lookup; broken on the 64-bit ELF. | -- | -- |
| pipeline/selftest.sh | none (manual) | none | n/a | The cycle.sh state machine with fake agents. | 17 | seconds |

### 8.2 Verified run on this host (coordinator)

`bash run.sh -T` ran on the pc machine with `-m 64`, the persistent `zelr.img` (16 MiB, created by that run), `-netdev user -device rtl8139`, `-append selftest` and debug-exit.

The log is a local selftest log:
- **556 passed, 0 failed, `SELFTEST_PASS`**;
- 50 `[section]` headers, including `[sound]`;
- 3 SKIPs: `only one processor on this machine`, `no mcfg on this machine, legacy ports only`, `no sound controller`;
- boot log: 54640 KiB usable, heap 24 MiB, `video 1024x768 32bpp, set through vbe`, `disk QEMU HARDDISK via ata, 16 MiB`, `fs new disk prepared`, `net rtl8139`.

README.md says 552 checks and lists 49 sections with no `[sound]`, so it has drifted (section 10). The gate's own selftest invocation differs: `-m 256`, a fresh disk, and the default e1000 NIC.

### 8.3 What tests this area itself

- `pipeline/selftest.sh` covers only `cycle.sh`, with 17 checks (3.6). batch.sh, land.sh, loop.sh, preflight.sh, release.sh and models.sh have no tests. batch.sh cannot even run in fake-agent mode (section 10, J).
- `gate.sh` was checked by hand three ways, according to pipeline/README.md:141-145: a failing kernel check, a syntax error, and a known-good tree. There is no automated test of the gate.
- `harness.py` has no tests of its own. Its behaviour is exercised by every harness.
- The launcher has no tests at all. release.sh's `grep "$VER"` is the only automated check on the exe.

---

## 9. How to extend

**Adding a harness**
1. Create `tools/<name>check.py`.
2. `sys.path.insert(0, dirname(__file__))` and `from harness import Guest, Checks, build_once, ROOT`.
3. Name every file after the process: `os.path.join(ROOT, "<name>.%d.img" % os.getpid())`.
4. Call `build_once()` first. Use `Guest(..., memory=…, machine=…, extra=[…])` and `vm.wait_boot()`.
5. Decide with `c.add(label, cond, shot)`. End with `return c.report(keep="--keep" in sys.argv)` and `sys.exit(main())`. **The exit code is the contract.**
6. Clean up in `finally` blocks (`vm.stop()`, remove temporary files).
7. Wire it into `gate.sh`: define `<fn>() { keep timeout <N> python tools/<name>check.py; }` and add `par_start "<label>" <fn>` in the right tier block.
   - **Use a new, unique function name.** `par_wait` re-runs by function name, and a reused name makes the retry run the wrong test. `boottest` is already broken this way.
   - Add its artifact patterns to the `stale` list (gate.sh:618-620).
8. Add it to the harness list in README.md:374-394.

**Harness pitfalls the comments warn about**
- Never sleep for a fixed time. Wait for a condition with `wait_screen`, `wait_serial` or `wait_prompt(vm.prompts()+1)`. `wait_prompt()` with no count returns immediately after boot.
- Use `fresh()`, or fence between echoed markers, when checking that something is **absent**, or when the string you look for also appears in the typed command. The console echoes everything typed (mountcheck.py:75-78, usbcheck.py:258-265, blackbox_test.sh:11-17).
- Serial input is lossy. Type through `vm.type()` (50 ms per character), and use a gap of 0.18 s while the browser is running. While the desktop is up, serial input goes to the focused window, not the shell (netcheck.py:84-87).
- Get geometry from the kernel's arithmetic (wm.c, winsrv.c, theme defaults), not from remembered numbers. Use `face_width()` for text widths, and find things by colour (`centre_of`, `count_near` for translucent surfaces).
- Retry a gesture only when the screen shows it had no effect (`click_for`, `drag_for`). A keypad must not be retried key by key (appcheck.py:135-156). Retry menu gestures as a whole (shotcheck.py:223-247; shots.py:205-223).
- Park the pointer out of any region you compare byte for byte (browsercheck.py:90-107; deskcheck.py:225-230).
- Audio checks must end QEMU with monitor `quit` so the WAV header is completed, although `read_wav` tolerates zero lengths.
- Kernel-side strings and colours are contracts (5.3). Changing a boot-log line or a theme colour silently breaks harnesses.

**Adding a gate tier step with no machine** (a static check): use `run_step "<label>" <fn>` next to vercheck, abicheck and defaultcheck.

**Pipeline**
- Add backlog rows as `| id | author | title | todo |`. The id must match `[a-z0-9-]+`, and the title must not contain `|`. batch.sh's sed also cannot handle `&` or `\`.
- **Prune the stale todo rows first** (section 10). Pick `PROFILE` and models through the environment.
- Update the check counts and descriptions in `prompts/*.md` and `cycle.sh`'s commit template when the tests change.
- To test the pipeline, extend `selftest.sh` scenarios. They run in seconds.

**Release**
1. Bump `KERNEL_VERSION` in include/types.h and commit.
2. Run `gate.sh full`.
3. Push and create the tag.
4. Run `bash pipeline/release.sh vX.Y.Z notes.md`. This needs `gh` and `dotnet`.
5. `git fetch --tags` so check_version.py sees the new tag.

**Launcher**
- Instructions shown to the user live in MainWindow.xaml:54-92.
- QEMU arguments live in `Emulator.Start`. Consider adding `-accel whpx -accel tcg` or `-machine accel=whpx:tcg` as zelr.bat does, a per-user QEMU search path, and capturing stderr for errors.
- A starter disk would need an `EmbeddedResource` ending in `starter.img.gz` in the csproj (EnsureDisk already handles it).

---

## 10. Doc drift and suspicious code

All of the following were verified by reading. Anything that depends on runtime behaviour says so.

### 10.1 Suspected bugs and weak tests, most serious first

- **A. gate.sh `boottest` name collision (full mode).**
  - `boottest` is defined at gate.sh:401 as `iso_test.sh` and started at 402. It is then **redefined** at 519 as `bootcheck.py`.
  - `par_start` records only the function name (128), and `par_wait` re-runs failed steps as `${again_cmds[$i]}` (177).
  - So if "all four boot paths" fails in the parallel run, the "alone" retry runs **bootcheck.py**. A genuine ISO/UEFI boot regression can then be reported as `PASS  all four boot paths, alone`.
- **B. Build failure is detected by a word, not an exit status** (gate.sh:227-235).
  - `grep -qE '\berror\b'` catches compiler and linker `error:` lines.
  - It misses build.sh failures that print no lowercase "error":
    - `zig not found; set ZIG=/path/to/zig` (build.sh:18, exit 1);
    - Python tracebacks from `loadaddr.py` or `flatten.py`, e.g. `FileNotFoundError` does not match `\berror\b`.
  - The gate then tests the stale `build/zelr.bin`. This is exactly what 222-226 says must not happen, and it contradicts the gate's own "decide on the exit status" rule (205-211).
- **C. The kernel image is rewritten in parallel in `full`.**
  - `mkiso.py` always runs `flatten.py … build/zelr.bin` (mkiso.py:312-317), even when `ZELR_PREBUILT=1` (which only skips build.sh at 306-310).
  - In `full`, `iso_test.sh` (and so mkiso) is the first step of group C. shotcheck, termcheck and deskcheck start at almost the same moment and boot QEMU with `-kernel build/zelr.bin`.
  - The rewrite is byte-identical, but `open(dst,"wb")` truncates first, so a QEMU loading during that window can read a short image. mkiso's own comment says this is a Windows permission error.
  - This is a likely source of flaky first-run failures, and the retry-alone would mask it. It cannot be confirmed without running.
- **D. batch.sh's per-task fast gate gates the wrong tree.**
  - `( cd "$wt" && bash "$PIPE/gate.sh" fast )` (batch.sh:110, 161): `PIPE=$ROOT/pipeline` was fixed at lib.sh:12 to the main checkout, and gate.sh:31 `cd`s to its own parent. So the **main checkout** is built and tested, not the task's worktree.
  - With `BATCH>1`, overlapping fast gates (and agents running the gate in their worktrees, as the prompts tell them) hit the global lock, **exit 2**, and the task is marked `blocked`.
  - This is probably why `BATCH` defaults to 1, while the header and README still say three.
- **E. batch.sh cannot see an author who only added files** (batch.sh:106). It uses `git diff --quiet`, which ignores untracked files. This is the exact bug cycle.sh fixed and documented at cycle.sh:112-118.
- **F. `loop.sh` can land at most one task per run.**
  - cycle.sh finishes with `backlog_set … done` (215) or, in `abandon`, `backlog_set … blocked` (75). Both leave pipeline/backlog.md **modified and uncommitted on main**; nothing commits it.
  - The next `cycle.sh` dies at 59-61 ("uncommitted changes"). loop.sh counts that as "blocked" and stops after two of them.
  - selftest.sh does not catch this because each scenario uses a fresh repo and a single cycle.
- **G. cycle.sh's review-answer step.**
  - Runs the author **without** `codex_args_for address` or `claude_args_for` (167-171), so the CLI default model and effort are used. batch.sh passes them (154-156).
  - `MAX_REPAIRS=2` is dead because the loop always breaks after one round (186).
  - The high-finding recount reads the unchanged findings.json (180-185), and the commit message's "N finding(s) answered" uses that number.
  - cycle.sh answers findings of **any** severity (158); batch.sh answers only `high` (141).
- **H.** `ANSWER_FINDINGS` (models.sh:74) is never read.
- **I.** batch.sh prints who writes and reviews from the backlog column, ignoring `PROFILE` (57). The default `balanced` profile actually forces codex to write and claude to review.
- **J.** batch.sh never exports `FAKE_ROLE`. With `FAKE_AGENTS` set, `lib.sh:68/83` would reference an unset variable under `set -u`, so batch.sh cannot be run with scripted agents. Only cycle.sh is covered by selftest.sh.
- **K. An unreadable review still lands.**
  - parse_review produces `{"verdict":"unreadable","findings":[]}`. `N_FIND` becomes 0, there is no answer round, and the full gate lands the change (cycle.sh:149-196; batch.sh:133-141).
  - selftest scenario 5 only checks that the word `unreadable` was logged.
  - This contradicts parse_review.py's intent (lines 3-6, 58-61: "a review that cannot be read is not a pass").
  - Also, `first_object` gives up if the first `{` in the text is not the JSON object (for example braces in prose before it).
- **L. Untracked files in the main checkout.**
  - cycle.sh's clean check ignores untracked files (59-61), but `abandon` runs `git clean -qfd` (72), deleting a person's untracked, non-ignored files. The landing `git add -A` (200) commits them.
  - batch.sh's bookkeeping commits on main use `git add -A` (202, 229, 250) and would commit leftovers from the full gate, which runs in the main checkout.
  - `*.wav`, `mountseed.*.txt`, `fat32probe.*.txt` and `fat32high.*.txt` are **not** in .gitignore. They are left behind whenever `timeout` kills a harness, because Python `finally` blocks do not run on SIGTERM/TerminateProcess (runtime behaviour, inferred).
- **M.** preflight.sh:45-46 accepts `[ -x "$CLAUDE" ]` but then probes with the PATH `claude`.
- **N. Tests that cannot fail because of echo.**
  - The console echoes typed characters (kernel/shell.c:626-628 → printf.c:16-24). In shell_test.sh these 4 of 17 checks match the **echo of the typed command** and cannot fail while echo works:
    - `notes` "ls shows the seeded files" (103-104; matched by `write notes.txt …`);
    - `shell wrote this` (106);
    - `nested file` (109);
    - `done testing` (119).
  - iso_test.sh's fifth check per path, `$what` (69), matches the echoed `write booted.txt $what`, so it does not prove the write/cat round trip that its comment at 44-45 claims.
- **O.** termcheck's last check (140-142) waits for AMBER, which was already on screen. It proves only that the terminal survived, not that `/cfg/term` was written or read.
- **P. `vm.wait_prompt()` with the default `count=1` after pressing ESC** is a no-op, because the prompt count has been at least 1 since boot. This appears in framecheck.py:67-68, netcheck.py:101-104 (`leave_desktop`) and setcheck.py:263-265, which therefore rely on fixed sleeps. framecheck also names its `wait_screen` lambda `(px, w, h)` (56); this is harmless because it returns True.
- **Q.** tlscheck's "the body is really X's" (138-140) passes on `marker in out or body > 400`, so the marker is optional.
- **R.** whereis.py asserts a 32-bit ELF (line 8). `build/zelr.elf` is x86-64 (built by build.sh with `-target x86_64-freestanding-none`), so the tool is unusable on the kernel.
- **S.** check_sse.py is not called by build.sh or by any `*/build.sh` (grep). README.md:1431 says it "now fails the build". The real protection is build.sh's `-mno-sse -mno-sse2 -mno-mmx -mno-80387` (build.sh:45).
- **T. The gate's tidy step is incomplete.**
  - `stale()` knows 10 patterns (618-620). Pid-named artifacts it never sweeps include `ring3.*.img`, `soundcheck.*.{img,wav}`, `enscheck.*`, `volcheck.*`, `apptone`/`apprec.*.wav`, `usbhub`/`usbhot`/`usbstick`/`stick.*.img`, `deskauto.*.img`, `mountseed.*.txt`, and others.
  - The `rm -f` of fixed names at 617 (`deskcheck.img`, `sel.img`, …) is mostly for names no longer produced.
- **U.** crashcheck's "both versions turn up" check (129-131) is probabilistic: 6 random kill times between 0.15 and 2.5 s. If one write cycle ever takes longer than the kill window, every round reads `A` and the check fails with the machine working.
- **V. Fixed sleeps remain** despite harness.py's philosophy. Examples: appcheck.py:171, 176, 220, 251 (12 s); setcheck.py:153 (8 s), 202; netcheck.py:135, 161 (9 s); browsercheck.py:229, 233 (14 s); livecheck.py:81, 86, 109 (16 s); formcheck.py:166, 170; findcheck.py:77, 81, 97, 110, 127; gamecheck.py:96, 118, 225, 227; tearcheck.py:45, 49. These harnesses are load-sensitive and rely on the gate's retry-alone.
- **W. Launcher.**
  1. There is no hardware acceleration: pure TCG. zelr.bat:47-60 says this makes the selftest about 5× slower (44 s against 9 s).
  2. `EnsureDisk` **recreates an existing `disk.img` smaller than 32 MiB** (122, then `File.Create` at 132 or `FileMode.Create` at 140), despite the "An existing disk is never touched" comment (112-118).
  3. QEMU errors are invisible (`CreateNoWindow`, stderr not captured). An immediate QEMU failure reads as "zelr closed".
  4. `StatusDot` stays Warn after a failed start even once a later start succeeds.
  5. The per-user QEMU problem in 5.5: an in-app winget install to `%LocalAppData%\Programs\qemu` is not detected until restart, and the registry fallback reads HKLM only.
  6. The "type these" list is stale (10.2).
- **X. release.sh.**
  1. No gate run and no clean-tree check.
  2. `gh release create TAG` creates the tag on the remote default branch if it is missing, so the uploaded artifacts can come from a different commit than the tag.
  3. `grep -q "$VER"` treats the version as a regex.
  4. Upload uses `--clobber`, so existing assets are replaced silently.
- **Y.** review.md:17 tells the reviewer to run `git diff` on an **uncommitted** tree, which does not show new untracked files. New files are invisible to the review unless the reviewer uses `git status`.
- **Z.** batch.sh:94 builds the prompt with `sed "s|{{TASK}}|$title|"`. A title containing `|`, `&` or `\` corrupts it. cycle.sh uses a Python replacement.
- **AA. gate.sh's signal handling.** `trap 'rm -rf "$LOCK"' EXIT INT TERM` has no `exit` in the handler. In bash, a handled INT or TERM **resumes the script** after the handler runs. So Ctrl-C, or a TERM from an outer `timeout`, removes the lock and the gate keeps running remaining steps, now unlocked. This is inferred from bash semantics and was not run.
- **AB. defaultcheck.py passes vacuously.**
  - settings.c no longer holds per-setting defaults. It declares `static int light = 1, look = 0, preset = 1, custom = 0;` on one line (settings.c:54), which the per-line regex at defaultcheck.py:88 cannot match. `save()` writes through `line_num`/`line_hex` (settings.c:197-225), not `put_kv`.
  - kernel/theme.c's `theme_init` (324-342) sets only `look`, `light` and the preset; everything else comes from the `KNOBS` table (theme.c:88-131, `theme_defaults` at 351-353).
  - Result: `settings_defaults()` returns `{}` and `settings_saved_keys()` returns an empty set, so no key is compared. Of the 3 checks, only "the wallpaper enum could be read" tests anything.
  - The original drift it guarded against is now designed out (settings.c:15-29), but the check reports a green result for nothing.
- **AC.** appcheck.py:57 still uses `ICON_LEFT, ICON_CELL_W = 14, 78`. wm.c:855-858 with the theme defaults (theme.c:112-113) gives 18 and 92, which deskcheck, browsercheck and formcheck use, and browsercheck.py:39-43 documents the change. The calculator clicks land about 18 px left of each key centre. The keys are about 61 px wide, so this probably still passes, with a reduced margin.
- **AD. Orphans after `timeout`.** A harness killed by the gate's `timeout` leaves its QEMU child running and its files behind (runtime inference). The next gate's `ps -W` note will count those QEMUs.
- **AE. Per-user firmware.** zelr.bat `modern` cannot find the firmware of a per-user QEMU (zelr.bat:113-119) and silently falls back to BIOS. iso_test.sh without `ZELR_UEFI_FW` skips UEFI and the gate still passes (5.5).
- **AF. QEMU short-form booleans.** The harness monitor argument `tcp:127.0.0.1:N,server,nowait` (harness.py:624) uses short-form booleans, which QEMU has deprecated since 6.0 in favour of `server=on,wait=off`. I could not check whether QEMU 11.1 still accepts them. If it does not, every monitor-driven harness fails at connect. The coordinator's selftest run used no monitor.

### 10.2 Documentation that disagrees with the code

1. **gate.sh header**, lines 9-14:
   - `fast` also runs check_version, abicheck, defaultcheck and ring3check.
   - `full` also runs clipcheck and the NVMe selftest.
   - `screen` includes network, web, TLS (internet), sound, SDK, crash and so on, not only "desktop, terminal, windows, USB, input".
   - The `~5 min` for `screen` and `~17 min` for `full` are implausible for 32 or 33 harnesses at `PAR_MAX=4`.
   - Line 107 says "the last group starts thirteen"; it starts 32 or 33.
   - Lines 25-26 say "four of full's steps … all four are about disks"; full-only has 6 steps, including clipcheck.
   - Line 544 says crashcheck is "Six boots"; it is 15.
2. **pipeline/README.md**:
   - Lines 47-52 and 69: `batch.sh` is "three tasks at once"; `BATCH` defaults to 1.
   - Lines 58-63: the fast gate is "a build and two QEMU runs with no monitor port" and runs "in each worktree". It is five QEMU steps, Guests always open a monitor port, and it actually gates the main checkout (10.1 D).
   - Line 82: "234 self checks … About five minutes"; the selftest is about 556 checks.
   - Lines 89-91: full is "three harnesses … About thirty-five minutes"; the gate header says 17 minutes, and it is about 30 harnesses.
   - Lines 100-112: the model table (opus / sol / high) is the `max` profile. The default is `balanced` (sonnet, sol, medium/medium/low, codex writes and claude reviews regardless of the backlog), so "they swap by task" (21) is false by default.
   - Lines 165-168: "Nothing that reaches the network" is not enforced for claude's allowed `Bash`.
   - The Files list (202-219) omits `land.sh` and the `screen` mode.
   - Lines 54-56: the cycle "takes about forty minutes, thirty-five of which is the full gate".
3. **batch.sh header**: lines 22-24 ("three tasks"); lines 16-18 ("shotcheck, termcheck and deskcheck each drive QEMU's monitor on a fixed port", which is no longer true since `free_port`).
4. **cycle.sh**: the commit template (207-209) says "234 kernel checks … the three harnesses that drive the desktop".
5. **Prompts**:
   - implement.md says "FAT16 driver" (4), "213 checks" and "three harnesses" (19-21, 41), and "about five minutes" (42).
   - review.md says "213 self checks" (21) and "The window manager runs in one task and programs run in others, with no locks" (31-33). The kernel is now SMP with a big kernel lock.
6. **backlog.md**. Rows marked blocked or todo describe things that exist:
   - `rtc-clock` (blocked): kernel/rtc.c exists.
   - `pipes` (blocked): kernel/pipe.c exists.
   - `lfn-read` (blocked): LFN works, and namecheck tests it.
   - `editor` (todo): userland/notes.c exists.
   - `nvme` (todo): kernel/nvme.c and nvme_test.sh exist.
   - `demand-pages` (todo): the file's own Ideas section says anonymous mmap pages "arrive as they are touched", and maptest covers it.
   - `xhci-hid` (todo): kernel/xhci.c, usb.c and usbcheck exist.
   - Preflight would pass with "4 task(s) to do", and the pipeline would start re-implementing an editor.
7. **harness.py docstrings** say "Three of them boot zelr" (3) and "Identical in all three harnesses" (776-777). 37 files import it.
8. **ring3check.py docstring** says "six things", "All five" and "One machine for all five" (1-9). There are 19 suites.
9. **deskcheck.py docstring** (8-12) gives "x=40 y=36 … outer frame is 762x505 … buttons 26, 46 and 66 pixels in from the right edge". The code computes x=124, an outer frame of 762x514, and button centres 18, 50 and 82 px in from the right edge (107-128).
10. **README.md:396-399**: the launcher "embeds `build/zelr.elf` and a starter disk". The csproj embeds only `build/zelr.bin` (csproj:30-33), and no starter image is produced or embedded.
11. **README.md:82**: "The kernel inside it is about 1.5 MB". `build/zelr.bin` is 9,971,400 bytes and `zelr.elf` is 11,271,744 bytes; flatten zero-fills up to `p_memsz`.
12. **README.md:93**: "Type `guide` when you get there". The machine opens the desktop, and its ring-3 terminal has no `guide` command (userland/term.c:1315-1354). `guide` exists only in the kernel console after pressing ESC (kernel/shell.c:299).
13. **README.md:1330-1360**: "552 checks" and a table of 49 sections. The verified run gives 556 and 50 sections (`[sound]` is missing from the table). The `-smp 4` figure of 563 and q35 figure of 560 (1373, 1376) could not be verified.
14. **README.md:1403**: iso_test is "the only one that does not use QEMU's -kernel". bootcheck.py's bare-BIOS boot does not use it either.
15. **README.md:1411-1416**: shotcheck "draw[s] a stroke in paint". The current shotcheck opens Settings and changes the accent. Paint strokes are in shots.py.
16. **README.md:1428-1432**: "Fixed with -mcpu=i686, and tools/check_sse.py now fails the build". check_sse is unwired (10.1 S), and `-mcpu=i686` belongs to the 32-bit era; build.sh uses `-mno-sse…`.
17. **README.md:374-394**: the harness list omits ring3check, clipcheck, the gpt/fat32/nvme/blackbox tests, webcheck, tlscheck, browsercheck, livecheck, formcheck, wirecheck, bootcheck, findcheck, gamecheck, tearcheck, progcheck, crashcheck, sdkcheck, libccheck, smpcheck and shcheck.
18. **MainWindow.xaml:54-92**, the "type these" list:
    - `exec hello.elf` and `bg count.elf` name files that do not exist. /bin programs have no extension (kernel/builtin.c:67-70), and the shell starts in /home.
    - `dhcp` is unnecessary; an address is requested at boot (netcheck.py:121-129).
    - `fetch` is a kernel-console command; the desktop terminal calls it `get`.
    - The whole list targets the kernel console, while the launched machine opens the desktop (no `-append`).
    - The window also promises a "FAT16 disk image". That is plausible for a 32 MiB disk, but it is decided by the kernel.
19. **zelr.bat:111-112**: "zelr has no USB stack yet". xHCI, HID, storage and usb-net all exist (usbcheck, mountcheck, netcheck).
20. **mkiso.py:31**: the kernel "loads at 1 MiB". It uses `load_address()`, which gives 16 MiB (linker.ld:33 `. = 16M;`).
21. **crashcheck.py:31**: the usage says `[--rounds N]`, but the parser only accepts `--rounds=N` (57-59).
22. **gate.sh:16-18**: "Each boots its own machine and builds its own disk, named after its own process id". The gate's own selftests use the fixed names `gate.img`, `gateq.img` and `gatenv.img`, and iso_test uses `build/isotest.img`.

---

## 11. Open questions

1. Does .NET honour `ProcessStartInfo.ArgumentList` when `UseShellExecute=true` (Emulator.cs:174-183)? If it ignores the list in that mode, winget would run with no arguments.
2. How long do `gate.sh screen` and `full` really take on this host with QEMU 11.1 (TCG only; no script enables WHPX except zelr.bat)? Is `PAR_MAX=4` right for this machine?
3. Is HMP `screendump` still synchronous in QEMU 11.1, i.e. does the prompt return only after the file is complete? harness.py:553-568 depends on this, and has only a size check after the fact. Does 11.1 still accept the short-form `server,nowait`?
4. Does winget's `SoftwareFreedomConservancy.QEMU` install per user or per machine by default, and does it prompt for elevation? README.md:96 says no administrator rights are needed. On this host the result was per user, which the launcher cannot detect without a restart.
5. Is the backlog meant to drive work any more, or has development moved away from the pipeline? Every todo row appears to be done, and there is no `pipeline/state/` in this copy, so no runs are recorded here.
6. Was `BATCH` changed from 3 to 1 because of the global gate lock (10.1 D)? Nothing records why.
7. `zelr.bat test` runs the selftest on the user's persistent `zelr.img`, and `run.sh -T` does the same with the repo-root `zelr.img`. The gate insists on fresh disks. Can the selftest's disk and FAT sections damage a user's files on that image?
8. Should defaultcheck.py be retired, or rewritten to compare `KNOBS` defaults with what Settings shows via `/sys/settings` (10.1 AB)?
9. Are `spawntest` (`/bin/spawntest`, listed in the welcome text) and `jsprobe` meant to be covered by a harness? Neither is run anywhere.
10. Would crashcheck's "both versions" check be deterministic if the kill window were derived from a measured write time (10.1 U)?
