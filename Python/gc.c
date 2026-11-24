#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal/gc.h"
#include "internal/runtime.h"
#include "internal/regex.h"

/*
 * 수집기 구조
 *
 * - 모든 블록은 GCBlock 머리말 뒤에 payload가 붙어 있고, 주소 순으로 정렬한
 *   색인에서 이진 탐색해 payload 안쪽 주소(내부 포인터)도 찾아낸다.
 * - 루트는 세 가지다: 지금 실행 중인 C 스택(+레지스터), 서스펜드된 파이버
 *   스택, 그리고 스택 밖에 있는 Interp(모듈 환경을 타입대로 따라간다).
 * - 블록 자체는 보수적으로 워드 단위로 훑되, 문자열과 원시 배열은 건너뛴다.
 *   값(RValue)에서 AST 구조로 이어지는 참조는 AST를 훑지 않고 필요한
 *   필드만 타입대로 따라가므로, 아레나 AST가 런타임 값을 붙잡고 있어도
 *   정확히 표시된다.
 */

typedef struct GCBlock {
    struct GCBlock *next;
    size_t size;
    unsigned kind;
    unsigned mark;
    unsigned pinned;
} GCBlock;

typedef struct GCStack {
    struct GCStack *next;
    char *low;               /* 마지막으로 기록한 사용 중 스택의 낮은 주소 */
    char *high;
    jmp_buf regs;            /* 서스펜드 시점의 레지스터 */
    int regs_valid;
} GCStack;

static GCBlock *g_blocks;
static size_t g_bytes;
static size_t g_threshold;
static int g_initialized;
static int g_collecting;
static int g_stress;
static int g_pinning;
static int g_enabled = 1;
static int g_debug;
static int g_trace;

/*
 * 크기 클래스 할당기.
 *
 * 인터프리터는 반복문 한 번에 작은 블록(환경, RValue, 바인딩)을 여러 개
 * 만든다. 블록마다 malloc/free를 부르면 그 비용이 인터프리터 실행 비용보다
 * 커지므로, 작은 블록은 슬랩에서 잘라 쓰고 수거된 블록은 크기 클래스별
 * 자유 목록에 되돌린다. 큰 블록만 시스템 할당자를 그대로 쓴다.
 */
#define GC_ALIGN 16
#define GC_SMALL_MAX 2048 /* 블록 전체 크기(머리말 포함) 상한 */
#define GC_NCLASSES (GC_SMALL_MAX / GC_ALIGN)
#define GC_SLAB_SIZE ((size_t)256 * 1024)

typedef struct GCSlab {
    struct GCSlab *next;
    size_t used, cap;
    char *mem;
} GCSlab;

static GCBlock *g_freecls[GC_NCLASSES];
static GCSlab *g_slabs;

static void *xrealloc_checked(void *p, size_t n);

static size_t block_class(size_t payload) {
    size_t total = sizeof(GCBlock) + (payload ? payload : 1);
    size_t cls = (total + GC_ALIGN - 1) / GC_ALIGN;
    return cls <= GC_NCLASSES ? cls : 0;
}

static void *slab_alloc(size_t bytes) {
    GCSlab *sl = g_slabs;

    if (!sl || sl->used + bytes > sl->cap) {
        size_t cap = bytes > GC_SLAB_SIZE ? bytes : GC_SLAB_SIZE;
        sl = xrealloc_checked(NULL, sizeof *sl);
        sl->mem = xrealloc_checked(NULL, cap);
        sl->used = 0;
        sl->cap = cap;
        sl->next = g_slabs;
        g_slabs = sl;
    }
    {
        void *p = sl->mem + sl->used;
        sl->used += bytes;
        return p;
    }
}

static GCBlock **g_index;
static size_t g_nindex;
static size_t g_capindex;

static GCBlock **g_gray;
static size_t g_ngray;
static size_t g_capgray;

static uintptr_t *g_seen;
static size_t g_seen_cap;
static size_t g_seen_count;

static GCStack *g_stacks;
static GCStack *g_current;
static GCStack *g_main_stack;

