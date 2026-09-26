# 16 -- The zelr design system

Synthesis of how zelr looks and feels, drawn from the window manager (atlas 07), the ring 3 toolkit
(atlas 09a), the typeface generator (atlas 13), the README's own design narrative and the screenshots in
`docs/`. File:line references are to the tree at commit 6048716 (main, two commits after v0.37.0).

Everything visual in zelr is **drawn in code**. There are no image, icon or font files anywhere: the
typeface is generated from outlines in `tools/genface.py`, icons are shapes in a 32-unit square, playing
cards are discs and triangles, wallpapers are integer arithmetic. Keep it that way: adding an asset file
breaks the project's central promise.

---

## 1. Principles (as the code and README state them)

1. **A surface is a plane lit from above and to the left.** Raised things have a bright top-left and a
   dark bottom-right edge. Sunk things are the reverse. A pressed button turns its bevel over and moves its
   label 1 px down and right; it does not change tint (README "A desktop that is built rather than tinted").
2. **Two looks, both complete.** `look 0` MODERN ("material": anti-aliased rounded panes, hairlines,
   translucent glass dock, sheen, soft shadows) is the default. `look 1` BUILT (2-px bevels, square windows,
   no shadows, gradient title bars). Neither is a fallback, and every component has both branches. The
   setting is one line in `/zelr.cfg`.
3. **Everything is a setting.** The dock height, gap and corner; what sits on the dock; title bar, frame
   and buttons; icon size; fade length; double-click time. That is 31 numeric knobs plus colours, all in
   `/zelr.cfg`, with one table (`KNOBS[]`) driving the parser, the writer, the defaults, `/sys/settings` and
   the Settings program's "Everything" page, so the file and the UI cannot drift.
4. **Settings is a file, not an API.** Ring 3 cannot reach the window manager. Settings writes
   `/zelr.cfg`; the WM re-reads it about four times a second (every ~26 ticks). Programs read the same file
   plus `/sys/theme` and derive their own palette. There is no theme syscall.
5. **Nothing knows a resolution.** Every window, the dock, the launcher and every wallpaper is laid out from
   the framebuffer's width and height every frame, so the mode can change at runtime.
6. **Cheap on purpose.** The machine has no GPU acceleration. A wallpaper costs "a column fill or a few
   thousand points a frame, never a calculation per pixel of the screen". A maximised window that covers the
   screen suppresses the wallpaper and everything under it.
7. **Integer arithmetic only in the kernel.** Curves come from a 17-entry quarter-sine table, easing is a
   cubic in 1/256, corner coverage is in eighths of a pixel, and glows and vignette use thousandths. Ring 3
   *may* use floating point (userland is built without `-mno-sse`, and the JPEG IDCT uses float), but the
   toolkit does not.
8. **Honest states.** The network icon has three states (no link / link without an address / reachable),
   not two. Find says "nothing to look at" and "that word is not here" as different answers. A setting the
   hardware cannot honour, such as the resolution under a UEFI framebuffer, is reported rather than offered.
9. **Drawn, not stored.** Icons, card pips, tray glyphs and the pointer are geometry, so a bigger icon is a
   bigger drawing.

---

## 2. Where the design lives

| Layer | Files | Role |
|---|---|---|
| Tokens (kernel) | `include/theme.h`, `kernel/theme.c` | `theme_t`; the 6 accent presets; the ground constants per look × light; `derive()` palette; the 31 knobs; the `/zelr.cfg` parser and writer |
| Drawing primitives (kernel) | `include/gfx.h`, `kernel/gfx.c` | stepped and AA rounded rects, shadow, gradients, bevels, glow, vignette, sheen, face text |
| Components (kernel) | `kernel/wm.c` | window chrome, dock/tray/chips, launcher, context menu, popups, find bar, icons, wallpapers, pointer, animation |
| Tokens + widgets (ring 3) | `userland/ui.h`, `userland/draw.h` | `ui_theme` palette derived independently from `/zelr.cfg` + `/sys/theme`; metrics; widget set |
| Typeface | `tools/genface.py` → `include/face.h` (kernel, 8 faces), `userland/face.h` (6), `userland/facetext.h` (browser, 33) | outline letterforms, coverage rasteriser |
| Bitmap font | `tools/genfont.py` → `kernel/font.c`, `userland/font.h` | 95 hand-drawn 8x16 glyphs for the boot console and a few legacy spots |
| Live exports | `/sys/theme` (sysfs.c:339-351), `/sys/settings` (sysfs.c:321-330) | current colours; the knob list for Settings |
| Drift guards | `tools/defaultcheck.py`, `tools/setcheck.py` | defaultcheck is meant to compare kernel and Settings defaults but currently matches nothing (see §14) |

