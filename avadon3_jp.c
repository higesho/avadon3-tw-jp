// Avadon 3: The Warborn - Japanese text rendering hook
//
// Built as a proxy dsound.dll (DirectSoundCreate is passed through to the system dsound.dll).
// On load it replaces the game's "draw text in rect" routine (VA 0x488780) and the
// string width routine (0x488600) with EUC-JP / UTF-8 aware versions. Ported from the
// Avadon: The Black Fortress patch (same engine, but Avadon 3.exe is an unoptimised
// build with plain cdecl calls). ASCII runs are still drawn with the game's own bitmap
// font; CJK characters are rasterised with GDI into extra textures and drawn
// through the game's own blit routine so clipping/colour/scaling match.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// ---------------------------------------------------------------- addresses
static BYTE *g_base;
#define VA(x) ((void *)(g_base + ((DWORD)(x) - 0x400000)))

// Every address the DLL uses is located at start-up with the byte patterns in
// resolve_addresses() (tools/gen_patterns.py makes them), so that other builds of the
// game (other stores) can work too. The values below are those of the Steam build;
// there each pattern is checked at that exact place first. They are kept as
// unrelocated VAs (0x4xxxxx); VA() adds the load base.
enum
{
    I_DRAW_TEXT, I_DRAW_ASCII, I_BLIT, I_NAME_SUBST, I_PEN_X, I_PEN_Y, I_CUR_FONT, I_LINE_ACC,
    I_STR_WIDTH, I_SET_COLOR, I_CHARMAP,
    I_FONT_TEX, I_BOUND_TEX_CACHE, I_FONT0, I_SPRINTF, I_NAMED_PUSH, I_NAMED_CALL, I_FMT_S_SPACE_S,
    I_TOKCLASS, I_TOK_DIGIT, I_TOK_OTHER, I_TOK_LETTER, I_COUNT
};
static const char *const kAddrName[I_COUNT] = {
    "draw_text", "draw_ascii", "blit", "name_subst", "pen_x", "pen_y", "cur_font", "line_acc",
    "str_width", "set_color", "charmap",
    "font_tex", "bound_tex_cache", "font0", "sprintf", "named_push", "named_call", "fmt_s_space_s",
    "tok_class", "tok_digit", "tok_other", "tok_letter"};
static const DWORD kSteamVa[I_COUNT] = {
    0x488780, 0x48c070, 0x409b30, 0x489c70, 0x6b52ec, 0x6b52f0, 0x6b52e8, 0x6b4edc,
    0x488600, 0x40a8a0, 0x5e9050,
    0x6b52e4, 0x5e809c, 0x5e80f8, 0x4caaf5, 0x483ddc, 0x483de8, 0x5cb85c,
    0x4727a9, 0x4727ae, 0x4727cb, 0x4727ed};
static DWORD g_va[I_COUNT]; // found addresses (0 = not found)

#define A_DRAW_TEXT_RECT g_va[I_DRAW_TEXT] /* replaced: cdecl (port, rect, str, mode, font code, r, g, b) */
#define A_DRAW_ASCII g_va[I_DRAW_ASCII]    /* cdecl (port, char*, r, g, b), pen at globals */
#define A_BLIT g_va[I_BLIT]                /* cdecl (texobj, Rect src, 0, Rect dst, 1, 0, 0) */
#define A_NAME_SUBST g_va[I_NAME_SUBST]    /* cdecl (buffer): replaces "[N]" with the leader's name */
#define A_STR_WIDTH g_va[I_STR_WIDTH]      /* cdecl (str), returns ax = width in font 0; replaced */
#define A_SET_COLOR g_va[I_SET_COLOR]      /* cdecl (r - 31, g - 31, b - 31): tint of the next blits */
#define A_PEN_X g_va[I_PEN_X]
#define A_PEN_Y g_va[I_PEN_Y]
#define A_CUR_FONT g_va[I_CUR_FONT]
#define A_LINE_ACC g_va[I_LINE_ACC]
#define A_CHARMAP g_va[I_CHARMAP]
#define A_FONT_TEX g_va[I_FONT_TEX]
#define A_BOUND_TEX_CACHE g_va[I_BOUND_TEX_CACHE]
#define A_FONT0 g_va[I_FONT0] /* five fonts, FONT_STRIDE apart */
#define FONT_STRIDE 0x310

#define TEXOBJ_SIZE 0x170
#define TEXOBJ_GLID 0x2c
#define TEXOBJ_PIXELS 0x134
#define TEXOBJ_W 0x13c
#define TEXOBJ_H 0x140
#define TEXOBJ_TARGET 0x144

static const BYTE kExpectedPrologue[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x80, 0x08, 0x00, 0x00};
static const BYTE kWidthPrologue[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x30, 0x04, 0x00, 0x00};

