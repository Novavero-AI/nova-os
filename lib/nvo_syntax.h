/** @file
    nvo's source text: its tokens and grammar (docs/spec/shell.md L1 and
    L2) as a parser that makes a syntax tree, and a printer that writes a
    tree back as source that parses to the same tree.

    The parser reads the source once, in the two modes L1 gives, and
    makes no system call: the caller gives it the nodes' memory and it
    takes no other. It recurses to a depth of NVO_DEPTH_MAX at most. It
    serves the guest and the host alike. */

#ifndef NOVA_NVO_SYNTAX_H
#define NOVA_NVO_SYNTAX_H

#include "nvo_decimal.h"

/** The most levels of nesting a source holds: brackets (including `${` and
    a parameter list's `(`), blocks, groups, substitutions, types and prefix
    operators. The parser recurses once for each, so this bounds the stack a
    source can use; a source nested deeper is a parse error
    (NVO_ERROR_DEPTH). */
#define NVO_DEPTH_MAX 32

/** The longest source the parser takes, in bytes: a node's offsets are
    32 bits, and NVO_NODES_FOR of this fits 32 bits. */
#define NVO_SOURCE_MAX 0x0fffffffu

/** The nodes enough for a source of the given length, in bytes, whatever
    it holds, for the capacity nvo_parse is given. */
#define NVO_NODES_FOR(length) ((UINT32)(length) * 4u + 8u)

/** The longest decimal, size or duration literal nvo_number_parse takes,
    its number without the unit, in bytes. */
#define NVO_NUMBER_LITERAL_MAX 256

/** What a node stands for. The children of each are in order; `text` is
    the node's span of the source. */
typedef enum {
    NVO_NODE_NONE = 0, /**< no node: the end of a list of children */

    /* Structure. */
    NVO_NODE_FILE,    /**< a file: [needs], script */
    NVO_NODE_NEEDS,   /**< `needs`: a record */
    NVO_NODE_SCRIPT,  /**< statements */
    NVO_NODE_BLOCK,   /**< `{ }`: a script */
    NVO_NODE_CLOSURE, /**< `{ |a, b| }`: params, script */
    NVO_NODE_PARAMS,  /**< a closure's names, or a function's parameters */

    /* Statements. */
    NVO_NODE_LET,      /**< name, [type], expr; flag NVO_FLAG_EXPORT */
    NVO_NODE_VAR,      /**< name, [type], expr */
    NVO_NODE_SET,      /**< name, members, expr; flags: NVO_SET_* */
    NVO_NODE_DEF,      /**< defname, params, [type], block; NVO_FLAG_EXPORT */
    NVO_NODE_USE,      /**< path, [name] */
    NVO_NODE_IF,       /**< expr, block, elifs, [else] */
    NVO_NODE_ELIF,     /**< expr, block */
    NVO_NODE_ELSE,     /**< block */
    NVO_NODE_FOR,      /**< name, expr, block */
    NVO_NODE_WHILE,    /**< expr, block */
    NVO_NODE_TRY,      /**< block, [catch] */
    NVO_NODE_CATCH,    /**< [name], block */
    NVO_NODE_BREAK,    /**< `break` */
    NVO_NODE_CONTINUE, /**< `continue` */
    NVO_NODE_RETURN,   /**< [expr] */
    NVO_NODE_PIPELINE, /**< two or more stages */

    /* Parameters and types. */
    NVO_NODE_PARAM,      /**< name, type, [default]; flag NVO_FLAG_OPTIONAL */
    NVO_NODE_FLAG_PARAM, /**< defname, [short], [type], [default] */
    NVO_NODE_REST_PARAM, /**< name, type */
    NVO_NODE_SHORT,      /**< a flag parameter's one letter */
    NVO_NODE_TYPE,       /**< a type: text, then parameters; NVO_FLAG_READ,
                              NVO_FLAG_WRITE for a path */
    NVO_NODE_FIELD_TYPE, /**< a record or table field: key, type */

    /* Commands. */
    NVO_NODE_COMMAND, /**< head, arguments; flag NVO_FLAG_CARET */
    NVO_NODE_FLAG,    /**< a flag, and the parts that touch it */
    NVO_NODE_GRANT,   /**< `=>`: a word or a spread */
    NVO_NODE_SPREAD,  /**< `...`: a variable or a group, or in a list or
                           record an expression */
    NVO_NODE_WORD,    /**< parts that touch */

    /* Names, words and strings. */
    NVO_NODE_NAME,      /**< an identifier */
    NVO_NODE_DEFNAME,   /**< a function's name */
    NVO_NODE_BARE,      /**< a run of characters that are not
                             metacharacters */
    NVO_NODE_PATH,      /**< a path literal */
    NVO_NODE_GLOB,      /**< a glob literal */
    NVO_NODE_STRING_SQ, /**< a single-quoted string, quotes included */
    NVO_NODE_STRING_DQ, /**< a double-quoted string: texts and
                             interpolations */
    NVO_NODE_TEXT,      /**< the text of a double-quoted string between its
                             interpolations, escapes as written */
    NVO_NODE_VARIABLE,  /**< a variable: the name without the `$`, then
                             members; NVO_FLAG_BRACED, NVO_FLAG_OPTIONAL */
    NVO_NODE_MEMBER,    /**< a member, after a dot, as a name, digits or a
                             string; flags NVO_MEMBER_* */
    NVO_NODE_SUBST,     /**< `$( )`: a pipeline */
    NVO_NODE_GROUP,     /**< `( )`: a pipeline */

    /* Literals. */
    NVO_NODE_INT,      /**< an integer literal */
    NVO_NODE_DECIMAL,  /**< a decimal literal */
    NVO_NODE_SIZE,     /**< a size literal */
    NVO_NODE_DURATION, /**< a duration literal */
    NVO_NODE_BYTES,    /**< a bytes literal */
    NVO_NODE_TRUE,     /**< `true` */
    NVO_NODE_FALSE,    /**< `false` */
    NVO_NODE_NOTHING,  /**< `nothing` */
    NVO_NODE_LIST,     /**< elements */
    NVO_NODE_RECORD,   /**< entries and spreads */
    NVO_NODE_ENTRY,    /**< key, expr */
    NVO_NODE_TABLE,    /**< columns, rows */
    NVO_NODE_COLUMNS,  /**< a table's names */
    NVO_NODE_ROW,      /**< a table's row: exprs */

    /* Operators, loosest first. The binary ones have two children. */
    NVO_NODE_OR,     /**< `or` */
    NVO_NODE_AND,    /**< `and` */
    NVO_NODE_NOT,    /**< `not`: one child */
    NVO_NODE_EQ,     /**< `==` */
    NVO_NODE_NE,     /**< `!=` */
    NVO_NODE_LT,     /**< `<` */
    NVO_NODE_LE,     /**< `<=` */
    NVO_NODE_GT,     /**< `>` */
    NVO_NODE_GE,     /**< `>=` */
    NVO_NODE_IN,     /**< `in` */
    NVO_NODE_NOT_IN, /**< `not-in` */
    NVO_NODE_CONCAT, /**< `++` */
    NVO_NODE_ADD,    /**< `+` */
    NVO_NODE_SUB,    /**< `-` */
    NVO_NODE_MUL,    /**< `*` */
    NVO_NODE_DIV,    /**< `/` */
    NVO_NODE_IDIV,   /**< `//` */
    NVO_NODE_MOD,    /**< `%` */
    NVO_NODE_NEG,    /**< unary `-`: one child */
    NVO_NODE_ACCESS, /**< `expr.member`: expr, member; NVO_FLAG_OPTIONAL
                          for `?` */

    NVO_NODE_KINDS /**< how many kinds there are */
} NVO_NODE_KIND;

