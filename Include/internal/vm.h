#ifndef ASTER_VM_H
#define ASTER_VM_H

#include "internal/ast.h"
#include "internal/runtime.h"

/*
 * 바이트코드 VM.
 *
 * 본문(문장 목록)이나 식 하나를 스택 머신용 바이트코드로 컴파일한다.
 * 지원하지 않는 노드는 실행 중에 트리 워커(Interp_ExecStmtNode /
 * Interp_EvalExprNode)로 위임하므로 의미가 달라지지 않는다.
 * 모든 메모리는 인터프리터 아레나에 있어 모듈 수명과 함께 해제된다.
 */
typedef struct BcProto BcProto;

BcProto *VM_CompileBody(Interp *in, asdl_seq *body);
BcProto *VM_CompileExpr(Interp *in, expr_ty expr);

/* 본문/식을 실행한다. 상태는 트리 워커와 같이 in->sig에 남긴다. */
Signal VM_RunBody(Interp *in, BcProto *proto, Env *env);
RValue *VM_RunExpr(Interp *in, BcProto *proto, Env *env);

#endif