static struct Interp **g_interps;
static size_t g_ninterps;
static size_t g_capinterps;

typedef struct GCRoot {
    int kind;
    void **slot;
} GCRoot;

static GCRoot *g_roots;
static size_t g_nroots;
static size_t g_caproots;

static void mark_words(void *lo, void *hi);
static void mark_ptr(void *p);
static void mark_env(Env *env);
static void mark_ast_fn(AstFn *fn);
static void mark_ast_class(AstClass *cls);
static void mark_ast_module(AstModule *module);
static void mark_ast_enum(AstEnumType *type);
static void mark_ast_struct_type(AstStructType *type);
static void mark_ast_overload(AstOverload *overload);
static void mark_ast_builtin(AstBuiltin *builtin);
static void mark_instance(AstInstance *obj);
static void mark_struct_value(AstStructValue *stv);
static void mark_interp(struct Interp *in);

/* ---- 보수적 워드 스캔 ---- */

static void *xrealloc_checked(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("out of memory\n", stderr);
        abort();
    }
    return q;
}

static int block_cmp(const void *a, const void *b) {
    uintptr_t pa = (uintptr_t)(*(const GCBlock *const *)a + 1);
    uintptr_t pb = (uintptr_t)(*(const GCBlock *const *)b + 1);
    return pa < pb ? -1 : (pa > pb ? 1 : 0);
}

static GCBlock *find_block(void *p) {
    uintptr_t key = (uintptr_t)p;
    size_t lo = 0, hi = g_nindex;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uintptr_t start = (uintptr_t)(g_index[mid] + 1);
        if (start <= key)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    {
        GCBlock *b = g_index[lo - 1];
        uintptr_t start = (uintptr_t)(b + 1);
        if (key >= start && key < start + b->size)
            return b;
    }
    return NULL;
}

static void build_index(void) {
    GCBlock *b;
    size_t n = 0;

    for (b = g_blocks; b; b = b->next)
        n++;
    if (n > g_capindex) {
        size_t cap = g_capindex ? g_capindex : 256;
        while (cap < n)
            cap *= 2;
        g_index = xrealloc_checked(g_index, cap * sizeof *g_index);
        g_capindex = cap;
    }
    n = 0;
    for (b = g_blocks; b; b = b->next)
        g_index[n++] = b;
    g_nindex = n;
    if (n > 1)
        qsort(g_index, n, sizeof *g_index, block_cmp);
}

/* ---- 아레나 구조 순환 방지 ---- */

static void seen_reset(void) {
    if (g_seen)
        memset(g_seen, 0, g_seen_cap * sizeof *g_seen);
    g_seen_count = 0;
}

static void seen_grow(void) {
    size_t oldcap = g_seen_cap;
    uintptr_t *old = g_seen;
    size_t i;

    g_seen_cap = oldcap ? oldcap * 2 : 256;
    g_seen = xrealloc_checked(NULL, g_seen_cap * sizeof *g_seen);
    memset(g_seen, 0, g_seen_cap * sizeof *g_seen);
    g_seen_count = 0;
    for (i = 0; i < oldcap; i++) {
        uintptr_t key = old[i];
        size_t j;
        if (!key)
            continue;
        j = (size_t)((key >> 4) & (g_seen_cap - 1));
        while (g_seen[j])
            j = (j + 1) & (g_seen_cap - 1);
        g_seen[j] = key;
        g_seen_count++;
    }
    free(old);
}

/* 처음 보는 아레나 주소면 1, 이미 본 주소면 0. */
static int seen_add(const void *p) {
    uintptr_t key = (uintptr_t)p;
    size_t j;

    if (!key)
        return 0;
    if (!g_seen_cap)
        seen_grow();
    if ((g_seen_count + 1) * 10 >= g_seen_cap * 7)
        seen_grow();
    j = (size_t)((key >> 4) & (g_seen_cap - 1));
    while (g_seen[j]) {
        if (g_seen[j] == key)
            return 0;
        j = (j + 1) & (g_seen_cap - 1);
    }
    g_seen[j] = key;
    g_seen_count++;
    return 1;
}

