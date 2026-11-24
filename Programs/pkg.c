#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#if defined(_WIN32)
/* windows.h는 내부 헤더의 VT_* 열거자와 충돌하므로 필요한 API만 직접 선언한다. */
#include <process.h>
#include <direct.h>
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int cp,
                                                        unsigned long flags,
                                                        const char *s, int slen,
                                                        wchar_t *out, int outlen);
#define PKG_CP_UTF8 65001u
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "internal/pkg.h"

#define MANIFEST "aster.pkg"
#define LOCKFILE "aster.lock"
#define INDEXFILE "index.txt"
#define VENDOR_DIR "vendor"
#define DIST_DIR "dist"

typedef struct {
    const char *alias;
    const char *path;
} Dep;

typedef struct {
    const char *name;
    const char *version;
    const char *registry;
    const char *token_env;
    Dep *deps;
    size_t ndeps, cap;
} Manifest;

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int dir_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *xstrndup(Arena *arena, const char *s, size_t n) {
    return Arena_Strndup(arena, s, n);
}

static char *join_path(Arena *arena, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *p = Arena_Alloc(arena, la + lb + 2);
    memcpy(p, a, la);
    if (la && p[la - 1] != '/' && p[la - 1] != '\\')
        p[la++] = '/';
    memcpy(p + la, b, lb);
    p[la + lb] = 0;
    return p;
}

/* 경로의 부모 디렉터리. 위가 없으면 NULL. */
static char *parent_dir(Arena *arena, const char *path) {
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        n--;
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\')
        n--;
    if (n == 0)
        return NULL;
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        n--;
    return xstrndup(arena, path, n);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *cut = slash > bslash ? slash : bslash;
    return cut ? cut + 1 : path;
}

static int is_absolute(const char *path) {
    if (!path || !path[0])
        return 0;
    if (path[0] == '/' || path[0] == '\\')
        return 1;
    return path[1] == ':';
}

static char *trim(char *s) {
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = 0;
    return s;
}

static char *read_text(Arena *arena, const char *path) {
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    buf = Arena_Alloc(arena, (size_t)size + 1);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return NULL;
    }
    buf[size] = 0;
    fclose(f);
    return buf;
}

static void manifest_push_dep(Arena *arena, Manifest *m, const char *alias,
                              const char *path) {
    size_t i;
    for (i = 0; i < m->ndeps; i++) {
        if (!strcmp(m->deps[i].alias, alias)) {
            m->deps[i].path = path;
            return;
        }
    }
    if (m->ndeps == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 4;
        Dep *grown = Arena_Alloc(arena, cap * sizeof(Dep));
        if (m->deps)
            memcpy(grown, m->deps, m->ndeps * sizeof(Dep));
        m->deps = grown;
        m->cap = cap;
    }
    m->deps[m->ndeps].alias = alias;
    m->deps[m->ndeps].path = path;
    m->ndeps++;
}

static int manifest_remove_dep(Manifest *m, const char *alias) {
    size_t i;
    for (i = 0; i < m->ndeps; i++) {
        if (!strcmp(m->deps[i].alias, alias)) {
            memmove(&m->deps[i], &m->deps[i + 1],
                    (m->ndeps - i - 1) * sizeof(Dep));
            m->ndeps--;
            return 1;
        }
    }
    return 0;
}

static void parse_manifest(Arena *arena, const char *text, Manifest *m) {
    const char *p = text;
    int in_deps = 0;

    memset(m, 0, sizeof(*m));
    while (*p) {
        char line[1024];
        size_t n = 0;
        const char *eq;
        while (*p && *p != '\n' && n + 1 < sizeof line)
            line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n')
            p++;
        {
            char *work = line;
            char *key;
            char *value;
            char *t = trim(work);
            if (!t[0] || t[0] == '#')
                continue;
            if (t[0] == '[') {
                char *close = strchr(t, ']');
                if (!close)
                    continue;
                *close = 0;
                in_deps = !strcmp(trim(t + 1), "deps");
                continue;
            }
            eq = strchr(t, '=');
            if (!eq)
                continue;
            key = (char *)xstrndup(arena, t, (size_t)(eq - t));
            value = (char *)trim((char *)eq + 1);
            {
                char *k = (char *)trim(key);
                size_t vn = strlen(value);
                if (vn >= 2 && (value[0] == '"' || value[0] == '\'') &&
                    value[vn - 1] == value[0]) {
                    value++;
                    vn -= 2;
                }
                value = (char *)xstrndup(arena, value, vn);
                if (in_deps) {
                    manifest_push_dep(arena, m, k, value);
                }
                else if (!strcmp(k, "name")) {
                    m->name = value;
                }
                else if (!strcmp(k, "version")) {
                    m->version = value;
                }
                else if (!strcmp(k, "registry")) {
                    m->registry = value;
                }
                else if (!strcmp(k, "token_env")) {
                    m->token_env = value;
                }
            }
        }
    }
}

static int load_manifest_at(Arena *arena, const char *dir, Manifest *m) {
    char *path = join_path(arena, dir, MANIFEST);
    char *text = read_text(arena, path);
    if (!text)
        return 0;
    parse_manifest(arena, text, m);
    return 1;
}

static int write_manifest(const char *dir, const Manifest *m) {
    size_t len = strlen(dir) + strlen(MANIFEST) + 2;
    char *path = malloc(len);
    FILE *f;
    size_t i;

    if (!path)
        return 0;
    snprintf(path, len, "%s/%s", dir, MANIFEST);
    f = fopen(path, "wb");
    free(path);
    if (!f)
        return 0;
    fprintf(f, "# Aster package manifest\n");
    if (m->name)
        fprintf(f, "name = %s\n", m->name);
    if (m->version)
        fprintf(f, "version = %s\n", m->version);
    if (m->registry)
        fprintf(f, "registry = %s\n", m->registry);
    if (m->token_env)
        fprintf(f, "token_env = %s\n", m->token_env);
    fprintf(f, "\n[deps]\n");
    for (i = 0; i < m->ndeps; i++)
        fprintf(f, "%s = %s\n", m->deps[i].alias, m->deps[i].path);
    fclose(f);
    return 1;
}

/* dep 경로가 import alias를 실제로 찾을 수 있는지 검사한다. */
static int dep_resolvable(const char *root, const char *alias) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s/%s.ast", root, alias);
    if (file_exists(buf))
        return 1;
    snprintf(buf, sizeof buf, "%s/%s/__init__.ast", root, alias);
    return file_exists(buf);
}

/* ---- 공통 유틸 ---- */

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

static int is_registry_dep(const char *path) {
    return path && starts_with(path, "registry:");
}

static const char *registry_dep_version(const char *path) {
    return path + strlen("registry:");
}

/* 이름/버전/URL은 명령줄과 경로에 그대로 들어가므로 문자 집합을 제한한다. */
static int valid_name(const char *s) {
    const char *p;
    if (!s || !s[0] || !strcmp(s, ".") || strstr(s, ".."))
        return 0;
    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static int valid_version(const char *s) {
    const char *p;
    if (!s || !s[0])
        return 0;
    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '-' || c == '+' || c == '_'))
            return 0;
    }
    return 1;
}

static int valid_url(const char *s) {
    const char *p;
    if (!s || !s[0] || s[0] == '-')
        return 0;
    for (p = s; *p; p++)
        if ((unsigned char)*p < 0x20)
            return 0;
    return 1;
}

static char *concat2(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *p = malloc(la + lb + 2);
    if (!p)
        return NULL;
    memcpy(p, a, la);
    p[la] = '/';
    memcpy(p + la + 1, b, lb + 1);
    return p;
}

