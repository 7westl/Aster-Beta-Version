/*
 * 바이트코드 VM.
 *
 * 컴파일러는 AST를 스택 머신 명령으로 바꾸고, 지원하지 않는 노드는 실행 중에
 * 트리 워커(Interp_ExecStmtNode / Interp_EvalExprNode)로 그대로 넘긴다.
 * 따라서 바이트코드가 다루지 않는 문법도 의미가 달라지지 않는다.
 *
 * 값 스택은 GC가 관리하는 포인터 배열(GK_PTR_ARRAY)이라 수집 중에도 루트로
 * 보인다. 환경/이터레이터 상태는 C 스택의 프레임에 두어 보수적 스캔이 찾는다.
 */
#include <stdio.h>
#include <string.h>

#include "internal/ast.h"
#include "internal/gc.h"
#include "internal/interp.h"
#include "internal/object.h"
#include "internal/runtime.h"
#include "internal/vm.h"

/* ---- 바이트코드 ---- */

enum BcOp {
    BC_NOP = 0,
    BC_NULL,
    BC_CONST,        /* a=상수 번호 */
    BC_NAME,         /* a=이름 번호 */
    BC_SELF,
    BC_POP,
    BC_LIST,         /* a=원소 수 */
    BC_TUPLE,        /* a=원소 수 */
    BC_SET,          /* a=원소 수 */
    BC_DICT,         /* a=엔트리 수 (키,값 쌍) */
    BC_ENTER_BLOCK,
    BC_LEAVE_BLOCK,
    BC_UNARY,        /* a=unaryop_ty */
    BC_BINOP,        /* a=operator_ty */
    BC_COMPARE,      /* a=cmpop_ty */
    BC_IS,           /* p=typeref */
    BC_IN,
    BC_CAST,         /* p=typeref */
    BC_SIZEOF,       /* p=typeref */
    BC_DEREF,
    BC_ATTR,         /* a=이름 번호 */
    BC_OPT_ATTR,     /* a=이름 번호 */
    BC_PTR_ATTR,     /* a=이름 번호 */
    BC_GETITEM,
    BC_CALL,         /* a=인자 수 */
    BC_CALL_KW,      /* a=호출 설명 번호 */
    BC_DELEGATE_EXPR,/* p=expr */
    BC_DELEGATE_STMT,/* p=stmt */
    BC_EXPR_POP,
    BC_DEFINE,       /* p=VarDecl */
    BC_ASSIGN,       /* a=assign_op_ty, p=target expr */
    BC_RETURN,
    BC_RETURN_VALUE,
    BC_JUMP,         /* a=대상 */
    BC_JUMP_IF_FALSE,/* a=대상, 값을 꺼낸다 */
    BC_JUMP_IF_FALSE_KEEP,
    BC_JUMP_IF_TRUE_KEEP,
    BC_JUMP_IF_NULL_KEEP,
    BC_LOOP_PUSH,    /* c=continue, d=end */
    BC_LOOP_POP,
    BC_RANGE_BEGIN,  /* a=이름, b=has_step, c=continue, d=end */
    BC_RANGE_TEST,   /* a=끝 */
    BC_RANGE_STEP,
    BC_FORIN_BEGIN,  /* a=이름, c=continue, d=end, p=선언 타입 */
    BC_FORIN_NEXT,   /* a=끝, p=선언 타입 */
    BC_BREAK,
    BC_CONTINUE,
    BC_LINE,         /* 문장 시작: 줄 번호/프레임 갱신 */
    BC_END
};

typedef struct {
    unsigned char op;
    int a, b, c, d;
    const void *p;
    int line, col, span;
} BcInsn;

typedef struct {
    int tag;        /* 0=null, 1=bool, 2=int, 3=float, 4=str */
    int64_t i;
    double f;
    const void *s;  /* AsterString* (파서가 고정해 둔 리터럴) */
} BcConst;

typedef struct {
    int nargs;
    int nkw;
    const char **kwn;
} BcCallDesc;

struct BcProto {
    Arena *arena;
    BcInsn *code;
    size_t ncode, ccap;
    BcConst *consts;
    size_t nconsts, xcap;
    const char **names;
    size_t nnames, ncap;
    BcCallDesc *calls;
    size_t ncalls, kcap;
    int max_blocks;
    int max_loops;
};

/* ---- 컴파일 ---- */

#define BC_MAX_BLOCKS 256
#define BC_MAX_LOOPS 64

typedef struct BcLoopCtx {
    struct BcLoopCtx *prev;
    size_t push_pc;
} BcLoopCtx;

typedef struct {
    Interp *in;
    BcProto *p;
    int failed;
    int depth;
    int max_depth;
    int loop_depth;
    int max_loop_depth;
    BcLoopCtx *loop;
} BcCtx;

static void bc_expr(BcCtx *c, expr_ty e);
static void bc_stmt(BcCtx *c, stmt_ty s);

static size_t bc_emit(BcCtx *c, int op, int a, int b, int cc, int dd, const void *p,
                      int line, int col, int span) {
    BcProto *p_ = c->p;

    if (p_->ncode == p_->ccap) {
        size_t ncap = p_->ccap ? p_->ccap * 2 : 64;
        BcInsn *ins = Arena_Alloc(p_->arena, ncap * sizeof(BcInsn));
        if (p_->ncode)
            memcpy(ins, p_->code, p_->ncode * sizeof(BcInsn));
        p_->code = ins;
        p_->ccap = ncap;
    }
    p_->code[p_->ncode].op = (unsigned char)op;
    p_->code[p_->ncode].a = a;
    p_->code[p_->ncode].b = b;
    p_->code[p_->ncode].c = cc;
    p_->code[p_->ncode].d = dd;
    p_->code[p_->ncode].p = p;
    p_->code[p_->ncode].line = line;
    p_->code[p_->ncode].col = col;
    p_->code[p_->ncode].span = span;
    return p_->ncode++;
}

static void expr_pos(expr_ty e, int *line, int *col, int *span) {
    int sp;
    *line = e->lineno;
    *col = e->col_offset;
    sp = (e->lineno == e->end_lineno && e->end_col_offset > e->col_offset)
             ? e->end_col_offset - e->col_offset
             : 1;
    if (sp > 40)
        sp = 40;
    *span = sp;
}

static size_t emit_e(BcCtx *c, int op, int a, int b, int cc, int dd, const void *p,
                     expr_ty e) {
    int line, col, span;
    expr_pos(e, &line, &col, &span);
    return bc_emit(c, op, a, b, cc, dd, p, line, col, span);
}

