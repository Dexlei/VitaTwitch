/*
 * VitaTwitch
 * Home with Siguiendo / Explorar / Buscar tabs, Twitch login with a device
 * code, and live playback with the hardware decoders.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <vita2d.h>

#include "auth.h"
#include "http.h"
#include "images.h"
#include "log.h"
#include "player.h"
#include "task.h"
#include "twitch.h"
#include "ui.h"
#include "i18n.h"
#include "ime_utf8.h"
#include "playback_policy.h"

int _newlib_heap_size_user = 96 * 1024 * 1024;

#define DATA_DIR      "ux0:data/VitaTwitch"
#define AUTH_FILE     DATA_DIR "/auth.txt"
#define SETTINGS_FILE DATA_DIR "/settings.txt"
#define LOG_FILE      DATA_DIR "/log.txt"
#define LOG_PREV_FILE DATA_DIR "/log_prev.txt"
#define APP_VERSION   "1.33-netfix1"

#define MAX_ITEMS 100

/* grid geometry */
#define COLS      3
#define CARD_W    288
#define THUMB_H   162
#define CARD_H    214
#define GAP_X     18
#define ROW_PITCH 232
#define GRID_X    30
#define HEADER_H  56
#define FOOTER_Y  506
#define SEARCH_H  44
#define TAB_X     230
#define TAB_STEP  165
#define TAB_W     156

static unsigned g_btn_ok = SCE_CTRL_CROSS, g_btn_back = SCE_CTRL_CIRCLE;
static const char *g_ok_name = "X", *g_back_name = "O";

static auth_t g_auth;
static int g_logged;
static task_t g_auth_task;
static int g_auth_checking;
static char g_pref_quality[32];
static char g_search[64];
static int g_chat_on = 1;
static int g_profile=VT_PROFILE_BALANCED, g_auto_quality=0, g_diag=0, g_volume=100, g_remember=1;
static unsigned long long g_auth_last_check;
static int g_touch_manual_scroll = 0;

static ui_input_t g_in;
static char g_toast[160];
static unsigned long long g_toast_until;

/* ------------------------------------------------------------- tabs */

enum { TAB_FOLLOWED, TAB_EXPLORE, TAB_SEARCH, TAB_COUNT };
static const char *tab_name(int t) {return vt_tr((vt_text_id)(VT_I_FOLLOWING+t));}

typedef enum { LS_EMPTY, LS_LOADING, LS_READY, LS_ERROR } list_state_t;

typedef struct {
    twitch_list_t kind;
    char query[64];
    char oauth[64];
    twitch_stream_t out[MAX_ITEMS];
    int n;
    char err[160];
} list_job_t;

typedef struct {
    list_state_t state;
    twitch_stream_t items[MAX_ITEMS];
    int count;
    int sel;            /* -1 = search bar (Buscar tab) */
    float scroll;
    char err[160];
    int need_login;
    list_job_t job;
    task_t task;
    int busy;
} tab_t;

static tab_t g_tabs[TAB_COUNT];
static int g_tab = TAB_EXPLORE;

/* ---------------------------------------------------------- helpers */

static void toast(const char *msg)
{
    LOGI("aviso: %s", msg);
    snprintf(g_toast, sizeof(g_toast), "%s", msg);
    g_toast_until = sceKernelGetProcessTimeWide() + 3000000ULL;
}

static int net_connected(void)
{
    int st = 0;
    if (sceNetCtlInetGetState(&st) < 0)
        return 1; /* unknown: let the request decide */
    return st == SCE_NETCTL_STATE_CONNECTED;
}

static void ensure_data_dir(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
}

static void settings_load(void)
{
    char line[160];
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!strncmp(line, "quality=", 8))
            snprintf(g_pref_quality, sizeof(g_pref_quality), "%s", line + 8);
        else if (!strncmp(line, "search=", 7))
            snprintf(g_search, sizeof(g_search), "%s", line + 7);
        else if (!strncmp(line, "chat=", 5))
            g_chat_on = atoi(line + 5) != 0;
        else if (!strncmp(line, "lang=", 5))
            vt_lang_set_code(line + 5);
        else if (!strncmp(line, "profile=", 8)) {int p=atoi(line+8);if(p>=0&&p<VT_PROFILE_COUNT)g_profile=p;}
        else if (!strncmp(line, "auto_quality=", 13)) g_auto_quality=atoi(line+13)!=0;
        else if (!strncmp(line, "diag=", 5)) g_diag=atoi(line+5)!=0;
        else if (!strncmp(line, "volume=", 7)) {int v=atoi(line+7);if(v>=0&&v<=100)g_volume=v;}
        else if (!strncmp(line, "remember=", 9)) g_remember=atoi(line+9)!=0;
        else if (!strncmp(line, "touch=", 6))
            ui_touch_enable(atoi(line + 6) != 0);
    }
    fclose(f);
}

static void settings_save(void)
{
    ensure_data_dir();
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f)
        return;
    fprintf(f, "quality=%s\nsearch=%s\nchat=%d\nlang=%s\ntouch=%d\n",
            g_pref_quality, g_search, g_chat_on,
            vt_lang_code(vt_lang_current()), ui_touch_enabled());
    fprintf(f,"profile=%d\nauto_quality=%d\ndiag=%d\nvolume=%d\nremember=%d\n",g_profile,g_auto_quality,g_diag,g_volume,g_remember);
    fclose(f);
}

static void frame_begin(void)
{
    img_frame();
    vita2d_start_drawing();
    vita2d_clear_screen();
}

