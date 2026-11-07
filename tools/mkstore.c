/* mkstore: writes a store image (docs/spec/store-image.md) from store
   paths as nova-nix exports them, each a narinfo and its NAR
   (docs/spec/storage.md F16). It checks every NAR against its narinfo's
   size and hash, a content-addressed path's name against its contents,
   the NAR's grammar and names, and that the paths' references are closed
   among them, then writes the registrations, nodes and blobs in the
   fixed order of F11, so the same inputs give the same bytes.

   usage: mkstore IMAGE BOOT NARINFO NAR [NARINFO NAR]...

   BOOT is the base name of the store path holding the programs the first
   process starts. A narinfo's NAR is uncompressed ("Compression: none")
   and its NarHash a SHA-256 in nix-base32; its URL is not read, since
   the NAR is named on the command line. A content address, where there
   is one, is "fixed:r:sha256:" and the NAR hash (upstream
   content-address.cc), and the path's hash part must be the one upstream
   computes from it (store-api.cc, makeFixedOutputPath). A path without
   one is input-addressed, and its name cannot be checked against its
   contents (storage.md, fact 3).

   Formats: the NAR grammar of nova-cache's NovaCache.NAR, and its entry
   name rules (NovaCache.NAR.Stream, checkEntryName); the narinfo keys of
   NovaCache.NarInfo; UTF-8 as RFC 3629 defines it, which storage.md F8
   requires of names. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store_path.h"

#define FIRST_PAIR_ARGUMENT 3
#define NAR_ALIGNMENT 8
#define LENGTH_FIELD 8
#define TABLE_ALIGNMENT 8
#define NARINFO_LINE_MAX 65536
#define NAR_HASH_PREFIX "sha256:"
#define CA_PREFIX "fixed:r:sha256:"
#define TREE_LEVEL_DIGESTS (STORE_BLOCK_SIZE / STORE_DIGEST_SIZE)

/* UTF-8 (RFC 3629, 4): the lead bytes of 2, 3 and 4-byte sequences, the
   continuation bytes' form, and the code points they may encode. */
#define UTF8_CONTINUATION_MASK 0xc0
#define UTF8_CONTINUATION 0x80
#define UTF8_MAX 0x10ffff
#define UTF8_SURROGATE_FIRST 0xd800
#define UTF8_SURROGATE_LAST 0xdfff

struct buffer {
    uint8_t *bytes;
    size_t size, capacity;
};

struct tree;

struct entry {
    const uint8_t *name;
    size_t length;
    struct tree *node;
};

struct tree {
    int kind;
    int executable;
    const uint8_t *data; /* a file's bytes or a link's target, in the NAR */
    size_t size;
    struct entry *entries;
    size_t count;
    uint8_t digest[STORE_DIGEST_SIZE];
    uint8_t blob[STORE_DIGEST_SIZE];
    struct buffer *encoding; /* what digest is the SHA-256 of */
    struct buffer *tree;     /* a file's blob's hash tree */
};

struct path {
    char base[STORE_BASE_NAME_MAX + 1];
    uint8_t *nar;
    size_t nar_size;
    uint8_t nar_hash[SHA256_DIGEST_SIZE];
    char *references; /* the narinfo's References value */
    uint32_t *indices;
    size_t reference_count;
    char *deriver;
    char *ca;
    struct tree *root;
};

/* A record the image holds once: a node's encoding, or a blob's bytes
   and its hash tree. A table's records are in the order they first
   occur until it is written. */
struct record {
    uint8_t digest[STORE_DIGEST_SIZE];
    const uint8_t *bytes; /* a blob's data */
    size_t length;
    const struct buffer *encoding; /* a node's encoding; a blob's tree */
    uint64_t offset; /* a node's encoding in its table; a blob's data */
    uint64_t tree_offset;
};

struct table {
    struct record *records;
    size_t count, capacity;
};

static struct table nodes, blobs;

static void die(const char *what, const char *detail)
{
    fprintf(stderr, "mkstore: %s%s%s\n", what, detail ? ": " : "",
            detail ? detail : "");
    exit(1);
}

static void *allocate(size_t size)
{
    void *p = calloc(1, size ? size : 1);

    if (!p)
        die("out of memory", 0);
    return p;
}

