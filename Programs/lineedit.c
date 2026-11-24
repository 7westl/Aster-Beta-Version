#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
/* tcgetattr/ioctl 선언을 위해 시스템 헤더보다 먼저 정의한다.
   macOS는 기능 매크로를 지정하지 않으면 전체 POSIX 선언을 노출하므로 건드리지 않는다. */
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal/lineedit.h"

#if defined(_WIN32)
/* windows.h는 내부 헤더의 VT_* 열거자와 충돌하므로 필요한 API만 직접 선언한다. */
typedef void *AsterHandle;
__declspec(dllimport) AsterHandle __stdcall GetStdHandle(unsigned long n);
__declspec(dllimport) int __stdcall GetConsoleMode(AsterHandle h, unsigned long *mode);
__declspec(dllimport) int __stdcall SetConsoleMode(AsterHandle h, unsigned long mode);
__declspec(dllimport) unsigned int __stdcall GetConsoleCP(void);
__declspec(dllimport) int __stdcall SetConsoleCP(unsigned int cp);
__declspec(dllimport) unsigned int __stdcall GetConsoleOutputCP(void);
__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int cp);
__declspec(dllimport) int __stdcall GetConsoleScreenBufferInfo(AsterHandle h, void *info);
__declspec(dllimport) int __stdcall ReadFile(AsterHandle h, void *buf, unsigned long want,
                                             unsigned long *got, void *ov);
typedef int (__stdcall *AsterCtrlHandler)(unsigned long);
__declspec(dllimport) int __stdcall SetConsoleCtrlHandler(AsterCtrlHandler handler, int add);

#define ASTER_STD_INPUT_HANDLE ((unsigned long)-10)
#define ASTER_STD_OUTPUT_HANDLE ((unsigned long)-11)
#define ASTER_ENABLE_PROCESSED_INPUT 0x0001
#define ASTER_ENABLE_LINE_INPUT 0x0002
#define ASTER_ENABLE_ECHO_INPUT 0x0004
#define ASTER_ENABLE_WINDOW_INPUT 0x0008
#define ASTER_ENABLE_MOUSE_INPUT 0x0010
#define ASTER_ENABLE_INSERT_MODE 0x0020
#define ASTER_ENABLE_QUICK_EDIT 0x0040
#define ASTER_ENABLE_VT_PROCESSING 0x0004
#define ASTER_ENABLE_VT_INPUT 0x0200

typedef struct {
    short x, y;
} AsterCoord;
typedef struct {
    short left, top, right, bottom;
} AsterSmallRect;
typedef struct {
    AsterCoord size;
    AsterCoord cursor;
    unsigned short attributes;
    AsterSmallRect window;
    AsterCoord max_size;
} AsterScreenBufferInfo;
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#define LE_MAX_LINE 4094
#define LE_HISTORY_MAX 1000
#define LE_DEFAULT_WIDTH 80

struct LineEditor {
    char *buf;
    size_t len, cap, cursor;

    const char *prompt;
    int width;
    int prev_row;             /* 마지막 렌더에서 커서가 있던 줄 (0 = 첫 줄) */

    char **history;
    size_t nhistory, hist_cap;
    size_t hist_index;

    char **cands;
    size_t ncands, cand_cap;

    LeCompleteFn complete;
    void *complete_ctx;
    int skip_lf;

#if defined(_WIN32)
    unsigned long saved_in_mode;
    unsigned long saved_out_mode;
    unsigned int saved_cp;
    unsigned int saved_out_cp;
    int have_out_mode;
#else
    struct termios saved_tio;
#endif
};

/* ------------------------------------------------------------- UTF-8 도우미 */

static size_t cp_next(const char *s, size_t len, size_t i) {
    if (i >= len)
        return len;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80)
        i++;
    return i;
}

static size_t cp_prev(const char *s, size_t i) {
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        i--;
    return i;
}

static long cp_decode(const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    if (n == 0)
        return 0;
    if (n >= 4 && u[0] >= 0xF0)
        return ((long)(u[0] & 0x07) << 18) | ((long)(u[1] & 0x3F) << 12) |
               ((long)(u[2] & 0x3F) << 6) | (long)(u[3] & 0x3F);
    if (n >= 3 && u[0] >= 0xE0)
        return ((long)(u[0] & 0x0F) << 12) | ((long)(u[1] & 0x3F) << 6) |
               (long)(u[2] & 0x3F);
    if (n >= 2 && u[0] >= 0xC0)
        return ((long)(u[0] & 0x1F) << 6) | (long)(u[1] & 0x3F);
    return u[0];
}