static void frame_end(void)
{
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

/* ------------------------------------------------------- list loads */

static int list_job_run(void *arg, volatile int *abort_flag)
{
    list_job_t *j = arg;
    j->err[0] = 0;
    j->n = twitch_fetch_list(j->kind, j->query, j->oauth, j->out, MAX_ITEMS,
                             j->err, sizeof(j->err), abort_flag);
    return j->n;
}

static void tab_load(int t)
{
    tab_t *tb = &g_tabs[t];
    if (tb->busy)
        return;
    tb->need_login = 0;
    if (t == TAB_FOLLOWED && !g_logged) {
        tb->state = LS_READY;
        tb->count = 0;
        tb->need_login = 1;
        return;
    }
    if (t == TAB_FOLLOWED && g_auth_checking) {
        tb->state = LS_LOADING; /* started once the session check finishes */
        return;
    }
    if (t == TAB_SEARCH && !g_search[0]) {
        tb->state = LS_EMPTY;
        tb->count = 0;
        return;
    }
    if (!net_connected()) {
        tb->state = LS_ERROR;
        snprintf(tb->err, sizeof(tb->err), vt_msg("Sin conexion a Internet. Conectate a una red Wi-Fi."));
        LOGW("%s: sin Wi-Fi", tab_name(t));
        return;
    }
    list_job_t *j = &tb->job;
    j->kind = t == TAB_FOLLOWED ? TW_LIST_FOLLOWED : t == TAB_EXPLORE ? TW_LIST_TOP : TW_LIST_SEARCH;
    snprintf(j->query, sizeof(j->query), "%s", g_search);
    snprintf(j->oauth, sizeof(j->oauth), "%s", g_logged ? g_auth.access : "");
    if (task_start(&tb->task, list_job_run, j) == 0) {
        tb->busy = 1;
        if (tb->state != LS_READY)
            tb->state = LS_LOADING;
    }
}

static void tabs_update(void)
{
    for (int t = 0; t < TAB_COUNT; t++) {
        tab_t *tb = &g_tabs[t];
        if (!tb->busy || !task_done(&tb->task))
            continue;
        tb->busy = 0;
        if (tb->job.n >= 0)
            LOGI("%s: %d canales", tab_name(t), tb->job.n);
        else if (tb->job.n == -2)
            LOGI("%s: hace falta iniciar sesion", tab_name(t));
        else if (!tb->task.abort_flag)
            LOGE("%s: %s", tab_name(t), tb->job.err);
        if (tb->job.n >= 0) {
            /* keep the selection on the same channel if it is still there */
            char cur[32] = "";
            if (tb->sel >= 0 && tb->sel < tb->count)
                snprintf(cur, sizeof(cur), "%s", tb->items[tb->sel].login);
            memcpy(tb->items, tb->job.out, sizeof(twitch_stream_t) * (size_t)tb->job.n);
            tb->count = tb->job.n;
            int sel = t == TAB_SEARCH ? tb->sel : 0;
            for (int i = 0; i < tb->count && cur[0]; i++)
                if (!strcmp(tb->items[i].login, cur))
                    sel = i;
            if (sel >= tb->count)
                sel = tb->count - 1;
            if (sel < 0 && t != TAB_SEARCH)
                sel = 0;
            tb->sel = sel;
            tb->state = LS_READY;
        } else if (tb->job.n == -2) {
            tb->state = LS_READY;
            tb->count = 0;
            tb->need_login = 1;
        } else {
            snprintf(tb->err, sizeof(tb->err), "%s", tb->job.err);
            if (tb->state == LS_READY && tb->count > 0)
                toast(tb->err); /* keep showing the old list */
            else
                tb->state = LS_ERROR;
        }
    }
}

/* ----------------------------------------------------- auth checking */

typedef struct {
    auth_t a;
    char err[160];
} auth_job_t;
static auth_job_t g_auth_job;

static int auth_check_run(void *arg, volatile int *abort_flag)
{
    auth_job_t *j = arg;
    return auth_ensure_valid(&j->a, j->err, sizeof(j->err), abort_flag);
}

static void auth_update(void)
{
    if (!g_auth_checking || !task_done(&g_auth_task))
        return;
    g_auth_checking = 0;
    g_auth_last_check=sceKernelGetProcessTimeWide();
    int r = g_auth_task.result;
    if (r == 0)
        LOGI("sesion valida: @%s", g_auth_job.a.login);
    else
        LOGE("comprobacion de sesion fallida (%d): %s", r, g_auth_job.err);
    if (r == 0) {
        g_auth = g_auth_job.a;
        if(g_remember)auth_save(&g_auth, AUTH_FILE);
    } else if (r == -1) {
        g_logged = 0;
        memset(&g_auth, 0, sizeof(g_auth));
        sceIoRemove(AUTH_FILE);
        toast(g_auth_job.err);
    }
    if (g_tabs[TAB_FOLLOWED].state == LS_LOADING || g_tab == TAB_FOLLOWED)
        tab_load(TAB_FOLLOWED);
}

/* ---------------------------------------------------------- drawing */

static int grid_top(int t)
{
    return t == TAB_SEARCH ? HEADER_H + 16 + SEARCH_H + 14 : HEADER_H + 16;
}

static void draw_avatar(const char *url, const char *name, float x, float y, float size)
{
    vita2d_texture *t = img_get(url);
    if (t) {
        ui_image(t, x, y, size, size);
        return;
    }
    vita2d_draw_fill_circle(x + size / 2, y + size / 2, size / 2, C_PANEL2);
    char ini[2] = { name && name[0] ? (char)(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0]) : '?', 0 };
    ui_text_center((int)(x + size / 2), (int)(y + size / 2 + size * 0.17f), C_GREY, size / 40.0f, ini);
}

static void draw_card(const twitch_stream_t *s, int x, int y, int selected)
{
    char line[256], v[32];

    /* TV-inspired full-card focus, without scaling hitboxes or textures. */
    ui_round_rect(x - 5, y - 5, CARD_W + 10, CARD_H + 10, 10,
                  selected ? C_PURPLE : C_PANEL);
    ui_round_rect(x - 2, y - 2, CARD_W + 4, CARD_H + 4, 8,
                  selected ? C_PANEL2 : C_SURFACE);

    /* thumbnail */
    vita2d_texture *tex = s->live ? img_get(s->preview) : NULL;
    if (tex) {
        ui_image(tex, x, y, CARD_W, THUMB_H);
    } else {
        vita2d_draw_rectangle(x, y, CARD_W, THUMB_H, C_PANEL);
        if (!s->live) {
            draw_avatar(s->avatar, s->display, x + CARD_W / 2 - 36, y + 28, 72);
            ui_text_center(x + CARD_W / 2, y + THUMB_H - 22, C_DIM, 0.85f, vt_tr(VT_I_OFFLINE));
        } else {
            ui_text_center(x + CARD_W / 2, y + THUMB_H / 2 + 8, C_DIM, 1.0f, s->display);
        }
    }
    if (s->live) {
        const char *live=vt_tr(VT_I_LIVE);
        int live_w=ui_text_w(0.75f,live)+24;
        if(live_w> CARD_W-16)live_w=CARD_W-16;
        ui_round_rect(x+8,y+8,live_w,24,6,C_RED);
        ui_text_center(x+8+live_w/2,y+25,C_WHITE,0.75f,live);
        ui_fmt_viewers(s->viewers, v, sizeof(v));
        snprintf(line, sizeof(line), vt_msg("%s espectadores"), v);
        int w = ui_text_w(0.75f, line) + 16;
        if(w>CARD_W-16)w=CARD_W-16;
        ui_round_rect(x+CARD_W-w-8,y+THUMB_H-30,w,22,5,C_SHADE);
        ui_text_fit(x+CARD_W-w,y+THUMB_H-13,w-16,C_WHITE,0.75f,line);
    }

    /* text */
    int ty = y + THUMB_H + 10;
    draw_avatar(s->avatar, s->display, x, ty, 36);
    ui_clean_text(s->live && s->title[0] ? s->title : s->display, line, sizeof(line));
    ui_text_fit(x+46,ty+15,CARD_W-50,C_WHITE,0.88f,line);
    char second[160];
    snprintf(second, sizeof(second), "%s%s%s", s->display, s->game[0] ? "  -  " : "", s->game[0] ? s->game : "");
    ui_clean_text(second, line, sizeof(line));
    ui_text_fit(x + 46, ty + 36, CARD_W - 46, C_GREY, 0.78f, line);
}

static void draw_header(void)
{
    vita2d_draw_rectangle(0, 0, SCREEN_W, HEADER_H, C_SURFACE);
    vita2d_draw_rectangle(0, HEADER_H, SCREEN_W, 1, C_PANEL2);

    /* logo */
    ui_round_rect(20, 12, 32, 32, 6, C_PURPLE);
    ui_text_center(36, 36, C_WHITE, 1.0f, "VT");
    ui_text(62, 37, C_WHITE, 1.2f, "VitaTwitch");

    /* Fixed TV-style tab geometry: labels cannot collide with account. */
    ui_text_center(TAB_X-16,35,C_GREY,0.82f,"L");
    for(int t=0;t<TAB_COUNT;t++){
        int x=TAB_X+t*TAB_STEP;
        int active=t==g_tab;
        ui_round_rect(x,9,TAB_W,38,10,active?C_PURPLE:C_PANEL);
        const char *label=tab_name(t);
        float scale=ui_text_w(1.0f,label)>TAB_W-20?0.78f:1.0f;
        int tw=ui_text_w(scale,label);
        if(tw>TAB_W-20)tw=TAB_W-20;
        ui_text_fit(x+(TAB_W-tw)/2,35,TAB_W-20,active?C_WHITE:C_GREY,scale,label);
    }
    ui_text_center(735,35,C_GREY,0.82f,"R");

    /* account chip */
    if (g_logged) {
        char name[80];
        ui_clean_text(g_auth.display[0] ? g_auth.display : g_auth.login, name, sizeof(name));
        ui_round_rect(755,8,185,40,10,C_PANEL);
        draw_avatar(g_auth.avatar,name,902,12,32);
        ui_text_fit(766,34,126,C_WHITE,0.84f,name);
        if(g_auth_checking)ui_spinner(758,28,5,C_GREY);
    } else {
        ui_round_rect(755,8,185,40,10,C_PANEL);
        ui_round_rect(763,17,62,22,5,C_PURPLE_D);
        ui_text_center(794,33,C_WHITE,0.72f,"SELECT");
        ui_text_fit(834,34,96,C_WHITE,0.85f,vt_tr(VT_I_MENU));
    }
}

