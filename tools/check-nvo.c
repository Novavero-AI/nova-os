/* Checks nvo's source text and values (lib/nvo_syntax.c, lib/nvo_value.c,
   docs/spec/shell.md L1 to L3) on the host: that every example in the
   document parses, that each parse error L1 and L2 name is raised at its
   offset, that a tree printed and parsed again is the same tree, over the
   shapes listed and over sources generated from the grammar and mutated
   from those, that number literals have the values L1 gives, and that
   values meet L3's rules. Prints each failure and a count, and exits
   nonzero if any check fails.

   The document is the argument: the check reads its examples from it, so
   that an example edited there is the one checked here.

   cc -std=c99 -Wall -Wextra -pedantic -I include -I lib -o check-nvo \
     tools/check-nvo.c lib/nvo_decimal.c lib/nvo_syntax.c lib/nvo_value.c &&
     ./check-nvo docs/spec/shell.md */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvo_value.h"

/* The longest source and the largest tree a case in a table holds. */
#define CASE_SOURCE_MAX 4096
#define CASE_NODES NVO_NODES_FOR(CASE_SOURCE_MAX)

/* The unfenced examples shell.md holds: its fenced blocks other than the
   grammar. A new one fails the check until it is counted here. */
#define SPEC_EXAMPLES 8

/* The longest document the check reads. */
#define DOCUMENT_MAX 262144

static unsigned failures;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    printf("failed: %s\n", what);
    failures++;
}

/* A failure that names the source it is about. */
static void check_source(int passed, const char *what, const char *source)
{
    if (passed)
        return;
    printf("failed: %s: [%s]\n", what, source);
    failures++;
}

/* ------------------------------------------------------------------------
   Trees */

static const char *const kind_names[NVO_NODE_KINDS] = {
    [NVO_NODE_NONE] = "NONE",
    [NVO_NODE_FILE] = "FILE",
    [NVO_NODE_NEEDS] = "NEEDS",
    [NVO_NODE_SCRIPT] = "SCRIPT",
    [NVO_NODE_BLOCK] = "BLOCK",
    [NVO_NODE_CLOSURE] = "CLOSURE",
    [NVO_NODE_PARAMS] = "PARAMS",
    [NVO_NODE_LET] = "LET",
    [NVO_NODE_VAR] = "VAR",
    [NVO_NODE_SET] = "SET",
    [NVO_NODE_DEF] = "DEF",
    [NVO_NODE_USE] = "USE",
    [NVO_NODE_IF] = "IF",
    [NVO_NODE_ELIF] = "ELIF",
    [NVO_NODE_ELSE] = "ELSE",
    [NVO_NODE_FOR] = "FOR",
    [NVO_NODE_WHILE] = "WHILE",
    [NVO_NODE_TRY] = "TRY",
    [NVO_NODE_CATCH] = "CATCH",
    [NVO_NODE_BREAK] = "BREAK",
    [NVO_NODE_CONTINUE] = "CONTINUE",
    [NVO_NODE_RETURN] = "RETURN",
    [NVO_NODE_PIPELINE] = "PIPELINE",
    [NVO_NODE_PARAM] = "PARAM",
    [NVO_NODE_FLAG_PARAM] = "FLAG_PARAM",
    [NVO_NODE_REST_PARAM] = "REST_PARAM",
    [NVO_NODE_SHORT] = "SHORT",
    [NVO_NODE_TYPE] = "TYPE",
    [NVO_NODE_FIELD_TYPE] = "FIELD_TYPE",
    [NVO_NODE_COMMAND] = "COMMAND",
    [NVO_NODE_FLAG] = "FLAG",
    [NVO_NODE_GRANT] = "GRANT",
    [NVO_NODE_SPREAD] = "SPREAD",
    [NVO_NODE_WORD] = "WORD",
    [NVO_NODE_NAME] = "NAME",
    [NVO_NODE_DEFNAME] = "DEFNAME",
    [NVO_NODE_BARE] = "BARE",
    [NVO_NODE_PATH] = "PATH",
    [NVO_NODE_GLOB] = "GLOB",
    [NVO_NODE_STRING_SQ] = "STRING_SQ",
    [NVO_NODE_STRING_DQ] = "STRING_DQ",
    [NVO_NODE_TEXT] = "TEXT",
    [NVO_NODE_VARIABLE] = "VARIABLE",
    [NVO_NODE_MEMBER] = "MEMBER",
    [NVO_NODE_SUBST] = "SUBST",
    [NVO_NODE_GROUP] = "GROUP",
    [NVO_NODE_INT] = "INT",
    [NVO_NODE_DECIMAL] = "DECIMAL",
    [NVO_NODE_SIZE] = "SIZE",
    [NVO_NODE_DURATION] = "DURATION",
    [NVO_NODE_BYTES] = "BYTES",
    [NVO_NODE_TRUE] = "TRUE",
    [NVO_NODE_FALSE] = "FALSE",
    [NVO_NODE_NOTHING] = "NOTHING",
    [NVO_NODE_LIST] = "LIST",
    [NVO_NODE_RECORD] = "RECORD",
    [NVO_NODE_ENTRY] = "ENTRY",
    [NVO_NODE_TABLE] = "TABLE",
    [NVO_NODE_COLUMNS] = "COLUMNS",
    [NVO_NODE_ROW] = "ROW",
    [NVO_NODE_OR] = "OR",
    [NVO_NODE_AND] = "AND",
    [NVO_NODE_NOT] = "NOT",
    [NVO_NODE_EQ] = "EQ",
    [NVO_NODE_NE] = "NE",
    [NVO_NODE_LT] = "LT",
    [NVO_NODE_LE] = "LE",
    [NVO_NODE_GT] = "GT",
    [NVO_NODE_GE] = "GE",
    [NVO_NODE_IN] = "IN",
    [NVO_NODE_NOT_IN] = "NOT_IN",
    [NVO_NODE_CONCAT] = "CONCAT",
    [NVO_NODE_ADD] = "ADD",
    [NVO_NODE_SUB] = "SUB",
    [NVO_NODE_MUL] = "MUL",
    [NVO_NODE_DIV] = "DIV",
    [NVO_NODE_IDIV] = "IDIV",
    [NVO_NODE_MOD] = "MOD",
    [NVO_NODE_NEG] = "NEG",
    [NVO_NODE_ACCESS] = "ACCESS",
};

/* The node kinds the generated and the listed sources between them make:
   a kind none makes is a kind the checks above do not reach. */
static unsigned char kinds_seen[NVO_NODE_KINDS];

static void mark_kinds(const NVO_TREE *tree)
{
    UINT32 i;

    for (i = 1; i < tree->count; i++)
        kinds_seen[tree->nodes[i].kind] = 1;
}

typedef struct {
    NVO_NODE nodes[CASE_NODES];
    NVO_TREE tree;
} CASE_TREE;

static CASE_TREE case_a, case_b;

static NVO_STATUS parse_source(CASE_TREE *c, NVO_PARSE_MODE mode,
                               const UINT8 *source, UINT64 length)
{
    return nvo_parse(&c->tree, mode, source, length, c->nodes, CASE_NODES);
}

static NVO_STATUS parse_text(CASE_TREE *c, NVO_PARSE_MODE mode,
                             const char *source)
{
    return parse_source(c, mode, (const UINT8 *)source, strlen(source));
}

/* A buffer text is appended to. What does not fit is dropped and counted,
   so a description too long for it is noticed. */
typedef struct {
    char *text;
    size_t capacity;
    size_t length;
} OUTPUT;

static void put_bytes(OUTPUT *out, const char *bytes, size_t count)
{
    size_t room = out->length < out->capacity ? out->capacity - out->length : 0;

    if (room > 0)
        memcpy(out->text + out->length, bytes, count < room ? count : room);
    out->length += count;
}

static void put_string(OUTPUT *out, const char *text)
{
    put_bytes(out, text, strlen(text));
}

/* Append the tree below index as `KIND[flags]:text(child,child)`: the flags
   when not 0, the text for a node that has its own. Recursive, for the
   short sources of the tables. */
static void describe_node(OUTPUT *out, const NVO_TREE *tree,
                          const UINT8 *source, UINT32 index)
{
    const NVO_NODE *node = &tree->nodes[index];
    char flags[16];
    UINT32 child;

    put_string(out, kind_names[node->kind]);
    if (node->flags != 0) {
        snprintf(flags, sizeof flags, "[%u]", (unsigned)node->flags);
        put_string(out, flags);
    }
    if (nvo_node_has_text(node)) {
        put_string(out, ":");
        put_bytes(out, (const char *)source + node->offset, node->length);
    }
    if (node->first == 0)
        return;
    put_string(out, "(");
    for (child = node->first; child != 0; child = tree->nodes[child].next) {
        describe_node(out, tree, source, child);
        if (tree->nodes[child].next != 0)
            put_string(out, ",");
    }
    put_string(out, ")");
}

/* The two trees are the same: kinds, flags and children everywhere, and
   the text of every node that has its own. Walks both without recursing,
   as a tree can be as deep as its source is long. */
static int same_tree(const NVO_TREE *a, const UINT8 *sa, const NVO_TREE *b,
                     const UINT8 *sb)
{
    UINT32 x = a->root, y = b->root;

    for (;;) {
        const NVO_NODE *p = &a->nodes[x], *q = &b->nodes[y];

        if (p->kind != q->kind || p->flags != q->flags ||
            (p->first == 0) != (q->first == 0))
            return 0;
        if (nvo_node_has_text(p) &&
            (p->length != q->length ||
             memcmp(sa + p->offset, sb + q->offset, p->length) != 0))
            return 0;
        if (p->first != 0) {
            x = p->first;
            y = q->first;
            continue;
        }
        for (;;) {
            p = &a->nodes[x];
            q = &b->nodes[y];
            if (x == a->root)
                return y == b->root;
            if (p->next != 0 || q->next != 0) {
                if ((p->next == 0) != (q->next == 0))
                    return 0;
                x = p->next;
                y = q->next;
                break;
            }
            x = p->parent;
            y = q->parent;
        }
    }
}

/* Whether the tree is well formed: every child names its parent, the
   siblings link in order, and every span is inside the source. */
static int well_formed(const NVO_TREE *tree, UINT64 length)
{
    UINT32 i, child, last;

    for (i = 1; i < tree->count; i++) {
        const NVO_NODE *node = &tree->nodes[i];

        if ((UINT64)node->offset + node->length > length)
            return 0;
        last = 0;
        for (child = node->first; child != 0; child = tree->nodes[child].next) {
            if (child >= tree->count || tree->nodes[child].parent != i)
                return 0;
            last = child;
        }
        if (node->last != last)
            return 0;
    }
    return 1;
}

/* Parse the length bytes of source, print the tree, and parse that again:
   the same tree, and the same printing. Returns 1 if all of it holds, else
   0. */
static int round_trips_bytes(NVO_PARSE_MODE mode, const UINT8 *source,
                             UINT64 length)
{
    static UINT8 first[CASE_SOURCE_MAX * 4], second[CASE_SOURCE_MAX * 4];
    UINT64 first_length, second_length;

    if (parse_source(&case_a, mode, source, length) != NVO_OK)
        return 0;
    if (!well_formed(&case_a.tree, length))
        return 0;
    if (nvo_print(&case_a.tree, source, first, sizeof first, &first_length) !=
        NVO_OK)
        return 0;
    if (parse_source(&case_b, mode, first, first_length) != NVO_OK)
        return 0;
    if (!same_tree(&case_a.tree, source, &case_b.tree, first))
        return 0;
    if (nvo_print(&case_b.tree, first, second, sizeof second, &second_length) !=
        NVO_OK)
        return 0;
    return first_length == second_length &&
           memcmp(first, second, first_length) == 0;
}

static int round_trips(NVO_PARSE_MODE mode, const char *source)
{
    return round_trips_bytes(mode, (const UINT8 *)source, strlen(source));
}

/* ------------------------------------------------------------------------
   The document's examples */

/* The fenced blocks of the document that are nvo and not its grammar, each
   parsed as a file and round-tripped. */
static void spec_examples(const char *path)
{
    static char document[DOCUMENT_MAX + 1];
    static char block[CASE_SOURCE_MAX];
    size_t length, block_length = 0;
    char *line;
    int inside = 0;
    unsigned found = 0;
    FILE *in = fopen(path, "r");

    if (!in) {
        printf("failed: cannot open %s\n", path);
        failures++;
        return;
    }
    length = fread(document, 1, DOCUMENT_MAX, in);
    fclose(in);
    document[length] = '\0';
    for (line = document; *line != '\0';) {
        char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) + 1 : strlen(line);

        if (strncmp(line, "```", 3) == 0 &&
            (line[3] == '\n' || line[3] == '\0')) {
            if (inside) {
                block[block_length] = '\0';
                if (strstr(block, "(* Syntax *)") == 0) {
                    found++;
                    check_source(parse_text(&case_a, NVO_PARSE_FILE, block) ==
                                     NVO_OK,
                                 "a document example parses", block);
                    mark_kinds(&case_a.tree);
                    check_source(round_trips(NVO_PARSE_FILE, block),
                                 "a document example round-trips", block);
                }
                block_length = 0;
            }
            inside = !inside;
        } else if (inside && block_length + n < sizeof block) {
            memcpy(block + block_length, line, n);
            block_length += n;
        }
        line += n;
    }
    check(found == SPEC_EXAMPLES, "the document holds the examples counted");
}

/* ------------------------------------------------------------------------
   Shapes */

typedef struct {
    NVO_PARSE_MODE mode;
    const char *source;
    const char *tree; /* describe_node's form of the root */
} SHAPE;

#define SCRIPT NVO_PARSE_SCRIPT
#define FILE_MODE NVO_PARSE_FILE

