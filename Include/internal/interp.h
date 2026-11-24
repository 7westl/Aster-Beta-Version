#ifndef ASTER_INTERP_H
#define ASTER_INTERP_H

#include "internal/arena.h"
#include "internal/ast.h"
#include "internal/runtime.h"

/* 모듈을 실행한다. 0이면 정상 종료, 1이면 처리되지 않은 예외. */
int Interp_RunModule(Interp *in, mod_ty mod);

/* VM이 트리 워커의 의미를 그대로 재사용하기 위해 여는 진입점. */
Signal Interp_ExecStmtNode(Interp *in, stmt_ty s, Env *env);
RValue *Interp_EvalExprNode(Interp *in, expr_ty e, Env *env);
RValue *Interp_CallValueEx(Interp *in, RValue *callee, RValue **args, size_t nargs,
                           const char **kwn, RValue **kwv, size_t nkw);
RValue *Interp_ApplyBinOp(Interp *in, operator_ty op, RValue *left, RValue *right);
RValue *Interp_ApplyCompare(Interp *in, cmpop_ty op, RValue *left, RValue *right);
RValue *Interp_ApplyUnary(Interp *in, unaryop_ty op, RValue *operand);
/* obj.method(...) 호출의 빠른 경로 (묶인 메서드 값을 만들지 않는다). */
RValue *Interp_CallAttr(Interp *in, RValue *base, const char *name, RValue **args,
                        size_t nargs);
/* 메서드 호출 지점 캐시용: 단일 메서드로 확정되면 1과 함께 함수를 돌려준다. */
int Interp_ResolveMethod(AstClass *cls, const char *name, AstFn **fn);
RValue *Interp_CallMethodFast(Interp *in, AstFn *fn, RValue *self, RValue **args,
                              size_t nargs);
RValue *Interp_GetAttr(Interp *in, RValue *base, const char *name);
RValue *Interp_IsType(Interp *in, RValue *value, typeref_ty ty);
/* VM catch 진입점: 지금 풀려 나가는 예외가 ty와 맞는지 (맞으면 1). */
int Interp_ExceptionMatches(Interp *in, typeref_ty ty);
RValue *Interp_Cast(Interp *in, typeref_ty ty, RValue *value);
/* 이미 계산한 값을 변수 선언에 정의한다 (exec_var_decl의 뒷부분). */
Signal Interp_DefineVarValue(Interp *in, Env *env, stmt_ty decl, RValue *value);
/* 이미 계산한 값을 할당 대상에 쓴다 (exec_assign의 뒷부분). */
Signal Interp_AssignTarget(Interp *in, expr_ty target, assign_op_ty op,
                           RValue *value, Env *env);
/* 선언된 타입과 값이 맞는지 검사한다 (맞으면 1). */
int Interp_CheckDeclaredValue(Interp *in, typeref_ty ty, RValue *value);
/* 블록 본문이 자기 환경에 이름을 만드는지 / 그 환경이 밖으로 새는지. */
int Interp_BodyDeclaresNames(asdl_seq *body);
int Interp_BodyEscapesEnv(asdl_seq *body);

#endif
