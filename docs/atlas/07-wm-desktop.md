# 07 -- Window manager, compositor, window server, theme, gfx

Atlas section for zelr (main @ 2026-09-22, two commits after v0.37.0). Pure static reading; nothing was built or run. Every claim below was checked against the source; file:line references are to the tree at the repository root.

---

## 1. Scope

| File | Lines | Role |
|---|---|---|
| `include/wm.h` | 163 | `window_t`, `wm_event_t`, event codes, WM API, `WM_TITLE_H`/`WM_BORDER`/`WM_TOP` macros |
| `include/winsrv.h` | 55 | Window-server API (ring-3 side of the WM), surface address layout |
| `include/gfx.h` | 96 | Drawing primitives: 8x16 text, off-screen surfaces, chrome (rounded rects, shadows, gradients, bevels), AA face text |
| `include/theme.h` | 228 | `theme_t`, wallpaper/look enums, knob table type, preset API |
| `include/face.h` (skim) | 5222 | Generated (tools/genface.py) anti-aliased glyph coverage for the kernel: 8 faces |
| `kernel/wm.c` | 4078 | The whole desktop: compositor, chrome, dock/panel, launcher, context menu, find, icons, wallpapers, input, main loop |
| `kernel/winsrv.c` | 430 | Handle table, surface allocation, mapping into the caller with PTE_USER, commit (double buffer), deferred resize, release |
| `kernel/gfx.c` | 503 | Implementations of gfx.h (framebuffer and surface drawing, 16-sample AA corners, glow, vignette, sheen, face rendering) |
| `kernel/theme.c` | 549 | Theme state, 6 presets, palette derivation, the 31-entry knob table, `/zelr.cfg` parser/writer, volume |
| `tools/deskcheck.py` | 799 | QEMU harness: minimise/maximise/snap/resize, panel tuck, wallpapers, alt+tab/d, wheel, resolution, band, menus, autodesktop |
| `tools/shotcheck.py` | 300 | QEMU harness: terminal draws, launcher opens Settings, clicking an accent recolours the WM |
| `tools/setcheck.py` | 292 | QEMU harness: `/sys/settings` table sanity, `dock_h` written from console moves the dock, Everything-page switch, file contents |
| `tools/defaultcheck.py` | 138 | Static source check: kernel vs settings.c defaults (currently vacuous, see §10) |
| `tools/findcheck.py` | 166 | QEMU harness: ctrl+f bar, browser highlight of a found word, miss, escape |
| `tools/shots.py` | 436 | Produces the 12 README screenshots in `docs/*.png` |

Also read to trace interfaces (not owned by this area): `kernel/apps.c` (81, the "System info" window, read fully), `kernel/syscall.c` window syscalls (412-421, 916-992, table 1021-1085), `kernel/sysfs.c` `render_settings`/`render_theme` (304-351), `kernel/shell.c` `enter_desktop`/`desktop`/autodesktop (213-243, 300-303, 591-608), `userland/ui.h` `ui_load_theme` (183-300), `sdk/zelr.h` window API (813-945), `kernel/paging.c` (157-179, 203-290, 318-344, 388-404), `kernel/pmm.c` (129-181), `kernel/fb.c` `fb_flush`/`fb_flush_rect` (410-526), `kernel/idt.c` (150-289), `kernel/sched.c` (97-118, 423-464, 635-641, 668-684), `kernel/signal.c` (29-45), `kernel/selftest.c` test_gfx/test_wm/test_winsrv/test_theme/test_pins (666-876, 998-1135), `include/pins.h`, `kernel/pins.c`.

---

## 2. Big picture

**What it is.** A compositing window manager that lives entirely in the kernel (`kernel/wm.c`) and runs as a loop (`wm_run`, wm.c:3878) inside the kernel *shell task* -- not a task of its own. `enter_desktop()` (shell.c:217) spawns `/bin/term` and then calls `wm_run()`, which only returns when the user leaves the desktop (Escape, launcher "Leave desktop", failed power-off). The shell calls `enter_desktop()` automatically at boot when `fb_active() && !console_only && theme()->autodesktop` (shell.c:607-608); `console` on the kernel command line sets `console_only` (main.c:315). Typing `desktop` at the console re-enters (shell.c:300-303).

**Everything else on the desktop is ring 3.** Programs talk to the WM only through the *window server* (`kernel/winsrv.c`) via int 0x80 syscalls 7-12, 36, 37, 56, 57. A program gets an integer handle (slot index 0..7) and a pointer to its own pixels, mapped at `WINSRV_SURFACE_BASE + handle*8 MiB` in its address space with `PTE_USER`. The only kernel-drawn window left is "about" / System info (`kernel/apps.c`), which uses the in-kernel callback path (`on_mouse`/`on_key`/`on_close`).

**Compositing model.** Every frame redraws the *entire* back buffer (`fb_pixels()`): wallpaper → desktop icons → rubber band → every window bottom-to-top (chrome, then a `memcpy` row blit of its surface) → snap preview → resize preview → dock → volume popup → find bar → network popup → launcher → context menu → pointer. Then either `fb_flush()` (whole screen, which fb.c itself reduces to changed row bands against a `sent` mirror, optionally on a helper CPU) or `fb_flush_rect()` of one damage rectangle (wm.c:2894-2952). Stated reasons: drawing into RAM is cheap; "Copying the result out is ninety two per cent of a frame, measured, because video memory is uncached" (wm.c:220-230), so only the menu/context-menu animations and the panel slide ask for partial flushes. A maximised window that really covers the screen suppresses the wallpaper, icons and everything under it; the screen is filled flat with the desktop colour instead ("chrome blends with what is beneath it", wm.c:2899-2903).

**Double-buffered surfaces.** Each ring-3 window has two page-aligned buffers: `pixels` (mapped into the program, drawn into) and `shown` (what `window_t.canvas` points at). `win_commit` copies `pixels`→`shown` word by word and marks the window dirty (winsrv.c:34-58, 80-88, 400-413). Reason: before this, programs whose frame starts by clearing the window were composited mid-frame ("a maximised card game with no cards").

**Deferred resize.** When the WM resizes a program's window (maximise, snap, corner drag, screen change) it only records `want_cw/want_ch` and pushes `WM_EV_RESIZE`; the surface swap happens on the *program's* next syscall into the server (`apply_pending`, winsrv.c:362-370), "when by definition it is not" drawing (wm.h:110-115).

**Theme = a file.** All colours and ~all geometry come from `theme()` (theme.c). The settings program (ring 3) cannot reach the WM, so it writes `/zelr.cfg`; the WM re-reads that file roughly four times a second (wm.c:3991-3996) and repaints on change. Numeric settings are one table (`KNOBS[]`, theme.c:88-131, 31 entries) used by the parser, the writer, `theme_defaults()` and `/sys/settings` (sysfs.c:321-330), from which userland/settings.c builds its "Everything" page -- so kernel and settings cannot drift on the list of knobs. Colours are separate keys and are exported live via `/sys/theme` (sysfs.c:339-351). There is **no theme syscall**: ring-3 programs parse `/zelr.cfg` + `/sys/theme` themselves (`ui_load_theme`, userland/ui.h:233) and derive their own palette.

**Two looks.** `look 0` = MODERN ("material": AA rounded panes, hairlines, translucent glass dock, sheen, drop shadows) is the default; `look 1` = BUILT (bevels lit from the top-left, square windows, no shadows, gradient title bars). Both are complete code paths in wm.c; the palette derivation in theme.c differs per look (theme.c:289-319).

**No floating point.** All curves are integer: `ease_out` cubic in 1/256 (wm.c:304-310), a 17-entry quarter-sine table (`SINE_Q`, wm.c:744-755), corner coverage in eighths of a pixel (gfx.c:360-370), glows/vignette in thousandths.

**Frame pacing.** None fixed. The loop composites whenever `needs_composite` is set and otherwise halts with `task_idle_wait()` (a bare `hlt`, sched.c:635-641) until the next interrupt (100 Hz timer, keyboard, mouse). Sources of frames: any pointer change (whole frame), a periodic tick every `timer_hz()` (clock) or every `timer_hz()/12` ticks (≈12.5 fps) while a moving wallpaper is visible and nothing covers the screen (wm.c:4021-4031), any `wm_invalidate` (every commit), theme changes, and in-flight animations (panel slide, launcher/context-menu rise and hover) which request a frame on every pass with no cap.

---

## 3. File-by-file detail

### 3.1 `include/wm.h`

- `WM_MAX_WINDOWS 8` (4): size of the WM stack; includes kernel windows (about).
- `int wm_title_h(void); int wm_border(void);` with `WM_TITLE_H`, `WM_BORDER`, `WM_TOP = WM_BORDER + WM_TITLE_H` (20-30). Kept as macros "so the forty places that use them do not each have to change" but read the theme every time.
- Event types (36-51): `WM_EV_NONE 0`, `WM_EV_MOUSE 1`, `WM_EV_KEY 2`, `WM_EV_CLOSE 3`, `WM_EV_RESIZE 4` (x,y = new content size), `WM_EV_SCROLL 5` (y = steps, positive down), `WM_EV_FIND 6` (y = local match index to go to; the WM also sends y = -1 to clear, wm.c:3337).
- `wm_event_t { u32 type; i32 x, y; u32 buttons; u32 key; }` (53-58): 20 bytes, copied verbatim to ring 3 (`win_event` in sdk/zelr.h:842-847 is layout-identical). Buttons: bit0 left, bit1 right, **bit7 (0x80) = "this event is the initial press"** (set at wm.c:3786; documented in sdk/zelr.h:836-840 as `WIN_BTN_DOWN`, not in wm.h).
- `WM_EVENT_QUEUE 32` (60): ring of 32, one slot always empty → 31 usable.
- Callback types `wm_mouse_fn(window_t*, int x, int y, u8 buttons, bool just_pressed)`, `wm_key_fn(window_t*, char)` (63-64).
- `WM_TEXT_MAX 4096` (77): published text per window.
- `struct window` (79-127), field by field:
  - `int x, y` -- outer top-left (frame + title bar included).
  - `int cw, ch` -- content size.
  - `char title[32]` -- shown in the title bar and the dock chip.
  - `char app[32]` -- owning task's name, set by `winsrv_create` (winsrv.c:209-213). **Never read anywhere** (it served the removed pinned-apps matching).
  - `u32 *canvas` -- `cw*ch` pixels. For kernel windows owned by the window (kmalloc); for user windows it points at the slot's `shown` buffer (page-aligned inside a larger kmalloc -- must not be `kfree`d by the WM).
  - `bool open` -- set true by `wm_create`; nothing reads it.
  - `bool dirty` -- content changed since last composite; set by `wm_invalidate`/`wm_create`/resize, cleared in `composite`.
  - `on_mouse`, `on_key`, `on_close`, `void *data` -- kernel-window callbacks; `on_close` is also used by winsrv (`on_wm_close`).
  - `wm_event_t queue[32]; u32 q_head, q_tail` -- per-window event ring (user windows only).
  - `u32 q_last_buttons, q_prev_buttons` -- buttons of the last two *queued* mouse events, for motion folding.
  - `bool owned_by_user` -- surface belongs to winsrv; WM must push events rather than call callbacks and must not free `canvas`.
  - `bool resizable` -- set once by `SYS_WIN_RESIZABLE`; gates maximise button, grip, snap, WM-initiated resize.
  - `int want_cw, want_ch` -- pending WM-requested size (0 = none).
  - `char text[4096]; int textlen` -- words the program says it is showing (find).
  - `bool minimized, maximized; int restore_x, restore_y, restore_cw, restore_ch`.
- API (130-163): `wm_set_text`, `wm_find_query`, `wm_create`, `wm_close`, `wm_invalidate`, `wm_raise`, `wm_resize`, `wm_run`, `wm_quit`, `wm_active`, `wm_push_event`, `wm_pop_event`, `wm_outer_w`, `wm_outer_h`, `wm_icons_right`, `wm_work_height`.

### 3.2 `include/winsrv.h`

- `WINSRV_MAX 8` (10) -- slots; handle = slot index, global across processes.
- `WINSRV_SURFACE_BASE (USER_SPACE_BASE + 0x60000000ull)` = `0x0000_0080_6000_0000` (USER_SPACE_BASE = 0x80_0000_0000, paging.h:62) and `WINSRV_SURFACE_STEP 0x00800000` (8 MiB) (19-20). Comment: 1600x1200x4 = 7.5 MiB fits one step. The window region is above the user stack top (`USER_SPACE_BASE + 0x50000000`, user.h:109) and the mmap window (user.h:62-63).
- Prototypes (22-55): `winsrv_init`, `winsrv_create(pid, title, cw, ch)`, `winsrv_resize(pid, h, cw, ch)`, `winsrv_allow_resize(pid, h)`, `winsrv_resize_window(window_t*, cw, ch)` (WM entry), `winsrv_reap_retired()`, `winsrv_surface(pid, h, dir)`, `winsrv_size(pid, h)` (`cw<<16|ch` or -1), `winsrv_poll`, `winsrv_commit`, `winsrv_close`, `winsrv_window(pid, h)`, `winsrv_release(pid)`. (`winsrv_frames`/`winsrv_bytes` exist in winsrv.c but are not declared here.)

