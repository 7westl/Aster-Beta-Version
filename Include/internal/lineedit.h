#ifndef ASTER_LINEEDIT_H
#define ASTER_LINEEDIT_H
#include <stddef.h>

/* REPL용 터미널 줄 편집기.
   터미널이 아니면 Le_New가 NULL을 돌려주고 호출자는 fgets로 대체한다. */

typedef struct LineEditor LineEditor;

/* 완성 후보를 추가한다. 완성 콜백 안에서만 호출한다. */
void Le_AddCompletion(LineEditor *le, const char *text);

/* line[start..end)를 완성할 후보를 Le_AddCompletion으로 제공한다. */
typedef void (*LeCompleteFn)(void *ctx, LineEditor *le, const char *line,
                             size_t start, size_t end);

LineEditor *Le_New(void);
void Le_Free(LineEditor *le);

void Le_SetCompleter(LineEditor *le, LeCompleteFn fn, void *ctx);

/* 한 줄을 읽어 '\n'을 붙여 malloc한 문자열로 돌려준다. EOF면 NULL.
   편집 키: ←/→, Home/End, Ctrl-A/E/B/F, Backspace/Delete, Ctrl-K/U/W,
   Ctrl-P/N(히스토리), Tab(완성), Ctrl-C(줄 취소), Ctrl-D(빈 줄이면 EOF). */
char *Le_ReadLine(LineEditor *le, const char *prompt);

void Le_HistoryAdd(LineEditor *le, const char *line);
int Le_HistoryLoad(LineEditor *le, const char *path);
int Le_HistorySave(LineEditor *le, const char *path);

#endif
