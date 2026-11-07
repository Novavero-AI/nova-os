# The toolkit

The libraries a program draws its interface with: the rasterizer, the
font and its format, text layout, the interface as data and the
renderer that draws it, widgets, layout, themes and accessibility; and
the first programs built on them, the HUD, Files, Settings and the
terminal. The display's services and protocols, which every graphical
program meets whether it uses these libraries or not, are
[display.md](display.md)'s, which also gives the build plan and why the
two are separate documents.

Each item gives the design, why, what it costs, and the main
alternative not taken. Items the owner has ruled on are marked
**Decided**; every other item is **Proposed**. The shell's
specification, `shell.md`, proposed separately, is cited in
plain text by section, as "shell.md L7".

Sources are pinned in [README.md](README.md) and cited by these names:

| Name | Document |
|---|---|
| OpenType | The OpenType specification 1.9.1 (Microsoft Learn), by table: `glyf`, `CFF2`, `GPOS`, `cmap`, `hmtx`, `OS/2`, and "TrueType fundamentals" |
| Apple TrueType | Apple's TrueType Reference Manual, by chapter |
| Inter | Inter 4.1, its release and `LICENSE.txt` |
| OFL | The SIL Open Font License 1.1, by condition, and its FAQ, by question |
| FreeType | FreeType's "On slight hinting, proper text rendering, stem darkening and LCD filters", and its Glyph Conventions, section VI |
| UAX #9 | Unicode Standard Annex #9, the Unicode Bidirectional Algorithm, revision 52 |
| HTML, Adaptive Cards, Arcan, Dear ImGui, Flutter, RemoteViews, Plan 9 | The systems under [Display and interface design references](README.md#display-and-interface-design-references) |

## U1. The layers

**Proposed.** Each layer is a library in C, integer arithmetic only, that
a program links or leaves; each uses only the layers above it in the
table.

| Layer | Where | What | Used by |
|---|---|---|---|
| Pixels | `gfx/` | sRGB and linear light, the strokes, filled outlines ([U2](#u2-the-rasterizer)) | nova-boot, the kernel, the console, the compositor, the renderer |
| Fonts | `lib/font/` | The font format's reader and a cache of glyph masks ([U3](#u3-fonts)) | The compositor, the renderer |
| Text | `lib/text/` | UTF-8 to positioned glyphs and lines ([U4](#u4-text-layout)) | The compositor, the renderer |
| Values | `lib/value/` | The shell's value encoding (shell.md L7, rule 9) | The shell, the HUD, the renderer, programs |
| Display | `lib/display/` | The display protocol's client ([display.md D5 to D7](display.md#d5-connections-labels-and-layers)) | Every graphical program |
| Interface | `lib/ui/` | The interface as data: nodes, layout, widgets, themes, drawing and input ([U5](#u5-the-interface-as-data) to [U10](#u10-accessibility)) | The renderer, and login, the powerbox and the HUD in their own processes |

The compositor is a client of the pixel, font and text layers like any
program, and of the interface layer's token reader alone: it draws the
strip's label with the same text layout, and the frames and the pointer
with the same rasterizer, and it uses no widget. `gfx/` stays where it
is because nova-boot and the kernel build it, and no image that draws
links a C runtime (`gfx/gfx.h`).

- **Why:** a game that draws every pixel itself links `lib/display`
  and nothing above it; a program with a dialog links the interface
  layer; the system's own interface is built from the same calls a
  third party's is (DESIGN.md, principle 3).
- **Cost:** six libraries with stable interfaces to keep.
- **Not taken:** one toolkit library holding everything. Its cost is
  the compositor and a game carrying widgets they never use.

## U2. The rasterizer

**Proposed.** `gfx/` keeps its strokes, the capsules, rings and arcs of
the wordmark and the console font, and adds filled outlines.

1. **Outlines.** A path is contours of segments, each a line, a
   quadratic Bezier or a cubic Bezier, in 256ths of a pixel. TrueType's
   quadratics come with implied points: "Two successive conic 'off'
   points force the rasterizer to create ... a virtual 'on' point
   inbetween, at their exact middle" (FreeType, Glyph Conventions,
   1.a). Cubics are for the theme's icons and for fonts with cubic
   outlines (OpenType, `CFF2`).
2. **Flattening.** A curve is split at its midpoint, in integers, until
   each piece lies within `FLATNESS` (a sixteenth of a pixel) of its
   chord, at most `SPLIT_DEPTH_MAX` (16) times, and drawn as lines.
3. **Nonzero winding.** "A point is considered to be an interior point
   of a glyph if it has a non-zero winding number" (OpenType, TrueType
   fundamentals), and Apple TrueType, chapter 1, gives the same rule.
   Each pixel row is
   `SUBSCANLINES` (16) rows of samples; on each, the lines' crossings,
   each with its direction, give the spans where the winding number is
   not zero, exactly, and each span adds its horizontal extent, in
   256ths of a pixel, to the pixels it covers. A pixel's total over its
   sixteen rows is 0 to 4,096, and its coverage is that total times
   `OUTLINE_COVERAGE_MAX` (255) over 4,096, rounded to the nearest.
4. **Linear light.** Coverage scales light, not codes, as the strokes'
   does (`gfx/raster.c`): a pixel an outline covers in part emits that
   share of the ink's light over the rest of the background's, decoded,
   mixed and encoded with the same tables.
5. **Checked on the host.** A prototype in Python, in the same integer
   arithmetic, renders every glyph of the font at several sizes, and
   `design/outline/check-outline.sh` compares the C port's masks with
   it byte for byte, as `check-limb.sh` and `check-font.sh` do.

- **Why exact nonzero on each row:** Inter's TrueType outlines hold
  composite glyphs whose contours overlap in the same direction (the
  stroke of U+0141, L with stroke, over its L, measured with fontTools),
  and "Black filled areas when placed over other black filled areas will
  remain black" (Apple TrueType, chapter 1); a rule that is exact on
  every row of samples fills them as the font means.
- **Why linear light:** "The correct way of rendering a glyph image on
  a surface is to alpha blend it onto the surface in linear space and
  then apply gamma correction" (FreeType), and the rest of the system
  mixes that way, so text and shapes agree at their edges.
- **Cost:** sixteen rows of crossings a pixel row, where an exact area
  computed per pixel takes one pass; text blended in linear light
  "thins out, making it harder to read" at small sizes (FreeType),
  which [U4](#u4-text-layout), rule 7, addresses.
- **Not taken:** the exact area of each pixel accumulated from signed
  edges in one pass. Its cost is a winding
  number summed over a pixel rather than tested at each sample, so a
  pixel half under two overlapping contours counts as fully covered,
  which Inter's overlapping composites would show at every overlap's
  edge.

## U3. Fonts

**Decided** (by the owner): Inter, under the OFL 1.1, is the interface's
typeface, rendered by nova-os from its outlines; it is converted at build
time into nova-os's font format; the code points converted are Latin,
Greek and Cyrillic with punctuation and currency, 1,025 of them; and two
static weights start, settled by the approved mockups. **Proposed:** the
rest.

**The source.** Inter 4.1, the release at tag `v4.1` (commit
`e3a3d4c57d5ecc01453a575621882a384c1995a3`), fetched by its SHA-256 as
the test disk's seed files are. Its static TrueType fonts,
`extras/ttf/`, have quadratic outlines in `glyf`, 2,048 units to the em
and 2,937 glyphs; the regular weight maps 2,852 code points and is
411,640 bytes, hinted with ttfautohint (Inter's `Makefile`).

**What is converted.** Latin, Greek and Cyrillic with general
punctuation and currency, 1,025 code points, the scripts
[U4](#u4-text-layout) can lay out; and two static weights to start, the
two the approved mockups settle on.

**The format.** `tools/mkfont.c`, a C tool the chain compiles, as it
compiles `tools/mkstore.c`, reads the pinned TrueType fonts at build
time and writes, for each weight the theme uses, a nova-os font file of
fixed little-endian records (`include/font_format.h`):

| Record | What it holds |
|---|---|
| Header | `NOVAFONT`, a version, the units to the em, the ascender, descender and line gap (from `OS/2`'s typographic values, since Inter sets `USE_TYPO_METRICS`), the x-height and cap height, the counts and offsets of the records below |
| Map | Sorted ranges of code points, each to its first glyph |
| Glyphs | For each glyph its advance (`hmtx`) and its outline: contours of points, each two signed 16-bit coordinates and an on-curve flag; composites flattened into their components' contours |
| Kerning | Classes of first and second glyphs and a matrix of advance adjustments in font units, resolved at build time from `GPOS` for the glyphs kept |
| Notice | Inter's copyright line and the OFL's text, as condition 2 requires |

The reader takes a file only whole and exact, as the accounts service
takes a record ([accounts.md A3](accounts.md#a3-records)): every count
and offset is checked once when the font is loaded, against the file's
size and each other, and a glyph is then read with no length to follow.
Instructions, `GSUB` and every `GPOS` feature but `kern` are left out.

The trade-offs:

| | Converted at build time | Parsed at run time |
|---|---|---|
| What is audited | The reader of fixed records, run by every program; the converter's TrueType parser runs once, on a file pinned by its SHA-256 | A TrueType parser in every program that draws text, run on whatever font it is given: tables of offsets and lengths, composites that "may be nested within other composite glyphs" (OpenType, `glyf`) |
| Size | Outlines, metrics and kerning for the glyphs kept; for comparison, Inter's regular weight cut to U+0020 to U+017F with kerning and no hinting is 35,028 bytes as TrueType (fontTools 4.68.0's `pyftsubset`), and to Latin, Greek and Cyrillic 87,136 | The fonts whole, 411,640 bytes a weight, or a subsetting tool in the build |
| Hinting | None: Inter's 76,964 bytes of instructions in 1,965 glyphs, and its 3,596-byte `fpgm`, are dropped | Possible only with TrueType's interpreter, a stack machine whose "Instructions associated with a glyph are executed every time that glyph is requested" (Apple TrueType, chapter 2) |
| Kerning | Resolved by the converter: in U+0020 to U+017F, 12,112 ordered pairs of Inter's 317 glyphs have a nonzero adjustment, from Inter's `kern` lookups, an extension lookup and pair adjustments of both formats with a mark filtering set. Among Latin, Greek and Cyrillic, 91,311 pairs do; Inter itself holds its kerning as 10,114 single pairs and two class matrices, of 121 by 104 and 64 by 61, so the format keeps classes rather than a list of pairs | `GPOS` read at run time, lookup by lookup, "in the order the lookup definitions occur in the LookupList" (OpenType, common table formats) |
| Fonts | Only those converted in the build | Any font a user brings |

**The licence.** The OFL counts as a Modified Version any derivative
made "by adding to, deleting, or substituting -- in part or in whole --
any of the components of the Original Version, by changing formats or
by porting the Font Software to a new environment" (OFL, definitions),
so the converted font, a subset in a new format, is one. A Modified
Version may not use a Reserved Font Name (condition 3), and a font
converted for a firmware or a system "must respect any names reserved
by the font authors" (OFL FAQ 1.21); names are reserved only "after
the copyright statement(s)" (OFL, definitions), and Inter's
`LICENSE.txt` names none, so the converted font may still be called
Inter. It must be distributed under the OFL and no other licence
(condition 5), with the copyright notice and the licence (condition 2),
so the font's store path holds Inter's `LICENSE.txt` beside the font,
and the notice record repeats it. The rest of nova-os keeps its own
licence: "Only the portions based on the Font Software are required to
be released under the OFL" (OFL FAQ 1.3).

- **Why convert:** text is drawn by the compositor, which holds every
  pixel, and by the renderer of every program; a parser of fixed
  records there is a small, auditable surface, and the complicated
  parsing happens once, on bytes known by their hash. Kerning resolved
  by a tool can be checked against fontTools' reading of the same font.
- **Cost:** a font a user brings needs converting in a build, through
  nova-nix, before it can be drawn; the converter is a TrueType and
  `GPOS` reader in C to write and keep; Inter's features beyond
  kerning, tabular figures among them, need adding to the converter one
  by one.
- **Why these characters and weights:** the rest of Inter needs
  shaping, or marks that U4 cannot place; each weight is a converted
  file, and the variable font's deltas would be a second outline format
  in the reader.
- **Not taken:** a TrueType parser in `lib/font/`. Its cost is the
  table walk above in every graphical process, and either hinting's
  interpreter or hinting dropped at run time anyway. U+0020 to U+017F
  alone, 317 glyphs; its cost is Greek and Cyrillic drawn as the
  missing glyph. Everything Inter maps, 2,852 code points; its cost is
  glyphs U4 cannot lay out in every font file. Inter's variable font,
  instanced at run time; its cost is a second outline format in the
  reader. Every weight the mockups use; its cost is a converted file
  for each.

## U4. Text layout

**Decided** (by the owner): small text, rule 7. **Proposed:** the rest.

`lib/text/` turns UTF-8 into positioned glyphs and lines:

1. **Decoding.** UTF-8 as RFC 3629 gives it; an ill-formed sequence is
   U+FFFD.
2. **Glyphs.** The font's map gives each code point its glyph, and one
   it does not hold gets glyph 0, the font's missing-glyph shape, as
   `cmap` asks of fonts (OpenType, `cmap`).
3. **Advances and kerning.** Each glyph advances by its width, and the
   kerning matrix adjusts the advance between each pair of neighbours.
   Positions are kept in 256ths of a pixel; a glyph's mask is drawn at
   the nearest quarter pixel across, from a cache of masks per glyph,
   size and quarter, and at whole pixels down.
4. **Lines.** The line's height is the ascender, descender and line gap,
   scaled; the baseline sits on a whole pixel. A line breaks at a space,
   after a hyphen, or, for a word wider than the line, anywhere.
5. **Alignment and fitting.** Start, centre or end, and a line too long
   for its box ends in an ellipsis.
6. **Carets.** A caret stands between code points; the text field moves
   it by one, and by words at spaces.
7. **Small text.** Below scale 2
   ([display.md D7](display.md#d7-presenting-and-pacing), rule 5), each
   size's baseline and x-height are snapped to whole pixels, the
   vertical snapping FreeType's light hinting does, and stems are
   darkened, "emboldening a glyph slightly in relation to its pixel
   size" (FreeType), by an amount set on the specimen phase 2 renders at
   1280 x 800 ([display.md D19](display.md#d19-the-build-plan)). At scale
   2 and above, neither.

Out of scope: right-to-left and bidirectional text, for scripts "where
the natural ordering of horizontal text in display is from right to
left" (UAX #9, 1); shaping, which scripts such as Arabic need because
they "require the selection of positional character shapes that depend
on adjacent characters" (UAX #9, 3.5); every `GSUB` substitution and
Inter's contextual alternates with it; `GPOS` mark attachment (lookup
types 4 to 6), so combining marks draw unpositioned; line breaking by
Unicode's rules; hyphenation;
vertical text; other fonts for characters Inter lacks; colour glyphs;
normalization, so text is drawn as its code points come, and a
character written as a base and a combining mark shows the mark
unpositioned.

- **Why:** left-to-right text in Latin, Greek and Cyrillic, written
  with precomposed characters, needs only advances and pair kerning,
  which the font format holds already; every item out of scope is a
  stage of its own, and none changes the interface of this one.
- **Why snapping and darkening below scale 2:** Inter's hinting is
  dropped ([U3](#u3-fonts)), and text mixed in linear light thins out at
  small sizes; FreeType says stem darkening "should only be enabled when
  doing gamma correction, so ideally it should always be done", and that
  at twice the density "smaller, fully black pixels reign supreme".
- **Cost:** no Arabic, Hebrew or Indic text, and no languages written
  with combining marks, until shaping exists; glyphs at small sizes that
  differ from their outlines by the snapping and the darkening.
- **Not taken:** a shaping engine from the start. Its cost is a large
  body of OpenType layout code before a single label is drawn. Nothing
  for small text, relying on screens of twice the density; its cost is
  thin text at scale 1, 1280 x 800 among the sizes. Snapping alone; its
  cost is stems that stay thin in linear light.

## U5. The interface as data

**Decided** (by the owner): the interface as data, drawn by a renderer,
with the raw window of [display.md D6](display.md#d6-windows-and-buffers)
as the escape. **Proposed:** the rest.

A program describes its interface as a value in the shell's value
model (shell.md L3): a record whose `kind` names a node from a fixed
vocabulary ([U7](#u7-widgets)), or any other value, which draws by its
type, a table as a table, a record as its fields, a path as a file. It
sends the value, and then changes to it, on a channel to a renderer,
which lays it out, draws it in the program's window, edits fields and
moves focus itself, and sends back what the user did in the program's
terms: this button activated, this field's new text, this row chosen.

```
{ kind: column, children: [
    { kind: text, text: "Rename", style: title },
    { kind: field, id: name, text: "notes.md" },
    { kind: row, children: [
        { kind: button, id: cancel, label: "Cancel" },
        { kind: button, id: ok, label: "Rename", role: default } ] } ] }
```

The console already works this way for a line of text: the shell asks
for a line, the console edits it where it draws it and the shell gets
the finished line, so "a client neither handles keys nor draws its own
echo, and a password read with no echo never reaches the screen"
([console.md C4](console.md#c4-sessions)). The interface as data is that
design widened from a line to a window.

Three ways to build interfaces, against what nova-os needs:

| | The interface as data, a system renderer | A retained toolkit in each program | Immediate mode |
|---|---|---|---|
| What it is | The program sends values; the renderer, a system program, draws them and reports actions | The program builds a tree of widget objects in its own process, and the library draws them into its window | The interface "fully issued on every update" by the program's code, the library keeping as little as it can (Dear ImGui, `FAQ.md`) |
| Consistency and polish | One renderer draws every program; a fix or a theme reaches all at once | As consistent as programs keep their library; each program's closure pins its version, so programs built a year apart look a year apart | Consistent within the library, which aims at "content creation tools and visualization / debug tools (as opposed to UI for the average end-user)" (Dear ImGui, `README.md`) |
| Security | The renderer is the program's only way to its window, so outside its canvases a program draws nothing the vocabulary lacks; the renderer sees each program's interface, so a flaw in it spans programs unless each has its own | A program draws any pixel in its window, a picture of the powerbox included; the strip and labels tell them apart ([display.md D16](display.md#d16-trusted-interface-and-the-powerbox)) | As the retained toolkit |
| Replay and audit | A program's inputs are actions, small and readable; the renderer's recording, which the kernel makes and the program cannot write, holds what the user was shown and chose | A program's inputs are raw events; what the user saw is in the program's pixels alone | As the retained toolkit, with a frame's whole interface rebuilt from state on every input |
| Accessibility | The value is the accessibility tree: names, roles and actions are there for a screen reader without a second protocol, as an HTML page "could equally be used by a blind user using a browser based around speech synthesis" (HTML, 3.2.1) | Each toolkit must export a tree beside its drawing, and programs drawn by hand export none | No persistent tree to export; "accessibility features are not supported" (Dear ImGui, `README.md`) |
| Theming | Tokens apply everywhere the renderer draws: "the application displaying it owns the look and feel" (Adaptive Cards) | Tokens apply where programs read them | Its own styles |
| Remote display | Values cross a network as they cross a channel, and the far side draws them for its own screen, as an HTML page "can also be used by a small browser on a mobile phone, without any change to the page" (HTML, 3.2.1) | Pixels | Pixels |
| nvo's results | A command's table is already an interface: the HUD shows it as the shell shows it | A widget that renders values, written for each toolkit | As the retained toolkit |
| CPU | Layout and drawing per program, in the renderer; actions cross a channel | Layout and drawing per program, in the program | The interface laid out again on every frame it draws |
| Flexibility | The vocabulary's, and a canvas node or a raw window for anything else | Any widget a program writes | Any, in code |

[Prior art](#prior-art) gives each system this draws on.

The raw window is the escape for what the vocabulary cannot say. This
document proposes a renderer process of its own for each program
([U6](#u6-the-renderer)), from its first use in phase 7, the trusted
programs drawing their own with the same library, and no immediate-mode
library in the system.

- **Why:** it is the only one of the three in which the system, not the
  program, knows what each window shows and what the user did there,
  which principle 8 asks for, and in which an nvo result and a window
  are the same thing, which principle 3 asks for. A renderer per program
  keeps a flaw that one program's values reach confined to that
  program's window.
- **Cost:** a process for every program with an interface; a vocabulary
  to design and grow, with programs waiting on it for widgets it lacks;
  an interface that changes every frame, a game's or a chart's, drawn
  on a canvas, outside what the renderer knows, and where a program can
  draw anything, a picture of the powerbox included.
- **Not taken:** a retained toolkit linked into programs. Its cost is
  the column above, chiefly a system that cannot say what a window
  showed. Immediate mode; its cost is the table's last column, chiefly
  no tree a screen reader can read. One renderer for the whole session;
  its cost is every program's interface, secret fields included, in one
  process that any program's values can attack, and every program's
  layout on one thread. The interface as data drawn by a library in the
  program's own process, which would speak the same protocol; its cost
  is what a renderer apart is for, a record of what was shown that the
  program cannot write, and a renderer the program's values cannot take
  over.

## U6. The renderer

**Proposed.**

1. **Its start.** When login starts a program whose recipe declares an
   interface, it creates a job for the program and starts there both
   the program and the renderer, `uihost.exe`, with the program's
   display connection
   ([display.md D5](display.md#d5-connections-labels-and-layers)), the
   session's theme from the settings service and the debug log; it gives
   the program the other end of a UI channel instead of a display
   connection. A program that declares a raw window gets the connection
   itself. Login needs `CREATE_JOB` on the sessions' job for this, and
   ending the job needs `task_kill`, which the kernel lacks
   ([display.md, Changes elsewhere](display.md#changes-elsewhere)).
2. **The protocol** (`include/ui_protocol.h`) carries values in the
   shell's encoding (shell.md L7, rule 9), a value larger than one
   message split across several in the value protocol's framing:

   | From | Message | What |
   |---|---|---|
   | Program | `ui-set` | The whole interface |
   | Program | `ui-change` | A path into the interface and the value to put there |
   | Program | `ui-rows` | Rows a list asked for, by their index |
   | Renderer | `activate` | A button or a row, by its `id` |
   | Renderer | `change` | A field's text, a toggle's state, a list's selection |
   | Renderer | `submit` | A field ended with Enter; a secret field's text comes only here |
   | Renderer | `rows` | A list's rows from one index to another, as they scroll into view |
   | Renderer | `canvas` | A canvas node's `id`, its size, and the handle of its window |
   | Renderer | `close` | The user closed the window |

3. **Long lists.** A list node gives its count, not its rows; the
   renderer asks for the rows it shows, so a directory of 100,000 names
   costs the rows on the screen.
4. **A canvas.** A `canvas` node is a rectangle the program draws
   itself: the renderer makes a child of its window at the node's place
   ([display.md D6](display.md#d6-windows-and-buffers)) and sends the
   program the child's handle in a `canvas` message, so the program
   draws there and nowhere else. The renderer keeps placing and
   clipping the child on its own window's channel as the layout moves
   or scrolls.
5. **Neither waits.** Neither side ever blocks on the other: each keeps
   what the other's queue cannot take in its own memory, at most
   `UI_PENDING_MAX` (1 MiB), and closes the channel past it, so a
   program that stops reading loses its interface rather than stalling
   its renderer.
6. **Limits.** A program's interface holds at most `UI_NODES_MAX`
   (65,536) nodes; past it the renderer refuses the change and says so.
7. **The log.** The renderer annotates its own recording with the
   actions it reported, as the shell annotates its transcript (shell.md
   L23), and a recording of the program's job holds both; it writes no
   text a field held to any log.
8. **In process.** Login, the powerbox and the HUD link `lib/ui` and
   draw their own windows: they are trusted already, and a renderer of
   their own would add a process to the path of the secure screen.

- **Why a job of its own:** the program's quota pays for its interface,
  and ending the job ends both.
- **Cost:** every change crosses a channel, so an interface that
  changes faster than a frame wastes the copies; a program cannot
  measure text itself to lay out a canvas to match, except by asking.
- **Not taken:** shared memory for the interface. Its cost is a
  renderer reading memory the program writes while it reads, and a
  renderer that, reading memory written outside it, cannot be recorded
  without the program ([replay.md](replay.md#shared-memory)).

## U7. Widgets

**Proposed.** The first vocabulary, each a record with `kind` and the
fields below; every node can carry an `id`, which actions name, and a
`label` a screen reader speaks.

| Kind | Fields | Behaviour | Reports |
|---|---|---|---|
| `text` | `text`, `style` (a role: body, title, caption, code), `wrap` | Drawn with [U4](#u4-text-layout); selectable later | |
| `button` | `label`, `role` (default, destructive, plain), `enabled` | Activated by a click, by Space or Enter with the focus, and, for `default`, by Enter where the focus is on no field and no other button | `activate` |
| `field` | `text`, `placeholder`, `secret` | A line edited as the console edits one ([console.md C3](console.md#c3-keys)), with selection by Shift and the pointer; Enter in it reports `submit` and nothing else; `secret` draws a mark for each character and lets nothing be copied out | `change` (not for `secret`), `submit` |
| `list` | `count`, `selection` (none, one, many), row template | Rows drawn as values; arrows and the pointer move the selection; Enter or a double click activates | `rows`, `change`, `activate` |
| `table` | A table value, or `count` and columns | A `list` whose rows are records, in columns sized to their content, sortable by a column's header | As `list` |
| `scroll` | `child` | Clips its child; the wheel, Page Up and Page Down, and dragging its bar move it | |
| `row`, `column` | `children`, `gap`, `align` | Lay their children out ([U8](#u8-layout)) | |
| `toggle` | `label`, `on` | Space or a click flips it | `change` |
| `canvas` | `width`, `height` | [U6](#u6-the-renderer), item 4 | |

Tab moves the focus through the nodes that take it, in the order of the
value; the focused node carries the theme's focus mark.

- **Why these:** a dialog, a settings page and a file list need no more,
  and each matches an nvo value: a record is a form, a table a table, a
  list a list.
- **Cost:** no menus, trees, tabs, images or rich text in the first
  vocabulary; each is a later addition every renderer must learn.
- **Not taken:** a vocabulary open to programs, widgets a program
  defines and the renderer runs. Its cost is the program's code inside
  the renderer, which is what the renderer is apart from the program to
  avoid.

## U8. Layout

**Proposed.** Layout is two passes over the value: each node's
preferred size, from its leaves up, then each node's place, from the
root down.

1. **Rows and columns** place their children along one axis with the
   `gap` between them; a child with `grow` takes a share of the room
   left over, in proportion; `align` places children across the axis.
2. **Sizes** are in the theme's units, scaled by the display's scale
   ([display.md D7](display.md#d7-presenting-and-pacing), rule 5); a
   text's preferred size is its laid-out size, a button's its
   label's and the theme's padding.
3. **Tables** size each column to the widest of the cells shown, at most
   `TABLE_COLUMN_MAX`, half the table's width, and cut longer cells
   with an ellipsis.
4. **Rounding.** Positions and sizes are kept in 256ths of a pixel and
   rounded to whole pixels once, at the end, so a gap is the same width
   wherever it falls.
5. **A window's size** is what the compositor configures; the root
   takes it, and a `scroll` takes up what does not fit.

- **Why:** two passes over rows and columns lay out every interface of
  the first vocabulary, in time proportional to its nodes, with the same
  result on every machine.
- **Cost:** no constraints between nodes in different branches, no
  overlapping nodes and no absolute positions.
- **Not taken:** a constraint solver, as a layout of linear constraints
  between any two nodes needs. Its cost is a solver in every renderer,
  and layouts whose result a reader cannot predict from the value.

## U9. Theming from tokens

**Decided** (by the owner): login and the desktop share one visual
language. The recommended direction, Horizon, keeps Limb as a sharp
wallpaper, marks the focus with a small horizon glow, and has no
full-screen blur; the visuals are designed apart from this document,
which depends on none of them. **Proposed:** the rest.

A theme is a value of tokens, served by the settings service as the
keys under `theme.` (shell.md L24). There are two: the system's, in the
system's settings, which the compositor, login and the powerbox draw
with, the same before any session; and the session's, which starts as
the system's and which the renderers and the HUD draw with. Neither
holds the strip's marks or its least depth, which are the compositor's
own ([display.md D8](display.md#d8-composition)).

| Tokens | What they give |
|---|---|
| Colours, by role | Ink, muted ink, surface, accent, danger, the focus mark, selection |
| Materials | Each translucent surface's tint and amount ([display.md D8](display.md#d8-composition)) |
| Type | Each style's weight, size and line height |
| Space | The spacing scale, padding, gaps |
| Shapes | Corner radii, stroke widths, the shadow's radius and depth, and, in the system's theme, how far the strip is deeper than its least |
| Motion | Each transition's duration, all 0 under `ui.reduce-motion` |
| The pointer | Its shapes, as outlines |

Programs name roles, never colours, as the shell's styles are roles
(shell.md L21, rule 2). Changing a token changes every window the next
time it draws.

- **Why:** one source of every colour and size is what makes login, the
  HUD and a third party's dialog look like one system, and settings
  make the theme reachable from the shell like any other setting
  (DESIGN.md, principle 3).
- **Cost:** a program cannot have a colour no role names, except on a
  canvas.
- **Why two:** a session that could restyle the trusted interface could
  make login's screen look like a program's, or a program's like
  login's.
- **Not taken:** a theme compiled into each program. Its cost is a
  change of theme that reaches only programs built after it.

## U10. Accessibility

**Proposed.**

1. Every node has a role, from its kind, and a name, its `label` or its
   text, and every action a node reports can be done from the keyboard.
2. The renderer serves a program's interface, with each node's role,
   name, state and actions, to an accessibility service on a channel
   the user grants through the system, as the powerbox grants a file; a
   screen reader is such a service, and so is an automation script the
   user runs. A `secret` field's text is never served: the service
   learns its role, its name and how many characters it holds.
3. The setting `ui.text-scale` multiplies the type tokens;
   `ui.reduce-motion` stops the caret's blink and makes every transition
   end at once, which is also what CI sets
   ([display.md D19](display.md#d19-the-build-plan)); a high-contrast
   theme is a set of tokens.
4. A canvas is opaque to all of it, beyond its label.

- **Why:** the interface as data makes the accessibility tree the
  interface itself, so it cannot fall out of step with what is drawn.
- **Cost:** no screen reader exists yet; the protocol is specified when
  one is built.
- **Not taken:** a screen reader that reads pixels. Its cost is a
  capture of every window for the reader, and text recovered by
  guessing at what the renderer already knew.

## U11. The HUD

**Decided** (by the owner): a central HUD command bar launches
programs, finds files, opens settings and runs nvo commands with
structured results, one API for the interface and the shell.
**Proposed:** the rest.

1. **Its place.** `hud.exe` is the session's, started by login with an
   `OVERLAY` connection and its end of a shell's session; the HUD key
   opens and closes it
   ([display.md D12](display.md#d12-the-secure-attention-key-and-global-keys)).
2. **What it runs.** Everything typed is nvo. Login starts a shell,
   `shell.exe`, for the HUD, with the session's handles
   ([accounts.md A7](accounts.md#a7-the-session)) and a session in
   values mode: the HUD sends each line, the shell runs it with its
   grants and authority as at the console (shell.md L15 to L20), and
   answers with values in the value encoding (shell.md L7), which the
   HUD shows as [U5](#u5-the-interface-as-data) draws them. The HUD
   holds no handle of the session's and runs no command itself.
3. **Searching.** A line that is not a command is a search, and each
   suggestion is the command it stands for, shown before Enter as the
   console shows grants (shell.md L21, rule 3): a program in the
   profile (`run`), a file under the home by name (the program a
   setting names for its kind, run with the file as its argument and so
   granted it, shell.md L15), a setting by key or description
   (`setting`), and history (shell.md L22).
4. **Assistance.** Highlighting by role, completions with descriptions
   and inline suggestions come from the shell as at the console
   (shell.md L21), so the HUD and the terminal suggest alike.
5. **Results.** A table is a table, a record its fields, a path a file
   row whose activation opens it, an error the error's message and the
   span it names; a setting's result is an editor for its schema. A
   program the HUD launches gets the focus
   ([display.md D11](display.md#d11-routing-input)).

- **Why the shell inside:** the HUD then holds no power and no command
  the terminal lacks, and a thing done in the HUD can be typed in the
  terminal as the same line (DESIGN.md, principle 3); the grants a
  command makes are the shell's, already specified and shown.
- **Cost:** the HUD waits on the nvo interpreter, which shell.md
  proposes and nothing yet implements; every HUD search is a shell
  round trip.
- **Not taken:** a launcher with its own index of programs and files.
  Its cost is a second authority over the session's files, and actions
  the shell cannot repeat.

## U12. The first programs

**Proposed.** Each draws with [U5](#u5-the-interface-as-data), and each
action it offers is an nvo command the shell can run.

| Program | What it shows | Its actions |
|---|---|---|
| Login | The accounts, a password field, the first boot's form, the secure screen ([display.md D13](display.md#d13-the-graphical-login)) | The accounts service's requests |
| Files | A directory of the home as `ls` gives it, a table | Running a file's program on it, the shell's commands that make, move and remove files, and the powerbox for anything outside the home |
| Settings | The settings service's keys by group, each by its schema: a `bool` a toggle, a choice a list, text a field | `setting set` and `setting reset` |
| The terminal | The console in a window ([display.md D4](display.md#d4-the-console-in-a-window-and-as-the-fallback)) | The shell |

- **Why these:** each is the graphical face of something the shell
  already does, so each tests the interface as data against a shell
  command whose result is known.
- **Cost:** Files and Settings depend on the shell's built-ins and the
  settings service, which are proposed in shell.md and not built. They
  are the first programs drawn through the renderer, in phase 7
  ([display.md D19](display.md#d19-the-build-plan)).
- **Not taken:** a desktop of icons and folders as the first programs.
  Its cost is an interface with no shell command behind each act, the
  split principle 3 rules out.

## Prior art

| Source | Taken | Left |
|---|---|---|
| The console's sessions ([console.md C4](console.md#c4-sessions)) | Editing where the text is drawn; the client gets finished input | Lines as the only interface |
| HTML | An interface as a tree of meaning, not presentation, that a trusted program renders for its screen or reads aloud (3.2.1) | Scripts in the page |
| RemoteViews | "a view hierarchy that can be displayed in another process", from a fixed set of layouts and widgets | A set chosen for widgets on a home screen |
| Adaptive Cards | UI "authored in JSON"; "Purely declarative - No code is needed or allowed"; the host owns the look | Cards as the only shape |
| Arcan | A server that draws a client's text itself, "the actual colours used will fit with the overall visual theme of the desktop" | Text cells as the only vocabulary |
| Flutter | Its own widgets, "an immutable declaration of part of the user interface", drawn by the toolkit | Every widget compiled into each program |
| Dear ImGui | | Immediate mode for interfaces for "the average end-user" |
| Plan 9's draw | Drawing as messages written to a server, `/dev/draw` | Pixels and strings as the vocabulary |
| FreeType | Linear-light blending; vertical-only snapping as the useful part of hinting | Hinting by the font's bytecode |
| Inter | The typeface, its metrics and its kerning | Its hinting and its substitutions |

## Changes elsewhere

- **`gfx/`:** filled outlines beside the strokes ([U2](#u2-the-rasterizer)).
- **`lib/`:** `font/`, `text/`, `value/`, `display/` and `ui/`
  ([U1](#u1-the-layers)).
- **`tools/`:** `mkfont.c` ([U3](#u3-fonts)).
- **`include/`:** `font_format.h` and `ui_protocol.h`.
- **`design/`:** `outline/`, the rasterizer's prototype and its check.
- **README.md:** the build fetches Inter's release by its SHA-256 and
  converts it; the font's store path carries Inter's licence.
- **accounts.md and the first process:** login with `CREATE_JOB` on the
  sessions' job, a job for each program and its renderer, and a shell
  in values mode for the HUD
  ([display.md, Changes elsewhere](display.md#changes-elsewhere)).
- **shell.md:** a session in values mode, for the HUD; the rendering of
  values (L7, rule 7) shared with `lib/ui`; L24's schemas, which
  Settings draws; the transcript's annotations (L23), which the renderer
  adds to.
- **nova-nix:** a derivation attribute that declares an interface, a
  raw window, or both.