The kernel and ring 3 each carry their own copy of the palette logic. They agree on the six accents and
the defaults (modern, light, preset 1 indigo) but derive *different* surface colours (compare §3.4 and
§3.6). That is by construction: chrome is drawn by the kernel, content by the program.

---

## 3. Colour

### 3.1 Inputs
The file carries five colours (`accent`, `desktop`, `surface`, `text`, `text_dim`, hex with or without
`0x`) plus three switches: `look` (0 modern, 1 built), `light` (1 light, 0 dark) and `preset` (0..5).
Kernel defaults (`theme_init`, theme.c:324-342): modern, light, preset 1. A `look`, `light` or `preset`
line re-applies a preset and overwrites the five colours, so hand-set colours must come after it in the file.

### 3.2 Accent presets (theme.c:22-32; identical list in ui.h `UI_ACCENTS`)

| # | name | accent | built-look dark desktop | built-look dark surface |
|---|---|---|---|---|
| 0 | teal | `#2CC7A0` | `#12181E` | `#1A222A` |
| 1 | **indigo (default)** | `#6E8AE8` | `#141622` | `#1C1F2E` |
| 2 | amber | `#E0A03C` | `#1A1612` | `#25201A` |
| 3 | rose | `#E06A8C` | `#1B1318` | `#271C22` |
| 4 | slate | `#8A9BB0` | `#14171B` | `#1E2329` |
| 5 | lime | `#9AD14A` | `#141A14` | `#1D251D` |

In the MODERN look every preset shares the same grounds and only the accent differs. In the BUILT dark
look each preset brings its own tinted desktop and surface.

### 3.3 Kernel grounds per look × light (theme.c:52-67, applied by `theme_apply_preset`, 188-206)

| | desktop | surface | text | text_dim |
|---|---|---|---|---|
| modern light (default) | `MODERN_DESKTOP #10254A` | `MODERN_L_SURFACE #F4F4F7` | `#17181C` | `#5D606A` |
| modern dark | `#10254A` | `MODERN_D_SURFACE #22242B` | `DARK_TEXT #E2E9EE` | `DARK_DIM #778693` |
| built light | `LIGHT_DESKTOP #334452` | `LIGHT_SURFACE #D6D3CD` (warm grey) | `#121214` | `#5C5A57` |
| built dark | preset's own | preset's own | `#E2E9EE` | `#778693` |

The built light surface is a warm grey rather than off-white because "a near-white one has nowhere to put a
highlight: every bevel collapses to a single grey line" (README).

### 3.4 Kernel derived palette (`derive()`, theme.c:237-320)
`mix(a,b,t)` = per-channel `(a*(255-t) + b*t)/255`; `luma` = `(77R + 151G + 28B) >> 8`;
`dark = luma(surface) < 128`; W = white, K = black.

