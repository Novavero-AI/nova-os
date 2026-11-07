/* mknar: writes a store path as nova-nix exports one, a NAR and a
   narinfo, from a tree described on the command line, and prints the
   path's base name. It stands in for nova-nix's export where nova-nix
   does not run: the path is content-addressed, named as upstream Nix
   names a recursive SHA-256 path with references (makeFixedOutputPath:
   type "source", then ":" and each reference's path), which for a path
   without references is nova-nix's makeFixedOutputPath too. Its
   references are the candidates whose hash part occurs in the NAR, the
   scan nova-nix's registration makes (docs/spec/storage.md, fact 5). A
   path cannot refer to itself, since its name is computed after its
   contents.

   usage: mknar OUT NAME [-r CANDIDATE]... NODE...

   NODE is "dir PATH", "file PATH FILE", "exec PATH FILE" or
   "link PATH TARGET": a directory, a file holding FILE's bytes, the
   same with the executable flag, or a symbolic link. PATH "." is the
   root; without one the root is a directory. Every other PATH is
   '/'-separated below the root, and its parent is a directory given.
   CANDIDATE is a base name. The tool writes OUT.nar and OUT.narinfo,
   whose URL is the NAR's file name.

   Formats: the NAR grammar of nova-cache's NovaCache.NAR; the narinfo
   keys of NovaCache.NarInfo; the content address "fixed:r:sha256:" and
   a nix-base32 hash (upstream content-address.cc). */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store_path.h"

#define NAR_ALIGNMENT 8
#define LENGTH_FIELD 8
#define FIRST_NODE_ARGUMENT 3
#define CANDIDATES_MAX 64
#define NODES_MAX 256

enum kind { DIRECTORY, REGULAR, EXECUTABLE, LINK };

struct node {
    const char *path;   /* "." for the root */
    enum kind kind;
    const char *source; /* a file's FILE, a link's TARGET */
};

struct buffer {
    uint8_t *bytes;
    size_t size, capacity;
};

static struct node nodes[NODES_MAX];
static size_t node_count;

static void die(const char *what, const char *detail)
{
    fprintf(stderr, "mknar: %s%s%s\n", what, detail ? ": " : "",
            detail ? detail : "");
    exit(1);
}