static void draw_footer(void)
{
    tab_t *tb = &g_tabs[g_tab];
    char ok[48];
    vita2d_draw_rectangle(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, C_SURFACE);
    vita2d_draw_rectangle(0, FOOTER_Y, SCREEN_W, 1, C_PANEL2);
    int x = 20, y = FOOTER_Y + 26;
    if (g_tab == TAB_SEARCH && tb->sel < 0)
        snprintf(ok, sizeof(ok), "%s", vt_tr(VT_I_SEARCH));
    else
        snprintf(ok, sizeof(ok), "%s", vt_tr(VT_I_PLAY));
    x += ui_hint(x, y, g_ok_name, ok);
    x += ui_hint(x, y, "\xE2\x96\xB3" /* triangle */, vt_tr(VT_I_REFRESH));
    if (g_tab == TAB_SEARCH)
        x += ui_hint(x, y, "\xE2\x96\xA1" /* square */, vt_msg("Ir al canal exacto"));
    x += ui_hint(x, y, "L/R", vt_tr(VT_I_TAB));
    x += ui_hint(x, y, "SELECT", vt_tr(VT_I_MENU));
    ui_hint(x, y, "START", vt_tr(VT_I_EXIT));
}

static void draw_center_msg(const char *title, const char *msg, int top)
{
    int cy = top + (FOOTER_Y - top) / 2;
    ui_text_center(SCREEN_W / 2, cy - 10, C_WHITE, 1.15f, title);
    if (msg) {
        int w=ui_text_w(0.9f,msg);
        if(w>SCREEN_W-48)w=SCREEN_W-48;
        ui_text_fit((SCREEN_W-w)/2,cy+22,w,C_GREY,0.9f,msg);
    }
}

static void draw_home(void)
{
    tab_t *tb = &g_tabs[g_tab];
    int top = grid_top(g_tab);
    char buf[160];

    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, C_BG);

    /* Scroll follows d-pad selection; manual touch scrolling may override. */
    if (g_touch_manual_scroll) {
        int rows = (tb->count+COLS-1)/COLS;
        float total=(float)(rows*ROW_PITCH);
        float max=total-(FOOTER_Y-top-8);
        if(max<0)max=0;
        if(tb->scroll>max)tb->scroll=max;
        if(tb->scroll<0)tb->scroll=0;
    } else if (tb->sel >= 0) {
        float row_y = (float)(tb->sel / COLS) * ROW_PITCH;
        float view_h = (float)(FOOTER_Y - top - 8);
        float target = tb->scroll;
        if (row_y < target)
            target = row_y;
        if (row_y + CARD_H > target + view_h)
            target = row_y + CARD_H - view_h;
        tb->scroll += (target - tb->scroll) * 0.3f;
        if (tb->scroll - target < 0.5f && target - tb->scroll < 0.5f)
            tb->scroll = target;
    } else {
        tb->scroll += (0 - tb->scroll) * 0.3f;
    }

    if (tb->count > 0 && tb->state == LS_READY) {
        for (int i = 0; i < tb->count; i++) {
            int x = GRID_X + (i % COLS) * (CARD_W + GAP_X);
            int y = top + (i / COLS) * ROW_PITCH - (int)tb->scroll;
            if (y + CARD_H < HEADER_H || y > FOOTER_Y) {
                /* off screen: preload the next rows (and one above) so they
                 * already have their images when the user scrolls there */
                if (y + CARD_H >= HEADER_H - ROW_PITCH && y <= FOOTER_Y + 3 * ROW_PITCH) {
                    if (tb->items[i].live)
                        img_prefetch(tb->items[i].preview);
                    img_prefetch(tb->items[i].avatar);
                }
                continue;
            }
            draw_card(&tb->items[i], x, y, i == tb->sel);
        }
    } else if (tb->need_login) {
        draw_center_msg(vt_msg("Inicia sesion para ver los canales que sigues"),
                        vt_msg("Pulsa SELECT y sigue las instrucciones (no hace falta escribir la contrasena en la Vita)"), top);
    } else if (tb->state == LS_LOADING) {
        ui_spinner(SCREEN_W / 2, top + (FOOTER_Y - top) / 2 - 10, 16, C_PURPLE);
    } else if (tb->state == LS_ERROR) {
        draw_center_msg(vt_msg("No se pudo cargar"), tb->err, top);
    } else if (g_tab == TAB_SEARCH && !g_search[0]) {
        snprintf(buf, sizeof(buf), vt_msg("Pulsa %s para buscar un canal por nombre"), g_ok_name);
        draw_center_msg(vt_msg("Busca canales de Twitch"), buf, top);
    } else if (tb->state == LS_READY) {
        draw_center_msg(g_tab == TAB_FOLLOWED ? vt_msg("Ninguno de tus canales esta en directo") :
                        g_tab == TAB_SEARCH ? vt_msg("Sin resultados") : vt_msg("No hay directos"),
                        vt_msg("Pulsa Triangulo para actualizar"), top);
    }

    /* search bar */
    if (g_tab == TAB_SEARCH) {
        int y = HEADER_H + 14;
        vita2d_draw_rectangle(0, HEADER_H + 1, SCREEN_W, SEARCH_H + 26, C_BG);
        ui_round_rect(GRID_X, y, SCREEN_W - 2 * GRID_X, SEARCH_H, 8, C_PANEL);
        if (tb->sel < 0)
            ui_frame(GRID_X, y, SCREEN_W - 2 * GRID_X, SEARCH_H, 2, C_PURPLE);
        vita2d_draw_fill_circle(GRID_X + 24, y + 20, 8, C_GREY);
        vita2d_draw_fill_circle(GRID_X + 24, y + 20, 5, C_PANEL);
        vita2d_draw_line(GRID_X + 30, y + 26, GRID_X + 35, y + 32, C_GREY);
        if (g_search[0])
            ui_text(GRID_X + 48, y + 29, C_WHITE, 1.0f, g_search);
        else
            ui_text(GRID_X + 48, y + 29, C_DIM, 1.0f, vt_tr(VT_I_SEARCH));
    }

    if (tb->busy && tb->count > 0)
        ui_spinner(SCREEN_W - 40, HEADER_H + 26, 9, C_PURPLE);

    draw_header();
    draw_footer();

    if (g_toast[0] && sceKernelGetProcessTimeWide() < g_toast_until) {
        int w = ui_text_w(0.9f, g_toast) + 40;
        if (w > SCREEN_W - 40) w = SCREEN_W - 40;
        ui_round_rect((SCREEN_W - w) / 2, FOOTER_Y - 56, w, 40, 8, C_PANEL2);
        ui_text_fit((SCREEN_W - w) / 2 + 20, FOOTER_Y - 30, w - 40, C_WHITE, 0.9f, g_toast);
    }
}