/** A node's flags, which each kind reads as its own. */
typedef enum {
    NVO_FLAG_EXPORT = 1,   /**< let and def: written with `export` */
    NVO_FLAG_CARET = 1,    /**< command: written with a leading `^` */
    NVO_FLAG_OPTIONAL = 1, /**< a parameter written `name?`, a variable or
                                access written with a trailing `?` */
    NVO_FLAG_BRACED = 2,   /**< a variable written `${name}` */
    NVO_FLAG_READ = 1,     /**< a type written `path(read)` */
    NVO_FLAG_WRITE = 2,    /**< a type written `path(write)` */
    NVO_SET_ASSIGN = 0,    /**< set written with `=` */
    NVO_SET_ADD = 1,       /**< set written with `+=` */
    NVO_SET_SUBTRACT = 2,  /**< set written with `-=` */
    NVO_SET_CONCAT = 3,    /**< set written with `++=` */
    NVO_MEMBER_NAME = 0,   /**< a member written as a name */
    NVO_MEMBER_INT = 1,    /**< a member written as digits */
    NVO_MEMBER_STRING = 2  /**< a member written as a string, its child */
} NVO_NODE_FLAGS;

/** A node of a syntax tree. Nodes are in an array and name each other by
    index, 0 being none. */
typedef struct {
    UINT16 kind;   /**< an NVO_NODE_KIND */
    UINT16 flags;  /**< an NVO_NODE_FLAGS, as the kind reads them */
    UINT32 first;  /**< the first child, or 0 */
    UINT32 last;   /**< the last child, or 0 */
    UINT32 next;   /**< the next sibling, or 0 */
    UINT32 parent; /**< the parent, or 0 for the root */
    UINT32 offset; /**< where the node's span begins in the source */
    UINT32 length; /**< the span's bytes */
} NVO_NODE;

