#ifndef ASTER_GC_H
#define ASTER_GC_H

/*
 * 표시-스윕(mark-sweep) 세대 없는 추적 GC.
 *
 * 값(RValue), 문자열, 컬렉션, 환경, 인스턴스처럼 실행 중에 만들어지는 객체는
 * 이 힙에서 할당한다. 수집 시에는 C 스택·레지스터·파이버 스택을 보수적으로
 * 훑어 루트를 찾고, 값에서 AST 구조로 이어지는 참조는 타입을 알고 있으므로
 * 곧바로 따라간다. 메모리를 옮기지 않으므로 값의 주소(0x...)는 수집 후에도
 * 그대로다.
 */

#include <stddef.h>
#include <setjmp.h>

struct Interp;

typedef enum {
    GK_RAW = 0,       /* 스캔하지 않는 단일 객체 */
    GK_RVALUE,
    GK_STRING,        /* 바이트만 있으므로 스캔하지 않는다 */
    GK_LIST,
    GK_TUPLE,
    GK_DICT,
    GK_DICT_ENTRIES,  /* AsterDictEntry 배열 */
    GK_SET,
    GK_INSTANCE,      /* AstInstance (GC/아레나 모두) */
    GK_STRUCTVALUE,
    GK_EXCEPTION,
    GK_SUPER,
    GK_GENERATOR,
    GK_INTERP,
    GK_ENV,
    GK_BINDING,
    GK_BUILTIN,       /* AstBuiltin */
    GK_PTR_ARRAY,     /* GC 객체 포인터 배열. 각 워드를 표시한다 */
    GK_RAW_ARRAY,     /* 표시하지 않는 배열 (long, size_t, 바이트 등) */
    GK_AST_FN,        /* 실행 중 복사한 AstFn (self/owner/env를 따라간다) */
    GK_AST_OVERLOAD   /* 실행 중 복사한 AstOverload */
} GKind;

void Aster_GC_Init(void *stack_anchor);
void Aster_GC_Shutdown(void);
int Aster_GC_IsInitialized(void);

/* size 바이트를 0으로 채워 돌려준다. 수집은 이 호출 앞에서 일어난다. */
void *Aster_GC_Alloc(size_t size, GKind kind);
/* old를 새 블록으로 복사해 돌려준다. old는 다음 수집에서 회수된다. */
void *Aster_GC_Grow(const void *old, size_t old_size, size_t new_size, GKind kind);

void Aster_GC_Collect(void);
size_t Aster_GC_LiveBytes(void);
void Aster_GC_SetStress(int on);
int Aster_GC_Stress(void);
/* 켜져 있는 동안 만든 객체는 영원히 살아남는다 (파서 리터럴 보호용). */
void Aster_GC_SetPinning(int on);
int Aster_GC_Pinning(void);

/* 아직 GC가 따라갈 수 없는 곳(아레나 지역 변수)에만 있는 객체를 임시 루트로
   등록한다. Push/Pop은 반드시 짝을 맞춘다. */
typedef enum {
    GC_ROOT_VALUE = 0,   /* RValue * */
    GC_ROOT_FN,          /* AstFn * (실행 중 사본) */
    GC_ROOT_CLASS,       /* AstClass * */
    GC_ROOT_ENUM,        /* AstEnumType * */
    GC_ROOT_STRUCTTYPE,  /* AstStructType * */
    GC_ROOT_OVERLOAD     /* AstOverload * */
} GRootKind;

void Aster_GC_PushRoot(GRootKind kind, void **slot);
void Aster_GC_PopRoot(void);
void Aster_GC_MarkPointer(void *p);   /* 수집 중에만 유효하다 */

/* C 스택에 있지 않은 실행 상태 (전역 Interp)를 루트로 등록한다. */
void Aster_GC_AddInterp(struct Interp *in);
void Aster_GC_RemoveInterp(struct Interp *in);

/* 제너레이터 실행 컨텍스트(파이버)의 스택을 등록한다.
   high는 그 스택에서 실제로 쓰이는 가장 높은 주소다. */
void *Aster_GC_NewFiberStack(void *high);
void Aster_GC_FreeFiberStack(void *stack);
void Aster_GC_EnterFiberStack(void *stack);
void Aster_GC_EnterMainStack(void);
/* 다른 실행 컨텍스트로 넘어가기 직전에 현재 스택의 사용 범위를 기록한다. */
void Aster_GC_SuspendCurrent(void);

#endif
