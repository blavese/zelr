# 11 -- The web browser (everything except JS-engine internals and image decoders)

**Since 0.149.0** (the rest of this file is older; trust the code and the README's "since 6048716" list):
- **Percentage heights through inlines** (css.h `cstyle.cb_h`, set in layout.h `lay_style`): each style carries the
  height its children's percentages would be of were it an inline; an inline or display: contents parent with no
  height hands that on, so a block or picture inside a link takes its percentage of the box around the link. A flex or
  grid item (which says inline but is a box) hands on -1.
- **A stretched flex item's height is known before its contents** (lay_flex_line): in a single-line row with a height
  or a least height (`line_h`), an item that stretches and has no height or max-height of its own is laid out at the
  line's height less its margins through `force_node`, so a percentage inside it resolves. Wrapping rows and grid items
  are still stretched afterwards (lay_stretch).
- **text-overflow: ellipsis on a word alone on its line**: the word itself is cut to fit with its dots (it had no word
  before it to take them, and vanished).
- **`slot[name=x]::slotted(y)`** is scoped as `slot[name=x][data-zs=N] > y[data-zl=N]` (css.h `css_scope_compound`):
  what is put into a slot is the slot's child in the page as laid out (dom_shadows, dom_flat). The slot part was
  dropped, so every slot's rule applied to all of them.
- **Absolute boxes**: both sides, a width and auto margins share what is left (margin: 0 auto centres); a box with a
  width of its own is given room for its margins and is placed by its margin box at `right:` (`hmargins`).
- **The static position in a row** (layout.h `lay_flex_static`): an absolute child of a flex container with no offset
  on an axis is moved where align-items/align-self and justify-content put a sole item (across only when the row's
  height is set). Its box and contents are shifted after it is laid out (`L->box_node`/`box_w`/`box_h`).
- **A drawing written `width="0"` / `height="0"`** keeps its nought (lay_hints): a symbol sheet in a row was 300 by 150.
- **aspect-ratio: inherit** (`cstyle.ratio_up_w/_h`), and **an aspect-ratio box's height is set from its width before
  its contents are laid out** (lay_block_placed, not while measuring and not for tables): percentages inside resolve,
  and a box hiding its overflow keeps the ratio's height.
- **Backgrounds partly see-through are drawn over what is under them** (css.h `cstyle.bg_a`, `bg_raw`; layout.h
  `litem.bgt`; browser.c `see_through_rect`). `cstyle.background` is still the colour laid over the parent's, for what
  is drawn on the box (a word's or a drawing's backdrop); the box item carries its own colour and `bgt` = 255 - alpha.
  Masked boxes keep the mixed colour. browsercheck's /glass counts purple over a red band and lavender over the page.

**Since 0.148.0**:
- **Sheets up to 4 MB** (browser.c `CSS_MAX`; `sheet_abs` grows to the largest sheet read; css.h `CSS_FLAT_MAX` 5 MB).
  Ubuntu's sheet is 1.1 MB and GitHub Docs' 2.4 MB. browsercheck's /bigsheet reads a rule at the end of 1.2 MB.
- **XPath** in jsprelude.h (script): document.evaluate, createExpression, createNSResolver, XPathEvaluator,
  XPathExpression, XPathResult; XPath 1.0 paths on every axis, tests, predicates, operators and core functions; no
  variables or namespaces. htmx 2 needs it as it loads.
- **Speed** (CNN 27 s to 10 s on the host): a custom property's lookup is kept for the layout (layout.h `lay_vcache`,
  keyed by chain and name; chains and `lay_gen` start together in lay_run); each element's rules are kept across
  layouts while browser.c's print of what matching depends on (page, jsdom tree and sheet versions, sheet print and
  size, drawn document, width, hover) is unchanged (`lay_hits_keep`, `lay_hit_epoch`; anything that empties the
  pool calls `lay_hits_forget`, as style_in_drawn does); classes hash into 8192 chains (`CSS_BUCKETS`; `css_tries`
  counts tries for tests); ancestor bits are 512 (`CSS_BLOOM_W`, `css_bloom_lacks`).

**Since 0.147.0**:
- **Floats**: the width a float is placed at keeps to its max-width and min-width (lay_float, `wframe`); a box whose
  parent is a flex or grid container is never floated (lay_block `in_row`; reached through display: contents it is
  judged by the contents element).
- **Custom properties set to a keyword** (layout.h `lay_var_keyword`, `lay_var_entry`, `lay_var_push_own`): inherit or
  unset takes the parent's entry when gathered, passing over the element's own; initial (or inherit with nothing to
  inherit) is none, so var() takes its fallback. Tailwind 4 sets `--tw-*: initial` on every element.
- **line-height** in rem/vw/vh is a length (converted to a share of the font size); calc()/min()/max()/clamp() are
  worked out, a number a share, a length a length (css.h P_LINE_HEIGHT).
- **text-indent is inherited** (css_inherit), so text hidden by -9999px on a link reaches the heading inside it.
- **An absolute box against the window** (no positioned ancestor, `!L->pos_id`) with a top and a bottom and no height
  is laid out at `css_view_h` less both and its margins (force_node); a fixed one is not. A forced height its box did
  not take is let go after lay_block_placed.
- **SVG arcs**: the two flags are one digit each (svg.h `sv_flag`), so "a.5.5 0 01.016.707" reads as icon tools write it.

**Since 0.146.0**:
- **!important**: a declaration keeps a mark (css.h `CSS_IMPORTANT`, `CSS_PROP` strips it), and `lay_style` applies
  the marked ones after the rest in each of its two passes (font sizes, then the rest), in the same source order
  (browser, page, style attribute; `imp_pass`, skipped unless `saw_important`). The layer order is not turned
  round for them, the browser's are not put above the page's, and custom properties ignore the mark.
- **CSS nesting** (css.h `css_flat`, called by `css_parse_in` and `css_parse_style`, which parse with
  `css_parse_flat`): a sheet with a rule inside a rule's block is written out flat into `css_flat_buf` (3 MB) before
  it is read; a sheet that nests nothing is read as written. A nested selector is joined to each parent selector
  (& replaced; none means a descendant, or the leading combinator's relation), past 16 parents as `:is()`.
  Declarations after a nested rule become a rule of their own after it; @media/@supports/@layer/@container inside
  a rule hold its declarations. Twelve levels; & takes each parent's own weight, not :is()'s greatest.
- **Media queries**: a feature beginning `-ms-` or `-moz-` makes its part false, "not" or not (`foreign`); widths
  are worked out with calc()/min()/max()/clamp() at an em of 16 (`css_mq_px` through `css_len_at`).
- **Measuring**: an inline-block with no width, laid out in the room left on a line being measured, goes to a new
  line and is laid out again when it comes out wider than that room and something is already on the line (lay_inline_piece,
  `relay`); a flex row measured with no width of its own gives its line the width of its items (`measured` in
  lay_flex), so nothing grows or is spread into the room it was measured in.
- **A nowrap row that would scroll** is still wrapped, unless it is fixed to the window (lay_inline_piece `nobreak`).

**Since 0.145.0**:
- **A fixed box with only a bottom** is moved to stand that far above the foot of the first screen (`css_view_h`)
  once its height is known (lay_block's positioned branch). Fixed boxes still scroll with the page.
- **Long words** (lay_word): a word wider than a whole line is broken at the last letter that fits when
  `overflow-wrap`/`word-wrap` break-word|anywhere or `word-break` break-all|break-word say so (`cstyle.wbreak`), or
  when it would run past the page's side (`css_view_w`); never in nowrap/pre, never while measuring.
- **letter-spacing** (`cstyle.lspace`, px, -20..40, inherited): `lay_word_w` adds it after each letter, `litem.ls`
  carries it, browser.c `tface_draw_sp` draws it. pre text, ellipsis dots and list numbers are measured without it.
- **Columns** (`col_n`, `col_w`; column-count, column-width, columns): the contents are laid out one column wide and
  dealt by `lay_columns` at line starts after equal shares of the height. No balancing beyond that, no column-span
  or column-rule; measured as one column.

**Since 0.144.0**:
- **The two-word display** (`inline flex`, `block grid`, `inline flow-root`...) is read by both words (css.h P_DISPLAY).
- **Opacity** is read as a number (percentages too) into `cstyle.op_hidden`, and `lay_style` makes the element
  hidden only after every rule (a later opacity undoes an earlier one; below a fifth hides, the rest is drawn solid).
- **A box whose caller settled its width from its own width in pixels keeps it when measured** (lay_block_placed
  `own_w`); it was shrunk to its contents, since `flex_sized` makes `want` -1.
- **A box in the flow that hides its overflow and has a height keeps it**: what lies wholly below its foot is dropped
  and boxes running past are cut, as the sides are; a box that shows its overflow still grows.
- **A nested `<svg>`** is a viewport (svg.h `sv_viewport` from `sv_transform`): x/y, viewBox fitted into width and
  height, meet and centred. Not clipped to its place.

**Since 0.143.0**:
- **The page's `<html>` is the document's root** (dom.h `dom_parse_into`): its attributes go to the root made at the
  start, while the root's attributes can still grow in place (before any other element has any). It was a second html
  element inside the root, which the layout never styles (it starts at the body), so rules and custom properties on
  `html.x` or `html[data-...]` were lost. An `<html>` in a fragment is still made an element.
- **display: inherit** takes the parent's display (`cstyle.display_up`); initial and unset are inline. Other unknown
  words are still block.
- **A media query's width adds the page's scroll bar** (css.h `css_mq_extra`, set to `UI_SCROLL_W` by the browser):
  the sheets' rules, matchMedia and innerWidth; not the layout width, vw or clientWidth. **min-height/max-height**
  are read against `css_view_h` when the query is read (a sheet is read again per page, not per resize).
- **A picture element's `<source>`s** (browser.c `pic_from_sources`): the first before the img whose media holds and
  whose type is drawable, its srcset's 1x (or bare, or first) candidate (`srcset_pick`; an address runs to white
  space, commas inside it allowed).
- **TREES_MAX 8192** in browser.c, matching jsdom's `JD_SHADOWS`: past 1024 a shadow tree was kept by jsdom but not
  drawn (Rotten Tomatoes' header).

**Since 0.142.0**:
- **A top or bottom in a percentage** is of the containing box's height: resolved at once when the parent has a height
  (`css_parent_h`), else marked (`cstyle.off_pct`) and left auto, and the box is laid out again when the positioned
  box's height is settled (lay_block's relay, now also for `off_pct`). Only direct children of the positioned box.
- **A float met after words on a line** (a floated block in the block walk when the line has something on it,
  `lay_floated_here`; or one written inline) is laid out from the line: its items are a pinned group, which
  `lay_line_end` leaves alone (it had moved them with the words' alignment and set every line of them on the
  baseline), and `lay_line_beside_float` shortens the line beside it, moving the words when the float is on their
  side, or moves the float below the line when the words do not fit beside it. Measured, a right float met after
  words follows them (`lctx.float_pen`). A float's bottom margin is counted once in `fl_h` (a box's layout already
  ends past it; a picture's does not).
- **Inline boxes with position: relative** are moved by their offsets once the block holding them has laid out its
  lines (`lctx.rel_*`, `lay_rel_open`/`lay_rel_close` from the inline stack, `lay_rel_apply` in lay_block_placed),
  background and all; nothing round them moves.

**Since 0.141.0**:
- **A float's percentages are of the room it was floated in** (`lctx.pct_node`/`pct_w`, read by `lay_style`): laid out
  in the room `lay_float` decided, margins included, they were worked out again from that room (a slide of width 100%,
  margin-right -100% came to 0 wide). **A float is a formatting context of its own** (`lay_block_placed`'s
  `own_floats`): its contents see only floats placed inside it (`fl_floor`) and those are dropped after it, as a
  clipping box's are. Absolutely positioned boxes and inline-blocks are not treated so yet.
- **Font sizes**: `cstyle.font_up` is the parent's size, which em, %, smaller, larger, inherit and unset of a font size
  are of (they were of whatever an earlier rule had made the element's: the browser's h1 2em doubled a page's em
  size). **`lay_style` applies the cascade twice** (`lctx.font_pass`): font sizes first (the sheets', `<font size>` in
  `lay_font_hint`, the style attribute), then everything else, so every em of a length is of the final size. A
  shorthand split late (`P_DEFER`) runs in both passes: `font` gives its size in the first (`lay_font_put`), the rest
  in the second. Anything new that sets a font size must be a `P_FONT_SIZE` declaration or run in the first pass.
- **Units** (css.h `css_len`): ch 0.55em, ex 0.5em, svh/lvh/dvh/cqh/cqb as vh, svw/lvw/dvw/cqw/cqi as vw, vmin/vmax by
  the window's shape, in/pc/cm/mm in pixels. lh and svmin and the like are still pixels.
- **The plain page's dressing** (css.h `CSS_UA_PLAIN`, `csheet.plain_at`/`plain_end`): pre's grey card and
  blockquote's side rule are parsed between the browser's sheet and the page's and skipped by `lay_style` when the page
  has rules of its own; a blockquote there is margin 0.9em 40px. A new parser of `CSS_UA` must parse it too.

**Since 0.140.0**:
- **line-clamp** (css.h `P_LINE_CLAMP`, `cstyle.line_clamp`; lay_block_placed): lines past the Nth are put away, the box
  ends after them (no shorter than its height or min-height), and the last word shown takes "..." (the face is ASCII).
  **text-overflow: ellipsis** (`cstyle.ellipsis`): in the side cut of a box hiding its overflow, words running past the
  side go too and the line's last word is cut to fit the dots.
- **A button's label** (`lay_words_of`) has each run of white space made one space and none at its ends.
- **Measuring floats**: while measuring, a right float sits at the left; a trial sees only floats placed during it
  (`lctx.fl_floor`, raised by `lay_measure`; anything new that walks the floats must start at `lay_fl_first`); a float
  with a width in pixels keeps it (`flex_sized` only for auto or percentage widths).

**Since 0.139.0**:
- **A fixed box's containing box is the window** (`lay_block`'s absolute branch: `cbx`/`cby` 0, `cbw` `css_view_w`), not
  the nearest positioned box; one with only a bottom stays where the flow put it, and one whose height its box decides is
  not laid out again (the layout does not know the window's height). Transformed ancestors are not looked for; fixed
  boxes still scroll with the page.
- **A nowrap or pre line keeps its inline-blocks on it** (`lay_inline_piece`'s `nobreak`), except in a box that would
  scroll (`clip` 2), which is wrapped on purpose since nothing inside a page scrolls here; and a line that may not break
  and runs past its room counts as widened (`L->widened`, words and boxes alike), so a box hiding its overflow drops what
  lies wholly past its sides.

**Since 0.138.0**:
- **Stacking** (css.h `P_Z_INDEX`, `cstyle.z_set`/`zidx`; layout.h `lzctx`, `lay_z_enter`/`lay_z_leave`, `lay_z_order`,
  `litem.zc`/`zl`, `ldoc.zorder`/`zsorted`): a positioned box with a z-index, or an item of a row or a grid with one, is
  a stacking context; a positioned box with none is a pseudo context (drawn as one piece with its flow, its positioned
  insides going to the context round it, `lctx.z_real`). Each context draws its own background (`ZL_BG`), contexts
  below 0, its flow, positioned boxes and contexts at 0 in the order written, contexts above 0. browser.c `draw_page`,
  `lay_link_at` and `lay_node_at` follow `zorder` when `zsorted`; a page with nothing positioned is drawn as laid out.
  Anything new that draws items or finds what is under a point must go through `zorder` too. opacity, transforms and
  isolation make no context; fixed boxes are not fixed to the window. Positioned boxes drawn on top now show what the
  layout fails to hide (a closed menu it cannot tell is closed): look there first when a page gains a panel over it.
- **A rem is the html element's font size** (`lay_run` sets `L.root_px` from the root's style; browser.c
  `style_in_drawn` too): `html { font-size: 62.5% }` or `10px` made every rem 1.6 times too big.
- **An absolutely positioned or fixed box with a height is that high** (lay_block_placed, after its floor): a box in the
  flow still grows to what it holds, on purpose.
- **A box placed by its right side** is placed by its width after max-/min-width and, for content-box, its frame; that
  width is its room, except that a percentage is given its box's width (`lay_block`'s absolute branch).
- **A translucent background** is laid over the parent's background (`background_up`), not over white.
- **A fresh style is content-box**: `css_default_style` never set `border_box`, so a box that said nothing about
  box-sizing took whatever the last style in that stack slot had. Every field of `cstyle` must be set there (scratchpad
  `audit_cstyle.py` lists any that is not).