// ---------------------------------------------------------------- logging
static FILE *g_log;
static void jp_log(const char *fmt, ...)
{
    va_list ap;
    if (!g_log)
        return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ---------------------------------------------------------------- config
static char g_fontFace[64] = "Meiryo";
static int g_fontWeight = FW_BOLD;
static float g_alphaGamma = 0.7f; // <1 = darker/thicker strokes
static int g_sizeAdjust = 0;   // px added to CJK em size
static int g_yAdjust = 0;      // px added to CJK vertical placement
static int g_cjkSpacing = 0;   // extra px between CJK glyphs
static int g_eucjp = 1;        // text encoding of game files: 1 = EUC-JP, 0 = UTF-8
static int g_lineGap = 1;      // min px between the CJK em box and the next line

static void load_config(const char *dir)
{
    char ini[MAX_PATH], enc[16];
    _snprintf(ini, sizeof ini, "%s\\avadon3_jp.ini", dir);
    GetPrivateProfileStringA("text", "encoding", "eucjp", enc, sizeof enc, ini);
    g_eucjp = _stricmp(enc, "utf8") != 0 && _stricmp(enc, "utf-8") != 0;
    GetPrivateProfileStringA("font", "face", g_fontFace, g_fontFace, sizeof g_fontFace, ini);
    g_fontWeight = GetPrivateProfileIntA("font", "weight", g_fontWeight, ini);
    g_sizeAdjust = GetPrivateProfileIntA("font", "size_adjust", g_sizeAdjust, ini);
    g_yAdjust = GetPrivateProfileIntA("font", "y_adjust", g_yAdjust, ini);
    g_cjkSpacing = GetPrivateProfileIntA("font", "spacing", g_cjkSpacing, ini);
    g_lineGap = GetPrivateProfileIntA("font", "line_gap", g_lineGap, ini);
    {
        char buf[32];
        GetPrivateProfileStringA("font", "alpha_gamma", "0.7", buf, sizeof buf, ini);
        g_alphaGamma = (float)atof(buf);
        if (g_alphaGamma <= 0.05f)
            g_alphaGamma = 1.0f;
    }
}

// ---------------------------------------------------------------- game font helpers
static short fs(const BYTE *font, int off) { return *(const short *)(font + off); }
static int font_index(const BYTE *font)
{
    int i = (int)((DWORD)font - (DWORD)VA(A_FONT0)) / FONT_STRIDE;
    return (i < 0 || i > 4) ? 0 : i;
}
static int glyph_of(unsigned char c) { return ((short *)VA(A_CHARMAP))[c]; }
static int box_h(const BYTE *font) { return fs(font, 8) - fs(font, 4); }

// Width of an ASCII run, mirroring the game's own position routine (0x48c340).
static int ascii_width(const BYTE *font, const unsigned char *s, int n)
{
    int pos = 0, i;
    for (i = 0; i < n; i++)
    {
        int g = glyph_of(s[i]);
        if (i > 0)
        {
            int gp = glyph_of(s[i - 1]);
            short kc = fs(font, 0xc + g * 8 + 6), kp = fs(font, 0xc + gp * 8 + 4);
            if (kc > 0 && kp > 0)
                pos -= kp;
            else
                pos += fs(font, 0xa);
        }
        pos += fs(font, 0xc + g * 8 + 2) - fs(font, 0xc + g * 8 + 0);
    }
    return pos;
}

// ---------------------------------------------------------------- CJK glyph cache
typedef struct
{
    DWORD key; // (fontIdx << 24) | codepoint, 0 = empty
    unsigned char page;
    short x, y, w, h, adv;
} CGlyph;

#define CACHE_SIZE 16384
#define MAX_PAGES 32
static CGlyph g_cache[CACHE_SIZE];
static BYTE *g_pages[MAX_PAGES]; // cloned texture objects
static int g_pageCount;
static int g_texW, g_texH;
static GLenum g_target;
static int g_curX, g_curY, g_shelfH = 32;
static HDC g_dc;
static HBITMAP g_dib;
static DWORD *g_dibBits;
#define DIB_W 128
#define DIB_H 64
static HFONT g_hfont[5];
static BYTE g_alphaLut[256];
static int g_emTop[5];

static BYTE *new_page(void)
{
    BYTE *fontObj = *(BYTE **)VA(A_FONT_TEX);
    BYTE *clone;
    GLuint id;
    void *zeros;
    if (!fontObj || g_pageCount >= MAX_PAGES)
        return NULL;
    if (g_pageCount == 0)
    {
        g_texW = (int)*(float *)(fontObj + TEXOBJ_W);
        g_texH = (int)*(float *)(fontObj + TEXOBJ_H);
        g_target = *(GLenum *)(fontObj + TEXOBJ_TARGET);
        jp_log("font texobj=%p glid=%u size=%dx%d target=0x%x\n", fontObj,
             *(DWORD *)(fontObj + TEXOBJ_GLID), g_texW, g_texH, g_target);
        if (g_texW < 64 || g_texH < 64 || g_texW > 4096 || g_texH > 4096)
            return NULL;
    }
    clone = (BYTE *)malloc(TEXOBJ_SIZE);
    memcpy(clone, fontObj, TEXOBJ_SIZE);
    *(DWORD *)(clone + TEXOBJ_PIXELS) = 0;

    glGenTextures(1, &id);
    glBindTexture(g_target, id);
    *(DWORD *)VA(A_BOUND_TEX_CACHE) = id;
    glTexParameteri(g_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(g_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(g_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(g_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    zeros = calloc((size_t)g_texW * g_texH, 4);
    glTexImage2D(g_target, 0, GL_RGBA, g_texW, g_texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros);
    free(zeros);
    *(DWORD *)(clone + TEXOBJ_GLID) = id;
    g_pages[g_pageCount++] = clone;
    g_curX = 1;
    g_curY = 1;
    jp_log("new glyph page %d glid=%u\n", g_pageCount - 1, id);
    return clone;
}

static void ensure_gdi(void)
{
    BITMAPINFO bi;
    int i;
    if (g_dc)
        return;
    for (i = 0; i < 256; i++)
        g_alphaLut[i] = (BYTE)(255.0 * pow(i / 255.0, g_alphaGamma) + 0.5);
    g_dc = CreateCompatibleDC(NULL);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = DIB_W;
    bi.bmiHeader.biHeight = -DIB_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    g_dib = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, (void **)&g_dibBits, NULL, 0);
    SelectObject(g_dc, g_dib);
    SetTextColor(g_dc, RGB(255, 255, 255));
    SetBkColor(g_dc, RGB(0, 0, 0));
    SetBkMode(g_dc, OPAQUE);
}

static HFONT get_hfont(int fi, const BYTE *font)
{
    if (!g_hfont[fi])
    {
        // Line advance used by my_draw_text for each font slot (code + 1).
        static const int kLineH[5] = {13, 16, 15, 17, 25};
        TEXTMETRICW tm;
        int bh = box_h(font), em = bh, shift;
        // The ASCII cell (box_h) can be taller than the line advance; a CJK glyph
        // fills its em box, so cap it or consecutive lines overlap.
        if (em > kLineH[fi] - g_lineGap)
            em = kLineH[fi] - g_lineGap;
        em += g_sizeAdjust;
        if (em < 8)
            em = 8;
        shift = bh > em ? (bh - em) / 2 : 0; // centre the smaller em box in the cell
        g_hfont[fi] = CreateFontA(-em, 0, 0, 0, g_fontWeight, 0, 0, 0, SHIFTJIS_CHARSET,
                                  OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                  DEFAULT_PITCH, g_fontFace);
        SelectObject(g_dc, g_hfont[fi]);
        GetTextMetricsW(g_dc, &tm);
        // Place the ideographic em box (approx. 0.88em above baseline) at the top of the cell.
        g_emTop[fi] = tm.tmAscent - (int)(em * 0.88 + 0.5) - shift;
        jp_log("font %d: box=%d em=%d ascent=%d descent=%d emTop=%d\n", fi, bh, em, tm.tmAscent, tm.tmDescent, g_emTop[fi]);
    }
    return g_hfont[fi];
}

static CGlyph *get_glyph(const BYTE *font, DWORD cp)
{
    int fi = font_index(font);
    DWORD key = ((DWORD)fi << 24) | cp;
    DWORD h = (cp * 2654435761u ^ (DWORD)fi * 40503u) & (CACHE_SIZE - 1);
    CGlyph *g;
    WCHAR wc[2];
    int wn, bh, adv, x, y;
    SIZE sz;
    DWORD *rgba;
    GLint oldRow, oldAlign;

    for (;;)
    {
        g = &g_cache[h];
        if (g->key == key)
            return g;
        if (g->key == 0)
            break;
        h = (h + 1) & (CACHE_SIZE - 1);
    }

    ensure_gdi();
    SelectObject(g_dc, get_hfont(fi, font));
    if (cp >= 0x10000)
    {
        wc[0] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10));
        wc[1] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        wn = 2;
    }
    else
    {
        wc[0] = (WCHAR)cp;
        wn = 1;
    }
    GetTextExtentPoint32W(g_dc, wc, wn, &sz);
    adv = sz.cx;
    if (adv > DIB_W - 2)
        adv = DIB_W - 2;
    bh = box_h(font);

    memset(g_dibBits, 0, DIB_W * DIB_H * 4);
    TextOutW(g_dc, 0, -g_emTop[fi] + g_yAdjust, wc, wn);
    GdiFlush();

    if (g_pageCount == 0 || g_curX + adv + 2 > g_texW)
    {
        g_curX = 1;
        g_curY += g_shelfH;
    }
    if (g_pageCount == 0 || g_curY + g_shelfH > g_texH)
    {
        if (!new_page())
            return NULL;
    }

    rgba = (DWORD *)malloc((size_t)adv * bh * 4 + 4);
    for (y = 0; y < bh; y++)
        for (x = 0; x < adv; x++)
        {
            DWORD p = (y < DIB_H) ? g_dibBits[y * DIB_W + x] : 0;
            DWORD a = ((p & 0xff) + ((p >> 8) & 0xff) + ((p >> 16) & 0xff)) / 3;
            a = g_alphaLut[a];
            rgba[y * adv + x] = (a << 24) | 0x00ffffff;
        }
    glBindTexture(g_target, *(DWORD *)(g_pages[g_pageCount - 1] + TEXOBJ_GLID));
    *(DWORD *)VA(A_BOUND_TEX_CACHE) = *(DWORD *)(g_pages[g_pageCount - 1] + TEXOBJ_GLID);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldRow);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlign);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(g_target, 0, g_curX, g_curY, adv, bh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldRow);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlign);
    free(rgba);

    g->key = key;
    g->page = (unsigned char)(g_pageCount - 1);
    g->x = (short)g_curX;
    g->y = (short)g_curY;
    g->w = (short)adv;
    g->h = (short)bh;
    g->adv = (short)(adv + g_cjkSpacing);
    g_curX += adv + 2;
    return g;
}

// ---------------------------------------------------------------- calls into the game
typedef void(__cdecl *BlitFn)(void *, DWORD, DWORD, int, DWORD, DWORD, int, int, int);
typedef void(__cdecl *SetColorFn)(int, int, int);
// Rect {top, left, bottom, right} passed by value as two dwords.
static void call_blit(void *obj, DWORD src1, DWORD src2, DWORD dst1, DWORD dst2)
{
    ((BlitFn)VA(A_BLIT))(obj, src1, src2, 0, dst1, dst2, 1, 0, 0);
}

// "[N]" -> the leader's name, in place (the game does this before drawing). The game's
// routine uses a 0x400-byte work buffer without bounds checks, so only short strings go in.
typedef void(__cdecl *NameSubstFn)(char *);
static void call_name_subst(char *buf)
{
    ((NameSubstFn)VA(A_NAME_SUBST))(buf);
}

typedef void(__cdecl *DrawAsciiFn)(void *, const char *, int, int, int);
static void *g_port; // first argument of the game's draw_text, passed on to draw_ascii

// ---------------------------------------------------------------- text units
typedef struct
{
    DWORD cp;     // codepoint (ASCII for single bytes)
    short x;      // x offset from start of the text (for wrapping)
    short w;      // advance
    BYTE ascii;   // 1 = draw with game font
} Unit;

static unsigned char mac_to_ascii(unsigned char c)
{
    switch (c)
    {
    case 0xc4: case 0xd0: return '-';
    case 0xd2: case 0xd3: return '"';
    case 0xd4: case 0xd5: return '\'';
    }
    return c > 0x7f ? ' ' : c;
}

// EUC-JP pair -> Unicode (0 if not a valid pair). Both bytes are >= 0x8E, so
// they never collide with ASCII control characters the game uses ('_', '|', ...).
static DWORD euc_pair(const unsigned char *s)
{
    WCHAR wc = 0;
    unsigned char c = s[0], d = s[1];
    if (!((c >= 0xA1 && c <= 0xFE && d >= 0xA1 && d <= 0xFE) || (c == 0x8E && d >= 0xA1 && d <= 0xDF)))
        return 0;
    if (MultiByteToWideChar(20932, MB_ERR_INVALID_CHARS, (const char *)s, 2, &wc, 1) != 1)
        return 0;
    return wc;
}

// Decode EUC-JP or UTF-8 (per config), falling back to the game's MacRoman
// cleanup for bytes that are not part of a valid sequence.
static int decode(const unsigned char *s, Unit *u, int max)
{
    int n = 0;
    while (*s && n < max)
    {
        DWORD cp;
        int len = 0;
        unsigned char c = *s;
        if (c < 0x80)
            cp = c, len = 1;
        else if (g_eucjp)
        {
            cp = euc_pair(s);
            if (cp)
                len = 2;
            else
                cp = mac_to_ascii(c), len = 1;
        }
        else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80)
            cp = ((c & 0x1F) << 6) | (s[1] & 0x3F), len = 2;
        else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
            cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F), len = 3;
        else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80)
            cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F), len = 4;
        else
            cp = mac_to_ascii(c), len = 1;
        if (cp >= 0x80 && cp < 0x100)
            cp = mac_to_ascii((unsigned char)cp); // Latin-1 range: not in our fonts
        u[n].cp = cp;
        u[n].ascii = cp < 0x80;
        n++;
        s += len;
    }
    return n;
}

static int is_cjk(DWORD cp) { return cp >= 0x2E80; }

// Kinsoku: characters that must not start a line / must not end a line.
static int no_line_start(DWORD cp)
{
    static const WCHAR *s = L"、。，．・：；？！ー―‐～…‥）〕］｝〉》」』】’”ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ々ゝゞヽヾ!?),.:;";
    const WCHAR *p;
    for (p = s; *p; p++)
        if (*p == cp)
            return 1;
    return 0;
}
static int no_line_end(DWORD cp)
{
    static const WCHAR *s = L"（〔［｛〈《「『【‘“(";
    const WCHAR *p;
    for (p = s; *p; p++)
        if (*p == cp)
            return 1;
    return 0;
}