/* Dark box in the middle of the screen; returns its top-left. */
static void draw_modal_box(int w, int h, int *ox, int *oy)
{
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, C_SCRIM);
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    ui_round_rect(x, y, w, h, 12, C_SURFACE);
    vita2d_draw_rectangle(x + 12, y, w - 24, 3, C_PURPLE);
    *ox = x;
    *oy = y;
}

/* ----------------------------------------------------------- modals */

static void wrap_text(int x, int y, int max_w, unsigned color, float scale, const char *s, int max_lines)
{
    char line[256];
    const char *p = s;
    for (int ln = 0; ln < max_lines && *p; ln++) {
        size_t best = 0, i = 0;
        while (p[i]) {
            while (p[i] && p[i] != ' ') i++;
            if (i >= sizeof(line)) break;
            memcpy(line, p, i);
            line[i] = 0;
            if (ui_text_w(scale, line) > max_w && best)
                break;
            best = i;
            if (!p[i]) break;
            i++;
        }
        if (!best) best = strlen(p) < sizeof(line) - 1 ? strlen(p) : sizeof(line) - 1;
        memcpy(line, p, best);
        line[best] = 0;
        ui_text(x, y + ln * (int)(28 * scale), color, scale, line);
        p += best;
        while (*p == ' ') p++;
    }
}

static void modal_message(const char *title, const char *msg)
{
    char hint[32];
    snprintf(hint, sizeof(hint), vt_msg("Aceptar"));
    LOGE("mensaje en pantalla: %s: %s", title, msg);
    for (;;) {
        ui_input_poll(&g_in);
        if (g_in.pressed & (g_btn_ok | g_btn_back))
            return;
        frame_begin();
        draw_home();
        int x, y;
        draw_modal_box(560, 210, &x, &y);
        ui_text(x + 28, y + 48, C_WHITE, 1.2f, title);
        wrap_text(x + 28, y + 90, 504, C_GREY, 0.95f, msg, 3);
        ui_hint(x + 560 - 140, y + 210 - 22, g_ok_name, hint);
        frame_end();
    }
}

/* Runs a task while showing a spinner over the home screen.
 * Returns 0 when it finished, -1 if the user cancelled. */
static int run_with_spinner(task_t *t, task_fn fn, void *arg, const char *msg)
{
    if (task_start(t, fn, arg) < 0)
        return -1;
    for (;;) {
        ui_input_poll(&g_in);
        if (g_in.pressed & g_btn_back) {
            task_cancel(t);
            /* keep drawing while the worker notices the cancellation */
            while (!task_done(t)) {
                frame_begin(); draw_home(); frame_end();
            }
            return -1;
        }
        if (task_done(t))
            return 0;
        tabs_update();
        frame_begin();
        draw_home();
        int x, y;
        draw_modal_box(460, 150, &x, &y);
        ui_spinner(x + 50, y + 62, 14, C_PURPLE);
        ui_text_fit(x + 84, y + 70, 460 - 110, C_WHITE, 1.0f, msg);
        ui_hint(x + 460 - 130, y + 150 - 20, g_back_name, vt_msg("Cancelar"));
        frame_end();
    }
}

static int ime_input(const char *title, const char *initial, char *out, size_t cap, int max_len)
{
    static SceWChar16 title16[64], init16[128], buf16[128];
    vt_utf8_to_utf16((uint16_t*)title16,64,title);
    vt_utf8_to_utf16((uint16_t*)init16,128,initial);
    memset(buf16, 0, sizeof(buf16));

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages =
        SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_SPANISH |
        SCE_IME_LANGUAGE_PORTUGUESE_BR | SCE_IME_LANGUAGE_FRENCH |
        SCE_IME_LANGUAGE_GERMAN | SCE_IME_LANGUAGE_ITALIAN |
        SCE_IME_LANGUAGE_RUSSIAN | SCE_IME_LANGUAGE_JAPANESE |
        SCE_IME_LANGUAGE_KOREAN | SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE |
        SCE_IME_LANGUAGE_TRADITIONAL_CHINESE | SCE_IME_LANGUAGE_TURKISH;
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION | SCE_IME_OPTION_NO_ASSISTANCE;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = title16;
    param.maxTextLength = (SceUInt32)max_len;
    param.initialText = init16;
    param.inputTextBuffer = buf16;

    if (sceImeDialogInit(&param) < 0)
        return 0;

    for (;;) {
        frame_begin();
        draw_home();
        frame_end();
        if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED)
            break;
    }

    SceImeDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    ui_input_poll(&g_in); /* swallow the button that closed the dialog */

    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER)
        return 0;
    size_t n = vt_utf16_to_utf8(out,cap,(const uint16_t*)buf16,128);
    /* trim */
    while (n && out[n - 1] == ' ')
        out[--n] = 0;
    size_t s = 0;
    while (out[s] == ' ')
        s++;
    memmove(out, out + s, n - s + 1);
    return 1;
}

/* ---------------------------------------------------------- quality */

static int quality_modal(const char *display, m3u8_variant_t **list, int n, int fallback)
{
    int sel = -1;
    for (int i = 0; i < n && sel < 0; i++)
        if (g_pref_quality[0] && !strcmp(list[i]->name, g_pref_quality))
            sel = i;
    for (int i = 0; i < n && sel < 0; i++)
        if (list[i]->height == 720 && list[i]->fps < 40)
            sel = i;
    if (sel < 0)
        sel = 0;

    const int row_h = 50, w = 520;
    int vis = n < 5 ? n : 5;
    int top = 0;
    char title[96], name[80];
    ui_clean_text(display, name, sizeof(name));
    snprintf(title, sizeof(title), "%s", name);

    for (;;) {
        ui_input_poll(&g_in);
        if (g_in.pressed & SCE_CTRL_UP) sel = (sel + n - 1) % n;
        if (g_in.pressed & SCE_CTRL_DOWN) sel = (sel + 1) % n;
        if (g_in.pressed & g_btn_ok) return sel;
        if (g_in.pressed & g_btn_back) return -1;
        if (g_in.touch_tap) {
            int box_h = 120 + vis * row_h + (fallback ? 26 : 0);
            int yy = (SCREEN_H-box_h)/2 + 84 + (fallback ? 26 : 0);
            for (int k=0;k<vis;k++)
                if (ui_touch_hit(&g_in,(SCREEN_W-w)/2+16,yy+k*row_h,w-32,row_h-6))
                    return top+k;
        }
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;

        frame_begin();
        draw_home();
        int h = 120 + vis * row_h + (fallback ? 26 : 0);
        int x, y;
        draw_modal_box(w, h, &x, &y);
        ui_text_fit(x + 28, y + 42, w - 56, C_WHITE, 1.15f, title);
        ui_text(x + 28, y + 68, C_GREY, 0.85f, vt_tr(VT_I_QUALITY));
        int ly = y + 84;
        if (fallback) {
            ui_text(x + 28, ly + 14, C_DIM, 0.75f, vt_msg("Este canal no ofrece 480p/720p: se muestran las disponibles."));
            ly += 26;
        }
        for (int k = 0; k < vis; k++) {
            int i = top + k;
            int ry = ly + k * row_h;
            char info[96];
            if (i == sel)
                ui_round_rect(x + 16, ry, w - 32, row_h - 6, 8, C_PURPLE);
            ui_text(x + 32, ry + 30, C_WHITE, 1.05f, list[i]->name);
            snprintf(info, sizeof(info), "%dx%d  -  %.0f fps  -  %.1f Mbps", list[i]->width, list[i]->height,
                     list[i]->fps, list[i]->bandwidth / 1e6);
            ui_text(x + 170, ry + 29, i == sel ? C_WHITE : C_GREY, 0.85f, info);
        }
        ui_hint(x + 28, y + h - 18, g_ok_name, vt_tr(VT_I_PLAY));
        ui_hint(x + 200, y + h - 18, g_back_name, vt_tr(VT_I_BACK));
        frame_end();
    }
}