static const SHAPE shapes[] = {
    {SCRIPT, "^needs x", "SCRIPT(COMMAND[1](BARE:needs,BARE:x))"},
    {SCRIPT, "^needs { }", "SCRIPT(COMMAND[1](BARE:needs,BLOCK(SCRIPT)))"},
    {SCRIPT, "1 + 2 * 3", "SCRIPT(ADD(INT:1,MUL(INT:2,INT:3)))"},
    {SCRIPT, "1 - 2 - 3", "SCRIPT(SUB(SUB(INT:1,INT:2),INT:3))"},
    {SCRIPT, "not $a == $b", "SCRIPT(NOT(EQ(VARIABLE:a,VARIABLE:b)))"},
    {SCRIPT, "$a or $b and $c",
     "SCRIPT(OR(VARIABLE:a,AND(VARIABLE:b,VARIABLE:c)))"},
    {SCRIPT, "-$x * 2", "SCRIPT(MUL(NEG(VARIABLE:x),INT:2))"},
    {SCRIPT, "- -$x", "SCRIPT(NEG(NEG(VARIABLE:x)))"},
    {SCRIPT, "$a ++ $b + $c",
     "SCRIPT(CONCAT(VARIABLE:a,ADD(VARIABLE:b,VARIABLE:c)))"},
    {SCRIPT, "$a < $b ++ $c",
     "SCRIPT(LT(VARIABLE:a,CONCAT(VARIABLE:b,VARIABLE:c)))"},
    {SCRIPT, "$a in $b", "SCRIPT(IN(VARIABLE:a,VARIABLE:b))"},
    {SCRIPT, "$a not-in $b", "SCRIPT(NOT_IN(VARIABLE:a,VARIABLE:b))"},
    {SCRIPT, "$a.b.0.\"c d\"?",
     "SCRIPT(ACCESS[1](ACCESS(ACCESS(VARIABLE:a,MEMBER:b),MEMBER[1]:0),MEMBER["
     "2](STRING_DQ(TEXT:c d))))"},
    {SCRIPT, "$a.b?.c",
     "SCRIPT(ACCESS(ACCESS[1](VARIABLE:a,MEMBER:b),MEMBER:c))"},
    {SCRIPT, "$a // $b % $c / $d",
     "SCRIPT(DIV(MOD(IDIV(VARIABLE:a,VARIABLE:b),VARIABLE:c),VARIABLE:d))"},
    {SCRIPT, "ls ./src | length",
     "SCRIPT(PIPELINE(COMMAND(BARE:ls,PATH:./src),COMMAND(BARE:length)))"},
    {SCRIPT, "^ls", "SCRIPT(COMMAND[1](BARE:ls))"},
    {SCRIPT, "^if", "SCRIPT(COMMAND[1](BARE:if))"},
    {SCRIPT, "ls => ./x ...$y --f=1 -x -- --",
     "SCRIPT(COMMAND(BARE:ls,GRANT(PATH:./"
     "x),SPREAD(VARIABLE:y),FLAG:--f=1,FLAG:-x,FLAG:--,FLAG:--))"},
    {SCRIPT, "ls \"a\"b$c",
     "SCRIPT(COMMAND(BARE:ls,WORD(STRING_DQ(TEXT:a),BARE:b,VARIABLE:c)))"},
    {SCRIPT, "ls ./\"My Documents\"/notes.md",
     "SCRIPT(COMMAND(BARE:ls,WORD(PATH:./,STRING_DQ(TEXT:My "
     "Documents),BARE:/notes.md)))"},
    {SCRIPT, "ls ${f}.bak",
     "SCRIPT(COMMAND(BARE:ls,WORD(VARIABLE[2]:f,BARE:.bak)))"},
    {SCRIPT, "ls $f.bak?",
     "SCRIPT(COMMAND(BARE:ls,VARIABLE[1]:f(MEMBER:bak)))"},
    {SCRIPT, "ls $f.bak", "SCRIPT(COMMAND(BARE:ls,VARIABLE:f(MEMBER:bak)))"},
    {SCRIPT, "ls $f. .x",
     "SCRIPT(COMMAND(BARE:ls,WORD(VARIABLE:f,BARE:.),BARE:.x))"},
    {SCRIPT, "ls a>b --format=%h,%s a#b #c",
     "SCRIPT(COMMAND(BARE:ls,BARE:a>b,FLAG:--format=%h,%s,BARE:a#b))"},
    {SCRIPT, "ls ...(cat x) ... $x \"...\" ... =>x",
     "SCRIPT(COMMAND(BARE:ls,SPREAD(GROUP(COMMAND(BARE:cat,BARE:x))),BARE:...,"
     "VARIABLE:x,STRING_DQ(TEXT:...),BARE:...,BARE:=>x))"},
    {SCRIPT, "ls \"=>\" => /x",
     "SCRIPT(COMMAND(BARE:ls,STRING_DQ(TEXT:=>),GRANT(PATH:/x)))"},
    {SCRIPT, "ls [1, 2] x[3]",
     "SCRIPT(COMMAND(BARE:ls,LIST(INT:1,INT:2),WORD(BARE:x,LIST(INT:3))))"},
    {SCRIPT, "ls {a} { |x| b }",
     "SCRIPT(COMMAND(BARE:ls,BLOCK(SCRIPT(COMMAND(BARE:a))),CLOSURE(PARAMS("
     "NAME:x),SCRIPT(COMMAND(BARE:b)))))"},
    {SCRIPT, "(ls | length)",
     "SCRIPT(GROUP(PIPELINE(COMMAND(BARE:ls),COMMAND(BARE:length))))"},
    {SCRIPT, "(1 + 2)", "SCRIPT(GROUP(ADD(INT:1,INT:2)))"},
    {SCRIPT, "[1, ...$a, 2,]", "SCRIPT(LIST(INT:1,SPREAD(VARIABLE:a),INT:2))"},
    {SCRIPT, "[[true, false]]", "SCRIPT(LIST(LIST(TRUE:true,FALSE:false)))"},
    {SCRIPT, "[[a, b]; [1, 2], [3, 4],]",
     "SCRIPT(TABLE(COLUMNS(NAME:a,NAME:b),ROW(INT:1,INT:2),ROW(INT:3,INT:4)))"},
    {SCRIPT, "{a: 1, \"b c\": 2, ...$d,}",
     "SCRIPT(RECORD(ENTRY(NAME:a,INT:1),ENTRY(STRING_DQ(TEXT:b "
     "c),INT:2),SPREAD(VARIABLE:d)))"},
    {SCRIPT, "{}", "SCRIPT(RECORD)"},
    {SCRIPT, "[]", "SCRIPT(LIST)"},
    {SCRIPT, "{ |x, y| $x + $y }",
     "SCRIPT(CLOSURE(PARAMS(NAME:x,NAME:y),SCRIPT(ADD(VARIABLE:x,VARIABLE:y)))"
     ")"},
    {SCRIPT, "{||1}", "SCRIPT(CLOSURE(PARAMS,SCRIPT(INT:1)))"},
    {SCRIPT, "if $a { b } else if $c { d } else { e }",
     "SCRIPT(IF(VARIABLE:a,BLOCK(SCRIPT(COMMAND(BARE:b))),ELIF(VARIABLE:c,"
     "BLOCK(SCRIPT(COMMAND(BARE:d)))),ELSE(BLOCK(SCRIPT(COMMAND(BARE:e))))))"},
    {SCRIPT, "let x: list<int> = 1",
     "SCRIPT(LET(NAME:x,TYPE:list(TYPE:int),INT:1))"},
    {SCRIPT, "var y: record<a: int, \"b c\": path(read)> = {a: 1}",
     "SCRIPT(VAR(NAME:y,TYPE:record(FIELD_TYPE(NAME:a,TYPE:int),FIELD_TYPE("
     "STRING_DQ(TEXT:b c),TYPE[1]:path)),RECORD(ENTRY(NAME:a,INT:1))))"},
    {SCRIPT, "set r.size.0 += 1KiB",
     "SCRIPT(SET[1](NAME:r,MEMBER:size,MEMBER[1]:0,SIZE:1KiB))"},
    {SCRIPT, "set x = 1", "SCRIPT(SET(NAME:x,INT:1))"},
    {SCRIPT, "set x ++= [1]", "SCRIPT(SET[3](NAME:x,LIST(INT:1)))"},
    {SCRIPT, "export let a = 1", "SCRIPT(LET[1](NAME:a,INT:1))"},
    {SCRIPT, "use ./m.nvo as m", "SCRIPT(USE(PATH:./m.nvo,NAME:m))"},
    {SCRIPT, "for x in $l { break; continue }",
     "SCRIPT(FOR(NAME:x,VARIABLE:l,BLOCK(SCRIPT(BREAK,CONTINUE))))"},
    {SCRIPT, "while true { return }",
     "SCRIPT(WHILE(TRUE:true,BLOCK(SCRIPT(RETURN))))"},
    {SCRIPT, "try { a } catch e { b }",
     "SCRIPT(TRY(BLOCK(SCRIPT(COMMAND(BARE:a))),CATCH(NAME:e,BLOCK(SCRIPT("
     "COMMAND(BARE:b))))))"},
    {SCRIPT, "try { a } catch { b }",
     "SCRIPT(TRY(BLOCK(SCRIPT(COMMAND(BARE:a))),CATCH(BLOCK(SCRIPT(COMMAND("
     "BARE:b))))))"},
    {SCRIPT, "return -1", "SCRIPT(RETURN(NEG(INT:1)))"},
    {FILE_MODE, "needs {a: 1}\nls",
     "FILE(NEEDS(RECORD(ENTRY(NAME:a,INT:1))),SCRIPT(COMMAND(BARE:ls)))"},
    {SCRIPT,
     "def f(a: int, b?: string = \"x\", --flag(-f): bool = true, --other, "
     "...rest: string) -> int { $a }",
     "SCRIPT(DEF(DEFNAME:f,PARAMS(PARAM(NAME:a,TYPE:int),PARAM[1](NAME:b,TYPE:"
     "string,STRING_DQ(TEXT:x)),FLAG_PARAM(DEFNAME:flag,SHORT:f,TYPE:bool,TRUE:"
     "true),FLAG_PARAM(DEFNAME:other),REST_PARAM(NAME:rest,TYPE:string)),TYPE:"
     "int,BLOCK(SCRIPT(VARIABLE:a))))"},
    {SCRIPT, "x", "SCRIPT(COMMAND(BARE:x))"},
    {SCRIPT, "$x | $y", "SCRIPT(PIPELINE(VARIABLE:x,VARIABLE:y))"},
    {SCRIPT, "1 | 2", "SCRIPT(PIPELINE(INT:1,INT:2))"},
    {SCRIPT, "ls |\n wc",
     "SCRIPT(PIPELINE(COMMAND(BARE:ls),COMMAND(BARE:wc)))"},
    {SCRIPT, "ls | # c\n wc",
     "SCRIPT(PIPELINE(COMMAND(BARE:ls),COMMAND(BARE:wc)))"},
    {SCRIPT, "(ls\n| wc)",
     "SCRIPT(GROUP(PIPELINE(COMMAND(BARE:ls),COMMAND(BARE:wc))))"},
    {SCRIPT, "ls\n\n\nwc ; ; pwd",
     "SCRIPT(COMMAND(BARE:ls),COMMAND(BARE:wc),COMMAND(BARE:pwd))"},
    {SCRIPT, "\"a${b.c}$d.e$(f)\\u{1F600}\\\\\\\"\\$\"",
     "SCRIPT(STRING_DQ(TEXT:a,VARIABLE[2]:b(MEMBER:c),VARIABLE:d(MEMBER:e),"
     "SUBST(COMMAND(BARE:f)),TEXT:\\u{1F600}\\\\\\\"\\$))"},
    {SCRIPT, "'a''b'", "SCRIPT(STRING_SQ:'a''b')"},
    {SCRIPT, "0x[ de ad  be ef ] ++ 0x[]",
     "SCRIPT(CONCAT(BYTES:0x[ de ad  be ef ],BYTES:0x[]))"},
    {SCRIPT, "0x2a + 0o52 + 0b101 + 1_0",
     "SCRIPT(ADD(ADD(ADD(INT:0x2a,INT:0o52),INT:0b101),INT:1_0))"},
    {SCRIPT, "1.5 + 2.5e-3 + 3e4 + 4E+2",
     "SCRIPT(ADD(ADD(ADD(DECIMAL:1.5,DECIMAL:2.5e-3),DECIMAL:3e4),DECIMAL:4E+2)"
     ")"},
    {SCRIPT, "4KiB + 512B + 1.5MB + 250ms + 1.5s + 2min + 3h + 1d + 10ns + 5us",
     "SCRIPT(ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(SIZE:4KiB,SIZE:512B),SIZE:1."
     "5MB),DURATION:250ms),DURATION:1.5s),DURATION:2min),DURATION:3h),DURATION:"
     "1d),DURATION:10ns),DURATION:5us))"},
    {SCRIPT, ".", "SCRIPT(PATH:.)"},
    {SCRIPT, "..", "SCRIPT(PATH:..)"},
    {SCRIPT, "~", "SCRIPT(PATH:~)"},
    {SCRIPT, "/", "SCRIPT(PATH:/)"},
    {SCRIPT, "~/src/**/*.c", "SCRIPT(GLOB:~/src/**/*.c)"},
    {SCRIPT, "true and false or nothing",
     "SCRIPT(OR(AND(TRUE:true,FALSE:false),NOTHING:nothing))"},
    {SCRIPT, "$in.size > 10MB",
     "SCRIPT(GT(ACCESS(VARIABLE:in,MEMBER:size),SIZE:10MB))"},
    {SCRIPT, "ls ~/Documents", "SCRIPT(COMMAND(BARE:ls,PATH:~/Documents))"},
    {SCRIPT, "ls ./src", "SCRIPT(COMMAND(BARE:ls,PATH:./src))"},
    {SCRIPT, "7z", "SCRIPT(COMMAND(BARE:7z))"},
    {SCRIPT, "42", "SCRIPT(INT:42)"},
    {SCRIPT, "4KiB", "SCRIPT(SIZE:4KiB)"},
    {SCRIPT, "./src / \"x\"", "SCRIPT(DIV(PATH:./src,STRING_DQ(TEXT:x)))"},
    {SCRIPT, "./*.md", "SCRIPT(GLOB:./*.md)"},
    {SCRIPT, "./a?.c", "SCRIPT(GLOB:./a?.c)"},
    {SCRIPT, "[./a, ./b]\n", "SCRIPT(LIST(PATH:./a,PATH:./b))"},
    {SCRIPT, "[./a,./b]", "SCRIPT(LIST(PATH:./a,PATH:./b))"},
    {SCRIPT, "[~,.,..,/]", "SCRIPT(LIST(PATH:~,PATH:.,PATH:..,PATH:/))"},
    {SCRIPT, "[./a,]", "SCRIPT(LIST(PATH:./a))"},
    {SCRIPT, "{a: ./x, b: ./y}",
     "SCRIPT(RECORD(ENTRY(NAME:a,PATH:./x),ENTRY(NAME:b,PATH:./y)))"},
    {SCRIPT, "{a: ./x,b: ./y}",
     "SCRIPT(RECORD(ENTRY(NAME:a,PATH:./x),ENTRY(NAME:b,PATH:./y)))"},
    {SCRIPT, "[[a, b]; [./x,./y], [./z, ./w]]",
     "SCRIPT(TABLE(COLUMNS(NAME:a,NAME:b),ROW(PATH:./x,PATH:./y),ROW(PATH:./"
     "z,PATH:./w)))"},
    {SCRIPT, "[./*.md,./a?, ./b]",
     "SCRIPT(LIST(GLOB:./*.md,GLOB:./a?,PATH:./b))"},
    {SCRIPT, "[./\"a,b\", ./c]",
     "SCRIPT(LIST(WORD(PATH:./,STRING_DQ(TEXT:a,b)),PATH:./c))"},
    {SCRIPT, "[./\"a b\"x,./c]",
     "SCRIPT(LIST(WORD(PATH:./,STRING_DQ(TEXT:a b),BARE:x),PATH:./c))"},
    {SCRIPT, "[...$a,./b]", "SCRIPT(LIST(SPREAD(VARIABLE:a),PATH:./b))"},
    {SCRIPT, "ls [./a,./b]",
     "SCRIPT(COMMAND(BARE:ls,LIST(PATH:./a,PATH:./b)))"},
    {SCRIPT, "ls ./a,b", "SCRIPT(COMMAND(BARE:ls,PATH:./a,b))"},
    {SCRIPT, "cp ./a,b ./c,d",
     "SCRIPT(COMMAND(BARE:cp,PATH:./a,b,PATH:./c,d))"},
    {SCRIPT, "ls ./*,?", "SCRIPT(COMMAND(BARE:ls,GLOB:./*,?))"},
    {SCRIPT, "a,b c", "SCRIPT(COMMAND(BARE:a,b,BARE:c))"},
    {SCRIPT, "1,2 x", "SCRIPT(COMMAND(BARE:1,2,BARE:x))"},
    {SCRIPT, "if,x y", "SCRIPT(COMMAND(BARE:if,x,BARE:y))"},
    {SCRIPT, "let,x y", "SCRIPT(COMMAND(BARE:let,x,BARE:y))"},
    {SCRIPT, "ls \"a\",b",
     "SCRIPT(COMMAND(BARE:ls,WORD(STRING_DQ(TEXT:a),BARE:,b)))"},
    {SCRIPT, "ls ./\"c\",d $a,b",
     "SCRIPT(COMMAND(BARE:ls,WORD(PATH:./,STRING_DQ(TEXT:c),BARE:,d),WORD("
     "VARIABLE:a,BARE:,b)))"},
    {SCRIPT, "ls --out=\"a\",b",
     "SCRIPT(COMMAND(BARE:ls,FLAG:--out=(STRING_DQ(TEXT:a),BARE:,b)))"},
    {SCRIPT, "def f(a: path = ./x,b: int = 1) { }",
     "SCRIPT(DEF(DEFNAME:f,PARAMS(PARAM(NAME:a,TYPE:path,PATH:./x),PARAM(NAME:"
     "b,TYPE:int,INT:1)),BLOCK(SCRIPT)))"},
    {SCRIPT, "$a in [./x,./y]",
     "SCRIPT(IN(VARIABLE:a,LIST(PATH:./x,PATH:./y)))"},
    {SCRIPT, "ls (cat ./a,b) x,y",
     "SCRIPT(COMMAND(BARE:ls,GROUP(COMMAND(BARE:cat,PATH:./a,b)),BARE:x,y))"},
    {SCRIPT, "ls a,b ,c d,",
     "SCRIPT(COMMAND(BARE:ls,BARE:a,b,BARE:,c,BARE:d,))"},
    {SCRIPT, "$x + 1", "SCRIPT(ADD(VARIABLE:x,INT:1))"},
    {SCRIPT, "\"text\"", "SCRIPT(STRING_DQ(TEXT:text))"},
    {SCRIPT, "[1, 2]", "SCRIPT(LIST(INT:1,INT:2))"},
    {SCRIPT, "1_000", "SCRIPT(INT:1_000)"},
    {SCRIPT, "0b101010", "SCRIPT(INT:0b101010)"},
    {SCRIPT, "0.1", "SCRIPT(DECIMAL:0.1)"},
    {SCRIPT, "512B", "SCRIPT(SIZE:512B)"},
    {SCRIPT, "0x[de ad be ef]", "SCRIPT(BYTES:0x[de ad be ef])"},
    {SCRIPT, "/users/alice", "SCRIPT(PATH:/users/alice)"},
    {SCRIPT, "./notes.md", "SCRIPT(PATH:./notes.md)"},
    {SCRIPT, "../lib", "SCRIPT(PATH:../lib)"},
    {SCRIPT, "'no $ or \\ here, and it''s raw'",
     "SCRIPT(STRING_SQ:'no $ or \\ here, and it''s raw')"},
    {SCRIPT, "\"report ${name}.txt\"",
     "SCRIPT(STRING_DQ(TEXT:report ,VARIABLE[2]:name,TEXT:.txt))"},
    {SCRIPT, "\"tab\\tthen $(^date)\"",
     "SCRIPT(STRING_DQ(TEXT:tab\\tthen ,SUBST(COMMAND[1](BARE:date))))"},
    {SCRIPT, "./a/\"b\"/c",
     "SCRIPT(WORD(PATH:./a/,STRING_DQ(TEXT:b),BARE:/c))"},
    {SCRIPT, "git log --format=%h,%s",
     "SCRIPT(COMMAND(BARE:git,BARE:log,FLAG:--format=%h,%s))"},
    {SCRIPT, "echo a>b", "SCRIPT(COMMAND(BARE:echo,BARE:a>b))"},
    {SCRIPT, "1+2", "SCRIPT(COMMAND(BARE:1+2))"},
    {SCRIPT, "$a/$b", "SCRIPT(DIV(VARIABLE:a,VARIABLE:b))"},
    {SCRIPT, "$a//$b", "SCRIPT(IDIV(VARIABLE:a,VARIABLE:b))"},
    {SCRIPT, "$a-1", "SCRIPT(SUB(VARIABLE:a,INT:1))"},
    {SCRIPT, "1.foo", "SCRIPT(COMMAND(BARE:1.foo))"},
    {SCRIPT, "1.5.3", "SCRIPT(COMMAND(BARE:1.5.3))"},
    {SCRIPT, "$x.1.2",
     "SCRIPT(ACCESS(ACCESS(VARIABLE:x,MEMBER[1]:1),MEMBER[1]:2))"},
    {SCRIPT, "{a: {b: [1, {c: 2}]}}",
     "SCRIPT(RECORD(ENTRY(NAME:a,RECORD(ENTRY(NAME:b,LIST(INT:1,RECORD(ENTRY("
     "NAME:c,INT:2))))))))"},
    {SCRIPT, "ls -5 - -x", "SCRIPT(COMMAND(BARE:ls,BARE:-5,BARE:-,FLAG:-x))"},
    {SCRIPT, "ls $x?", "SCRIPT(COMMAND(BARE:ls,WORD(VARIABLE:x,BARE:?)))"},
    {SCRIPT, "\"$x?\" ", "SCRIPT(STRING_DQ(VARIABLE:x,TEXT:?))"},
    {SCRIPT, "\"$f.bak?\" ", "SCRIPT(STRING_DQ(VARIABLE[1]:f(MEMBER:bak)))"},
    {SCRIPT, "ls --out=\"a b\" -o$x",
     "SCRIPT(COMMAND(BARE:ls,FLAG:--out=(STRING_DQ(TEXT:a "
     "b)),FLAG:-o(VARIABLE:x)))"},
    {SCRIPT, "ls a(b)c",
     "SCRIPT(COMMAND(BARE:ls,WORD(BARE:a,GROUP(COMMAND(BARE:b)),BARE:c)))"},
    {SCRIPT, "ls ~user .hidden",
     "SCRIPT(COMMAND(BARE:ls,BARE:~user,BARE:.hidden))"},

    /* `else` and `catch` are keywords as whole words: any other run that
       begins with one is a command, after one newline or two. */
    {SCRIPT, "if $x { 1 }\nelse! { 2 }",
     "SCRIPT(IF(VARIABLE:x,BLOCK(SCRIPT(INT:1))),COMMAND(BARE:else!,BLOCK("
     "SCRIPT(INT:2))))"},
    {SCRIPT, "if $x { 1 }\n\nelse! { 2 }",
     "SCRIPT(IF(VARIABLE:x,BLOCK(SCRIPT(INT:1))),COMMAND(BARE:else!,BLOCK("
     "SCRIPT(INT:2))))"},
    {SCRIPT, "try { 1 }\ncatch! { 2 }",
     "SCRIPT(TRY(BLOCK(SCRIPT(INT:1))),COMMAND(BARE:catch!,BLOCK(SCRIPT(INT:2)"
     ")))"},
    {SCRIPT, "if $x { 1 }\nelse,y",
     "SCRIPT(IF(VARIABLE:x,BLOCK(SCRIPT(INT:1))),COMMAND(BARE:else,y))"},
    {SCRIPT, "if $x { 1 }\nelse.y",
     "SCRIPT(IF(VARIABLE:x,BLOCK(SCRIPT(INT:1))),COMMAND(BARE:else.y))"},
    {SCRIPT, "if $x { 1 }\nelse*",
     "SCRIPT(IF(VARIABLE:x,BLOCK(SCRIPT(INT:1))),COMMAND(BARE:else*))"},
    {SCRIPT, "if $x { 1 }\nelse@",
     "SCRIPT(IF(VARIABLE:x,BLOCK(SCRIPT(INT:1))),COMMAND(BARE:else@))"},
    {SCRIPT, "try { 1 }\ncatch,y",
     "SCRIPT(TRY(BLOCK(SCRIPT(INT:1))),COMMAND(BARE:catch,y))"},
    {SCRIPT, "try { 1 }\ncatch@",
     "SCRIPT(TRY(BLOCK(SCRIPT(INT:1))),COMMAND(BARE:catch@))"},
    {SCRIPT, "if $x { 1 }\nelse { 2 }\nelse! y",
     "SCRIPT(IF(VARIABLE:x,BLOCK(SCRIPT(INT:1)),ELSE(BLOCK(SCRIPT(INT:2)))),"
     "COMMAND(BARE:else!,BARE:y))"},

    /* `=>` is a mark only as a whole word: any part touching it makes a word
       of two parts, and a run that continues it is text. */
    {SCRIPT, "ls =>$x", "SCRIPT(COMMAND(BARE:ls,WORD(BARE:=>,VARIABLE:x)))"},
    {SCRIPT, "ls =>\"x\"",
     "SCRIPT(COMMAND(BARE:ls,WORD(BARE:=>,STRING_DQ(TEXT:x))))"},
    {SCRIPT, "ls =>'x'",
     "SCRIPT(COMMAND(BARE:ls,WORD(BARE:=>,STRING_SQ:'x')))"},
    {SCRIPT, "ls =>(a)",
     "SCRIPT(COMMAND(BARE:ls,WORD(BARE:=>,GROUP(COMMAND(BARE:a)))))"},
    {SCRIPT, "ls =>[1]", "SCRIPT(COMMAND(BARE:ls,WORD(BARE:=>,LIST(INT:1))))"},
    {SCRIPT, "ls =>x", "SCRIPT(COMMAND(BARE:ls,BARE:=>x))"},
    {SCRIPT, "ls =>./x", "SCRIPT(COMMAND(BARE:ls,BARE:=>./x))"},
    {SCRIPT, "ls => =>x", "SCRIPT(COMMAND(BARE:ls,GRANT(BARE:=>x)))"},
    {SCRIPT, "ls => $x\n", "SCRIPT(COMMAND(BARE:ls,GRANT(VARIABLE:x)))"},
    {SCRIPT, "^=> x", "SCRIPT(COMMAND[1](BARE:=>,BARE:x))"},
    {SCRIPT, "=>x y", "SCRIPT(COMMAND(BARE:=>x,BARE:y))"},

    /* The grammar's rows and spreads: a row may hold no values, and a
       spread ends where its variable's members do. */
    {SCRIPT, "[[a]; []]", "SCRIPT(TABLE(COLUMNS(NAME:a),ROW))"},
    {SCRIPT, "[[a, b]; [1, 2], []]",
     "SCRIPT(TABLE(COLUMNS(NAME:a,NAME:b),ROW(INT:1,INT:2),ROW))"},
    {SCRIPT, "ls ...$x.y?",
     "SCRIPT(COMMAND(BARE:ls,SPREAD(VARIABLE:x(MEMBER:y)),BARE:?))"},
    {SCRIPT, "ls ...$x.\"y\"",
     "SCRIPT(COMMAND(BARE:ls,SPREAD(VARIABLE:x(MEMBER[2](STRING_DQ(TEXT:y)))))"
     ")"},

    /* No blank is needed after a block, a closure, a spread or a head: the
       next argument begins where it does, or a comment. */
    {SCRIPT, "ls {a}#c",
     "SCRIPT(COMMAND(BARE:ls,BLOCK(SCRIPT(COMMAND(BARE:a)))))"},
    {SCRIPT, "ls {a}b",
     "SCRIPT(COMMAND(BARE:ls,BLOCK(SCRIPT(COMMAND(BARE:a))),BARE:b))"},
    {SCRIPT, "ls {a}{b}",
     "SCRIPT(COMMAND(BARE:ls,BLOCK(SCRIPT(COMMAND(BARE:a))),BLOCK(SCRIPT("
     "COMMAND(BARE:b)))))"},
    {SCRIPT, "ls a{b}",
     "SCRIPT(COMMAND(BARE:ls,BARE:a,BLOCK(SCRIPT(COMMAND(BARE:b)))))"},
    {SCRIPT, "ls { |x| b }c",
     "SCRIPT(COMMAND(BARE:ls,CLOSURE(PARAMS(NAME:x),SCRIPT(COMMAND(BARE:b))),"
     "BARE:c))"},
    {SCRIPT, "ls ...$x\"abc\"",
     "SCRIPT(COMMAND(BARE:ls,SPREAD(VARIABLE:x),STRING_DQ(TEXT:abc)))"},
    {SCRIPT, "ls ...(a)b",
     "SCRIPT(COMMAND(BARE:ls,SPREAD(GROUP(COMMAND(BARE:a))),BARE:b))"},
    {SCRIPT, "ls ...$x#c", "SCRIPT(COMMAND(BARE:ls,SPREAD(VARIABLE:x)))"},
    {SCRIPT, "ls ...$x ...$y",
     "SCRIPT(COMMAND(BARE:ls,SPREAD(VARIABLE:x),SPREAD("
     "VARIABLE:y)))"},
    {SCRIPT, "ls [1]{a}",
     "SCRIPT(COMMAND(BARE:ls,LIST(INT:1),BLOCK(SCRIPT(COMMAND(BARE:a)))))"},
    {SCRIPT, "ls (a){b}",
     "SCRIPT(COMMAND(BARE:ls,GROUP(COMMAND(BARE:a)),BLOCK(SCRIPT(COMMAND(BARE:"
     "b)))))"},
    {SCRIPT, "ls $(a){b}",
     "SCRIPT(COMMAND(BARE:ls,SUBST(COMMAND(BARE:a)),BLOCK(SCRIPT(COMMAND(BARE:"
     "b)))))"},
    {SCRIPT, "ls \"a\"{b}",
     "SCRIPT(COMMAND(BARE:ls,STRING_DQ(TEXT:a),BLOCK(SCRIPT(COMMAND(BARE:b))))"
     ")"},
    {SCRIPT, "ls$x", "SCRIPT(COMMAND(BARE:ls,VARIABLE:x))"},
    {SCRIPT, "ls\"a\"", "SCRIPT(COMMAND(BARE:ls,STRING_DQ(TEXT:a)))"},
    {SCRIPT, "ls'a'", "SCRIPT(COMMAND(BARE:ls,STRING_SQ:'a'))"},
    {SCRIPT, "ls[1]", "SCRIPT(COMMAND(BARE:ls,LIST(INT:1)))"},
    {SCRIPT, "^ls(a)", "SCRIPT(COMMAND[1](BARE:ls,GROUP(COMMAND(BARE:a))))"},

    /* In an expression a token can begin after an operand, so a `#` after
       one is a comment. */
    {SCRIPT, "\"a\"#c", "SCRIPT(STRING_DQ(TEXT:a))"},
    {SCRIPT, "$y#c", "SCRIPT(VARIABLE:y)"},
    {SCRIPT, "(1)#c", "SCRIPT(GROUP(INT:1))"},

    /* In a command's arguments a `#` that touches an earlier part of a word
       is a character of it; one where a word could begin is a comment. */
    {SCRIPT, "ls \"a\"#b",
     "SCRIPT(COMMAND(BARE:ls,WORD(STRING_DQ(TEXT:a),BARE:#b)))"},
    {SCRIPT, "ls $x#c", "SCRIPT(COMMAND(BARE:ls,WORD(VARIABLE:x,BARE:#c)))"},
    {SCRIPT, "ls [1]#c", "SCRIPT(COMMAND(BARE:ls,WORD(LIST(INT:1),BARE:#c)))"},
    {SCRIPT, "ls a #b", "SCRIPT(COMMAND(BARE:ls,BARE:a))"},
    {SCRIPT, "ls ./x#y", "SCRIPT(COMMAND(BARE:ls,PATH:./x#y))"},
    {SCRIPT, "ls -f#x", "SCRIPT(COMMAND(BARE:ls,FLAG:-f#x))"},
    {SCRIPT, "ls -f\"a\"#c",
     "SCRIPT(COMMAND(BARE:ls,FLAG:-f(STRING_DQ(TEXT:a),BARE:#c)))"},
    {SCRIPT, "./x\"a\"#b", "SCRIPT(WORD(PATH:./x,STRING_DQ(TEXT:a),BARE:#b))"},
    {SCRIPT, "^ls#c", "SCRIPT(COMMAND[1](BARE:ls#c))"},

    /* Every dot inside the braces of a variable begins a member. */
    {SCRIPT, "ls ${a.b.c}",
     "SCRIPT(COMMAND(BARE:ls,VARIABLE[2]:a(MEMBER:b,MEMBER:c)))"},
    {SCRIPT, "ls ${a.0}",
     "SCRIPT(COMMAND(BARE:ls,VARIABLE[2]:a(MEMBER[1]:0)))"},
    {SCRIPT, "ls ${a.\"b\"}",
     "SCRIPT(COMMAND(BARE:ls,VARIABLE[2]:a(MEMBER[2](STRING_DQ(TEXT:b)))))"},
};

static void shape_cases(void)
{
    static char got[CASE_SOURCE_MAX];
    unsigned i;

    for (i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        OUTPUT out;
        NVO_STATUS status =
            parse_text(&case_a, shapes[i].mode, shapes[i].source);

        if (status != NVO_OK) {
            check_source(0, "a shape parses", shapes[i].source);
            continue;
        }
        mark_kinds(&case_a.tree);
        out.text = got;
        out.capacity = sizeof got - 1;
        out.length = 0;
        describe_node(&out, &case_a.tree, (const UINT8 *)shapes[i].source,
                      case_a.tree.root);
        got[out.length < out.capacity ? out.length : out.capacity] = '\0';
        if (strcmp(got, shapes[i].tree) != 0) {
            printf("failed: [%s]\n  got  %s\n  want %s\n", shapes[i].source,
                   got, shapes[i].tree);
            failures++;
        }
        check_source(round_trips(shapes[i].mode, shapes[i].source),
                     "a shape round-trips", shapes[i].source);
    }
}