| token | dark surface | light surface | used for |
|---|---|---|---|
| raised | mix(s,W,26) | mix(s,W,12) | (not drawn by wm.c) |
| overlay | mix(s,W,42) | mix(s,W,18) | launcher / menu panels |
| sheen | mix(s,W,92) | mix(s,W,48) | hover patch, top gloss |
| hairline | mix(s,W,44) | mix(s,K,28) | (not drawn by wm.c) |
| accent_soft | mix(s,accent,48) | mix(s,accent,40) | pressed button face |
| accent_text | luma(accent) > 140 ? mix(accent,K,205) : W | same | text on accent |
| text_mute | mix(text_dim, s, 95) | same | network icon when nothing is connected |
| edge_hi | mix(s,W,120) | mix(s,W,255) | outer top-left bevel |
| edge_light | mix(s,W,56) | mix(s,W,96) | inner top-left bevel |
| edge_shadow | mix(s,K,78) | mix(s,K,96) | inner bottom-right bevel |
| edge_dark | mix(s,K,150) | mix(s,K,190) | outer bottom-right bevel |
| well | mix(s,K,66) | mix(s,W,210) | sunk track (volume slider) |
| glass | mix(s,W,10) | mix(s,W,80) | modern dock fill |
| stroke | mix(s,W,60) | mix(s,K,26) | modern hairline outlines |

Title bars: MODERN has no title fill (the window is one surface; title text is `text` when focused and
`text_dim` otherwise). BUILT uses `title_a = accent` → `title_b = mix(accent,W,58)` as a horizontal
gradient with `accent_text`; unfocused uses `title_off_a = mix(s, dark ? W : K, dark ? 10 : 34)` →
`title_off_b = mix(s, W, dark ? 30 : 70)` with `text_dim`.

### 3.5 Colour helpers in wm.c (814-826)
`lighten(c, a)`, `darken(c, a)` (mix toward white or black) and `paper(amount)`: a mark *away from* the
desktop colour, darker on a light desktop and lighter on a dark one. Pattern wallpapers use `paper()` so they
work on any ground.

### 3.6 Ring 3 palette (`ui_load_theme`, ui.h:233-342)
Read from `/zelr.cfg` (preset default 1, light default 1, look default 0), with the accent overridden by
`/sys/theme` when present, so a hand-set accent follows the WM.

| token | modern light | modern dark | built light | built dark |
|---|---|---|---|---|
| bg | `#F4F4F7` | `#22242B` | `#D6D3CD` | `#2A2D33` |
| panel | `#FAFAFC` | `#2A2D35` | `#D6D3CD` | `#2A2D33` |
| fg | `#17181C` | `#E6E8EA` | `#121214` | `#E6E8EA` |
| dim | `#5D606A` | `#9AA0A8` | `#5C5A57` | `#9AA0A8` |
| line | `#DDDDE3` | `#353841` | `#8E8B86` | `#181A1E` |
| stroke | `#D2D3DA` | `#3C404A` | 0 | 0 |
| raised | `#FFFFFF` | `#2F323B` | 0 | 0 |
| well | `#FFFFFF` | `#1B1D23` | `#FFFFFF` | `#1A1C20` |
| edge_hi / light / shadow / dark | (= raised, raised, stroke, stroke) | same | `#FFFFFF` / `#E8E6E1` / `#868480` / `#3C3B39` | `#5A5F68` / `#3C4048` / `#1C1E22` / `#0C0D10` |
| accent_fg | white | white | (computed) | (computed) |
| soft | mix(bg, accent, 40) | mix(bg, accent, 48) | same rule | same rule |
| warn | `#E06C60` | `#E06C60` | `#E06C60` | `#E06C60` |

Programs typically call `ui_load_theme()` every frame, so a Settings change reaches every open window
within a frame (at the cost of two file reads per frame).

### 3.7 Fixed colours (not themed)
- Close button hover: red `#D93A3A` at α240 with a white mark (wm.c chrome).
- Pointer: outline `#080C10`, fill `#F4F7F9` (`CURSOR[19][12]`).
- BLOOM wallpaper (default): vertical gradient `#0A0F2A` → `#070A1B`; a cool glow `#3A7BFF` at (30%W, 25%H),
  radius 60%×60%, strength 96; a warm glow `#9B4BE0` at (80%W, 70%H), 40%×40%, strength 64; a bright core
  `#C8DEFF`; then a vignette at strength 150.
- The System info window (`kernel/apps.c`) uses fixed colours and the 8x16 bitmap font, not the theme.
- Terminal icon: dark tile with a green chevron.