/* ---- 표시 ---- */

static void gray_push(GCBlock *b) {
    if (g_ngray == g_capgray) {
        size_t cap = g_capgray ? g_capgray * 2 : 256;
        g_gray = xrealloc_checked(g_gray, cap * sizeof *g_gray);
        g_capgray = cap;
    }
    g_gray[g_ngray++] = b;
}

static void follow_rvalue(RValue *v) {
    if (!v)
        return;
    switch (v->kind) {
    case RV_STR:
        mark_ptr(v->u.s);
        break;
    case RV_OBJECT:
        mark_instance(v->u.obj);
        break;
    case RV_STRUCT:
        mark_struct_value(v->u.stv);
        break;
    case RV_FUNC:
        mark_ast_fn(v->u.fn);
        break;
    case RV_BUILTIN:
        mark_ptr(v->u.builtin);
        mark_ast_builtin(v->u.builtin);
        break;
    case RV_PTR:
        if (v->u.ptr.slot)
            mark_ptr(*v->u.ptr.slot);
        break;
    case RV_EXC:
        mark_ptr(v->u.exc);
        break;
    case RV_CLASS:
        mark_ast_class(v->u.cls);
        break;
    case RV_STRUCTTYPE:
        mark_ast_struct_type(v->u.stype);
        break;
    case RV_LIST:
        mark_ptr(v->u.list);
        break;
    case RV_TUPLE:
        mark_ptr(v->u.tuple);
        break;
    case RV_DICT:
        mark_ptr(v->u.dict);
        break;
    case RV_SET:
    case RV_FROZENSET:
        mark_ptr(v->u.set);
        break;
    case RV_MODULE:
        mark_ast_module(v->u.module);
        break;
    case RV_SUPER:
        mark_ptr(v->u.super);
        break;
    case RV_ENUM:
        mark_ast_enum(v->u.en.type);
        mark_ptr(v->u.en.payload);
        break;
    case RV_ENUMTYPE:
        mark_ast_enum(v->u.etype);
        break;
    case RV_OVERLOAD:
        mark_ast_overload(v->u.overload);
        break;
    case RV_GENERATOR:
        mark_ptr(v->u.gen);
        break;
    case RV_VARIANT_CTOR:
        mark_ast_enum(v->u.vc.type);
        break;
    default:
        break;
    }
}

static void follow_block(GCBlock *b) {
    char *payload = (char *)(b + 1);

    switch (b->kind) {
    case GK_RVALUE:
        follow_rvalue((RValue *)payload);
        break;
    case GK_INSTANCE:
        mark_instance((AstInstance *)payload);
        break;
    case GK_STRUCTVALUE:
        mark_struct_value((AstStructValue *)payload);
        break;
    case GK_EXCEPTION:
        mark_ptr(((AstException *)payload)->message);
        break;
    case GK_SUPER: {
        AsterSuper *s = (AsterSuper *)payload;
        mark_ast_class(s->owner);
        mark_ptr(s->self);
        break;
    }
    case GK_GENERATOR: {
        AstGenerator *g = (AstGenerator *)payload;
        mark_ast_fn(g->fn);
        mark_env(g->body_env);
        mark_ptr(g->self);
        mark_ptr(g->yielded);
        mark_ptr(g->interp);
        mark_ptr(g->caller_gen);
        break;
    }
    case GK_INTERP:
        mark_interp((struct Interp *)payload);
        break;
    case GK_BUILTIN:
        mark_ast_builtin((AstBuiltin *)payload);
        break;
    case GK_AST_FN:
        mark_ast_fn((AstFn *)payload);
        break;
    case GK_AST_OVERLOAD:
        mark_ast_overload((AstOverload *)payload);
        break;
    default:
        break;
    }
}

static void mark_block(GCBlock *b) {
    if (b->mark)
        return;
    b->mark = 1;
    follow_block(b);
    gray_push(b);
}