static char *rel_join(Arena *arena, const char *base, const char *name) {
    if (!base || !base[0])
        return xstrndup(arena, name, strlen(name));
    return join_path(arena, base, name);
}

static int mkdir_p(const char *path) {
    size_t n = strlen(path), i;
    char *buf;

    if (n == 0)
        return 0;
    buf = malloc(n + 1);
    if (!buf)
        return 0;
    memcpy(buf, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\\' || buf[i] == 0) {
            char save = buf[i];
            buf[i] = 0;
            if (!(i == 2 && buf[1] == ':')) {
#if defined(_WIN32)
                _mkdir(buf);
#else
                mkdir(buf, 0777);
#endif
            }
            buf[i] = save;
        }
    }
    free(buf);
    return dir_exists(path);
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[65536];
    size_t n;
    int ok = 1;

    if (!in)
        return 0;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    if (ferror(in))
        ok = 0;
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;
    return ok;
}

static unsigned char *read_bytes(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *buf;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static int remove_dir_recursive(const char *path) {
    DIR *d;
    struct dirent *e;

    if (!dir_exists(path))
        return remove(path) == 0;
    d = opendir(path);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            char *full;
            struct stat st;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            full = concat2(path, e->d_name);
            if (!full)
                break;
            if (stat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode))
                    remove_dir_recursive(full);
                else
                    remove(full);
            }
            free(full);
        }
        closedir(d);
    }
#if defined(_WIN32)
    return _rmdir(path) == 0;
#else
    return rmdir(path) == 0;
#endif
}

/* ---- 패키지 내용 수집 ---- */

typedef struct {
    char **items;
    size_t n, cap;
} StrList;

static void strlist_push(Arena *arena, StrList *l, const char *s) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        char **grown = Arena_Alloc(arena, cap * sizeof(char *));
        if (l->items)
            memcpy(grown, l->items, l->n * sizeof(char *));
        l->items = grown;
        l->cap = cap;
    }
    l->items[l->n++] = xstrndup(arena, s, strlen(s));
}

static int excluded_dir(const char *name) {
    static const char *names[] = {".git", "vendor", "dist", "build",
                                  "Objects", "tmp", "node_modules", "__pycache__"};
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}

static int include_file(const char *name) {
    if (!strcmp(name, MANIFEST))
        return 1;
    return ends_with(name, ".ast");
}

