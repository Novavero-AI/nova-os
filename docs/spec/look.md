# The look

The visual design of nova-os, Horizon: the values the theme's tokens
hold, and the layout, sizes and motion of each screen the system draws,
login, the bar, windows and their frames, the HUD, notifications and the
system's requests. [ui.md U9](ui.md#u9-theming-from-tokens) gives the
tokens and how programs use them, and [display.md](display.md) which
process draws what, on which layer; this document gives what they look
like.

Each item gives the design, why, what it costs, and the main
alternative not taken, and every item is **Decided**. "The notes" are
the design notes that accompany the screens this document records; the
repository holds neither.

Sources are pinned in [README.md](README.md) and cited by these names:

| Name | Document |
|---|---|
| WCAG | Web Content Accessibility Guidelines 2.2, W3C Recommendation of 12 December 2024, by success criterion |

## V1. The direction

**Decided:** Horizon, for login and the desktop alike,
with a slim bar of type across the top rather than a frosted dock, an
account list at login
([display.md D13](display.md#d13-the-graphical-login)), the HUD as one
command bar ([ui.md U11](ui.md#u11-the-hud)), requests the system draws
([V14](#v14-the-system-asking)), and Inter for the interface's text
([ui.md U3](ui.md#u3-fonts)).

Limb is the wallpaper, whole and sharp. Login's text is set on its sky
with no panel behind it, the desktop's only chrome is the bar, and the
focus is marked by Limb's own light ([V7](#v7-focus-the-horizon)).
Nothing blurs and nothing is frosted: a surface is Limb tinted
([V4](#v4-surfaces-edges-and-shadows)), as display.md D8's materials
are, and what moves is light and small rectangles
([V8](#v8-motion)).

- **Why:** Horizon is the one direction that keeps Limb, nova-os's one
  image, whole and sharp, and it makes Limb's horizon the interface's
  focus signal: the field in focus is a small Limb with its sun under
  the warm caret. It is the cheapest to build: no blur, no glass and no
  full-screen buffer, and its one new effect is the Lorentzian falloff
  `gfx/limb.c` already computes, so its port can be checked against a
  prototype byte for byte, as Limb and the font are. Large type, a
  monogram and no chrome carry macOS's restraint.
- **Cost:** familiarity. It is the least like the login screens of
  macOS and Windows, and the Windows feel is weakest; a pointer user
  has no dock to launch from with one click.
- **Not taken:** A, Halo, the macOS-like direction: Limb blurred whole
  behind a centred column with a monogram and a pill-shaped field. Its
  cost is a full-screen pass, about one more Limb render, and the
  largest buffer of the three, a blurred backdrop of 20.7 MB at
  2880 x 1800. B, Pane, the Windows-11-like direction: a card of
  acrylic over Limb with a text box, a default button and a clock. Its
  cost is moderate and bounded to the card, a blur at quarter size and
  a shadow at full extent, and a wall-clock service for its clock. A
  frosted menu bar and a centred dock: its cost is a blur of Limb under
  two strips always on screen, frost that over Limb's near-black sky is
  a flat grey band (`#24272F`), and a dock that repeats the HUD's
  launching and the bar's switching. The console font for the
  interface's text: its even strokes set headings heavy and body text
  wide; it stays the terminal's ([V11](#v11-windows)).

## V2. Scale

**Decided.**

Every size and place in this document is in points. On a screen of
scale s a size of v points is drawn at v x s pixels, kept in 256ths of
a pixel and rounded once, as [ui.md U8](ui.md#u8-layout), rule 4,
rounds; type is then multiplied by `ui.text-scale`
([ui.md U10](ui.md#u10-accessibility), rule 3). A place is given from
the edge or the centre it is measured from, and scales the same way.
The approved screens were drawn at 2880 x 1800 with every size twice
its value here.

| Screen | Scale | The bar, 36 | A hairline, 0.75 | Body text, 13 |
|---|---|---|---|---|
| 1280 x 800 | 1 | 36 | 0.75 | 13 |
| 2560 x 1600 | 2 | 72 | 1.5 | 26 |
| 2880 x 1800 | 2.25 | 81 | 1.69 | 29.25 |

Three things follow the screen's size instead: Limb, which `gfx/limb.c`
computes for the screen; the console font's cell, its width a hundredth
of the screen's height ([console.md C5](console.md#c5-the-panel)), 8
points wherever the scale is the height over 800 exactly; and the
strip's least depth, `STRIP_MIN`
([display.md D8](display.md#d8-composition)). The console font draws
cells a whole number of pixels wide (`gfx/font.h`), so a code chip's
cell of 7 points is 7 x s rounded to the nearest pixel: 7, 14 and 16 at
scales 1, 2 and 2.25.

- **Why:** the approved lengths and type sizes halve to whole points,
  so they are whole pixels at scales 1 and 2, and the screens' 8-pixel
  grid is a grid of 4 points ([V6](#v6-shapes-and-controls)); hairlines
  and strokes stay fractions, as on the approved screens. One scale
  serves the system's screens and every program's windows, so the two
  grow alike.
- **Cost:** at 2880 x 1800, scale 2.25, every size is 12.5 percent
  larger than on the approved screens, which were drawn as at scale 2.
  At scale 1 the label is 10 pixels and body text 13, under
  [ui.md U4](ui.md#u4-text-layout)'s small text, rule 7.
- **Not taken:** the approved screens' pixels at 2880 x 1800, each
  divided by 2.25. Their cost is fractional base sizes: the bar 32,
  body text 11.6, the grid 3.56. The notes' rule, every size multiplied
  by the screen's height over 1800; its cost is a second scale beside
  D7's, so the system's screens and programs' windows would grow by
  different steps.

## V3. Colours

**Decided.**

Colours by role, in sRGB, drawn at full strength unless a percentage is
given, and mixed in linear light as `gfx/raster.c` mixes.

| Role | Colour | Where |
|---|---|---|
| Ink | `#F2F2F6` | Text and values; the boot screen's ink (`gfx/boot_screen.c`) |
| Secondary ink | `#A9B0BE` | Subtitles, the date, a focused field's label, a focused window's controls |
| Tertiary ink | `#8C95A8` | Receding text below 32 points: hints, placeholders, captions, labels, an unfocused window's label and title |
| Display tertiary ink | `#717A8C` | Receding text of 32 points and above: login's unchosen names |
| Warm | `#ECAA60` | The caret where the keyboard is, the console's caret (`console/panel.c`); grants past reading, write and the network |
| Accent | `#7EA8F8` | Selection, the picker's ring, read grants, the prompt's directory, primary buttons, the search field's edge while the HUD is open |
| Ink on the accent | `#0B1222` | A primary button's label and its keycap |
| Danger | `#FF7A6B` | Errors and their underlines; close under the pointer, at 92 percent |
| Ink on danger | `#1A0B0A` | Close's cross under the pointer |
| Valid | `#7DD3A0` | A form's valid state; no approved screen shows it |

Code, in the terminal and the HUD: [shell.md
L21](shell.md#l21-interactive-behaviour)'s styles, rule 2, and the
plain words, pipes and prompt between them:

| Style | Colour |
|---|---|
| Command, built-in | `#9DBBF7`, the rim blue lightened for text |
| String, number | `#E6D5B8`, Limb's rim warmed |
| Flag | `#A9B0BE` |
| Read grant | The path underlined in the accent |
| Write grant | Warm |
| Error | The word underlined in danger |
| Plain words, and variables | `#F2F2F6` |
| Pipes, and the prompt's `>` | `#8C95A8` |

A value drawn by its type: a string in the ink, a number or a size in
`#E6D5B8`, a boolean in `#9DBBF7`.

Limb's peaks light every horizon ([V7](#v7-focus-the-horizon)): rim
`#F0E8DA`, band `#224E8C`, warm `#CD9448`, `gfx/limb.c`'s.

A monogram's gradient is one of eight pairs
([V6](#v6-shapes-and-controls)):

| Index | Name | From | To |
|---|---|---|---|
| 0 | dawn | `#F6C177` | `#D9644A` |
| 1 | rim | `#8EB8FF` | `#3B5BC4` |
| 2 | tide | `#7EE0C8` | `#2A7F98` |
| 3 | dusk | `#C6A2FF` | `#6648C8` |
| 4 | rose | `#FFA3B5` | `#B8406E` |
| 5 | moss | `#B9E07E` | `#3F8F5A` |
| 6 | ice | `#A9D8F5` | `#4A7BA6` |
| 7 | ember | `#FFB86B` | `#C74B3A` |

**Contrast.** Every text ink meets WCAG 1.4.3, "a contrast ratio of at
least 4.5:1", and 3:1 for "large-scale text", on every surface it is
drawn on, measured where Limb shows brightest through it:

1. The tertiary ink is `#8C95A8` below 32 points: 6.42:1 against the
   sky beside the horizon's glow, `#060E1C`, under login's key hints,
   and at least 4.57:1 on every surface, the least where the horizon's
   peak shows through a raised surface. `#717A8C` is 4.47:1 there, and
   is kept for text of 32 points and up, which is large-scale (4.78:1).
2. A fill behind text is at most white at 2.5 percent: white at 6
   percent over a window measures `#494A4E`, on which the secondary ink
   is 4.06:1. A selection is the accent at 9 percent instead, `#2C3C59`,
   5.08:1.
3. A keycap's label is drawn at 86 percent of its ink, but at full
   strength in a primary button: at 86 percent the dark ink on the
   accent mixes to `#32456A`, 4.03:1; at full, 7.87:1.
4. An unfocused window's label and title are the tertiary ink at full
   strength, 4.96:1 where the horizon's peak shows through a window. At
   80 percent the title would be 4.18:1 there.

- **Why:** roles, so a theme changes a colour in one place
  ([ui.md U9](ui.md#u9-theming-from-tokens)). The accent is Limb's band
  brightened; the warm ink is the console's caret, Limb's sunrise,
  which a grant past reading shares so that it stands apart from the
  blue of reads; danger is a coral apart from that amber. An unfocused
  window's text at full strength keeps it above 4.5:1.
- **Cost:** two tertiary inks, chosen by size; an unfocused window's
  text recedes by its ink alone.
- **Not taken:** `#717A8C` for all receding text. Its cost is 4.47:1
  beside the horizon's glow, below 4.5:1. A selection of white at 6
  percent; its cost is the grey slab above. A lower bar for an
  unfocused window's text; WCAG exempts only text that is "part of an
  inactive user interface component", and an unfocused window is not
  inactive.

## V4. Surfaces, edges and shadows

**Decided.** A surface is a material of [display.md
D8](display.md#d8-composition): Limb mixed with a tint in linear light,
so a surface's pixel is Limb's at that place on the screen, tinted,
whatever window lies between.

| Surface | Tint | Weight | Over the sky | Over the horizon's peak |
|---|---|---|---|---|
| Window | `#151A24` | 0.99 | `#151A24` | `#24272F` |
| Raised: the HUD, a notification, a system box | `#1B2130` | 0.99 | `#1B2130` | `#292D39` |

Over a surface, mixed in linear light:

| Fill | What | Measured, over the sky |
|---|---|---|
| Sidebar | Black at 24 percent | `#11161F` |
| Selection where the keyboard is | The accent at 9 percent | `#2C3C59` to `#2F3F5E` |
| Selection in a window without the focus | White at 2.5 percent | |
| Code chip, secondary button | White at 2.5 percent | `#32353A` |
| A new grant's row | White at 2.5 percent | |
| Rights chip | Its ink at 10 percent | |
| A glyph's square in a HUD row | White at 6 percent | |
| The bar's search field, on the sky itself | White at 1.2 percent | `#1D1D1E` |

**Edges** are hairlines of white, 0.75 points: a focused window's at 10
percent and an unfocused one's at 7; raised surfaces 11; secondary
buttons 12; chips 8; a rights chip's its ink at 45; dividers and the
rules under a table's head 5 to 10, as each screen below gives. Raised
things also carry a top sheen, a second hairline brighter along the top
that fades out by a depth: windows 10 percent (5 unfocused) over 60
points, raised surfaces 12 over 70, primary buttons 22 over 16, app
tiles 16 over half their size.

**Shadows** are black in linear light, the shape blurred by a Gaussian:

| Shadow | Black | Sigma | Down |
|---|---|---|---|
| A focused window | 60 percent | 20 | 12 |
| An unfocused window | 45 percent | 12 | 6 |
| The HUD | 65 percent | 32 | 16 |
| A notification | 60 percent | 20 | 10 |
| A system box | 70 percent | 36 | 18 |
| A monogram | 50 percent, 40 in login's account rows | 0.09 of its diameter | 0.04 of its diameter |

- **Why:** a surface that samples the wallpaper alone costs three
  lookups a pixel whatever lies beneath, as D8 gives. 0.99 is the
  lowest weight at which every text ink passes WCAG where Limb's arc
  shows through, at the arc's brightest pixel: 0.96 gives `#3E4045`
  (the tertiary ink 3.45:1), 0.98 `#2F3237` (4.28:1) and 0.99 `#25282F`
  (4.90:1); a raised surface there is `#292D39` (4.57:1). The horizon
  still shows through a window as a faint line. Over Limb's sky a
  shadow changes little; it separates windows that overlap.
- **Cost:** a surface shows the wallpaper and not the window beneath
  (D8's cost). A shadow is a mask per window size, made as nine slices,
  four corners, four edge profiles and a flat middle, and applied
  where the window moves.
- **Not taken:** lower weights, more of Limb through each surface. Their
  cost is the tertiary ink below 4.5:1 at the arc.

## V5. Type

**Decided.**

Inter Regular, Medium and SemiBold for text up to 20 points, and Inter
Display Light, Regular and SemiBold from 22. A line's height is
[ui.md U4](ui.md#u4-text-layout)'s, rule 4, Inter's ascender, descender
and line gap, 2,478 of 2,048 units to the em: 1.21 em, rounded to whole
pixels at the size drawn.

| Role | Face | Size | Line at scales 1 and 2 | Where |
|---|---|---|---|---|
| Display | Inter Display Light | 64, 52 | 77 and 155, 63 and 126 | Login's name; login's titles |
| Account | Inter Display Regular | 32 | 39 and 77 | Login's account list |
| Heading | Inter Display SemiBold | 24 | 29 and 58 | Settings panes, system boxes |
| Input | Inter Regular | 20 | 24 and 48 | The HUD's input, every field's value |
| Greeting | Inter Regular | 17 | 21 and 41 | Login's "Welcome back," |
| Lead | Inter Regular | 16 | 19 and 39 | Login's line under a title |
| Row | Inter Regular | 14 | 17 and 34 | HUD rows, a reason quoted |
| Message | Inter Medium | 14 | 17 and 34 | An error's message |
| Title | Inter SemiBold | 13 | 16 and 31 | A window's label, a notification's title |
| Body | Inter Regular, Inter Medium | 13 | 16 and 31 | A window's title, lists, tables, settings rows; buttons, in Medium |
| Bar | Inter Medium | 12 | 15 and 29 | The bar, small buttons |
| Small | Inter Regular | 12 | 15 and 29 | Subtitles, descriptions |
| Caption | Inter Regular | 11 | 13 and 27 | Hints, footers, sources |
| Label | Inter SemiBold, in capitals, tracked 1.5 points | 10 | 12 and 24 | Section labels, column heads, a field's label |
| Key | Inter Medium | 10 | 12 and 24 | Keycaps |
| Monogram | Inter SemiBold | 0.44 of the diameter | | A monogram's letter |
| Code | The console font | The console's cell, and cells of 7 points | The cell's height | The terminal; code chips, store paths and types |

Text sits on its baseline. A single line in a row, the bar or a button
centres its capitals on the row's middle. Figures in tables, clocks and
sizes are tabular, Inter's `tnum`; what is typed as code in Inter, the
HUD's input, a command in its rows, a field's path, takes `cv05`,
Inter's l with a tail, so that `ls` does not read as `Is`. Both are
glyphs the converter writes at build time, and layout reads no `GSUB`.

- **Why:** the faces the converted font holds
  ([font-format.md](font-format.md#what-nova-os-ships)), one role for
  each step of the scale, so a program names a role and never a size.
  Tabular figures line up in columns and clocks; `cv05` keeps code
  typed in Inter readable.
- **Cost:** six faces in `/store`, with the alternates' glyphs; text at
  scale 1 is small enough for U4's rule 7.
- **Not taken:** the console font for the interface's text, which sets
  headings heavy and body text wide ([V1](#v1-the-direction)). `tnum`
  alone; its cost is `ls` read as `Is` in the HUD's input. Neither; its
  cost is figures that do not line up in a column.

## V6. Shapes and controls

**Decided.**

**Grid** of 4 points. Insets: the bar 24; windows 20 to their content;
the HUD 24 to its icons and 56 to its text; system boxes 32.

**Radii.** Windows 12; the HUD and system boxes 16; notifications 12;
rows and buttons 6, HUD rows 8; chips 5, rights chips 4.5; thumbnails
6; app tiles 0.225 of their size; keycaps 0.24 of their height;
switches and the search field fully round.

**Controls:**

| Control | Its shape |
|---|---|
| Keycap | The key's name in the Key style at 86 percent of its ink, in a 0.75-point box of its ink at 42 percent; 7 points each side of the name, 2.3 times its cap height tall unless given |
| Key hint | A keycap, then 6 points, then what it does in the Caption style in the tertiary ink; hints 20 apart |
| Button | 32 tall, 24 in a notification in the Bar style; its label in Body Medium, 32 wider than its label and keycap (24 at 24 tall) unless given; a keycap naming its key 8 before the label, in the label's ink |
| Primary button | The accent, with its top sheen; label and keycap in the ink on the accent, the keycap's label at full strength |
| Secondary button | White at 2.5 percent, its edge at 12 |
| Toggle | 36 x 20, fully round. On: the accent, with a knob of the ink 2.5 points inside it. Off: white at 10 percent, the knob in the secondary ink |
| Code chip | The console font at a cell of 7 points in the secondary ink, 8 points each side, 22 tall |
| Rights chip | The right in the console font at a cell of 7 points in its ink, 7 points each side, 18 tall: `read` in the accent; `write` and `network` warm |

**A field**, in login and the powerbox: a label in the Label style,
the secondary ink with the focus and the tertiary without; the value in
the Input style 36 below the label's baseline; and a line 17 below the
value's baseline, at rest 0.75 points of white at 5 percent, in focus
the small horizon with its sun at the caret, in error the error horizon
([V7](#v7-focus-the-horizon)). Password dots are filled circles of 8
points on a 17-point pitch, the first 1 point in. The caret is
1.5 x 25, centred on the middle of the value's capitals, 6 points after
the last dot and after a name typed at login, and 3 after other typed
text; warm where the keyboard is and the tertiary ink at 60 percent
elsewhere, so only one caret on screen is warm. It blinks every 500 ms,
as the console's does, and is solid while keys arrive.

**A monogram** is the name's first letter, a capital centred on its cap
height, in the Monogram style in white at 97 percent, on a 135-degree
gradient between its pair ([V3](#v3-colours)) mixed in linear light,
with an inner edge of white at 10 percent, 0.75 points, and its shadow
([V4](#v4-surfaces-edges-and-shadows)). The pair is
`lowbias32(fnv1a32(name)) mod 8`, FNV-1a over the name's bytes, and
lowbias32 is `gfx/limb.c`'s `hash32`. Monograms are 72 points for one
account, 48 in the account list, 22 before a typed name and 16 in the
bar; the last two have no shadow.

**The spinner** is the boot spinner (`gfx/boot_screen.c`), unchanged in
motion: its head advances 7.5 degrees every 33 ms, a turn in 1.58 s,
with a tail of 270 degrees whose brightness falls as the square, over a
track of brightness 22. Around a monogram it is an orbit 8 points
outside it, of half-width 1.3. Over a backdrop, brightness b is the
weight `light(242 * b / 255) / light(242)` of the ink, which is the
boot spinner exactly over black. Under reduced motion it is one still
frame ([V8](#v8-motion)).

**Glyphs** are strokes of one width with round ends, capsules, rings
and arcs, drawn as the wordmark and the console font are
(`gfx/raster.c`). **An app tile** is a rounded square of a gradient,
its sheen, an edge of white at 6 percent, and its glyph in the ink at
96 percent, 52 percent of the tile's size. **A thumbnail** of a
background has an edge of white at 10 percent.

- **Why:** fills behind text kept to 2.5 percent white by
  [V3](#v3-colours)'s contrast; a monogram's colour from its name alone,
  since an account has nothing else to show ([accounts.md
  A3](accounts.md#a3-records)), with lowbias32 because FNV-1a alone
  spreads short names badly.
- **Cost:** two names can share one of eight pairs.
- **Not taken:** FNV-1a alone for the pair. Its cost is short names
  crowded into few pairs.

## V7. Focus: the horizon

**Decided.** Focus is Limb's own light, in three signals:

1. **The window with the focus** catches Limb's light along its top
   edge. The bar marks the same window: its name in the ink over a
   short rim line.
2. **The keyboard's focus** is the small horizon with its warm sun,
   once per screen: under a field with the sun at the caret, under a
   list row with the sun 12 points past the name, under a thumbnail's
   name with the sun at its centre.
3. **A selection** is the accent at 9 percent; the backgrounds picker's
   chosen tile adds a 1.5-point ring of the accent at 95 percent, 3.5
   points outside it, the tertiary ink in a window without the focus.

**The small horizon** is light added over a strip along a line of
length L, the sum of three terms. Each term is its colour times a
strength, a falloff across the line and a falloff along it from the
sun; each falloff is a Lorentzian, `w^2 / (w^2 + d^2)`, d the distance
from the line or from the sun:

| Term | Colour | Strength | Across, w | Along, w |
|---|---|---|---|---|
| Core | Limb's rim | 1.0 | 0.45 | 0.30 L |
| Glow | Limb's band | 1.5 | 5 above the line, 1.5 below | 0.55 L |
| Warm | Limb's warm | 0.8 | 2.25 | 0.07 L |

The ends fade by a smoothstep over a tenth of L, and the light tapers as
`(1 - (d / reach)^2)^2` to zero at its reach, 70 points above the line
and 20 below. Measured on the approved screens: `#FFFBFC` at the sun,
`#959DB9` along the core far from it, `#284576` 4 points above. In
error the core is `#FFD6CC` at 1.0 and the glow `#962820` at 1.4, with
no warm term and the sun at the line's centre.

| Form | Where | Strength | Reach, above and below | Glow across, above and below | Along, core and glow | Ends fade over | Warm term |
|---|---|---|---|---|---|---|---|
| Field | A field's line; the HUD's input, from 20 inside each side | 1 | 70, 20 | 5, 1.5 | 0.30, 0.55 | 0.10 L | Yes |
| Row | A row or a thumbnail's name in a window | 0.85 | 20, 6 | 3, 1.5 | 0.30, 0.55 | 0.10 L | Yes |
| Window | The focused window's top edge, from 12 inside each corner, the sun at its centre | Core 0.75, glow 1.3 | 60, 10 | 7, 1 | 0.34, 0.60 | 0.12 L | No |
| Bar | Under the focused window's name, 3 past each end, 5 above the bar's foot, the sun at its centre | As the window's, times 0.9 | 15, 5 | 3, 1 | 0.30, 0.55 | 0.20 L | No |

The terms are `gfx/limb.c`'s `lorentz` in Q16. With the falloffs across
tabulated per row and along per distance from the sun, a keystroke
costs three multiply-adds a pixel and channel and redraws only the
strip, 382 x 90 points under a login field. The window's light depends
only on its width, so it is tabulated once per width and added where
the edge is drawn, about the window's width by 70 points.

- **Why:** Limb's horizon is the image's subject, and the field in
  focus is a small Limb; one warm sun on the screen shows where the
  keys go. The window's light is drawn by the compositor, which draws
  the focus mark ([display.md D8](display.md#d8-composition)), so no
  program can claim a focus it lacks.
- **Cost:** a strip of added light per field and per focused window;
  a second palette for errors.
- **Not taken:** a focus ring around the field, A's ring outside the
  pill with a blurred glow. Its cost is a precomputed glow mask per
  field size, and a mark that is not Limb's.

## V8. Motion

**Decided.**

Ease-out, `1 - (1 - t)^3`, for arrivals, and ease-in, `t^3`, for
departures. Nothing scales and nothing blurs: what moves is light and
small rectangles, so every animation is a bounded strip. A cross-fade
mixes two prerendered images of a bounded rectangle, as
`gfx/background.c` fades the boot screen into Limb.

Login:

| Event | What happens | Duration |
|---|---|---|
| The screen appears | Its lines arrive in reading order, 60 ms apart, each fading in as it rises 6 points | 280 ms each, ease-out |
| The focus moves between fields | The new line's light rises from 0 to full while the old one's falls: the dawn | 240 ms ease-out |
| A key | The sun moves to the caret, redrawing only the strip | 90 ms ease-out |
| Enter | The field fades out and the orbit in | 160 ms; 200 ms |
| A wrong password | The line turns to the error light; the field, label, caret and line shake together, `x(t) = A * exp(-t / 0.14 s) * sin(2 pi * 7 Hz * t)`, A 12 points | 150 ms; 420 ms |
| The next key | The line eases back from the error light | 240 ms |
| Up or Down in the account list | The names and monograms cross-fade and the field moves to the new choice | 180 ms ease-out |
| Success | The orbit closes into a full ring; the column fades while the bar stays; the bar's window list and search field fade in | 120 ms; 300 ms; 200 ms |
| The sunrise, with success | Limb's warm term brightens 1.6 times and settles back, recomputing about 1200 x 200 pixels of Limb at 2880 x 1800 | 600 ms |

The desktop:

| Event | What happens | Duration |
|---|---|---|
| The focus moves between windows | The old edge's light falls as the new one's rises; labels, titles and controls cross-fade | 240 ms ease-out; the inks 120 ms |
| A window opens | It appears in place at once; its edge dawns as it takes the focus | 240 ms |
| A window closes or is minimised | It is removed at once; the next window's edge dawns | 240 ms |
| A window is raised | It is drawn on top at once | 0 |
| Maximise | It snaps to the work area, with no animated resize | 0 |
| The HUD opens | The panel appears at once; the search field's edge turns to the accent; the input's horizon dawns | 240 ms |
| Typing in the HUD | Results are replaced in place; the sun follows the caret | 90 ms ease-out |
| Choosing in the HUD | The selection moves at once | 0 |
| Choosing in a list with the focus | The row's horizon slides to the next row | 180 ms ease-out |
| A command fails | The input's line turns to the error light and the error appears; nothing shakes, since the shake means a wrong password | 150 ms |
| The HUD closes | It is removed at once; the window below dawns again | 240 ms |
| A notification arrives | It slides 16 points in from the right while fading in; it stays 6 s, longer while the pointer is on it, and leaves fading | 240 ms in; 160 ms ease-in out |
| The system asks | Everything but the bar dims in one step; the box appears; its field dawns | 0, then 240 ms |

**Reduced motion.** Under `ui.reduce-motion`
([ui.md U10](ui.md#u10-accessibility), rule 3) the caret does not
blink, and every transition in both tables draws its last frame first:
nothing rises, shakes or slides, a dawn is full at once, the sunrise is
skipped, and a notification appears and leaves in place, its stay
unchanged. The spinner is one still frame, its first, and the text
beside it, "Creating your account..." or "Signing in...", says that the
work goes on, so [display.md D19](display.md#d19-the-build-plan) can
check those screens.

- **Why:** light in bounded strips is cheap: a key typed into a field
  has [display.md D9](display.md#d9-budgets)'s budget of 0.5 ms. A fade
  of a whole window mixes every pixel of the window each frame, so
  windows open and close at once and only light moves. The sunrise
  settles back to Limb as `gfx/limb.c` computes it, so skipping it
  leaves the same screen.
- **Cost:** no animated opening, closing, minimising or resizing of
  windows, which other desktops have; the sunrise recomputes part of
  Limb for each of its frames; under reduced motion a still spinner
  shows no progress of its own.
- **Not taken:** windows that fade or scale as they open and close. Its
  cost is every pixel of the window mixed each frame, a full-screen
  pass for a maximised one. Sign-in without the sunrise, which the
  notes left optional; its cost is a sign-in that ends without the one
  moment of Limb's own light the design leads to. The spinner turning
  under reduced motion; its cost is screens that change with time,
  which D19 cannot check, and motion where the setting asks for none.

## V9. The bar

**Decided.**

The bar is [display.md D8](display.md#d8-composition)'s strip, along
the top of the screen: the compositor draws it, no window covers it and
nothing dims it. It is 36 tall across the top with no fill, type on the
sky over Limb untouched, its text centred on its middle.

- **Left,** inset 24: the wordmark, the boot screen's letters at an
  x-height of 9, in the ink at 92 percent. From 96, the mark and the
  focused window's label in the Bar style in the ink, as D16 rule 1
  gives them; then, 20 after, the window list, each `APP` window's
  label in the Bar style, 20 apart, the focused window's in the ink
  over a short rim line ([V7](#v7-focus-the-horizon)), the others in
  the tertiary ink. The terminal's entry reads `terminal`, its label
  ([display.md D4](display.md#d4-the-console-in-a-window-and-as-the-fallback)).
- **Centre:** the search field, 280 x 24, fully round, white at 1.2
  percent with an edge of white at 7 percent, the edge the accent at 55
  percent while the HUD is open; an 11-point magnifier 8 from its left
  end, "Search, open or run" in the Caption style in the tertiary ink 6
  after it, and a "Super" keycap 14 tall in the tertiary ink, 8 from
  its right end.
- **Right,** inset 24: the time, hours and minutes on a 24-hour clock in
  the Bar style, tabular, in the ink; the date 8 before it, weekday, day
  and month ("Sat 7 Nov"), in the secondary ink; 20 before the date the
  user, the name in the secondary ink with a 16-point monogram 6 before
  it; and 20 before the user the compositor's notices.

At login the bar holds the wordmark, the secure mark, the notices and
the clock. While
the system asks ([V14](#v14-the-system-asking)), its left holds what
D16 rule 1 gives for a powerbox dialog, and its centre, in the search
field's place, an 18-point shield in the accent and, 8 after it, "paint
is asking for more access" in the Bar style in the ink, the asking
program's label in place of paint. Each name in the list is a button
that gives its window the focus, and the search field opens the HUD;
Alt+Tab and Super with 1 to 9 move the focus through the list
([display.md D12](display.md#d12-the-secure-attention-key-and-global-keys)).

The time and date need a wall-clock service, which nova-os does not
have ([storage.md F2](storage.md#f2-attributes)); until it does, the
bar shows neither. Once it does, the clock redraws about 100 x 36
points once a minute.

- **Why:** keyboard first: the HUD launches and the bar's list
  switches, where a dock would repeat both for the pointer. Trust: no
  window overlaps the bar and no request dims it, so it is where the
  system speaks for itself; the wordmark opens it on every screen, and
  the mark that proves the trusted interface follows at once, where the
  strip starts. The focused window's label is in the mark block and in
  the list, as macOS shows the active app in its menu bar and its Dock:
  the block says who holds the focus, which no program can draw, and
  the list is where the user switches. Limb: a dock at the
  bottom sits on the planet under the arc, where a top strip of type
  leaves the horizon untouched. It is one static strip, redrawn for the
  clock, the notices and when windows open or close.
- **Cost:** no one-click launching with the pointer; the strip takes 36
  points of every screen; the focused window's label takes its room
  twice.
- **Not taken:** a frosted menu bar 32 tall with menus and a centred
  dock of 52-point tiles ([V1](#v1-the-direction)); its cost is a blur
  under two strips always on screen, and launching and switching
  repeated for the pointer. The strip as display.md first had it, the
  marks, the focused window's label and the notices alone; its cost is
  a desktop with no window list, no pointer's way to the HUD and no
  clock. The approved screens' bar with the marks at its right edge;
  its cost is the proof of the trusted interface placed among the
  clock and the notices, which change, rather than at the strip's
  start. The bar without the wordmark, the trust mark first; its cost
  is a strip whose start changes with every focus, with nothing fixed
  where the eye begins. The focused window in the mark block alone;
  its cost is a list that leaves out the window the user is in.

## V10. Login

**Decided.**

Every login screen is Limb, whole and sharp, under the bar
([V9](#v9-the-bar)), with no panel, blur or shadow but the monograms'.
Every line starts at a left margin of 160, except a title in the
Display style, which starts 3 left of it, and the name at 64 points,
4 left. Positions down the screen are baselines unless given otherwise;
fields are [V6](#v6-shapes-and-controls)'s.

| Screen | Layout |
|---|---|
| Choose an account, while there are two or more | "Choose an account" in the Display style, 52, at 280. Rows 72 apart from 320: a 48-point monogram centred 24 right of the margin and the name in the Account style at 228, centred on the row; the chosen name in the ink, the others in the display tertiary ink with their monograms at 50 percent. The chosen account opens its password field under its name, at 228 and 320 wide, its label 12 below the row; the rows below follow 20 below its line. Key hints 52 below the last row: Up and Down, "Choose"; Enter, "Sign in" |
| One account | Its monogram, 72, centred at (196, 260); "Welcome back," in the Greeting style in the secondary ink at 350; the name in the Display style, 64, at 415; the password field 380 wide, its label at 480; Enter, "Sign in", 48 below its line |
| A wrong password | The field in error; 35 below its line "Incorrect password." in the Message style in danger and, 6 after, "2 attempts left." in Inter Regular 14 in the tertiary ink; the key hint 85 below the line |
| Checking | The orbit around the monogram; "Signing in..." in the Input style in the secondary ink in the field's place |
| First boot ([accounts.md A9](accounts.md#a9-the-first-boot)) | "Welcome to nova-os" in the Display style, 52, at 280; "First, create the administrator account for this computer." in the Lead style in the secondary ink at 318; Name, Password and Confirm password, 380 wide, their labels at 390, 482 and 574; the focused field's hint right-aligned on its label's row in the Caption style in the tertiary ink, "a to z, 0 to 9 and -, starting with a letter" for the name and "At least 8 characters" for the password; a typed name carries its 22-point monogram 9 before it; Tab, "Next field", and Enter, "Create account", at 670 |
| A password too long | A password of more than 128 bytes, [accounts.md A2](accounts.md#a2-accounts)'s limit, is not sent: the field shows its error, and 35 below its line "Passwords are at most 128 bytes." in the Message style in danger |
| Creating the account | The monogram, 72, at (196, 260) with the orbit; "Creating your account..." in the Display style, 52, at 395; "Protecting your password. This takes a few seconds." in the Lead style in the secondary ink at 433 |

After a third wrong password the screen says "Three incorrect
passwords." for 1.5 s and returns to the choice of account, as
[accounts.md A5](accounts.md#a5-checking-a-password), rule 4, ends an
attempt.

Keys: Tab and Shift+Tab move between fields; Enter moves to the next
field and submits from the last; Up and Down choose an account; typing
goes to the chosen account's field.

- **Why:** the type and a monogram carry the screen, on Limb with no
  chrome, and the bar is already there at sign-in, so only the column
  changes. The hint gives the rule a person meets while typing, and the
  byte limit, which only a long password of characters outside ASCII
  reaches, is said when it is.
- **Cost:** a name is all an account shows, so the screen is its names
  and their monograms; the clock waits for a wall-clock service; the
  upper limit is not shown until a password passes it.
- **Not taken:** Halo's centred column and Pane's card
  ([V1](#v1-the-direction)). The hint "8 to 128 bytes"; its cost is a
  unit a person typing does not count in.

## V11. Windows

**Decided.**

**The frame** of an `APP` window is the compositor's; the HUD, the
powerbox's boxes and notifications have none. Radius 12, the window's
surface, edge, sheen and shadow
([V4](#v4-surfaces-edges-and-shadows)), and the focused window's light
([V7](#v7-focus-the-horizon)). The title bar is
40 tall: the window's label in the Title style 20 from the left edge,
or 20 past a sidebar, after the system's mark where its connection
carries one ([display.md D5](display.md#d5-connections-labels-and-layers)),
and 8 after it the client's title in Body Regular; in the focused
window the label in the ink and the title in the secondary ink, in the
others both in the tertiary ink. The content starts in line with the
label, 20 in, and a row's fill reaches 8 past it.

**Controls** on the right: minimise, maximise and close, 10-point
glyphs in 1.35-point strokes on a 28-point pitch, close centred 24 from
the right edge, all centred on the title bar; in the secondary ink in
the focused window and the tertiary ink at 55 percent otherwise. Under
the pointer each has a 22-point disc of white at 10 percent; close
becomes a disc of danger at 92 percent with the cross in the ink on
danger. Super+W closes, Super+Up maximises and Super+Down minimises
the window with the focus.

**A sidebar** is the window's surface under black at 24 percent, the
full height, with a divider of white at 6 percent; its items are 28
tall, inset 8: a 14-point glyph 8 in, the accent when selected in the
focused window, and the name in the Body style 8 after it.

A window maximised fills the work area, the screen below the bar.

**The terminal** is the console in window mode ([display.md
D4](display.md#d4-the-console-in-a-window-and-as-the-fallback)), its
shell's session in values mode ([shell.md L7](shell.md#l7-pipelines),
rule 7): nvo in the console font at the console's cell, the window 80
columns and an inset of 20 on each side wide. Its title bar reads the
system's mark, its label `terminal`, and its title, the directory:
`terminal ~/Documents`. The prompt is [shell.md
L21](shell.md#l21-interactive-behaviour)'s, rule 11, the directory in
the accent and ` > ` in the tertiary ink; a command takes its styles'
colours ([V3](#v3-colours)). The console draws the values the shell
answers as structure: a rule of white at 12 percent, 1 point, beside
each value; a record as two columns, its values coloured by type; a
table with its header in capitals in the tertiary ink and a hairline of
white at 10 percent under it, sizes right-aligned, and no stripes, since
a white stripe is what a selection looks like in a window without the
focus; and the value's type on the line after it in the tertiary ink,
`table<name: string, ...>`. Bytes a program writes are drawn as text. A
path the next command will grant is underlined in the accent before
Enter, as L21, rule 3, asks, and the line under the input says what the
program gets ("gzip gets read ./report.csv, its closure, its streams").
Times are UTC (rule 9).

**Files** lists the records `ls` gives, in its order: rows 32 tall, a
16-point glyph (the accent for a folder), the name in the Body style 24
past the content's edge, the size right-aligned and the time, both
tabular in the secondary ink; column heads as labels over a hairline of
white at 8 percent. The focused row has the selection and the row
horizon with its sun past the name. Its sidebar, 160 wide, lists first
"Your view", the session's two roots with their rights ("devon", "read,
write"; "Store", "read"), then its folders. A footer 32 tall under a
divider says first what the window's folder grants
("/users/devon/Documents: read and write"), then the count and the
total size. Its title is the folder's name.

**Settings** has a sidebar 180 wide and its pane 20 in, under a heading
in the Heading style. Each section's label carries its settings key on
its right in the console font at a cell of 7 points, the HUD's
`desktop.hud-keys` among them, and a setting the pane changes shows the
command that makes the same change in a code chip after "From the
shell" ([shell.md L24](shell.md#l24-settings-and-service-commands)).
The backgrounds are 120 x 75 tiles, 20 apart, their names below them in
Body Medium; the chosen one has the ring and the keyboard's focus the
row horizon under its name, and its description,
`design/backgrounds/README.md`'s line, follows in the Small style.

- **Why:** the system draws the frame, so a program draws nothing
  outside its content, and every window, the unfocused ones included,
  shows which program owns it. The terminal is labelled `terminal`, the
  word people know on every platform, macOS's Terminal and Windows
  Terminal among them. Values drawn as structure read as the
  data they are, and Files and Settings show what a session holds and
  the command behind each change, as DESIGN.md's principle 3 asks.
- **Cost:** a label in every title bar; the console decodes values and
  lays out tables and records in its window.
- **Not taken:** the client's title alone; its cost is an unfocused
  window whose owner is not shown. The terminal labelled `console`; it
  needs no new store path, but it reads as the fallback screen. Frames
  on every window; their cost is a title bar on the system's own
  surfaces naming what the strip already proves, which macOS, Windows
  and GNOME also leave off. Window keys left to programs; their cost is
  windows switched, closed and sized by the pointer and Alt+Tab alone.
  Plain text in the terminal; its cost is values drawn as characters,
  tables as aligned columns of text. Stripes in tables; their cost is
  rows that look selected in a window without the focus.

## V12. The HUD

**Decided.**

A raised surface 800 wide, centred, its top 116 below the screen's,
radius 16, as tall as its content. It opens on its keys and with the
pointer from the bar's search field; Esc closes it.

- **The input,** 68 tall: an 18-point magnifier in the secondary ink at
  24, centred 2 above the row's middle; the text in the Input style at
  56; while empty, "Search apps, files and settings, or type a command"
  in the tertiary ink 7 after the caret; the warm caret, 1.5 x 25; and
  the field's horizon along the row's foot, from 20 inside each side,
  with the sun at the caret. When the line parses as a command, words
  take their styles' colours and an `nvo` code chip in the accent stands
  at the right, 24 in; a path that will be granted is underlined in the
  accent at 85 percent, 1.5 points, 5 below the baseline, before Enter.
- **Sections** start 8 below the input and 4 apart, each a label at 24
  in a header 26 tall. **Rows** are 40 tall: a 24-point image at 24, an
  app's tile, a background's thumbnail or a glyph on a square of white
  at 6 percent; the title in the Row style at 56; a subtitle in the
  Small style 10 after it, in the tertiary ink, the secondary when
  selected; and the kind or size at the right in the Caption style in
  the tertiary ink. **The selected row** has the selection, 12 in from
  the panel's sides and 2 from the row's, and in place of its kind the
  nvo command Enter runs, in a code chip 10 before an Enter keycap.
- **Values,** 12 below the input: a summary, "4 rows" in the Small style
  in the secondary ink and 10 after it the value's type in the console
  font at a cell of 7 points in the tertiary ink. A table: column heads
  as labels over a hairline of white at 8 percent, rows 32 tall with
  16-point glyphs, sizes in `#E6D5B8` and times in UTC, both tabular. A
  record: rows 32 tall under dividers of white at 5 percent, the key in
  the Small style in the tertiary ink at 56 and the value at 216
  coloured by type. Ctrl+Enter carries the pipeline into a terminal
  window.
- **Errors,** [shell.md L11](shell.md#l11-errors)'s fields: the input's
  line in the error light and the offending word underlined in danger;
  the kind in a code chip in danger (`type`); the message in the Message
  style; the source line in the console font with the span marked and
  labelled in danger; the help; and a "Fix" section whose row applies
  the help on Tab.
- **The footer,** 40 tall under a hairline of white at 7 percent: the
  keys that work now as key hints, and Esc, "Close", at the right.

- **Why:** Super is the key every desktop gives the system, Windows'
  Start, GNOME's Activities, ChromeOS's launcher, and no program's
  shortcut uses it alone; the console's key events already carry
  presses and releases ([console.md C3](console.md#c3-keys)). Alt+Space
  opens the HUD on a keyboard without Super, or under a host that keeps
  Super, as D12 gives. Every action the HUD takes is a shell command,
  and the selected row says which; typing that command does the same,
  DESIGN.md's principle 3 made visible.
- **Cost:** Alt+Space, Windows' window menu, is taken from every
  program; whether QEMU's Cocoa display on a Mac sends Command as
  `KEY_LEFTMETA` is not checked; the HUD's matching and ranking are not
  designed.
- **Not taken:** Ctrl+Space, which input methods use to switch. Its cost
  is a key an input method then cannot have. Alt+Space as a setting to
  turn on; its cost is a keyboard without Super, or a host that keeps
  Super, with no key for the HUD until someone changes a setting.

## V13. Notifications

**Decided.**

A raised surface 380 x 124, radius 12, at the top right, 12 below the
bar and 24 from the screen's right edge, one at a time. A 32-point
tile the system draws for the sender, 16 in from its top left; 64 from
its left, the sender's label in the Caption style in the tertiary ink
at 24, with the time, "now", 16 from its right; the title in the Title
style at 46; the body in the Small style in the secondary ink at 65;
and the actions as buttons 24 tall, 8 apart, centred 96 below its top.
The program supplies only the title, the body and the actions. A
notification never takes the keyboard. It arrives and leaves as
[V8](#v8-motion) gives.

- **Why:** a sender named by the system, as login names a window's
  program ([display.md D5](display.md#d5-connections-labels-and-layers)),
  and no keyboard taken, since a new window opens without the focus
  unless the user or `ACTIVATE` gives it
  ([display.md D11](display.md#d11-routing-input), rule 1).
- **Cost:** a notification covers what lies under it for 6 s; a process
  for each session to draw them.
- **Not taken:** the compositor drawing notifications; its cost is
  programs' text and actions laid out in the process that holds every
  pixel and every key, which display.md D1 keeps to composing and
  routing.

## V14. The system asking

**Decided.**

While a request is shown, everything but the bar and the request's box
is drawn at 28 percent of its light, in one step: the other windows,
which D16, rule 4, darkens, and the wallpaper too. Beside Limb's arc
the sky `#070F1E` becomes `#02040C`. It is one full-screen pass, a
table lookup a channel, after which the bar and the box are drawn
undimmed; the windows' own buffers restore the screen when the box
goes.

**The box** is a raised surface 600 wide, centred, its top 116 below
the screen's as the HUD's, radius 16, padding 32, as tall as its
content. On the title's baseline, 56 below its top, its top right
corner holds "Asked by nova" in the Caption style in the tertiary ink
and, 6 after, a 14-point shield in the accent.

**The powerbox's save** ([shell.md
L18](shell.md#l18-asking-for-more-the-powerbox)), from the top:

1. The title, what is asked, in the Heading style: "paint asks to save
   a file".
2. Who asks, 20 below the title's baseline: the program's 28-point
   tile, and 40 from the left its name and version in Body Medium, 12
   below the tile's top, and its store path in the console font at a
   cell of 7 points in the tertiary ink, 28 below.
3. The program's reason, 84 below the title's baseline: a rule of the
   tertiary ink at 80 percent, 1.5 x 48, and 14 right of it the label
   "paint says" and the reason in the Row style in the secondary ink,
   in typographic double quotes, U+201C and U+201D.
4. A "Save as" field the box's width, its label 76 below the reason's
   top, empty until the user types, since the program cannot propose a
   path, and completing against the session's view; under its line, 26
   below, "Type a name or Tab to complete. paint cannot suggest one." in
   the Caption style.
5. "paint will hold", 60 below the field's line, then from 12 below it
   a row 32 tall for each grant: its right in a rights chip, the path in
   the Body style 72 from the left, and where it comes from at the
   right in the Caption style, "this request, this name only", "the
   command line", "its recipe". The new grant's row has a fill of white
   at 2.5 percent and its source in the secondary ink.
6. 24 below the rows, "Grants last until paint exits; none can be taken
   back while it runs." in the Caption style in the tertiary ink.
7. Centred 52 below that, buttons 120 wide, 8 apart, at the right:
   "Refuse" with an Esc keycap, and "Allow", primary, with an Enter
   keycap.

Enter allows; Escape refuses, and the program gets `denied`.

**Consent** ([shell.md L20](shell.md#l20-the-recipe-and-the-command-line),
rule 3) is the same box, for a program from a store path in no profile,
or a script, before it first runs: the title asks to run the program
with what it declares ("Run paint with the network?" for a network
declaration); who asks; two lines in the Row style, 22 apart, saying
why ("Its recipe declares the network service. paint is not in your
profile, so nova asks once for this build."); "Its recipe declares"
over a grant row for each declaration, on the new grant's fill with
"asks you" as its source ("network", "connect to other computers"),
and last the closure's read, which every program gets ("read", "its
closure in /store", "always"); the derivation's SHA-256, or the
script's, in the console
font at a cell of 7 points; "Your answer is kept as a setting, for this
derivation only."; and the buttons "Don't run", with Esc, and "Allow
and run", primary, with Enter.

- **Why:** a program can draw neither outside its content nor over the
  bar, so the bar's request beside the box cannot be imitated from
  inside a window: it is the trusted path L18's cost asks for, "a style
  a program's output cannot imitate". Consent is a grant like the
  powerbox's others, shown the same way.
- **Cost:** the dim is a full-screen pass when a request opens and
  again when it closes, as D16's cost gives.
- **Not taken:** a box without the bar's mark; its cost is a request a
  program can draw a picture of, the dim included, which D16 rule 4
  quotes EWS on. Consent for declared services alone; its cost is paths
  and the console a program declares given without the asking L20
  decides. Login drawing consent; its cost is a request a recipe shapes
  parsed in the process that holds `SYSTEM`.

## Not designed

The approved screens are login, the desktop at rest, the HUD in five
states, three windows, a notification and two requests. They do not
show these, which are designed before they are built:

- the secure screen the secure attention key shows in a session, lock,
  log out and back
  ([display.md D12](display.md#d12-the-secure-attention-key-and-global-keys));
  how the secure mark, the system's mark and the compositor's notices
  look ([V9](#v9-the-bar));
- the pointer's shapes ([display.md D10](display.md#d10-the-pointer),
  rule 7), and every pointer state but close under the pointer:
  pressed, dragging, resizing, selecting text;
- popups and menus ([display.md D6](display.md#d6-windows-and-buffers)),
  a destructive button and a scroll's bar
  ([ui.md U7](ui.md#u7-widgets));
- a light theme, a high-contrast theme
  ([ui.md U10](ui.md#u10-accessibility), rule 3) and a second display;
- the HUD's matching and ranking, Files' other views, the settings
  panes other than the desktop's, and a place where past notifications
  are kept;
- colours for nvo's keyword, comment and suggestion styles
  ([shell.md L21](shell.md#l21-interactive-behaviour), rule 2);
- each glyph's strokes, each app tile's gradient, and how a program's
  tile is chosen.