static size_t emit_s(BcCtx *c, int op, int a, int b, int cc, int dd, const void *p,
                     stmt_ty s) {
    return bc_emit(c, op, a, b, cc, dd, p, s->lineno, s->col_offset, 1);
}

static int bc_name(BcCtx *c, const char *name) {
    BcProto *p = c->p;
    size_t i;
    for (i = 0; i < p->nnames; i++)
        if (p->names[i] == name || !strcmp(p->names[i], name))
            return (int)i;
    if (p->nnames == p->ncap) {
        size_t ncap = p->ncap ? p->ncap * 2 : 16;
        const char **n = Arena_Alloc(p->arena, ncap * sizeof(const char *));
        if (p->nnames)
            memcpy(n, p->names, p->nnames * sizeof(const char *));
        p->names = n;
        p->ncap = ncap;
    }
    p->names[p->nnames] = name;
    return (int)p->nnames++;
}

static int bc_const(BcCtx *c, Value v) {
    BcProto *p = c->p;
    BcConst k;
    size_t i;

    memset(&k, 0, sizeof k);
    switch (v.tag) {
    case VT_BOOL:
        k.tag = 1;
        k.i = v.u.i != 0;
        break;
    case VT_INT:
        k.tag = 2;
        k.i = v.u.i;
        break;
    case VT_FLOAT:
        k.tag = 3;
        k.f = v.u.f;
        break;
    case VT_OBJ:
        if (v.u.o && v.u.o->kind == ASTER_OBJ_STRING) {
            k.tag = 4;
            k.s = v.u.o;
        }
        else {
            k.tag = 0;
        }
        break;
    default:
        k.tag = 0;
        break;
    }
    for (i = 0; i < p->nconsts; i++) {
        const BcConst *o = &p->consts[i];
        if (o->tag != k.tag)
            continue;
        if (k.tag == 3 && o->f != k.f)
            continue;
        if (k.tag != 3 && o->i != k.i)
            continue;
        if (k.tag == 4 && o->s != k.s)
            continue;
        return (int)i;
    }
    if (p->nconsts == p->xcap) {
        size_t ncap = p->xcap ? p->xcap * 2 : 16;
        BcConst *n = Arena_Alloc(p->arena, ncap * sizeof(BcConst));
        if (p->nconsts)
            memcpy(n, p->consts, p->nconsts * sizeof(BcConst));
        p->consts = n;
        p->xcap = ncap;
    }
    p->consts[p->nconsts] = k;
    return (int)p->nconsts++;
}

static int bc_call_desc(BcCtx *c, int nargs, int nkw, asdl_seq *keywords) {
    BcProto *p = c->p;
    size_t i;

    if (p->ncalls == p->kcap) {
        size_t ncap = p->kcap ? p->kcap * 2 : 8;
        BcCallDesc *n = Arena_Alloc(p->arena, ncap * sizeof(BcCallDesc));
        if (p->ncalls)
            memcpy(n, p->calls, p->ncalls * sizeof(BcCallDesc));
        p->calls = n;
        p->kcap = ncap;
    }
    p->calls[p->ncalls].nargs = nargs;
    p->calls[p->ncalls].nkw = nkw;
    p->calls[p->ncalls].kwn =
        nkw ? Arena_Alloc(p->arena, (size_t)nkw * sizeof(const char *)) : NULL;
    for (i = 0; i < (size_t)nkw; i++) {
        keyword_ty kw = asdl_seq_GET(keywords, i);
        p->calls[p->ncalls].kwn[i] = kw->name;
    }
    return (int)p->ncalls++;
}

static void enter_block(BcCtx *c) {
    c->depth++;
    if (c->depth > c->max_depth)
        c->max_depth = c->depth;
    if (c->depth > BC_MAX_BLOCKS)
        c->failed = 1;
}

static void leave_block(BcCtx *c) {
    c->depth--;
}

static void patch(BcCtx *c, size_t at, int field, size_t target) {
    BcInsn *ins = &c->p->code[at];
    switch (field) {
    case 0: ins->a = (int)target; break;
    case 1: ins->b = (int)target; break;
    case 2: ins->c = (int)target; break;
    default: ins->d = (int)target; break;
    }
}

static int has_spread(asdl_seq *seq) {
    size_t i;
    for (i = 0; i < asdl_seq_LEN(seq); i++) {
        expr_ty e = asdl_seq_GET(seq, i);
        if (e && e->kind == Spread_kind)
            return 1;
    }
    return 0;
}

static void delegate_expr(BcCtx *c, expr_ty e) {
    emit_e(c, BC_DELEGATE_EXPR, 0, 0, 0, 0, e, e);
}

static void compile_call(BcCtx *c, expr_ty e) {
    asdl_seq *args = e->v.Call.args;
    asdl_seq *kws = e->v.Call.keywords;
    size_t nargs = asdl_seq_LEN(args);
    size_t nkw = asdl_seq_LEN(kws);
    size_t i;
    size_t jnull = 0, jend = 0;
    int optional = e->v.Call.func->kind == OptionalAttribute_kind;

    if (has_spread(args)) {
        delegate_expr(c, e);
        return;
    }
    if (optional) {
        expr_ty attr = e->v.Call.func;
        bc_expr(c, attr->v.OptionalAttribute.value);
        jnull = emit_e(c, BC_JUMP_IF_NULL_KEEP, 0, 0, 0, 0, NULL, attr);
        emit_e(c, BC_ATTR, bc_name(c, attr->v.OptionalAttribute.attr), 0, 0, 0, NULL,
               attr);
    }
    else {
        bc_expr(c, e->v.Call.func);
    }
    for (i = 0; i < nargs; i++)
        bc_expr(c, asdl_seq_GET(args, i));
    if (nkw) {
        int desc;
        for (i = 0; i < nkw; i++) {
            keyword_ty kw = asdl_seq_GET(kws, i);
            bc_expr(c, kw->value);
        }
        desc = bc_call_desc(c, (int)nargs, (int)nkw, kws);
        emit_e(c, BC_CALL_KW, desc, 0, 0, 0, NULL, e);
    }
    else {
        emit_e(c, BC_CALL, (int)nargs, 0, 0, 0, NULL, e);
    }
    if (optional) {
        /* null 경로에서 인자를 평가하지 않고 결과만 남긴다. */
        jend = emit_e(c, BC_JUMP, 0, 0, 0, 0, NULL, e);
        patch(c, jnull, 0, c->p->ncode);
        patch(c, jend, 0, c->p->ncode);
    }
}

