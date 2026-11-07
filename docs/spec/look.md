# The look

The visual design of nova-os, Horizon: the values the theme's tokens
hold, and the layout, sizes and motion of each screen the system draws,
login, the bar, windows and their frames, the HUD, notifications and the
system's requests. [ui.md U9](ui.md#u9-theming-from-tokens) gives the
tokens and how programs use them, and [display.md](display.md) which
process draws what, on which layer; this document gives what they look
like.

Each item gives the design, why, what it costs, and the main
alternative not taken. Every item is **Decided**, from the screens the
owner approved, and **Decided** (by the owner) marks a choice the owner
made between alternatives. A part marked **Open** conflicts with a
decided item of another document, or was left undecided, and waits for
the owner's ruling: it quotes both sides, and [Open items](#open-items)
lists them. "The notes" are the design notes approved with the screens;
the repository holds neither.

Sources are pinned in [README.md](README.md) and cited by these names:

| Name | Document |
|---|---|
| WCAG | Web Content Accessibility Guidelines 2.2, W3C Recommendation of 12 December 2024, by success criterion |

## V1. The direction

**Decided** (by the owner): Horizon, for login and the desktop alike,
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

**Decided** (by the owner): sizes follow the display's scale
([display.md D7](display.md#d7-presenting-and-pacing), rule 5).
**Decided:** the rest. **Open:** whether the type sizes are points at
2x (below).

Every size and place in this document is in pixels at 2880 x 1800,
where the scale is 2.25. On a screen of scale s a size v is drawn at
v x s / 2.25 pixels, kept in 256ths of a pixel and rounded once, as
[ui.md U8](ui.md#u8-layout), rule 4, rounds; type is then multiplied by
`ui.text-scale` ([ui.md U10](ui.md#u10-accessibility), rule 3). A place
is given from the edge or the centre it is measured from, and scales
the same way.

| Screen | Scale | The bar, 72 | A hairline, 1.5 | Body text, 26 |
|---|---|---|---|---|
| 1280 x 800 | 1 | 32 | 0.67 | 11.6 |
| 2560 x 1600 | 2 | 64 | 1.33 | 23.1 |
| 2880 x 1800 | 2.25 | 72 | 1.5 | 26 |

Three things follow the screen's size instead: Limb, which `gfx/limb.c`
computes for the screen; the console font's cell, its width a hundredth
of the screen's height ([console.md C5](console.md#c5-the-panel)); and
the strip's least depth, `STRIP_MIN`
([display.md D8](display.md#d8-composition)). The console font draws
cells a whole number of pixels wide (`gfx/font.h`), so the code chips'
cell, 14 at 2880 x 1800, is 14 x s / 2.25 rounded to the nearest
pixel: 6 at 1280 x 800 and 12 at 2560 x 1600.

- **Why:** one scale for the system's screens and every program's
  windows, so the two grow alike; and sizes given at the screen they
  were approved at, so 2880 x 1800 shows them exactly, and
  1280 x 800 and 2560 x 1600 show them at the same proportions of the
  screen, 4/9 and 8/9.
- **Cost:** most sizes are fractions of a pixel below scale 2.25, and
  the 8-pixel grid ([V6](#v6-shapes-and-controls)) holds only at 2.25.
  At scale 1 the label's 20 pixels are 8.9 and body text 11.6, under
  [ui.md U4](ui.md#u4-text-layout)'s small text, rule 7.
- **Not taken:** the notes' rule, every size multiplied by the
  screen's height over 1800. It equals this one wherever the height
  over 800 is a whole quarter, at 800, 1,000, 1,200 and every 200
  pixels above, and differs between: at 1920 x 1080 it gives 0.600 of
  each size, where D7's scale of 1.25 gives 0.556. Its cost is a second
  scale beside D7's, so the system's screens and programs' windows
  would grow by different steps.

**Open:** the notes' type table says its "sizes are points at 2x", but
D7 gives 2880 x 1800 a scale of 2.25: "the screen's height over 800,
rounded to the nearest quarter: 1 at 1280 x 800, 2 at 2560 x 1600 and
2.25 at 2880 x 1800". Read as points at 2x, each type size is drawn at
v x s / 2: 12.5 percent larger at 2880 x 1800 than the approved
screens, exact at 2560 x 1600, and a whole number at scale 1 (body
text 13, the label 10). Read as pixels at 2880 x 1800, as this document
gives them, the approved screens are exact at 2880 x 1800, and most
sizes are fractions at scale 1.

## V3. Colours

**Decided.** **Open:** an unfocused window's subtitle (below).

Colours by role, in sRGB, drawn at full strength unless a percentage
is given, and mixed in linear light as `gfx/raster.c` mixes.

| Role | Colour | Where |
|---|---|---|
| Ink | `#F2F2F6` | Text and values; the boot screen's ink (`gfx/boot_screen.c`) |
| Secondary ink | `#A9B0BE` | Subtitles, the date, a focused field's label, a focused window's controls |
| Tertiary ink | `#8C95A8` | Receding text below 64 pixels: hints, placeholders, captions, labels, an unfocused window's title |
| Display tertiary ink | `#717A8C` | Receding text of 64 pixels and above: login's unchosen names |
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

1. The tertiary ink is `#8C95A8` below 64 pixels: 6.42:1 against the
   sky beside the horizon's glow, `#060E1C`, under login's key hints,
   and at least 4.96:1 on every surface. `#717A8C` is 4.47:1 there, and
   is kept for text of 64 pixels and up, which is large-scale (4.78:1).
2. A fill behind text is at most white at 2.5 percent: white at 6
   percent over a window measures `#494A4E`, on which the secondary ink
   is 4.06:1. A selection is the accent at 9 percent instead, `#2C3C59`,
   5.08:1.
3. A keycap's label is drawn at 86 percent of its ink, but at full
   strength in a primary button: at 86 percent the dark ink on the
   accent mixes to `#32456A`, 4.03:1; at full, 7.87:1.

- **Why:** roles, so a theme changes a colour in one place
  ([ui.md U9](ui.md#u9-theming-from-tokens)). The accent is Limb's band
  brightened; the warm ink is the console's caret, Limb's sunrise,
  which a grant past reading shares so that it stands apart from the
  blue of reads; danger is a coral apart from that amber.
- **Cost:** two tertiary inks, chosen by size.
- **Not taken:** `#717A8C` for all receding text. Its cost is 4.47:1
  beside the horizon's glow, below 4.5:1. A selection of white at 6
  percent; its cost is the grey slab above.

**Open:** an unfocused window's subtitle is the tertiary ink at 80
percent ([V11](#v11-windows)). Computed, not measured on a screen, it
is 4.84:1 on a window over the sky but 4.18:1 over the horizon's peak,
`#24272F`, below the 4.5:1 above; the notes' "at least 4.96:1 on every
surface" holds for the ink at full strength. To rule: the subtitle at
full strength, or a lower bar for an unfocused window's text.

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

**Edges** are hairlines of white, 1.5 pixels: a focused window's at 10
percent and an unfocused one's at 7; raised surfaces 11; secondary
buttons 12; chips 8; a rights chip's its ink at 45; dividers and the
rules under a table's head 5 to 10, as each screen below gives.
Raised things also carry a top sheen, a second hairline brighter along
the top that fades out by a depth: windows 10 percent (5 unfocused)
over 120 pixels, raised surfaces 12 over 140, primary buttons 22 over
32, app tiles 16 over half their size.

**Shadows** are black in linear light, the shape blurred by a Gaussian:

| Shadow | Black | Sigma | Down |
|---|---|---|---|
| A focused window | 60 percent | 40 | 24 |
| An unfocused window | 45 percent | 24 | 12 |
| The HUD | 65 percent | 64 | 32 |
| A notification | 60 percent | 40 | 20 |
| A system box | 70 percent | 72 | 36 |
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

**Decided** (by the owner): Inter, in its six static faces
([ui.md U3](ui.md#u3-fonts)). **Decided:** the rest. **Open:** tabular
figures and `cv05` (below).

Inter Regular, Medium and SemiBold for text up to 40 pixels, and Inter
Display Light, Regular and SemiBold from 44. A line's height is
[ui.md U4](ui.md#u4-text-layout)'s, rule 4, Inter's ascender, descender
and line gap, 2,478 of 2,048 units to the em: 1.21 em, rounded at the
size drawn.

| Role | Face | Size | Line | Where |
|---|---|---|---|---|
| Display | Inter Display Light | 128, 104 | 155, 126 | Login's name; login's titles |
| Account | Inter Display Regular | 64 | 77 | Login's account list |
| Heading | Inter Display SemiBold | 48 | 58 | Settings panes, system boxes |
| Input | Inter Regular | 40 | 48 | The HUD's input, every field's value |
| Greeting | Inter Regular | 34 | 41 | Login's "Welcome back," |
| Lead | Inter Regular | 32 | 39 | Login's line under a title |
| Row | Inter Regular | 28 | 34 | HUD rows, a reason quoted |
| Message | Inter Medium | 28 | 34 | An error's message |
| Title | Inter SemiBold | 26 | 31 | Window titles, notification titles |
| Body | Inter Regular, Inter Medium | 26 | 31 | Lists, tables, settings rows; buttons, in Medium |
| Bar | Inter Medium | 24 | 29 | The bar, small buttons |
| Small | Inter Regular | 24 | 29 | Subtitles, descriptions |
| Caption | Inter Regular | 22 | 27 | Hints, footers, sources |
| Label | Inter SemiBold, in capitals, tracked 3 pixels | 20 | 24 | Section labels, column heads, a field's label |
| Key | Inter Medium | 20 | 24 | Keycaps |
| Monogram | Inter SemiBold | 0.44 of the diameter | | A monogram's letter |
| Code | The console font | Cells 18 and 14 wide | The cell's height | The terminal, at the console's own cell; code chips, store paths and types |

Text sits on its baseline. A single line in a row, the bar or a button
centres its capitals on the row's middle. Figures in tables, clocks and
sizes are tabular, Inter's `tnum`; what is typed as code in Inter, the
HUD's input, a command in its rows, a field's path, takes `cv05`,
Inter's l with a tail, so that `ls` does not read as `Is`.

- **Why:** the faces the converted font holds
  ([font-format.md](font-format.md#what-nova-os-ships)), one role for
  each step of the scale, so a program names a role and never a size.
- **Cost:** six faces in `/store`, 1,327,296 bytes; text at scale 1 is
  small enough for U4's rule 7.
- **Not taken:** the console font for the interface's text, which sets
  headings heavy and body text wide ([V1](#v1-the-direction)).

**Open:** the notes take two of Inter's substitutions: "Figures in
tables, clocks and sizes are tabular (Inter's `tnum`)", and code typed
in Inter "takes `cv05`", and propose that "Inter's `tnum` is a GSUB
substitution, which the C can replace by a ten-entry table of glyph ids
made when the font is subset; `cv05` is one more substitution, the l's
glyph id swapped". U4 puts "every `GSUB` substitution" out of scope,
and font-format.md leaves out "ligatures, contextual alternates or
tabular figures". Taken, the converter writes the substituted glyphs
and U4's scope admits two fixed substitutions; not taken, figures keep
Inter's proportional widths and code's l is Inter's plain l.

## V6. Shapes and controls

**Decided.**

**Grid** of 8 pixels. Insets: the bar 48; windows 40 to their content;
the HUD 48 to its icons and 112 to its text; system boxes 64.

**Radii.** Windows 24; the HUD and system boxes 32; notifications 24;
rows and buttons 12, HUD rows 16; chips 10, rights chips 9; thumbnails
12; app tiles 0.225 of their size; keycaps 0.24 of their height;
switches and the search field fully round.

**Controls:**

| Control | Its shape |
|---|---|
| Keycap | The key's name in the Key style at 86 percent of its ink, in a 1.5-pixel box of its ink at 42 percent; 14 pixels each side of the name, 2.3 times its cap height tall unless given |
| Key hint | A keycap, then 12 pixels, then what it does in the Caption style in the tertiary ink; hints 40 apart |
| Button | 64 tall, 48 in a notification in the Bar style; its label in Body Medium, 64 wider than its label and keycap (48 at 48 tall) unless given; a keycap naming its key 16 before the label, in the label's ink |
| Primary button | The accent, with its top sheen; label and keycap in the ink on the accent, the keycap's label at full strength |
| Secondary button | White at 2.5 percent, its edge at 12 |
| Toggle | 72 x 40, fully round. On: the accent, with a knob of the ink 5 pixels inside it. Off: white at 10 percent, the knob in the secondary ink |
| Code chip | The console font at cell 14 in the secondary ink, 16 pixels each side, 44 tall |
| Rights chip | The right in the console font at cell 14 in its ink, 14 pixels each side, 36 tall: `read` in the accent; `write` and `network` warm |

**A field**, in login and the powerbox: a label in the Label style,
the secondary ink with the focus and the tertiary without; the value in
the Input style 72 below the label's baseline; and a line 34 below the
value's baseline, at rest 1.5 pixels of white at 5 percent, in focus
the small horizon with its sun at the caret, in error the error horizon
([V7](#v7-focus-the-horizon)). Password dots are filled circles of 16
pixels on a 34-pixel pitch, the first 2 pixels in. The caret is 3 x 50,
centred on the middle of the value's capitals, 12 pixels after the last
dot and after a name typed at login, and 6 after other typed text; warm
where the keyboard is and the tertiary ink at 60 percent elsewhere, so
only one caret on screen is warm. It blinks every 500 ms, as the
console's does, and is solid while keys arrive.

**A monogram** is the name's first letter, a capital centred on its cap
height, in the Monogram style in white at 97 percent, on a 135-degree
gradient
between its pair ([V3](#v3-colours)) mixed in linear light, with an
inner edge of white at 10 percent, 1.5 pixels, and its shadow
([V4](#v4-surfaces-edges-and-shadows)). The pair is
`lowbias32(fnv1a32(name)) mod 8`, FNV-1a over the name's bytes, and
lowbias32 is `gfx/limb.c`'s `hash32`. Monograms are 144 pixels for one
account, 96 in the account list, 44 before a typed name and 32 in the
bar; the last two have no shadow.

**The spinner** is the boot spinner (`gfx/boot_screen.c`), unchanged in
motion: its head advances 7.5 degrees every 33 ms, a turn in 1.58 s,
with a tail of 270 degrees whose brightness falls as the square, over a
track of brightness 22. Around a monogram it is an orbit 16 pixels
outside it, of half-width 2.6. Over a backdrop, brightness b is the
weight `light(242 * b / 255) / light(242)` of the ink, which is the
boot spinner exactly over black.

**The wordmark** is the boot screen's letters at the x-height given, in
the ink at 92 percent. **Glyphs** are strokes of one width with round
ends, capsules, rings and arcs, drawn as the wordmark and the console
font are (`gfx/raster.c`). **An app tile** is a rounded square of a
gradient, its sheen, an edge of white at 6 percent, and its glyph in
the ink at 96 percent, 52 percent of the tile's size. **A thumbnail** of
a background has an edge of white at 10 percent.

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
   list row with the sun 24 pixels past the name, under a thumbnail's
   name with the sun at its centre.
3. **A selection** is the accent at 9 percent; the backgrounds picker's
   chosen tile adds a 3-pixel ring of the accent at 95 percent, 7
   pixels outside it, the tertiary ink in a window without the focus.

**The small horizon** is light added over a strip along a line of
length L, the sum of three terms. Each term is its colour times a
strength, a falloff across the line and a falloff along it from the
sun; each falloff is a Lorentzian, `w^2 / (w^2 + d^2)`, d the distance
from the line or from the sun:

| Term | Colour | Strength | Across, w | Along, w |
|---|---|---|---|---|
| Core | Limb's rim | 1.0 | 0.9 | 0.30 L |
| Glow | Limb's band | 1.5 | 10 above the line, 3 below | 0.55 L |
| Warm | Limb's warm | 0.8 | 4.5 | 0.07 L |

The ends fade by a smoothstep over a tenth of L, and the light tapers as
`(1 - (d / reach)^2)^2` to zero at its reach, 140 pixels above the line
and 40 below. Measured: `#FFFBFC` at the sun, `#959DB9` along the core
far from it, `#284576` 8 pixels above. In error the core is `#FFD6CC`
at 1.0 and the glow `#962820` at 1.4, with no warm term and the sun at
the line's centre.

| Form | Where | Strength | Reach, above and below | Glow across, above and below | Along, core and glow | Ends fade over | Warm term |
|---|---|---|---|---|---|---|---|
| Field | A field's line; the HUD's input, from 40 inside each side | 1 | 140, 40 | 10, 3 | 0.30, 0.55 | 0.10 L | Yes |
| Row | A row or a thumbnail's name in a window | 0.85 | 40, 12 | 6, 3 | 0.30, 0.55 | 0.10 L | Yes |
| Window | The focused window's top edge, from 24 inside each corner, the sun at its centre | Core 0.75, glow 1.3 | 120, 20 | 14, 2 | 0.34, 0.60 | 0.12 L | No |
| Bar | Under the focused window's name, 6 past each end, 10 above the bar's foot, the sun at its centre | As the window's, times 0.9 | 30, 10 | 6, 2 | 0.30, 0.55 | 0.20 L | No |

The terms are `gfx/limb.c`'s `lorentz` in Q16. With the falloffs across
tabulated per row and along per distance from the sun, a keystroke
costs three multiply-adds a pixel and channel and redraws only the
strip, 764 x 180 pixels under a login field. The window's light depends
only on its width, so it is tabulated once per width and added where
the edge is drawn, about the window's width by 140 pixels.

- **Why:** Limb's horizon is the image's subject, and the field in
  focus is a small Limb; one warm sun on the screen shows where the
  keys go. The window's light is drawn by the compositor, which draws
  the focus mark ([display.md D8](display.md#d8-composition)), so no
  program can claim a focus it lacks.
- **Cost:** a strip of added light per field and per focused window;
  a second palette for errors.
- **Not taken:** a focus ring around the field, A's 3 pixels 4 outside
  with a blurred glow. Its cost is a precomputed glow mask per field
  size, and a mark that is not Limb's.

## V8. Motion

**Decided.** Ease-out, `1 - (1 - t)^3`, for arrivals, and ease-in,
`t^3`, for departures. Nothing scales and nothing blurs: what moves is
light and small rectangles, so every animation is a bounded strip. A
cross-fade mixes two prerendered images of a bounded rectangle, as
`gfx/background.c` fades the boot screen into Limb.

Login:

| Event | What happens | Duration |
|---|---|---|
| The screen appears | Its lines arrive in reading order, 60 ms apart, each fading in as it rises 12 pixels | 280 ms each, ease-out |
| The focus moves between fields | The new line's light rises from 0 to full while the old one's falls: the dawn | 240 ms ease-out |
| A key | The sun moves to the caret, redrawing only the strip | 90 ms ease-out |
| Enter | The field fades out and the orbit in | 160 ms; 200 ms |
| A wrong password | The line turns to the error light; the field, label, caret and line shake together, `x(t) = A * exp(-t / 0.14 s) * sin(2 pi * 7 Hz * t)`, A 24 pixels | 150 ms; 420 ms |
| The next key | The line eases back from the error light | 240 ms |
| Up or Down in the account list | The names and monograms cross-fade and the field moves to the new choice | 180 ms ease-out |
| Success | The orbit closes into a full ring; the column fades while the bar stays; the bar's window list and search field fade in | 120 ms; 300 ms; 200 ms |

The desktop:

| Event | What happens | Duration |
|---|---|---|
| The focus moves between windows | The old edge's light falls as the new one's rises; titles and controls cross-fade | 240 ms ease-out; the inks 120 ms |
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
| A notification arrives | It slides 32 pixels in from the right while fading in; it stays 6 s, longer while the pointer is on it, and leaves fading | 240 ms in; 160 ms ease-in out |
| The system asks | Everything but the bar dims in one step; the box appears; its field dawns | 0, then 240 ms |

**Reduced motion.** Under `ui.reduce-motion`
([ui.md U10](ui.md#u10-accessibility), rule 3) the caret does not
blink, and every transition in both tables draws its last frame first:
nothing rises, shakes or slides, a dawn is full at once, and a
notification appears and leaves in place, its stay unchanged.

- **Why:** light in bounded strips is cheap: a key typed into a field
  has [display.md D9](display.md#d9-budgets)'s budget of 0.5 ms. A fade
  of a whole window mixes every pixel of the window each frame, so
  windows open and close at once and only light moves.
- **Cost:** no animated opening, closing, minimising or resizing of
  windows, which other desktops have.
- **Not taken:** windows that fade or scale as they open and close. Its
  cost is every pixel of the window mixed each frame, a full-screen
  pass for a maximised one.

**Open:** two motions the notes leave undecided. The sunrise: on
success "optionally Limb's warm term brightens 1.6 x over 600 ms as a
sunrise, which means recomputing about 1200 x 200 px of Limb". The
spinner under reduced motion: U10 rule 3 makes "every transition end at
once" and stops the caret's blink, and
[display.md D19](display.md#d19-the-build-plan) checks every screen
under it, but the spinner is neither a transition nor the caret, and
the notes give it no reduced form, so the screens that show it,
creating an account and signing in, change with time.

## V9. The bar

**Decided** (by the owner): a slim bar of type across the top, not a
frosted menu bar and dock. **Decided:** the rest. **Open:** what it
shows beside display.md's strip (below).

The bar is [display.md D8](display.md#d8-composition)'s strip, along
the top of the screen: the compositor draws it, no window covers it and
nothing dims it. It is 72 tall across the top with no fill, type on the
sky over Limb untouched, its text centred on its middle.

- **Left:** the wordmark, x-height 18, at 48. From 192, the windows'
  names in the Bar style, 40 apart: the focused window's in the ink
  over a short rim line ([V7](#v7-focus-the-horizon)), the others in
  the tertiary ink.
- **Centre:** the search field, 560 x 48, fully round, white at 1.2
  percent with an edge of white at 7 percent, the edge the accent at 55
  percent while the HUD is open; a 22-pixel magnifier 16 from its left
  end, "Search, open or run" in the Caption style in the tertiary ink
  12 after it, and a "Super" keycap 28 tall in the tertiary ink, 16
  from its right end.
- **Right,** inset 48: the time, hours and minutes on a 24-hour clock in
  the Bar style, tabular, in the ink; the date 16 before it, weekday,
  day and month ("Sat 7 Nov"), in the secondary ink; and 40 before the
  date the user, the name in the secondary ink with a 32-pixel monogram
  12 before it.

At login the bar holds the wordmark and the clock alone. While the
system asks ([V14](#v14-the-system-asking)), its centre holds, in the
search field's place, a 36-pixel shield in the accent and, 16 after it,
"paint is asking for more access" in the Bar style in the ink, the
asking program's label in place of paint. With a pointer each name is a
button that gives its window the focus, and the search field opens the
HUD. Alt+Tab moves the focus through the windows
([display.md D12](display.md#d12-the-secure-attention-key-and-global-keys)).

The time and date need a wall-clock service, which nova-os does not
have ([storage.md F2](storage.md#f2-attributes)); until it does, the
bar shows neither. Once it does, the clock redraws about 200 x 72
pixels once a minute.

- **Why:** keyboard first: the HUD launches and the bar's list
  switches, where a dock would repeat both for the pointer. Trust: no
  window overlaps the bar and no request dims it, so it is where the
  system speaks for itself, as in the request it shows while a powerbox
  is open. Limb: a dock at the bottom sits on the planet under the arc,
  where a top strip of type leaves the horizon untouched. It is one
  static strip, redrawn for the clock and when windows open or close.
- **Cost:** no one-click launching with the pointer; the strip takes 72
  pixels of every screen at 2880 x 1800.
- **Not taken:** a frosted menu bar 64 tall with menus and a centred
  dock of 104-pixel tiles ([V1](#v1-the-direction)). Its cost is a blur
  under two strips always on screen, and launching and switching
  repeated for the pointer.

**Open:** the notes' bar shows the wordmark, every window's name, a
search field, the time and date and the user, and during a request
"paint is asking for more access" beside a shield. D8 gives the strip
"the focused window's label and marks ([D16](display.md#d16-trusted-interface-and-the-powerbox)),
and the compositor's notices", and D16 rule 1 has it show "the secure
mark while a `SYSTEM` window has the focus; the system's mark and
`powerbox`, then the asking program's label, while a powerbox dialog
has it; and a program's label alone while one of its windows does".
The notes draw neither the secure mark nor the system's mark before a
label ([D5](display.md#d5-connections-labels-and-layers)), and none of
the strip's notices: the display restarted
([D3](display.md#d3-the-compositors-supervision-its-restart-and-what-a-crash-shows),
rule 2), a window that wants attention
([D11](display.md#d11-routing-input), rule 1), a screenshot taken and
a capture's holder, which the user can end there
([D15](display.md#d15-capture-copy-and-paste)). To rule: whether the
strip holds the window list, the search field, the clock and the user,
and how the marks and the notices look and where they go.

## V10. Login

**Decided** (by the owner): an account list
([display.md D13](display.md#d13-the-graphical-login)), drawn in
Horizon. **Decided:** the rest. **Open:** the first boot's password hint
(below).

Every login screen is Limb, whole and sharp, under the bar
([V9](#v9-the-bar)), with no panel, blur or shadow but the monograms'.
Every line starts at a left margin of 320, except a title in the
Display style, which starts 6 left of it, and the name at 128 pixels,
8 left.
Positions down the screen are baselines unless given otherwise; fields
are [V6](#v6-shapes-and-controls)'s.

| Screen | Layout |
|---|---|
| Choose an account, while there are two or more | "Choose an account" in the Display style, 104, at 560. Rows 144 apart from 640: a 96-pixel monogram centred 48 right of the margin and the name in the Account style at 456, centred on the row; the chosen name in the ink, the others in the display tertiary ink with their monograms at 50 percent. The chosen account opens its password field under its name, at 456 and 640 wide, its label 24 below the row; the rows below follow 40 below its line. Key hints 104 below the last row: Up and Down, "Choose"; Enter, "Sign in" |
| One account | Its monogram, 144, centred at (392, 520); "Welcome back," in the Greeting style in the secondary ink at 700; the name in the Display style, 128, at 830; the password field 760 wide, its label at 960; Enter, "Sign in", 96 below its line |
| A wrong password | The field in error; 70 below its line "Incorrect password." in the Message style in danger and, 12 after, "2 attempts left." in Inter Regular 28 in the tertiary ink; the key hint 170 below the line |
| Checking | The orbit around the monogram; "Signing in..." in the Input style in the secondary ink in the field's place |
| First boot ([accounts.md A9](accounts.md#a9-the-first-boot)) | "Welcome to nova-os" in the Display style, 104, at 560; "First, create the administrator account for this computer." in the Lead style in the secondary ink at 636; Name, Password and Confirm password, 760 wide, their labels at 780, 964 and 1148; the focused field's hint right-aligned on its label's row in the Caption style in the tertiary ink, "a to z, 0 to 9 and -, starting with a letter" for the name and "8 to 128 characters" for the password; a typed name carries its 44-pixel monogram 18 before it; Tab, "Next field", and Enter, "Create account", at 1340 |
| Creating the account | The monogram, 144, at (392, 520) with the orbit; "Creating your account..." in the Display style, 104, at 790; "Protecting your password. This takes a few seconds." in the Lead style in the secondary ink at 866 |

After a third wrong password the screen says "Three incorrect
passwords." for 1.5 s and returns to the choice of account, as
[accounts.md A5](accounts.md#a5-checking-a-password), rule 4, ends an
attempt.

Keys: Tab and Shift+Tab move between fields; Enter moves to the next
field and submits from the last; Up and Down choose an account; typing
goes to the chosen account's field.

- **Why:** the type and a monogram carry the screen, on Limb with no
  chrome, and the bar is already there at sign-in, so only the column
  changes.
- **Cost:** a name is all an account shows, so the screen is its names
  and their monograms; the clock waits for a wall-clock service.
- **Not taken:** Halo's centred column and Pane's card
  ([V1](#v1-the-direction)).

**Open:** the hint "8 to 128 characters" against
[accounts.md A2](accounts.md#a2-accounts), rule 4: "A password is 8 to
128 bytes of any value". A password of characters outside ASCII
reaches 128 bytes in fewer than 128 characters.

## V11. Windows

**Decided.** **Open:** what a title bar names, the window keys, and the
terminal's values and prompt (below).

**The frame** is the compositor's
([display.md D8](display.md#d8-composition)): radius 24, the window's
surface, edge, sheen and shadow ([V4](#v4-surfaces-edges-and-shadows)),
and the focused window's light ([V7](#v7-focus-the-horizon)). The title
bar is 80 tall: the title in the Title style 40 from the left edge, or
40 past a sidebar, and a subtitle in Body Regular 16 after it; in the
focused window the title in the ink and the subtitle in the secondary
ink, in the others both in the tertiary ink, the subtitle at 80
percent. The content starts on the title's line, 40 in, and a row's fill
reaches 16 past it.

**Controls** on the right: minimise, maximise and close, 20-pixel
glyphs in 2.7-pixel strokes on a 56-pixel pitch, close centred 48 from
the right edge, all centred on the title bar; in the secondary ink in
the focused window and the tertiary ink at 55 percent otherwise. Under
the pointer each has a 44-pixel disc of white at 10 percent; close
becomes a disc of danger at 92 percent with the cross in the ink on
danger.

**A sidebar** is the window's surface under black at 24 percent, the
full height, with a divider of white at 6 percent; its items are 56
tall, inset 16: a 28-pixel glyph 16 in, the accent when selected in the
focused window, and the name in the Body style 16 after it.

A window maximised fills the work area, the screen below the bar.

**The terminal** is the console in window mode
([display.md D4](display.md#d4-the-console-in-a-window-and-as-the-fallback)):
nvo in the console font at the console's cell, 18 x 36 at
2880 x 1800, so 80 columns are 1440 pixels and the window 1520 wide.
The prompt is the directory in the accent and ` > ` in the tertiary
ink; a command takes its styles' colours ([V3](#v3-colours)). Values
are drawn as structure: a rule of white at 12 percent, 2 pixels, beside
each value; a record as two columns, its values coloured by type; a
table with its header in capitals in the tertiary ink and a hairline of
white at 10 percent under it, sizes right-aligned, and no stripes,
since a white stripe is what a selection looks like in a window without
the focus; and the value's type on the line after it in the tertiary
ink, `table<name: string, ...>`. A path the next command will grant is
underlined in the accent before Enter, as
[shell.md L21](shell.md#l21-interactive-behaviour), rule 3, asks, and
the line under the input says what the program gets ("gzip gets read
./report.csv, its closure, its streams"). Times are UTC (rule 9).

**Files** lists the records `ls` gives, in its order: rows 64 tall, a
32-pixel glyph (the accent for a folder), the name in the Body style 48
past the content's edge, the size right-aligned and the time, both
tabular in the secondary ink; column heads as labels over a hairline of
white at 8 percent. The focused row has the selection and the row
horizon with its sun past the name. Its sidebar, 320 wide, lists first
"Your view", the session's two roots with their rights ("devon", "read,
write"; "Store", "read"), then its folders. A footer 64 tall under a
divider says first what the window's folder grants
("/users/devon/Documents: read and write"), then the count and the
total size. The title is the folder's name alone.

**Settings** has a sidebar 360 wide and its pane 40 in, under a heading
in the Heading style. Each section's label carries its settings key on
its right in the console font at cell 14, and a setting the pane
changes shows the command that makes the same change in a code chip
after "From the shell" ([shell.md
L24](shell.md#l24-settings-and-service-commands)). The backgrounds are
240 x 150 tiles, 40 apart, their names below them in Body Medium; the
chosen one has the ring and the keyboard's focus the row horizon under
its name, and its description, `design/backgrounds/README.md`'s line,
follows in the Small style.

- **Why:** the system draws the frame, so a program draws nothing
  outside its content, and every window moves and closes alike. Values
  drawn as structure read as the data they are, and Files and Settings
  show what a session holds and the command behind each change, as
  DESIGN.md's principle 3 asks.
- **Cost:** a terminal's values need more from the console than text
  (below).
- **Not taken:** stripes in tables. Their cost is rows that look
  selected in a window without the focus.

**Open:** what a title bar names. The notes: "title Inter SemiBold 26
at 40 from the edge (40 past a sidebar), a subtitle in Inter Regular 26
16 after it", and of Files, "The title is the folder's name alone"; the
approved screens title the terminal "Terminal", with "~/Documents"
after it, and Files "Documents", and the bar lists "Terminal" and
"Files". D8: "A window's frame, its shadow, its corners and the mark of
its focus are the compositor's, drawn from the theme
([ui.md U9](ui.md#u9-theming-from-tokens)), with its label and the
title the client sets after it"; and D5 labels a terminal's
connection `console`, and a program's with "Its name" from its store
path. Read as the label then the client's title, Files' frame would
show its label before "Documents".

**Open:** the window keys. The notes: "Super+1 to 9 picks the bar's nth
window" and "Super+W closes, Super+Up maximises, Super+Down
minimises". [D12](display.md#d12-the-secure-attention-key-and-global-keys)'s
table of keys that "belong to the system and never reach a window"
lists Ctrl+Alt+Delete, the HUD's keys, Alt+Tab and Alt+Shift+Tab, and
Print Screen; taking these reserves twelve more combinations from every
program.

**Open:** the terminal's values and prompt. The notes draw values "as
structure, not characters"; [shell.md L7](shell.md#l7-pipelines), rule
7: "At the prompt, the values a statement outputs are rendered by the
shell (a table as columns, a record as two)", as text on its console
session, which carries no style for output (L21's style spans are for
the line being edited). Rules, hairlines and colours by type need the
session to carry values or styled output. The prompt: the notes say
"The prompt `~/Documents >` is a proposal; A8's v0 prompt is
`devon@nova /users/devon>`", and [accounts.md
A8](accounts.md#a8-the-shell) has version 0 prompt "with `name@nova`
and its current directory"; shell.md does not give nvo's.

## V12. The HUD

**Decided** (by the owner): one command bar
([ui.md U11](ui.md#u11-the-hud)). **Decided:** the rest. **Open:** its
default keys (below).

A raised surface 1600 wide, centred, its top 232 below the screen's,
radius 32, as tall as its content. It opens on Super pressed and
released alone, and with the pointer from the bar's search field; Esc
closes it.

- **The input,** 136 tall: a 36-pixel magnifier in the secondary ink at
  48, centred 4 above the row's middle; the text in the Input style at
  112; while empty, "Search apps, files and settings, or type a command"
  in the tertiary ink 14 after the caret; the warm caret, 3 x 50; and
  the field's horizon along the row's foot, from 40 inside each side,
  with the sun at the caret. When the line parses as a command, words
  take their styles' colours and an `nvo` code chip in the accent stands
  at the right, 48 in; a path that will be granted is underlined in the
  accent at 85 percent, 3 pixels, 10 below the baseline, before Enter.
- **Sections** start 16 below the input and 8 apart, each a label at 48
  in a header 52 tall. **Rows** are 80 tall: a 48-pixel image at 48, an
  app's tile, a background's thumbnail or a glyph on a square of white
  at 6 percent; the title in the Row style at 112; a subtitle in the
  Small style 20 after it, in the tertiary ink, the secondary when
  selected; and the kind or size at the right in the Caption style in
  the tertiary ink. **The selected row** has the selection, 24 in from
  the panel's sides and 4 from the row's, and in place of its kind the
  nvo command Enter runs, in a code chip 20 before an Enter keycap.
- **Values,** 24 below the input: a summary, "4 rows" in the Small style
  in the secondary ink and 20 after it the value's type in the console
  font at cell 14 in the tertiary ink. A table: column heads as labels
  over a hairline of white at 8 percent, rows 64 tall with 32-pixel
  glyphs, sizes in `#E6D5B8` and times in UTC, both tabular. A record:
  rows 64 tall under dividers of white at 5 percent, the key in the
  Small style in the tertiary ink at 112 and the value at 432 coloured
  by type. Ctrl+Enter carries the pipeline into a terminal window.
- **Errors,** [shell.md L11](shell.md#l11-errors)'s fields: the input's
  line in the error light and the offending word underlined in danger;
  the kind in a code chip in danger (`type`); the message in the Message
  style; the source line in the console font with the span marked and
  labelled in danger; the help; and a "Fix" section whose row applies
  the help on Tab.
- **The footer,** 80 tall under a hairline of white at 7 percent: the
  keys that work now as key hints, and Esc, "Close", at the right.

- **Why:** Super is the key every desktop gives the system, Windows'
  Start, GNOME's Activities, ChromeOS's launcher, and no program's
  shortcut uses it alone; the console's key events already carry
  presses and releases ([console.md C3](console.md#c3-keys)). Every
  action the HUD takes is a shell command, and the selected row says
  which; typing that command does the same, DESIGN.md's principle 3
  made visible.
- **Cost:** whether QEMU's Cocoa display on a Mac sends Command as
  `KEY_LEFTMETA` is not checked; the HUD's matching and ranking are not
  designed.
- **Not taken:** Ctrl+Space, which input methods use to switch. Its cost
  is a key an input method then cannot have.

**Open:** the HUD's keys. The notes: "Not taken: Ctrl+Space, which
input methods use to switch, and Alt+Space, Windows' window menu;
Alt+Space is offered as a setting for keyboards without Super", and the
approved Settings screen names the section's key `desktop.hud`.
[D12](display.md#d12-the-secure-attention-key-and-global-keys), decided
by the owner: "the HUD key is Super tapped alone and Alt+Space, both
settings", and its table gives Super "and Alt+Space" as opening the
HUD, in "A system setting, `desktop.hud-keys`, which holds both".

## V13. Notifications

**Decided.** **Open:** their layer and who draws them (below).

A raised surface 760 x 248, radius 24, at the top right, 24 below the
bar and 48 from the screen's right edge. A 64-pixel tile 32 in from its
top left; 128 from its left, the sender's name in the Caption style in
the tertiary ink at 48, with the time, "now", 32 from its right; the
title in the Title style at 92; the body in the Small style in the
secondary ink at 130; and buttons 48 tall, 16 apart, centred 192 below
its top. The sender's name comes from its store path and is drawn by
the system; the program supplies only the title, the body and the
actions. A notification never takes the keyboard. It arrives and
leaves as [V8](#v8-motion) gives.

- **Why:** a sender named by the system, as login names a window's
  program ([display.md D5](display.md#d5-connections-labels-and-layers)),
  and no keyboard taken, since a new window opens without the focus
  unless the user or `ACTIVATE` gives it
  ([display.md D11](display.md#d11-routing-input), rule 1).
- **Cost:** a notification covers what lies under it for 6 s.
- **Not taken:** a sender's name the program supplies. Its cost is a
  notification that claims any sender, as a window could claim any
  label.

**Open:** the notes' layers are "Limb; windows; notifications; the HUD;
a system request with the dim under it; the bar", and a notification is
"drawn by the system". D8's layers are the wallpaper, `APP`,
`OVERLAY`, `TRUSTED`, `SYSTEM`, the pointer and the strip, with no
layer for notifications, and display.md gives no request that posts one
and no process that draws it.

## V14. The system asking

**Decided** (by the owner): requests are drawn by the system, the bar
marking them ([V9](#v9-the-bar)). **Decided:** the powerbox's request.
**Open:** consent to a recipe's declarations (below).

While a request is shown, everything but the bar and the request's box
is drawn at 28 percent of its light, in one step: the other windows,
which [display.md D16](display.md#d16-trusted-interface-and-the-powerbox),
rule 4, darkens, and the wallpaper too. Beside Limb's arc the sky
`#070F1E` becomes
`#02040C`. It is one full-screen pass, a table lookup a channel, after
which the bar and the box are drawn undimmed; the windows' own buffers
restore the screen when the box goes.

**The box** is a raised surface 1200 wide, centred, its top 232 below
the screen's as the HUD's, radius 32, padding 64, as tall as its
content. On the title's baseline, 112 below its top, its top right
corner holds "Asked by nova" in the Caption style in the tertiary ink
and, 12 after, a 28-pixel shield in the accent.

**The powerbox's save** ([shell.md
L18](shell.md#l18-asking-for-more-the-powerbox)), from the top:

1. The title, what is asked, in the Heading style: "paint asks to save
   a file".
2. Who asks, 40 below the title's baseline: the program's 56-pixel
   tile, and 80 from the left its name and version in Body Medium, 24
   below the tile's top, and its store path in the console font at cell
   14 in the tertiary ink, 56 below.
3. The program's reason, 168 below the title's baseline: a rule of the
   tertiary ink at 80 percent, 3 x 96, and 28 right of it the label
   "paint says" and the reason in the Row style in the secondary ink,
   in typographic double quotes, U+201C and U+201D.
4. A "Save as" field the box's width, its label 152 below the reason's
   top, empty until the user types, since the program cannot propose a
   path, and completing against the session's view; under its line, 52
   below, "Type a name or Tab to complete. paint cannot suggest one." in
   the Caption style.
5. "paint will hold", 120 below the field's line, then from 24 below it
   a row 64 tall for each grant: its right in a rights chip, the path in
   the Body style 144 from the left, and where it comes from at the
   right in the Caption style, "this request, this name only", "the
   command line", "its recipe". The new grant's row has a fill of white
   at 2.5 percent and its source in the secondary ink.
6. 48 below the rows, "Grants last until paint exits; none can be taken
   back while it runs." in the Caption style in the tertiary ink.
7. Centred 104 below that, buttons 240 wide, 16 apart, at the right:
   "Refuse" with an Esc keycap, and "Allow", primary, with an Enter
   keycap.

Enter allows; Escape refuses, and the program gets `denied`.

- **Why:** a program can draw neither outside its content nor over the
  bar, so the bar's request beside the box cannot be imitated from
  inside a window: it is the trusted path L18's cost asks for, "a style
  a program's output cannot imitate".
- **Cost:** the dim is a full-screen pass when a request opens and
  again when it closes, as D16's cost gives.
- **Not taken:** a box without the bar's mark. Its cost is a request a
  program can draw a picture of, the dim included, which D16 rule 4
  quotes EWS on.

**Open:** consent to a recipe's declarations. The notes draw a second
request, "consent to a recipe's network (open decision 5, proposed)",
"Shown before a program runs that is not in the profile and whose
recipe declares a service: the declarations as the powerbox's grant
rows (`network` in `#ECAA60`, the closure's `read` in `#7EA8F8`), the
new one marked, the derivation's hash, and that the answer is kept as a
setting for that derivation only". [shell.md
L20](shell.md#l20-the-recipe-and-the-command-line), rule 3, since
decided by the owner: "A program run from a store path that is in no
profile asks once per SHA-256 of its derivation ... the shell shows what
it declares, and keeps the answer as a setting". The screen matches the
rule; open are which trusted program draws it on the desktop, since D16
gives the powerbox only requests for files and to share, and whether
it is shown for any declaration, as L20 asks, or for a declared service
only, as the notes draw it. As approved, it is the powerbox's box with
the title "Run paint with the network?", who asks, two lines in the Row
style ("Its recipe declares the network service. paint is not in your
profile, so nova asks once for this build."), "Its recipe declares"
over the grant rows ("network", "connect to other computers", "asks
you", marked new; "read", "its closure in /store", "always"), the
derivation's SHA-256 in the console font at cell 14, "Your answer is
kept as a setting, for this derivation only.", and the buttons "Don't
run", with Esc, and "Allow and run", primary, with Enter.

## Not designed

The approved screens are login, the desktop at rest, the HUD in five
states, three windows, a notification and two requests. They do not
show these, which are designed before they are built:

- the secure screen the secure attention key shows in a session, lock,
  log out and back
  ([display.md D12](display.md#d12-the-secure-attention-key-and-global-keys));
  the secure mark, the system's mark and the strip's notices
  ([V9](#v9-the-bar));
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
- each glyph's strokes and each app tile's gradient.

## Open items

| Item | Section |
|---|---|
| The type sizes as points at 2x, against D7's scale of 2.25 at 2880 x 1800 | [V2](#v2-scale) |
| An unfocused window's subtitle below 4.5:1 over the horizon's peak | [V3](#v3-colours) |
| Tabular figures and `cv05`, against U4's scope | [V5](#v5-type) |
| The sunrise at sign-in, and the spinner under reduced motion | [V8](#v8-motion) |
| What the bar shows beside D8's and D16's strip | [V9](#v9-the-bar) |
| The first boot's hint in characters, against A2's bytes | [V10](#v10-login) |
| What a title bar names, against D8's label and title | [V11](#v11-windows) |
| The window keys, against D12's reserved keys | [V11](#v11-windows) |
| The terminal's values as structure, against L7 rule 7, and its prompt | [V11](#v11-windows) |
| Alt+Space for the HUD, against D12 | [V12](#v12-the-hud) |
| Notifications' layer and who draws them, absent from D8 | [V13](#v13-notifications) |
| Consent to a recipe's declarations on the desktop | [V14](#v14-the-system-asking) |

## Changes elsewhere

- **[ui.md](ui.md):** U9 decides Horizon and points here for its values.
  On rulings: U4's scope, for tabular figures and `cv05`.
- **[display.md](display.md):** D8 puts the strip along the top. On
  rulings: D8 and D16, the bar's contents and marks and a layer for
  notifications; D12, the window keys and the HUD's keys.
- **[font-format.md](font-format.md) and `tools/mkfont.c`:** on a
  ruling, the substituted glyphs for `tnum` and `cv05`.
- **[shell.md](shell.md) and [console.md](console.md):** on a ruling,
  values drawn as structure in the terminal.
- **A wall-clock service,** for the bar's time and date.
- **`ci/screens.c`** ([display.md D19](display.md#d19-the-build-plan))
  renders each expected screen from these values, through the same C
  that draws it.