// Can we break the line before unit i?
static int can_break_before(Unit *u, int i)
{
    DWORD a = u[i - 1].cp, b = u[i].cp;
    if (a == ' ')
        return 1;
    if (!(is_cjk(a) || is_cjk(b)))
        return 0;
    if (no_line_start(b) || no_line_end(a))
        return 0;
    return 1;
}

static void measure(const BYTE *font, Unit *u, int n)
{
    int i, x = 0;
    for (i = 0; i < n; i++)
    {
        u[i].x = (short)x;
        if (u[i].ascii)
        {
            unsigned char c = (unsigned char)u[i].cp;
            u[i].w = (short)ascii_width(font, &c, 1) + fs(font, 0xa);
        }
        else
        {
            CGlyph *g = get_glyph(font, u[i].cp);
            u[i].w = g ? g->adv : 0;
        }
        x += u[i].w;
    }
}

static void draw_line(const BYTE *font, Unit *u, int n, int x, int y, int r, int g, int b)
{
    DrawAsciiFn drawAscii = (DrawAsciiFn)VA(A_DRAW_ASCII);
    char buf[1024];
    int i = 0;
    int bh = box_h(font);
    while (i < n)
    {
        if (u[i].ascii)
        {
            int k = 0;
            while (i < n && u[i].ascii && k < (int)sizeof buf - 1)
                buf[k++] = (char)u[i++].cp;
            buf[k] = 0;
            *(short *)VA(A_PEN_X) = (short)x;
            *(short *)VA(A_PEN_Y) = (short)y;
            drawAscii(g_port, buf, r, g, b);
            x += ascii_width(font, (unsigned char *)buf, k) + fs(font, 0xa);
        }
        else
        {
            CGlyph *gl = get_glyph(font, u[i].cp);
            if (gl && u[i].cp != 0x3000)
            {
                DWORD src1 = ((DWORD)(WORD)gl->x << 16) | (WORD)gl->y;
                DWORD src2 = ((DWORD)(WORD)(gl->x + gl->w) << 16) | (WORD)(gl->y + bh);
                DWORD dst1 = ((DWORD)(WORD)x << 16) | (WORD)y;
                DWORD dst2 = ((DWORD)(WORD)(x + gl->w) << 16) | (WORD)(y + bh);
                ((SetColorFn)VA(A_SET_COLOR))(r - 31, g - 31, b - 31);
                call_blit(g_pages[gl->page], src1, src2, dst1, dst2);
                ((SetColorFn)VA(A_SET_COLOR))(0, 0, 0);
            }
            x += gl ? gl->adv : 0;
            i++;
        }
    }
}

// ---------------------------------------------------------------- replacement
#define MAX_UNITS 4096
static Unit g_units[MAX_UNITS];

static int g_trace; // [debug] trace=N: log the next N draw_text calls

static void __cdecl my_draw_text(void *port, DWORD ra, DWORD rb, const char *str, int mode, int code, int r, int g, int b)
{
    short rect[4]; // top, left, bottom, right
    const BYTE *font;
    int lh, n, i;
    Unit *u = g_units;
    static char subst[0x400];

    g_port = port;
    mode = (short)mode;
    code = (short)code;
    r = (short)r;
    g = (short)g;
    b = (short)b;
    rect[0] = (short)(ra & 0xffff);
    rect[1] = (short)(ra >> 16);
    rect[2] = (short)(rb & 0xffff);
    rect[3] = (short)(rb >> 16);

    if (g_trace > 0 && str)
    {
        // log each distinct text once (screens redraw every frame)
        static DWORD seen[8192];
        DWORD h = 2166136261u, slot;
        const unsigned char *q;
        for (q = (const unsigned char *)str; *q; q++)
            h = (h ^ *q) * 16777619u;
        h ^= (DWORD)mode * 31u + (DWORD)code;
        if (!h)
            h = 1;
        slot = h & 8191;
        while (seen[slot] && seen[slot] != h)
            slot = (slot + 1) & 8191;
        if (seen[slot] == h)
            goto traced;
        seen[slot] = h;
        g_trace--;
        jp_log("draw_text port=%p rect=%d,%d,%d,%d mode=%d code=%d rgb=%d,%d,%d len=%u \"%.48s\"\n", port, rect[0],
               rect[1], rect[2], rect[3], mode, code, r, g, b, (unsigned)strlen(str), str);
    }
traced:
    if (!str || !*str)
        return;
    if (strstr(str, "[N]") && strlen(str) < 0x300)
    {
        strcpy(subst, str);
        call_name_subst(subst);
        str = subst;
    }

    switch (code)
    {
    case 14: font = (BYTE *)VA(A_FONT0 + 2 * FONT_STRIDE); break;
    case 15: font = (BYTE *)VA(A_FONT0 + 1 * FONT_STRIDE); break;
    case 16: case 18: font = (BYTE *)VA(A_FONT0 + 3 * FONT_STRIDE); code = 16; break;
    case 20: case 22: case 24: font = (BYTE *)VA(A_FONT0 + 4 * FONT_STRIDE); code = 24; break;
    default: font = (BYTE *)VA(A_FONT0); code = 12; break;
    }
    lh = code + 1;
    *(BYTE **)VA(A_CUR_FONT) = (BYTE *)font;
    *(short *)VA(A_LINE_ACC) = (short)lh;

    n = decode((const unsigned char *)str, u, MAX_UNITS);
    if (mode == 2)
        for (i = 0; i < n; i++)
            if (u[i].cp == '|')
            {
                mode = 0;
                break;
            }
    measure(font, u, n);

    if (mode == 1)
    {
        int w = n ? u[n - 1].x + u[n - 1].w : 0;
        int x = (rect[1] + rect[3]) / 2 - w / 2 + 2;
        int y = (rect[2] - lh + rect[0]) / 2 + 2;
        draw_line(font, u, n, x, y, r, g, b);
    }
    else if (mode == 2)
    {
        draw_line(font, u, n, rect[1] + 2, rect[0] + 2, r, g, b);
    }
    else if (mode == 0)
    {
        int maxw = rect[3] - rect[1] - 6;
        int start = 0, y = rect[0] + 2, first = 1;
        while (start < n)
        {
            int lastBreak = -1, end = n, next = n;
            while (start < n && u[start].cp == ' ' && !first)
                start++;
            first = 0;
            for (i = start; i < n; i++)
            {
                if (u[i].cp == '|')
                {
                    end = i;
                    next = i + 1;
                    break;
                }
                if (i > start && can_break_before(u, i))
                    lastBreak = i;
                if (u[i].x + u[i].w - u[start].x > maxw && i > start)
                {
                    if (lastBreak > start)
                        end = next = lastBreak;
                    else
                        end = next = i; // force break
                    break;
                }
            }
            // trim trailing spaces
            {
                int e = end;
                while (e > start && u[e - 1].cp == ' ')
                    e--;
                if (e > start)
                {
                    // re-measure x relative to line start
                    draw_line(font, u + start, e - start, rect[1] + 2, y, r, g, b);
                }
            }
            if (next >= n)
                break;
            y += lh;
            *(short *)VA(A_LINE_ACC) = (short)(*(short *)VA(A_LINE_ACC) + lh);
            start = next;
        }
    }
}

// Avadon 3's draw_text (0x488780) is a plain cdecl function with the same arguments, so
// the patch jumps straight to my_draw_text.

// String width in font 0 (0x488600, cdecl (str), result in ax). Like the original it
// selects font 0 and returns the x of the last character.
static int __cdecl my_str_width(const char *str)
{
    const BYTE *font = (BYTE *)VA(A_FONT0);
    Unit *u = g_units;
    int n;
    *(BYTE **)VA(A_CUR_FONT) = (BYTE *)font;
    if (!str || !*str)
        return 0;
    n = decode((const unsigned char *)str, u, MAX_UNITS);
    measure(font, u, n);
    if (!n)
        return 0;
    return u[n - 1].ascii ? u[n - 1].x : u[n - 1].x + u[n - 1].w;
}

static void jmp_patch(BYTE *target, void *to)
{
    DWORD old;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(DWORD *)(target + 1) = (DWORD)to - ((DWORD)target + 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
}

static void install(void)
{
    BYTE *target;
    if (!A_DRAW_TEXT_RECT)
        return;
    target = (BYTE *)VA(A_DRAW_TEXT_RECT);
    if (memcmp(target, kExpectedPrologue, sizeof kExpectedPrologue) != 0)
    {
        jp_log("unexpected code at draw_text_rect; game version mismatch, hook NOT installed\n");
        return;
    }
    jmp_patch(target, (void *)my_draw_text);
    jp_log("hook installed at %p\n", target);
    target = (BYTE *)VA(A_STR_WIDTH);
    if (A_STR_WIDTH && memcmp(target, kWidthPrologue, sizeof kWidthPrologue) == 0)
    {
        jmp_patch(target, (void *)my_str_width);
        jp_log("width hook installed at %p\n", target);
    }
    else
        jp_log("width hook NOT installed\n");
}

// ---------------------------------------------------------------- exe string replacement
//
// avadon3_jp_exe.tsv (UTF-8, tab separated): va, sink, refs, english, japanese.
// For every row with a translation we check that the exe really has the English
// string at that address, then rewrite every relocated pointer to it (code
// immediates and data tables, found via the exe's base relocation table) so it
// points at the translated string instead. The exe file itself is untouched.

#define A_SPRINTF g_va[I_SPRINTF]
static const BYTE kSprintfPrologue[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};

typedef struct
{
    DWORD oldPtr;
    DWORD newPtr;
} StrMap;
#define STRMAP_SIZE 4096
static StrMap g_strmap[STRMAP_SIZE];
static int g_strmapCount;

static void strmap_put(DWORD oldPtr, DWORD newPtr)
{
    DWORD h = (oldPtr * 2654435761u) & (STRMAP_SIZE - 1);
    while (g_strmap[h].oldPtr && g_strmap[h].oldPtr != oldPtr)
        h = (h + 1) & (STRMAP_SIZE - 1);
    if (!g_strmap[h].oldPtr)
        g_strmapCount++;
    g_strmap[h].oldPtr = oldPtr;
    g_strmap[h].newPtr = newPtr;
}
static DWORD strmap_get(DWORD oldPtr)
{
    DWORD h = (oldPtr * 2654435761u) & (STRMAP_SIZE - 1);
    while (g_strmap[h].oldPtr)
    {
        if (g_strmap[h].oldPtr == oldPtr)
            return g_strmap[h].newPtr;
        h = (h + 1) & (STRMAP_SIZE - 1);
    }
    return 0;
}

// In-place unescape of \n \r \t \\ (as written by the exporter).
static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++)
    {
        if (*s == '\\' && s[1])
        {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s == 't' ? '\t' : *s;
        }
        else
            *o++ = *s;
    }
    *o = 0;
}