---

## 4. The two looks, side by side

| Element | MODERN (`look 0`, default) | BUILT (`look 1`) |
|---|---|---|
| Window body | `fb_round_rect_aa`, radius `corner` (12), AA 16-sample corners | square, `raised` 2-px bevel |
| Window outline | 1-px AA hairline: focused `mix(stroke, accent, 190)` α235, else `stroke` α110 | bevel edges |
| Shadow | `fb_shadow` spread 7 (focused) / 3, only if `corner > 0 && shadows` | none ("a bevel already says which way is up") |
| Title bar | no fill; title `FACE_BODY_BOLD` + `text` when focused, `FACE_BODY` + `text_dim` otherwise, at x + border + 10 | horizontal gradient accent → mix(accent, W, 58), `FACE_BODY_BOLD` in `accent_text`; unfocused grey pair |
| Title buttons | invisible at rest; hover `sheen` α130 (close: red); pressed `accent_soft` α235 | surface face with raised/sunken bevel |
| Client area | 1-px `stroke` frame | `sunken` bevel |
| Resize grip | 3 bevelled strokes | same |
| Dock | floating glass: shadow spread 10, glass α208, stroke α70, glass α232 inset, sheen α44, radius `dock_radius` (14) | full-width surface bar with a thin highlight bevel, still floating by `dock_gap` |
| Focused chip | accent pill α46 + 14×3 accent underline | pressed button |
| Clock | `FACE_HEAD` (20 px) at dock right − 20 | `FACE_BODY` at screen right − 8 |
| Launcher/menus | shadow r16 spread 12, `overlay` α250, stroke α85 | surface + raised bevel |
| Desktop icons | flat AA shapes on a rounded tile (radius `IS1(14)`), accent wash when selected | bevelled pictograms, accent box behind a selected label |
| Selection band | accent wash α46 + 1-px lines α190 | dotted frame in `text` |
| Ring 3 buttons (`ui_button`) | rounded card, accent outline on hover or press | bevel flips on press, label moves 1 px |
| Ring 3 wells, fields, status bars | rounded, hairline | sunk bevel panels |

---

## 5. Shape and depth

- **Radii**: window 12 (`corner` knob, 0..24), dock 14 (`dock_radius`), launcher 16, context menu 14, ring 3
  `UI_RADIUS` 6, desktop icon tile `IS1(14)` at size 32.
- **AA corners**: `corner_cover` samples a 4×4 grid per pixel (16 samples, positions in eighths) and returns
  `inside*255/16` (gfx.c:360-370). Every pixel of an AA rect goes through `fb_put` (and `fb_get` when
  translucent). Ring 3 has its own `ui_corner_cover` using the same method.
- **Hairline trick**: draw the shape in the stroke colour, then the surface inset by 1 px on top, which leaves
  an anti-aliased 1-px outline.
- **Shadow** (`fb_shadow`): for s = spread..1, alpha = 70/(s+1), a ring shifted 2 px down with radius r+s.
  Each ring darkens only its top 2 and bottom 2 rows and 2-px side edges.
- **Sheen** (`fb_sheen`): white over the top half, falloff `strength*fall²/255²`, keeping the top corners round.
- **Glow** (`fb_glow`): elliptical, (1 − d²)² falloff, no rim. **Vignette**: squared distance in thousandths,
  untouched inside 120, darkening up to 255.
- **Bevel rule**: 2 px, where "one is a line and two is a bevel". The outer pair carry the strong colours
  (`edge_hi`, `edge_dark`) and the inner pair the soft ones (`edge_light`, `edge_shadow`). The top row owns the
  corner, so there is no notch (`fb_bevel`, gfx.c:243-259; `ui_bevel`).

---

## 6. Typography

### 6.1 The typeface (`tools/genface.py`)
- Original outlines on a **1000-unit em**: cap height 700, x-height 500, ascender 730, descender -210.
  Stroke weights: regular STEM 92 / THIN 78, bold STEM 126 / THIN 107 (+37%). Bold is the same drawing with
  heavier strokes, not a second outline set.