/* 터미널 칸 수. 결합 문자/제로 폭은 0, 동아시아 넓은 문자는 2로 본다. */
static int cp_cols(const char *s, size_t n) {
    long c = cp_decode(s, n);
    if (c == 0)
        return 0;
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
        (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
        (c >= 0xFE00 && c <= 0xFE0F) || c == 0x200B || c == 0x200C ||
        c == 0x200D || c == 0xFEFF)
        return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

static size_t display_cols(const char *s, size_t n) {
    size_t i = 0, cols = 0;
    while (i < n) {
        size_t j = cp_next(s, n, i);
        cols += (size_t)cp_cols(s + i, j - i);
        i = j;
    }
    return cols;
}

/* i 앞의 바이트가 온전한 UTF-8 코드포인트를 끝내는지 확인한다.
   멀티바이트 입력을 한 바이트씩 받을 때 중간 상태를 그리지 않기 위한 것. */
static int cp_complete_before(const char *s, size_t i) {
    size_t start;
    unsigned char lead;
    int need;

    if (i == 0)
        return 1;
    start = i - 1;
    while (start > 0 && ((unsigned char)s[start] & 0xC0) == 0x80)
        start--;
    lead = (unsigned char)s[start];
    if (lead < 0xC0)
        return 1;
    if (lead >= 0xF0)
        need = 4;
    else if (lead >= 0xE0)
        need = 3;
    else
        need = 2;
    return i - start >= (size_t)need;
}

/* ------------------------------------------------------------- 출력 도우미 */

static void le_put(LineEditor *le, const char *s, size_t n) {
    (void)le;
    fwrite(s, 1, n, stdout);
}

static void le_beep(LineEditor *le) {
    le_put(le, "\a", 1);
    fflush(stdout);
}

static int le_os_width(void) {
    int width = 0;
#if defined(_WIN32)
    AsterScreenBufferInfo info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(ASTER_STD_OUTPUT_HANDLE), &info))
        width = info.window.right - info.window.left + 1;
#elif defined(TIOCGWINSZ)
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0)
        width = ws.ws_col;
#else
    const char *cols = getenv("COLUMNS");
    if (cols)
        width = atoi(cols);
#endif
    return width;
}

static int le_terminal_width(LineEditor *le) {
    int width = le_os_width();
    if (width < 2)
        width = le->width > 1 ? le->width : LE_DEFAULT_WIDTH;
    return width;
}

/* 프롬프트+버퍼를 다시 그리고 커서를 제자리로 옮긴다.
   이전 렌더가 차지한 줄 수를 추적해 위로 되돌아간 뒤 지운다. */
static void le_render(LineEditor *le) {
    int width = le_terminal_width(le);
    size_t plen = display_cols(le->prompt, strlen(le->prompt));
    size_t total = plen + display_cols(le->buf, le->len);
    size_t cur_total = plen + display_cols(le->buf, le->cursor);
    int end_row = total ? (int)((total - 1) / (size_t)width) : 0;
    int cur_row = cur_total ? (int)((cur_total - 1) / (size_t)width) : 0;
    size_t cur_col = cur_total - (size_t)cur_row * (size_t)width;

    if (le->prev_row > 0)
        printf("\x1b[%dA", le->prev_row);
    fputc('\r', stdout);
    fputs(le->prompt, stdout);
    fwrite(le->buf, 1, le->len, stdout);
    fputs("\x1b[J", stdout);

    if (cur_row < end_row)
        printf("\x1b[%dA", end_row - cur_row);
    else if (cur_row > end_row)
        printf("\x1b[%dB", cur_row - end_row);
    fputc('\r', stdout);
    if (cur_col)
        printf("\x1b[%zuC", cur_col);

    le->prev_row = cur_row;
    fflush(stdout);
}

/* --------------------------------------------------------------- 편집 동작 */

static void le_set_buffer(LineEditor *le, const char *text) {
    size_t n = strlen(text);
    if (n > LE_MAX_LINE)
        n = LE_MAX_LINE;
    memcpy(le->buf, text, n);
    le->buf[n] = 0;
    le->len = n;
    le->cursor = n;
}