/* The printer writes a comma straight after a path, which an expression
   ends at, with one blank after it, and ends a script with a newline. */
static const struct {
    const char *source;
    const char *printed;
} printings[] = {
    {"[./a,./b]", "[./a, ./b]\n"},
    {"[ ./a ,  ./b ]", "[./a, ./b]\n"},
    {"{a: ./x,b: ./*.c}", "{a: ./x, b: ./*.c}\n"},
    {"[[a, b]; [./x ,./y]]", "[[a, b]; [./x, ./y]]\n"},
};

static void printing_cases(void)
{
    unsigned i;

    for (i = 0; i < sizeof printings / sizeof printings[0]; i++) {
        static UINT8 out[CASE_SOURCE_MAX];
        const char *source = printings[i].source;
        UINT64 written = 0;
        int printed = parse_text(&case_a, SCRIPT, source) == NVO_OK &&
                      nvo_print(&case_a.tree, (const UINT8 *)source, out,
                                sizeof out, &written) == NVO_OK &&
                      written == strlen(printings[i].printed) &&
                      memcmp(out, printings[i].printed, written) == 0;

        check_source(printed, "a source is printed as written", source);
    }
}

/* ------------------------------------------------------------------------
   Errors */

typedef struct {
    NVO_PARSE_MODE mode;
    const char *source;
    NVO_ERROR error;
    UINT64 offset; /* of the first byte the error is about */
} ERROR_CASE;

/* Each error L1 and L2 name, at the offset they name: a byte sequence that
   is not UTF-8 at its first bad byte, and the others at the token they
   are about, or an unterminated one at its opening. */
static const ERROR_CASE errors[] = {
    /* L1 rule 1: UTF-8 is RFC 3629. */
    {SCRIPT, "\377", NVO_ERROR_UTF8, 0},
    {SCRIPT, "ls caf\303", NVO_ERROR_UTF8, 6},
    {SCRIPT, "a \340\200\200", NVO_ERROR_UTF8, 2},
    {SCRIPT, "\300\200", NVO_ERROR_UTF8, 0},
    {SCRIPT, "ls \355\240\200", NVO_ERROR_UTF8, 3},
    {SCRIPT, "ls \364\220\200\200", NVO_ERROR_UTF8, 3},
    {SCRIPT, "ls \360\217\277\277", NVO_ERROR_UTF8, 3},
    {SCRIPT, "ls \370\210\200\200\200", NVO_ERROR_UTF8, 3},
    {SCRIPT, "ls \200", NVO_ERROR_UTF8, 3},
    {SCRIPT, "\"\303\251\"\303", NVO_ERROR_UTF8, 4},
    {SCRIPT, ") \377", NVO_ERROR_UTF8, 2},
    {SCRIPT, "\"abc \377", NVO_ERROR_UTF8, 5},
    {FILE_MODE, "needs \377", NVO_ERROR_UTF8, 6},

    /* L1 rule 4: a bare word in an expression. */
    {SCRIPT, "1 + x", NVO_ERROR_BARE_WORD, 4},
    {SCRIPT, "$a == b", NVO_ERROR_BARE_WORD, 6},
    {SCRIPT, "[1, two]", NVO_ERROR_BARE_WORD, 4},
    {SCRIPT, "let x = y", NVO_ERROR_BARE_WORD, 8},
    {SCRIPT, "-x", NVO_ERROR_BARE_WORD, 1},
    {SCRIPT, "1 < x < 3", NVO_ERROR_BARE_WORD, 4},
    {SCRIPT, "if x { }", NVO_ERROR_BARE_WORD, 3},
    {SCRIPT, "(1 + y)", NVO_ERROR_BARE_WORD, 5},
    {SCRIPT, "1 + \\\n2", NVO_ERROR_BARE_WORD, 4},
    {SCRIPT, "needs { }", NVO_ERROR_UNEXPECTED, 0},
    {FILE_MODE, "ls\nneeds { }", NVO_ERROR_UNEXPECTED, 3},

    /* A comma ends an expression's path, and no statement holds one. */
    {SCRIPT, "./a,b", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "let x = ./a,b", NVO_ERROR_UNEXPECTED, 11},
    {SCRIPT, "use ./a,b as m", NVO_ERROR_UNEXPECTED, 7},
    {SCRIPT, "[./a,,./b]", NVO_ERROR_UNEXPECTED, 5},
    {SCRIPT, "[,./a]", NVO_ERROR_UNEXPECTED, 1},

    /* L2: comparisons do not chain, and the error is at the second. */
    {SCRIPT, "1 < $x < 3", NVO_ERROR_CHAINED, 7},
    {SCRIPT, "$a == $b == $c", NVO_ERROR_CHAINED, 9},
    {SCRIPT, "$a != $b < $c", NVO_ERROR_CHAINED, 9},
    {SCRIPT, "1 in [1] in [2]", NVO_ERROR_CHAINED, 9},
    {SCRIPT, "$a in $b not-in $c", NVO_ERROR_CHAINED, 9},
    {SCRIPT, "$a >= 1 >= 2", NVO_ERROR_CHAINED, 8},
    {SCRIPT, "(1 < $x < 3)", NVO_ERROR_CHAINED, 8},

    /* Structure the grammar does not allow. */
    {SCRIPT, ")", NVO_ERROR_UNEXPECTED, 0},
    {SCRIPT, "}", NVO_ERROR_UNEXPECTED, 0},
    {SCRIPT, "]", NVO_ERROR_UNEXPECTED, 0},
    {SCRIPT, "ls )", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "(1 + 2]", NVO_ERROR_UNEXPECTED, 6},
    {SCRIPT, "1 +", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "ls |", NVO_ERROR_UNEXPECTED, 4},
    {SCRIPT, "ls | | ls", NVO_ERROR_UNEXPECTED, 5},
    {SCRIPT, "()", NVO_ERROR_UNEXPECTED, 1},
    {SCRIPT, "$x.", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "let = 1", NVO_ERROR_UNEXPECTED, 4},
    {SCRIPT, "let x", NVO_ERROR_UNEXPECTED, 5},
    {SCRIPT, "let x = ", NVO_ERROR_UNEXPECTED, 8},
    {SCRIPT, "for x $y { }", NVO_ERROR_UNEXPECTED, 6},
    {SCRIPT, "def f(a) { }", NVO_ERROR_UNEXPECTED, 7},
    {SCRIPT, "{ |a b| 1 }", NVO_ERROR_UNEXPECTED, 5},
    {SCRIPT, "else { 1 }", NVO_ERROR_UNEXPECTED, 0},
    {SCRIPT, "catch { 1 }", NVO_ERROR_UNEXPECTED, 0},
    {SCRIPT, "if $x { 1 }\n\nelse { 2 }", NVO_ERROR_UNEXPECTED, 13},
    {SCRIPT, "try { 1 }\n\ncatch { 2 }", NVO_ERROR_UNEXPECTED, 11},
    {SCRIPT, "ls\rfoo", NVO_ERROR_UNEXPECTED, 2},
    {SCRIPT, "[1,\n2\n,3\n]\r4", NVO_ERROR_UNEXPECTED, 10},
    {SCRIPT, "[[a, b]; ]", NVO_ERROR_UNEXPECTED, 9},
    {SCRIPT, "$x $y", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "1 2", NVO_ERROR_UNEXPECTED, 2},
    {SCRIPT, "{ a: 1 b: 2 }", NVO_ERROR_UNEXPECTED, 7},
    {SCRIPT, "set x 1", NVO_ERROR_UNEXPECTED, 6},
    {SCRIPT, "use x", NVO_ERROR_UNEXPECTED, 4},
    {FILE_MODE, "needs { }\nneeds { }", NVO_ERROR_UNEXPECTED, 10},

    /* A string or a bracket that is never closed, at its opening. */
    {SCRIPT, "\"abc", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "'abc", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "'it''s", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "ls \"abc", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "[1, 2", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "(1 + 2", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "ls (ls", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "if $x { ls", NVO_ERROR_UNTERMINATED, 6},
    {SCRIPT, "$(ls", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "\"a $(ls", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "\"a ${b", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "0x[de ad", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "{ a: 1", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "\"a\\u{41", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "ls \"a\\u{41", NVO_ERROR_UNTERMINATED, 3},

    /* A source that ends where a bracket needs more is a bracket never
       closed, at the opening of the innermost one the parser is reading: a
       `(`, `[`, `{`, `$(`, `${`, a type's `<`, a parameter list's `(`. */
    {SCRIPT, "(", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "[1,", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "{a:", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "$(", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "(1 +", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "ls (a |", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "def f(", NVO_ERROR_UNTERMINATED, 5},
    {SCRIPT, "def f(a", NVO_ERROR_UNTERMINATED, 5},
    {SCRIPT, "def f(a:", NVO_ERROR_UNTERMINATED, 5},
    {SCRIPT, "def f(a: int =", NVO_ERROR_UNTERMINATED, 5},
    {SCRIPT, "def f(a: list<", NVO_ERROR_UNTERMINATED, 13},
    {SCRIPT, "let x: list<", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "let x: list<int", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "let x: record<a:", NVO_ERROR_UNTERMINATED, 13},
    {SCRIPT, "let x: path(", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "let x: path( ", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "let x: path(r", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "let x: path(re", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "let x: path(read", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "let x: path(w", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "let x: path(writ", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "let x: path(write ", NVO_ERROR_UNTERMINATED, 11},
    {SCRIPT, "def f(a: path(", NVO_ERROR_UNTERMINATED, 13},
    {SCRIPT, "def f(a: path(w", NVO_ERROR_UNTERMINATED, 13},
    {SCRIPT, "def f(a: path(read", NVO_ERROR_UNTERMINATED, 13},
    {SCRIPT, "def f(--x(", NVO_ERROR_UNTERMINATED, 9},
    {SCRIPT, "def f(--x(-", NVO_ERROR_UNTERMINATED, 9},
    {SCRIPT, "def f(--x(-x", NVO_ERROR_UNTERMINATED, 9},
    {SCRIPT, "def f(--x(-x)", NVO_ERROR_UNTERMINATED, 5},
    {SCRIPT, "def f(a: list<path(re", NVO_ERROR_UNTERMINATED, 18},
    {SCRIPT, "[(1 +", NVO_ERROR_UNTERMINATED, 1},
    {SCRIPT, "(a [1,", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "[[a]; [1,", NVO_ERROR_UNTERMINATED, 6},
    {SCRIPT, "[[a];", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "{ |a,", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "{ |", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "if $a { ls |", NVO_ERROR_UNTERMINATED, 6},
    {SCRIPT, "if $x { 1 +", NVO_ERROR_UNTERMINATED, 6},
    {SCRIPT, "\"a $(ls |", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "(\"abc", NVO_ERROR_UNTERMINATED, 1},
    {SCRIPT, "ls ${", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "ls ${a", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "ls ${a.", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "ls ${a.b.", NVO_ERROR_UNTERMINATED, 3},
    {SCRIPT, "\"${", NVO_ERROR_UNTERMINATED, 1},
    {SCRIPT, "\"${a.", NVO_ERROR_UNTERMINATED, 1},
    {SCRIPT, "(ls ${a.", NVO_ERROR_UNTERMINATED, 4},
    {SCRIPT, "ls ${a.\"${b.", NVO_ERROR_UNTERMINATED, 8},
    {SCRIPT, "(^", NVO_ERROR_UNTERMINATED, 0},
    {FILE_MODE, "needs {a:", NVO_ERROR_UNTERMINATED, 6},
    /* Bytes that cannot begin what a bracket is waiting for are an error
       where they are, not a bracket never closed. */
    {SCRIPT, "let x: path(x", NVO_ERROR_UNEXPECTED, 12},
    {SCRIPT, "let x: path(reada", NVO_ERROR_UNEXPECTED, 12},
    {SCRIPT, "let x: path(re ", NVO_ERROR_UNEXPECTED, 12},
    {SCRIPT, "let x: path(read)x", NVO_ERROR_UNEXPECTED, 17},
    {SCRIPT, "def f(--x(-1", NVO_ERROR_UNEXPECTED, 9},
    {SCRIPT, "def f(--x(-x ", NVO_ERROR_UNEXPECTED, 9},
    {SCRIPT, "def f(--x(a)", NVO_ERROR_UNEXPECTED, 9},
    {SCRIPT, "def f(--x(a", NVO_ERROR_UNEXPECTED, 9},
    /* A source cut inside a number or an operator is refused at the first
       byte the grammar has no place for, and a newline after the cut does
       not move it. */
    {SCRIPT, "[2.5e-", NVO_ERROR_UNEXPECTED, 4},
    {SCRIPT, "[2.5e-\n", NVO_ERROR_UNEXPECTED, 4},
    {SCRIPT, "[5K", NVO_ERROR_UNEXPECTED, 2},
    {SCRIPT, "(1 !", NVO_ERROR_UNEXPECTED, 3},
    /* A prefix operator is not a bracket, and with no bracket open the end
       of the source is where the token was missing. */
    {SCRIPT, "not", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "-", NVO_ERROR_UNEXPECTED, 1},
    {SCRIPT, "(not", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "[-", NVO_ERROR_UNTERMINATED, 0},
    {SCRIPT, "if", NVO_ERROR_UNEXPECTED, 2},
    {SCRIPT, "ls | (a)\n1 +", NVO_ERROR_UNEXPECTED, 12},

    /* `=>` is a mark, and a mark has no place at the head of a stage. */
    {SCRIPT, "=> x", NVO_ERROR_UNEXPECTED, 0},
    {SCRIPT, "ls | => x", NVO_ERROR_UNEXPECTED, 5},
    {SCRIPT, "x; => y", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "(=> x)", NVO_ERROR_UNEXPECTED, 1},
    {SCRIPT, "=>$a", NVO_ERROR_UNEXPECTED, 0},
    {SCRIPT, "=>", NVO_ERROR_UNEXPECTED, 0},
    {SCRIPT, "ls =>", NVO_ERROR_UNEXPECTED, 5},
    {SCRIPT, "ls =>{a}", NVO_ERROR_UNEXPECTED, 5},
    {SCRIPT, "ls => | wc", NVO_ERROR_UNEXPECTED, 6},

    /* The grammar's closures, rows and words: no comma after the last
       parameter or value, and no table in a word. */
    {SCRIPT, "{|a,| 1}", NVO_ERROR_UNEXPECTED, 4},
    {SCRIPT, "{ |a,b,| 1 }", NVO_ERROR_UNEXPECTED, 7},
    {SCRIPT, "[[a]; [1,]]", NVO_ERROR_UNEXPECTED, 9},
    {SCRIPT, "[[a]; [1], [2,]]", NVO_ERROR_UNEXPECTED, 14},
    {SCRIPT, "[[a]; [,]]", NVO_ERROR_UNEXPECTED, 7},
    {SCRIPT, "[[a,]; [1]]", NVO_ERROR_BARE_WORD, 2},
    {SCRIPT, "echo [[a]; [1]]", NVO_ERROR_BARE_WORD, 7},
    {SCRIPT, "echo [[1]; [2]]", NVO_ERROR_UNEXPECTED, 9},
    {SCRIPT, "ls x[[a]; [1]]", NVO_ERROR_BARE_WORD, 6},
    {SCRIPT, "ls -f[[a]; [1]]", NVO_ERROR_BARE_WORD, 7},

    /* A `#` where a word could begin is a comment, so a caret has no name. */
    {SCRIPT, "^#x", NVO_ERROR_UNEXPECTED, 1},
    {SCRIPT, "^ #x", NVO_ERROR_UNEXPECTED, 1},
    {SCRIPT, "^", NVO_ERROR_UNEXPECTED, 1},
    {SCRIPT, "ls | ^#x", NVO_ERROR_UNEXPECTED, 6},

    /* A `$` that no name follows is an error at the `$`, in every mode. */
    {SCRIPT, "1 + $5", NVO_ERROR_UNEXPECTED, 4},
    {SCRIPT, "ls $5", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "\"$5\"", NVO_ERROR_UNEXPECTED, 1},
    {SCRIPT, "ls ${5}", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "ls ${5", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "ls ${a.}", NVO_ERROR_UNEXPECTED, 7},
    {SCRIPT, "ls ${a.b.}", NVO_ERROR_UNEXPECTED, 9},
    {SCRIPT, "\"${a.}\"", NVO_ERROR_UNEXPECTED, 5},
    {SCRIPT, "\"${5}\"", NVO_ERROR_UNEXPECTED, 1},
    {SCRIPT, "ls \"a $\"", NVO_ERROR_UNEXPECTED, 6},
    {SCRIPT, "ls -f$5", NVO_ERROR_UNEXPECTED, 5},
    {SCRIPT, "ls => $5", NVO_ERROR_UNEXPECTED, 6},
    {SCRIPT, "$", NVO_ERROR_UNEXPECTED, 0},
    {SCRIPT, "ls $", NVO_ERROR_UNEXPECTED, 3},
    {SCRIPT, "(ls $", NVO_ERROR_UNEXPECTED, 4},
    {SCRIPT, "if $x { 1 } else if./x { 2 }", NVO_ERROR_UNEXPECTED, 17},

    /* L1: the escapes of a double-quoted string, and only those. */
    {SCRIPT, "\"a\\qb\"", NVO_ERROR_ESCAPE, 2},
    {SCRIPT, "\"\\u{110000}\"", NVO_ERROR_ESCAPE, 1},
    {SCRIPT, "\"\\u{D800}\"", NVO_ERROR_ESCAPE, 1},
    {SCRIPT, "\"\\u{DFFF}\"", NVO_ERROR_ESCAPE, 1},
    {SCRIPT, "\"\\u{}\"", NVO_ERROR_ESCAPE, 1},
    {SCRIPT, "\"\\u{41\"", NVO_ERROR_ESCAPE, 1},
    {SCRIPT, "\"\\u41\"", NVO_ERROR_ESCAPE, 1},
    {SCRIPT, "\"\\u{4g}\"", NVO_ERROR_ESCAPE, 1},
    {SCRIPT, "\"\\'\"", NVO_ERROR_ESCAPE, 1},
    {SCRIPT, "\"x\\\n\"", NVO_ERROR_ESCAPE, 2},
    {SCRIPT, "ls \"\\x\"", NVO_ERROR_ESCAPE, 4},

    /* L1 rule 5: a number that does not fit, and half a byte. */
    {SCRIPT, "9223372036854775808", NVO_ERROR_LITERAL, 0},
    {SCRIPT, "1 + 99999999999999999999", NVO_ERROR_LITERAL, 4},
    {SCRIPT, "0xffffffffffffffff", NVO_ERROR_LITERAL, 0},
    {SCRIPT, "1.5B", NVO_ERROR_LITERAL, 0},
    {SCRIPT, "0.0001kB", NVO_ERROR_LITERAL, 0},
    {SCRIPT, "1.5ns", NVO_ERROR_LITERAL, 0},
    {SCRIPT, "9223372036854775808B", NVO_ERROR_LITERAL, 0},
    {SCRIPT, "106752d", NVO_ERROR_LITERAL, 0},
    {SCRIPT, "8589934592GiB", NVO_ERROR_LITERAL, 0},
    {SCRIPT, "1e6145", NVO_ERROR_LITERAL, 0},
    {SCRIPT, "[1, 1.5B]", NVO_ERROR_LITERAL, 4},
    {SCRIPT, "0x[abc]", NVO_ERROR_LITERAL, 5},
    {SCRIPT, "0x[ab c]", NVO_ERROR_LITERAL, 6},
    {SCRIPT, "0x[a b]", NVO_ERROR_LITERAL, 3},
    {SCRIPT, "0x[zz]", NVO_ERROR_LITERAL, 3},
    {SCRIPT, "[1, 0x[a]]", NVO_ERROR_LITERAL, 7},
};

static void error_cases(void)
{
    unsigned i;

    for (i = 0; i < sizeof errors / sizeof errors[0]; i++) {
        const ERROR_CASE *e = &errors[i];
        NVO_STATUS status = parse_text(&case_a, e->mode, e->source);

        if (status != NVO_SYNTAX || case_a.tree.error != e->error ||
            case_a.tree.error_offset != e->offset) {
            printf("failed: [%s]\n  got  status %d error %d at %lu\n"
                   "  want error %d at %lu\n",
                   e->source, (int)status, (int)case_a.tree.error,
                   (unsigned long)case_a.tree.error_offset, (int)e->error,
                   (unsigned long)e->offset);
            failures++;
        }
    }
}

/* The first and last sequence of each length RFC 3629 allows, and the edges
   of the lead bytes whose second byte it narrows. */
static const char *const utf8_edges[] = {
    "\302\200",         "\337\277",         "\340\240\200",
    "\340\277\277",     "\341\200\200",     "\354\277\277",
    "\355\200\200",     "\355\237\277",     "\356\200\200",
    "\357\277\277",     "\360\220\200\200", "\360\277\277\277",
    "\361\200\200\200", "\363\277\277\277", "\364\200\200\200",
    "\364\217\277\277",
};

/* Each edge is UTF-8 and a word of a command, and its first bytes alone,
   with the rest of it in memory just past them, are not. */
static void utf8_edge_cases(void)
{
    unsigned i;

    for (i = 0; i < sizeof utf8_edges / sizeof utf8_edges[0]; i++) {
        const UINT8 *bytes = (const UINT8 *)utf8_edges[i];
        size_t length = strlen(utf8_edges[i]);
        char text[16];
        UINT64 offset = 99;

        check_source(nvo_utf8_check(bytes, length, &offset) == 1,
                     "a UTF-8 edge is accepted", utf8_edges[i]);
        check_source(nvo_utf8_check(bytes, length - 1, &offset) == 0 &&
                         offset == 0,
                     "a cut UTF-8 edge is refused", utf8_edges[i]);
        strcpy(text, "ls ");
        strcat(text, utf8_edges[i]);
        check_source(parse_text(&case_a, SCRIPT, text) == NVO_OK,
                     "a UTF-8 edge is a word", text);
    }
}

/* ------------------------------------------------------------------------
   Nesting */

/* The levels of one kind of nesting a source can have, at most: the
   brackets, blocks, groups, substitutions, types and prefix operators the
   parser recurses for, and each route to each. */
typedef struct {
    const char *head;  /* written once, before the levels */
    const char *open;  /* written once for each repetition */
    const char *body;  /* in the innermost */
    const char *close; /* written once for each repetition */
    const char *tail;  /* written once, after the levels */
    unsigned token;    /* where in open the first nesting token is */
    unsigned levels;   /* the levels one repetition of open nests */
    unsigned held;     /* the levels head nests that tail closes */
} NESTING;

static const NESTING nestings[] = {
    {"", "(", "1", ")", "", 0, 1, 0},
    {"", "[", "1", "]", "", 0, 1, 0},
    {"", "{ a: ", "1", " }", "", 0, 1, 0},
    {"", "if $x { ", "ls", " }", "", 6, 1, 0},
    {"", "{ |a| ", "ls", " }", "", 0, 1, 0},
    {"", "$(", "ls", ")", "", 0, 1, 0},
    {"ls ", "(", "ls", ")", "", 0, 1, 0},
    {"ls ", "[", "1", "]", "", 0, 1, 0},
    {"", "- ", "$x", "", "", 0, 1, 0},
    {"", "not ", "$x", "", "", 0, 1, 0},
    {"", "\"$(", "ls", ")\"", "", 1, 1, 0},
    {"let x: ", "list<", "int", ">", " = [ ]", 4, 1, 0},
    /* A braced variable holds a string member that holds one: in a string,
       in a word and in an expression. */
    {"\"", "${a.\"", "x", "\"}", "\"", 0, 1, 0},
    {"ls ", "${a.\"", "x", "\"}", "", 0, 1, 0},
    {"", "$a.\"$(", "ls", ")\"", "", 4, 1, 0},
    /* Each other place a word nests: a substitution, a spread, a flag, a
       block as an argument. */
    {"", "ls $(", "ls", ")", "", 3, 1, 0},
    {"", "ls ...(", "ls", ")", "", 6, 1, 0},
    {"", "ls -x(", "ls", ")", "", 5, 1, 0},
    {"", "ls { ", "ls", " }", "", 3, 1, 0},
    /* A table is a bracket and so is each row. */
    {"", "[[a]; [", "1", "]]", "", 0, 2, 0},
    /* A parameter list's parentheses, nested by closures in a default, and
       what a parameter holds, with the parentheses a level of its own. */
    {"", "def f(a: int = { |x| ", "ls", " }) { }", "", 5, 2, 0},
    {"def f(a: int = ", "[", "1", "]", ") { }", 0, 1, 1},
    {"def f(a: ", "list<", "int", ">", ") { }", 4, 1, 1},
    {"def f(a: ", "record<a: ", "int", ">", ") { }", 6, 1, 1},
    {"def f(a: int = ", "$(", "ls", ")", ") { }", 0, 1, 1},
};

