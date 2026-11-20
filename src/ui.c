#include "ui.h"
#include "text.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

vita2d_pgf *ui_font;

void ui_clean_text(const char *in, char *out, size_t cap)
{
    text_clean(in, out, cap);
}

int ui_text_w(float scale, const char *s)
{
    return vita2d_pgf_text_width(ui_font, scale, s);
}

void ui_text(int x, int y, unsigned color, float scale, const char *s)
{
    vita2d_pgf_draw_text(ui_font, x, y, color, scale, s);
}

void ui_text_fit(int x, int y, int max_w, unsigned color, float scale, const char *s)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s);
    if (ui_text_w(scale, buf) <= max_w) {
        ui_text(x, y, color, scale, buf);
        return;
    }
    size_t n = strlen(buf);
    while (n > 0) {
        /* step back one UTF-8 character */
        do { n--; } while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80);
        if (n + 4 > sizeof(buf))
            continue;
        memcpy(buf + n, "...", 4);
        if (ui_text_w(scale, buf) <= max_w)
            break;
    }
    ui_text(x, y, color, scale, buf);
}

void ui_text_center(int cx, int y, unsigned color, float scale, const char *s)
{
    ui_text(cx - ui_text_w(scale, s) / 2, y, color, scale, s);
}

void ui_round_rect(float x, float y, float w, float h, float r, unsigned color)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    vita2d_draw_rectangle(x + r, y, w - 2 * r, h, color);
    vita2d_draw_rectangle(x, y + r, r, h - 2 * r, color);
    vita2d_draw_rectangle(x + w - r, y + r, r, h - 2 * r, color);
    vita2d_draw_fill_circle(x + r, y + r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + r, r, color);
    vita2d_draw_fill_circle(x + r, y + h - r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + h - r, r, color);
}

void ui_frame(float x, float y, float w, float h, float t, unsigned color)
{
    vita2d_draw_rectangle(x - t, y - t, w + 2 * t, t, color);
    vita2d_draw_rectangle(x - t, y + h, w + 2 * t, t, color);
    vita2d_draw_rectangle(x - t, y, t, h, color);
    vita2d_draw_rectangle(x + w, y, t, h, color);
}

void ui_spinner(float cx, float cy, float r, unsigned color)
{
    float t = (float)(sceKernelGetProcessTimeWide() % 1000000ULL) / 1000000.0f;
    int head = (int)(t * 8);
    for (int i = 0; i < 8; i++) {
        float a = (float)i / 8.0f * 6.2831853f;
        int age = (head - i + 8) % 8;
        unsigned alpha = (unsigned)(255 - age * 28);
        unsigned c = (color & 0x00FFFFFFu) | (alpha << 24);
        vita2d_draw_fill_circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.22f, c);
    }
}

void ui_image(vita2d_texture *t, float x, float y, float w, float h)
{
    float tw = (float)vita2d_texture_get_width(t), th = (float)vita2d_texture_get_height(t);
    if (tw > 0 && th > 0)
        vita2d_draw_texture_scale(t, x, y, w / tw, h / th);
}

int ui_hint(int x, int y, const char *btn, const char *label)
{
    int bw = ui_text_w(0.8f, btn) + 12;
    if (bw < 24) bw = 24;
    ui_round_rect(x, y - 16, bw, 22, 6, C_PANEL2);
    ui_text_center(x + bw / 2, y, C_WHITE, 0.8f, btn);
    ui_text(x + bw + 6, y, C_GREY, 0.85f, label);
    return bw + 6 + ui_text_w(0.85f, label) + 20;
}

void ui_fmt_viewers(int v, char *out, size_t cap)
{
    if (v >= 1000000)
        snprintf(out, cap, "%.1f M", v / 1e6);
    else if (v >= 10000)
        snprintf(out, cap, "%.1f mil", v / 1e3);
    else if (v >= 1000)
        snprintf(out, cap, "%d.%03d", v / 1000, v % 1000);
    else
        snprintf(out, cap, "%d", v);
    /* Spanish decimal comma */
    for (char *p = out; *p; p++)
        if (*p == '.' && v >= 10000)
            *p = ',';
}

#define DPAD (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)

void ui_input_poll(ui_input_t *in)
{
    SceCtrlData pad;
    unsigned long long now = sceKernelGetProcessTimeWide();
    sceCtrlPeekBufferPositive(0, &pad, 1);

    /* left stick acts as a d-pad too */
    unsigned b = pad.buttons;
    if (pad.ly < 40) b |= SCE_CTRL_UP;
    if (pad.ly > 215) b |= SCE_CTRL_DOWN;
    if (pad.lx < 40) b |= SCE_CTRL_LEFT;
    if (pad.lx > 215) b |= SCE_CTRL_RIGHT;

    in->pressed = b & ~in->prev;
    if (in->pressed & DPAD) {
        in->repeat_at = now + 380000;
    } else if ((b & DPAD) && now >= in->repeat_at) {
        in->pressed |= b & DPAD;
        in->repeat_at = now + 90000;
    }
    in->held = b;
    in->prev = b;
}