- Built from `stem`, `bar`, `diag` and arc primitives with proportions defined once at the top. The `a` is
  **double-storey** (it was single-storey once; the change is recorded in its docstring). Default advance is
  560 units.
- Rasteriser: exact horizontal coverage over 4 sub-scanlines per row, giving 8-bit coverage per pixel.
  **No hinting or grid-fitting**, so stems straddle two columns at small sizes. `OVER = 12` (overshoot) is
  defined but never applied.
- **Monospaced cut**: derived from the proportional face into a 620-unit cell. Wider glyphs are condensed,
  narrower ones centred, "so that column n is under column n on the line above". The terminal and the editor
  use it.
- **ASCII only** (32..126). Everything else draws as a space; the browser transliterates curly quotes and
  dashes to ASCII.

### 6.2 Face sets
| Set | Header | Faces |
|---|---|---|
| `FACES_KERNEL` | `include/face.h` | 13, 15, 20, 26 regular; 15b, 20b; 15 mono; 15b mono (8) |
| `FACES_USER` | `userland/face.h` | 13, 15, 20 regular; 15b; 15 mono; 15b mono (6) |
| `FACES_TEXT` (browser) | `userland/facetext.h` | regular and bold at 11, 12, 13, 14, 15, 16, 17, 19, 21, 24, 28, 34, 42; mono 12, 13, 14, 15, 17; bold mono 13, 15 (33, about half a megabyte) |

Face **indices are not portable** between kernel and ring 3: kernel `FACE_BODY_BOLD` = 4, while ring 3
`UI_FACE_BOLD` = 3. `tools/harness.py` hard-codes the kernel order to compute text widths.

### 6.3 Roles
| Role | Kernel | Ring 3 |
|---|---|---|
| counts, small print | `FACE_SMALL` 13 | `UI_FACE_SMALL` 13 |
| body text, unfocused titles, chips, icon labels | `FACE_BODY` 15 | `UI_FACE_BODY` 15 (all widgets) |
| focused window title | `FACE_BODY_BOLD` 15b | -- |
| section headings | -- | `UI_FACE_BOLD` 15b (`ui_section`) |
| clock (modern) | `FACE_HEAD` 20 | -- |
| "zelr" badge on the dock | `FACE_HEAD_BOLD` 20b in accent | -- |
| terminal, editor | -- | `UI_FACE_MONO` 15m (`MONO_H` = size + 3) |
| boot console, System info, parts of Settings | 8x16 bitmap `font8x16` | `font.h` (only settings.c and css.h still use it) |

Baseline rule everywhere: `y` is the top of the line, baseline = `y + size*4/5`. A coverage of 255 paints
the colour; anything less blends with what is underneath.

---

## 7. Layout and spacing

### 7.1 Kernel chrome geometry (defaults; most are knobs)
- Window: title bar 32 (`title_h`), frame 1 (`border`), buttons 30×24 (`button_w`/`button_h`) with a 2-px
  gap, placed right to left (close, max, min); marks are 10 px (`MARK`); grip 16×16 bottom-right.
  `WM_TOP = border + title_h`.
- New program windows cascade: `x = wm_icons_right() + 14 + 48*step`, `y = 36 + 38*step`, `step = handle % 5`,
  clamped to the screen. Program surfaces are 32..1600 × 32..1200.
- Minimum sizes: 120×60 through `wm_resize`, 160×80 for a pointer resize or after a screen change.
- Snap zones are 12 px from the top (full), left and right edges. The snap preview is 3 nested accent frames.
- Dock: height 44 (`dock_h`), 14 above the bottom edge (`dock_gap`), 16 from the sides (`dock_side`), radius 14.
  Badge 76 wide at dock_x + 16. Chips: text width + 22, capped at 160, 6 apart. Tray from the right: clock
  (right − 20), speaker (30 wide, 16 left of the clock), network (26, 6 left), find (26, 6 left).
