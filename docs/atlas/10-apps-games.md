# Atlas area 10: desktop applications and card games (ring 3)

Source: the repository root (blavese/zelr `main`, 2026-09-22, two commits after v0.37.0).
Static reading only. Nothing was built or run. Every `file:line` below refers to that tree.
The PIT runs at 100 Hz (`kernel/main.c:459 timer_init(100)`), so **1 tick = 10 ms** wherever a constant is in ticks.

---

## 1. Scope

| File | Lines | Role |
|---|---:|---|
| `userland/files.c` | 700 | File manager ("Files"): Places sidebar, toolbar, File/Edit/View/Help menu bar, context menu, filter, in-place rename, open-by-name mapping to other programs |
| `userland/settings.c` | 843 | Settings: 10 pages. Generic controls built from `/sys/settings`. Writes `/zelr.cfg` |
| `userland/notes.c` | 361 | Text editor ("Notes"), one flat 64 KiB buffer |
| `userland/paint.c` | 235 | Paint: 16 colours, 4 brush sizes, Bresenham strokes. Draws everything itself; does not use ui.h |
| `userland/music.c` | 446 | WAV player: RIFF chunk walk, 16.16 nearest-sample resampler, mono duplication, built-in tune |
| `userland/calc.c` | 322 | Calculator in `i64` millionths (no FPU in this system) |
| `userland/monitor.c` | 365 | System monitor: CPU %, memory, task count, uptime, 30-second graph, task list, Stop button |
| `userland/blackjack.c` | 748 | Blackjack: 6-deck shoe, insurance, split to 4 hands, late surrender |
| `userland/poker.c` | 927 | No-limit Texas hold'em against 3 AI opponents, with side pots |
| `userland/poker.h` | 173 | Header only: best-5-of-7 hand evaluator and hand names |
| `userland/cards.h` | 314 | Header only: card encoding, xorshift RNG, Fisher-Yates shuffle, drawn cards, `money()` |
| `userland/cardtest.c` | 264 | Headless test of poker.h, cards.h and blackjack arithmetic (56 checks) |
| `tools/appcheck.py` | 281 | QEMU harness for the calculator, the monitor and the music player |
| `tools/gamecheck.py` | 276 | QEMU harness for the launcher's Games row, blackjack and poker |

Context files, summarised in §3.15 only (other agents document them): `userland/ui.h` (832), `userland/draw.h` (231), `sdk/zelr.h` (983).

I also read, without documenting them in depth: `tools/setcheck.py`, `tools/defaultcheck.py`, `tools/shotcheck.py`, `tools/ring3check.py`, `tools/deskcheck.py` (Paint part), `kernel/theme.c`, `kernel/sysfs.c` (render_settings/theme/screen), `kernel/wm.c` (launcher, desktop icons, key delivery, theme reload, apply_screen_size), `kernel/sound.c`, `kernel/syscall.c` (kill/tasks/sound/win_text/sysinfo), `kernel/sched.c` (slices/idle), `kernel/idt.c`, `kernel/builtin.{S,c}`, `kernel/pins.c`, `kernel/keyboard.c`, `kernel/signal.c`, `kernel/vfs.c`, `kernel/fat.c` (delete), `kernel/layout.c`, `pipeline/backlog.md`, and README.md.

