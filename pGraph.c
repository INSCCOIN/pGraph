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

enum { MODE_WIRE, MODE_COLOR, MODE_SOLID, MODE_DOTS, MODE_N };

static int mode = MODE_COLOR;
static const char *MODE_NAME[] = {"wire", "color", "solid", "dots"};

static uint16_t heat(double zn)
{
    int r, g, b;
    if (zn < -1)
        zn = -1;
    if (zn > 1)
        zn = 1;
    if (zn < 0) {
        double t = zn + 1;
        r = (int)(20 * t);
        g = (int)(40 + 140 * t);
        b = (int)(180 - 80 * t);
    } else {
        double t = zn;
        r = (int)(40 + 200 * t);
        g = (int)(200 - 80 * t);
        b = (int)(40 - 30 * t);
    }
    if (r < 0)
        r = 0;
    if (g < 0)
        g = 0;
    if (b < 0)
        b = 0;
    if (r > 255)
        r = 255;
    if (g > 255)
        g = 255;
    if (b > 255)
        b = 255;
    return rgb565(r, g, b);
}

static void hline(int x0, int x1, int y, uint16_t c)
{
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    for (; x0 <= x1; x0++)
        px(x0, y, c);
}

static void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{
    int i;
    int xs[3] = {x0, x1, x2};
    int ys[3] = {y0, y1, y2};
    for (i = 0; i < 2; i++) {
        int j;
        for (j = i + 1; j < 3; j++)
            if (ys[j] < ys[i]) {
                int t = ys[i];
                ys[i] = ys[j];
                ys[j] = t;
                t = xs[i];
                xs[i] = xs[j];
                xs[j] = t;
            }
    }
    if (ys[2] == ys[0])
        return;
    for (i = ys[0]; i <= ys[2]; i++) {
        int xa, xb;
        if (i <= ys[1] && ys[1] != ys[0])
            xa = xs[0] + (xs[1] - xs[0]) * (i - ys[0]) / (ys[1] - ys[0]);
        else if (ys[2] != ys[1])
            xa = xs[1] + (xs[2] - xs[1]) * (i - ys[1]) / (ys[2] - ys[1]);
        else
            xa = xs[1];
        xb = xs[0] + (xs[2] - xs[0]) * (i - ys[0]) / (ys[2] - ys[0]);
        hline(xa, xb, i, c);
    }
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

static int parse_args(Expr *e, double *a, double *b)
{
    int n = 1;
    skip(e);
    if (*e->p != '(') {
        e->err = 1;
        return 0;
    }
    e->p++;
    *a = parse_expr(e);
    skip(e);
    if (*e->p == ',') {
        e->p++;
        *b = parse_expr(e);
        n = 2;
        skip(e);
    }
    if (*e->p == ')')
        e->p++;
    return n;
}

static double parse_ident(Expr *e)
{
    char id[16];
    int n = 0, ac;
    double a = 0, b = 0;
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
    ac = parse_args(e, &a, &b);
    if (e->err)
        return 0;
    if (!strcmp(id, "sin"))
        return sin(a);
    if (!strcmp(id, "cos"))
        return cos(a);
    if (!strcmp(id, "tan"))
        return tan(a);
    if (!strcmp(id, "asin"))
        return fabs(a) > 1 ? NAN : asin(a);
    if (!strcmp(id, "acos"))
        return fabs(a) > 1 ? NAN : acos(a);
    if (!strcmp(id, "atan"))
        return atan(a);
    if (!strcmp(id, "atan2"))
        return ac == 2 ? atan2(a, b) : NAN;
    if (!strcmp(id, "sinh"))
        return sinh(a);
    if (!strcmp(id, "cosh"))
        return cosh(a);
    if (!strcmp(id, "tanh"))
        return tanh(a);
    if (!strcmp(id, "sqrt"))
        return a < 0 ? NAN : sqrt(a);
    if (!strcmp(id, "abs"))
        return fabs(a);
    if (!strcmp(id, "exp"))
        return exp(a);
    if (!strcmp(id, "ln") || !strcmp(id, "log"))
        return a <= 0 ? NAN : log(a);
    if (!strcmp(id, "log10"))
        return a <= 0 ? NAN : log10(a);
    if (!strcmp(id, "floor"))
        return floor(a);
    if (!strcmp(id, "ceil"))
        return ceil(a);
    if (!strcmp(id, "round"))
        return round(a);
    if (!strcmp(id, "sign"))
        return a > 0 ? 1 : a < 0 ? -1 : 0;
    if (!strcmp(id, "pow"))
        return ac == 2 ? pow(a, b) : NAN;
    if (!strcmp(id, "min"))
        return ac == 2 ? fmin(a, b) : a;
    if (!strcmp(id, "max"))
        return ac == 2 ? fmax(a, b) : a;
    if (!strcmp(id, "hypot"))
        return ac == 2 ? hypot(a, b) : fabs(a);
    if (!strcmp(id, "mod"))
        return ac == 2 && b != 0 ? fmod(a, b) : NAN;
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
        } else if (*e->p == '%') {
            double d;
            e->p++;
            d = parse_pow(e);
            v = d == 0 ? NAN : fmod(v, d);
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
#define GN 20
static char formula[96] = "sin(sqrt(x*x+y*y))";
static double yaw = 0.85, pitch = 0.55, zoom = 78;
static double zbuf[GN][GN];
static int zok[GN][GN];
static double xmin = -3.0, xmax = 3.0, ymin = -3.0, ymax = 3.0;
static double zmid, zspan = 1;

typedef struct {
    int x0, y0, x1, y1;
    double depth;
    uint16_t col;
} Edge;

static Edge edges[GN * GN * 2];
static int nedge;

static uint16_t COL_BG, COL_GRID, COL_LINE, COL_AXIS, COL_TXT, COL_DIM, COL_HI;

static int project(double x, double y, double z, int *sx, int *sy, double *depth)
{
    double zn, cy, syw, cp, sp, x1, y1, y2, z2, f;
    zn = (z - zmid) / zspan;
    if (zn > 1.8)
        zn = 1.8;
    if (zn < -1.8)
        zn = -1.8;
    cy = cos(yaw);
    syw = sin(yaw);
    cp = cos(pitch);
    sp = sin(pitch);
    x1 = x * cy - y * syw;
    y1 = x * syw + y * cy;
    y2 = y1 * cp - zn * sp;
    z2 = y1 * sp + zn * cp;
    if (z2 < -2.2)
        return 0;
    f = 3.6 + z2;
    if (f < 0.55)
        return 0;
    f = zoom / f;
    *sx = (int)(W * 0.50 + x1 * f);
    *sy = (int)(H * 0.40 - y2 * f);
    if (*sx < -80 || *sx > (int)W + 80 || *sy < -80 || *sy > (int)H + 80)
        return 0;
    if (depth)
        *depth = z2;
    return 1;
}

static void rebuild(void)
{
    int i, j, n = 0;
    double zmin = 0, zmax = 0;
    for (j = 0; j < GN; j++) {
        for (i = 0; i < GN; i++) {
            double x = xmin + (xmax - xmin) * i / (GN - 1);
            double y = ymin + (ymax - ymin) * j / (GN - 1);
            double z;
            if (eval_xy(formula, x, y, &z) || !isfinite(z) || fabs(z) > 1e6) {
                zok[j][i] = 0;
                zbuf[j][i] = 0;
            } else {
                zok[j][i] = 1;
                zbuf[j][i] = z;
                if (!n || z < zmin)
                    zmin = z;
                if (!n || z > zmax)
                    zmax = z;
                n++;
            }
        }
    }
    zmid = n ? (zmin + zmax) * 0.5 : 0;
    zspan = n ? (zmax - zmin) * 0.5 : 1;
    if (zspan < 0.35)
        zspan = 0.35;
}

static int cmp_edge(const void *a, const void *b)
{
    const Edge *x = a, *y = b;
    if (x->depth < y->depth)
        return -1;
    if (x->depth > y->depth)
        return 1;
    return 0;
}

static void add_edge(int i0, int j0, int i1, int j1, uint16_t col)
{
    double x0, y0, x1, y1, d0, d1;
    int sx0, sy0, sx1, sy1;
    if (!zok[j0][i0] || !zok[j1][i1])
        return;
    if (fabs(zbuf[j0][i0] - zbuf[j1][i1]) > zspan * 1.6)
        return;
    x0 = xmin + (xmax - xmin) * i0 / (GN - 1);
    y0 = ymin + (ymax - ymin) * j0 / (GN - 1);
    x1 = xmin + (xmax - xmin) * i1 / (GN - 1);
    y1 = ymin + (ymax - ymin) * j1 / (GN - 1);
    if (!project(x0, y0, zbuf[j0][i0], &sx0, &sy0, &d0))
        return;
    if (!project(x1, y1, zbuf[j1][i1], &sx1, &sy1, &d1))
        return;
    if ((sx0 - sx1) * (sx0 - sx1) + (sy0 - sy1) * (sy0 - sy1) > (int)(W * W))
        return;
    if (nedge >= (int)(sizeof edges / sizeof edges[0]))
        return;
    edges[nedge].x0 = sx0;
    edges[nedge].y0 = sy0;
    edges[nedge].x1 = sx1;
    edges[nedge].y1 = sy1;
    edges[nedge].depth = (d0 + d1) * 0.5;
    if (mode == MODE_WIRE)
        edges[nedge].col = col;
    else {
        double zn = ((zbuf[j0][i0] + zbuf[j1][i1]) * 0.5 - zmid) / zspan;
        edges[nedge].col = heat(zn);
    }
    nedge++;
}

typedef struct {
    int x[4], y[4];
    double depth;
    uint16_t col;
} Face;

static Face faces[GN * GN];
static int nface;

static int cmp_face(const void *a, const void *b)
{
    const Face *x = a, *y = b;
    if (x->depth < y->depth)
        return -1;
    if (x->depth > y->depth)
        return 1;
    return 0;
}

static void add_face(int i, int j)
{
    int k, p[4][2];
    double dsum = 0, zn;
    if (!zok[j][i] || !zok[j][i + 1] || !zok[j + 1][i] || !zok[j + 1][i + 1])
        return;
    if (fabs(zbuf[j][i] - zbuf[j][i + 1]) > zspan * 1.4)
        return;
    if (fabs(zbuf[j][i] - zbuf[j + 1][i]) > zspan * 1.4)
        return;
    for (k = 0; k < 4; k++) {
        int ii = i + (k == 1 || k == 2);
        int jj = j + (k >= 2);
        double x = xmin + (xmax - xmin) * ii / (GN - 1);
        double y = ymin + (ymax - ymin) * jj / (GN - 1);
        double dep;
        if (!project(x, y, zbuf[jj][ii], &p[k][0], &p[k][1], &dep))
            return;
        dsum += dep;
    }
    zn = (zbuf[j][i] + zbuf[j][i + 1] + zbuf[j + 1][i] + zbuf[j + 1][i + 1]) * 0.25;
    faces[nface].x[0] = p[0][0];
    faces[nface].y[0] = p[0][1];
    faces[nface].x[1] = p[1][0];
    faces[nface].y[1] = p[1][1];
    faces[nface].x[2] = p[2][0];
    faces[nface].y[2] = p[2][1];
    faces[nface].x[3] = p[3][0];
    faces[nface].y[3] = p[3][1];
    faces[nface].depth = dsum * 0.25;
    faces[nface].col = heat((zn - zmid) / zspan);
    nface++;
}

static void draw_gui(const char *status)
{
    char bar[128];
    fill_rect(0, 0, (int)W, 12, COL_DIM);
    text(2, 3, "pGraph", COL_HI);
    snprintf(bar, sizeof bar, " %s y%.0f", MODE_NAME[mode], yaw * 180 / M_PI);
    text((int)W - 6 * (int)strlen(bar) - 2, 3, bar, COL_TXT);

    fill_rect(0, (int)H - 22, (int)W, 22, COL_DIM);
    text(2, (int)H - 19, "z=", COL_HI);
    text(16, (int)H - 19, formula, COL_TXT);
    text(2, (int)H - 10, status, COL_TXT);
}

static void axis(double ax0, double ay0, double az0, double ax1, double ay1, double az1)
{
    int x0, y0, x1, y1;
    if (project(ax0, ay0, az0 * zspan + zmid, &x0, &y0, NULL) &&
        project(ax1, ay1, az1 * zspan + zmid, &x1, &y1, NULL))
        line(x0, y0, x1, y1, COL_AXIS);
}

static void render(const char *status)
{
    int i, j;
    clear(COL_BG);
    axis(-3.2, 0, 0, 3.2, 0, 0);
    axis(0, -3.2, 0, 0, 3.2, 0);
    axis(0, 0, -1.1, 0, 0, 1.1);

    if (mode == MODE_SOLID) {
        nface = 0;
        for (j = 0; j < GN - 1; j++)
            for (i = 0; i < GN - 1; i++)
                add_face(i, j);
        qsort(faces, (size_t)nface, sizeof(Face), cmp_face);
        for (i = 0; i < nface; i++) {
            tri(faces[i].x[0], faces[i].y[0], faces[i].x[1], faces[i].y[1],
                faces[i].x[2], faces[i].y[2], faces[i].col);
            tri(faces[i].x[0], faces[i].y[0], faces[i].x[2], faces[i].y[2],
                faces[i].x[3], faces[i].y[3], faces[i].col);
        }
    } else if (mode == MODE_DOTS) {
        for (j = 0; j < GN; j++)
            for (i = 0; i < GN; i++) {
                int sx, sy;
                double dep, zn;
                double x = xmin + (xmax - xmin) * i / (GN - 1);
                double y = ymin + (ymax - ymin) * j / (GN - 1);
                if (!zok[j][i])
                    continue;
                if (!project(x, y, zbuf[j][i], &sx, &sy, &dep))
                    continue;
                zn = (zbuf[j][i] - zmid) / zspan;
                px(sx, sy, heat(zn));
                px(sx + 1, sy, heat(zn));
                px(sx, sy + 1, heat(zn));
            }
    } else {
        nedge = 0;
        for (j = 0; j < GN; j++)
            for (i = 0; i < GN - 1; i++)
                add_edge(i, j, i + 1, j, COL_LINE);
        for (j = 0; j < GN - 1; j++)
            for (i = 0; i < GN; i++)
                add_edge(i, j, i, j + 1, COL_GRID);
        qsort(edges, (size_t)nedge, sizeof(Edge), cmp_edge);
        for (i = 0; i < nedge; i++)
            line(edges[i].x0, edges[i].y0, edges[i].x1, edges[i].y1, edges[i].col);
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
    char status[80] = "m mode  arrows  type  enter plot";
    int pre = 0;
    int editing = 0;
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
                    pitch -= 0.10;
                if (seq[1] == 'B')
                    pitch += 0.10;
                if (seq[1] == 'C')
                    yaw += 0.10;
                if (seq[1] == 'D')
                    yaw -= 0.10;
                if (pitch > 1.15)
                    pitch = 1.15;
                if (pitch < -0.15)
                    pitch = -0.15;
                snprintf(status, sizeof status, "rotate");
                render(status);
            } else
                running = 0;
            continue;
        }
        if (!editing && (ch == 'm' || ch == 'M')) {
            mode = (mode + 1) % MODE_N;
            snprintf(status, sizeof status, "mode %s", MODE_NAME[mode]);
            render(status);
        } else if (ch == 'q' || ch == 'Q') {
            if (editing) {
                size_t n = strlen(formula);
                if (n + 1 < sizeof formula) {
                    formula[n] = (char)ch;
                    formula[n + 1] = 0;
                }
                snprintf(status, sizeof status, "edit  enter to plot");
                render(status);
            } else
                running = 0;
        } else if (!editing && (ch == '+' || ch == '=')) {
            zoom *= 1.12;
            snprintf(status, sizeof status, "zoom in");
            render(status);
        } else if (!editing && (ch == '-' || ch == '_')) {
            zoom /= 1.12;
            snprintf(status, sizeof status, "zoom out");
            render(status);
        } else if (!editing && ch == '0') {
            yaw = 0.7;
            pitch = 0.45;
            zoom = 70;
            snprintf(status, sizeof status, "reset view");
            render(status);
        } else if (!editing && ch >= '1' && ch <= '6') {
            pre = ch - '1';
            snprintf(formula, sizeof formula, "%s", PRE[pre]);
            rebuild();
            snprintf(status, sizeof status, "preset %c", ch);
            render(status);
        } else if (ch == 8 || ch == 127) {
            size_t n = strlen(formula);
            if (n)
                formula[n - 1] = 0;
            editing = 1;
            snprintf(status, sizeof status, "edit");
            render(status);
        } else if (ch == 10 || ch == 13) {
            double dummy;
            editing = 0;
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
            editing = 1;
            snprintf(status, sizeof status, "edit  enter to plot");
            render(status);
        }
    }

    raw_term(0);
    munmap(map, maplen);
    close(fb);
    return 0;
}