static void mark_ptr(void *p) {
    GCBlock *b;

    if (!p)
        return;
    b = find_block(p);
    if (b)
        mark_block(b);
}

void Aster_GC_MarkPointer(void *p) {
    if (g_collecting)
        mark_ptr(p);
}

static void mark_words(void *lo, void *hi) {
    uintptr_t p = (uintptr_t)lo;
    uintptr_t end = (uintptr_t)hi;
    const uintptr_t mask = sizeof(void *) - 1;

    p = (p + mask) & ~mask;
    while (p + sizeof(void *) <= end) {
        void *word;
        memcpy(&word, (void *)p, sizeof word);
        if (word)
            mark_ptr(word);
        p += sizeof(void *);
    }
}

/* ---- AST/환경 타입별 따라가기 ---- */

static void mark_env(Env *env) {
    mark_ptr(env);
}

static void mark_ast_fn(AstFn *fn) {
    if (!fn)
        return;
    mark_ptr(fn);   /* 실행 중 복사본이면 GC 블록 자체를 표시한다 */
    if (!seen_add(fn))
        return;
    mark_env(fn->env);
    mark_ptr(fn->self);
    mark_ptr(fn->builtin.self);
    if (fn->owner)
        mark_ast_class(fn->owner);
}

static void mark_ast_class(AstClass *cls) {
    size_t i;

    if (!cls || !seen_add(cls))
        return;
    if (cls->base)
        mark_ast_class(cls->base);
    mark_env(cls->env);
    if (cls->static_values) {
        for (i = 0; i < cls->nfields; i++)
            mark_ptr(cls->static_values[i]);
    }
    if (cls->methods) {
        for (i = 0; i < cls->nmethods; i++)
            mark_ast_fn(cls->methods[i].fn);
    }
    if (cls->props) {
        for (i = 0; i < cls->nprops; i++) {
            mark_ast_fn(cls->props[i].getter);
            mark_ast_fn(cls->props[i].setter);
        }
    }
    if (cls->ctors) {
        for (i = 0; i < cls->nctors; i++)
            mark_ast_fn(cls->ctors[i]);
    }
}

static void mark_ast_module(AstModule *module) {
    if (!module || !seen_add(module))
        return;
    mark_env(module->env);
}

static void mark_ast_enum(AstEnumType *type) {
    size_t i;

    if (!type || !seen_add(type))
        return;
    mark_env(type->env);
    if (type->members) {
        for (i = 0; i < type->nmembers; i++)
            mark_ptr(type->members[i].value);
    }
    if (type->methods) {
        for (i = 0; i < type->nmethods; i++)
            mark_ast_fn(type->methods[i].fn);
    }
}

static void mark_ast_struct_type(AstStructType *type) {
    if (!type || !seen_add(type))
        return;
    mark_env(type->env);
}

static void mark_ast_overload(AstOverload *overload) {
    size_t i;

    if (!overload)
        return;
    mark_ptr(overload);   /* 실행 중 복사본이면 GC 블록 자체를 표시한다 */
    if (!seen_add(overload))
        return;
    if (!overload->fns)
        return;
    for (i = 0; i < overload->count; i++)
        mark_ast_fn(overload->fns[i]);
}

static void mark_ast_builtin(AstBuiltin *builtin) {
    if (!builtin || !seen_add(builtin))
        return;
    mark_ptr(builtin->self);
}

static void mark_instance(AstInstance *obj) {
    size_t i;

    if (!obj)
        return;
    mark_ptr(obj);   /* GC 인스턴스면 블록 표시 (아레나 인스턴스는 무시) */
    if (!seen_add(obj))
        return;
    mark_ast_class(obj->cls);
    if (obj->fields) {
        for (i = 0; i < obj->nfields; i++)
            mark_ptr(obj->fields[i].cell);
    }
}