(Line counts are physical lines. PowerShell's `Measure-Object -Line` skips blank lines and reports smaller numbers.)

---

## 2. Big picture

### 2.1 What these are and how they get onto the machine
- Each `.c` file here is a standalone static ELF. `userland/build.sh` compiles **every** `userland/*.c` with `zig cc -target x86_64-freestanding-none -O2 -std=gnu11 -ffreestanding -nostdlib -static -fno-builtin -fno-pic -fno-pie -mcmodel=large -mno-red-zone -I../sdk -T ../sdk/zelr.ld` into `build/user/<name>.elf` (build.sh:25-37).
- `kernel/builtin.S` pastes each ELF into the kernel image with `.incbin "build/user/<name>.elf"` (for example blackjack at :250-254, poker at :257-261, cardtest at :264-268). `kernel/builtin.c:80-86, 104-106` registers the names `settings, paint, files, notes, monitor, calc, music, blackjack, poker, cardtest`. They then appear read-only as `/bin/<name>` (sysfs; the selftest proves `/bin/paint` cannot be overwritten or deleted, `kernel/selftest.c:895-903`).
- The only interface to the kernel is `sdk/zelr.h` (int 0x80 wrappers and inline "sugar"). There is no libc, no heap use (none of these call `sbrk`/`alloc.h`) and no floating point. All state is static arrays.

### 2.2 How they are started
| Entry point | What it starts |
|---|---|
| Launcher categories (`kernel/wm.c:393-438`) | Productivity: Terminal, **Files, Notes, Calculator**. Media: **Paint, Music**. System: **Settings, Monitor**, System info. Games: **Blackjack, Poker** (the Games row "did not exist until it had two things under it", wm.c:421-425) |
| Desktop icons (`wm.c:868-878`) | Terminal, Files, Notes, Paint, Settings |
| Desktop right-click menu (`wm.c:371-381`) | "Browse files" → /bin/files, "Personalise" → /bin/settings |
| Default dock pins (`kernel/pins.c:19-26`) | Terminal, Files, Notes, Paint, Settings, Browser |
| Console shell (`kernel/shell.c:257-290` run_by_name, `:304-333` bg/exec) | Any name, searched in cwd, then /usb, then /bin, with the full argv (so `music /usb/TONE.WAV` works) |
| Ring-3 terminal (`userland/term.c:915-917`) | `spawnv(path, argv)` |
| Files itself (`files.c:227-258`) | notes / music with a path, or any /bin program with no argument |

Three programs read an argument: **files** (a folder), **notes** (a file) and **music** (a WAV). All the others declare `main(void)` and ignore argv.

### 2.3 The common skeleton (every app except paint and cardtest)
```
win = win_create(title, w, h); win_allow_resize(win);
for (;;) {
    w,h = win_width/height(win); px = win_surface(win); if (!px||w<=0||h<=0) break;
    t = ui_load_theme();                 // every frame (notes: once at startup)
    ui_begin(&in); while (win_poll(win,&ev)) { if CLOSE -> break out; ui_feed(&in,&ev); }
    ...keys (in.key only)... ...draw + immediate-mode widgets...
    win_commit(win); sleep_ms(16);       // music 4/30 ms, monitor 60 ms
}
win_close(win); exit(0);
```
- Nothing handles `WIN_EV_RESIZE` explicitly. A resize is absorbed because width, height and surface are re-read every frame.
- `ui_load_theme()` slurps `/zelr.cfg` (first 1023 bytes) and `/sys/theme` each time it is called (ui.h:233-271). So every ui.h app except notes does **two file reads per frame**, which gives it the live theme.

### 2.4 Design decisions and the reasons the comments give
- **No privileged path.** Files "goes through the ordinary file syscalls… nothing it can do that a program written by anyone else could not" (files.c:7-9). Monitor likewise (monitor.c:6-7).
- **Opening = starting the owning program with the path.** The type is decided by name and location, never by content: "a guess that disagrees with the name is a guess nobody can correct" (files.c:114-116).
- **Settings only writes a file.** "This program cannot reach into the window manager… What it can do is write a file". The WM re-reads it four times a second. The file is plain text so that the shell's `write` can do everything the window can (settings.c:1-13).
- **Settings no longer knows the list of settings.** Past costs of a hand-kept list are recorded: a default written twice, a key deleted on save, a control writing a key nothing parsed. The list now lives in `kernel/theme.c` and arrives through `/sys/settings` with range and default (settings.c:15-29; theme.c:69-85).
- **Flat editor buffer.** One char array with newlines. Insertion is O(n), but load is one read and save is one write (notes.c:7-11).
- **Integer calculator.** "a program that used a double would fault", so everything is i64 millionths. What is shown is "a string being typed, not a number being edited" (calc.c:1-12).
- **Nearest-sample resampling at a 16.16 ratio.** No FPU, and "the difference is audible only to somebody listening for it" (music.c:9-14).
- **Paint shares nothing with the kernel.** It has its own primitives (paint.c:1-9).
- **The games write their rules down.** "a game that does not say which it made is a game you cannot tell is wrong" (blackjack.c:1-21; poker.c:11-28). The poker opponents "are not very good. They are honest": no function takes another seat's hand (poker.c:4-9).
- **Shared card code** so that a card looks the same in both games (cards.h:4-6). Cards are drawn from discs and triangles, never loaded as images (cards.h:8-11).
- **Evaluator correctness over speed.** It enumerates all C(7,5)=21 subsets and packs a hand into one comparable integer (poker.h:3-18).
- **RNG seeding.** Seeded from the clock and then stirred by every click and key, because a machine that boots in four seconds would otherwise deal the same game (cards.h:58-72).
- **Whole-unit money.** A 3:2 payout on an odd bet rounds down "in the house's favour… silently rounding a payout up would be a generous bug" (cards.h:288-294; blackjack.c:14-16).

---

## 3. File-by-file detail

### 3.1 `userland/files.c`: "Files"

**Constants.**
- `MAX_ENTRIES 512`, `NAME_MAX 64` ("as wide as the kernel's own names"), `PATH_MAX 256` (:19-21)
- `KIND_DIR 0, KIND_TEXT 1, KIND_SOUND 2, KIND_PROG 3, KIND_OTHER 4` (:24-28)
- `TOOLBAR_H 40, CRUMB_H 28, SIDE_W 124` (:303-305)

**Types and globals.**
- `entry { char name[64]; u32 size; u32 is_dir; int kind; }` (:30-35)
- `entries[512]`, `count` (after the filter), `total` (everything readdir returned), `total_bytes` (sum of file sizes), `cwd[256]="/home"`, `selected=-1`, `first_row`, `status[128]`, `clip_path[256]`, `clip_is_cut`, `filter_buf[32]`, `filter_field` (:37-51)
- Menu bar state: `bar_open=-1`, `bar_x[4]` (:79-80)
- Locals in `main`: `renaming` (row index or -1), `rename_buf[64]`, `rename_field`, `menu_open`, `menu_x`, `menu_y`, `last_filter_len` (:358-366)

**Window and layout.** `win_create("Files", 780, 520)`, resizable (:340-342).

| Region | Position |
|---|---|
| Menu bar | y 0..20 (`UI_MENUBAR_H`) |
| Toolbar | y 20..60 with a groove |
| Sidebar | x 0..124: label "PLACES" at y 68; Places rows at `y = 88 + 28·i`, x 2..120, height 26 (:443-452) |
| Path "crumb" well | y 60..88, right of the sidebar |
| List well | y 88..h-26 |
| Status bar | h-26..h |

**Toolbar** (buttons at y 25, :461-476):
- `Up` (x 132, w 48) calls `go_up`
- `Open` (w 60) calls `open_entry(selected)`
- `New folder` (w 86) calls `mkdir(cwd+"/new folder")` ("could not create" if it exists; there is no numbering)
- `Paste` (w 60) calls `do_paste`
- Filter field: 150 px, placeholder "filter", at the right end, drawn only when the content width `cw > 460` (:479-482)

**Places** (:315-323): Home `/home`, Documents `/doc`, Programs `/bin`, System `/sys`, Temp `/tmp`, Stick `/usb`, Root `/`. The row whose path equals `cwd` is highlighted. /usb is always listed, "because a list that changes shape under the pointer is worse".

**List rows** (:518-576).
- Each row shows: a 10×12 tag in the kind colour (`kind_tint` :329-337: dir = accent, text = mix(fg,accent,60), sound `0x9A86E8`, program `0xE0A03C`, other = dim), the name, the kind word ("folder/text/sound/program/file"; only if `cw > 380`), and the size (`N K` if ≥1024, else `N B`; none for folders).
- A click selects. A click on the already-selected row opens it (there is no double-click timing).
- A right click selects the row and opens the context menu at the pointer.
- The wheel scrolls 3 rows per step. An empty list shows "(empty)", or "nothing matches" when a filter is set.

**Sorting** (`sort_entries` :148-163): insertion sort, directories first, then `strcmp` (byte order, so capitals sort before lower case). The reason given: FAT readdir returns creation order.

**Filter** (`matches` :168-182): case-insensitive substring. It is re-applied (via `reload`) whenever the field length changes (:416-419). Esc clears and unfocuses it (:403-404). Any navigation (`go_to`) clears it (:210-212). Every reload also resets `selected=-1` and `first_row=0` (:188-189).

**Keyboard** (:385-413; only `in.key`, the first key of the frame):
- While renaming: Enter commits, Esc cancels, everything else goes to the rename field.
- While the filter is focused: Esc clears it, everything else goes to the filter (so Enter does not open anything).
- Otherwise: Up/Down move the selection, Enter opens, Backspace goes to the parent directory, Delete calls `do_delete`.
- There are no Ctrl shortcuts.

**Menu bar** (:63-77, :426-438, :610-660).
- Titles `File, Edit, View, Help`. Items and widths:

  | Menu | Items | Width |
  |---|---|---:|
  | File | Open, New folder, Rename, Delete, Close | 130 |
  | Edit | Copy, Cut, Paste | 110 |
  | View | Refresh, Home, Root | 110 |
  | Help | About Files | 130 |

- A click on a title toggles it. Hovering another title while one is open switches to it (:432-433). A release anywhere else closes it (:656-659). The program, not the widget, owns which title is open "so it can be driven by a keyboard later" (:60-62); no keyboard driving exists yet.
- Actions:
  - File: Open → `open_entry`. New folder → `mkdir`. Rename → in-place field. **Delete → `unlink` even for folders** (:633-637). **Close → sets a local that is never read again** (bug, §10).
  - Edit: Copy/Cut store `clip_path` only (no system clipboard). Paste → `do_paste`.
  - View: Refresh → `reload`. Home → `/home`. Root → `/`.
  - Help: status text "Files, a ring 3 program".

**Context menu** (:307-310, :662-692): `Open, Edit, Play, Copy, Cut, Rename, Delete`, width 150.
- Open → `open_entry`. Edit → `open_with(notes)`. Play → `open_with(music)`.
- Copy → `clip_path`, **plus `clip_set(path)`**, status "copied a path".
- Cut → `clip_path` with `clip_is_cut=1`. Rename → in-place field. Delete → `do_delete`.
- It closes on any left press (bug, §10).

**Open-by-name mapping** (`kind_of` :117-126, `open_entry` :236-258, `open_with` :227-234):

| Condition (checked in this order) | Action |
|---|---|
| Directory | navigate into it (`go_to`) |
| Current directory starts with `"/bin"` (`strncmp(dir,"/bin",4)`) | `spawn(path)`: runs it with no argument. Status "started" or "would not run" |
| Name ends `.wav` | `spawn_arg("/bin/music", path)` |
| `.txt .md .cfg .log .c .h .sh` | `spawn_arg("/bin/notes", path)` (KIND_TEXT) |
| Anything else (KIND_OTHER) | also `/bin/notes` (the `default:` branch) |

`ends_with` lowercases only the name side and needs the name to be longer than the extension (:103-112). `spawn_arg` builds `argv = {program, path, NULL}` and calls `SYS_SPAWN_ARGV` (zelr.h:535-541).

**Copy, move, rename, delete.**
- `copy_file` (:264-269) slurps into a **static 64 KiB buffer** and then `spit`s it. The comment says there is no copy syscall "and adding one would put a loop in the kernel that belongs here".
- `do_paste` (:271-288): destination is `cwd/leaf`. It refuses "already here" when the destination equals the source. It silently overwrites an existing file. For a cut, it unlinks the source afterwards and clears `clip_path`.
- Rename (:385-395): `copy_file(from,to)` then `unlink(from)`. It does **not** use `SYS_RENAME`/`rename()`, which exists (zelr.h:727-735) but only works within one directory for 8.3 names.
- `do_delete` (:290-299): `rmdir` for a directory, reporting "directory is not empty" on any failure; `unlink` for a file.

**Status bar** (:581-600).
- Left: the last `say()` message if there is one, otherwise `cwd`. While no message is set, it shows "name - kind" of the selection.
- Right: "N item(s), M K" for the unfiltered directory.

**Argument.** A non-empty `argv[1]` means `go_to(argv[1])` (:352).

**Syscalls.**
- Windows: `win_create`, `win_allow_resize`, `win_width`, `win_height`, `win_surface`, `win_poll`, `win_commit`, `win_close`.
- Files: `readdir`, `open`/`fread`/`close` (slurp), `open`/`fwrite`/`close` (spit), `mkdir`, `unlink`, `rmdir`.
- Processes and clipboard: `spawn`, `SYS_SPAWN_ARGV` (via `spawn_arg`), `clip_set`.
- Misc: `ticks` (field caret blink), `sleep_ms(16)`, `exit`.

**Find feature.** Does not publish text (no `win_set_text`), so ctrl+f cannot search it.

---

### 3.2 `userland/settings.c`: "Settings"

**Constants and types.**
- `CFG "/zelr.cfg"`, `SIDEBAR_W 160`, `MAX_KNOBS 48`, `ROW_H 30`, `NAME_W 172` (:34-39, :301-302)
- `knob { char key[20]; char label[26]; int value, lo, hi, def; }` (:41-45); `knobs[48]`, `nknobs` (:47-48)
- Palette state (not in the knob list): `accent, ground (desktop), surface_c, ink (text)`; `light=1, look=0, preset=1, custom=0`. `custom` = 0 (preset), 1 (accent set by hand) or 2 (whole palette by hand) (:50-54)
- `saved_at, reset_at, dirty, last_write` (:56-58)
- Tables: `PRESET_NAMES` Teal/Indigo/Amber/Rose/Slate/Lime (:60-62); 12 `WALLPAPERS` in 4 columns (:64-69); `HIDE_NAMES` (:71-73); `MODES` (:78-88); `PAGES` (10) with `page` and `scroll` (:90-96)

**Where the list of settings comes from: `/sys/settings`.**
- The kernel renders it in `kernel/sysfs.c:321-330` `render_settings`: a header `# key value low high default label` and then `"%s %d %d %d %d %s\n"` per entry of `KNOBS[]` (`kernel/theme.c:88-131`, 31 entries, about 1.05 KB of text).
- `knobs_load` (:123-151) slurps it into a 2048-byte stack buffer and skips blank lines and `#` lines. For each line it reads key, value, lo, hi, def (`num()` accepts a leading `-`); the label is the rest of the line, truncated to 25 characters. At most 48 knobs are kept.
- The current kernel list, grouped: dock (dock_h, dock_gap, dock_side, dock_radius, dock_brand, dock_search, dock_search_w, dock_clock, clock_24, dock_hide), windows (title_h, border, button_w, button_h, corner, shadows, snap), desktop (desk_icons, icon_size, icon_gap, vignette, glows, wallpaper), behaviour (animate, anim_ms, dblclick_ms, quirks, autodesktop), machine (volume 0..100 default 70, width 0..4096, height 0..4096 with 0 meaning "boot size").
- Colours are not in this list ("a slider from nought to sixteen million is not a way to choose one", sysfs.c:318-319). They come from `/sys/theme` (`accent, desktop, surface, text, text_dim` as `0x%06x`, plus `light`, `look`, `preset`; sysfs.c:339-351).

**Writing `/zelr.cfg`: `save()`** (:197-225). The whole file is rebuilt each time into `out[2048]`:
```
# zelr desktop settings
look <0|1>
light <0|1>
preset <n>                                   (custom==0)
accent 0xrrggbb                              (custom>=1)
desktop/surface/text 0xrrggbb                (custom==2)
<every knob, in kernel order> <value>
```
- It is written whole because "a partial rewrite is a way to end up with two values for one key".
- `look` and `light` come first because the kernel rebuilds the palette when it reads them, so colour lines must follow (:190-196; theme.c:392-406 agrees).
- It is written with `spit()` (open with `O_WRITE|O_CREATE|O_TRUNC`, then one write). It never writes `text_dim`; the kernel's own `theme_save` does (theme.c:528-539).
- `touch()` / `flush(held)` (:227-239): sliders, toggles and colour sliders only mark `dirty`. `flush(in.down)` runs once per frame and saves immediately, unless the mouse is held and fewer than 8 ticks (80 ms) have passed since the last write. Buttons call `save()` directly.
- The kernel's WM re-reads the file every `timer_hz()/4` = 25 ticks (`wm.c:3994-3996`, `theme_reload`). Keys it does not know are skipped. Values are clamped to the table's bounds (theme.c:159-165, 378-386).

**`colours_load`** (:244-271).
- It reads the palette from `/sys/theme`, with fallbacks: accent `UI_ACCENTS[1]`, desktop `0x102542`, surface `0xf4f4f7`, text `0x17181c`, light 1, look 0, preset 1.
- It decides `custom` from what is **in the file** (`/zelr.cfg`): an `accent` key gives 1, a `surface` key gives 2. This is so that "a machine rebooted into a hand written palette has to come back to this window with the palette still in it".
- An out-of-range preset becomes 1.

**`reset_everything`** (:276-284): every knob back to its `def` from `/sys/settings` ("no second copy of them here to drift"); preset 1, light 1, look 0, custom 0; `save()`; `colours_load()`.

**Widgets private to this program.**
- `swatch` (:288-299): 34 px rounded square with a ring when chosen or hovered.
- `colour_row` (:307-331): label, preview chip, three 0..255 sliders (40..104 px each), and a `#rrggbb` label.
- `knob_row` (:337-363): a toggle if `lo==0 && hi==1`, otherwise the number plus a slider (60..280 px).
- `row(key)` calls `knob_row(K(key))`. A key missing from `/sys/settings` draws nothing.

**Pages** (sidebar rows at `UI_PAD + i·(UI_ROW+2)`; content starts at x = 176):

| # | Page | Contents |
|---|---|---|
| 0 | Colours (:372-455) | **Accent**: 6 preset swatches (a click sets preset, `custom=0`, saves), a label (preset name or "set by hand"), and the "Any colour at all" RGB row (sets `custom=1`, touch). **Ground**: Light/Dark toggle (saves); **Modern / Built** buttons set `look` and also set `corner` to 12/0 and `shadows` to 1/0, "what this avoids is landing there without asking" (:404-414). **The rest of the palette**: "Set them by hand" (`custom=2`), then three colour rows (Behind everything = desktop, Window and dock = surface, Text) and "Work them out again" (`custom = accent==preset accent ? 0 : 1`; save; colours_load) |
| 1 | Dock (:457-496) | dock_h, dock_gap, dock_side, dock_radius; dock_brand, dock_search, dock_search_w, dock_clock, clock_24; dock_hide as 3 buttons Never / When needed / Always (132 or 160 px), underline on the current one |
| 2 | Windows (:498-517) | title_h, border, corner, button_w, button_h; shadows, snap; hint lines about alt+arrow / alt+tab / alt+d |
| 3 | Desktop (:519-547) | 12 wallpaper buttons (4×3, 90 px; fallback index 11 = Bloom if the knob is missing), vignette, glows; desk_icons, icon_size, icon_gap |
| 4 | Behaviour (:549-566) | animate, anim_ms, dblclick_ms; volume, quirks, autodesktop |
| 5 | Screen (:568-633) | Current size from `/sys/screen`. If `settable`: 7 mode buttons in 3 columns (800x600, 1024x768, 1280x720, 1280x1024, 1440x900, 1600x900, 1920x1080); a click sets knobs `width`/`height` and saves; the underline marks the button equal to the **actual** screen. Otherwise "The firmware chose this one and it cannot be changed." **Start again**: two-press reset, armed for 300 ticks (3 s), button text "Yes, put it all back" |
| 6 | Everything (:638-664) | Every knob, in kernel order, as a generic row. `shown = (h - top - 86)/30` rows plus a scrollbar; the wheel scrolls |
| 7 | The file (:699-729) | `/zelr.cfg` printed in two columns with the 8×16 bitmap `text()` font; `#` lines are dim |
| 8 | System (:731-741) | `/sys/memory`, `/sys/cpu`, `/sys/devices` via `show_file` |
| 9 | About (:743-753) | `/sys/version`, `/sys/uptime`, `/sys/net` |

**Resolution picking.**
- `/sys/screen` (sysfs.c:290-302) gives `width`, `height`, `settable` (= `fb_mode_settable()`: `active && !adopted && !via_svga`, fb.c:247), `source`, and frame counters.
- The WM applies `want_w`/`want_h` in `apply_screen_size` (wm.c:3179-3191). 0 means the boot size. A mode that is not settable, or that `fb_set_mode` refuses, "quietly does nothing".
- The Everything page also exposes width/height as raw 0..4096 sliders.

**`main`** (:757-843).
- Window 700×580, resizable. Calls `knobs_load()` and `colours_load()`.
- Each frame: theme loaded ("so the window recolours itself the moment a choice is made"); **Up/Down arrow keys change page**; the wheel adds to `scroll`.
- `knobs_load()` again every >15 frames when `!dirty && !in.down`, "Four times a second", so that a volume change from the dock shows up (:792-799).
- Status bar: "changes apply as you make them", or "saved to /zelr.cfg" for 90 ticks after a save; the page name on the right.

**Files touched.**
- Reads: `/sys/settings`, `/sys/theme`, `/sys/screen`, `/zelr.cfg`, `/sys/memory`, `/sys/cpu`, `/sys/devices`, `/sys/version`, `/sys/uptime`, `/sys/net`.
- Writes: `/zelr.cfg`.

**Syscalls.** The `win_*` set, `open`/`fread`/`fwrite`/`close` (via slurp/spit), `ticks`, `sleep_ms`, `exit`.

**Defaults still duplicated here** (for the defaultcheck question):
- The initialisers `light=1, look=0, preset=1` (:54)
- `reset_everything` (:278-281)
- The `colours_load` fallbacks (:249-255)
- Wallpaper fallback 11 (:524), dock_hide fallback 1 (:479), Modern/Built corner 12/0 and shadows 1/0 (:412-413)

Compared with the kernel: `theme_init` sets look MODERN, light true, preset 1 (theme.c:336-338), and KNOBS defaults are corner 12, shadows 1, wallpaper BLOOM (11), dock_hide 1. All of these agree **except** the desktop fallback `0x102542` against `MODERN_DESKTOP 0x10254A` (theme.c:59). Per-knob defaults are no longer duplicated at all. See §10 on `tools/defaultcheck.py`, which now compares nothing.

---

### 3.3 `userland/notes.c`: "Notes" (the backlog's "editor")

**State.** `BUF_MAX 65536`, `PATH_MAX 256` (:16-17). `buf[65536]`, `len`, `cursor`, `dirty`, `path[256]`, `status[128]`, `scroll_line` (first line drawn), `sel_anchor=-1` (:19-27).

**Buffer operations.**
- `insert_text` (:36-44) refuses ("file is full") when `len+n >= BUF_MAX`. It memmoves the tail (O(n)), advances the cursor and sets `dirty`.
- `delete_range(from,to)` (:48-58) clamps its arguments and sets `cursor=from`.
- `backspace` (:60-62).

**Positions.** `line_start/line_end/line_of/column_of/start_of_line/total_lines` are all linear scans (:66-94). `move_line(delta)` keeps the column, clamped to the end of the target line (:97-108).

**Files.**
- `load(from)` (:112-128): `slurp(from, buf, 65535)`. On failure it gives an empty "new file"; otherwise "opened". A **larger file is silently truncated to 65,535 bytes**. It resets cursor, scroll, selection and dirty, and sets `path=from`.
- `save()` (:130-135): `spit(path, buf, len)`; messages "saved", "could not save" or "no filename". It saves to `path` (the loaded file), **not** to whatever is typed in the path field.

**Startup** (:178-192), in order:
1. `argv[1]` if it is non-empty (this is how Files opens a file).
2. Otherwise `/cfg/notes-open`: its content is trimmed of trailing newlines/spaces, loaded, and the file is **unlinked** ("the older way").
3. Otherwise `/home/untitled.txt`.

**Window.** 700×520, resizable. `ui_load_theme()` is called **once** (:174), so Notes does not follow live theme changes.

**Toolbar** (40 px, :253-273):
- `Save` (primary, 56 px), `Open` (56) → `load(name_buf)`, `Copy` (56), `Cut` (48), `Paste` (64).
- A path field fills the rest (placeholder "path"). Enter in the field → `load(name_buf)` and unfocus.

**Keys** (only `in.key`, and only when the path field is not focused; :224-250):
- Left/Right; Up/Down (by line); Home/End (start/end of line); PageUp/PageDown (by the number of visible rows).
- Delete (removes the selection, or the next character); Backspace (removes the selection, or the previous character).
- Enter, Tab and printable characters replace the selection. **Tab inserts 4 spaces.**
- There are no Ctrl shortcuts, and Ctrl chords insert control bytes (§10).

**Text area.**
- Gutter `5·MONO_W` with 1-based line numbers in dim. `MONO_H = face size 15 + 3 = 18` (draw.h:152-157).
- The selection is painted under the glyphs with `mix(bg, accent, 140)`.
- Glyphs are drawn with `mono_char`. There is no word wrap and no horizontal scroll: long lines are clipped at `cols`.
- The caret is 2 px of accent colour, blinking every 30 ticks (:291-324).

**Scrolling** (:276-289). The wheel moves 3 lines per step (clamped to the last line) without moving the cursor. On other frames the view follows the cursor. The comment explains why: "or the two fight and the cursor always wins".

**Mouse** (:327-337). A press in the text area sets `cursor = sel_anchor =` the clicked position; dragging moves the cursor.

**Clipboard** (:143-162). Copy/Cut call `clip_set(selection)`. Paste checks that `clip_len()` fits, reads into a static `incoming[65536]`, and replaces the selection.

**Status bar.** Left: `"* "` when dirty, then the path. Right: `line:col` and the last message.

**Not present.** Undo, find, word wrap, save-as, a dirty-check before Open or close, and `win_set_text` (so ctrl+f cannot search it).

**Syscalls.** `win_*`, `open`/`fread`/`fwrite`/`close` (via slurp/spit), `unlink`, `clip_set`/`clip_get`/`clip_len` (SYS_CLIP_SET 38, SYS_CLIP_GET 39 with cap 0 meaning length), `ticks`, `sleep_ms`, `exit`.

---

### 3.4 `userland/paint.c`: "paint"

- **No ui.h and no font.** It has its own `rect`, `frame`, `dot` (filled disc clipped below the toolbar), and `line` (Bresenham, placing a `dot` of radius r at each step) (:44-97).
- **Window.** `win_create("paint", 640, 420)`, lower-case title. **Not resizable.** The canvas is simply the window surface below `TOOLBAR_H 58` (:12, :204-217).
- **Palette.** 16 fixed colours (:16-23). The initial colour is index 2 (`0xC74A3C`, red). `BRUSHES[4] = {1,3,6,12}` are radii; r ≤ 1 paints a single pixel. The initial brush is index 1 (r = 3) (:25, :37-38).
- **Toolbar** (`draw_toolbar` :103-145), background `0x222931`:
  - Swatches 24 px at `x = 8 + 28·i`, y 6. The row stops early if the window were narrow. The selected swatch gets a white and a dark frame.
  - Brush previews at `(20 + 34·i, 44)`, drawn at their real size.
  - A "Clear" box (34×34 at x W-44, y 8) with a red X plotted pixel by pixel (the comment says "there is no font on this side of the ring boundary").
- **Mouse** (`on_mouse` :153-195).
  - In the toolbar, it acts only on the **press** event (`WIN_BTN_DOWN`, bit 7 marks "the event that started a press", zelr.h:836-840): pick a colour, a brush, or clear.
  - On the canvas, while the left button is held: the first event of a stroke (`!drawing || pressed`) draws a dot; later events draw a line from the last point. Releasing the button ends the stroke.
- **Keys** (`on_key` :197-202): `1`-`4` choose the brush; `c`/`C` clear the canvas (no confirmation); `[` and `]` step to the previous/next colour.
- **Loop.** Drain all events, `win_commit` only if something changed, `sleep_ms(10)`. The window closes on `WIN_EV_CLOSE`.
- **Not present.** Save, load, undo, resize, `win_set_text`.
- **Syscalls.** `win_create`, `win_surface`, `win_width`, `win_height`, `win_poll`, `win_commit`, `win_close`, `sleep_ms`, and `write` (puts "paint: no window" / "paint: no surface" on failure). It exits by returning from main (`_start` then issues SYS_EXIT, zelr.h:969-982).
- `tools/deskcheck.py:79-82, 492-518` uses Paint as the "second window": it opens it via Media→Paint and relies on the canvas `(300,200,700,450)` being uniformly paper-coloured.

---

### 3.5 `userland/music.c`: "Music"

**Constants and state.**
- `MAX_FILES 64`, `NAME_MAX 64`, `PATH_MAX 128`, `CHUNK 2048` (frames per write) (:19-22).
- `song_t {name[64], path[128], size}`, `songs[64]` (:24-30).
- Playback: `fd`, `playing`, `data_at`, `data_len`, `data_done`, `src_rate`, `src_chans`, `src_bits`, `dev_rate=48000`, `dev_chans=2`, `step` (16.16), `frac` (:36-43).
- Buffers: `short out[CHUNK*2]`, `u8 in_buf[CHUNK*4]` (8 KiB) (:45-46).

**Finding files** (:93-111). `rescan()` scans `/home`, `/music` and `/usb`, non-recursively. `is_wav` requires the name to end in ".wav" (any case) with length ≥ 5. The first song is selected. (`/music` is never created by `kernel/layout.c:27`, so that readdir simply fails.)

**WAV parsing: `open_song`** (:120-190).
1. Open the file (`O_READ`) and read a 12-byte `RIFF....WAVE` header.
2. Walk the chunks from offset 12 (`seek` + read the 8-byte id/len):
   - `"fmt "`: read 16 bytes. Format tag must be `1` (plain PCM), otherwise "only plain pcm" (so WAVE_FORMAT_EXTENSIBLE 0xFFFE is refused). Take channels (+2), sample rate (+4) and bits per sample (+14).
   - `"data"`: set `data_at = at+8`, `data_len = len`, and **stop walking**.
   - Anything else: skip `8+len+(len&1)` (chunks are padded to an even size).
3. Require fmt, data, a non-zero rate, non-zero channels, and **8 or 16 bits**. Otherwise "that header is not one this can read".
4. Set `step = (src_rate<<16)/dev_rate` and seek to the data. The status shows "R Hz, B bit, mono|stereo".

**Accepted formats.** PCM, unsigned 8-bit or signed 16-bit little-endian, any non-zero rate, one or more channels (only the first two are used: `r` is sample +2 bytes / +1 byte). The data chunk must come after fmt.

**Resampling: `pump()`** (:194-244), once per frame while playing.
- Source frames wanted: `want_src = (CHUNK·step + frac) >> 16`, clamped to [1, CHUNK]. Bytes are clamped to `in_buf` and to what is left of the data.
- For each output frame: take source index `i = pos>>16`. For 16-bit, take the little-endian sample; for 8-bit, compute `(u8-128)<<8`. **A mono file gets `r = l`** (each sample written twice).
- Advance `pos += step` until CHUNK output frames are made or the source runs out. Then `frac = pos & 0xFFFF`, and `sound_write(out, made)`.
- This is nearest (floor) sampling: no interpolation, no filter.

**Sound syscalls.**
- `sound_info` (SYS_SOUND_INFO 40) fills `zelr_sound {present, rate, channels, reserved}` (:296-303).
- `sound_write` (SYS_SOUND_WRITE 41): the kernel caps each call at 4096 frames (syscall.c:454-460). It **blocks until the frames fit in the ring** (about a third of a second), sleeping 2 ms at a time and giving up after ~50 stalled waits (sound.c:249-301). That blocking is what paces playback, since the UI loop only sleeps 4 ms while playing (:440).

**Volume.** Nothing in the app. The kernel scales every written sample by the global `volume` (sound.c:84-101, default 70). That value is the `volume` knob (Settings → Behaviour) or the dock speaker (wm.c:3936-3938).

**Tune** (:248-289). Eight notes: C5 D5 E5 C5 E5 G5 E5 C5 (523/587/659/523/659/784/659/523 Hz; 180-320 ms). Each is a parabola approximating a sine, scaled ×3 (this overflows; see §10). The whole note is written in 2048-frame blocks in one call.

**UI** (:358-438).
- Toolbar of height `UI_ROW+UI_PAD` = 34 px with buttons `Play`/`Stop` (80 px; Stop closes the file, so there is no pause), `Tune` (80) and `Look again` (90, rescan).
- List rows show the name and the size in K. A click selects; a click on the selected row plays it; the wheel scrolls 1 row per step.
- With no files: "No .wav files in /home, /music or /usb." and "Copy one onto a stick and plug it in, or press Tune."
- A progress bar (`data_done/data_len`) sits just above the status bar. Status left is the message; right is "playing", "tune" or "ready".
- **No keyboard handling at all.**

**Argument** (:308-329). `argv[1]` is copied into a `song_t`, inserted at the top of the list and played at once. The insertion uses a field-by-field `song_copy` because "A struct assignment of this size becomes a call to memcpy, and there is no library here to have one in".

**Syscalls.** `win_*`, `sound_info`, `sound_write`, `readdir`, `open`, `fread`, `seek`, `close`, `ticks`, `sleep_ms`, `exit`.

---

### 3.6 `userland/calc.c`: "Calculator"

**Representation.** `SCALE 1000000LL` (millionths), `ENTRY_MAX 24` (:17-18).

**State** (:20-26):
- `entry[25]="0"`, `entry_len` -- the display is a *string being typed*
- `typing` -- whether the next digit appends to `entry`
- `acc` (i64, scaled) -- the left operand waiting for an operator
- `pending` -- `0` or one of `+ - x /`
- `error`

**Parsing and formatting.**
- `parse_entry` (:30-47): optional `-`, integer digits, `.`, up to 6 fraction digits, giving `whole·SCALE + frac`.
- `format(v,out,cap)` (:58-85): sign, integer digits, then up to 6 fraction digits with trailing zeros dropped ("2 / 4 is 0.5 and not 0.500000"). `cap` is ignored; the maximum length is 21 characters.
- `show(v)` writes the formatted value into the display and sets `typing=0` (:87-92).

**Operations** (`apply` :96-112), with `rhs = parse_entry()`:
- `+` / `-` : exact
- `x` : `(acc*rhs)/SCALE`
- `/` : `(acc*SCALE)/rhs`; division by 0 sets `error` ("cannot" is shown in `t.warn`)
- Both `x` and `/` truncate toward zero. There is no 128-bit intermediate, so they overflow (§10).

**Entry functions** (:114-176).
- `digit` (:114-129): allows one leading zero and no second point.
- `operator` (:131-137): chains `apply()` if an operation is pending and typing; otherwise `acc = parse_entry()`.
- `equals` (:139-144): with nothing pending, just `acc = entry`; there is no repeat-last-operation.
- `clear_all`, `backspace` (only while typing), `negate` (toggles a leading `-` in the string).
- `percent` = entry/100 on its own, not "x% of acc" (:173-176).

**The 78/4 bug story** (:49-57; README 1482-1490). `format` used to write only into the display. Drawing the pending line ("12 +") saved and restored the string but not the `typing` flag. So the digit after an operator was appended to the first number: "78 / 4 came out as 78 / 784" (0.099489). The fix is that `format` writes into a caller buffer and the pending line uses its own buffer (:264-273). `tools/appcheck.py` guards against a regression.

**UI** (:207-322).
- Window 280×380, resizable. The display is a sunken well 86 px tall: the entry in `UI_FACE_HEAD` aligned right; "acc op" small at top-left while something is pending.
- The keypad is 5 rows of 4 (`KEYS[]` :184-190; `0` is double width):

  | Row | Keys |
  |---|---|
  | 0 | C, +/-, %, / |
  | 1 | 7, 8, 9, x |
  | 2 | 4, 5, 6, - |
  | 3 | 1, 2, 3, + |
  | 4 | 0 (wide), ., = |

- Key width `kw = (w-16-18)/4`, key height `kh = max(20, (h-102-24)/5)`. Operators are drawn in the accent colour; the pending operator is darker. Keys are bevelled, fire on release, and shift their label 1 px while held.
- **Keyboard** (:233-241):
  - Enter or `=` → equals
  - Esc, `c` or `C` → clear
  - Backspace → backspace
  - `*` → x
  - Any other character is passed through, so digits, `.`, `+`, `-`, `x`, `/`, `%` and `~` (negate) work.

**Syscalls.** `win_*`, `sleep_ms`, `exit` (plus the theme slurps).

---

### 3.7 `userland/monitor.c`: "Monitor"

**Constants.** `MAX_TASKS 32`, `HISTORY 120` samples, `SAMPLE_MS 250` (:25-27). The sample cadence is `ticks() - next >= SAMPLE_MS/10`, i.e. 25 ticks, which is 250 ms only because the tick is 10 ms (:214-217). 120 × 250 ms gives the "last thirty seconds" graph.

**Types and globals.**
- `row_t {pid, state, slices, idle, user, name[64], was, was_idle, share}` (:29-39); `rows[32]`, `nrows`
- `history[120]` (u8 %, oldest first), `history_n`, `selected`, `last_tick`, `last_total` (written, never read), `info` (`zelr_sysinfo`) (:41-51)

**Data sources.** Only syscalls:
- `tasks(i, &zelr_task)` (SYS_TASKS 35; `{pid,state,slices,idle,user,name[64]}`, zelr.h:508-515) walks the scheduler ring (syscall.c:477-504).
- `sysinfo()` (SYS_SYSINFO 31; cpus_found/started, mem_total/used/free_kb, heap_total_kb, uptime_seconds, tasks, screen_w/h, syscalls, disk_kb_free; syscall.c:994-1014).
- `ticks`, `getpid`, `kill` (SYS_KILL 34).
- It reads **no file**, although the header says "two system calls and one file" (§10).

**CPU computation: `sample()`** (:72-119).
- For each task: `did = (slices − was) − (idle − was_idle)`, clamped at 0. `share = did·100/span`, capped at 100, where `span` = PIT ticks since the last sample.
- Machine total: `busy = Σdid·100/span`, capped at 100. It is pushed onto the history.
- The idea (:9-20): a loop waiting for a key is picked every tick and spends its slice halted, which read as 100 % busy. Loops that call `task_idle_wait()` have their halted ticks counted in `idle_ticks` (sched.c:635-641). The idle tasks count theirs too (sched.c:146-155).
- Important nuance: `slices` goes up on **every** scheduler switch that picks the task. That means timer ticks, voluntary yields (`VEC_YIELD`) and each CPU's local APIC tick (idt.c:256-258 → sched.c:520). `idle_ticks` are PIT ticks. See §10.

**Layout** (:219-356).
- Window 640×560.
- Four cards, each 62 px tall with a 2 px tint bar: `N%` "processor"; `U MB` "of T MB" (tint shifts from accent to warn as memory fills); `nrows` "tasks"; `Mm` "Ss up".
- Graph (110 px): 1 px columns, newest against the right edge, quarter lines, colour turning to `warn` above 80 %.
- Memory bar plus a line "heap N KiB, S of F processors running, N system calls".
- Task list "what is running", with columns: pid (x pad+8), name (pad+44), state word (pad+300; `TASK_BLOCKED` is shown as "waiting"), a share bar 90×6 (accent for user tasks, dim for kernel tasks), and `share%`.
- A click selects a row. When a row is selected, a `Stop <name>` button (200 px) calls `kill(pid)` unless it is the monitor itself, then deselects and resamples (:340-356).
- Status: "updated four times a second". The loop sleeps 60 ms per frame.

---

### 3.8 `userland/blackjack.c`: "Blackjack"

**Rule constants** (:27-33, header :1-21):

| Constant | Value | Meaning |
|---|---|---|
| `PACKS` | 6 | decks in the shoe |
| `SHOE` | 312 | cards |
| `CUT` | 234 (3/4) | reshuffle before a round once `shoe_at >= CUT` (:328); mid-round only if the shoe is empty (:80) |
| `HAND_MAX` | 12 | cards per hand (`give()` silently ignores more) |
| `HANDS_MAX` | 4 | split up to four hands |
| `DEAL_TICKS` | 9 | 90 ms between cards landing |
| `bank` | 1000 | starting bankroll (:63) |
| `wager` | 25 | default bet (:64) |
| Chips | 5 / 25 / 100 / 500 | red, green, blue, purple (:507-512) |

Rules as implemented:
- The dealer stands on all 17s, soft included (:196-197).
- Blackjack pays 3:2 **rounded down** (`bet + bet*3/2`, :167).
- Double on any two cards, after a split too, and not on split aces (:252-256).
- Split by **rank** (K+Q is not a pair; 10+10 is) up to 4 hands (:258-266). Split aces get one card each and are done (:304-312, :225).
- Insurance is offered on an ace up card if `bank >= wager/2`; it costs `wager/2` and pays 2:1 (:358, :365-372, :179-183).
- The dealer **peeks** on an ace (after the insurance question) or a ten-value up card (:357-361).
- **Late surrender**: only on the first two cards of an unsplit single hand; half the bet comes back (:268-272, :163).
- Not implemented: side bets, even money as a separate button (it is insurance on a blackjack), count help (:18-21).

**Types.**
- `hand { card[12], shown[12] (tick each card becomes visible), n, bet, doubled, done, surrendered, from_split, split_ace, payout, verdict }` (:35-47)
- States `ST_BET, ST_INSURE, ST_PLAY, ST_DEALER, ST_OVER` (:49)

**Globals.**
- `rng r`, `shoe[312]`, `shoe_at`, `shuffled_at` (really a boolean)
- `player[4]`, `hands`, `active`, `dealer`, `hole_shown`
- `bank`, `wager`, `insurance`, `state`, `last_swing`, `note[96]` (never written, dead) (:51-68)

**Hand value** (`value_of` :90-101): an ace counts 11, with as many as needed reduced to 1; `soft` means an ace still counts 11. `is_blackjack` = 2 cards, not from a split, and 21 (:105-107).

**`move_hand`** (:120-124) copies byte by byte through a `volatile` pointer. At -O2 clang turns a struct assignment, or even a plain byte loop, into a call to the external `memcpy` symbol, which does not exist. zelr.h's memcpy is `static inline` and does not satisfy that emitted call.

**Payouts** (`settle` :154-185), per hand:

| Outcome | Chips returned |
|---|---|
| Surrendered | bet/2 |
| Bust | 0 |
| Player blackjack, dealer none | bet + ⌊1.5·bet⌋ |
| Dealer blackjack | push (bet) if player blackjack, else 0 |
| Dealer bust | 2·bet |
| Higher | 2·bet |
| Lower | 0 |
| Equal | bet |

Insurance returns `3·insurance` on a dealer blackjack; otherwise it is lost. `last_swing` is the net result of the round.

**Drawing.**
- Baize: a vertical gradient `FELT_DARK 0x0B3A28` → `FELT_LIT 0x14543A` out of 200 levels, brightest at h/3 (:418-434).
- Cards are 62×88, or 48×68 when w < 640. They are fanned at 5/9 of the card width (:440-445), drop a shadow, and the dealer's hole card is drawn as a back until `hole_shown`.
- A count label shows only landed cards, as "soft N" or N, red above 21 (:469-489).
- Chips are discs with 6 marks from a hexagon table (:492-505).
- Top bar: Bankroll plus a shoe-remaining bar. Hands are centred as a group with a verdict banner above each (green win, grey push/surrender, red loss). The result banner reads "You win $X", "You lose $X" or "Push". At bank 0 in ST_OVER: "Out of chips. The house always was going to win."

**Controls** (:624-680; buttons 92 px wide at y h-44):
- ST_BET / ST_OVER: 4 chips (a click adds the value, or all-in if not affordable), the wager amount, `Clear`, primary `Deal`.
- ST_INSURE: "Dealer shows an ace. Insurance?" with `No thanks` / `Insure`.
- ST_PLAY, **only once every card has landed** (`settled_on_screen` :136-143): `Hit` (primary), `Stand`, `Double`, `Split`, `Surrender` (+20 px wide). A button is hidden, but keeps its slot, when not allowed.

**Keys** (:712-731; letters are folded to lower case):
- Bet/Over: Enter or Space deals; `1`-`4` add a chip; Backspace or Esc clears the wager.
- Insure: `y` insures; `n` or Esc declines.
- Play: `h` hit, `s` stand, `d` double, `p` split, `r` surrender.

**RNG.** `rng_start` at launch (:520). Every mouse or key event stirs it with `ticks·31 + x·7 + y` (:543-544).

**Find text** (`publish` :379-411, every >25 ticks, buffer 512):
```
Blackjack bankroll $1,000 bet $25
dealer K hidden              (ranks only; the hole card reads "hidden")
hand 10 7 - won              (one line per hand, verdict if any)
```
**Syscalls.** `win_*`, `win_set_text` (SYS_WIN_TEXT 56), `ticks`, `getpid`, `sleep_ms`, `exit`.

---

### 3.9 `userland/cards.h`

- **Encoding** (:26-46). A card is 0..51: `RANK(c)=c%13` with 0 = Two … 12 = Ace; `SUIT(c)=c/13` in the order Club, Diamond, Heart, Spade (bridge order, which is also alphabetical). `MAKE_CARD(r,s)`, `CARD_NONE -1`, `R_TWO 0, R_FIVE 3, R_TEN 8, R_JACK 9, R_QUEEN 10, R_KING 11, R_ACE 12`, `CARD_RED`.
- **Names.** `RANK_NAME` {"2".."10","J","Q","K","A"}; `RANK_WORD` {"Twos".."Aces"} (:48-56).
- **RNG** (:73-94). `rng {u64 s}`.
  - `rng_stir(r,x)`: `s ^= x + 0x9E3779B97F4A7C15 + (s<<6) + (s>>2)`, and never leaves `s` at 0.
  - `rng_start`: `s = 0x243F6A8885A308D3`, then stir with `ticks()`, `getpid()·2654435761` and `ticks()<<17`.
  - `rng_next`: xorshift64 (12/25/27) with the output multiplied by `0x2545F4914F6CDD1D` and the top 32 bits taken.
- **`rng_below(r,n)`** (:104-110): rejection sampling. The comment says "with no bias". The limit formula is off by a little (§10).
- **`shuffle`** (:114-119): Fisher-Yates walking down, `j = rng_below(i+1)`.
- **`deck_fill(deck, packs)`** (:123-127): `packs` × 0..51 in order.
- **Shapes** (:139-211). `tri()` is a scanline triangle. `pip()`: heart = 2 discs + a downward triangle; diamond = 2 triangles; spade = triangle + 2 discs + a flared stem; club = 3 discs + a stem.
- **Cards** (:222-286).
  - `card_face`: white with a hairline, the rank and a pip top-left, a big middle pip at 2/5 of the width, and the corner repeated bottom-right (not rotated). Ink is `0xC02030` for red suits, `0x1A1A22` otherwise. The corner uses the BOLD face when w ≥ 50, else SMALL.
  - `card_back`: accent-tinted panel with a diagonal lattice.
  - `card_ring`: a 2 px frame with no fill.
  - `CARD_W 62` / `CARD_H 88` are defined but unused.
- **`money(n,out,cap)`** (:295-314): `$` first, then `-` for a negative value, then digits with thousands commas (for example "$1,234,567" and "$-50").

### 3.10 `userland/poker.h`

- **Categories.** `HC_HIGH 0 … HC_STRFLUSH 8` (:23-31).
- **Value layout.** `category<<20 | key0<<16 | key1<<12 | key2<<8 | key3<<4 | key4`. `HAND_CAT(v)=v>>20`; `HAND_KEY(v,i)=(v>>(16-4i))&0xF` (:33-34).
- **`eval5`** (:43-96).
  1. Count ranks and check for a flush.
  2. Build `order[]`: the distinct ranks sorted by count (4→1) and then by rank, high first. This one ordering is the tie-break for every category.
  3. Straight: 5 distinct ranks with `order[0]-order[4]==4`, or the **wheel** A-5-4-3-2, recorded as five-high.
  4. Category order: straight flush > quads > full house (top 3, 2 distinct) > flush > straight > trips > two pair (top 2, 3 distinct) > pair > high card.
  5. Straights and straight flushes encode only the high card (so equal straights compare equal). Every other category encodes all of `order[]`.
  - No real hand evaluates to 0.
- **`eval_best(cards,n)`** (:100-116): the best `eval5` over all 5-subsets (21 for n=7, 6 for n=6). Returns 0 if n < 5.
- **`eval_best_five`** (:121-140): the same, but also outputs the winning five cards. **Never called anywhere**, although the comment says it lifts the winning hand out at a showdown.
- **`hand_words`** (:147-173): "a royal flush", "a straight flush, K high", "four Nines", "a full house, Kings over Fours", "a flush, A high", "a straight, 9 high", "three Queens", "two pair, Kings and Fives", "a pair of Tens", "A high".
- **`HAND_SHORT[9]`** is defined but unused.

### 3.11 `userland/poker.c`: "Poker"

**Constants** (:35-41): `SEATS 4`, `START_STACK 1000`, `SMALL_BLIND 10`, `BIG_BLIND 20` (fixed; no escalation), `DEAL_TICKS 7`, `THINK_MIN 45`, `THINK_SPAN 55` (the AI thinks for 0.45-0.99 s).

**Types.**
- `seat { name, stack, hole[2], shown[2] (landing ticks), street_bet, total_bet (for side pots), folded, allin, acted (has had a turn since the last full raise), out, won (paid last hand), said[28], show (the u32 hand value at showdown), shows[40] }` (:43-58)
- States `PS_DEAL` (initial value only, never re-entered), `PS_ACT`, `PS_SHOWDOWN`, `PS_OVER`, `PS_GAMEOVER` (:60)

**Globals.** `rg`, `deck[52]`, `deck_at`, `tbl[4]`, `board[5]`, `board_at[5]`, `board_n`, `button=-1`, `street` (0-3), `level` (the bet to match), `min_raise`, `turn`, `think_until`, `state`, `hand_no`, `headline[96]`, `raise_to` (the slider value) (:62-78).

Seats are fixed: `tbl[0]="You"`, `1="Marge"`, `2="Bishop"`, `3="Cordelia"` (:648-651).

**A hand: `new_hand`** (:504-557).
1. Shuffle all 52 cards. Set `level = min_raise = BIG_BLIND`. Reset the seats (`folded = out`).
2. Move the button to the next seat that is not out.
3. Deal two rounds starting left of the button, skipping out seats, 7 ticks apart.
4. **Blinds.** When exactly two players are live, the **button posts the small blind** and the other seat the big blind. Otherwise SB = next seat after the button and BB = next after SB (:539-546).
5. `commit()` posts what the player has, and all-in for less is automatic.
6. First to act is `next_seat(bb)` (heads-up that is the button/SB). Set `state = PS_ACT` and `raise_to = level + min_raise`.

**Moving chips: `commit(i, target)`** (:125-134). The amount added is `target − street_bet`, clamped to the stack. A seat reaching stack 0 becomes `allin`.

**Actions.**
- `act_fold` (:368-373).
- `act_call` (:375-385): "checks", or "calls $X" / "all in $X".
- `act_raise(i,to)` (:391-413):
  1. Clamp `to` to street_bet + stack. If `to <= level`, treat it as a call.
  2. `full = (to-level) >= min_raise`. Only a full raise updates `min_raise` and clears `acted` for the other active seats.
  3. `level = to`. The seat says "all in" or "raises to $X". (The "calls" branch at :410 is dead code.)

**Turn order.**
- `next_seat(from)` returns the next seat that is not out, folded or all-in; the seat itself counts last (:294-300).
- `betting_done` (:304-313): true when at most one seat is live, or every active seat has acted and matched `level`.
- `pass_turn` (:315-326): one seat live → `uncontested()`; betting done → `advance_street()`; otherwise the next seat, with a new think deadline.
- `advance_street` (:328-364):
  1. `street++`; past the river → `showdown()`.
  2. Reset `street_bet`, `acted` and `said`; `level = 0`; `min_raise = BB`.
  3. Deal the flop (3 cards) or the turn/river (1 card) with landing times.
  4. If fewer than 2 seats can act, recurse, which runs out the board.
  5. First to act is `next_seat(button)`, so heads-up the BB acts first after the flop.

**Ending a hand.**
- `uncontested()` (:221-240): the last live seat takes `pot_total()` and **no cards are shown**. Headline "You take $X" or "Name takes $X". Then `finish()`.
- `showdown()` (:242-290): every live seat gets `show = eval_best(hole+board)` and its `hand_words`. Then `award()`. The headline names the holder(s) of the **overall best** hand: "Bishop wins with …" or "You win with …", with a grammar special case (:275-279). Then `finish()`. The state becomes PS_SHOWDOWN, or PS_GAMEOVER.
- `finish()` (:206-217): a seat with stack 0 is marked `out`. If at most one seat is still standing: PS_GAMEOVER with "You have all the chips." or "You are out of chips." Otherwise PS_OVER. If you bust while two or more AIs remain, they keep playing and you press "Next hand" until one of them has everything.

**Side pots: `award()`** (:146-198).
1. `levels` = the distinct `total_bet` values of **live** seats, sorted ascending.
2. For each level `lv` (with `prev` = the previous level), the layer's pot = Σ over **all** seats (including folded) of `min(c,lv) − min(c,prev)`. Folded money stays in the layers it was put into.
3. The layer is eligible to live seats with `total_bet ≥ lv`. `best` = the highest `show` among them; the winners are those equal to it. `each = pot/winners`.
4. **Odd chip.** The remainder goes to the first winner walking left from the button (`(button+step)%4`, steps 1..4). "Where a dealer puts it and is at least a rule rather than whoever happens to be first in the array."
5. An uncalled excess forms a top layer that only its owner is eligible for, so it is returned to them.

**AI strength** (`strength_of` :430-468; scale 0..1000).
- Preflop:
  - Pair: `520 + hi·38` (from 520 for 22 up to 976 for AA).
  - Otherwise: `hi·30 + lo·12`, +70 if suited, +55 for a gap of 1, +25 for a gap of 2, −40 for a gap above 4. Clamped to 0..1000.
- Postflop: `HAND_CAT·105 + key0·6`. At the river, a hand that is entirely the board (`mine == eval_best(board)`) is divided by 4.

**AI decision** (`ai_move` :470-500), with `roll = rng_below(100)`:

| Situation | Rule |
|---|---|
| Nothing owed (check or bet) | `str > 620 && roll < 62` → bet `street_bet + pot/2 + BB`. Else **bluff**: `str < 260 && roll < 11` (11 % ≈ README's "one time in nine") → bet `street_bet + pot/2`. Else check |
| Owed > 0 | `need = owed·100/(pot+owed)` (pot odds %), `est = str/10`. Fold if `est + roll%12 < need − 4` **and** `owed > stack/25` (it never folds to a bet under 4 % of its stack). Raise if `str > 720 && roll < 50` → `level + max(pot/2, min_raise)`. Else call |

Consequences: postflop, value bets need a full house or better (≥ 630) and raises need quads or better (≥ 735). Preflop pairs 88+ may raise.

**Information hiding.**
- `strength_of`/`ai_move` read only `tbl[i]` (their own seat), `board`, the pot, `level` and `min_raise` (poker.c:415-429).
- Opponents' cards are drawn face down unless the state is `PS_SHOWDOWN` or `PS_GAMEOVER`; folded seats show empty rings (:724-739).
- The find text publishes only your own hole ranks.
- All hands do live in one process's memory, and one RNG stream serves both the deck and the AI rolls. The deck order is fixed at `new_hand`, so the rolls cannot steer the cards.

**Drawing.**
- Table: dark ground `0x14100C`, rail `0x3A2418`, felt `0x0A3326`, and three stacked lit rounded rectangles drawn solid. The comment explains that a translucent version cost most of a frame (:585-620).
- Cards are 54×76. The board sits at `y = max(120, h/2−110)`, with empty rings for undealt slots. The pot is shown below the board and the headline above it.
- Seats (:714-797):

  | Seat | Position |
  |---|---|
  | You | bottom centre `(w/2−cw−4, h−250)` |
  | Marge | left `(44, by−20)` |
  | Bishop | top `(w/2−cw−4, 46)` |
  | Cordelia | right `(w−44−2cw−8, by−20)` |

- Each seat has a plate (accent when it is that seat's turn) with name and stack (or "out"), and a "D" disc for the dealer button. The label (last action, or the hand at showdown, green if it won) sits outward; the street chips (`chips_at`, up to 4 discs plus the amount) sit inward.

**Your controls** (only when `PS_ACT && turn==0 && cards_landed()`; :800-866):
- `Fold`; `Check`, `Call $X` or `All in $X`.
- If `most > level`: a slider from `least = min(level+min_raise, most)` to `most`, quick buttons `Min`, `Half` (`level+pot/2`), `Pot` (`level+pot`), `All in`, and a primary button "Bet $X" / "Raise to $X" / "All in $X".
- Between hands: `Next hand`. After the game: `New game`.
- Keys: `f` fold; `c` or Enter call; `r` raise to the slider value; `a` all-in. When it is not your turn, Enter or Space means next hand or new game.

**Find text** (every >25 ticks):
```
Poker hand N
board A K 7
you Q J
<name> $stack [said]     (one line per seat, "out" if busted)
<headline>
```

**Syscalls.** `win_*`, `win_set_text`, `ticks`, `getpid`, `sleep_ms`, `exit`.

### 3.12 `userland/cardtest.c`

- **Headless.** It includes `zelr.h`, `cards.h` and `poker.h`. Because `cards.h` includes `draw.h` and `ui.h`, the font data is linked in even though nothing is drawn.
- **Parsing.** Cards are written as rank letters `2..9,T,J,Q,K,A` plus suit `C,D,H,S` ("AS KS QS JS TS"); an unknown card becomes 0 (:36-64).
- **Output.** Each check prints `"  PASS  "` or `"  FAIL  "` plus its description. The run ends with `CARDTEST_PASS` or `CARDTEST_FAIL`, and the return code is the failure count (:26-32, :262-263).

**The 56 checks:**

| Group | Count | What they check |
|---|---:|---|
| Categories | 9 | one per category |
| Ladder | 8 | each category beats the next lower |
| Ace at both ends | 5 | the wheel is a straight; a six-high beats the wheel; broadway beats king-high; a steel wheel is a straight flush; a king-high straight flush beats the steel wheel |
| Tie-breakers | 9 | pair rank, kicker, high pair, low pair, fifth card, flush top card, flush second card, plus 2 exact ties (suits ignored; straights in different suits) |
| Seven cards | 4 | best five, not the first five; flush within 7; a straight needing both hole cards; better kicker from the same board |
| Words | 3 | royal flush; "two pair, Kings and Fives"; "four Nines" |
| Deck | 4 | 40 shuffles each keep all 52 once; two shuffles differ; `rng_below(6)` always < 6; 6000 draws land roughly evenly (each count 800-1200) |
| Blackjack | 9 | via a **local copy** `bj_value` (:86-97), not blackjack.c's `value_of` |
| Money | 5 | "$0", "$25", "$1,000", "$1,234,567", "$-50" |

- **How it runs.** `tools/ring3check.py:41` runs `exec /bin/cardtest` and waits up to 120 s for `CARDTEST_PASS`, alongside the other ring-3 suites. `pipeline/gate.sh:527` wrongly calls that tool "ring3test".

### 3.13 `tools/appcheck.py`

Machine: `Guest(memory=256, machine="q35")` with `qemu-xhci` + `usb-storage` (the stick) and `intel-hda` + `hda-output` recording to a wav file (:39-45, :167). It boots, types `desktop`, then:

1. **Calculator** (typed `calc`).
   - The key centres are computed the way calc.c computes them: window at the WM cascade `CALC_X=154`, `CALC_Y=74`; `WM_BORDER 1`; `WM_TITLE_H 32`; `UI_PAD 8`, `UI_GAP 6`, `DISPLAY_H 86`; window 280×380 (:47-85).
   - It photographs the display well empty. Then, up to 3 tries, each starting from `C`: taps `7 8 / 4 =` and photographs; taps `C 1 9 . 5` and photographs. The two photographs must be **pixel-identical** and different from the empty one. Then it taps `2` and checks the display changed.
   - Clicks go out one at a time with no retry (`tap`, :135-155), because "a digit entered twice is not a lost keystroke, it is a different sum".
   - Checks: "the keys reach the calculator at all", "the calculator divides…", "and a different number looks different".
   - `sendkey alt-q` closes the window.
2. **Monitor.** Photographs the band `(100,100,700,200)`, runs `run count &`, and waits up to 25 s for the band to change: "the monitor notices another program starting".
3. **Music.**
   - The stick is a FAT image made by `tools/mkfat.py` containing `TONE.WAV`: 22050 Hz (deliberately not the device rate), mono, 16-bit, a ±9000 square wave at 440 Hz for 900 ms (:88-114).
   - Checks "usb volume mounted" in the serial log, then types `music /usb/TONE.WAV` and waits 12 s (6 s proved too short under a loaded gate).
   - It then reads the recording: "something was recorded", "the player made a sound", "at the pitch that was in the file" within 8 % (`soundcheck.close_to`).
   - This exercises argv delivery, RIFF parsing, resampling from 22050 to the device rate, mono duplication and `sound_write`.

### 3.14 `tools/gamecheck.py`

Screen 1024×768, `Guest(memory=256)`. It counts **exact white** (`0xFFFFFF`) pixels as "card" (:76-84; `harness.count_in`).

1. Opens the launcher from the dock badge, then the **Games** row (index 3). Checks that the pane is panel-coloured (>3000 pixels): "the launcher has a Games row with something under it".
2. Clicks pane row 0 (**Blackjack**).
   - Fewer than 200 white pixels in `(180,120,980,600)`: "opens with no cards".
   - Sends Enter up to 4 times until white rises by more than 2000: "dealing puts cards on it".
   - Records the dealer band `(180,130,980,260)`. Sends `n` then `s` up to 5 times until red chip pixels `0xD64545` exceed 150 in `(180,580,980,680)`. (The `n` declines a possible insurance offer, "a rule the check had not been told about".)
   - Checks: dealer white +600 ("standing turns the dealer's hand over and plays it out"), and "the hand is over, with the chips back out".
3. Esc leaves the desktop; types `desktop`, then `poker`.
   - More than 1500 white pixels in `(330,380,700,560)`: "poker deals you two cards".
   - Board `(330,230,700,330)` below 200: "the board is empty before the flop".
   - Presses `c` up to 14 times until the board exceeds 1500: "calling round brings a board out".

### 3.15 Context headers the apps depend on (summary only)

**`ui.h`**
- Metrics: `UI_PAD 8, UI_GAP 6, UI_ROW 26, UI_BTN_H 30, UI_TITLE_H 28, UI_RADIUS 6, UI_SCROLL_W 10, UI_MENUBAR_H 20, UI_KEYS 16`.
- `ui_theme {bg, panel, fg, dim, accent, accent_fg, soft, line, warn, edge_hi, edge_light, edge_shadow, edge_dark, well, modern, stroke, raised}`.
- `ui_load_theme()` parses `/zelr.cfg` (keys preset/light/look/accent) and then `/sys/theme`'s accent (:233-342). `ui_cfg_int(text,key,fallback)` parses decimal or `0x` hex at the start of a line (:207-231). `UI_ACCENTS[6]` duplicates the kernel's presets (:201-205).
- `ui_input {mx,my,down,pressed,released,right_pressed,key (first key this frame),keys[16],nkeys,scroll}`; `ui_begin` and `ui_feed` (:359-397).
- Widget conventions: a button returns 1 on the **release** inside it and **consumes `released`**. `ui_row` returns 1, or 2 for a right click. `ui_toggle` and `ui_slider` return the new value. `ui_field {buf,cap,len,cursor,focused}`, where `ui_field_draw` focuses on a click and unfocuses on a click elsewhere. `ui_menubar` returns the hot title. `ui_menu` returns an item index on release. Also `ui_statusbar` (the bottom 26 px), `ui_well`, `ui_scrollbar`, `ui_section`, `ui_toolbar`, `ui_groove`, `ui_raised/sunken`, `ui_round`, `ui_round_outline`.

**`draw.h`**
- `surface {px,w,h}`; `fill/rect/frame/round_rect/disc/line/glyph/text/text_centred/mix`.
- Anti-aliased faces `UI_FACE_SMALL 13px, BODY 15, HEAD 20, BOLD 15, MONO 15, MONOB`; `face_w/face_h/face_draw/face_draw_clip/face_centred/mono_char`; `MONO_W` (the mono face's space advance) and `MONO_H` (size+3).

**`sdk/zelr.h`**
- Syscalls used here: `SYS_EXIT 0, WRITE 2, GETPID 3, TICKS 4, SLEEP 5, WIN_CREATE 7, WIN_SURFACE 8, WIN_SIZE 9, WIN_POLL 10, WIN_COMMIT 11, WIN_CLOSE 12, OPEN 13, CLOSE 14, FREAD 15, FWRITE 16, SEEK 17, UNLINK 18, MKDIR 19, RMDIR 20, READDIR 21, SYSINFO 31, SPAWN 32, KILL 34, TASKS 35, WIN_RESIZABLE 36, CLIP_SET 38, CLIP_GET 39, SOUND_INFO 40, SOUND_WRITE 41, SPAWN_ARGV 43, WIN_TEXT 56`.
- Sugar: `strncpy(d,s,n)` copies **at most n−1** characters and always terminates (:272-276). `slurp` reads until the file ends or the cap is reached and returns the length, **with no truncation signal** (:381-393). `spit` does `O_WRITE|O_CREATE|O_TRUNC` and one write (:395-401). `utoa`, `memset`/`memcpy` are static inline.
- Events and keys: `WIN_EV_* 1..6`; `WIN_BTN_LEFT 1, RIGHT 2, DOWN 0x80`; `KEY_UP 0x100 … KEY_DELETE 0x108`; `KEY_MOD_CTRL 0x20000`; `KEY_IS_SPECIAL(k) = KEY_CODE(k) >= 0x100`; `RGB`.

---

## 4. Control flow and lifecycles

### 4.1 Launch
The kernel's `user_spawn_elf(_argv)` maps the ELF and places argc/argv on the stack System V style. `_start` passes them to `main(argc, argv)` (zelr.h:969-982). The loop then runs as in §2.3 until `WIN_EV_CLOSE` or the surface going away. `exit(0)` or a return from main ends the task. `signal_end_task` (on kill) releases its windows (signal.c:29-45).

### 4.2 Files: open, paste, rename
- **Open.** Click the selected row, Enter, the Open button, File→Open or the context-menu Open → `open_entry`.
  - Directory → `go_to` → `reload` (readdir 0..511, filter, sort) → selection cleared.
  - Otherwise it spawns the owning program. The status becomes "opened" or "could not start it" ("started" or "would not run" for programs).
- **Copy/Cut → Paste.** Copy or Cut sets `clip_path` (and `clip_is_cut`). Paste → `do_paste` → `copy_file` (≤ 64 KiB) → if it was a cut, `unlink(source)` → `reload`.
- **Rename.**
  1. `renaming = selected`; the field is pre-filled and focused.
  2. Keys are routed to the field. Enter → `copy_file(old,new)` + `unlink(old)` → `renaming=-1` → `reload`. Esc → `renaming=-1`.

### 4.3 Settings
- **Startup.** `knobs_load` (reads `/sys/settings`) and `colours_load` (reads `/sys/theme` and `/zelr.cfg`).
- **A change.**
  1. Slider, toggle or colour slider → `touch()` (`dirty=1`). `flush(in.down)` → `save()` straight away, or held until 8 ticks have passed while dragging.
  2. A button → `save()` immediately.
  3. `save()` → `spit("/zelr.cfg")`.
  4. The WM's next check (≤ 25 ticks) → `theme_reload` → `apply` each key (clamped) → `derive` → repaint, and `apply_screen_size` if anything changed.
  5. Only now do `/sys/settings` and `/sys/theme` reflect the new state.
- **Periodic.** `knobs_load` every >15 frames when `!dirty && !in.down`.
- **Reset.**
  1. The first press arms it for 3 s.
  2. The second press runs `reset_everything`: knobs back to `def`, palette to preset 1/light/modern, `save`, `colours_load`.

### 4.4 Notes
- Startup file selection: argv, then `/cfg/notes-open` (read, then deleted), then `/home/untitled.txt`.
- Edits change `buf`, `len`, `cursor` and set `dirty`. Save → `spit(path)` → `dirty=0`.
- Open, or Enter in the path field → `load(field)`. This replaces the buffer unconditionally.

### 4.5 Music
- **Startup.** `sound_info` → `rescan` → optional argv song (inserted first, played).
- **Each frame.** If playing: `pump()` (read, resample, `sound_write`, which blocks when the ring is full). `pump()` returns 0 at the end of data → "finished". `tune_step()` if a tune is running.
- **Play.** `open_song(selected)`. **Stop.** Close the fd.

### 4.6 Calculator state machine
Variables `(entry, typing, acc, pending, error)`:

| Input | Effect |
|---|---|
| digit | if `!typing`, start a new entry (`typing=1`); append |
| operator | `pending && typing` → `apply()` (acc = acc op entry; shown); else `acc = entry`. Then `pending=op`, `typing=0` |
| `=` | `pending` → `apply()` then `pending=0`; else `acc = entry` |
| `C` | everything reset to "0" |
| divide by zero | `error=1`: display "cannot"; every key but C ignored |

### 4.7 Monitor
Every 25 ticks it runs `sample()`: walk `tasks()`, compute the per-task `did`, the total and the history, then `sysinfo()`. It redraws every 60 ms. Stop → `kill(pid)` → resample.

### 4.8 Blackjack state machine
```
ST_BET --Deal/Enter/Space--> deal_round:
   reshuffle if first/at cut; bet; deal P,D,P,D (9-tick spacing); state=ST_PLAY
   up==A && bank>=wager/2         -> ST_INSURE
   up in {A, 10-value} && dealer BJ -> hole shown, settle -> ST_OVER
   player BJ                       -> done, finish_hand
ST_INSURE --y/n--> answer_insurance: insurance=wager/2|0; ST_PLAY; dealer BJ -> settle; player BJ -> finish_hand
ST_PLAY  (input only when every dealt card has landed)
   hit (auto-stand at >=21) | stand | double (one card, done) | split (move later hands up, new hand gets card[1],
   first hand gets a new card; split aces done) | surrender
   -> next_hand(): skip done hands; a 1-card split hand gets its 2nd card (auto-done on split ace or 21)
   -> no hands left: finish_hand(): any live hand -> dealer_plays() (hole shown, draws queued until >=17)
                                     -> ST_DEALER ; else hole shown + settle -> ST_OVER
ST_DEALER --all dealer cards landed--> settle -> ST_OVER
ST_OVER --Deal--> next round (same wager, clamped to bank)
```

### 4.9 Poker lifecycle
```
new_game: stacks=1000, button=-1 -> new_hand
new_hand: shuffle, button -> next non-out seat, deal 2 rounds, blinds (heads-up: button=SB), turn=next_seat(BB), PS_ACT
PS_ACT: seat `turn` acts (you: buttons/keys once cards landed; AI: once think_until passed)
   act_* -> pass_turn: 1 live -> uncontested -> finish -> PS_OVER/PS_GAMEOVER
                       betting_done -> advance_street: flop(3)/turn/river; if <2 can act, recurse; after river -> showdown
                       else next seat
showdown: evaluate live hands, award (side pots, odd chip), headline, finish -> PS_SHOWDOWN or PS_GAMEOVER
PS_OVER/PS_SHOWDOWN --Next hand/Enter/Space--> new_hand ; PS_GAMEOVER --New game--> new_game
```

---

## 5. Interfaces

### 5.1 What this area exports
- **`cards.h`**, used by `blackjack.c`, `poker.c`, `poker.h` and `cardtest.c`: card macros and names, `rng` / `rng_stir` / `rng_start` / `rng_next` / `rng_below`, `shuffle`, `deck_fill`, `tri`, `pip`, `card_face`, `card_back`, `card_ring`, `money`.
- **`poker.h`**, used by `poker.c` and `cardtest.c`: `HC_*`, `HAND_CAT`, `HAND_KEY`, `eval5`, `eval_best`, `eval_best_five` (unused), `hand_words`, `HAND_SHORT` (unused).
- **Text for the desktop's find bar.** Only blackjack and poker call `win_set_text` (the kernel copies up to `WM_TEXT_MAX 4096` bytes, syscall.c:964-975). Neither handles `WIN_EV_FIND` (ui_feed ignores it). Files, notes, settings, music, calc, monitor and paint are **not searchable**.
- **`/zelr.cfg`.** Written by settings. Read by the kernel theme (`theme_reload`) and by every ui.h app's `ui_load_theme`. The kernel also writes it (`theme_save`, for example on a dock volume change, `theme.c:471-473`), so there are two writers and the last one wins.
- **Clipboard.** Notes copy/cut/paste use the single system text buffer. The Files context-menu Copy puts a path on it.

### 5.2 What this area depends on
| App | Kernel services and files |
|---|---|
| files | readdir, open/fread/fwrite/close, mkdir, unlink, rmdir, spawn, spawn_argv, clip_set; `/bin/notes`, `/bin/music` |
| settings | `/sys/settings` (theme.c KNOBS via sysfs.c:321), `/sys/theme`, `/sys/screen`, `/sys/{memory,cpu,devices,version,uptime,net}`, `/zelr.cfg`; the WM reload cadence and `apply_screen_size` |
| notes | slurp/spit, unlink, clipboard |
| paint | windows only |
| music | readdir, open/fread/seek/close, sound_info/sound_write (sound.c ring, volume) |
| calc | windows only |
| monitor | tasks, sysinfo, kill, getpid (scheduler slices/idle_ticks, syscall.c:477-504, 994-1014) |
| blackjack/poker | ticks, getpid, win_set_text |
| all ui.h apps | `/zelr.cfg`, `/sys/theme` (every frame), `ticks` (caret) |

- WM key delivery: programs receive `KEY_CODE | KEY_MOD_CTRL` (wm.c:3982). The keyboard folds ctrl+letter to 1..26 (keyboard.c:156). Alt chords, Esc, find-bar keys and launcher typing are consumed by the desktop first (wm.c:3952-3967).

---

## 6. Concurrency, locking, memory ownership, invariants

**Threading and memory.**
- Every program is single-threaded. The only concurrency is with the kernel: the WM compositor and the theme reload, the sound ring drained by hardware, and other programs sharing the clipboard and `/zelr.cfg`.
- All buffers are static or on the stack, and there is no heap.
  - Large statics: files `copy_file` 64 KiB; notes `buf` + `incoming` 128 KiB; music 8 KiB + 8 KiB.
  - Settings keeps several 2 KiB stack buffers per frame (`knobs_load`, `save`, `colours_load`, `page_file`, `show_file`).
- `win_surface` memory belongs to the kernel mapping. Apps re-fetch the pointer every frame (except paint, which is not resizable).
- `win_set_text` copies its buffer, so the caller may reuse it (zelr.h:909-910).

**Settings and the kernel.** The kernel's view (`/sys/settings`, `/sys/theme`) lags the file by up to 25 ticks after each write, because nothing re-reads it except the WM loop (`wm.c:3994-3996`; the only other `theme_reload()` callers are init and selftest). Settings sometimes reads the kernel's view immediately after writing. See §10.

**Sound.** `sound_write` blocks inside the kernel while the ring is full (it sleeps 2 ms per retry), so music's UI loop is paced by audio. The tune writes whole notes synchronously, which stalls the UI.

**Invariants.**
- Poker: the sum of stacks plus `pot_total()` stays at 4000 (every chip moved goes through `commit` or `award`/`uncontested`; `award` distributes `each·winners + odd = pot` per layer). A seat with stack 0 is marked out at `finish()`. `acted` is cleared only on a full raise or a new street.
- Blackjack: `bank >= 0` holds because the wager is clamped to the bank and double/split require `bank >= bet`. The shoe is always a permutation of 6×52 after `reshuffle`.
- `eval5` values: 0 is never a real hand, so `best=0` is a safe initial value in `eval_best` and `award`.
- Calc: while typing, `entry` is always a valid decimal string of at most 24 characters. `error` latches until C.

---

## 7. Limits and magic numbers

| Where | Value | Meaning |
|---|---|---|
| files.c:19 | 512 | directory entries read (first 512 readdir indices; the total is capped too) |
| files.c:20 | 64 | name buffer; zelr strncpy keeps ≤ 62 characters (a 63-character name is truncated) |
| files.c:265 | 65536 | copy / move / rename size limit (larger files are silently truncated) |
| files.c:50 | 32 | filter text (31 characters) |
| files.c:340 | 780×520 | window |
| files.c:303-305 | 40 / 28 / 124 | toolbar, crumb, sidebar |
| files.c:77 | 130/110/110/130 | dropdown widths |
| settings.c:39 | 48 | max knobs (the kernel has 31) |
| settings.c:42-43 | 20 / 26 | key / label buffer sizes |
| settings.c:124,198 | 2048 | `/sys/settings` read buffer, `/zelr.cfg` write buffer |
| settings.c:236 | 8 ticks | slider write hold-off |
| settings.c:620 | 300 ticks | reset arm window |
| settings.c:796 | 15 frames | knob re-read period |
| settings.c:834 | 90 ticks | "saved" message |
| settings.c:758 | 700×580 | window |
| ui.h:234 | 1024 | `ui_load_theme` reads only the first 1023 bytes of `/zelr.cfg` |
| notes.c:16 | 65536 | buffer (a file larger than 65,535 bytes loads truncated) |
| notes.c:17 | 256 | path |
| notes.c:167 | 5·MONO_W | gutter |
| notes.c:247 | 4 | spaces per Tab |
| notes.c:317 | 30 ticks | caret blink |
| notes.c:170 | 700×520 | window |
| paint.c:12-14 | 58 / 24 / 4 | toolbar, swatch, gap |
| paint.c:25 | {1,3,6,12} | brush radii |
| paint.c:233 | 10 ms | loop sleep |
| paint.c:205 | 640×420 | fixed window |
| music.c:19-22 | 64 / 64 / 128 / 2048 | files, name, path, frames per block |
| music.c:40 | 48000 / 2 | default device rate and channels |
| syscall.c:456 | 4096 | frames per sound_write call |
| music.c:440 | 4 / 30 ms | loop sleep while playing / idle |
| music.c:292 | 520×420 | window |
| calc.c:17-18 | 1e6 / 24 | scale, entry length |
| calc.c | ~9.22e6 | real-valued limit of \|a·b\| and of a dividend before i64 overflow |
| calc.c:208 | 280×380 | window |
| calc.c:250 | 86 | display height |
| monitor.c:25-27 | 32 / 120 / 250 | tasks listed, history samples, sample period in ms (as 25 ticks) |
| monitor.c:360 | 60 ms | frame sleep |
| monitor.c:186 | 640×560 | window |
| blackjack.c:27-33 | 6 / 312 / 234 / 12 / 4 / 9 | packs, shoe, cut, hand max, hands max, deal ticks |
| blackjack.c:63-64 | 1000 / 25 | bankroll, default bet |
| blackjack.c:507-512 | 5/25/100/500 | chips |
| blackjack.c:551-552 | 62×88 (48×68 if w<640) | card size |
| blackjack.c:516 | 800×560 | window |
| poker.c:35-41 | 4 / 1000 / 10 / 20 / 7 / 45 / 55 | seats, stack, SB, BB, deal ticks, think min/span |
| poker.c:478-497 | 620/62, 260/11, 720/50, need−4, stack/25 | AI thresholds |
| poker.c:437-447 | 520+38·hi; 30·hi+12·lo +70/+55/+25/−40 | preflop strength |
| poker.c:457 | 105·cat + 6·key0 | postflop strength |
| poker.c:678 | 54×76 | card size |
| poker.c:640 | 940×660 | window |
| blackjack/poker publish | >25 ticks, 512 bytes | find-text refresh period and size |
| cards.h:81-84 | 0x243F6A8885A308D3, 2654435761, <<17 | seed constants |
| cards.h:76 | 0x9E3779B97F4A7C15 | stir constant |
| cards.h:93 | 0x2545F4914F6CDD1D | xorshift* multiplier |
| cardtest | 56 checks; 40 shuffles; 6000 draws, 800-1200 each | |
| ring3check.py:41 | 120 s | cardtest timeout |

---

## 8. Tests

| Test | What it covers here |
|---|---|
| `userland/cardtest.c` via `tools/ring3check.py` (SUITES :41) | The evaluator ladder, wheel, tie-breaks, 7-card selection, hand words, shuffle integrity and variety, the `rng_below` range with a coarse flatness check, blackjack ace arithmetic (on a copy), `money()` formatting. Passes on the `CARDTEST_PASS` marker |
| `tools/appcheck.py` (gate `apptest`, gate.sh:456-459) | Calculator 78/4 = 19.5 by pixel comparison; the monitor reacting to a new program; music playing a 22050 Hz mono WAV from `/usb` given as argv, measured at 440 Hz ±8 % in the recording |
| `tools/gamecheck.py` (gate `gametest`, gate.sh:526-530) | The Games launcher row; blackjack starts with an empty table, deals, the dealer plays out after stand, chips return; poker deals your 2 cards and calling produces a flop |
| `tools/setcheck.py` (gate `settest`, gate.sh:462-468) | `/sys/settings` has ≥25 sane rows including the key sample; `/sys/theme` gives colours; `write /zelr.cfg dock_h 96` moves the dock; Settings opened from the launcher (typing "sett"); the Everything-page toggle for `dock_brand` removes and restores the dock badge; the written `/zelr.cfg` contains `dock_brand 1`, `dock_h 44`, `wallpaper 11`, `anim_ms 120`. 10 checks |
| `tools/shotcheck.py` | Settings opens from the launcher (System → Settings); all 6 swatches are visible; clicking teal swaps the WM chrome from indigo to teal |
| `tools/deskcheck.py` | Uses Paint as a second window (Media → Paint) for the alt+tab and chip tests |
| `tools/defaultcheck.py` (gate step "and on what a new machine looks like", gate.sh:267-276) | Intended to compare settings.c against theme.c defaults. **Now vacuous** (§10) |
| Kernel selftest `test_builtin` (selftest.c:878-916) | `/bin/paint` exists, is > 1 KiB, has the ELF magic, and cannot be written or deleted; every builtin is present |
| Kernel selftest `test_theme` (selftest.c:1057-1135) | The `/zelr.cfg` contract settings relies on: save/reload round trip, a hand-written file picked up, clamping every knob to its table bounds |
| Kernel selftest `test_idle_accounting` (selftest.c:1351-1407) | The slices vs `idle_ticks` split that the monitor's CPU figure relies on |

**Not tested at all:** Files (navigation, copy, rename, menus), Notes (editing, save), Paint drawing, the Music tune or its UI, the Monitor Stop button, blackjack split/double/insurance/surrender payouts, poker side pots, the odd chip, heads-up blinds and AI behaviour (only the evaluator is tested), and the Settings Colours/Screen/Reset flows (only a swatch click and one toggle are).

---

## 9. How to extend

**Add a new desktop program.**
1. Write `userland/<name>.c` on the §2.3 skeleton. `build.sh` builds every `*.c` automatically.
2. Add `.incbin` symbols in `kernel/builtin.S` and a row in the `kernel/builtin.c` table. Keep within `SYSFS_MAX_PROGRAMS`: the selftest compares the counts because the 17th program was once silently dropped (selftest.c:881-887).
3. Add it to a launcher category in `wm.c:393-419`, and optionally to the desktop icons (wm.c:868-878) or the default pins (pins.c:19-26).

**Make a program searchable with ctrl+f.** Call `win_set_text(win, buf, n)` whenever what is shown changes, as blackjack.c:379-411 and poker.c:889-919 do. To support "go to match", handle `WIN_EV_FIND` and `win_find_query` as `browser.c` does (browser.c:254-262, 1374). Notes is the obvious candidate: it currently publishes nothing.

**New file-type mapping in Files.** Add a `KIND_*`, extend `kind_of` (files.c:117-126), `kind_name`, `kind_tint` and the `open_entry` switch (:250-257), and perhaps a context-menu item.

**New setting.** Add one line to `KNOBS[]` in `kernel/theme.c:88-131` plus a field in `theme_t`. It appears on Settings' Everything page automatically. Also call `row(s, in, t, x, y, w, "key")` on the page that fits. Watch these limits:
- key < 20 characters (`knob.key[20]`), label < 26
- `MAX_KNOBS 48`
- `/sys/settings` must fit settings' 2048-byte buffer (about 1.05 KB today)
- `/zelr.cfg` must fit the kernel's 2048-byte reload buffer and its own 1536-byte `theme_save` buffer (theme.c:415, :499)

**Pitfalls the comments warn about.**
- Struct assignment and byte-copy loops may compile into calls to a `memcpy` symbol that does not exist (blackjack.c:109-119; music.c:320-322). Use field copies or `volatile`.
- No floating point anywhere (calc.c:3-5; music.c:12-14; blackjack.c:496-497 uses a hexagon table instead of sine).
- In `/zelr.cfg`, `look` and `light` must precede the colour lines (settings.c:194-196).
- Frames must be finished before `win_commit`. The window server now double-buffers; README 992-1001 describes the history.
- Immediate-mode input is first come, first served. A widget that paints *over* others (menus, popups) must be hit-tested **before** them, or the widgets underneath must be skipped while it is open (Files gets this wrong, §10).
- Read `in.keys[0..nkeys)` rather than `in.key` if more than one key per frame matters (ui.h:349-356; only browser.c does).
- Compare key codes with `KEY_CODE(k)` and handle `KEY_CTRL(k)`. The raw value carries `0x20000` for ctrl chords.
- After writing `/zelr.cfg`, `/sys/theme` and `/sys/settings` stay stale until the WM reloads (≤ 250 ms). Do not read them back immediately.
- Monitor's tick maths assumes 100 Hz (`SAMPLE_MS/10`).

---

## 10. Doc drift and suspicious code (verified by reading)

### 10.1 Likely bugs

1. **Files: silent 64 KiB truncation causes data loss on move and rename.** `copy_file` slurps into `static char buf[65536]` (files.c:265-268). `slurp` returns the capped length with no error (zelr.h:381-393), and `spit` writes that. Cut+Paste then `unlink`s the source (:282-285), and Rename does `copy_file(...)==0 → unlink(from)` (:391). Any file larger than 65,536 bytes (for example a WAV) is cut down to 64 KiB and the original is deleted.
2. **Files: File → Close does nothing.** `closing = 1` at :638 sets a variable declared inside the loop (:377) that was already tested at :382. It is never read again, and the next iteration re-declares it as 0.
3. **Files: the context menu cannot be used with a normal click.** `ui_menu` returns an item only on **release** (ui.h:829). But files.c:689-691 closes the menu when `in.pressed` is set, which is the press frame. An item can therefore be chosen only if press and release arrive in the same frame, which is not an ordinary human click. Even then, if the item lies over a list row, the row loop has already consumed `released` (:564-568).
4. **Files: dropdown items are shadowed by the widgets under them.** The menus are painted last (:610) but hit-tested last too. Toolbar buttons (:463-476), Places rows (:447-452) and list rows (:564-568) consume `released` earlier in the frame. So a click on a menu item that overlaps them fires the underlying control, and the menu stays open. Concretely:
   - Help → "About Files" (y 26-52, starting at x ≈ 135) lies over Up/Open/New folder (y 25-55, x 132-404).
   - File → Rename, Delete and Close (y 78-156, x 2-132) lie over the Home, Documents and Programs rows (x 2-120, y 88-170).
   - Edit and View items overlap the Up/Open buttons.
   - The files.png screenshot confirms the title positions.
5. **Files: rename state survives navigation.** `renaming` is a row index that is not reset by `go_to`/`go_up`/`reload` (:207-219). If you start a rename, click a Place and press Enter, then `entries[renaming]` of the *new* directory is renamed (copy+unlink) (:385-395).
6. **Files: the wheel fights the selection.** When something is selected, the list is forced back to show it every frame (:505-507), before the wheel offset is applied (:509). Scrolling the selection out of view snaps back. notes.c:277-288 names and avoids exactly this problem.
7. **Files: inconsistent commands.**
   - File → Delete uses `unlink` for folders (:633-637); FAT refuses that (fat.c:1583), so the result is "could not delete". The context menu and the Delete key use `rmdir`.
   - Edit → Copy (:640-643) does not `clip_set`, while the context-menu Copy does (:673).
   - The status message is never cleared after the first `say()` (only `say("")` at :354), so the selection detail (:590-599) disappears for the rest of the session.
8. **Notes: after a click, the next keystroke eats the previous one.**
   - A click sets `sel_anchor = cursor` (:335). `insert_text` does not reset the anchor (:36-44).
   - Typing "a" after a click leaves `anchor ≠ cursor`, so "a" is now selected. Typing "b" deletes it (:245). Result: "b".
   - Arrow keys after a click likewise extend a selection that Backspace or typing then deletes. The `shifted` variable (:226, :249) is dead.
9. **Notes and the other ui.h apps: keys beyond the first per frame are dropped.** Notes reads only `in.key` (:224). ui.h:349-356 says keys used to go missing ("the editor dropped characters out of sentences") and fixed it by adding `keys[]`. Only browser.c iterates `in.keys` (browser.c:1391-1412). The same applies to Files, Calc, Settings, Blackjack and Poker.
10. **Notes and `ui_field_key`: Ctrl chords insert control bytes.** The WM passes `KEY_CODE|KEY_MOD_CTRL` (wm.c:3982) and ctrl+s arrives as `0x20013` (keyboard.c:156). notes.c:244 accepts `k >= 32 && !KEY_IS_SPECIAL(k)` and inserts `(char)k` = 0x13. ui.h:616-617 tests `key < 32` on the full value, so it inserts too. Ctrl+S, C, V, X and Z silently put invisible bytes into documents, the Files filter and the rename field. (Ctrl+C also raises SIGINT for forked children of the console reader, keyboard.c:44-47 / signal.c:262-287. Desktop-spawned apps have `parent_pid 0` and are not affected.)
11. **Notes: loses data without warning.**
    - Open, or Enter in the path field, replaces the buffer without checking `dirty` (:260, :271).
    - There is no save-as: `save()` writes `path`, not the field.
    - Closing the window discards unsaved edits.
    - Files larger than 65,535 bytes load truncated (:113), and saving writes the truncation back. Files opens every unknown file type in Notes (files.c:256).
12. **Music: the tune overflows `short`.** `y = (half·(32768−half))>>13` peaks at 32768, and `v = (short)(y*3)` wraps for about 82 % of each half-cycle (music.c:275-278). The kernel's copy of the same parabola clamps to 32767 and does not multiply (sound.c:117-122). The tune plays at the right pitch but badly distorted.
13. **Music: tune pacing contradicts its comment.** `tune_until = ticks()` (:288), so the next note is written on the very next frame. Nothing is "left to drain" (:257-258). `sound_write` then blocks for most of each note (sound.c:265-282) and the UI stalls during the tune.
14. **Music: channel count assumed.** `dev_chans` is stored (:40, :299) but never used. `pump` always writes 2-sample frames, while the kernel sizes a frame as `count·sound_channels()·2` (syscall.c:458). A mono device would be fed wrongly. Whether any driver reports a channel count other than 2 is unverified.
15. **Music: minor resampler slip.** When `step > 1.0`, `frac = pos & 0xFFFF` (:241) drops the whole-frame overshoot at a block boundary. At most one source frame per block is not skipped, which is inaudible. Also, the comment "not guaranteed to be in any particular order" (:113-119) is only half honoured: a `data` chunk before `fmt` fails (:154-168).
16. **Calc: silent i64 overflow.**
    - `(acc*rhs)/SCALE` overflows once `|a·b| > ~9.22·10^6` in real units (5000 x 5000 gives garbage) (:103).
    - `(acc*SCALE)/rhs` overflows once `|dividend| > ~9.22·10^6` (:106).
    - `parse_entry` overflows above 13 integer digits (:45), yet `ENTRY_MAX` allows 24.
    - The header's "leaves room for the multiply" (:6-8) overstates the headroom.
17. **Calc: out-of-bounds read.** `negate` reads `entry[25]`, one past the array, when `entry_len == 24` and the string starts with `-` (:164).
18. **Monitor: the CPU metric mixes units.**
    - `slices++` happens on every switch that picks the task: PIT ticks, **yields** and every CPU's **local APIC ticks** (idt.c:256-258; sched.c:520).
    - `idle_ticks` are PIT ticks spent halted (sched.c:635-641).
    - So `did = Δslices − Δidle` over PIT ticks counts scheduling picks, not work. A frame loop that sleeps each frame (via SYS_SLEEP rather than `task_idle_wait`) is charged about one slice per wake-up. On SMP the total is not divided by the CPU count; it is only capped at 100 (:112-113).
    - The comment (:10-12) and README (930-931) say the count "increments on every timer tick".
    - docs/monitor.png shows six sleeping tasks at 90 % each on one CPU, but that screenshot predates the idle task (no "idle" row), so it is illustrative only.
19. **Monitor: a doc line and a UI clash.**
    - The header says "two system calls and one file" (:3); no file is read.
    - The graph reads `history[history_n−1−i]` before checking the index (:172-173), which reads `history[-1]` for the first 30 s.
    - The Stop button sits at y h-36..h-6 (:341, :350) and the status bar painted afterwards covers h-26..h (:358; ui.h:765-766). The label is mostly hidden.
    - The tasks card shows `nrows` (capped at 32), not `info.tasks`.
20. **Monitor: Stop can end kernel tasks, including "idle".** `sys_kill` refuses only itself and dead tasks (syscall.c:321-334). `signal_end_task` has no user-task check (signal.c:29-45). `pick_next` falls back to `idle_of[cpu]` regardless of its state (sched.c:473), and the reaper frees dead tasks after a grace period (sched.c:544-551). The consequences are **unverified** but potentially a use-after-free. It is reachable by selecting the "idle" row and pressing Stop.
21. **Settings: Reset is undone by stale state.** `reset_everything` calls `save()` and then immediately `colours_load()` (:282-283). That reads `/sys/theme` before the WM has reloaded the file (the only reload is every 25 ticks, wm.c:3994-3996). `preset`, `light`, `look` and the colours come back **pre-reset**, the page shows them, and the next save of anything writes them back into `/zelr.cfg`.
22. **Settings: likely lost update from periodic re-reads.** `knobs_load` runs every >15 frames when `!dirty && !in.down` (:796-799), including within the ≤ 250 ms after a `save()` while `/sys/settings` still holds the old values. A just-changed control then reverts on screen, and a second change made before the WM catches up writes the stale value back. The same applies to the corner/shadows side-effect of Modern/Built (:410-414). This is timing-dependent.
23. **Settings: `text_dim` and duplicated defaults.**
    - Settings never writes `text_dim`, while the kernel's `theme_save` writes all five colours whenever the accent is custom (theme.c:520-540). After a dock volume change, the next Settings start sees `surface` in the file and assumes `custom = 2` (:267-268).
    - The fallback desktop `0x102542` (:250) has drifted from the kernel's `0x10254A` (theme.c:59). This is harmless because `/sys/theme` always supplies it.
24. **`tools/defaultcheck.py` checks nothing.**
    - Its settings regex `^static int <key>\s*=\s*(-?\d+)\s*;` (:86-90) cannot match settings.c:54, which is a combined declaration, and no other line matches.
    - Its saved-keys regex looks for `put_kv(out, n, "…")` (:100); `save()` uses `line_num`/`line_hex`.
    - `theme_init` no longer assigns the knob fields (theme.c:324-342).
    - Result: the gate step "and on what a new machine looks like" (gate.sh:267-276) passes with only "the wallpaper enum could be read" doing any work. Its docstring (:3-16) and the gate comment still describe duplicated defaults in settings.c that no longer exist.
25. **`cards.h rng_below` is still biased.** `limit = 0xFFFFFFFF − (0xFFFFFFFF % n) − (n−1)` (:106) accepts `limit+1 ≡ 2 (mod n)` values for every n ≥ 3. So residues 0 and 1 each get one extra draw: for n = 52 they are favoured by 1 in 82,595,523, the same order of bias the comment says it removes ("with no bias", :96-103). The correct form is `limit = 0xFFFFFFFF − ((0xFFFFFFFF % n) + 1) % n`. The cardtest flatness check (6000 draws) cannot see this.
26. **Poker: "does not reopen the betting" is not enforced.** The claim is at :13-16 and :387-390. After an incomplete all-in raise, earlier callers still get the turn (`street_bet < level` in `betting_done`, :310). They may then raise: your slider is shown whenever `most > level` (:827), and the AI raise branch runs for `str > 720` (:495-497). Withholding the `acted` reset changes nothing, because those seats owe chips anyway.
27. **Poker: minor issues.**
    - Dead branch at :410: after `level = to` (:406), `level == to` is always true, and preflop `to > level ≥ 20`.
    - `act_raise` enforces no minimum for raises that are not all-in (:394-396); only the callers limit it.
    - A run-out calls `advance_street` recursively with `at = ticks()` each time (:342), so turn and river land at +7 ticks while the third flop card lands at +14. The winner's headline appears before the board has visibly finished.
28. **Blackjack: rare hang and rule deviations.**
    - `dealer_plays` loops forever if the dealer reaches `HAND_MAX` (12) cards below 17, because `give()` becomes a no-op (:127, :195-200). Astronomically unlikely.
    - The dealer draws against a lone player natural (:362 → `finish_hand` → `dealer_plays`). The payout is unaffected, but the comment at :204-206 covers only bust/surrender.
    - The first split hand is not auto-stood on 21 (:311-312), unlike the second (:226), and can be doubled.
    - `shuffled_at` is documented as a count (:54) but is a flag, and `note[]` is never written.
29. **Cosmetic and dead code.**
    - `card_back`'s `i < 0` iterations draw nothing (cards.h:261-265), so the two lower triangles of a card back are hatched in one direction only.
    - Unused: `CARD_W`/`CARD_H`, `HAND_SHORT`, `eval_best_five` (whose comment claims showdown use), `monitor last_total`, `music dev_chans`.

### 10.2 Doc drift
30. **cardtest tests a copy.** Its "blackjack's arithmetic, which is the other half of the shared code" is a copied `bj_value` (cardtest.c:84-97), not blackjack.c's `value_of`. The two are identical today but free to drift.
31. **gamecheck.py docstrings are loose.**
    - "the only white on the table is the face of a card" (:10-15, :79-83), but blackjack draws labels in pure `0xFFFFFF` (blackjack.c:559-560, 580/607, 618-619, 640, 652, 661-662); the thresholds tolerate it.
    - "the second one [Return] would stand the hand" (:136-138): Return is unbound in ST_PLAY. In ST_OVER it would deal a new hand.
32. **README.**
    - "Eleven wallpapers" (README 915): the enum and Settings have 12.
    - "userland/ … a terminal, paint, settings and three small tests" (README 1749-1750) is stale.
    - "A task now carries one string" (README 1005; also files.c:12-14 and :222-223): it is a full argv via `SYS_SPAWN_ARGV` (zelr.h:108-110; `SYS_GETARG` 44 is retired).
33. **Stale leftovers.**
    - `/cfg/notes-open` (notes.c:178-188): nothing in the tree writes it any more.
    - `/music` (music.c:108): no such directory is created (`layout.c:27` makes `/home /doc /cfg /tmp`).
    - `pipeline/backlog.md:22`: "editor" is still `todo`, but Notes exists, and it does not use "the terminal's line editing" as that entry specifies.
34. **ui.h "Read once at startup"** (ui.h:183): files, settings, music, calc, monitor, blackjack and poker call `ui_load_theme()` every frame, which is two slurps per frame each. Notes is the only app that reads it once.
35. **gate.sh:527** calls the cardtest runner "ring3test"; the tool is `tools/ring3check.py`.

---

## 11. Open questions

1. What exactly happens after the Monitor stops `idle` or a kernel task such as the shell/WM host? Does `idle_of[]` keep a pointer to a freed task? This needs the scheduler owner to confirm.
2. Does the WM ever deliver a left press and release to a window within one frame? If it batches pointer events, the Files context menu (§10 #3) may work more often than a plain reading suggests.
3. Is the Settings stale-read race (#21, #22) observable in practice? It depends on the WM loop cadence under load. Was it the cause of any "settings reverted" report?
4. Are the per-task shares and processor % on the *current* kernel (with idle tasks) sensible on an idle desktop with a few apps open? The only screenshot is older.
5. How large is the user stack? Settings stacks several 2 KiB buffers per frame, and notes/music/files rely on large statics.
6. Is the `/bin*` prefix test in `kind_of` (files.c:119) meant to match `/binx` and similar names?
7. Should Files' rename use `SYS_RENAME` when the name fits 8.3 and the directory is the same, keeping copy+delete only as a fallback? Should `copy_file` stream in chunks rather than slurp?
8. Is poker's odd-chip and side-pot behaviour tested anywhere outside this code? cardtest covers only the evaluator.