/** Why a source is not valid. */
typedef enum {
    NVO_ERROR_NONE = 0,     /**< no error */
    NVO_ERROR_UTF8,         /**< bytes that are not UTF-8 (L1 rule 1) */
    NVO_ERROR_UNEXPECTED,   /**< a character or end the grammar does not
                                 allow here */
    NVO_ERROR_BARE_WORD,    /**< a word where an expression needs an operand,
                                 a keyword excepted (L1 rule 4) */
    NVO_ERROR_CHAINED,      /**< a second comparison in a row, at its
                                 operator (L2, precedence) */
    NVO_ERROR_UNTERMINATED, /**< a string or bracket that is not closed, at
                                 its opening, or for a source that ends where
                                 a bracket needs more, at the opening of the
                                 innermost bracket it ends in */
    NVO_ERROR_ESCAPE,       /**< a backslash that is not an escape of a
                                 double-quoted string, or `\u{ }` that is
                                 not a Unicode scalar value */
    NVO_ERROR_LITERAL,      /**< a number that does not fit its type, or a
                                 bytes literal of half a byte */
    NVO_ERROR_DEPTH         /**< nesting past NVO_DEPTH_MAX */
} NVO_ERROR;

/** What a source is: a file may begin with `needs`, a script a user types
    may not. */
typedef enum {
    NVO_PARSE_FILE,  /**< a script or module file (L2's `file`) */
    NVO_PARSE_SCRIPT /**< what is typed at the prompt (L2's `script`) */
} NVO_PARSE_MODE;

/** A parsed source. */
typedef struct {
    NVO_NODE *nodes;      /**< the caller's array; node 0 is not a node */
    UINT32 capacity;      /**< the array's length */
    UINT32 count;         /**< the nodes made, with node 0 */
    UINT32 root;          /**< the root's index, when the parse succeeded */
    NVO_ERROR error;      /**< when it gave NVO_SYNTAX: why */
    UINT64 error_offset;  /**< and where, as a byte offset in the source */
} NVO_TREE;

/** Set *offset to the first byte of source that is not UTF-8 (RFC 3629:
    no overlong form, no surrogate, nothing above U+10FFFF) and return 0,
    or return 1 if the length bytes are all UTF-8. */
int nvo_utf8_check(const UINT8 *source, UINT64 length, UINT64 *offset);

/** Parse the length bytes of source into tree, using the capacity
    elements of nodes. Returns NVO_OK; NVO_SYNTAX, with tree->error and
    tree->error_offset set, if the source is not valid, a source with bytes
    that are not UTF-8 giving that first, at the first such byte;
    or NVO_MEMORY if nodes is too small or the source longer than
    NVO_SOURCE_MAX. NVO_NODES_FOR(length) nodes are enough. */
NVO_STATUS nvo_parse(NVO_TREE *tree, NVO_PARSE_MODE mode, const UINT8 *source,
                     UINT64 length, NVO_NODE *nodes, UINT32 capacity);

/** Whether a node's span of the source is its own text, which the printer
    copies and a comparison of two trees reads: names, words, literals,
    types, variables and members written as names or digits. The other
    nodes' spans only say where they were. */
int nvo_node_has_text(const NVO_NODE *node);

/** Write tree as source into out, which holds capacity bytes, with no
    terminator, and set *written to the bytes written. The source parses,
    in the mode the tree was parsed in, to a tree equal to this one, and is
    the same for equal trees. Returns NVO_OK or NVO_MEMORY, the output not
    fitting capacity. source is the text the tree was parsed from. */
NVO_STATUS nvo_print(const NVO_TREE *tree, const UINT8 *source, UINT8 *out,
                     UINT64 capacity, UINT64 *written);

/** What kind of literal a number token is. */
typedef enum {
    NVO_NUMBER_INT,     /**< an integer */
    NVO_NUMBER_DECIMAL, /**< a decimal */
    NVO_NUMBER_SIZE,    /**< a size, in bytes */
    NVO_NUMBER_DURATION /**< a duration, in nanoseconds */
} NVO_NUMBER_KIND;

/** The value of a number literal. */
typedef struct {
    NVO_NUMBER_KIND kind; /**< which literal it was */
    INT64 integer;        /**< an integer, a size or a duration */
    NVO_DECIMAL decimal;  /**< a decimal */
} NVO_NUMBER;

/** Set *number to the value of the number literal the length bytes of text
    are. Returns NVO_OK; NVO_SYNTAX if text is not one literal wholly;
    NVO_OVERFLOW if it does not fit its type; or NVO_INVALID if a size or
    duration does not come to a whole number of its unit's smallest part
    (`1.5B`), or the number of a decimal, size or duration is longer than
    NVO_NUMBER_LITERAL_MAX bytes. */
NVO_STATUS nvo_number_parse(const UINT8 *text, UINT64 length,
                            NVO_NUMBER *number);

#endif