static void mark_struct_value(AstStructValue *stv) {
    size_t i;

    if (!stv)
        return;
    mark_ptr(stv);
    if (!seen_add(stv))
        return;
    mark_ast_struct_type(stv->type);
    if (stv->type && stv->cells) {
        for (i = 0; i < stv->type->nfields; i++)
            mark_ptr(stv->cells[i]);
    }
}

static void mark_interp(struct Interp *in) {
    size_t i;

    if (!in)
        return;
    mark_env(in->globals);
    mark_ptr(in->ret);
    mark_ptr(in->exc);
    mark_env(in->free_envs);
    mark_env(in->free_block_envs);
    mark_ptr(in->gen);
    for (i = 0; i < in->nmodules; i++)
        mark_ast_module(in->modules[i]);
    for (i = 0; i < in->nbuiltin_modules; i++)
        mark_ast_module(in->builtin_modules[i]);
    Regex_MarkInterp(in);
}

/* ---- 루트 ---- */

static void mark_temp_roots(void) {
    size_t i;

    for (i = 0; i < g_nroots; i++) {
        void *p = g_roots[i].slot ? *g_roots[i].slot : NULL;
        if (!p)
            continue;
        switch (g_roots[i].kind) {
        case GC_ROOT_FN:
            mark_ast_fn((AstFn *)p);
            break;
        case GC_ROOT_CLASS:
            mark_ast_class((AstClass *)p);
            break;
        case GC_ROOT_ENUM:
            mark_ast_enum((AstEnumType *)p);
            break;
        case GC_ROOT_STRUCTTYPE:
            mark_ast_struct_type((AstStructType *)p);
            break;
        case GC_ROOT_OVERLOAD:
            mark_ast_overload((AstOverload *)p);
            break;
        default:
            mark_ptr(p);
            break;
        }
    }
}

void Aster_GC_PushRoot(GRootKind kind, void **slot) {
    if (g_nroots == g_caproots) {
        size_t cap = g_caproots ? g_caproots * 2 : 16;
        g_roots = xrealloc_checked(g_roots, cap * sizeof *g_roots);
        g_caproots = cap;
    }
    g_roots[g_nroots].kind = (int)kind;
    g_roots[g_nroots].slot = slot;
    g_nroots++;
}

void Aster_GC_PopRoot(void) {
    if (g_nroots)
        g_nroots--;
}

static void mark_roots(void) {
    jmp_buf regs;
    char anchor;
    GCStack *st;
    size_t i;

    (void)setjmp(regs);
    mark_words(&regs, (char *)&regs + sizeof regs);

    if (!g_current)
        g_current = g_main_stack;
    if (g_current)
        g_current->low = &anchor;

    for (st = g_stacks; st; st = st->next) {
        char *low = st == g_current ? &anchor : st->low;
        if (low && st->high && low < st->high)
            mark_words(low, st->high);
        if (st != g_current && st->regs_valid)
            mark_words(&st->regs, (char *)&st->regs + sizeof st->regs);
    }
    for (i = 0; i < g_ninterps; i++)
        mark_interp(g_interps[i]);
    mark_temp_roots();
}

static void drain_gray(void) {
    while (g_ngray) {
        GCBlock *b = g_gray[--g_ngray];
        switch (b->kind) {
        case GK_STRING:
        case GK_RAW:
        case GK_RAW_ARRAY:
            break;
        default:
            mark_words(b + 1, (char *)(b + 1) + b->size);
            break;
        }
    }
}

static void sweep(void) {
    GCBlock **link = &g_blocks;

    while (*link) {
        GCBlock *b = *link;
        if (b->mark || b->pinned) {
            b->mark = 0;
            link = &b->next;
        }
        else {
            size_t cls = block_class(b->size);
            if (g_trace)
                fprintf(stderr, "[gc] free kind=%u size=%zu at %p\n", b->kind, b->size,
                        (void *)(b + 1));
            *link = b->next;
            g_bytes -= b->size;
            if (cls) {
                b->next = g_freecls[cls - 1];
                g_freecls[cls - 1] = b;
            }
            else {
                free(b);
            }
        }
    }
}

