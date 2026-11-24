#ifndef ASTER_FORMATTER_H
#define ASTER_FORMATTER_H
#include <stdio.h>
#include "internal/ast.h"

/* 모듈을 표준 형식(4칸 들여쓰기)으로 출력한다.
   source를 주면 주석을 문장 경계에 맞춰 보존하고, 없으면 AST만 출력한다.
   빈 줄은 보존하지 않는다. */
void Aster_FormatModule(FILE *out, mod_ty mod, const char *source);

#endif