static void walk_collect(Arena *arena, const char *root, const char *rel,
                         StrList *out, int depth) {
    char *dir;
    DIR *d;
    struct dirent *e;

    if (depth > 32)
        return;
    dir = rel_join(arena, root, rel);
    d = opendir(dir);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char *full;
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        full = join_path(arena, dir, e->d_name);
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (!excluded_dir(e->d_name))
                walk_collect(arena, root, rel_join(arena, rel, e->d_name),
                             out, depth + 1);
        } else if (S_ISREG(st.st_mode) && include_file(e->d_name)) {
            strlist_push(arena, out, rel_join(arena, rel, e->d_name));
        }
    }
    closedir(d);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ---- SHA-256 ---- */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    unsigned char data[64];
    size_t datalen;
} Sha256;

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static const uint32_t sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha256_transform(Sha256 *c, const unsigned char *d) {
    uint32_t m[64], a, b, cc, dd, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        m[i] = ((uint32_t)d[i * 4] << 24) | ((uint32_t)d[i * 4 + 1] << 16) |
               ((uint32_t)d[i * 4 + 2] << 8) | (uint32_t)d[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    a = c->state[0]; b = c->state[1]; cc = c->state[2]; dd = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + s1 + ch + sha_k[i] + m[i];
        t2 = s0 + maj;
        h = g; g = f; f = e; e = dd + t1;
        dd = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += dd;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_init(Sha256 *c) {
    c->datalen = 0;
    c->bitlen = 0;
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
}

static void sha256_update(Sha256 *c, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len > 0) {
        size_t take = 64 - c->datalen;
        if (take > len)
            take = len;
        memcpy(c->data + c->datalen, p, take);
        c->datalen += take;
        p += take;
        len -= take;
        c->bitlen += (uint64_t)take * 8;
        if (c->datalen == 64) {
            sha256_transform(c, c->data);
            c->datalen = 0;
        }
    }
}

static void sha256_final(Sha256 *c, unsigned char out[32]) {
    uint64_t bits = c->bitlen;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    int i;

    sha256_update(c, &pad, 1);
    while (c->datalen != 56)
        sha256_update(c, &zero, 1);
    for (i = 0; i < 8; i++)
        lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(c->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->state[i];
    }
}

static int sha256_file(const char *path, char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    FILE *f = fopen(path, "rb");
    Sha256 c;
    unsigned char buf[65536], out[32];
    size_t n;
    int i;

    if (!f)
        return 0;
    sha256_init(&c);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sha256_update(&c, buf, n);
    if (ferror(f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    sha256_final(&c, out);
    for (i = 0; i < 32; i++) {
        hex[i * 2] = digits[out[i] >> 4];
        hex[i * 2 + 1] = digits[out[i] & 15];
    }
    hex[64] = 0;
    return 1;
}

/* ---- ustar 아카이브 (.astpkg) ----
 * mtime/uid/gid를 0으로 고정해 같은 내용이면 같은 바이트가 나오게 한다. 따라서
 * 레지스트리 인덱스의 sha256이 재현 가능하다. */

#define TAR_BLOCK 512

static void tar_octal(unsigned char *dst, size_t len, uint64_t value) {
    size_t i;
    dst[len - 1] = 0;
    for (i = len - 1; i > 0; i--) {
        dst[i - 1] = (unsigned char)('0' + (value & 7));
        value >>= 3;
    }
}

static int tar_header(unsigned char h[TAR_BLOCK], const char *name,
                      uint64_t size) {
    size_t n = strlen(name), i;
    unsigned sum = 0;

    memset(h, 0, TAR_BLOCK);
    if (n <= 100) {
        memcpy(h, name, n);
    } else {
        size_t split = 0;
        for (i = n - 100; i < n; i++) {
            if (name[i] == '/') {
                split = i;
                break;
            }
        }
        if (!split || split > 155 || n - split - 1 > 100)
            return 0;
        memcpy(h + 345, name, split);
        memcpy(h, name + split + 1, n - split - 1);
    }
    tar_octal(h + 100, 8, 0644);
    tar_octal(h + 108, 8, 0);
    tar_octal(h + 116, 8, 0);
    tar_octal(h + 124, 12, size);
    tar_octal(h + 136, 12, 0);
    memset(h + 148, ' ', 8);
    h[156] = '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    for (i = 0; i < TAR_BLOCK; i++)
        sum += h[i];
    for (i = 0; i < 6; i++) {
        h[148 + i] = (unsigned char)('0' + (sum & 7));
        sum >>= 3;
    }
    /* 6자리 8진수는 위에서 역순으로 넣었으므로 뒤집는다. */
    for (i = 0; i < 3; i++) {
        unsigned char t = h[148 + i];
        h[148 + i] = h[153 - i];
        h[153 - i] = t;
    }
    h[154] = 0;
    h[155] = ' ';
    return 1;
}

static int tar_write_entry(FILE *f, const char *name, const void *data,
                           size_t size) {
    unsigned char h[TAR_BLOCK], zero[TAR_BLOCK];
    size_t pad = (TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK;

    if (!tar_header(h, name, size))
        return 0;
    if (fwrite(h, 1, TAR_BLOCK, f) != TAR_BLOCK)
        return 0;
    if (size > 0 && fwrite(data, 1, size, f) != size)
        return 0;
    if (pad > 0) {
        memset(zero, 0, sizeof zero);
        if (fwrite(zero, 1, pad, f) != pad)
            return 0;
    }
    return 1;
}

static int pkg_pack(Arena *arena, const char *root, const char *out_path,
                    char *err, size_t errcap) {
    StrList files;
    FILE *out;
    size_t i;
    unsigned char zero[TAR_BLOCK];
    int ok = 1;

    memset(&files, 0, sizeof files);
    walk_collect(arena, root, "", &files, 0);
    if (files.n == 0) {
        snprintf(err, errcap, "no .ast files to pack under %s", root);
        return 0;
    }
    qsort(files.items, files.n, sizeof(char *), cmp_str);
    out = fopen(out_path, "wb");
    if (!out) {
        snprintf(err, errcap, "cannot write %s", out_path);
        return 0;
    }
    for (i = 0; i < files.n && ok; i++) {
        char *abs = join_path(arena, root, files.items[i]);
        size_t size = 0;
        unsigned char *data = read_bytes(abs, &size);
        if (!data) {
            snprintf(err, errcap, "cannot read %s", abs);
            ok = 0;
            break;
        }
        if (!tar_write_entry(out, files.items[i], data, size)) {
            snprintf(err, errcap, "path too long or write failed: %s",
                     files.items[i]);
            ok = 0;
        }
        free(data);
    }
    if (ok) {
        memset(zero, 0, sizeof zero);
        if (fwrite(zero, 1, sizeof zero, out) != sizeof zero ||
            fwrite(zero, 1, sizeof zero, out) != sizeof zero) {
            snprintf(err, errcap, "cannot finish %s", out_path);
            ok = 0;
        }
    }
    if (fclose(out) != 0)
        ok = 0;
    return ok;
}

static size_t bounded_len(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static uint64_t tar_octal_value(const unsigned char *p, size_t len) {
    uint64_t value = 0;
    size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == 0))
        i++;
    while (i < len && p[i] >= '0' && p[i] <= '7') {
        value = value * 8 + (uint64_t)(p[i] - '0');
        i++;
    }
    return value;
}

static int safe_rel_path(const char *name) {
    const char *p = name;

    if (!name[0] || name[0] == '/' || name[0] == '\\')
        return 0;
    if (isalpha((unsigned char)name[0]) && name[1] == ':')
        return 0;
    while (*p) {
        const char *seg = p;
        size_t len = 0;
        while (*p && *p != '/') {
            if (*p == '\\')
                return 0;
            p++;
            len++;
        }
        if (len == 0)
            return 0;
        if ((len == 1 && seg[0] == '.') ||
            (len == 2 && seg[0] == '.' && seg[1] == '.'))
            return 0;
        if (*p == '/')
            p++;
    }
    return 1;
}

static int pkg_extract(const char *archive, const char *dest,
                       char *err, size_t errcap) {
    FILE *f = fopen(archive, "rb");
    unsigned char h[TAR_BLOCK];
    int ok = 1;

    if (!f) {
        snprintf(err, errcap, "cannot open %s", archive);
        return 0;
    }
    while (ok && fread(h, 1, TAR_BLOCK, f) == TAR_BLOCK) {
        size_t i, empty = 1, stored, actual = 0;
        uint64_t size;
        char name[257], buf[8192];
        size_t nl = 0, leftpad, pad;
        char *full, *slash;
        FILE *out = NULL;

        for (i = 0; i < TAR_BLOCK; i++)
            if (h[i] != 0)
                empty = 0;
        if (empty)
            break;
        if (memcmp(h + 257, "ustar", 5) != 0) {
            snprintf(err, errcap, "not an aster package (bad header)");
            ok = 0;
            break;
        }
        stored = (size_t)tar_octal_value(h + 148, 6);
        for (i = 0; i < TAR_BLOCK; i++)
            actual += (i >= 148 && i < 156) ? ' ' : h[i];
        if (stored != actual) {
            snprintf(err, errcap, "corrupt archive header (checksum)");
            ok = 0;
            break;
        }
        size = tar_octal_value(h + 124, 12);
        if (size > (uint64_t)256 * 1024 * 1024) {
            snprintf(err, errcap, "archive entry too large");
            ok = 0;
            break;
        }
        if (h[345]) {
            size_t pl = bounded_len((const char *)h + 345, 155);
            memcpy(name, h + 345, pl);
            nl = pl;
            name[nl++] = '/';
        }
        {
            size_t bl = bounded_len((const char *)h, 100);
            memcpy(name + nl, h, bl);
            nl += bl;
            name[nl] = 0;
        }
        if (!safe_rel_path(name)) {
            snprintf(err, errcap, "unsafe archive path: %s", name);
            ok = 0;
            break;
        }
        full = concat2(dest, name);
        if (!full) {
            ok = 0;
            break;
        }
        slash = strrchr(full, '/');
        if (slash) {
            *slash = 0;
            mkdir_p(full);
            *slash = '/';
        }
        if (h[156] == '0' || h[156] == 0) {
            out = fopen(full, "wb");
            if (!out) {
                snprintf(err, errcap, "cannot write %s", full);
                ok = 0;
            }
        }
        {
            uint64_t left = size;
            while (ok && left > 0) {
                size_t take = left > sizeof buf ? sizeof buf : (size_t)left;
                if (fread(buf, 1, take, f) != take) {
                    snprintf(err, errcap, "truncated archive");
                    ok = 0;
                    break;
                }
                if (out && fwrite(buf, 1, take, out) != take) {
                    snprintf(err, errcap, "cannot write %s", full);
                    ok = 0;
                    break;
                }
                left -= take;
            }
            if (out && fclose(out) != 0) {
                snprintf(err, errcap, "cannot write %s", full);
                ok = 0;
            }
        }
        free(full);
        pad = (size_t)((TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK);
        leftpad = pad;
        while (ok && leftpad > 0) {
            size_t take = leftpad > sizeof buf ? sizeof buf : leftpad;
            if (fread(buf, 1, take, f) != take) {
                snprintf(err, errcap, "truncated archive");
                ok = 0;
                break;
            }
            leftpad -= take;
        }
    }
    fclose(f);
    return ok;
}

/* ---- 외부 프로세스 ---- */

static int run_process(const char *const argv[]) {
#if defined(_WIN32)
    int n = 0, i, rc = -1;
    wchar_t **wargv;

    while (argv[n])
        n++;
    wargv = calloc((size_t)n + 1, sizeof(wchar_t *));
    if (!wargv)
        return -1;
    for (i = 0; i < n; i++) {
        int len = MultiByteToWideChar(PKG_CP_UTF8, 0, argv[i], -1, NULL, 0);
        if (len <= 0)
            break;
        wargv[i] = malloc((size_t)len * sizeof(wchar_t));
        if (!wargv[i] || MultiByteToWideChar(PKG_CP_UTF8, 0, argv[i], -1,
                                             wargv[i], len) != len)
            break;
    }
    if (i == n)
        rc = (int)_wspawnvp(_P_WAIT, wargv[0], (const wchar_t *const *)wargv);
    for (i = 0; i < n; i++)
        free(wargv[i]);
    free(wargv);
    return rc;
#else
    pid_t pid = fork();
    int status;
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

static long pkg_pid(void) {
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

/* ---- 원격 URL ---- */

static int is_http_url(const char *u) {
    return starts_with(u, "http://") || starts_with(u, "https://");
}

static char *url_join(Arena *arena, const char *base, const char *rel) {
    size_t n = strlen(base);
    char *p;
    while (n > 0 && base[n - 1] == '/')
        n--;
    p = Arena_Alloc(arena, n + strlen(rel) + 2);
    memcpy(p, base, n);
    p[n] = '/';
    memcpy(p + n + 1, rel, strlen(rel) + 1);
    return p;
}

/* 'file://' 및 스킴 없는 문자열은 로컬 경로로 본다. */
static const char *local_path(const char *url) {
    if (starts_with(url, "file://")) {
        const char *p = url + strlen("file://");
#if defined(_WIN32)
        if (p[0] == '/' && isalpha((unsigned char)p[1]) && p[2] == ':')
            p++;
#endif
        return p;
    }
    return url;
}

static int fetch_to_file(const char *url, const char *out) {
    if (is_http_url(url)) {
        const char *argv[] = {"curl", "-fsL", "--connect-timeout", "20",
                              "-o", out, url, NULL};
        /* curl 8.14 returns 0 with --retry even when the last try failed, so
           we avoid --retry and fall back to our own error message. */
        return run_process(argv) == 0;
    }
    return copy_file(local_path(url), out);
}

static int put_file(const char *url, const char *path, const char *token) {
    char hdr[1024];
    if (!is_http_url(url))
        return 0;
    if (token && token[0]) {
        const char *argv[12];
        snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", token);
        argv[0] = "curl"; argv[1] = "-fs";
        argv[2] = "--connect-timeout"; argv[3] = "20";
        argv[4] = "-X"; argv[5] = "PUT";
        argv[6] = "-H"; argv[7] = hdr;
        argv[8] = "--upload-file"; argv[9] = path;
        argv[10] = url; argv[11] = NULL;
        return run_process(argv) == 0;
    }
    {
        const char *argv[] = {"curl", "-fs", "--connect-timeout", "20",
                              "-X", "PUT", "--upload-file", path, url, NULL};
        return run_process(argv) == 0;
    }
}

/* ---- 레지스트리 인덱스 ---- */

typedef struct {
    const char *name;
    const char *version;
    const char *sha;
    const char *file;
    const char *deps;
} IndexEntry;

typedef struct {
    Arena *arena;
    IndexEntry *items;
    size_t n, cap;
} Index;

static int index_add(Index *ix, const char *name, const char *version,
                     const char *sha, const char *file, const char *deps) {
    IndexEntry *e;
    size_t i;

    if (!valid_name(name) || !valid_version(version) || strlen(sha) != 64)
        return 0;
    for (i = 0; i < ix->n; i++) {
        if (!strcmp(ix->items[i].name, name) &&
            !strcmp(ix->items[i].version, version)) {
            ix->items[i].sha = xstrndup(ix->arena, sha, strlen(sha));
            ix->items[i].file = xstrndup(ix->arena, file, strlen(file));
            ix->items[i].deps =
                xstrndup(ix->arena, deps ? deps : "", strlen(deps ? deps : ""));
            return 1;
        }
    }
    if (ix->n == ix->cap) {
        size_t cap = ix->cap ? ix->cap * 2 : 16;
        IndexEntry *grown = Arena_Alloc(ix->arena, cap * sizeof(IndexEntry));
        if (ix->items)
            memcpy(grown, ix->items, ix->n * sizeof(IndexEntry));
        ix->items = grown;
        ix->cap = cap;
    }
    e = &ix->items[ix->n++];
    e->name = xstrndup(ix->arena, name, strlen(name));
    e->version = xstrndup(ix->arena, version, strlen(version));
    e->sha = xstrndup(ix->arena, sha, strlen(sha));
    e->file = xstrndup(ix->arena, file, strlen(file));
    e->deps = xstrndup(ix->arena, deps ? deps : "", strlen(deps ? deps : ""));
    return 1;
}

static void parse_index(Index *ix, const char *text) {
    const char *p = text;
    while (p && *p) {
        char line[2048];
        size_t n = 0;
        char *tok[5], *t, *q;
        size_t nt = 0;

        while (*p && *p != '\n' && n + 1 < sizeof line)
            line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n')
            p++;
        t = trim(line);
        if (!t[0] || t[0] == '#')
            continue;
        q = t;
        while (*q && nt < 5) {
            while (*q == ' ' || *q == '\t')
                q++;
            if (!*q)
                break;
            tok[nt++] = q;
            while (*q && *q != ' ' && *q != '\t')
                q++;
            if (*q)
                *q++ = 0;
        }
        if (nt < 4)
            continue;
        if (!valid_name(tok[0]) || !valid_version(tok[1]) ||
            strlen(tok[2]) != 64)
            continue;
        index_add(ix, tok[0], tok[1], tok[2], tok[3], nt > 4 ? tok[4] : "");
    }
}

static const IndexEntry *index_find_exact(const Index *ix, const char *name,
                                          const char *version) {
    size_t i;
    for (i = 0; i < ix->n; i++)
        if (!strcmp(ix->items[i].name, name) &&
            !strcmp(ix->items[i].version, version))
            return &ix->items[i];
    return NULL;
}

static int version_cmp(const char *a, const char *b) {
    const char *oa = a, *ob = b;
    while (*a || *b) {
        unsigned long na = 0, nb = 0;
        while (isdigit((unsigned char)*a))
            na = na * 10 + (unsigned long)(*a++ - '0');
        while (isdigit((unsigned char)*b))
            nb = nb * 10 + (unsigned long)(*b++ - '0');
        if (na != nb)
            return na < nb ? -1 : 1;
        if (*a)
            a++;
        if (*b)
            b++;
        if (!*a && !*b)
            break;
    }
    return strcmp(oa, ob);
}

static const IndexEntry *index_find_best(const Index *ix, const char *name,
                                         const char *version) {
    size_t i;
    const IndexEntry *best = NULL;
    int exact = version && version[0] && strcmp(version, "latest") != 0;
    for (i = 0; i < ix->n; i++) {
        const IndexEntry *e = &ix->items[i];
        if (strcmp(e->name, name))
            continue;
        if (exact) {
            if (!strcmp(e->version, version))
                return e;
            continue;
        }
        if (!best || version_cmp(e->version, best->version) > 0)
            best = e;
    }
    return best;
}

static int cmp_entry_ptr(const void *a, const void *b) {
    const IndexEntry *ea = *(const IndexEntry *const *)a;
    const IndexEntry *eb = *(const IndexEntry *const *)b;
    int c = strcmp(ea->name, eb->name);
    return c ? c : version_cmp(ea->version, eb->version);
}

static int write_index(const char *path, Index *ix) {
    FILE *f;
    IndexEntry **arr;
    size_t i;

    f = fopen(path, "wb");
    if (!f)
        return 0;
    fprintf(f, "# aster registry index v1\n");
    fprintf(f, "# name\tversion\tsha256\tfile\tdeps\n");
    arr = malloc(ix->n ? ix->n * sizeof(IndexEntry *) : 1);
    if (!arr) {
        fclose(f);
        return 0;
    }
    for (i = 0; i < ix->n; i++)
        arr[i] = &ix->items[i];
    qsort(arr, ix->n, sizeof(IndexEntry *), cmp_entry_ptr);
    for (i = 0; i < ix->n; i++)
        fprintf(f, "%s\t%s\t%s\t%s\t%s\n", arr[i]->name, arr[i]->version,
                arr[i]->sha, arr[i]->file, arr[i]->deps ? arr[i]->deps : "");
    free(arr);
    return fclose(f) == 0;
}

/* ---- 의존성 해석 ---- */

typedef struct {
    const char *name;
    const char *version;
    const char *sha;
    const char *file;
} Pin;

typedef struct {
    Arena *arena;
    Pin *items;
    size_t n, cap;
} Pins;

static Pin *pin_find(Pins *pins, const char *name) {
    size_t i;
    for (i = 0; i < pins->n; i++)
        if (!strcmp(pins->items[i].name, name))
            return &pins->items[i];
    return NULL;
}

static void pin_set(Pins *pins, const char *name, const char *version,
                    const char *sha, const char *file) {
    Pin *p = pin_find(pins, name);
    if (!p) {
        if (pins->n == pins->cap) {
            size_t cap = pins->cap ? pins->cap * 2 : 8;
            Pin *grown = Arena_Alloc(pins->arena, cap * sizeof(Pin));
            if (pins->items)
                memcpy(grown, pins->items, pins->n * sizeof(Pin));
            pins->items = grown;
            pins->cap = cap;
        }
        p = &pins->items[pins->n++];
        p->name = xstrndup(pins->arena, name, strlen(name));
    }
    p->version = xstrndup(pins->arena, version, strlen(version));
    p->sha = xstrndup(pins->arena, sha, strlen(sha));
    p->file = xstrndup(pins->arena, file, strlen(file));
}

/* 요구 사항(name[@version])을 핀 목록에 추가한다. */
static int pin_add_requirement(Arena *arena, Index *ix, Pins *pins,
                               const char *req, char *err, size_t errcap) {
    const char *at = strchr(req, '@');
    const char *name;
    const char *version;
    const IndexEntry *e;

    if (at) {
        name = xstrndup(arena, req, (size_t)(at - req));
        version = at + 1;
    } else {
        name = req;
        version = "latest";
    }
    if (!valid_name(name) || !valid_version(version)) {
        snprintf(err, errcap, "invalid package requirement: %s", req);
        return 0;
    }
    e = index_find_best(ix, name, version);
    if (!e) {
        snprintf(err, errcap, "package not found in registry: %s@%s", name,
                 version);
        return 0;
    }
    pin_set(pins, name, e->version, e->sha, e->file);
    return 1;
}

/* 전이 의존성을 포함해 고정점까지 버전을 올린다(충돌 시 높은 버전 우선). */
static int resolve_all(Arena *arena, Index *ix, Pins *pins,
                       char *err, size_t errcap) {
    size_t iter;
    for (iter = 0; iter < 128; iter++) {
        int changed = 0;
        size_t i;
        for (i = 0; i < pins->n; i++) {
            const IndexEntry *e = index_find_exact(ix, pins->items[i].name,
                                                   pins->items[i].version);
            char *copy, *p;
            if (!e) {
                snprintf(err, errcap, "missing registry entry: %s@%s",
                         pins->items[i].name, pins->items[i].version);
                return 0;
            }
            if (!e->deps || !e->deps[0])
                continue;
            copy = xstrndup(arena, e->deps, strlen(e->deps));
            p = copy;
            while (p && *p) {
                char *comma = strchr(p, ',');
                char *req;
                if (comma)
                    *comma = 0;
                req = trim(p);
                if (*req) {
                    char *at = strchr(req, '@');
                    const char *dn = req;
                    const char *dv = "latest";
                    const IndexEntry *de;
                    Pin *existing;
                    if (at) {
                        *at = 0;
                        dv = at + 1;
                    }
                    de = index_find_best(ix, dn, dv);
                    if (!de) {
                        snprintf(err, errcap,
                                 "dependency not found: %s@%s (required by %s@%s)",
                                 dn, dv, e->name, e->version);
                        return 0;
                    }
                    existing = pin_find(pins, dn);
                    if (!existing) {
                        pin_set(pins, dn, de->version, de->sha, de->file);
                        changed = 1;
                    } else if (version_cmp(de->version, existing->version) > 0) {
                        pin_set(pins, dn, de->version, de->sha, de->file);
                        changed = 1;
                    }
                }
                p = comma ? comma + 1 : NULL;
            }
        }
        if (!changed)
            return 1;
    }
    return 1;
}

static void print_usage(void) {
    puts("usage: aster pkg <command> [args]");
    puts("  init [name]        create aster.pkg in the current directory");
    puts("  add NAME PATH      add a local dependency (PATH is the package's parent directory)");
    puts("  add NAME[@VER]     install a package from the registry");
    puts("  remove NAME        remove a dependency");
    puts("  list               list dependencies");
    puts("  check              verify that dependencies exist");
    puts("  install [NAME[@VER]] [--registry URL] [--token TOKEN]");
    puts("                     install registry dependencies into vendor/");
    puts("  publish [--registry URL] [--token TOKEN] [--out DIR]");
    puts("          [--force] [--new] [--no-upload]");
    puts("                     build NAME-VER.astpkg and update the registry");
    puts("  registry is taken from --registry, ASTER_REGISTRY, or aster.pkg");
}

static int pkg_init(Arena *arena, int argc, char **argv) {
    Manifest m;
    const char *name = argc > 0 ? argv[0] : NULL;
    if (file_exists(MANIFEST)) {
        puts("aster.pkg already exists");
        return 1;
    }
    memset(&m, 0, sizeof m);
    m.name = name ? name : "app";
    m.version = "0.1.0";
    if (!write_manifest(".", &m)) {
        fputs("cannot write aster.pkg\n", stderr);
        return 1;
    }
    printf("created: aster.pkg (name = %s)\n", m.name);
    (void)arena;
    return 0;
}

static int pkg_add(Arena *arena, int argc, char **argv) {
    Manifest m;
    const char *alias;
    const char *path;
    const char *root;

    if (argc != 2) {
        fputs("usage: aster pkg add NAME PATH\n", stderr);
        return 2;
    }
    alias = argv[0];
    path = argv[1];
    if (!dir_exists(path)) {
        fprintf(stderr, "no such directory: %s\n", path);
        return 1;
    }
    /* 패키지 디렉터리 자체를 줬다면 부모를 저장한다. */
    if (dep_resolvable(path, alias)) {
        root = path;
    }
    else if (!strcmp(base_name(path), alias) && file_exists(join_path(arena, path, "__init__.ast"))) {
        root = parent_dir(arena, path);
        if (!root) {
            fprintf(stderr, "cannot find the parent directory of %s\n", path);
            return 1;
        }
    }
    else {
        fprintf(stderr, "cannot find package '%s' under %s "
                        "(expected %s/%s.ast or %s/%s/__init__.ast)\n",
                alias, path, path, alias, path, alias);
        return 1;
    }

    if (!load_manifest_at(arena, ".", &m)) {
        memset(&m, 0, sizeof m);
        m.name = "app";
        m.version = "0.1.0";
    }
    manifest_push_dep(arena, &m, xstrndup(arena, alias, strlen(alias)),
                      xstrndup(arena, root, strlen(root)));
    if (!write_manifest(".", &m)) {
        fputs("cannot write aster.pkg\n", stderr);
        return 1;
    }
    printf("added: %s -> %s\n", alias, root);
    return 0;
}

static int pkg_remove(Arena *arena, int argc, char **argv) {
    Manifest m;
    if (argc != 1) {
        fputs("usage: aster pkg remove NAME\n", stderr);
        return 2;
    }
    if (!load_manifest_at(arena, ".", &m)) {
        fputs("aster.pkg not found\n", stderr);
        return 1;
    }
    if (!manifest_remove_dep(&m, argv[0])) {
        fprintf(stderr, "no such dependency: %s\n", argv[0]);
        return 1;
    }
    if (!write_manifest(".", &m)) {
        fputs("cannot write aster.pkg\n", stderr);
        return 1;
    }
    printf("removed: %s\n", argv[0]);
    return 0;
}

static int vendor_resolvable(Arena *arena, const char *alias) {
    char *dir = join_path(arena, VENDOR_DIR, alias);
    char buf[1024];
    if (!dir_exists(dir))
        return 0;
    snprintf(buf, sizeof buf, "%s/__init__.ast", dir);
    if (file_exists(buf))
        return 1;
    snprintf(buf, sizeof buf, "%s/%s.ast", dir, alias);
    return file_exists(buf);
}

static int pkg_list(Arena *arena, int argc, char **argv) {
    Manifest m;
    size_t i;
    (void)argc;
    (void)argv;
    if (!load_manifest_at(arena, ".", &m)) {
        fputs("aster.pkg not found (run 'aster pkg init')\n", stderr);
        return 1;
    }
    if (m.name)
        printf("name: %s\n", m.name);
    if (m.version)
        printf("version: %s\n", m.version);
    if (m.registry)
        printf("registry: %s\n", m.registry);
    if (m.ndeps == 0)
        puts("no dependencies");
    for (i = 0; i < m.ndeps; i++)
        printf("%s = %s%s\n", m.deps[i].alias, m.deps[i].path,
               is_registry_dep(m.deps[i].path) &&
                       vendor_resolvable(arena, m.deps[i].alias)
                   ? " (installed)"
                   : "");
    return 0;
}

static int pkg_check(Arena *arena, int argc, char **argv) {
    Manifest m;
    size_t i;
    int failed = 0;
    (void)argc;
    (void)argv;
    if (!load_manifest_at(arena, ".", &m)) {
        fputs("aster.pkg not found (run 'aster pkg init')\n", stderr);
        return 1;
    }
    for (i = 0; i < m.ndeps; i++) {
        const char *path = m.deps[i].path;
        if (is_registry_dep(path)) {
            if (vendor_resolvable(arena, m.deps[i].alias))
                printf("ok   %s = %s\n", m.deps[i].alias, path);
            else {
                printf("fail %s = %s (run 'aster pkg install')\n",
                       m.deps[i].alias, path);
                failed = 1;
            }
        } else if (dep_resolvable(path, m.deps[i].alias)) {
            printf("ok   %s = %s\n", m.deps[i].alias, path);
        } else {
            printf("fail %s = %s (module not found)\n", m.deps[i].alias, path);
            failed = 1;
        }
    }
    return failed ? 1 : 0;
}

/* ---- install / publish ---- */

static int write_file_bytes(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    if (n > 0 && fwrite(data, 1, n, f) != n) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static int flag_takes_value(const char *f) {
    return !strcmp(f, "--registry") || !strcmp(f, "--token") ||
           !strcmp(f, "--out");
}

static const char *find_flag(int argc, char **argv, const char *flag) {
    int i;
    for (i = 0; i < argc; i++)
        if (!strcmp(argv[i], flag))
            return i + 1 < argc ? argv[i + 1] : NULL;
    return NULL;
}

static int has_flag(int argc, char **argv, const char *flag) {
    int i;
    for (i = 0; i < argc; i++)
        if (!strcmp(argv[i], flag))
            return 1;
    return 0;
}

static const char *first_positional(int argc, char **argv) {
    int i;
    for (i = 0; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != 0) {
            if (flag_takes_value(argv[i]))
                i++;
            continue;
        }
        return argv[i];
    }
    return NULL;
}

static const char *optional_registry(const Manifest *m, int argc,
                                     char **argv) {
    const char *r = find_flag(argc, argv, "--registry");
    if (!r)
        r = getenv("ASTER_REGISTRY");
    if (!r && m)
        r = m->registry;
    if (!r || !r[0] || !valid_url(r))
        return NULL;
    return r;
}

static const char *resolve_token(const Manifest *m, int argc, char **argv) {
    const char *t = find_flag(argc, argv, "--token");
    if (t)
        return t;
    t = getenv("ASTER_REGISTRY_TOKEN");
    if (t)
        return t;
    if (m && m->token_env) {
        t = getenv(m->token_env);
        if (t)
            return t;
    }
    return NULL;
}

static char *pkg_tempdir(Arena *arena) {
    char name[64];
    snprintf(name, sizeof name, ".aster-tmp-%ld", pkg_pid());
    mkdir_p(name);
    return xstrndup(arena, name, strlen(name));
}

static const char *lock_version(Arena *arena, const char *path,
                                const char *name) {
    char *text = read_text(arena, path);
    char *p = text;
    if (!text)
        return NULL;
    while (p && *p) {
        char line[512];
        size_t n = 0;
        char *t, *tokens[3];
        size_t nt = 0;
        char *q;
        while (*p && *p != '\n' && n + 1 < sizeof line)
            line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n')
            p++;
        t = trim(line);
        if (!t[0] || t[0] == '#')
            continue;
        q = t;
        while (*q && nt < 3) {
            while (*q == ' ' || *q == '\t')
                q++;
            if (!*q)
                break;
            tokens[nt++] = q;
            while (*q && *q != ' ' && *q != '\t')
                q++;
            if (*q)
                *q++ = 0;
        }
        if (nt >= 2 && !strcmp(tokens[0], name))
            return xstrndup(arena, tokens[1], strlen(tokens[1]));
    }
    return NULL;
}

static int write_lock(const char *registry, const Pins *pins) {
    FILE *f = fopen(LOCKFILE, "wb");
    size_t i;
    if (!f)
        return 0;
    fprintf(f, "# Aster lock file v1\n");
    if (registry)
        fprintf(f, "# registry = %s\n", registry);
    for (i = 0; i < pins->n; i++)
        fprintf(f, "%s %s %s\n", pins->items[i].name,
                pins->items[i].version, pins->items[i].sha);
    return fclose(f) == 0;
}

static int file_is_url_or_abs(const char *f) {
    return is_http_url(f) || starts_with(f, "file://") || is_absolute(f);
}

static int install_pins(Arena *arena, const char *registry, const Pins *pins,
                        char *err, size_t errcap) {
    size_t i;
    char *tmpdir = pkg_tempdir(arena);

    mkdir_p(VENDOR_DIR);
    for (i = 0; i < pins->n; i++) {
        const Pin *p = &pins->items[i];
        char artifact[512], meta_text[512], hex[65];
        char *dest = join_path(arena, VENDOR_DIR, p->name);
        char *meta = join_path(arena, dest, ".asterpkg");
        char *path;
        char *url;
        char *cached;

        snprintf(artifact, sizeof artifact, "%s-%s.astpkg", p->name, p->version);
        snprintf(meta_text, sizeof meta_text, "%s %s %s\n", p->name,
                 p->version, p->sha);
        cached = read_text(arena, meta);
        if (cached && !strcmp(cached, meta_text)) {
            printf("cached    %s %s\n", p->name, p->version);
            continue;
        }
        if (!file_is_url_or_abs(p->file)) {
            if (!valid_url(p->file)) {
                snprintf(err, errcap, "invalid artifact path: %s", p->file);
                return 0;
            }
            url = url_join(arena, registry, p->file);
        } else {
            url = xstrndup(arena, p->file, strlen(p->file));
        }
        path = join_path(arena, tmpdir, artifact);
        if (!fetch_to_file(url, path)) {
            snprintf(err, errcap, "cannot download %s", url);
            return 0;
        }
        if (!sha256_file(path, hex)) {
            snprintf(err, errcap, "cannot hash %s", path);
            return 0;
        }
        if (strcmp(hex, p->sha) != 0) {
            snprintf(err, errcap,
                     "checksum mismatch for %s@%s (expected %s, got %s)",
                     p->name, p->version, p->sha, hex);
            return 0;
        }
        if (dir_exists(dest))
            remove_dir_recursive(dest);
        if (!mkdir_p(dest)) {
            snprintf(err, errcap, "cannot create %s", dest);
            return 0;
        }
        if (!pkg_extract(path, dest, err, errcap))
            return 0;
        if (!write_file_bytes(meta, meta_text, strlen(meta_text))) {
            snprintf(err, errcap, "cannot write %s", meta);
            return 0;
        }
        printf("installed %s %s\n", p->name, p->version);
    }
    remove_dir_recursive(tmpdir);
    return 1;
}

static int pkg_install(Arena *arena, int argc, char **argv) {
    Manifest m;
    const char *req = first_positional(argc, argv);
    const char *registry;
    const char *token;
    const char *lockv = NULL;
    char err[512];
    char *tmpdir, *index_path, *index_text;
    char *lock_path;
    Index ix;
    Pins pins;
    int have_manifest;
    int code = 1;

    have_manifest = load_manifest_at(arena, ".", &m);
    if (!have_manifest) {
        if (!req) {
            fputs("aster.pkg not found (run 'aster pkg init')\n", stderr);
            return 1;
        }
        memset(&m, 0, sizeof m);
        m.name = "app";
        m.version = "0.1.0";
    }
    registry = optional_registry(&m, argc, argv);
    if (!registry) {
        fputs("no registry configured: pass --registry URL or set "
              "'registry = URL' in aster.pkg\n", stderr);
        return 1;
    }
    token = resolve_token(&m, argc, argv);
    (void)token;
    lock_path = join_path(arena, ".", LOCKFILE);

    tmpdir = pkg_tempdir(arena);
    index_path = join_path(arena, tmpdir, INDEXFILE);
    {
        char *url = url_join(arena, registry, INDEXFILE);
        if (!fetch_to_file(url, index_path)) {
            fprintf(stderr, "cannot read registry index: %s\n", url);
            return 1;
        }
    }
    index_text = read_text(arena, index_path);
    if (!index_text) {
        fputs("registry index is empty or unreadable\n", stderr);
        return 1;
    }
    memset(&ix, 0, sizeof ix);
    ix.arena = arena;
    parse_index(&ix, index_text);
    memset(&pins, 0, sizeof pins);
    pins.arena = arena;

    if (req) {
        if (!pin_add_requirement(arena, &ix, &pins, req, err, sizeof err)) {
            fprintf(stderr, "%s\n", err);
            goto done;
        }
    } else {
        size_t i;
        char reqbuf[512];
        for (i = 0; i < m.ndeps; i++) {
            const char *path = m.deps[i].path;
            const char *spec;
            if (!is_registry_dep(path))
                continue;
            spec = registry_dep_version(path);
            if (!strcmp(spec, "latest") || !spec[0]) {
                const char *lv = lock_version(arena, lock_path, m.deps[i].alias);
                if (lv)
                    lockv = lv;
            }
            snprintf(reqbuf, sizeof reqbuf, "%s@%s", m.deps[i].alias,
                     lockv ? lockv : spec);
            lockv = NULL;
            if (!pin_add_requirement(arena, &ix, &pins, reqbuf, err, sizeof err)) {
                fprintf(stderr, "%s\n", err);
                goto done;
            }
        }
        if (pins.n == 0) {
            fputs("no registry dependencies to install\n", stderr);
            code = 0;
            goto done;
        }
    }
    if (!resolve_all(arena, &ix, &pins, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        goto done;
    }
    if (!install_pins(arena, registry, &pins, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        goto done;
    }

    {
        /* 전이 의존성까지 매니페스트에 고정해 다음 실행에서 바로 import된다. */
        size_t pi;
        for (pi = 0; pi < pins.n; pi++) {
            char *value = Arena_Alloc(
                arena, strlen("registry:") + strlen(pins.items[pi].version) + 1);
            sprintf(value, "registry:%s", pins.items[pi].version);
            manifest_push_dep(arena, &m, pins.items[pi].name, value);
        }
        if (!write_manifest(".", &m)) {
            fputs("cannot write aster.pkg\n", stderr);
            goto done;
        }
    }
    if (!write_lock(registry, &pins)) {
        fputs("cannot write aster.lock\n", stderr);
        goto done;
    }
    printf("installed %lu package(s)\n", (unsigned long)pins.n);
    code = 0;

done:
    remove_dir_recursive(tmpdir);
    return code;
}

static int pkg_entry_exists(Arena *arena, const char *name) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s.ast", name);
    if (file_exists(join_path(arena, ".", buf)))
        return 1;
    return file_exists(join_path(arena, ".", "__init__.ast"));
}

static int pkg_publish(Arena *arena, int argc, char **argv) {
    Manifest m;
    const char *registry;
    const char *token;
    const char *out_opt;
    char *out_dir;
    char artifact[512], hex[65];
    char deps[1024];
    char err[512];
    char *archive_path;
    int force, allow_new, no_upload;
    Index ix;
    size_t i;

    if (!load_manifest_at(arena, ".", &m)) {
        fputs("aster.pkg not found (run 'aster pkg init')\n", stderr);
        return 1;
    }
    if (!m.name || !valid_name(m.name)) {
        fputs("aster.pkg needs a valid 'name'\n", stderr);
        return 1;
    }
    if (!m.version || !valid_version(m.version)) {
        fputs("aster.pkg needs a valid 'version'\n", stderr);
        return 1;
    }
    if (!pkg_entry_exists(arena, m.name)) {
        fprintf(stderr,
                "package entry not found: %s.ast or __init__.ast is required\n",
                m.name);
        return 1;
    }
    deps[0] = 0;
    for (i = 0; i < m.ndeps; i++) {
        if (is_registry_dep(m.deps[i].path)) {
            const char *v = registry_dep_version(m.deps[i].path);
            size_t len;
            if (!strcmp(v, "latest") || !v[0]) {
                fprintf(stderr,
                        "dependency '%s' must be pinned to a version before "
                        "publishing\n",
                        m.deps[i].alias);
                return 1;
            }
            len = strlen(deps);
            snprintf(deps + len, sizeof deps - len, "%s%s@%s",
                     len ? "," : "", m.deps[i].alias, v);
        } else {
            fprintf(stderr,
                    "warning: local dependency '%s' is not published; add it "
                    "as 'registry:VERSION' to include it\n",
                    m.deps[i].alias);
        }
    }

    force = has_flag(argc, argv, "--force");
    allow_new = has_flag(argc, argv, "--new");
    no_upload = has_flag(argc, argv, "--no-upload");
    registry = optional_registry(&m, argc, argv);
    token = resolve_token(&m, argc, argv);
    out_opt = find_flag(argc, argv, "--out");
    out_dir = xstrndup(arena, out_opt ? out_opt : DIST_DIR,
                       strlen(out_opt ? out_opt : DIST_DIR));
    if (!mkdir_p(out_dir)) {
        fprintf(stderr, "cannot create %s\n", out_dir);
        return 1;
    }
    snprintf(artifact, sizeof artifact, "%s-%s.astpkg", m.name, m.version);
    archive_path = join_path(arena, out_dir, artifact);
    if (!pkg_pack(arena, ".", archive_path, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (!sha256_file(archive_path, hex)) {
        fprintf(stderr, "cannot hash %s\n", archive_path);
        return 1;
    }
    printf("built     %s\n", archive_path);
    printf("sha256    %s\n", hex);

    memset(&ix, 0, sizeof ix);
    ix.arena = arena;

    if (!registry || no_upload) {
        char *idx_path = join_path(arena, out_dir, INDEXFILE);
        char *text = read_text(arena, idx_path);
        if (text)
            parse_index(&ix, text);
        if (!index_add(&ix, m.name, m.version, hex, artifact, deps)) {
            fputs("invalid package metadata\n", stderr);
            return 1;
        }
        if (!write_index(idx_path, &ix)) {
            fprintf(stderr, "cannot write %s\n", idx_path);
            return 1;
        }
        if (!registry) {
            printf("no registry configured; wrote %s and %s\n", archive_path,
                   idx_path);
            fputs("upload both files to your static registry, or set "
                  "'registry = URL' and rerun\n", stderr);
        } else {
            printf("wrote     %s and %s (upload disabled)\n", archive_path,
                   idx_path);
        }
        return 0;
    }

    if (is_http_url(registry)) {
        char *tmpdir = pkg_tempdir(arena);
        char *idx_path = join_path(arena, tmpdir, INDEXFILE);
        char *idx_url = url_join(arena, registry, INDEXFILE);
        int have_index = fetch_to_file(idx_url, idx_path);
        char *text;
        const IndexEntry *prev;
        int ok = 1;

        if (!have_index && !allow_new) {
            fprintf(stderr,
                    "cannot read remote index %s (pass --new to create a new "
                    "registry)\n",
                    idx_url);
            remove_dir_recursive(tmpdir);
            return 1;
        }
        text = have_index ? read_text(arena, idx_path) : NULL;
        if (text)
            parse_index(&ix, text);
        prev = text ? index_find_exact(&ix, m.name, m.version) : NULL;
        if (prev && strcmp(prev->sha, hex) != 0 && !force) {
            fprintf(stderr,
                    "refusing to overwrite %s@%s (different sha256); use "
                    "--force\n",
                    m.name, m.version);
            remove_dir_recursive(tmpdir);
            return 1;
        }
        if (!index_add(&ix, m.name, m.version, hex, artifact, deps) ||
            !write_index(idx_path, &ix)) {
            fprintf(stderr, "cannot prepare registry index\n");
            remove_dir_recursive(tmpdir);
            return 1;
        }
        {
            char *art_url = url_join(arena, registry, artifact);
            if (!put_file(art_url, archive_path, token)) {
                fprintf(stderr, "cannot upload %s\n", art_url);
                ok = 0;
            }
        }
        if (ok && !put_file(idx_url, idx_path, token)) {
            fprintf(stderr, "cannot upload %s\n", idx_url);
            ok = 0;
        }
        remove_dir_recursive(tmpdir);
        if (!ok)
            return 1;
        printf("published %s@%s to %s\n", m.name, m.version, registry);
        return 0;
    }

    {
        const char *dir = local_path(registry);
        char *idx_path = join_path(arena, dir, INDEXFILE);
        char *dst = join_path(arena, dir, artifact);
        char *text = read_text(arena, idx_path);
        const IndexEntry *prev;
        if (text)
            parse_index(&ix, text);
        prev = text ? index_find_exact(&ix, m.name, m.version) : NULL;
        if (prev && strcmp(prev->sha, hex) != 0 && !force) {
            fprintf(stderr,
                    "refusing to overwrite %s@%s (different sha256); use "
                    "--force\n",
                    m.name, m.version);
            return 1;
        }
        mkdir_p(dir);
        if (!copy_file(archive_path, dst)) {
            fprintf(stderr, "cannot write %s\n", dst);
            return 1;
        }
        if (!index_add(&ix, m.name, m.version, hex, artifact, deps) ||
            !write_index(idx_path, &ix)) {
            fprintf(stderr, "cannot write %s\n", idx_path);
            return 1;
        }
        printf("published %s@%s to %s\n", m.name, m.version, dir);
        return 0;
    }
}

int Pkg_Main(int argc, char **argv) {
    Arena *arena = Arena_New();
    int code;

    if (argc == 0 || !strcmp(argv[0], "-h") || !strcmp(argv[0], "--help")) {
        print_usage();
        Arena_Free(arena);
        return argc == 0 ? 2 : 0;
    }
    if (!strcmp(argv[0], "init"))
        code = pkg_init(arena, argc - 1, argv + 1);
    else if (!strcmp(argv[0], "add")) {
        if (argc - 1 == 1 && strchr(argv[1], '@'))
            code = pkg_install(arena, argc - 1, argv + 1);
        else
            code = pkg_add(arena, argc - 1, argv + 1);
    }
    else if (!strcmp(argv[0], "remove"))
        code = pkg_remove(arena, argc - 1, argv + 1);
    else if (!strcmp(argv[0], "list"))
        code = pkg_list(arena, argc - 1, argv + 1);
    else if (!strcmp(argv[0], "check"))
        code = pkg_check(arena, argc - 1, argv + 1);
    else if (!strcmp(argv[0], "install"))
        code = pkg_install(arena, argc - 1, argv + 1);
    else if (!strcmp(argv[0], "publish"))
        code = pkg_publish(arena, argc - 1, argv + 1);
    else {
        fprintf(stderr, "unknown pkg command: %s\n", argv[0]);
        print_usage();
        code = 2;
    }
    Arena_Free(arena);
    return code;
}

/* ---- 로더 연결 ---- */

static const char *find_manifest_dir(Arena *arena, const char *base_dir) {
    const char *start = base_dir && base_dir[0] ? base_dir : ".";
    char *dir = xstrndup(arena, start, strlen(start));
    int depth;
    for (depth = 0; depth < 32 && dir; depth++) {
        if (file_exists(join_path(arena, dir, MANIFEST)))
            return dir;
        dir = parent_dir(arena, dir);
    }
    return NULL;
}

void Pkg_LoadDeps(Arena *arena, const char *base_dir, Interp *in) {
    const char *dir = find_manifest_dir(arena, base_dir);
    Manifest m;
    size_t i;
    const char *env;

    if (dir && load_manifest_at(arena, dir, &m)) {
        char *vendor = NULL;
        for (i = 0; i < m.ndeps; i++) {
            const char *root = m.deps[i].path;
            if (is_registry_dep(root)) {
                if (!vendor)
                    vendor = join_path(arena, dir, VENDOR_DIR);
                if (dir_exists(vendor)) {
                    char *pkgdir = join_path(arena, vendor, m.deps[i].alias);
                    Interp_AddModuleDir(in, vendor);
                    if (dir_exists(pkgdir))
                        Interp_AddModuleDir(in, pkgdir);
                }
                continue;
            }
            if (!is_absolute(root))
                root = join_path(arena, dir, root);
            Interp_AddModuleDir(in, root);
            /* __init__.ast 패키지는 내부 모듈을 자기 디렉터리에서 찾는다. */
            {
                char *inner = join_path(arena, root, m.deps[i].alias);
                if (dir_exists(inner))
                    Interp_AddModuleDir(in, inner);
            }
        }
    }

    env = getenv("ASTER_PATH");
    if (env && env[0]) {
        char *copy = xstrndup(arena, env, strlen(env));
        char *p = copy;
        while (p && *p) {
            char *sep = strchr(p, ';');
#ifdef __unix__
            if (!sep)
                sep = strchr(p, ':');
#endif
            if (sep)
                *sep = 0;
            if (*p)
                Interp_AddModuleDir(in, Arena_Strndup(arena, p, strlen(p)));
            p = sep ? sep + 1 : NULL;
        }
    }
}