### 3.3 `include/gfx.h`

Groups (with implementation notes in §3.8):
- Straight-to-framebuffer 8x16 text: `gfx_char`, `gfx_text`, `gfx_text_bg`, `gfx_text_width`.
- Off-screen surface ops (all clip): `surf_clear`, `surf_rect`, `surf_frame`, `surf_disc`, `surf_line` (Bresenham of discs), `surf_char`, `surf_text`, `surf_blit` (surface→fb via `fb_put`).
- Chrome to fb: `fb_round_rect`, `fb_round_frame`, `fb_shadow(x,y,w,h,r,spread)`, `fb_vgradient`, `fb_hgradient`.
- Material: `fb_round_rect_aa(x,y,w,h,r,rgb,alpha)`, `fb_glow(cx,cy,rx,ry,rgb,strength)`, `fb_vignette(strength)`, `fb_sheen(x,y,w,h,r,strength)`, `gfx_mix(under, over, alpha)`.
- Bevels: `fb_bevel(x,y,w,h, tl_outer, tl_inner, br_inner, br_outer)`, `fb_bevel_thin(x,y,w,h,tl,br)`.
- AA face: `FACE_SMALL 0` (13px), `FACE_BODY 1` (15), `FACE_HEAD 2` (20), `FACE_TITLE 3` (26), `FACE_BODY_BOLD 4` (15b), `FACE_HEAD_BOLD 5` (20b) (82-87); `face_text(x, y_top, s, fg, which)`, `face_width`, `face_height`, `face_surf_text`. Named "rather than numbers because the order changes whenever a size is added". The kernel face table has 8 entries; 6 (15px mono) and 7 (15px bold mono) have no name and are unused by the kernel.

### 3.4 `include/theme.h`

- `THEME_FILE "/zelr.cfg"` (11).
- `wallpaper_t` (13-27): `PLAIN 0, GRID 1, DOTS 2, GRADIENT 3, STARS 4, WAVES 5, WEAVE 6, AURORA 7, RAIN 8, ORBS 9, PULSE 10, BLOOM 11, WALLPAPER_COUNT 12`. **12 wallpapers**, 6 animated (see `wallpaper_moves`).
- `look_t { LOOK_MODERN = 0, LOOK_BUILT }` (42-45).
- `theme_t` (51-176): file colours `accent, desktop, surface, text, text_dim`; derived (not in file) `raised, overlay, hairline, sheen, accent_soft, accent_text, text_mute, edge_hi, edge_light, edge_shadow, edge_dark, well, glass, stroke, title_a, title_b, title_fg, title_off_a, title_off_b, title_off_fg`; flags/values `light, look, wallpaper, corner, shadows, animate, quirks, autodesktop, volume, want_w, want_h, dock_h, dock_gap, dock_side, dock_radius, dock_brand, dock_search, dock_search_w, dock_clock, clock_24, dock_hide, title_h, border, button_w, button_h, snap, desk_icons, icon_size, icon_gap, vignette, glows, anim_ms, dblclick_ms`.
- `theme_knob { const char *key; const char *label; u16 at; u8 is_bool; int lo, hi; int def; }` (210-217).
- API: `theme_init`, `theme()`, `theme_reload` (true if anything changed), `theme_save`, `theme_set_volume`, `theme_set_volume_live`, `theme_knob_count/at/get`, `THEME_PRESETS 6`, `theme_preset_name/accent`, `theme_apply_preset`, `theme_current_preset`, `wallpaper_moves`.

### 3.5 `include/face.h` (skimmed)

Generated by `tools/genface.py` (`FACES_KERNEL`, genface.py:953-954). `face_glyph { short w, h; short left, top; short advance; unsigned int at; }` (8-13); `FACE_FIRST 32`, `FACE_LAST 126`, `FACE_COUNT 95`, `FACE_SIZES 8` (15-18). One coverage blob (`face_px_*`, one byte per pixel, 0..255) and one 95-entry glyph table (`face_g_*`) per face. `face_t { short size; short bold, mono; const face_glyph *glyphs; const unsigned char *pixels; }` (5206-5211). `face_faces[8]` (5213-5222): 13, 15, 20, 26, 15b, 20b, 15 mono, 15b mono. Userland has its own `userland/face.h` (6 faces) and `facetext.h` (33 faces); `tools/harness.py` reads `include/face.h` to compute text widths for test coordinates.

### 3.6 `kernel/theme.c`

**State.** `static theme_t current;` (18) -- zero until `theme_init`.

**Presets** (22-32; name, accent, dark desktop, dark surface):

| i | name | accent | desktop | surface |
|---|---|---|---|---|
| 0 | teal | 2CC7A0 | 12181E | 1A222A |
| 1 | indigo (default) | 6E8AE8 | 141622 | 1C1F2E |
| 2 | amber | E0A03C | 1A1612 | 25201A |
| 3 | rose | E06A8C | 1B1318 | 271C22 |
| 4 | slate | 8A9BB0 | 14171B | 1E2329 |
| 5 | lime | 9AD14A | 141A14 | 1D251D |

Ground constants (52-67): `MODERN_L_SURFACE F4F4F7`, `MODERN_L_TEXT 17181C`, `MODERN_L_DIM 5D606A`, `MODERN_D_SURFACE 22242B`, `MODERN_DESKTOP 10254A`, `LIGHT_DESKTOP 334452`, `LIGHT_SURFACE D6D3CD`, `LIGHT_TEXT 121214`, `LIGHT_DIM 5C5A57`, `DARK_TEXT E2E9EE`, `DARK_DIM 778693`.

`theme_apply_preset(i)` (188-206): sets accent from the preset, then desktop/surface/text/text_dim by (light, look): light+modern → MODERN_DESKTOP/MODERN_L_*; light+built → LIGHT_*; dark+modern → MODERN_DESKTOP/MODERN_D_SURFACE/DARK_*; dark+built → the preset's own desktop/surface + DARK_*. So in the modern look all presets share grounds; only the accent differs. Calls `derive()`.

`theme_current_preset()` (208-212): index of the preset whose **accent** equals `current.accent`, else -1 ("a colour someone set by hand").

**Derivation** `derive()` (237-320); `mix(a,b,t)` = per-channel `(a*(255-t)+b*t)/255` (220-228); `luma` = `(77R+151G+28B)>>8` (232-235); `dark = luma(surface) < 128`; W white, K black:

| field | dark | light |
|---|---|---|
| raised | mix(surface,W,26) | mix(surface,W,12) |
| overlay | mix(surface,W,42) | mix(surface,W,18) |
| sheen | mix(surface,W,92) | mix(surface,W,48) |
| hairline | mix(surface,W,44) | mix(surface,K,28) |
| accent_soft | mix(surface,accent,48) | mix(surface,accent,40) |
| accent_text | luma(accent)>140 ? mix(accent,K,205) : W | same |
| text_mute | mix(text_dim,surface,95) | same |
| edge_hi | mix(surface,W,120) | mix(surface,W,255) |
| edge_light | mix(surface,W,56) | mix(surface,W,96) |
| edge_shadow | mix(surface,K,78) | mix(surface,K,96) |
| edge_dark | mix(surface,K,150) | mix(surface,K,190) |
| well | mix(surface,K,66) | mix(surface,W,210) |
| glass | mix(surface,W,10) | mix(surface,W,80) |
| stroke | mix(surface,W,60) | mix(surface,K,26) |

Title bars: MODERN `title_a = title_b = raised; title_fg = text; title_off_a = title_off_b = surface; title_off_fg = text_dim` (295-298, 311-314). BUILT `title_a = accent; title_b = mix(accent,W,58); title_fg = accent_text; title_off_a = mix(surface, dark?W:K, dark?10:34); title_off_b = mix(surface, W, dark?30:70); title_off_fg = text_dim` (299-303, 315-319). The modern values of `title_*`, and `raised`/`hairline` entirely, are never drawn by wm.c (field-use count: `t->raised` 0, `t->hairline` 0).

**The knob table** (86-131; `K(field)` = `offsetof`, u16). Parser, writer, defaults and `/sys/settings` all walk it. "Adding a setting is adding a line here and a field in theme.h" (78-80). Values out of range are clamped by `knob_set` (159-165).

| # | key | label | type | lo..hi | default | what it controls (wm.c) |
|---|---|---|---|---|---|---|
| 1 | dock_h | Height | int | 20..120 | 44 | `TASKBAR_H` = `dock_h_now()` (68-71, fallback 44 if <20) |
| 2 | dock_gap | Gap below it | int | 0..80 | 14 | `TASKBAR_GAP`: dock bottom to screen bottom; also the side clamp margin of the find bar and popups (117-123, 2298-2300, 2489-2491) |
| 3 | dock_side | Gap at the sides | int | 0..400 | 16 | `DOCK_SIDE`: `dock_x()`, `dock_w()` (125-126); modern dock inset; item anchor in both looks |
| 4 | dock_radius | Corner | int | 0..40 | 14 | `DOCK_R`: dock corner (modern) |
| 5 | dock_brand | Show the name | bool | 0..1 | 1 | "zelr" badge (= launcher button) and chip start (130-133, 2698, 3240) |
| 6 | dock_search | Show find | bool | 0..1 | 1 | find button in the tray (2766, 3427); ctrl+f works regardless |
| 7 | dock_search_w | Find bar width | int | 120..700 | 260 | width of the find bar that opens (117-123) |
| 8 | dock_clock | Show the clock | bool | 0..1 | 1 | clock; `clock_slot_w()` = 0 when off so the tray shifts right (2191-2197) |
| 9 | clock_24 | Twenty four hour | bool | 0..1 | 1 | 24 h vs "h:mm am/pm" (2158-2175) |
| 10 | dock_hide | Tuck away | int | 0..2 | 1 | 0 never, 1 when a maximised window exists, 2 always (only out when reached for) (713-718, 789-794) |
| 11 | title_h | Title bar | int | 16..64 | 32 | `WM_TITLE_H` (523-526, fallback 32 if <16) |
| 12 | border | Frame | int | 0..8 | 1 | `WM_BORDER` (528-531, fallback 1 only if outside 0..8) |
| 13 | button_w | Button width | int | 12..60 | 30 | title-bar button width (1490, fallback 30) |
| 14 | button_h | Button height | int | 10..48 | 24 | title-bar button height (1491, fallback 24) |
| 15 | corner | Window corner | int | 0..24 | 12 | modern window radius; shadow drawn only if corner>0 (1617-1619); snap/resize preview radius |
| 16 | shadows | Shadows | bool | 0..1 | 1 | modern shadows (windows, menu, ctx, popups, find bar, dock) |
| 17 | snap | Snap to the edges | bool | 0..1 | 1 | edge-drag snapping only (`snap_zone_at`, 1830); alt+arrow snapping ignores it |
| 18 | desk_icons | Icons on it | bool | 0..1 | 1 | icons drawn/hit (1132, 1204); `wm_icons_right` (864-866) |
| 19 | icon_size | Icon size | int | 16..64 | 32 | `ICON_TILE` (844-847), `IS()` scale |
| 20 | icon_gap | Room around one | int | 8..60 | 30 | icon cell width/height (848-856) |
| 21 | vignette | Darken at the edges | int | 0..255 | 150 | `fb_vignette` strength -- BLOOM wallpaper only (1266) |
| 22 | glows | Lights in it | bool | 0..1 | 1 | `fb_glow` lights -- BLOOM only (1252-1262) |
| 23 | wallpaper | Wallpaper | int(enum) | 0..11 | 11 (BLOOM) | `draw_wallpaper` (1213-1479) |
| 24 | animate | Fade rather than snap | bool | 0..1 | 1 | `phase_of` returns ANIM_FULL when off (317-323) |
| 25 | anim_ms | How long a fade takes | int | 20..600 | 120 | `anim_scale` = ms*anim_ms/120 for MENU_MS 200, HOVER_MS 110, PANEL_MS 170 (287-294, 685) |
| 26 | dblclick_ms | Double click within | int | 150..1200 | 500 | icon double-click window (3723-3725) |
| 27 | quirks | Shake to clear | bool | 0..1 | 1 | shake gesture (3625) |
| 28 | autodesktop | Desktop at boot | bool | 0..1 | 1 | shell.c:608 |
| 29 | volume | Volume | int | 0..100 | 70 | dock slider, `sound_set_volume` |
| 30 | width | Screen width | int | 0..4096 | 0 | `want_w`, 0 = boot mode (3179-3191) |
| 31 | height | Screen height | int | 0..4096 | 0 | `want_h` |

`_Static_assert(sizeof(wallpaper_t) == sizeof(int))` (140-141) because the wallpaper enum is read as an int. `knob_get` reads bool as 1 byte, others as int (153-157).

**Non-knob keys** handled in `apply()` (378-407): `accent`, `desktop`, `surface`, `text`, `text_dim` (hex, with or without `0x`, `parse_hex` 355-370); `look` (nonzero → BUILT, then re-applies the current preset or preset **1** if custom); `preset` (0..5, `theme_apply_preset`; out-of-range ignored); `light` (nonzero → light, then re-applies current preset or preset **0** if custom). Total 39 recognised keys.