- Popups float 8 px above the dock, clamped to `[gap, W − gap]`: volume 208×40 with a 136-px track; network
  268×150 with a full-width "ask for an address" button 26 tall; find bar 38 tall, 260 wide (`dock_search_w`),
  right-aligned.
- Launcher: rail 132, pane 152, width 304, item height 32, pad 10; 6 rows gives a height of 212, shrinking to
  the result count while filtering. Context menu: 204 wide, 30 per item, pad 8, 6 items, with a separator
  above "Personalise".
- Desktop icons: tile 32 (`icon_size`), room 30 (`icon_gap`), cell 92×88, from (18, 20), labels 16 below the
  tile (modern).

### 7.2 Ring 3 metrics (ui.h:25-31, 734)
`UI_PAD 8`, `UI_GAP 6`, `UI_ROW 26`, `UI_BTN_H 30`, `UI_TITLE_H 28`, `UI_RADIUS 6`, `UI_SCROLL_W 10`,
`UI_MENUBAR_H 20`; toggle 40×20; scrollbar thumb at least 20 px; the field caret blinks every 30 ticks.

---

## 8. Iconography
- **Desktop pictograms** (wm.c:913-1055), each with modern and built variants and every coordinate in a
  32-unit square through `IS(v) = v*tile/32`:
  - Terminal: dark tile, green chevron stepped a pixel at a time, and an underscore.
  - Files: two-tone folder.
  - Notes: page with five lines.
  - Paint: palette with five wells.
  - Settings: three rails with accent knobs.
- **Tray glyphs** are 16×16 digit maps where each digit is a part number, so one map draws several states:
  speaker (cone, near wave, far wave), mute (cone and cross), signal (4 arcs over a dot), plug. A static assert
  checks each is 16×16. The magnifier is plotted pixel by pixel.
- **Network icon, 3 states**: has an address → `text`; link only → `text_dim`; nothing → `text_mute`. It shows
  the plug glyph when the link is up and faint arcs otherwise.
- **Pointer**: a 19×12 two-colour bitmap.
- **Playing cards** (`userland/cards.h`): pips are discs and triangles sized from the card.

---

## 9. Motion
- Durations scale with `anim_ms` (default 120): `anim_scale(ms) = ms*anim_ms/120`. MENU_MS 200, HOVER_MS 110,
  PANEL_MS 170.
- Easing: `ease_out(p) = 256 − (256 − p)³/256²`, progress in 1/256. With `animate` off every phase is complete.
- What moves: the dock slides in and out (tuck-away), the launcher rises 8 px, the context menu rises 6 px.
  Hover "fades" are really a delayed switch at phase > 128, which the knob label "Fade rather than snap"
  overstates.
- No fixed frame rate: the loop composites on change and otherwise halts until the next interrupt. Moving
  wallpapers repaint about 12.5 times a second while nothing covers the screen.
- The dock tucks away (`dock_hide`: 0 never, 1 when a maximised window exists (default), 2 always). With the
  pointer at the bottom 3 px it comes out; it stays while the pointer is within 6 px of it.

---

## 10. Wallpapers (12; enum `wallpaper_t`, theme.h:13-27)

| # | name | moves | what it is |
|---|---|---|---|
| 0 | PLAIN | | flat desktop colour |
| 1 | GRID | | 32-px lines in `paper(14)` |
| 2 | DOTS | | 2×2 dots every 28 px |
| 3 | GRADIENT | | `paper(22)` → desktop |
| 4 | STARS | yes | 240 hashed stars drifting at 3 speeds, every 9th larger |
| 5 | WAVES | yes | 5 sine lines tinted toward the accent |
| 6 | WEAVE | | diagonals both ways every 24 px |
| 7 | AURORA | yes | 3 accent bands blended over 17 rows, drifting |
| 8 | RAIN | yes | 170 fading accent streaks |
| 9 | ORBS | yes | 5 soft discs on two sines |
| 10 | PULSE ("rings") | yes | 7 concentric triple rings from the centre |
| 11 | **BLOOM (default)** | | dark gradient, two glows and a core, vignette |