static void compile_seq_items(BcCtx *c, asdl_seq *seq) {
    size_t i;
    for (i = 0; i < asdl_seq_LEN(seq); i++)
        bc_expr(c, asdl_seq_GET(seq, i));
}

static void bc_expr(BcCtx *c, expr_ty e) {
    if (c->failed)
        return;
    if (!e) {
        c->failed = 1;
        return;
    }
    switch (e->kind) {
    case Constant_kind:
        emit_e(c, BC_CONST, bc_const(c, e->v.Constant.value), 0, 0, 0, NULL, e);
        return;
    case Name_kind:
        emit_e(c, BC_NAME, bc_name(c, e->v.Name.id), 0, 0, 0, NULL, e);
        return;
    case Self_kind:
        emit_e(c, BC_SELF, 0, 0, 0, 0, NULL, e);
        return;
    case BoolOp_kind: {
        size_t j;
        bc_expr(c, e->v.BoolOp.left);
        j = emit_e(c, e->v.BoolOp.op == And ? BC_JUMP_IF_FALSE_KEEP
                                           : BC_JUMP_IF_TRUE_KEEP,
                   0, 0, 0, 0, NULL, e);
        emit_e(c, BC_POP, 0, 0, 0, 0, NULL, e);
        bc_expr(c, e->v.BoolOp.right);
        patch(c, j, 0, c->p->ncode);
        return;
    }
    case UnaryOp_kind:
        bc_expr(c, e->v.UnaryOp.operand);
        emit_e(c, BC_UNARY, (int)e->v.UnaryOp.op, 0, 0, 0, NULL, e);
        return;
    case BinOp_kind:
        bc_expr(c, e->v.BinOp.left);
        bc_expr(c, e->v.BinOp.right);
        emit_e(c, BC_BINOP, (int)e->v.BinOp.op, 0, 0, 0, NULL, e);
        return;
    case Compare_kind:
        bc_expr(c, e->v.Compare.left);
        bc_expr(c, e->v.Compare.right);
        emit_e(c, BC_COMPARE, (int)e->v.Compare.op, 0, 0, 0, NULL, e);
        return;
    case Is_kind:
        bc_expr(c, e->v.Is.value);
        emit_e(c, BC_IS, 0, 0, 0, 0, e->v.Is.ty, e);
        return;
    case In_kind:
        bc_expr(c, e->v.In.item);
        bc_expr(c, e->v.In.container);
        emit_e(c, BC_IN, 0, 0, 0, 0, NULL, e);
        return;
    case Cast_kind:
        bc_expr(c, e->v.Cast.value);
        emit_e(c, BC_CAST, 0, 0, 0, 0, e->v.Cast.ty, e);
        return;
    case Deref_kind:
        bc_expr(c, e->v.Deref.value);
        emit_e(c, BC_DEREF, 0, 0, 0, 0, NULL, e);
        return;
    case SizeOf_kind:
        emit_e(c, BC_SIZEOF, 0, 0, 0, 0, e->v.SizeOf.ty, e);
        return;
    case Attribute_kind:
        bc_expr(c, e->v.Attribute.value);
        emit_e(c, BC_ATTR, bc_name(c, e->v.Attribute.attr), 0, 0, 0, NULL, e);
        return;
    case PtrAttribute_kind:
        bc_expr(c, e->v.PtrAttribute.value);
        emit_e(c, BC_PTR_ATTR, bc_name(c, e->v.PtrAttribute.attr), 0, 0, 0, NULL, e);
        return;
    case OptionalAttribute_kind:
        bc_expr(c, e->v.OptionalAttribute.value);
        emit_e(c, BC_OPT_ATTR, bc_name(c, e->v.OptionalAttribute.attr), 0, 0, 0, NULL,
               e);
        return;
    case Subscript_kind:
        if (e->v.Subscript.index->kind == Slice_kind) {
            delegate_expr(c, e);
            return;
        }
        bc_expr(c, e->v.Subscript.value);
        bc_expr(c, e->v.Subscript.index);
        emit_e(c, BC_GETITEM, 0, 0, 0, 0, NULL, e);
        return;
    case Call_kind:
        compile_call(c, e);
        return;
    case Ternary_kind: {
        size_t jf, jend;
        bc_expr(c, e->v.Ternary.test);
        jf = emit_e(c, BC_JUMP_IF_FALSE, 0, 0, 0, 0, NULL, e);
        bc_expr(c, e->v.Ternary.body);
        jend = emit_e(c, BC_JUMP, 0, 0, 0, 0, NULL, e);
        patch(c, jf, 0, c->p->ncode);
        bc_expr(c, e->v.Ternary.orelse);
        patch(c, jend, 0, c->p->ncode);
        return;
    }
    case Coalesce_kind: {
        size_t jnull, jend;
        bc_expr(c, e->v.Coalesce.left);
        jnull = emit_e(c, BC_JUMP_IF_NULL_KEEP, 0, 0, 0, 0, NULL, e);
        jend = emit_e(c, BC_JUMP, 0, 0, 0, 0, NULL, e);
        patch(c, jnull, 0, c->p->ncode);
        emit_e(c, BC_POP, 0, 0, 0, 0, NULL, e);
        bc_expr(c, e->v.Coalesce.right);
        patch(c, jend, 0, c->p->ncode);
        return;
    }
    case List_kind:
        if (has_spread(e->v.List.elements)) {
            delegate_expr(c, e);
            return;
        }
        compile_seq_items(c, e->v.List.elements);
        emit_e(c, BC_LIST, (int)asdl_seq_LEN(e->v.List.elements), 0, 0, 0, NULL, e);
        return;
    case Tuple_kind:
        if (has_spread(e->v.Tuple.elements)) {
            delegate_expr(c, e);
            return;
        }
        compile_seq_items(c, e->v.Tuple.elements);
        emit_e(c, BC_TUPLE, (int)asdl_seq_LEN(e->v.Tuple.elements), 0, 0, 0, NULL, e);
        return;
    case Set_kind:
        if (has_spread(e->v.Set.elements)) {
            delegate_expr(c, e);
            return;
        }
        compile_seq_items(c, e->v.Set.elements);
        emit_e(c, BC_SET, (int)asdl_seq_LEN(e->v.Set.elements), 0, 0, 0, NULL, e);
        return;
    case Dict_kind: {
        size_t n = asdl_seq_LEN(e->v.Dict.keys);
        size_t i;
        if (has_spread(e->v.Dict.keys)) {
            delegate_expr(c, e);
            return;
        }
        for (i = 0; i < n; i++) {
            bc_expr(c, asdl_seq_GET(e->v.Dict.keys, i));
            bc_expr(c, asdl_seq_GET(e->v.Dict.values, i));
        }
        emit_e(c, BC_DICT, (int)n, 0, 0, 0, NULL, e);
        return;
    }
    default:
        delegate_expr(c, e);
        return;
    }
}