- **Fields measured as Chromium measures them** (the browser's sheet): a text field's frame is four pixels a side
  (3px padding, 1px border; Chromium's is 2 and 2), and select and button-like inputs are border-box.
- **All 148 named colours**, as Chromium computes them; **a rule keeps up to 1024 selectors** (`CSS_LIST_MAX`; it was
  32, and the rest were dropped).

**Since 0.137.0**:
- **A drawing's root painted by the page's sheet** (css.h `P_FILL`/`P_STROKE`, `cstyle.fill`/`fill_said`, inherited;
  browser.c `drawing_of` works the root's style out with `style_in_drawn` and hands it to svg.h `svg_root_paint`, which
  `sv_inherit` takes for the root ahead of its attributes). Shapes inside keep their own paint; a sheet's paint for them
  is not read, nor url() paints.
- **What the layout puts away is not drawn** (browser.c `draw_page` passes over kind 0): text items cut by a box that
  hides its overflow, and the first layout of a box laid out again (0.134.0), were drawn as words all the same.
- **A box of no height that hides its overflow** is unseen only when its padding and borders above and below leave it 2
  pixels or less (`lay_unseen`); one whose own room is that small is cut to its height and frame (Debian's banner
  heading). A box 0 wide is unseen whatever its side padding: its words would be cut to the padding, which this layout
  cannot do (Forbes' carousel, made 0 wide by its script, drew every word down a column).
- **Colours a page takes back** (css.h `cstyle.color_up`/`background_up`, the parent's, set in `css_inherit`):
  `color: inherit`/`unset` take the parent's colour over the browser's link colour (BBC's links, and so its
  currentcolor logo), `initial` the first colour; `background: none`, a shorthand naming no colour (in the sheet and
  split late from a variable, the colour looked for in twelve parts), `initial`/`unset`/`none` and a transparent colour
  leave no background and put back the parent's as the backdrop drawings inside are made on. A shorthand over a gradient
  that is not drawn (radial, conic, repeating: `css_bg_undrawn`) keeps the colour before it. `revert` and
  `background: inherit` are not read.
- **color()** in srgb, srgb-linear and display-p3 (`css_colorfn`, `css_srgb_linear`; P3 through the specification's
  matrix, clamped to sRGB). Media queries answer no to `dynamic-range: high` and `color-gamut: p3`/`rec2020`
  (Substack swapped its greys for display-p3 ones on such a screen).
- **inline-size, block-size** and their min-/max- forms are width, height and their limits (w3.org's full-width hero,
  `inline-size: 100vw` between `margin-inline: -50vw`).
- **An unquoted attribute value keeps a last slash** (dom.h `bare_end`): `<a href=/>` was taken as closing itself, and
  the link was empty (Old Reddit's block page). `<div/>` still closes, as before.
- **oklab() and oklch()** (css.h `css_oklab`, with `css_root`, `css_srgb_byte`, `css_sincos`): Tailwind 4's palette.
  `css_supports` answers no for `color-mix()` and `gradient(in ...)`, so Tailwind's plain `#rrggbbaa` fallbacks stand
  (custom properties overridden by an unreadable color-mix() took colours away). lab(), lch() and color-mix() are not
  read; color() is, in three spaces (below).
- **padding-inline/-block and margin-inline/-block** (css.h `css_two_sided`, `css_two_sides`; deferred with var() and split
  in layout.h `lay_apply_short`, as inset-inline/-block now are too), and the eight single sides in the property table:
  Tailwind 4's px-/py-/mx-/my-. The start side is always left or top.
- **The sheet holds 32000 rules** (80000 selectors, 160000 declarations, 6 MB of text; zero data until used): GitHub's
  repository page sends about 25000, and the rest were lost.

**Since 0.136.0**:
- **Escapes in a selector's class and id names** (css.h `css_name_read`): a backslash takes the next character, or up to
  six hex digits and a space as the character they number. Tailwind writes every class with a colon, a bracket or a
  slash so (`.md\:flex`, `.w-\[37\%\]`); read up to the backslash, no responsive class ever matched. `css_list_end`
  and `css_scope_selectors` step over an escaped character. Names past ASCII are read but still do not match a page's
  class attribute (not looked into).
- **A centred column measures as wide as its widest item**: a column flex item placed at the centre or the end is not
  offset while measuring (`L->measuring`), as a grid's are not (0.135.0).
- **A row's item is never narrower than its min-width** (`mn`, frame and margins included, in lay_flex's gathering):
  both where it starts (`want`) and how far it shrinks (`low`).

**Since 0.135.0**:
- **row-gap apart from column-gap** (css.h `P_ROW_GAP`/`P_COL_GAP`, `cstyle.row_gap`): `gap` is the rows' then the
  columns', `grid-gap`/`grid-row-gap`/`grid-column-gap` too; flex lines down a column or wrapped and every grid's rows
  use `row_gap`.
- **Pictures follow the elements in the page** (browser.c `pic_in_page`, `pics_rehome`): gathering and following pass
  over img elements a script took out (`jd_connected_deep` when jsdom is open). A picture or an ask for an element taken
  out goes to an element put in at the same address; what nothing takes is let go, so the 128 places are the page's.
- **A box the page sized may be narrower than 16** (layout.h `lay_block`): the 16 pixel floor is only for a box with no
  width, max-width or flex size (`flex_sized`); its contents still get 16 (`cw`).
- **A flex row of one line is as tall as its height** within min/max (`lay_inner_height`, `lay_flex_line`'s `line_h`),
  so center, flex-end and stretch work against it; auto margins above and below an item (`amarg` bits 2 and 3) come
  before its align-self. Wrapped lines are still as tall as their items.
- **Grid items across their cells** (`lay_grid_across`): justify-items, justify-self, place-items and place-self are read
  (css.h `css_place_word`, `cstyle.justify_items`/`justify_self`, the align-items values). An item with a width keeps it,
  laid out in the whole cell (a percentage is of the cell) and moved to its middle or end, auto margins placing it
  otherwise; one set to start, center or end is as wide as its contents; a grid item's align-self is read. A grid being
  measured puts nothing in the middle or at the end. A plain grid whose items make one row stretches it to the grid's
  height.
- **An absolutely positioned box with no width** (and not both sides) is measured and given that width, up to the room
  (`lctx.abs_sizing`: the box being measured is laid out as its body), so right: 0 and translate(-50%) place it right.
  **It is no part of a measurement**: while measuring (`L->measuring`) its items are zeroed and `measure_right` put back,
  so a box sized by its contents is not widened by a positioned child.
- **A unit that is not a length** (fr, s, ms, deg, grad, rad, turn, dpi, dpcm, dppx, hz, khz) makes `css_len` answer
  auto, as a browser takes such a width; grid tracks cut their `fr` off before reading the number.
- **unset, initial and revert** (css.h `css_reset_word`) put grid-template-rows/-columns/-areas back to none and
  grid-auto-rows/-columns to auto; kept as a list, `unset` was one row 0 pixels high.

**Since 0.134.0**:
- **`:is()` and `:where()` with lists** (css.h `css_expand_list`): a selector list holding one is written out as one
  selector per choice before it is read, each choice in its own single-compound `:is()`/`:where()` so it weighs what
  the standard says; a choice more than a compound is written bare where it stands alone, dropped inside a compound;
  32 at most. The sheet (css_parse_in) and a script's querySelector (jsdom.h `jd_sel_open`) both read through it.
- **What fills a box by its height** (layout.h, `rl_*` in lctx): an absolutely positioned child of a positioned box
  with a percentage height (`cstyle.height_pct`) or a top and a bottom and no height is noted, and when the box's
  height is settled its first layout is put away (kind 0) and it is laid out again at the end of the list with that
  height (`force_node`/`force_h` for top and bottom). The aspect box: `::before { padding-bottom: 56.25% }` and the
  picture `inset: 0`.
- **Bookmarks** (browser.c `bm_*`): ctrl+D keeps or lets go the page in `/home/bookmarks` (address, tab, title, a
  line each); the start page lists them where `<!--bookmarks-->` stands and its status counts them.

**Since 0.133.0**:
- **Pictures**: 128 a page (`PICS_MAX`), each kept at no more than `PIC_KEEP` (1280) a side (`pic_shrink`, blocks
  averaged with the alpha plane); `shown.nat_w/nat_h` is the natural size, which `pic_sizes` (the layout),
  `picture_size` (naturalWidth), object-fit (`pic_natural`) and the canvas's drawImage use. No budget across a page.
- **The clipboard**: the toolkit's fields take ctrl+V/C/X (ui.h `ui_field_key`, `ui_field_paste`), a `secret` field
  is never copied (a page's password field); ctrl+L goes to the address bar; a paste into a fresh bar replaces it.
- **Tab** gives the keyboard to the next text field, checkbox or radio drawn after the focused one (`tab_next`).
- **`content-visibility: hidden`** (`cstyle.cv_hidden`, lay_block lays out no children) and `[hidden=until-found i]`
  given it in the browser's sheet instead of display:none.
- **Quoted strings** are passed over in declarations and skipped at-rule bodies (css.h `css_past_string`):
  Tumblr's `content:":("` ate two thousand rules.
- **Absolute positioning**: a box with only a bottom is noted with its positioned box (`lctx.pos_id`, `bot_*`) and
  moved once that box's height is settled at the end of its lay_block; against the page it stays where the flow put
  it. A positioned picture, drawing, video or field takes the positioned layout (`lay_positioned_picture`, from the
  block walk and the inline walk) and is not squeezed to the line. `inset-inline-start/-end`, `inset-block-start/-end`
  and the `inset-inline`/`inset-block` shorthands are read as left/right/top/bottom (left to right only).
- **A file and not a page** (browser.c `body_kind`): an attachment or a type that is not a page, text, JSON, a picture
  or an MP4 is saved in `/home/downloads` (`download_name`, `save_download`, read back before it is said), the console
  told "saved PATH, N bytes, read back the same"; a picture's own address is shown on a page of its own and an MP4's
  played (`<video autoplay>`). Nothing past the 8 MB the page is read into.

**Since 0.132.0**:
- **Keys go to the page first** (browser.c key loop; jsdom.h `jsdom_key_down`, `jsdom_key_up`): keydown at the element
  with the keyboard (the body when none has it), keypress for a character or Return, keyup after the browser's own
  action, with key/code/keyCode/which/charCode for a US keyboard (`jd_key_info`). A refused keydown or keypress keeps the
  key from the browser. **The keyboard is shared**: a control the reader clicks or leaves tells the page
  (`give_focus` -> `jsdom_focused`), and a field the page focuses takes what is typed next (`follow_page_focus`, after
  each key and once a pass; never while the address bar is typed in). There is no separate key release, repeat, alt,
  meta or Tab between fields.
- **Background pictures are gathered after every layout** (`relayout` ends with `gather_backgrounds` when
  `pics_follow_on`), so a background a script sets later is fetched.
- **Grid**: a span on an item's end line alone is kept when it is flowed (`lay_grid_place` returns the span either
  way); row lines are resolved against the rows grid-template-rows writes out (`lay_grid_explicit_rows`), so
  `grid-row: 1 / -1` spans the card.
- **Selectors**: a second `:not()` on a compound extends the first one's list (css.h, the contiguous `negs` range).

**Since 0.131.0**:
- **Carousels** (layout.h `lay_block`): inside a box with `overflow: hidden` a box is as wide as it asks (`overflows`,
  counted in `lctx.widened`); a box holding one so widened drops items wholly left or right of it and cuts LK_BOX
  items at its edges (only then: elsewhere a box is placed after its children and the cut would land wrong). A clipping
  flex row asking twice its room or more is given it too (SoundCloud's, overflow on the row itself), its slides past
  the window's edge. **A box with `overflow: hidden` or `flow-root` keeps its floats** (`fl_seq` put back to
  `floats_at` once it is laid out): Debian's floated slides reached into the next column.
- **`flex-flow`** is read (css.h `css_flex_flow_split`, sheet and late split).

**Since 0.130.0**:
- **Icon sprites in files of their own** (browser.c `sprites_follow`, `sprite_take`, `sprites_into`): a `<use>` naming
  `file.svg#id` has the file fetched as a waiting picture whose `bg` is -2 less its slot (8 a page), kept as markup, and
  read into the copy of the page that is laid out (`page_drawn`, which now makes the copy when a sprite has come) under
  `<head>`, ids prefixed `zs<slot>-`, each use pointed at its prefixed id. **Inline drawings are made from what was
  laid out** (`drawn_from`, `laid`), not `doc`. Sprites are gathered at build and after each layout.
- **`display: inline-table` is an inline-block** (css.h); a table element is laid out as a table whatever its display.
- **`CSS_NEGS` 16384** (css.h): the insides of every `:not()`; CNN's sheet has more than six thousand, and a selector
  past the old 2048 was dropped whole.
- **The font shorthand** (css.h `css_font_split`, used by the sheet and by layout.h `lay_apply_short` for one with
  var()): a bare number is a weight (600 and up bold), the size needs a unit, a percentage or a keyword, a line height
  after a slash, then the families; weight and style it does not name go back to normal; `0/0` is a size. font-size
  knows its keywords (`css_size_keyword`).

**Since 0.129.0**:
- **White space is in the tree as written** (dom.h `dom_parse_into`): every run, in the page, innerHTML and template
  contents, with its own text (it was left out at an element's start and after a block, and kept as one space
  elsewhere). Svelte's templates hold a space node for each piece of text; crates.io drew nothing without it. **Layout
  passes over what separates nothing** (dom.h `dom_passed_space`, called by layout.h's block walk and `lay_inline`):
  white space whose last thing before it, past comments and other white space, is a block element, or which starts a
  block element; at the start of an inline element it is a word space (python.org's `<span> </span>`). Anything new
  in layout that walks text nodes must ask `dom_passed_space`, or a cell holding only a space is a space wide
  (kernel.org's `<td> </td>`). `:empty` no longer matches an element holding only white space, as in other browsers.
- **`white-space`** (css.h `WS_PRE_WRAP`, `WS_PRE_LINE`; layout.h `lay_text_run`): pre-wrap keeps spaces and line
  breaks and wraps, pre-line keeps line breaks only; a value not known is dropped (it was normal, and python.org's
  `-moz-pre-wrap !important` undid its pre-wrap).
- **The title is shown with its white space collapsed** (dom.h `dom_collapse`, browser.c `title_take`, jsdom.h
  `nat_doc_title`): dictionary.com's began with a line break and split the console line in two.

**Since 0.128.0**:
- **`DOM_NODES` 80000, `DOM_ATTRS` 160000** (dom.h): Wikipedia's United States article was cut off at forty thousand.
  The tables sized by them are zero data, paid for only as used now (03).

**Since 0.127.0**:
- **`ahead_take` gives up on a file not yet asked for after 200 ms when none of the list's own are on their way**: the
  asks may all be the page's requests, stepped only between passes, and a module's imports found inside `import()`
  waited for ever (Mastodon's columns). Behind the list's own it waits its turn.
- scriptdump's wait calls `sheets_follow_if_changed` each pass, as the browser's layouts do, so links a script adds are
  told load (Vite's preload of a chunk's sheets). Harness page `/dyn` (`DYN_OK`).

**Since 0.126.0**:
- **A script's or module's file is asked for again once** (browser.c `ask_again`, shared with sheets): no answer, cut
  short, 429 or 5xx; 429/503 after their Retry-After (half a second if none, two at most).
- **A module's imports are read ahead** (jsmod.h calls `jd_want_script` as each new import is found; browser.c
  `script_want` puts it on the `ahead` list, now 384 long), and `modulepreload` / `preload as=script` links join the list
  in `files_ahead`. Mastodon: 115 s of script time on the host to 6 s.
- **An empty sheet or script is a file** (gather_linked_sheets, `link_text`, `fetch_script` hands an empty script over as
  one space, since nought means it did not come).
- Harness pages: `/flaky` (a module refused once, `MODFLAKY_*`; `FLAKY_*` is the busy sheet's), `/mods` (`MDELAY_MOST`),
  `/empty` (judged by colour, `EMPTY_OK`: the console line is printed before a timer's title).

**Since 0.125.0**:
- **`LK_EDGE`** (layout.h): a block with no background or border gets an item of its own, made where its background
  would be and drawn as nothing, carried by every move of its contents (`lay_cell_fit`, `lay_translate`, flex
  stretch). browser.c `box_of` (getBoundingClientRect, offset/client sizes) and `lay_node_at` see it; `lay_link_at`
  follows a block link anywhere in it. Anything new that counts items or takes "the box" of a node must allow for it.
- **Custom properties for scripts**: browser.c `custom_style` finds a name in the chain `computed_style` leaves
  (`cstyle.vars`, `lay_var_find`), with var() put in (`lay_var_subst`).

**Since 0.124.0**:
- **dom.h reads `<?...>` and a `<!` that starts no comment or doctype as a comment** (HTML's bogus comment, the `?`
  kept): Lit finds where a template's values go by comments `?lit$...$`, and with none every value was lost.
- **A close tag's name ends at any white space or `/`**, not only a space: Lit writes `</a` + newline + `>`.
- With jsdom.h's template copy (12), archive.org's front page draws (179 items, where there was one).

**Since 0.123.0**:
- **A player from another site is a box that opens it.** The browser's rules no longer hide `iframe`; layout.h
  counts it with video and canvas (`lay_painted`) and `lay_video` lays it out only when `lay_frame_player` says its
  address names an embed or a player (a host starting `player.`/`embed.`, `w.soundcloud.`, `www.youtube-nocookie.`,
  or a path with `/embed` or `/player`), at its width and height (nought kept, so a counter's 0 by 0 is not drawn) or
  300 by 150, with a link of its own to its src. Every other frame is left out as before. browser.c
  `draw_frame_box` draws it dark with a play mark and "open the player from HOST"; `link_target` sends a frame's
  link through sites.h `site_frame_page` (YouTube's embed to its watch page, Twitch's player to the channel's or
  video's page). The frame's document is still not loaded or drawn. Tests: layouttest's player section, sitetest's.
- **A noscript is never laid out** (layout.h, after the cascade), whatever the page's rules say: PeerTube's sheet makes
  it a block and its raw markup was drawn.
- **The void elements with no tag number** (base, source, wbr, col, embed, param, track, area, keygen; dom.h
  `dom_void_name`) are no longer containers in `dom_parse`: a `<base>` took the rest of the head.
- scriptdump: "page nodes" on each script's line (the summary's "tree N nodes" is the script engine's syntax tree),
  "[N laid out]" in `tree`, and a `nodes` option (the page's nodes by kind, in the page and removed).

**Since 0.121.0**:
- **`<base href>`** (browser.c `base`, `base_take` in `build`; every `url_join` of what the page wrote is against
  `base`, `here` stays the page's own address): PeerTube's scripts under `<base href="/client/en-US/">` came back as
  the site's page. jsdom's side is jsurl.h `jd_base`/`ju_base` (baseURI, element href/src, a link's parts, location's
  and history's addresses, modules, import maps, import(), workers). A `<base>` a script adds moves jsdom's base, not
  the browser's.
- **A media file's stretches follow a redirect** (jsmedia.h: `d->url` becomes where the last stretch was sent):
  archive.org sends each request on to the server holding the film. Test: msecheck `/film/moved.mp4`.
- **`crypto.subtle`** (jscrypto.h): digest with SHA-1/256/384/512 on secure pages; every other method rejects with
  NotSupportedError.
- Found, not fixed: a frame from another site has no document, so players that live in one (Dailymotion's, BitChute's,
  PBS's) never start, and Dailymotion's whole page waits on its player.

**Since 0.120.0**:
- **Text past ASCII is folded where it is laid out** (layout.h `lay_fold`, in `lay_word` and the preformatted path):
  a script's text reaches the tree as it was given, and its curly apostrophes, dashes, ellipses and accented letters
  were drawn as gaps; now each word is folded with html.h `html_fold_cp`, as the parser folds a page's own text. The
  tree keeps the real characters. The title shown in the status line and told to the console is folded the same way
  (browser.c `title_take`). Tests: layouttest (raw UTF-8 put in with `dom_set_text`, laid out again with
  `lay_again`), browsercheck `/folded`.
- The host shim answers `/sys/time` from the host's clock (it answered no file, and pages took 2026-01-01).

**Since 0.119.0**:
- **The page's style sheets join the read-ahead** (browser.c `files_ahead`, called in `build` as soon as the page is
  read, before `gather_linked_sheets`; `ahead_add` with each file's own most, `CSS_MAX` or `SCRIPT_MAX`;
  `AHEAD_MAX` 192, since The Verge links 66 sheets). `gather_linked_sheets` takes a sheet from the list and fetches it
  the old way only when it is not there. The old page's pictures (`pics_drop`) and asks (`asks_drop_all`) are let go
  first, since a picture still coming holds the number of a slot that is about to be another file's. Medians of three
  on the host: BBC News 4.4 s to 3.7, The Verge 10.6 to 9.5, Imgur 6.0 to 5.5. Test: browsercheck `/sheets` (four
  sheets a second late each, counted at once; the last written must win).

**Since 0.118.0**:
- **A page's script files are asked for together as it loads** (browser.c `scripts_ahead`, `ahead_pump`,
  `ahead_take`, `AHEAD_MAX` 64): before `jsdom_scripts`, every script element with a src that will run (classic, or a
  module's first file) is listed and asked for over the ask slots, as many at once as are free; `fetch_script` takes
  its file from the list, waiting only for its own, and the rest go on coming; what is left is let go after
  (`ahead_drop`); a file that would not come this way is fetched the old way once. They still run in the order
  written. On the host: BBC News 8.3 s to 3.7 s, The Verge 67 s to 12 s, The Guardian 3.8 s to 1.2 s. Not yet: style
  sheets and pictures, and modules' imports. Test: browsercheck `/together` (six files the test server answers a
  second late each and counts at once, `webserver.DELAY_MOST`).

**Since 0.117.0**:
- **A script element a page's script inserts with a src is an ask** (jsnet.h `jd_script_queue`, `jd_scripts_pump`;
  browser.c `script_ask_start`/`script_ask_poll` on `ask_begin`, counted against `SCRIPTS_MAX`/`SCRIPTS_BYTES`, not
  `ASKS_MAX`): started on one pass, run when its file is whole, in the order the files come, then told load or error.
  It was fetched with a waiting `web_get` on the next pass, and nothing else on the page happened meanwhile: on Imgur,
  sampled, the program was waiting on the network nine looks in ten, and its gallery's posts came after forty seconds.
  Without `jsdom_script_asks_with` (the tests' stand-ins) the old way stands. The page's own scripts, read as it
  loads (`fetch_script`), still wait, as do modules and workers.
- **The page's own site first** (jsnet.h `jd_same_site`, `jd_site_part`: the host's last two labels, or three under
  a two-letter country with a short label before it): what waits for one of the three ways out goes in two rounds,
  scripts before requests. Imgur's posts now come at about fifteen seconds on the host.
- **scriptdump `net`** prints each ask as it starts and ends, with the time (browser.c `ASK_SEEN`, a macro only
  scriptdump defines). Tests: pagetest (an inserted script fetched while a timer runs and run in the order the files
  come, a missing one told error; the own site's scripts and requests asked first; `jd_site_part`); browsercheck
  `/inserted` (a script the test server answers two seconds late, purple if the page's timer counted on meanwhile).

**Since 0.113.0**:
- **`::before` and `::after` are drawn.** css.h keeps a rule whose selector ends in one (`crule.pe`, 1 or 2; nothing
  may follow it; `::first-line`, `::first-letter` and the rest are still refused), indexes those rules in chains of
  their own (`cindex.pe_by_tag`, `pe_by_key`, `pe_universal`, `npe`), and reads `content` (`P_CONTENT`). layout.h
  `lay_generated` makes, in the copy of the page that is laid out, an element of no tag named "::before" or "::after"
  as each host's first or last child, holding its text (`lay_content_text`: strings and their escapes, `attr()`, the
  quotes; private-use characters, an icon font's, are left out; counters, pictures and variables make nothing), for
  every element whose cascaded content is not none or normal (pictures, fields and painted boxes have none).
  `css_collect` gives such a node its host's rules for that pseudo-element (`css_pseudo_node`, `css_collect_pe`), and
  none of the element rules, so it is laid out as any element: inline, a block, a float, clearing. browser.c makes the
  copy (`page_drawn`) whenever the sheet has such rules, not only for shadow trees, and maps a generated node to its
  host for clicks, hovers and elementFromPoint (`page_node`). A box here already grows to hold its floats, so a
  clearfix changes nothing that was wrong; what was missing were the boxes themselves. `getComputedStyle(el,
  '::before')` reads a pseudo-element's style and content through `pseudo_style` (atlas 12).

**Since 0.112.0**:
- **`<canvas>` is `T_CANVAS`** (html.h), a box drawn from pixels like a `<video>` (layout.h `lay_painted`): laid out by
  `lay_video`, as big as its width and height attributes (300 by 150 without them) unless the page's rules say
  otherwise, keeping its shape when they give one side; what is written inside it is not laid out. The browser draws its
  bitmap and alpha in the box (`canvas_of`, from jscanvas.h `jsdom_canvas_picture`) and draws it again, the box alone
  where it can (`draw_video_frames`), when `jsdom_canvas_drawn` says it changed. The 2D context is atlas 12.
- The browser's faces are picked and measured in `userland/tface.h` (moved out of layout.h), shared with the canvas.

**Since 0.111.0**:
- **`<noscript>` is raw text** (dom.h `dom_raw`), as in any browser that runs scripts: its style elements, refreshes and
  pictures are not elements at all, and its text is the markup (the serialiser, `jd_raw_text`, already wrote it so).
  DuckDuckGo's home page keeps `body { display: none }` there and was blank. browser.c's `in_noscript` went with it.
- **A NUL in the page is U+FFFD** (dom.h `dom_parse`, from a copy made only when there is one): text is kept as C
  strings, and a NUL had ended X's first script mid-string.
- **A transform's percentages are of the box as laid out** (layout.h `lay_translate` reads `box_node`, `box_w`, `box_h`,
  which `lay_block_placed` sets as it finishes a box and clears as it starts one): from the items, a box drawing nothing
  of its own was as wide as its words, and DuckDuckGo's closed menu (fixed, right 0, `translate(100%)`) stood at the
  window's edge.
- SCRIPTS_MAX is 1024 (X has 604 modules).

**Since 0.110.0**:
- **Pictures a script adds or points elsewhere are fetched** (browser.c `pictures_follow`, called at the end of
  `relayout` while the page is live): every `<img>` whose source (`pic_source`) is not drawn, coming or tried for that
  element (`pic_known`: `shown.key`, `pic_wait_t.key`, `pic_tried`, keys from `pic_key`, a hash of the address) is put
  on the waiting list, a data: one made at once; a picture made for an element that had one replaces it (`pic_take`);
  one that would not come is remembered (`pic_tried`, 64) so it is not asked for at every layout. On only for a page
  whose build gathered pictures (`pics_follow_on`). Before, only the build's gathering saw pictures, so a picture loaded
  as it comes into view (data-src copied to src by a script) was never drawn.

**Since 0.107.0**:
- **A page is shown before its pictures** (browser.c `pic_wait`, `pictures_pump`). `gather_pictures` makes a picture
  written into the page (data:) at once and puts one at an address on the waiting list; `gather_backgrounds` does the
  same for backgrounds, keeping the slot as nothing (`ok` 0) until it comes. The decoding is `pic_take` (an element's)
  and `bg_take` (a background's). Each pass, `pictures_pump` starts what waits as asks (`ask_begin`, not counted against
  the page's `ASKS_MAX`; an Accept line for pictures), and makes what has come: an element's picture lays the page out
  again (`pics_relayout`, at most every 25 ticks while more are coming, and once at the end), a background's is only
  drawn. A new page or `load` drops what was coming (`pic_wait_drop`). The status is said in halves (`status_head`,
  `status_tail`) around the pictures, "N pictures (M more coming)" (`status_compose`), and the console's `browser:` line
  waits until the last picture has settled (`announce_owed`, `announce_page`), or is said as far as it got when another
  page is asked for, since what reads it takes it to mean the page is all there.

**Since 0.106.0**:
- **A page's requests do not stop the browser** (fetch.h `web_job`, browser.c `ask_t`). fetch.h's exchange is split
  into `web_head_for` (the head), `web_send_all`, `web_shape_of`/`web_shape_done` (where the head ends and how the body
  is measured), `web_keepable` and `web_read_answer`; `web_fetch_once` is made of them, and so is a job:
  `web_job_start` (its own connection, kept between its exchanges, or a new one; the request sent),
  `web_job_step` (what has arrived, read with `sock_wait` 0; 0 still coming, 1 whole in `j->r`, or a `WEB_ERR_`;
  redirects followed as `web_send_hops` does, a kept connection the far end closed tried once more afresh, given up after
  `WJ_QUIET` ticks without a byte), `web_job_stop`, `web_job_free`. The browser has `ASK_SLOTS` (3) asks: `ask_start`
  (a slot whose connection is to the same place first; -1 when all are busy; `request_lines` adds Origin as
  `do_request` does; the body copied), `ask_poll`, `ask_end`; idle connections are hung up after `ASK_IDLE` (5 s,
  `asks_tidy`, and `browser_wait` wakes for it) or at once when a connect finds every socket busy; a new page drops
  them all (`asks_drop_all`). jsnet.h's queue uses them through `jsdom_asks_with`: a request is started on one pass
  (`jxhr.ask`, plus one) and answered on a later one, several at a time; `jd_asks_full` makes a request waiting for a
  slot ask again on the next tick rather than at once. jsmedia.h's file stretches use them too (`jdmedia.ask`,
  `ask_from`), ended when the element or the page goes. Without the hooks (the host's tests) both block as before.
  Making a connection still waits (the kernel's connect and handshake).
- **Since 0.109.0 a job's connection is made without waiting** (fetch.h `web_job_send` with `connect_nowait` /
  `connect_tls_nowait`, state `WJ_CONNECTING`; `web_job_step` asks `sock_state` and sends the request with
  `web_job_ask` once it is up, giving up after `WJ_QUIET`). The blocking path (`web_fetch_once`: the page, its sheets,
  scripts) connects as before.
- **The WebSockets' reset drops only slots in use** (jsws.h `jd_ws_reset`): a free slot's socket is 0 before it was ever
  used, and dropping it hung up socket 0 at the first page's build -- the page's kept connection -- after which fetch.h
  and the next connection made shared the number.

**Since 0.103.0**:
- **A video's frame is drawn alone** (browser.c `draw_video_frames`): when a frame is all that changed (`video_due`
  without `dirty`), each video in sight has its box drawn into a surface that size (`frame_buf`): the page's canvas or
  the well's colour, then `draw_page` with the scroll moved down to the box's top and every item not crossing its
  columns skipped (`draw_x0`, `draw_x1`), so what lies over the video is drawn over it again; then copied into the
  window and committed alone (`win_commit_rect`). A box that meets the well's rounded corners falls back to the whole
  page. The image loop in `draw_page` now also keeps its rows inside the surface. Leaving a page prints
  "browser: video frames drawn alone N, with the page M" when it had frames.
- **A block picture keeps its margins** (layout.h): an img, svg or video laid out as a block -- a block's child or a box
  of its own -- has its own top and bottom margins, and auto margins at its sides centre it (both paths used to start it
  on a line at the block's top, its margins dropped).

**Since 0.99.0**:
- **Referer** (fetch.h, "who is asking"): `web_referrer_from` (the asking document's address) and `web_referrer_policy`
  (`REF_*`, the W3C Referrer Policy's eight, by name from `web_policy_of`, which also takes the old meta keywords) give
  every request a Referer from `web_referrer_for`: the default strict-origin-when-cross-origin sends the address less its
  fragment and user name to the same origin, `scheme://host[:port]/` to another, nothing from https to http. browser.c
  keeps `nav_from`/`nav_policy` for the next page (set by a link click, `want_go` -- forms, a script's navigation, a meta
  refresh -- and cleared after each load; nothing for an address typed or the history buttons), asks the page from there
  (`page_referrer` is what was sent, handed to jsdom as `document.referrer`, `jsdom_referrer`), then from the page
  itself under its Referrer-Policy header (`response_t.refpol`) or `<meta name="referrer">` (`meta_referrer`, read in
  `build` after the parse). A failed load asks from nowhere afterwards.
- **A page's requests are what it asked for** (jsnet.h, browser.c `do_request`): any method a token names but CONNECT,
  TRACE and TRACK (`jd_method_norm`: the standard's six in capitals, others as written), the page's headers but the
  forbidden ones (`jd_header_forbidden`, `jd_header_lines`; an Accept of its own replaces fetch.h's, `wh_has_line`), a
  body of bytes with its length (a BufferSource says no type: `web_body_type` "" sends no Content-Type), and Origin with
  a request to another origin or one that is not GET or HEAD. fetch.h takes `web_method` and `web_body_len` for one call,
  sends Content-Length 0 for a POST, PUT or PATCH without a body, reads no body after HEAD, keeps a 303 (and a 301/302
  after a POST) as a GET, puts the method back after redirects (`web_send`), and leaves the answer's head where the page
  can read it (`response_t.head`, `hlen`): fetch and XMLHttpRequest show every header but Set-Cookie.
- `SCRIPT_MAX` is 16 MB (YouTube's main script is 10.8 MB; it was 4).
- `site_readers_off` (set by scriptdump's `real`) runs YouTube's or Twitch's own page for the next load instead of
  sites.h's reading of it.

**Since 0.98.0**:
- **`<video>` is drawn.** html.h knows `video` and `audio` (`T_VIDEO`, `T_AUDIO`, after `T_CENTER`). The browser's rules
  give a video `object-fit: contain` and a sound no box (nothing draws a player's controls). layout.h `lay_video` lays a
  video out as a picture whatever its display (like `lay_drawing`, at every place an SVG is): the page's width and
  height (its attributes are hints, as an SVG's), either alone with the picture's proportions, neither and the picture's
  own size once its first frame is known (handed in with the pictures' sizes) or 300 by 150; kept to max-width and the
  line. Its children are never laid out (fallback for browsers without video), nor read as its words. browser.c gives
  the layout `sizes_for_layout()` (the pictures' sizes and each video's, `jsdom_media_size`), draws a video's item from
  `video_of` (the latest frame, lent by jsmedia.h), and after the timers pass redraws when `jsdom_media_frames` says a
  frame came, laying out again first when a video's size changed. The whole page is drawn for each frame.

**Since 0.92.0**:
- **WebSocket.** userland/wsock.h is RFC 6455's client and nothing of the page: `ws_start` connects (`connect_tls` for
  wss:), sends the upgrade (key from `random_bytes`, Origin, the offered subprotocols, the jar's cookies, the browser's
  User-Agent) and sets the socket's wait to 0; `ws_pump` reads what has come and gives one event a call (the answer
  checked: 101, Upgrade, Connection, Sec-WebSocket-Accept against SHA-1 of the key, no extension, a subprotocol only one
  offered; then frames: masked from the server or RSV bits fail it, ping gets pong, close gets close, pieces are joined
  up to 8 MB, text must be UTF-8); `ws_send` and `ws_close` frame with a fresh mask each. SHA-1 (`ws_sha1_*`) is FIPS
  180-4, used only here. jsws.h is the page's object: the constructor resolves the address (http: and https: become ws:
  and wss:, a fragment is dropped), checks the subprotocols, takes one of `JD_SOCKETS` (4) slots and connects on the
  next pass (`jd_ws_begin`); `jd_ws_pump`, called first in `jsdom_timers`, turns events into open, message
  (MessageEvent; binary as a Blob or an ArrayBuffer by binaryType), error and close (CloseEvent, added to the event
  kinds); `jsdom_next_due` wakes the browser every 5 ticks while one is open; `jsdom_close` drops them. browser.c gives
  the cookies (`socket_cookies`, `jsdom_websockets_with`). A failure is said on the console as "WebSocket: why".

**Since 0.90.0**:
- **A box that is its own formatting context sits beside a float** (layout.h `lay_beside_floats`, from
  `lay_block_placed`): overflow other than visible, `display: flow-root` (css.h `cstyle.flow_root`), flex, grid and
  tables are laid out in the room beside the floats at their top, and below them when a width of their own does not
  fit there (48 pixels for one without). Only the room at the box's top is looked at. An ordinary block still runs
  under a float with only its lines shortened, as in a browser.
- **Collapsed tables share their cells' borders** (css.h `cstyle.collapse`, inherited; layout.h `lay_table_share`): a
  cell leaves out its left border past the first column, its top past the first row, and any side on the table's edge
  where the table has a border of its own. The cells do not move, so their words sit a border's width from where a
  browser puts them; the wider of two borders does not win (each cell keeps its own right and bottom).

**Since 0.89.0**:
- **A row's items stretch** (layout.h `lay_stretch`, called from `lay_flex_line` and both grid ways): an item whose
  align-self (or the row's align-items) is stretch, with no height or max-height of its own, has its own box made as
  tall as the row's tallest. Only the box grows: what is in it stays at the top and nothing is laid out again, so a
  percentage height inside does not follow (in a browser it would). An item that draws no box of its own shows no
  change.

**Since 0.88.0**:
- **A grid's rows take their sizes** (css.h `grid_rows`, `grid_auto_rows`; layout.h `lay_grid_rows`, read by the
  column reader `lay_grid_tracks` handed the row template): a length is the row's height, minmax a floor and cap,
  fr/auto and percentages from the grid's own `height` when it has one and the content's height when not, and rows the
  template names exist empty. A grid that sizes its rows goes the placed way (`lay_grid_placed`), which keeps a table
  of rows; the row-by-row way had none. (Items stretch to their row since 0.89.0.)

**Since 0.85.0**:
- **Every named character reference** (userland/entities.h, written by tools/mkentities.py from Python's copy of the
  standard's table: 2125 names, sorted, `html_named` halves to one). Read as written (case matters); with its
  semicolon any name, without one only the 106 old ones, the longest that starts the run (`&notit;` is the not sign
  and "it;"). The text is still folded to ASCII (`html_fold_cp`), so a sign it has no fold for is `?`.

**Since 0.83.0**:
- **Shadow trees a script attaches are drawn** (dom.h `dom_flat`, browser.c `page_drawn`). The page is laid out from a
  copy (`flat_mem`) in which each tree jsdom knows of (`jsdom_shadow`) is in place: its contents are its element's
  children, and each slot holds its element's children given to it by their slot attribute (the rest to the first slot
  with no name), in place of its own, which stay only when none were; children given to no slot are not drawn. It is
  read from the page as the scripts left it, so the trees' order does not matter, and a node is the same number in
  both. The copy has nodes and attributes of its own and shares the page's strings: its marks go after the page's last
  string, where the page's next one will go, so it is made again before every layout (`page_drawn(1)`) and whenever
  the page has changed or grown since (`flat_used`, `flat_count`, the dom version). A value an element already has is
  pointed at a new string, never written over (`dom_flat_mark`). Marks are dom_shadows's with the tree's scope name:
  data-zs, data-zh, data-zl.
- **Each tree's sheets reach it alone** (browser.c `trees_gather`, `sheet_read`). Its style and link elements and its
  adopted sheets (`jsdom_shadow_adopted`) are read after the document's, scoped (css.h `css_scope`) to a name made from
  what they are ("f" and eight hex digits of `tree_key`): every copy of one component adopts the same sheets, so the
  copies share one name and one reading, where a name each read the sheets again for every copy. A tree with none has
  no name and no marks. `styles_print` takes the keys in, so a change to a tree's sheets remakes the sheet.
- **getComputedStyle, getBoundingClientRect and :hover use the tree as drawn** (`computed_style` from `page_drawn(0)`,
  `box_of` and `css_hover_reach` from `laid`, the tree the last layout walked): an element in a shadow tree has its
  tree's rules and inherits from where it is shown, and a component's box holds what its tree drew.
- **The border shorthand's colour** is the colour part (css.h): the whole value went on as the colour, which is no
  colour, so every border it set was drawn in the default grey.
- On the host MSN draws its search box, section navigation and panels, where it drew nothing (the feed does not come:
  the page is at 121 MB of its 128 MB of script memory four seconds in). `scriptdump URL SECS tree` prints the page as
  it is drawn, shadow trees in place; `'js=EXPR'` evaluates in the page once it has run.

**Since 0.82.0**:
- **Scripts run in a browser's order** (`jsdom_scripts`, `jd_script_when`): first those the parser stops for, in the
  order written (`JR_NOW`); then, the document read and `interactive` (readystatechange fires between), the deferred
  files and every module that is not async, in the order written (`JR_AFTER`); then the async ones (`JR_ARRIVES`).
  They had all run in the order written, so NHS's deferred main.js ran before the inline script below it that sets
  the settings it reads. defer and async mean nothing to a classic script without a src. A deferred or async
  script's document.write is dropped, since the document is no longer loading (`jd_doc_write`).
- **Web Workers** (`userland/jsworker.h`, `jd_setup_workers`): a worker is a JavaScript context of its own
  (`js_init`, `JW_MAX` 8 slots, each with half what a page is given and none when under 24 MB is free), made from a
  fetched, `data:` or `blob:` script and run on the page's thread: its script, each message to it, each of its timers
  and each of its fetches is a task in the page's queue (`jd_later_native`). Messages cross by a copy made in the other
  context (`jsc_across`). Tasks carry the slot's generation (`jw_name`, `jw_named`), so a slot used again never runs
  what was queued for the worker before; what a worker posted before close() still arrives, and nothing from one the
  page terminated (`jw_ended`). Its globals are postMessage, close, importScripts (fetched and run in its global scope,
  in order), setTimeout and the rest, fetch (GET or POST with a text body, data: and blob: too), name, location,
  navigator (the page's), performance, crypto, and from a JavaScript prelude run in its context (`JW_PRELUDE`, handed
  its natives as arguments): addEventListener and onmessage, Event, MessageEvent, ErrorEvent, EventTarget,
  WorkerGlobalScope and the rest of its interfaces, MessageChannel, AbortController, URL and URLSearchParams (parsed
  by the page's parser, `nat_jw_parse_url`), Headers and Response. An uncaught error is an error event at the Worker
  and a line on the console. Not done: module workers (run as classic scripts), SharedWorker, transfer (copied, not
  moved), XMLHttpRequest in a worker, a fetch's headers beyond its content type.
- **A document's arena is its owner's to give** (`ddoc.arena`, `arena_cap`, `dom_use_arena`; `own_arena` is the
  DOM_ARENA of 3 MB a document given none has). The browser gives its page 16 MB (`DOC_ARENA`, twice `SRC_MAX`), lazily
  mapped, so a page costs only what it uses; Netflix's page is 3.2 MB, nearly all one script, which had been cut in the
  middle of a string. svg.h and svgtest, which malloc a document, say they use its own.
- On the host: NHS runs all 10 of its scripts, Netflix all 10 and is drawn, OpenStreetMap and MSN run theirs (MSN still
  draws nothing: its content is in shadow trees a script attaches, which are not drawn).

**Since 0.81.0**:
- **The page's styles follow its scripts** (browser.c, `sheets_follow`). What `build` read before any script ran is
  the base (`styles_base`: the sheet's counts and each rule's `decl_n`), with every style and link element it read
  and the rules each gave (`sources`, 512, `ok` 1 fetched, 0 failed, -1 another medium or disabled, -2 past
  SHEETS_MAX). `styles_print` hashes each style and link element's number, whether it is in the page and outside a
  template, and what it is read from (`source_hash`: a style's text by `dom_text_hash`, its media, a link's rel,
  href, as and disabled), with `jsdom_css_version`. When that differs from the last, `sheets_remake` cuts the sheet
  back to the base, switches off (`decl_n` 0) the base rules of each element that has gone, changed or been given a
  script's version (`jsdom_sheet_override`), reads every other style element, stylesheet link and adopted sheet
  (`jsdom_adopted`) after it in the page's order, reads again the style attributes read since the base (their
  declarations went with everything else after it), and indexes the sheet. `relayout` calls it, and
  `computed_style` calls `sheets_follow_if_changed`, which skips the walk while `jsdom_dom_version` and
  `jsdom_css_version` are as they were (a loop of getComputedStyle cost three times 0.80.0's without it), so
  getComputedStyle sees a style the script has just made. A changed element's rules come after the whole base,
  where a browser keeps its place: which rule wins a tie can differ. `rel` is read as a list of words
  (`rel_is_sheet`: stylesheet and not alternate), and a disabled link is not read. Linked sheets fetched after the
  build are kept for the page (`links_kept`, 48 and 3 MB) and handed to scripts that read a linked sheet's rules
  (`sheet_text_for_script`).
- **Link events** (`links_report`, after `jsdom_open` and after each remake): each stylesheet link and preloaded
  sheet (`rel=preload as=style`), once for each address, is told whether it arrived (`jsdom_link_loaded`, which
  fires load or error as a task); one the base did not fetch (for another medium, preloaded, or added by a script)
  is fetched for that. So a preload's onload makes it a stylesheet, a print sheet's onload makes it the screen's,
  and webpack's chunk sheets say they have loaded.
- **The CSSOM** (`userland/jscssom.h`, `jd_setup_cssom`): `HTMLStyleElement.sheet` and `HTMLLinkElement.sheet`
  (null out of the page, for a link that is not a stylesheet's, or with no href), `document.styleSheets` (made again
  when `jd_version` moves), `new CSSStyleSheet()`, `replace`, `replaceSync`, `insertRule`, `deleteRule`, `addRule`,
  `removeRule`, `disabled`, `media` (MediaList), `ownerNode`, `href`, and the rules: CSSStyleRule (selectorText,
  style), CSSMediaRule, CSSSupportsRule, CSSContainerRule, CSSLayerBlockRule, CSSLayerStatementRule, CSSScopeRule,
  CSSStartingStyleRule, CSSKeyframesRule (appendRule, deleteRule, findRule) and CSSKeyframeRule, CSSFontFaceRule,
  CSSImportRule, CSSPageRule, CSSNamespaceRule, CSSPropertyRule, CSSCounterStyleRule, CSSFontFeatureValuesRule, with
  CSSRule's type numbers. A holder (a sheet, or a rule that holds rules) keeps its rules' texts and, once asked for,
  their objects (`jcs_k_texts`, `jcs_k_objs`); a sheet's texts are split from its element's text or the fetched file
  when first wanted (`jcs_ready`). What a browser drops is dropped (`jcs_read`, `jcs_valid`): a selector the style
  sheets' own parser will not read (`jcs_selector_ok`), an at-rule it does not know, an @import after other rules.
  A sheet is the element's while the element's text (or a link's href) hashes as it did (`jcs_node_hash`); after
  that the element has a new one and the old belongs to nobody. A change goes up through the holders' texts to the
  sheet (`jcs_changed`), which is marked for the browser (`jcs_publish`: a record per sheet, 128, written out only
  when the browser asks, since emotion inserts thousands of rules one at a time). cssRules is a live list (host
  numbers from `JD_RLIST`), and a rule's style is a CSSStyleDeclaration (from `JD_RSTYLE`) sharing the element
  style's code through `jd_decls`, so camelCase names, setProperty and cssText work on either. Only a made sheet may
  be replaced or adopted (NotAllowedError); `adoptedStyleSheets` keeps a copy of what it was given, and pushing onto
  that is seen (`jsdom_css_version` hashes the list). Not done (0.81.0; a shadow root's adopted sheets are applied
  since 0.83.0): an @import's styleSheet is null, shorthands are not expanded into longhands, and rules nested in a
  style rule are kept as written.
- **Forms, ranges and markup** (`userland/jsform.h`): constraint validation (ValidityState, checkValidity,
  reportValidity, setCustomValidity, validationMessage in Chrome's words), DocumentType and document.doctype,
  XMLSerializer, Range (boundaries, the contents methods, comparisons, createContextualFragment whose scripts run
  when inserted) and a Selection holding the ranges a script adds. Also Attr nodes: `createAttribute(NS)`,
  `get/set/removeAttributeNode(NS)`, NamedNodeMap's `setNamedItem(NS)`, `getNamedItemNS` and `removeNamedItemNS`;
  an Attr's value is kept under a symbol and read and written through the element that holds it. Namespaces:
  `lookupNamespaceURI`, `lookupPrefix` and `isDefaultNamespace` through the element's own namespace and the xmlns
  attributes round it. `execCommand` and the queryCommand* answer no (nothing is editable). DOMImplementation's
  `createDocument` (an empty document, a root element when named; not marked as XML, so the root's nodeName is upper
  case) and `createDocumentType`.
- **What Mozilla's consent manager takes off the prototypes**: XMLHttpRequest's fields are getters on its prototype
  (`JD_XHR_FIELDS`, kept under symbols), NodeList and HTMLCollection have a `length` getter, performance entries keep
  their fields under symbols and read them through getters on PerformanceEntry, PerformanceResourceTiming and
  PerformanceNavigationTiming (`PE_*`, `nat_pe_field`), marks and measures are PerformanceMark and PerformanceMeasure
  with `detail` (and a mark's `startTime` option), and `clearResourceTimings` and `setResourceTimingBufferSize` exist
  (no resource entries are kept). New event constructors: PointerEvent, DragEvent, TouchEvent, AnimationEvent,
  TransitionEvent, ClipboardEvent, SecurityPolicyViolationEvent, FormDataEvent and ToggleEvent. On the host,
  Mozilla now runs all 11 of its scripts.
- The host shim (`tools/host/shim.c`) takes `pause=MS` in a script, for a picture after a page's timers have run.

**Since 0.79.0**:
- **DOMMatrix, DOMMatrixReadOnly, DOMPoint, DOMPointReadOnly** (`userland/jsmatrix.h`, `jd_setup_matrix`): sixteen
  numbers in the standard's order under a symbol key, accessors a-f and m11-m44 on the prototypes, every method and
  its Self form, transform text read as CSS writes it (pixels and angles; % and em refused), toString, toJSON, the
  typed-array forms and the from* statics; `WebKitCSSMatrix` is DOMMatrix. getComputedStyle's `transform` is what the
  layout moves the element by, `none` or `matrix(1, 0, 0, 1, x, y)`, since translation is all of a transform it draws.

**Since 0.78.0**:
- **Module scripts** (`userland/jsmod.h`). `<script type=module>` runs after the classic scripts in the order written,
  as defer has it; a `nomodule` script does not run; `HTMLScriptElement.supports` says yes to classic, module and
  importmap. The module map `jd_mods` (256, states `JM_NEW` ... `JM_FAILED`) holds each address once, fetched
  breadth first through `jd_get_script` (`jd_mod_fetch_all`), a `data:` address decoded from itself and a `blob:` one
  read from its Blob. Linking (`jd_mod_link`) gives each module a scope under the global one and a namespace whose
  exports are getters reading the exporter's bindings (`nat_mod_binding`, `nat_mod_forward`), so they are live; its
  imports are getters in its scope. An address resolves against the module's own (`jd_mod_resolve`), a bare name
  through the page's first import map, exact names then the longest prefix ending in a slash; one nothing resolves
  is said ("a module name this cannot resolve: X"). `import()` gives a promise of the namespace; `import.meta` has
  `url` and `resolve`. A module that awaits at its top level is `JM_ASYNC` until its promise settles; one importing
  a module still pending is `JM_WAITING` with a count and runs when the last settles; each has a `done` promise
  that importers and `import()` wait on. What such a module throws later is put on the page's error line
  (`jd_mod_note_value`). Not done: an import map's scopes, import attributes (read and set aside), and a cycle
  through a module that awaits. SCRIPTS_MAX is 1024 (256 until 0.111.0) and SCRIPTS_BYTES 16 MB (browser.c), since a
  module page is many files.
- **performance.timing, performance.navigation and the navigation entry** (jswin.h): the fetch's moments are the
  page's beginning (its world opens after the answer has come), and interactive, DOMContentLoaded, complete and
  load are recorded as they happen (`jd_nav_mark`) and 0 until then. An observer of `navigation` is told once load
  has finished. No redirect or reload is known, so none is said.
- **attachShadow** makes a `ShadowRoot` (a fragment, kept on the element; `shadowRoot` when open, `host`, `mode`,
  `innerHTML`; `getRootNode({composed})` passes it). Drawn since 0.83.0 (above).
- **document.fonts**: an empty `FontFaceSet`, loaded, since the browser draws its own typeface.

Source tree: the repository root (zelr main, 2026-09-22, two commits after v0.37.0). Static reading only: nothing was built or run. Every `file:line` below was checked against the tree. "Verified" in section 10 means verified by reading the code, not by running it.

---

## 1. Scope

| File | Lines | Role |
|---|---|---|
| `userland/browser.c` | 1746 | The browser program: window, chrome, main loop, page build (parse → sheets → index → pictures → scripts → layout), drawing, forms, history, find, status line. |
| `userland/web.h` | 299 | `url_t`, bounded string helpers, URL parse/print/join, the address-or-search rule, query escaping. |
| `userland/fetch.h` | 760 | HTTP/1.1 client over the kernel's sockets: request building, the three body-end modes, dechunking, gzip, keep-alive with retry-once, redirects, an in-memory cookie jar. |
| `userland/html.h` | 445 | Tag table (`T_*`, `HTML_TAGS`), void tags, entity + UTF-8 decoding with ASCII transliteration, raw-text end finder. Also a flat tokenizer (`html_parse`, `hdoc`) that nothing uses any more. |
| `userland/dom.h` | 698 | The document tree (`ddoc`/`dnode`), arena, attributes, tree-building parser with implied-end rules and a whitespace rule, tree mutation used by scripts. |
| `userland/css.h` | 1468 | CSS: colours, lengths, property table, selector and declaration parser, shorthands, at-rules, matching, cascade (specificity then order), inheritance, rule index, and the UA stylesheet `CSS_UA`. |
| `userland/layout.h` | 1416 | Styled tree → flat display list (`ldoc`): block/inline/flex layout, positioned boxes, line breaking against the 33 browser faces, form-control sizing, hit testing. |
| `userland/layouttest.c` | 515 | Headless ring-3 test of layout coordinates (flex, box-sizing, min/max, absolute/relative, `<center>`, pseudo-element rules, big lengths, vw, % height). Prints `LAYOUTTEST_PASS/FAIL`. |
| `userland/pagetest.c` | 639 | Headless ring-3 test of dom.h + jsdom.h: parse, attributes, whitespace, scripts, events, tree mutation, classList, timers, fields, querySelector. Prints `PAGETEST_PASS/FAIL`. |
| `tools/browsercheck.py` | 404 | QEMU + desktop + real HTTP server: draws pages, follows a link, goes back, chunked vs measured, sheets, PNG, missing picture, SVG, script, redirect, 404, https-to-a-plain-server. Asserts by pixel colour and by render equality. Since 0.72.0 also an SVG written into the page, drawn in its link's colour, and a WebP picture (a one-colour lossless file `webserver.py` writes bit by bit; Windows' decoder reads it the same). |
| `tools/formcheck.py` | 341 | QEMU: finds controls by colour, types, ticks, submits GET and POST; the server records what arrived. Checks rounded field corners. |
| `tools/webcheck.py` | 170 | QEMU: the **kernel shell's** `fetch` command (kernel/http.c) against the test server on e1000, pcnet, an undriven ne2k, and an instant-answer path. Does **not** exercise userland `fetch.h`. |
| `tools/livecheck.py` | 154 | QEMU: the `/live` page -- timer, external script, XHR, click listener -- each proven by a colour; `/live-quiet` is the control. |

Read for the interface only (or as test infrastructure):

| File | Lines | Why |
|---|---|---|
| `userland/jsdom.h` | 1314 | Read fully: the DOM bindings, events, timers, XHR, script running -- the whole browser↔engine interface. |
| `userland/js.h` | 447 | Read lines 1–260: header (what the engine has/has not), limits, value/object types, `jctx` with the `host_get`/`host_set` hooks, region allocator. |
| `tools/webserver.py` | 633 | The test server every browser check talks to (routes listed in §8). |
| `userland/wiretest.c` 185 + `tools/wirecheck.py` 90 | -- | The only tests that drive `fetch.h` directly (gzip, cookies, keep-alive counted by the server, three sockets at once). |
| `tools/findcheck.py` 167, `tools/piccheck.py` 80, `tools/ring3check.py` 108 | -- | Find highlight check; runners for layouttest/pagetest. |
| `userland/facetext.h` (generated, 1.7 MB), `userland/face.h` | -- | Only the face table (`TFACE_SIZES 33`, `face_t`, `face_glyph`, `FACE_FIRST 32`..`FACE_LAST 126`). |
| `userland/ui.h`, `userland/draw.h` | -- | Widgets/primitives the browser calls (`ui_field`, `ui_field_key`, `ui_field_draw`, `ui_button*`, `ui_toolbar`, `ui_statusbar`, `ui_well`, `ui_sunken`, `ui_scrollbar`, `ui_round`, `rect`, `round_rect`, `disc`, `mix`). |
| `sdk/zelr.h` 120–200, 415–475, 820–930 | -- | Socket, TLS-status, window, find and key syscalls. |
| `kernel/syscall.c` 700–900 | -- | What `connect`/`connect_tls`/`send`/`recv`/`disconnect`/`tls_status` actually do (socket table, one-TLS-socket rule, 1400/8192-byte send cap, 4 s recv timeout). |

---

## 2. Big picture

### 2.1 What the area is

`browser.c` is a ring-3 program (`/bin/browser`, embedded in the kernel image via `kernel/builtin.c:31,87`, launcher entry `kernel/wm.c:400`). Everything else in scope is header-only (`static inline`) code compiled into it (and into the test programs). Built by `userland/build.sh` with `zig cc -target x86_64-freestanding-none -O2 -std=gnu11 -ffreestanding -nostdlib -fno-builtin -mcmodel=large -mno-red-zone`; unlike the kernel, ring 3 may use floating point (the JS engine needs doubles).

The end-to-end pipeline (one page):

```
address bar text
  └─ go_or_search (browser.c:1308)
       url_looks_like_address (web.h:127) ? set_address(typed)
                                          : set_search → "https://lite.duckduckgo.com/lite/?q=" + url_escape(typed)
  └─ load(address) (browser.c:865)
       url_parse (web.h:173)           bare name ⇒ https:443; "http://" ⇒ :80
       web_get/web_post (fetch.h:753/757) → web_send: ≤6 hops of redirects
         web_fetch: one retry on a fresh socket if a *reused* socket gave nothing back
           web_fetch_once: connect / connect_tls (kernel), request, recv loop,
                           length | chunked | close, dechunk, gunzip (inflate.h), cookies
       text/plain or application/json ⇒ show_plain (one <pre>)
       otherwise ⇒ build (browser.c:769)
         jsdom_close; dom_parse (dom.h:486)      tree, implied ends, whitespace rule
         css_init; CSS_UA; accent_sheet; every <style>; ≤6 <link rel=stylesheet>;
         every style="" (gather_inline_styles); css_index
         gather_pictures (≤24 <img>; PNG/JPEG by magic bytes, else SVG)
         jsdom_open → scripts in node order (external ones fetched synchronously) →
                      DOMContentLoaded, load (to document listeners)
         relayout → lay_run (layout.h:1348); if no words came out but the DOM has words,
                    lay out again ignoring visibility (lay_show_hidden)
         publish_text (for the desktop's find)
       status line: counts + ", encrypted" / ", NOT encrypted"; maybe follow <meta refresh>
main loop (browser.c:1350): events → keys → timers → one XHR → form navigation →
  scroll → (if dirty) load / hover relayout / JS click / controls / links / chrome / draw_page
```

### 2.2 Design decisions and their stated reasons

- **Fixed buffers, decided once, reused for every page** (browser.c:40-57): "a browser that allocated per page would be a browser whose failure to show one depended on which one it showed before". Sized from measured pages (a 670 KB encyclopaedia article; 240 KB ordinary article): `SRC_MAX` became 1 MiB after a 320 KB buffer turned a compressed page into "the fetch failed". The JS engine does allocate (quarter-megabyte region chunks), as do gunzip and the image decoders.
- **One buffer for the whole response** (fetch.h:426-431): headers and body share `buf`; the body is a pointer into it. (The comment's reason, "a program here has no allocator", is stale -- see §10.)
- **Three body-end modes decided by headers, not by sniffing** (fetch.h:1-14): Content-Length, chunked, or connection close; "a client that only knows one of them loses the end of about half the pages on the web".
- **Keep-alive with retry-once** (fetch.h:432-450): a page with a dozen pictures used to pay a dozen (TLS) handshakes. The connection is kept only when the response was self-delimiting and not `Connection: close`. A failure on a reused connection is retried once, "only when nothing came back at all, because a request the server answered and then dropped may have been acted on, and asking again is how somebody orders twice."
- **https by default for a bare name** (web.h:97-106): guessing http would send the typed address in the clear before the redirect to https. Scheme-relative links keep the page's scheme so an encrypted page's links are never "quietly taken down to http" (web.h:229-232).
- **Search goes to DuckDuckGo Lite, not Google** (browser.c:1270-1291): Google's `/search` result page arrives with no results in it and is built by script; DDG Lite is plain HTML that renders. "A limitation stated rather than a preference."
- **ASCII transliteration** (html.h:96-106): the typefaces cover printable ASCII only; curly quotes, dashes, accents are written the way they were before Unicode ("a reader loses the shape of the mark rather than the sense of it"). Unknown code points become `?`.
- **A real tree with browser-style implied-end rules** (dom.h:1-26, 362-368): `<p>` ends at a block, `<li>` at the next `<li>`, etc.; a stray close tag for something not open is dropped "because that is how one stray `</div>` in a footer swallows the rest of a document."
- **Whitespace is content only between inline things** (dom.h:502-520): dropped at the start of a container and after a block; otherwise one `" "` text node is kept (the Google nav read "GmailImages" when all whitespace was dropped).
- **The UA stylesheet is a stylesheet** (css.h:1416-1422): parsed first so every page rule beats it by ordinary cascade. A second tiny sheet `a{color:#<theme accent>}` sits between UA and page (browser.c:681-702).
- **Rule index by rightmost key** (css.h:1293-1304): rules filed under the id, first class, or tag of their rightmost compound (or "universal"), so an element only tests rules that could match -- "turns the tens of millions into tens".
- **Lengths kept as written, resolved at layout** (css.h:82-85), in hundredths in an `int` (css.h:99-107: shorts overflowed at 327.67 and broke `width:960px`).
- **Transparency composited against white** (css.h:213-219), since nothing composites.
- **One-pass layout into a flat display list in document coordinates** (layout.h:1-20); drawing and scrolling are "a subtraction". Since 0.69.0 tables, floats, inline-blocks and wrapping flex are laid out (see the atlas README, 0.69.0); grids since 0.70.0 (`lay_grid`: tracks, spans, auto-fill, named areas), with track minimums, `grid-auto-flow: column` and sideways grids wrapping since 0.72.0.
- **Inline boxes have left/right edges but not top/bottom** (layout.h:544-562): left/right padding/border/margin separate links; vertical padding "does not move the line it is on"; a box that wraps across lines draws nothing rather than a band.
- **Flex by measure-then-lay-out** (layout.h:825-847): measure each child by laying it out and throwing that away -- "exactly right, because the thing being measured is the thing that will be drawn". Since 0.50.0 a flex row that is itself being measured (`L->measuring`) does not lay its children out a second time: it reports how far right they reach (`measure_right`: each child's place plus the narrower of its measured and given width) and its height from the measured heights, which it used anyway. Nesting had doubled the work at every level (twelve rows deep laid out thousands of boxes; now 79, `ldoc.laid`); the reach is exact unless a child centres its content or sizes it by percentage.
- **Positioned boxes** (layout.h:1055-1078): relative = lay out then shift items; absolute = from nearest positioned ancestor, takes no space; fixed = treated like absolute against the page ("the closer of the two wrong answers"); bottom-only boxes stay where the flow put them.
- **Pages that hide themselves until a framework runs** (layout.h:48-64; browser.c:666-678): if a layout produced no words but the DOM has words, lay out again ignoring `visibility`/`opacity`, and say so.
- **Form control values live in the DOM as attributes** (browser.c:149-158): layout, drawing, scripts and submission read the same place ("two answers that agree until somebody types").
- **A click goes to the page first** (browser.c:1594-1601): handlers can `preventDefault`, and then neither control nor link acts.
- **The script world lives as long as the page** (jsdom.h:26-42), costs nothing for a page with no script and no `on*` attribute; XHR is performed on the browser's *next pass* so code after `send()` runs before `onload` (jsdom.h:759-764).
- **"encrypted" / "NOT encrypted" in words** (browser.c:20-25, 1006-1010): a padlock reads as a promise about the site; the code only promises the bytes came from whoever holds the typed name. Marking only the encrypted case "trains people to read a missing mark as nothing in particular".
- **Find reads what was laid out** (browser.c:221-236): the desktop cannot read pixels, so the browser publishes the laid-out words (`win_set_text`) and re-finds the n-th match itself to scroll and highlight.


---

## 3. File-by-file detail

### 3.1 `userland/web.h` (299 lines) -- URLs

(0.162.0: `URL_FILE`, a `secure` of 2 for a file on the machine, "file:///path": parsed, printed and joined as the
README's since list says; fetch.h `web_file` reads one, only while `web_files_ok`.)

Purpose (web.h:1-6): "everything here is bytes in and bytes out, with no screen anywhere in it."

**Constants**: `URL_HOST 128` (10), `URL_PATH 640` (11), `URL_TEXT (URL_HOST + URL_PATH + 24)` = 792 (12). `URL_HOST` matches the kernel's 128-byte host copy in `sys_connect` (kernel/syscall.c:748).

**Type** `url_t` (14-19): `char host[URL_HOST]` (no port), `char path[URL_PATH]` (path + query, never a fragment; always starts with `/` except the bug in §10), `int port`, `int secure` (1 = https/TLS).

**String helpers** (21-60, "written out rather than pulled in" so bounds are obvious):
- `w_len(s)` (29); `w_copy(dst, cap, src, n)` (35) copies ≤ min(n, cap-1) chars, stops at NUL, always terminates; `w_same(a,b)` (41) exact equality; `w_lower(c)` (46) ASCII; `w_same_fold(a,b)` (51) case-insensitive **whole-string** equality; `w_starts_fold(s,prefix)` (56) case-insensitive prefix.
- `url_copy(dst, src)` (68): field-by-field, because "a struct assignment of this size becomes a call to memcpy, and there is no library here to have one in" (62-67). The same reasoning recurs in layout.h:855-859.

**`url_text(u, out, cap)`** (80): `http(s)://` + host + `:port` only when not the scheme default (and `n < cap-8`) + path. Used for the address bar, history, hover status, meta-refresh comparison.

**`url_looks_like_address(in)`** (127) -- the address-or-search rule (107-126), in order:
1. skip leading spaces; empty → search (0);
2. starts with `http://`, `https://` or `//` → address;
3. contains a space or tab anywhere → search;
4. starts with `/` → address; starts with `localhost` (prefix test) → address;
5. the **last** `.` before any `/`, `?`, `#` must not be the first character and must be followed by `[A-Za-z0-9]` → address; otherwise search.
So `zelr` → search, `zelr.com` → address, `10.0.2.2:8000/form` → address, but also `3.14` or `e.g` → address, and a trailing space turns `example.com ` into a search.

**`url_escape(in, out, cap)`** (157): keeps `A-Z a-z 0-9 - _ . ~`, everything else `%XX` (uppercase hex); space is `%20` ("one rule rather than two", 152-156). Stops when `w >= cap-4`. Used only for the search query. (Form submission uses a different encoder with `+` for space: browser.c:353.)

**`url_parse(in, out)`** (173):
1. skip spaces; `secure = 1` by default; strip `https://` (8), `http://` (7, `secure = 0`) or `//` (2);
2. host = everything up to `/`, `?`, `#` (stored up to 127 chars; scanning continues past); empty host → return 0;
3. port: at the first `:` in the host, digits only, accepted if 1..65535 else the scheme default; the host is cut at the `:` regardless (so IPv6 literals and `user:pass@` break);
4. path: a `/` is prepended only if the next char is not `/` **and not `?`**; copied up to `#` (≤ 639 chars); empty → `/`. Returns 1.

**`url_join(base, href, out)`** (222) -- link resolution:
- leading spaces skipped; empty or `#...` → a copy of `base` (so a fragment link re-fetches the same page; there is no in-page anchor navigation);
- `http://`/`https://` → `url_parse`;
- `//host/...` → parsed, then `secure = base->secure`; the port is reset to the scheme default unless a `:` appeared in the authority (229-243);
- any `:` in the first 12 chars before a `/` → return 0 ("a scheme this cannot speak", 245-249): rejects `javascript:`, `mailto:`, `data:`, `tel:`, but also relative references with an early colon;
- `/abs/path` → copied verbatim up to `#` (no dot-segment removal);
- otherwise: base path up to and including its last `/` (the scan covers the whole `path`, **including the query**), plus `href` up to `#`; then `/./` removed and `/../` pops a segment (276-296); empty → `/`.

### 3.2 `userland/fetch.h` (760 lines) -- HTTP client

Includes `zelr.h`, `alloc.h`, `inflate.h`, `web.h` (15-19).

**Errors** (21-30): `WEB_ERR_SCHEME -1`, `WEB_ERR_CONNECT -2`, `WEB_ERR_SEND -3`, `WEB_ERR_EMPTY -4`, `WEB_ERR_HEADERS -5` (no blank line), `WEB_ERR_TLS -6`, `WEB_ERR_DOWN -7`, `WEB_ERR_RESOLVE -8`, `WEB_ERR_BUSY -9`, `WEB_ERR_ENCODING -10`. `web_err_from(rc)` (35) maps kernel `NET_ERR_DOWN/RESOLVE/TLS/BUSY` (sdk/zelr.h:189-193: -2,-3,-5,-6) and folds everything else (incl. `NET_ERR_CONNECT -4`, `-1`) into `WEB_ERR_CONNECT`. `WEB_ERR_SCHEME` is never returned by fetch.h itself.

**`response_t`** (45-54): `int status`; `char *body` (points into the caller's buffer); `int len`; `int truncated` (buffer filled, or gunzip ran out of room); `char location[URL_TEXT]`; `char ctype[64]`; `int secure`; `char how[64]` (TLS parameters agreed, or why the handshake failed).

**Request helpers**: `wh_add(buf,cap,n,s)` (58) / `wh_add_num` (67) append or return -1 when full.

**Header readers**: `wh_header(head, hlen, name, out, cap)` (84): case-insensitive name at a line start followed by `:`, value with leading blanks skipped, up to CR/LF; first occurrence only. `wh_header_nth(..., nth)` (111): the nth occurrence (for `Set-Cookie`). `wh_number` (363) decimal or -1; `wh_hex` (370) hex prefix or -1.

**`wh_split(buf, len, *skip)`** (387): first `\n\n` (skip 2) or `\r\n\r\n` (skip 4); bare-LF servers accepted ("not broken enough to refuse to talk to").

**`wh_dechunk(body, len)`** (399): in place ("every byte written is behind the byte being read"); chunk extensions ignored (skips to LF); size 0 ends; a short last chunk is kept ("keep what there is"); returns length, or -1 if the first size did not parse (the caller treats ≤ 0 as 0). Trailers are not parsed (irrelevant: size 0 returns immediately).

**`wh_gunzip(body, len, cap, *cut)`** (151): checks `1F 8B 08`, skips the 10-byte header, `FEXTRA` (flag 4), `FNAME` (8), `FCOMMENT` (16), `FHCRC` (2); the CRC/ISIZE trailer is ignored "because what a checksum would catch the transport has already checked" (137-150). `malloc(cap)` for output, `inf_run` (inflate.h:279), copy back, `free`. `INF_FULL` ⇒ keep the front and set `*cut`; **any other error, including `INF_TRUNCATED`, ⇒ -1** (176). Only `Content-Encoding` values starting with `gzip` are decoded (687-688); the request advertises `gzip` only (532). No `deflate`, no `br`.

**Cookie jar** (184-361). Limits (196-200): `CK_MAX 64`, `CK_NAME 96`, `CK_VALUE 768`, `CK_DOMAIN = URL_HOST (128)`, `CK_PATH 160`. `cookie_t` (202-209): `name, value, domain, path, secure, used`. Global `ck_jar[CK_MAX]` (211) -- memory only, gone when the browser exits ("a decision and not half a job", 190-192). `ck_forget_all()` (213) exists but is never called.
- `ck_domain_ok(host, domain)` (219): case-insensitive suffix on a label boundary; empty domain never matches.
- `ck_path_ok(path, cpath)` (228): empty or `/` matches all; else prefix with a boundary (`\0`, `/`, or cpath ending in `/`).
- `ck_take_one(u, line)` (243): `name=value` (name ≤ 95, value ≤ 767, trailing spaces trimmed; no name or no `=` ⇒ ignored). Defaults: domain = request host, path = `/`. Attributes: `Secure`; `Domain` (leading `.` stripped; accepted only if `ck_domain_ok(host, d)` -- so a host can widen to any parent, even a bare TLD); `Path` (only if it starts with `/`); `Max-Age` starting with `0` or `-` ⇒ delete; `Expires` containing `1970` ⇒ delete (the only date check, 303-308). `HttpOnly`/`SameSite` ignored. Same (name, domain, path) ⇒ update value or delete; else take the first free slot; a full jar silently drops new cookies.
- `ck_take(u, head, hlen)` (332): up to 16 `Set-Cookie` headers per response (on every response, including redirects and errors).
- `ck_header(u, out, cap)` (342): all matching cookies (secure only over https) as `a=1; b=2`, stopping when full.

**Keep-alive state** (451-469): `ka_host[URL_HOST]`, `ka_port`, `ka_secure`, `ka_live`, `ka_sock = -1`. Exactly one kept connection, by design ("what the several [sockets] are for is the browser asking for a page and its pictures at once, which is a layer above this one", 454-458 -- not implemented). `web_drop()` (461) disconnects; `ka_matches(u)` (466) = live && same port, scheme, host (case-folded).

**`web_fetch_once(u, body, buf, cap, r)`** (474) -- one exchange:
1. Reset `*r`.
2. Connection: reuse if `ka_matches` (for TLS, `r->how = tls_what`); else `web_drop()` then `connect_tls(host, port)` (the handshake verifies the certificate against `u->host`, 491-493) or `connect(host, port)`. On `NET_ERR_TLS` only, `r->how = tls_why` (496-499).
3. Request (512-551) in `req[URL_PATH + URL_HOST + CK_VALUE + 512]` (2048 B):
   ```
   GET|POST <path> HTTP/1.1\r\n
   Host: <host>[:<port> only if not the scheme default]\r\n     (518-524: "Host: www.google.com:443" loops some servers)
   User-Agent: zelr\r\n
   Accept: text/html,text/plain,*/*\r\n
   Accept-Encoding: gzip\r\n
   Connection: keep-alive\r\n
   [Cookie: <jar>\r\n]
   [Content-Type: application/x-www-form-urlencoded\r\nContent-Length: <strlen(body)>\r\n]
   \r\n
   ```
   Overflow ⇒ `WEB_ERR_SEND`. Sent in ≤ 1400-byte pieces (554-562; the kernel refuses plain sends > 1400 and TLS sends > 8192, kernel/syscall.c:840), then the body in the same pieces (564-577).
4. Receive loop (579-630): reads ≤ 32768 bytes per `recv` into `buf + total`, always leaving room for a NUL; `room <= 0` ⇒ `truncated = 1`, stop. `NET_EOF` (-2) or any negative ⇒ stop. `0` (the kernel's 4000 ms recv timeout expired, kernel/syscall.c:860,869) ⇒ count; **three in a row** ⇒ stop ("a server that has stopped talking without saying so"). Once headers are complete: `Transfer-Encoding` starting `chunked` ⇒ chunked; else `Content-Length` ⇒ `want`. Stop as soon as `total - hlen - skip >= want`, or when a chunked buffer **ends with** `0\r\n\r\n` (617-629). Since 0.76.0 an interim answer (1xx but 101, such as 103 Early Hints) ahead of the real one is passed over, and a 204 or 304 has no body whatever its head says (`want = 0`).
5. Keep decision (635-651): keep iff the answer was whole (since 0.76.0; else `r->cut` unless `truncated`), headers were found, the body was self-delimiting (`want >= 0 || chunked`) and `Connection` does not start with `close`; else `disconnect`.
6. Parse (653-682): `total == 0` ⇒ `WEB_ERR_EMPTY`; no blank line ⇒ `WEB_ERR_HEADERS`; status = digits at bytes 9-11 when `buf[0] == 'H'`; `Location`, `Content-Type`, cookies; body = after headers; dechunk or trim to Content-Length; NUL-terminate.
7. gzip (686-699) as above; failure ⇒ `web_drop()`, `WEB_ERR_ENCODING`.
8. Return `status`, or `WEB_ERR_EMPTY` if no status was parsed.

**`web_fetch`** (706): `reused = ka_matches(u)`; on failure *of a reused connection* with `WEB_ERR_SEND`, `WEB_ERR_EMPTY` or `WEB_ERR_CONNECT`, drop and try **once** more on a fresh connection.

**`web_send(u, body, buf, cap, r)`** (722): up to `WEB_MAX_HOPS 6` (720) fetches. Returns immediately on an error or a non-redirect status. For 301/302/303/307/308: no `Location` ⇒ return the 3xx; `url_join` failure ⇒ return; a Location equal to the current URL (host, path, port **and scheme**) ⇒ return the 3xx (a loop is "reported rather than followed"; http→https on the same path is not a loop, 733-736); else `*u = next`. After a 301/302/303 a POST becomes a GET; 307/308 keep the body (742-748). After 6 redirects returns the last 3xx. `*u` ends as the final URL (the browser uses this for the address bar and relative links).

**`web_get(u, buf, cap, r)`** (753) = `web_send(u, 0, ...)`; **`web_post(u, body, ...)`** (757) = `web_send(u, body ? body : "", ...)`.

**Syscalls used** (sdk/zelr.h): `connect` (425), `connect_tls` (443), `tls_what`/`tls_why` (450-451, via `SYS_TLS_STATUS`, machine-wide "last handshake"), `send` (452), `recv` (462, `NET_EOF -2`), `disconnect` (465); plus `malloc`/`free` (alloc.h). Kernel side (kernel/syscall.c:712-879): `SOCK_MAX = TCP_MAX` sockets checked against the owner pid; `sys_connect_tls` refuses with `NET_ERR_BUSY` while **any** secure socket is open machine-wide (786-787); a failed handshake closes the TCP connection (798-802); DNS and TCP connect time out at 6000 ms.

### 3.3 `userland/html.h` (445 lines) -- tags and characters

**Tags** (23-42): `T_OTHER 0`, then 64 known tags in this order: `html head body title h1..h6 p br hr div span a ul ol li dl dt dd b strong i em code pre kbd samp tt blockquote table tr td th caption script style img form input button textarea select option nav header footer section article main aside figure figcaption small noscript iframe svg label meta link hgroup center`; `T_COUNT = 65`. `HTML_TAGS[T_COUNT]` (65-77) holds the names in the same order (it is consistent, although the promised check "at the bottom of this file", 62-64, does not exist). Everything else (`tbody`, `thead`, `u`, `s`, `sup`, `sub`, `mark`, `abbr`, `picture`, `source`, `video`, `canvas`, `details`, `summary`, `template`, `fieldset`, `address`, `wbr`, …) is `T_OTHER`.

**Node kinds** `N_TEXT=1, N_OPEN, N_CLOSE` (21) -- only for the dead flat tokenizer.

- `html_tag_of(name, len)` (79): case-insensitive exact match by linear scan; `T_OTHER` if none.
- `html_void(t)` (91): `br hr img input meta link` only.
- `html_fold_cp(cp, out)` (107): code point → ASCII (writes 1-4 bytes). `< 0x80` itself; `0xC0-0xFF` via the table at 108-110 (`AAAAAAACEEEEIIIIDNOOOOOx0UUUUYPB` / `aaaaaaaceeeeiiiionooooo/0uuuuypy`: é→e, ñ→n, ß→B, Ø→0, ×→x, ÷→/); specific mappings (116-141): NBSP→space, ©→`(c)`, «→`<<`, ®→`(R)`, °→` deg`, ·→`-`, »→`>>`, ½→`1/2`, U+2010..2013→`-`, U+2014/2015→`--`, U+2018/2019/201B→`'`, U+201C/201D/201F→`"`, U+2022/25CF/25AA→`*`, U+2026→`...`, U+2039/203A→`<`/`>`, U+2122→`(TM)`, U+2190/2192→`<-`/`->`, U+2212→`-`, U+20AC→`EUR`, U+00A3→`GBP`, U+00A5→`JPY`; anything else `?`.
- `html_char(p, left, *used, out)` (153): one source character. `&` (if `left > 2`): numeric `&#123;`/`&#x7B;` (≤ 8 digits; none ⇒ literal `&`) or one of 36 named entities (178-195: `amp lt gt quot apos nbsp mdash ndash hellip lsquo rsquo ldquo rdquo copy reg trade bull middot laquo raquo times deg euro pound yen eacute egrave agrave ccedil uuml ouml auml szlig ntilde rarr larr`, matched exactly and case-insensitively on an alphanumeric run ≤ 12; unknown ⇒ literal `&`); the `;` is optional (212-214). UTF-8 2/3/4-byte sequences decoded (no overlong/surrogate checks); a bad lead or continuation byte ⇒ `?` consuming one byte. Everything then goes through `html_fold_cp`.
- `html_raw_end(p, len, at, name)` (298): offset of the first `</name` (case-insensitive, no check of what follows the name) at or after `at`, or `len`.
- **Dead code**: `hnode` (44-50), `hdoc` (52-59), `HTML_NODES 9000`, `HTML_ARENA 192K`, `ha_*` (245-260), `html_attr_value` (266), `html_parse` (309) -- a flat open/text/close tokenizer from before dom.h; no `.c` or other header references them (grep).

### 3.4 `userland/dom.h` (698 lines) -- the tree

**Limits** (32-40, "measured against real pages rather than picked": 5600 elements, 14000 attributes, 670 KB of HTML): `DOM_NODES 20000`, `DOM_ATTRS 40000`, `DOM_ARENA (1536*1024)`, `DOM_DEPTH 64` (parser stack).

**Types**:
- `DN_ELEMENT = 1, DN_TEXT` (42); since 0.77.0 also `DN_COMMENT`: the parser keeps each comment (its first `DOM_COMMENT_MAX` 4096 bytes, and none once the document is within 1024 nodes of full), the whitespace rule looks past comments, `:empty` ignores them, and the layout draws nothing for one (an empty inline no rule matches). jsdom's comments are the same nodes (`jd_new_comment`).
- `dattr {int name, value}` (47-49): arena offsets; names folded to lower case on the way in.
- `dnode` (51-58): `short kind, tag; int text` (DN_TEXT, DN_COMMENT: its string; DN_ELEMENT with `T_OTHER`: its lower-cased name; else -1); `int attr_at, attr_n` (a contiguous run in `attrs`); `int parent, first, last, next, prev` (-1 = none).
- `ddoc` (60-73): `nodes[DOM_NODES], count, attrs[DOM_ATTRS], nattrs, arena[DOM_ARENA], used, root` (a synthetic `<html>` element, always index 0), `body` (first `<body>`, else `root`), `head`, `title` (arena offset or -1), `overflowed`. About 2.6 MB; the browser holds one static `doc`.

Nothing is freed: removed nodes keep their index and data; the whole arena is discarded when the next page is parsed (108-117).

**Arena**: `dom_put` (77, sets `overflowed` when full), `dom_str` (82), `dom_get` (89, unused).

**Nodes and mutation**: `dom_new(d, kind, tag)` (95); `dom_unlink` (118); `dom_contains(a, b)` (136: b is a or under a); `dom_append(parent, child)` (144: refuses `parent == child` and cycles -- "a ring is not a tree that terminates" -- and moves a node that is already attached); `dom_insert_before(parent, child, ref)` (161: a `ref` that is not a child of `parent` means append, as the DOM says); `dom_create_element(name, len)` (181: unknown names kept as text so `tagName` answers).

**Attributes**: `dom_attr_add` (196, only valid while the element is the newest run); `dom_attr(el, name)` (204: exact, first match, or 0); `dom_attr_fold` (225: case-insensitive name, added for SVG `viewBox`, 214-224); `dom_attr_set(el, name, value)` (254): rewrite in place if the new value fits, else a fresh arena string; if absent and the element's run is not last, **copy the run to the end** of `attrs` (old copy abandoned) and append -- needed for `el.className = ...` on an element without a class (249-253).

**Walking and text**: `dom_next(i, top)` (293) iterative pre-order within `top` ("a recursive walk on a fixed stack is how a browser meets a page built out of nested divs and stops", 288-292); `dom_text_content(el, out, cap)` (304) concatenates all text descendants (including script/style text); `dom_set_text(el, s, len)` (325) creates one new text node and sets `first = last = -1` **without unlinking** the old children; `dom_has_class(el, want, len)` (341) whole-word class test; `dom_has_words(d)` (407) any non-space text under body outside `script/style/textarea/title` (drives the hidden-page fallback).

**Tree-building rules**:
- `dom_is_block(t)` (369): `p div ul ol li dl dt dd h1-h6 hr blockquote table tr td th form nav header footer section article main aside figure figcaption pre caption hgroup body center`.
- `dom_closes(open, now)` (385): `P` by any block; `LI` by `LI`; `DT`/`DD` by `DT`/`DD`; `TD`/`TH` by `TD`/`TH`/`TR`; `TR` by `TR`; `OPTION` by `OPTION`. Only the **top** of the stack is tested (repeatedly).
- `dom_raw(t)` (399): `script style textarea title` -- content is one text node, decoded only for title/textarea (650).

**Parser state** `dparse {ddoc *d; int stack[DOM_DEPTH]; int depth}` (434-438); `dp_top` (440, `root` when empty), `dp_tag_top` (444), `dp_push` (449: beyond 64 sets `overflowed` and the element is simply not pushed), `dp_pop_to(tag)` (457: to the nearest open element with that tag; nothing if none), `dp_pop_to_name(name, len)` (0.42.0: the same for `T_OTHER` by name), `dp_text(p, len, decode)` (465), `dp_all_space` (480).

**`dom_parse(d, p, len)`** (486):
1. Reset; `root = dom_new(DN_ELEMENT, T_HTML)`.
2. Text up to `<`: decoded. If all whitespace (502-536): dropped when the parent has no children yet or its last child is a block element; otherwise one `" "` text node. Else a text node on `dp_top`.
3. `<!-- ... -->` skipped; `<!...>`, `<?...>` skipped to `>`.
4. Close tag: name up to `>`/space; a known tag ⇒ `dp_pop_to`; ~~unknown ⇒ ignored~~ unknown ⇒ `dp_pop_to_name` (0.42.0), which closes the nearest open unknown element with that name, and does nothing if there is none.
5. Open tag: name up to `>`, whitespace or `/`; unknown ⇒ lower-cased name stored; pop while `dom_closes(top, tag)`; create; every attribute (name lower-cased; value `"..."`, `'...'` or bare; entity-decoded; a bare attribute gets `""`); `self_closed` iff the byte before `>` is `/`; append to `dp_top`; remember the first `<body>`/`<head>`; raw elements consume to `</name` and skip past `>`; `<title>` sets `d->title` once; otherwise push unless void or self-closed.
6. `body = root` if there was no `<body>`.

Note: the synthetic `root` is a `T_HTML`; a page's own `<html>` becomes a second `T_HTML` child of it.

**Finding**: `dom_by_id(id)` (675) and `dom_by_tag(tag, nth)` (686) scan node indices 0..count (document order for parsed nodes; detached and script-created nodes included); `dom_tag_name(el)` (694).


### 3.5 `userland/css.h` (1468 lines) -- style sheets

Includes `zelr.h`, `web.h`, `dom.h` (20-23).

**Limits** (25-35, measured: "two sheets ... two hundred and seventeen kilobytes and eleven hundred rules"): `CSS_RULES 4000`, `CSS_SELS 9000`, `CSS_DECLS 16000`, `CSS_TEXT (512*1024)`, `CSS_MAXCLS 4` (classes/attributes per compound; extras are ignored, which makes the compound match more broadly). Also `CSS_BUCKETS 512` (1306), `CSS_HITS 192` (1344).

**Enums**: `P_*` properties (39-52: `COLOR, BACKGROUND, DISPLAY, FONT_SIZE, FONT_WEIGHT, FONT_STYLE, FONT_FAMILY, TEXT_ALIGN, TEXT_DECORATION, MARGIN_T/R/B/L, PADDING_T/R/B/L, BORDER_T/R/B/L, BORDER_COLOR, WIDTH, MAX_WIDTH, HEIGHT, LINE_HEIGHT, WHITE_SPACE, LIST_STYLE, RADIUS, TEXT_INDENT, VISIBILITY, OPACITY, FLEX_DIR, JUSTIFY, ALIGN_ITEMS, FLEX_WRAP, GAP, FLEX_GROW, MIN_WIDTH, MIN_HEIGHT, MAX_HEIGHT, BOX_SIZING, POSITION, TOP, RIGHT, BOTTOM, LEFT`); `POS_STATIC/RELATIVE/ABSOLUTE/FIXED` (60); `CSS_AUTO_OFF ((short)-32768)` (64); `D_INLINE 0, D_BLOCK, D_INLINE_BLOCK, D_LIST_ITEM, D_NONE, D_TABLE_CELL, D_FLEX` (66-67); `FD_ROW, FD_ROW_REVERSE, FD_COLUMN, FD_COLUMN_REVERSE` (75); `JC_START, JC_CENTER, JC_END, JC_BETWEEN, JC_AROUND, JC_EVENLY` (76); `AI_STRETCH, AI_START, AI_CENTER, AI_END, AI_BASELINE` (77); `A_LEFT, A_CENTER, A_RIGHT, A_JUSTIFY` (78); `WS_NORMAL, WS_PRE, WS_NOWRAP` (79); `LS_DISC, LS_DECIMAL, LS_NONE, LS_CIRCLE, LS_SQUARE` (80); units `U_PX, U_EM, U_REM, U_PCT, U_VW, U_VH, U_AUTO` (86); combinators `CB_FIRST, CB_DESC, CB_CHILD` (160); pseudo-classes `PS_NONE, PS_HOVER, PS_LINK, PS_VISITED, PS_FIRST_CHILD, PS_ROOT` (161).

**Globals**: `css_view_w`, `css_view_h` (96-97) -- the window for `vw`/`vh`; `lay_run` sets `css_view_w = width` (layout.h:1357), `browser.c` sets `css_view_h = view_h` each loop pass (1327, 1362). Nothing sets `css_view_h` in layouttest (so `vh` resolves to auto there).

**Types**:
- `clen {int v; uchar unit}` (99-109): hundredths of the unit.
- `cstyle` (111-144): `u32 color, background, border_color; short font_px; uchar bold, italic, mono, underline, strike, display, align, white, list, visible, has_bg; short mt, mr, mb, ml, pt, pr, pb, pl, bt, br, bb, bl; short width, max_width, height, min_width, min_height, max_height` (-1 = auto/none); `uchar border_box, position; short top, right_off, bottom, left` (`CSS_AUTO_OFF` = auto); `short line_h` (percent of font size); `short radius, indent; uchar flex_dir, justify, align_items, flex_wrap; short gap, grow`.
- `csel` (151-158): `short tag` (-1 any), `int id` (text offset or -1), `int cls[CSS_MAXCLS]` (class text offsets; an attribute-presence test is stored as `-offset-2`), `short ncls, combinator` (how this part joins the part to its left), `short pseudo`.
- `cdecl {short prop; int value}` (163-166), `crule {sel_at, sel_n, decl_at, decl_n, spec, order}` (168-173; parts leftmost first).
- `csheet` (175-185): `rules[4000], nrules, sels[9000], nsels, decls[16000], ndecls, text[512K], used, overflowed` (~1 MB).
- `cinline {int at, n}` (193-195): a node's `style=""` declarations, one per DOM node (parsed once per page, not per layout, 187-192).
- `cmatch {int hover; int visited_links}` (935-938).
- `cindex {by_tag[T_COUNT], by_key[CSS_BUCKETS], universal, next[CSS_RULES]}` (1308-1313); `chit {rule, spec, order}` (1346-1350). Since 0.50.0 `cindex` also holds `need[CSS_RULES]`, a `cbloom` (128 bits, two per key) of the tags, ids and classes every part but the last insists on, and `css_collect` takes the element's ancestor `cbloom`: a rule wanting a bit the ancestors lack is skipped without `css_matches` (keys: `css_key_tag/id/cls`, raw bytes, exactly as `css_part_matches` compares; collisions only let a rule through to be walked).

**Text arena**: `css_put` (197), `css_put_lower` (205, unused).

**Colours** (213-314): `CSS_NAMES` (223-249): 53 names -- `black white red green blue gray grey silver maroon yellow olive lime aqua cyan teal navy fuchsia magenta purple orange transparent(=FFFFFF) lightgray/lightgrey darkgray/darkgrey whitesmoke gainsboro dimgray/dimgrey darkblue darkred darkgreen steelblue royalblue dodgerblue crimson tomato gold beige ivory linen salmon khaki indigo violet pink brown coral turquoise slategray/slategrey midnightblue rebeccapurple`. `css_color(s, *out)` (259): `#rgb`/`#rrggbb` (4/8-digit forms use the first 3/6 digits; alpha ignored); `rgb(...)`/`rgba(...)` with commas, spaces or `/`, `%` channels, alpha 0..1 or %, **composited over white** (303-308); otherwise an exact case-insensitive name. No `hsl()`, no `currentColor`.

**Lengths**: `css_len(s)` (318): `auto` or no digits ⇒ `U_AUTO`; optional sign; integer part capped at 10000; two fractional digits; suffix `em`, `rem`, `vw`, `vh`, `%`, `pt` (×4/3 → px); anything else (px, ex, ch, cm, vmin…) is px. `css_px(L, font_px, root_px, pct_of)` (359): em × font_px, rem × root_px, % of `pct_of` (0 if `pct_of < 0`), vw/vh of `css_view_w/h` (-1 if unset), auto = -1.

**Property names**: `CSS_PROPS` (373-425) maps: `color`; `background-color`, `background` → P_BACKGROUND; `display`; `flex-direction`; `justify-content`; `align-items`; `flex-wrap`; `gap`, `column-gap`, `row-gap` → P_GAP; `flex-grow`, `flex` → P_FLEX_GROW; `font-size/-weight/-style/-family`; `text-align`; `text-decoration`, `text-decoration-line`; `margin-*`, `padding-*`, `border-*-width`; `border-color`; `width`, `min-width`, `min-height`, `max-height`, `box-sizing`, `position`, `top`, `right`, `bottom`, `left`, `max-width`, `height`, `line-height`, `white-space`, `list-style-type`, `border-radius`, `text-indent`, `visibility`, `opacity`. `css_prop_of(name)` (427) exact lower-case.

**Parsing**:
- `css_skip(p, len, i)` (452) whitespace and `/* */` together (so a comment inside a selector list is not a descendant combinator).
- `css_parse_selector(s, p, len, *at, *spec)` (479): one selector up to `,` or `{`. Returns the number of compound parts, or **0 = drop this selector** for: `+`/`~` combinators (490-498); any pseudo-element (`::x`, or `:before/:after/:first-line/:first-letter`, 533-554 -- the `::-webkit-scrollbar{width:6px}` bug); any pseudo-class with an argument (`:not()`, `:nth-child()`, `:is()`…, 563-573); `[attr=value]`-style attribute tests (583-589); an unknown element name (606-613); running out of `CSS_SELS`. Supported: `*`, tag, `#id` (spec `1<<20`), `.class` (spec `1<<10`, max 4), `[attr]` presence (spec `1<<10`), pseudo-classes recognised **by length and first letter only** (556-560): 5+`h` → `:hover`, 4+`l` → `:link`, 7+`v` → `:visited`, 4+`r` → `:root`, any 11-letter name → `:first-child`; any other argument-less pseudo-class is **accepted and ignored** (spec +`1<<10`, no test). Descendant (space) and child (`>`) combinators. Tag spec 1.
- `css_add` (639), `css_parts` (649: up to N space-separated parts, parentheses kept together; parts are slices, not NUL-terminated), `css_shorthand4` (665: 1/2/3/4-value box expansion).
- `css_declare(s, name, nlen, v, vlen)` (681): trims; strips a trailing `!important` (only when the value is longer than 10 chars; the priority is **not** honoured); lower-cases the name (≤ 39 chars). Shorthands: `margin`, `padding` (4-value); `border` and `border-width` → one width for all four sides (last length, or `thin`=1/`medium`=2/`thick`=4, 1 if none, 0 if `none`/`hidden`) plus `border-color` if a part parses as a colour (711-746); `border-top/-bottom/-left/-right` → the whole value into that side (width and colour extracted at apply time); `font` → the first part that parses as a positive length becomes `font-size`, `bold`/`italic` before it are taken (751-765); `list-style` → P_LIST_STYLE. `background` (not `-color`) keeps only the first part that parses as a colour, else nothing (774-787: "a gradient or an image ... paints panels black"). Everything else via `CSS_PROPS`; unknown properties dropped.
- `css_parse_block` (792): `name: value;` pairs to `}`; `;`/`}` inside parentheses do not end a value.
- `css_at_rule(p, len, i, *open_body)` (828): `@x ...;` skipped (so `@import` is never fetched); `@media` (5 letters starting `m`) and `@supports` (8 letters starting `s`) are opened when they can apply (since 0.69.0 `css_mq` evaluates them: widths kept per rule and compared with the window at layout; print, dark, high density, portrait and touch never apply); every other block at-rule (`@font-face`, `@keyframes`, `@layer`, `@container`, …) is skipped whole.
- `css_parse(s, p, len)` (867): loop of at-rules, stray `}` (closing an opened media block), and rules: a selector list (≤ 32 selectors) then one declaration block shared by one `crule` per selector (`order = nrules`). A rule whose block is not directly after the selectors is discarded.

**Matching**: `css_part_matches(s, d, el, c, m)` (940): element; tag; `id` exact; each class via `dom_has_class`, each `[attr]` via `dom_attr`; pseudo: `:hover` ⇔ `el == m->hover`; `:link` **and** `:visited` ⇔ an `<a>` with `href` (`visited_links` is never read); `:first-child` ⇔ first element child; `:root` ⇔ `el == d->root` (the synthetic root). `css_matches(s, d, el, r, m)` (977): rightmost part first; child ⇒ parent must match; descendant ⇒ the **nearest** matching ancestor is taken (no backtracking).

**Cascade support**:
- `css_default_style(st, root_px)` (1003): colour `0x1A1A1A`, background `0xFFFFFF`, border colour `0xD0D0D0`, font `root_px`, display inline, flex row/start/stretch, align left, white normal, list disc, visible, no bg, zero boxes, sizes -1, static, offsets auto, `line_h 145`, radius/indent 0. It does **not** set `border_box` (see §10).
- `css_inherit(child, parent)` (1037): defaults, then inherits `color, font_px, bold, italic, mono, align, white, list, line_h, visible, underline, background` (with `has_bg = 0`). Not inherited: margins, strike, sizes, display, flex settings, position.
- `css_apply(s, dcl, st, root_px, pct_of)` (1054) per property: colours via `css_color` (background sets `has_bg`); `display` (`none`, `inline-block`, `inline-flex`→inline-block, `inline`, `list-item`, `table-cell`, `flex`, everything else → block, 1063-1072); flex keywords (1074-1103; `flex-wrap` stored but unused by layout); `gap` px ≥ 0; `flex`/`flex-grow` leading integer (`none` 0, `auto` 1); `font-size` (`smaller` ×5/6, `larger` ×6/5, else a length resolved against the **current** `st->font_px`, clamped 7..96, 1126-1133); `font-weight` (`bold*`, `normal`, digit ≥ 6); `font-style` italic/oblique; `font-family` mono iff the value contains `mono`; `text-align`; `text-decoration` (first word: `underline`/`line-through`/`none`); `white-space` (`pre*` → pre, `nowrap`); `list-style(-type)`; `box-sizing`; `position` (`sticky` → relative); `visibility:hidden`; `opacity` `0`/`0.0`/`0.1…` → invisible; `line-height` (% → percent, px > 4 → percent of font size, a number → ×100, clamped 90..300, 1195-1204); lengths (margins, paddings, border sides, sizes, offsets, radius, indent): `height/min-height/max-height` in % → auto (1238-1252), `top/bottom` in % → auto; border sides: `none/hidden` → 0, no length → 1, any colour part sets `border_color`; offsets keep sign, clamp ±4000, auto; **all other lengths clamp to [-1, 4000]** (1285-1287). Since 0.69.0 margins keep their sign like the offsets, with auto as `CSS_AUTO_OFF`.

**Index and collection**: `css_hash` (1315, FNV-1a masked to 512); `css_index(s, x)` (1321): files each rule (walking backwards so chains are in source order) under its rightmost part's id, else first real class, else tag, else universal. `css_collect_chain` (1352) appends matching rules (≤ `CSS_HITS`). `css_collect(s, x, d, el, m, hits)` (1369): universal chain, the element's tag chain, its id's bucket, and each class's bucket (class tokens copied into a 64-byte buffer), then a stable insertion sort by `(spec, order)`. A bucket shared by the id and a class, by two classes (hash collision), or by a repeated class token is walked twice, producing duplicate hits.

**UA stylesheet** `CSS_UA` (1423-1468), verbatim rules:
```
html,body{display:block;margin:0;padding:0;color:#1a1a1a}
body{padding:0 2px}
div,section,article,main,aside,nav,header,footer,figure,figcaption,hgroup,form,dl,dt,fieldset,address{display:block}
p{display:block;margin:0.85em 0}
center{display:block;text-align:center}
h1{display:block;font-size:2em;font-weight:bold;margin:0.55em 0 0.4em}
h2{display:block;font-size:1.5em;font-weight:bold;margin:0.7em 0 0.35em}
h3{display:block;font-size:1.22em;font-weight:bold;margin:0.8em 0 0.3em}
h4{display:block;font-size:1.05em;font-weight:bold;margin:0.9em 0 0.3em}
h5,h6{display:block;font-weight:bold;margin:1em 0 0.3em}
h6{font-size:0.9em}
ul,ol{display:block;margin:0.7em 0;padding-left:1.8em}
li{display:list-item;margin:0.25em 0}
ol{list-style-type:decimal}
dd{display:block;margin-left:1.8em}
blockquote{display:block;margin:0.9em 0;padding-left:1em;border-left:3px #d6d6d6;color:#555}
pre{display:block;white-space:pre;font-family:monospace;margin:0.9em 0;padding:10px 12px;background:#f6f7f9;border-radius:6px;color:#222}
code,kbd,samp,tt{font-family:monospace;font-size:0.93em}
b,strong{font-weight:bold}
i,em{font-style:italic}
small{font-size:0.86em}
a{color:#0b5ed7;text-decoration:underline}
hr{display:block;margin:1.1em 0;border-top:1px #dcdcdc}
table{display:block;margin:0.8em 0}
tr{display:block}
td,th{display:table-cell;padding:4px 10px 4px 0}
th{font-weight:bold;text-align:left}
caption{display:block;font-style:italic;margin:0.3em 0}
head,script,style,title,meta,link,noscript{display:none}
button{display:inline-block;padding:5px 12px;background:#f2f3f5;border:1px #c9ccd1;border-radius:6px}
input,textarea,select{display:inline-block;padding:4px 8px;background:#ffffff;border:1px #c9ccd1;border-radius:6px}
img{display:inline-block}
center{display:block;text-align:center}
iframe,svg{display:none}
```
(`fieldset`/`address` are unknown tags, so those two selectors are dropped; `center` appears twice; the `a` colour is always overridden by the accent sheet.)

### 3.6 `userland/layout.h` (1416 lines) -- boxes and lines

Includes `web.h`, `dom.h`, `css.h`, `facetext.h` (21-26).

**Limits** (28-44, measured on an article with 1600 anchors that produced 46 000 items and 6 500 links): `LAY_ITEMS 96000`, `LAY_TEXT (1024*1024)`, `LAY_LINKS 12000`, `LAY_LINE 400` (unused), `LAY_DEPTH = DOM_DEPTH (64)`, `LAY_FLEX_MAX 128` (since 0.69.0; the rest are laid out below the row). A link past `LAY_LINKS` is drawn as plain words (not clickable).

**Item kinds** `LK_BOX 1, LK_TEXT, LK_BULLET, LK_IMAGE, LK_FIELD` (46). **Control kinds** `CTL_NONE 0, CTL_TEXT, CTL_PASSWORD, CTL_BUTTON, CTL_CHECK, CTL_RADIO, CTL_AREA, CTL_SELECT, CTL_HIDDEN` (168-169), shared by layout (size) and browser (look and behaviour).

**Global** `lay_show_hidden` (65): set by the browser for the second layout pass of a page that produced no words (ignores `visibility`/`opacity` checks; `display:none` still applies).

**Types**:
- `litem` (67-77): `int x, y, w, h` (document coordinates; y down the document); `int at` (text offset for LK_TEXT and numbered bullets, else -1); `u32 color, bg, border`; `short face`; `int node` (the element; **text runs leave it -1**); `int link` (index into `links` or -1); `uchar kind, under, strike, radius, bt, br, bb, bl, has_bg`.
- `llink {int node; int href}` (79-82); `limage {int node; int w, h}` (87-90: size only -- pixels belong to the browser).
- `ldoc` (92-101): `items[96000], nitems, text[1 MiB], used, links[12000], nlinks, height, overflowed` (~6.5 MB).
- `lctx` (243-278): `d, s, x, m, inl, imgs, nimgs, out, root_px`; line state `line_at, line_n, pen, line_top, line_h, line_base, line_left, line_width, align, pending_space, line_started`; `cur_link`; `list_depth, list_count[LAY_DEPTH]`; containing block for positioned boxes `pos_x, pos_y, pos_w`.

**Faces** (103-153): `face_pick(px, bold, mono)` (109) nearest size among faces with the same bold and mono flags, falling back to nearest size alone. The 33 faces (`facetext.h:6, 26255-26289`): regular and bold at 11, 12, 13, 14, 15, 16, 17, 19, 21, 24, 28, 34, 42 px; monospaced regular 12, 13, 14, 15, 17; monospaced bold 13, 15. **No italic face** (italic is parsed and dropped). `tface_of` (131), `tface_h` (136, = face size), `tface_wn` (138: sum of glyph advances; bytes outside 32..126 measured as space), `tface_w` (149).

**Controls**: `lay_control_kind(d, el)` (181): `textarea` → AREA, `select` → SELECT, `button` → BUTTON; `input` by `type` (case-insensitive): missing/empty → TEXT, `hidden`, `password`, `checkbox`, `radio`, `submit`/`button`/`reset`/`image` → BUTTON, anything else → TEXT. `lay_control_label(d, el, kind)` (207): button: `value`, else `<button>` text (≤ 255) or `Button`, else `Reset`/`Submit`; select: text of the first direct `<option>` child ("nothing here opens one"); textarea: `value` attribute or text content; others: `value` or `""`. Returns a shared static 256-byte buffer for computed labels.

**Helpers**: `lay_put` (280), `lay_number` (291: leading digits, capped 100000, so `100%` reads as 100), `lay_image_of` (302), `lay_item` (308: defaults colour `0x1A1A1A`, bg `0xFFFFFF`, border `0xD0D0D0`, kind LK_TEXT, node/link -1).

**Lines**: `lay_line_end(L, *y)` (331): shift the line's items by the slack for centre/right (justify is left); drop each item onto the baseline (`y = line_top + line_base - height`, never above `line_top`); `*y = line_top + line_h`. `lay_line_start` (359). `lay_line_fit(L, h, pct)` (376): line box `h*pct/100`, baseline at `h*4/5 + (box-h)/2`, both maxima over the line.

**Text**: `lay_word(L, s, n, st, *y, face)` (389): wraps when the word (plus a pending space measured in its own face) passes the right edge, the line is not empty and `white-space` is not `nowrap`; one LK_TEXT item per word (colour, underline, strike, `link = cur_link`). Long words are never broken. `lay_text_run(L, s, st, *y)` (423): `pre` → one item per source line, newlines end lines, spaces kept, no wrapping (and no underline/strike set); otherwise split on space/tab/CR/LF/FF into words with collapsing.

**Style**: `lay_style(L, el, parent, out, pct_of)` (470): `css_inherit`, `css_collect`, apply every declaration of every hit in order, then the element's inline declarations (always last). Since 0.50.0 the matched rules are kept per element for the length of one `lay_run` (`lay_hit_pool`, 131072 rule numbers, stamped with `lay_gen`; past the pool they are matched every time), and each element's ancestor bits are built once a run from its parent's (`lay_ancestors`, `lay_keys_of`). `ldoc.matched` counts elements matched, `ldoc.laid` boxes laid out.

**Inline layout** `lay_inline(L, node, parent, *y)` (563): iterative walk of `node`'s subtree with an explicit 64-entry style stack (`stack`, `stack_node`, `stack_x`, `stack_top`, `stack_slot`). For each node: pop styles whose element has been left; text ⇒ `lay_text_run` (if visible); element ⇒ `lay_style`; `display:none` ⇒ skip the subtree (600-609); `<br>` ⇒ end the line; `<img>` ⇒ if its picture arrived, size = attributes (`width`+`height`, or one of them scaling the other) else intrinsic, capped at the line width, wrapped like a tall word, LK_IMAGE carrying `link`, line fit at 100 % (a fix for `ih*ih/100`, 660-665); else its `alt` text in grey `0x6B6B6B`; controls ⇒ sized (check/radio: face+2 square; button: label width + 20 by face+10; textarea: `cols` (28) × width of `0` + 10 by `rows` (3) × (face+4) + 8; others: `size` (20) × `0` + 10), clamped to [8, line width], hidden controls take no room, LK_FIELD carrying face, colour, bg, radius, border colour and borders (≤ 8), `pen += fw + 2`, then the control's subtree is skipped (762-768); any other element ⇒ pushed (`lay_inline_open`), and an `<a href>` registers a link and sets `cur_link` (775-783). At the end remaining boxes are closed and `cur_link = -1`. Block-level descendants inside an inline run are flattened into the line.
- `lay_inline_open` (501): reserves an LK_BOX slot if the element has a background or border; `pen += max(ml,0) + bl + pl`.
- `lay_inline_close` (521): `pen += pr + br + max(mr,0)`; fills the slot (x after the left margin, y = line top, h = line height) only if the box began on this line -- "a box that started on one line and ended on another is drawn as nothing" (558-561); radius ≤ 40.

**`lay_is_block_node(L, n, parent)`** (811): element whose display is BLOCK, LIST_ITEM or FLEX. Inline-block, table-cell, inline and none are all laid out inline.

**Flex**: `lay_measure(L, node, parent, avail, *height)` (853): saves every line/list field and the output counters, lays `node` out at x = 0, returns the rightmost item edge and the height, restores everything (including `overflowed`, 872-877). `lay_flex(L, node, st, cx, cw, *y)` (908): items = up to 32 **element** children (text children are not laid out); column directions stack children as blocks with `gap` (931-938); rows: each child measured at `3*cw` (or 2000), clamped to `cw`, an explicit `width` wins; too wide ⇒ shrink proportionally with a floor of 8; spare room ⇒ given to `flex-grow` children (remainder to the last grower), otherwise distributed by `justify-content`; the row is as tall as the tallest **measured** child; each child laid out at its position and width, then shifted down for `align-items: center/end` (stretch/start/baseline stay at the top). `flex-wrap` is ignored.

**`lay_block(L, node, parent, x, avail, *y)`** (1079): style probe; none or invisible ⇒ nothing. Absolute/fixed (1086-1117): containing width `pos_w` (if > 16) else `avail`; x from `left` (`pos_x + left`) or `right` (`pos_x + aw - right - width`) else the flow x; y from `top` (`pos_y + top`) else the flow y (bottom-only stays in flow); laid out with `lay_block_placed` and `*y` restored (takes no space). Relative (1119-1133): laid out in flow, then every item it produced shifted by `left`/`-right`, `top`/`-bottom`. Otherwise `lay_block_placed`.

**`lay_block_placed(L, node, parent, x, avail, *y)`** (1138): style again; negative margins treated as 0; `box_w = avail - ml - mr`; `width`, `max-width`, `min-width` (content-box adds the horizontal frame, 1150-1169); auto left and right margins centre a box narrowed by width/max-width; min beats max; `box_w >= 16`; `*y += mt`; LK_BOX slot if background or border; content box `cx = x + ml + bl + pl`, `cw = box_w - frame` (≥ 16); list-item marker (1208-1236: decimal `N.` right-aligned 6 px left of the content, else a disc glyph slot at `cx - 18`; circle/square draw as discs); `<ul>`/`<ol>` push a list counter; a non-static box becomes the containing block (`pos_x = cx`, `pos_y = box_top`, `pos_w = cw`) for its descendants; flex containers go to `lay_flex`; otherwise children: block children via `lay_block`, runs of inline/text children share one line context (text-indent at each run start); `height`/`min-height` grow the box, `max-height` shrinks it (content not clipped, 1317-1321); `<hr>` at least its border; the slot is filled with the final border box; `*y += mb`. Vertical margins never collapse.

**`lay_run(out, d, s, x, m, inl, imgs, nimgs, width, root_px)`** (1348): resets the output, `css_view_w = width`, containing block = the page, root style = `css_default_style(root_px)`, lays out from `d->body` (or `d->root`) at x = 0. Styles on the page's `<html>` element therefore do not reach `<body>` (its parent style is the default), and `rem` always means the browser's `root_px`.

**Queries**: `lay_words(o)` (1386) counts LK_TEXT items; `lay_link_at(o, x, y)` (1397): the last **LK_TEXT** item with a link under the point (±2 px vertically); `lay_node_at(o, x, y)` (1408): the last item with `node >= 0` under the point (boxes, images, fields, bullets -- never text runs).


### 3.7 `userland/browser.c` (1746 lines) -- the program

Includes (27-38): `zelr.h draw.h ui.h web.h fetch.h dom.h css.h layout.h jsdom.h png.h jpeg.h svg.h`.

**Limits and buffers** (40-92): `SRC_MAX (1024*1024)` -- the page response (headers + body) and pages the browser writes itself; `CSS_MAX (512*1024)` -- each linked sheet and **each picture** (the same `cssbuf`, reused once sheets are parsed, 704-709); `SHEETS_MAX 6`; `SCRIPT_MAX (128*1024)`, `SCRIPTS_MAX 8` (external scripts); `REPLY_MAX (128*1024)` (XHR replies, separate "because a reply arriving must not write over the text of the script that asked for it"); `ASKS_MAX 64` (XHRs per page); `HIST_MAX 40`; `PICS_MAX 24` (123); `REFRESH_MAX 3` (312); `post_body[4096]` (349); `focus_buf[1024]` (168); `title[160]` (101); `status[URL_TEXT + 96]` (102).

**Global state**:
- page data: `src, cssbuf, scriptbuf, replybuf` (80-83); `doc` (ddoc), `sheet` (csheet), `index_` (cindex), `page` (ldoc), `match` (cmatch), `inl[DOM_NODES]` (cinline) (87-92); roughly 12 MB of static arrays in total;
- navigation: `hist_t {char text[URL_TEXT]; int scroll}` (94), `hist[40], hist_n, hist_at = -1` (95-96); `here` (url_t of the current page, 98); `reply` (response_t, 99); `go_to[URL_TEXT], load_post` (297-298); `want_go, go_is_post` (350-351); `refreshes, go_is_refresh` (313-314); `scripts_outside, asks_made` (84-85);
- view: `scroll` (103), `over_link = -1` (104), `hover_node = -1` (105), `root_px = 16` (109, the zoom), `find_item = -1` (237), `page_unhidden` (657), `title`, `status`;
- pictures: `shown {int node; picture pic}` (125-128), `pics[24], npics, pic_sizes[24] (limage), npic_sizes, pics_skipped` (130-134);
- controls: `browser_win` (160), `focus_node = -1` (162), `focus_field` (169, a `ui_field` over `focus_buf`); address bar `address[URL_TEXT]`, `bar` (1245-1246), `bar_fresh` (1262);
- scripts: `scripts_ran, scripts_changed, script_err[128], said_script_err` (448-453).

**Pictures**: `pics_drop()` (136) frees every decoded picture; `pic_of(node)` (143).

**Controls** (149-219): values live in the DOM. `field_checked(el)` (171): `checked` present and not `"0"`; `field_set_checked(el, on)` (179) writes `"1"`/`"0"` ("unchecking one has to leave something behind"); `field_value(el, kind)` (183) = `lay_control_label`; `field_set_value(el, v)` (187) writes `value`; `form_of(el)` (194) nearest `<form>` ancestor; `focus_control(el)` (203): sets `focus_node`, and for TEXT/PASSWORD/AREA copies the current value into `focus_buf` and focuses the editor (other kinds get `focus_node` but no editable field).

**Find** (221-284): `publish_text()` (243) concatenates the text of every LK_TEXT item, each followed by a space, into a static 4096-byte buffer and calls `win_set_text` (the kernel keeps `WM_TEXT_MAX 4096`, include/wm.h:77). `find_show(which, view_h)` (258): `win_find_query` (≤ 63 chars), then walks LK_TEXT items counting case-insensitive, non-overlapping matches **inside each item**; the `which`-th sets `find_item` and scrolls to `item.y - view_h/3`. The kernel counts matches in the published text the same way (kernel/wm.c:3266-3275) and sends `which = -1` to clear marks (3334-3341).

**Meta refresh** `meta_refresh(out, cap)` (316): first `<meta http-equiv="refresh">` whose `content` has `;` then optional spaces, `u`/`U`, then `=`; the URL is taken with quotes stripped. The delay is ignored; `content="5"` (no URL) is ignored (327-329).

**Forms** (286-445):
- `url_encode_into(out, cap, *at, v)` (353): unreserved kept, space → `+`, else `%XX`.
- `form_query(form, out, cap)` (372): every element in **node-index order** whose control kind is not NONE or BUTTON, belongs to `form` (`form_of`), and has a non-empty `name`; checkboxes/radios only when checked (value or `on`); others via `field_value` (so a `<select>` sends its first option's *text*); `name=value` pairs joined by `&`. Buttons never contribute (not even the one clicked).
- `submit_form(form)` (405): action resolved against `here` (empty ⇒ this page, "what a search box on a site relies on"); `method` `post` (case-insensitive) ⇒ POST; `go_to = url_text(target)` with any existing query removed (426-429); POST ⇒ `post_body = query`; GET ⇒ `go_to + "?" + query`; `want_go = 1` (navigation happens later in the loop).

**Status text**: `say(a, b)` (457), `say_more(s)` (464), `why(rc)` (471) human reasons per `WEB_ERR_*`, `why_heading(rc)` (497): `No network address` (DOWN), `That name did not resolve` (RESOLVE), `This connection was refused` (TLS), else `Cannot show this page`.

**Building a page** (506-815):
- `gather_inline_styles()` (517): for each element (indices `< doc.count` at call time) with a `style` attribute, parses `name: value;` pairs (parentheses respected) through `css_declare` into `sheet.decls`, recording `inl[i] = {at, n}`.
- `gather_inline_sheets()` (556): every `<style>` element's text children in node order ⇒ `css_parse` (no `media` check).
- `gather_linked_sheets(*fetched, *skipped)` (578): `<link>` whose `rel` is exactly `stylesheet` (case-insensitive whole string) with a non-empty `href`; at most 6 fetched (`web_get` into `cssbuf`, 2xx with a body ⇒ `css_parse`); the rest counted as skipped. No `media` check.
- `fetch_script(src, *out)` (611): the `jsdom_fetch_with` hook; ≤ 8 per page; resolved against `here`; `web_get` into `scriptbuf`; returns the length for 2xx (a truncated script is still returned).
- `do_request(method, url, body, *out, *status)` (636): the `jsdom_request_with` hook; ≤ 64 per page; resolved against `here`; a method starting with `P`/`p` ⇒ `web_post` (so PUT/PATCH are POST), else `web_get`; `*status = rc` (negative on network errors). The comment says no cookies are sent -- they are (see §10).
- `relayout(width)` (659): `match.hover = hover_node`, `match.visited_links = 0`; `lay_run`; if `lay_words == 0 && dom_has_words`, a second `lay_run` with `lay_show_hidden = 1`; `page_unhidden` = the second pass produced words.
- `accent_sheet()` (688): `a{color:#<ui_load_theme().accent>}` parsed after the UA sheet.
- Since 0.72.0: WebP too (`RIFF....WEBP` ⇒ `webp_decode`), asked for first in the Accept header; the old rewrite of `photo.jpg.webp` to `photo.jpg` is gone. An `<svg>` in the markup is laid out as a picture (`lay_drawing`: CSS width/height, else its attributes as hints in any unit, else its viewBox's proportions, else the line's width -- 300 while measured -- kept to the line; its children never laid out; the item carries the backdrop in `bg` and the text colour in `color`) and drawn by `drawing_of` from the page's tree the first time it is shown, cached by node, size, backdrop and ink (256, 12 MB), dropped on every relayout. The browser's sheet no longer hides `svg`. A picture or drawing given a box of its own (a flex or grid item, positioned) is laid out by the inline code (`lay_block_placed`). A percentage height resolves against a parent that has a height of its own (`css_parent_h`, set in `lay_style`); a percentage width is auto while measured (`cstyle.width_pct`).
- `gather_pictures()` (710): up to 24 `<img src>` in node order; `web_get` into `cssbuf`; accepted if `0 <= rc < 400` with a body; format from the bytes: `89 'P' 'N' 'G'` ⇒ `png_decode(body, len, &pic, 0xFFFFFF)`; `FF D8` ⇒ `jpeg_decode`; otherwise (after whitespace) `<` ⇒ `svg_render(body, len, width attr, height attr, &pic, 0xFFFFFF)`; failures counted in `pics_skipped` (the element falls back to alt text).
- `build(html, len, width, want_sheets, *fetched, *skipped)` (769): `jsdom_close`; `dom_parse`; `css_init`; UA; accent; `<style>` sheets; linked sheets (if `want_sheets`); inline styles; `css_index`; `pics_drop`; pictures (if `want_sheets`); reset focus, script counters and `asks_made`; `jsdom_open(&doc, &sheet)` ⇒ `jsdom_fetch_with`, `jsdom_request_with`, `jsdom_scripts`, `jsdom_loaded`, `jsdom_changed`; `hover_node = -1`; `relayout`; `find_item = -1`; `publish_text`; `title` from `doc.title`.
- `show_message(heading, body, width)` (820): a small HTML page (`body{padding:28px 32px;max-width:640px}...`) through `build(..., want_sheets = 0)` -- "the one path that draws anything is the path that is used".
- `show_plain(body, len, width)` (837): `<style>body{padding:16px}pre{font-size:14px;line-height:1.45}</style><pre>` + body with `<` and `&` escaped + `</pre>`, written into `src`.
- `number_into` (855).

**`load(address, width, keep_scroll)`** (865):
1. `url_parse` fails ⇒ "Not an address" page, status `nothing to go to`.
2. `here = u`; status `fetching <address>`.
3. `web_post(&here, post_body, src, SRC_MAX, &reply)` if `load_post` (then cleared: "going back to it afterwards asks again with GET"), else `web_get`. `here` becomes the post-redirect URL.
4. Error ⇒ an error page: for `WEB_ERR_TLS` with a reason, heading `This connection was refused` and the kernel's reason, status `refused: <reason>`; else `why_heading`/`why`. Title cleared. Return.
5. `Content-Type` starting `text/plain` or `application/json` ⇒ `show_plain` (title = path); anything else ⇒ `build` (binary types are parsed as HTML).
6. `scroll = 0` (`keep_scroll` is always passed 0).
7. Status, first match wins: `rc >= 400` ⇒ `the server said this page is not there`; `page_unhidden` ⇒ `this page hides itself until its own script rebuilds it; shown as it arrived`; `reply.truncated` ⇒ `shown as far as it fits: more page arrived than this can hold`; `doc.overflowed` ⇒ `... more markup than this can hold`; `page.overflowed` ⇒ `... more on the page than this can lay out`; else `N link(s) on this page`. Then `, N style sheet(s)`, ` (more were not read)`, `, N picture(s)` / ` (more would not show)` / `, no picture on it would show`, then `, a script stopped: <error>` or `, N script(s) ran` + ` (N from a file/files)` + `, one would not come`.
8. If `200 <= rc < 400`, `refreshes < 3` and a meta refresh names a different URL: `go_to` = it, `go_is_refresh = 1`, GET, `want_go = 1`, `, following the page's own redirect`.
9. Always last: `, encrypted` or `, NOT encrypted` (from `reply.secure`, i.e. the final hop).

**History** `push_history(address)` (1013): saves the current scroll into the current entry, drops the oldest when full (shifting 39 entries), truncates forward history, appends. Entries' `scroll` is saved but never read back.

**Drawing**:
- `tface_draw(s, x, y, str, fg, which)` (1035): the 33-face blitter (draw.h's own covers only 6 faces); baseline `y + size*4/5`; coverage 255 ⇒ `fg`, else `mix(under, fg, a)`; clipped to the surface.
- `draw_page(s, ox, oy, vw, vh)` (1060): for each item with `y = it->y - scroll` inside `[-8 - h, vh + 8]`:
  - LK_BOX: width clamped to `vw`; `round_rect` or `rect` for the background if `has_bg`; each non-zero border side as a solid `rect` in `border` colour (no styles, one colour).
  - LK_IMAGE: nearest-neighbour scaling from the RGB picture, clipped to the view on all sides.
  - LK_FIELD: checkbox/radio = filled square with a 1 px edge (`0x3B6FD6` when focused, else `0xA9A9A9`), ticked = inner square or a disc; others: inside colour = page background if set, else `0xE6E6EA` for buttons / `0xFFFFFF`; rounded (`ui_round` twice) when the page gave a radius; border colour = page's if it set any border, else the edge colour; value from the DOM **at draw time** (passwords as `*`, CR/LF as spaces), showing the tail that fits; buttons centred; text always `0x1A1A1A`; caret after the visible text when focused.
  - LK_BULLET: numbered text or a radius-3 disc.
  - LK_TEXT: find highlight `0xFFE58F` behind the whole item (1206-1212); hovered link colour `0x0842A0`; underline if `under` or hovered (extended across the space to the next word of the same link on the same line, 1225-1235); strike-through line.
- Chrome constants: `TOOLBAR_H = UI_PAD + UI_BTN_H + UI_PAD` = 46 (1243; `UI_PAD 8`, `UI_BTN_H 30`, `UI_ROW 26`, `UI_SCROLL_W 10`, ui.h:25-31).

**Address bar and search** (1245-1311): `set_address(s)` (1264) copies into the bar and puts the cursor at the end; `bar_fresh` (1248-1262) makes the first printable key after a click replace the text ("a selection in everything but drawing"); `SEARCH_PREFIX "https://lite.duckduckgo.com/lite/?q="` (1291); `set_search(what)` (1293) = prefix + `url_escape(what)`; `go_or_search(typed)` (1308).

**`main(argc, argv)`** (1313): `win_create("Browser", 860, 620)`, `win_allow_resize`; arguments joined with spaces and passed to `go_or_search` (so `browser how an operating system boots` searches), else `https://example.com/`; `push_history`; `css_view_h = 600` until the first frame; `want_load = 1`. The loop (1350-1742) is detailed in §4.2. Geometry per pass: `view_x = 3`, `view_y = TOOLBAR_H + 3` (49), `view_w = w - 6 - UI_SCROLL_W`, `view_h = h - view_y - UI_ROW - 3` (minimums 80/40); layout width `view_w - 2*UI_PAD`; for the default window: 844×542 view, layout width 828. Toolbar: `<` (x 8, w 34), `>` (x 46, w 34), `Reload` (x 86, w 62), the address field from x 154 to `w - 54`, `Go` (primary, w 40). Status bar: left = status, or the resolved URL of the link under the pointer; right = page title or `zelr`. The page is drawn into a surface cut off at the bottom of the well (1732) and the bevel redrawn afterwards; `ui_scrollbar` (display only) when the page is taller than the view.

**Keyboard** (1411-1482):

| Where the keyboard is | Key | Effect |
|---|---|---|
| Address bar (`bar.focused`) | Return | `go_or_search(address)`, push history, load |
| | Esc | leave the bar, restore the current history entry's text |
| | anything else | `ui_field_key` (Backspace, Delete, Left, Right, Home, End, printable; the first printable key after a click replaces the text) |
| A page control (`focus_node >= 0`) | Esc | unfocus |
| | Return (not a textarea) | submit the control's form |
| | Space on checkbox/radio | toggle `checked` |
| | anything else | `ui_field_key` on `focus_field`, then write `value` back to the DOM |
| The page | Down / Up | scroll ±40 px |
| | PageDown / PageUp, Space | ±(view_h − 40) |
| | Home / End | top / `page.height` |
| | `+` or `=` / `-` | `root_px` ±2 within 10..28, relayout |
| | Left / Right | history back / forward (re-fetch) |

Mouse: wheel ±48 px per step (`scroll -= steps*48`, 1511); click on a link, a control, the toolbar buttons, or the bar. There is no Ctrl+L, no Backspace-for-back, no F5, no draggable scrollbar.

### 3.8 The JS interface (`userland/jsdom.h`, 1314 lines; `userland/js.h` 1-260)

The engine (js.h) runs a language and knows nothing of pages; its `jctx` has two hooks, `host_get(J, obj, name, *out)` and `host_set(J, obj, name, v)` (js.h:214-219), and every `jobj` carries an `int host` (js.h:133-135). jsdom.h fills them in. Engine limits relevant to pages: `JS_MEM_CAP 24 MiB` (since 0.77.0 raised per page to a quarter of free memory, at most 128 MiB: `jd_mem_cap`), `JS_STEP_CAP 40 000 000`, `JS_DEPTH_CAP 160` (js.h:61-63), region allocation in `JS_CHUNK 256 KiB` pieces via `malloc`, and since 0.84.0 a conservative collector (jsgc.h, atlas 12). (The js.h header's "no regular expressions" is contradicted by jsregex.h -- the engine agent's area.)

**What browser.c calls** (all static in jsdom.h unless noted):

| Function | Line | Contract |
|---|---|---|
| `jsdom_close()` | 1125 | drop pending XHRs; if open, `js_done`, forget doc/sheet/wrappers/listeners/timers. Called first thing in `build`. |
| `jsdom_open(ddoc*, csheet*)` | 1139 | opens a world only if the page has a `<script>` or any `on*` attribute (`jd_page_scripts`, 1109); installs hooks, `document`, `window` (= the global object), `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`, `XMLHttpRequest`; allocates `jd_wrap[DOM_NODES]`. |
| `jsdom_fetch_with(fn)` / `jsdom_request_with(fn)` (non-static) | 1229 / 781 | the browser's `fetch_script` and `do_request`: jsdom knows nothing about the network or the page address. |
| `jsdom_scripts(err, cap)` | 1240 | every `<script>` element in **node-index order** (re-reading `d->count`, so script elements created meanwhile are run too), `src` ⇒ fetched synchronously at its position (in-page text ignored), else its text child; `type`, `async`, `defer` are not examined; returns how many ran; `err` = the first error (with "at line N"). |
| `jsdom_loaded()` | 1293 | `DOMContentLoaded` then `load`, dispatched with target -1: only `document` listeners run (no `window` listeners, no `window.onload`, no `<body onload>`). |
| `jsdom_changed()` | 1308 | read-and-clear of `jd_dirty` (set by any DOM write, `document.title` write, class/value/checked writes, tree mutations). |
| `jsdom_live()` | 1303 | a world is open. |
| `jsdom_timers()` | 894 | run every due timer once (repeaters rebooked from now); returns how many ran. Clock = `ticks()` at `JD_HZ 100`. |
| `jsdom_requests()` | 847 | perform **one** pending XHR via `do_request`, set `status`, `readyState = 4`, `responseText`, call `onload` (always, even on failure); returns 1 if one was made. |
| `jsdom_click(node)` | 1301 | `jd_dispatch(node, "click")`: from `node` up the parent chain running the `on<type>` attribute (wrapped as `(function(event){...})`, compiled on demand, ≤ 4096 bytes) then listeners, then `document` listeners; returns whether `preventDefault()` was called. No capture phase (208-218). |
| `jsdom_outside()` / `jsdom_outside_failed()` | 1237 / 1238 | external scripts fetched / not fetched. |
| `jsdom_error()` | 1314 | the first error text. |

**DOM surface for scripts**: `document.getElementById`, `getElementsByTagName` (known tags only; a snapshot array), `createElement`, `createTextNode`, `querySelector(All)` (selectors parsed onto the browser's own sheet and rolled back, 564-619 -- same parser and matcher as CSS), `addEventListener`/`removeEventListener`, `document.title` (read/write), `document.body`. Elements: `getAttribute`, `setAttribute`, `addEventListener`, `removeEventListener`, `appendChild`, `insertBefore`, `removeChild`, `remove`, `querySelector(All)` (descendants only); properties `textContent` (r/w; read up to 4096 bytes), `tagName`, `id`, `className` (r/w), `value` (r/w; the attribute, or a textarea's text), `checked` (r/w), `classList` (`add/remove/contains/toggle`), `parentNode/parentElement`, `firstChild`, `lastChild`, `nextSibling`, `previousSibling`, `children` (snapshot). Event object: `type`, `target`, `preventDefault`, `stopPropagation`. XHR: `open(method, url)`, `send(body)`, `setRequestHeader` (accepted and dropped), `onload`, `status`, `readyState`, `responseText`; no `onreadystatechange`, `onerror`, `getResponseHeader`. Not present: `el.style`, `innerHTML`, `location`, `window.addEventListener`, `document.cookie`, `localStorage`, `fetch`/Promises.

**Limits**: `JD_LISTENERS 256`, `JD_TYPE_MAX 24` (event-type name), `JD_TIMERS 64`, `JD_REQUESTS 8` pending (a ninth `send` is silently dropped, 821), `JD_WRAPS = DOM_NODES`, host numbers `JD_DOCUMENT 0x1000000`, `JD_CLASSLIST 0x2000000` (+ node).

### 3.8a `userland/sites.h` (since 0.61.0) -- sites read another way

YouTube and Twitch send an application, not a page. `site_youtube(u, src, n, out, cap)` reads a YouTube page's own
data (`ytInitialData`, `ytInitialPlayerResponse`) and writes a plain HTML page into `out`; `site_twitch(u, out,
cap)` asks `https://gql.twitch.tv/gql` (Client-Id `kimne78kx3ncx6brgo4mv6wki5h1ko`, body type
`text/plain;charset=UTF-8`, through fetch.h's `web_body_type`/`web_extra`) and writes the page from the answer
(`twitch_query` picks the question, `twitch_page` writes the answer, so both are testable offline).
`site_google_search(u, q, cap)` gives the words of a `google.<country>/search?q=` address, which browser.c sends to
`SEARCH_PREFIX` with a note (`build_noted`). JSON is read in place: `sj_skip` (past a value, strings and escapes
respected), `sj_find` (a key's value inside a range; a quote preceded by an odd run of backslashes is not a string
edge), `sj_str` (escapes to UTF-8, surrogate pairs joined), `sj_text` (string, `simpleText`, `runs`, `content`).
Everything written into markup goes through `sp_text` (`& < > "` escaped); ids and logins through `site_ident`
(only their own characters, so a value cannot close its attribute or GraphQL string); pictures only from
`i.ytimg.com` and `static-cdn.jtvnw.net`. Rows are flex rows (`sp_row_open`), since tables lay out as blocks.
Since 0.62.0 a watch page also shows storyboard sheets (`yt_frames`), and the browser opens on `about:start` (`show_start`, no network). Since 0.67.0 a watch page has its comments (`site_youtube_comments`, YouTube's `/youtubei/v1/next`). Since 0.65.0 YouTube's Shorts and playlists (in searches, and a playlist's own page) are read too. Since 0.63.0 Twitch has past broadcasts (`TW_VIDEO`, frames through `seekPreviewsURL` and `twitch_frames`), a channel's list of them, and search (`TW_SEARCH`). browser.c: `load()` runs the Twitch reader instead of a fetch; fetches a YouTube page into `SITE_SRC_MAX` (4 MiB,
malloc'd and freed) and builds `site_youtube`'s page when it returns one; prints `browser: <address> -- <title> --
<status>` after each page.

Since 0.96.0 a live channel's page has a link to the player (since 0.97.0 "watch", the player showing the picture;
"listen" before), `play:twitch:<login>` (the login through `site_ident` and
written only when nothing was taken out of it). browser.c does not go to a `play:` address: a click starts
`/bin/play` on what follows the colon and leaves the page where it is, so the stream's waiting is the player's
(13 §3.11). sitetest checks that a live channel offers the link and an offline one does not; the first failed with
the link left out. playcheck clicks such a link on a page from a host server, listens and looks; it failed with browser.c
not handing `play:` over.

### 3.9 Test programs in scope

- **`userland/layouttest.c`** (515): builds pages like the browser minus fetching -- `dom_parse`, `CSS_UA`, `<style>` sheets, zeroed `inl`, `css_index`, `lay_run(width, 16)` (56-83) -- and asserts on `litem` coordinates found via `box_of(node)` (LK_BOX with that node) or LK_IMAGE items; `picture_at(id, w, h)` fakes a decoded picture (51-92). No accent sheet, no linked sheets, `css_view_h` never set. Ends `LAYOUTTEST_PASS`/`LAYOUTTEST_FAIL`, exit status = failures. About 46 assertions (listed in §8).
- **`userland/pagetest.c`** (639): dom.h + jsdom.h only (no css parsing beyond query selectors, no layout): `run_scripts` = `jsdom_open(d, &qsheet)` + `jsdom_scripts` + `jsdom_loaded` + `jsdom_changed` (65-73); `pump_until(want, ms)` polls `jsdom_timers()` against `ticks()` (82-87). About 71 assertions. Ends `PAGETEST_PASS`/`PAGETEST_FAIL`.
- **`tools/browsercheck.py`**, **`tools/formcheck.py`**, **`tools/livecheck.py`** drive a real guest with the desktop (`vm.type("desktop\n")`), start `browser http://10.0.2.2:<port>/...` and compare screenshots; **`tools/webcheck.py`** drives the kernel shell's `fetch`. Details in §8.


---

## 4. Control flow and lifecycles

### 4.1 Start-up (browser.c:1313-1348)
1. `win_create("Browser", 860, 620)`; exit(1) on failure; `win_allow_resize`.
2. `argv[1..]` joined with spaces → `go_or_search` (address or DDG search); none → `https://example.com/`.
3. `push_history(address)` (hist_at 0); `css_view_h = 600`; `want_load = 1`, `dirty = 1`.

### 4.2 One pass of the main loop (browser.c:1350-1742)
1. Window size and surface; compute the view rectangle; `css_view_h = view_h`; if `view_w != laid_for` and nothing is loading, `want_width = 1` (1351-1366).
2. `ui_begin`; drain `win_poll`: `WIN_EV_CLOSE` ⇒ exit loop; `WIN_EV_FIND` ⇒ `find_show(ev.y, view_h)`; `WIN_EV_SCROLL` accumulates `scrolled`; everything goes through `ui_feed`; stop draining once 16 keys are held so the rest stay queued for the next pass (1382-1391).
3. Deferred reflow: if `want_width` and no button is down ⇒ `relayout(view_w - 16)`, `laid_for = view_w`, `publish_text` (1397-1403).
4. Every key of the pass (1411-1482; table in §3.7).
5. `jsdom_timers()`; if any ran and `jsdom_changed()` ⇒ relayout (1487-1490).
6. `jsdom_requests()` (at most one XHR, blocking); if one ran and changed the DOM ⇒ relayout (1495-1498).
7. `want_go` (form submission or meta refresh) ⇒ `load_post = go_is_post`, `set_address(go_to)`, `push_history(go_to)`, `want_load = 1` (1502-1509).
8. `scroll -= scrolled * 48`; clamp to `[0, page.height - view_h]` (1511-1516).
9. Dirty check: pointer moved, keys, press/release, wheel, scroll changed or a load pending ⇒ draw; else `sleep_ms(16)` and loop (1530-1538). Timers and XHRs (steps 5-6) run even when idle. Since 0.52.0 both sleeps are `browser_wait(win)`: `ui_due` at the page's next timer or waiting request (`jsdom_next_due`, a tick at the least so a zero timeout set from inside one cannot take the processor), then `ui_wait`; an idle page wakes about once a second (termcheck: 5 slices in five seconds).
10. `ui_load_theme()` (reads the config from disk on drawn frames).
11. If `want_load`: reset the refresh counter unless this load was the page's own refresh; draw a "fetching..." frame and `win_commit`; `load(address, view_w - 16, 0)` (blocking); put the landed URL (after redirects) in the bar and in the current history entry (1544-1569).
12. `fill` the window; hit-test the pointer if it is inside the view: `over_link = lay_link_at`, `node_under = lay_node_at` (1571-1580).
13. Hover: if `node_under` changed ⇒ `hover_node = node_under`, `relayout`, recompute `over_link` (1587-1592) -- a full relayout whenever the pointer crosses between box-bearing elements, whether or not any `:hover` rule exists.
14. JS click: on release over a node with a live world ⇒ `jsdom_click(node_under)`; if the DOM changed ⇒ relayout; the first handler error goes to the status line once; `preventDefault` ⇒ the release is consumed (1602-1619).
15. Controls (1625-1658): checkbox toggles; radio clears every radio of the same name in the same form (`form_of`) and sets itself; button ⇒ `submit_form(form_of(node))` unless `type=reset` (reset does nothing); text-like controls (and select) ⇒ `focus_control`; `bar.focused = 0`.
16. A release elsewhere (no node, or a non-control node) unfocuses the page control (1663-1666).
17. Link: release over a link ⇒ `url_join(here, href)` ⇒ bar, history, `want_load` (1668-1678).
18. Chrome: toolbar, `<`/`>` (history, re-fetch), `Reload` (`want_load` with whatever is in the bar), the address field (`ui_field_draw`; a click into it sets `bar_fresh`), `Go` (`push_history(address)` + load, **without** `go_or_search`) (1681-1710).
19. Status bar (hovered link URL overrides the status text) (1713-1724).
20. Page: `ui_well`, `draw_page` into a surface cut at the well's bottom, `ui_sunken` (bevel back on top), `ui_scrollbar` (1727-1738); `win_commit`; `sleep_ms(16)` (`browser_wait` since 0.52.0).

### 4.3 Navigation state
Flags: `want_load` (fetch `address` on the next drawn frame), `want_go` (take `go_to` next pass), `load_post` (the next load is a POST of `post_body`; consumed by that load), `go_is_refresh` (the next load came from a meta refresh; keeps `refreshes`), `want_width`/`laid_for` (reflow when the width changed and the mouse is up).

| Trigger | What happens |
|---|---|
| Return in the bar | `go_or_search(address)`, push, load |
| `Go` button | push, load (no search detection) |
| `Reload` | load the bar's current text (no new history entry) |
| `<` `>` or Left/Right keys | save scroll into the entry (never restored), move `hist_at`, load (GET, full re-fetch) |
| Link click | `url_join`, push, load |
| Return in a field / submit button | `submit_form` ⇒ `want_go` ⇒ push, load (POST if the form says so) |
| Meta refresh | ≤ 3 in a row, to a different URL ⇒ `want_go` (GET) |
| Redirects | followed inside `web_send` (≤ 6), invisible to history except that the entry is rewritten to the landed URL |

### 4.4 Building a page (build, browser.c:769-815)
`jsdom_close` → `dom_parse` → `css_init` → UA sheet → accent sheet → all `<style>` → linked sheets (≤ 6) → all `style=""` → `css_index` → pictures (≤ 24, fetched and decoded) → scripts (open world, run all, DOMContentLoaded, load) → `relayout` (possibly twice) → `publish_text` → title. Sheets, the index and `inl[]` are **not** rebuilt afterwards; script changes to classes are seen (the cascade reads the DOM), changes to `style` attributes, new `<style>`/`<link>`/`<img>` elements are not.

### 4.5 One HTTP exchange
`web_get/web_post` → `web_send` (hop loop) → `web_fetch` (retry rule) → `web_fetch_once`:

```
[ka_live && same host/port/scheme] --reuse--> send request
[else] web_drop -> connect / connect_tls --fail--> WEB_ERR_* (TLS: r->how = tls_why)
send in 1400-byte pieces --fail--> web_drop, WEB_ERR_SEND
recv loop: until Content-Length satisfied | buffer ends with 0\r\n\r\n (chunked) |
           NET_EOF | error | 3 consecutive 4-second silences | buffer full (truncated)
keep = headers found && (Content-Length || chunked) && !Connection: close
       keep -> ka_live=1 (socket stays open, including after a truncated body)
       else -> disconnect
parse status, Location, Content-Type, Set-Cookie; dechunk / trim; gunzip
```

Keep-alive is a single slot: any request to another host/port/scheme closes it first. Until 0.59.0 the kernel allowed only one TLS socket on the whole machine, so while the browser idled on an https page holding its kept one, other programs' `connect_tls` got `NET_ERR_BUSY`; any socket can be secure now. The browser still fetches one thing at a time.

### 4.6 The script world
Opened in `build` (only if the page has a `<script>` or an `on*` attribute), lives until the next `build` (any navigation, including error pages). Scripts run synchronously during `build` (external ones fetched at their position), then `DOMContentLoaded` and `load` go to document listeners. Afterwards, each main-loop pass runs due timers and at most one queued XHR; clicks are delivered on release. After each of these, `jsdom_changed()` decides whether to relayout. A handler error is reported once per page on the status line (`said_script_err`).

### 4.7 Keyboard focus
Three mutually exclusive destinations, tested in order: the address bar (`bar.focused`, toggled by `ui_field_draw` on releases inside/outside it), a page control (`focus_node >= 0`, set by clicking a control, cleared by Esc, by Return-submit, by clicking a non-control, or by a new page), otherwise the page (scroll/zoom/history keys). Clicking a control clears `bar.focused`.

### 4.8 Hover, find and the display list
- Hover: `hover_node` feeds `cmatch.hover`; only the exact element under the pointer matches `:hover` (not its ancestors), and only elements that produced a box/field/image/bullet can be "under" the pointer.
- Find: `WIN_EV_FIND(n)` ⇒ `find_show(n)` ⇒ `find_item` (an index into `page.items`) + scroll; `draw_page` paints `0xFFE58F` behind that item; `n = -1` clears. `publish_text` runs on build and on width reflows only.
- Relayout triggers: build; width change (deferred until the button is up); zoom; hover change; timers/XHR/clicks that changed the DOM. Each relayout re-runs the whole cascade for every element (style is computed up to three times per block element: `lay_is_block_node`, `lay_block`, `lay_block_placed`; since 0.50.0 the selector matching is done once per element a run and the rest is applying declarations), and every flex level measures its children with a full throw-away layout (since 0.50.0 a row that is being measured only measures, so nesting no longer doubles the work).

---

## 5. Interfaces

### 5.1 What this area provides (and to whom)

| Header / symbol | Used by |
|---|---|
| `web.h` (`url_t`, `url_parse`, `url_join`, `url_text`, `url_looks_like_address`, `url_escape`, `w_*` helpers) | `fetch.h`, `html.h`, `dom.h`, `css.h`, `layout.h`, `jsdom.h`, `browser.c`, `wiretest.c`, `jsprobe.c` |
| `fetch.h` (`web_get`, `web_post`, `response_t`, `WEB_ERR_*`, cookie jar, `web_drop`) | `browser.c`, `wiretest.c`, `jsprobe.c` |
| `html.h` (`T_*`, `HTML_TAGS`, `html_tag_of`, `html_void`, `html_char`, `html_raw_end`) | `dom.h`, `css.h` (`html_tag_of` for tag selectors), `jsdom.h` (`getElementsByTagName`) |
| `dom.h` (`ddoc`, `dom_parse`, `dom_attr*`, mutation, `dom_next`, `dom_by_*`) | `css.h`, `layout.h`, `jsdom.h`, `svg.h` (parses SVG files with `dom_parse` into a malloc'd `ddoc`, svg.h:687-699, and uses `dom_attr`, `dom_attr_fold`, `dom_tag_name`, `dom_next`), `browser.c`, `layouttest.c`, `pagetest.c`, `jsprobe.c` |
| `css.h` (`csheet`, `css_parse`, `css_declare`, `css_parse_selector`, `css_matches`, `css_color`, `CSS_UA`, …) | `layout.h`, `jsdom.h` (query selectors), `svg.h` (`css_color`, svg.h:523), `browser.c`, `layouttest.c`, `pagetest.c` |
| `layout.h` (`lay_run`, `ldoc`, `litem`, `lay_link_at`, `lay_node_at`, `lay_control_kind/label`, `CTL_*`, `lay_show_hidden`, `lay_number`) | `browser.c`, `layouttest.c` |
| The browser program | launched as `/bin/browser` from the desktop launcher (kernel/wm.c:400) or a shell (`browser <address or words>`); publishes its words to the desktop's find via `win_set_text` and receives `WIN_EV_FIND` |

### 5.2 What this area depends on

- **Kernel via sdk/zelr.h**: sockets `connect` (SYS_CONNECT), `connect_tls` (SYS_TLS_CONNECT 45), `tls_what`/`tls_why` (SYS_TLS_STATUS 46), `send`, `recv`, `disconnect`; windows `win_create`, `win_allow_resize`, `win_surface`, `win_width/height`, `win_poll`, `win_commit`, `win_close`, `win_set_text` (SYS_WIN_TEXT 56), `win_find_query` (SYS_WIN_FIND 57); `ticks()` (timers), `sleep_ms`, `exit`, `memset`; `KEY_*` codes, `KEY_CODE`, `KEY_IS_SPECIAL`.
- **Kernel behaviour relied on**: `recv` returns 0 after 4000 ms of silence and `NET_EOF (-2)` at the end (kernel/syscall.c:852-872); plain sends ≤ 1400 bytes, TLS sends ≤ 8192 (840); one TLS socket machine-wide (786-787); sockets owned per pid and released at exit (883-886); certificate verified against the host name inside `connect_tls` (802); DHCP/DNS/TCP from the network area; `JD_HZ 100` must equal the kernel timer rate (jsdom.h:680-686).
- **Userland libraries**: `alloc.h` (`malloc`/`free` for gunzip, the JS region, decoders), `inflate.h` (`inf_run`, `INF_FULL`), `png.h` (`png_decode(data, n, &picture, bg)`, `PNG_OK`, `picture {w, h, u8 *rgb}` 3 bytes/pixel, `picture_free`), `jpeg.h` (`jpeg_decode(data, n, &picture)`, `JPG_OK`), `svg.h` (`svg_render(xml, len, want_w, want_h, &picture, bg)`, `SVG_OK`), `facetext.h`/`face.h` (33 faces), `draw.h` (`surface`, `fill`, `rect`, `round_rect`, `disc`, `mix`), `ui.h` (`ui_input`, `ui_field` + `ui_field_key` + `ui_field_draw`, `ui_button`, `ui_button_primary`, `ui_label`, `ui_toolbar`, `ui_statusbar`, `ui_well`, `ui_sunken`, `ui_scrollbar`, `ui_round`, `ui_load_theme` → `accent`, `bg`, …; `UI_PAD 8`, `UI_GAP 6`, `UI_ROW 26`, `UI_BTN_H 30`, `UI_SCROLL_W 10`, `UI_KEYS 16`), `jsdom.h`/`js.h`/`jsparse.h`/`jsrun.h` (the engine).
- **Network services**: `lite.duckduckgo.com` for searches; the theme's accent colour from the settings file (via `ui_load_theme`).

---

## 6. Concurrency, locking, memory ownership, invariants

- **Single-threaded.** One ring-3 process, one loop; every network operation (page, sheets, pictures, scripts, XHR) blocks the loop. No locks. The kernel's socket table is shared with other programs; the browser holds at most one socket at a time (the kept connection) -- and it may be the machine's only TLS slot (§4.5).
- **Static ownership.** All page state lives in static arrays sized at compile time (`src`, `cssbuf`, `scriptbuf`, `replybuf`, `doc`, `sheet`, `index_`, `page`, `inl`, `hist`, `ck_jar`, `pics`). They are overwritten by the next page. Heap: decoded pictures (`picture.rgb`, freed by `pics_drop` at the next build), gunzip's temporary output buffer (freed before return), the JS region (freed by `jsdom_close`), SVG's temporary `ddoc` (svg.h).
- **Borrowed pointers**: `response_t.body` points into the caller's buffer and is valid until that buffer is reused; `fetch_script`'s returned text lives in `scriptbuf` until the next script fetch (jsdom runs it immediately); `do_request`'s text lives in `replybuf` until the next request (jsdom copies it into a JS string before calling `onload`); `lay_control_label` returns a static buffer shared by all callers (hence `form_query`'s copy into `keep`, 386-394).
- **Arenas only grow**: DOM text/attributes, CSS text/decls, layout text. Removing a DOM node never frees anything; a node count consumed by script churn is not recovered until the next page.
- **Invariants relied upon**:
  - `doc.nodes[0]` is the synthetic root `T_HTML`; `doc.body >= 0` after `dom_parse`.
  - An element's attributes are one contiguous run `attrs[attr_at .. attr_at+attr_n)`; `dom_attr_set` preserves this by moving runs (dom.h:271-279).
  - The tree has no cycles (`dom_append`/`dom_insert_before` refuse them) -- layout and `dom_next` would not terminate otherwise.
  - Rules in each `cindex` chain are in source order (built backwards, css.h:1326-1339), and `css_collect`'s insertion sort is stable, so equal specificity resolves by order.
  - `page.items` is in paint order: a box's slot is reserved before its contents (layout.h:1189-1200, 497-500), so backgrounds are drawn under text and hit tests walk backwards to find the topmost item.
  - Item coordinates are document coordinates; drawing subtracts `scroll` and adds the view origin.
  - `lay_measure` restores every piece of layout state it touches (including `overflowed`) -- any new field added to `lctx` or `ldoc` must be saved/restored there too. (Except `ldoc.laid`, since 0.50.0, which counts trial boxes on purpose; `measuring` and `measure_right` are saved and restored.)
  - Selector queries from scripts append to the live sheet and are rolled back (`nsels`, `used`, `overflowed`) -- the index never references those temporary selectors.
- **Stack**: the user stack is 64 KiB (`USER_STACK_PAGES 16`, kernel/user.c:35-49, sized "against what the work needs" for deep pages). Layout recursion depth follows the DOM depth of block elements (`lay_block` ↔ `lay_block_placed`, plus `lay_flex`/`lay_measure` frames); the parser caps that at 64, but scripts can nest deeper. `lay_inline` alone keeps ~7 KB of arrays; `lay_style` a 2.3 KB hit array per call.

---

## 7. Limits and magic numbers

| Name / value | Where | Meaning |
|---|---|---|
| `URL_HOST 128`, `URL_PATH 640`, `URL_TEXT 792` | web.h:10-12 | host, path+query, printed URL |
| `WEB_MAX_HOPS 6` | fetch.h:720 | redirect fetches per request |
| 1400 | fetch.h:554-576 | send piece (kernel plain-send cap) |
| 32768 | fetch.h:585 | recv piece |
| 3 × 4000 ms | fetch.h:590-595; kernel/syscall.c:860,869 | silence before giving up (~12 s) |
| request buffer 2048 | fetch.h:512 | `URL_PATH + URL_HOST + CK_VALUE + 512` |
| `CK_MAX 64`, `CK_NAME 96`, `CK_VALUE 768`, `CK_DOMAIN 128`, `CK_PATH 160` | fetch.h:196-200 | cookie jar |
| 16 | fetch.h:334 | Set-Cookie headers read per response |
| `HTML_TAGS` 64 known + `T_OTHER`; `T_COUNT 65` | html.h:23-77 | tag table |
| 36 named entities; ≤ 8 digits numeric; name ≤ 12 | html.h:165-201 | entity decoding |
| `DOM_NODES 20000`, `DOM_ATTRS 40000`, `DOM_ARENA 1.5 MiB`, `DOM_DEPTH 64` | dom.h:37-40 | document |
| `CSS_RULES 4000`, `CSS_SELS 9000`, `CSS_DECLS 16000`, `CSS_TEXT 512 KiB`, `CSS_MAXCLS 4` | css.h:31-35 | sheet |
| 32 | css.h:885 | selectors per rule |
| `CSS_BUCKETS 512`, `CSS_HITS 192` | css.h:1306, 1344 | index buckets, matched rules per element |
| spec `1<<20` / `1<<10` / 1 | css.h:517-606 | id / class, attribute, pseudo / tag |
| length clamp [-1, 4000]; offsets ±4000; integer part ≤ 10000 | css.h:1279-1287, 346 | lengths |
| font-size 7..96 px; `smaller` ×5/6, `larger` ×6/5 | css.h:1127-1131 | font size |
| `line_h` default 145 %, clamp 90..300 | css.h:1029, 1201-1202 | line height |
| `pt` × 4/3 | css.h:353 | points to px |
| `CSS_AUTO_OFF -32768` | css.h:64 | auto offset |
| default colours 0x1A1A1A text, 0xFFFFFF background, 0xD0D0D0 border | css.h:1004-1006; layout.h:314 | |
| `LAY_ITEMS 96000`, `LAY_TEXT 1 MiB`, `LAY_LINKS 12000`, `LAY_LINE 400` (unused), `LAY_DEPTH 64`, `LAY_FLEX_MAX 32` | layout.h:40-44, 849 | display list |
| `LAY_HIT_POOL 131072` (512 KiB), per-node `lay_hit_*` and `lay_anc` (about 560 KiB for `DOM_NODES` 20000), ancestor walk up to 96 deep | layout.h, since 0.50.0 | matched rules and ancestor bits kept for one layout |
| 33 faces: 11,12,13,14,15,16,17,19,21,24,28,34,42 (regular, bold); mono 12,13,14,15,17; mono bold 13,15 | facetext.h:26255-26289 | typefaces |
| baseline 4/5 of height | layout.h:379; browser.c:1038 | text placement |
| `3*cw` or 2000 | layout.h:948 | flex measuring width |
| 8 px | layout.h:973 | flex shrink floor |
| 16 px | layout.h:1184, 1204 | minimum block and content width |
| radius ≤ 40 (boxes), ≤ 255 (fields); field borders ≤ 8 | layout.h:541, 742-749, 1340 | |
| textarea 28 cols × 3 rows; input size 20; check/radio face+2; button label+20; field height face+10 | layout.h:685-705 | control sizes |
| bullet at content x − 18, 14 wide; decimal 6 px left | layout.h:1215-1231 | list markers |
| `SRC_MAX 1 MiB`, `CSS_MAX 512 KiB`, `SHEETS_MAX 6`, `SCRIPT_MAX 128 KiB`, `SCRIPTS_MAX 8`, `REPLY_MAX 128 KiB`, `ASKS_MAX 64`, `HIST_MAX 40`, `PICS_MAX 24`, `REFRESH_MAX 3` | browser.c:58-78, 123, 312 | per-page limits |
| `post_body 4096`, `focus_buf 1024`, `title 160`, `status 888` | browser.c:349, 168, 101, 102 | |
| publish buffer 4096 (= `WM_TEXT_MAX`) | browser.c:244; include/wm.h:77 | find text |
| find query ≤ 63 chars | browser.c:261 | |
| window 860×620; toolbar 46 px; view at (3, 49) | browser.c:1314, 1243, 1356-1359 | chrome |
| scroll: arrows 40 px; page = view_h − 40; wheel 48 px/step | browser.c:1458-1465, 1511 | |
| zoom `root_px` 16, step 2, range 10..28 | browser.c:109, 1466-1469 | |
| find highlight 0xFFE58F; hovered link 0x0842A0; field edge 0x3B6FD6 focused / 0xA9A9A9; button face 0xE6E6EA; field text 0x1A1A1A; alt text 0x6B6B6B | browser.c:1111-1118, 1211, 1221; layout.h:677 | colours |
| frame sleep 16 ms | browser.c:1537, 1741 | ~60 Hz loop (since 0.52.0 `browser_wait`: until an event, a timer or a request) |
| `SEARCH_PREFIX "https://lite.duckduckgo.com/lite/?q="` | browser.c:1291 | search |
| default page `https://example.com/` | browser.c:1346 | |
| `JD_LISTENERS 256`, `JD_TYPE_MAX 24`, `JD_TIMERS 64`, `JD_HZ 100`, `JD_REQUESTS 8`, handler source ≤ 4096 | jsdom.h:219-220, 685-686, 766, 311 | script bindings |
| `JS_MEM_CAP 24 MiB`, `JS_STEP_CAP 40 000 000`, `JS_DEPTH_CAP 160`, `JS_CHUNK 256 KiB` | js.h:61-63, 224 | engine caps |
| timers clamped to 1 h, never 0 ticks | jsdom.h:699-707 | |


---

## 8. Tests

The kernel selftest (`-append selftest`) does not touch the browser: everything here is ring 3. (The `[layout] 9 checks` line in the README's selftest listing is kernel/layout.c, the on-disk directory layout -- unrelated.)

### 8.1 `userland/layouttest.c` (ring 3, `LAYOUTTEST_PASS`)
Run by `tools/piccheck.py` (PROGRAMS, line 38) and `tools/ring3check.py` (SUITES, line 40); gate step `pictest`. About 46 assertions:
- flex row of three 60 px boxes: exist; x strictly increasing; equal y (114-136);
- the same markup without flex stacks (same x, increasing y) (142-156);
- `gap:20px` ⇒ 18..22 px between boxes (159-172);
- `justify-content:space-between` ⇒ first x < 40, second x > 400 in 600 px (178-191);
- `justify-content:center` ⇒ single box x in (200, 340) (194-203);
- `flex:1` child takes > 400 px (209-222);
- `flex-direction:column; gap` ⇒ stacked, same x (225-238);
- five 100 px boxes in 300 px ⇒ last box right edge ≤ 320 (245-259);
- `/drawn` page exactly as webserver.py serves it: `<img width=200 height=120>` with a faked 100×60 picture ⇒ LK_IMAGE 200×120 (271-304); no attributes ⇒ 100×60 (307-326);
- `box-sizing` content-box 200+2×20+2×5 = 250, border-box = 200 (335-349);
- `min-width` raises to 180, `max-width` lowers to 120, min beats max = 200 (352-368);
- absolute box at ancestor + (30, 10), and following content closes up at +40 (377-405);
- relative box moved to x = 25, next block still at y = 30 (412-424);
- `<center>` centres an inline-block's box: x in (200, 300) (434-448);
- `::-webkit-scrollbar{width:6px}` and `div:before{width:6px}` do not shrink boxes (461-476);
- `width:960px` is 960 (483-488); `width:50vw` in 600 is 300 (491-496); `height:100%` with nothing to resolve against is auto (505-511).
Not covered: text wrapping, inline edges, line heights, fonts, tables, lists, floats (absent), anything with `inl[]`, the hidden-page fallback, hit testing.

### 8.2 `userland/pagetest.c` (ring 3, `PAGETEST_PASS`)
Run by `ring3check.py` (line 36) and `piccheck.py` (line 39, whose label "a whole page parses, styles and lays out" overstates it). About 71 assertions: `dom_by_id`/tag name/text (93-100); attribute read/missing/change/longer/add-to-older-element/neighbour untouched (102-131); `dom_set_text` (133-134); whitespace rule "Gmail Images", dropped after a block, runs left for the line breaker (137-154); a script that sums 1..10, writes textContent/className/title and reports `changed` (157-191); `getElementsByTagName().length` (194-206); writing to a missing element is an error (209-223); a syntax error is reported (225-232); a page with no script opens nothing (234-240); `onclick` attribute-only page opens a world, fires on `jsdom_click`, sets dirty (249-268); listeners outlive the script, bubble from child to parent, `e.target` is the hit element (272-312); `preventDefault` ⇒ `jsdom_click` returns 1, a passive listener ⇒ 0 (316-334); `stopPropagation` (336-352); `DOMContentLoaded` on document (354-365); a throwing handler is reported and the world survives (369-382); `createElement` + `appendChild`, `removeChild`, `insertBefore`, move-on-append, cycle refused (389-461); `classList` add/remove/contains/toggle (463-477); `parentNode`/`children` (479-490); `setTimeout` 300 ms runs once, not early (498-515); `setInterval` 10 ms repeats (517-531); `clearTimeout` (533-546); `value`/`checked` read/write (550-568); `querySelector(All)` with descendant/compound selectors, element-scoped query, selector lists, and 400 queries not exhausting the sheet (575-635). Clicks are delivered by node index, so the browser's hit test (text runs have no node) is not exercised.

### 8.3 `tools/browsercheck.py` (gate step `browsertest`, 600 s)
Boots with `memory=256` and `-nic user,model=e1000`, runs `dhcp`, starts the desktop, then `browser http://10.0.2.2:<port>/`. Window geometry is recomputed from the WM's rules (lines 33-88: `WIN_X = CASCADE_X + 48`, `WIN_Y = 74`, clamped to the work area; `BACK`, `BAR`, `PAGE` rectangles from `UI_PAD 8`, `UI_GAP 6`, `UI_BTN_H 30`, `UI_ROW 26`, `UI_SCROLL_W 10`). `page_settled` waits for three identical frames 0.4 s apart. Checks (by colour counts in `PAGE` or byte-equality of the page region):
1. first page drawn (> 8 colours) on paper (> 200 000 white pixels);
2. the one link is drawn in the accent `LINK = (0x6E,0x8A,0xE8)` (the default theme preset), found by `centre_of`;
3. clicking it changes the page; the new page differs;
4. Back lands on the first page **pixel for pixel**;
5. `/measured` renders; `/framed` (same `SAMPLE` HTML, chunked in awkward 97-byte chunks) renders **identically** (dechunker check);
6. `/styled` shows the `BAND (0x1D,0x4E,0xD8)` > 3000 px; `/bare` (same markup, no sheets) differs and has < 200 band pixels;
7. `/picture` shows > 10 000 `LOGO (0xE1,0x1D,0x48)` pixels (PNG 160×90); `/missing-picture` < 100 and differs;
8. `/drawn` (SVG at 200×120) > 10 000 logo pixels;
9. `/scripted` shows the band (script set the class); `/unscripted` does not;
10. `/redirect` (302 → `/second`) renders identically to the page reached by the link;
11. `/nothing-here` (404) differs from the previous page;
12. `https://<test server>/` differs from the 404 page (the plain-HTTP server cannot complete a TLS handshake, so an error page appears).

### 8.4 `tools/formcheck.py` (gate `formtest`)
Opens `/form` (webserver.py:314-338): `#q` green `00A000`, `#spare` (no name) `00C0C0`, `#deep` checkbox `C000C0`, `#off` checkbox `808000`, a hidden `from=zelr`, submit `#go` `E08000`, and a rounded `#round` `00A0FF` (`border-radius:14px`). Checks: field, button, checkbox found by colour; hidden field not drawn (it looks for colour `0x000001`, which nothing paints -- vacuous); the rounded field's four corner pixels are not its colour; typing `hello world` changes the page; ticking `#deep` and pressing Search makes the **server** receive a GET with `q=hello+world`, `deep=yes`, `from=zelr`, no `off=`, no `spare=`/`=ignored`; the landing page has a link (≥ 60 accent pixels) to `/posts`; typing `over the wire` there and pressing Send arrives as a POST body `q=over+the+wire`. Typing uses a 0.18 s gap because the guest drops serial input it cannot drain (the docstring blames a kernel sleep that returns early).

### 8.5 `tools/livecheck.py` (gate `livetest`)
`/live-quiet` (scripts stripped): blue bands > 8000 px and < 200 px of each other colour. Then (Alt+Q closes the window; a new browser) `/live`: timer (900 ms) ⇒ ORANGE `FF8000` > 8000; external `/live.js` ⇒ VIOLET `8000FF` > 8000; XHR to `/live.txt` whose `onload` is assigned **after** `send()` ⇒ TEAL `00A0A0` > 8000 (proves the request runs on a later pass); nothing green yet; clicking the first blue band (a div with a background, so it has a box and a node) ⇒ GREEN `00B050` > 8000 (listener ran and the page was re-laid out). `PAGE = (200,120,900,620)` is a fixed screen rectangle.

### 8.6 `tools/webcheck.py` (gate `webtest`)
Uses the kernel shell's `fetch <host> <path>` (kernel/http.c, output `status %d, %d bytes of headers, %d bytes of body`, kernel/http.c:160) -- **not** fetch.h: `/size/1000` twice, `/size/200000` three times (receive-buffer walk, FIN ordering), `/size/40000`, `/slow` (2000 bytes in dribs), 404; the same on a PCnet card including 200 000 bytes; an `ne2k_pci` card is named by its ids `10ec:8029`; one fetch via the instant `guestfwd` path to 10.0.2.100. It validates the TCP stack under the browser, not the browser.

### 8.7 Tests of fetch.h itself: `userland/wiretest.c` + `tools/wirecheck.py` (gate `wiretest`)
`exec /bin/wiretest http://10.0.2.2:<port>`: `/gz` comes back decompressed ("Sent compressed" … "existed."); `/setcookie` then `/whoami` returns `sid=abc123` and `pref=dark`; `/bye` (Max-Age=0) removes `sid` but keeps `pref`; three raw sockets open at once, each gets its own answer (kernel multi-socket check). Server side: ≥ 5 GET requests over ≤ half as many connections (keep-alive), first request carried no cookie, a later one carried `sid=abc123`.

### 8.8 `tools/findcheck.py` (gate `findtest`)
Ctrl+F, type `connection` (a word on the test server's `/` page): > 60 pixels of `0xFFE58F` in the page (the browser's highlight), the find bar shows a count; `zzzqqq` lights nothing; Esc closes the bar. Single-word queries only.

### 8.9 The test server `tools/webserver.py`
`ThreadingHTTPServer` on 127.0.0.1:0, HTTP/1.1, reached from the guest as `10.0.2.2:<port>` through slirp (`HOST_IP`, line 27); `instant_args` uses `guestfwd` to `10.0.2.100:80` (one connection only). Routes (do_GET 432-560): `/`, `/index.html` (PAGE with one link to `/second`), `/gz` (gzip only if the client sent `Accept-Encoding: gzip`), `/one`, `/two`, `/setcookie` (two Set-Cookie), `/whoami` (echo Cookie), `/bye` (`sid=; Max-Age=0`), `/form`, `/posts`, `/said` (GET echo; POST at do_POST 406-413), `/second`, `/styled`, `/style.css`, `/bare`, `/logo.svg`, `/drawn`, `/logo.png`, `/picture`, `/missing-picture`, `/scripted`, `/unscripted`, `/live`, `/live.js`, `/live.txt` ("teal"), `/live-quiet`, `/big` (200 000 bytes), `/size/N`, `/framed` (chunked `SAMPLE`), `/measured`, `/chunked` (50 000 bytes in 4000-byte chunks), `/close` (no length, `Connection: close`), `/redirect` (302 → `/second`), `/redirect-relative` (301 → `second`), `/loop` (302 → itself), `/slow`, else 404. Server-side records: `COUNTS` (connections, GET requests), `COOKIES` (per GET), `RECEIVED` (forms).

### 8.10 Not covered by any test
https inside the browser (no TLS test server; `tlscheck.py` uses the kernel `fetch` against the real web; since 0.61.0 `sitecheck.py` loads YouTube, Twitch and a Google search over https in the browser), redirect method rules (307/308, POST→GET), `/redirect-relative`, `/loop`, `/close`, `/chunked`, `/big` in the browser, meta refresh, the retry-once rule, truncation paths, gzip truncation, cookie domain/path/secure rules, the cascade (specificity ties, `!important`, media queries, pseudo-classes, attribute selectors), inline edges and wrapping, lists, tables, `<select>`, radio groups, textareas, keyboard scrolling, the wheel, zoom, history scroll, hover, clicks on text, multi-word find, script `type`, the hidden-page fallback.

---

## 9. How to extend

- **A new HTML tag**: add it to the `T_*` enum and to `HTML_TAGS` at the same position (html.h:23-77; nothing checks the order despite the comment), then decide `html_void` (html.h:91), `dom_is_block`/`dom_closes`/`dom_raw` (dom.h:369-401), give it UA CSS in `CSS_UA` (a selector naming an unknown tag drops the whole selector), and, if it is a control, `lay_control_kind`. `T_COUNT` sizes `cindex.by_tag` automatically.
- **A new CSS property**: `P_*` before `P_COUNT` (css.h:39-52), a `CSS_PROPS` row (373-425) or a shorthand branch in `css_declare` (681), a `cstyle` field, **its default in `css_default_style`** (1003 -- `border_box` shows what happens otherwise), `css_inherit` if it inherits (1037), a `css_apply` case (1054), and its use in layout. Lengths should stay `clen` until layout resolves them.
- **Selectors**: recognise pseudo-classes by full name (the current length/first-letter test at css.h:556-560 lets every unknown pseudo-class match); sibling combinators need a new `CB_*`, parsing at 489-498 and a previous-element walk in `css_matches` (977); attribute value tests need a second list in `csel` (currently attribute presence is encoded as negative class offsets, 591-595). Anything not understood must return 0 (drop), never approximate -- the `::-webkit-scrollbar` and `div:before` regressions are the reason (layouttest 461-476).
- **Layout modes**: `lay_is_block_node` (811) decides block vs inline; `lay_block_placed` (1138) owns block children; an inline-block or table would need a measuring pass like `lay_flex`'s `lay_measure` (853). Every new `lctx`/`ldoc` field touched during layout must be saved and restored in `lay_measure` (field by field: struct copies are `memcpy` calls the freestanding link cannot resolve, layout.h:855-859).
- **Items and drawing**: a new `LK_*` needs a `draw_page` branch (browser.c:1060) and a decision about hit testing (`lay_link_at` only considers LK_TEXT; `lay_node_at` any item with `node >= 0`; text runs currently carry no node). Reserve background slots before contents so paint order stays correct.
- **Form controls**: `CTL_*` + `lay_control_kind`/`lay_control_label` (layout.h:168-239) + sizing in `lay_inline` (682-768) + drawing (browser.c:1108-1196) + click/keyboard handling (1411-1482, 1625-1658) + `form_query` (372).
- **Script bindings**: properties in `jd_host_get`/`jd_host_set` (jsdom.h:921-1092, set `jd_dirty` on any write the layout must see); methods via `js_set` in `jd_element` (105) or on `document` in `jsdom_open` (1162-1189); new global functions via `js_declare` on `jd_J.global`. Anything that changes styles must also refresh what the browser caches (`inl[]`, the sheet index).
- **Networking**: fetch.h is the only userland HTTP client (also used by wiretest and jsprobe). The comment at 454-458 anticipates several kept connections; the kernel still allows only one TLS socket machine-wide (kernel/syscall.c:775-787). New encodings need both `Accept-Encoding` (532) and a decoder branch (686-699).
- **Pitfalls stated in comments**: never re-send a POST after 301/302/303 ("how somebody orders twice", fetch.h:742-748) or retry a request that got any answer (446-449); http→https on the same path is not a loop (733-736); omit the default port from `Host` (518-520); keep `publish_text`'s buffer equal to `WM_TEXT_MAX`; `JD_HZ` must match kernel/timer.c (jsdom.h:680-686); keep at most 16 keys per frame and leave the rest queued (browser.c:1382-1391); reflow only once dragging stops (1395-1396); draw the chrome before the page so a torn frame still has its toolbar (1518-1529); XHR must complete on a later pass (jsdom.h:759-764); a relayout per frame for hover must be avoided (browser.c:1582-1586).

---

## 10. Doc drift and suspicious code

### 10.1 Likely bugs (verified by reading; not run)

1. **Inline skip escapes its subtree -- content laid out twice** (layout.h:600-608 and 762-767). `lay_block_placed` calls `lay_inline(child)` for every non-block child (layout.h:1286-1287), including `display:none` elements (`<script>`, `<style>`, `<noscript>`, `<svg>`, `<iframe>`, hidden elements) and form controls (including `<input type=hidden>`). Inside `lay_inline`, when `at == node` and `node` has no next sibling, the climb `while (skip >= 0 && next < 0 && parent != node) skip = parent` rises above `node` (its parent is never `node`), `at` becomes the next sibling of some ancestor, and the walk continues through the rest of the document, laying it out inline on the current line; the ordinary block walk then lays it out again. The `if (skip == node) at = -1;` guard only covers the case where `node` has a next sibling. Pretty-printed HTML hides this (a trailing whitespace text node follows the element, dom.h:521-535); tight or minified markup such as `<div><b>x</b><script>…</script></div><p>…` or `<form><input name=q><input type=submit></form><p>…` triggers it. Fix: `if (at == node) { at = -1; continue; }` before the climb.
2. **FIXED in 0.74.0 (`lay_link_open` in `lay_block_placed`; layouttest, formcheck): block-level and flex-item links are not links** (layout.h:775-783 is the only place a link is registered, and only when `lay_inline` pushes an `<a>`). An `<a>` with `display:block`, or any `<a>` that is a flex item (flex items are laid out through `lay_block`, layout.h:1029), produces text with `link = cur_link = -1`: not clickable, no hover URL. Common in navigation bars and cards.
3. **`cstyle.border_box` is never initialised** (css.h:1003-1032 omits it; `css_inherit` does not copy it). `lay_block_placed` reads it (layout.h:1165-1169, 1308) from a stack `cstyle`, so for any element without an explicit `box-sizing` declaration the width/height/min/max frame adjustment depends on stale stack contents. layouttest sets `box-sizing` explicitly in the one test that measures it (336-348).
4. **Relative font sizes compound across the cascade** (css.h:1126-1132 resolves `em`/`%`/`smaller`/`larger` against the current `st->font_px`, which earlier-applied rules have already changed; layout.h:475-480 applies every matching rule in order). UA `h1{font-size:2em}` plus a page `h1{font-size:1.5em}` gives 3em. Duplicate hits make it worse: an element whose `id` and a class hash to the same bucket, two classes colliding in the 512 buckets, or a repeated class token, walks that chain twice (css.h:1377-1399) and applies the same relative size twice.
5. **`rgba(…, .5)` is opaque** (css.h:287-298): `any` is set only by integer digits, so an alpha written without a leading zero (the minified form) breaks out before `c[3]` is stored and alpha stays 255 -- `rgba(0,0,0,.1)` borders and overlays draw solid black.
6. **`font:` shorthand takes a numeric weight as the size** (css.h:753-760): the first part that `css_len` reads as a positive length wins, and a unitless `400`/`700` is a length in px, so `font: 700 14px/1.4 sans-serif` sets `font-size:700` (clamped to 96 px).
7. **Named colours are only found as the last token of a multi-part value** (css.h:311-312 uses `w_same_fold`, a whole-string compare, on slices from `css_parts` at 723, 783 and 1267 that are not NUL-terminated): `border: solid black 1px` loses its colour; `background: white url(x.png)` is dropped. Hex and `rgb()` are unaffected.
8. **Unknown pseudo-classes match everything** (css.h:556-576): anything argument-less that is not hover/link/visited/root/11-letters is accepted with no test, so `li:last-child{border:0}`, `a:focus{…}`, `input:checked{…}` apply unconditionally. And **`:visited` matches every link** (css.h:959-962): `cmatch.visited_links` (937) is never read, although browser.c:661 sets it to 0 expecting `:visited` to match nothing -- `a:link{…} a:visited{purple}` colours every link purple.
9. **Negative lengths become `-1`, which means auto** (css.h:1285-1287): negative margins are lost (`lay_block_placed` treats `< 0` as 0, layout.h:1146-1147), and a box with both side margins negative and a width is centred as if `margin: auto` (1174, 1178).
10. **The page is not clipped at the top of the view** (browser.c:1060-1080, 1727-1735): `draw_page` draws every item overlapping `[-8, vh+8]` into a surface cut only at the bottom; `rect`/`round_rect`/`tface_draw` clip to the window, not the well. The toolbar is drawn before the page in the same frame, so a background box (e.g. a body or wrapper with a background colour) scrolled partly above the view paints over the toolbar, and text straddling the top edge leaves fragments above the well. `ui_sunken` only restores the bevel. Images are clipped correctly (1092-1097).
11. **Flex rows are as tall as a measurement taken at triple width** (layout.h:948, 1021, 1037, 1048): children are measured at `3*cw`, laid out at their final (narrower) widths, but the row height is the tallest *measured* height; a child whose text wraps more at its real width overflows into what follows. Also in measurement: a child with a background or border reports the full `3*cw` width (its LK_BOX spans `avail`, 1329-1331), so it asks for the whole row; a child without one reports its right-most *content* edge, excluding its right padding/border.
12. **FIXED in 0.74.0: flex drops text children**; items past 32 (now 128) are laid out in a column under the row, `display:none` items are not items and `flex-wrap` wraps (since 0.69.0). Words straight inside a row or a grid are an item of their own (`lay_items_next`, laid out by `lay_block` as a line); layouttest.
13. **FIXED in 0.74.0 (`lctx.word_node`; `:hover` reaches ancestors, `css_hover_reach`): text runs have no node, so clicks and `:hover` target the nearest box-bearing element** (`lay_word` never sets `it->node`, layout.h:405-418; `lay_node_at` skips `node < 0`, 1408-1416). `jsdom_click(node_under)` bubbles *up* from that element (jsdom.h:360-366), so an `onclick` on a `<div>`/`<span>`/`<a>` without a background or border is never reached when its text is clicked (the tests click by node or on a coloured band). `:hover` likewise applies only to box-bearing elements, and only to the element itself, not its ancestors (css.h:959).
14. **FIXED in 0.74.0 (`lay_link_at` takes pictures and boxes): image links are not clickable** (layout.h:1400: `lay_link_at` ignores LK_IMAGE, although images carry `link`, 658). Logos wrapped in `<a>` do nothing.
15. **FIXED in 0.74.0 for `type="button"` (formcheck): every button submits** (browser.c:1649-1653): `<button type="button">` and `<input type="button">` (all mapped to `CTL_BUTTON`, layout.h:187, 199-201) call `submit_form` unless a click handler prevented it; `type=reset` does nothing.
16. **The Go button skips search detection** (browser.c:1706-1710): only Return runs `go_or_search` (1421); Go loads the raw bar text, so words typed then Go try DNS on "operating system".
17. **Stale inline styles** (browser.c:517-553): `inl[]` is reset only for indices `< doc.count` at build time, before scripts run. Nodes created by scripts later (higher indices) read `inl[]` entries left from a previous, larger page -- declaration ranges into the current sheet's arrays, i.e. wrong styles. Style attribute changes made by scripts (`setAttribute('style', …)`) are never re-parsed.
18. **`<style>` sheets always precede linked sheets** (browser.c:778-779): source order is not document order, so a page's override `<style>` placed after its `<link>` loses equal-specificity ties.
19. **`show_plain` writes into the buffer it reads** (browser.c:837-853): `reply.body` points into `src` (load() fetched into `src`, 883-885), and the expanded output (a 74-byte header, `<`→`&lt;`, `&`→`&amp;`) is written to `src[0…]`. The writer gains 3-4 bytes per escaped character, so once those gains exceed the slack (header length − 74) it overwrites input not yet read: text/plain or JSON bodies with many `<`/`&` display garbled. (Bounds are respected; no crash.)
20. **`host?query` produces a request target without `/`** (web.h:205): the `/` is prepended only if the next char is neither `/` nor `?`, so `http://example.com?x=1` requests `GET ?x=1 HTTP/1.1`.
21. **Query-only and query-bearing relative links resolve wrongly** (web.h:263-274): the "directory" is up to the last `/` anywhere in `path`, query included, and `?page=2` replaces the last path segment rather than just the query: base `/w/index.php?title=A` + `?title=B` ⇒ `/w/?title=B`; base `/a/b?next=/c/d` + `e` ⇒ `/a/b?next=/c/e`. Pagination links (`?page=2`) break unless the path ends in `/`.
22. **Relative links with an early colon are rejected** (web.h:246-249): any `:` in the first 12 chars before a `/` is taken as a scheme (`?t=10:30`, `a:b.html`).
23. FIXED in 0.76.0 -- an answer is kept only when it reached the end it promised (`whole`: its length, the last chunk, or the close), and one that stopped short is marked (`r->cut`; the browser says "shown as far as it came", a cut script is not run, a cut sheet is asked for again). wiretest: an answer that stops short says so, and the answers after it are their own. The original finding: **A truncated response keeps its connection** (fetch.h:584, 635-646): when the buffer fills before the body ends (`truncated`), a Content-Length or chunked response is still kept alive; the unread remainder is what the next request on that connection reads first. The retry rule rescues it only when that garbage produces `WEB_ERR_EMPTY` (no status); `WEB_ERR_HEADERS` is returned as is.
24. **A truncated gzip stream discards everything** (fetch.h:176): only `INF_FULL` keeps the decoded front; `INF_TRUNCATED` (stream cut short) fails the whole fetch with `WEB_ERR_ENCODING`, contrary to the "keep the front of the page" philosophy stated at 170-173.
25. **Chunked end detection is a tail heuristic** (fetch.h:617-629): it stops when the buffer *ends* with `0\r\n\r\n` -- a chunk whose data ends in `0\r\n` arriving exactly at a read boundary looks like the terminator (truncated page); a bare-LF terminator is never seen (waits ~12 s).
26. **Cookie scope and script access** (fetch.h:219-226, 294-298, 536-541; browser.c:626-635): `Domain=com` is accepted from `example.com` (no public-suffix check); cookies are attached to every request including sheets, pictures, scripts and XHR; there is no same-origin check on XHR and responses are readable by the page -- so a page's script can read another site's responses made with that site's cookies. The comment claiming "no cookies are sent with it" is false.
27. **Wheel direction looks inverted** (browser.c:1511 `scroll -= scrolled * 48`): the kernel documents wheel steps as positive downward (include/wm.h:43-45; selftest "two fingers moved down the pad scroll down" expects +2, kernel/selftest.c:2804-2805), and every other app adds (`music.c:388`, `monitor.c:302`, `settings.c:790`, `files.c:509`, `notes.c:282`; `term.c:1835` subtracts only because its `view` counts from the end).
28. **Field caret and textarea** (browser.c:1190-1194): the caret is drawn after the visible text even when `ui_field_key` moved the cursor (Left/Home); a textarea shows one line (newlines drawn as spaces, 1169) and Return cannot insert a newline (`ui_field_key` ignores keys < 32).
29. **Radio via keyboard** (browser.c:1448-1451): Space toggles a radio on or off without clearing the others of its name.
30. **`<select>`** : it shows and submits its first option's *text* (layout.h:220-229; `form_query` via `field_value`), `value` attributes of options are ignored, and it cannot be changed; focusing one and typing writes `value=""` onto the select (browser.c:1452-1455, since `focus_field` is not focused and `field_set_value` runs anyway).
31. **Find only works for single words in the first 4 KB** (browser.c:243-284): the published text is capped at 4096 bytes (so is the kernel's copy), and `find_show` matches inside individual word items, while the desktop counts matches in the space-joined text -- a query with a space is counted by the desktop but never highlighted or scrolled to. `publish_text` is not re-run after script-driven relayouts.
32. **History does not restore scroll** (browser.c:1014-1026, 1471, 1476, 1684, 1691 save `hist[].scroll`; nothing reads it; `load()`'s `keep_scroll` is always 0).
33. **Media handling** (browser.c:589 and 556-569 ignore `media` on `<link>`/`<style>`, so print sheets apply; css.h:841-852 opens every non-print `@media`, including `max-width` mobile overrides and `prefers-color-scheme: dark`, which then win by source order; `@import` is never fetched; `@layer` blocks are skipped whole).
34. **FIXED in 0.42.0** (`dp_pop_to_name` closes back to the nearest open `T_OTHER` element with the same lower-cased name; layouttest "the end tag of an unknown element closes it", svgtest "a group's paint stops at its end tag"; with paint now inherited, the old parse would also have leaked a group's fill into every later sibling). **Unknown elements never close** (dom.h:566 ignores close tags of `T_OTHER`): `<my-el>a</my-el> b` puts `b` inside `my-el`; `<tbody>`, `<u>`, `<sup>`, custom elements accumulate following siblings as children until a known ancestor closes. This also feeds svg.h, which parses SVG with `dom_parse` and composes transforms up the parent chain (svg.h:753-762): after `<g transform=…>…</g>`, later siblings become children of the `<g>` and inherit its transform.
35. **Unquoted attribute ending in `/` self-closes** (dom.h:640): `<a href=/>Home</a>` or `<a href=https://x.com/>` is treated as `<a/>` and its text is not a link.
36. **`<` not followed by a letter starts a tag** (dom.h:571-591): text like `a < b` or `<3` creates a nameless/odd element that swallows text up to the next `>` and stays open.
37. **`&#0;` writes a NUL into the text arena** (html.h:176, 216 → `html_fold_cp(0)`), truncating that text node; other control code points are passed through.
37b. Since 0.77.0 a `<template>`'s children are moved into its content fragment when scripts open (`jsdom_open`, `jd_template_content`), so no script sees them as children: made on the first read of `.content`, they had been children until then, and Alpine's walk went into them. Also since 0.77.0, scriptdump prints the page's console output.
38. **Script `type` is ignored** (jsdom.h:1248-1281): `application/ld+json`, templates and `type=module` are run as classic scripts; the first error recorded (`jd_note_error` keeps only the first) can mask a real one.
39. **`load` events go to `document` only** (jsdom.h:1293-1297 dispatch with node -1): `window.addEventListener('load', …)` throws (window is the global object without that method), `window.onload` and `<body onload>` never run; XHR supports only `onload`.
40. **`visibility:hidden` removes the box and its space** (layout.h:1084, 1144), unlike CSS; `opacity` below 0.2 does the same (css.h:1189-1194).
41. **`transparent` paints opaque white** (css.h:231, 1059-1060 set `has_bg`): a transparent button on a coloured header becomes a white box.
42. **`rem` and `html` styles** (layout.h:1374-1379): layout starts at `<body>` with a default parent style, so `html{font-size;color;background}` never reaches the body, and `rem` is always the browser's `root_px` (16 by default), e.g. `html{font-size:62.5%} body{font-size:1.6rem}` renders at 25.6 px.
43. **Greedy descendant matching** (css.h:991-995): the nearest matching ancestor is taken without backtracking, so `A > B C` fails when the nearest `B` is not a child of `A` but a farther one is.
44. **The kept TLS connection blocks other programs** (fetch.h:635-646 keep; kernel/syscall.c:786-787): after the browser loads an https page it holds the machine's single TLS slot until it navigates elsewhere or exits; `get https://…` in a shell then fails with BUSY.
45. **Dead or no-op code**: `int ax = *y >= 0 ? x : x;` (layout.h:1091); `L->list_count[d] = L->list_count[d];` (1237-1238); `case 0x00D7` unreachable (html.h:138); `LAY_LINE` (layout.h:43), `css_put_lower` (css.h:205), `dom_get` (dom.h:89), `ck_forget_all` (fetch.h:213 -- cookies cannot be cleared); the whole flat parser in html.h (44-59, 245-445); UA selectors `fieldset,address` (unknown tags ⇒ dropped) and the duplicate `center` rule (css.h:1427, 1432/1467); `WEB_ERR_CONNECT` in the retry set cannot occur on a reused connection (fetch.h:711).
46. **Other gaps worth knowing**: `inline-block` and `table-cell` are laid out as plain inline (width/height/vertical padding ignored; layout.h:811-822); block elements inside an inline run are flattened; `!important` is stripped but not honoured (css.h:683-695); no margin collapsing; `text-align:justify` is left; italic, `LS_CIRCLE`/`LS_SQUARE` and `pre` underlines are not drawn; binary content types are parsed as HTML (browser.c:904-913); 5xx pages say "not there" (920); images > 512 KB fail (they share `cssbuf`); external scripts > 128 KB run truncated (611-624, `truncated` not checked); the window title is not updated when a script changes `document.title` after load; a page nested deeper than 64 reports "more markup than this can hold" although kernel/user.c:40-41 calls 100-deep nesting ordinary.

### 10.2 Comments and docs that disagree with the code

- fetch.h:426-431 -- "a program here has no allocator": alloc.h is included (17) and `wh_gunzip` mallocs (167).
- fetch.h:525-528 -- "identity, because there is no decompressor here … Close, because this makes one request per connection": the code sends `Accept-Encoding: gzip` and `Connection: keep-alive` (532-533).
- css.h:14-16 -- positioning listed as absent (it is implemented: css.h:1178-1184, layout.h:1086-1133); css.h:18 -- "Nothing is allocated, here or anywhere in this program" (the browser allocates for JS, gunzip, pictures).
- layout.h:9-16 -- "absolute and fixed positioning, flex and grid tracks" listed as deliberately absent; flex (908-1049) and positioning are implemented and tested.
- README.md:1150-1153 -- "What is still absent … floats, positioned boxes, table column widths and grid": positioned boxes exist.
- README.md:787-788 -- lists forms support; `<select>` is drawn but inert. README.md:1155-1158 says bodies are "asked for compressed" -- gzip only (the brief's "gzip/deflate" is not what the code does).
- dom.h:117 and jsdom.h:46-47 -- "twelve thousand" nodes; `DOM_NODES` is 20000 (dom.h:37).
- dom.h:288-292 -- "Everything that traverses the tree uses this rather than recursion": `lay_block`/`lay_block_placed`/`lay_flex`/`lay_measure` recurse.
- html.h:1-13 -- "Not a tree … the layout below needs": describes the dead flat parser; html.h:62-64 -- the tag-table check "at the bottom of this file" does not exist.
- web.h:97-106 -- the https-default comment sits above `url_looks_like_address` but describes `url_parse`.
- browser.c:115-117 and tools/browsercheck.py:207-209, tools/wirecheck.py:6-8 -- "one connection at a time is all this kernel's TCP does": the kernel has a socket table (kernel/syscall.c:700-745); only TLS is single. browser.c:481-482 (`WEB_ERR_BUSY` text "the one connection this machine has") has the same drift.
- browser.c:628-632 -- XHR "no cookies are sent with it": they are (fetch.h:536-541).
- browser.c:865 -- `keep_scroll` parameter is always 0; the history `scroll` field is write-only.
- sdk/zelr.h:429-431 -- `connect_tls`: "Zero means the connection is open": it returns the socket handle (kernel/syscall.c:808), which fetch.h relies on.
- tools/browsercheck.py:387-392 -- "an address it cannot speak … https says it cannot do that": https is supported; the check passes only because the test server has no TLS.
- tools/formcheck.py:184-186 -- the hidden-field check searches for colour `0x000001` and can never fail.
- tools/piccheck.py:39 -- pagetest "parses, styles and lays out": it does neither styling nor layout.

---

## 11. Open questions

1. Does the wheel really scroll backwards in QEMU/hardware (10.1 #27), or does the PS/2 path deliver the opposite sign from the documented one?
2. Has the uninitialised `border_box` (10.1 #3) shown up as layout that changes between relayouts (e.g. on hover)? A one-line fix in `css_default_style` would settle it.
3. Does the top-edge overdraw (10.1 #10) appear on real pages? Nothing in the WM or `ui_well` clips it as far as the code shows.
4. How often does the inline-skip escape (10.1 #1) fire on real (minified) pages? It would show as duplicated text and a doubled page height.
5. Is the flat tokenizer in html.h meant to be removed, or kept for something planned?
6. fetch.h:454-458 plans several connections "a layer above this one" (a page and its pictures at once). Is that still intended, given the kernel's single TLS session state?
7. Should XHR/sub-resource requests carry cookies at all without an origin check (10.1 #26)? The code comment assumes they don't.
8. `JD_HZ 100` is asserted to match kernel/timer.c; the kernel area should confirm the tick rate.
9. The search engine is an external dependency (`lite.duckduckgo.com`); is there a fallback if it changes its markup or blocks the user agent `zelr`?
10. What exactly `tls_what`/`tls_why` print, and whether `tls_status`'s machine-wide "last handshake" can report another program's handshake to the browser (kernel TLS area).
11. Whether `svg.h` compensates for dom.h's never-closing unknown elements (10.1 #34) -- the SVG agent should confirm the `<g>` transform leak.