static void le_insert(LineEditor *le, const char *text, size_t n) {
    if (le->len + n > LE_MAX_LINE) {
        le_beep(le);
        return;
    }
    memmove(le->buf + le->cursor + n, le->buf + le->cursor,
            le->len - le->cursor + 1);
    memcpy(le->buf + le->cursor, text, n);
    le->len += n;
    le->cursor += n;
}

static void le_replace_span(LineEditor *le, size_t start, size_t end,
                            const char *text, size_t n) {
    if (le->len - (end - start) + n > LE_MAX_LINE) {
        le_beep(le);
        return;
    }
    memmove(le->buf + start + n, le->buf + end, le->len - end + 1);
    memcpy(le->buf + start, text, n);
    le->len = le->len - (end - start) + n;
    le->cursor = start + n;
}

static void le_backspace(LineEditor *le) {
    if (le->cursor == 0)
        return;
    {
        size_t start = cp_prev(le->buf, le->cursor);
        size_t n = le->cursor - start;
        memmove(le->buf + start, le->buf + le->cursor, le->len - le->cursor + 1);
        le->len -= n;
        le->cursor = start;
    }
}

static void le_delete(LineEditor *le) {
    if (le->cursor >= le->len)
        return;
    {
        size_t end = cp_next(le->buf, le->len, le->cursor);
        size_t n = end - le->cursor;
        memmove(le->buf + le->cursor, le->buf + end, le->len - end + 1);
        le->len -= n;
    }
}

static void le_delete_word(LineEditor *le) {
    size_t start = le->cursor;
    while (start > 0 && isspace((unsigned char)le->buf[start - 1]))
        start--;
    while (start > 0 && !isspace((unsigned char)le->buf[start - 1]))
        start = cp_prev(le->buf, start);
    if (start == le->cursor)
        return;
    memmove(le->buf + start, le->buf + le->cursor, le->len - le->cursor + 1);
    le->len -= le->cursor - start;
    le->cursor = start;
}

static size_t le_word_prev(const char *buf, size_t i) {
    while (i > 0 && isspace((unsigned char)buf[i - 1]))
        i = cp_prev(buf, i);
    while (i > 0 && !isspace((unsigned char)buf[i - 1]))
        i = cp_prev(buf, i);
    return i;
}

static size_t le_word_next(const char *buf, size_t len, size_t i) {
    while (i < len && isspace((unsigned char)buf[i]))
        i = cp_next(buf, len, i);
    while (i < len && !isspace((unsigned char)buf[i]))
        i = cp_next(buf, len, i);
    return i;
}

static void le_kill_to_end(LineEditor *le) {
    le->buf[le->cursor] = 0;
    le->len = le->cursor;
}

static void le_kill_to_start(LineEditor *le) {
    memmove(le->buf, le->buf + le->cursor, le->len - le->cursor + 1);
    le->len -= le->cursor;
    le->cursor = 0;
}

/* ------------------------------------------------------------- 완성/히스토리 */

void Le_AddCompletion(LineEditor *le, const char *text) {
    char *copy;
    if (!text || !text[0])
        return;
    if (le->ncands == le->cand_cap) {
        size_t cap = le->cand_cap ? le->cand_cap * 2 : 32;
        char **grown = realloc(le->cands, cap * sizeof *grown);
        if (!grown)
            return;
        le->cands = grown;
        le->cand_cap = cap;
    }
    copy = malloc(strlen(text) + 1);
    if (!copy)
        return;
    strcpy(copy, text);
    le->cands[le->ncands++] = copy;
}

