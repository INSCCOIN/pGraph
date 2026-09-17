/* pGraph — 3D function plotter for SharkDeck framebuffer. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fb = -1;
static unsigned char *map;
static size_t maplen;
static unsigned W, H, BPP, LINE;
static struct termios oldt;
static int raw_on;

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void px(int x, int y, uint16_t c)
{
    if ((unsigned)x >= W || (unsigned)y >= H)
        return;
    unsigned char *p = map + (size_t)y * LINE + (size_t)x * (BPP / 8);
    if (BPP == 16) {
        ((uint16_t *)p)[0] = c;
    } else if (BPP == 32) {
        p[0] = (unsigned char)((c & 0x1f) << 3);
        p[1] = (unsigned char)(((c >> 5) & 0x3f) << 2);
        p[2] = (unsigned char)(((c >> 11) & 0x1f) << 3);
        p[3] = 0;
    }
}

static void clear(uint16_t c)
{
    unsigned y, x;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            px(x, y, c);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            px(x + i, y + j, c);
}

/* tiny 5x7 font */
static const unsigned char FONT[96][5] = {
    {0,0,0,0,0}, {0,0,0x5f,0,0}, {0,7,0,7,0}, {0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50},
    {0,5,3,0,0}, {0,0x1c,0x22,0x41,0}, {0,0x41,0x22,0x1c,0}, {0x14,8,0x3e,8,0x14},
    {8,8,0x3e,8,8}, {0,0x50,0x30,0,0}, {8,8,8,8,8}, {0,0x60,0x60,0,0},
    {0x20,0x10,8,4,2}, {0x3e,0x51,0x49,0x45,0x3e}, {0,0x42,0x7f,0x40,0},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31}, {0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3c,0x4a,0x49,0x49,0x30}, {1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36}, {6,0x49,0x49,0x29,0x1e}, {0,0x36,0x36,0,0},
    {0,0x56,0x36,0,0}, {8,0x14,0x22,0x41,0}, {0x14,0x14,0x14,0x14,0x14},
    {0,0x41,0x22,0x14,8}, {2,1,0x51,9,6}, {0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36}, {0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41}, {0x7f,9,9,9,1},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,8,8,8,0x7f}, {0,0x41,0x7f,0x41,0},
    {0x20,0x40,0x41,0x3f,1}, {0x7f,8,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,2,0x0c,2,0x7f}, {0x7f,4,8,0x10,0x7f}, {0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,9,9,9,6}, {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {1,1,0x7f,1,1}, {0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f}, {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7}, {0x61,0x51,0x49,0x45,0x43}, {0,0x7f,0x41,0x41,0},
    {2,4,8,0x10,0x20}, {0,0x41,0x41,0x7f,0}, {4,2,1,2,4}, {0x40,0x40,0x40,0x40,0x40},
    {0,1,2,4,0}, {0x20,0x54,0x54,0x54,0x78}, {0x7f,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7f}, {0x38,0x54,0x54,0x54,0x18},
    {8,0x7e,9,1,2}, {0x0c,0x52,0x52,0x52,0x3e}, {0x7f,8,4,4,0x78},
    {0,0x44,0x7d,0x40,0}, {0x20,0x40,0x44,0x3d,0}, {0x7f,0x10,0x28,0x44,0},
    {0,0x41,0x7f,0x40,0}, {0x7c,4,0x18,4,0x78}, {0x7c,8,4,4,0x78},
    {0x38,0x44,0x44,0x44,0x38}, {0x7c,0x14,0x14,0x14,8}, {8,0x14,0x14,0x18,0x7c},
    {0x7c,8,4,4,8}, {0x48,0x54,0x54,0x54,0x20}, {4,0x3f,0x44,0x40,0x20},
    {0x3c,0x40,0x40,0x20,0x7c}, {0x1c,0x20,0x40,0x20,0x1c}, {0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44}, {0x0c,0x50,0x50,0x50,0x3c}, {0x44,0x64,0x54,0x4c,0x44},
};

static void glyph(int x, int y, char ch, uint16_t c)
{
    int gx, gy;
    unsigned char col;
    if (ch < 32 || ch > 127)
        ch = '?';
    for (gx = 0; gx < 5; gx++) {
        col = FONT[ch - 32][gx];
        for (gy = 0; gy < 7; gy++)
            if (col & (1 << gy))
                px(x + gx, y + gy, c);
    }
}