// UTF-8 -> game encoding (EUC-JP or UTF-8). Returns malloc'd string.
static char *to_game_encoding(const char *utf8, int *unmappable)
{
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0), i, n;
    WCHAR *w;
    char *out;
    BOOL usedDefault = FALSE;
    *unmappable = 0;
    if (!g_eucjp)
        return _strdup(utf8);
    w = (WCHAR *)malloc(wn * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wn);
    for (i = 0; i < wn; i++) // same normalisation as tools/jpconv.py
        switch (w[i])
        {
        case 0xFF5E: w[i] = 0x301C; break;
        case 0xFF0D: w[i] = 0x2212; break;
        case 0x2225: w[i] = 0x2016; break;
        }
    n = WideCharToMultiByte(20932, 0, w, wn, NULL, 0, NULL, NULL);
    out = (char *)malloc(n + 1);
    WideCharToMultiByte(20932, 0, w, wn, out, n, "?", &usedDefault);
    out[n] = 0;
    *unmappable = usedDefault;
    free(w);
    return out;
}

static int g_needPositional;
static int g_forceScan; // [debug] force_scan=1: search everything as for another build (tests the search)

static int __cdecl my_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    if (strchr(fmt, '$'))
        r = _vsprintf_p(buf, 65536, fmt, ap); // supports %1$s style reordering
    else
        r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

static void install_sprintf_hook(void)
{
    BYTE *target;
    DWORD old;
    if (!A_SPRINTF)
    {
        jp_log("sprintf not found; positional formats NOT supported\n");
        return;
    }
    target = (BYTE *)VA(A_SPRINTF);
    if (memcmp(target, kSprintfPrologue, sizeof kSprintfPrologue) != 0)
    {
        jp_log("unexpected code at sprintf; positional formats NOT supported\n");
        return;
    }
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(DWORD *)(target + 1) = (DWORD)my_sprintf - ((DWORD)target + 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    {
        char t[64];
        my_sprintf(t, "%2$s-%1$d-%3$s", 7, "b", "c");
        jp_log("sprintf hook installed (positional arguments enabled), self-test \"%s\" (expect b-7-c)\n", t);
    }
}

static int is_positional(const char *s)
{
    for (; *s; s++)
        if (s[0] == '%' && s[1] >= '1' && s[1] <= '9' && s[2] == '$')
            return 1;
    return 0;
}

// Other builds keep the same strings at other addresses. A string is identified by its
// text and by how many relocated pointers refer to it (the "refs" column, counted in
// the Steam build), so short texts that also occur as plain data are not mistaken.
static DWORD *g_relocTargets; // values of all relocated pointers, sorted
static int g_relocCount;

static int cmp_dword(const void *a, const void *b)
{
    DWORD x = *(const DWORD *)a, y = *(const DWORD *)b;
    return x < y ? -1 : x > y;
}

static void build_reloc_targets(void)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *rd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    BYTE *rel = g_base + rd->VirtualAddress, *end = rel + rd->Size;
    int cap = rd->Size / 2 + 1;
    g_relocTargets = (DWORD *)malloc(cap * sizeof(DWORD));
    g_relocCount = 0;
    while (g_relocTargets && rd->VirtualAddress && rel < end)
    {
        IMAGE_BASE_RELOCATION *blk = (IMAGE_BASE_RELOCATION *)rel;
        WORD *e = (WORD *)(blk + 1);
        int n = (blk->SizeOfBlock - sizeof *blk) / 2, i;
        if (blk->SizeOfBlock < sizeof *blk)
            break;
        for (i = 0; i < n && g_relocCount < cap; i++)
            if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW)
                g_relocTargets[g_relocCount++] = *(DWORD *)(g_base + blk->VirtualAddress + (e[i] & 0xfff));
        rel += blk->SizeOfBlock;
    }
    if (g_relocTargets)
        qsort(g_relocTargets, g_relocCount, sizeof(DWORD), cmp_dword);
}

static int reloc_refs(DWORD ptr)
{
    int lo = 0, hi = g_relocCount, n = 0;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (g_relocTargets[mid] < ptr)
            lo = mid + 1;
        else
            hi = mid;
    }
    while (lo + n < g_relocCount && g_relocTargets[lo + n] == ptr)
        n++;
    return n;
}

// Returns the RVA of a whole NUL-terminated occurrence of s in the data sections with
// exactly `refs` references (refs < 0: any number above zero) that is not in used[].
static DWORD find_string_rva(const char *s, int refs, const DWORD *used, int nUsed)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    size_t len = strlen(s);
    int i, j, r;
    if (!len)
        return 0;
    if (!g_relocTargets)
        build_reloc_targets();
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        BYTE *a, *p, *end;
        if ((sec[i].Characteristics & IMAGE_SCN_CNT_CODE) || !(sec[i].Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA))
            continue;
        a = g_base + sec[i].VirtualAddress;
        end = a + min(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
        for (p = a; p + len < end; p++)
            if (*p == (BYTE)s[0] && (p == a || p[-1] == 0) && p[len] == 0 && memcmp(p, s, len) == 0)
            {
                DWORD rva = (DWORD)(p - g_base);
                r = reloc_refs((DWORD)p);
                for (j = 0; j < nUsed && used[j] != rva; j++)
                    ;
                if (j == nUsed && (refs < 0 ? r > 0 : r == refs))
                    return rva;
                p += len;
            }
    }
    return 0;
}

static void load_exe_strings(const char *dir)
{
    DWORD *used = (DWORD *)calloc(4096, sizeof(DWORD)); // RVAs of strings already taken
    int nUsed = 0, foundBySearch = 0;
    char path[MAX_PATH];
    FILE *f;
    long size;
    char *buf, *line, *next;
    int lineno = 0, ok = 0, skipped = 0, inplace = 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    DWORD imageSize = nt->OptionalHeader.SizeOfImage;

    _snprintf(path, sizeof path, "%s\\avadon3_jp_exe.tsv", dir);
    f = fopen(path, "rb");
    if (!f)
    {
        jp_log("no %s (exe strings stay English)\n", path);
        free(used);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = 0;
    fclose(f);

    for (line = buf; line && *line; line = next)
    {
        char *col[5] = {0};
        char *p;
        int c = 0, unmappable;
        DWORD va, rva;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        lineno++;
        if ((p = strchr(line, '\r')) != NULL)
            *p = 0;
        if (lineno == 1 && (strncmp(line, "va\t", 3) == 0 || strncmp(line, "\xEF\xBB\xBFva\t", 6) == 0))
            continue;
        col[0] = line;
        for (p = line; *p && c < 4; p++)
            if (*p == '\t')
            {
                *p = 0;
                col[++c] = p + 1;
            }
        if (c < 4 || !col[4][0])
            continue; // untranslated
        unescape(col[3]);
        unescape(col[4]);
        va = strtoul(col[0], NULL, 16);
        rva = va - 0x400000;
        if (g_forceScan || va < 0x400000 || rva >= imageSize || strcmp((char *)g_base + rva, col[3]) != 0)
        {
            // not at the Steam build's address: look for the same text elsewhere
            char *e;
            long refs = strtol(col[2], &e, 10);
            rva = find_string_rva(col[3], (e != col[2] && refs > 0) ? (int)refs : -1, used, nUsed);
            if (!rva)
            {
                jp_log("exe_tsv line %d: English text not found in this exe (%s), skipped\n", lineno, col[0]);
                skipped++;
                continue;
            }
            if (rva != va - 0x400000)
                foundBySearch++;
        }
        if (nUsed < 4096)
            used[nUsed++] = rva;
        {
            char *jp = to_game_encoding(col[4], &unmappable);
            if (unmappable)
                jp_log("exe_tsv line %d: characters not in EUC-JP were replaced with '?'\n", lineno);
            if (strstr(col[1], "float") && strlen(jp) > 39)
                jp_log("exe_tsv line %d: floating text is %u bytes (max 39), it will not be shown\n", lineno, (unsigned)strlen(jp));
            if (is_positional(jp))
                g_needPositional = 1;
            if (strncmp(col[1], "inplace", 7) == 0)
            {
                // a fixed-size char array in .data (no pointer to redirect): write in place
                int field = atoi(col[1] + 7);
                DWORD old;
                BYTE *dst = g_base + rva;
                if (field > 0 && (int)strlen(jp) < field)
                {
                    VirtualProtect(dst, field, PAGE_READWRITE, &old);
                    memset(dst, 0, field);
                    memcpy(dst, jp, strlen(jp));
                    VirtualProtect(dst, field, old, &old);
                    inplace++;
                }
                else
                    jp_log("exe_tsv line %d: too long for its %d-byte field, skipped\n", lineno, field);
                free(jp);
                ok++;
                continue;
            }
            strmap_put((DWORD)(g_base + rva), (DWORD)jp);
            ok++;
        }
    }
    free(buf);
    free(used);
    free(g_relocTargets);
    g_relocTargets = NULL;
    if (foundBySearch)
        jp_log("exe strings: %d found at other addresses than in the Steam build\n", foundBySearch);

    // Walk the exe's base relocations and redirect every pointer to a translated string.
    {
        IMAGE_DATA_DIRECTORY *rd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        BYTE *rel = g_base + rd->VirtualAddress, *end = rel + rd->Size;
        int patched = 0;
        while (rd->VirtualAddress && rel < end)
        {
            IMAGE_BASE_RELOCATION *blk = (IMAGE_BASE_RELOCATION *)rel;
            WORD *e = (WORD *)(blk + 1);
            int n = (blk->SizeOfBlock - sizeof *blk) / 2, i;
            if (blk->SizeOfBlock < sizeof *blk)
                break;
            for (i = 0; i < n; i++)
            {
                if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW)
                {
                    DWORD *site = (DWORD *)(g_base + blk->VirtualAddress + (e[i] & 0xfff));
                    DWORD np = strmap_get(*site);
                    if (np)
                    {
                        DWORD old;
                        VirtualProtect(site, 4, PAGE_EXECUTE_READWRITE, &old);
                        *site = np;
                        VirtualProtect(site, 4, old, &old);
                        patched++;
                    }
                }
            }
            rel += blk->SizeOfBlock;
        }
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        jp_log("exe strings: %d translated (%d in place), %d skipped, %d references patched\n", ok, inplace, skipped, patched);
    }
    if (g_needPositional)
        install_sprintf_hook();
}