/* ------------------------------------------------------------- play */

typedef struct {
    char login[32];
    char oauth[64];
    m3u8_variant_t v[M3U8_MAX_VARIANTS];
    int n;
    char err[256];
} variants_job_t;

static int variants_run(void *arg, volatile int *abort_flag)
{
    variants_job_t *j = arg;
    j->err[0] = 0;
    j->n = twitch_get_variants(j->login, j->oauth, j->v, M3U8_MAX_VARIANTS, j->err, sizeof(j->err), abort_flag);
    return j->n;
}

static void play_channel(const char *login, const char *display, const char *title)
{
    static variants_job_t job;
    static task_t task;
    static char err[256];
    char msg[128], clean_title[200];

    if (!net_connected()) {
        modal_message(vt_msg("Sin conexion"), vt_msg("Conecta la Vita a una red Wi-Fi e intentalo de nuevo."));
        return;
    }
    memset(&job, 0, sizeof(job));
    snprintf(job.login, sizeof(job.login), "%s", login);
    snprintf(job.oauth, sizeof(job.oauth), "%s", g_logged ? g_auth.access : "");
    snprintf(msg, sizeof(msg), vt_msg("Abriendo %s..."), display && display[0] ? display : login);
    LOGI("abrir canal %s (%s)", login, g_logged ? "con sesion" : "anonimo");
    if (run_with_spinner(&task, variants_run, &job, msg) < 0) {
        LOGI("cancelado por el usuario");
        return;
    }
    for (int i = 0; i < job.n; i++)
        LOGI("  calidad %s %dx%d %.0f fps %.2f Mbps%s", job.v[i].name, job.v[i].width, job.v[i].height,
             job.v[i].fps, job.v[i].bandwidth / 1e6, job.v[i].audio_only ? " (solo audio)" : "");
    if (job.n <= 0) {
        modal_message(vt_msg("No se pudo abrir el canal"), job.err);
        return;
    }

    m3u8_variant_t *list[M3U8_MAX_VARIANTS];
    int count = 0, fallback = 0;
    for (int i = 0; i < job.n; i++)
        if (!job.v[i].audio_only && job.v[i].height > 0 && job.v[i].height <=720)
            list[count++] = &job.v[i];
    if (count == 0) {
        fallback = 1;
        for (int i = 0; i < job.n; i++)
            if (!job.v[i].audio_only && job.v[i].height > 0 && job.v[i].height <= 1080)
                list[count++] = &job.v[i];
    }
    if (count == 0) {
        modal_message(vt_msg("Sin video"), vt_msg("El stream no tiene calidades de video compatibles."));
        return;
    }

    int sel = g_auto_quality ? vt_profile_pick(list,count,g_profile) : quality_modal(display && display[0] ? display : login, list, count, fallback);
    if (sel < 0)
        return;
    snprintf(g_pref_quality, sizeof(g_pref_quality), "%s", list[sel]->name);
    settings_save();

    ui_clean_text(title ? title : "", clean_title, sizeof(clean_title));
    player_params_t pp;
    int retries=0, reductions=0, r=0, ad_switched=0;
    char ad_backup[M3U8_URL_MAX]={0};
    char wanted[48];
    m3u8_variant_t previous;
    for(;;){
    memset(&pp, 0, sizeof(pp));
    pp.url = list[sel]->url;
    pp.channel = display && display[0] ? display : login;
    pp.title = clean_title;
    pp.quality = list[sel]->name;
    pp.width = list[sel]->width;
    pp.height = list[sel]->height;
    pp.fps = (float)list[sel]->fps;
    pp.font = ui_font;
    pp.btn_back = g_btn_back;
    pp.login = job.login;
    pp.chat_nick = g_logged ? g_auth.login : NULL;
    pp.chat_oauth = g_logged ? g_auth.access : NULL;
    pp.chat_on = &g_chat_on;
    pp.profile=g_profile;pp.auto_quality=g_auto_quality;
    pp.has_lower=vt_profile_lower(list,count,list[sel])>=0;
    pp.diagnostics=g_diag;pp.volume=&g_volume;
    pp.ad_backup_url=ad_switched?NULL:ad_backup;
    {
        char u[160];
        LOGI("reproducir %s en %s: %s", login, pp.quality, log_url(pp.url, u, sizeof(u)));
    }
    scePowerSetArmClockFrequency(vt_profile_clock(g_profile,pp.height,pp.fps));
    r = player_run(&pp, err, sizeof(err));
    scePowerSetArmClockFrequency(333);
    settings_save();
    if(r==PLAYER_AD_SWITCH && !ad_switched && ad_backup[0]){
        ad_switched=1;
        snprintf(list[sel]->url,sizeof(list[sel]->url),"%s",ad_backup);
        LOGI("player: reopening on verified alternate HLS session");
        continue;
    }
    if(r!=PLAYER_RECONNECT&&r!=PLAYER_LOWER_QUALITY)break;
    if(r==PLAYER_RECONNECT&&++retries>2){r=-1;break;}
    if(r==PLAYER_LOWER_QUALITY&&++reductions>4){r=-1;break;}
    previous=*list[sel];
    snprintf(wanted,sizeof(wanted),"%s",previous.name);
    const int lower=(r==PLAYER_LOWER_QUALITY);
    memset(&job,0,sizeof(job));
    snprintf(job.login,sizeof(job.login),"%s",login);
    snprintf(job.oauth,sizeof(job.oauth),"%s",g_logged?g_auth.access:"");
    if(run_with_spinner(&task,variants_run,&job,vt_opt(lower?VT_O_LOWER:VT_O_RECONNECT))<0){r=0;break;}
    if(job.n<=0){snprintf(err,sizeof(err),"%s",job.err);r=-1;break;}
    count=0;
    for(int i=0;i<job.n;i++)if(!job.v[i].audio_only&&job.v[i].height>0&&job.v[i].height<=720)list[count++]=&job.v[i];
    if(!count)for(int i=0;i<job.n;i++)if(!job.v[i].audio_only&&job.v[i].height>0&&job.v[i].height<=1080)list[count++]=&job.v[i];
    if(!count){snprintf(err,sizeof(err),"%s",vt_msg("El stream no tiene calidades de video compatibles."));r=-1;break;}
    sel=-1;
    if(lower)sel=vt_profile_lower(list,count,&previous);
    else for(int i=0;i<count;i++)if(!strcmp(wanted,list[i]->name)){sel=i;break;}
    if(sel<0&&!lower)sel=vt_profile_pick(list,count,g_profile);
    if(sel<0){snprintf(err,sizeof(err),"%s",vt_msg("Reproduccion detenida"));r=-1;break;}
    }
    ui_input_poll(&g_in);
    if (r < 0)
        modal_message(vt_msg("Reproduccion detenida"), err);
    /* viewer counts / thumbnails are stale after watching */
    tab_load(g_tab);
}

/* ---------------------------------------------------------- account */

typedef struct {
    auth_device_t dev;
    auth_t a;
    char err[160];
} login_job_t;

static int login_start_run(void *arg, volatile int *abort_flag)
{
    login_job_t *j = arg;
    return auth_device_start(&j->dev, j->err, sizeof(j->err), abort_flag);
}