static void bc_block(BcCtx *c, asdl_seq *body) {
    size_t i;
    for (i = 0; i < asdl_seq_LEN(body); i++) {
        if (c->failed)
            return;
        bc_stmt(c, asdl_seq_GET(body, i));
    }
}

static void bc_stmt(BcCtx *c, stmt_ty s) {
    if (c->failed)
        return;
    if (!s) {
        c->failed = 1;
        return;
    }
    emit_s(c, BC_LINE, 0, 0, 0, 0, NULL, s);
    switch (s->kind) {
    case ExprStmt_kind:
        bc_expr(c, s->v.ExprStmt.value);
        emit_s(c, BC_EXPR_POP, 0, 0, 0, 0, NULL, s);
        return;
    case VarDecl_kind:
        if (s->v.VarDecl.init && s->v.VarDecl.init->kind == ExprInit_kind) {
            bc_expr(c, s->v.VarDecl.init->v.ExprInit.value);
            emit_s(c, BC_DEFINE, 0, 0, 0, 0, s, s);
        }
        else {
            emit_s(c, BC_DELEGATE_STMT, 0, 0, 0, 0, s, s);
        }
        return;
    case Assign_kind:
        bc_expr(c, s->v.Assign.value);
        emit_s(c, BC_ASSIGN, (int)s->v.Assign.op, 0, 0, 0, s->v.Assign.target, s);
        return;
    case If_kind: {
        size_t jf, jend;
        bc_expr(c, s->v.If.test);
        jf = emit_s(c, BC_JUMP_IF_FALSE, 0, 0, 0, 0, NULL, s);
        enter_block(c);
        emit_s(c, BC_ENTER_BLOCK, 0, 0, 0, 0, NULL, s);
        bc_block(c, s->v.If.body);
        leave_block(c);
        emit_s(c, BC_LEAVE_BLOCK, 0, 0, 0, 0, NULL, s);
        jend = emit_s(c, BC_JUMP, 0, 0, 0, 0, NULL, s);
        patch(c, jf, 0, c->p->ncode);
        enter_block(c);
        emit_s(c, BC_ENTER_BLOCK, 0, 0, 0, 0, NULL, s);
        bc_block(c, s->v.If.orelse);
        leave_block(c);
        emit_s(c, BC_LEAVE_BLOCK, 0, 0, 0, 0, NULL, s);
        patch(c, jend, 0, c->p->ncode);
        return;
    }
    case While_kind: {
        BcLoopCtx loop;
        size_t push_pc, cont, test_jf, end;

        if (c->loop_depth >= BC_MAX_LOOPS) {
            c->failed = 1;
            return;
        }
        loop.prev = c->loop;
        push_pc = emit_s(c, BC_LOOP_PUSH, 0, 0, 0, 0, NULL, s);
        loop.push_pc = push_pc;
        c->loop = &loop;
        c->loop_depth++;
        if (c->loop_depth > c->max_loop_depth)
            c->max_loop_depth = c->loop_depth;
        cont = c->p->ncode;
        bc_expr(c, s->v.While.test);
        test_jf = emit_s(c, BC_JUMP_IF_FALSE, 0, 0, 0, 0, NULL, s);
        enter_block(c);
        emit_s(c, BC_ENTER_BLOCK, 0, 0, 0, 0, NULL, s);
        bc_block(c, s->v.While.body);
        leave_block(c);
        emit_s(c, BC_LEAVE_BLOCK, 0, 0, 0, 0, NULL, s);
        emit_s(c, BC_JUMP, (int)cont, 0, 0, 0, NULL, s);
        end = c->p->ncode;
        patch(c, test_jf, 0, end);
        emit_s(c, BC_LOOP_POP, 0, 0, 0, 0, NULL, s);
        patch(c, push_pc, 2, cont);
        patch(c, push_pc, 3, end);
        c->loop = loop.prev;
        c->loop_depth--;
        return;
    }
    case ForRange_kind: {
        BcLoopCtx loop;
        size_t push_pc, cont, test_pc, end;
        int has_step = s->v.ForRange.iter->step != NULL;

        if (c->loop_depth >= BC_MAX_LOOPS) {
            c->failed = 1;
            return;
        }
        bc_expr(c, s->v.ForRange.iter->start);
        bc_expr(c, s->v.ForRange.iter->stop);
        if (has_step)
            bc_expr(c, s->v.ForRange.iter->step);
        enter_block(c);
        push_pc = emit_s(c, BC_RANGE_BEGIN, bc_name(c, s->v.ForRange.name), has_step,
                         0, 0, NULL, s);
        loop.prev = c->loop;
        loop.push_pc = push_pc;
        c->loop = &loop;
        c->loop_depth++;
        if (c->loop_depth > c->max_loop_depth)
            c->max_loop_depth = c->loop_depth;
        cont = c->p->ncode;
        test_pc = emit_s(c, BC_RANGE_TEST, 0, 0, 0, 0, NULL, s);
        enter_block(c);
        emit_s(c, BC_ENTER_BLOCK, 0, 0, 0, 0, NULL, s);
        bc_block(c, s->v.ForRange.body);
        leave_block(c);
        emit_s(c, BC_LEAVE_BLOCK, 0, 0, 0, 0, NULL, s);
        emit_s(c, BC_RANGE_STEP, 0, 0, 0, 0, NULL, s);
        emit_s(c, BC_JUMP, (int)cont, 0, 0, 0, NULL, s);
        end = c->p->ncode;
        patch(c, test_pc, 0, end);
        leave_block(c);
        emit_s(c, BC_LEAVE_BLOCK, 0, 0, 0, 0, NULL, s);
        emit_s(c, BC_LOOP_POP, 0, 0, 0, 0, NULL, s);
        patch(c, push_pc, 2, cont);
        patch(c, push_pc, 3, end);
        c->loop = loop.prev;
        c->loop_depth--;
        return;
    }
    case ForIn_kind: {
        BcLoopCtx loop;
        size_t push_pc, cont, next_pc, end;

        if (c->loop_depth >= BC_MAX_LOOPS) {
            c->failed = 1;
            return;
        }
        bc_expr(c, s->v.ForIn.iter);
        enter_block(c);
        push_pc = emit_s(c, BC_FORIN_BEGIN, bc_name(c, s->v.ForIn.name), 0, 0, 0,
                         s->v.ForIn.ty, s);
        loop.prev = c->loop;
        loop.push_pc = push_pc;
        c->loop = &loop;
        c->loop_depth++;
        if (c->loop_depth > c->max_loop_depth)
            c->max_loop_depth = c->loop_depth;
        cont = c->p->ncode;
        next_pc = emit_s(c, BC_FORIN_NEXT, 0, 0, 0, 0, s->v.ForIn.ty, s);
        enter_block(c);
        emit_s(c, BC_ENTER_BLOCK, 0, 0, 0, 0, NULL, s);
        bc_block(c, s->v.ForIn.body);
        leave_block(c);
        emit_s(c, BC_LEAVE_BLOCK, 0, 0, 0, 0, NULL, s);
        emit_s(c, BC_JUMP, (int)cont, 0, 0, 0, NULL, s);
        end = c->p->ncode;
        patch(c, next_pc, 0, end);
        leave_block(c);
        emit_s(c, BC_LEAVE_BLOCK, 0, 0, 0, 0, NULL, s);
        emit_s(c, BC_LOOP_POP, 0, 0, 0, 0, NULL, s);
        patch(c, push_pc, 2, cont);
        patch(c, push_pc, 3, end);
        c->loop = loop.prev;
        c->loop_depth--;
        return;
    }
    case For_kind: {
        BcLoopCtx loop;
        size_t push_pc, test_pc, test_jf, step_pc, end;

        if (c->loop_depth >= BC_MAX_LOOPS) {
            c->failed = 1;
            return;
        }
        enter_block(c);
        emit_s(c, BC_ENTER_BLOCK, 0, 0, 0, 0, NULL, s);
        push_pc = emit_s(c, BC_LOOP_PUSH, 0, 0, 0, 0, NULL, s);
        loop.prev = c->loop;
        loop.push_pc = push_pc;
        c->loop = &loop;
        c->loop_depth++;
        if (c->loop_depth > c->max_loop_depth)
            c->max_loop_depth = c->loop_depth;
        bc_stmt(c, s->v.For.init);
        test_pc = c->p->ncode;
        bc_expr(c, s->v.For.test);
        test_jf = emit_s(c, BC_JUMP_IF_FALSE, 0, 0, 0, 0, NULL, s);
        enter_block(c);
        emit_s(c, BC_ENTER_BLOCK, 0, 0, 0, 0, NULL, s);
        bc_block(c, s->v.For.body);
        leave_block(c);
        emit_s(c, BC_LEAVE_BLOCK, 0, 0, 0, 0, NULL, s);
        step_pc = c->p->ncode;
        bc_stmt(c, s->v.For.step);
        emit_s(c, BC_JUMP, (int)test_pc, 0, 0, 0, NULL, s);
        end = c->p->ncode;
        patch(c, test_jf, 0, end);
        patch(c, push_pc, 2, step_pc);
        patch(c, push_pc, 3, end);
        leave_block(c);
        emit_s(c, BC_LEAVE_BLOCK, 0, 0, 0, 0, NULL, s);
        emit_s(c, BC_LOOP_POP, 0, 0, 0, 0, NULL, s);
        c->loop = loop.prev;
        c->loop_depth--;
        return;
    }
    case Return_kind:
        if (s->v.Return.value) {
            bc_expr(c, s->v.Return.value);
            emit_s(c, BC_RETURN_VALUE, 0, 0, 0, 0, NULL, s);
        }
        else {
            emit_s(c, BC_RETURN, 0, 0, 0, 0, NULL, s);
        }
        return;
    case Break_kind:
        if (!c->loop) {
            c->failed = 1;
            return;
        }
        emit_s(c, BC_BREAK, 0, 0, 0, 0, NULL, s);
        return;
    case Continue_kind:
        if (!c->loop) {
            c->failed = 1;
            return;
        }
        emit_s(c, BC_CONTINUE, 0, 0, 0, 0, NULL, s);
        return;
    default:
        emit_s(c, BC_DELEGATE_STMT, 0, 0, 0, 0, s, s);
        return;
    }
}