/* Write count levels of n into text and return its length. */
static size_t nest(char *text, const NESTING *n, unsigned count)
{
    size_t length = strlen(n->head), open = strlen(n->open),
           close = strlen(n->close);
    unsigned i;

    memcpy(text, n->head, length);
    for (i = 0; i < count; i++) {
        memcpy(text + length, n->open, open);
        length += open;
    }
    memcpy(text + length, n->body, strlen(n->body));
    length += strlen(n->body);
    for (i = 0; i < count; i++) {
        memcpy(text + length, n->close, close);
        length += close;
    }
    memcpy(text + length, n->tail, strlen(n->tail));
    length += strlen(n->tail);
    text[length] = '\0';
    return length;
}

/* A nesting far past the limit, in repetitions: the parser stops at the first
   level over it and does not recurse to the end. */
#define NESTING_FAR 100

/* The bytes nest() writes for count repetitions, with the terminator. */
static size_t nesting_size(const NESTING *n, unsigned count)
{
    return strlen(n->head) +
           (size_t)count * (strlen(n->open) + strlen(n->close)) +
           strlen(n->body) + strlen(n->tail) + 1;
}

/* How many times in a row the deepest nesting is written to show that its
   levels are given back. */
#define NESTING_COPIES 3

/* A nesting parses to NVO_DEPTH_MAX levels, counting the levels its head
   holds, and so does the deepest one several times in a row, one repetition
   more is NVO_ERROR_DEPTH at the nesting token of the level that is one too
   many, and so is the same nesting far deeper. */
static void depth_cases(void)
{
    static char source[CASE_SOURCE_MAX], again[CASE_SOURCE_MAX];
    unsigned i;

    for (i = 0; i < sizeof nestings / sizeof nestings[0]; i++) {
        const NESTING *n = &nestings[i];
        unsigned fits = (NVO_DEPTH_MAX - n->held) / n->levels, k;
        unsigned too_many[2];
        size_t length;
        UINT64 want =
            strlen(n->head) + (UINT64)fits * strlen(n->open) + n->token;

        check(n->levels * fits + n->held == NVO_DEPTH_MAX,
              "a nesting fills the limit exactly");
        check(nesting_size(n, NESTING_FAR) <= sizeof source,
              "a nesting far past the limit fits its buffer");
        length = nest(source, n, fits);
        check_source(parse_text(&case_a, SCRIPT, source) == NVO_OK,
                     "the deepest nesting parses", source);
        check_source(round_trips(SCRIPT, source),
                     "the deepest nesting round-trips", source);
        check((length + 1) * NESTING_COPIES < sizeof again,
              "the deepest nesting repeated fits its buffer");
        again[0] = '\0';
        for (k = 0; k < NESTING_COPIES; k++) {
            strcat(again, source);
            strcat(again, "\n");
        }
        check_source(parse_text(&case_a, SCRIPT, again) == NVO_OK,
                     "the deepest nesting parses again and again", again);
        too_many[0] = fits + 1;
        too_many[1] = NESTING_FAR;
        for (k = 0; k < 2; k++) {
            unsigned count = too_many[k];
            NVO_STATUS status;

            nest(source, n, count);
            status = parse_text(&case_a, SCRIPT, source);
            if (status != NVO_SYNTAX || case_a.tree.error != NVO_ERROR_DEPTH ||
                case_a.tree.error_offset != want) {
                printf("failed: nesting %u with %u repetitions: status %d "
                       "error %d at %lu, want depth at %lu\n",
                       i, count, (int)status, (int)case_a.tree.error,
                       (unsigned long)case_a.tree.error_offset,
                       (unsigned long)want);
                failures++;
            }
        }
    }
}

/* Left-grouping operators and long pipelines nest nothing in the parser,
   so a source long in them parses, and prints and compares without a stack
   as deep as the tree. */
static void chain_cases(void)
{
    static char source[CASE_SOURCE_MAX];
    size_t length;

    length = (size_t)snprintf(source, sizeof source, "$x");
    while (length + 8 < sizeof source)
        length +=
            (size_t)snprintf(source + length, sizeof source - length, " + $x");
    check(parse_text(&case_a, SCRIPT, source) == NVO_OK, "a long sum parses");
    check(round_trips(SCRIPT, source), "a long sum round-trips");

    length = (size_t)snprintf(source, sizeof source, "ls");
    while (length + 8 < sizeof source)
        length +=
            (size_t)snprintf(source + length, sizeof source - length, " | ls");
    check(parse_text(&case_a, SCRIPT, source) == NVO_OK,
          "a long pipeline parses");
    check(round_trips(SCRIPT, source), "a long pipeline round-trips");

    length = (size_t)snprintf(source, sizeof source, "$x");
    while (length + 8 < sizeof source)
        length +=
            (size_t)snprintf(source + length, sizeof source - length, ".a?");
    check(parse_text(&case_a, SCRIPT, source) == NVO_OK,
          "a long member chain parses");
    check(round_trips(SCRIPT, source), "a long member chain round-trips");

    length = 0;
    while (length + 8 < sizeof source)
        length +=
            (size_t)snprintf(source + length, sizeof source - length, "ls;\n");
    check(parse_text(&case_a, SCRIPT, source) == NVO_OK,
          "a long script parses");
    check(round_trips(SCRIPT, source), "a long script round-trips");
}

/* ------------------------------------------------------------------------
   Limits of the call */

static void call_cases(void)
{
    static NVO_NODE small[2];
    static const UINT8 one[] = {'1'};
    NVO_TREE tree;

    check(nvo_parse(&tree, SCRIPT, (const UINT8 *)"1 + 2", 5, small, 2) ==
              NVO_MEMORY,
          "too few nodes is NVO_MEMORY");
    check(nvo_parse(&tree, SCRIPT, (const UINT8 *)"1", 1, small, 0) ==
              NVO_MEMORY,
          "no nodes is NVO_MEMORY");
    /* The source is not read: a length past the limit is refused first. */
    check(nvo_parse(&tree, SCRIPT, one, (UINT64)NVO_SOURCE_MAX + 1, small, 2) ==
              NVO_MEMORY,
          "a source past NVO_SOURCE_MAX is NVO_MEMORY");
    check(parse_text(&case_a, SCRIPT, "") == NVO_OK, "an empty script parses");
    check(parse_text(&case_a, FILE_MODE, "") == NVO_OK, "an empty file parses");
    check(parse_text(&case_a, SCRIPT, " \t\n ;\n# only a comment") == NVO_OK,
          "blanks and a comment parse");
}

/* The printer says how many bytes it needs when it has too few, and has no
   tree to print after a parse that failed. */
static void print_cases(void)
{
    static const UINT8 source[] = "1 + 2";
    static UINT8 out[16];
    UINT64 needed = 0, length = 0;

    check(parse_source(&case_a, SCRIPT, source, 5) == NVO_OK,
          "the source to print parses");
    check(nvo_print(&case_a.tree, source, out, sizeof out, &needed) == NVO_OK &&
              needed > 0,
          "a tree prints in room");
    check(nvo_print(&case_a.tree, source, out, needed - 1, &length) ==
                  NVO_MEMORY &&
              length == needed,
          "a tree that does not fit reports the room it needs");
    check(nvo_print(&case_a.tree, source, out, needed, &length) == NVO_OK &&
              length == needed,
          "a tree prints in exactly the room it needs");
    check(parse_text(&case_a, SCRIPT, ")") == NVO_SYNTAX,
          "the source that fails parses to an error");
    check(nvo_print(&case_a.tree, source, out, sizeof out, &length) ==
              NVO_INVALID,
          "a failed parse has no tree to print");
}

/* ------------------------------------------------------------------------
   Number literals */

typedef struct {
    const char *text;
    NVO_STATUS status;
    NVO_NUMBER_KIND kind; /* when NVO_OK */
    INT64 integer;        /* an integer, a size or a duration */
    const char *decimal;  /* a decimal, as nvo_decimal_to_text writes it */
} NUMBER_CASE;

#define INT_LARGEST 9223372036854775807LL