static void append(struct buffer *b, const void *data, size_t size)
{
    if (size > b->capacity - b->size) {
        size_t capacity = b->capacity ? b->capacity : STORE_BLOCK_SIZE;

        while (capacity - b->size < size)
            capacity *= 2;
        b->bytes = realloc(b->bytes, capacity);
        if (!b->bytes)
            die("out of memory", 0);
        b->capacity = capacity;
    }
    memcpy(b->bytes + b->size, data, size);
    b->size += size;
}

static void put32(uint8_t *p, uint32_t v)
{
    int i;

    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void append32(struct buffer *b, uint32_t v)
{
    uint8_t bytes[4];

    put32(bytes, v);
    append(b, bytes, sizeof bytes);
}

static void append64(struct buffer *b, uint64_t v)
{
    uint8_t bytes[8];

    put64(bytes, v);
    append(b, bytes, sizeof bytes);
}

static void pad(struct buffer *b, size_t alignment)
{
    static const uint8_t zero;

    while (b->size % alignment)
        append(b, &zero, 1);
}

static void sha256(const void *data, size_t size,
                   uint8_t digest[SHA256_DIGEST_SIZE])
{
    SHA256_CONTEXT context;

    sha256_init(&context);
    sha256_update(&context, data, size);
    sha256_final(&context, digest);
}

static uint8_t *read_file(const char *path, size_t *size)
{
    struct buffer b = {0, 0, 0};
    uint8_t chunk[STORE_BLOCK_SIZE];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        die(path, strerror(errno));
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        append(&b, chunk, n);
    if (ferror(f))
        die(path, strerror(errno));
    fclose(f);
    append(&b, "", 1);
    *size = b.size - 1;
    return b.bytes;
}

/* ------------------------------------------------------------------------
   The narinfo. */

static char *copy_string(const char *text, size_t length)
{
    char *copy = allocate(length + 1);

    memcpy(copy, text, length);
    return copy;
}

/* The value of key in the narinfo text, or 0: "Key: value" lines, the
   last one with key, as nova-cache's parser takes them. */
static char *field(const char *text, const char *key)
{
    size_t key_length = strlen(key);
    const char *line = text, *end, *value;
    char *found = 0;

    while (*line) {
        end = strchr(line, '\n');
        if (!end)
            end = line + strlen(line);
        if ((size_t)(end - line) > key_length + 1 &&
            !strncmp(line, key, key_length) && line[key_length] == ':') {
            value = line + key_length + 1;
            if (*value == ' ')
                value++;
            free(found);
            found = copy_string(value, (size_t)(end - value -
                                                (end[-1] == '\r')));
        }
        line = *end ? end + 1 : end;
    }
    return found;
}

static void read_narinfo(const char *file, struct path *path)
{
    size_t size, base_length;
    char *text = (char *)read_file(file, &size);
    char *store_path = field(text, "StorePath");
    char *nar_hash = field(text, "NarHash");
    char *nar_size = field(text, "NarSize");
    char *compression = field(text, "Compression");
    char *url = field(text, "URL");
    char *end;

    if (!store_path || !nar_hash || !nar_size || !url)
        die("a narinfo lacks StorePath, URL, NarHash or NarSize", file);
    if (strlen(text) != size)
        die("a narinfo holds a NUL", file);
    if (!compression || strcmp(compression, "none"))
        die("the NAR is compressed", file);
    if (strncmp(store_path, NIX_STORE_PREFIX, strlen(NIX_STORE_PREFIX)))
        die("not a path in " NIX_STORE_DIR, store_path);
    base_length = strlen(store_path) - strlen(NIX_STORE_PREFIX);
    if (!store_base_name_valid(store_path + strlen(NIX_STORE_PREFIX),
                               base_length))
        die("not a store path", store_path);
    memcpy(path->base, store_path + strlen(NIX_STORE_PREFIX), base_length);
    if (strncmp(nar_hash, NAR_HASH_PREFIX, strlen(NAR_HASH_PREFIX)) ||
        !nix32_decode(nar_hash + strlen(NAR_HASH_PREFIX),
                      strlen(nar_hash) - strlen(NAR_HASH_PREFIX),
                      path->nar_hash, sizeof path->nar_hash))
        die("NarHash is not a SHA-256 in nix-base32", file);
    errno = 0;
    path->nar_size = (size_t)strtoull(nar_size, &end, 10);
    if (errno || end == nar_size || *end)
        die("NarSize is not a number", file);
    path->references = field(text, "References");
    if (!path->references)
        path->references = copy_string("", 0);
    path->deriver = field(text, "Deriver");
    if (path->deriver &&
        !store_base_name_valid(path->deriver, strlen(path->deriver)))
        die("Deriver is not a base name", file);
    path->ca = field(text, "CA");
    free(store_path);
    free(nar_hash);
    free(nar_size);
    free(compression);
    free(url);
    free(text);
}

/* ------------------------------------------------------------------------
   The NAR. */

struct reader {
    const uint8_t *nar;
    size_t size, at;
    const char *file;
};

static void bad_nar(const struct reader *r, const char *what)
{
    die(what, r->file);
}

/* A NAR string: an 8-byte little-endian length, the bytes, and zero
   padding to a multiple of 8. */
static const uint8_t *string(struct reader *r, size_t *length)
{
    const uint8_t *bytes;
    uint64_t n = 0;
    size_t padding, i;

    if (r->size - r->at < LENGTH_FIELD)
        bad_nar(r, "the NAR ends early");
    for (i = 0; i < LENGTH_FIELD; i++)
        n |= (uint64_t)r->nar[r->at + i] << (8 * i);
    r->at += LENGTH_FIELD;
    padding = (size_t)((NAR_ALIGNMENT - n % NAR_ALIGNMENT) % NAR_ALIGNMENT);
    if (n > r->size - r->at || padding > r->size - r->at - n)
        bad_nar(r, "the NAR ends early");
    bytes = r->nar + r->at;
    for (i = 0; i < padding; i++)
        if (bytes[n + i])
            bad_nar(r, "the NAR's padding is not zero");
    r->at += (size_t)n + padding;
    *length = (size_t)n;
    return bytes;
}

static int is_token(const uint8_t *bytes, size_t length, const char *token)
{
    return length == strlen(token) && !memcmp(bytes, token, length);
}

static void expect(struct reader *r, const char *token)
{
    size_t length;
    const uint8_t *bytes = string(r, &length);

    if (!is_token(bytes, length, token))
        bad_nar(r, "the NAR breaks its grammar");
}

/* Well-formed UTF-8: no overlong form, no surrogate, nothing past
   U+10FFFF. */
static int utf8(const uint8_t *s, size_t length)
{
    size_t i = 0, n, k;
    uint32_t c, least;

    while (i < length) {
        if (s[i] < 0x80) {
            i++;
            continue;
        }
        if ((s[i] & 0xe0) == 0xc0) {
            n = 1;
            c = s[i] & 0x1f;
            least = 0x80;
        } else if ((s[i] & 0xf0) == 0xe0) {
            n = 2;
            c = s[i] & 0x0f;
            least = 0x800;
        } else if ((s[i] & 0xf8) == 0xf0) {
            n = 3;
            c = s[i] & 0x07;
            least = 0x10000;
        } else {
            return 0;
        }
        if (n > length - i - 1)
            return 0;
        for (k = 1; k <= n; k++) {
            if ((s[i + k] & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION)
                return 0;
            c = c << 6 | (s[i + k] & 0x3f);
        }
        if (c < least || c > UTF8_MAX ||
            (c >= UTF8_SURROGATE_FIRST && c <= UTF8_SURROGATE_LAST))
            return 0;
        i += n + 1;
    }
    return 1;
}

static int compare_bytes(const uint8_t *a, size_t a_length, const uint8_t *b,
                         size_t b_length)
{
    int c = memcmp(a, b, a_length < b_length ? a_length : b_length);

    if (c)
        return c;
    return (a_length > b_length) - (a_length < b_length);
}

/* A name is not empty, ".", or "..", holds no '/' or NUL, follows the
   one before it in bytewise order, is UTF-8, and is no longer than
   STORE_ENTRY_NAME_MAX. */
static void check_name(const struct reader *r, const uint8_t *name,
                       size_t length, const struct entry *previous)
{
    if (!length || (length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.') ||
        memchr(name, '/', length) || memchr(name, 0, length))
        bad_nar(r, "a NAR entry name is not a name");
    if (previous &&
        compare_bytes(previous->name, previous->length, name, length) >= 0)
        bad_nar(r, "the NAR's entries are not in bytewise order");
    if (!utf8(name, length))
        bad_nar(r, "a NAR entry name is not UTF-8");
    if (length > STORE_ENTRY_NAME_MAX)
        bad_nar(r, "a NAR entry name is too long");
}

static struct tree *parse_node(struct reader *r)
{
    struct tree *node = allocate(sizeof *node);
    struct buffer entries = {0, 0, 0};
    const uint8_t *bytes;
    size_t length;

    expect(r, "(");
    expect(r, "type");
    bytes = string(r, &length);
    if (is_token(bytes, length, "regular")) {
        node->kind = STORE_NODE_FILE;
        bytes = string(r, &length);
        if (is_token(bytes, length, "executable")) {
            expect(r, "");
            node->executable = 1;
            bytes = string(r, &length);
        }
        if (!is_token(bytes, length, "contents"))
            bad_nar(r, "the NAR breaks its grammar");
        node->data = string(r, &node->size);
        expect(r, ")");
    } else if (is_token(bytes, length, "symlink")) {
        node->kind = STORE_NODE_LINK;
        expect(r, "target");
        node->data = string(r, &node->size);
        if (!node->size || node->size > STORE_TARGET_MAX ||
            memchr(node->data, 0, node->size))
            bad_nar(r, "a link's target is empty, too long or holds a NUL");
        expect(r, ")");
    } else if (is_token(bytes, length, "directory")) {
        node->kind = STORE_NODE_DIRECTORY;
        for (;;) {
            struct entry entry;

            bytes = string(r, &length);
            if (is_token(bytes, length, ")"))
                break;
            if (!is_token(bytes, length, "entry"))
                bad_nar(r, "the NAR breaks its grammar");
            expect(r, "(");
            expect(r, "name");
            entry.name = string(r, &entry.length);
            check_name(r, entry.name, entry.length,
                       node->count ? (const struct entry *)entries.bytes +
                                         node->count - 1
                                   : 0);
            expect(r, "node");
            entry.node = parse_node(r);
            expect(r, ")");
            append(&entries, &entry, sizeof entry);
            node->count++;
        }
        node->entries = (struct entry *)entries.bytes;
    } else {
        bad_nar(r, "the NAR breaks its grammar");
    }
    return node;
}

static void read_nar(const char *file, struct path *path)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    struct reader r;

    path->nar = read_file(file, &r.size);
    if (r.size != path->nar_size)
        die("the NAR's size is not the narinfo's NarSize", file);
    sha256(path->nar, r.size, digest);
    if (memcmp(digest, path->nar_hash, sizeof digest))
        die("the NAR's SHA-256 is not the narinfo's NarHash", file);
    r.nar = path->nar;
    r.at = 0;
    r.file = file;
    expect(&r, "nix-archive-1");
    path->root = parse_node(&r);
    if (r.at != r.size)
        bad_nar(&r, "the NAR has bytes after its root");
}

/* ------------------------------------------------------------------------
   The paths. */

static int compare_paths(const void *a, const void *b)
{
    return strcmp(((const struct path *)a)->base,
                  ((const struct path *)b)->base);
}

static int compare_key(const void *key, const void *p)
{
    return strcmp((const char *)key, ((const struct path *)p)->base);
}

/* Each reference names a path given; the indices go in ascending order,
   which is bytewise order of base name. */
static void resolve(struct path *paths, size_t count, struct path *path)
{
    char *references = path->references, *word;
    struct path *found;
    size_t i, j;

    path->indices = allocate((strlen(references) / 2 + 1) * sizeof(uint32_t));
    for (word = strtok(references, " "); word; word = strtok(0, " ")) {
        found = bsearch(word, paths, count, sizeof *paths, compare_key);
        if (!found)
            die("a reference is not among the paths given", word);
        path->indices[path->reference_count++] = (uint32_t)(found - paths);
    }
    for (i = 1; i < path->reference_count; i++)
        for (j = i; j > 0 && path->indices[j - 1] > path->indices[j]; j--) {
            uint32_t t = path->indices[j];

            path->indices[j] = path->indices[j - 1];
            path->indices[j - 1] = t;
        }
    for (i = 1; i < path->reference_count; i++)
        if (path->indices[i] == path->indices[i - 1])
            die("a reference is listed twice", path->base);
}

/* A content-addressed path's hash part is computed from its NAR hash and
   its references, a reference to itself as ":self". */
static void check_content_address(const struct path *paths, size_t self,
                                  const struct path *path)
{
    struct buffer type = {0, 0, 0};
    uint8_t hash[SHA256_DIGEST_SIZE];
    char computed[STORE_HASH_CHARS + 1];
    const char *ca = path->ca;
    int has_self = 0;
    size_t i;

    if (strncmp(ca, CA_PREFIX, strlen(CA_PREFIX)) ||
        !nix32_decode(ca + strlen(CA_PREFIX), strlen(ca) - strlen(CA_PREFIX),
                      hash, sizeof hash))
        die("an unsupported content address", ca);
    if (memcmp(hash, path->nar_hash, sizeof hash))
        die("the content address is not the NAR hash", path->base);
    append(&type, "source", strlen("source"));
    for (i = 0; i < path->reference_count; i++) {
        if (path->indices[i] == self) {
            has_self = 1;
            continue;
        }
        append(&type, ":" NIX_STORE_PREFIX, strlen(":" NIX_STORE_PREFIX));
        append(&type, paths[path->indices[i]].base,
               strlen(paths[path->indices[i]].base));
    }
    if (has_self)
        append(&type, ":self", strlen(":self"));
    append(&type, "", 1);
    store_path_hash((const char *)type.bytes, hash,
                    path->base + STORE_HASH_CHARS + 1, computed);
    if (memcmp(computed, path->base, STORE_HASH_CHARS))
        die("the path's name is not the one its content address gives",
            path->base);
    free(type.bytes);
}

/* ------------------------------------------------------------------------
   Nodes and blobs. */

/* The digest of one block of a blob's hash tree. */
static void block_digest(uint32_t level, uint64_t offset, const uint8_t *bytes,
                         size_t length, uint8_t digest[STORE_DIGEST_SIZE])
{
    uint8_t prefix[STORE_TREE_PREFIX_SIZE];
    SHA256_CONTEXT context;

    put64(prefix, offset);
    put32(prefix + 8, level);
    put32(prefix + 12, (uint32_t)length);
    sha256_init(&context);
    sha256_update(&context, prefix, sizeof prefix);
    sha256_update(&context, bytes, length);
    sha256_final(&context, digest);
}

/* A blob's hash tree, every level of more than one digest from the
   blocks' up, and its digest: the SHA-256 of its length and the single
   digest at the top. */
static void hash_blob(const uint8_t *data, size_t length, struct buffer *tree,
                      uint8_t digest[STORE_DIGEST_SIZE])
{
    struct buffer level = {0, 0, 0}, next = {0, 0, 0};
    uint8_t d[STORE_DIGEST_SIZE], top[STORE_DIGEST_SIZE + LENGTH_FIELD];
    size_t count = length ? (length + STORE_BLOCK_SIZE - 1) / STORE_BLOCK_SIZE
                          : 1;
    size_t i, part;
    uint32_t height = 0;

    for (i = 0; i < count; i++) {
        part = length - i * STORE_BLOCK_SIZE < STORE_BLOCK_SIZE
                   ? length - i * STORE_BLOCK_SIZE
                   : STORE_BLOCK_SIZE;
        block_digest(0, (uint64_t)i * STORE_BLOCK_SIZE,
                     data + i * STORE_BLOCK_SIZE, part, d);
        append(&level, d, sizeof d);
    }
    while (count > 1) {
        append(tree, level.bytes, level.size);
        height++;
        next.size = 0;
        for (i = 0; i * STORE_BLOCK_SIZE < level.size; i++) {
            part = level.size - i * STORE_BLOCK_SIZE < STORE_BLOCK_SIZE
                       ? level.size - i * STORE_BLOCK_SIZE
                       : STORE_BLOCK_SIZE;
            block_digest(height, (uint64_t)i * STORE_BLOCK_SIZE,
                         level.bytes + i * STORE_BLOCK_SIZE, part, d);
            append(&next, d, sizeof d);
        }
        count = next.size / STORE_DIGEST_SIZE;
        level.size = 0;
        append(&level, next.bytes, next.size);
    }
    put64(top, length);
    memcpy(top + LENGTH_FIELD, level.bytes, STORE_DIGEST_SIZE);
    sha256(top, sizeof top, digest);
    free(level.bytes);
    free(next.bytes);
}

static struct record *find_record(const struct table *t,
                                  const uint8_t *digest)
{
    size_t i;

    for (i = 0; i < t->count; i++)
        if (!memcmp(t->records[i].digest, digest, STORE_DIGEST_SIZE))
            return &t->records[i];
    return 0;
}

static struct record *add_record(struct table *t, const uint8_t *digest)
{
    struct record *r;

    if (t->count == t->capacity) {
        t->capacity = t->capacity ? t->capacity * 2 : 64;
        t->records = realloc(t->records, t->capacity * sizeof *t->records);
        if (!t->records)
            die("out of memory", 0);
    }
    r = &t->records[t->count++];
    memset(r, 0, sizeof *r);
    memcpy(r->digest, digest, STORE_DIGEST_SIZE);
    return r;
}

/* Encode node, its children first, since a directory's encoding names
   their digests, and name it by its encoding's SHA-256. */
static void encode(struct tree *node)
{
    struct buffer *out = allocate(sizeof *out);
    uint8_t byte = (uint8_t)node->kind;
    size_t i;

    append(out, &byte, 1);
    switch (node->kind) {
    case STORE_NODE_DIRECTORY:
        append32(out, (uint32_t)node->count);
        for (i = 0; i < node->count; i++) {
            encode(node->entries[i].node);
            append32(out, (uint32_t)node->entries[i].length);
            append(out, node->entries[i].name, node->entries[i].length);
            append(out, node->entries[i].node->digest, STORE_DIGEST_SIZE);
        }
        break;
    case STORE_NODE_FILE:
        node->tree = allocate(sizeof *node->tree);
        hash_blob(node->data, node->size, node->tree, node->blob);
        byte = (uint8_t)node->executable;
        append(out, &byte, 1);
        append64(out, node->size);
        append(out, node->blob, STORE_DIGEST_SIZE);
        break;
    case STORE_NODE_LINK:
        append32(out, (uint32_t)node->size);
        append(out, node->data, node->size);
        break;
    }
    node->encoding = out;
    sha256(out->bytes, out->size, node->digest);
}

/* Add node and what it names to the tables where they first occur, in
   NAR order: the node, then its blob or its entries. */
static void place(const struct tree *node)
{
    struct record *r;
    size_t i;

    if (!find_record(&nodes, node->digest)) {
        r = add_record(&nodes, node->digest);
        r->encoding = node->encoding;
    }
    if (node->kind == STORE_NODE_FILE && !find_record(&blobs, node->blob)) {
        r = add_record(&blobs, node->blob);
        r->bytes = node->data;
        r->length = node->size;
        r->encoding = node->tree;
    }
    for (i = 0; node->kind == STORE_NODE_DIRECTORY && i < node->count; i++)
        place(node->entries[i].node);
}

static int compare_records(const void *a, const void *b)
{
    return memcmp(((const struct record *)a)->digest,
                  ((const struct record *)b)->digest, STORE_DIGEST_SIZE);
}

/* The table's records in digest order, for its index; the table keeps
   the order they first occur in. */
static struct record *by_digest(const struct table *t)
{
    struct record *sorted = allocate(t->count * sizeof *sorted);

    memcpy(sorted, t->records, t->count * sizeof *sorted);
    qsort(sorted, t->count, sizeof *sorted, compare_records);
    return sorted;
}

/* ------------------------------------------------------------------------
   The image. */

/* The registration table: entries, then each path's name, references,
   deriver and content address. */
static void write_registrations(const struct path *paths, size_t count,
                                struct buffer *out)
{
    struct buffer strings = {0, 0, 0};
    size_t start = out->size, i, j;

    for (i = 0; i < count; i++) {
        uint8_t entry[STORE_REGISTRATION_SIZE];
        const struct path *p = &paths[i];
        size_t base = count * STORE_REGISTRATION_SIZE;

        memset(entry, 0, sizeof entry);
        memcpy(entry + STORE_REGISTRATION_ROOT, p->root->digest,
               STORE_DIGEST_SIZE);
        memcpy(entry + STORE_REGISTRATION_NAR_HASH, p->nar_hash,
               STORE_DIGEST_SIZE);
        put64(entry + STORE_REGISTRATION_NAR_SIZE, p->nar_size);
        put32(entry + STORE_REGISTRATION_NAME,
              (uint32_t)(base + strings.size));
        put32(entry + STORE_REGISTRATION_NAME_LENGTH,
              (uint32_t)strlen(p->base));
        append(&strings, p->base, strlen(p->base));
        pad(&strings, STORE_REFERENCE_SIZE);
        put32(entry + STORE_REGISTRATION_REFERENCES,
              (uint32_t)(base + strings.size));
        put32(entry + STORE_REGISTRATION_REFERENCE_COUNT,
              (uint32_t)p->reference_count);
        for (j = 0; j < p->reference_count; j++)
            append32(&strings, p->indices[j]);
        if (p->deriver) {
            put32(entry + STORE_REGISTRATION_DERIVER,
                  (uint32_t)(base + strings.size));
            put32(entry + STORE_REGISTRATION_DERIVER_LENGTH,
                  (uint32_t)strlen(p->deriver));
            append(&strings, p->deriver, strlen(p->deriver));
        }
        if (p->ca) {
            put32(entry + STORE_REGISTRATION_CA,
                  (uint32_t)(base + strings.size));
            put32(entry + STORE_REGISTRATION_CA_LENGTH,
                  (uint32_t)strlen(p->ca));
            append(&strings, p->ca, strlen(p->ca));
        }
        append(out, entry, sizeof entry);
    }
    append(out, strings.bytes, strings.size);
    pad(out, TABLE_ALIGNMENT);
    if (out->size - start > UINT32_MAX)
        die("the registration table is too large", 0);
    free(strings.bytes);
}

/* The node table: the index in digest order, then the encodings in the
   order they first occur. */
static void write_nodes(struct buffer *out)
{
    size_t at = nodes.count * STORE_NODE_ENTRY_SIZE, i;
    struct record *sorted;

    for (i = 0; i < nodes.count; i++) {
        nodes.records[i].offset = at;
        at += nodes.records[i].encoding->size;
    }
    if (at > UINT32_MAX)
        die("the node table is too large", 0);
    sorted = by_digest(&nodes);
    for (i = 0; i < nodes.count; i++) {
        uint8_t entry[STORE_NODE_ENTRY_SIZE];

        memcpy(entry + STORE_NODE_ENTRY_DIGEST, sorted[i].digest,
               STORE_DIGEST_SIZE);
        put32(entry + STORE_NODE_ENTRY_OFFSET, (uint32_t)sorted[i].offset);
        put32(entry + STORE_NODE_ENTRY_LENGTH,
              (uint32_t)sorted[i].encoding->size);
        append(out, entry, sizeof entry);
    }
    for (i = 0; i < nodes.count; i++)
        append(out, nodes.records[i].encoding->bytes,
               nodes.records[i].encoding->size);
    pad(out, TABLE_ALIGNMENT);
    free(sorted);
}

/* The blob table, then the hash trees, then from a block boundary each
   blob's data, a whole number of blocks, the trees and the data in the
   order the blobs first occur. Returns the table's size. */
static size_t write_blobs(struct buffer *out)
{
    size_t at = out->size + blobs.count * STORE_BLOB_ENTRY_SIZE, i;
    struct record *sorted;

    for (i = 0; i < blobs.count; i++) {
        blobs.records[i].tree_offset = at;
        at += blobs.records[i].encoding->size;
    }
    at = (at + STORE_BLOCK_SIZE - 1) / STORE_BLOCK_SIZE * STORE_BLOCK_SIZE;
    for (i = 0; i < blobs.count; i++) {
        blobs.records[i].offset = at;
        at += (blobs.records[i].length + STORE_BLOCK_SIZE - 1) /
              STORE_BLOCK_SIZE * STORE_BLOCK_SIZE;
    }
    sorted = by_digest(&blobs);
    for (i = 0; i < blobs.count; i++) {
        uint8_t entry[STORE_BLOB_ENTRY_SIZE];

        memcpy(entry + STORE_BLOB_DIGEST, sorted[i].digest, STORE_DIGEST_SIZE);
        put64(entry + STORE_BLOB_LENGTH, sorted[i].length);
        put64(entry + STORE_BLOB_TREE, sorted[i].tree_offset);
        put64(entry + STORE_BLOB_DATA, sorted[i].offset);
        append(out, entry, sizeof entry);
    }
    free(sorted);
    for (i = 0; i < blobs.count; i++)
        append(out, blobs.records[i].encoding->bytes,
               blobs.records[i].encoding->size);
    pad(out, STORE_BLOCK_SIZE);
    for (i = 0; i < blobs.count; i++) {
        append(out, blobs.records[i].bytes, blobs.records[i].length);
        pad(out, STORE_BLOCK_SIZE);
    }
    return blobs.count * STORE_BLOB_ENTRY_SIZE;
}

int main(int argc, char **argv)
{
    struct buffer image = {0, 0, 0};
    struct path *paths, *boot;
    size_t count, i, registrations, nodes_at, blobs_at, blob_table_size;
    uint8_t *header, digest[SHA256_DIGEST_SIZE];
    FILE *f;

    if (argc < FIRST_PAIR_ARGUMENT + 2 || (argc - FIRST_PAIR_ARGUMENT) % 2) {
        fprintf(stderr,
                "usage: mkstore IMAGE BOOT NARINFO NAR [NARINFO NAR]...\n");
        return 2;
    }
    count = (size_t)(argc - FIRST_PAIR_ARGUMENT) / 2;
    paths = allocate(count * sizeof *paths);
    for (i = 0; i < count; i++) {
        read_narinfo(argv[FIRST_PAIR_ARGUMENT + 2 * i], &paths[i]);
        read_nar(argv[FIRST_PAIR_ARGUMENT + 2 * i + 1], &paths[i]);
    }
    qsort(paths, count, sizeof *paths, compare_paths);
    for (i = 1; i < count; i++)
        if (!strcmp(paths[i - 1].base, paths[i].base))
            die("a path is given twice", paths[i].base);
    for (i = 0; i < count; i++)
        resolve(paths, count, &paths[i]);
    for (i = 0; i < count; i++)
        if (paths[i].ca)
            check_content_address(paths, i, &paths[i]);
    boot = bsearch(argv[2], paths, count, sizeof *paths, compare_key);
    if (!boot)
        die("the boot path is not among the paths given", argv[2]);

    for (i = 0; i < count; i++) {
        encode(paths[i].root);
        place(paths[i].root);
    }

    header = allocate(STORE_HEADER_SIZE);
    append(&image, header, STORE_HEADER_SIZE);
    free(header);
    pad(&image, TABLE_ALIGNMENT);
    registrations = image.size;
    write_registrations(paths, count, &image);
    nodes_at = image.size;
    write_nodes(&image);
    blobs_at = image.size;
    blob_table_size = write_blobs(&image);

    header = image.bytes;
    memcpy(header + STORE_HEADER_MAGIC, STORE_MAGIC, STORE_MAGIC_SIZE);
    put32(header + STORE_HEADER_VERSION, STORE_VERSION);
    put32(header + STORE_HEADER_REGISTRATION_COUNT, (uint32_t)count);
    put32(header + STORE_HEADER_NODE_COUNT, (uint32_t)nodes.count);
    put32(header + STORE_HEADER_BLOB_COUNT, (uint32_t)blobs.count);
    put64(header + STORE_HEADER_IMAGE_SIZE, image.size);
    put64(header + STORE_HEADER_REGISTRATIONS, registrations);
    put64(header + STORE_HEADER_REGISTRATIONS_SIZE, nodes_at - registrations);
    put64(header + STORE_HEADER_NODES, nodes_at);
    put64(header + STORE_HEADER_NODES_SIZE, blobs_at - nodes_at);
    put64(header + STORE_HEADER_BLOBS, blobs_at);
    put64(header + STORE_HEADER_BLOBS_SIZE, blob_table_size);
    sha256(image.bytes + registrations, nodes_at - registrations, digest);
    memcpy(header + STORE_HEADER_REGISTRATIONS_DIGEST, digest, sizeof digest);
    put32(header + STORE_HEADER_BOOT_NAME_LENGTH, (uint32_t)strlen(boot->base));
    memcpy(header + STORE_HEADER_BOOT_NAME, boot->base, strlen(boot->base));

    f = fopen(argv[1], "wb");
    if (!f || fwrite(image.bytes, 1, image.size, f) != image.size || fclose(f))
        die(argv[1], strerror(errno));
    return 0;
}