static void text(int x, int y, const char *s, uint16_t c)
{
    while (*s) {
        glyph(x, y, *s++, c);
        x += 6;
    }
}

/* ---- expression ---- */
typedef struct {
    const char *s;
    const char *p;
    int err;
    double x, y;
} Expr;

static double parse_expr(Expr *e);

static void skip(Expr *e)
{
    while (*e->p == ' ')
        e->p++;
}

static double parse_num(Expr *e)
{
    char *end;
    double v = strtod(e->p, &end);
    if (end == e->p) {
        e->err = 1;
        return 0;
    }
    e->p = end;
    return v;
}

static double parse_ident(Expr *e)
{
    char id[16];
    int n = 0;
    double a;
    while (isalpha((unsigned char)*e->p) && n < 15)
        id[n++] = (char)tolower((unsigned char)*e->p++);
    id[n] = 0;
    skip(e);
    if (!strcmp(id, "x"))
        return e->x;
    if (!strcmp(id, "y"))
        return e->y;
    if (!strcmp(id, "pi"))
        return M_PI;
    if (!strcmp(id, "e"))
        return 2.718281828459045;
    if (*e->p != '(') {
        e->err = 1;
        return 0;
    }
    e->p++;
    a = parse_expr(e);
    skip(e);
    if (*e->p == ')')
        e->p++;
    if (!strcmp(id, "sin"))
        return sin(a);
    if (!strcmp(id, "cos"))
        return cos(a);
    if (!strcmp(id, "tan"))
        return tan(a);
    if (!strcmp(id, "sqrt"))
        return a < 0 ? NAN : sqrt(a);
    if (!strcmp(id, "abs"))
        return fabs(a);
    if (!strcmp(id, "ln") || !strcmp(id, "log"))
        return a <= 0 ? NAN : log(a);
    e->err = 1;
    return 0;
}

static double parse_unary(Expr *e)
{
    skip(e);
    if (*e->p == '+') {
        e->p++;
        return parse_unary(e);
    }
    if (*e->p == '-') {
        e->p++;
        return -parse_unary(e);
    }
    if (*e->p == '(') {
        double v;
        e->p++;
        v = parse_expr(e);
        skip(e);
        if (*e->p == ')')
            e->p++;
        return v;
    }
    if (isalpha((unsigned char)*e->p))
        return parse_ident(e);
    return parse_num(e);
}

static double parse_pow(Expr *e)
{
    double v = parse_unary(e);
    skip(e);
    if (*e->p == '^') {
        e->p++;
        v = pow(v, parse_pow(e));
    }
    return v;
}

static double parse_term(Expr *e)
{
    double v = parse_pow(e);
    for (;;) {
        skip(e);
        if (*e->p == '*') {
            e->p++;
            v *= parse_pow(e);
        } else if (*e->p == '/') {
            double d;
            e->p++;
            d = parse_pow(e);
            v = d == 0 ? NAN : v / d;
        } else
            break;
    }
    return v;
}

static double parse_expr(Expr *e)
{
    double v = parse_term(e);
    for (;;) {
        skip(e);
        if (*e->p == '+') {
            e->p++;
            v += parse_term(e);
        } else if (*e->p == '-') {
            e->p++;
            v -= parse_term(e);
        } else
            break;
    }
    return v;
}

static int eval_xy(const char *s, double x, double y, double *out)
{
    Expr e;
    double v;
    e.s = e.p = s;
    e.err = 0;
    e.x = x;
    e.y = y;
    v = parse_expr(&e);
    skip(&e);
    if (e.err || *e.p)
        return -1;
    *out = v;
    return 0;
}

/* ---- plot ---- */
#define GN 22
static char formula[96] = "sin(sqrt(x*x+y*y))";
static double yaw = 0.7, pitch = 0.45, zoom = 70;
static double zbuf[GN][GN];
static int zok[GN][GN];
static double xmin = -3.2, xmax = 3.2, ymin = -3.2, ymax = 3.2;