static BcProto *bc_proto_new(Interp *in) {
    BcProto *p = Arena_Alloc(in->arena, sizeof(BcProto));
    p->arena = in->arena;
    return p;
}

BcProto *VM_CompileBody(Interp *in, asdl_seq *body) {
    BcProto *p = bc_proto_new(in);
    BcCtx c;

    memset(&c, 0, sizeof c);
    c.in = in;
    c.p = p;
    bc_block(&c, body);
    bc_emit(&c, BC_END, 0, 0, 0, 0, NULL, 0, 0, 1);
    if (c.failed)
        return NULL;
    p->max_blocks = c.max_depth;
    p->max_loops = c.max_loop_depth;
    return p;
}

BcProto *VM_CompileExpr(Interp *in, expr_ty expr) {
    BcProto *p = bc_proto_new(in);
    BcCtx c;

    memset(&c, 0, sizeof c);
    c.in = in;
    c.p = p;
    bc_expr(&c, expr);
    bc_emit(&c, BC_END, 0, 0, 0, 0, NULL, 0, 0, 1);
    if (c.failed)
        return NULL;
    p->max_blocks = c.max_depth;
    p->max_loops = c.max_loop_depth;
    return p;
}

/* ---- 실행 ---- */

#define VM_MAX_BLOCKS BC_MAX_BLOCKS
#define VM_MAX_LOOPS BC_MAX_LOOPS

typedef struct {
    int kind;             /* 0=일반, 1=range, 2=for-in */
    int scope_base;
    size_t cont_pc;
    size_t end_pc;
    int64_t i, stop, step;
    RValue **cell;
    typeref_ty iter_ty;
    IterCursor cur;
} VmLoop;

typedef struct {
    Interp *in;
    BcProto *proto;
    RValue **stack;
    size_t sp, cap;
    Env *env;
    Env **blocks;
    int nblocks;
    VmLoop *loops;
    int nloops;
} VmState;