static int login_poll_run(void *arg, volatile int *abort_flag)
{
    login_job_t *j = arg;
    int r = auth_device_poll(&j->dev, &j->a, j->err, sizeof(j->err), abort_flag);
    if (r == AUTH_OK) {
        /* fill login / display name / avatar */
        if (auth_ensure_valid(&j->a, j->err, sizeof(j->err), abort_flag) != 0)
            return -1;
    }
    return r;
}

static void account_logged_in(void)
{
    char name[80];
    ui_clean_text(g_auth.display[0] ? g_auth.display : g_auth.login, name, sizeof(name));
    for (;;) {
        ui_input_poll(&g_in);
        if (g_in.pressed & g_btn_back)
            return;
        if (g_in.pressed & SCE_CTRL_TRIANGLE) {
            LOGI("sesion cerrada por el usuario");
            if (g_auth_checking) {
                task_cancel(&g_auth_task);
                task_wait(&g_auth_task);
                g_auth_checking = 0;
                memset(&g_auth_job, 0, sizeof(g_auth_job));
            }
            g_logged = 0;
            memset(&g_auth, 0, sizeof(g_auth));
            sceIoRemove(AUTH_FILE);
            g_tabs[TAB_FOLLOWED].count = 0;
            tab_load(TAB_FOLLOWED);
            toast(vt_msg("Has cerrado sesion"));
            return;
        }
        frame_begin();
        draw_home();
        int x, y;
        draw_modal_box(500, 220, &x, &y);
        draw_avatar(g_auth.avatar, name, x + 28, y + 34, 72);
        ui_text_fit(x + 120, y + 64, 350, C_WHITE, 1.3f, name);
        char at[48];
        snprintf(at, sizeof(at), "@%s", g_auth.login);
        ui_text(x + 120, y + 94, C_GREY, 0.9f, at);
        ui_text(x + 28, y + 150, C_GREY, 0.85f, vt_msg("Sesion iniciada: Siguiendo y anuncios segun tu cuenta."));
        ui_hint(x + 28, y + 196, "\xE2\x96\xB3", vt_msg("Cerrar sesion"));
        ui_hint(x + 260, y + 196, g_back_name, vt_msg("Volver"));
        frame_end();
    }
}

static void account_login(void)
{
    static login_job_t job;
    static task_t task;

    if (!net_connected()) {
        modal_message(vt_msg("Sin conexion"), vt_msg("Conecta la Vita a una red Wi-Fi para iniciar sesion."));
        return;
    }
    memset(&job, 0, sizeof(job));
    LOGI("login: pidiendo codigo de dispositivo");
    if (run_with_spinner(&task, login_start_run, &job, vt_msg("Pidiendo un codigo a Twitch...")) < 0)
        return;
    if (task.result >= 0)
        LOGI("login: codigo recibido, caduca en %d s, consulta cada %d s", job.dev.expires_in, job.dev.interval);
    if (task.result < 0) {
        modal_message(vt_msg("No se pudo iniciar sesion"), job.err);
        return;
    }

    unsigned long long now = sceKernelGetProcessTimeWide();
    unsigned long long expires = now + (unsigned long long)job.dev.expires_in * 1000000ULL;
    unsigned long long next_poll = now + (unsigned long long)job.dev.interval * 1000000ULL;
    int polling = 0;

    /* show the code with spaced letters: "ABCD EFGH" */
    char code[32];
    size_t cl = strlen(job.dev.user_code);
    snprintf(code, sizeof(code), "%.*s %s", (int)(cl / 2), job.dev.user_code, job.dev.user_code + cl / 2);

    for (;;) {
        ui_input_poll(&g_in);
        now = sceKernelGetProcessTimeWide();
        if (g_in.pressed & g_btn_back) {
            if (polling) {
                task_cancel(&task);
                task_wait(&task);
            }
            return;
        }
        if (polling && task_done(&task)) {
            polling = 0;
            int r = task.result;
            if (r == AUTH_SLOW_DOWN)
                LOGW("login: Twitch pide consultar mas despacio");
            if (r == AUTH_OK) {
                LOGI("login: completado como @%s", job.a.login);
                g_auth = job.a;
                g_logged = 1;
                ensure_data_dir();
                if(g_remember)auth_save(&g_auth, AUTH_FILE);
                char hi[96], name[80];
                ui_clean_text(g_auth.display, name, sizeof(name));
                snprintf(hi, sizeof(hi), vt_msg("Hola, %s"), name);
                toast(hi);
                g_tab = TAB_FOLLOWED;
                tab_load(TAB_FOLLOWED);
                return;
            }
            if (r < 0) {
                modal_message(vt_msg("No se pudo iniciar sesion"), job.err);
                return;
            }
            if (r == AUTH_SLOW_DOWN)
                job.dev.interval += 5;
            next_poll = now + (unsigned long long)job.dev.interval * 1000000ULL;
        }
        if (!polling && now >= next_poll) {
            if (now >= expires) {
                modal_message(vt_msg("Codigo caducado"), vt_msg("El codigo ha caducado. Pulsa SELECT para pedir otro."));
                return;
            }
            if (task_start(&task, login_poll_run, &job) == 0)
                polling = 1;
        }

        frame_begin();
        draw_home();
        int x, y, w = 640, h = 330;
        draw_modal_box(w, h, &x, &y);
        ui_text(x + 32, y + 50, C_WHITE, 1.3f, vt_msg("Iniciar sesion en Twitch"));
        ui_text(x + 32, y + 92, C_GREY, 0.95f, vt_msg("1. En tu movil o PC abre:"));
        ui_text(x + 60, y + 128, C_PURPLE, 1.4f, "twitch.tv/activate");
        ui_text(x + 32, y + 168, C_GREY, 0.95f, vt_msg("2. Inicia sesion e introduce este codigo:"));
        ui_round_rect(x + 60, y + 184, 300, 64, 10, C_PANEL2);
        ui_text_center(x + 210, y + 230, C_WHITE, 2.0f, code);
        ui_spinner(x + 400, y + 216, 10, C_PURPLE);
        ui_text(x + 422, y + 222, C_GREY, 0.85f, vt_msg("Esperando..."));
        unsigned long long left = expires > now ? (expires - now) / 1000000ULL : 0;
        char t[64];
        snprintf(t, sizeof(t), vt_msg("El codigo caduca en %llu:%02llu"), left / 60, left % 60);
        ui_text(x + 32, y + h - 50, C_DIM, 0.8f, t);
        ui_hint(x + w - 150, y + h - 22, g_back_name, vt_msg("Cancelar"));
        frame_end();
    }
}

static void account_screen(void)
{
    if (g_logged)
        account_logged_in();
    else
        account_login();
}