Curves come from `SINE_Q[17] = {0,24,49,73,97,120,142,163,181,198,212,224,233,240,245,247,248}` (a quarter
wave; `sine64` gives a 64-step period). The README says "eleven wallpapers, six of which move"; the code has
twelve (BLOOM was added later).

---

## 11. Components

### 11.1 Kernel-drawn (wm.c)
Window chrome, dock (badge = launcher button, window chips, tray: find, network, speaker, clock), launcher
(6 categories: Productivity, Internet, Media, Games, System, Session; 15 items; substring type-to-filter,
up to 4 results, "nothing by that name"), context menu (Open terminal, Browse files, Select all,
Personalise, Close all windows, System info), volume popup, network popup, find bar ("%d to look in",
"not on screen", "%d of %d", "find on screen"), snap and resize previews, selection band, desktop icons,
pointer, and the System info window (`apps.c`).

### 11.2 Ring 3 widgets (ui.h)
`ui_button`, `ui_button_primary`, `ui_row` (selected = `soft` fill + 3-px accent bar; returns 2 on right
click), `ui_label`, `ui_dim_label`, `ui_section`, `ui_toggle`, `ui_slider`, `ui_scrollbar`, `ui_field` /
`ui_field_key` / `ui_field_draw`, `ui_well`, `ui_toolbar` / `ui_toolbar_gap`, `ui_menubar` (File, Edit,
View, Help as in the file manager), `ui_statusbar`, `ui_menu`. Widgets act on the frame the button is
*released* inside them and consume the release, so overlapping widgets cannot both take a click. Input goes
through `ui_input` built by `ui_feed` from `win_event`s. `ui_menubar`, `ui_row`, `ui_toggle`, `ui_slider`,
`ui_scrollbar` and `ui_menu` have no modern branch and draw the same in both looks.

---

## 12. Settings as design tokens

### 12.1 The 31 knobs (theme.c:88-131; key, label as shown in Settings, range, default)
| key | label | range | default |
|---|---|---|---|
| dock_h | Height | 20..120 | 44 |
| dock_gap | Gap below it | 0..80 | 14 |
| dock_side | Gap at the sides | 0..400 | 16 |
| dock_radius | Corner | 0..40 | 14 |
| dock_brand | Show the name | bool | 1 |
| dock_search | Show find | bool | 1 |
| dock_search_w | Find bar width | 120..700 | 260 |
| dock_clock | Show the clock | bool | 1 |
| clock_24 | Twenty four hour | bool | 1 |
| dock_hide | Tuck away | 0..2 | 1 |
| title_h | Title bar | 16..64 | 32 |
| border | Frame | 0..8 | 1 |
| button_w | Button width | 12..60 | 30 |
| button_h | Button height | 10..48 | 24 |
| corner | Window corner | 0..24 | 12 |
| shadows | Shadows | bool | 1 |
| snap | Snap to the edges | bool | 1 |
| desk_icons | Icons on it | bool | 1 |
| icon_size | Icon size | 16..64 | 32 |
| icon_gap | Room around one | 8..60 | 30 |
| vignette | Darken at the edges | 0..255 | 150 |
| glows | Lights in it | bool | 1 |
| wallpaper | Wallpaper | 0..11 | 11 (BLOOM) |
| animate | Fade rather than snap | bool | 1 |
| anim_ms | How long a fade takes | 20..600 | 120 |
| dblclick_ms | Double click within | 150..1200 | 500 |
| quirks | Shake to clear | bool | 1 |
| autodesktop | Desktop at boot | bool | 1 |
| volume | Volume | 0..100 | 70 |
| width | Screen width | 0..4096 | 0 (boot mode) |
| height | Screen height | 0..4096 | 0 |

Plus 8 non-knob keys: `accent`, `desktop`, `surface`, `text`, `text_dim`, `look`, `preset`, `light`
(39 recognised keys). Out-of-range values are clamped.