static void adjust_threshold(void) {
    size_t target = g_bytes * 2;
    if (target < (size_t)2 * 1024 * 1024)
        target = (size_t)2 * 1024 * 1024;
    g_threshold = target;
}

void Aster_GC_Collect(void) {
    size_t before;

    if (!g_initialized || g_collecting || !g_enabled)
        return;
    g_collecting = 1;
    before = g_bytes;
    if (g_trace)
        fprintf(stderr, "[gc] collect begin (%zu bytes)\n", before);
    build_index();
    seen_reset();
    mark_roots();
    if (g_trace)
        fprintf(stderr, "[gc] roots done (gray=%zu)\n", g_ngray);
    drain_gray();
    if (g_trace)
        fprintf(stderr, "[gc] drain done\n");
    sweep();
    adjust_threshold();
    g_collecting = 0;
    if (g_debug)
        fprintf(stderr, "[gc] %zu KiB -> %zu KiB (%zu blocks)\n", before / 1024,
                g_bytes / 1024, g_nindex);
}

/* ---- 할당 ---- */

static void gc_init_internal(void *stack_anchor) {
    char probe;
    const char *env;

    if (g_initialized)
        return;
    g_initialized = 1;
    g_threshold = (size_t)8 * 1024 * 1024;
    env = getenv("ASTER_GC_STRESS");
    if (env && *env && strcmp(env, "0") != 0)
        g_stress = 1;
    env = getenv("ASTER_GC_DEBUG");
    if (env && *env && strcmp(env, "0") != 0)
        g_debug = 1;
    env = getenv("ASTER_GC_OFF");
    if (env && *env && strcmp(env, "0") != 0)
        g_enabled = 0;
    env = getenv("ASTER_GC_TRACE");
    if (env && *env && strcmp(env, "0") != 0)
        g_trace = 1;
    g_main_stack = xrealloc_checked(NULL, sizeof *g_main_stack);
    memset(g_main_stack, 0, sizeof *g_main_stack);
    g_main_stack->high = stack_anchor ? (char *)stack_anchor : &probe;
    g_main_stack->low = g_main_stack->high;
    g_main_stack->next = NULL;
    g_stacks = g_main_stack;
    g_current = g_main_stack;
    atexit(Aster_GC_Shutdown);
}

void Aster_GC_Init(void *stack_anchor) {
    gc_init_internal(stack_anchor);
}

int Aster_GC_IsInitialized(void) {
    return g_initialized;
}

void *Aster_GC_Alloc(size_t size, GKind kind) {
    GCBlock *b;
    size_t cls;

    if (!g_initialized)
        gc_init_internal(NULL);
    if (!g_collecting && g_enabled && (g_stress || g_bytes > g_threshold))
        Aster_GC_Collect();
    cls = block_class(size);
    if (cls) {
        size_t total = cls * GC_ALIGN;
        b = g_freecls[cls - 1];
        if (b)
            g_freecls[cls - 1] = b->next;
        else
            b = slab_alloc(total);
    }
    else {
        b = xrealloc_checked(NULL, sizeof *b + (size ? size : 1));
    }
    b->next = g_blocks;
    b->size = size;
    b->kind = (unsigned)kind;
    b->mark = 0;
    b->pinned = (unsigned)g_pinning;
    g_blocks = b;
    g_bytes += size;
    if (size)
        memset((char *)(b + 1), 0, size);
    if (g_trace)
        fprintf(stderr, "[gc] alloc kind=%u size=%zu at %p (live=%zu) pinned=%d\n",
                (unsigned)kind, size, (void *)(b + 1), g_bytes, (int)g_pinning);
    return b + 1;
}

void *Aster_GC_Grow(const void *old, size_t old_size, size_t new_size, GKind kind) {
    void *p = Aster_GC_Alloc(new_size, kind);
    if (old && old_size) {
        size_t n = old_size < new_size ? old_size : new_size;
        memcpy(p, old, n);
    }
    return p;
}

size_t Aster_GC_LiveBytes(void) {
    return g_bytes;
}

