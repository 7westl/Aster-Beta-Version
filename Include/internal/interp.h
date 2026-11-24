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
RValue *Interp_GetAttr(Interp *in, RValue *base, const char *name);
RValue *Interp_IsType(Interp *in, RValue *value, typeref_ty ty);
RValue *Interp_Cast(Interp *in, typeref_ty ty, RValue *value);
/* 이미 계산한 값을 변수 선언에 정의한다 (exec_var_decl의 뒷부분). */
Signal Interp_DefineVarValue(Interp *in, Env *env, stmt_ty decl, RValue *value);
/* 이미 계산한 값을 할당 대상에 쓴다 (exec_assign의 뒷부분). */
Signal Interp_AssignTarget(Interp *in, expr_ty target, assign_op_ty op,
                           RValue *value, Env *env);
/* 선언된 타입과 값이 맞는지 검사한다 (맞으면 1). */
int Interp_CheckDeclaredValue(Interp *in, typeref_ty ty, RValue *value);

#endif