**File format and parser** `theme_reload()` (409-454): reads up to 2047 bytes of `/zelr.cfg` (`buf[2048]`); if `n <= 0` returns false (no file = no change). Per line: skip spaces/newlines/CRs; `#` at the start of a token begins a comment line; key = chars up to space/newline/CR (max 31 kept); skip spaces; value = rest of the line up to newline/CR (max 31 kept, trailing spaces kept but ignored by the digit parsers). Tabs are not separators. Unknown keys ignored. Values: `parse_dec` stops at the first non-digit (no sign → negative text becomes 0 and is clamped). After all lines `derive()` runs and the function returns `memcmp(&before, &current, sizeof(theme_t)) != 0`. **Reload is incremental**: lines absent from the file leave `current` as it was (the harnesses rely on this: `write /zelr.cfg wallpaper 4` replaces the file with one line and nothing else changes). Order matters: `look`/`light`/`preset` re-apply a preset and overwrite the five colours, so hand-set colours must come after them.

`theme_save()` (498-549): builds ≤1536 bytes (worst case ≈680): 4 header comment lines; `look N`; `light N`; then `preset N` if `theme_current_preset() >= 0`, else `accent/desktop/surface/text/text_dim 0xRRGGBB`; then all 31 knobs `key value`. `vfs_write` replaces the file.

`theme_init()` (324-342): `look = MODERN; light = true; theme_apply_preset(1); theme_defaults(); derive(); theme_reload();`. Called by `shell_task` (shell.c:607), at every `wm_run` start (wm.c:3881) and by the selftest. `theme_defaults()` (351-353) sets every knob to its `def`.

Volume (456-473): `theme_set_volume_live(p)` clamps 0..100, returns false if unchanged, else stores and calls `sound_set_volume`; `theme_set_volume(p)` = live + `theme_save()` if changed.

`wallpaper_moves(w)` (169-173): STARS, WAVES, AURORA, RAIN, ORBS, PULSE.

### 3.7 `kernel/winsrv.c`

**slot_t** (27-65): `bool used; u32 pid; window_t *win; u32 *raw` (kmalloc result for `pixels`), `u32 *pixels` (page-aligned inside `raw`, what is mapped), `u32 *shown_raw, *shown` (the buffer the desktop reads), `u64 bytes` (page-rounded size of each buffer), `u64 user_addr` (0 until mapped), `u64 dir` (PML4 it was mapped into). `static slot_t slots[8]` (90).

**Counters** `published_frames/bytes` + `winsrv_frames()`/`winsrv_bytes()` (67-71) -- "counted so /sys can be asked" but nothing declares or calls them.