static int cmp_cand(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static size_t common_prefix_len(char **cands, size_t n) {
    size_t len = strlen(cands[0]);
    size_t k;
    for (k = 1; k < n; k++) {
        size_t i = 0;
        while (i < len && cands[k][i] && cands[0][i] == cands[k][i])
            i++;
        len = i;
    }
    while (len > 0 && ((unsigned char)cands[0][len] & 0xC0) == 0x80)
        len--;
    return len;
}

static void le_show_candidates(LineEditor *le) {
    int width = le_terminal_width(le);
    size_t maxw = 0, i, cols, colw;

    for (i = 0; i < le->ncands; i++) {
        size_t w = display_cols(le->cands[i], strlen(le->cands[i]));
        if (w > maxw)
            maxw = w;
    }
    colw = maxw + 2;
    cols = (size_t)width / colw;
    if (cols == 0)
        cols = 1;
    fputs("\r\n", stdout);
    for (i = 0; i < le->ncands; i++) {
        size_t w = display_cols(le->cands[i], strlen(le->cands[i]));
        fputs(le->cands[i], stdout);
        if ((i + 1) % cols == 0 || i + 1 == le->ncands)
            fputs("\r\n", stdout);
        else {
            size_t pad = colw - w;
            while (pad--)
                fputc(' ', stdout);
        }
    }
    le->prev_row = 0;
}

static void le_complete(LineEditor *le) {
    size_t start = le->cursor;
    size_t info_len;
    size_t lcp;

    while (start > 0) {
        unsigned char c = (unsigned char)le->buf[start - 1];
        if (isalnum(c) || c == '_' || c == ':')
            start--;
        else
            break;
    }
    if (start == le->cursor || !le->complete) {
        le_beep(le);
        return;
    }
    le->ncands = 0;
    le->complete(le->complete_ctx, le, le->buf, start, le->cursor);
    if (le->ncands == 0) {
        le_beep(le);
        return;
    }
    qsort(le->cands, le->ncands, sizeof *le->cands, cmp_cand);
    {
        size_t i, out = 1;
        for (i = 1; i < le->ncands; i++) {
            if (strcmp(le->cands[i], le->cands[out - 1]) == 0)
                free(le->cands[i]);
            else
                le->cands[out++] = le->cands[i];
        }
        le->ncands = out;
    }
    if (le->ncands == 1) {
        le_replace_span(le, start, le->cursor, le->cands[0], strlen(le->cands[0]));
        le_render(le);
        return;
    }
    lcp = common_prefix_len(le->cands, le->ncands);
    info_len = le->cursor - start;
    if (lcp > info_len) {
        le_replace_span(le, start, le->cursor, le->cands[0], lcp);
        le_render(le);
        return;
    }
    le_show_candidates(le);
    le_render(le);
}

static void le_history_apply(LineEditor *le, size_t index) {
    le_set_buffer(le, le->history[index]);
    le_render(le);
}

static void le_history_prev(LineEditor *le) {
    if (le->hist_index > 0) {
        le->hist_index--;
        le_history_apply(le, le->hist_index);
    }
    else
        le_beep(le);
}

static void le_history_next(LineEditor *le) {
    if (le->hist_index + 1 < le->nhistory) {
        le->hist_index++;
        le_history_apply(le, le->hist_index);
    }
    else if (le->hist_index < le->nhistory) {
        le->hist_index = le->nhistory;
        le->len = 0;
        le->cursor = 0;
        le->buf[0] = 0;
        le_render(le);
    }
}

/* ------------------------------------------------------------- 터미널 모드 */

static int le_width_init(void) {
    int width = le_os_width();
    return width > 1 ? width : LE_DEFAULT_WIDTH;
}

static int le_can_edit(void) {
#if defined(_WIN32)
    AsterHandle hin = GetStdHandle(ASTER_STD_INPUT_HANDLE);
    AsterHandle hout = GetStdHandle(ASTER_STD_OUTPUT_HANDLE);
    unsigned long mode;
    if (!hin || !hout || !GetConsoleMode(hin, &mode))
        return 0;
    return SetConsoleMode(hin, mode) && GetConsoleMode(hout, &mode) &&
           SetConsoleMode(hout, mode);
#else
    struct termios tio;
    return tcgetattr(0, &tio) == 0;
#endif
}

void Le_SetCompleter(LineEditor *le, LeCompleteFn fn, void *ctx) {
    le->complete = fn;
    le->complete_ctx = ctx;
}

LineEditor *Le_New(void) {
    LineEditor *le;

    if (!le_can_edit())
        return NULL;
    le = calloc(1, sizeof *le);
    if (!le)
        return NULL;
    le->cap = LE_MAX_LINE + 1;
    le->buf = malloc(le->cap);
    if (!le->buf) {
        free(le);
        return NULL;
    }
    le->buf[0] = 0;
    le->width = le_width_init();
    return le;
}

void Le_Free(LineEditor *le) {
    size_t i;
    if (!le)
        return;
    for (i = 0; i < le->nhistory; i++)
        free(le->history[i]);
    free(le->history);
    for (i = 0; i < le->ncands; i++)
        free(le->cands[i]);
    free(le->cands);
    free(le->buf);
    free(le);
}

void Le_HistoryAdd(LineEditor *le, const char *line) {
    char *copy;

    if (!line || !line[0])
        return;
    if (le->nhistory > 0 && strcmp(le->history[le->nhistory - 1], line) == 0)
        return;
    if (le->nhistory == LE_HISTORY_MAX) {
        free(le->history[0]);
        memmove(le->history, le->history + 1,
                (le->nhistory - 1) * sizeof *le->history);
        le->nhistory--;
    }
    if (le->nhistory == le->hist_cap) {
        size_t cap = le->hist_cap ? le->hist_cap * 2 : 64;
        char **grown = realloc(le->history, cap * sizeof *grown);
        if (!grown)
            return;
        le->history = grown;
        le->hist_cap = cap;
    }
    copy = malloc(strlen(line) + 1);
    if (!copy)
        return;
    strcpy(copy, line);
    le->history[le->nhistory++] = copy;
}

int Le_HistoryLoad(LineEditor *le, const char *path) {
    FILE *f = fopen(path, "rb");
    char line[LE_MAX_LINE + 1];

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        Le_HistoryAdd(le, line);
    }
    fclose(f);
    return 1;
}