void Aster_GC_SetStress(int on) {
    g_stress = on ? 1 : 0;
}

int Aster_GC_Stress(void) {
    return g_stress;
}

void Aster_GC_SetPinning(int on) {
    if (g_trace)
        fprintf(stderr, "[gc] SetPinning(%d)\n", on);
    g_pinning = on ? 1 : 0;
}

int Aster_GC_Pinning(void) {
    return g_pinning;
}

/* ---- 실행 상태 루트 ---- */

void Aster_GC_AddInterp(struct Interp *in) {
    size_t i;

    if (!g_initialized)
        gc_init_internal(NULL);
    for (i = 0; i < g_ninterps; i++)
        if (g_interps[i] == in)
            return;
    if (g_ninterps == g_capinterps) {
        size_t cap = g_capinterps ? g_capinterps * 2 : 4;
        g_interps = xrealloc_checked(g_interps, cap * sizeof *g_interps);
        g_capinterps = cap;
    }
    g_interps[g_ninterps++] = in;
}

void Aster_GC_RemoveInterp(struct Interp *in) {
    size_t i;

    for (i = 0; i < g_ninterps; i++) {
        if (g_interps[i] == in) {
            g_interps[i] = g_interps[--g_ninterps];
            return;
        }
    }
}

void *Aster_GC_NewFiberStack(void *high) {
    GCStack *st;

    if (!g_initialized)
        gc_init_internal(NULL);
    st = xrealloc_checked(NULL, sizeof *st);
    memset(st, 0, sizeof *st);
    st->high = (char *)high;
    st->low = st->high;
    st->next = g_stacks;
    g_stacks = st;
    return st;
}

void Aster_GC_FreeFiberStack(void *stack) {
    GCStack *st = stack;
    GCStack **link;

    if (!st)
        return;
    for (link = &g_stacks; *link; link = &(*link)->next) {
        if (*link == st) {
            *link = st->next;
            break;
        }
    }
    if (g_current == st)
        g_current = g_main_stack;
    free(st);
}

void Aster_GC_EnterFiberStack(void *stack) {
    g_current = stack ? (GCStack *)stack : g_main_stack;
}

void Aster_GC_EnterMainStack(void) {
    g_current = g_main_stack;
}

void Aster_GC_SuspendCurrent(void) {
    char anchor;
    GCStack *st;

    if (!g_initialized)
        return;
    st = g_current ? g_current : g_main_stack;
    if (!st)
        return;
    st->low = &anchor;
    st->regs_valid = 1;
    (void)setjmp(st->regs);
}

/* ---- 종료 ---- */

void Aster_GC_Shutdown(void) {
    GCBlock *b;
    GCSlab *sl;
    size_t i;

    if (!g_initialized)
        return;
    g_initialized = 0;
    g_collecting = 1;
    b = g_blocks;
    while (b) {
        GCBlock *next = b->next;
        if (!block_class(b->size))
            free(b);
        b = next;
    }
    g_blocks = NULL;
    sl = g_slabs;
    while (sl) {
        GCSlab *next = sl->next;
        free(sl->mem);
        free(sl);
        sl = next;
    }
    g_slabs = NULL;
    for (i = 0; i < GC_NCLASSES; i++)
        g_freecls[i] = NULL;
    g_bytes = 0;
    g_nindex = 0;
    free(g_index);
    g_index = NULL;
    g_capindex = 0;
    free(g_gray);
    g_gray = NULL;
    g_capgray = 0;
    g_ngray = 0;
    free(g_seen);
    g_seen = NULL;
    g_seen_cap = 0;
    g_seen_count = 0;
    g_stacks = NULL;
    g_current = NULL;
    free(g_main_stack);
    g_main_stack = NULL;
    g_ninterps = 0;
    free(g_interps);
    g_interps = NULL;
    g_capinterps = 0;
    free(g_roots);
    g_roots = NULL;
    g_caproots = 0;
    g_nroots = 0;
    g_collecting = 0;
    if (g_debug)
        fprintf(stderr, "[gc] shutdown\n");
}