// ---------------------------------------------------------------- dsound passthrough
typedef HRESULT(WINAPI *DSCreateFn)(const GUID *, void **, void *);
static HMODULE g_realDsound;
static FARPROC real_dsound(const char *name)
{
    if (!g_realDsound)
    {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dsound.dll");
        g_realDsound = LoadLibraryA(path);
    }
    return g_realDsound ? GetProcAddress(g_realDsound, name) : NULL;
}
HRESULT WINAPI DirectSoundCreate(const GUID *dev, void **out, void *outer)
{
    DSCreateFn fn = (DSCreateFn)real_dsound("DirectSoundCreate");
    return fn ? fn(dev, out, outer) : E_FAIL;
}

// ---------------------------------------------------------------- script token estimate
// Before tokenizing a script, 0x4726a0 estimates the token count. It counts every byte that
// is not a digit, letter, '_' or blank as its own token -- also inside string literals -- so
// each byte of a Japanese character adds one. The estimate itself is 32-bit, but a script
// whose estimate passes 32767 is not loaded (all its strings come out empty; the intro
// slides of textplot.txt went blank at 32904). The real token count does not change (a
// literal is one token), so classify bytes >= 0x80 like letters, as in the Avadon 1 patch.
//   0x4727a5: movsx eax,byte [ecx+edx] / cmp eax,30h / jl 4727cb  (0x4727a9 -> jmp tok_class_stub)
//   0x4727ae: next test (digit 39h), 0x4727cb: not a digit, 0x4727ed: class 1 (letter)
#define A_TOKCLASS g_va[I_TOKCLASS]
static const BYTE kTokClassOrig[5] = {0x83, 0xF8, 0x30, 0x7C, 0x1D};
static DWORD g_tokDigit, g_tokOther, g_tokLetter;

__declspec(naked) static void tok_class_stub(void)
{
    __asm {
        test eax, eax
        js letter
        cmp eax, 0x30
        jl other
        jmp dword ptr [g_tokDigit]
    other:
        jmp dword ptr [g_tokOther]
    letter:
        jmp dword ptr [g_tokLetter]
    }
}