static void vm_push(VmState *st, RValue *v) {
    if (st->sp == st->cap) {
        size_t ncap = st->cap ? st->cap * 2 : 64;
        st->stack = Aster_GC_Grow(st->stack, st->cap * sizeof(RValue *),
                                  ncap * sizeof(RValue *), GK_PTR_ARRAY);
        st->cap = ncap;
    }
    st->stack[st->sp++] = v;
}

static void vm_unwind(VmState *st, int scope_base) {
    while (st->nblocks > scope_base)
        st->env = st->blocks[--st->nblocks];
}

/* 트리 워커는 식을 평가할 때마다 위치를 갱신하므로, 마지막으로 평가한
   피연산자의 위치가 오류 지점이 된다. 연산 명령은 위치를 덮어쓰지 않는다. */
static int vm_sets_pos(int op) {
    switch (op) {
    case BC_CONST:
    case BC_NULL:
    case BC_NAME:
    case BC_SELF:
    case BC_SIZEOF:
    case BC_DELEGATE_EXPR:
    case BC_DELEGATE_STMT:
        return 1;
    default:
        return 0;
    }
}

static Signal vm_exec(Interp *in, BcProto *proto, Env *env, int expr_mode,
                      RValue **result) {
    VmState st;
    size_t pc = 0;
    Signal sig = SIG_OK;
    memset(&st, 0, sizeof st);
    st.in = in;
    st.proto = proto;
    st.env = env;
    if (proto->max_blocks > 0)
        st.blocks = Aster_GC_Alloc((size_t)proto->max_blocks * sizeof(Env *),
                                   GK_PTR_ARRAY);
    if (proto->max_loops > 0)
        st.loops = Aster_GC_Alloc((size_t)proto->max_loops * sizeof(VmLoop),
                                  GK_PTR_ARRAY);

    while (pc < proto->ncode) {
        BcInsn *ins = &proto->code[pc++];

        if (ins->op == BC_LINE) {
            in->line = ins->line;
            in->col = ins->col;
            in->span = ins->span;
            if (in->nframes > 0)
                in->frames[in->nframes - 1].line = ins->line;
            continue;
        }
        if (ins->line > 0 && vm_sets_pos(ins->op)) {
            in->line = ins->line;
            in->col = ins->col;
            in->span = ins->span;
        }

        switch (ins->op) {
        case BC_NOP:
            break;
        case BC_NULL:
            vm_push(&st, RValue_Null(in));
            break;
        case BC_CONST: {
            const BcConst *k = &proto->consts[ins->a];
            switch (k->tag) {
            case 1: vm_push(&st, RValue_Bool(in, k->i != 0)); break;
            case 2: vm_push(&st, RValue_Int(in, k->i)); break;
            case 3: vm_push(&st, RValue_Float(in, k->f)); break;
            case 4: vm_push(&st, RValue_Str(in, (AsterString *)k->s)); break;
            default: vm_push(&st, RValue_Null(in)); break;
            }
            break;
        }
        case BC_NAME: {
            RValue **cell = Env_LookupCell(st.env, proto->names[ins->a]);
            if (!cell) {
                RT_Throw(in, "NameError", "name '%s' is not defined",
                         proto->names[ins->a]);
                sig = SIG_THROW;
                goto done;
            }
            vm_push(&st, *cell);
            break;
        }
        case BC_SELF: {
            RValue **cell = Env_LookupCell(st.env, "self");
            if (!cell) {
                RT_Throw(in, "RuntimeError", "'self' is only allowed inside methods");
                sig = SIG_THROW;
                goto done;
            }
            vm_push(&st, *cell);
            break;
        }
        case BC_POP:
        case BC_EXPR_POP: {
            RValue *v = st.sp ? st.stack[--st.sp] : NULL;
            if (ins->op == BC_EXPR_POP && in->repl && in->sig == SIG_OK && v &&
                v->kind != RV_NULL) {
                RValue_Print(stdout, v, 1);
                fputc('\n', stdout);
                fflush(stdout);
            }
            break;
        }
        case BC_LIST:
        case BC_TUPLE:
        case BC_SET: {
            int n = ins->a;
            size_t base = st.sp - (size_t)n;
            int i;
            if (ins->op == BC_LIST) {
                AsterList *list = List_New(in);
                RValue *rv;
                for (i = 0; i < n; i++)
                    List_Append(in, list, RValue_Copy(in, st.stack[base + (size_t)i]));
                st.sp = base;
                rv = RValue_List(in, list);
                vm_push(&st, rv);
            }
            else if (ins->op == BC_TUPLE) {
                AsterTuple *tuple = Tuple_New(in, (size_t)n);
                RValue *rv;
                for (i = 0; i < n; i++)
                    tuple->items[i] = RValue_Copy(in, st.stack[base + (size_t)i]);
                st.sp = base;
                rv = RValue_Tuple(in, tuple);
                vm_push(&st, rv);
            }
            else {
                AsterSet *set = Set_New(in);
                RValue *rv;
                int ok = 1;
                for (i = 0; i < n; i++) {
                    if (!Set_Add(in, set, RValue_Copy(in, st.stack[base + (size_t)i]),
                                 &ok)) {
                        sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                        goto done;
                    }
                }
                st.sp = base;
                rv = RValue_Set(in, set);
                vm_push(&st, rv);
            }
            break;
        }
        case BC_DICT: {
            int n = ins->a;
            size_t base = st.sp - (size_t)(2 * n);
            AsterDict *dict = Dict_New(in);
            RValue *rv;
            int i, ok = 1;
            for (i = 0; i < n; i++) {
                if (!Dict_Set(in, dict, st.stack[base + (size_t)(2 * i)],
                              RValue_Copy(in, st.stack[base + (size_t)(2 * i + 1)]),
                              &ok)) {
                    sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                    goto done;
                }
            }
            st.sp = base;
            rv = RValue_Dict(in, dict);
            vm_push(&st, rv);
            break;
        }
        case BC_ENTER_BLOCK:
            st.blocks[st.nblocks++] = st.env;
            st.env = Env_New(in, st.env);
            break;
        case BC_LEAVE_BLOCK:
            if (st.nblocks > 0)
                st.env = st.blocks[--st.nblocks];
            break;
        case BC_UNARY: {
            RValue *v = st.stack[--st.sp];
            RValue *r = Interp_ApplyUnary(in, (unaryop_ty)ins->a, v);
            if (in->sig != SIG_OK) {
                sig = in->sig;
                goto done;
            }
            vm_push(&st, r);
            break;
        }
        case BC_BINOP: {
            RValue *r = st.stack[--st.sp];
            RValue *l = st.stack[--st.sp];
            RValue *v = Interp_ApplyBinOp(in, (operator_ty)ins->a, l, r);
            if (in->sig != SIG_OK) {
                sig = in->sig;
                goto done;
            }
            vm_push(&st, v);
            break;
        }
        case BC_COMPARE: {
            RValue *r = st.stack[--st.sp];
            RValue *l = st.stack[--st.sp];
            RValue *v = Interp_ApplyCompare(in, (cmpop_ty)ins->a, l, r);
            if (in->sig != SIG_OK) {
                sig = in->sig;
                goto done;
            }
            vm_push(&st, v);
            break;
        }
        case BC_IS: {
            RValue *v = st.stack[--st.sp];
            vm_push(&st, Interp_IsType(in, v, (typeref_ty)ins->p));
            break;
        }
        case BC_IN: {
            RValue *container = st.stack[--st.sp];
            RValue *item = st.stack[--st.sp];
            int ok = 1;
            int found = RValue_Contains(in, container, item, &ok);
            if (!ok) {
                sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                goto done;
            }
            vm_push(&st, RValue_Bool(in, found));
            break;
        }
        case BC_CAST: {
            RValue *v = st.stack[--st.sp];
            RValue *r = Interp_Cast(in, (typeref_ty)ins->p, v);
            if (in->sig != SIG_OK) {
                sig = in->sig;
                goto done;
            }
            vm_push(&st, r);
            break;
        }
        case BC_SIZEOF: {
            RValue *v = RValue_Int(in, (int64_t)Type_SizeOf(in, st.env,
                                                           (typeref_ty)ins->p));
            vm_push(&st, v);
            break;
        }
        case BC_DEREF: {
            RValue *operand = st.stack[--st.sp];
            RValue *v;
            if (!operand || operand->kind == RV_NULL) {
                RT_Throw(in, "NullPointerError", "dereferenced a null pointer");
                sig = SIG_THROW;
                goto done;
            }
            if (operand->kind == RV_OBJECT) {
                v = operand;
            }
            else if (operand->kind != RV_PTR) {
                RT_Throw(in, "TypeError", "value cannot be dereferenced");
                sig = SIG_THROW;
                goto done;
            }
            else if (!operand->u.ptr.slot || !*operand->u.ptr.slot) {
                RT_Throw(in, "NullPointerError", "dereferenced a null pointer");
                sig = SIG_THROW;
                goto done;
            }
            else {
                v = RValue_Copy(in, *operand->u.ptr.slot);
            }
            vm_push(&st, v);
            break;
        }
        case BC_ATTR:
        case BC_PTR_ATTR: {
            RValue *base = st.stack[--st.sp];
            RValue *v = Interp_GetAttr(in, base, proto->names[ins->a]);
            if (in->sig != SIG_OK) {
                sig = in->sig;
                goto done;
            }
            vm_push(&st, v);
            break;
        }
        case BC_OPT_ATTR: {
            RValue *base = st.stack[--st.sp];
            if (!base || base->kind == RV_NULL ||
                (base->kind == RV_PTR &&
                 (!base->u.ptr.slot || !*base->u.ptr.slot ||
                  (*base->u.ptr.slot)->kind == RV_NULL))) {
                vm_push(&st, RValue_Null(in));
                break;
            }
            {
                RValue *v = Interp_GetAttr(in, base, proto->names[ins->a]);
                if (in->sig != SIG_OK) {
                    sig = in->sig;
                    goto done;
                }
                vm_push(&st, v);
            }
            break;
        }
        case BC_GETITEM: {
            RValue *index = st.stack[--st.sp];
            RValue *container = st.stack[--st.sp];
            int ok = 1;
            RValue *v = RValue_GetItem(in, container, index, &ok);
            if (!ok) {
                sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                goto done;
            }
            vm_push(&st, v);
            break;
        }
        case BC_CALL: {
            int nargs = ins->a;
            size_t base = st.sp - (size_t)nargs;
            RValue *callee = st.stack[base - 1];
            RValue *v = Interp_CallValueEx(in, callee, &st.stack[base],
                                           (size_t)nargs, NULL, NULL, 0);
            if (in->sig != SIG_OK) {
                sig = in->sig;
                goto done;
            }
            st.sp = base - 1;
            vm_push(&st, v);
            break;
        }
        case BC_CALL_KW: {
            BcCallDesc *desc = &proto->calls[ins->a];
            size_t nargs = (size_t)desc->nargs;
            size_t nkw = (size_t)desc->nkw;
            size_t base = st.sp - nargs - nkw;
            RValue *callee = st.stack[base - 1];
            RValue *v = Interp_CallValueEx(in, callee, &st.stack[base], nargs,
                                           desc->kwn, &st.stack[base + nargs], nkw);
            if (in->sig != SIG_OK) {
                sig = in->sig;
                goto done;
            }
            st.sp = base - 1;
            vm_push(&st, v);
            break;
        }
        case BC_DELEGATE_EXPR: {
            RValue *v = Interp_EvalExprNode(in, (expr_ty)ins->p, st.env);
            if (in->sig != SIG_OK) {
                sig = in->sig;
                goto done;
            }
            vm_push(&st, v ? v : RValue_Null(in));
            break;
        }
        case BC_DELEGATE_STMT: {
            Signal s2 = Interp_ExecStmtNode(in, (stmt_ty)ins->p, st.env);
            if (s2 == SIG_BREAK || s2 == SIG_CONTINUE) {
                VmLoop *l = st.nloops ? &st.loops[st.nloops - 1] : NULL;
                if (!l) {
                    sig = s2;
                    goto done;
                }
                vm_unwind(&st, l->scope_base);
                pc = s2 == SIG_BREAK ? l->end_pc : l->cont_pc;
                in->sig = SIG_OK;
            }
            else if (s2 != SIG_OK) {
                sig = s2;
                goto done;
            }
            break;
        }
        case BC_DEFINE: {
            RValue *v = st.stack[--st.sp];
            sig = Interp_DefineVarValue(in, st.env, (stmt_ty)ins->p, v);
            if (sig != SIG_OK)
                goto done;
            break;
        }
        case BC_ASSIGN: {
            RValue *v = st.stack[--st.sp];
            sig = Interp_AssignTarget(in, (expr_ty)ins->p, (assign_op_ty)ins->a, v,
                                      st.env);
            if (sig != SIG_OK)
                goto done;
            break;
        }
        case BC_RETURN:
            in->ret = RValue_Null(in);
            in->sig = SIG_RETURN;
            sig = SIG_RETURN;
            goto done;
        case BC_RETURN_VALUE:
            in->ret = st.stack[--st.sp];
            in->sig = SIG_RETURN;
            sig = SIG_RETURN;
            goto done;
        case BC_JUMP:
            pc = (size_t)ins->a;
            break;
        case BC_JUMP_IF_FALSE: {
            RValue *v = st.stack[--st.sp];
            if (!RValue_Truthy(v))
                pc = (size_t)ins->a;
            break;
        }
        case BC_JUMP_IF_FALSE_KEEP:
            if (!RValue_Truthy(st.stack[st.sp - 1]))
                pc = (size_t)ins->a;
            break;
        case BC_JUMP_IF_TRUE_KEEP:
            if (RValue_Truthy(st.stack[st.sp - 1]))
                pc = (size_t)ins->a;
            break;
        case BC_JUMP_IF_NULL_KEEP: {
            RValue *v = st.stack[st.sp - 1];
            if (!v || v->kind == RV_NULL)
                pc = (size_t)ins->a;
            break;
        }
        case BC_LOOP_PUSH: {
            VmLoop *l = &st.loops[st.nloops++];
            l->kind = 0;
            l->scope_base = st.nblocks;
            l->cont_pc = (size_t)ins->c;
            l->end_pc = (size_t)ins->d;
            break;
        }
        case BC_LOOP_POP:
            if (st.nloops > 0)
                st.nloops--;
            break;
        case BC_RANGE_BEGIN: {
            int has_step = ins->b;
            int ok = 1;
            int64_t from, to, step;
            RValue *start, *stop;
            Env *loop_env;
            VmLoop *l;

            step = has_step ? RValue_AsInt(in, st.stack[--st.sp], &ok) : 1;
            if (!ok) {
                sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                goto done;
            }
            stop = st.stack[--st.sp];
            start = st.stack[--st.sp];
            to = RValue_AsInt(in, stop, &ok);
            if (!ok) {
                sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                goto done;
            }
            from = RValue_AsInt(in, start, &ok);
            if (!ok) {
                sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                goto done;
            }
            if (step == 0) {
                RT_Throw(in, "ValueError", "range for step cannot be zero");
                sig = SIG_THROW;
                goto done;
            }
            st.blocks[st.nblocks++] = st.env;
            loop_env = Env_New(in, st.env);
            st.env = loop_env;
            Env_Define(in, loop_env, proto->names[ins->a], RValue_Int(in, from));
            l = &st.loops[st.nloops++];
            l->kind = 1;
            l->scope_base = st.nblocks;
            l->cont_pc = (size_t)ins->c;
            l->end_pc = (size_t)ins->d;
            l->i = from;
            l->stop = to;
            l->step = step;
            l->cell = Env_LookupCell(loop_env, proto->names[ins->a]);
            break;
        }
        case BC_RANGE_TEST: {
            VmLoop *l = &st.loops[st.nloops - 1];
            if (l->step > 0 ? l->i < l->stop : l->i > l->stop) {
                if (l->cell)
                    *l->cell = RValue_Int(in, l->i);
            }
            else {
                pc = (size_t)ins->a;
            }
            break;
        }
        case BC_RANGE_STEP: {
            VmLoop *l = &st.loops[st.nloops - 1];
            l->i += l->step;
            break;
        }
        case BC_FORIN_BEGIN: {
            RValue *iterable = st.stack[--st.sp];
            Env *loop_env;
            VmLoop *l;

            st.blocks[st.nblocks++] = st.env;
            loop_env = Env_New(in, st.env);
            st.env = loop_env;
            l = &st.loops[st.nloops++];
            l->kind = 2;
            l->scope_base = st.nblocks;
            l->cont_pc = (size_t)ins->c;
            l->end_pc = (size_t)ins->d;
            l->iter_ty = (typeref_ty)ins->p;
            if (!Iter_Begin(in, iterable, &l->cur)) {
                sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                goto done;
            }
            Env_Define(in, loop_env, proto->names[ins->a], RValue_Null(in));
            l->cell = Env_LookupCell(loop_env, proto->names[ins->a]);
            break;
        }
        case BC_FORIN_NEXT: {
            VmLoop *l = &st.loops[st.nloops - 1];
            int ended = 0, ok = 1;
            RValue *item = Iter_Next(in, &l->cur, &ended, &ok);
            if (!ok) {
                sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                goto done;
            }
            if (ended) {
                pc = (size_t)ins->a;
                break;
            }
            if (ins->p && !Interp_CheckDeclaredValue(in, (typeref_ty)ins->p, item)) {
                sig = in->sig != SIG_OK ? in->sig : SIG_THROW;
                goto done;
            }
            if (l->cell)
                *l->cell = item;
            break;
        }
        case BC_BREAK:
        case BC_CONTINUE: {
            VmLoop *l = st.nloops ? &st.loops[st.nloops - 1] : NULL;
            if (!l) {
                RT_Throw(in, "RuntimeError",
                         "'break' and 'continue' are only allowed inside loops");
                sig = SIG_THROW;
                goto done;
            }
            vm_unwind(&st, l->scope_base);
            pc = ins->op == BC_BREAK ? l->end_pc : l->cont_pc;
            in->sig = SIG_OK;
            break;
        }
        case BC_END:
            goto done;
        default:
            RT_Throw(in, "RuntimeError", "invalid bytecode instruction");
            sig = SIG_THROW;
            goto done;
        }
    }

done:
    if (sig == SIG_OK) {
        if (expr_mode) {
            *result = st.sp ? st.stack[st.sp - 1] : NULL;
        }
        else {
            *result = NULL;
        }
    }
    else {
        *result = NULL;
    }
    return sig;
}

Signal VM_RunBody(Interp *in, BcProto *proto, Env *env) {
    RValue *result = NULL;
    return vm_exec(in, proto, env, 0, &result);
}

RValue *VM_RunExpr(Interp *in, BcProto *proto, Env *env) {
    RValue *result = NULL;
    if (vm_exec(in, proto, env, 1, &result) != SIG_OK)
        return NULL;
    return result;
}
