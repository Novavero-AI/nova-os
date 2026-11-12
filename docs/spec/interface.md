# Interface descriptions

How a service describes its interface once, in the interface
description language, and what the generator, `tools/mkinterface.c`,
makes from that text. This document gives the language's lines and
grammar, how a description lays out messages and the handles they
carry, what a receiver does with a message that breaks its
description, how messages are numbered, settings and their defaults,
the C header the generator writes with the functions both ends encode
and check messages with, where that header lives, the console's
description, the first protocol described, and what the generator
refuses where the grammar says nothing.
[shell.md L24](shell.md#l24-settings-and-service-commands), rules 4 to
7, decides that each service is described this way; the nvo commands,
the Settings window's forms and the documentation it names are later
outputs of the same descriptions ([P13](#p13-descriptions-commands-and-the-later-outputs)).

Each item gives the design, why, what it costs, and the main
alternative not taken. Every item is **Decided**. Specifications are
cited by the versions in [README.md](README.md): "RFC 3629" for UTF-8,
pinned under [Storage and file systems](README.md#storage-and-file-systems),
and "Wayland" for its protocol and book, pinned under [Display and
interface design references](README.md#display-and-interface-design-references).
Fuchsia's FIDL pages and TinyCC's source are linked under
[References](#references).

## At a glance

```
# Read a line of at most length bytes, edited at the console and
# ended by Enter. The reply carries the request's transaction ID, the
# line's length and the line, printable ASCII without its end. A
# session has at most one read waiting.
request read CONSOLE_READ 2
  body CONSOLE_REQUEST
  field length min 1 max CONSOLE_LINE_MAX
  field flags mask CONSOLE_READ_NO_ECHO
  reply line
end
```

From these lines, part of the console's session channel
([P19](#p19-the-consoles-description)), the generator writes
`#define CONSOLE_READ 2` into `include/console_protocol.h`, with
`console_session_read_encode`, which lays a read out and refuses one
the description does not allow, and `console_session_read_check`, which
the console runs on what it reads before it acts on it. The comment
above the request is the macro's documentation, and later the help
text of the command the shell makes from it.

## P1. The parts

**Decided.**

| Part | What it is |
|---|---|
| A description, `interfaces/NAME.desc` | One service's interface: its constants, layouts, channels and messages, the handles each message carries, and its settings, each with a description of what it is for |
| The generator, `tools/mkinterface.c` | A host tool in C99 with only the standard library, which `build.sh` builds as it builds `mkfont` and runs on every description ([P15](#p15-the-generator)) |
| The header, `include/NAME_protocol.h` | What the generator makes of a description: its constants, structures and settings' defaults, and the functions that encode and check each message ([P14](#p14-the-generated-header)). It is tracked in the tree, and the build fails unless it is the generator's output ([P16](#p16-where-the-header-lives)) |
| The later outputs | The nvo commands that speak the protocol, the Settings window's form for each setting, and the documentation, from the same text ([P13](#p13-descriptions-commands-and-the-later-outputs)) |

A new service is described from its start; an existing protocol keeps
its hand-written header until it moves over (shell.md L24, rule 7),
and the console's moves first
([P17](#p17-the-console-first)). The kernel's own messages, the
first process's bootstrap message and the kernel's background in
`include/native_abi.h`, are not described: that header holds the
vocabulary every description builds on ([P5](#p5-imports-and-the-kernels-vocabulary)),
and the kernel and its assembly include it.

- **Why:** a header both ends are built against cannot let them
  disagree on a layout, a number or a right, and the checks a receiver
  runs come from the same lines as the encoding its sender runs.
  shell.md L24 asks for one text per service so that the header, the
  commands, the forms and the documentation cannot drift; the C header
  is the first of them, since the compositor
  ([display.md D19](display.md#d19-the-build-plan), phase 3) and the
  network service
  ([network.md N20](network.md#n20-the-order-of-work), step 1) are
  built against generated headers.
- **Cost:** a language, a generator built from the seed, and three
  files that change together for each change of an interface: the
  description, its header and the header's line in `SHA256SUMS`.
- **Not taken:** headers kept by hand with a description checked
  against them. Its cost is every layout written twice. Descriptions as
  C preprocessor tables in the headers themselves, so-called X-macros.
  Their cost is the preprocessor as the language: it carries no
  settings and no descriptions for the commands and forms to read, and
  a table of macro calls is hard to read and to check.

## P2. Lines and words

**Decided.**

1. A description is ASCII: the bytes 20h to 7Eh, and LF (0Ah) ending
   every line, the last included. A tab, a CR or any other byte is
   refused.
2. Each line is one statement. Spaces before a statement are for the
   reader and mean nothing; a line of spaces alone is blank.
3. `#` starts a comment, which runs to the end of its line, except
   inside a string.
4. A line splits at spaces into words. `(` and `)` are words of their
   own wherever they stand. A string is `"`, then printable ASCII other
   than `"` and `\`, then `"`, and is one word, spaces and all.
5. A word is of one of these classes:

   | Class | Form | Used for |
   |---|---|---|
   | Name | `[a-z][a-z0-9_]*`, at most 31 characters | Keywords; the names of the interface, channels, messages, fields and tails; types and rights |
   | C name | `[A-Z][A-Z0-9_]*`, at most 63 characters | Constants, structures, message and handle macros |
   | Path | Names joined by `.` | A field inside an embedded structure or an earlier structure tail: `setup.kind` |
   | Setting name | Words of `[a-z][a-z0-9-]*` joined by `.` | `theme.material.panel` |
   | Number | `0`, `[1-9][0-9]*` or `0x[0-9A-Fa-f]+`, at most 2^64 - 1 | Values. A decimal above 2^63 - 1 is refused, since C gives it no type; such a value is written in hexadecimal |
   | Signed number | `-` and a decimal number | Settings' defaults and bounds alone |
   | Point | Two decimal numbers, each with or without a leading `-`, joined by `,` | Outlines ([P12](#p12-settings)) |

6. **Descriptions.** The comment lines directly above a declaration,
   with no blank line between, are its description; where there are
   none, the comment on its own line is. Each comment line loses its
   `#` and the one space after it. A declaration with both is refused,
   and so is a description the header's comment could not carry: one
   holding `/*` or `*/`, or a line ending in `??/`
   ([P24](#p24-text-in-a-description-and-in-a-string)). A declaration P3
   marks described must have a description; every other comment is the
   reader's alone.
7. **Blocks.** `import`, `struct`, `channel`, `request`, `reply`,
   `event` and `setting` open a block, which a line `end` closes. A
   `struct` block also opens inside an `import`, and messages open only
   inside a `channel`.

- **Why:** a C99 program with no library but the standard one reads it
  a line at a time, splits the line at spaces and switches on the first
  word. A change to an interface is a change to its lines, so a diff
  shows it. Comments above a declaration are its description, as the
  comment above an nvo `def` is
  ([shell.md L9](shell.md#l9-functions-and-signatures), rule 3), so the
  file reads like the documentation it becomes.
- **Cost:** no line continues onto the next, so a handle's line can be
  long; a colour is written `0xRRGGBB`, since `#` starts a comment.
- **Not taken:** XML, in which Wayland writes its protocol
  ([Wayland](README.md#display-and-interface-design-references),
  `wayland.xml`). Its cost is a parser for elements, attributes,
  entities and escapes in a tool built from the seed, and descriptions
  inside markup. Indentation that means something. Its cost is rules on
  tabs and depth that a parser and a reader must agree on.

## P3. The grammar

**Decided.** Each statement is one line, and a block's lines are those
between its opening line and its `end`. A form marked (described) has a
description ([P2](#p2-lines-and-words), rule 6).

```
description   = interface { import | const | rights | struct
                          | channel | setting }
interface     = "interface" name                        (described)
import        = "import" header
                  { import-const | import-struct } "end"
import-const  = "const" CNAME number
import-struct = "struct" CNAME number [ field { field } "end" ]
const         = "const" CNAME ( expression | string )   (described)
rights        = "rights" CNAME type rights-list          (described)
struct        = "struct" CNAME field { field } "end"     (described)
field         = scalar name [ array ] [ constraint ]     (described)
              | CNAME name [ array ]                     (described)
array         = "array" value [ "count" name ] [ "text" ]
constraint    = "=" value | "in" value value { value }
              | "mask" value | "min" value [ "max" value ]
              | "max" value
channel       = "channel" name { channel-line }
                  { message } "end"                      (described)
channel-line  = "capacity" CNAME | "select" name
              | "invalid" ( "server" | "client" ) policy
policy        = "close" | "drop" | "exit"
              | "answer" name [ CNAME ]
message       = ( "request" name [ CNAME number ]
                | "event" name [ CNAME number ]
                | "reply" name )
                  body { message-line } "end"            (described)
body          = "body" ( CNAME | "transaction" | "none" )
message-line  = "first" | "retired" | "status" path
              | "reply" name [ "on" name ] | "command" word
              | "field" path ( constraint | "=" "tails" )
              | tail | handle | handles
tail          = "tail" name CNAME                        (described)
              | "tail" name ( "bytes" | "text" | CNAME )
                  ( "count" path | "rest" ) "max" value  (described)
handle        = "handle" type [ "min" value ] [ "max" value ]
                  "travels" rights-spec "needs" rights-spec
                  [ "index" CNAME ]                      (described)
handles       = "handles" CNAME                          (described)
rights-spec   = "same" | "none" | CNAME | rights-list
rights-list   = right { right }
setting       = "setting" setting-name setting-type
                  { setting-line } "end"                 (described)
setting-type  = "bool" | "int" | "enum" | "text" | "colour"
              | "material" | "outline"
setting-line  = "default" setting-value { setting-value }
              | "allowed" word word { word }
              | "min" integer | "max" integer
              | "contour" contour-point contour-point { contour-point }
setting-value = word | number | signed | string
integer       = number | signed
contour-point = point | "q" point | "c" point
expression    = term { op term }
term          = value | "(" expression ")"
op            = "+" | "-" | "*" | "/" | "|"
value         = number | CNAME | "sizeof" CNAME
scalar        = "u8" | "u16" | "u32" | "u64" | "i32" | "i64"
```

`header` is a file name in `include/`, `type` and `right` the words of
[P9](#p9-handles), and `word` a setting's word or an nvo command's
name, `[a-z][a-z0-9-]*`; `number`, `signed`, `string` and `point` are
[P2](#p2-lines-and-words)'s classes. Which values a `default` takes, and
how many, is its setting's type's ([P12](#p12-settings)). Rules beside
the grammar:

1. The first statement is `interface`, and its name is the
   description's file name without `.desc`.
2. Every name is declared once in its namespace, and before it is used,
   in the file's order, except that `reply NAME` and `on CHANNEL` in a
   request may name a reply or a channel declared after it. The
   namespaces are: C names, one for the whole description; channels;
   messages, one for each channel; fields, one for each structure; and
   tails, one for each message. So `window` may name a channel, a
   request on another channel and a field. No declared C name is one the
   generator makes ([P14](#p14-the-generated-header)) or one the
   generated functions take from `native_abi.h` and `types.h`: `OK`,
   `ERR_INVALID_ARGS`, `ERR_NOT_SUPPORTED`, `RIGHTS_SAME`,
   `RECEIVED_HANDLE`, `HANDLE_TRANSFER`, the scalar types' C names, the
   object types' `TYPE_` macros and the rights' macros of
   [P9](#p9-handles); a setting's macros are names the generator makes.
   No field or tail is named for a C keyword, no tail for a parameter or
   a local of the generated functions (`bytes`, `byte_count`,
   `handles`, `handle_count`, `transfers`, `which`, `transaction`,
   `body`, `out`, `p`, `at`, `total`, `i`, `j`, `n`, `c`, `k`, `lo` and
   `hi`), and no generated function has two parameters of one name.
3. A message's `handle` lines give its handles' positions in the order
   written, and its `tail` lines its tails' order; its other lines
   stand in any order after `body`.
4. The `travels` of a handle may be `same`; its `needs` may not.

- **Why:** one form for each line, so a construct is a word a reader
  can look up here, and the parser holds one block on a stack for each
  open line.
- **Cost:** a description holds no more structure than these forms: no
  conditional field, no union, no generic type
  ([P8](#p8-tails)).
- **Not taken:** FIDL, Fuchsia's interface definition language, whose
  bindings are "generated by the FIDL compiler (fidlc)" ([FIDL
  overview][fidl-overview]). Its cost is a language of structs, tables,
  unions, bits, enums, protocols and libraries, and a compiler for all
  of it built from the seed, for protocols whose every message here is
  a structure and a few tails. Structures written as C and read by a
  parser of a C subset. Its cost is a C parser in the tool, and
  settings and descriptions bolted onto C's syntax.

## P4. Constants and expressions

**Decided.**

1. `const NAME EXPRESSION` is evaluated in unsigned 64-bit arithmetic.
   Its operands are numbers, constants declared or imported before it,
   and `sizeof S`, the size of a structure declared or imported before
   it. `+`, `-`, `*`, `/` and `|` are its operators, `/` dividing
   rounded down. At each level of parentheses one operator kind is
   used, applied left to right, so `A + B * C` is refused and
   `A + ( B * C )` is not. A result past 2^64 - 1 or below 0, and a
   division by 0, are refused.
2. The header gets `#define NAME TEXT`: the expression's words joined
   by single spaces, with `(` and `)` against what they enclose,
   wrapped in parentheses when it holds an operator, and each
   `sizeof S` written as the size in decimal. So
   `const CONSOLE_CONTROL_CAPACITY CONSOLE_SESSIONS + 2` gives
   `#define CONSOLE_CONTROL_CAPACITY (CONSOLE_SESSIONS + 2)`, and every
   constant is an integer constant expression, usable in `#if` and in
   an array's size. A level of parentheses where an operand or a result
   passes 2,147,483,647, what a C `int` holds, starts with `1ULL * `, so
   that C computes it in unsigned 64-bit arithmetic as the generator
   did: `const BIG 2000000000 + 2000000000` gives
   `#define BIG (1ULL * 2000000000 + 2000000000)`. A level within what an
   `int` holds is written as the description has it
   ([P22](#p22-constants-as-operands)).
3. `const NAME "text"` gives `#define NAME "text"`; a string is no
   operand.
4. `rights NAME TYPE RIGHT...` gives the rights' macros of
   [P9](#p9-handles) joined by `|`, in the order written:
   `rights CONSOLE_SESSION_RIGHTS channel read write wait transfer`
   gives `#define CONSOLE_SESSION_RIGHTS (CHANNEL_RIGHT_READ |
   CHANNEL_RIGHT_WRITE | RIGHT_WAIT | RIGHT_TRANSFER)`. Its name is a
   `rights-spec`, and its value is no operand
   ([P22](#p22-constants-as-operands)).
5. The generator knows the channel's limits, [ipc.md
   I1](ipc.md#i1-channels)'s: `CHANNEL_MAX_BYTES` (65,536),
   `CHANNEL_MAX_HANDLES` (64) and `CHANNEL_MAX_CAPACITY` (1,024). They
   are operands with no import, and every header asserts each against
   `native_abi.h`'s.

- **Why:** the generator computes every value, so it checks a message's
  size against the channel's limit before a compiler runs; the header
  keeps the expression as written, so it says what a value means, as
  the hand-written headers do.
- **Cost:** no precedence, so mixed arithmetic takes parentheses; no
  constant is negative.
- **Not taken:** C's precedence table. Its cost is a table to implement
  and to know, for constants that rarely mix operators. Expressions
  copied into the header unevaluated. Their cost is a limit broken
  that only a compiler, or a boot, finds.

## P5. Imports and the kernel's vocabulary

**Decided.**

1. `import HEADER` names a header in `include/` whose names the
   description uses, and gives what the generator needs to know of
   each: a constant's value; a structure's size; and for a structure
   whose fields the description reads, as a body, a selector's home or
   a count's, those fields, as a `struct` block lists them, with no
   constraints, which belong to the header that owns the type.
2. The header the generator writes includes each imported header,
   after `native_abi.h`, in the order imported, and redefines nothing
   from it. It asserts what the import says: `_Static_assert(NAME ==
   VALUE, ...)`, `_Static_assert(sizeof(S) == SIZE, ...)`, and for each
   field listed `_Static_assert(offsetof(S, f) == OFFSET, ...)`, so a
   description that misstates a header fails to compile.
3. `native_abi.h` is always included. Its object types and rights are
   the language's own words ([P9](#p9-handles)), and its channel limits
   operands ([P4](#p4-constants-and-expressions)), with no import; its
   other names, `SECTION_MAX_SIZE` or `BOOTSTRAP_DISPLAY`, are imported
   as any header's are.
4. A description never reads another description. It imports the
   header generated from it, as it imports a hand-written one.

- **Why:** a type shared between protocols is defined once, by the
  header it belongs to, and `BOOTSTRAP_DISPLAY`, `BOOTSTRAP_BACKGROUND` and the `INPUT_` types
  stay where the kernel's handoff and the input drivers define them.
  The compiler checks each import against the header itself, so the
  generator needs no C parser and reads one file.
- **Cost:** a description restates the size and the fields it relies
  on, which the assertions keep true.
- **Not taken:** the generator reading C headers. Its cost is a
  preprocessor and a C parser in the tool. Each description defining
  the shared types again. Its cost is two definitions of one layout,
  and two headers that cannot be included together.

## P6. Layout

**Decided.**

1. Every field is little-endian, the order of both ends on x86-64. The
   scalar types `u8`, `u16`, `u32`, `u64`, `i32` and `i64` are
   `types.h`'s `UINT8`, `UINT16`, `UINT32`, `UINT64`, `INT32` and
   `INT64`.
2. A structure's fields lie one after another, each at the sum of the
   sizes before it, with no gap. A scalar whose offset is not a
   multiple of its size, an embedded structure whose offset is not a
   multiple of its largest scalar's size, and a structure whose size is
   not a multiple of its largest scalar's size are refused. Padding is
   a field the description names and fixes at 0, `u32 reserved = 0`, as
   the hand-written headers name it. Every structure's size and every
   field's offset are asserted in the header, so a gap the compiler
   would add, beside an imported structure the generator knows only
   the size of, fails to compile.
3. A scalar takes one constraint: `= V`; `in V W ...`; `mask V`, no bit
   outside V; or `min V` and `max V`, either or both, inclusive and
   compared as signed for `i32` and `i64`. In a structure it holds
   wherever the structure is used; a message's `field` line constrains,
   for that message alone, a field its structure leaves unconstrained.
   A constraint the field's type already guarantees, `min 0` on an
   unsigned field or a `max` at its type's largest, is refused, and so
   is a value above what the type holds
   ([P21](#p21-arrays-and-constraints)).
4. `TYPE name array N` is N elements of a scalar type or a structure.
   With `count F`, F is a scalar field before it in the same structure,
   at most N, and the elements past F are 0. With `text`, which needs
   `count` and `u8`, the counted bytes are text ([P8](#p8-tails),
   rule 6). An array takes no constraint of its own, and a structure it
   holds carries its fields' constraints into each element
   ([P21](#p21-arrays-and-constraints)).
5. `S name` embeds structure S, declared or imported before it; its
   own constraints hold inside it.

- **Why:** fixed-width fields at fixed offsets read with no decoding
  beyond a cast, as every hand-written protocol already does. Padding
  fixed at 0 carries no stale memory from one process to another, and
  keeps a recorded message's bytes a function of what its sender meant
  ([replay.md](replay.md#inputs-through-the-kernel)); FIDL asks the
  same, "The creator of a message must fill all alignment padding gaps
  with zeros. The consumer of a message must verify that padding
  contains zeros" ([FIDL wire format][fidl-wire]).
- **Cost:** the author writes the padding, and a field moved costs a
  new structure ([P11](#p11-numbers-and-change)). A constraint that
  relates two fields, such as a stride of at least a width, has no
  form, and the receiver checks it where it reads both.
- **Not taken:** Wayland's padding, where "the value of padding bytes
  is undefined" ([Wayland](README.md#display-and-interface-design-references),
  `Protocol.md`). Its cost is a process's leftover memory in every
  message it sends. Packed structures. Their cost is unaligned fields
  and a compiler extension `-pedantic` refuses.

## P7. Channels and messages

**Decided.** A `channel` block describes one kind of channel and its
two ends, the client and the server. The client sends requests; the
server answers some of them with replies and sends events of its own,
as Wayland names them: "A message sent by a client to the server is
called request. A message from the server to a client is called event"
([Wayland](README.md#display-and-interface-design-references),
`Protocol.md`).

1. **`capacity NAME`** names the constant, 1 to `CHANNEL_MAX_CAPACITY`,
   that the channel's creator passes to `channel_create`, where the
   description decides it.
2. **`select NAME`** names the selector: a `u16` or `u32` field that
   every request and every event of the channel holds at one offset,
   among its body's own fields or, failing that, in the structure that
   is its body's first field, and so on down; a `first` message need
   not hold it. Each request and event that holds it has a C name and a
   value, emitted as `#define`, unique among its direction's and never
   `0xFFFFFFFF`. Without `select`, each
   direction holds one message, or a `first` message and one other, and
   none has a name or value.
3. **`body`** is the message's fixed part: a structure; `transaction`,
   four bytes holding a `u32` transaction alone; or `none`, no bytes,
   which only a `first` message may have.
4. **Transactions**
   ([ipc.md I3](ipc.md#i3-synchronous-call-and-asynchronous-queues)).
   Every body begins with a `u32` field `transaction`, its own first
   field or its first structure's, except a `first` message's, which
   may begin otherwise or hold no bytes: a bootstrap message, which
   answers nothing and is never answered. A request's transaction is
   its sender's, any value unless constrained; a reply carries its
   request's, which the client matches against what it sent; an event's
   is 0, and its check refuses any other, as in FIDL, where "A value of
   0 for txid is reserved for messages that do not require a response
   from the other side" ([FIDL wire format][fidl-wire]). On a channel
   that has both replies and events, a request that has a reply must
   carry a transaction other than 0, and its check requires it, so a
   client tells a reply from an event by its first four bytes.
5. **`reply NAME`** in a request names its one reply. `reply NAME on
   CHANNEL` names a reply that is the first message the server sends on
   another channel of the description, whose end the request carries,
   as an open's reply goes on the endpoint it carried
   ([storage.md F1](storage.md#f1-the-directory-protocol)); what the
   generator checks of it is [P23](#p23-a-reply-on-another-channel)'s. A
   reply no request names is refused.
6. **`status PATH`** in a reply names a `u32` field holding a status,
   0 for success, as [syscalls.md S7](syscalls.md#s7-error-reporting)'s
   `OK` is. With any other status the reply is its body alone, with no
   tails and no handles.
7. **`first`** marks the first message its sender writes on the
   channel, and only the first. A receiver checks it with its own
   check function, and no dispatcher matches its value.
8. Every message's largest size is at most `CHANNEL_MAX_BYTES` and its
   most handles at most `CHANNEL_MAX_HANDLES`.

- **Why:** these are every existing protocol's frames: a transaction
  and then an operation or a kind (the console's, block, file,
  directory and accounts protocols), a `u16` type (input), and a kind
  inside an embedded setup (the console's control channel); and a
  reply matched by its transaction, as I3 gives. A channel is one
  object already, so no message names an object.
- **Cost:** a reply has no name of its own on the wire, so a client
  keeps which request each transaction belongs to; a channel has one
  selector for both directions.
- **Not taken:** FIDL's ordinals: "Methods are identified by their
  ordinal. The compiler calculates it by: Taking the SHA-256 hash of
  the method's fully qualified name. Extracting the first 8 bytes of
  the hash digest" ([FIDL language specification][fidl-language]).
  Their cost is a 64-bit ordinal that no hand-written protocol has, so
  none could move over with its bytes unchanged. Wayland's header of an object ID and a size in every
  message ([Wayland](README.md#display-and-interface-design-references),
  `Protocol.md`). Its cost is eight bytes a message that a channel and
  `channel_read`'s byte count already give.

## P8. Tails

**Decided.** After its body, a message may carry tails, in the order
written, with nothing between them:

1. `tail NAME S`: one structure S ([P25](#p25-structure-tails)).
2. `tail NAME bytes|text|S count PATH max V`: PATH names a `u16`,
   `u32` or `u64` field of the body or of an earlier structure tail,
   holding the tail's bytes, or for S its elements, at most V. A
   message's `field` line may give that field a `min`; its maximum is
   the tail's.
3. `tail NAME bytes|text rest max V`: every byte after the tails before
   it, at most V; only the last tail.
4. `field PATH = tails`: the field of the body holds the number of
   bytes after the body, as `IO_REQUEST`'s `length` does.
5. A message is its body and its tails, exactly: its check refuses a
   byte more or fewer.
6. **Text** is UTF-8 as RFC 3629 gives it, with no overlong form, no
   surrogate and nothing above U+10FFFF, and holds no NUL, as
   Wayland's strings hold none: "Interior null bytes are not
   permitted" ([Wayland](README.md#display-and-interface-design-references),
   `Protocol.md`). A C reader can then treat counted text as a string
   without meeting its end early. A text field or tail is checked, not
   cut: a sender cuts a long text at a character's boundary before it
   encodes it.
7. In a reply with `status`, the tails come only with status 0
   ([P7](#p7-channels-and-messages), rule 6).
8. The language has no tail that is present on a condition, and no
   list of records each followed by its own bytes. The directory
   protocol's two such parts are `bytes` tails its service reads by hand
   ([P18](#p18-the-hand-written-protocols-in-the-language)).

- **Why:** these are every trailing form the hand-written protocols
  use: a console write's bytes counted by `length`, the accounts
  service's credentials and the two strings they count, a rename's two
  paths split at `offset` with the second running to the end, and the
  frame protocol's entries counted by the header
  ([network.md N4](network.md#n4-the-frame-protocol)).
- **Cost:** the directory's entries and a link's paths are checked only
  for their length.
- **Not taken:** Wayland's string, a length counting a NUL, then the
  bytes and the NUL, padded to 32 bits
  ([Wayland](README.md#display-and-interface-design-references),
  `Protocol.md`). Its cost is bytes no protocol here has. FIDL's
  vectors and out-of-line objects, which nest ([FIDL wire
  format][fidl-wire]). Their cost is a decoder that walks pointers in a
  message for protocols whose tails are flat.

## P9. Handles

**Decided.**

1. `handle TYPE [min A] [max B] travels RIGHTS needs RIGHTS [index
   NAME]` gives one position, or with `min` and `max` a run of A to B
   handles, by default exactly 1. A run of a fixed number, A equal to B,
   takes that many positions, each checked and transferred as the line
   says, and may come anywhere in the message. Only a message's last
   `handle` line may vary in number, and the most a message carries is
   at most `CHANNEL_MAX_HANDLES`. `min` and `max` come together
   ([P20](#p20-handle-runs)).
2. The types and rights are those of
   [objects.md O2](objects.md#o2-object-types) and
   [O5](objects.md#o5-access-rights), by the names `native_abi.h`
   gives them:

   | Type | Object type | Its own rights |
   |---|---|---|
   | `process` | `TYPE_PROCESS` | `map`, `create_thread`, `terminate`, `inspect` |
   | `thread` | `TYPE_THREAD` | `start`, `terminate` |
   | `section` | `TYPE_SECTION` | `read`, `write`, `execute`, `pin` |
   | `channel` | `TYPE_CHANNEL` | `read`, `write` |
   | `port` | `TYPE_PORT` | `queue`, `read` |
   | `job` | `TYPE_JOB` | `create_process`, `create_job`, `terminate`, `write_execute` |
   | `resource` | `TYPE_RESOURCE` | `use` |
   | `interrupt` | `TYPE_INTERRUPT` | `bind`, `ack` |
   | `initiator` | `TYPE_INITIATOR` | `pin`, `release` |
   | `pin` | `TYPE_PIN` | none |

   `duplicate`, `transfer`, `wait` and `signal` are every type's. A
   right `r` of type `t` is the macro `T_RIGHT_R`, such as
   `CHANNEL_RIGHT_READ`, and a common one `RIGHT_R`, such as
   `RIGHT_WAIT`. A type the kernel adds to `native_abi.h` is added
   here.
3. **`travels`** is the rights the handle carries in the message: a
   list of rights, a `rights` constant, `same` for `RIGHTS_SAME`, or
   `none`. The encoding writes them into each `HANDLE_TRANSFER`, so the
   kernel narrows the handle as it moves
   ([ipc.md I2](ipc.md#i2-handle-transfer)).
4. **`needs`** is the rights the receiver requires: a list, a `rights`
   constant, or `none`. The check refuses a handle whose type is not
   TYPE or whose rights lack any of these, and accepts one with more.
   Needs that `travels` does not hold, `same` aside, are refused, since
   no receiver could get them.
5. `index NAME` emits the position as a macro, and a message's
   `handles NAME` its most handles.

- **Why:** the description, not each sender's code, gives what
  authority a message passes, and the check gives what the receiver may
  rely on. FIDL's handles are "validated to have the correct Zircon
  object type and at least as many rights as are specified in FIDL"
  ([FIDL language specification][fidl-language]). A handle with rights
  beyond its needs gives the receiver authority it does not use, and
  refusing it would fail a sender that passes `RIGHTS_SAME` for no
  safety.
- **Cost:** a description cannot require that a handle lack a right; a
  sender keeps its own handle to the object anyway.
- **Not taken:** FIDL's reduction, where "If the handle has more rights
  than is specified in FIDL, then its rights will be reduced by a call
  to zx_handle_replace" ([FIDL language specification][fidl-language]).
  Its cost is a system call for each handle received, where the
  sender's transfer already narrowed it. Exact rights. Their cost is a
  sender that must know the receiver's every right.

## P10. Checking a message

**Decided.**

1. **A message's check** runs in this order and returns `OK`, or
   `ERR_INVALID_ARGS` at the first rule broken:
   1. the bytes hold the body: 0 for `none`, 4 for `transaction`, or
      the structure's size;
   2. with a selector, it holds the message's value;
   3. the transaction rules of [P7](#p7-channels-and-messages), rule 4;
   4. the body's constraints, in offset order, embedded structures'
      included, then for each array in offset order its count, its
      zeros, its text and the constraints of its elements
      ([P21](#p21-arrays-and-constraints));
   5. for a reply with `status` other than 0, the message is its body
      alone with no handle, and the check ends;
   6. each tail in order, its count within its maximum, its text, and
      each element's constraints; then each `= tails` field; then the
      bytes are exactly the body and the tails;
   7. the handles number between the sum of their minimums and the sum
      of their maximums, and each has its position's type and needs.
2. **A dispatcher**, for a direction with a selector, reads the
   selector and gives it to its caller: where the bytes are too short
   to hold it, `P_UNNAMED` ([P14](#p14-the-generated-header)) and
   `ERR_INVALID_ARGS`; where no message of the direction has the value,
   or its message is `first` or `retired`, `ERR_NOT_SUPPORTED`;
   otherwise the message's check.
3. **What a receiver does** with a message whose check fails is its
   channel's policy, `invalid server` for requests and `invalid
   client` for replies and events, each required where its end receives
   anything:

   | Policy | The receiver |
   |---|---|
   | `close` | Closes the handles the message carried and its own end, and answers nothing; the sender sees `PEER_CLOSED` |
   | `drop` | Closes the handles the message carried and goes on. A server answers a request the dispatcher named that has a reply, with that reply's body alone: the request's transaction, the check's status in its `status` field if it has one, and every other byte 0, so its sender is not left waiting. A client's call that the message answers fails |
   | `answer REPLY [STATUS]` | A server's only. Closes the handles the message carried and answers every message of at least four bytes with REPLY's body alone: its first four bytes as the transaction, and STATUS, or the check's status, in its `status` field. A message under four bytes holds no transaction, so the server acts as `close`. Every request of the channel must have a reply, and REPLY a `status` |
   | `exit` | Ends its process, as a service does whose bootstrap channel breaks its protocol |

4. The generated code acts on nothing: it makes no system call. The
   receiver's code does what the policy says, and the header's comment
   on the channel says it.

- **Why:** each hand-written protocol already says what its receiver
  does, and they differ for reasons: a console session ends, since a
  client that sends nonsense to the screen is broken
  ([console.md C4](console.md#c4-sessions)); a block request is
  answered, since its client waits on every request; the console's
  control channel goes on, since the first process is its only sender.
  One word for each end keeps that choice in the description and its
  documentation, where both ends read it. Two statuses are what the
  hand-written servers answer: `ERR_NOT_SUPPORTED` for an operation
  they do not have and `ERR_INVALID_ARGS` for the rest.
- **Cost:** the block and directory protocols' handling of a message
  too short to answer differs from `answer`'s
  ([P18](#p18-the-hand-written-protocols-in-the-language)); a check
  says only that a message is wrong, not where.
- **Not taken:** one policy for every channel. Its cost is a console
  that answers writes, which C4 rules out, or block clients never told
  a request was refused. Generated receive loops that read, check and
  act. Their cost is system calls in a header the host's checks
  compile, and one loop shape every server must fit.

## P11. Numbers and change

**Decided.**

1. Each request's and event's value is written in its description,
   unique among its direction's.
2. Once a description is on `main`, a message's value, body, tails,
   handles and constraints never change, no value is used again, and a
   structure a message uses never changes. A change is a new message
   with a new value, with a new structure where its layout changes. A
   description's text may change at any time.
3. A message withdrawn from use is marked `retired`: its macro stays, the
   generator makes no function for it, and a dispatcher answers it
   `ERR_NOT_SUPPORTED`, so its value is never taken again.
4. A message enters its description in the phase that builds it, so a
   description holds what both ends at one commit implement, and a
   later phase appends. A receiver built from an older description
   meets a newer message as one its dispatcher does not know.
5. Review keeps these rules: the generator reads one file and sees no
   history.

- **Why:** a recording replays against a later build only if its
  messages mean the same there ([replay.md](replay.md#recordings)), and
  a program in `/store` built against an older header keeps working
  with a newer server. Values written out, not counted from the order
  of lines, cannot shift when a line moves.
- **Cost:** a mistake in a message on `main` is kept, retired, and
  costs a new value to fix; review alone holds the rule.
- **Not taken:** Wayland's versions: "Every interface is versioned"
  (`Protocol.md`), and `wayland.xml` marks each message added later
  with the version that added it, its `since`
  ([Wayland](README.md#display-and-interface-design-references)).
  Their cost is a version agreed on each channel and checked on each
  message. FIDL's tables, each of whose fields travels in an envelope
  with an ordinal ([FIDL wire format][fidl-wire]). Their cost is an
  envelope for every field of every message.

## P12. Settings

**Decided.** A `setting` block declares a key of the settings service
with its schema, as [shell.md
L24](shell.md#l24-settings-and-service-commands) gives one: its name,
its type, its allowed values, its default and its description. Rule 5
of L24 makes a service's settings keys whose schemas come from its
description.

| Type | Lines | Default |
|---|---|---|
| `bool` | `default` | `true` or `false` |
| `int` | `default`; `min`, `max`, either or both | A signed number within them |
| `enum` | `allowed`, two or more words; `default` | One of the words |
| `text` | `max`, the most bytes; `default` | A string of text ([P8](#p8-tails), rule 6) within it |
| `colour` | `default` | `0xRRGGBB`: sRGB codes, red in the high byte |
| `material` | `default` | A colour and an amount, the percent of the tint mixed over the wallpaper, 0 to 100, as the console's panel mixes its tint ([display.md D8](display.md#d8-composition)) |
| `outline` | `contour`, one or more | Each line one closed contour, its last point joined to its first: a bare point lies on the curve; a point after `q` is a quadratic Bezier curve's control point, between two points on the curve; two points each after `c` are a cubic's two control points, between two points on the curve, as `lib/outline.c` draws them ([ui.md U2](ui.md#u2-the-rasterizer)). A contour holds at least two points on the curve and starts with one. Coordinates are in 256ths of a point ([look.md V2](look.md#v2-scale)), x right and y down from the shape's own origin, such as a pointer's hotspot, each within 2^24 of 0 |

**Defaults in the header.** The C name of a setting `C` is its
name in capitals with each `.` and `-` made `_`. The header defines its
default:

| Type | Defined |
|---|---|
| `bool` | `C_DEFAULT`, 1 or 0 |
| `int` | `C_DEFAULT`, the number |
| `enum` | `C_WORD` for each allowed word, its own name in capitals with `-` made `_`, numbered from 0 in the order allowed; `C_DEFAULT`, the default's macro |
| `text` | `C_DEFAULT`, the string |
| `colour` | `C_DEFAULT`, `0xRRGGBB` |
| `material` | `C_DEFAULT_TINT`, `0xRRGGBB`, and `C_DEFAULT_AMOUNT`, the percent |
| `outline` | `C_DEFAULT_POINTS`, an initializer `{ x0, y0, x1, y1, ... }` of `INT32`s; `C_DEFAULT_FLAGS`, an initializer of one `UINT8` per point, 1 on the curve, 0 a quadratic's control point and 2 a cubic's, as [font-format.md](font-format.md#point-flags)'s point flags are with the cubic's added; `C_DEFAULT_ENDS`, an initializer of each contour's end, the index one past its last point, as [font-format.md](font-format.md#contour-ends)'s contour ends are; `C_DEFAULT_POINT_COUNT` and `C_DEFAULT_CONTOURS` |

So `theme.material.panel` with `default 0x121620 68` gives
`THEME_MATERIAL_PANEL_DEFAULT_TINT` and
`THEME_MATERIAL_PANEL_DEFAULT_AMOUNT`, and a program holds an outline
with `static const INT32 body[] = THEME_POINTER_BODY_DEFAULT_POINTS;`.
A service draws with these until the settings service serves its keys,
and whenever that service holds no value for a key. A setting's type
is added here with the first setting that needs it; the reserved keys
of [display.md D12](display.md#d12-the-secure-attention-key-and-global-keys)
need one for key chords.

- **Why:** the compositor draws from phase 3, with its theme's
  materials and its pointer, and the settings service comes with
  phase 6 ([display.md D19](display.md#d19-the-build-plan)); one
  declaration of each default serves the compositor before the service
  exists, when the service has no value, and the Settings window's form
  later, so no default is written twice and no theme is written by
  hand into a program ([ui.md U9](ui.md#u9-theming-from-tokens)).
- **Cost:** a changed default reaches only the programs built after it,
  the cost U9 gives a theme compiled in, here for the defaults alone; a
  value a user sets reaches every program once the service serves it.
  The settings service, not the generator, must refuse one key declared
  by two descriptions.
- **Not taken:** defaults written into each program that reads them.
  Their cost is a second source of every default, and one that drifts
  from the form that shows it. Defaults kept only by the settings
  service. Their cost is a compositor with no theme until phase 6, and
  none whenever the service is down.

## P13. Descriptions, commands and the later outputs

**Decided.**

1. The interface and every constant, rights constant, structure,
   field, channel, message, tail, handle, handle count and setting has a
   description, and the generator refuses a declaration without one.
   The header carries each as its Doxygen comment, and the later outputs
   read the same text.
2. `command WORD` in a request names the nvo command the request
   becomes, a word unique in the description
   ([shell.md L24](shell.md#l24-settings-and-service-commands), rule 6).
   The C header makes nothing of it; the generator checks its form and
   that it is unique.
3. The nvo commands, the Settings window's forms and the documentation
   are made by emitters built with the shell's settings service, in
   the shell's build plan. They read the settings and their schemas,
   the requests marked `command` with their fields, tails, handles and
   replies, and the descriptions. Their rules, how a command's
   parameters come from a request's fields, how the shell finds a
   service's commands and reaches it through a handle its session
   holds, and how a form shows each type, are added to this item with
   them; a construct they need beyond these is a change to the
   language made with them.

- **Why:** the constructs those outputs read exist and are checked from
  the first description, so no description needs a new pass over its
  text when they arrive; the documentation is the same words
  as the header's comments. `wayland.xml` likewise carries a
  `description` on each interface, request and event and a `summary`
  on each argument it generates code for
  ([Wayland](README.md#display-and-interface-design-references)).
- **Cost:** a description on every field, which the hand-written
  headers already have; `command` lines that make nothing yet.
- **Not taken:** documentation kept in separate files. Its cost is a
  second text for every message. Every construct deferred to its
  emitter. Its cost is every description reopened when the emitters
  come.

## P14. The generated header

**Decided.**

1. **The file.** `include/NAME_protocol.h`, guarded by
   `NOVA_NAME_PROTOCOL_H` in capitals. It opens with a `/** @file`
   comment: the interface's description, then "Generated by
   tools/mkinterface.c from interfaces/NAME.desc
   (docs/spec/interface.md)." It includes `<stddef.h>`, for `offsetof`,
   then `"native_abi.h"`, then each import, and asserts
   `CHANNEL_MAX_BYTES`, `CHANNEL_MAX_HANDLES`, `CHANNEL_MAX_CAPACITY`
   and each import. Then the declarations follow in the description's
   order: constants, rights, structures with their assertions, setting
   defaults, and each channel's macros, limits and functions, inside a
   Doxygen `@name` group titled with the channel's name and holding its
   description and its policies' sentences.
2. **The C.** Freestanding C99 with `_Static_assert` and `offsetof`, as
   the tree's headers are, which TinyCC compiles and the tree checks as
   `-std=c11`. It uses `types.h`'s types and `native_abi.h`'s
   `RECEIVED_HANDLE` and `HANDLE_TRANSFER`. It calls nothing: it
   assigns scalars and copies bytes in loops, and never assigns a
   structure or initializes a local aggregate, which TinyCC turns into
   calls to `memmove` and `memset` (`tccgen.c`, `vstore` on a target
   without its native structure copy, such as ntdll's i386, and
   `init_putz`), since no program here links a C library. It makes no
   system call and does not include `native.h`. Every parameter is used
   or cast to `void`; every comparison is made in `UINT64`, or `INT64`
   for a signed field; no comparison is one the type decides. It is
   clean under clang's `-Wall -Wextra -pedantic -Wshadow` and TinyCC's
   `-Wall`.
3. **Inline functions.** Every function is `static inline`. TinyCC
   compiles one only where it is used: "static inline functions are
   just recorded as a kind of macro. Their code will be emitted at the
   end of the compilation unit only if they are used" ([TinyCC
   `tccgen.c`][tcc-gen]). Wayland's scanner does the same: "The
   protocol entry points are generated as inline functions"
   ([Wayland](README.md#display-and-interface-design-references),
   `Protocol.md`).
4. **Names.** For interface `i` and channel `c`, `P` is `I_C` in
   capitals and `p` is `i_c`; for message `m`, tail `t`:

   | Name | What it is |
   |---|---|
   | `P_TO_SERVER_BYTES_MAX`, `P_TO_SERVER_HANDLES_MAX` | The most bytes and handles a request on the channel holds, where it has requests; each asserted within the channel's limits |
   | `P_TO_CLIENT_BYTES_MAX`, `P_TO_CLIENT_HANDLES_MAX` | The same for a reply or an event |
   | `P_UNNAMED` | `0xFFFFFFFF`, a dispatcher's `which` for a message too short to hold its selector, on a channel with `select` |
   | `p_m_check` | `UINT32 p_m_check(const void *bytes, UINT32 byte_count, const RECEIVED_HANDLE *handles, UINT32 handle_count)`: the check of [P10](#p10-checking-a-message), rule 1 |
   | `p_request_check`, `p_event_check` | `UINT32 p_request_check(const void *bytes, UINT32 byte_count, const RECEIVED_HANDLE *handles, UINT32 handle_count, UINT32 *which)`: the dispatcher of P10, rule 2, for a direction with a selector, writing the selector's value or `P_UNNAMED` to `*which` |
   | `p_m_encode` | Lays message m out; below |
   | `p_m_t` | `const UINT8 *p_m_t(const void *bytes)` for a `bytes` or `text` tail, `const S *p_m_t(const void *bytes)` for a structure's: where the tail starts in a message whose check passed |

   A retired message has its macro and none of these. A name longer
   than 63 characters, one the description declares too, or one two
   rules above both make, is refused.
5. **Encoding.** `UINT32 p_m_encode(BODY, TAILS, HANDLES, void *bytes,
   UINT32 *byte_count)` takes `const S *body` for a structure body,
   `UINT32 transaction` for `body transaction` and nothing for `none`;
   then for each tail in order `const S *t` for one structure, or
   `const UINT8 *t, UINT32 t_count` for bytes or text, or
   `const S *t, UINT32 t_count` for a counted structure tail; then,
   where the message has handles, `const UINT32 *handles,
   UINT32 handle_count, HANDLE_TRANSFER *transfers`. It copies the body
   byte by byte, so `body` may point at `bytes`; sets the selector to
   the message's value, each count field to its tail's count and each
   `= tails` field; appends the tails, each of which lies where it is
   written or outside the message; fills `transfers` with each handle
   and its `travels` rights; and checks the bytes and the number of
   handles as the receiver's check does. It returns `OK` with the
   message's size in `*byte_count`, or `ERR_INVALID_ARGS` with 0 there.
   A reply whose status is not 0 takes tails of count 0 and no handles,
   whatever the minimums of its runs are, and the encoding writes no
   transfer for it; the number of handles is the runs' only where the
   status is 0.
6. **Buffers.** `bytes` is aligned to 8 bytes and holds at least the
   direction's `BYTES_MAX`, for encoding and for reading alike; a
   receiver reads a message's body by casting `bytes` once the check
   passes, and a structure tail through its accessor, which returns a
   pointer aligned for the structure ([P25](#p25-structure-tails)).
7. **Literals and comments.** A literal is written as the description
   writes it, a string with each `?` as `\?`
   ([P24](#p24-text-in-a-description-and-in-a-string)); a computed size
   in decimal. A description written above
   its declaration becomes a `/** */` comment above the declaration, its
   lines as written; one written on the declaration's own line becomes
   a `/**< */` comment after it, and a structure's trailing comments
   start in one column, a space past its longest field that has one.
   Indentation is four spaces. A generated declaration's comment is one
   fixed sentence for its kind followed by its message's or channel's
   description, and names no parameter, so Doxygen's
   `WARN_IF_INCOMPLETE_DOC` has nothing to flag.

- **Why:** one header holds everything a program needs to speak a
  protocol, so nothing changes in `build.sh`'s program lines; a program
  pays for the functions it calls; and every source that includes the
  header has clang check its functions. Pure functions run on the host,
  so a host check exercises them.
- **Cost:** a function used in two sources of a program is compiled
  twice into it; the generated names are long.
- **Not taken:** a `.c` file for each interface linked into each
  program that speaks it. Its cost is a source more on every program's
  line in `build.sh`, and code in programs that call none of it. Tables
  that a library interprets to encode and check. Their cost is a table
  format and an interpreter, and checks a reader cannot see as C.

## P15. The generator

**Decided.**

1. `tools/mkinterface.c` is one file of C99 using only `<stdio.h>`,
   `<stdlib.h>` and `<string.h>`, held to `tools/compile_flags.txt`.
   `build.sh` builds it with the host tools, before anything is
   downloaded: `cc "$HOST_OPTIMIZATION" -o "$out/mkinterface"
   tools/mkinterface.c`. In the Seed job `cc` is TinyCC built from the
   seed.
2. `mkinterface DESCRIPTION HEADER` reads DESCRIPTION whole, at most
   `DESCRIPTION_MAX` (1 MiB), opened `"rb"`; checks it against every
   rule of this document; builds the header in memory; and only then
   writes HEADER whole, opened `"wb"`, with LF line ends on every host.
   It exits 0; or 1, writing no file and one line,
   `DESCRIPTION:LINE: what is wrong`, on standard error; or 2, with its
   usage, for the wrong arguments.
3. The header is a function of the description's bytes alone: no time,
   no path but the description's file name, no host, no locale. The
   tool computes in unsigned 64-bit arithmetic and formats numbers
   itself.
4. The Lint job runs `ci/check-mkinterface.sh`, which runs the tool on
   each description in `ci/mkinterface/`, each breaking one rule of
   this document and naming in its first comment the line the refusal
   must give, and fails unless each is refused at that line. A rule the
   generator gains gets a description there. The script then makes the
   header of `ci/probe/probe.desc`, which uses what the descriptions in
   `interfaces/` do not, and runs `tools/check-probe-protocol.c` against
   it under the sanitizers; output the generator must get right gets a
   line in that description and a check there.

- **Why:** the seed builds every byte (DESIGN.md, principle 1), so the
  tool is C a TinyCC from the seed compiles, as every host tool here is;
  `"wb"` and the tool's own formatting give the same header on Linux,
  macOS and Windows.
- **Cost:** a parser and a generator written in C.
- **Not taken:** a Python generator. Its cost is a Python the seed does
  not build, in a build that needs only `cc`, `curl`, `tar` and `unzip`.

## P16. Where the header lives

**Decided.** The generated header is tracked in `include/`, where
every protocol header is. For each description it names, `build.sh`
runs `"$out/mkinterface" interfaces/NAME.desc
"$out/include/NAME_protocol.h"` and compares the result with the
tracked header using `cmp`, which busybox-w32 has; on any difference it
stops, naming the command that regenerates the tracked one. Programs
are built from the tracked header. `SHA256SUMS` lists
`include/NAME_protocol.h`, as it lists every file the build writes and
must reproduce, and `ci/update-hashes.sh OUT include/NAME_protocol.h`
adds a new header's line.

- **Why tracked:** `ci/clang-warnings.sh` checks every tracked `.c`
  file with the flags of its nearest `compile_flags.txt`, which name
  `-I../include`, so each source that includes the header is checked
  with it, its functions and all, only because the header is there.
  Doxygen reads `include`, `lib` and `native`, and fails on a
  declaration without a comment (`Doxyfile`, `WARN_AS_ERROR`), so the
  header joins the API reference with its descriptions. clangd and a
  reader find it where they find every header, and a change to a
  description shows the C it makes in the same diff.
- **Why the comparison in `build.sh`:** every CI job and the Seed job
  run `build.sh`, so a stale header fails every build, on all three
  hosts' compilers, before anything is compiled from it.
- **Cost:** the header is kept twice, in the tree and in the build, and
  a description's change is three files' change.
- **Not taken:** the header written only into the build's output. Its
  cost is a lint that fails on every source including it, or that
  builds the generator first with `compile_flags.txt` naming a build
  directory; the same for Doxygen; and C a change makes that review
  never sees.

## P17. The console first

**Decided.** The console's protocols, its session, its control channel
and its bootstrap channel
([console.md C2](console.md#c2-starting-the-console) and
[C4](console.md#c4-sessions)), move over first, in two steps:

1. **The header.** `interfaces/console.desc`
   ([P19](#p19-the-consoles-description)) and the generator replace the
   hand-written `include/console_protocol.h` with the generated one.
   `SHA256SUMS` gains the header's line, and no other line changes:
   every program built against the generated header is, byte for byte,
   the one built against the hand-written header. TinyCC compiles no
   inline function a program does not call, so that holds exactly when
   every name the programs use keeps its value and its layout, which is
   what proves the description reproduces the header.
2. **The functions.** The console's checks of its bootstrap, control
   and session messages (`console/console.c`), the first process's
   setups (`init/console.c`), and a client's reads and writes and its
   check of a line (`lib/console.c`) use the generated functions in
   place of their own.
   `tools/check-console-protocol.c`, which the Lint job builds with the
   host's compiler and runs, as it runs `tools/check-text.c`, and which
   `ci/clang-warnings.sh`'s list of C11 sources gains, encodes each
   message with the values of console.md C2's and C4's tables and
   requires the bytes a structure laid out by hand holds, and checks
   that each message C4 says ends a session fails its check. Every boot
   in CI then sends each of the console's messages. The lines of
   `console.exe`, `init.exe`, `login.exe` and `shell.exe` change.

A message the console gains later is added to its description, not to
the header.

- **Why the console:** its three channels use most constructs the
  compositor's phase 3 needs: requests with replies and without, events,
  a selector inside an embedded setup, a `first` message, a body that is
  a transaction alone, bytes counted by a field, handles of four types
  in runs of 0 to 64 whose needs differ from what they travel with, the
  shared `BOOTSTRAP_DISPLAY` and `BOOTSTRAP_BACKGROUND` imported, three
  of the four policies, and a capacity. It is the compositor's
  neighbour in phase 3, both its ends are in this tree, and every boot
  in CI exercises every message, so a mistake fails CI. The display's
  description adds the rest: replies with a `status`, structure and
  `text` tails, a counted `array`, `u16`, `i32` and `u64` fields, `in`
  constraints, events beside replies on one channel, and the `material`,
  `outline`, `colour` and `int` settings; the generator's refusal tests
  ([P15](#p15-the-generator)) hold a case of each, and its header is
  compiled by a host check before any program includes it.
- **Cost:** the header's every name must keep its value, so the
  description names things as the header did; step 2 changes four
  programs, and a malformed message the console refuses is refused in
  the check's order, which its log lines show.
- **Not taken:** the block protocol, the smallest. Its cost is a proof
  of one channel with no event, no import and no shared type. The input
  protocol. Its cost is a proof of events alone, from drivers whose types
  the compositor's description imports. The directory protocol. Its
  cost is the largest change first, every service at once, and the two
  `bytes` tails of [P18](#p18-the-hand-written-protocols-in-the-language).

## P18. The hand-written protocols in the language

**Decided.** Each hand-written protocol header can be described with
every byte where the header puts it; the table gives how, and what stays the
service's own.

| Header | Described as | Outside the language |
|---|---|---|
| `console_protocol.h` | [P19](#p19-the-consoles-description) | A session's one read at a time (console.md C4) |
| `input_protocol.h` | One channel, the driver its server; events selected by the `u16` `type`; the description a `first` event; `INPUT_POINTER`'s `button` an array counted by `buttons`; signed `i32` fields | Which events follow, by the description's kind |
| `block_protocol.h` | Requests selected by `operation`, each with a `BLOCK_REPLY` reply with `status`; `BLOCK_ATTACH`'s section `travels read write pin transfer needs read write pin`; `invalid server answer` with that reply | The attach first and once. A message under four bytes is closed, where the driver answers it with transaction 0 |
| `file_protocol.h` | `FILE_REQUEST` requests selected by `operation`; an open's endpoint a handle and its reply `on` the opened channel; reads' and writes' bytes counted by `length` and `information`; `FILE_REPLY`'s `status`, whose 0 is `FILE_OK` | The open's disposition and access, NT's values |
| `directory_protocol.h` | `IO_REQUEST` and `IO_REPLY` with `status`; an open's reply `on` its endpoint, with `IO_ATTRIBUTES` a structure tail; a rename's two paths, `count offset` then `rest`; a section's handle on a status-0 reply | `IO_READ_DIR`'s entries, each followed by its name, and a link's target and the rest of its path are `bytes` tails the service reads. A message of 4 to 31 bytes is answered, where the server closes its channel |
| `accounts_protocol.h` | `IO_REQUEST` and `IO_REPLY` imported with their fields; credentials a structure tail and the name and password tails it counts; `ACCOUNTS_USER`'s `name` an array counted by `name_length`; a login's reply with the home directory and an administrator channel of `min 0`; the control channel's setups `invalid server drop`; the bootstrap `first`, `body none` | Which requests a login channel and an administrator channel may send, the service's to answer `ERR_ACCESS_DENIED` |
| `session.h` | Login's bootstrap `first`, `body none`, its path a `bytes rest` tail; `LOGIN_SETUP`; the shell's bootstrap `first`, its body the imported `ACCOUNTS_USER`; a run's `LAUNCHER_RUN_REQUEST` tail and the path and arguments it counts; the string constants | That each argument ends in a NUL |

`services.h`'s bootstrap messages and `STORE_OPEN_CLOSURE` take the
same forms. `native_abi.h` is not described ([P1](#p1-the-parts)).

- **Why:** shell.md L24 has every existing protocol move over, so the
  language must hold each as it is on the wire; the table shows it
  does, and where a service keeps a rule of its own.
- **Cost:** the two `bytes` tails above; and when the block and
  directory protocols move over, a message too short to answer is
  handled as `answer` gives, a change their conversions state.
- **Not taken:** a tail present on a condition and a list of records
  with their own bytes. Their cost is two constructs, each for one
  protocol's one part.

## P19. The console's description

**Decided.** `interfaces/console.desc`, as [P17](#p17-the-console-first)'s
first step commits it, reproducing the hand-written header, is the text
below. Phase 3 then adds to it [console.md C2](console.md#c2-starting-the-console)'s
kinds 4 to 6 and makes `CONSOLE_CONTROL_CAPACITY` `CONSOLE_SESSIONS` + 3.
Its `background` request and `prepared` reply are marked `retired`
([P11](#p11-numbers-and-change), rule 3;
[console.md C1](console.md#c1-the-display-handoff)), so the tracked
header keeps `CONSOLE_SETUP_BACKGROUND` and
`CONSOLE_SETUP_BACKGROUND_MESSAGE`, has no function for either message,
and has no `CONSOLE_PREPARED_SIZE`, which the tracked description does
not declare:

```
# The console's protocols (docs/spec/console.md): a client's session,
# over which it writes text and reads lines typed at the console (C4),
# and the messages with which the first process starts the console and
# hands it the display and its sessions (C2). Every field is
# little-endian.
interface console

import native_abi.h
  const BOOTSTRAP_BACKGROUND_SECTIONS_MAX 64
  struct BOOTSTRAP_DISPLAY 24
  struct BOOTSTRAP_BACKGROUND 16
end

# The most bytes one write shows.
const CONSOLE_WRITE_MAX 4096
# The most bytes one line holds.
const CONSOLE_LINE_MAX 256
# The messages the console's end of a session queues (ipc.md I1); a
# client that fills it waits for CHANNEL_WRITABLE.
const CONSOLE_SESSION_CAPACITY 16
# The sessions the console serves at once.
const CONSOLE_SESSIONS 8
# A read's flag: the line is not shown as it is typed, nor is its end.
const CONSOLE_READ_NO_ECHO 0x1
# The bytes of the answer to CONSOLE_SETUP_BACKGROUND.
const CONSOLE_PREPARED_SIZE 4
# The messages the control channel queues: the background, the display
# and a session for each the console serves.
const CONSOLE_CONTROL_CAPACITY CONSOLE_SESSIONS + 2

# A client's end of a session: it writes requests and reads replies,
# and the end may move to the program that is to use it.
rights CONSOLE_SESSION_RIGHTS channel read write wait transfer

# A request on a session.
struct CONSOLE_REQUEST
  u32 transaction  # echoed in a read's reply (ipc.md I3)
  u32 operation    # CONSOLE_WRITE or CONSOLE_READ
  u32 length       # a write's bytes, or a read's most
  u32 flags        # 0, or CONSOLE_READ_NO_ECHO on a read
end

# The reply to a read, which the line follows.
struct CONSOLE_REPLY
  u32 transaction  # the read's
  u32 length       # the line's bytes, which follow
end

# A message on the control channel.
struct CONSOLE_SETUP
  u32 transaction = 0  # 0
  # CONSOLE_SETUP_DISPLAY, CONSOLE_SETUP_SESSION or
  # CONSOLE_SETUP_BACKGROUND
  u32 kind
end

# The bytes of a setup.
const CONSOLE_SETUP_SIZE sizeof CONSOLE_SETUP

# The setup that hands the console the display.
struct CONSOLE_SETUP_DISPLAY_MESSAGE
  CONSOLE_SETUP setup        # CONSOLE_SETUP_DISPLAY
  BOOTSTRAP_DISPLAY display  # the display, as nova-boot drew on it
end

# The setup that offers the console the kernel's background, to
# prepare its panel from before the display comes (console.md C5).
struct CONSOLE_SETUP_BACKGROUND_MESSAGE
  CONSOLE_SETUP setup  # CONSOLE_SETUP_BACKGROUND
  # the display the console is to be given
  BOOTSTRAP_DISPLAY display
  # what the sections hold, as the kernel labelled them
  BOOTSTRAP_BACKGROUND background
end

# The channel the platform driver keeps to the input drivers' client
# (input.md K3), as the console takes it: its bootstrap message, then
# one input endpoint a message.
channel bootstrap
  invalid client exit

  # The console's bootstrap message: a transaction ID of 0 and these
  # handles in this order.
  event start
    body transaction
    first
    # the debug log's resource
    handle resource travels use needs none index CONSOLE_BOOTSTRAP_DEBUG_LOG
    # its process, with MAP
    handle process travels map needs map index CONSOLE_BOOTSTRAP_SELF
    # its end of the control channel
    handle channel travels read write wait needs read write wait index CONSOLE_BOOTSTRAP_CONTROL
    handles CONSOLE_BOOTSTRAP_HANDLES  # how many handles it carries
  end

  # An endpoint to read a description and then key events or pointer
  # reports from, as a driver reported it (input_protocol.h).
  event endpoint
    body transaction
    handle channel travels read wait needs read wait  # the endpoint
  end
end

# The control channel, from the first process: a CONSOLE_SETUP, its
# kind saying what follows it and what handles it carries. The console
# answers every CONSOLE_SETUP_BACKGROUND, so its end carries
# CHANNEL_RIGHT_WRITE as well as CHANNEL_RIGHT_READ and RIGHT_WAIT.
channel control
  capacity CONSOLE_CONTROL_CAPACITY
  select kind
  invalid server drop
  invalid client drop

  # The display.
  request display CONSOLE_SETUP_DISPLAY 1
    body CONSOLE_SETUP_DISPLAY_MESSAGE
    # an MMIO resource covering the framebuffer's pages
    handle resource travels use needs none
  end

  # A session.
  request session CONSOLE_SETUP_SESSION 2
    body CONSOLE_SETUP
    # the console's end of the session
    handle channel travels read write wait needs read write wait
  end

  # The kernel's background, before the display.
  request background CONSOLE_SETUP_BACKGROUND 3
    body CONSOLE_SETUP_BACKGROUND_MESSAGE
    # the background's sections, mapped one after another
    handle section min 0 max BOOTSTRAP_BACKGROUND_SECTIONS_MAX travels read needs read
    reply prepared
  end

  # The answer to a background, once the panel is prepared from it or
  # it is refused: a transaction ID of 0 and no handles.
  reply prepared
    body transaction
  end
end

# Sessions (C4). A client writes requests on its end of a session; the
# console answers each read with a CONSOLE_REPLY and the line.
channel session
  capacity CONSOLE_SESSION_CAPACITY
  select operation
  invalid server close
  invalid client drop

  # Show the length bytes that follow the request. No reply.
  request write CONSOLE_WRITE 1
    body CONSOLE_REQUEST
    field flags = 0
    tail text bytes count length max CONSOLE_WRITE_MAX  # the bytes shown
  end

  # Read a line of at most length bytes, edited at the console and
  # ended by Enter. The reply carries the request's transaction ID, the
  # line's length and the line, printable ASCII without its end. A
  # session has at most one read waiting.
  request read CONSOLE_READ 2
    body CONSOLE_REQUEST
    field length min 1 max CONSOLE_LINE_MAX
    field flags mask CONSOLE_READ_NO_ECHO
    reply line
  end

  # The line a read asked for, printable ASCII without its end.
  reply line
    body CONSOLE_REPLY
    tail line bytes count length max CONSOLE_LINE_MAX  # the line
  end
end
```

The header it makes declares these structures, and the constants in
the table after them, which are the hand-written header's names,
values and layouts:

```
typedef struct {
    UINT32 transaction; /**< echoed in a read's reply (ipc.md I3) */
    UINT32 operation;   /**< CONSOLE_WRITE or CONSOLE_READ */
    UINT32 length;      /**< a write's bytes, or a read's most */
    UINT32 flags;       /**< 0, or CONSOLE_READ_NO_ECHO on a read */
} CONSOLE_REQUEST;

typedef struct {
    UINT32 transaction; /**< the read's */
    UINT32 length;      /**< the line's bytes, which follow */
} CONSOLE_REPLY;

typedef struct {
    UINT32 transaction; /**< 0 */
    /** CONSOLE_SETUP_DISPLAY, CONSOLE_SETUP_SESSION or
        CONSOLE_SETUP_BACKGROUND */
    UINT32 kind;
} CONSOLE_SETUP;

typedef struct {
    CONSOLE_SETUP setup;       /**< CONSOLE_SETUP_DISPLAY */
    BOOTSTRAP_DISPLAY display; /**< the display, as nova-boot drew on it */
} CONSOLE_SETUP_DISPLAY_MESSAGE;

typedef struct {
    CONSOLE_SETUP setup; /**< CONSOLE_SETUP_BACKGROUND */
    /** the display the console is to be given */
    BOOTSTRAP_DISPLAY display;
    /** what the sections hold, as the kernel labelled them */
    BOOTSTRAP_BACKGROUND background;
} CONSOLE_SETUP_BACKGROUND_MESSAGE;
```

| Name | Value | From the line |
|---|---|---|
| `CONSOLE_WRITE`, `CONSOLE_READ` | 1, 2 | `request write CONSOLE_WRITE 1`, `request read CONSOLE_READ 2` |
| `CONSOLE_READ_NO_ECHO` | `0x1` | `const` |
| `CONSOLE_WRITE_MAX`, `CONSOLE_LINE_MAX` | 4096, 256 | `const` |
| `CONSOLE_SESSION_CAPACITY`, `CONSOLE_SESSIONS` | 16, 8 | `const` |
| `CONSOLE_BOOTSTRAP_DEBUG_LOG`, `_SELF`, `_CONTROL` | 0, 1, 2 | each handle's `index` |
| `CONSOLE_BOOTSTRAP_HANDLES` | 3 | `handles` |
| `CONSOLE_SETUP_DISPLAY`, `_SESSION`, `_BACKGROUND` | 1, 2, 3 | the control channel's requests |
| `CONSOLE_SETUP_SIZE` | 8 | `const CONSOLE_SETUP_SIZE sizeof CONSOLE_SETUP` |
| `CONSOLE_PREPARED_SIZE` | 4 | `const` |
| `CONSOLE_CONTROL_CAPACITY` | `(CONSOLE_SESSIONS + 2)` | `const` |
| `CONSOLE_SESSION_RIGHTS` | `(CHANNEL_RIGHT_READ \| CHANNEL_RIGHT_WRITE \| RIGHT_WAIT \| RIGHT_TRANSFER)` | `rights` |

The sizes 16, 8, 8, 32 and 48 are asserted, and so is
`CONSOLE_SESSION_TO_SERVER_BYTES_MAX`, 4,112, within
`CHANNEL_MAX_BYTES`, the assertion that a whole write fits one
message. The checks are those the console and its clients make
themselves: a write's flags 0, its length at most `CONSOLE_WRITE_MAX`
and its bytes exactly its length; a read's length 1 to
`CONSOLE_LINE_MAX`, its flags within `CONSOLE_READ_NO_ECHO`, and no
bytes after it; no handle on a session; a setup's transaction 0, its
size its kind's, and its handles' types and rights; the bootstrap
message's three handles and an endpoint's one.

- **Why:** each line is a fact console.md C2 and C4 already decide, so
  the description adds no behaviour; the step 1 check of
  [P17](#p17-the-console-first) shows the header is the same.
- **Cost:** the console's names keep forms the generator would not
  make, `CONSOLE_SETUP_DISPLAY` for the control channel's `display`
  request among them, so its macros and its functions follow two
  patterns.
- **Not taken:** renaming the console's messages to the generator's
  pattern on the way over. Its cost is a change to every source that
  uses them in the step meant to change no byte.

## P20. Handle runs

**Decided.**

1. In a `handle` line, `min` and `max` come together: a line with one of
   them alone is refused at its line, and so is a `min` above its `max`.
2. The generator adds each `handle` line's largest count to the
   message's total as it reads the line, and refuses a message whose
   total would pass `CHANNEL_MAX_HANDLES` at the message's own line,
   whatever any one count is, so no count wraps the sum
   ([P7](#p7-channels-and-messages), rule 8).

- **Why:** a lone `min 1` or a lone `max 4` has two readings, a run
  that ends at `CHANNEL_MAX_HANDLES` or at the other bound's default,
  and a description read each way gives its receiver different handles.
  A count is any number up to 2^64 - 1, so the total is checked before
  it is made.
- **Cost:** a run of no minimum is written `min 0 max 4`.
- **Not taken:** a lone `min` running to `CHANNEL_MAX_HANDLES` and a
  lone `max` running from 0. Their cost is a default every reader must
  know, in a line whose point is the receiver's authority.

## P21. Arrays and constraints

**Decided.**

1. A constraint on an array field is refused. A scalar takes a
   constraint ([P6](#p6-layout), rule 3), and an array's elements that
   need one are the fields of a structure.
2. The constraints of a structure hold for every element of an array of
   it: the counted elements of an array with `count`, and all the
   elements of one without. A message's check applies them after the
   array's count, its zeros and its text, in the elements' order
   ([P10](#p10-checking-a-message), rule 1). An element past the count
   is 0 and is held to nothing.
3. A structure that holds an array, directly or in a structure it
   embeds, is no element: an array of such structures, and a structure
   tail of one, are refused, since no check walks the arrays inside an
   element.
4. A value of a constraint is at most what the field's type holds, for a
   signed type its largest positive number, and a `mask` is within it;
   a larger one is refused at its line.
5. An array whose size in bytes passes 2^64 - 1, and a structure whose
   size does, are refused at the field's line, so no size wraps.

- **Why:** a receiver trusts every field of a structure the check
  passed, and an element is a structure like the body's own; a bound or
  a mask above the type is a check that never fails or always does, and
  the value is a mistake whichever it is.
- **Cost:** an array of scalars that must lie in a range is an array of a
  structure of one field; a list of lists is a structure tail or an
  array of structures with no array in them.
- **Not taken:** a constraint on an array that holds for each element.
  Its cost is a second reading of a scalar's constraint, and its
  meaning for `text` and for counts. Nested loops in the check for
  arrays inside elements. Their cost is a local for each depth in
  functions that declare few, for a shape no protocol here has.

## P22. Constants as operands

**Decided.**

1. A `rights` constant is no value of P3's grammar: a name that holds
   one in an expression, a constraint, an array's size or a tail's `max`
   is refused at its line. It names a set of rights for the `travels`
   and `needs` of a `handle` line, whose macros the header joins
   ([P4](#p4-constants-and-expressions), rule 4).
2. A constant that is a number has the value the generator computed, and
   the header spells each expression so that C computes it: a level of parentheses where an operand or a result
   passes what a C `int` holds starts with `1ULL * `, and any other is
   written as the description has it (P4, rule 2).

- **Why:** the generator knows a right by its name and `native_abi.h`
  alone gives it a number, so it cannot compute with one and keeps one
  source of the numbers. The values it checks sizes and limits against
  are the values a compiler computes, so a constant that wrapped in C's
  `int` arithmetic would be a different number in the header from the one
  a message was checked against, with no diagnostic from either.
- **Cost:** a set of rights is not combined with another in a constant;
  a header's constants that pass a C `int` carry `1ULL * `.
- **Not taken:** the generator holding the rights' numbers. Its cost is a
  second copy of `native_abi.h`'s constants that must change with it. A
  suffix on every number the header writes. Its cost is every constant
  of the shipped headers changing, nearly all of them within an `int`.

## P23. A reply on another channel

**Decided.** In `reply NAME on CHANNEL`
([P7](#p7-channels-and-messages), rule 5), the generator checks that:

1. CHANNEL is another channel of the description, and has a reply
   NAME.
2. That reply is marked `first` in its channel: it is the first message
   the server sends on the end the request carries.
3. The request has a `handle` line of type `channel`: the end the reply
   goes on.

Each is refused at the request's line. A request that carries two
channel ends does not say in the language which the reply uses; its
description does.

- **Why:** these are the three facts rule 5 states, so a reply that no
  server sends first, or on an end the request never gave, is refused
  where it is written and not found by a boot.
- **Cost:** the language does not tie the reply to one of several
  channel ends; a protocol that needs it describes it in the request's
  comment.
- **Not taken:** naming the handle, as in `reply NAME on CHANNEL via
  INDEX`. Its cost is a construct for a request with two channel ends,
  which no protocol here has.

## P24. Text in a description and in a string

**Decided.**

1. A description that holds `/*` or `*/`, or has a line that ends in
   `??/`, is refused at its declaration's line. The header carries it
   as a comment: `*/` ends the comment early; `/*` inside a comment
   makes a compiler warn, and the tree compiles with warnings as
   errors; and `??/` at the end of a line is a backslash to a compiler
   that reads trigraphs, which joins the line to the next.
2. The header writes each `?` of a string as `\?`, in a constant's string
   and in a text setting's default, so that no two question marks and a
   character form a trigraph, which a compiler reads before it reads the
   string. `const S "a??/"` gives `#define S "a\?\?/"`, the same
   characters.

- **Why:** a description is text the author writes freely, and a
  header that fails to compile for a character in it fails far from the
  line that holds it; the escape is the one C gives for this.
- **Cost:** a description cannot hold `/*`, `*/` or a line ending in
  `??/`; a string is longer in the header than in the description by a
  backslash for each `?`.
- **Not taken:** refusing a `?` in a string. Its cost is a text such as
  a prompt that cannot hold a question mark. Splitting a string into
  two literals between question marks. Its cost is a rule to find the
  places, where an escape on each needs none.

## P25. Structure tails

**Decided.**

1. A structure tail starts at a multiple of its structure's alignment,
   its largest scalar's size, wherever the tails before it end: the
   body's size and the sizes of the structure tails before it add up to
   a multiple of it, and every counted tail before it holds elements
   whose size is one. A `bytes` or `text` tail holds elements of 1 byte,
   so a structure that needs more alignment does not follow one. A tail
   that breaks this is refused at its line.
2. A structure used as a tail has its alignment known: an imported
   structure used as one lists its fields ([P5](#p5-imports-and-the-kernels-vocabulary),
   rule 1). A structure with an array is no tail's structure
   ([P21](#p21-arrays-and-constraints), rule 3).
3. `field PATH = tails` ([P8](#p8-tails), rule 4) names a field of the
   body. A field of a structure tail is refused at its line.

- **Why:** the accessor `p_m_t` returns `const S *` into the message,
  and a pointer not aligned for S is undefined to use and a fault on
  some targets, so a layout with no aligned reading is refused where it
  is written. The encoding writes the `= tails` field with the body's,
  from the total, before any tail's fields exist to write into.
- **Cost:** a structure tail after a `bytes` or `text` tail, or after a
  body that is not a multiple of its alignment, is written with the
  padding the author names, in the body or in a structure, or with the
  structure first.
- **Not taken:** an accessor that copies the structure out, which
  reads at any start. Its cost is a copy in every receiver, and a
  signature unlike the body's view. Padding a tail's start in the
  encoding. Its cost is bytes between tails that every count and the
  `rest` tail must then account for.

## P26. The generator's limits

**Decided.** A description that fills one of the generator's tables is
refused at the line that overflows it, naming the table, as `too many
constants` does, and a description past its size at line 0, as no line
holds it:

| Table | Holds |
|---|---|
| A line | 4,096 bytes and 1,024 words |
| A description | 1,048,576 bytes ([P15](#p15-the-generator)) |
| C names | 4,096 |
| Constants | 1,024 |
| `rights` constants | 64 |
| Structures | 256 |
| Fields | 2,048, and 96 in a structure |
| Imports | 16, naming 256 constants and structures in all |
| Channels | 32 |
| Messages | 256 |
| In a message | 8 tails, 16 `field` lines, 16 `handle` lines, and 1,024 constrained fields and 1,024 arrays to check |
| The documentation of a declaration | 512 lines, the sentence the generator adds included, refused at the first |
| An `in` constraint | 16 values |
| Settings | 128, and 32 allowed words in one |
| An outline | 16 contours and 512 points |
| Names the generator makes | 8,192 |

- **Why:** the tool is C99 with the standard library and a seed's
  compiler, and a fixed table is one comparison to check where a growing
  one is an allocation to make and release in each place; every
  description in the tree holds a small part of each.
- **Cost:** a description that outgrows a table raises it in the tool,
  with a case for it in `ci/mkinterface/` and a change to this table.
- **Not taken:** tables that grow with the description. Their cost is
  an allocation for each, and the limit of memory in place of one the
  author can read.

## References

Fuchsia, FIDL documentation, with the date each page shows as its last
update: [FIDL overview][fidl-overview] (2026-10-30), [FIDL language
specification][fidl-language] (2026-02-03), [FIDL wire format
specification][fidl-wire] (2026-05-11).

TinyCC, the source `build.sh` builds, at its pinned commit
`cb41cbfe717e4c00d7bb70035cda5ee5f0ff9341`: [`tccgen.c`][tcc-gen], for
inline functions (`gen_inline_functions` and the declaration that
records them), structure assignment (`vstore`) and the zeroing of a
local aggregate (`init_putz`), and `x86_64-gen.c`, which defines
`TCC_TARGET_NATIVE_STRUCT_COPY`, where `i386-gen.c` does not.

[fidl-overview]: https://fuchsia.dev/fuchsia-src/concepts/fidl/overview
[fidl-language]: https://fuchsia.dev/fuchsia-src/reference/fidl/language/language
[fidl-wire]: https://fuchsia.dev/fuchsia-src/reference/fidl/language/wire-format
[tcc-gen]: https://github.com/TinyCC/tinycc/blob/cb41cbfe717e4c00d7bb70035cda5ee5f0ff9341/tccgen.c