static void install_token_estimate_fix(void)
{
    BYTE *p;
    DWORD old;
    if (!A_TOKCLASS)
        return;
    p = (BYTE *)VA(A_TOKCLASS);
    if (memcmp(p, kTokClassOrig, sizeof kTokClassOrig) != 0)
    {
        jp_log("token estimate fix skipped: unexpected bytes at %06x\n", A_TOKCLASS);
        return;
    }
    g_tokDigit = (DWORD)VA(g_va[I_TOK_DIGIT]);
    g_tokOther = (DWORD)VA(g_va[I_TOK_OTHER]);
    g_tokLetter = (DWORD)VA(g_va[I_TOK_LETTER]);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(DWORD *)(p + 1) = (DWORD)tok_class_stub - (DWORD)(p + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    jp_log("token estimate fix installed\n");
}


// ---------------------------------------------------------------- print_named_str
// The script command print_named_str(who, "text") builds the message with
//   0x483ddc: push 5cb85ch ("%s %s") / lea eax,[ebp-3f8h] / push eax / call sprintf
//   (buffer: [ebp-3f8h, ebp-2f8h), 256 bytes)
// i.e. "<name> <text>". A Japanese text continues the name directly
// ("<name>が応じない。"), so drop the space when the text starts with a non-ASCII byte.
#define A_NAMED_PUSH g_va[I_NAMED_PUSH]
#define A_NAMED_CALL g_va[I_NAMED_CALL]
#define A_FMT_S_SPACE_S g_va[I_FMT_S_SPACE_S]
// The buffer is 256 bytes; other locals follow it.
#define NAMED_BUF 256

static int __cdecl named_sprintf(char *buf, const char *fmt, const char *name, const char *text)
{
    int n;
    (void)fmt;
    if (!name)
        name = "";
    if (!text)
        text = "";
    n = _snprintf(buf, NAMED_BUF - 1, ((unsigned char)text[0] >= 0x80) ? "%s%s" : "%s %s", name, text);
    if (n < 0) // truncated: _snprintf does not terminate
    {
        buf[NAMED_BUF - 1] = 0;
        n = NAMED_BUF - 1;
    }
    return n;
}

static void install_named_str_fix(void)
{
    BYTE *push, *call;
    DWORD old;
    if (!A_NAMED_PUSH || !A_NAMED_CALL || !A_FMT_S_SPACE_S || !A_SPRINTF ||
        strcmp((const char *)VA(A_FMT_S_SPACE_S), "%s %s") != 0)
    {
        jp_log("print_named_str fix skipped: code not found\n");
        return;
    }
    push = (BYTE *)VA(A_NAMED_PUSH);
    call = (BYTE *)VA(A_NAMED_CALL);
    if (push[0] != 0x68 || *(DWORD *)(push + 1) != (DWORD)VA(A_FMT_S_SPACE_S) || call[0] != 0xE8 ||
        (DWORD)(call + 5) + *(DWORD *)(call + 1) != (DWORD)VA(A_SPRINTF))
    {
        jp_log("print_named_str fix skipped: unexpected code at %06x\n", A_NAMED_PUSH);
        return;
    }
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(call + 1) = (DWORD)named_sprintf - (DWORD)(call + 5);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    jp_log("print_named_str fix installed\n");
}

// ---------------------------------------------------------------- crash diagnostics
typedef int(WINAPI *MessageBox_t)(HWND, const void *, const void *, UINT);
static MessageBox_t g_msgBoxBodyA, g_msgBoxBodyW; // entry + 2 (past the hot-patch "mov edi,edi")

static void log_stack(DWORD *sp)
{
    DWORD lo = (DWORD)g_base + 0x1000, hi = (DWORD)g_base + 0x192000;
    int i, n = 0;
    jp_log("  exe return addresses on stack:");
    for (i = 0; i < 0x2000 && n < 40; i++)
    {
        DWORD v;
        if (IsBadReadPtr(sp + i, 4))
            break;
        v = sp[i];
        if (v >= lo && v < hi)
        {
            jp_log(" %06x", v - (DWORD)g_base + 0x400000);
            n++;
        }
    }
    jp_log("\n");
}

static int WINAPI my_MessageBoxA(HWND h, const void *text, const void *caption, UINT type)
{
    jp_log("MessageBoxA: %s / %s\n", caption ? (const char *)caption : "", text ? (const char *)text : "");
    log_stack((DWORD *)&h);
    return g_msgBoxBodyA(h, text, caption, type);
}

static int WINAPI my_MessageBoxW(HWND h, const void *text, const void *caption, UINT type)
{
    jp_log("MessageBoxW: %ls / %ls\n", caption ? (const wchar_t *)caption : L"", text ? (const wchar_t *)text : L"");
    log_stack((DWORD *)&h);
    return g_msgBoxBodyW(h, text, caption, type);
}

static void hot_patch(const char *name, void *hook, MessageBox_t *body)
{
    BYTE *f = (BYTE *)GetProcAddress(GetModuleHandleA("user32.dll"), name);
    DWORD old;
    int i;
    if (!f || f[0] != 0x8B || f[1] != 0xFF)
    {
        jp_log("%s hook skipped (not hot-patchable: %02x %02x)\n", name, f ? f[0] : 0, f ? f[1] : 0);
        return;
    }
    for (i = 1; i <= 5; i++)
        if (f[-i] != 0x90 && f[-i] != 0xCC)
        {
            jp_log("%s hook skipped (no padding)\n", name);
            return;
        }
    *body = (MessageBox_t)(f + 2);
    VirtualProtect(f - 5, 7, PAGE_EXECUTE_READWRITE, &old);
    f[-5] = 0xE9;
    *(DWORD *)(f - 4) = (DWORD)hook - (DWORD)f;
    f[0] = 0xEB; // jmp short -7 -> the long jmp above
    f[1] = 0xF9;
    VirtualProtect(f - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f - 5, 7);
    jp_log("%s hook installed\n", name);
}

// The CRT reports abort() and other runtime errors with a message box. Log the exe
// return addresses found on the stack (as unrelocated 0x4xxxxx addresses) first.
static void install_msgbox_hook(void)
{
    hot_patch("MessageBoxA", (void *)my_MessageBoxA, &g_msgBoxBodyA);
    hot_patch("MessageBoxW", (void *)my_MessageBoxW, &g_msgBoxBodyW);
}

// ---------------------------------------------------------------- translated data files (drop-in mode)
// The game's own files are never changed. avadon3_jp\jp_patch.bin holds deltas against the
// original files; on start-up they are applied into avadon3_jp\cache\ (only when the
// original's SHA-256 matches), and the exe's CreateFileA/W and FindFirstFileExA are
// redirected there. Removing dsound.dll restores the English game.
typedef struct
{
    DWORD h[8];
    BYTE buf[64];
    unsigned __int64 len;
    int n;
} Sha256;

static const DWORD kShaK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const BYTE *p)
{
    DWORD w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (DWORD)p[i * 4] << 24 | (DWORD)p[i * 4 + 1] << 16 | (DWORD)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 64; i++)
        w[i] = (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7] +
               (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (i = 0; i < 64; i++)
    {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + kShaK[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void sha256(const BYTE *data, DWORD size, BYTE out[32])
{
    static const DWORD init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Sha256 s;
    DWORD i;
    unsigned __int64 bits = (unsigned __int64)size * 8;
    memcpy(s.h, init, sizeof init);
    for (i = 0; i + 64 <= size; i += 64)
        sha_block(&s, data + i);
    s.n = size - i;
    memcpy(s.buf, data + i, s.n);
    s.buf[s.n++] = 0x80;
    if (s.n > 56)
    {
        memset(s.buf + s.n, 0, 64 - s.n);
        sha_block(&s, s.buf);
        s.n = 0;
    }
    memset(s.buf + s.n, 0, 56 - s.n);
    for (i = 0; i < 8; i++)
        s.buf[56 + i] = (BYTE)(bits >> (56 - 8 * i));
    sha_block(&s, s.buf);
    for (i = 0; i < 8; i++)
    {
        out[i * 4] = (BYTE)(s.h[i] >> 24);
        out[i * 4 + 1] = (BYTE)(s.h[i] >> 16);
        out[i * 4 + 2] = (BYTE)(s.h[i] >> 8);
        out[i * 4 + 3] = (BYTE)s.h[i];
    }
}

static BYTE *read_all(const char *path, DWORD *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    BYTE *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (BYTE *)malloc(n > 0 ? n : 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (DWORD)n;
    return buf;
}

static void make_parent_dirs(const char *path)
{
    char tmp[MAX_PATH];
    char *p;
    lstrcpynA(tmp, path, MAX_PATH);
    for (p = tmp + 3; *p; p++)
        if (*p == '\\')
        {
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
}

#define MAX_REDIRECTS 1024
typedef struct
{
    char rel[MAX_PATH];   // lower case, '\' separated, relative to the game folder
    char cache[MAX_PATH]; // translated copy
} Redirect;
static Redirect *g_redir;
static int g_redirCount;
static char g_gameDirLower[MAX_PATH];
static int g_gameDirLen;

static void lower_path(char *s)
{
    for (; *s; s++)
        *s = *s == '/' ? '\\' : (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
}

static const char *redirect_for(const char *path)
{
    char full[MAX_PATH * 2];
    DWORD n;
    int i;
    if (!g_redirCount || !path)
        return NULL;
    n = GetFullPathNameA(path, sizeof full, full, NULL);
    if (!n || n >= sizeof full)
        return NULL;
    lower_path(full);
    if (strncmp(full, g_gameDirLower, g_gameDirLen) != 0 || full[g_gameDirLen] != '\\')
        return NULL;
    for (i = 0; i < g_redirCount; i++)
        if (strcmp(full + g_gameDirLen + 1, g_redir[i].rel) == 0)
            return g_redir[i].cache;
    return NULL;
}

typedef HANDLE(WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *FindFirstFileExA_t)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
static CreateFileA_t g_realCreateFileA;
static CreateFileW_t g_realCreateFileW;
static FindFirstFileExA_t g_realFindFirstFileExA;
static int g_redirLogged;

static void log_redirect(const char *from, const char *to)
{
    if (g_redirLogged < 5) // the first few, to confirm the redirect works
    {
        g_redirLogged++;
        jp_log("redirect %s -> %s\n", from, to);
    }
}

static HANDLE WINAPI my_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD flags, HANDLE tmpl)
{
    const char *to = (access & (GENERIC_WRITE | GENERIC_ALL)) ? NULL : redirect_for(name);
    if (to)
    {
        log_redirect(name, to);
        name = to;
    }
    return g_realCreateFileA(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI my_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD flags, HANDLE tmpl)
{
    char a[MAX_PATH * 2];
    WCHAR w[MAX_PATH];
    const char *to = NULL;
    if (name && !(access & (GENERIC_WRITE | GENERIC_ALL)) && g_redirCount &&
        WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof a, NULL, NULL))
        to = redirect_for(a);
    if (to && MultiByteToWideChar(CP_ACP, 0, to, -1, w, MAX_PATH))
    {
        log_redirect(a, to);
        name = w;
    }
    return g_realCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI my_FindFirstFileExA(LPCSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
                                         FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
{
    const char *to = redirect_for(name); // stat() of a translated file reports the translated size
    return g_realFindFirstFileExA(to ? to : name, level, data, op, filter, flags);
}

// Replace one entry of the exe's import address table; returns the original function.
static void *patch_iat(const char *dll, const char *func, void *hook)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dir.VirtualAddress);
    for (; dir.VirtualAddress && d->Name; d++)
    {
        IMAGE_THUNK_DATA *names, *addrs;
        if (_stricmp((const char *)(g_base + d->Name), dll) != 0)
            continue;
        names = (IMAGE_THUNK_DATA *)(g_base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        addrs = (IMAGE_THUNK_DATA *)(g_base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++)
        {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            void *orig;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) != 0)
                continue;
            orig = (void *)addrs->u1.Function;
            VirtualProtect(&addrs->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old);
            addrs->u1.Function = (DWORD)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(DWORD), old, &old);
            return orig;
        }
    }
    return NULL;
}

static DWORD rd32(const BYTE *p) { return *(const DWORD *)p; }

// Apply one patch entry; returns a malloc'ed buffer of dst_len bytes or NULL.
static BYTE *apply_entry(const BYTE *ops, DWORD count, const BYTE *src, DWORD srcLen, DWORD dstLen)
{
    BYTE *o = (BYTE *)malloc(dstLen ? dstLen : 1);
    DWORD k, w = 0;
    for (k = 0; o && k < count; k++)
    {
        BYTE t = *ops++;
        DWORD off, len, i;
        if (t == 2)
        {
            len = rd32(ops), ops += 4;
            if (w + len > dstLen)
                goto bad;
            memcpy(o + w, ops, len);
            ops += len;
        }
        else
        {
            off = rd32(ops), len = rd32(ops + 4), ops += 8;
            if (w + len > dstLen || !srcLen)
                goto bad;
            if (t == 1)
            {
                if (off + len > srcLen)
                    goto bad;
                memcpy(o + w, src + off, len);
            }
            else
            {
                for (i = 0; i < len; i++)
                    o[w + i] = (BYTE)(ops[i] + src[(off + i) % srcLen]);
                ops += len;
            }
        }
        w += len;
    }
    if (o && w == dstLen)
        return o;
bad:
    free(o);
    return NULL;
}

// Skip over the ops of one entry (to reach the next entry).
static const BYTE *skip_ops(const BYTE *p, DWORD count)
{
    DWORD k;
    for (k = 0; k < count; k++)
    {
        BYTE t = *p++;
        p += t == 1 ? 8 : t == 2 ? 4 + rd32(p) : 8 + rd32(p + 4);
    }
    return p;
}

static void prepare_redirects(const char *gameDir, const char *dataDir)
{
    char path[MAX_PATH], stampPath[MAX_PATH], line[MAX_PATH + 64];
    BYTE *patch, h[32];
    const BYTE *p, *end;
    DWORD size, n, i;
    WIN32_FILE_ATTRIBUTE_DATA pa;
    char *stamps = NULL; // old stamp file contents, searched by line
    DWORD stampsLen = 0;
    FILE *out;
    int made = 0, reused = 0, already = 0, mismatch = 0;

    _snprintf(path, sizeof path, "%s\\jp_patch.bin", dataDir);
    patch = read_all(path, &size);
    if (!patch)
    {
        jp_log("no %s (data files are used as they are)\n", path);
        return;
    }
    if (size < 12 || memcmp(patch, "AJPATCH1", 8) != 0 || !GetFileAttributesExA(path, GetFileExInfoStandard, &pa))
    {
        jp_log("%s is broken\n", path);
        free(patch);
        return;
    }
    lstrcpynA(g_gameDirLower, gameDir, MAX_PATH);
    lower_path(g_gameDirLower);
    g_gameDirLen = (int)strlen(g_gameDirLower);
    g_redir = (Redirect *)calloc(MAX_REDIRECTS, sizeof(Redirect));

    // A stamp line per cached file: "rel <TAB> original size <TAB> original mtime <TAB> patch stamp".
    // A cached file is reused while the original and the patch file are unchanged.
    _snprintf(stampPath, sizeof stampPath, "%s\\cache\\stamp.txt", dataDir);
    stamps = (char *)read_all(stampPath, &stampsLen);
    if (stamps)
    {
        // keep a leading '\n' so that a search for "\n<line>" only matches whole lines
        char *s = (char *)malloc(stampsLen + 2);
        s[0] = '\n';
        memcpy(s + 1, stamps, stampsLen);
        s[stampsLen + 1] = 0;
        free(stamps);
        stamps = s;
    }
    make_parent_dirs(stampPath);
    out = fopen(stampPath, "wb");

    n = rd32(patch + 8);
    p = patch + 12;
    end = patch + size;
    for (i = 0; i < n && p < end && g_redirCount < MAX_REDIRECTS; i++)
    {
        char rel[MAX_PATH], target[MAX_PATH], cache[MAX_PATH], *q;
        WORD plen = *(const WORD *)p;
        const BYTE *srcHash, *dstHash, *ops;
        DWORD srcLen, dstLen, count;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        p += 2;
        if (plen >= MAX_PATH)
            break;
        memcpy(rel, p, plen);
        rel[plen] = 0;
        p += plen;
        srcLen = rd32(p), srcHash = p + 4, p += 36;
        dstLen = rd32(p), dstHash = p + 4, p += 36;
        count = rd32(p), p += 4;
        ops = p;
        p = skip_ops(p, count);
        for (q = rel; *q; q++)
            if (*q == '/')
                *q = '\\';
        _snprintf(target, sizeof target, "%s\\%s", gameDir, rel);
        _snprintf(cache, sizeof cache, "%s\\cache\\%s", dataDir, rel);
        if (!GetFileAttributesExA(target, GetFileExInfoStandard, &fa))
            continue;
        _snprintf(line, sizeof line, "\n%s\t%lu\t%lu.%lu\t%lu.%lu\n", rel, fa.nFileSizeLow,
                  fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime,
                  pa.ftLastWriteTime.dwHighDateTime, pa.ftLastWriteTime.dwLowDateTime);
        if (stamps && strstr(stamps, line) && GetFileAttributesExA(cache, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeLow == dstLen)
        {
            reused++;
        }
        else
        {
            DWORD olen;
            BYTE *orig = read_all(target, &olen), *res = NULL;
            if (!orig)
                continue;
            sha256(orig, olen, h);
            if (olen == dstLen && memcmp(h, dstHash, 32) == 0)
            {
                already++; // the game file is already the Japanese one
                free(orig);
                continue;
            }
            if (olen == srcLen && memcmp(h, srcHash, 32) == 0)
                res = apply_entry(ops, count, orig, olen, dstLen);
            if (res)
                sha256(res, dstLen, h);
            if (!res || memcmp(h, dstHash, 32) != 0)
            {
                if (mismatch++ < 10)
                    jp_log("not translated (the original differs): %s\n", rel);
                free(orig);
                free(res);
                continue;
            }
            {
                FILE *f;
                make_parent_dirs(cache);
                f = fopen(cache, "wb");
                if (!f || fwrite(res, 1, dstLen, f) != dstLen)
                {
                    jp_log("cannot write %s\n", cache);
                    if (f)
                        fclose(f);
                    free(orig);
                    free(res);
                    continue;
                }
                fclose(f);
            }
            made++;
            free(orig);
            free(res);
        }
        if (out)
            fputs(line + 1, out);
        lstrcpynA(g_redir[g_redirCount].rel, rel, MAX_PATH);
        lower_path(g_redir[g_redirCount].rel);
        lstrcpynA(g_redir[g_redirCount].cache, cache, MAX_PATH);
        g_redirCount++;
    }
    if (out)
        fclose(out);
    free(stamps);
    free(patch);
    jp_log("data files: %d translated (%d built, %d cached), %d already Japanese, %d left as they are\n",
           g_redirCount, made, reused, already, mismatch);
    if (!g_redirCount)
        return;
    g_realCreateFileA = (CreateFileA_t)patch_iat("kernel32.dll", "CreateFileA", (void *)my_CreateFileA);
    g_realCreateFileW = (CreateFileW_t)patch_iat("kernel32.dll", "CreateFileW", (void *)my_CreateFileW);
    g_realFindFirstFileExA = (FindFirstFileExA_t)patch_iat("kernel32.dll", "FindFirstFileExA", (void *)my_FindFirstFileExA);
    if (!g_realCreateFileA)
        g_realCreateFileA = CreateFileA;
    if (!g_realCreateFileW)
        g_realCreateFileW = CreateFileW;
    if (!g_realFindFirstFileExA)
        g_realFindFirstFileExA = FindFirstFileExA;
    jp_log("file redirect installed\n");
}

// ---------------------------------------------------------------- finding the hook points
// Patterns made by tools/gen_patterns.py from the Steam build. "??" bytes are absolute
// addresses or call targets, which differ between builds. A pattern with an anchor is
// searched in the 0x400 bytes after the anchor's match; the others must be unique in
// the code section.
typedef struct
{
    const char *name, *anchor;
    DWORD steamVa;
    const char *pat;
} Pattern;

static const Pattern kPatterns[] = {
    {"draw_text", NULL, 0x488780,
     "55 8B EC 81 EC 80 08 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 33 C0 66 89 85 C0 F7 FF FF 33 C9 66 89 8D E4 F9 FF FF 33 D2 66 89 95 B8 F7 FF FF 33 C0 66 89 85 C4 F7 FF FF C6 85 DF F9 FF FF 00 B9 02 00 00 00 66 89 8D D0 F9 FF FF BA 02 00 00 00 66 89 95 D8 F9 FF FF 8B 45 14 89 85 B4 F7 FF FF 8D 8D C8 F7 FF FF 89 8D B0 F7 FF FF 8B 95 B0 F7 FF FF 89 95 AC F7 FF FF 8B 85 B4 F7 FF FF 8A 08 88 8D AB F7 FF FF 8B 95 B0 F7 FF FF 8A 85 AB F7 FF FF 88 02 8B 8D B4 F7 FF FF 83 C1 01 89 8D B4 F7 FF FF 8B 95 B0 F7 FF FF 83 C2 01 89 95 B0 F7 FF FF 80 BD AB F7 FF FF 00 75 BD 8D 85 C8 F7 FF FF 50 E8 ?? ?? ?? ?? 83 C4 04"},
    {"font_select", "draw_text", 0x48894a,
     "B8 0C 00 00 00 66 89 45 1C 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 66 8B 4D 1C 66 83 C1 01 66 89 4D 1C 66 8B 55 1C 66 89 15 ?? ?? ?? ??"},
    {"draw_ascii", NULL, 0x48c070,
     "55 8B EC 83 EC 20 33 C0 66 89 45 E8 66 8B 0D ?? ?? ?? ?? 66 89 4D F4 66 8B 15 ?? ?? ?? ?? 66 89 55 E0 81 3D ?? ?? ?? ?? ?? ?? ?? ?? 75 06 33 C0 66 89 45 E4"},
    {"charmap", "draw_ascii", 0x48c121,
     "0F BF 0C 45 ?? ?? ?? ??"},
    {"ascii_blit", "draw_ascii", 0x48c2c1,
     "0F BF 45 18 83 E8 1F 50 0F BF 4D 14 83 E9 1F 51 0F BF 55 10 83 EA 1F 52 E8 ?? ?? ?? ?? 83 C4 0C 6A 00 6A 00 6A 01 8B 45 F0 50 8B 4D EC 51 6A 00 8B 55 FC 52 8B 45 F8 50 8B 0D ?? ?? ?? ?? 51 E8 ?? ?? ?? ?? 83 C4 24 6A 00 6A 00 6A 00 E8 ?? ?? ?? ?? 83 C4 0C"},
    {"bind_tex", NULL, 0x49cd40,
     "55 8B EC 51 89 4D FC 8B 45 FC 8B 0D ?? ?? ?? ?? 3B 48 2C 74 55 8B 55 FC 81 BA 44 01 00 00 F5 84 00 00 75 18 68 F5 84 00 00 FF 15 ?? ?? ?? ?? 68 E1 0D 00 00 FF 15 ?? ?? ?? ?? EB 0B 68 E1 0D 00 00 FF 15 ?? ?? ?? ?? 8B 45 FC 8B 48 2C 51 8B 55 FC 8B 82 44 01 00 00 50 FF 15 ?? ?? ?? ?? 8B 4D FC 8B 51 2C 89 15 ?? ?? ?? ?? 8B E5 5D"},
    {"str_width", NULL, 0x488600,
     "55 8B EC 81 EC 30 04 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 33 C0 66 89 85 E8 FB FF FF 8B 4D 08 89 8D DC FB FF FF 8B 95 DC FB FF FF 83 C2 01 89 95 D8 FB FF FF 8B 85 DC FB FF FF 8A 08 88 8D D7 FB FF FF 83 85 DC FB FF FF 01 80 BD D7 FB FF FF 00 75 E2 8B 95 DC FB FF FF 2B 95 D8 FB FF FF 89 95 D0 FB FF FF 66 8B 85 D0 FB FF FF 66 89 85 EC FB FF FF 33 C9 66 89 8D E4 FB FF FF EB 12 66 8B 95 E4 FB FF FF 66 83 C2 01 66 89 95 E4 FB FF FF 0F BF 85 E4 FB FF FF 3D 01 02 00 00 7D 13 0F BF 8D E4 FB FF FF 33 D2 66 89 94 4D F0 FB FF FF EB CD 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 8D 85 F0 FB FF FF 50 8B 4D 08 51 68 00 02 00 00 E8 ?? ?? ?? ??"},
    {"named_str", NULL, 0x483dc8,
     "0F BF 95 BC FD FF FF 69 D2 B0 1A 00 00 81 C2 ?? ?? ?? ?? 52 68 ?? ?? ?? ?? 8D 85 08 FC FF FF 50 E8 ?? ?? ?? ??"},
    {"tok_class", NULL, 0x4727a5,
     "0F BE 04 11 83 F8 30 7C 1D 8B 4D E4 8B 11 8B 45 EC 0F BE 0C 02 83 F9 39 7F 0C C7 45 F4 02 00 00 00 E9 ?? ?? ?? ?? 8B 55 E4 8B 02 8B 4D EC 0F BE 14 08 83 FA 61 7C 1A 8B 45 E4 8B 08 8B 55 EC 0F BE 04 11 83 F8 7A 7F 09 C7 45 F4 01 00 00 00 EB 5D"},
};
enum { P_DRAW_TEXT, P_FONT_SELECT, P_DRAW_ASCII, P_CHARMAP, P_ASCII_BLIT, P_BIND_TEX, P_STR_WIDTH, P_NAMED_STR, P_TOK_CLASS, P_COUNT };

static BYTE *g_textStart, *g_textEnd;

static int parse_pattern(const char *s, BYTE *bytes, BYTE *mask)
{
    int n = 0;
    while (*s)
    {
        if (*s == ' ')
        {
            s++;
            continue;
        }
        if (s[0] == '?')
            bytes[n] = 0, mask[n] = 0;
        else
            bytes[n] = (BYTE)strtoul(s, NULL, 16), mask[n] = 1;
        n++;
        s += 2;
    }
    return n;
}

static int match_at(const BYTE *p, const BYTE *bytes, const BYTE *mask, int n)
{
    int i;
    if (p < g_textStart || p + n > g_textEnd)
        return 0;
    for (i = 0; i < n; i++)
        if (mask[i] && p[i] != bytes[i])
            return 0;
    return 1;
}

// Returns the match, or NULL (not found, or not unique).
static BYTE *find_pattern(int idx, BYTE **found, int forceScan, int *viaScan)
{
    const Pattern *pt = &kPatterns[idx];
    BYTE bytes[256], mask[256], *p, *from, *to, *hit = NULL;
    int n = parse_pattern(pt->pat, bytes, mask), hits = 0, a;
    *viaScan = 0;
    if (!forceScan && match_at((BYTE *)VA(pt->steamVa), bytes, mask, n))
        return (BYTE *)VA(pt->steamVa); // the Steam build: the known place
    *viaScan = 1;
    from = g_textStart;
    to = g_textEnd - n;
    if (pt->anchor)
    {
        for (a = 0; a < P_COUNT && strcmp(kPatterns[a].name, pt->anchor) != 0; a++)
            ;
        if (a == P_COUNT || !found[a])
            return NULL;
        from = found[a];
        if (from + 0x400 < to)
            to = from + 0x400;
    }
    for (p = from; p <= to; p++)
        if ((!mask[0] || p[0] == bytes[0]) && match_at(p, bytes, mask, n))
        {
            if (pt->anchor)
                return p; // the first one after the anchor
            hits++;
            hit = p;
        }
    if (hits != 1)
    {
        jp_log("pattern %s: %d matches\n", pt->name, hits);
        return NULL;
    }
    return hit;
}

static DWORD unreloc(DWORD runtime) { return runtime - (DWORD)g_base + 0x400000; }
static DWORD abs_at(const BYTE *p) { return unreloc(*(const DWORD *)p); }
static DWORD call_at(const BYTE *p) { return unreloc((DWORD)(p + 4) + *(const DWORD *)p); } // rel32 at p

// Fills g_va. Returns 1 when everything needed for Japanese text was found.
static int resolve_addresses(int forceScan)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *m[P_COUNT] = {0};
    int i, scanned = 0, viaScan, core;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].Characteristics & IMAGE_SCN_CNT_CODE)
        {
            g_textStart = g_base + sec[i].VirtualAddress;
            g_textEnd = g_textStart + sec[i].Misc.VirtualSize;
            break;
        }
    if (!g_textStart)
        return 0;
    for (i = 0; i < P_COUNT; i++)
    {
        m[i] = find_pattern(i, m, forceScan, &viaScan);
        if (forceScan == 2 && i == P_TOK_CLASS)
            m[i] = NULL; // [debug] force_scan=2: pretend one is missing (tests the fall-back to English)
        scanned += viaScan;
        if (!m[i])
            jp_log("not found in this Avadon 3.exe: %s\n", kPatterns[i].name);
    }
    memset(g_va, 0, sizeof g_va);
    if (m[P_DRAW_TEXT])
    {
        g_va[I_DRAW_TEXT] = unreloc((DWORD)m[P_DRAW_TEXT]);
        g_va[I_NAME_SUBST] = call_at(m[P_DRAW_TEXT] + 0xc2);
    }
    if (m[P_FONT_SELECT])
    {
        g_va[I_FONT0] = abs_at(m[P_FONT_SELECT] + 0xa);
        g_va[I_LINE_ACC] = abs_at(m[P_FONT_SELECT] + 0x29);
    }
    if (m[P_DRAW_ASCII])
    {
        g_va[I_DRAW_ASCII] = unreloc((DWORD)m[P_DRAW_ASCII]);
        g_va[I_PEN_X] = abs_at(m[P_DRAW_ASCII] + 0xf);
        g_va[I_PEN_Y] = abs_at(m[P_DRAW_ASCII] + 0x1a);
        g_va[I_CUR_FONT] = abs_at(m[P_DRAW_ASCII] + 0x24);
        // the first font must be the one draw_text selects by default
        if (abs_at(m[P_DRAW_ASCII] + 0x28) != g_va[I_FONT0])
        {
            jp_log("draw_ascii does not match font_select\n");
            g_va[I_DRAW_ASCII] = 0;
        }
    }
    if (m[P_CHARMAP])
        g_va[I_CHARMAP] = abs_at(m[P_CHARMAP] + 4);
    if (m[P_ASCII_BLIT])
    {
        g_va[I_SET_COLOR] = call_at(m[P_ASCII_BLIT] + 0x19);
        g_va[I_FONT_TEX] = abs_at(m[P_ASCII_BLIT] + 0x3a);
        g_va[I_BLIT] = call_at(m[P_ASCII_BLIT] + 0x40);
    }
    if (m[P_BIND_TEX])
        g_va[I_BOUND_TEX_CACHE] = abs_at(m[P_BIND_TEX] + 0xc);
    if (m[P_STR_WIDTH])
    {
        g_va[I_STR_WIDTH] = unreloc((DWORD)m[P_STR_WIDTH]);
        if (abs_at(m[P_STR_WIDTH] + 0xb1) != g_va[I_FONT0])
        {
            jp_log("str_width does not match font_select\n");
            g_va[I_STR_WIDTH] = 0;
        }
    }
    if (m[P_NAMED_STR])
    {
        g_va[I_NAMED_PUSH] = unreloc((DWORD)(m[P_NAMED_STR] + 0x14));
        g_va[I_FMT_S_SPACE_S] = abs_at(m[P_NAMED_STR] + 0x15);
        g_va[I_NAMED_CALL] = unreloc((DWORD)(m[P_NAMED_STR] + 0x20));
        g_va[I_SPRINTF] = call_at(m[P_NAMED_STR] + 0x21);
    }
    if (m[P_TOK_CLASS])
    {
        g_va[I_TOKCLASS] = unreloc((DWORD)(m[P_TOK_CLASS] + 4));
        g_va[I_TOK_DIGIT] = unreloc((DWORD)(m[P_TOK_CLASS] + 9));
        g_va[I_TOK_OTHER] = unreloc((DWORD)(m[P_TOK_CLASS] + 0x26));
        g_va[I_TOK_LETTER] = unreloc((DWORD)(m[P_TOK_CLASS] + 0x48));
    }

    {
        int same = 1;
        for (i = 0; i < I_COUNT; i++)
            if (g_va[i] != kSteamVa[i])
            {
                same = 0;
                jp_log("  %-16s %06x (Steam build: %06x)\n", kAddrName[i], g_va[i], kSteamVa[i]);
            }
        jp_log("hook points: %s%s\n", same ? "same as the Steam build" : "differ from the Steam build (see above)",
               scanned ? (forceScan ? " [search forced by force_scan]" : " [searched]") : "");
    }
    // Japanese text needs the renderer; without it the
    // game is left completely untouched (English).
    core = 1;
    for (i = 0; i < I_COUNT; i++)
        if (!g_va[i] && i != I_SPRINTF && i != I_NAMED_PUSH && i != I_NAMED_CALL && i != I_FMT_S_SPACE_S)
            core = 0;
    return core;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char dir[MAX_PATH], data[MAX_PATH], path[MAX_PATH], *p;
        DWORD attr;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, dir, MAX_PATH);
        p = strrchr(dir, '\\');
        if (p)
            *p = 0;
        // Files of the patch live in the "avadon3_jp" folder (drop-in layout); the game
        // folder itself is still searched for them, as in the earlier layout.
        _snprintf(data, sizeof data, "%s\\avadon3_jp", dir);
        attr = GetFileAttributesA(data);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            lstrcpynA(data, dir, MAX_PATH);
        _snprintf(path, sizeof path, "%s\\avadon3_jp.log", data);
        g_log = fopen(path, "w");
        load_config(dir);
        g_base = (BYTE *)GetModuleHandleA(NULL);
        jp_log("Avadon 3 JP hook loaded. base=%p face=%s\n", g_base, g_fontFace);
        _snprintf(path, sizeof path, "%s\\avadon3_jp.ini", dir);
        g_forceScan = GetPrivateProfileIntA("debug", "force_scan", 0, path);
        g_trace = GetPrivateProfileIntA("debug", "trace", 0, path);
        if (!resolve_addresses(g_forceScan))
        {
            jp_log("this Avadon 3.exe is not supported: Japanese is turned off and the game runs unchanged\n");
            return TRUE;
        }
        install();
        _snprintf(path, sizeof path, "%s\\avadon3_jp_exe.tsv", data);
        load_exe_strings(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? data : dir);
        prepare_redirects(dir, data);
        install_token_estimate_fix();
        install_named_str_fix();
        install_msgbox_hook();
    }
    return TRUE;
}