### 12.2 File format and semantics
Plain `key value` lines. `#` starts a comment; tabs are not separators; keys and values keep at most 31
characters; the parser reads at most 2047 bytes and the writer produces at most 1536. **Reload is
incremental**: keys absent from the file leave the current value alone (the harnesses rely on
`write /zelr.cfg wallpaper 4` changing only the wallpaper). `theme_save` writes 4 comment lines, `look`,
`light`, then `preset N` if the accent matches a preset (else the five colours), then all 31 knobs.

---

## 13. Voice and copy
- Plain, lower-case, literal labels: "ask for an address", "asking...", "nothing by that name", "not on
  screen", "find on screen", "Leave desktop", "Shut down", "Shake to clear", "Darken at the edges", "Room
  around one", "Twenty four hour". Numbers are spelled out in labels ("Twenty four hour").
- British spelling: colour, Personalise, maximised, centred.
- States say what is true and what to do: "no driver" names the controller it found; "nothing can be reached
  without one" follows "no address".
- The same voice runs through code comments (explain *why*, not *what*; no em-dashes; plain words), commit
  subjects ("A frame that was finished, rather than one that was being drawn") and the README. The pipeline
  prompts enforce it for agents.

---

## 14. Known design-system bugs and drift
1. `theme_save` writes only `preset N` whenever the accent matches a preset, dropping hand-set desktop,
   surface and text colours. It runs on every dock volume change (theme.c:512-540, 208-212).
2. Inconsistent fallback when the accent is custom: a `look` line re-applies preset 1, a `light` line
   preset 0 (theme.c:397, 405).
3. Hover "fade" is a threshold, not a fade (wm.c:1953, 1980, 2031, 2090).
4. `tools/defaultcheck.py` passes without comparing anything: its regexes no longer match settings.c
   (atlas 07 §10), so kernel/Settings default drift is unguarded.
5. The modern values of `title_*`, and `raised`/`hairline` entirely, are computed but never drawn by wm.c.
6. The dock chip hit test measures the focused chip in bold while drawing uses regular weight.
7. README drift: says 11 wallpapers (12), says the built panel sits flush at the bottom (it floats by
   `dock_gap`), says "window chrome gets four [sizes]" (kernel ships 8 faces, ring 3 ships 6), and says page
   text uses `face.h` (the browser uses `facetext.h`).
8. Face indices differ between kernel and ring 3, a trap when porting drawing code between them.
9. Ring 3 reloads `/zelr.cfg` and `/sys/theme` every frame in most programs, costing two file reads per frame.
10. A volume change written to `/zelr.cfg` never reaches the sound driver (atlas 07 §10).
11. On 1920×1080, maximising a program window fails silently (width over 1600) but the window is still
    flagged maximised, so the dock tucks away.

---

## 15. How to extend it
- **A new numeric or switch setting**: add the field to `theme_t` (theme.h) and one `KNOBS[]` line (key,
  label, `K(field)`, is_bool, lo, hi, def). The parser, writer, defaults, `/sys/settings` and Settings'
  Everything page follow automatically. Keep lo < hi (setcheck asserts it). Read it through `theme()`.
- **A new colour token**: derive it in `derive()` from the five inputs rather than adding a file key. Mirror
  it in `ui_load_theme()` if ring 3 needs it, and remember the two derivations are independent.
- **A new wallpaper**: extend `wallpaper_t` before `WALLPAPER_COUNT`, raise the knob's `hi`, draw it in
  `draw_wallpaper()` within the cost budget, list it in `wallpaper_moves()` if animated, and update Settings'
  wallpaper names.
- **A new face size**: edit the set in `genface.py` (`FACES_KERNEL` / `FACES_USER` / `FACES_TEXT`) and
  regenerate. Then fix `FACE_*` in `include/gfx.h`, `UI_FACE_*` in `userland/draw.h`, and the hard-coded
  order in `tools/harness.py`, because "the order changes whenever a size is added".
- **A new icon**: a function drawing in a 32-unit square through `IS()`/`IS1()`, with both a modern and a
  built branch.
- **A new widget**: follow `ui.h`'s pattern (act on release, consume the event, draw both looks from
  `ui_theme`).