/* ----------------------------------------------------- SELECT extras menu */
static void language_screen(void)
{
    int sel=vt_lang_current(), first=sel>7?sel-7:0;
    const int x=198,y=50,w=564,h=440,row=44,visible=8;
    for(;;) {
        ui_input_poll(&g_in);
        if(g_in.pressed&(g_btn_back|SCE_CTRL_SELECT)) return;
        if(g_in.pressed&SCE_CTRL_UP) sel=(sel+VT_LANG_COUNT-1)%VT_LANG_COUNT;
        if(g_in.pressed&SCE_CTRL_DOWN) sel=(sel+1)%VT_LANG_COUNT;
        if(g_in.touch_dy && g_in.touch_dragged) {
            if(g_in.touch_dy>16 && first>0) {first--;sel=first;}
            else if(g_in.touch_dy< -16 && first<VT_LANG_COUNT-visible) {first++;sel=first;}
        }
        int choose=!!(g_in.pressed&g_btn_ok);
        if(g_in.touch_tap) {
            for(int k=0;k<visible;k++)
                if(first+k<VT_LANG_COUNT && ui_touch_hit(&g_in,x+16,y+62+k*row,w-32,row-3)) {
                    sel=first+k;choose=1;break;
                }
        }
        if(choose) {
            vt_lang_set(sel);
            settings_save();
            LOGI("ui language selected: %s",vt_lang_code(sel));
            return;
        }
        if(sel<first) first=sel;
        if(sel>=first+visible) first=sel-visible+1;
        frame_begin();
        draw_home();
        int bx,by; draw_modal_box(w,h,&bx,&by);
        ui_text(x+25,y+42,C_WHITE,1.25f,vt_tr(VT_I_LANGUAGE));
        for(int k=0;k<visible;k++){
            int i=first+k; if(i>=VT_LANG_COUNT)break;
            if(i==sel) ui_round_rect(x+12,y+60+k*row,w-24,row-3,6,C_PURPLE);
            ui_text_fit(x+30,y+90+k*row,w-60,C_WHITE,0.94f,vt_lang_name(i));
        }
        ui_text(x+24,y+h-13,C_GREY,0.7f,vt_msg("Up/Down  |  X  |  Back"));
        frame_end();
    }
}
static void extras_menu(void)
{
    /* All entries share the same hit boxes for touch and controller use. */
    const int x=145,y=20,w=670,h=504,row=39,count=11;
    int sel=0;
    for(;;) {
        ui_input_poll(&g_in);
        if(g_in.pressed&(SCE_CTRL_SELECT|g_btn_back))return;
        if(g_in.pressed&SCE_CTRL_UP)sel=(sel+count-1)%count;
        if(g_in.pressed&SCE_CTRL_DOWN)sel=(sel+1)%count;
        int choose=!!(g_in.pressed&g_btn_ok);
        if(g_in.touch_tap){
            for(int i=0;i<count;i++)
                if(ui_touch_hit(&g_in,x+14,y+68+i*row,w-28,row-4)){sel=i;choose=1;break;}
            if(!choose && !ui_touch_hit(&g_in,x,y,w,h))return;
        }
        if(choose){
            switch(sel){
                case 0: account_screen();break;
                case 1: language_screen();break;
                case 2: ui_touch_enable(!ui_touch_enabled());settings_save();break;
                case 3: g_chat_on=!g_chat_on;settings_save();break;
                case 4: img_clear();toast(vt_msg("Thumbnail cache cleared"));break;
                case 5: modal_message("VitaTwitch", vt_msg("19 languages, touchscreen, Twitch chat, hardware video. Independent VitaTwitch fork."));break;
                case 6:g_profile=(g_profile+1)%VT_PROFILE_COUNT;settings_save();break;
                case 7:g_auto_quality=!g_auto_quality;settings_save();break;
                case 8:g_diag=!g_diag;settings_save();break;
                case 9:g_remember=!g_remember;if(g_remember&&g_logged)auth_save(&g_auth,AUTH_FILE);else sceIoRemove(AUTH_FILE);settings_save();break;
                case 10:return;
            }
        }
        frame_begin();draw_home();
        int bx,by;draw_modal_box(w,h,&bx,&by);
        ui_text(x+26,y+41,C_WHITE,1.2f,vt_tr(VT_I_SETTINGS));
        for(int i=0;i<count;i++){
            int yy=y+68+i*row;
            if(sel==i)ui_round_rect(x+14,yy,w-28,row-4,7,C_PURPLE);
            const char *label=i==0?vt_tr(VT_I_ACCOUNT):i==1?vt_tr(VT_I_LANGUAGE):
                i==2?vt_tr(VT_I_TOUCH):i==3?vt_tr(VT_I_CHAT):
                i==4?vt_tr(VT_I_CLEAR_CACHE):i==5?vt_tr(VT_I_ABOUT):i==6?vt_opt(VT_O_PROFILE):i==7?vt_opt(VT_O_AUTO):i==8?vt_opt(VT_O_DIAG):i==9?vt_opt(VT_O_REMEMBER):vt_tr(VT_I_CLOSE);
            ui_text_fit(x+32,yy+31,w-220,C_WHITE,0.97f,label);
            if(i==1) ui_text_fit(x+w-182,yy+31,157,C_GREY,0.76f,vt_lang_code(vt_lang_current()));
            if(i==6)ui_text_fit(x+w-215,yy+31,190,C_WHITE,0.83f,vt_opt((vt_option_id)(VT_O_ECONOMY+g_profile)));
            if(i==2||i==3||i==7||i==8||i==9)ui_text_fit(x+w-215,yy+31,190,C_WHITE,0.83f,
                (i==2?ui_touch_enabled():i==3?g_chat_on:i==7?g_auto_quality:i==8?g_diag:g_remember)?vt_tr(VT_I_ON):vt_tr(VT_I_OFF));
        }
        frame_end();
    }
}

/* ------------------------------------------------------------- init */

static void log_header(void)
{
    SceDateTime dt;
    memset(&dt, 0, sizeof(dt));
    sceRtcGetCurrentClockLocalTime(&dt);
    SceKernelFwInfo fw;
    memset(&fw, 0, sizeof(fw));
    fw.size = sizeof(fw);
    sceKernelGetSystemSwVersion(&fw);
    LOGI("VitaTwitch %s - %04d-%02d-%02d %02d:%02d:%02d - firmware %s", APP_VERSION, dt.year, dt.month, dt.day,
         dt.hour, dt.minute, dt.second, fw.versionString);
}

static void init_system(void)
{
    ensure_data_dir();
    log_open(LOG_FILE, LOG_PREV_FILE);
    log_header();

    scePowerSetArmClockFrequency(333);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    /* main thread (H.264 decode + drawing) gets core 0; downloads use cores 1-2 */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);

    /* (the H.264 decoder needs no sysmodule: SceVideodec is linked directly;
     *  loading SCE_SYSMODULE_AVCDEC only returned an error) */
    int r1 = sceSysmoduleLoadModule(SCE_SYSMODULE_NET), r2;
    if (r1 < 0)
        LOGE("init: carga del modulo NET 0x%08X", (unsigned)r1);

    static char net_mem[1 * 1024 * 1024];
    SceNetInitParam np;
    np.memory = net_mem;
    np.size = sizeof(net_mem);
    np.flags = 0;
    r1 = sceNetInit(&np);
    r2 = sceNetCtlInit();
    if (r1 < 0 || r2 < 0)
        LOGE("init: sceNetInit 0x%08X, sceNetCtlInit 0x%08X", (unsigned)r1, (unsigned)r2);

    SceAppUtilInitParam ai;
    SceAppUtilBootParam bi;
    memset(&ai, 0, sizeof(ai));
    memset(&bi, 0, sizeof(bi));
    sceAppUtilInit(&ai, &bi);

    SceCommonDialogConfigParam cfg;
    sceCommonDialogConfigParamInit(&cfg);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cfg.language);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cfg.enterButtonAssign);
    sceCommonDialogSetConfigParam(&cfg);
    vt_lang_auto((int)cfg.language); /* saved lang= still overrides this */
    if (cfg.enterButtonAssign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
        g_btn_ok = SCE_CTRL_CIRCLE;
        g_btn_back = SCE_CTRL_CROSS;
        g_ok_name = "O";
        g_back_name = "X";
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    if (sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,SCE_TOUCH_SAMPLING_STATE_START)<0)
        LOGW("touch: failed to enable front panel sampling");

    SceRtcTick tick;
    sceRtcGetCurrentTick(&tick);
    srand((unsigned)tick.tick);

    if (http_global_init() < 0)
        LOGE("init: curl_global_init fallo");

    vita2d_init();
    vita2d_set_clear_color(C_BG);
    ui_font = vita2d_load_default_pgf();
    if (!ui_font)
        LOGE("init: no se pudo cargar la fuente del sistema");
    img_init();

    int st = 0;
    sceNetCtlInetGetState(&st);
    LOGI("init: listo, boton aceptar %s, Wi-Fi %s", g_ok_name, st == SCE_NETCTL_STATE_CONNECTED ? "conectado" : "desconectado");
}