int Le_HistorySave(LineEditor *le, const char *path) {
    FILE *f;
    size_t i, start = 0;

    if (le->nhistory == 0)
        return 1;
    f = fopen(path, "wb");
    if (!f)
        return 0;
    if (le->nhistory > LE_HISTORY_MAX)
        start = le->nhistory - LE_HISTORY_MAX;
    for (i = start; i < le->nhistory; i++)
        fprintf(f, "%s\n", le->history[i]);
    fclose(f);
    return 1;
}

/* --------------------------------------------------------------- 입력 루프 */

static int le_getc(LineEditor *le) {
    unsigned char c;
    (void)le;
#if defined(_WIN32)
    /* UCRT _read()는 콘솔 입력 모드를 raw로 다시 설정한 다음 줄에서 EACCES로
       실패한다(입력은 버퍼에 남아 있는데 읽지 못한다). 같은 핸들의 ReadFile은
       매번 정상 동작하므로 콘솔에서는 직접 읽는다. */
    {
        unsigned long got = 0;
        if (!ReadFile(GetStdHandle(ASTER_STD_INPUT_HANDLE), &c, 1, &got, 0) ||
            got != 1)
            return -1;
    }
#else
    if (read(0, &c, 1) != 1)
        return -1;
#endif
    return (int)c;
}

static int le_enter_raw(LineEditor *le) {
#if defined(_WIN32)
    AsterHandle hin = GetStdHandle(ASTER_STD_INPUT_HANDLE);
    AsterHandle hout = GetStdHandle(ASTER_STD_OUTPUT_HANDLE);
    unsigned long mode;

    if (!GetConsoleMode(hin, &mode))
        return 0;
    le->saved_in_mode = mode;
    /* PROCESSED_INPUT을 끄면 Ctrl-C가 시그널이 아니라 바이트로 들어와
       줄 취소로 처리할 수 있고, 편집 중 터미널이 raw로 남는 일이 없다.
       마우스/창 이벤트는 VT 입력 모드에서 CSI 시퀀스로 섞여 들어오므로
       편집 중에는 끈다. 빠른 편집/삽입 플래그는 콘솔에 따라 거부되므로
       한 번 더 시도한다. */
    {
        unsigned long base = mode & ~(ASTER_ENABLE_PROCESSED_INPUT |
                                      ASTER_ENABLE_LINE_INPUT |
                                      ASTER_ENABLE_ECHO_INPUT |
                                      ASTER_ENABLE_MOUSE_INPUT |
                                      ASTER_ENABLE_WINDOW_INPUT);
        unsigned long trimmed = base & ~(ASTER_ENABLE_INSERT_MODE |
                                         ASTER_ENABLE_QUICK_EDIT);
        if (!SetConsoleMode(hin, trimmed | ASTER_ENABLE_VT_INPUT)) {
            if (!SetConsoleMode(hin, base | ASTER_ENABLE_VT_INPUT)) {
                SetConsoleMode(hin, mode);
                return 0;
            }
        }
    }
    le->have_out_mode = GetConsoleMode(hout, &mode) ? 1 : 0;
    if (le->have_out_mode) {
        le->saved_out_mode = mode;
        SetConsoleMode(hout, mode | ASTER_ENABLE_VT_PROCESSING);
    }
    le->saved_cp = GetConsoleCP();
    if (le->saved_cp != 65001)
        SetConsoleCP(65001);
    le->saved_out_cp = GetConsoleOutputCP();
    if (le->saved_out_cp != 65001)
        SetConsoleOutputCP(65001);
    return 1;
#else
    struct termios raw;
    if (tcgetattr(0, &le->saved_tio) != 0)
        return 0;
    raw = le->saved_tio;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSADRAIN, &raw) != 0)
        return 0;
    return 1;
