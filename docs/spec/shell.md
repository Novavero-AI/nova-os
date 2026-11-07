# The shell language

nvo is the language of nova-os's shell: typed at the console first, kept
in script files second. It is a shell language, not a general-purpose
one; its command is `nvo`, and its script files end in `.nvo` ([The
name](#the-name)). This document gives its tokens and grammar, its
values, how pipelines carry values and bytes, variables, control flow,
functions and modules, errors and exit statuses, how a command is found
and run from `/store`, the authority the shell grants a command,
interactive behaviour, what a session recording holds, how settings are
reached, and the built-ins of the first version.

The shell is an ordinary process. It holds the handles its session gives
it ([objects.md O6](objects.md#o6-no-ambient-authority)); it is a client
of the same services the graphical interface uses (DESIGN.md, principle
3); it gives each program it runs what the program's recipe declares
(principle 7) and what the user designates for that run, the paths its
arguments name, and nothing else; and every effect it has goes through a
handle or a message (principle 8). Principle 7 names only the recipe's
part; [Changes elsewhere](#changes-elsewhere) adds the user's.

Each item gives the design, why, what it costs, and the main alternative
not taken. Items the owner has ruled on are marked **Decided**; every
other item is **Proposed**.

Sources are pinned under [Shell design
references](README.md#shell-design-references) and cited by these names:

| Name | Document |
|---|---|
| POSIX | Shell Command Language, The Open Group Base Specifications Issue 8 (IEEE Std 1003.1-2024), by section |
| rc | Tom Duff, Rc - The Plan 9 Shell, by section |
| Plan 9 names | Rob Pike and others, The Use of Name Spaces in Plan 9 |
| PowerShell | Windows PowerShell Language Specification 3.0, by section |
| Nushell | The Nushell book, by page |
| fish | The fish-shell 4.9.3 documentation: Design, The fish language, Interactive use |
| Oils | The Oils 0.38.0 documentation, by document |
| Chu 2023 | Andy Chu, Oils Is Exterior-First (Code, Text, and Structured Data), June 20, 2023 |
| Elvish | Elvish's Language specification, by section, and its Unique semantics page |
| Plash | Mark Seaborn's Plash documentation: the introduction, The Plash shell, and Powerboxes |
| Capsicum | Robert N. M. Watson, Jonathan Anderson, Ben Laurie and Kris Kennaway, Capsicum: practical capabilities for UNIX (USENIX Security 2010), by section and PDF page |
| Nix | The Nix 2.35.4 Reference Manual, by command |
| Decimal | Mike Cowlishaw, General Decimal Arithmetic Specification, version 1.70, by section and page |
| CBOR | Concise Binary Object Representation, RFC 8949, by section |
| Clear | The pages that give `clear`, `cls` and Ctrl+L in other shells: ncurses's clear(1), Nushell's `clear`, PowerShell's `Clear-Host` and PSReadLine, cmd.exe's `cls`, the Bash and zsh manuals' line editors, and reedline's key bindings |

## At a glance

```
ls ~/Documents | where { $in.size > 10MB } | sort-by { $in.modified } | select name size
```

`ls` is a built-in that gives a table; `where`, `sort-by` and `select`
take its records and pass records on. Nothing is split on spaces.

```
^gzip --stdout ./report.csv | save ./report.csv.gz
```

`gzip`, an external program with no signature, receives a read-only
handle to `report.csv`, its standard streams and its own closure in
`/store`, and nothing else. `save`, a built-in, writes its bytes.

```
^tar -czf => ./site.tar.gz ./site
```

`=>` grants `tar` write access to the one name after it; `./site` is
granted read-only, with everything below it.

```
let big = (ls ./src | where { $in.size > 1MB } | get path)
^du -h ...$big
```

`...` spreads a list into separate arguments; each path is granted to
`du`, and each lies inside `./src`, the directory that produced it.

```
if (ok { ^grep -q TODO ./notes.md }) { print "notes have TODOs" }
setting set desktop.background tide
with ./tools.nix { rg TODO ./src }
run /store/<hash>-hello-2.12.1/bin/hello
grants { ^tar -czf => ./site.tar.gz ./site }
```

`ok` turns grep's exit status 1 into `false` and lets any other failure
through as an error; `setting` is the same call the Settings window
makes; `with` runs a block with programs from a recipe, and `run` a
program from the store, without installing either; `grants` shows,
without running anything, every handle the command would receive.

## The name

**Decided** (by the owner): the language is nvo, its command is `nvo`,
and a script file's extension is `.nvo`.

- **Why:** one short name for the language and its command, as rc and
  fish each name a language and its command alike. Homebrew, PyPI and
  nixpkgs have no package named `nvo`, npm holds only an unpublished
  record of one, and GitHub's Linguist lists no language for `.nvo`
  ([References](#references)). Trademark registers were not searched.
- **Cost:** a name that says nothing of what it names to someone who
  has not met it.
- **Not taken:** "NovaScript", taken twice in the same field: the npm
  package `novascript` describes itself as "an English-like programming
  language built on TypeScript" (version 3.0.1, changed 2026-03-18), and
  in Numerus "All running Numerus simulations are expressed in a
  language called NovaScript", "an extension of" JavaScript. `nsh` for
  the command, which other shells hold: Homebrew core's `nsh` formula is
  "Fish-like, POSIX-compatible shell" (nuta/nsh), which nixpkgs also
  packages, and Apache NuttX's shell is the NuttShell (NSH). `.nsh` for
  the extension, which belongs to NSIS ("Header files have the .nsh
  extension"), as Linguist maps it, and to the UEFI Shell, whose EDK II
  source sets `mScriptExtension` to `L".NSH"`. `nv`, which packages
  install as a command: npm's `nv` ("Front-end development environment")
  installs `bin/nv`, and PyPI's `nv` is "A utility for managing multiple
  configurations & environments". Each costs a name a user may already
  have on a machine, meaning something else.

## L1. Source and tokens

**Proposed.**

1. Source text is UTF-8 (RFC 3629); a byte sequence that is not UTF-8
   is a `parse` error at its offset. Identifiers (variable, parameter
   and record-field names) are ASCII letters, digits and `_`, starting
   with a letter or `_`. A function's name may also hold `-`, and a
   program's name is any word. Strings and paths may hold any character.
2. A newline ends a statement where the innermost open bracket is the
   `{` of a block or closure, or there is none; inside `(`, `[` and a
   record's `{` it is a blank. A line that ends with `|` continues on
   the next, and `else` and `catch` may begin the line after a block's
   `}`. There is no backslash continuation.
3. `#` begins a comment, to the end of the line, where a token could
   begin; inside a word it is an ordinary character.
4. The shell reads in two modes, and a stage's first token decides
   which ([L2](#l2-grammar)). In command mode a word is any run of
   characters but the metacharacters: blank, newline, `|`, `;`, `(`,
   `)`, `[`, `]`, `{`, `}`, `"`, `'` and `$`. So `--format=%h,%s` and
   `a>b` are each one word. Three words are marks, not text: `=>`
   standing alone ([L17](#l17-rights)), `...` touching a variable or a
   `(` ([L4](#l4-words-quoting-and-expansion)), and a leading `^`
   ([L13](#l13-command-resolution)); quoted, each is text. In expression
   mode the operators of L2 are tokens of their own and a bare word is a
   `parse` error; `/` where an operator can stand is the operator, and a
   path literal begins only where an operand can.
5. Literals:

   | Literal | Examples | Value |
   |---|---|---|
   | Integer | `42`, `1_000`, `0x2a`, `0o52`, `0b101010` | `int` |
   | Decimal | `0.1`, `2.5e-3` | `decimal` |
   | Size | `512B`, `4KiB`, `1.5MB` | `size` |
   | Duration | `250ms`, `1.5s`, `2min`, `3h`, `1d` | `duration` |
   | Double-quoted string | `"report ${name}.txt"`, `"tab\tthen $(^date)"` | `string`, with escapes and interpolation |
   | Single-quoted string | `'no $ or \ here, and it''s raw'` | `string`, raw; `''` is one `'` |
   | Bytes, in expressions | `0x[de ad be ef]` | `bytes` |
   | Path | `/users/alice`, `./notes.md`, `../lib`, `~/Documents`, `.` | `path` |
   | Glob | `./*.md`, `~/src/**/*.c` | a list of paths ([L5](#l5-globs)) |
   | Words | `true`, `false`, `nothing` | `bool`, `nothing` |

   A double-quoted string takes the escapes `\\`, `\"`, `\$`, `\n`,
   `\t`, `\r` and `\u{` hex digits `}`, and interpolates `$name`,
   `$name.member`, `${name}` (which ends where the brace does) and `$(`
   pipeline `)`. A path literal starts with
   `/`, `./`, `../` or `~/`, or is `/`, `.`, `..` or `~`; an unquoted
   `*` or `?` in it makes it a glob.

- **Why:** UTF-8 is what names are in the namespace
  ([objects.md O8](objects.md#o8-the-namespace)). Two modes keep both
  shell words and arithmetic natural: Oils makes the case that word
  expressions should be "parsed and evaluated in a straightforward way",
  like `1 + x / 2 + x * 3` (Oils, Simple Word Evaluation, "An Analogy").
  Fewer metacharacters in command mode mean fewer names that need
  quoting. One quoting form that interpolates and one that is raw follow
  fish's verdict, "The many Posix quoting styles are silly" (fish,
  Design, "The law of orthogonality"), and the raw form keeps rc's rule:
  "An apostrophe in a quoted argument must be doubled" (rc, 3).
- **Cost:** the same characters mean different things in the two modes:
  `>` is part of a word in a command and an operator in an expression.
  A bare word in an expression must be quoted. ASCII identifiers exclude
  names in other scripts. A file name holding `*` or `?` is quoted to be
  named exactly.
- **Not taken:** POSIX's four quoting mechanisms, the backslash escape,
  single quotes, double quotes and dollar-single-quotes (POSIX 2.2.1 to
  2.2.4). Their cost is a set of interacting rules users get wrong.
  Elvish's single mode, in which a comparison is a command and a
  condition captures its output, `if (> $n 2)` (Elvish, Unique
  semantics, "Predicates and if"); its cost is arithmetic written as
  commands.

## L2. Grammar

**Proposed.** In this notation `=` defines a rule, `|` separates
alternatives, `[ x ]` is optional, `{ x }` repeats zero or more times,
`( )` groups, terminals are quoted, and `(* *)` is a comment. Names in
capitals are tokens, defined after the syntax.

```
(* Syntax *)

file        = { sep } [ needs_stmt ] script ;  (* a script or module file *)
needs_stmt  = "needs" record sep ;
script      = { sep } [ statement { sep { sep } statement } { sep } ] ;
sep         = NEWLINE | ";" ;
block       = "{" script "}" ;
closure     = "{" "|" [ NAME { "," NAME } ] "|" script "}" ;

statement   = let_stmt | var_stmt | set_stmt | def_stmt | use_stmt
            | if_stmt | for_stmt | while_stmt | try_stmt
            | "break" | "continue" | return_stmt | pipeline ;

let_stmt    = [ "export" ] "let" NAME [ ":" type ] "=" expr ;
var_stmt    = "var" NAME [ ":" type ] "=" expr ;
set_stmt    = "set" NAME { "." member } set_op expr ;
set_op      = "=" | "+=" | "-=" | "++=" ;
def_stmt    = [ "export" ] "def" DEFNAME "(" [ params ] ")"
              [ "->" type ] block ;
use_stmt    = "use" PATH [ "as" NAME ] ;
if_stmt     = "if" expr block { [ NEWLINE ] "else" "if" expr block }
              [ [ NEWLINE ] "else" block ] ;
for_stmt    = "for" NAME "in" expr block ;
while_stmt  = "while" expr block ;
try_stmt    = "try" block [ [ NEWLINE ] "catch" [ NAME ] block ] ;
return_stmt = "return" [ expr ] ;

params      = param { "," param } [ "," ] ;
param       = NAME [ "?" ] ":" type [ "=" expr ]
            | "--" DEFNAME [ "(" "-" LETTER ")" ] [ ":" type ] [ "=" expr ]
            | "..." NAME ":" type ;

type        = "any" | "nothing" | "bool" | "int" | "decimal" | "number"
            | "string" | "bytes" | "duration" | "size" | "time"
            | "closure" | "error" | "glob"
            | "path" [ "(" ( "read" | "write" ) ")" ]
            | "list" [ "<" type ">" ]
            | "stream" "<" type ">"
            | ( "record" | "table" ) [ "<" field_type { "," field_type } ">" ] ;
field_type  = ( NAME | STRING ) ":" type ;

pipeline    = stage { "|" [ NEWLINE ] stage } ;
stage       = command | expr ;                  (* see "Stages" below *)

command     = head { argument } ;
head        = BARE | "^" BARE ;     (* the first BARE: not a keyword, not
                                       wholly a literal, not starting
                                       with "-"; after "^", any BARE *)
argument    = FLAG | "=>" ( word | "..." spread ) | "..." spread
            | word | block | closure ;
spread      = VARIABLE { "." member } | "(" pipeline ")" ;
word        = part { part } ;                   (* parts touch: no blank *)
path_word   = ( PATH | GLOB ) { part } ;
part        = BARE | PATH | GLOB | STRING | interp
            | "(" pipeline ")" | list ;
interp      = VARIABLE { "." member } [ "?" ]
            | "${" NAME { "." member } "}" | "$(" pipeline ")" ;

expr        = or_expr ;
or_expr     = and_expr { "or" and_expr } ;
and_expr    = not_expr { "and" not_expr } ;
not_expr    = "not" not_expr | compare ;
compare     = concat [ cmp_op concat ] ;        (* not associative *)
cmp_op      = "==" | "!=" | "<" | "<=" | ">" | ">=" | "in" | "not-in" ;
concat      = sum { "++" sum } ;
sum         = product { ( "+" | "-" ) product } ;
product     = unary { ( "*" | "/" | "//" | "%" ) unary } ;
unary       = "-" unary | postfix ;
postfix     = primary { "." member [ "?" ] } ;
member      = NAME | INT | STRING ;             (* digits after "." are an INT *)
primary     = literal | VARIABLE | path_word | list | record | table
            | closure | "(" pipeline ")" | "$(" pipeline ")" ;
literal     = INT | DECIMAL | SIZE | DURATION | STRING | BYTES
            | "true" | "false" | "nothing" ;
list        = "[" [ element { "," element } [ "," ] ] "]" ;
element     = [ "..." ] expr ;
record      = "{" [ entry { "," entry } [ "," ] ] "}" ;
entry       = ( NAME | STRING ) ":" expr | "..." expr ;
table       = "[" "[" NAME { "," NAME } "]" ";"
              row { "," row } [ "," ] "]" ;
row         = "[" expr { "," expr } "]" ;

(* Tokens. A token ends where the next character cannot continue it. *)

NEWLINE     = LF | CR LF ;
LETTER      = "a" ... "z" | "A" ... "Z" ;
DIGIT       = "0" ... "9" ;
NAME        = ( LETTER | "_" ) { LETTER | DIGIT | "_" } ;
DEFNAME     = ( LETTER | "_" ) { LETTER | DIGIT | "_" | "-" } ;
VARIABLE    = "$" NAME ;
meta        = " " | TAB | LF | CR | "|" | ";" | "(" | ")" | "[" | "]"
            | "{" | "}" | '"' | "'" | "$" ;
BARE        = char_not_meta_or_hash { char_not_meta } ;
path_start  = "/" | "./" | "../" | "~/" ;
PATH        = path_start { char_not_meta } | "/" | "." | ".." | "~" ;
                                    (* with no unquoted "*" or "?" *)
GLOB        = path_start { char_not_meta } ;
                                    (* with at least one "*" or "?" *)
FLAG        = "--" | "--" DEFNAME [ "=" { char_not_meta } ]
            | "-" LETTER { char_not_meta } ;
dec         = DIGIT { DIGIT | "_" } ;
INT         = dec | "0x" hexd { hexd | "_" } | "0o" octd { octd | "_" }
            | "0b" bind { bind | "_" } ;
DECIMAL     = dec "." dec [ exponent ] | dec exponent ;
exponent    = ( "e" | "E" ) [ "+" | "-" ] dec ;
SIZE        = dec [ "." dec ] ( "B" | "kB" | "MB" | "GB" | "TB"
            | "KiB" | "MiB" | "GiB" | "TiB" ) ;
DURATION    = dec [ "." dec ] ( "ns" | "us" | "ms" | "s" | "min" | "h" | "d" ) ;
STRING      = "'" { char_not_quote | "''" } "'"
            | '"' { char_not_dquote_backslash_dollar | escape | interp } '"' ;
escape      = "\" ( "\" | '"' | "$" | "n" | "t" | "r"
            | "u{" hexd { hexd } "}" ) ;
BYTES       = "0x[" { " " } [ hexd hexd { { " " } hexd hexd } ] { " " } "]" ;
```

**Stages.** A stage is a command when its first token is a bare word
that is not a keyword, is not wholly a literal and does not start with
`-`, or a word starting with `^`; otherwise it is an expression. A
statement keyword (`let`, `var`, `set`, `def`, `export`, `use`, `if`,
`for`, `while`, `try`, `break`, `continue`, `return`, and `needs` at the
top of a file) is tried first. So `ls ./src` and `7z` are commands, and
`42`, `4KiB`, `-$x * 2`, `./src / "x"`, `./*.md`, `$x + 1`, `"text"`
and `[1, 2]` are expressions. A path is always a value: a program or a
script is run by its name or with `run`
([L14](#l14-running-from-the-store)). Inside `( )` the same rule holds,
so `(ls | length)` runs a pipeline and `(1 + 2)` is 3. An expression
stage outputs exactly one value, and after `|` it sees its input as
`$in`. The keywords are those above and `else`, `catch`, `in`, `as`,
`and`, `or`, `not`, `true`, `false` and `nothing`.

**Precedence**, loosest first: `or`; `and`; `not`; comparisons and
`in`, which do not chain (`1 < x < 3` is a `parse` error); `++`; `+`
and `-`; `*`, `/`, `//` and `%`; unary `-`; member access `.`. Binary
operators group from the left.

**Arguments.** A command with a signature ([L9](#l9-functions-and-signatures))
reads each word as its parameter's type: `10` for an `int` parameter is
10, for a `string` parameter it is `"10"`, and for a `path` parameter a
bare word is a path relative to `$cwd`. A command without a signature
receives every word as a string, except that path and glob words are
paths. A glob given for a parameter of type `glob` is passed as its
pattern, unexpanded. `FLAG` tokens are matched against the signature's
flags; a word that starts with `-` and a digit is a number where a
number is expected and a string otherwise. A flag is one word: a path
inside it (`-I./include`, `--out=./x`) is text and is not granted, so a
path meant to be granted is its own word (`-I ./include`).

- **Why:** a grammar fixed before evaluation means data can never become
  code: "parsing and evaluation aren't interleaved, and code and data
  aren't confused" (Oils, Simple Word Evaluation). Deciding a stage's
  mode from its first token is a static rule a reader applies by eye.
  Reading words by the parameter's declared type keeps a version string
  such as `1.10` a string where a string is wanted.
- **Cost:** two grammars inside one; a newline-sensitive grammar with a
  rule for `else`; signature-directed reading means the same word is a
  different value for two commands, though always one the signature
  states; a script is run as `run ./deploy.nvo`, not `./deploy.nvo`.
- **Not taken:** the POSIX grammar (POSIX 2.10), whose words are
  expanded in stages after parsing (POSIX 2.6). Its cost is the class of
  bugs L4 removes. POSIX's rule that a command name containing a slash is
  run as a file (POSIX 2.9.1.4); here it would make `./src / "x"` a
  command, so a path at the head stays a value.

## L3. Values and types

**Proposed.**

| Type | What it is |
|---|---|
| `nothing` | No value: what a command with no output gives |
| `bool` | `true` or `false` |
| `int` | A 64-bit two's-complement integer. A result that does not fit raises `overflow`; nothing wraps |
| `decimal` | A decimal floating-point number of 34 significant digits, rounded half-even. Division by zero, overflow and an invalid operation raise errors; there is no infinity and no NaN |
| `string` | Text: a sequence of Unicode scalar values, held as UTF-8 |
| `bytes` | A sequence of octets in no stated encoding |
| `path` | An absolute, normalized name in the shell's view, with the bound [L16](#l16-designation) gives it |
| `duration` | A signed 64-bit count of nanoseconds, the unit of `clock_monotonic` ([syscalls.md S8](syscalls.md#s8-the-first-calls)); `d` is 86,400 s, and there are no months or years, which have no fixed length |
| `size` | A whole number of bytes (64-bit). `kB`, `MB`, `GB` and `TB` are powers of 1,000, `KiB`, `MiB`, `GiB` and `TiB` powers of 1,024; a fractional literal must come to whole bytes, so `1.5KiB` is 1,536 B and `1.5B` is an error |
| `time` | An instant, nanoseconds since 1970-01-01T00:00:00 UTC: a file's modification time ([storage.md F2](storage.md#f2-attributes)), from the wall-clock service |
| `list` | An ordered sequence of values of any types |
| `record` | Ordered, unique string keys, each with a value |
| `table` | A list of records that share the same keys in the same order; `describe` reports `table<name: string, size: size>` |
| `closure` | Code with the values it captured |
| `error` | A raised failure as a value ([L11](#l11-errors)) |

`number` in a signature accepts `int` or `decimal`; `any` accepts every
type; `stream<T>` says a command outputs zero or more `T`
([L7](#l7-pipelines)); `glob` receives a glob literal's pattern
unexpanded ([L5](#l5-globs)).

Rules:

1. No value converts to another type implicitly, except an `int`
   operand meeting a `decimal`, which is widened exactly, and scalars
   placed in a string by interpolation or in a compound word
   ([L4](#l4-words-quoting-and-expansion)). Conversions are written:
   `into int`, `into path`, `to json`.
2. `==` compares structurally. Values of different types are unequal,
   except `int` and `decimal`, which compare by value. Ordering is
   defined within numbers, strings (bytewise in UTF-8), sizes,
   durations, times and paths (bytewise); across other types it raises
   `type`.
3. A condition must be a `bool`. No other value is true or false.
4. `+` adds numbers, sizes, durations, and a duration to a time; `++`
   concatenates strings, lists and bytes; `/` on two `int`s gives a
   `decimal`, `//` the floor of the quotient as an `int`, and `%` the
   remainder that makes `a == (a // b) * b + a % b`. `/` with a path on
   the left and a string or relative path on the right joins them.
5. A table is a list that meets its condition, not a second kind of
   container: `ls` gives one, and a list of records with differing keys
   is a list.

**Decimal, not binary floating point.** A shell computes with what
people type and read: sizes, durations, money, percentages, version
fractions. Binary floating point shows them approximately; Nushell's
documentation gives `10.2 * 5.1` as `52.019999999999996` and warns that
"decimal values in Nushell are approximate" (Nushell, Types of Data,
"Floats/Decimals"). Decimal arithmetic rests on the principle that
"computers must provide an arithmetic that works in the same way as the
arithmetic that people learn at school" (Decimal, Appendix B, p. 59).
34 digits rounded half-even is the precision and rounding of that
specification's extended default context for its 34-digit format
(Decimal, "Default contexts", p. 16). Arithmetic done in software gives
the same results on x86-64 and ARM64 (DESIGN.md, principle 4) and reads
no processor rounding mode, and CBOR carries it exactly as a decimal
fraction, "most useful if an application needs the exact representation
of a decimal fraction such as 1.1" (CBOR 3.4.4).

- **Why:** each type has a reader. `path` is what the authority model
  works on ([L15](#l15-grants-from-arguments)), so a name meant as a
  file can never be confused with text. `size`, `duration` and `time`
  are the units of the attributes and clock the system reports. Ordered
  records keep the columns a user selected in the order selected.
  Overflow that raises an error, instead of wrapping, keeps arithmetic
  on sizes honest.
- **Cost:** decimal arithmetic is slower than hardware binary floating
  point, which no shell workload notices; a program that sends a binary
  float in values ([L7](#l7-pipelines)) has it converted to the nearest
  34-digit decimal; scientific computation belongs in a language
  embedded for it. Strictness costs explicit conversions that other
  shells do silently.
- **Not taken:** binary64 as the only non-integer type, as Nushell's
  `float` (Nushell, Types of Data). Its cost is approximate results
  shown to people who typed exact ones. PowerShell's implicit
  conversions (PowerShell 6), under which "double argument 4.7 is
  rounded to int 5" when bound to an `[int]` parameter (PowerShell
  8.14). Elvish's truthiness, in which "all exceptions are booleanly
  false" (Elvish, Unique semantics, "Exit Status and Exceptions").

## L4. Words, quoting and expansion

**Proposed.**

1. **Input is scanned once.** A value is never parsed again: what a
   variable, a capture or an interpolation produces stays one value, as
   rc's design has it: "Input is never scanned more than once by the
   lexical and syntactic analysis code" (rc, 28).
2. **A word is one argument.** No field splitting, no empty-argument
   removal, no globbing of values: a variable holding `my pic.jpg`, an
   empty string or `*.py` is one argument, exactly that text (Oils,
   Simple Word Evaluation, "No Implicit Splitting, Dynamic Globbing, or
   Empty Elision").
3. **Compound words** join their parts with no blank between them.
   Strings join into a string, and numbers, sizes, durations and bools
   join as their text. A word whose first part is a path literal, a glob
   or a path value is a path or glob; later parts are added to its last
   component's text, literally (`${f}.bak` is a path), and its bound
   follows [L16](#l16-designation). Only unquoted characters written in
   the source make a glob. A list, record or table inside a compound word
   raises `type`, as Elvish raises "cannot concatenate string and list"
   (Elvish, 6.7).
4. **One argument per value, many only by request.** `...$files` spreads
   a list into one argument per element. A glob literal is the one word
   that becomes several arguments by itself ([L5](#l5-globs)).
5. **`~`** is the session's home, `/users/<name>`, only at the start of
   a path literal and only unquoted. There is no `~user`.
6. **Quoting makes text.** A quoted word is a string even when its text
   starts with `/` or `./`; it designates no file ([L15](#l15-grants-from-arguments)).
   Plash draws the same line: "you must quote arguments to prevent them
   from being interpreted as filenames" (Plash, The Plash shell). Where a
   signature declares a `path` parameter, the literal word written there
   is that path (L2, Arguments).
7. **Names with spaces** are written with a quoted part inside a path
   word, `./"My Documents"/notes.md`, and completion writes that form.
   A path value holding a space is never split.
8. **Interpolation** in double quotes and compound words takes `$name`,
   `$name.member`, `${name}` and `$( pipeline )`. A member must exist:
   `"$f.bak"` with `$f` a path raises `type` with the hint `"${f}.bak"`,
   never a silent empty text. A list, record or table interpolated raises
   `type`, with the hint to convert it with `to json` or `to text`.

- **Why:** these rules remove two hazards: a name with a space split
  into two arguments, and globbing surprises. In POSIX, the
  results of unquoted expansions are split at every byte of `IFS`, by
  default "<space>, <tab>, and <newline>" (POSIX 2.6.5), and then
  matched against file names (POSIX 2.6.6), so correctness depends on
  quoting every expansion. Here correctness needs no quoting at all.
- **Cost:** a list that should become many arguments needs `...`;
  scripts ported from sh need their expansions rewritten; a path cannot
  be written as a quoted string, except where a signature says so.
- **Not taken:** fish's command substitution, which uses "each separate
  line as a separate argument" unless quoted (fish, The fish language,
  "Command substitution"). Its cost is splitting by data, the hazard
  rule 2 removes.

## L5. Globs

**Proposed.**

1. A glob is a path literal with an unquoted `*` (any characters but
   `/`), `?` (one character but `/`) or `**` (any characters, `/`
   included, across directories). There are no bracket classes and no
   brace expansion.
2. Globs exist only in source text. A string or a variable's value is
   never matched, however it was made: Oils calls such globs "dynamic
   when the pattern comes from program data rather than the source
   code" and does not perform them (Oils, Simple Word Evaluation).
3. The shell expands a glob against its own view before the command
   starts, listing directories with `read_dir`
   ([storage.md F1](storage.md#f1-the-directory-protocol)). The program
   never sees the pattern, only the matches, each granted on its own
   with the rights the argument carries ([L17](#l17-rights)).
4. Matches are ordered component by component, each component in
   bytewise order, which is the order directories list in (F1), whatever
   the locale.
5. A glob that matches nothing raises `glob-no-match` before anything
   runs. `glob --allow-empty <pattern>` gives an empty list instead; its
   parameter has type `glob`, so the pattern reaches it unexpanded
   ([L2](#l2-grammar), Arguments).
6. `*`, `?` and `**` do not match a name's leading `.`; a pattern
   component that starts with `.` does. `**` does not descend through a
   symbolic link.
7. In an argument a glob becomes one argument per match, and after `=>`
   each match is granted write; in an expression it is a `list<path>`.

- **Why:** expanding in the shell lets it grant exactly the matched
  files and nothing else. A pattern that matches nothing is an error,
  as fish does ("the command is not executed", fish, The fish language,
  "Wildcards") and as Elvish throws (Elvish, 6.9). Bytewise order makes
  a glob's result the same on every machine holding the same files, as
  storage.md F1 does for listings.
- **Cost:** a program that does its own matching, such as a package
  query, needs its pattern quoted; there is no character class; a glob
  over a large tree grants a handle per match, each an entry in the
  launcher's request ([Changes elsewhere](#changes-elsewhere)).
- **Not taken:** POSIX's rule that a pattern matching nothing "shall be
  left unchanged", and its sorting by "the collating sequence in effect
  in the current locale" (POSIX 2.14.3), which rc keeps for the first
  ("it stands for itself", rc, 4). Their cost is a command that receives
  a literal `*.tmp` and acts on a file of that name, and an order that
  changes with the locale.

## L6. Variables and scope

**Proposed.**

1. `let x = <expr>` binds a name that cannot change. `var x = <expr>`
   binds one that can, and `set x = <expr>` (or `+=`, `-=`, `++=`)
   changes it; `set` on a `let` is a `parse` error.
2. Scope is lexical: every block opens a scope, and a name is visible
   from its binding to the end of its block. A name used before it is
   bound is an `unbound` error found when the script is parsed. At the
   prompt each entered line is a scope nested in the previous lines, so
   a `let` can bind a name again.
3. Values are immutable. `set r.size = 1KiB` on a `var` record replaces
   the variable's value with an updated copy; another variable holding
   the old record still holds it, as Elvish's assignment is copying
   (Elvish, Unique semantics, "Assignment Semantics").
4. A closure captures the values of the names it uses when it is made.
   It cannot use a `var`, as Nushell's closures "cannot capture mutable
   variables" (Nushell, Variables). A block given to a command as an
   argument is a closure with no parameters.
5. Names the shell binds: `$in`, the current value for a block or
   closure a command applies to each value (`where`, `each`), the input
   of a function ([L9](#l9-functions-and-signatures)), and the input of
   an expression stage ([L2](#l2-grammar)); `$cwd`, the current directory,
   a `path` changed only by `cd`; `$home`; `$user`, the session's user
   name; and `$env`, a `var` record of strings that external programs
   receive as their environment. `$env` carries no authority.
6. There are no dynamically scoped variables and no variables shared
   between sessions.

- **Why:** most shell values are computed once and read; making change
  explicit lets a reader trust a `let`. Nushell recommends immutable
  variables "for most use-cases" (Nushell, Variables, "Choosing between
  mutable and immutable variables"). Keywords parsed statically, as
  Oils's are (Oils, Variables, "Keywords Are More Consistent and
  Powerful Than Builtins"), let unbound names be reported before
  anything runs.
- **Cost:** loops that accumulate use a `var` or a fold; closures that
  must change a value return it instead.
- **Not taken:** fish's universal variables, "shared between all the
  user's fish sessions on the computer" (fish, The fish language,
  "Universal Variables"); persistent state belongs in settings
  ([L24](#l24-settings-and-service-commands)), where the graphical
  interface sees it too. POSIX's single global namespace of string
  variables (POSIX 2.5).

## L7. Pipelines

**Decided** (by the owner): the value encoding, rule 9. **Proposed:** the
rest.

1. **A stream is a sequence of values.** Built-ins and functions write
   values. An external program that speaks bytes contributes one `bytes`
   value to its stage's output, delivered as it writes; one whose
   signature says it speaks values contributes the values it sends.
2. **A block's output** is the output of its statements in order, as in
   every shell; a `let`, a `set` or a statement that produces `nothing`
   adds nothing. A function declared `-> T` must output exactly one `T`,
   or raises `type` naming the statement that broke it.
3. **Into a stage** (`a | b`):

   | `a` writes | `b` is | `b` receives |
   |---|---|---|
   | Values | A built-in or function | The values |
   | Values | A program that speaks values | The values, in the value encoding (rule 9) |
   | `bytes` values | A program that speaks bytes | The bytes as they are |
   | `string` values | A program that speaks bytes | Each string's UTF-8, then LF |
   | Any other value | A program that speaks bytes | Nothing: `type` is raised, naming both stages and suggesting `to text`, `to json` or `to csv` |
   | Bytes | A built-in that reads bytes (`lines`, `from`, `into string`, `save`) | The `bytes` value |
   | Bytes | Any other built-in | Nothing: `type` is raised, suggesting `lines` or `from` |

   Two adjacent programs that both speak bytes are joined by one channel
   the shell creates, so their bytes never pass through the shell, as
   POSIX connects "the standard output of the command to the standard
   input of the next command" (POSIX 2.9.2).

   A stream carries data, not authority. Every stream channel the shell
   creates is one that refuses handles
   ([Changes elsewhere](#changes-elsewhere)), so a program cannot pass a
   handle to the program it is piped to, or to the shell. It can still
   act for that program, reading what it holds and writing the bytes,
   for as long as it runs; the pipe the user wrote is that path. A path
   in a value stream is a name: it is granted only when it reaches a
   program as an argument, within its bound ([L16](#l16-designation)).
4. **Concurrency.** External stages are processes and run concurrently.
   Built-in and function stages run in the shell process, on one thread,
   taking turns at channel boundaries. A full channel holds its writer
   back ([ipc.md I1](ipc.md#i1-channels)).
5. **Standard error** of an external goes to the shell's standard error,
   the console at the prompt, never into the pipe. `complete` captures it
   ([L12](#l12-exit-status-and-the-outcome-of-a-pipeline)).
6. **Capture.** `( pipeline )` gives a `list` of the values written,
   never unwrapped; if the last stage declares a single value (`-> T`,
   not `stream<T>`), it gives that value. A program that speaks bytes
   declares one `bytes` value, so `(^git rev-parse HEAD)` is that
   value; a predicate, a program whose signature maps its statuses to
   `true` and `false` ([L12](#l12-exit-status-and-the-outcome-of-a-pipeline)),
   declares one `bool`, and the bytes it writes go to the shell's
   standard output. `$( pipeline )` gives a `string`: bytes decoded as
   UTF-8, raising `type` if they are not, and strings joined by LF, with
   every trailing LF removed, as POSIX removes trailing newlines from a
   command substitution (POSIX 2.6.3).
7. **At the prompt**, the values a statement outputs are rendered by the
   shell (a table as columns, a record as two), and bytes are written as
   they come. In a session in values mode, which login gives the shell
   it starts for the HUD ([ui.md U11](ui.md#u11-the-hud)), each line
   comes as a string and a statement's values go back unrendered, in the
   value encoding (rule 9), for the HUD to draw.
8. **`print`** writes its argument to the shell's own standard output
   at once, outside the pipeline, so a function can report progress
   without adding to its output.
9. **The value encoding** between processes is CBOR with the core
   deterministic encoding (CBOR 4.2.1). A `decimal` is CBOR's decimal
   fraction, tag 4 (CBOR 3.4.4); `string` and `bytes` are CBOR's text and
   byte strings, which keep text and "unstructured bytes" apart (CBOR
   3.1, major types 2 and 3); `path`, `size`, `duration`, `time`, `error`
   and records take nova-os tags. A record is a tagged array of keys and
   values, since the deterministic encoding sorts a map's keys (CBOR
   4.2.1) and a record's order is its own. A program that speaks only
   text exchanges JSON with `to json` and `from json`
   ([L25](#l25-built-ins)). The stream's framing in channel messages of
   up to 65,536 bytes is the value protocol's to specify
   ([Changes elsewhere](#changes-elsewhere)).

- **Why:** records and tables make a pipeline's data addressable by
  name, which PowerShell built its pipeline on ("Instead of processing
  text, the shell processes objects", PowerShell 1) and Nushell made
  tabular. Andy Chu separates interior structures, "within a process",
  from exterior ones, "outside or between processes, like Unix files",
  and finds most structured shells interior-first (Chu 2023): their
  objects exist only inside the shell. Values here cross process
  boundaries in one exterior encoding, so a program in any language can
  read and write them, while built-ins pass them without encoding.
  CBOR keeps integers, decimals, text and bytes apart and exact, and the
  core deterministic encoding (CBOR 4.2.1) gives a value one encoding.
  Conversions are explicit because automatic ones depend on data:
  Nushell renders a table with box-drawing characters before it reaches
  an external program, so the line `grep` selected reached `ls` with the
  table's borders in it ("What's wrong?",
  Nushell, Pipelines, "Output Result to External Commands"), and it
  decodes an external's bytes as UTF-8 "if successful" and passes binary
  otherwise (Nushell, Pipelines, "Working with External Commands"). One
  thread for built-ins keeps the shell's own execution deterministic for
  replay ([L23](#l23-replay)). An unwrapped capture is a trap PowerShell
  documents: "If the statement writes a single scalar value, that is the
  value of the statement" and otherwise an array (PowerShell 8.1.2).
- **Cost:** a program exchanges values only if it speaks CBOR and its
  signature says so; text from programs that do not is parsed with
  `lines` or `from`; a string stream into a byte program is the one
  conversion done for the user; built-in stages never use two cores;
  pipes need a channel that refuses handles.
- **Not taken:** PowerShell's .NET objects, "accepts and returns .NET
  Framework objects" (PowerShell 1). Their cost is a pipeline that only
  programs in one runtime can join. Oils's text data languages as the
  wire format, "JSON-based data languages on the wire" (Chu 2023). Their
  cost is printing and parsing every number and escaping every byte
  string, where CBOR keeps types and bytes as they are. Elvish's two outputs
  per command, a byte part and a value part (Elvish, Unique semantics,
  "Passing Data Structures Through the Pipeline"); a byte output here is
  one value in the one stream.

## L8. Control flow

**Proposed.**

1. `if <bool> { } else if <bool> { } else { }`, `for x in <list or
   stream> { }`, `while <bool> { }`, with `break` and `continue`, and
   `return` in a function.
2. `try { } catch e { }` runs the first block and, if it raises an
   error, binds the error to `e` and runs the second. Without `catch`,
   the error is dropped and the statement outputs `nothing`.
3. `ok { block }` is `false` if the block raises `exit` with a status in
   `--false` (default `[1]`) or its last statement is a predicate that
   gave `false`, and `true` if it finishes otherwise; every other error
   is raised. A predicate, a program whose signature maps its statuses
   to `true` and `false`
   ([L12](#l12-exit-status-and-the-outcome-of-a-pipeline)), can be a
   condition directly: `if (^diff -q ./a ./b) { }` for a `diff` that
   maps 0 to `true` and 1 to `false`.
4. There is no `&&` or `||`: a failure already stops the statements
   after it, and a choice between commands is an `if` or a `try`.
5. There are no background jobs in the first version.

- **Why:** a condition is a boolean, not an exit status, so an `if`
  cannot confuse "false" with "failed". Oils names the hazard: with
  `grep`, a syntax error's status 2 and no match's status 1 both take
  the `else` branch, "the Error or False Pitfall" (Oils, Error Handling,
  "The Meaning of if"). `ok` takes the boolean convention, "0 for true,
  1 for false, or a different number like 2 for an error" (Oils, Error
  Handling, "What Does $? Mean?"), and lets status 2 raise.
- **Cost:** `if ^grep ...` from sh becomes `if (ok { ^grep ... })`;
  background work waits for a later version.
- **Not taken:** Elvish's `?()`, which "will prevent any kind of
  exceptions from throwing", so `diff`'s genuine error status 2 is
  swallowed too (Elvish, Unique semantics, "Exit Status and
  Exceptions").

## L9. Functions and signatures

**Proposed.**

```
# Copy a tree into a dated backup.
export def backup(
    src: path,               # the tree to copy
    dest: path(write),       # where the backup directory is made
    --dry-run,               # list what would be copied
) {
    let stamp = $(date --format "%Y-%m-%d")
    if $dry_run { ls $src | get path; return }
    ^cp -r $src => ($dest / $stamp)
}
```

1. A parameter is positional (`name: type`), optional (`name?: type`,
   which is `nothing` when absent), defaulted (`name: type = expr`), a
   flag (`--name: type`, or a switch `--name` of type `bool`; `--dry-run`
   binds `$dry_run`; `--verbose(-v)` adds a short form), or a rest
   parameter (`...names: type`).
2. `path(write)` declares that the function or program writes the path
   given there ([L17](#l17-rights)); `path` alone is `path(read)`.
3. The comment above a `def` is its description, and a comment after a
   parameter describes it; `help` shows both and completion prints the
   first line beside each name.
4. `$in` is the function's input stream. Used as a value it is collected
   into a list.
5. A function runs in the shell with the shell's authority
   ([L19](#l19-scripts)).
6. A function may not take a built-in's name; defining one is a `parse`
   error.

**Signatures of programs.** A program's derivation may declare a
signature in the same terms: its parameters and flags with types and
access, whether its standard input and output speak bytes or values, how
its exit statuses map ([L12](#l12-exit-status-and-the-outcome-of-a-pipeline)),
and a description of each part. The shell reads it before the program
runs ([L20](#l20-the-recipe-and-the-command-line)), so arguments are
checked, completed and highlighted as a built-in's are. A program with
no signature takes strings and paths, speaks bytes, and succeeds with
status 0 alone.

- **Why:** PowerShell's commands each have "a consistent interface"
  with typed, named parameters (PowerShell 1, 8.14), which is what makes
  them discoverable, and fish asks that "every tab completion should
  have a description" (fish, Design, "The law of discoverability"). A
  wrong argument is reported at its word, before any process starts.
  Arguments are checked, never converted: a decimal given for an `int`
  parameter is a `type` error.
- **Cost:** a packaged program's recipe carries its signature, which can
  be wrong; a program's own argument parser and its signature must
  agree.
- **Not taken:** PowerShell's Verb-Noun names, "a verb and noun
  separated by a dash (-), such as Get-Help" (PowerShell 13). Their cost
  is typing: consistency here comes from parameters and help, and names
  stay short, as Unix's are.

## L10. Modules

**Proposed.**

1. `use ./lib/net.nvo` binds the module to `net`, its base name;
   `use ./lib/net.nvo as n` binds it to `n`. A path is resolved against
   the importing file's directory, or is absolute, `/store` included.
2. A module's top level holds only `def`, `let` and `use`, and a
   top-level `let` is an expression with no command in it: a capture
   there is a `parse` error. Exported functions are called as
   `net.fetch`, and exported `let`s read as `$net.timeout`.
3. Importing runs no command, and a cycle of imports is a `parse` error.
   `use` takes a literal path, so the files a script imports are known
   before it runs ([L19](#l19-scripts)).
4. Imported code runs in the importing shell with its authority. Code
   that should not have that authority runs as a script
   ([L19](#l19-scripts)).

- **Why:** an import that runs nothing can be read for its definitions
  alone, and the order of imports cannot matter.
- **Cost:** a module has no initialization step; a value computed when
  first needed is computed by a function.
- **Not taken:** Elvish's modules, whose top-level code runs on `use`
  (its example module prints `mod a loading`, Elvish, 12.3.3). Its cost
  is effects at import time.

## L11. Errors

**Proposed.**

An error is a value with these fields:

| Field | Holds |
|---|---|
| `kind` | One of the kinds below |
| `msg` | One line saying what went wrong |
| `span` | Where in the source: `file`, `line`, `column`, `length` |
| `label` | Text shown under the span |
| `help` | How to fix it, where the shell can tell |
| `command`, `stage` | The command and its position in its pipeline, from 1 |
| `status` | For `exit`, the exit code |
| `code` | For `io`, the status code ([syscalls.md S7](syscalls.md#s7-error-reporting)) |
| `cause` | The error that led to this one, or `nothing` |
| `errors` | For `pipeline`, each stage's error |

| Kind | Raised when |
|---|---|
| `parse` | Source that does not match the grammar or breaks a static rule |
| `type` | A value of the wrong type: an operand, an argument, a declared type, a stream that cannot meet its consumer |
| `unbound` | A name that is not defined |
| `not-found` | `ERR_NOT_FOUND`, or a command name that resolves nowhere |
| `exists` | `ERR_ALREADY_EXISTS` |
| `access-denied` | `ERR_ACCESS_DENIED`, or a grant outside its bound or the shell's view ([L16](#l16-designation)) |
| `denied` | The user refused a request ([L18](#l18-asking-for-more-the-powerbox)) |
| `integrity` | `ERR_DATA_INTEGRITY` |
| `io` | Any other status of S7, with `code` |
| `glob-no-match`, `overflow`, `division-by-zero` | As named |
| `exit` | A program ended with a status its signature does not count as success |
| `fault` | A program ended on a fault no handler resolved ([objects.md O10](objects.md#o10-object-lifetime), rule 3) |
| `interrupted` | The user interrupted the statement |
| `launch` | The launcher could not start a program, a declared grant the shell cannot give included ([storage.md F5](storage.md#f5-per-process-views), rule 3) |
| `pipeline` | More than one stage of a pipeline failed |
| `custom` | `fail` raised it |

1. A failing command raises its error, which ends the statement and
   every enclosing block up to the nearest `try`.
2. Uncaught at the prompt, it is printed and the prompt returns.
   Uncaught in a script, it is printed to the script's standard error
   and the script ends with exit code 1.
3. `fail "message"` raises `custom`; `--kind`, `--label` and `--help`
   set the other fields, and `--span-of $x` points at where `$x` was
   written.
4. The shell prints an error with its source line and a mark under the
   span:

   ```
   error[not-found]: no such file
     --> line 1, column 5
      |
    1 | cat ./notse.md
      |     ^^^^^^^^^^ /users/alice/notse.md is not there
      = help: ./notes.md exists
   ```

- **Why:** a typed error can be caught by kind and read by field, where
  an exit status is a number each program assigns its own meaning. An
  error that points at the word that caused it is Nushell's: "the label
  of error message, which includes both the text of the label and the
  span to underline" (Nushell, Creating Your Own Errors). Elvish shows
  that turning failure into an exception that stops execution makes a
  script safe by default (Elvish, Unique semantics, "Exit Status and
  Exceptions").
- **Cost:** a script that expects a command to fail says so with `try`
  or `ok`; the kinds are a closed set, extended only by `custom`.
- **Not taken:** POSIX's `$?` with `set -e` as an option, subject to
  what Oils calls the "Disabled errexit Quirk ... when you use a shell
  function in a conditional context, errors are unexpectedly ignored"
  (Oils, Error Handling). PowerShell's two handlers, `try` (PowerShell
  8.7) and `trap` (PowerShell 8.8); one handler is simpler to reason
  about.

## L12. Exit status and the outcome of a pipeline

**Proposed.**

1. A program's exit code is the 32-bit value `process_info` reports
   ([syscalls.md S8](syscalls.md#s8-the-first-calls)). Without a
   signature, 0 is success and any other code raises `exit` with
   `status` set. A signature maps codes to `ok`, an error kind, or, for
   a predicate, `true` and `false`, which become the command's value
   ([L7](#l7-pipelines), rule 6): a `grep` port maps 0 to `true` and 1
   to `false`, and 2 stays an error.
2. A program that ended on a fault raises `fault`; one the shell
   stopped because the user interrupted raises `interrupted`. Telling
   these from an exit needs `process_info` to report how a process ended
   ([Changes elsewhere](#changes-elsewhere)).
3. Every stage of a pipeline runs to its end. If one failed, its error
   is raised with `stage` set; if several did, `pipeline` is raised with
   all of them in `errors`, as Elvish throws "a 'composite exception',
   containing information all exceptions involved" (Elvish, 9.1).
4. A stage that ends after the stage reading its output has already
   ended, and fails, has been stopped, not failed: its failure is not
   raised. A program writing to a channel whose reader is gone gets
   `ERR_PEER_CLOSED` ([ipc.md I1](ipc.md#i1-channels)), so
   `^yes | lines | first 1` succeeds.
5. `complete { block }` runs the block and gives a record of its
   standard output (`bytes`), its standard error (`bytes`) and its
   `status`, raising nothing for a status; it raises for anything else,
   such as `not-found`.
6. `exit <int>` ends a script or the shell with that code.

- **Why:** no failure in a pipeline is lost: YSH "never silently ignores
  an error, and it never loses an exit code" (Oils, Error Handling).
  Rule 4 is Elvish's: an exception from a command that "tried to write
  output when the next command has terminated ... is suppressed" (Elvish,
  9.1); Oils turns on `sigpipe_status_ok` for the same case, `yes | head`
  (Oils, Error Handling). Mapping statuses in the signature, once per
  program, is the "more sophisticated system of handling exit status"
  Elvish says it still lacks (Elvish, Unique semantics).
- **Cost:** a real failure of a stage after its reader ended is hidden
  (it stays in the session's recording); a pipeline waits for every
  stage before reporting.
- **Not taken:** POSIX's pipeline status, "the exit status of the last
  (rightmost) command" unless `pipefail` is set (POSIX 2.9.2), and its
  encodings of 127 for a command not found, 126 for one not executable
  and above 128 for a signal (POSIX 2.8.2). Their cost is failures lost
  by default and causes folded into numbers. rc's `$status` joins every
  component's status with `|` (rc, 11), which keeps them but as text to
  parse.

## L13. Command resolution

**Proposed.** A bare command name resolves, first match wins:

1. a keyword, which is never a command;
2. a built-in ([L25](#l25-built-ins));
3. a function in lexical scope, or a module's exported function;
4. the environments of the enclosing `with` blocks, innermost first
   ([L14](#l14-running-from-the-store));
5. the user's profile, `/users/<name>/profile/bin`
   ([storage.md F13](storage.md#f13-profiles-system-and-garbage-collection));
6. the running system's `bin`, under `/system`.

`^name` skips the first three and finds an external program, as
Nushell's caret "calls the external command found in the user's PATH"
instead of a built-in of the same name (Nushell, Running System
Commands). A path is never a command name: `run` runs a store path or a
script ([L14](#l14-running-from-the-store)). Where a name in a profile
is a symbolic link into `/store`, as the entries of a Nix user
environment are, the shell follows it on the client side, as every link
is followed ([storage.md F3](storage.md#f3-symbolic-links)), and hands
the launcher the store path it reaches, so the program's closure and
declarations are its own derivation's, not the profile's. Following it
needs the profile's closure in the shell's view
([Changes elsewhere](#changes-elsewhere)). Resolution happens at every
run, so a profile switch takes effect at the next command. `which name`
reports where a name resolves and, for a program, its store path and
signature.

- **Why:** no list of directories held in a string decides what runs,
  and the current directory is never searched, so a file planted in a
  directory the user visits never runs by its name. Built-ins before
  functions means a module cannot replace `ls` in its importer.
- **Cost:** wrapping a built-in needs a new name; adding a directory to
  the search order means a profile switch or a `with` block.
- **Not taken:** POSIX's `PATH`, a variable "used to effect command
  interpretation" (POSIX 2.5.3), through which "the command shall be
  searched for" (POSIX 2.9.1.4). Its cost is a search order that any
  script can change for the commands after it.

## L14. Running from the store

**Decided** (by the owner): rule 3, that a program file outside `/store`
is refused in the first version. **Proposed:** the rest.

1. **By store path.** `run /store/<hash>-hello-2.12.1/bin/hello
   <arguments>` runs that program without installing it. The shell names
   the path to the launcher, which opens the path's closure directory and
   starts it ([storage.md F5](storage.md#f5-per-process-views)); the
   shell needs no access to the path itself. A path that is not valid
   raises `not-found`; where the session holds a channel to the
   substituter, `run --substitute` asks it to fetch the path first
   ([storage.md F12](storage.md#f12-builds-and-substitution)).
2. **From a recipe.** `with <recipe> { block }` has nova-nix evaluate the
   recipe, build or substitute its outputs through the store service
   (F12), and run the block with those outputs' `bin` directories first
   in resolution (L13, rule 4). The environment lasts as long as the
   block, and nothing is installed. The recipe file and its directory
   are granted read-only to the evaluator. The shell holds the outputs'
   closure directories, opened through the launcher, until the block
   ends, so the outputs stay live
   ([storage.md F13](storage.md#f13-profiles-system-and-garbage-collection),
   roots, rule 3).
3. **Only store paths run as programs.** `run` takes a file whose name
   ends in the script extension as a script, from anywhere, since the
   program that runs it is the shell from its own store path
   ([L19](#l19-scripts)); any other file must be in `/store`. A program
   file outside `/store` is refused in the first version with `launch`.

- **Why:** Nix's commands do the same on Unix: `nix run` "builds and
  runs installable", running `<out>/bin/<name>` of a derivation (Nix,
  `nix run`); `nix env shell` will "run a shell in which the specified
  packages are available" (Nix, `nix env shell`); and `nix-shell` builds
  a derivation's dependencies to start "an interactive shell" for
  "reproducing the environment of a derivation for development" (Nix,
  `nix-shell`). On nova-os a store path carries its authority with it,
  its closure and its derivation's declarations
  ([L20](#l20-the-recipe-and-the-command-line)), so running one is safe
  without installing it. A recipe that pins its inputs gives the same
  environment on every machine (DESIGN.md, principle 1). A file built by
  hand has no derivation, so principle 7 gives it no declared authority;
  the question returns when nova-os builds itself and an edit, compile
  and run loop needs it (DESIGN.md, Stages, 5).
- **Cost:** a program built by hand runs only after a build puts it in
  the store; a recipe's evaluation and build can take long before the
  block starts.
- **Not taken:** installing before running, as most package managers
  require. Its cost is a profile changed to try a program once. Running
  a file outside `/store` with its arguments and streams alone, no
  closure and no declarations. Its cost is a program that cannot load
  what it was built against, and a way to run code no derivation
  describes.

## L15. Grants from arguments

**Proposed.** For each path that reaches an external program as one
argument (a path literal, a glob's match, a path-typed variable or
field, a spread list's element), the shell:

1. normalizes it against `$cwd`, resolving `.` and `..` textually, as
   [objects.md O8](objects.md#o8-the-namespace) rule 2 resolves `..`;
2. resolves it in its own view, following symbolic links on the client
   side ([storage.md F3](storage.md#f3-symbolic-links)); where a link
   changed the target, highlighting and `grants` show the target
   ([L21](#l21-interactive-behaviour));
3. checks the path's bound ([L16](#l16-designation)) against the path as
   written and against the node it reached, so a link cannot lead a
   grant outside the bound;
4. opens the node with the rights the argument carries
   ([L17](#l17-rights)), which can only narrow the shell's own
   ([storage.md F5](storage.md#f5-per-process-views), rule 2);
5. adds to the program's view an entry at the same absolute path,
   mapping it to that handle (F5).

A path that lies above the shell's view entries, such as `/` or `/users`
when the shell holds `/users/alice`, names no single directory the shell
can open, and raises `access-denied` naming the entries below it. A path
inside a list or record passed whole is data, and a program without a
signature that is given a list or record raises `type`.

The program's argument is the absolute path; its working directory is
`$cwd` as text, against which it resolves relative names textually, with
no handle behind it unless it is granted. A file argument becomes a view
entry for that file alone, and a directory argument an entry for that
directory and everything below it. A file granted with write inside a
directory granted read-only is writable, since the longest matching
prefix wins (F5). Grants end with the program, whose handles close when
it ends ([objects.md O10](objects.md#o10-object-lifetime)), except
what it passes on through a channel it holds to a service its recipe
names: a holder of a directory can always open a new channel through it
and send that, so what a program can hand on is bounded by what it
holds and whom it can reach, never by the rights on one handle.

The program's view is then exactly: its closure at `/store`, what its
recipe declares ([L20](#l20-the-recipe-and-the-command-line)), these
grants, and its standard streams. No home directory, no `/data`, no
settings and no network reach it unless one of these names them.

- **Why:** this is Plash's design on a system built for it. In Plash
  "Arguments that were implicit before must now be made explicit":
  `ls .` is needed "to grant access to the current directory" (Plash,
  The Plash shell). The user's act of naming a file is the grant, so a
  program that is attacked through its input can reach only what the
  user named. Capsicum shows the confinement such a program needs, a
  mode in which processes "are denied access to global namespaces"
  and lookups stay "under" the directory handle given (Capsicum, 2.1,
  p. 2); nova-os has no global namespace to deny
  ([objects.md O6](objects.md#o6-no-ambient-authority)). Capsicum's
  authors name Seaborn's shell as a way to confine programs "most
  frequently used in pipelines", in which "pipeline components are
  placed in sandboxes by the shell", with "near-zero application
  modification" (Capsicum, 4.3, p. 10).
- **Cost:** a program that opens files it was not given fails: a `make`
  whose recipe declares no working directory, run with no `.` granted,
  cannot read its `Makefile`. Each grant is an open through a server
  before the program starts. A grant names a node, so a program that
  expects to find siblings of a file it was given does not; and granting
  one file needs view entries that name a single file
  ([Changes elsewhere](#changes-elsewhere)).
- **Not taken:** giving every program the user's whole view, which is
  what every Unix shell does. Its cost is ambient authority: "because it
  acts with the full power of the user", a program processing untrusted
  data "has access to all his or her resources" (Capsicum, 1, p. 1).

## L16. Designation

**Decided** (by the owner): the bound of `into path` is `$cwd` when
`--within` is not given. **Proposed:** the rest.

Every path value carries a bound: the subtree within
which the shell will grant it. Granting a path outside its bound raises
`access-denied`, naming where the path came from.

| The path came from | Its bound |
|---|---|
| A path or glob literal in source the user wrote or ran | The shell's view |
| A glob's match | The glob's literal directory part |
| `ls` or `glob` | The directory listed |
| A program's value output | What that program was granted, without its closure |
| `into path` on a string | `$cwd`, or the path given with `--within` |
| A path `p` extended only by text written in the source (`$p / "a.txt"`, `${p}.bak`) | `p`'s bound |
| A path `p` extended by any text not written in the source (`~/$name`, `$p / $in`) | `p` itself, within `p`'s bound |
| A field of a record, an element of a list | The path's own bound |

The bound is checked after `.` and `..` are resolved, so text from data
cannot climb out of it: `~/$name` with `$name` holding `../bob/notes`
names `/users/bob/notes`, outside `~`, and is refused.

- **Why:** the shell holds the user's authority and acts on what it is
  told, so a path that a program produced must not steer the shell into
  granting what the program could not reach itself, or the program has
  laundered the user's authority through the shell. A program can point
  only within what it was given. A path the user typed or a pattern the
  user wrote is the user's designation, as in Plash; where the user joins
  a path to text from data, the part the user wrote is the designation,
  and the data chooses only below it.
- **Cost:** a bound per path value; a script that builds paths from
  text names a `--within`; a grant refused for its bound needs the path
  written out or granted again. `~/$name` lets the data choose anything
  under `~`, because that is what was written.
- **Why `$cwd` for `into path`:** it covers converting names read from a
  file in the directory being worked in, and fails clearly outside it.
- **Not taken:** any path value grants. Its cost is a pipeline such as
  `^find-big ./src | each { ^upload $in }` handing `upload` whatever
  `find-big` names, `~/.ssh` included. `--within` required on every
  `into path`; its cost is a flag written out for the common case of
  names in the directory being worked in.

## L17. Rights

**Decided** (by the owner): rule 2, that a signature's `path(write)`
grants write where the user names the path, and that highlighting shows
the write before Enter. **Proposed:** the rest.

| Written | A file gets | A directory gets |
|---|---|---|
| A path | `READ` | `READ` and `LIST`, below it too |
| `=> path` | `READ` and `WRITE`; the name may not exist yet | `READ`, `LIST` and `WRITE`: create, remove and rename within it |
| A path given for a `path(write)` parameter | As `=>` | As `=>` |

1. `=>` grants write to the one word after it: `^cp ./a.txt => ./b.txt`;
   before a glob or a spread, to each match or element. A name that does
   not exist yet is granted as a slot in its directory,
   through which the program can create that one name, as a file or a
   directory, and then use it, everything below it included, with the
   rights granted; it can touch nothing else in that directory.
2. A program's signature can declare a parameter `path(write)`, and the
   path given there is then granted write without `=>`; highlighting
   shows it as a write before Enter ([L21](#l21-interactive-behaviour)).
3. `grant <paths> { block }` attaches paths to the programs the block
   runs without putting them in their arguments; `=>` marks write there
   too: `grant => . { ^make }`.
4. No argument grants `EXECUTE`, publication, or anything but files and
   directories. Services and devices come only from the recipe
   ([L20](#l20-the-recipe-and-the-command-line)).
5. A program can ask for more while it runs ([L18](#l18-asking-for-more-the-powerbox)).

- **Why:** Plash's rules, made narrower. In Plash, "Files are passed to
  the program as read-only by default", "Files that appear to the right
  of '=>' are passed to the program with write access", objects there
  are "passed as read-write slots, so the object doesn't have to exist
  in advance", and directories to its left are "recursive (or
  transitive) read-only"; `+` attaches files "to the namespace of the
  process being run, but the filename is not included in the argument
  list" (Plash, The Plash shell). Here `=>` binds one word, so no
  bracketing is needed to limit it, and `grant` takes the place of `+`.
  A signature's `path(write)` grants write because naming a destination
  to `cp` is the user's designation, highlighting shows the write before
  Enter, and a signature lives in the derivation, where an audit reads
  it.
- **Cost:** a program with no signature that writes files needs `=>`
  written; a program that writes through a temporary name and renames it
  over the target needs its directory granted write; a wrong signature
  grants write where its program should only read, which only its
  highlighting shows.
- **Not taken:** Plash's `=>`, which applies to every argument after it
  and is scoped with `{ }` (Plash, The Plash shell); here `{ }` is a
  block. Plash's `!!`, which runs a command "in the conventional way,
  without running the process with a virtualised filesystem" (Plash,
  The Plash shell); here no command runs with the user's whole view.
  Every write marked with `=>` on the command line, signatures or not.
  Its cost is `=>` written for every program whose signature already
  says it writes there, and a `cp` that reads its destination when it is
  forgotten.

## L18. Asking for more: the powerbox

**Proposed.**

1. A program whose recipe declares it receives a powerbox channel to the
   shell. On it the program asks to open a file, to save a file, or to
   pick a directory, with read or write, and a reason of at most 200
   characters. It cannot propose a path. A program with windows has the
   channel to the session's powerbox program instead, which answers the
   same protocol in a window and takes on it, too, a request to share
   the screen or a window ([display.md
   D16](display.md#d16-trusted-interface-and-the-powerbox)).
2. The shell stops forwarding the program's output and shows, on its own
   console session, the program's name taken from its store path, the
   rights asked for, the reason marked as the program's words, and a path
   field that completes against the shell's view, starting at `$cwd`.
   Enter grants; Escape refuses, and the program gets `denied`. While the
   prompt is shown the console gives the keyboard to the shell's read
   before any other session's, and draws the prompt over any session a
   program holds ([Changes elsewhere](#changes-elsewhere)).
3. The shell grants as [L15](#l15-grants-from-arguments) does, bounded
   by its own view, and replies with the handle and the absolute path,
   which the program's native layer adds to its view. A handle a program
   sends the shell on this channel is closed unread.
4. Outside an interactive session every request is refused at once.
5. `grants` lists, for each running program, what it holds from its
   arguments and from requests.

- **Why:** Plash's powerbox, a file chooser that, "as well as telling
  the application which file to access, ... dynamically grants the
  application the right to access the file" (Plash, Powerboxes). It
  "must be implemented as a separate, trusted component", with "a
  trusted path to the user, so only the user can enter a filename into
  it"; and "The application cannot specify a default pathname for the
  powerbox to open", so a program attacked through its input cannot lead
  the user to a sensitive file. The shell already forwards to the
  console the output of every program it runs that holds no console
  session of its own, and grants from its own view, so it is that
  component. Capsicum's authors
  found capability delegation "most valuable" in windowing systems,
  driven "with UI behaviors, such as Powerboxes (file dialogues running
  with ambient authority)" (Capsicum, 7, p. 15).
- **Cost:** the prompt needs a style a program's output cannot imitate,
  and keys and drawing the console gives the shell first
  ([Changes elsewhere](#changes-elsewhere)). A grant cannot be taken back
  while the program runs: the kernel revokes nothing, and access that
  must be revocable "is given through a server that can cut it off"
  ([objects.md O10](objects.md#o10-object-lifetime)), which the file
  servers do not offer the shell. Plash had "no way to revoke this
  authority" either (Plash, Powerboxes).
- **Not taken:** a powerbox channel for every program. Its cost is that
  any program can raise prompts until a user grants one to be rid of
  them.

## L19. Scripts

**Proposed.** A script is a `.nvo` file run as a program; `main` gives
its parameters.

```
# Back up a directory under a dated name.
needs {
    services: ["clock"],
}

def main(src: path, dest: path(write)) {
    ^cp -r $src => ($dest / $(date --format "%Y-%m-%d"))
}
```

1. `run ./backup.nvo ~/docs ~/backups` starts a new shell process from
   the shell's own store path. It is given: the script file and every
   module the script imports, found from its literal `use` paths
   ([L10](#l10-modules)), read-only; the grants its arguments make under
   `main`'s signature; its standard streams; `$cwd` as text; a launcher
   channel, through which the programs it runs get of the user's files
   only what the script itself holds, and of services only what its
   `needs` declares; the user's profile and the running system,
   read-only, so names resolve as they do at the prompt
   ([L13](#l13-command-resolution)); and what its `needs` block
   declares, its paths from the shell's view and its services and
   network from the launcher, as a program's are
   ([L20](#l20-the-recipe-and-the-command-line), rule 2).
2. It inherits nothing else: not the caller's variables, functions,
   history, settings, home or `$env` beyond what the call passes, and
   nothing the shell's own derivation declares.
3. `needs` is a record whose keys are `read` and `write`, each a list
   of paths; `services` and `devices`, each a list of names; and
   `network`, a list of rules as
   [network.md N12](network.md#n12-grants) writes them. The wall clock
   is the service `date` reads, so a script that runs `date` needs it,
   and `settings` is the service
   [L24](#l24-settings-and-service-commands) reads. At the prompt the
   shell shows them and asks once per SHA-256 of the file, so an edited
   script asks again, and keeps the answer as a setting; elsewhere an
   unapproved `needs` fails the launch
   ([L20](#l20-the-recipe-and-the-command-line), rule 3). A script
   outside `/store` has no derivation for the launcher to read, so the
   shell sends the approved `needs` with the run request, and the
   launcher gives no more of any service than the session holds.
4. A script in `/store` takes its `needs` from its derivation
   ([L20](#l20-the-recipe-and-the-command-line)); a `needs` block that
   asks for more than the derivation declares fails the launch.
5. A script's standard output is bytes unless `main` declares a value
   output.

- **Why:** a script is code the user may not have written, so it starts
  with what a program starts with. Plash runs scripts in the shell's
  own authority ("The source file command will run file as a script",
  with "no error handling", Plash, The Plash shell); here a script is
  confined as a program is.
- **Cost:** a script that needs the caller's functions imports them
  from a module; a script that touches files beyond its arguments says
  so in `needs` and is asked about once; for a script outside `/store`
  the launcher takes the rules the shell sends, the `needs` its user
  approved, since no deriver holds them, and bounds them only by what
  the session holds.
- **Not taken:** sourcing, running a file in the caller's scope with
  the caller's authority (POSIX's `.` built-in). Its cost is that every
  script holds everything its caller holds.

## L20. The recipe and the command line

**Decided** (by the owner): consent, rule 3. **Proposed:** the rest.

What an external program holds when it starts is the union of:

| Part | From |
|---|---|
| Its closure, at `/store` | Its store path, through the launcher ([storage.md F5](storage.md#f5-per-process-views)) |
| The network, the display and the other services and devices its derivation declares | The launcher, from the session's own channels to them (rule 2), never from the shell |
| Paths its derivation declares | The shell's view, by name; one the shell cannot give fails the launch with `launch`, naming it (F5, rule 3) |
| Its working directory, with read or write | Its derivation's declaration, from `$cwd`, shown as a grant before Enter ([L21](#l21-interactive-behaviour)) |
| A console session | Its derivation's declaration, given by the shell |
| A powerbox channel | Its derivation's declaration ([L18](#l18-asking-for-more-the-powerbox)) |
| What its arguments name | The command line ([L15](#l15-grants-from-arguments)) |
| What `grant` attaches | The command line ([L17](#l17-rights)) |
| Standard input, output and error | The pipeline ([L7](#l7-pipelines)) |

1. The declarations and the signature are read from the program's
   derivation, the deriver its registration names
   ([storage.md F8](storage.md#f8-the-store-volume)), never from bytes
   its build wrote. They count only if that derivation's outputs,
   computed from it as nova-nix computes them, include the program's
   path, so whoever supplies a registration cannot attach another
   derivation's declarations ([Changes elsewhere](#changes-elsewhere)).
   The launcher reads them, since the deriver is outside the shell's
   closure. A path with no deriver, such as a source, has none and gets
   only its closure, its arguments and its streams.
2. **Services from the launcher.** For each service or device the
   derivation declares, the launcher makes the program's handle from
   the session's own channel to that service: a network grant with
   exactly the declared rules, on the session's network control channel,
   or for a program a script runs on the script's
   ([network.md N15](network.md#n15-declared-authority)), and a display
   connection, minted from the session's
   ([display.md D5](display.md#d5-connections-labels-and-layers)). One
   the session's channel does not cover, or for a program a script runs
   one the script's declaration does not name, fails the launch with
   `launch`, naming it. The shell asks the launcher to run the program and sends
   it no handle to a service, its own included.
3. **Consent.** Installing a program into a profile, the user's or the
   system's ([storage.md F13](storage.md#f13-profiles-system-and-garbage-collection)),
   is consent to what its derivation declares. A program run from a
   store path that is in no profile asks once per SHA-256 of its
   derivation, and a script once per SHA-256 of its file
   ([L19](#l19-scripts)): the shell shows what it declares, and keeps the
   answer as a setting, so the same derivation or file is not asked
   about again. Outside an interactive session a declaration without
   consent fails the launch with `launch`.
4. `grants { command }` shows every row above for each program the
   command would start whose arguments are known without running
   anything; it runs no capture, and shows an argument that depends on
   one, and a program a condition chooses, as unknown.

- **Why:** principle 7: "what a program can reach is read from its
  recipe before it runs". The recipe gives what a program needs whatever
  it is asked to do, the part Plash calls the "installation endowment",
  "the set of files, directories and other objects that it needs and
  should have access to regardless of the parameters you give to the
  program" (Plash, The Plash shell); the command line gives what this
  run needs. Reading the declaration from the derivation and not the
  output means a build cannot widen its own program's authority.
- **Why services from the launcher:** a service can check only a grant
  it issued itself, so the launcher, which reads the declaration, has
  the service issue each one on a channel it served to the session;
  the rules a program gets are then its declaration's, never the
  shell's choice.
- **Cost:** a substituted path needs its derivation present to have its
  declarations; an endowment declared wrongly fails the launch until
  the recipe is fixed; a program that declares its working directory
  gets whichever directory the user stands in, so `cd` is a designation
  too. Consent is a prompt the first time each program from outside a
  profile or each script runs, and a setting kept per answer; a program
  installed is given its whole declaration without a prompt.
- **Why consent where the user acts:** installing is the act of
  choosing a program, so it is asked nothing more; a program tried once
  without installing, or a script someone else may have written, is
  shown before it gets what it declares, and a change to either, which
  changes its SHA-256, is shown again.
- **Not taken:** Plash's default endowment, read-only access to "/bin,
  /lib, /usr and /etc" for every program, because "Unix does not have
  this information" (Plash, The Plash shell). nova-os has it in every
  recipe. Granting what is declared without asking, as principle 7 reads
  on its own; its cost is a script from anywhere given its `needs` the
  first time it runs. Asking at every run; its cost is a prompt users
  learn to accept unread. The shell deriving each program's service
  grants from its own and sending them with its run request. Its cost
  is a grant the service cannot tell from a channel of the shell's own
  making, since an endpoint's handle reports its own object and not its
  peer ([objects.md O4](objects.md#o4-handle-operations)), and rules the
  shell, not the declaration, chooses
  ([network.md N15](network.md#n15-declared-authority)).

## L21. Interactive behaviour

**Decided** (by the owner): Ctrl+L, rule 10. **Proposed:** the rest.

The console edits the line where it draws it
([console.md C4](console.md#c4-sessions)); the shell assists through the
console protocol:

1. **Assistance.** While a read waits, the console sends the shell the
   line and a version number on each edit. The shell answers with style
   spans, an inline suggestion and errors for that version; the console
   applies an answer only to the version it was made for, so typing never
   waits for the shell. Tab asks for completions, which come with a
   description each. Up and Down ask for history entries that start with
   the line's text.
2. **Styles are roles,** not colours: command, built-in, keyword,
   string, number, variable, read grant, write grant, flag, comment,
   error, suggestion. The console's theme, a setting
   ([L24](#l24-settings-and-service-commands)), gives them colours.
3. **Grants are shown before Enter.** Each path argument is styled by
   the right it will grant, a write as a write, and one a link redirects
   shows its target; a program whose recipe declares its working
   directory shows that grant beside its name.
4. **Errors are shown before Enter:** an unknown command, a read grant
   of a path that does not exist, an argument a signature refuses, a glob
   with no match, an unclosed bracket.
5. **Completion** offers commands with their descriptions, flags and
   parameter values from signatures (a setting's allowed values among
   them), paths from the shell's view, variables, and history.
6. **Suggestions** come from history, preferring lines entered in the
   same `$cwd` whose paths still exist.
7. **Enter** with a bracket open continues the line.
8. **Interrupt** stops the statement: the shell terminates the
   pipeline's processes and raises `interrupted`.
9. **Rendering:** tables in columns to the panel's width, sizes in IEC
   units, times in UTC until a time zone is a setting.
10. **Ctrl+L** clears the panel, as `clear` does ([L25](#l25-built-ins)),
    and draws the line being edited again at its top.

- **Why:** fish's interactive defaults, on by default and not
  configurable: autosuggestions "based on command history, completions,
  and valid file paths", tab completion with a pager, and syntax
  highlighting whose "most important feedback is the detection of
  potential errors" (fish, Interactive use). Its law of responsiveness:
  "It is only acceptable to block in response to a user initiated
  action" (fish, Design). Colours as roles answer fish's own note that
  "text color preferences [should] be defined centrally by the user for
  all programs" (fish, Design, "Configurability is the root of all
  evil"). Showing grants before Enter makes the authority a command gets
  visible at the moment the user decides.
- **Cost:** a round trip to the shell for every edit; the console
  protocol grows assisted reads, style spans, an interrupt key and a
  clear operation ([Changes elsewhere](#changes-elsewhere)); until the
  console draws UTF-8, names outside ASCII show as `?` (console.md C4).
- **Not taken:** the shell editing the line itself in a raw key mode,
  which the console's design turns down: "editing that each client does
  itself in a raw mode" (console.md C4).

## L22. History

**Proposed.**

1. Each entered line is kept as a record: the line, `$cwd`, the time,
   the outcome's kind and how long it took. Duplicates are removed.
2. A line that starts with a space is kept for the session but not
   written, as fish does (fish, Interactive use, "Searchable command
   history"); `--private` keeps none, like fish's private mode.
3. History is a file in the user's volume that the shell writes through
   its home handle; the accounts design fixes its name.
4. `history` gives the history as a table, for `where` and `select`.

- **Why:** structured history answers "what did I run here, and did it
  work" with the same commands as any other table.
- **Cost:** a file write per line entered.
- **Not taken:** history as the session recording. History is the
  user's to edit and delete; a recording is the system's account of
  what happened ([L23](#l23-replay)).

## L23. Replay

**Proposed.**

1. The kernel records what the shell receives as for any process
   ([replay.md](replay.md#inputs-through-the-kernel)): the console's
   replies, lines typed and assistance requests included; the launcher's
   replies; directory listings and file reads; the messages on its
   children's channels; `process_info` results; `clock_monotonic` and
   `random_read`. Replaying them reproduces the shell's execution.
2. The shell is deterministic given those inputs: built-ins run on one
   thread ([L7](#l7-pipelines)), so replay.md's thread interleaving does
   not arise; globs and listings are in bytewise order; records keep
   their order; decimal arithmetic is software; the time comes only from
   `clock_monotonic` and the wall-clock service's messages, and random
   bytes only from `random_read`.
3. The shell shares no writable memory with its children, only
   channels, so it can be recorded alone or with them
   ([replay.md](replay.md#shared-memory)).
4. The shell adds to the recording a transcript: each statement's
   source text and, for every program it started, the store path, each
   grant (path, rights, and whether an argument, the recipe or a request
   made it) and the outcome. Replay ignores it, since it is not an input;
   it is what an audit reads to learn what a session's commands could
   reach without replaying. A line read without echo never enters the
   transcript.

- **Why:** principle 8 makes what a program "could and could not do ...
  a fact that can be checked". The recording proves it by replay; the
  transcript states it directly, per command, for a reader.
- **Cost:** the transcript repeats facts the recording already implies;
  a recording holds everything typed, and is "as sensitive as the
  session it records" (replay.md, Recordings).
- **Not taken:** deriving the transcript by replaying a session each
  time it is read. Its cost is a full replay to answer a question about
  one command.

## L24. Settings and service commands

**Decided** (by the owner): every service describes its interface once,
in one interface description language, and a generator makes from that
description the service's C protocol header, its nvo commands, its
Settings forms and its documentation. New services start on it, and
existing protocols move over to it one by one. **Proposed:** the rest.

Settings are served by a settings service whose keys each have a name
(`desktop.background`), a schema (type, allowed values, default,
description) and a value. The Settings window and the shell
are two clients of the same protocol, and neither has a path the other
lacks (DESIGN.md, principle 3).

```
setting get desktop.background          # limb
setting set desktop.background tide
setting list desktop                    # name, value, default, type, description
setting reset desktop.background
setting watch desktop.background        # one record per change, until interrupted
```

1. The shell checks a value against the key's schema before it sends
   it, and the service checks it again: the service is the authority.
2. Completion offers keys and allowed values from the schemas.
3. What a session may read and change is the settings handle it holds;
   settings for the whole system need the handle an administrator holds.
   A script asks for them in `needs` as the service `settings`
   ([L19](#l19-scripts)).
4. **The generic bridge.** Each service describes its interface once, in
   the interface description language ([Changes
   elsewhere](#changes-elsewhere)): its requests and replies, its
   settings with their schemas, and what each is for. From that one text
   the generator makes the service's C protocol header, the nvo commands
   that speak its protocol, the Settings window's form for each of its
   settings, and its documentation.
5. **A service's settings** are keys of the settings service whose
   schemas come from the service's description; the service reads them
   there. The network service's log is one
   ([network.md N12](network.md#n12-grants)).
6. **A service's actions** that are not settings, such as switching the
   system's generation or creating a user, are its generated nvo
   commands, with signatures as [L9](#l9-functions-and-signatures)
   gives, so they are checked, completed and described as any command
   is. How the shell finds them and reaches the service through a handle
   the session holds is the interface description language's
   specification to give.
7. A new service is described this way from its start. An existing
   protocol keeps its hand-written header until it moves over.

- **Why:** one protocol behind both interfaces is principle 3's
  guarantee, and typed keys with descriptions make settings
  discoverable from the shell. One description per service makes the
  guarantee mechanical: the header a service is built against, the
  command a user types, the form the Settings window draws and the
  documentation a reader reads come from the same text, so none of them
  can hold a request, a setting or a field the others lack.
- **Cost:** every setting the interface shows needs a schema; a
  description language and a generator to specify, build from the seed
  and keep; a protocol in two forms, hand-written and described, until
  each moves over.
- **Not taken:** configuration files that the shell edits as text while
  the interface keeps its own store. Its cost is two sources of truth.
  A program of commands for each service, written by hand beside its
  protocol, with a generic bridge once two services' interfaces showed
  what it must describe. Its cost is each service's commands, forms and
  documentation kept in step with its protocol by hand, each a place to
  drift, and a bridge fitted afterwards to protocols not described for
  it.

## L25. Built-ins

**Decided** (by the owner): `clear`, which clears the console's panel.
**Proposed:** the rest.

A command is built in only if it changes the shell's own
state (S), transforms values in a pipeline, where an external process
would only add a round trip (V), mediates authority, launching or the
console (A), or bridges files and values so that a pipeline can begin or
end (F). File operations (`cp`, `mv`, `rm`, `mkdir`) and text tools are
programs in `/store`, confined like any other.

| Built-in | Does | |
|---|---|---|
| `cd` | Sets `$cwd` | S |
| `exit` | Ends the script or the shell with a code | S |
| `clear` | Clears the console's panel, as Ctrl+L does ([L21](#l21-interactive-behaviour)) | A |
| `history` | The history as a table | S |
| `help` | A command's signature and description | A |
| `which` | Where a name resolves | A |
| `describe` | A value's type | V |
| `ls` | A directory as a table: `name`, `path`, `kind`, `size`, `modified`, `executable` | F |
| `glob` | A pattern's matches as a list | F |
| `open` | A file's contents as one `bytes` value | F |
| `save` | Writes bytes or strings to a file (`--append`) | F |
| `lines` | Bytes or strings into one string per line | V |
| `from` | Parses `json`, `csv` or `cbor` | V |
| `to` | Formats as `text`, `json`, `csv` or `cbor` | V |
| `into` | Converts a scalar: `int`, `decimal`, `string`, `bytes`, `path`, `size`, `duration`, `time` | V |
| `where` | Keeps the values a closure is true for | V |
| `each` | Maps a closure over the values | V |
| `select` | Keeps the named columns | V |
| `get` | The value at a member path | V |
| `sort-by` | Orders by a closure's result | V |
| `first`, `last` | The first or last n values | V |
| `length` | How many values | V |
| `reduce` | Folds the values with a closure | V |
| `ignore` | Discards its input | V |
| `print` | Writes to the shell's output now | S |
| `fail` | Raises an error | S |
| `ok` | A block's success as a `bool` | S |
| `complete` | A block's output, error output and status as a record | A |
| `run` | Runs a store path (`--substitute` fetches it first) or a script | A |
| `with` | Runs a block with programs from a recipe | A |
| `grant` | Attaches paths to the programs a block runs | A |
| `grants` | What a command would hold; what running programs hold | A |
| `setting` | Reads and changes settings | A |

- **Why:** each built-in either cannot be a separate process (`cd`,
  `with`, `grant`) or is used in nearly every pipeline. fish's rule is
  "The differences between built-in commands and shellscript functions
  should be made as small as possible" (fish, Design, "The law of user
  focus"); here built-ins, functions and programs share one signature
  format and one way of being called. A program such as `date`, which
  reads the wall clock, stays a program, so only what is given the clock
  reads it.
- **Why `clear` and Ctrl+L:** they are what users of other shells
  already type. `clear` is ncurses's command to "clear the terminal
  screen", Nushell's built-in, and on Windows an alias of PowerShell's
  `Clear-Host`; Ctrl+L clears the screen in the line editors of Bash and
  zsh, of Nushell (reedline) and of PowerShell (PSReadLine), and in fish
  (Clear; fish, Interactive use).
- **Cost:** file management needs its programs in the profile from the
  first boot; the set will grow, and each addition is a decision.
- **Not taken:** Nushell's hundreds of built-in commands. Their cost is
  a shell that is also the largest program on the system, with its
  authority. `cls`, the DOS spelling, which cmd.exe keeps ("Clears the
  Command Prompt window") and PowerShell aliases; and `clr`, which none
  of the sources above uses. Each is a name to learn for an act users
  already know by another.

## Prior art

| Source | Taken | Left |
|---|---|---|
| POSIX sh | Pipelines of small commands; `$name`; 0 as success | Field splitting (2.6.5); globs that stand for themselves and sort by locale (2.14.3); the last stage's status (2.9.2); 126, 127 and 128+ (2.8.2); four quoting forms (2.2); `PATH` (2.5.3) |
| rc | Input scanned once (28); lists as values (5); `''` in single quotes (3); every stage's status (11) | Patterns that stand for themselves (4); statuses as text (11) |
| PowerShell | Structured values in pipelines; typed, named parameters; help per command (1, 8.14, 13) | Verb-Noun names (13); single-value unwrapping and all-output returns (8.1.2, 8.5.4); conversions at binding (6, 8.14); .NET objects (1) |
| Nushell | Tables and records; `$in`; immutable `let`; errors with labelled spans; `^`; `complete` | Automatic rendering into programs and automatic decoding out of them (Pipelines); binary floats (Types of Data) |
| fish | Autosuggestions, highlighting, described completions (Interactive use); no-match is an error (Wildcards); space-prefixed private lines; responsiveness and discoverability (Design) | Command substitution split on lines; universal variables (The fish language) |
| Oils (YSH) | Simple word evaluation; static globs only; no lost errors; `sigpipe_status_ok`; exterior data (Chu 2023) | Text data languages on the wire; bash compatibility |
| Elvish | Values in pipelines; failure as exceptions; composite pipeline errors and stopped writers (9.1); assignment as copying | `?()` that swallows every error; truthiness; imports that run code (12.3.3) |
| Plash | Arguments granted; read-only by default; `=>` and slots; attachments; quoting for strings; the endowment; the powerbox with no default path | `=>` over all later arguments; `!!`; a default endowment; scripts with the shell's authority |
| Capsicum | No global namespaces; lookups under a held directory (2.1); the pipeline shell as the confining point (4.3) | Entering a sandbox from ambient authority: nova-os starts without it (objects.md O6) |
| Nix | Running from the store without installing; environments from a recipe | Flake references and registries |
| Plan 9 | Per-process name spaces, "a per-process attribute" (Plan 9 names), as views (storage.md F5) | Union directories (storage.md F4) |

## Changes elsewhere

What this design needs outside this document:

- **DESIGN.md, principle 7:** a program's creator passes it what its
  derivation declares and what the user designates for one run (its
  arguments, `grant`, the powerbox), and nothing else.
- **storage.md:**
  - view entries that name one file or one slot, whose handle answers
    opens of that one name, so a program can open a granted file more
    than once and create a granted name that did not exist
    ([F5](storage.md#f5-per-process-views)); a slot open in the
    directory protocol ([F1](storage.md#f1-the-directory-protocol));
  - F5 rule 2 for a launcher that acts for a requester: it grants what
    the requester sends, which can only be what the requester's view
    holds;
  - the profile's closure joined to the `/store` closure directory of
    the user's programs, as the system's is (F5), so a profile's links
    resolve;
  - a registration's deriver kept only if the derivation's computed
    outputs include the path
    ([F12](storage.md#f12-builds-and-substitution));
  - a way to look registrations up by name, for `which` and completion
    of store paths outside the shell's closure.
- **The launcher's protocol:** a run request that carries the view
  entries and their handles, the standard streams and whether each
  speaks bytes or values, the working directory as text, the
  environment, and a powerbox channel and console session where
  declared, within the 64 handles a message carries
  ([ipc.md I1](ipc.md#i1-channels)) or across several messages; a reply
  whose process handle also carries `TERMINATE`, so an interrupt can
  stop the program; a request that returns a store path's declarations
  and signature, read from its deriver; and a request that opens a store
  path's closure directory for the shell to hold.
- **The substituter:** a request to fetch a store path, from a session
  that holds a channel to it.
- **ipc.md and syscalls.md:** a channel created to refuse handles, on
  which a write that carries one fails, for pipes
  ([I1](ipc.md#i1-channels), [I2](ipc.md#i2-handle-transfer));
  `process_info` reports how a process ended (an exit, a fault and its
  vector, or a kill) beside its code
  ([S8](syscalls.md#s8-the-first-calls)).
- **console.md:** assisted reads ([L21](#l21-interactive-behaviour));
  writes with style spans by role, and a style that only the shell's own
  session may use, for its prompts; an interrupt key reported to the
  session; UTF-8 text; a session the shell can give a program that
  declares the console; while the shell shows a powerbox prompt, the
  keyboard given to the shell's read first and the prompt drawn over
  every other session; and a clear operation, which `clear` and Ctrl+L
  use ([L21](#l21-interactive-behaviour), [L25](#l25-built-ins)).
- **replay.md:** annotations a recorded process adds to its recording,
  which replay skips ([L23](#l23-replay)).
- **nova-nix:** derivation attributes for a program's declared grants,
  working directory, console and powerbox, and its signature: parameters,
  stream kinds and exit statuses.
- **New specifications:** the value protocol, the CBOR tags and framing
  of [L7](#l7-pipelines) and the session in values mode; the settings
  service and its schemas ([L24](#l24-settings-and-service-commands));
  the interface description language and its generator
  ([L24](#l24-settings-and-service-commands)); the powerbox protocol
  ([L18](#l18-asking-for-more-the-powerbox)).

## References

The registry and documentation pages behind [The name](#the-name), read
on November 7, 2026, which show no fixed versions:
[npm `novascript`][npm-novascript], [Numerus: NovaScript
Introduction][numerus], [Homebrew `nsh`][brew-nsh], [nixpkgs
`pkgs/by-name/ns/nsh`][nixpkgs-nsh], [NuttShell (NSH)][nuttx-nsh],
[npm `nv`][npm-nv], [PyPI `nv`][pypi-nv], [NSIS Chapter 2][nsis],
[Linguist `languages.yml`][linguist] at commit
`d456527fe93355baa28c3c29d542c97282b61bc7`, and EDK II's
[`ShellPkg/Application/Shell/Shell.c`][edk2-shell] at the commit pinned
in [README.md](README.md#firmware-and-platform). `nvo` was looked up as
the [Homebrew formula][brew-nvo], the [PyPI project][pypi-nvo], the
[npm package][npm-nvo] and the nixpkgs directory
`pkgs/by-name/nv/nvo`.

[npm-novascript]: https://registry.npmjs.org/novascript
[numerus]: https://wiki.numerusinc.com/index.php/NovaScript_Introduction
[brew-nsh]: https://formulae.brew.sh/formula/nsh
[nixpkgs-nsh]: https://github.com/NixOS/nixpkgs/tree/master/pkgs/by-name/ns/nsh
[nuttx-nsh]: https://nuttx.apache.org/docs/latest/applications/nsh/index.html
[npm-nv]: https://registry.npmjs.org/nv
[pypi-nv]: https://pypi.org/project/nv/
[nsis]: https://nsis.sourceforge.io/Docs/Chapter2.html
[linguist]: https://github.com/github-linguist/linguist/blob/d456527fe93355baa28c3c29d542c97282b61bc7/lib/linguist/languages.yml
[edk2-shell]: https://github.com/tianocore/edk2/blob/2970e5699ba6267f3384ffab20f96647578aebc8/ShellPkg/Application/Shell/Shell.c
[brew-nvo]: https://formulae.brew.sh/formula/nvo
[pypi-nvo]: https://pypi.org/project/nvo/
[npm-nvo]: https://registry.npmjs.org/nvo