/* ------------------------------------------------------------- main */

static void open_selected(void)
{
    tab_t *tb = &g_tabs[g_tab];
    if (g_tab == TAB_SEARCH && tb->sel < 0) {
        char q[64];
        if (ime_input(vt_msg("Buscar canal"), g_search, q, sizeof(q), 40) && q[0]) {
            snprintf(g_search, sizeof(g_search), "%s", q);
            settings_save();
            tb->count = 0;
            tb->state = LS_LOADING;
            tab_load(TAB_SEARCH);
        }
        return;
    }
    if (tb->sel < 0 || tb->sel >= tb->count)
        return;
    twitch_stream_t s = tb->items[tb->sel]; /* copy: the list may refresh meanwhile */
    play_channel(s.login, s.display, s.title);
}

int main(void)
{
    init_system();
    settings_load();
    for (int t = 0; t < TAB_COUNT; t++)
        g_tabs[t].sel = t == TAB_SEARCH ? -1 : 0;

    if(!g_remember)sceIoRemove(AUTH_FILE);
    if (g_remember && auth_load(&g_auth, AUTH_FILE) == 0) {
        LOGI("sesion guardada de @%s, comprobando", g_auth.login);
        g_logged = 1;
        g_tab = TAB_FOLLOWED;
        g_auth_job.a = g_auth;
        if (net_connected() && task_start(&g_auth_task, auth_check_run, &g_auth_job) == 0)
            g_auth_checking = 1;
    }
    tab_load(g_tab);
    if (g_tab != TAB_EXPLORE)
        tab_load(TAB_EXPLORE);
    if (g_search[0])
        tab_load(TAB_SEARCH);

    unsigned long long last_tick = 0;
    for (;;) {
        ui_input_poll(&g_in);
        auth_update();
        if(g_logged&&!g_auth_checking&&net_connected()&&sceKernelGetProcessTimeWide()-g_auth_last_check>3600000000ULL){
            g_auth_job.a=g_auth;
            if(task_start(&g_auth_task,auth_check_run,&g_auth_job)==0)g_auth_checking=1;
            g_auth_last_check=sceKernelGetProcessTimeWide();
        }
        tabs_update();
        tab_t *tb = &g_tabs[g_tab];
        unsigned p = g_in.pressed;

        if (p & SCE_CTRL_START)
            break;
        if (p & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
            g_tab = (g_tab + (p & SCE_CTRL_RTRIGGER ? 1 : TAB_COUNT - 1)) % TAB_COUNT;
            g_touch_manual_scroll=0;
            if (g_tabs[g_tab].state == LS_EMPTY || g_tabs[g_tab].state == LS_ERROR)
                tab_load(g_tab);
            continue;
        }
        if (p & SCE_CTRL_SELECT) {
            extras_menu();
            continue;
        }
        if(g_in.touch_tap && g_in.touch_y<HEADER_H) {
            if(g_in.touch_x>745) {extras_menu();continue;}
            for(int t=0;t<TAB_COUNT;t++){
                int tx=TAB_X+t*TAB_STEP;
                if(g_in.touch_x>=tx && g_in.touch_x<tx+TAB_W){
                    g_tab=t;g_touch_manual_scroll=0;
                    if(g_tabs[t].state==LS_EMPTY||g_tabs[t].state==LS_ERROR)tab_load(t);
                    break;
                }
            }
            continue;
        }
        if(g_in.touch_down && g_in.touch_dragged && g_in.touch_y>=grid_top(g_tab)
           && g_in.touch_y<FOOTER_Y){
            g_touch_manual_scroll=1;
            tb->scroll-=g_in.touch_dy;
        }
        if(g_in.touch_tap && g_in.touch_y>=HEADER_H && g_in.touch_y<FOOTER_Y){
            int top=grid_top(g_tab);
            if(g_tab==TAB_SEARCH && g_in.touch_y<top){
                tb->sel=-1;g_touch_manual_scroll=0;open_selected();continue;
            }
            int yy=g_in.touch_y-top+(int)tb->scroll;
            int xx=g_in.touch_x-GRID_X;
            if(yy>=0 && xx>=0 && xx<COLS*(CARD_W+GAP_X)){
                int col=xx/(CARD_W+GAP_X), row=yy/ROW_PITCH;
                if(xx%(CARD_W+GAP_X)<CARD_W && yy%ROW_PITCH<CARD_H){
                    int index=row*COLS+col;
                    if(tb->state==LS_READY&&index>=0&&index<tb->count){
                        tb->sel=index;g_touch_manual_scroll=0;open_selected();continue;
                    }
                }
            }
        }
        if (p & SCE_CTRL_TRIANGLE) {
            img_clear(); /* thumbnails are refreshed by Twitch every few minutes */
            tab_load(g_tab);
        }
        if (g_tab == TAB_SEARCH && (p & SCE_CTRL_SQUARE) && g_search[0]) {
            char login[32];
            if (twitch_sanitize_login(g_search, login, sizeof(login)) >= 3)
                play_channel(login, login, NULL);
            else
                toast(vt_msg("Ese nombre no es un canal valido"));
            continue;
        }

        /* grid navigation */
        if(p&(SCE_CTRL_UP|SCE_CTRL_DOWN|SCE_CTRL_LEFT|SCE_CTRL_RIGHT))
            g_touch_manual_scroll=0;
        int n = tb->state == LS_READY ? tb->count : 0;
        int min_sel = g_tab == TAB_SEARCH ? -1 : 0;
        if (p & SCE_CTRL_RIGHT && tb->sel >= 0 && tb->sel + 1 < n) tb->sel++;
        if (p & SCE_CTRL_LEFT && tb->sel > 0) tb->sel--;
        if (p & SCE_CTRL_DOWN) {
            if (tb->sel < 0 && n > 0) tb->sel = 0;
            else if (tb->sel + COLS < n) tb->sel += COLS;
            else if (tb->sel / COLS < (n - 1) / COLS) tb->sel = n - 1;
        }
        if (p & SCE_CTRL_UP) {
            if (tb->sel >= COLS) tb->sel -= COLS;
            else if (tb->sel >= 0) tb->sel = min_sel < 0 ? -1 : tb->sel;
        }
        if (tb->sel >= n && n > 0) tb->sel = n - 1;
        if (n == 0 && g_tab == TAB_SEARCH) tb->sel = -1;

        if (p & g_btn_ok)
            open_selected();

        unsigned long long now = sceKernelGetProcessTimeWide();
        if (now - last_tick > 1000000ULL) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            last_tick = now;
        }

        frame_begin();
        draw_home();
        frame_end();
    }

    for (int t = 0; t < TAB_COUNT; t++)
        if (g_tabs[t].busy) {
            task_cancel(&g_tabs[t].task);
            task_wait(&g_tabs[t].task);
        }
    if (g_auth_checking) {
        task_cancel(&g_auth_task);
        task_wait(&g_auth_task);
    }
    img_shutdown();
    vita2d_fini();
    vita2d_free_pgf(ui_font);
    http_global_term();
    sceNetCtlTerm();
    sceNetTerm();
    LOGI("salida normal");
    log_close();
    sceKernelExitProcess(0);
    return 0;
}