#endif
}

static void le_leave_raw(LineEditor *le) {
#if defined(_WIN32)
    if (le->saved_out_cp != 65001)
        SetConsoleOutputCP(le->saved_out_cp);
    if (le->saved_cp != 65001)
        SetConsoleCP(le->saved_cp);
    if (le->have_out_mode)
        SetConsoleMode(GetStdHandle(ASTER_STD_OUTPUT_HANDLE), le->saved_out_mode);
    SetConsoleMode(GetStdHandle(ASTER_STD_INPUT_HANDLE), le->saved_in_mode);
#else
    tcsetattr(0, TCSADRAIN, &le->saved_tio);
#endif
}

#if defined(_WIN32)
static LineEditor *active_editor;

/* Ctrl-Break, 창 닫기처럼 raw를 되돌릴 틈 없이 끝나는 경우에도 콘솔 모드를
   복구한다. 반환값 0은 기본 종료 처리를 계속하라는 뜻이다. */
static int __stdcall le_restore_console(unsigned long type) {
    (void)type;
    if (active_editor)
        le_leave_raw(active_editor);
    return 0;
}
#endif

static void le_handle_escape(LineEditor *le) {
    int c = le_getc(le);
    char seq[16];
    size_t n = 0;
    int d;
    int last = 0;

    if (c < 0)
        return;
    if (c != '[' && c != 'O') {
        if (c == 'b' || c == 'B')
            le->cursor = le_word_prev(le->buf, le->cursor);
        else if (c == 'f' || c == 'F')
            le->cursor = le_word_next(le->buf, le->len, le->cursor);
        else if (c == 'd' || c == 'D') {
            size_t end = le_word_next(le->buf, le->len, le->cursor);
            le_replace_span(le, le->cursor, end, "", 0);
        }
        else if (c == 0x7F) {
            le_delete_word(le);
        }
        return;
    }
    /* CSI는 파라미터(0x30-0x3F)와 중간 바이트(0x20-0x2F) 뒤 마지막
       바이트(0x40-0x7E)로 끝난다. Windows 콘솔은 마우스 보고처럼 긴
       시퀀스를 보내므로 길이를 제한하지 않고 끝까지 소비해야 한다.
       앞부분만 저장하고, 남는 바이트는 버린다. */
    for (;;) {
        d = le_getc(le);
        if (d < 0)
            return;
        if (n < sizeof seq - 1)
            seq[n++] = (char)d;
        if (d >= 0x40 && d <= 0x7E) {
            last = d;
            break;
        }
        if (d < 0x20)           /* 시퀀스가 아닌 제어 바이트에서 중단 */
            return;
    }
    seq[n] = 0;
    if (last == '~') {
        int num = atoi(seq);
        if (num == 1 || num == 7)
            le->cursor = 0;
        else if (num == 4 || num == 8)
            le->cursor = le->len;
        else if (num == 3)
            le_delete(le);
        return;
    }
    if (last == 'A' || last == 'B') {
        if (last == 'A')
            le_history_prev(le);
        else
            le_history_next(le);
    }
    else if (last == 'C')
        le->cursor = cp_next(le->buf, le->len, le->cursor);
    else if (last == 'D')
        le->cursor = cp_prev(le->buf, le->cursor);
    else if (last == 'H')
        le->cursor = 0;
    else if (last == 'F')
        le->cursor = le->len;
}