static uint16_t COL_BG, COL_GRID, COL_LINE, COL_AXIS, COL_TXT, COL_DIM, COL_HI;

static void project(double x, double y, double z, int *sx, int *sy)
{
    double cy = cos(yaw), syw = sin(yaw);
    double cp = cos(pitch), sp = sin(pitch);
    double x1 = x * cy - y * syw;
    double y1 = x * syw + y * cy;
    double y2 = y1 * cp - z * sp;
    double z2 = y1 * sp + z * cp;
    double f = zoom / (4.2 + z2);
    *sx = (int)(W * 0.52 + x1 * f);
    *sy = (int)(H * 0.42 - y2 * f);
}

static void rebuild(void)
{
    int i, j;
    for (j = 0; j < GN; j++) {
        for (i = 0; i < GN; i++) {
            double x = xmin + (xmax - xmin) * i / (GN - 1);
            double y = ymin + (ymax - ymin) * j / (GN - 1);
            double z;
            if (eval_xy(formula, x, y, &z) || !isfinite(z)) {
                zok[j][i] = 0;
                zbuf[j][i] = 0;
            } else {
                zok[j][i] = 1;
                zbuf[j][i] = z;
            }
        }
    }
}

static void draw_gui(const char *status)
{
    char bar[128];
    fill_rect(0, 0, (int)W, 12, COL_DIM);
    text(2, 3, "pGraph", COL_HI);
    snprintf(bar, sizeof bar, " y%.0f p%.0f", yaw * 180 / M_PI, pitch * 180 / M_PI);
    text((int)W - 6 * (int)strlen(bar) - 2, 3, bar, COL_TXT);

    fill_rect(0, (int)H - 22, (int)W, 22, COL_DIM);
    text(2, (int)H - 19, "z=", COL_HI);
    text(16, (int)H - 19, formula, COL_TXT);
    text(2, (int)H - 10, status, COL_TXT);
}

static void render(const char *status)
{
    int i, j, x0, y0, x1, y1;
    clear(COL_BG);

    /* axes */
    project(-3.5, 0, 0, &x0, &y0);
    project(3.5, 0, 0, &x1, &y1);
    line(x0, y0, x1, y1, COL_AXIS);
    project(0, -3.5, 0, &x0, &y0);
    project(0, 3.5, 0, &x1, &y1);
    line(x0, y0, x1, y1, COL_AXIS);
    project(0, 0, -2, &x0, &y0);
    project(0, 0, 2, &x1, &y1);
    line(x0, y0, x1, y1, COL_AXIS);

    for (j = 0; j < GN; j++) {
        for (i = 0; i < GN - 1; i++) {
            if (!zok[j][i] || !zok[j][i + 1])
                continue;
            {
                double x = xmin + (xmax - xmin) * i / (GN - 1);
                double y = ymin + (ymax - ymin) * j / (GN - 1);
                double x2 = xmin + (xmax - xmin) * (i + 1) / (GN - 1);
                project(x, y, zbuf[j][i], &x0, &y0);
                project(x2, y, zbuf[j][i + 1], &x1, &y1);
                line(x0, y0, x1, y1, COL_LINE);
            }
        }
    }
    for (j = 0; j < GN - 1; j++) {
        for (i = 0; i < GN; i++) {
            if (!zok[j][i] || !zok[j + 1][i])
                continue;
            {
                double x = xmin + (xmax - xmin) * i / (GN - 1);
                double y = ymin + (ymax - ymin) * j / (GN - 1);
                double y2 = ymin + (ymax - ymin) * (j + 1) / (GN - 1);
                project(x, y, zbuf[j][i], &x0, &y0);
                project(x, y2, zbuf[j + 1][i], &x1, &y1);
                line(x0, y0, x1, y1, COL_GRID);
            }
        }
    }
    draw_gui(status);
}

static void raw_term(int on)
{
    struct termios t;
    if (on) {
        tcgetattr(0, &oldt);
        t = oldt;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        raw_on = 1;
    } else if (raw_on) {
        tcsetattr(0, TCSANOW, &oldt);
        raw_on = 0;
    }
}