static void append(struct buffer *b, const void *data, size_t size)
{
    if (size > b->capacity - b->size) {
        size_t capacity = b->capacity ? b->capacity : 4096;

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

/* A NAR string: its length as 8 little-endian bytes, the bytes, and
   zeros to the next multiple of 8. */
static void nar_bytes(struct buffer *b, const void *data, size_t size)
{
    static const uint8_t zeros[NAR_ALIGNMENT];
    uint8_t length[LENGTH_FIELD];
    size_t i;

    for (i = 0; i < LENGTH_FIELD; i++)
        length[i] = (uint8_t)((uint64_t)size >> (i * 8));
    append(b, length, sizeof length);
    append(b, data, size);
    append(b, zeros, (NAR_ALIGNMENT - size % NAR_ALIGNMENT) % NAR_ALIGNMENT);
}

static void nar_token(struct buffer *b, const char *token)
{
    nar_bytes(b, token, strlen(token));
}

static void read_file(const char *path, struct buffer *out)
{
    FILE *f = fopen(path, "rb");
    uint8_t chunk[4096];
    size_t n;

    if (!f)
        die(path, strerror(errno));
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        append(out, chunk, n);
    if (ferror(f))
        die(path, strerror(errno));
    fclose(f);
}

/* stem then suffix, written to out, which holds size bytes. Not
   snprintf: msvcrt.dll, the C library TinyCC links on Windows, has
   none. */
static void file_name(char *out, size_t size, const char *stem,
                      const char *suffix)
{
    size_t stem_size = strlen(stem), suffix_size = strlen(suffix);

    if (stem_size + suffix_size >= size)
        die("name too long", stem);
    memcpy(out, stem, stem_size);
    memcpy(out + stem_size, suffix, suffix_size + 1);
}

/* The parent directory of path, written to parent, or "." for a
   top-level path; and its last component. */
static const char *split(const char *path, char *parent, size_t size)
{
    const char *slash = strrchr(path, '/');

    if (!slash) {
        strcpy(parent, ".");
        return path;
    }
    if ((size_t)(slash - path) >= size)
        die("path too long", path);
    memcpy(parent, path, (size_t)(slash - path));
    parent[slash - path] = 0;
    return slash + 1;
}

static const struct node *find(const char *path)
{
    size_t i;

    for (i = 0; i < node_count; i++)
        if (!strcmp(nodes[i].path, path))
            return &nodes[i];
    return 0;
}

static int compare_names(const void *a, const void *b)
{
    char pa[FILENAME_MAX], pb[FILENAME_MAX];
    const char *na = split((*(const struct node *const *)a)->path, pa,
                           sizeof pa);
    const char *nb = split((*(const struct node *const *)b)->path, pb,
                           sizeof pb);

    return strcmp(na, nb);
}

static void nar_node(struct buffer *b, const struct node *node);

/* A directory's entries in bytewise order of name, as the NAR grammar
   requires; strcmp compares as unsigned char, which is bytewise. */
static void nar_directory(struct buffer *b, const char *path)
{
    const struct node *children[NODES_MAX];
    char parent[FILENAME_MAX];
    size_t count = 0, i;

    for (i = 0; i < node_count; i++) {
        if (!strcmp(nodes[i].path, "."))
            continue;
        split(nodes[i].path, parent, sizeof parent);
        if (!strcmp(parent, path))
            children[count++] = &nodes[i];
    }
    qsort(children, count, sizeof children[0], compare_names);
    for (i = 0; i < count; i++) {
        nar_token(b, "entry");
        nar_token(b, "(");
        nar_token(b, "name");
        nar_token(b, split(children[i]->path, parent, sizeof parent));
        nar_token(b, "node");
        nar_node(b, children[i]);
        nar_token(b, ")");
    }
}

static void nar_node(struct buffer *b, const struct node *node)
{
    struct buffer contents = {0, 0, 0};

    nar_token(b, "(");
    nar_token(b, "type");
    switch (node->kind) {
    case DIRECTORY:
        nar_token(b, "directory");
        nar_directory(b, node->path);
        break;
    case EXECUTABLE:
    case REGULAR:
        nar_token(b, "regular");
        if (node->kind == EXECUTABLE) {
            nar_token(b, "executable");
            nar_token(b, "");
        }
        nar_token(b, "contents");
        read_file(node->source, &contents);
        nar_bytes(b, contents.bytes, contents.size);
        free(contents.bytes);
        break;
    case LINK:
        nar_token(b, "symlink");
        nar_token(b, "target");
        nar_token(b, node->source);
        break;
    }
    nar_token(b, ")");
}

/* Each component of a path below the root is a NAR entry name: not
   empty, not "." or "..". */
static void check_path(const char *path)
{
    const char *start = path, *end;

    for (;;) {
        end = strchr(start, '/');
        if (!end)
            end = start + strlen(start);
        if (end == start || (end - start == 1 && start[0] == '.') ||
            (end - start == 2 && start[0] == '.' && start[1] == '.'))
            die("not a path below the root", path);
        if (!*end)
            return;
        start = end + 1;
    }
}

static void add_node(const char *word, const char *path, const char *source)
{
    char parent[FILENAME_MAX];
    const struct node *above;
    struct node *node;

    if (node_count == NODES_MAX)
        die("too many nodes", 0);
    if (find(path))
        die("path given twice", path);
    node = &nodes[node_count];
    node->path = path;
    node->source = source;
    if (!strcmp(word, "dir"))
        node->kind = DIRECTORY;
    else if (!strcmp(word, "file"))
        node->kind = REGULAR;
    else if (!strcmp(word, "exec"))
        node->kind = EXECUTABLE;
    else if (!strcmp(word, "link"))
        node->kind = LINK;
    else
        die("unknown node", word);
    if (node->kind == LINK && !*source)
        die("a link needs a target", path);
    if (strcmp(path, ".")) {
        check_path(path);
        split(path, parent, sizeof parent);
        above = find(parent);
        if (strcmp(parent, ".") && (!above || above->kind != DIRECTORY))
            die("parent is not a directory given before", path);
        if (!strcmp(parent, ".") && find(".") && find(".")->kind != DIRECTORY)
            die("the root is not a directory", path);
    } else if (node_count) {
        die("the root comes first", path);
    }
    node_count++;
}

static int contains(const struct buffer *b, const char *text, size_t length)
{
    size_t i;

    for (i = 0; i + length <= b->size; i++)
        if (!memcmp(b->bytes + i, text, length))
            return 1;
    return 0;
}

static int compare_strings(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(int argc, char **argv)
{
    const char *candidates[CANDIDATES_MAX], *found[CANDIDATES_MAX];
    size_t candidate_count = 0, found_count = 0, i;
    struct buffer nar = {0, 0, 0}, type = {0, 0, 0};
    uint8_t nar_hash[SHA256_DIGEST_SIZE];
    char nix32[NIX32_DIGEST_CHARS + 1], hash[STORE_HASH_CHARS + 1];
    char nar_name[FILENAME_MAX], info_name[FILENAME_MAX];
    const char *name;
    SHA256_CONTEXT context;
    struct node root;
    FILE *f;
    int arg;

    if (argc < FIRST_NODE_ARGUMENT + 1) {
        fprintf(stderr, "usage: mknar OUT NAME [-r CANDIDATE]... NODE...\n");
        return 2;
    }
    name = argv[2];
    if (!store_name_valid(name, strlen(name)))
        die("not a store path name", name);
    for (arg = FIRST_NODE_ARGUMENT; arg < argc; arg++) {
        if (!strcmp(argv[arg], "-r") && arg + 1 < argc) {
            if (candidate_count == CANDIDATES_MAX)
                die("too many candidates", 0);
            if (!store_base_name_valid(argv[arg + 1], strlen(argv[arg + 1])))
                die("not a base name", argv[arg + 1]);
            candidates[candidate_count++] = argv[++arg];
        } else if (!strcmp(argv[arg], "dir") && arg + 1 < argc) {
            add_node(argv[arg], argv[arg + 1], 0);
            arg++;
        } else if (arg + 2 < argc) {
            add_node(argv[arg], argv[arg + 1], argv[arg + 2]);
            arg += 2;
        } else {
            die("incomplete node", argv[arg]);
        }
    }

    nar_token(&nar, "nix-archive-1");
    if (find("."))
        nar_node(&nar, find("."));
    else {
        root.path = ".";
        root.kind = DIRECTORY;
        root.source = 0;
        nar_node(&nar, &root);
    }
    sha256_init(&context);
    sha256_update(&context, nar.bytes, nar.size);
    sha256_final(&context, nar_hash);
    nix32_encode(nar_hash, sizeof nar_hash, nix32);

    qsort(candidates, candidate_count, sizeof candidates[0], compare_strings);
    append(&type, "source", strlen("source"));
    for (i = 0; i < candidate_count; i++)
        if ((!found_count || strcmp(found[found_count - 1], candidates[i])) &&
            contains(&nar, candidates[i], STORE_HASH_CHARS)) {
            found[found_count++] = candidates[i];
            append(&type, ":" NIX_STORE_PREFIX, strlen(":" NIX_STORE_PREFIX));
            append(&type, candidates[i], strlen(candidates[i]));
        }
    append(&type, "", 1);
    store_path_hash((const char *)type.bytes, nar_hash, name, hash);

    file_name(nar_name, sizeof nar_name, argv[1], ".nar");
    file_name(info_name, sizeof info_name, argv[1], ".narinfo");
    f = fopen(nar_name, "wb");
    if (!f || fwrite(nar.bytes, 1, nar.size, f) != nar.size || fclose(f))
        die(nar_name, strerror(errno));
    f = fopen(info_name, "wb");
    if (!f)
        die(info_name, strerror(errno));
    fprintf(f, "StorePath: " NIX_STORE_PREFIX "%s-%s\n", hash, name);
    fprintf(f, "URL: %s\n", strrchr(nar_name, '/') ? strrchr(nar_name, '/') + 1
                                                   : nar_name);
    fprintf(f, "Compression: none\n");
    fprintf(f, "NarHash: sha256:%s\n", nix32);
    fprintf(f, "NarSize: %llu\n", (unsigned long long)nar.size);
    fprintf(f, "References: ");
    for (i = 0; i < found_count; i++)
        fprintf(f, i ? " %s" : "%s", found[i]);
    fprintf(f, "\nCA: fixed:r:sha256:%s\n", nix32);
    if (fclose(f))
        die(info_name, strerror(errno));
    printf("%s-%s\n", hash, name);
    free(nar.bytes);
    free(type.bytes);
    return 0;
}