static char *le_read_line(LineEditor *le, const char *prompt) {
    le->prompt = prompt;
    le->len = 0;
    le->cursor = 0;
    le->buf[0] = 0;
    le->hist_index = le->nhistory;
    le->prev_row = 0;
    le_render(le);

    for (;;) {
        int c = le_getc(le);
        if (le->skip_lf) {
            le->skip_lf = 0;
            if (c == '\n')
                continue;
        }
        if (c < 0) {
            if (le->len == 0) {
                fputs("\r\n", stdout);
                return NULL;
            }
            break;
        }
        if (c == '\r' || c == '\n') {
            /* \r\n으로 들어오는 입력에서 다음 \n은 빈 줄로 처리하지 않는다. */
            le->skip_lf = (c == '\r');
            le->cursor = le->len;
            le_render(le);         /* 커서가 중간에 있어도 줄 전체를 남긴다 */
            fputs("\r\n", stdout);
            break;
        }
        if (c == 0x03) {           /* Ctrl-C: 현재 줄만 취소한다 */
            le->len = 0;
            le->cursor = 0;
            le->buf[0] = 0;
            le->hist_index = le->nhistory;
            fputs("^C\r\n", stdout);
            le->prev_row = 0;
            le_render(le);
            continue;
        }
        if (c == 0x04) {           /* Ctrl-D */
            if (le->len == 0) {
                fputs("\r\n", stdout);
                return NULL;
            }
            le_delete(le);
            le_render(le);
            continue;
        }
        if (c == '\t') {
            le_complete(le);
            continue;
        }
        if (c == 0x7F || c == 0x08) {
            le_backspace(le);
            le_render(le);
            continue;
        }
        if (c == 0x1B) {
            le_handle_escape(le);
            le_render(le);
            continue;
        }
        if (c == 0x01) {           /* Ctrl-A */
            le->cursor = 0;
            le_render(le);
            continue;
        }
        if (c == 0x05) {           /* Ctrl-E */
            le->cursor = le->len;
            le_render(le);
            continue;
        }
        if (c == 0x02) {           /* Ctrl-B */
            le->cursor = cp_prev(le->buf, le->cursor);
            le_render(le);
            continue;
        }
        if (c == 0x06) {           /* Ctrl-F */
            le->cursor = cp_next(le->buf, le->len, le->cursor);
            le_render(le);
            continue;
        }
        if (c == 0x10) {           /* Ctrl-P */
            le_history_prev(le);
            continue;
        }
        if (c == 0x0E) {           /* Ctrl-N */
            le_history_next(le);
            continue;
        }
        if (c == 0x0B) {           /* Ctrl-K */
            le_kill_to_end(le);
            le_render(le);
            continue;
        }
        if (c == 0x15) {           /* Ctrl-U */
            le_kill_to_start(le);
            le_render(le);
            continue;
        }
        if (c == 0x17) {           /* Ctrl-W */
            le_delete_word(le);
            le_render(le);
            continue;
        }
        if (c == 0x0C) {           /* Ctrl-L */
            fputs("\x1b[2J\x1b[H", stdout);
            le->prev_row = 0;
            le_render(le);
            continue;
        }
        if (c < 0x20)
            continue;              /* 나머지 제어 문자는 무시 */
        {
            char ch = (char)c;
            le_insert(le, &ch, 1);
            if (cp_complete_before(le->buf, le->cursor))
                le_render(le);
        }
    }

    le->buf[le->len] = 0;
    if (le->len > 0)
        Le_HistoryAdd(le, le->buf);
    {
        char *result = malloc(le->len + 2);
        if (!result)
            return NULL;
        memcpy(result, le->buf, le->len);
        result[le->len] = '\n';
        result[le->len + 1] = 0;
        return result;
    }
}

/* 터미널 모드를 바꿀 수 없을 때는 기존 fgets 동작으로 돌아간다. */
static char *le_read_fallback(const char *prompt) {
    char line[4096];
    size_t n;

    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin))
        return NULL;
    n = strlen(line);
    if (n == 0 || line[n - 1] != '\n') {
        if (n + 1 >= sizeof line)
            n = sizeof line - 2;
        line[n++] = '\n';
        line[n] = 0;
    }
    {
        char *result = malloc(n + 1);
        if (!result)
            return NULL;
        memcpy(result, line, n + 1);
        return result;
    }
}

char *Le_ReadLine(LineEditor *le, const char *prompt) {
    char *result;

    if (!le)
        return NULL;
#if defined(_WIN32)
    {
        static int handler_added;
        if (!handler_added) {
            SetConsoleCtrlHandler(le_restore_console, 1);
            handler_added = 1;
        }
    }
#endif
    if (!le_enter_raw(le))
        return le_read_fallback(prompt);
#if defined(_WIN32)
    active_editor = le;
#endif
    result = le_read_line(le, prompt);
#if defined(_WIN32)
    active_editor = NULL;
#endif
    le_leave_raw(le);
    return result;
}