static int fb_open(void)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0)
        return -1;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &v) < 0)
        return -1;
    if (ioctl(fb, FBIOGET_FSCREENINFO, &f) < 0)
        return -1;
    W = v.xres;
    H = v.yres;
    BPP = v.bits_per_pixel;
    LINE = f.line_length;
    maplen = f.smem_len ? f.smem_len : (size_t)LINE * H;
    map = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    return map == MAP_FAILED ? -1 : 0;
}

static const char *PRE[] = {
    "sin(sqrt(x*x+y*y))",
    "cos(x)*cos(y)",
    "x*x-y*y",
    "sin(x)+cos(y)",
    "x*y/4",
    "sqrt(x*x+y*y)",
};

int main(void)
{
    char status[80] = "arrows rotate  +- zoom  1-6 presets  type formula";
    int pre = 0;
    int running = 1;

    if (fb_open() < 0) {
        fprintf(stderr, "pGraph: cannot open /dev/fb0: %s\n", strerror(errno));
        fprintf(stderr, "run on the deck console, not a pipe\n");
        return 1;
    }
    COL_BG = rgb565(0, 0, 0);
    COL_GRID = rgb565(0, 90, 0);
    COL_LINE = rgb565(0, 220, 40);
    COL_AXIS = rgb565(0, 70, 0);
    COL_TXT = rgb565(180, 255, 180);
    COL_DIM = rgb565(0, 28, 0);
    COL_HI = rgb565(0, 255, 80);

    raw_term(1);
    rebuild();
    render(status);

    while (running) {
        unsigned char ch = 0;
        fd_set rf;
        struct timeval tv = {0, 40000};
        FD_ZERO(&rf);
        FD_SET(0, &rf);
        if (select(1, &rf, NULL, NULL, &tv) > 0)
            if (read(0, &ch, 1) != 1)
                ch = 0;
        if (!ch)
            continue;
        if (ch == 0x1b) {
            unsigned char seq[2] = {0, 0};
            struct timeval t2 = {0, 80000};
            FD_ZERO(&rf);
            FD_SET(0, &rf);
            if (select(1, &rf, NULL, NULL, &t2) > 0)
                read(0, seq, 2);
            if (seq[0] == '[') {
                if (seq[1] == 'A')
                    pitch -= 0.12;
                if (seq[1] == 'B')
                    pitch += 0.12;
                if (seq[1] == 'C')
                    yaw += 0.12;
                if (seq[1] == 'D')
                    yaw -= 0.12;
                snprintf(status, sizeof status, "rotate");
                render(status);
            } else
                running = 0;
            continue;
        }
        if (ch == 'q' || ch == 'Q')
            running = 0;
        else if (ch == '+' || ch == '=') {
            zoom *= 1.12;
            snprintf(status, sizeof status, "zoom in");
            render(status);
        } else if (ch == '-' || ch == '_') {
            zoom /= 1.12;
            snprintf(status, sizeof status, "zoom out");
            render(status);
        } else if (ch == '0') {
            yaw = 0.7;
            pitch = 0.45;
            zoom = 70;
            snprintf(status, sizeof status, "reset view");
            render(status);
        } else if (ch >= '1' && ch <= '6') {
            pre = ch - '1';
            snprintf(formula, sizeof formula, "%s", PRE[pre]);
            rebuild();
            snprintf(status, sizeof status, "preset %c", ch);
            render(status);
        } else if (ch == 8 || ch == 127) {
            size_t n = strlen(formula);
            if (n)
                formula[n - 1] = 0;
            snprintf(status, sizeof status, "edit");
            render(status);
        } else if (ch == 10 || ch == 13) {
            double dummy;
            if (eval_xy(formula, 0.1, 0.1, &dummy) && eval_xy(formula, 1, 1, &dummy))
                snprintf(status, sizeof status, "bad formula");
            else {
                rebuild();
                snprintf(status, sizeof status, "plotted");
            }
            render(status);
        } else if (ch >= 32 && ch < 127) {
            size_t n = strlen(formula);
            if (n + 1 < sizeof formula) {
                formula[n] = (char)ch;
                formula[n + 1] = 0;
            }
            snprintf(status, sizeof status, "edit  enter to plot");
            render(status);
        }
    }

    raw_term(0);
    munmap(map, maplen);
    close(fb);
    return 0;
}