static const NUMBER_CASE numbers[] = {
    /* Integers: radixes, underscores, the 64-bit limit. */
    {"0", NVO_OK, NVO_NUMBER_INT, 0, 0},
    {"42", NVO_OK, NVO_NUMBER_INT, 42, 0},
    {"1_000", NVO_OK, NVO_NUMBER_INT, 1000, 0},
    {"1__0", NVO_OK, NVO_NUMBER_INT, 10, 0},
    {"1_", NVO_OK, NVO_NUMBER_INT, 1, 0},
    {"0x2a", NVO_OK, NVO_NUMBER_INT, 42, 0},
    {"0x2A", NVO_OK, NVO_NUMBER_INT, 42, 0},
    {"0xdead_beef", NVO_OK, NVO_NUMBER_INT, 0xdeadbeefLL, 0},
    {"0x2aB", NVO_OK, NVO_NUMBER_INT, 0x2ab, 0},
    {"0o52", NVO_OK, NVO_NUMBER_INT, 42, 0},
    {"0b101010", NVO_OK, NVO_NUMBER_INT, 42, 0},
    {"0b1111_1111", NVO_OK, NVO_NUMBER_INT, 255, 0},
    {"0007", NVO_OK, NVO_NUMBER_INT, 7, 0},
    {"9223372036854775807", NVO_OK, NVO_NUMBER_INT, INT_LARGEST, 0},
    {"9223372036854775808", NVO_OVERFLOW, NVO_NUMBER_INT, 0, 0},
    {"18446744073709551616", NVO_OVERFLOW, NVO_NUMBER_INT, 0, 0},
    {"0x7fffffffffffffff", NVO_OK, NVO_NUMBER_INT, INT_LARGEST, 0},
    {"0x8000000000000000", NVO_OVERFLOW, NVO_NUMBER_INT, 0, 0},
    {"0o777777777777777777777", NVO_OK, NVO_NUMBER_INT, INT_LARGEST, 0},
    {"0o1000000000000000000000", NVO_OVERFLOW, NVO_NUMBER_INT, 0, 0},
    {"0b0111111111111111111111111111111111111111111111111111111111111111",
     NVO_OK, NVO_NUMBER_INT, INT_LARGEST, 0},
    {"0b1000000000000000000000000000000000000000000000000000000000000000",
     NVO_OVERFLOW, NVO_NUMBER_INT, 0, 0},

    /* Decimals: exact digits, rounding half-even at 34, the range. */
    {"0.1", NVO_OK, NVO_NUMBER_DECIMAL, 0, "0.1"},
    {"2.5e-3", NVO_OK, NVO_NUMBER_DECIMAL, 0, "0.0025"},
    {"2.5E-3", NVO_OK, NVO_NUMBER_DECIMAL, 0, "0.0025"},
    {"1e3", NVO_OK, NVO_NUMBER_DECIMAL, 0, "1E+3"},
    {"1.5e+2", NVO_OK, NVO_NUMBER_DECIMAL, 0, "1.5E+2"},
    {"100.0", NVO_OK, NVO_NUMBER_DECIMAL, 0, "100.0"},
    {"1_0.5_5", NVO_OK, NVO_NUMBER_DECIMAL, 0, "10.55"},
    {"0.10", NVO_OK, NVO_NUMBER_DECIMAL, 0, "0.10"},
    {"0.0", NVO_OK, NVO_NUMBER_DECIMAL, 0, "0.0"},
    {"0.12345678901234567890123456789012345", NVO_OK, NVO_NUMBER_DECIMAL, 0,
     "0.1234567890123456789012345678901234"},
    {"0.12345678901234567890123456789012335", NVO_OK, NVO_NUMBER_DECIMAL, 0,
     "0.1234567890123456789012345678901234"},
    {"0.12345678901234567890123456789012346", NVO_OK, NVO_NUMBER_DECIMAL, 0,
     "0.1234567890123456789012345678901235"},
    {"0.123456789012345678901234567890123450000000001", NVO_OK,
     NVO_NUMBER_DECIMAL, 0, "0.1234567890123456789012345678901235"},
    {"1e6144", NVO_OK, NVO_NUMBER_DECIMAL, 0,
     "1.000000000000000000000000000000000E+6144"},
    {"1e6145", NVO_OVERFLOW, NVO_NUMBER_DECIMAL, 0, 0},
    {"1e-6176", NVO_OK, NVO_NUMBER_DECIMAL, 0, "1E-6176"},
    {"1e-6177", NVO_OK, NVO_NUMBER_DECIMAL, 0, "0E-6176"},

    /* Sizes: whole bytes, the 64-bit limit. */
    {"0B", NVO_OK, NVO_NUMBER_SIZE, 0, 0},
    {"512B", NVO_OK, NVO_NUMBER_SIZE, 512, 0},
    {"1kB", NVO_OK, NVO_NUMBER_SIZE, 1000, 0},
    {"4KiB", NVO_OK, NVO_NUMBER_SIZE, 4096, 0},
    {"1.5MB", NVO_OK, NVO_NUMBER_SIZE, 1500000, 0},
    {"1.5KiB", NVO_OK, NVO_NUMBER_SIZE, 1536, 0},
    {"0.5kB", NVO_OK, NVO_NUMBER_SIZE, 500, 0},
    {"0.001kB", NVO_OK, NVO_NUMBER_SIZE, 1, 0},
    {"0.0001kB", NVO_INVALID, NVO_NUMBER_SIZE, 0, 0},
    {"1.5B", NVO_INVALID, NVO_NUMBER_SIZE, 0, 0},
    {"1.0B", NVO_OK, NVO_NUMBER_SIZE, 1, 0},
    {"2.50KiB", NVO_OK, NVO_NUMBER_SIZE, 2560, 0},
    {"1GB", NVO_OK, NVO_NUMBER_SIZE, 1000000000LL, 0},
    {"1GiB", NVO_OK, NVO_NUMBER_SIZE, 1073741824LL, 0},
    {"1TB", NVO_OK, NVO_NUMBER_SIZE, 1000000000000LL, 0},
    {"1TiB", NVO_OK, NVO_NUMBER_SIZE, 1099511627776LL, 0},
    {"1_000B", NVO_OK, NVO_NUMBER_SIZE, 1000, 0},
    {"0.0000000000000000000000000000000000000001B", NVO_INVALID,
     NVO_NUMBER_SIZE, 0, 0},
    {"9223372036854775807B", NVO_OK, NVO_NUMBER_SIZE, INT_LARGEST, 0},
    {"9223372036854775808B", NVO_OVERFLOW, NVO_NUMBER_SIZE, 0, 0},
    {"8589934591GiB", NVO_OK, NVO_NUMBER_SIZE, 9223372035781033984LL, 0},
    {"8589934592GiB", NVO_OVERFLOW, NVO_NUMBER_SIZE, 0, 0},
    {"9223372036GB", NVO_OK, NVO_NUMBER_SIZE, 9223372036000000000LL, 0},
    {"9223372037GB", NVO_OVERFLOW, NVO_NUMBER_SIZE, 0, 0},

    /* Durations: nanoseconds, with d as 86,400 s. */
    {"1ns", NVO_OK, NVO_NUMBER_DURATION, 1, 0},
    {"1us", NVO_OK, NVO_NUMBER_DURATION, 1000, 0},
    {"250ms", NVO_OK, NVO_NUMBER_DURATION, 250000000LL, 0},
    {"1.5s", NVO_OK, NVO_NUMBER_DURATION, 1500000000LL, 0},
    {"2min", NVO_OK, NVO_NUMBER_DURATION, 120000000000LL, 0},
    {"3h", NVO_OK, NVO_NUMBER_DURATION, 10800000000000LL, 0},
    {"1d", NVO_OK, NVO_NUMBER_DURATION, 86400000000000LL, 0},
    {"0.5us", NVO_OK, NVO_NUMBER_DURATION, 500, 0},
    {"0.001us", NVO_OK, NVO_NUMBER_DURATION, 1, 0},
    {"1.5ns", NVO_INVALID, NVO_NUMBER_DURATION, 0, 0},
    {"0.0001us", NVO_INVALID, NVO_NUMBER_DURATION, 0, 0},
    {"0.5d", NVO_OK, NVO_NUMBER_DURATION, 43200000000000LL, 0},
    {"1.25min", NVO_OK, NVO_NUMBER_DURATION, 75000000000LL, 0},
    {"9223372036854775807ns", NVO_OK, NVO_NUMBER_DURATION, INT_LARGEST, 0},
    {"9223372036854775808ns", NVO_OVERFLOW, NVO_NUMBER_DURATION, 0, 0},
    {"106751d", NVO_OK, NVO_NUMBER_DURATION, 9223286400000000000LL, 0},
    {"106752d", NVO_OVERFLOW, NVO_NUMBER_DURATION, 0, 0},
    {"153722867min", NVO_OK, NVO_NUMBER_DURATION, 9223372020000000000LL, 0},
    {"153722868min", NVO_OVERFLOW, NVO_NUMBER_DURATION, 0, 0},

    /* Text that is not exactly one literal. */
    {"", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"x", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"-1", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"+1", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1x", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1 ", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1.", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {".5", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1.5.2", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1e", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1e+", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1s2", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1sec", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1KB", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1mb", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"1PiB", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"0x", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"0x_1", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"0xg", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"0o8", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"0b2", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"0X2a", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
    {"0x2a.5", NVO_SYNTAX, NVO_NUMBER_INT, 0, 0},
};

static void number_cases(void)
{
    static UINT8 padded[NVO_NUMBER_LITERAL_MAX + 16];
    unsigned i;
    size_t k;
    NVO_NUMBER number;

    for (i = 0; i < sizeof numbers / sizeof numbers[0]; i++) {
        const NUMBER_CASE *c = &numbers[i];
        UINT8 text[NVO_DECIMAL_TEXT_MAX];
        NVO_STATUS status;

        status =
            nvo_number_parse((const UINT8 *)c->text, strlen(c->text), &number);
        if (status != c->status) {
            printf("failed: number [%s]: status %d, want %d\n", c->text,
                   (int)status, (int)c->status);
            failures++;
            continue;
        }
        if (status != NVO_OK)
            continue;
        check_source(number.kind == c->kind, "a number's kind", c->text);
        if (c->kind == NVO_NUMBER_DECIMAL) {
            UINT32 n = nvo_decimal_to_text(&number.decimal, text);

            text[n] = '\0';
            if (strcmp((const char *)text, c->decimal) != 0) {
                printf("failed: number [%s]: decimal %s, want %s\n", c->text,
                       (const char *)text, c->decimal);
                failures++;
            }
        } else if (number.integer != c->integer) {
            printf("failed: number [%s]: %lld, want %lld\n", c->text,
                   (long long)number.integer, (long long)c->integer);
            failures++;
        }
    }

    /* A literal past NVO_NUMBER_LITERAL_MAX bytes is refused, not cut. */
    for (k = 0; k < sizeof padded; k++)
        padded[k] = '0';
    padded[1] = '.';
    check(nvo_number_parse(padded, NVO_NUMBER_LITERAL_MAX, &number) == NVO_OK,
          "a decimal of the longest length parses");
    check(nvo_number_parse(padded, NVO_NUMBER_LITERAL_MAX + 1, &number) ==
              NVO_INVALID,
          "a decimal past the longest length is refused");
    check(nvo_number_parse(padded, sizeof padded, &number) == NVO_INVALID,
          "a decimal far past the longest length is refused");
}

/* ------------------------------------------------------------------------
   Generated sources

   A source is built from L2's grammar, every choice drawn from a seeded
   generator, so that each is valid by construction: it must parse, and
   its tree must print to source that parses to the same tree. */

/* Past this many bytes only atoms are written, so that every construct
   still open can close inside CASE_SOURCE_MAX. */
#define GENERATED_SOFT_LIMIT 2048

/* The most levels of constructs inside constructs a generated source has;
   each adds at most a few of the parser's own levels, under
   NVO_DEPTH_MAX. */
#define GENERATED_DEPTH 4

#define GENERATED_SOURCES 3000

#define MUTATIONS_PER_SOURCE 8

typedef struct {
    char text[CASE_SOURCE_MAX];
    size_t length;
    UINT64 state;
    int overflowed;
    int inside; /* the innermost bracket makes a newline a blank */
    int touch;  /* the argument just written ends where the next may begin */
} GENERATOR;

static UINT64 next_random(GENERATOR *g)
{
    UINT64 x = g->state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g->state = x;
    return x * 0x2545f4914f6cdd1dULL;
}

static unsigned below(GENERATOR *g, unsigned n)
{
    return (unsigned)((next_random(g) >> 33) % n);
}

static void emit(GENERATOR *g, const char *text)
{
    size_t n = strlen(text);

    if (g->length + n >= sizeof g->text) {
        g->overflowed = 1;
        return;
    }
    memcpy(g->text + g->length, text, n);
    g->length += n;
    g->text[g->length] = '\0';
}

#define PICK(g, list) ((list)[below((g), sizeof(list) / sizeof((list)[0]))])

static int roomy(const GENERATOR *g)
{
    return g->length < GENERATED_SOFT_LIMIT;
}

/* A blank between two tokens: more than a space, and a newline where the
   innermost bracket makes it a blank. */
static void gap(GENERATOR *g)
{
    static const char *const plain[] = {" ", " ", " ", "  ", "\t"};
    static const char *const bracketed[] = {" ",    " ",      "\n",
                                            "\n  ", " # c\n", "\r\n"};

    emit(g, g->inside ? PICK(g, bracketed) : PICK(g, plain));
}

/* A blank that may be absent. */
static void maybe_gap(GENERATOR *g)
{
    if (below(g, 2) == 0)
        gap(g);
}

/* A comma, with a blank before it or not: an expression's path ends at
   one. */
static void comma(GENERATOR *g)
{
    maybe_gap(g);
    emit(g, ",");
}

static void gen_expr(GENERATOR *g, unsigned d);
static void gen_pipeline(GENERATOR *g, unsigned d);
static void gen_script(GENERATOR *g, unsigned d);

static const char *const atoms[] = {
    "0",
    "42",
    "1_000",
    "0x2a",
    "0o52",
    "0b101",
    "0.1",
    "2.5e-3",
    "1e3",
    "512B",
    "4KiB",
    "1.5MB",
    "250ms",
    "1.5s",
    "2min",
    "true",
    "false",
    "nothing",
    "$a",
    "$b_1",
    "$a.b",
    "$a.0",
    "$a.\"x y\"",
    "$a.b?",
    "\"\"",
    "\"text\"",
    "\"tab\\tnl\\n \\u{1F600} \\\" \\\\ \\$ \303\251\"",
    "'raw $ \\ ''q'''",
    "''",
    "\"a $a b ${a}c $a.b\"",
    "0x[]",
    "0x[de ad  be]",
    "0x[ ]",
    "/",
    ".",
    "..",
    "~",
    "./x",
    "../x/y",
    "~/z",
    "/usr/bin",
    "./*.md",
    "~/s/**/*.c",
    "./a?b",
    "./\"a,b\"/c",
    "./\"a b\"x",
};

static const char *const names[] = {"a", "b", "c_1", "_x", "name"};

static const char *const keys[] = {"a", "b", "\"q r\"", "'k'", "c_1", "\"\""};

static void gen_atom(GENERATOR *g) { emit(g, PICK(g, atoms)); }

static const char *const comparisons[] = {
    "==", "!=", "<", "<=", ">", ">=", "in", "not-in"};

static void gen_list(GENERATOR *g, unsigned d)
{
    unsigned count = below(g, 4), i;
    int saved = g->inside;

    emit(g, "[");
    g->inside = 1;
    for (i = 0; i < count; i++) {
        maybe_gap(g);
        if (below(g, 6) == 0)
            emit(g, "...");
        gen_expr(g, d - 1);
        if (i + 1 < count || below(g, 4) == 0)
            comma(g);
    }
    maybe_gap(g);
    g->inside = saved;
    emit(g, "]");
}

static void gen_record(GENERATOR *g, unsigned d)
{
    unsigned count = below(g, 4), i;
    int saved = g->inside;

    emit(g, "{");
    g->inside = 1;
    for (i = 0; i < count; i++) {
        maybe_gap(g);
        if (below(g, 6) == 0) {
            emit(g, "...");
            gen_expr(g, d - 1);
        } else {
            emit(g, PICK(g, keys));
            emit(g, ": ");
            gen_expr(g, d - 1);
        }
        if (i + 1 < count || below(g, 4) == 0)
            comma(g);
    }
    maybe_gap(g);
    g->inside = saved;
    emit(g, "}");
}

static void gen_table(GENERATOR *g, unsigned d)
{
    unsigned columns = 1 + below(g, 3), rows = 1 + below(g, 3), r, c;
    int saved = g->inside;

    emit(g, "[[");
    g->inside = 1;
    for (c = 0; c < columns; c++) {
        if (c > 0)
            emit(g, ", ");
        emit(g, PICK(g, names));
    }
    emit(g, "];");
    for (r = 0; r < rows; r++) {
        gap(g);
        emit(g, "[");
        for (c = 0; c < columns; c++) {
            if (c > 0) {
                comma(g);
                gap(g);
            }
            gen_expr(g, d - 1);
        }
        emit(g, "]");
        if (r + 1 < rows || below(g, 4) == 0)
            comma(g);
    }
    maybe_gap(g);
    g->inside = saved;
    emit(g, "]");
}

static void gen_closure(GENERATOR *g, unsigned d)
{
    unsigned count = below(g, 3), i;
    int saved = g->inside;

    emit(g, "{ |");
    for (i = 0; i < count; i++) {
        if (i > 0)
            emit(g, ", ");
        emit(g, PICK(g, names));
    }
    emit(g, "|");
    g->inside = 0;
    gap(g);
    gen_script(g, d - 1);
    gap(g);
    g->inside = saved;
    emit(g, "}");
}

static void gen_group(GENERATOR *g, unsigned d, const char *open)
{
    int saved = g->inside;

    emit(g, open);
    g->inside = 1;
    maybe_gap(g);
    gen_pipeline(g, d - 1);
    maybe_gap(g);
    g->inside = saved;
    emit(g, ")");
}

/* Members of a value, which an expression may write with a `?`, and the
   target of a set may not. */
static void gen_members(GENERATOR *g, int optional)
{
    static const char *const members[] = {".a", ".b_1", ".0", ".\"q r\""};
    unsigned count = below(g, 3), i;

    for (i = 0; i < count; i++) {
        emit(g, PICK(g, members));
        if (optional && below(g, 4) == 0)
            emit(g, "?");
    }
}

/* A primary, with the members that may touch it. */
static void gen_postfix(GENERATOR *g, unsigned d)
{
    if (d == 0 || !roomy(g)) {
        gen_atom(g);
        return;
    }
    switch (below(g, 14)) {
    case 0:
        gen_group(g, d, "(");
        gen_members(g, 1);
        break;
    case 1:
        gen_group(g, d, "$(");
        break;
    case 2:
        gen_list(g, d);
        gen_members(g, 1);
        break;
    case 3:
        gen_record(g, d);
        gen_members(g, 1);
        break;
    case 4:
        gen_table(g, d);
        break;
    case 5:
        gen_closure(g, d);
        break;
    case 6:
        emit(g, "\"a ");
        gen_group(g, d, "$(");
        emit(g, " b\"");
        break;
    case 7:
        emit(g, "$a");
        gen_members(g, 1);
        break;
    default:
        gen_atom(g);
        break;
    }
}

static void gen_unary(GENERATOR *g, unsigned d)
{
    if (roomy(g) && below(g, 6) == 0)
        emit(g, below(g, 2) == 0 ? "-" : "- ");
    gen_postfix(g, d);
}

/* One level of the binary operators: operand, then operator and operand
   some more times. */
static void gen_level(GENERATOR *g, unsigned d, const char *const *operators,
                      unsigned count, void (*operand)(GENERATOR *, unsigned))
{
    unsigned more = roomy(g) && below(g, 4) == 0 ? 1 + below(g, 2) : 0, i;

    operand(g, d);
    for (i = 0; i < more; i++) {
        gap(g);
        emit(g, operators[below(g, count)]);
        gap(g);
        operand(g, d);
    }
}

static void gen_product(GENERATOR *g, unsigned d)
{
    static const char *const operators[] = {"*", "/", "//", "%"};

    gen_level(g, d, operators, 4, gen_unary);
}

static void gen_sum(GENERATOR *g, unsigned d)
{
    static const char *const operators[] = {"+", "-"};

    gen_level(g, d, operators, 2, gen_product);
}

static void gen_concat(GENERATOR *g, unsigned d)
{
    static const char *const operators[] = {"++"};

    gen_level(g, d, operators, 1, gen_sum);
}

static void gen_compare(GENERATOR *g, unsigned d)
{
    gen_concat(g, d);
    if (roomy(g) && below(g, 3) == 0) {
        gap(g);
        emit(g, PICK(g, comparisons));
        gap(g);
        gen_concat(g, d);
    }
}

static void gen_not(GENERATOR *g, unsigned d)
{
    unsigned count = roomy(g) && below(g, 5) == 0 ? 1 + below(g, 2) : 0, i;

    for (i = 0; i < count; i++)
        emit(g, "not ");
    gen_compare(g, d);
}

static void gen_and(GENERATOR *g, unsigned d)
{
    static const char *const operators[] = {"and"};

    gen_level(g, d, operators, 1, gen_not);
}

static void gen_expr(GENERATOR *g, unsigned d)
{
    static const char *const operators[] = {"or"};

    gen_level(g, d, operators, 1, gen_and);
}

/* A word of a command: parts that touch. */
static void gen_word(GENERATOR *g)
{
    static const char *const parts[] = {
        "abc",     "a.b",    "x=1",      "a#b",   "42",      "7z",
        "./x",     "/tmp",   "~/d",      "./*.c", "\"s t\"", "'r''s'",
        "$a",      "$a.b",   "${a}",     "$a?",   "-1x",     "$(ls)",
        "(ls)",    "[1, 2]", "\303\251", "a,b",   ",x",      "x,",
        "./a,b",   "./*,?",  "=>x",      "=>$a",  "=>\"s\"", "=>(ls)",
        "\"s\"#c", "$a#c",   "[1]#c",
    };
    unsigned count = 1 + (roomy(g) ? below(g, 3) : 0), i;

    for (i = 0; i < count; i++) {
        const char *part = PICK(g, parts);

        /* A word that begins with a hyphen is a flag. */
        emit(g, i == 0 && part[0] == '-' ? "abc" : part);
    }
}

static void gen_block(GENERATOR *g, unsigned d)
{
    int saved = g->inside;

    emit(g, "{");
    g->inside = 0;
    if (below(g, 4) != 0) {
        gap(g);
        gen_script(g, d - 1);
        gap(g);
    }
    g->inside = saved;
    emit(g, "}");
}

static void gen_argument(GENERATOR *g, unsigned d)
{
    static const char *const flags[] = {
        "-l",   "-lah", "--long",  "--out=x",     "--out=\"a b\"",
        "-o$a", "--",   "--x-y=1", "-I./include", "-f\"a\"#c"};
    static const char *const spreads[] = {"...$a", "...$a.b", "...(ls)"};

    /* A block or a closure ends where the next argument may begin. */
    if (!g->touch || below(g, 3) != 0)
        gap(g);
    g->touch = 0;
    switch (roomy(g) && d > 0 ? below(g, 9) : below(g, 6)) {
    case 0:
    case 1:
        emit(g, PICK(g, flags));
        break;
    case 2:
        emit(g, PICK(g, spreads));
        break;
    case 3:
        emit(g, "=> ");
        emit(g, below(g, 2) == 0 ? "./x" : PICK(g, spreads));
        break;
    case 4:
    case 5:
        gen_word(g);
        break;
    case 6:
        gen_block(g, d);
        g->touch = 1;
        break;
    case 7:
        gen_closure(g, d);
        g->touch = 1;
        break;
    default:
        emit(g, "$a");
        break;
    }
}

static void gen_command(GENERATOR *g, unsigned d)
{
    /* The names that begin with `else` or `catch` are commands, whatever
       statement comes before them. */
    static const char *const heads[] = {
        "ls",      "git",     "7z",     "a.b",    "x-y",    "z_1",
        "^ls",     "^42",     "^if",    "^true",  "^=>",    "=>x",
        "else!",   "else,y",  "else.y", "else*",  "else@",  "catch!",
        "catch,y", "catch.y", "catch@", "else-x", "catch-x"};
    unsigned count = roomy(g) ? below(g, 4) : 0, i;

    emit(g, PICK(g, heads));
    g->touch = 0;
    for (i = 0; i < count; i++)
        gen_argument(g, d);
}

static void gen_stage(GENERATOR *g, unsigned d)
{
    if (below(g, 2) == 0)
        gen_command(g, d);
    else
        gen_expr(g, d);
}

static void gen_pipeline(GENERATOR *g, unsigned d)
{
    unsigned more = roomy(g) ? below(g, 3) : 0, i;

    gen_stage(g, d);
    for (i = 0; i < more; i++) {
        gap(g);
        emit(g, below(g, 4) == 0 ? "|\n" : "| ");
        gen_stage(g, d);
    }
}

static void gen_type(GENERATOR *g, unsigned d)
{
    static const char *const simple[] = {
        "any",        "nothing",     "bool",  "int",      "decimal",
        "number",     "string",      "bytes", "duration", "size",
        "time",       "closure",     "error", "glob",     "path",
        "path(read)", "path(write)", "list",  "record",   "table",
    };
    unsigned count, i;

    if (d == 0 || !roomy(g) || below(g, 3) != 0) {
        emit(g, PICK(g, simple));
        return;
    }
    switch (below(g, 4)) {
    case 0:
        emit(g, "list<");
        gen_type(g, d - 1);
        emit(g, ">");
        break;
    case 1:
        emit(g, "stream<");
        gen_type(g, d - 1);
        emit(g, ">");
        break;
    default:
        emit(g, below(g, 2) == 0 ? "record<" : "table<");
        count = 1 + below(g, 3);
        for (i = 0; i < count; i++) {
            if (i > 0)
                emit(g, ", ");
            emit(g, PICK(g, keys));
            emit(g, ": ");
            gen_type(g, d - 1);
        }
        emit(g, ">");
        break;
    }
}

static void gen_parameter(GENERATOR *g, unsigned d)
{
    switch (below(g, 6)) {
    case 0:
        emit(g, "--long-name");
        break;
    case 1:
        emit(g, "--f(-f): ");
        gen_type(g, d);
        emit(g, " = 3");
        break;
    case 2:
        emit(g, "--g = true");
        break;
    case 3:
        emit(g, PICK(g, names));
        emit(g, "?: ");
        gen_type(g, d);
        break;
    case 4:
        emit(g, PICK(g, names));
        emit(g, ": ");
        gen_type(g, d);
        emit(g, " = ");
        gen_expr(g, 0);
        break;
    default:
        emit(g, "...");
        emit(g, PICK(g, names));
        emit(g, ": ");
        gen_type(g, d);
        break;
    }
}

static void gen_definition(GENERATOR *g, unsigned d)
{
    unsigned count = below(g, 4), i;
    int saved = g->inside;

    emit(g, below(g, 3) == 0 ? "export def " : "def ");
    emit(g, PICK(g, names));
    emit(g, below(g, 2) == 0 ? "-two" : "");
    emit(g, "(");
    g->inside = 1;
    for (i = 0; i < count; i++) {
        maybe_gap(g);
        gen_parameter(g, d - 1);
        if (i + 1 < count || below(g, 4) == 0)
            comma(g);
    }
    maybe_gap(g);
    g->inside = saved;
    emit(g, ")");
    if (below(g, 2) == 0) {
        emit(g, " -> ");
        gen_type(g, d - 1);
    }
    gap(g);
    gen_block(g, d);
}

/* The blank before an else or a catch: on the same line, or the next. */
static void gen_continuation(GENERATOR *g)
{
    static const char *const joins[] = {" ", "  ", "\n", "\r\n", " # c\n"};

    emit(g, PICK(g, joins));
}

static void gen_if(GENERATOR *g, unsigned d)
{
    unsigned elifs = below(g, 3), i;

    emit(g, "if ");
    gen_expr(g, d - 1);
    gap(g);
    gen_block(g, d);
    for (i = 0; i < elifs; i++) {
        gen_continuation(g);
        emit(g, "else if ");
        gen_expr(g, d - 1);
        gap(g);
        gen_block(g, d);
    }
    if (below(g, 2) == 0) {
        gen_continuation(g);
        emit(g, "else ");
        gen_block(g, d);
    }
}

static void gen_statement(GENERATOR *g, unsigned d)
{
    static const char *const set_operators[] = {"=", "+=", "-=", "++="};
    static const char *const paths[] = {"./lib.nvo", "/usr/lib/x.nvo", "~/x",
                                        "../y"};

    if (d == 0 || !roomy(g)) {
        gen_stage(g, 0);
        return;
    }
    switch (below(g, 16)) {
    case 0:
        emit(g, below(g, 3) == 0 ? "export let " : "let ");
        emit(g, PICK(g, names));
        if (below(g, 2) == 0) {
            emit(g, ": ");
            gen_type(g, d - 1);
        }
        emit(g, " = ");
        gen_expr(g, d - 1);
        break;
    case 1:
        emit(g, "var ");
        emit(g, PICK(g, names));
        if (below(g, 2) == 0) {
            emit(g, ": ");
            gen_type(g, d - 1);
        }
        emit(g, " = ");
        gen_expr(g, d - 1);
        break;
    case 2:
        emit(g, "set ");
        emit(g, PICK(g, names));
        gen_members(g, 0);
        emit(g, " ");
        emit(g, PICK(g, set_operators));
        emit(g, " ");
        gen_expr(g, d - 1);
        break;
    case 3:
        gen_definition(g, d);
        break;
    case 4:
        emit(g, "use ");
        emit(g, PICK(g, paths));
        if (below(g, 2) == 0) {
            emit(g, " as ");
            emit(g, PICK(g, names));
        }
        break;
    case 5:
    case 6:
        gen_if(g, d);
        break;
    case 7:
        emit(g, "for ");
        emit(g, PICK(g, names));
        emit(g, " in ");
        gen_expr(g, d - 1);
        gap(g);
        gen_block(g, d);
        break;
    case 8:
        emit(g, "while ");
        gen_expr(g, d - 1);
        gap(g);
        gen_block(g, d);
        break;
    case 9:
        emit(g, "try ");
        gen_block(g, d);
        if (below(g, 2) == 0) {
            gen_continuation(g);
            emit(g, "catch ");
            if (below(g, 2) == 0) {
                emit(g, PICK(g, names));
                emit(g, " ");
            }
            gen_block(g, d);
        }
        break;
    case 10:
        emit(g, below(g, 2) == 0 ? "break" : "continue");
        break;
    case 11:
        emit(g, "return");
        if (below(g, 2) == 0) {
            emit(g, " ");
            gen_expr(g, d - 1);
        }
        break;
    default:
        gen_pipeline(g, d - 1);
        break;
    }
}

/* What ends a statement: a newline, a semicolon, a comment's line. */
static void gen_separator(GENERATOR *g)
{
    static const char *const separators[] = {
        "\n", "\n\n", ";", "; ", " ;\n", "\r\n", "  # c\n", "\n# c\n"};

    emit(g, PICK(g, separators));
}

static void gen_script(GENERATOR *g, unsigned d)
{
    unsigned count = below(g, 4), i;

    if (below(g, 4) == 0)
        gen_separator(g);
    for (i = 0; i < count; i++) {
        if (i > 0)
            gen_separator(g);
        gen_statement(g, d);
    }
    if (below(g, 4) == 0)
        gen_separator(g);
}

/* A source for seed: a script, or a file that begins with `needs`. */
static NVO_PARSE_MODE generate(GENERATOR *g, UINT64 seed)
{
    NVO_PARSE_MODE mode = NVO_PARSE_SCRIPT;

    g->state = seed * 0x9e3779b97f4a7c15ULL + 1;
    g->length = 0;
    g->text[0] = '\0';
    g->overflowed = 0;
    g->inside = 0;
    g->touch = 0;
    if (below(g, 4) == 0) {
        mode = NVO_PARSE_FILE;
        emit(g, "needs ");
        gen_record(g, 1);
        emit(g, "\n");
    } else if (below(g, 2) == 0) {
        mode = NVO_PARSE_FILE;
    }
    gen_script(g, GENERATED_DEPTH);
    return mode;
}

/* The source around offset, a window of it, for a failure to name. */
static void show_near(const char *text, size_t length, UINT64 offset)
{
    size_t from = offset > 60 ? (size_t)offset - 60 : 0;
    size_t to = offset + 30 < length ? (size_t)offset + 30 : length;

    printf("  near [%.*s] (error at %lu of that)\n", (int)(to - from),
           text + from, (unsigned long)(offset - from));
}

static void coverage_cases(void)
{
    unsigned kind;

    for (kind = 1; kind < NVO_NODE_KINDS; kind++)
        if (!kinds_seen[kind]) {
            printf("failed: no source makes a %s node\n", kind_names[kind]);
            failures++;
        }
}

static void generated_cases(void)
{
    static GENERATOR g;
    UINT64 seed;

    for (seed = 1; seed <= GENERATED_SOURCES; seed++) {
        NVO_PARSE_MODE mode = generate(&g, seed);

        if (g.overflowed) {
            printf("failed: the generator outgrew its buffer at seed %lu\n",
                   (unsigned long)seed);
            failures++;
            continue;
        }
        if (parse_text(&case_a, mode, g.text) != NVO_OK) {
            printf("failed: seed %lu does not parse: error %d at %lu\n",
                   (unsigned long)seed, (int)case_a.tree.error,
                   (unsigned long)case_a.tree.error_offset);
            show_near(g.text, g.length, case_a.tree.error_offset);
            failures++;
            continue;
        }
        mark_kinds(&case_a.tree);
        if (!round_trips(mode, g.text)) {
            printf("failed: seed %lu does not round-trip\n  [%s]\n",
                   (unsigned long)seed, g.text);
            failures++;
        }
    }
}

/* ------------------------------------------------------------------------
   Mutated sources

   Valid sources with a byte deleted, changed, inserted, swapped, a slice
   repeated or the end cut: whatever comes of them, the parser must not
   read outside the source or the nodes, must stop with a verdict, must
   place an error inside the source, and when it accepts must give a tree
   that prints and parses again to itself. */

static void mutate(GENERATOR *g, UINT8 *text, size_t *length)
{
    static const UINT8 bytes[] =
        "'\"$(){}[]|;\\#=<>-+*/%.,:?!@ \t\n\r0aZ_\200\303\377";
    size_t n = *length, at, other, slice;
    UINT8 held;

    if (n == 0)
        return;
    at = (size_t)below(g, (unsigned)n);
    switch (below(g, 6)) {
    case 0:
        memmove(text + at, text + at + 1, n - at - 1);
        *length = n - 1;
        break;
    case 1:
        text[at] = bytes[below(g, sizeof bytes - 1)];
        break;
    case 2:
        if (n + 1 > CASE_SOURCE_MAX)
            break;
        memmove(text + at + 1, text + at, n - at);
        text[at] = bytes[below(g, sizeof bytes - 1)];
        *length = n + 1;
        break;
    case 3:
        other = (size_t)below(g, (unsigned)n);
        held = text[at];
        text[at] = text[other];
        text[other] = held;
        break;
    case 4:
        slice = 1 + (size_t)below(g, 16);
        if (at + slice > n || n + slice > CASE_SOURCE_MAX)
            break;
        memmove(text + at + slice, text + at, n - at);
        *length = n + slice;
        break;
    default:
        *length = at;
        break;
    }
}

static void mutated_cases(void)
{
    static GENERATOR g;
    static UINT8 text[CASE_SOURCE_MAX + 1];
    UINT64 seed, accepted = 0, refused = 0;
    unsigned errors_seen[NVO_ERROR_DEPTH + 1] = {0};
    unsigned e;

    for (seed = 1; seed <= GENERATED_SOURCES; seed++) {
        NVO_PARSE_MODE mode = generate(&g, seed);
        unsigned m;

        for (m = 0; m < MUTATIONS_PER_SOURCE; m++) {
            size_t length = g.length;
            unsigned changes = 1 + below(&g, 3), c;
            UINT64 offset = 0;
            NVO_STATUS status;

            memcpy(text, g.text, length);
            for (c = 0; c < changes; c++)
                mutate(&g, text, &length);
            status = parse_source(&case_a, mode, text, length);
            if (status == NVO_OK) {
                accepted++;
                if (!round_trips_bytes(mode, text, length)) {
                    printf("failed: a mutation of seed %lu is accepted but "
                           "does not round-trip\n  [%.*s]\n",
                           (unsigned long)seed, (int)length,
                           (const char *)text);
                    failures++;
                }
                continue;
            }
            refused++;
            if (case_a.tree.error <= NVO_ERROR_DEPTH)
                errors_seen[case_a.tree.error]++;
            if (status != NVO_SYNTAX || case_a.tree.error == NVO_ERROR_NONE ||
                case_a.tree.error_offset > length ||
                (nvo_utf8_check(text, length, &offset) !=
                 (case_a.tree.error != NVO_ERROR_UTF8)) ||
                (case_a.tree.error == NVO_ERROR_UTF8 &&
                 case_a.tree.error_offset != offset)) {
                printf("failed: a mutation of seed %lu: status %d error %d at "
                       "%lu of %lu\n  [%.*s]\n",
                       (unsigned long)seed, (int)status, (int)case_a.tree.error,
                       (unsigned long)case_a.tree.error_offset,
                       (unsigned long)length, (int)length, (const char *)text);
                failures++;
            }
        }
    }
    check(accepted > 0 && refused > 0,
          "mutations are both accepted and refused");
    /* Nesting past the limit is not reached by a byte change; the nesting
       cases above make it. */
    for (e = NVO_ERROR_UTF8; e < NVO_ERROR_DEPTH; e++)
        check(errors_seen[e] > 0, "mutations reach each kind of error");
}

/* ------------------------------------------------------------------------
   Every short source of tokens

   The sources above are what a grammar says is valid, and what a byte
   changed makes of it. This is neither: every sequence of up to
   SWEEP_LENGTH tokens from a vocabulary of words, marks, brackets,
   keywords and blanks, so that every way two tokens can touch is read,
   including the ways no one wrote down. Each is parsed. One that parses must
   print, and the printing must parse to the same tree and print the same again;
   one that does not must say why at an offset inside the source and at the
   start of a character. */

#define SWEEP_LENGTH 4

/* The most failures the sweep prints; each is still counted. */
#define SWEEP_REPORTS 20

/* The longest token below, so that a sequence fits its buffer. */
#define SWEEP_TOKEN_MAX 12

static const char *const sweep_tokens[] = {
    " ", "\n", "\n\n", "a",  "else", "catch",    "if a { }", "try { }", "{",
    "}", "(",  ")",    "[",  "]",    "$(",       "${",       "$a",      "\"",
    "'", "|",  ";",    "=>", "...",  "^",        ".",        ",",       "#",
    "1", "-",  "+",    "!",  "?",    "\303\251", "def",
};

#define SWEEP_TOKENS (sizeof sweep_tokens / sizeof sweep_tokens[0])

/* Print the text with its newlines and blanks readable. */
static void show_source(const char *text, size_t length)
{
    size_t i;

    putchar('[');
    for (i = 0; i < length; i++) {
        if (text[i] == '\n')
            printf("\\n");
        else
            putchar(text[i]);
    }
    putchar(']');
}

/* One failure of the sweep: the source, and what was wrong with it. */
static void sweep_failure(unsigned *reported, const char *text, size_t length,
                          const char *what)
{
    if ((*reported)++ < SWEEP_REPORTS) {
        printf("failed: sweep: %s: ", what);
        show_source(text, length);
        putchar('\n');
    }
    failures++;
}

static void sweep_source(unsigned *reported, UINT64 *accepted, const char *text,
                         size_t length)
{
    NVO_STATUS status =
        parse_source(&case_a, SCRIPT, (const UINT8 *)text, length);
    UINT64 offset = case_a.tree.error_offset;

    if (status == NVO_OK) {
        (*accepted)++;
        if (!round_trips_bytes(SCRIPT, (const UINT8 *)text, length))
            sweep_failure(reported, text, length,
                          "parses but does not round-trip");
        return;
    }
    if (status != NVO_SYNTAX || case_a.tree.error == NVO_ERROR_NONE ||
        offset > length ||
        (offset < length && ((UINT8)text[offset] & 0xc0u) == 0x80u))
        sweep_failure(reported, text, length,
                      "refused with an error that is not placed in the source");
}

static void sweep_cases(void)
{
    static char text[SWEEP_LENGTH * SWEEP_TOKEN_MAX + 1];
    unsigned reported = 0, length, k;
    UINT64 sources = 0, accepted = 0, expected = 0, power = 1;

    for (k = 0; k < SWEEP_TOKENS; k++)
        check(strlen(sweep_tokens[k]) <= SWEEP_TOKEN_MAX,
              "a sweep token fits the sequence buffer");
    for (length = 1; length <= SWEEP_LENGTH; length++) {
        unsigned choice[SWEEP_LENGTH] = {0};

        power *= SWEEP_TOKENS;
        expected += power;
        for (;;) {
            size_t size = 0;

            for (k = 0; k < length; k++) {
                size_t token = strlen(sweep_tokens[choice[k]]);

                memcpy(text + size, sweep_tokens[choice[k]], token);
                size += token;
            }
            text[size] = '\0';
            sweep_source(&reported, &accepted, text, size);
            sources++;
            for (k = length; k > 0 && ++choice[k - 1] == SWEEP_TOKENS; k--)
                choice[k - 1] = 0;
            if (k == 0)
                break;
        }
    }
    printf("sweep: %lu sources of up to %d tokens, %lu parse\n",
           (unsigned long)sources, SWEEP_LENGTH, (unsigned long)accepted);
    check(sources == expected, "the sweep reads every sequence once");
    check(accepted > 0 && accepted < sources,
          "the sweep's sources are both accepted and refused");
}

/* ------------------------------------------------------------------------
   Every prefix of a source

   A valid source cut where a token ends, inside a bracket or a string, is
   one never closed: its error is unterminated, at the opening of the
   innermost one, which the bytes show without the parser. Cut outside every
   bracket the source is accepted or the error is at the cut. The sources
   hold every kind of bracket but a table literal, whose first bytes are
   those of a list, and no escape in a string, which the count below does
   not read. */

static const char *const prefix_sources[] = {
    "let x: path(read) = 1",
    "def f(a: path(write), --v(-v): bool = true, ...r: list<string>) { ls }",
    "def f(--x(-x): int = (1 + 2)) { }",
    "def f(a: record<k: int, n: list<string>> = {k: 1, n: []}) { }",
    "def f(a: list<path(read)> = [], --b: path(write)) { ls --c=\"ab\" }",
    "let t: table<a: int, b: path(read)> = []",
    "let l = [1, [2, {a: $(ls)}], (3 + 4)]",
    "ls --flag=\"ab\" (ls | wc) ${v.k} $v.k.l \"a ${v.k} b\"",
    "if $x { 1 } else { 2 }",
    "for i in [1, 2] { ls $i }",
    "let c = { |x, y| $x + $y }",
    "def f(a?: int, b: stream<int>, ...r: list<any>) { ls ...$r ...(ls) }",
    "let r: record<a: int> = {a: 1, \"b\": [1, 2], ...$r}",
    "ls { |x| $x.y? } --o=${a.\"b\"} (1 + $l.0)",
    "try { ls } catch { 1 }",
    "while $x { break }",
};

/* The byte offset of the innermost bracket or string the first length bytes
   of text leave open, or -1 if they leave none. Inside a string only `"`,
   `$(` and `${` mean anything. */
static long prefix_open(const char *text, size_t length)
{
    size_t opened[NVO_DEPTH_MAX];
    size_t i;
    unsigned depth = 0;

    for (i = 0; i < length; i++) {
        char c = text[i];
        int in_string = depth > 0 && text[opened[depth - 1]] == '"';

        if (in_string && c == '"')
            depth--;
        else if (c == '$' && i + 1 < length &&
                 (text[i + 1] == '(' || text[i + 1] == '{'))
            opened[depth++] = i++;
        else if (in_string)
            continue;
        else if (c == '"' || c == '(' || c == '[' || c == '{' || c == '<')
            opened[depth++] = i;
        else if (c == ')' || c == ']' || c == '}' || c == '>')
            depth--;
    }
    return depth > 0 ? (long)opened[depth - 1] : -1;
}

static int prefix_word_byte(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Whether a cut before text[cut] falls inside a token: between two bytes of
   a word, of `--` or of `...`, or just after a `$`. */
static int prefix_cuts_token(const char *text, size_t cut)
{
    char before = text[cut - 1], after = text[cut];

    return (prefix_word_byte(before) && prefix_word_byte(after)) ||
           (before == after && (before == '-' || before == '.')) ||
           before == '$';
}

static void prefix_cases(void)
{
    unsigned i, closed = 0, open = 0;

    for (i = 0; i < sizeof prefix_sources / sizeof prefix_sources[0]; i++) {
        const char *source = prefix_sources[i];
        size_t length = strlen(source), cut;

        check_source(parse_text(&case_a, SCRIPT, source) == NVO_OK,
                     "a source of the prefix corpus parses", source);
        for (cut = 1; cut < length; cut++) {
            long bracket = prefix_open(source, cut);
            NVO_STATUS status;

            if (prefix_cuts_token(source, cut))
                continue;
            status = parse_source(&case_a, SCRIPT, (const UINT8 *)source, cut);
            if (bracket >= 0) {
                open++;
                if (status == NVO_SYNTAX &&
                    case_a.tree.error == NVO_ERROR_UNTERMINATED &&
                    case_a.tree.error_offset == (UINT64)bracket)
                    continue;
            } else {
                closed++;
                if (status == NVO_OK ||
                    (status == NVO_SYNTAX &&
                     case_a.tree.error == NVO_ERROR_UNEXPECTED &&
                     case_a.tree.error_offset == cut))
                    continue;
            }
            printf("failed: the source cut at %lu, [%.*s], gave status %d "
                   "error %d at %lu, want %s\n",
                   (unsigned long)cut, (int)cut, source, (int)status,
                   (int)case_a.tree.error,
                   (unsigned long)case_a.tree.error_offset,
                   bracket >= 0 ? "unterminated at the innermost opening"
                                : "accepted or unexpected at the cut");
            failures++;
        }
    }
    check(open > 100 && closed > 20, "the prefix cases cut in and out");
}

/* ------------------------------------------------------------------------
   Values (L3) */

#define INT_SMALLEST (-INT_LARGEST - 1)

/* The condition is true: the expression's text is what a failure names. */
#define EXPECT(condition) check((condition), #condition)

static NVO_VALUE int_of(INT64 n)
{
    NVO_VALUE v;

    nvo_int(&v, n);
    return v;
}

static NVO_VALUE decimal_of(const char *text)
{
    NVO_VALUE v;
    NVO_DECIMAL d;

    if (nvo_decimal_from_text(&d, (const UINT8 *)text, strlen(text)) != NVO_OK)
        printf("failed: the decimal %s is not valid\n", text);
    nvo_decimal_value(&v, &d);
    return v;
}

static NVO_VALUE string_of(const char *text)
{
    NVO_VALUE v;

    if (nvo_string(&v, (const UINT8 *)text, strlen(text)) != NVO_OK)
        printf("failed: the string %s is not UTF-8\n", text);
    return v;
}

static NVO_VALUE path_of(const char *text)
{
    NVO_VALUE v;

    if (nvo_path(&v, (const UINT8 *)text, strlen(text)) != NVO_OK)
        printf("failed: the path %s is not valid\n", text);
    return v;
}

static NVO_VALUE bytes_of(const char *text)
{
    NVO_VALUE v;

    nvo_bytes(&v, (const UINT8 *)text, strlen(text));
    return v;
}

static NVO_VALUE bool_of(int truth)
{
    NVO_VALUE v;

    nvo_bool(&v, truth);
    return v;
}

static NVO_VALUE size_of(INT64 n)
{
    NVO_VALUE v;

    nvo_size(&v, n);
    return v;
}

static NVO_VALUE duration_of(INT64 n)
{
    NVO_VALUE v;

    nvo_duration(&v, n);
    return v;
}

static NVO_VALUE time_of(INT64 n)
{
    NVO_VALUE v;

    nvo_time(&v, n);
    return v;
}

static NVO_VALUE nothing_of(void)
{
    NVO_VALUE v;

    nvo_nothing(&v);
    return v;
}

static NVO_VALUE list_of(const NVO_VALUE *items, UINT64 count)
{
    NVO_VALUE v;

    nvo_list(&v, items, count);
    return v;
}

static NVO_VALUE record_of(const NVO_FIELD *fields, UINT64 count)
{
    NVO_VALUE v;

    if (nvo_record(&v, fields, count) != NVO_OK)
        printf("failed: a record's fields are not valid\n");
    return v;
}

static int is_equal(const NVO_VALUE *a, const NVO_VALUE *b)
{
    int equal = -1;

    return nvo_equal(a, b, &equal) == NVO_OK && equal == 1;
}

static int is_unequal(const NVO_VALUE *a, const NVO_VALUE *b)
{
    int equal = -1;

    return nvo_equal(a, b, &equal) == NVO_OK && equal == 0;
}

/* The order of a and b, or 2 if the order is an error. */
static int order_of(const NVO_VALUE *a, const NVO_VALUE *b)
{
    int order = 0;

    return nvo_order(a, b, &order) == NVO_OK ? order : 2;
}

static int is_int(const NVO_VALUE *v, INT64 n)
{
    return v->kind == NVO_VALUE_INT && v->u.integer == n;
}

static int is_decimal(const NVO_VALUE *v, const char *text)
{
    UINT8 out[NVO_DECIMAL_TEXT_MAX];
    UINT32 n;

    if (v->kind != NVO_VALUE_DECIMAL)
        return 0;
    n = nvo_decimal_to_text(&v->u.decimal, out);
    out[n] = '\0';
    return strcmp((const char *)out, text) == 0;
}

/* A text of no bytes may have null data, as nvo_concat of two empty texts
   gives, and memcmp takes no null pointer even for no bytes. */
static int is_text_of(const NVO_VALUE *v, NVO_VALUE_KIND kind, const char *text)
{
    return v->kind == kind && v->u.text.length == strlen(text) &&
           (v->u.text.length == 0 ||
            memcmp(v->u.text.data, text, v->u.text.length) == 0);
}

static void kind_value_cases(void)
{
    NVO_VALUE a = int_of(1), b = int_of(2), c = string_of("x"),
              d = nothing_of();
    NVO_VALUE ab[2], other[2], v;
    NVO_VALUE rows[3], mixed[2], one[1], tall[2];
    NVO_FIELD x_y[2], y_x[2], x_only[1], none[1];
    NVO_FIELD longer[3], x_z[2];
    NVO_VALUE record_xy, record_xy2, record_yx, record_xz, record_x,
        record_empty;
    NVO_VALUE columns[2];

    ab[0] = a;
    ab[1] = b;
    v = list_of(ab, 2);
    EXPECT(nvo_kind_of(&v) == NVO_VALUE_LIST);
    EXPECT(!nvo_is_table(&v));
    other[0] = c;
    other[1] = d;
    v = list_of(other, 2);
    EXPECT(nvo_kind_of(&v) == NVO_VALUE_LIST);
    v = list_of(ab, 0);
    EXPECT(!nvo_is_table(&v));

    x_y[0].key = (const UINT8 *)"x";
    x_y[0].key_length = 1;
    x_y[0].value = &ab[0];
    x_y[1].key = (const UINT8 *)"y";
    x_y[1].key_length = 1;
    x_y[1].value = &ab[1];
    y_x[0] = x_y[1];
    y_x[1] = x_y[0];
    x_only[0] = x_y[0];
    longer[0] = x_y[0];
    longer[1] = x_y[1];
    longer[2].key = (const UINT8 *)"z";
    longer[2].key_length = 1;
    longer[2].value = &ab[0];
    x_z[0] = x_y[0];
    x_z[1] = longer[2];
    none[0] = x_y[0];
    record_xy = record_of(x_y, 2);
    record_xy2 = record_of(x_y, 2);
    record_yx = record_of(y_x, 2);
    record_xz = record_of(x_z, 2);
    record_x = record_of(x_only, 1);
    record_empty = record_of(none, 0);

    /* L3 rule 5: a list of records with the same keys in the same order. */
    rows[0] = record_xy;
    rows[1] = record_xy2;
    rows[2] = record_xy;
    v = list_of(rows, 3);
    EXPECT(nvo_is_table(&v));
    EXPECT(nvo_kind_of(&v) == NVO_VALUE_TABLE);
    v = list_of(rows, 1);
    EXPECT(nvo_is_table(&v));
    mixed[0] = record_xy;
    mixed[1] = record_yx;
    v = list_of(mixed, 2);
    EXPECT(!nvo_is_table(&v));
    EXPECT(nvo_kind_of(&v) == NVO_VALUE_LIST);
    mixed[1] = record_xz;
    v = list_of(mixed, 2);
    EXPECT(!nvo_is_table(&v));
    mixed[1] = record_x;
    v = list_of(mixed, 2);
    EXPECT(!nvo_is_table(&v));
    mixed[1] = a;
    v = list_of(mixed, 2);
    EXPECT(!nvo_is_table(&v));
    one[0] = record_empty;
    v = list_of(one, 1);
    EXPECT(!nvo_is_table(&v));
    tall[0] = record_empty;
    tall[1] = record_empty;
    v = list_of(tall, 2);
    EXPECT(!nvo_is_table(&v));
    columns[0] = record_of(longer, 3);
    columns[1] = record_xy;
    v = list_of(columns, 2);
    EXPECT(!nvo_is_table(&v));

    /* A record's keys are unique and UTF-8. */
    x_y[1].key = (const UINT8 *)"x";
    EXPECT(nvo_record(&v, x_y, 2) == NVO_INVALID);
    x_y[1].key = (const UINT8 *)"\377";
    EXPECT(nvo_record(&v, x_y, 2) == NVO_INVALID);
}

/* A chain of lists, each holding the next, ending in an int: the first of
   CHAIN_LENGTH - 1 lists one past NVO_VALUE_DEPTH_MAX deep, the third
   exactly that. */
#define CHAIN_LENGTH (NVO_VALUE_DEPTH_MAX + 3)

static void equality_cases(void)
{
    NVO_VALUE two = int_of(2), two_d = decimal_of("2.0"),
              neg_zero = decimal_of("-0"), zero = decimal_of("0"),
              zero_i = int_of(0), s2 = string_of("2"),
              big = int_of(INT_LARGEST),
              big_d = decimal_of("9223372036854775807"),
              near_d = decimal_of("9223372036854775806.9");
    NVO_VALUE v1 = string_of("abc"), v2 = path_of("abc"), v3 = bytes_of("abc"),
              v4 = string_of("abd");
    NVO_VALUE size = size_of(1), duration = duration_of(1), time = time_of(1),
              size2 = size_of(1);
    NVO_VALUE nothing = nothing_of(), yes = bool_of(1), no = bool_of(0);
    NVO_VALUE xs[2], ys[2], zs[2];
    NVO_VALUE nested_a[1], nested_b[1];
    NVO_VALUE chain[CHAIN_LENGTH];
    NVO_VALUE left, right;
    NVO_FIELD fa[2], fb[2], fc[2], fd[1], fe[2];
    NVO_VALUE one = int_of(1), three = int_of(3);
    int equal;
    unsigned i;

    /* L3 rule 2: structural; an int and a decimal compare by value. */
    EXPECT(is_equal(&two, &two_d));
    EXPECT(is_equal(&two_d, &two));
    EXPECT(is_equal(&zero, &neg_zero));
    EXPECT(is_equal(&zero, &zero_i));
    EXPECT(is_equal(&big, &big_d));
    EXPECT(is_unequal(&big, &near_d));
    EXPECT(is_unequal(&two, &s2));
    EXPECT(is_unequal(&v1, &v2));
    EXPECT(is_unequal(&v1, &v3));
    EXPECT(is_unequal(&v1, &v4));
    EXPECT(is_equal(&v1, &v1));
    EXPECT(is_unequal(&size, &duration));
    EXPECT(is_unequal(&duration, &time));
    EXPECT(is_unequal(&size, &one));
    EXPECT(is_equal(&size, &size2));
    EXPECT(is_equal(&nothing, &nothing));
    EXPECT(is_unequal(&nothing, &no));
    EXPECT(is_equal(&yes, &yes));
    EXPECT(is_unequal(&yes, &no));
    EXPECT(is_unequal(&zero_i, &no));

    xs[0] = one;
    xs[1] = two;
    ys[0] = one;
    ys[1] = two_d;
    zs[0] = one;
    zs[1] = three;
    left = list_of(xs, 2);
    right = list_of(ys, 2);
    EXPECT(is_equal(&left, &right));
    right = list_of(zs, 2);
    EXPECT(is_unequal(&left, &right));
    right = list_of(xs, 1);
    EXPECT(is_unequal(&left, &right));
    nested_a[0] = left;
    nested_b[0] = list_of(ys, 2);
    left = list_of(nested_a, 1);
    right = list_of(nested_b, 1);
    EXPECT(is_equal(&left, &right));

    /* Records are equal when their keys, in order, and values are. */
    fa[0].key = (const UINT8 *)"a";
    fa[0].key_length = 1;
    fa[0].value = &one;
    fa[1].key = (const UINT8 *)"b";
    fa[1].key_length = 1;
    fa[1].value = &two;
    fb[0] = fa[0];
    fb[1] = fa[1];
    fb[1].value = &two_d;
    fc[0] = fa[1];
    fc[1] = fa[0];
    fd[0] = fa[0];
    left = record_of(fa, 2);
    right = record_of(fb, 2);
    EXPECT(is_equal(&left, &right));
    right = record_of(fc, 2);
    EXPECT(is_unequal(&left, &right));
    right = record_of(fd, 1);
    EXPECT(is_unequal(&left, &right));
    fb[1].value = &three;
    right = record_of(fb, 2);
    EXPECT(is_unequal(&left, &right));
    fe[0] = fa[0];
    fe[1] = fa[1];
    fe[1].key = (const UINT8 *)"c";
    right = record_of(fe, 2);
    EXPECT(is_unequal(&left, &right));

    /* Closures are equal when they hold the same code and captures. */
    {
        static const int code_a = 1, code_b = 2;
        NVO_VALUE captured[1], f, g;

        captured[0] = one;
        f.kind = NVO_VALUE_CLOSURE;
        f.u.closure.code = &code_a;
        f.u.closure.captured = captured;
        f.u.closure.count = 1;
        f.error_kind = 0;
        g = f;
        EXPECT(is_equal(&f, &g));
        g.u.closure.code = &code_b;
        EXPECT(is_unequal(&f, &g));
        g = f;
        g.u.closure.count = 0;
        EXPECT(is_unequal(&f, &g));
    }

    /* An error is equal to another of its kind and message. */
    {
        NVO_VALUE e1, e2;

        e1 = string_of("boom");
        e1.kind = NVO_VALUE_ERROR;
        e1.error_kind = 3;
        e2 = e1;
        EXPECT(is_equal(&e1, &e2));
        e2.error_kind = 4;
        EXPECT(is_unequal(&e1, &e2));
        e2 = string_of("bang");
        e2.kind = NVO_VALUE_ERROR;
        e2.error_kind = 3;
        EXPECT(is_unequal(&e1, &e2));
    }

    /* Nesting past NVO_VALUE_DEPTH_MAX is an error, not a long walk: a list
       of lists that many deep is compared, one level more is not. */
    for (i = 0; i < CHAIN_LENGTH; i++)
        chain[i] = i + 1 < CHAIN_LENGTH ? list_of(&chain[i + 1], 1) : int_of(1);
    equal = -1;
    EXPECT(nvo_equal(&chain[2], &chain[2], &equal) == NVO_OK && equal == 1);
    EXPECT(nvo_equal(&chain[1], &chain[1], &equal) == NVO_OVERFLOW);
    EXPECT(nvo_equal(&chain[0], &chain[0], &equal) == NVO_OVERFLOW);
}

static void order_cases(void)
{
    NVO_VALUE one = int_of(1), two = int_of(2), half = decimal_of("1.5"),
              two_d = decimal_of("2.00");
    NVO_VALUE a = string_of("a"), b = string_of("b"), upper = string_of("Z"),
              z = string_of("z"), accent = string_of("\303\251"),
              empty = string_of(""), ab = string_of("ab");
    NVO_VALUE pa = path_of("./a"), pb = path_of("./b");
    NVO_VALUE s1 = size_of(1), s2 = size_of(2), d1 = duration_of(1),
              d2 = duration_of(-5), t1 = time_of(10), t2 = time_of(20);
    NVO_VALUE yes = bool_of(1), no = bool_of(0), nothing = nothing_of(),
              by = bytes_of("a"), items[1], list, big = int_of(INT_LARGEST),
              small = int_of(INT_SMALLEST);

    /* L3 rule 2: numbers, strings and paths bytewise, sizes, durations and
       times. */
    EXPECT(order_of(&one, &two) == -1);
    EXPECT(order_of(&two, &one) == 1);
    EXPECT(order_of(&one, &one) == 0);
    EXPECT(order_of(&one, &half) == -1);
    EXPECT(order_of(&half, &two) == -1);
    EXPECT(order_of(&two, &two_d) == 0);
    EXPECT(order_of(&big, &small) == 1);
    EXPECT(order_of(&small, &big) == -1);
    EXPECT(order_of(&a, &b) == -1);
    EXPECT(order_of(&upper, &a) == -1);
    EXPECT(order_of(&z, &accent) == -1);
    EXPECT(order_of(&accent, &z) == 1);
    EXPECT(order_of(&empty, &a) == -1);
    EXPECT(order_of(&a, &ab) == -1);
    EXPECT(order_of(&ab, &a) == 1);
    EXPECT(order_of(&pa, &pb) == -1);
    EXPECT(order_of(&pb, &pa) == 1);
    EXPECT(order_of(&s1, &s2) == -1);
    EXPECT(order_of(&d1, &d2) == 1);
    EXPECT(order_of(&t1, &t2) == -1);
    EXPECT(order_of(&t2, &t2) == 0);

    /* Across other types, and for the types with no order, `type`. */
    EXPECT(order_of(&s1, &d1) == 2);
    EXPECT(order_of(&d1, &t1) == 2);
    EXPECT(order_of(&s1, &one) == 2);
    EXPECT(order_of(&a, &pa) == 2);
    EXPECT(order_of(&a, &one) == 2);
    EXPECT(order_of(&yes, &no) == 2);
    EXPECT(order_of(&nothing, &nothing) == 2);
    EXPECT(order_of(&by, &by) == 2);
    items[0] = one;
    list = list_of(items, 1);
    EXPECT(order_of(&list, &list) == 2);
}

static void condition_cases(void)
{
    NVO_VALUE yes = bool_of(1), no = bool_of(0), one = int_of(1),
              nothing = nothing_of(), s = string_of("true"),
              empty = string_of(""), zero = decimal_of("0");
    int truth = -1;

    /* L3 rule 3: only a bool is a condition. */
    EXPECT(nvo_condition(&yes, &truth) == NVO_OK && truth == 1);
    EXPECT(nvo_condition(&no, &truth) == NVO_OK && truth == 0);
    EXPECT(nvo_condition(&one, &truth) == NVO_TYPE);
    EXPECT(nvo_condition(&nothing, &truth) == NVO_TYPE);
    EXPECT(nvo_condition(&s, &truth) == NVO_TYPE);
    EXPECT(nvo_condition(&empty, &truth) == NVO_TYPE);
    EXPECT(nvo_condition(&zero, &truth) == NVO_TYPE);
}

static void arithmetic_cases(void)
{
    NVO_VALUE r, one = int_of(1), two = int_of(2), three = int_of(3),
                 big = int_of(INT_LARGEST), small = int_of(INT_SMALLEST),
                 minus_one = int_of(-1), zero = int_of(0),
                 half = decimal_of("0.5"), tenth = decimal_of("0.1"),
                 s = string_of("x"), yes = bool_of(1),
                 big_less = int_of(INT_LARGEST - 1),
                 small_more = int_of(INT_SMALLEST + 1),
                 half_big = int_of(INT_LARGEST / 2 + 1), minus_two = int_of(-2);
    NVO_VALUE size1 = size_of(1), size2 = size_of(2),
              size_big = size_of(INT_LARGEST), dur1 = duration_of(1),
              dur_big = duration_of(INT_LARGEST), time1 = time_of(100),
              time2 = time_of(30), dur_small = duration_of(INT_SMALLEST);

    /* An int meeting a decimal is widened exactly. */
    EXPECT(nvo_add(&r, &one, &two) == NVO_OK && is_int(&r, 3));
    EXPECT(nvo_add(&r, &one, &half) == NVO_OK && is_decimal(&r, "1.5"));
    EXPECT(nvo_add(&r, &half, &one) == NVO_OK && is_decimal(&r, "1.5"));
    EXPECT(nvo_add(&r, &tenth, &tenth) == NVO_OK && is_decimal(&r, "0.2"));
    EXPECT(nvo_add(&r, &big, &half) == NVO_OK &&
           is_decimal(&r, "9223372036854775807.5"));
    EXPECT(nvo_subtract(&r, &one, &half) == NVO_OK && is_decimal(&r, "0.5"));
    EXPECT(nvo_multiply(&r, &two, &half) == NVO_OK && is_decimal(&r, "1.0"));
    EXPECT(nvo_multiply(&r, &three, &three) == NVO_OK && is_int(&r, 9));

    /* An int result that does not fit raises overflow; nothing wraps. */
    EXPECT(nvo_add(&r, &big, &one) == NVO_OVERFLOW);
    EXPECT(nvo_add(&r, &small, &minus_one) == NVO_OVERFLOW);
    EXPECT(nvo_add(&r, &big, &small) == NVO_OK && is_int(&r, -1));
    EXPECT(nvo_add(&r, &big_less, &one) == NVO_OK && is_int(&r, INT_LARGEST));
    EXPECT(nvo_add(&r, &small_more, &minus_one) == NVO_OK &&
           is_int(&r, INT_SMALLEST));
    EXPECT(nvo_subtract(&r, &big_less, &minus_one) == NVO_OK &&
           is_int(&r, INT_LARGEST));
    EXPECT(nvo_subtract(&r, &small_more, &one) == NVO_OK &&
           is_int(&r, INT_SMALLEST));
    EXPECT(nvo_subtract(&r, &small, &one) == NVO_OVERFLOW);
    EXPECT(nvo_subtract(&r, &big, &minus_one) == NVO_OVERFLOW);
    EXPECT(nvo_subtract(&r, &zero, &small) == NVO_OVERFLOW);
    EXPECT(nvo_subtract(&r, &minus_one, &small) == NVO_OK &&
           is_int(&r, INT_LARGEST));
    EXPECT(nvo_multiply(&r, &big, &two) == NVO_OVERFLOW);
    EXPECT(nvo_multiply(&r, &half_big, &two) == NVO_OVERFLOW);
    EXPECT(nvo_multiply(&r, &half_big, &minus_two) == NVO_OK &&
           is_int(&r, INT_SMALLEST));
    EXPECT(nvo_multiply(&r, &small, &minus_one) == NVO_OVERFLOW);
    EXPECT(nvo_multiply(&r, &minus_one, &small) == NVO_OVERFLOW);
    EXPECT(nvo_multiply(&r, &small, &one) == NVO_OK &&
           is_int(&r, INT_SMALLEST));
    EXPECT(nvo_multiply(&r, &big, &minus_one) == NVO_OK &&
           is_int(&r, -INT_LARGEST));
    EXPECT(nvo_negate(&r, &small) == NVO_OVERFLOW);
    EXPECT(nvo_negate(&r, &big) == NVO_OK && is_int(&r, -INT_LARGEST));
    EXPECT(nvo_negate(&r, &zero) == NVO_OK && is_int(&r, 0));

    /* Sizes, durations and times. */
    EXPECT(nvo_add(&r, &size1, &size2) == NVO_OK && r.kind == NVO_VALUE_SIZE &&
           r.u.integer == 3);
    EXPECT(nvo_add(&r, &size_big, &size1) == NVO_OVERFLOW);
    EXPECT(nvo_add(&r, &dur1, &dur1) == NVO_OK &&
           r.kind == NVO_VALUE_DURATION && r.u.integer == 2);
    EXPECT(nvo_add(&r, &dur_big, &dur1) == NVO_OVERFLOW);
    EXPECT(nvo_add(&r, &time1, &dur1) == NVO_OK && r.kind == NVO_VALUE_TIME &&
           r.u.integer == 101);
    EXPECT(nvo_add(&r, &dur1, &time1) == NVO_OK && r.kind == NVO_VALUE_TIME &&
           r.u.integer == 101);
    EXPECT(nvo_add(&r, &time1, &time2) == NVO_TYPE);
    EXPECT(nvo_add(&r, &size1, &dur1) == NVO_TYPE);
    EXPECT(nvo_add(&r, &size1, &one) == NVO_TYPE);
    EXPECT(nvo_subtract(&r, &time1, &time2) == NVO_OK &&
           r.kind == NVO_VALUE_DURATION && r.u.integer == 70);
    EXPECT(nvo_subtract(&r, &time1, &dur1) == NVO_OK &&
           r.kind == NVO_VALUE_TIME && r.u.integer == 99);
    EXPECT(nvo_subtract(&r, &dur1, &time1) == NVO_TYPE);
    EXPECT(nvo_subtract(&r, &size2, &size1) == NVO_OK &&
           r.kind == NVO_VALUE_SIZE && r.u.integer == 1);
    EXPECT(nvo_subtract(&r, &size1, &size2) == NVO_OK &&
           r.kind == NVO_VALUE_SIZE && r.u.integer == -1);
    EXPECT(nvo_subtract(&r, &time2, &dur_big) == NVO_OK);
    EXPECT(nvo_subtract(&r, &time2, &dur_small) == NVO_OVERFLOW);
    EXPECT(nvo_multiply(&r, &size2, &three) == NVO_OK &&
           r.kind == NVO_VALUE_SIZE && r.u.integer == 6);
    EXPECT(nvo_multiply(&r, &three, &dur1) == NVO_OK &&
           r.kind == NVO_VALUE_DURATION && r.u.integer == 3);
    EXPECT(nvo_multiply(&r, &size_big, &two) == NVO_OVERFLOW);
    EXPECT(nvo_multiply(&r, &size2, &size2) == NVO_TYPE);
    EXPECT(nvo_multiply(&r, &size2, &half) == NVO_TYPE);
    EXPECT(nvo_negate(&r, &size2) == NVO_OK && r.kind == NVO_VALUE_SIZE &&
           r.u.integer == -2);
    EXPECT(nvo_negate(&r, &dur1) == NVO_OK && r.kind == NVO_VALUE_DURATION &&
           r.u.integer == -1);
    EXPECT(nvo_negate(&r, &dur_small) == NVO_OVERFLOW);
    EXPECT(nvo_negate(&r, &time1) == NVO_TYPE);

    /* No implicit conversion: everything else is `type`. */
    EXPECT(nvo_add(&r, &s, &s) == NVO_TYPE);
    EXPECT(nvo_add(&r, &one, &s) == NVO_TYPE);
    EXPECT(nvo_add(&r, &yes, &yes) == NVO_TYPE);
    EXPECT(nvo_subtract(&r, &s, &one) == NVO_TYPE);
    EXPECT(nvo_multiply(&r, &s, &two) == NVO_TYPE);
    EXPECT(nvo_negate(&r, &s) == NVO_TYPE);
    EXPECT(nvo_negate(&r, &yes) == NVO_TYPE);

    /* A decimal is negated as Decimal's minus does: from zero, so zero
       stays plus, and the exponent is kept. */
    {
        NVO_VALUE zero_d = decimal_of("0.00"), minus_zero = decimal_of("-0");

        EXPECT(nvo_negate(&r, &zero_d) == NVO_OK && is_decimal(&r, "0.00"));
        EXPECT(nvo_negate(&r, &minus_zero) == NVO_OK && is_decimal(&r, "0"));
        EXPECT(nvo_negate(&r, &half) == NVO_OK && is_decimal(&r, "-0.5"));
    }

    /* r may be an operand. */
    r = one;
    EXPECT(nvo_add(&r, &r, &r) == NVO_OK && is_int(&r, 2));
    EXPECT(nvo_multiply(&r, &r, &r) == NVO_OK && is_int(&r, 4));
}

static void division_cases(void)
{
    NVO_VALUE r,
        zero = int_of(0), zero_d = decimal_of("0.00"), one = int_of(1),
        two = int_of(2), three = int_of(3), six = int_of(6), seven = int_of(7),
        minus_seven = int_of(-7), minus_two = int_of(-2),
        minus_three = int_of(-3), minus_one = int_of(-1),
        small = int_of(INT_SMALLEST), big = int_of(INT_LARGEST),
        s = string_of("x"), seven_half = decimal_of("7.5"),
        minus_seven_half = decimal_of("-7.5"), huge = decimal_of("1e30"),
        size = size_of(5), half = decimal_of("0.5"),
        just_under = decimal_of("9223372036854775807.9"),
        two_63 = decimal_of("9223372036854775808"),
        minus_two_63 = decimal_of("-9223372036854775808"),
        just_past = decimal_of("-9223372036854775808.1");

    /* `/` on two ints is a decimal. */
    EXPECT(nvo_divide(&r, &one, &three) == NVO_OK &&
           is_decimal(&r, "0.3333333333333333333333333333333333"));
    EXPECT(nvo_divide(&r, &six, &three) == NVO_OK && is_decimal(&r, "2"));
    EXPECT(nvo_divide(&r, &one, &two) == NVO_OK && is_decimal(&r, "0.5"));
    EXPECT(nvo_divide(&r, &minus_seven, &two) == NVO_OK &&
           is_decimal(&r, "-3.5"));
    EXPECT(nvo_divide(&r, &seven_half, &minus_three) == NVO_OK &&
           is_decimal(&r, "-2.5"));
    EXPECT(nvo_divide(&r, &small, &minus_one) == NVO_OK &&
           is_decimal(&r, "9223372036854775808"));
    EXPECT(nvo_divide(&r, &one, &zero) == NVO_DIVIDE_BY_ZERO);
    EXPECT(nvo_divide(&r, &one, &zero_d) == NVO_DIVIDE_BY_ZERO);
    EXPECT(nvo_divide(&r, &zero, &zero) == NVO_DIVIDE_BY_ZERO);
    EXPECT(nvo_divide(&r, &seven_half, &zero) == NVO_DIVIDE_BY_ZERO);
    EXPECT(nvo_divide(&r, &s, &one) == NVO_TYPE);
    EXPECT(nvo_divide(&r, &size, &one) == NVO_TYPE);
    EXPECT(nvo_divide(&r, &one, &s) == NVO_TYPE);

    /* `//` is the floor of the quotient, an int. */
    EXPECT(nvo_divide_floor(&r, &seven, &two) == NVO_OK && is_int(&r, 3));
    EXPECT(nvo_divide_floor(&r, &minus_seven, &two) == NVO_OK &&
           is_int(&r, -4));
    EXPECT(nvo_divide_floor(&r, &seven, &minus_two) == NVO_OK &&
           is_int(&r, -4));
    EXPECT(nvo_divide_floor(&r, &minus_seven, &minus_two) == NVO_OK &&
           is_int(&r, 3));
    EXPECT(nvo_divide_floor(&r, &six, &three) == NVO_OK && is_int(&r, 2));
    EXPECT(nvo_divide_floor(&r, &minus_seven, &minus_one) == NVO_OK &&
           is_int(&r, 7));
    EXPECT(nvo_divide_floor(&r, &zero, &three) == NVO_OK && is_int(&r, 0));
    EXPECT(nvo_divide_floor(&r, &seven_half, &two) == NVO_OK && is_int(&r, 3));
    EXPECT(nvo_divide_floor(&r, &minus_seven_half, &two) == NVO_OK &&
           is_int(&r, -4));
    EXPECT(nvo_divide_floor(&r, &seven_half, &minus_two) == NVO_OK &&
           is_int(&r, -4));
    EXPECT(nvo_divide_floor(&r, &seven, &half) == NVO_OK && is_int(&r, 14));
    EXPECT(nvo_divide_floor(&r, &big, &one) == NVO_OK &&
           is_int(&r, INT_LARGEST));
    EXPECT(nvo_divide_floor(&r, &small, &minus_one) == NVO_OVERFLOW);
    EXPECT(nvo_divide_floor(&r, &huge, &one) == NVO_OVERFLOW);
    EXPECT(nvo_divide_floor(&r, &just_under, &one) == NVO_OK &&
           is_int(&r, INT_LARGEST));
    EXPECT(nvo_divide_floor(&r, &two_63, &one) == NVO_OVERFLOW);
    EXPECT(nvo_divide_floor(&r, &minus_two_63, &one) == NVO_OK &&
           is_int(&r, INT_SMALLEST));
    EXPECT(nvo_divide_floor(&r, &just_past, &one) == NVO_OVERFLOW);
    EXPECT(nvo_divide_floor(&r, &one, &zero) == NVO_DIVIDE_BY_ZERO);
    EXPECT(nvo_divide_floor(&r, &one, &zero_d) == NVO_DIVIDE_BY_ZERO);
    EXPECT(nvo_divide_floor(&r, &s, &one) == NVO_TYPE);

    /* `%` has the sign of the divisor. */
    EXPECT(nvo_remainder(&r, &seven, &three) == NVO_OK && is_int(&r, 1));
    EXPECT(nvo_remainder(&r, &minus_seven, &three) == NVO_OK && is_int(&r, 2));
    EXPECT(nvo_remainder(&r, &seven, &minus_three) == NVO_OK && is_int(&r, -2));
    EXPECT(nvo_remainder(&r, &minus_seven, &minus_three) == NVO_OK &&
           is_int(&r, -1));
    EXPECT(nvo_remainder(&r, &six, &three) == NVO_OK && is_int(&r, 0));
    EXPECT(nvo_remainder(&r, &small, &minus_one) == NVO_OK && is_int(&r, 0));
    EXPECT(nvo_remainder(&r, &small, &big) == NVO_OK &&
           is_int(&r, INT_LARGEST - 1));
    EXPECT(nvo_remainder(&r, &seven_half, &two) == NVO_OK &&
           is_decimal(&r, "1.5"));
    EXPECT(nvo_remainder(&r, &minus_seven_half, &two) == NVO_OK &&
           is_decimal(&r, "0.5"));
    EXPECT(nvo_remainder(&r, &seven_half, &minus_two) == NVO_OK &&
           is_decimal(&r, "-0.5"));
    EXPECT(nvo_remainder(&r, &seven, &half) == NVO_OK &&
           nvo_decimal_is_zero(&r.u.decimal));
    EXPECT(nvo_remainder(&r, &one, &zero) == NVO_DIVIDE_BY_ZERO);
    EXPECT(nvo_remainder(&r, &seven_half, &zero_d) == NVO_DIVIDE_BY_ZERO);
    EXPECT(nvo_remainder(&r, &s, &one) == NVO_TYPE);
}

/* A random int: of every magnitude, the extremes over-represented. */
static INT64 random_int(GENERATOR *g)
{
    static const INT64 extremes[] = {0,
                                     1,
                                     -1,
                                     2,
                                     -2,
                                     INT_LARGEST,
                                     INT_SMALLEST,
                                     INT_LARGEST - 1,
                                     INT_SMALLEST + 1,
                                     10,
                                     -10,
                                     3,
                                     -3};
    UINT64 raw = next_random(g);
    unsigned bits = 1 + below(g, 63);

    if (below(g, 6) == 0)
        return extremes[below(g, sizeof extremes / sizeof extremes[0])];
    return (INT64)(raw >> bits) * (below(g, 2) == 0 ? 1 : -1);
}

/* L3 rule 4: a == (a // b) * b + a % b, with the remainder of b's sign and
   smaller than b, over ints of every size (the product is added in
   decimals, which hold what an int does not). */
static void division_law_cases(void)
{
    static GENERATOR g;
    unsigned i;

    g.state = 0x2545f4914f6cdd1dULL;
    for (i = 0; i < 20000; i++) {
        INT64 a = random_int(&g), b = random_int(&g);
        NVO_VALUE va = int_of(a), vb = int_of(b), q, r;
        NVO_DECIMAL da, dq, db, dr, product, sum;
        NVO_STATUS quotient;

        if (b == 0)
            continue;
        quotient = nvo_divide_floor(&q, &va, &vb);
        if (nvo_remainder(&r, &va, &vb) != NVO_OK || r.kind != NVO_VALUE_INT) {
            printf("failed: %lld %% %lld is not an int\n", (long long)a,
                   (long long)b);
            failures++;
            continue;
        }
        if (a == INT_SMALLEST && b == -1) {
            check(quotient == NVO_OVERFLOW && r.u.integer == 0,
                  "the smallest int over -1 overflows");
            continue;
        }
        if (quotient != NVO_OK || q.kind != NVO_VALUE_INT) {
            printf("failed: %lld // %lld is not an int\n", (long long)a,
                   (long long)b);
            failures++;
            continue;
        }
        nvo_decimal_from_int(&da, a);
        nvo_decimal_from_int(&db, b);
        nvo_decimal_from_int(&dq, q.u.integer);
        nvo_decimal_from_int(&dr, r.u.integer);
        if (nvo_decimal_multiply(&product, &dq, &db) != NVO_OK ||
            nvo_decimal_add(&sum, &product, &dr) != NVO_OK ||
            nvo_decimal_compare(&sum, &da) != 0) {
            printf("failed: %lld != (%lld // %lld) * %lld + %lld %% %lld\n",
                   (long long)a, (long long)a, (long long)b, (long long)b,
                   (long long)a, (long long)b);
            failures++;
        }
        if (b > 0 ? !(r.u.integer >= 0 && r.u.integer < b)
                  : !(r.u.integer <= 0 && r.u.integer > b)) {
            printf("failed: %lld %% %lld = %lld is not between 0 and the "
                   "divisor\n",
                   (long long)a, (long long)b, (long long)r.u.integer);
            failures++;
        }
    }
}

/* The same law over decimals with a fraction: exact in 34 digits at these
   sizes, and the remainder has the sign of the divisor and is smaller. */
static void decimal_law_cases(void)
{
    static const unsigned long scales[] = {10, 100, 1000, 10000};
    static GENERATOR g;
    unsigned i;

    g.state = 0x9e3779b97f4a7c15ULL;
    for (i = 0; i < 20000; i++) {
        char ta[48], tb[48];
        NVO_VALUE va, vb, q, r, product, sum;
        NVO_DECIMAL zero;
        unsigned places_a = 1 + below(&g, 4), places_b = 1 + below(&g, 4);
        int remainder_ok, divisor_positive;

        snprintf(ta, sizeof ta, "%s%lu.%0*lu", below(&g, 2) ? "-" : "",
                 (unsigned long)(next_random(&g) >> 40) % 100000, (int)places_a,
                 (unsigned long)(next_random(&g) >> 40) % scales[places_a - 1]);
        snprintf(tb, sizeof tb, "%s%lu.%0*lu", below(&g, 2) ? "-" : "",
                 (unsigned long)(next_random(&g) >> 40) % 100000, (int)places_b,
                 (unsigned long)(next_random(&g) >> 40) % scales[places_b - 1]);
        va = decimal_of(ta);
        vb = decimal_of(tb);
        if (nvo_decimal_is_zero(&vb.u.decimal))
            continue;
        if (nvo_divide_floor(&q, &va, &vb) != NVO_OK ||
            nvo_remainder(&r, &va, &vb) != NVO_OK) {
            printf("failed: %s // %s or %s %% %s is an error\n", ta, tb, ta,
                   tb);
            failures++;
            continue;
        }
        if (nvo_multiply(&product, &q, &vb) != NVO_OK ||
            nvo_add(&sum, &product, &r) != NVO_OK || !is_equal(&sum, &va)) {
            printf("failed: %s != (%s // %s) * %s + %s %% %s\n", ta, ta, tb, tb,
                   ta, tb);
            failures++;
        }
        nvo_decimal_zero(&zero);
        divisor_positive = nvo_decimal_compare(&vb.u.decimal, &zero) > 0;
        remainder_ok =
            divisor_positive
                ? nvo_decimal_compare(&r.u.decimal, &zero) >= 0 &&
                      nvo_decimal_compare(&r.u.decimal, &vb.u.decimal) < 0
                : nvo_decimal_compare(&r.u.decimal, &zero) <= 0 &&
                      nvo_decimal_compare(&r.u.decimal, &vb.u.decimal) > 0;
        if (!remainder_ok) {
            printf("failed: %s %% %s is not between 0 and the divisor\n", ta,
                   tb);
            failures++;
        }
    }
}

/* L3: a decimal `%` is the floored remainder, exact or an overflow. The
   first five need more than 34 digits at the exponent of the smaller
   operand, whose rounded sum would equal the divisor; the others are the
   edge on the near side of that and the quotient's 34 digits. A want of
   0 is an overflow. */
static void remainder_cases(void)
{
    static const struct {
        const char *a, *b, *want;
    } cases[] = {
        {"-1E-40", "3", 0},
        {"-1", "1E+35", 0},
        {"-1E-34", "7", 0},
        {"-0.0000000000000001", "9223372036854775807", 0},
        {"-1.000000000000000000001", "9223372036854775807", 0},
        {"-1E-33", "3", "2.999999999999999999999999999999999"},
        {"-1E-33", "7", "6.999999999999999999999999999999999"},
        {"-1", "1E+34", "9999999999999999999999999999999999"},
        {"1", "-1E+34", "-9999999999999999999999999999999999"},
        {"-10", "1E+35", "9.999999999999999999999999999999999E+34"},
        {"-1.0", "3", "2.0"},
        {"1E-40", "3", "1E-40"},
        {"-1E-40", "-3", "-1E-40"},
        {"1E+30", "0.001", "0.000"},
        {"1E+31", "0.001", 0},
    };
    NVO_VALUE r, minus_one = int_of(-1), three = int_of(3);
    unsigned i;

    EXPECT(nvo_remainder(&r, &minus_one, &three) == NVO_OK && is_int(&r, 2));
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        NVO_VALUE a = decimal_of(cases[i].a), b = decimal_of(cases[i].b);
        NVO_STATUS status = nvo_remainder(&r, &a, &b);

        if (cases[i].want ? status != NVO_OK || !is_decimal(&r, cases[i].want)
                          : status != NVO_OVERFLOW) {
            printf("failed: %s %% %s: status %d, want %s\n", cases[i].a,
                   cases[i].b, (int)status,
                   cases[i].want ? cases[i].want : "overflow");
            failures++;
        }
    }

    /* The quotient has more than 34 digits for `//` as well, even when
       the remainder is defined exactly. */
    {
        NVO_VALUE a = decimal_of("1E+31"), b = decimal_of("0.001");

        EXPECT(nvo_divide_floor(&r, &a, &b) == NVO_OVERFLOW);
    }
}

/* ------------------------------------------------------------------------
   Decimal division, checked in wide integers */

/* The oracle for L3 rule 4 over decimals: each operand is scaled to an
   integer at the smaller of their exponents and the two are divided by
   the schoolbook method on digit strings, which shares no code with the
   library. It gives the floored quotient and remainder, and the digits the
   remainder needs. */

#define WIDE_DIGITS 320

/* A signed integer: decimal digits, the least significant first, with no
   leading zero, so that zero has none. */
typedef struct {
    UINT8 digit[WIDE_DIGITS];
    unsigned length;
    int negative;
} WIDE;

static void wide_overflow(void)
{
    printf("failed: the wide integers are too narrow\n");
    failures++;
}

static void wide_trim(WIDE *w)
{
    while (w->length > 0 && w->digit[w->length - 1] == 0)
        w->length--;
    if (w->length == 0)
        w->negative = 0;
}

/* w x 10^places. */
static void wide_shift(WIDE *w, unsigned places)
{
    unsigned i;

    if (w->length == 0)
        return;
    if (w->length + places > WIDE_DIGITS) {
        wide_overflow();
        return;
    }
    for (i = w->length; i > 0; i--)
        w->digit[i - 1 + places] = w->digit[i - 1];
    for (i = 0; i < places; i++)
        w->digit[i] = 0;
    w->length += places;
}

/* w = the digits of text, most significant first, x 10^places. */
static void wide_set(WIDE *w, const char *text, int negative, unsigned places)
{
    size_t n = strlen(text), i;

    for (i = 0; i < n; i++)
        w->digit[i] = (UINT8)(text[n - 1 - i] - '0');
    w->length = (unsigned)n;
    w->negative = negative;
    wide_trim(w);
    wide_shift(w, places);
}

static void wide_set_u64(WIDE *w, UINT64 magnitude, int negative)
{
    w->length = 0;
    for (; magnitude > 0; magnitude /= 10)
        w->digit[w->length++] = (UINT8)(magnitude % 10);
    w->negative = negative && w->length > 0;
}

static int wide_compare_magnitude(const WIDE *a, const WIDE *b)
{
    unsigned i;

    if (a->length != b->length)
        return a->length < b->length ? -1 : 1;
    for (i = a->length; i > 0; i--)
        if (a->digit[i - 1] != b->digit[i - 1])
            return a->digit[i - 1] < b->digit[i - 1] ? -1 : 1;
    return 0;
}

/* r = |a| + |b|. r may be a or b. */
static void wide_add_magnitude(WIDE *r, const WIDE *a, const WIDE *b)
{
    unsigned n = a->length > b->length ? a->length : b->length, i, carry = 0;

    for (i = 0; i < n || carry != 0; i++) {
        unsigned sum = carry + (i < a->length ? a->digit[i] : 0u) +
                       (i < b->length ? b->digit[i] : 0u);

        if (i >= WIDE_DIGITS) {
            wide_overflow();
            return;
        }
        r->digit[i] = (UINT8)(sum % 10);
        carry = sum / 10;
    }
    r->length = i;
}

/* r = |a| - |b|, for |a| not less than |b|. r may be a. */
static void wide_subtract_magnitude(WIDE *r, const WIDE *a, const WIDE *b)
{
    unsigned i, borrow = 0;

    for (i = 0; i < a->length; i++) {
        int d = (int)a->digit[i] - (i < b->length ? (int)b->digit[i] : 0) -
                (int)borrow;

        borrow = d < 0;
        r->digit[i] = (UINT8)(d < 0 ? d + 10 : d);
    }
    r->length = a->length;
    wide_trim(r);
}

/* r = |a| x d, for one digit d. r may be a. */
static void wide_multiply_digit(WIDE *r, const WIDE *a, unsigned d)
{
    unsigned i, carry = 0, n = a->length;

    for (i = 0; i < n || carry != 0; i++) {
        unsigned product = carry + (i < n ? a->digit[i] * d : 0u);

        if (i >= WIDE_DIGITS) {
            wide_overflow();
            return;
        }
        r->digit[i] = (UINT8)(product % 10);
        carry = product / 10;
    }
    r->length = i;
    wide_trim(r);
}

/* q = |a| div |b| and r = |a| mod |b|, as long division goes: b is not
   zero. */
static void wide_divide_magnitude(WIDE *q, WIDE *r, const WIDE *a,
                                  const WIDE *b)
{
    WIDE shifted;
    unsigned place;

    *r = *a;
    r->negative = 0;
    q->length = 0;
    q->negative = 0;
    if (a->length < b->length)
        return;
    for (place = a->length - b->length + 1; place > 0; place--) {
        unsigned count = 0;

        shifted = *b;
        shifted.negative = 0;
        wide_shift(&shifted, place - 1);
        while (wide_compare_magnitude(r, &shifted) >= 0) {
            wide_subtract_magnitude(r, r, &shifted);
            count++;
        }
        q->digit[place - 1] = (UINT8)count;
    }
    q->length = a->length - b->length + 1;
    wide_trim(q);
}

/* r = a + b. */
static void wide_add(WIDE *r, const WIDE *a, const WIDE *b)
{
    WIDE sum;

    if (a->negative == b->negative) {
        wide_add_magnitude(&sum, a, b);
        sum.negative = a->negative;
    } else if (wide_compare_magnitude(a, b) >= 0) {
        wide_subtract_magnitude(&sum, a, b);
        sum.negative = a->negative;
    } else {
        wide_subtract_magnitude(&sum, b, a);
        sum.negative = b->negative;
    }
    if (sum.length == 0)
        sum.negative = 0;
    *r = sum;
}

/* r = a x b. */
static void wide_multiply(WIDE *r, const WIDE *a, const WIDE *b)
{
    WIDE sum, partial;
    unsigned i;

    sum.length = 0;
    for (i = 0; i < b->length; i++) {
        wide_multiply_digit(&partial, a, b->digit[i]);
        wide_shift(&partial, i);
        wide_add_magnitude(&sum, &sum, &partial);
    }
    sum.negative = a->negative != b->negative && sum.length > 0;
    *r = sum;
}

static int wide_equal(const WIDE *a, const WIDE *b)
{
    return a->negative == b->negative && wide_compare_magnitude(a, b) == 0;
}

/* w = d as a count of 10^least, which is not above d's exponent. */
static void wide_from_decimal(WIDE *w, const NVO_DECIMAL *d, int least)
{
    unsigned limb, k, n = 0;

    for (limb = 0; limb < NVO_DECIMAL_LIMBS; limb++) {
        UINT32 part = d->limb[limb];

        for (k = 0; k < NVO_DECIMAL_LIMB_DIGITS; k++) {
            w->digit[n++] = (UINT8)(part % 10);
            part /= 10;
        }
    }
    w->length = n;
    w->negative = (int)d->negative;
    wide_trim(w);
    wide_shift(w, (unsigned)((int)d->exponent - least));
}

/* A decimal written out: a coefficient of at most 34 digits and an
   exponent. */
typedef struct {
    char digits[NVO_DECIMAL_DIGITS + 1];
    int negative;
    int exponent;
    char text[NVO_DECIMAL_TEXT_MAX];
} OPERAND;

/* One digit of a coefficient of the given shape. */
static char operand_digit(GENERATOR *g, unsigned shape, unsigned i)
{
    switch (shape) {
    case 0: /* one digit, then zeros */
        return i == 0 ? (char)('1' + below(g, 9)) : '0';
    case 1: /* nines */
        return '9';
    default:
        return (char)((i == 0 ? '1' : '0') + below(g, i == 0 ? 9 : 10));
    }
}

/* An operand of count digits whose adjusted exponent is given; zero, of
   one digit, if may_be_zero and the dice say so. */
static void operand_make(GENERATOR *g, OPERAND *o, unsigned count, int adjusted,
                         int may_be_zero)
{
    unsigned shape = below(g, 8), i;
    int zero = may_be_zero && below(g, 32) == 0;

    if (zero)
        count = 1;
    for (i = 0; i < count; i++)
        o->digits[i] = zero ? '0' : operand_digit(g, shape, i);
    o->digits[count] = '\0';
    o->negative = (int)below(g, 2);
    o->exponent = adjusted - (int)(count - 1);
    snprintf(o->text, sizeof o->text, "%s%sE%d", o->negative ? "-" : "",
             o->digits, o->exponent);
}

static unsigned operand_digit_count(GENERATOR *g)
{
    switch (below(g, 4)) {
    case 0:
        return NVO_DECIMAL_DIGITS;
    case 1:
        return 1 + below(g, 5);
    case 2:
        return NVO_DECIMAL_DIGITS - 1 + below(g, 2);
    default:
        return 1 + below(g, NVO_DECIMAL_DIGITS);
    }
}

/* The classes of case the generator must reach, for a check that it does. */
typedef enum {
    CLASS_QUOTIENT_TOO_WIDE,  /* more than 34 digits in the integer quotient */
    CLASS_REMAINDER_TOO_WIDE, /* a floored remainder past 34 digits */
    CLASS_FLOORED,            /* a remainder moved by the divisor, exactly */
    CLASS_SAME_SIDE,          /* a remainder that needed no move */
    CLASS_COUNT
} WIDE_CLASS;

static void wide_failure(const OPERAND *a, const OPERAND *b, const char *what)
{
    printf("failed: %s %% %s: %s\n", a->text, b->text, what);
    failures++;
}

/* Check `//` and `%` of a and b against the oracle, and the identity of
   rule 4 on whatever they return. */
static void wide_division_case(const OPERAND *a, const OPERAND *b,
                               unsigned reached[CLASS_COUNT])
{
    NVO_VALUE va = decimal_of(a->text), vb = decimal_of(b->text), q, r;
    NVO_STATUS got_q = nvo_divide_floor(&q, &va, &vb);
    NVO_STATUS got_r = nvo_remainder(&r, &va, &vb), want_q, want_r;
    int least = a->exponent < b->exponent ? a->exponent : b->exponent;
    WIDE wa, wb, quotient, rest, floored_q, floored_r;
    int differ, too_wide;
    unsigned trailing = 0;
    INT64 want_int = 0;

    wide_set(&wa, a->digits, a->negative, (unsigned)(a->exponent - least));
    wide_set(&wb, b->digits, b->negative, (unsigned)(b->exponent - least));
    wide_divide_magnitude(&quotient, &rest, &wa, &wb);
    differ = wa.length > 0 && wa.negative != wb.negative;
    too_wide = quotient.length > NVO_DECIMAL_DIGITS;
    floored_q = quotient;
    floored_r = rest;
    if (differ && rest.length > 0) {
        WIDE one;

        wide_set_u64(&one, 1, 0);
        wide_add_magnitude(&floored_q, &quotient, &one);
        wide_subtract_magnitude(&floored_r, &wb, &rest);
    }
    floored_q.negative = differ && floored_q.length > 0;
    floored_r.negative = wb.negative && floored_r.length > 0;

    /* The remainder is exact unless it needs more than 34 digits once its
       trailing zeros, which rounding keeps, are set aside. */
    while (trailing < floored_r.length && floored_r.digit[trailing] == 0)
        trailing++;
    want_r = too_wide || floored_r.length - trailing > NVO_DECIMAL_DIGITS
                 ? NVO_OVERFLOW
                 : NVO_OK;
    want_q = NVO_OVERFLOW;
    if (!too_wide && floored_q.length <= 19) {
        UINT64 magnitude = 0;
        unsigned i;

        for (i = floored_q.length; i > 0; i--)
            magnitude = magnitude * 10 + floored_q.digit[i - 1];
        if (floored_q.negative ? magnitude <= 0x8000000000000000ULL
                               : magnitude <= 0x7fffffffffffffffULL) {
            want_q = NVO_OK;
            want_int = floored_q.negative ? (INT64)((UINT64)0 - magnitude)
                                          : (INT64)magnitude;
        }
    }

    if (too_wide)
        reached[CLASS_QUOTIENT_TOO_WIDE]++;
    else if (want_r == NVO_OVERFLOW)
        reached[CLASS_REMAINDER_TOO_WIDE]++;
    else if (differ && rest.length > 0)
        reached[CLASS_FLOORED]++;
    else
        reached[CLASS_SAME_SIDE]++;

    if (got_r != want_r)
        wide_failure(a, b,
                     want_r == NVO_OK ? "an error where the remainder is exact"
                                      : "a value where none is exact");
    if (got_q != want_q || (got_q == NVO_OK && (q.kind != NVO_VALUE_INT ||
                                                q.u.integer != want_int)))
        wide_failure(a, b, "the floored quotient is not the oracle's");
    if (got_r != NVO_OK || r.kind != NVO_VALUE_DECIMAL)
        return;
    {
        int common = (int)r.u.decimal.exponent < least
                         ? (int)r.u.decimal.exponent
                         : least;
        WIDE got, expected = floored_r, divisor, sum, product, whole, count;

        wide_from_decimal(&got, &r.u.decimal, common);
        wide_shift(&expected, (unsigned)(least - common));
        if (!wide_equal(&got, &expected))
            wide_failure(a, b, "the remainder is not the oracle's");

        /* It is 0 or has b's sign, and is smaller than b. */
        wide_set(&divisor, b->digits, 0, (unsigned)(b->exponent - common));
        if (!(got.length == 0 || got.negative == b->negative) ||
            wide_compare_magnitude(&got, &divisor) >= 0)
            wide_failure(a, b, "the remainder is not between 0 and b");

        /* And it makes a == (a // b) * b + a % b. */
        if (got_q == NVO_OK) {
            wide_set_u64(&count,
                         q.u.integer < 0 ? (UINT64)0 - (UINT64)q.u.integer
                                         : (UINT64)q.u.integer,
                         q.u.integer < 0);
            wide_set(&divisor, b->digits, b->negative,
                     (unsigned)(b->exponent - common));
            wide_multiply(&product, &count, &divisor);
            wide_add(&sum, &product, &got);
            wide_set(&whole, a->digits, a->negative,
                     (unsigned)(a->exponent - common));
            if (!wide_equal(&sum, &whole))
                wide_failure(a, b, "a is not (a // b) * b + a % b");
        }
    }
}

/* The law over operands of up to 34 digits, of either sign, whose
   exponents are far apart: so that the integer quotient has no digits or
   more than 34, and so that the floored remainder needs a few digits
   either side of 34. */
static void wide_decimal_law_cases(void)
{
    static const unsigned minimum[CLASS_COUNT] = {1000, 1000, 1000, 1000};
    static GENERATOR g;
    unsigned reached[CLASS_COUNT] = {0, 0, 0, 0}, i;

    g.state = 0x6a09e667f3bcc909ULL;
    for (i = 0; i < 40000; i++) {
        OPERAND a, b;
        unsigned count_a = operand_digit_count(&g);
        unsigned count_b = operand_digit_count(&g);
        int adjusted_b = (int)below(&g, 81) - 40, delta;

        /* Often the span from b's first digit to a's last is 33, 34 or 35
           digits, where the floored remainder stops being exact. */
        if (below(&g, 3) == 0)
            delta = (int)count_a - (int)(33 + below(&g, 3));
        else
            delta = (int)below(&g, 84) - 45;
        operand_make(&g, &b, count_b, adjusted_b, 0);
        operand_make(&g, &a, count_a, adjusted_b + delta, 1);
        wide_division_case(&a, &b, reached);
    }
    for (i = 0; i < CLASS_COUNT; i++)
        if (reached[i] < minimum[i]) {
            printf("failed: the wide division cases reach class %u %u times\n",
                   i, reached[i]);
            failures++;
        }
}

static void text_value_cases(void)
{
    static union {
        UINT8 bytes[1024];
        UINT64 alignment;
    } memory;
    NVO_ARENA arena;
    NVO_VALUE r, hello = string_of("hello, "), world = string_of("world"),
                 empty = string_of(""), one = int_of(1), by1 = bytes_of("ab"),
                 by2 = bytes_of("\377"), pa = path_of("./a"),
                 pb = path_of("b/c"), slash = path_of("/usr/"),
                 root = path_of("/"), abs = path_of("/x"),
                 str_abs = string_of("/x"), empty_path;
    NVO_VALUE xs[2], ys[1], list_a, list_b;

    nvo_arena_init(&arena, memory.bytes, sizeof memory.bytes);

    /* `++` concatenates strings, bytes and lists. */
    EXPECT(nvo_concat(&arena, &r, &hello, &world) == NVO_OK &&
           is_text_of(&r, NVO_VALUE_STRING, "hello, world"));
    EXPECT(nvo_concat(&arena, &r, &empty, &empty) == NVO_OK &&
           is_text_of(&r, NVO_VALUE_STRING, ""));
    EXPECT(nvo_concat(&arena, &r, &empty, &world) == NVO_OK &&
           is_text_of(&r, NVO_VALUE_STRING, "world"));
    EXPECT(nvo_concat(&arena, &r, &by1, &by2) == NVO_OK &&
           r.kind == NVO_VALUE_BYTES && r.u.text.length == 3 &&
           r.u.text.data[2] == 0xff);
    xs[0] = one;
    xs[1] = world;
    ys[0] = hello;
    list_a = list_of(xs, 2);
    list_b = list_of(ys, 1);
    EXPECT(nvo_concat(&arena, &r, &list_a, &list_b) == NVO_OK &&
           r.kind == NVO_VALUE_LIST && r.u.list.count == 3 &&
           is_int(&r.u.list.items[0], 1) &&
           is_text_of(&r.u.list.items[1], NVO_VALUE_STRING, "world") &&
           is_text_of(&r.u.list.items[2], NVO_VALUE_STRING, "hello, "));
    list_b = list_of(ys, 0);
    EXPECT(nvo_concat(&arena, &r, &list_b, &list_b) == NVO_OK &&
           r.kind == NVO_VALUE_LIST && r.u.list.count == 0);

    /* No other pair: no implicit conversion. */
    EXPECT(nvo_concat(&arena, &r, &hello, &one) == NVO_TYPE);
    EXPECT(nvo_concat(&arena, &r, &one, &one) == NVO_TYPE);
    EXPECT(nvo_concat(&arena, &r, &hello, &by1) == NVO_TYPE);
    EXPECT(nvo_concat(&arena, &r, &hello, &list_a) == NVO_TYPE);
    EXPECT(nvo_concat(&arena, &r, &pa, &pb) == NVO_TYPE);

    /* A full arena is NVO_MEMORY, and takes nothing. */
    nvo_arena_init(&arena, memory.bytes, 12);
    EXPECT(nvo_concat(&arena, &r, &hello, &world) == NVO_OK);
    EXPECT(nvo_concat(&arena, &r, &hello, &world) == NVO_MEMORY);
    EXPECT(nvo_concat(&arena, &r, &empty, &empty) == NVO_OK);
    EXPECT(arena.used == 12);
    nvo_arena_init(&arena, memory.bytes, 11);
    EXPECT(nvo_concat(&arena, &r, &hello, &world) == NVO_MEMORY);
    EXPECT(arena.used == 0);
    nvo_arena_init(&arena, memory.bytes, 2 * sizeof(NVO_VALUE));
    EXPECT(nvo_concat(&arena, &r, &list_a, &list_a) == NVO_MEMORY);
    EXPECT(nvo_concat(&arena, &r, &list_a, &list_b) == NVO_OK);

    /* `/` joins a path to a string or a relative path with one slash. */
    nvo_arena_init(&arena, memory.bytes, sizeof memory.bytes);
    EXPECT(nvo_join(&arena, &r, &pa, &world) == NVO_OK &&
           is_text_of(&r, NVO_VALUE_PATH, "./a/world"));
    EXPECT(nvo_join(&arena, &r, &pa, &pb) == NVO_OK &&
           is_text_of(&r, NVO_VALUE_PATH, "./a/b/c"));
    EXPECT(nvo_join(&arena, &r, &slash, &pb) == NVO_OK &&
           is_text_of(&r, NVO_VALUE_PATH, "/usr/b/c"));
    EXPECT(nvo_join(&arena, &r, &root, &pb) == NVO_OK &&
           is_text_of(&r, NVO_VALUE_PATH, "/b/c"));
    EXPECT(nvo_join(&arena, &r, &pa, &empty) == NVO_OK &&
           is_text_of(&r, NVO_VALUE_PATH, "./a/"));
    EXPECT(nvo_join(&arena, &r, &pa, &abs) == NVO_TYPE);
    EXPECT(nvo_join(&arena, &r, &pa, &str_abs) == NVO_TYPE);
    EXPECT(nvo_join(&arena, &r, &hello, &world) == NVO_TYPE);
    EXPECT(nvo_join(&arena, &r, &pa, &one) == NVO_TYPE);
    EXPECT(nvo_join(&arena, &r, &one, &world) == NVO_TYPE);
    nvo_arena_init(&arena, memory.bytes, 8);
    EXPECT(nvo_join(&arena, &r, &pa, &world) == NVO_MEMORY);

    /* Strings are UTF-8 and paths are not empty. */
    EXPECT(nvo_string(&r, (const UINT8 *)"\303\251", 2) == NVO_OK);
    EXPECT(nvo_string(&r, (const UINT8 *)"\303", 1) == NVO_INVALID);
    EXPECT(nvo_string(&r, (const UINT8 *)"\355\240\200", 3) == NVO_INVALID);
    EXPECT(nvo_string(&r, (const UINT8 *)"", 0) == NVO_OK);
    EXPECT(nvo_path(&empty_path, (const UINT8 *)"", 0) == NVO_INVALID);
    EXPECT(nvo_path(&empty_path, (const UINT8 *)"\377", 1) == NVO_INVALID);
    EXPECT(nvo_path(&empty_path, (const UINT8 *)"~", 1) == NVO_OK);
}

/* A number literal's value is the value of the type its text names. */
static void literal_value_cases(void)
{
    static const struct {
        const char *text;
        NVO_VALUE_KIND kind;
        INT64 integer;
    } literals[] = {
        {"42", NVO_VALUE_INT, 42},
        {"0x2a", NVO_VALUE_INT, 42},
        {"4KiB", NVO_VALUE_SIZE, 4096},
        {"1.5s", NVO_VALUE_DURATION, 1500000000LL},
        {"2.5e-3", NVO_VALUE_DECIMAL, 0},
    };
    unsigned i;

    for (i = 0; i < sizeof literals / sizeof literals[0]; i++) {
        NVO_NUMBER number;
        NVO_VALUE v;

        if (nvo_number_parse((const UINT8 *)literals[i].text,
                             strlen(literals[i].text), &number) != NVO_OK) {
            check_source(0, "a literal parses", literals[i].text);
            continue;
        }
        nvo_from_number(&v, &number);
        check_source(v.kind == literals[i].kind, "a literal's type",
                     literals[i].text);
        if (v.kind != NVO_VALUE_DECIMAL)
            check_source(v.u.integer == literals[i].integer,
                         "a literal's value", literals[i].text);
        else
            check_source(is_decimal(&v, "0.0025"), "a literal's value",
                         literals[i].text);
    }
}

static void value_cases(void)
{
    kind_value_cases();
    equality_cases();
    order_cases();
    condition_cases();
    arithmetic_cases();
    division_cases();
    division_law_cases();
    decimal_law_cases();
    remainder_cases();
    wide_decimal_law_cases();
    text_value_cases();
    literal_value_cases();
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: check-nvo shell.md\n");
        return 2;
    }
    spec_examples(argv[1]);
    shape_cases();
    printing_cases();
    error_cases();
    utf8_edge_cases();
    depth_cases();
    chain_cases();
    call_cases();
    print_cases();
    number_cases();
    generated_cases();
    mutated_cases();
    sweep_cases();
    prefix_cases();
    coverage_cases();
    value_cases();
    printf("%u failures\n", failures);
    return failures != 0;
}