**publish(s)** (80-88): copies `bytes/4` words `pixels`→`shown` through volatile pointers (so clang's loop-idiom pass does not turn it into a `memcpy` call).

**Retire list** (92-115): `RETIRED_MAX 8`, `retire(raw)` parks a replaced `shown_raw`; if full it `kfree`s immediately. `winsrv_reap_retired()` frees all parked buffers; called at the top of every `composite()` (wm.c:2897) "where it is holding no pointer into one".

**lookup(pid, h)** (125-130): `0 <= h < 8`, `used`, `pid` matches -- the only handle validation, applied on every entry point.

**on_wm_close(w)** (134-137): installed as `w->on_close`; when the WM closes a program's window it nulls `slots[i].win`; the slot (and its buffers and mapping) survive until the program calls close or exits.

**free_slot(s)** (139-154): unmaps every page from `s->dir` (named, not assumed live -- release may run in another task's context), then `win->on_close = 0; wm_close(win)`, `kfree(raw)`, `kfree(shown_raw)`, zero the slot. `wm_close` does not free a user window's canvas (wm.c:575-579) -- that guard is the "double free fixed in wm_close"; selftest checks the heap shrinks after close (selftest.c:864-868).

**winsrv_create(pid, title, cw, ch)** (156-233): bounds 32 ≤ cw ≤ 1600, 32 ≤ ch ≤ 1200; first free slot; `bytes = round_up(cw*ch*4, 4096)`; `raw = kmalloc(bytes + 4096)`, `pixels = align_up(raw)`, zeroed; same for `shown_raw/shown`. Placement: `step = handle % 5`, `x = wm_icons_right() + 14 + 48*step`, `y = 36 + 38*step`, clamped so the outer window fits `fb_width()` × `wm_work_height()` and not negative (183-203). `wm_create(title, x, y, cw, ch)`; copies the task name into `w->app`; frees the canvas `wm_create` allocated and points `canvas` at `shown`; `owned_by_user = true; on_close = on_wm_close`.

**winsrv_surface(pid, h, dir)** (235-253): `apply_pending`; returns existing `user_addr`; else maps `bytes` of `pixels` (identity-mapped heap, so phys = virt) at `WINSRV_SURFACE_BASE + h*STEP` with `PTE_PRESENT|PTE_RW|PTE_USER` via `map_page_in(dir, …)`; on failure unmaps what it mapped (with `unmap_page`, i.e. the *current* directory) and returns 0. Records `user_addr`, `dir`.

**winsrv_size** (255-260): `apply_pending`; `(cw<<16)|(ch&0xFFFF)` or -1 if no window.

**winsrv_poll** (262-274): `apply_pending`; if the WM already closed the window, synthesises `WM_EV_CLOSE` on every call; else `wm_pop_event`.

**resize_slot(s, cw, ch)** (283-360): bounds as create; no-op success if same size; refuses unless `paging_current_directory() == s->dir` (when mapped); `bytes <= WINSRV_SURFACE_STEP`; allocates new `pixels` and `shown`; if mapped, unmaps old pages and maps new ones at the same `user_addr` in `s->dir`, rolling back on failure; frees the old `raw` immediately and **retires** the old `shown_raw`; updates `canvas/cw/ch/want_*` with interrupts off ("the three fields the compositor reads together"); marks dirty + `wm_invalidate`.

**apply_pending(s)** (365-370): if `win->want_cw && want_ch`, clears them and calls `resize_slot`. Called first by `surface`, `size`, `poll`, `commit`.

**winsrv_resize(pid, h, cw, ch)** (372-375): program-initiated, direct `resize_slot` (does not require `resizable`, pushes no event, does not move the window or clear `maximized`).

**winsrv_allow_resize** (377-382): `win->resizable = true` (one-way).

**winsrv_resize_window(w, cw, ch)** (387-398): WM-initiated; bounds; records `want_cw/ch` and pushes `WM_EV_RESIZE{x=cw, y=ch}`; returns false for out-of-bounds sizes (e.g. widths > 1600).

**winsrv_commit** (406-413): `apply_pending`; `publish`; `wm_invalidate` (which also requests a whole frame).

**winsrv_close** (415-420) → `free_slot`. **winsrv_window** (422-425) → `s->win` (may be NULL). **winsrv_release(pid)** (427-430): frees every slot of `pid`; called from `task_exit_with` (sched.c:672) and `signal_end_task` (signal.c:33), before the address space is reaped.

**Syscall glue** (syscall.c): `SYS_WIN_CREATE 7` (922-937; rbx=title copied ≤31 bytes with per-byte `user_range_ok`, rcx=cw, rdx=ch → handle/-1), `SYS_WIN_SURFACE 8` (939-945; rbx=handle; passes `paging_current_directory()` → address/0), `SYS_WIN_SIZE 9` (947-949), `SYS_WIN_POLL 10` (951-959; rbx=handle, rcx=out, 1/0/-1), `SYS_WIN_COMMIT 11` (986-988), `SYS_WIN_CLOSE 12` (990-992), `SYS_WIN_RESIZABLE 36` (412-415), `SYS_WIN_RESIZE 37` (417-421; rbx,rcx=w,rdx=h), `SYS_WIN_TEXT 56` (964-975; rbx=handle, rcx=ptr, rdx=len clamped 0..4096, range-checked, → `wm_set_text`), `SYS_WIN_FIND 57` (979-984; rbx=out, rcx=cap>0 → `wm_find_query`; no handle). Numbers in include/syscall.h:14-19, 54-55, 180-181 and sdk/zelr.h:66-71, 97-98, 180-181. User wrappers: `win_create/win_surface/win_width/win_height/win_set_text/win_find_query/win_poll/win_commit/win_close/win_allow_resize/win_resize` (sdk/zelr.h:886-945).

### 3.8 `kernel/gfx.c`

- **8x16 text** (8-28): `gfx_char` plots `font8x16` bits with `fb_put` (chars outside 32..126 skipped); `gfx_text`, `gfx_text_bg` (fills a `strlen*8 x 16` box first), `gfx_text_width = strlen*8`.
- **Surfaces** (32-108): `surf_clear` fills `w*h`; `surf_rect` clips then fills; `surf_frame` = 4 rects; `surf_disc` filled circle (r≥1, `dx²+dy² ≤ r²`); `surf_line` Bresenham stamping a disc at every step ("so a fast drag does not leave gaps"); `surf_char/surf_text` clipped bitmap text; `surf_blit` copies a surface to the fb pixel by pixel via `fb_put`.
- **gfx_mix(under, over, alpha)** (116-123): per channel `u + (o-u)*alpha/255` (alpha 0..255; no clamping).
- **corner_inset(r, i)** (126-131): largest `dx ≤ r` with `dx²+(r-i)² ≤ r²`, returns `r-dx` -- the stepped (non-AA) corner.
- **fb_round_rect / fb_round_frame** (133-164): stepped rounded fill/outline; r clamped to w/2, h/2; r≤0 → plain rect/frame.
- **fb_shadow(x,y,w,h,r,spread)** (188-221): for `s = spread..1`: `alpha = 70/(s+1)`, ring rect `(x-s, y-s+2, w+2s, h+2s)` (shifted 2 px down), radius `r+s`; each ring darkens (read-modify-write via `fb_get`/`gfx_mix(…,0,alpha)`) only its top 2 and bottom 2 rows fully and 2-px-wide left/right edges in between (spans computed with `corner_inset`). `shadow_span` clips x to the screen and skips rows off-screen.
- **fb_vgradient / fb_hgradient** (223-237): one `fb_rect` per row/column with `gfx_mix(top, bottom, j*255/(h-1))`.
- **edge / fb_bevel / fb_bevel_thin** (243-259): top row whole in `tl`, left column from y+1, bottom row whole in `br`, right column from y -- corners belong to the run that starts there (no notch). `fb_bevel` = outer edge + inner edge (inset 1).
- **Faces** (263-337): `face_for` clamps invalid index to 0; `face_height` = size; `face_width` sums advances (non-ASCII → space); `face_text(x, y, s, fg, which)` treats `y` as the top of the line, baseline = `y + size*4/5`; per glyph pixel coverage `a`: 255 → `fg`, else `gfx_mix(fb_get, fg, a)`; clips to the fb. `face_surf_text` is the same into a surface.
- **16-sample AA corners**: `corner_cover(px, py, cx8, cy8, r8)` (360-370) counts which of a 4x4 grid of sample points (at `px*8 + {1,3,5,7}` eighths) lie inside the circle, returns `inside*255/16`. `round_row(x, y, w, r, row_from_edge, rgb, alpha)` (374-406): rows ≥ r are a full span (opaque `fb_put` or blend); corner rows compute coverage for columns `0..r-1` at both ends and fill the middle. `fb_round_rect_aa` (408-418) calls `round_row` for every row with `from_edge = min(j, h-1-j)`. Cost note: every pixel of the rectangle is touched with `fb_put` (and `fb_get` when alpha < 255), not just the edges.
- **fb_vignette(strength)** (431-454): normalised squared distance in thousandths against the half-width and half-height, halved so corners reach 1000; untouched while `d2 ≤ 120`; darkening `a = strength*(d2-120)/880` clamped to 255. Full-screen per-pixel read-modify-write.
- **fb_glow(cx, cy, rx, ry, rgb, strength)** (456-483): elliptical, `fall = 1000 - d2`, `a = strength*fall²/10⁶` -- (1-d²)² falloff, no rim.
- **fb_sheen(x,y,w,h,r,strength)** (492-503): top half only, `fall = (band-j)*255/band`, `a = strength*fall²/255²`, white, via `round_row` so the top corners stay rounded.

Callers outside gfx.c: only wm.c, apps.c (surf_*), blackbox.c (gfx_text) and selftest. Unused anywhere: `gfx_text_bg`, `gfx_text_width`, `surf_frame`, `surf_blit`, `fb_round_rect`, `face_surf_text` (and `gfx_char`/`surf_char` only internally).

### 3.9 `kernel/wm.c`

#### 3.9.1 Globals and geometry helpers
- Dock accessors (68-80): `dock_h_now` (fallback 44 if <20), `dock_gap_now`/`dock_side_now`/`dock_r_now` (fallbacks 14/16/14 only for negative values); macros `TASKBAR_H`, `TASKBAR_GAP`, `DOCK_SIDE`, `DOCK_R`; `TASKBAR_R 0` (unused).
- `panel_rest_y() = fb_height - TASKBAR_H - TASKBAR_GAP` (87-89) -- dock top when out.
- `TASKBAR_BADGE_W 76` (92); `FIND_W 26`, `FINDBAR_H 38` (114-115); `findbar_w()` = clamp(`dock_search_w`, 120, fb_width − 2·gap) (117-123).
- `dock_x() = DOCK_SIDE`, `dock_w() = fb_width − 2·DOCK_SIDE` (125-126).
- Tray, right to left, all derived from each other: clock at `dock_x+dock_w−20−clock_w` (modern); `taskbar_volume_x() = dock_x+dock_w−20−clock_slot_w()−16−VOL_W` (2199-2203); `taskbar_net_x() = volume_x − NET_W − 6` (2415-2417); `taskbar_find_x() = net_x − FIND_W − 6` (128). Chips from `taskbar_chips_x()` = `dock_x+16` (+76+18 with the badge) (130-133) to `taskbar_chips_end()` = min(net_x−8, find_x−6 if find shown) (144-151).
- `VOL_W 30, VOLPOP_W 208, VOLPOP_H 40` (153-155); `NET_W 26, NETPOP_W 268, NETPOP_H 150` (158-160).
- Launcher geometry (175-184): `MENU_RAIL 132, MENU_PANE 152, MENU_PAD 10, MENU_W 304, MENU_ITEM 32, MENU_RIGHT 100` (index offset for right-column/result items). `SHADOW 5` (185; margin used in damage rects and panel off-screen position).
- Window stack (187-189): `static window_t *stack[8]` (index 0 bottom), `nwin`, `running`.
- Pointer interaction state: `dragging, drag_off_x/y, mouse_capture` (191-193); `resizing, resize_off_x/y, resize_cw/ch` (198-200); `snap_t {SNAP_NONE, SNAP_LEFT, SNAP_RIGHT, SNAP_FULL}`, `snap_preview` (205-206); shake ring `shake_x[8], shake_t[8], shake_n` (211-214); `last_buttons, last_mx, last_my, needs_composite` (216-218).
- Frame requests (231-261): `frame_is_whole` (default true), damage union `dmg_x0..dmg_y1` (empty when `x1 <= x0`), `damage()`, `need_frame()` (whole), `need_frame_in(x,y,w,h)` (partial).

#### 3.9.2 Animation machinery (263-327)
`ANIM_FULL 256`; `anim_scale(ms) = ms*anim_ms/120` (fallback 120 if <20); `MENU_MS = anim_scale(200)`, `HOVER_MS = anim_scale(110)`, `PANEL_MS = anim_scale(170)` (685); `anim_len(ms)` = ticks (≥1); `ease_out(p) = 256 − (256−p)³/256²`; `phase_of(since, ms)` returns 256 if `since == 0` or `!animate`, else eased progress; `still_moving(since, ms)`. "Nothing owns a timer, nothing counts frames, and nothing has to be told to stop." What actually animates: the panel slide (position), the launcher rising 8 px and the context menu 6 px. Hover "fades" are drawn as a boolean at `phase > 128` (1953, 1980, 2031, 2090) -- a slightly delayed snap, not a fade.

#### 3.9.3 Dock/tray/popup state (329-385)
`volume_open, net_open, volume_drag, volume_before_mute = 70`; find state `find_open, find_q[64], find_qn, find_total, find_windows, find_at, find_win`; launcher `menu_open, menu_x, menu_y, menu_hover = -1, menu_left = -1, menu_since, menu_hover_since, menu_cat`; context menu `ctx_open, ctx_x, ctx_y, ctx_hover, ctx_left, ctx_since, ctx_hover_since`, `CTX_W 204, CTX_ITEM 30, CTX_PAD 8`, `CTX_H = 6*30+16 = 196`; `last_theme_check`.

Context menu items `CTX[]` (371-381): Open terminal `/bin/term`; Browse files `/bin/files`; Select all (kernel); Personalise `/bin/settings`; Close all windows (kernel); System info (kernel). A separator is drawn above item 4 (2105-2108).

#### 3.9.4 Launcher data and query (387-517)
`mitem_t {label, program}`; categories `MCAT[]` (426-438) in order: **Productivity** (Terminal `/bin/term`, Files `/bin/files`, Notes `/bin/notes`, Calculator `/bin/calc`), **Internet** (Browser `/bin/browser`), **Media** (Paint `/bin/paint`, Music `/bin/music`), **Games** (Blackjack `/bin/blackjack`, Poker `/bin/poker`), **System** (Settings `/bin/settings`, Monitor `/bin/monitor`, System info = kernel), **Session** (Close all, Leave desktop, Shut down = kernel). 6 categories, 15 items. `menu_rows()` = max(categories, largest category) = 6; `menu_full_h()` = 20 + 6·32 = 212 (445-455).
Type-to-filter: `MENU_QMAX 24`, `MENU_HITS 4` (469-470); `menu_matches(label)` = case-insensitive **substring** match over item labels only (481-490); `menu_hits(out, max)` walks all categories in order (492-501); `menu_h()` shrinks to `20 + (min(hits,4)+1)*32` (at least one row for "nothing by that name") (506-512); `menu_top()` keeps the panel hanging from the same bottom edge (517).

#### 3.9.5 Window lifecycle API (519-593)
- `wm_title_h()` (523-526), `wm_border()` (528-531), `wm_active()` (533, unused), `wm_outer_w = cw + 2·border`, `wm_outer_h = ch + WM_TOP + border` (535-536).
- `wm_invalidate(w)` (538): `dirty = true; need_frame()` (whole).
- `wm_create(title, x, y, cw, ch)` (540-557): fails at 8 windows; `kcalloc(window_t)`, `kmalloc(cw*ch*4)` canvas cleared to `surface`; pushes on top of the stack; no position clamping.
- `wm_close(w)` (559-582): user window → push `WM_EV_CLOSE` (into a queue about to be freed; the program learns via `winsrv_poll`'s synthesised CLOSE); `on_close(w)`; remove from stack; clears `dragging` and `mouse_capture` if equal (**not `resizing`**); frees canvas only if `!owned_by_user`; `kfree(w)`; `need_frame()`.
- `wm_raise(w)` (584-593): moves to the top.

#### 3.9.6 Event ring and motion folding (597-649)
`wm_push_event(w, ev)`: if `ev` is a mouse event without the 0x80 press bit and the ring is non-empty, and the newest queued event is a mouse event with the **same buttons as `ev`**, and `q_prev_buttons` (buttons of the mouse event queued before that one) also equals `ev->buttons`, then the newest event is overwritten in place ("only overwritten when it carries the same buttons as this one and as the one before it"). Otherwise enqueue; a full ring drops the *new* event. `q_prev/last_buttons` update only on real enqueues of mouse events. Rationale (600-621): a pointer crossing the screen filled the ring and pushed out the press; folding a release into the following move moved the release elsewhere. `wm_pop_event` (644-649) FIFO.

#### 3.9.7 Blit, panel tuck-away, work area, covering (653-739)
- `blit_surface` (653-669): clips to the screen, `memcpy` per row into `fb_pixels()` using `fb_pitch()`.
- `panel_shown = true`, `panel_since` (688-689); `taskbar_y()` interpolates between `panel_rest_y()` and `fb_height + SHADOW` by `phase_of(panel_since, PANEL_MS)` (694-701); `PANEL_EDGE 3` (686).
- `panel_in_the_way()` (705-709): any non-minimised window with `maximized` set (flag, not geometry).
- `work_h()` (713-718): mode 0 → `panel_rest_y()`; mode ≥2 → `fb_height`; mode 1 → `fb_height` if in the way else `panel_rest_y()`. Exported as `wm_work_height()`.
- `covering_index()` (728-739): topmost non-minimised *maximised* window whose outer rect actually covers the screen (`x≤0, y≤0, x+ow≥W, y+oh≥H`), else -1.

#### 3.9.8 Curves (741-775)
`SINE_Q[17] = {0,24,49,73,97,120,142,163,181,198,212,224,233,240,245,247,248}` -- quarter wave; `sine64(phase)` gives a 64-step period in −248..248 (749-755). `ring(cx,cy,r,c)` midpoint circle, 2 rows thick (759-775), used by the PULSE wallpaper.

#### 3.9.9 Panel visibility (777-812)
`panel_should_show(my)` (783-795): always out while the launcher, context menu or a window drag is active; mode 0 → out; mode 1 with nothing maximised → out; otherwise if shown stay while `my ≥ panel_rest_y()−6`, if hidden come out when `my ≥ fb_height − 3`. `panel_frame()` requests a partial frame covering the band the panel moves through (801-804). `panel_update(my)` flips `panel_shown`, stamps `panel_since` (806-812); called every pass with `last_my` (3929).

#### 3.9.10 Colour helpers (814-826)
`lighten`, `darken` (gfx_mix toward white/black), `paper(amount)` = a mark *away from* the desktop colour (darker on a light theme, lighter on a dark one).

#### 3.9.11 Desktop icons (828-1211)
- `icon_tile()` (fallback 32 if <16), `icon_gap()` (fallback 30 if <8); `ICON_TILE`, `IS(v) = v*tile/32`, `IS1(v)` (≥1, for thicknesses); `ICON_CELL_W = tile + 2·gap` (92), `ICON_CELL_H = tile + gap + 26` (88); `ICON_LEFT 18`, `ICON_TOP 20` (853-858). `wm_icons_right()` = 18 + 92 = 110 (or 18 without icons) (864-866).
- `DESK[]` (868-879): Terminal `/bin/term` (kind 0), Files `/bin/files` (1), Notes `/bin/notes` (2), Paint `/bin/paint` (3), Settings `/bin/settings` (4). `DESK_N 5`.
- Selection: `desk_sel` bitmask, `desk_last_click`, `desk_last_tick` (884-888).
- Band: `band_on, band_ax, band_ay, band_bx, band_by, band_button` (892-894).
- Pictograms, each with a modern (flat, AA rounded shapes) and a built (bevelled) variant, every coordinate in a 32-unit square through `IS()`: `icon_terminal` (dark tile, green chevron stepped a pixel at a time + underscore) (913-941), `icon_folder` (two-tone) (943-965), `icon_page` (page with 5 lines) (967-987), `icon_paint` (palette with 5 wells) (989-1021), `icon_sliders` (3 rails + accent knobs) (1023-1055).
- `draw_desk_icon(i, x, y, selected)` (1057-1129): modern draws a rounded "tile" behind (pad `IS1(10)`, radius `IS1(14)`) -- accent wash when selected, brighter when the pointer is over it (`hot`, suppressed while banding); label in `FACE_BODY` centred under the tile at `tile+16` (modern) / `+6` (built), with a 1-px dark drop copy; built selected labels get an accent box.
- `desk_icon_rect(i)` = (18, 20+i·88, 92, 82) -- shared by drawing, clicking and banding (1142-1147). `band_rect`, `band_select` (rect intersection → `desk_sel`), `draw_band` (modern: accent wash α46 + 1-px lines α190; built: dotted frame in `text`), `desk_icon_at` (1152-1211).

#### 3.9.12 Wallpapers -- `draw_wallpaper()` (1213-1479)
All draw the full screen height (the dock floats). `GRADIENT`: vgradient `paper(22)`→desktop. `BLOOM` (default): vgradient 0A0F2A→070A1B, then if `glows` two big glows (cool 3A7BFF at (30%W,25%H), radii 60%W×60%H, strength 96; warm 9B4BE0 at (80%W,70%H), 40%×40%, 64) and a bright core C8DEFF (W/9×H/9, 70); then `fb_vignette(vignette)` if >0. `GRID`: 32-px lines `paper(14)`. `DOTS`: 2x2 dots every 28 px from 16, `paper(26)`. `STARS` (moving): 240 stars, position = `i*2654435761` / `i*2246822519` hash `>>9`, x drift `(ticks/3)*speed` with speed 1..3, colour `paper(25+45*(i%3))`, every 9th star 2x2. `WAVES` (moving): 5 sine lines, `y = h*(band+1)/6 + sine64(x/3 + ticks/4 + 5*band)/8`, 2 px tall, `gfx_mix(desktop, accent, 22+12*band)`. `WEAVE`: diagonals both ways every 24 px, `paper(12)`. `AURORA` (moving): vgradient `paper(18)`→desktop, 3 accent bands (accent, lighten 70, darken 50) at `h/3 + band*h/8`, `REACH 17` rows blended per column, drift `ticks/6`. `RAIN` (moving): 170 streaks, speed `4+3*(i%5)`, length `5+4*(i%4)`, wrap `h+60`, fading accent. `ORBS` (moving): 5 discs, `r = 44+9i`, centres from two sines with coprime-ish rates, brightness `(r²−d²)*34/r²`. `PULSE` (moving, "rings"): 7 concentric triple rings from the centre, `r = (ticks*3 + k*reach/7) % reach`, `reach = (W+h)/2`, fade `90 − r*80/reach`. `PLAIN`/default: flat `desktop`.

#### 3.9.13 Title-bar buttons, grip, bevel pieces (1481-1602)
`btn_w()/btn_h()` (fallback 30/24), `BTN_GAP 2`; `button_t {BTN_NONE, BTN_CLOSE, BTN_MAX, BTN_MIN}`; `button_box` places close/max/min right-to-left at `x + ow − border − 2 − BTN_W − slot*(BTN_W+2)`, vertically centred in the title bar (1499-1504); `button_at` skips MAX on non-resizable windows (1506-1516). `GRIP 16` and `on_grip` (bottom-right 16x16 of the outer rect, resizable and not maximised) (1520-1527). `raised()`/`sunken()` = `fb_bevel` with the four edge colours in opposite orders (1535-1543). `button_face` (1548-1567): modern -- nothing at rest, `sheen` patch α110 when hot, `accent_soft` α235 when down; built -- surface face (lightened when hot) with raised/sunken bevel. Marks (1575-1602): `MARK 10`; minimise (bar), maximise (hollow box), restore (two boxes, fixed offsets), close (X).

#### 3.9.14 `draw_chrome(w, focused)` (1604-1724)
Modern: shadow `fb_shadow(x, y, ow, oh, corner, focused ? 7 : 3)` only if `corner > 0 && shadows`; `fb_round_rect_aa` surface α255; stroke (focused: `gfx_mix(stroke, accent, 190)` α235; else `stroke` α110) over the whole rect; surface α255 again inset 1 px (this leaves a 1-px AA hairline). Title text at `x+border+10`, `FACE_BODY_BOLD`/`text` if focused, `FACE_BODY`/`text_dim` otherwise; no title-bar fill. Buttons invisible unless hot (close → red D93A3A α240 with white mark; others `sheen` α130). Client area outlined by a 1-px `stroke` frame. Built: square surface + `raised` bevel; title bar `fb_hgradient(title_a→title_b)` or the `title_off_*` pair, title in `FACE_BODY_BOLD`; bevelled buttons; client area `sunken`. Both: 3 bevelled grip strokes when resizable and not maximised.

#### 3.9.15 Resize, snap, maximise, minimise, shake (1726-1871)
- `wm_resize(w, cw, ch)` (1732-1748): refuses below 120x60; user windows → `winsrv_resize_window` (deferred); kernel windows → new canvas cleared to surface.
- `remember_place` (1753-1759): saves x/y/cw/ch unless already maximised. `place(w, x, y, cw, ch)` (1761-1766): `wm_resize` then move (returns without moving if the resize is refused).
- `snap_rect(zone)` (1769-1782): LEFT (0,0, W/2−2b), RIGHT (W/2, 0, W/2−2b), FULL (0,0, W−2b); height = (FULL ? fb_height : work_h()) − WM_TOP − border.
- `apply_snap(w, zone)` (1784-1791): resizable only; `remember_place`; `place`; `maximized = (zone == SNAP_FULL)` (set regardless of whether `place` succeeded).
- `toggle_maximize` (1793-1801): restore to `restore_*` or `apply_snap(SNAP_FULL)`.
- `set_minimized(w, yes)` (1803-1808): un-minimising raises; minimising does **not** lower.
- `resize_from_pointer` (1812-1826): corner offset kept; min 160x80; max so the outer rect stays within the screen width and `work_h()`.
- `snap_zone_at(mx, my)` (1829-1836): if `snap`: `my ≤ 12` → FULL, `mx ≤ 12` → LEFT, `mx ≥ W−12` → RIGHT.
- Shake (1838-1871): `shake_note(mx)` keeps the last 8 x samples with ticks; `shake_detected()` needs 8 samples within `timer_hz()` ticks and ≥3 direction reversals counting only steps with |dx| ≥ 24.

#### 3.9.16 Launcher, context menu and previews drawing (1873-2138)
`item_light(i)` (1875-1880) and `ctx_light(i)` (2059-2064): rising for the hovered item, falling for the one just left. `draw_menu` (1882-2053): rise = `(256−p)*8/256`; modern panel: shadow r16 spread 12, overlay α250, stroke α85, overlay α252 inset; built: surface + raised. With a query: query text + steady caret, underline, up to 4 results or "nothing by that name". Without: left rail with category name, count in `FACE_SMALL` right-aligned, accent fill α58 + a 3-px accent bar on the open category, hover wash α20; a separator; right pane items (accent wash α42 on hover). `draw_ctx` (2066-2110): same style, rise 6, r14. `draw_snap_preview` (2114-2126): 3 nested `fb_round_frame`s in accent + a title-bar-shaped fill. `draw_resize_preview` (2131-2138): 2 nested frames at `resize_cw/ch`.

#### 3.9.17 Clock and tray glyphs (2150-2485)
`clock_text` (2150-2183): `rtc_format_short` ("HH:MM"); 12-hour conversion in place to "h:mm am/pm" (leading zero dropped); without a CMOS clock "up M:SS". `clock_slot_w` measures in `FACE_HEAD` (modern) or `FACE_BODY` (built). Glyphs are 16x16 digit maps (`GLYPH 16`, `glyph()` draws selected parts, 2232-2244): `G_SPEAKER` (cone 1, near wave 2, far wave 3), `G_MUTE` (cone + cross 4); `draw_speaker` shows parts 1+4 at 0, 1+2 up to 45, 1+2+3 above (2288-2292). `G_SIGNAL` (4 arcs over a dot) and `G_PLUG` (2435-2471), `_Static_assert`ed to 16x16. `draw_signal(on, off, bars)` colours parts ≤ bars in `on`, others `off`; `draw_wired`.

#### 3.9.18 Volume popup, find bar, network popup (2294-2650)
- `volume_track` (2296-2305): popup 208x40 centred over the speaker, clamped to `[gap, W−gap]`, 8 px above the dock; track at `px+16`, width 136. `draw_volume_panel` (2307-2347): track `well`, fill `accent`, knob (modern disc with surface ring; built bevelled button), number at right.
- `volume_blip()` (2618-2629): `sound_tone(880, 70)` rate-limited to one per `timer_hz()/8`. `volume_from_pointer(mx)` (2634-2650): maps x to 0..100, `theme_set_volume_live`, marks `volume_unsaved`, blips (no disk write during a drag).
- `draw_find_bar` (2363-2405): `findbar_w()` wide, 38 tall, right-aligned at `W − gap − w`, 8 px above the dock; right side shows "%d to look in" (no query; count of non-minimised windows with published text), "not on screen" (no hits) or "%d of %d"; left side query + accent caret, or caret + "find on screen".
- Network (2407-2604): `net_panel_rect` 268x150 centred over the icon; `dhcp_button_rect` = full width minus 28, 26 tall, at the bottom. `draw_net_panel` (2506-2594): line 1 card name (`netdev_name()` if `netdev_up()`), else "VVVV:DDDD, no driver" (`netdev_undriven`) or "no wired card"; line 2-3 "address a.b.c.d" / "router …" or "asking for an address"/"no address" + "nothing can be reached without one"; `wifi_describe()`; wifi maker/vendor/device if `wifi_state() != WIFI_NONE`; the button "ask for an address" / "asking..." (`net_dhcp_busy()`). `start_dhcp()` → `net_dhcp_start()` (runs in a net.c task, so the compositor does not stall) (2601-2604).

#### 3.9.19 `draw_taskbar()` (2652-2869)
Modern: shadow (spread 10, radius `DOCK_R`), glass α208, stroke α70, glass α232 inset, `fb_sheen` α44 -- at `dock_x..dock_x+dock_w`, `taskbar_y()`. Built: full-width surface bar with a thin highlight bevel and top line (at `taskbar_y()`, so it floats by `dock_gap` too). Badge "zelr" in `FACE_HEAD_BOLD` accent inside a `button_face` (pressed while the launcher is open) at `dock_x+16`, 76 wide, `dock_h−14` tall, `y+7`. Chips (2714-2753): one per window in stack order, width `face_width(title, FACE_BODY)+22` capped at 160, 6 px apart, stop at `taskbar_chips_end()`; focused = top of stack and not minimised → accent pill α46 + 14x3 accent underline (modern) or pressed button (built); minimised titles in `text_dim`. Find button: 26 wide, highlighted when hot/open, magnifier plotted pixel by pixel (2766-2794). Built look draws a sunken tray well (2805-2808). Clock: modern `FACE_HEAD` at the dock's right edge −20; built `FACE_BODY` at `W−8` (screen edge). Speaker with hover/open wash; network icon in three states: address (`net_ip() != 0`) → `text`, link only (`netdev_up()`) → `text_dim`, neither → `text_mute`; plug glyph when link is up, faint arcs (0 bars) otherwise (2846-2867).

#### 3.9.20 Pointer and `composite()` (2871-2952)
`CURSOR[19][12]` (1 outline 080C10, 2 fill F4F7F9) and `draw_cursor`. `composite()`: `winsrv_reap_retired()`; covering index → flat fill, else wallpaper + icons + band; windows from `max(cover,0)` up: clear `dirty`, skip minimised, `draw_chrome(w, i == nwin−1)`, read `canvas/cw/ch` with interrupts off, `blit_surface` at `(x+border, y+WM_TOP)`; clear `dirty` of covered windows; overlays (snap preview, resize preview, taskbar, volume panel, find bar, net panel, launcher, ctx, cursor); flush whole if `frame_is_whole` or the damage is empty, else `fb_flush_rect(dmg…)`; reset `frame_is_whole = false` and damage.

#### 3.9.21 Launching and menus (2954-3127)
`launch(path)` = `vfs_slurp` + `user_spawn_elf` (2956-2962). `menu_run(it)` (2964-2985): closes the launcher and clears the query; program → launch; "System info" → `app_about()`; "Close all" → `wm_close` every window; "Leave desktop" → `running = false`; "Shut down" → `diskfs_flush(); power_off();` then `running = false` if firmware refused. `menu_choose(i)` (2991-3008): results when a query exists; left column index → switch category (panel stays up); right index → run. `menu_key(key)` (3012-3041): Return runs the hovered right item or the first right item/result; Backspace/DEL edit; printable 32..126 appended (≤24). `menu_item_at` (3047-3069), `ctx_item_at` (3071-3077). `open_ctx_at` (3079-3092) and `open_menu_at` (3110-3127) clamp to x≥4, y≥4, right edge and `panel_rest_y()`, reset hover/animation stamps, close the other menu. `ctx_choose` (3094-3108): program → launch; "Select all" → `desk_sel = 0x1F`; "Close all windows"; "System info".

#### 3.9.22 Resolution changes (3129-3191)
`screen_changed()` (3137-3170): maximised windows are refitted with `place(SNAP_FULL)` (keeping `restore_*`); others clamped to `cw ≤ W−8`, `ch ≤ work_h − title_h − 8`, min 160x80 (`wm_resize`), then moved back on screen; `restore_x/y` zeroed if the restore rect would be off screen. `boot_w/boot_h` = size at the first `wm_run` (3177, 3885). `apply_screen_size()` (3179-3191): target = `want_w/h` or boot size; nothing if equal, if `!fb_mode_settable()` (firmware-owned framebuffer, e.g. UEFI GOP) or if `fb_set_mode` refuses; on success `fbcon_init()` + `screen_changed()`. Called at `wm_run` start and after every theme change.

#### 3.9.23 Hit testing (3193-3245)
`window_at(mx, my, &on_title, &button)` topmost non-minimised window containing the point; `on_title` = `y+border ≤ my < y+WM_TOP` (3195-3210). `taskbar_chip_at` (3214-3233) walks the chips (measuring the focused chip in `FACE_BODY_BOLD`, see §10). `on_taskbar_badge` (3239-3245).

#### 3.9.24 Find (3247-3432)
- `find_fold` ASCII lower-case (3262-3264); `find_count_in(w, q, qn)` non-overlapping case-insensitive count over `w->text` (3266-3275).
- `find_recount()` (3280-3309): front-to-back over non-minimised windows; `find_windows` = those with `textlen > 0`; `find_total` = sum; `find_win` = the window containing global match `find_at` (the first window with a match if `find_at` is out of range, in which case `find_at` resets to 0).
- `find_go()` (3313-3327): local index = `find_at` minus matches in windows in front; `wm_raise(find_win)`; push `WM_EV_FIND{y = local}`.
- `find_clear_marks()` (3334-3341): `WM_EV_FIND{y = -1}` to every user window.
- `wm_set_text(w, s, len)` (3343-3351): copies ≤4096 bytes into `w->text`; recounts if the find bar is open with a query. `wm_find_query(out, cap)` (3353-3359) copies the current query (global, not per window).
- `find_close` (3361-3370) clears marks and state; `find_start` (3372-3381) opens, closes the launcher, recounts.
- `find_key(key)` (3384-3424): Esc closes; Return advances `find_at` modulo total, recounts, `find_go`; Backspace edits and re-finds (or clears marks); printable appends (≤63), resets to match 0 and goes there immediately; every other key is swallowed while the bar is open.
- `on_find_button` (3426-3432).
- **Protocol (program side)**: the program calls `win_set_text(handle, words, len)` whenever what it shows changes (term.c:306-316 publishes its last 60 lines; browser.c:243-255 the laid-out text runs; blackjack.c:410, poker.c:918). On `WIN_EV_FIND` it calls `win_find_query()` and locates match number `ev.y` in its own content, scrolling and highlighting it; `ev.y == -1` clears (browser.c:257-284, 1374-1378 -- the only program that handles `WIN_EV_FIND`; the terminal is counted and raised but paints nothing). Both sides must count the same way (case-insensitive, non-overlapping) for the index to line up.

#### 3.9.25 `handle_mouse(mx, my, buttons)` (3434-3792) -- priority order
`pressed_now` / `right_now` = new left/right press; `released` = left release only.
1. Launcher open: update hover; resting on a category opens it; right press closes; left press chooses the item under the pointer or dismisses (consumed).
2. Context menu open: hover; left or right press chooses or dismisses.
3. `volume_drag` with left held → `volume_from_pointer`.
4. Network popup open and a press: inside → only the DHCP button reacts; outside and not on the net icon → close (consumed).
5. Volume popup open and a press: inside → start `volume_drag`; outside and not on the speaker → close (consumed).
6. Band in progress: while its button (left or right) is held, update the far corner and `band_select`; on release, if it moved < 4 px in both axes it was a click → clear selection and open the context menu (right) or the launcher (left) at the press point.
7. Left release: finish `volume_drag` (save once, final tone); send a release event (buttons 0) to a captured user window; commit a snap preview if dragging; finish a corner resize with one `wm_resize`; reset drag/resize/capture/shake.
8. `resizing` → update `resize_cw/ch` (outline only).
9. `dragging` → un-maximise if maximised (placing at the restore rect, grab point = centre of the title bar); move with clamps (y ≥ 0, x ≥ −(cw−60), x ≤ W−60, y ≤ work_h − title_h); snap preview (resizable only); shake detection → minimise all others if `quirks`.
10. `mouse_capture` → forward the position (and current buttons) to the captured window.
11. No new press → return.
12. Press with no window under the pointer: badge toggles the launcher (opened at `dock_x`, `panel_rest_y − 212 − 8`); find button toggles find; net icon toggles its popup (closes volume); speaker: right = mute/unmute (remembering `volume_before_mute`), left toggles the popup; a chip: minimised → restore, top → minimise, else raise; the dock band (full screen width) otherwise swallows the click; an icon: select, double click (same icon within `dblclick_ms`) launches, a right press also opens the context menu; otherwise on the wallpaper (`my < work_h()`) start a band.
13. Press on a window: `wm_raise`; close/max/min buttons; grip → start resize; title: right press toggles maximise, left starts a drag; content: `mouse_capture = w` and push `WM_EV_MOUSE{buttons | 0x80}` (user) or `on_mouse(…, true)` (kernel).

#### 3.9.26 Shortcuts `handle_shortcut(key)` (3794-3874)
- `KEY_CODE(key) == 6` (ctrl+f with or without the ctrl bit, so it works over serial) toggles find.
- With `KEY_MOD_ALT`: Tab → `cycle_windows()` (raise the first non-minimised window below the top; if all minimised, un-minimise `stack[nwin−2]`) (3802-3815); d → minimise all; m → minimise top; f → toggle maximise top; q → close top; ←/→ snap top left/right; ↑ snap full; ↓ restore if maximised else minimise. "Top" is always `stack[nwin−1]`.

#### 3.9.27 `wm_run()` (3878-4078) -- see §4.2.

### 3.10 `kernel/apps.c` (System info hook)
`app_about()` (69-78): if already open raise it; else `wm_create("about", 682, 120, 322, 300)`, render, `on_mouse = about_on_mouse` (re-render on every press), `on_close = app_about_forget`. `about_render` (23-60) paints with the 8x16 bitmap font and fixed colours (not the theme): version, memory, heap, display, uptime, tasks, files. Reached from launcher System → System info and the context menu. It is not resizable and never snaps.

---

## 4. Control flow and lifecycles

### 4.1 Boot to desktop
1. `main.c:610` `winsrv_init()` (zero slots and retire list).
2. `shell_task` → `theme_init()`; if `fb_active() && !console_only && autodesktop` → `enter_desktop()` (shell.c:595-608).
3. `enter_desktop()` spawns `/bin/term` (ring 3), then `wm_run()`; on return `fbcon_clear()`/`vga_clear()`, "back at the shell" (shell.c:217-243).
4. `wm_run` start (3879-3895): require a framebuffer; `theme_init()`; `sound_set_volume(theme()->volume)`; record `boot_w/h` once; `apply_screen_size()`; `running = true`; `mouse_set_autodraw(false)`; read pointer state; `need_frame()`.

### 4.2 One pass of the main loop (3897-4061)
1. Drain `mouse_take_edge()`: each button change, at the position it happened, through `handle_mouse` (so a press+release within one slow pass is not lost).
2. Sample `mouse_x/y/buttons`; if different from last, `handle_mouse`.
3. `panel_update(last_my)`.
4. `mouse_take_scroll()`: over the speaker → volume −5·steps (`theme_set_volume`, which saves) + blip; else `WM_EV_SCROLL{x = local x, y = steps}` to the user window under the pointer (not the focused one).
5. One key from `kbd_trygetchar()`: Esc → close find, else close launcher, else **leave the desktop**; else `handle_shortcut`; else `find_key` (swallows everything while open); else `menu_key` while the launcher is open; else deliver to the topmost non-minimised window: `WM_EV_KEY{key = KEY_CODE | (c & KEY_MOD_CTRL)}` or `on_key(char)`.
6. Every `> timer_hz()/4` ticks: `theme_reload()`; on change `apply_screen_size()` + `need_frame()`.
7. For every dirty window: minimised → whole frame, else partial damage (window rect + 7/13 px margin).
8. Periodic whole frame every `timer_hz()` (clock) or `timer_hz()/12` with a moving wallpaper and no covering window.
9. Panel sliding → `panel_frame()`; context menu/launcher animating → partial damage for the panel and the pointer.
10. `needs_composite` → `composite()`; else `task_idle_wait()` (halt until any interrupt).
On exit (4064-4077): close every window, reset menu/ctx/band/selection/drag/resize/capture/volume/panel/snap/shake, `mouse_set_autodraw(true)`.

### 4.3 A ring-3 window's life
1. `win_create(title, w, h)` → slot, two buffers, `wm_create` (cascade position), handle.
2. `win_surface(h)` → pages of `pixels` mapped at `0x80_6000_0000 + h·8 MiB` with PTE_USER in the caller's PML4.
3. Optional `win_allow_resize(h)`.
4. Loop: draw into the surface → `win_commit(h)` (copy to `shown`, whole frame requested) → `win_poll(h, &ev)` until 0; optionally `win_set_text`.
5. User closes (close button, alt+q, Close all, leaving the desktop) → WM removes the `window_t`; slot's `win = NULL`; every subsequent poll yields `WIN_EV_CLOSE`.
6. `win_close(h)` or task exit/kill (`winsrv_release`) → unmap, `wm_close` if still open, free both buffers.

### 4.4 Resize handshake
- WM-initiated (maximise/snap/corner release/screen change): `wm_resize` → `winsrv_resize_window` records `want_cw/ch`, pushes `WM_EV_RESIZE{x=cw, y=ch}` (bounds 32..1600 × 32..1200). The WM moves the frame immediately (`place`). On the program's next `surface/size/poll/commit`, `apply_pending` → `resize_slot` allocates new buffers, remaps at the same user address, retires the old `shown`. The program redraws and commits.
- Program-initiated: `win_resize(h, w, h)` → `resize_slot` directly (no event, no move).

### 4.5 Find
ctrl+f or the magnifier → `find_start` (count windows with text) → each typed character → `find_recount` → if hits: `find_go` (raise the window with match `find_at`, send `WM_EV_FIND{y = local index}`), else `WM_EV_FIND{-1}` to all → program queries the string and marks the match → Return cycles → Esc or the button closes and clears marks.

### 4.6 Theme change
Settings (or the shell's `write`) writes `/zelr.cfg` → within ~0.26 s `theme_reload` sees a difference → `derive()` → `apply_screen_size()` (mode change if width/height changed) → whole repaint. Ring-3 programs read `/zelr.cfg` + `/sys/theme` at start (and settings re-reads `/sys/settings`); they are not notified.

### 4.7 State machines
- **Panel**: {out, tucked} × animation phase; transitions in `panel_update` according to `dock_hide`, maximised windows, pointer y, open menus and dragging.
- **Pointer mode** (mutually exclusive in practice): idle / band (left or right) / dragging window / resizing corner / captured by window / dragging volume slider.
- **Window**: normal ↔ minimised (chip, button, alt+m/alt+↓, alt+d, shake) ; normal ↔ maximised (button, right-click title, alt+f, alt+↑, drag to top edge; dragging un-maximises) ; snapped left/right is just a size and position (not flagged).
- **Launcher**: closed → open(categories) ↔ open(query results) → closed (choose, click away, right click, Esc, badge).
- **Find**: closed ↔ open(empty) ↔ open(query, n hits, cursor `find_at`).

---

## 5. Interfaces

**Exported by wm.c** (callers found by grep):
- `wm_run` -- shell.c:230. `wm_quit`, `wm_active` -- no callers.
- `wm_create/wm_close/wm_invalidate/wm_raise/wm_push_event/wm_pop_event/wm_icons_right/wm_work_height` -- winsrv.c, apps.c, selftest.c.
- `wm_set_text/wm_find_query` -- syscall.c:973, 983.
- `wm_outer_w/h` -- selftest.c:725-726.
- `wm_title_h/wm_border` via `WM_TITLE_H/WM_BORDER/WM_TOP` -- winsrv.c, selftest.c.
- `wm_resize` -- wm.c only.

**Exported by winsrv.c**: syscall.c (all), sched.c:672 and signal.c:33 (`winsrv_release`), main.c:610 (`winsrv_init`), wm.c (`winsrv_resize_window`, `winsrv_reap_retired`), selftest.c.

**Exported by theme.c**: wm.c (everything), sysfs.c (`theme_knob_*`, `theme()`, `theme_current_preset`), shell.c (`theme_init`, `theme()->autodesktop`), selftest.c.

**Exported by gfx.c**: wm.c, apps.c, blackbox.c, selftest.c.

**Ring-3 contract**: syscalls 7-12, 36, 37, 56, 57 (see §3.7); files `/zelr.cfg` (read/write), `/sys/settings` (`# key value low high default label` lines, sysfs.c:321-330), `/sys/theme` (`accent/desktop/surface/text/text_dim 0x…`, `light`, `look`, `preset`, sysfs.c:339-351). `ui_load_theme()` (userland/ui.h:233) duplicates the 6 preset accents and its own light/dark × modern/built palettes; defaults preset 1, light 1, look 0 match the kernel.

**Depends on**: fb.c (`fb_width/height/pitch/pixels`, `fb_put/get/rect/frame`, `fb_flush`, `fb_flush_rect`, `fb_active`, `fb_mode_settable`, `fb_set_mode`), fbcon (`fbcon_init`), mouse.c (`mouse_take_edge`, `mouse_x/y/buttons`, `mouse_take_scroll`, `mouse_set_autodraw`), keyboard (`kbd_trygetchar`, `KEY_*`), timer (`timer_ticks`, `timer_hz`), rtc (`rtc_present`, `rtc_format_short`), vfs (`vfs_read/write/slurp`), user.c (`user_spawn_elf`), sched (`task_by_pid`, `task_idle_wait`), paging (`map_page_in`, `unmap_page_in`, `unmap_page`, `paging_current_directory`), heap (`kmalloc/kcalloc/kfree`), sound (`sound_present/tone/set_volume`), power (`power_off`), diskfs (`diskfs_flush`), net/netdev/wifi (panel), apps.c (`app_about`).

---

## 6. Concurrency, locking, memory ownership, invariants

- **Who runs where.** The WM loop runs in the kernel shell task, a ring-0 task pinned to the boot processor ("Kernel tasks stay on the boot processor", sched.c:423-427). It is preemptible by the timer like any task. Syscalls from programs (possibly on APs) run inside the int 0x80 *interrupt gate* (`set_gate(0x80, …, 0xEE)`, idt.c:63), so with IF=0 for the whole call (fd.c:194-199), and under the big kernel lock, which a CPU holds whenever it is not in ring 3 (idt.c:150-188, 287). Consequently a syscall (e.g. `win_commit`'s copy) is never interleaved with WM code, but the WM can be preempted *between* any two of its statements and syscalls then run; when it resumes, window state may have changed under it.
- **What the code guards.** `composite()` reads `canvas/cw/ch` with interrupts off (2923-2927) and `resize_slot` writes them with interrupts off (348-355), so a surface is never paired with the wrong size. Replaced `shown` buffers are retired and freed only at the start of the next composite. A torn frame (half old, half new commit) is possible when the WM is preempted mid-blit; the comments accept that.
- **Not guarded.** `free_slot` frees `shown_raw` immediately (winsrv.c:152), so a compositor preempted mid-blit of that window can read freed heap memory (reads only; `px`, `cw`, `ch` were captured). `wm_set_text` runs `find_recount` from syscall context over `stack[]` while the WM may be suspended mid-update of the same arrays.
- **Ownership.** Kernel windows own `canvas`. User windows' `canvas` = slot `shown` (page-aligned interior pointer; never `kfree` it; the slot frees `shown_raw`). The mapped `pixels` pages lie entirely within `[raw, raw+bytes+4096)`, so no heap header is exposed to ring 3. `window_t` is freed only by `wm_close`.
- **Invariants.** `nwin ≤ 8`; `stack[0..nwin)` distinct and non-NULL; handle == slot index; a slot's `win` is NULL or a live window; `canvas` holds `cw*ch` pixels; `want_cw/ch` both zero or both set; `textlen ≤ 4096`; ring `q_head/q_tail < 32`.
- **Surface pages are special.** They are PTE_USER mappings of *kernel heap* frames. The rest of the kernel treats PTE_USER leaf entries as user memory: fork COW-shares them (paging.c:272-282), `paging_free_directory` frees them to the PMM (paging.c:212-213). The only thing preventing corruption on exit is that `winsrv_release` unmaps them before the address space is reaped (free_slot comment 141-146). Nothing handles fork or exec (see §10 item 3).

---

## 7. Limits and magic numbers

| Name | Value | Where | Meaning |
|---|---|---|---|
| WM_MAX_WINDOWS | 8 | wm.h:4 | WM stack incl. the about window |
| WM_EVENT_QUEUE | 32 (31 usable) | wm.h:60 | per-window event ring |
| WM_TEXT_MAX | 4096 | wm.h:77 | published words per window |
| title/app | 32 bytes each | wm.h:82,86 | 31 chars + NUL |
| press flag | 0x80 in buttons | wm.c:3786 | first event of a press |
| WINSRV_MAX | 8 | winsrv.h:10 | slots; handle = index |
| WINSRV_SURFACE_BASE | 0x80_6000_0000 | winsrv.h:19 | first surface in user space |
| WINSRV_SURFACE_STEP | 8 MiB | winsrv.h:20 | per-handle spacing |
| surface bounds | 32..1600 × 32..1200 | winsrv.c:157,284,389 | create/resize limits |
| RETIRED_MAX | 8 | winsrv.c:99 | parked old surfaces |
| cascade | x = 110+14+48·(h%5), y = 36+38·(h%5) | winsrv.c:195-196 | first window at (124,36) |
| wm_resize min | 120×60 | wm.c:1733 | |
| corner drag min | 160×80 | wm.c:1817-1818 | |
| drag clamp | y≥0, x∈[−(cw−60), W−60], y≤work_h−title_h | wm.c:3615-3619 | |
| snap EDGE | 12 px | wm.c:1831 | top→full, left, right |
| GRIP | 16 px | wm.c:1520 | |
| shake | 8 samples, ≤1 s, |dx|≥24, ≥3 turns | wm.c:211,1857-1868 | |
| band click slack | <4 px | wm.c:1550 | |
| TASKBAR_BADGE_W | 76 | wm.c:92 | |
| FIND_W, FINDBAR_H | 26, 38 | wm.c:114-115 | |
| VOL_W / VOLPOP | 30 / 208×40 | wm.c:153-155 | |
| NET_W / NETPOP | 26 / 268×150 | wm.c:158-160 | |
| chip width | title+22, ≤160, gap 6 | wm.c:2720-2752 | |
| MENU_* | rail 132, pane 152, pad 10, item 32, W 304, full H 212 | wm.c:175-179, 455 | |
| MENU_RIGHT | 100 | wm.c:184 | right-column index offset |
| MENU_QMAX / MENU_HITS | 24 / 4 | wm.c:469-470 | |
| CTX | 204 wide, 30/item, pad 8, H 196 | wm.c:367-383 | |
| SHADOW | 5 | wm.c:185 | damage margin, panel off-screen offset |
| shadow spreads | window 7/3, menu 12, ctx 12, popups/find 7, dock 10 | wm.c | |
| ANIM_FULL | 256 | wm.c:278 | |
| MENU_MS/HOVER_MS/PANEL_MS | 200/110/170 ms × anim_ms/120 | wm.c:293-294, 685 | |
| rise | launcher 8 px, ctx 6 px | wm.c:1890, 2071 | |
| PANEL_EDGE | 3 px | wm.c:686 | bottom band that summons the panel |
| panel stay band | panel_rest_y − 6 | wm.c:793 | |
| ICON_LEFT/TOP | 18/20 | wm.c:857-858 | |
| icon cell | 92×88 (hit height 82) | wm.c:855-856, 1146 | defaults |
| DESK_N | 5 | wm.c:879 | |
| SINE_Q | 17 entries, peak 248 | wm.c:744 | 64-step period |
| wallpaper repaint | timer_hz()/12 ticks | wm.c:4025-4026 | moving wallpapers only |
| clock repaint | timer_hz() ticks | wm.c:4026 | |
| theme poll | > timer_hz()/4 ticks (26 at 100 Hz) | wm.c:3994 | |
| volume blip | 880 Hz, 70 ms, ≥ hz/8 apart | wm.c:2623-2628 | |
| wheel on speaker | 5 % per step | wm.c:3937 | |
| MARK | 10 px | wm.c:1575 | |
| GLYPH | 16×16 | wm.c:2232 | |
| cursor | 12×19 | wm.c:2871 | |
| find query | 63 chars | wm.c:340 | |
| theme file | read ≤2047, write ≤1536 | theme.c:415, 499 | |
| key / value | ≤31 chars each | theme.c:427 | |
| presets / knobs | 6 / 31 | theme.h:224, theme.c:88-131 | |
| AA samples | 4×4 per edge pixel, eighths | gfx.c:360-370 | |
| shadow alpha | 70/(s+1), shifted +2 px down | gfx.c:190-191 | |
| face baseline | top + size·4/5 | gfx.c:288 | |
| faces | 13, 15, 20, 26, 15b, 20b (+15m, 15bm unused) | face.h:5213-5222 | |

---

## 8. Tests

**Kernel selftest** (`selftest_run`, selftest.c:3548-3575; counts match README 1350-1357):
- `[graphics]` `test_gfx` (666-710), 13 checks: `surf_clear`, `surf_rect` interior/outside/clipping at both corners, `surf_line` endpoints, `surf_disc` centre/radius, `surf_text` ink, three `kformat` checks.
- `[windows]` `test_wm` (712-801), 7 checks (skipped without a framebuffer): create two windows; outer size = `cw+2·border`, `ch+WM_TOP+border`; two moves fold to one; presses (0x81) never fold (3 events); a press survives 64 moves; release stays at the release position (52) and is not folded into the following move; close. Runs before `theme_init` in selftest mode, so the theme is all zeros (title falls back to 32, border is 0).
- `[window server]` `test_winsrv` (803-876), 19 checks: create for pid 4242, size, foreign pid rejected, surface maps at `WINSRV_SURFACE_BASE`, writes alias the slot's pixels, the desktop's canvas is a different buffer and does not see the write before commit, commit copies first and last pixels, a later write is not visible until the next commit, same address on re-query, foreign map/commit rejected, close, dead handle, heap shrinks on close (the double-free guard), multiple windows, `winsrv_release`.
- `[theme]` `test_theme` (1057-1135), 19 checks: default accent is a preset; a preset changes only the accent; save/reload round trip reports change then no change; a hand-written file (preset 2, wallpaper 1, corner 14, shadows 0) is applied; out-of-range wallpaper and corner clamp to the table; every knob clamps to hi for 99999 and to lo for 0.
- `[taskbar]` `test_pins` (998-1055), 18 checks -- of `kernel/pins.c`, which no longer has any caller besides this test.

**QEMU harnesses** (all 1024x768 unless stated, run by pipeline/gate.sh in `screen`/`full` mode, lines 405-535; they drive QEMU's monitor for mouse/keys/screendump through tools/harness.py and wait for conditions rather than sleeping):
- `deskcheck.py` -- 37 checks: terminal opens at 760x480 (page-colour count 300k-380k); minimise button; restore from chip; maximise fills the screen (>730k page pixels incl. the bottom strip) and the program redrew at the new size; panel tucks away, returns at the bottom edge, leaves again; restore; corner drag shrinks; alt+← / alt+→ halves; `write /zelr.cfg wallpaper 4` (stars) changes the desktop and keeps moving; wallpaper 6 differs; launcher opens from the badge and starts Paint (Media row 2, item 0); alt+tab changes the front window; alt+d clears; terminal returns from its chip; wheel scrolls under the pointer and back; `width 800`/`height 600` via `write`/`append` resizes the screen and the panel follows; wallpaper click opens the launcher; right click opens the context menu; "Close all windows" closes everything; a band draws, selects icons 1-4, and a band elsewhere deselects; badge + typing "ain" + Return runs Paint; second machine with no command line (512 MiB) opens the desktop by itself, no console prompt, Escape returns to the shell, and a 1920x1080 mode is laid out across the screen.
- `shotcheck.py` -- 10 checks: ring-3 terminal drew (dark page), mode is 1024x768, launcher opens, menu top found on screen, Settings launches (System row 4, item 0) with all six swatches visible, the teal swatch is found and clicking it swings >600 pixels from indigo to teal in the chrome.
- `setcheck.py` -- 10 checks: `/sys/settings` has ≥25 rows, each with lo ≤ value/default ≤ hi and lo < hi; required keys present; `/sys/theme` has hex colours; dock at 44 px; `write /zelr.cfg dock_h 96` from the console makes the dock tall; Settings opens from the launcher by typing "sett"; the Everything-page `dock_brand` switch removes and restores the badge; the saved file contains `dock_brand 1`, `dock_h 44`, `wallpaper 11`, `anim_ms 120`.
- `findcheck.py` -- 8 checks (with a local web server): the middle of the dock is bare; ctrl+f (byte 6 over serial) opens something above the dock; typing "connection" makes the browser paint FFE58F behind the word, the count shows, the page changed; "zzzqqq" lights nothing but is answered; Escape (retried) closes the bar.
- `defaultcheck.py` -- static; nominally compares kernel `theme_init` defaults with settings.c initialisers and checks every held key is written. Currently yields 3 checks, two of them vacuous (§10).
- `shots.py` -- not a test: boots with a local web server, runs `dhcp`, and writes `docs/boot, desktop, launcher, files, paint (three strokes), settings, wallpaper (aurora, wallpaper 7), monitor (after 10 s), calc ("78/4="), browser, find ("connection"), maximised (alt+↑ on a fresh terminal)` as PNGs (own zlib PNG writer, colour type 2, filter 0). All 12 exist in `docs/`.
- Related harnesses not read in full: `tearcheck.py` (uses the `halfdrawn` program to prove commits are whole frames), `framecheck.py` (band flush cost), `volcheck.py` (dock slider loudness), `netcheck.py` (network icon, panel, DHCP button), `gamecheck.py`, `browsercheck.py`.

---

## 9. How to extend

- **A new setting that is a number or a switch**: add the field to `theme_t` (theme.h) and one `KNOBS[]` line (theme.c:88-131) with key, label, `K(field)`, is_bool, lo, hi, default; read it through `theme()` in wm.c. Parser, writer, defaults, `/sys/settings` and the settings program's Everything page follow automatically. Keep lo < hi (setcheck asserts it) and remember the WM's accessor fallbacks only trigger for out-of-range values.
- **A new colour/palette key**: extend `apply()` and the custom branch of `theme_save()`, `render_theme()` in sysfs.c, and `ui_load_theme()` in userland/ui.h (they duplicate preset accents and palettes); put it after `look`/`light`/`preset` in the file.
- **A new wallpaper**: add the enum value before `WALLPAPER_COUNT` (the knob range follows), a case in `draw_wallpaper()`, add it to `wallpaper_moves()` if it animates, and update settings.c's `N_WALLPAPERS`/`WALLPAPERS[]` (a hard-coded 12-name list). Keep it cheap: the comments insist on column fills or a few thousand points, never per-pixel work over the screen (BLOOM's glows and vignette are the per-pixel exception and it is static).
- **A new launcher program/category**: add to an `M_*` array or a new `MCAT[]` entry; heights are computed. Tests hard-code row/column positions (deskcheck `rail_row(2)`, shotcheck `MENU_KINDS`, shots.py tuples) and the comments record breakage every time a category was inserted.
- **A new window-server call**: validate with `lookup(pid, handle)`, call `apply_pending(s)` first, add the syscall to syscall.h, syscall.c's table and sdk/zelr.h.
- **A new event type**: add `WM_EV_*` and the matching `WIN_EV_*`; keep `wm_event_t` 20 bytes (ring 3 reads it raw); make sure `wm_push_event`'s fold condition cannot swallow it (only mouse moves fold).
- **Pitfalls stated in comments**: draw and hit-test must use the same geometry function (repeated in 82-86, 135-143, 2185-2190, 2693-2696, 3212-3238); never free a surface the compositor may be reading (retire it); never swap a surface except on the owning program's own call; keep integer arithmetic; the damage rectangle is a single union; the settings program writes the whole file so a key the kernel does not know is lost on the next save.

---

## 10. Doc drift and suspicious code (verified)

Ordered roughly by severity.

1. **Use-after-free: `resizing` is not cleared when its window closes.** `wm_close` clears `dragging` and `mouse_capture` only (wm.c:573-574). If the window being corner-resized is closed (its program exits or is killed, or alt+q during the drag), `draw_resize_preview` (2131-2137) and `resize_from_pointer` (1813-1822) keep dereferencing it every frame, and the left release calls `wm_resize(resizing, …)` (3587) on freed memory -- for a user window that writes `want_cw/ch` and pushes into the freed queue; for a kernel window it `kfree`s whatever `canvas` now contains.
2. **Right-button presses capture the pointer until a *left* release.** `released` tests only bit 0 (3437). A right press in window content sets `mouse_capture` (3782); a right press on the grip starts `resizing` (3762-3768); a right press in the volume popup sets `volume_drag` (3505-3512). The right release clears none of them. Until the next left release, every event goes to the captured window (3638-3648) -- including a later left click on the dock or another window, which is delivered to the captured window as a plain move with buttons=1 and no 0x80 press flag. Bands handle the right button correctly (3528), which shows the asymmetry is unintended.
3. **Window surfaces do not survive fork or exec (latent).** Surface pages are mapped PTE_USER (winsrv.c:244) but are kernel-heap frames. `copy_table` COW-shares every PTE_USER leaf (paging.c:272-282), so after a fork the parent's next write to its own surface is redirected to a private copy (the window stops updating: commit copies the stale `pixels`), and when the child later exits or execs `free_table` calls `pmm_free_frame` on the heap frame (paging.c:212-213), which marks it free in the PMM bitmap once the share count is back to zero -- the heap and the PMM then both own it. `sys_exec` (syscall.c:228-302) calls no `winsrv_release`, frees the old directory (300) that the slot still records in `s->dir`, so the eventual `free_slot` runs `unmap_page_in` over freed page-table frames and the same heap-frame freeing happens. No current windowed program forks or execs (only test programs and `sh.c` do), which is why nothing has hit it.
4. **Maximise can fail silently yet set `maximized`.** `apply_snap` sets `maximized = (zone == SNAP_FULL)` after `place()` even when `place` returned early because the resize was refused (wm.c:1761-1762, 1784-1791). `winsrv_resize_window` refuses widths > 1600 (winsrv.c:389), so on the 1920x1080 mode Settings offers, maximising any program window leaves it at its old size but flagged maximised: the panel tucks away (`panel_in_the_way` trusts the flag, 705-709), the grip disappears, the button shows "restore". `screen_changed` has the same path (3141-3146).
5. **Dock chip hit test disagrees with drawing.** `taskbar_chip_at` measures the focused chip in `FACE_BODY_BOLD` (3224-3226) while `draw_taskbar` measures and draws every chip in `FACE_BODY` (2720, 2743, 2749). The comment claims the front window "carries a heavier title" -- true of the title bar, not the chip. The focused chip's hit box is wider than drawn and every later chip's hit box is shifted.
6. **Minimised top window: keyboard target and shortcuts disagree.** Minimising does not lower the window (1803-1808). Keys go to the topmost *non-minimised* window (3971-3973), but that window is not drawn focused (`draw_chrome(w, i == nwin-1)`, 2917) and no chip is highlighted, while alt+m/f/q/arrows act on `stack[nwin-1]`, the minimised one (3849-3871) -- e.g. alt+q closes the invisible window rather than the one being typed into.
7. **alt+tab only alternates the two top windows.** `cycle_windows` raises the first non-minimised window below the top (3807-3812); pressing again raises the previous top. The comment "pressing it repeatedly walks the stack" (3800-3801) is false for three or more windows.
8. **Find navigation renumbers as it goes.** Matches are numbered front to back (3286), but `find_go` raises the window it goes to (3323), which changes the order for the next Return. With matches in two windows the sequence skips matches and revisits others instead of walking all of them.
9. **Volume from the file is never applied to the sound driver.** `theme_reload` stores `volume` (via the knob table) but nothing calls `sound_set_volume` on reload (theme.c:409-454, wm.c:3996); only `wm_run` start (3884) and `theme_set_volume_live` do. A volume changed from Settings' Everything page or with `write` shows on the dock slider but is not heard until the next desktop entry; dragging the slider to exactly that value then does nothing either (`theme_set_volume_live` returns early when unchanged, theme.c:465).
10. **Partial flushes can be dropped or leave stale pixels.** `fb_flush_rect(u32 x, u32 y, …)` returns immediately when `x >= width || y >= height` (fb.c:508-510). The WM passes the damage union as signed ints (wm.c:2946); for the context menu opened within 14 px of the left or top edge (`ctx_x − 14`, 4040), the launcher opened from a wallpaper click near the left edge or with `dock_side < 14` (4046), or the pointer within 2 px of the left/top edge (4041, 4048), the negative origin wraps and the whole animation frame is never sent. Separately, the launcher/context-menu damage rectangles ignore the 8/6-px rise, so up to 8 rows of the shadow drawn in the first frame stay on screen until the next whole frame.
11. **`theme_save` drops hand-set grounds when the accent matches a preset.** It writes only `preset N` whenever `theme_current_preset() >= 0` (theme.c:512-540), and that function compares accents only (208-212). A file with a preset accent plus custom desktop/surface/text loses them the next time the kernel saves -- which it does on every dock volume change, mute or wheel step (`theme_set_volume` → `theme_save`).
12. **Inconsistent fallback preset** when the accent is custom: a `look` line re-applies preset 1 (theme.c:397), a `light` line preset 0 (405). Either way a custom accent is discarded unless its colour lines come after them.
13. **State leaks across desktop sessions.** The cleanup at wm_run exit (4064-4077) does not reset `find_open`, `find_q`, `net_open`; leaving via "Leave desktop" with the find bar or network popup open brings them back on the next `desktop`.
14. **Escape is never delivered to programs and ignores open popups.** Esc always goes to the WM (3953-3956): it closes find or the launcher, otherwise leaves the desktop -- even with the context menu, volume or network popup open. No ring-3 program can receive Escape.
15. **`fb_shadow` is not "identical" to the per-pixel version its comment describes** (gfx.c:178-187). Rings are offset 2 px down; ring s paints full rows only at `y+h+s` and `y+h+s+1`, so the row directly under the rectangle (`y+h`) is darkened only by the curved edge spans near the corners, and rows further out receive fewer rings than a "darken everything in the ring rect outside the window" test would give.
16. **Chrome cost contradicts the header.** wm.c:8-12 says chrome is drawn per pixel "only around the edges of a window rather than across it", but modern `draw_chrome` calls `fb_round_rect_aa` three times over the whole outer rectangle (1620, 1632-1634), each a per-pixel `fb_put` loop and one of them a read-modify-write blend; the dock and menus do the same. Only `fb_shadow` is edge-only.
17. **`win_commit`'s copy runs with interrupts off**, contrary to winsrv.c:53-58 ("The copy is not atomic -- three megabytes with the interrupts off would stall the timer"). The int 0x80 gate is an interrupt gate (idt.c:63, fd.c:194-199), and nothing in the path re-enables interrupts, so the copy (up to 7.5 MiB) does stall that CPU's timer. Tearing can still happen, but because the compositor's blit is preemptible, not because the copy is.
18. **resize_slot comment vs code.** winsrv.c:307-312 says the remap happens "in the owner's address space, which is almost never the live one", but line 292 refuses unless it is the live one; resizes are deferred to the owner's own call precisely for this reason.
19. **SYS_WIN_FIND takes no handle.** Any process can read the current find query at any time (syscall.c:979-984, wm.c:3353-3359).
20. **About window position is fixed** at (682,120), 324 px wide (apps.c:71): on 800x600 it opens mostly off screen with its close button out of reach.
21. **`wm_border()` does not fall back to 1 for an unset theme.** The comment (519-522) says both accessors fall back "when nobody has said otherwise … the self test builds windows before theme_init has run", but a zeroed theme gives border 0, which the range check accepts (528-531).
22. **Restore mark not centred.** wm.c:1571-1574 says the marks are centred in whatever size the button is; `mark_restore` (1587-1594) still uses fixed offsets from the top-left.
23. **Hover "fade" is a threshold** (1953, 1980, 2031, 2090); theme.h:118 and the knob label "Fade rather than snap" overstate it. Only the panel slide and the menu rise animate.
24. **Restoring a window smaller than 120x60 fails** (`wm_resize` minimum, 1733) after `maximized` has already been cleared (1795-1797), leaving it full-size but unflagged.
25. `winsrv_surface`'s failure path unmaps with `unmap_page` (current directory) instead of `dir` (winsrv.c:245) -- harmless today because the syscall always passes the current directory.
26. **Dead code/data**: `winsrv_frames/winsrv_bytes` (winsrv.c:67-71, never declared or read); `window_t.app` (set, never read) and `window_t.open`; `TASKBAR_R` (wm.c:80); `wm_quit`, `wm_active`; theme fields `raised` and `hairline` (never drawn) and the modern `title_*` values; gfx `gfx_text_bg`, `gfx_text_width`, `surf_frame`, `surf_blit`, `fb_round_rect`, `face_surf_text`; kernel faces 6 and 7; `kernel/pins.c` (used only by selftest `[taskbar]`, 18 checks testing an unused module).
27. **Stale comments in wm.c**: pinned apps (91, 691, 2140-2146, and the empty block at 3998-4006 describing re-reading "the taskbar's own list"); `draw_taskbar` "Flush to the bottom edge and the full width of it … A floating rounded one was tried here once and read as an app" (2657-2666) vs the floating dock described at 44-67; "five rows today" (171) -- six categories; "thirteen labels is a loop" (467) -- 15; `"web"` as an example that should find the browser (478-480; also deskcheck.py:717-720) -- the match is on labels only and "web" is not in "Browser"; `"no matches"` (2357, 3257) -- the bar says "not on screen"; "a 512 byte read" (3993) -- the buffer is 2 KiB; "A second click on a title bar is the usual way to maximise" (3772) -- it is a right click; "Four times a second" -- the test is `> hz/4`, about 3.85 Hz.
28. **Other stale headers/comments**: wm.h:43-44 says for `WM_EV_SCROLL` "x and buttons are where the pointer was" but `buttons` is always 0 (wm.c:3945-3947); gfx.h:67-68 says the 8x16 bitmap "is still what the terminal draws with" -- the ring-3 terminal uses the monospaced AA face (term.c `UI_FACE_MONO`); gfx.c:110-114 ("a real rasteriser would antialias") predates the AA routines and gfx.c:420-422 is the `fb_glow` comment orphaned above `fb_vignette`; theme.c:34-35 ("Which ones have to be redrawn to look right") is an orphaned comment above the light-ground notes, its code having moved to `wallpaper_moves` (169-173).
29. **README drift**: "Eleven wallpapers, six of which move" (README:915) -- 12 exist (enum and settings.c `N_WALLPAPERS 12`); "The panel is flush to the bottom edge and the full width of it under either look" (1073-1075) -- it floats in both looks (built is full width but lifted by `dock_gap`); shotcheck "draw[s] a stroke in paint" (1411-1416) -- it now opens Settings and changes the accent; README lists alt+tab, alt+arrow, alt+d but not alt+m/f/q.
30. **Test drift**: `defaultcheck.py` is vacuous -- settings.c declares `static int light = 1, look = 0, preset = 1, custom = 0;` on one line, which its regex (defaultcheck.py:88) cannot match, so the kernel/settings comparison loop runs zero times, and its `put_kv` regex (100) matches nothing in the current `save()`; only "the wallpaper enum could be read" is a real check. `deskcheck.py` docstring (lines 8-12: window at x=40, buttons 26/46/66 px in, 762x505) disagrees with its own constants (x=124, button centres 18/50/82 px in, 762x514); deskcheck:444 calls the default wallpaper "a gradient" (it is BLOOM). `shotcheck.py` defines unused `CHROME`/`TITLEBAR`. `shots.py`:34-37 still says the panel "went flush to the bottom edge".
31. **Minor behaviour notes** (by design or harmless): modern windows get no shadow when `corner == 0` (1618); the `snap` knob does not disable alt+arrow snapping; the dock swallows clicks across the full screen width at its height even beside the floating dock (3712-3713); a left click then a right click on the same icon within `dblclick_ms` launches it and opens the menu; dragging a maximised program window re-centres on the *old* width because the resize is deferred (3609); `winsrv_resize` (program-initiated) neither moves the window nor clears `maximized`; if `resize_slot` fails after `WM_EV_RESIZE` was delivered, the event's size is wrong -- programs should re-query `win_width/height`.

---

## 11. Open questions

1. **Kernel lock while the WM idles.** The WM (a kernel task on CPU 0) calls `task_idle_wait()` (a bare `hlt`) while holding the big kernel lock; per idt.c:272-287 only the idle task drops the lock when halting. Do syscalls from programs on APs stall until CPU 0 schedules away from the WM? (Scheduler/SMP area should confirm; `smpcheck.py` exists.)
2. `test_winsrv` maps a surface into `paging_current_directory()` from the selftest task (selftest.c:813). If that is `kernel_pml4`, it creates a user-half PML4 entry in the kernel's directory that later `paging_new_directory()` calls would copy into every new process. Harmless if nothing spawns afterwards in selftest mode -- worth confirming.
3. Should Escape reach programs (a modifier-less Esc could be forwarded when no desktop popup is open)? The current rule makes Esc the only way out of an autostarted desktop, which deskcheck relies on.
4. Is the find numbering meant to be stable while navigating (raise without renumbering), and should the terminal mark its matches like the browser does?
5. Intended semantics of `/zelr.cfg` reload: incremental (current behaviour) or "file is the whole truth" (defaults for missing keys)? Settings always writes every key, the shell's `write` does not.
6. Should `apply_screen_size` apply a size with only one of `width`/`height` set (it currently combines it with the boot size of the other)?
7. Is the 1600x1200 surface limit (8 MiB step) meant to cap maximise on 1920x1080, or should `WINSRV_SURFACE_STEP` grow?
