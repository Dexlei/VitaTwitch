/* Host test for the chat parser and the 7TV/Twitch emote handling.
 *   gcc -DVT_NO_NET -Isrc tests/chat_test.c src/chat.c src/json.c src/text.c src/log.c -o chat_test
 *   chat_test <7tv_global.json> <7tv_user.json> <irc_capture.txt>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "chat.h"

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); failures++; } printf("\n"); } while (0)

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    fread(b, 1, (size_t)n, f);
    b[n] = 0;
    fclose(f);
    return b;
}

static void print_msg(const chat_msg_t *m)
{
    printf("    %s", m->system ? "[sistema] " : "");
    for (int i = 0; i < m->nbadge; i++) {
        const char *k = chat_emote(m->badge[i])->name + 1; /* skip \x02 */
        printf("<%s> ", k);
    }
    printf("%s: ", m->name);
    for (int i = 0; i < m->ntok; i++) {
        if (m->tok[i].emote >= 0) {
            const chat_emote_t *e = chat_emote(m->tok[i].emote);
            printf("[%s] ", e->name[0] == 1 ? "twitch" : e->name);
        } else {
            printf("%s ", m->text + m->tok[i].off);
        }
    }
    printf("\n");
}

static int count_emotes(const chat_msg_t *m)
{
    int n = 0;
    for (int i = 0; i < m->ntok; i++)
        n += m->tok[i].emote >= 0;
    return n;
}

int main(int argc, char **argv)
{
    static irc_line_t l;
    static chat_msg_t m;
    char v[256];

    if (argc < 4) {
        fprintf(stderr, "usage: chat_test global.json user.json irc.txt\n");
        return 2;
    }

    printf("[irc]\n");
    irc_parse("@badge-info=;color=#0000FF;display-name=Pepe;emotes=25:6-10;system-msg=Hola\\sque\\:tal\\\\ :pepe!pepe@pepe.tmi.twitch.tv PRIVMSG #canal :hola  Kappa ah", &l);
    CHECK(!strcmp(l.command, "PRIVMSG") && !strcmp(l.params, "#canal") && !strcmp(l.trailing, "hola  Kappa ah"),
          "PRIVMSG parsed (%s / %s / %s)", l.command, l.params, l.trailing);
    CHECK(irc_tag(&l, "system-msg", v, sizeof(v)) == 0 && !strcmp(v, "Hola que;tal\\"), "tag unescape (%s)", v);
    CHECK(irc_tag(&l, "badge-info", v, sizeof(v)) == 0 && v[0] == 0, "empty tag");
    CHECK(irc_tag(&l, "color-x", v, sizeof(v)) < 0, "missing tag");
    irc_parse("PING :tmi.twitch.tv", &l);
    CHECK(!strcmp(l.command, "PING") && !strcmp(l.trailing, "tmi.twitch.tv"), "PING");

    chat_emotes_reset();
    irc_parse("@color=#0000FF;display-name=Pepe;emotes=25:6-10 :pepe!pepe@pepe.tmi.twitch.tv PRIVMSG #c :hola  Kappa ah", &l);
    CHECK(chat_build_msg(&l, &m) == 0 && m.ntok == 3 && m.tok[1].emote >= 0 &&
          strstr(chat_emote(m.tok[1].emote)->url, "/emoticons/v2/25/"), "Twitch emote by position");
    CHECK((m.color & 0xFF) > 0x70, "dark blue name lightened (0x%08X)", (unsigned)m.color);
    /* code points, not bytes: "ñandú Kappa" -> Kappa at 6-10 */
    irc_parse("@display-name=A;emotes=25:6-10 :a!a@a PRIVMSG #c :\xC3\xB1" "and\xC3\xBA Kappa", &l);
    CHECK(chat_build_msg(&l, &m) == 0 && m.ntok == 2 && m.tok[1].emote >= 0, "emote positions with UTF-8 text");
    irc_parse("@display-name=A;emotes= :a!a@a PRIVMSG #c :\x01" "ACTION baila\x01", &l);
    CHECK(chat_build_msg(&l, &m) == 0 && m.action && !strcmp(m.text, "baila"), "/me message");
    irc_parse("@display-name=A :a!a@a PRIVMSG #c :\xF0\x9F\x98\x80\xF0\x9F\x98\x80", &l);
    CHECK(chat_build_msg(&l, &m) < 0, "emoji-only message dropped (font can't draw it)");
    irc_parse("@login=bob;display-name=Bob;system-msg=Bob\\sse\\sha\\ssuscrito. :tmi.twitch.tv USERNOTICE #c :gracias", &l);
    CHECK(chat_build_msg(&l, &m) == 0 && m.system && m.ntok == 5, "sub notice with text (%d tokens)", m.ntok);

    printf("[7tv]\n");
    clock_t t0 = clock();
    int g = chat_load_7tv(read_file(argv[1]));
    int u = chat_load_7tv(read_file(argv[2]));
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    CHECK(g > 20, "%d global emotes", g);
    CHECK(u > 100, "%d channel emotes (parse of both: %.0f ms on PC)", u, ms);
    int k = chat_find_emote("KEKW", 4);
    CHECK(k < 0 || strstr(chat_emote(k)->url, "cdn.7tv.app"), "lookup KEKW -> %s", k >= 0 ? chat_emote(k)->url : "(not in set)");
    int idx = chat_find_emote("GAMBA", 5);
    CHECK(idx >= 0 && strstr(chat_emote(idx)->url, "1x_static.png"), "animated emote uses the static PNG (%s)",
          idx >= 0 ? chat_emote(idx)->url : "-");
    idx = chat_find_emote("Hmm", 3);
    CHECK(idx >= 0 && strstr(chat_emote(idx)->url, "/1x.png") && chat_emote(idx)->h == 32, "static emote PNG %dx%d",
          idx >= 0 ? chat_emote(idx)->w : 0, idx >= 0 ? chat_emote(idx)->h : 0);
    CHECK(chat_find_emote("hola", 4) < 0, "normal word is not an emote");

    int with_badge_tag = 0, badges_resolved = 0, badge_tags_total = 0;
    if (argc >= 6) {
        printf("[insignias]\n");
        int bg = chat_load_badges(read_file(argv[4]));
        int bc = chat_load_badges(read_file(argv[5]));
        CHECK(bg > 300, "%d global badges", bg);
        CHECK(bc > 10, "%d channel badges", bc);
        int16_t b[CHAT_BADGES_MAX];
        int n = chat_resolve_badges("moderator/1,subscriber/12", b, CHAT_BADGES_MAX);
        CHECK(n == 2 && !strcmp(chat_emote(b[0])->name + 1, "moderator/1") &&
              strstr(chat_emote(b[0])->url, "/badges/v1/"), "moderator + subscriber resolved (%d)", n);
        n = chat_resolve_badges("vip/1,broadcaster/1,premium/1,partner/1", b, CHAT_BADGES_MAX);
        CHECK(n == 3, "at most %d badges per message", CHAT_BADGES_MAX);
        n = chat_resolve_badges("subscriber/9999", b, CHAT_BADGES_MAX);
        CHECK(n == 1, "unknown subscriber version falls back to the generic badge (%s)",
              n ? chat_emote(b[0])->name + 1 : "-");
        CHECK(chat_resolve_badges("", b, CHAT_BADGES_MAX) == 0 && chat_resolve_badges("nope/1", b, 3) == 0,
              "empty / unknown badge tag");
        irc_parse("@badges=moderator/1;display-name=Mod :mod!mod@mod PRIVMSG #c :hola", &l);
        CHECK(chat_build_msg(&l, &m) == 0 && m.nbadge == 1, "message carries its badges");
        CHECK(chat_find_emote("moderator/1", 11) < 0, "badge keys never match chat words");
    }

    printf("[captura real de #xqc]\n");
    char *cap = read_file(argv[3]);
    int lines = 0, msgs = 0, with_emotes = 0, roomstate = 0;
    for (char *line = strtok(cap, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        lines++;
        if (irc_parse(line, &l) < 0)
            continue;
        if (!strcmp(l.command, "ROOMSTATE") && irc_tag(&l, "room-id", v, sizeof(v)) == 0)
            roomstate = atoi(v);
        if (chat_build_msg(&l, &m) == 0) {
            msgs++;
            if (count_emotes(&m)) with_emotes++;
            if (irc_tag(&l, "badges", v, sizeof(v)) == 0 && v[0]) {
                with_badge_tag++;
                int tags = 1;
                for (char *q = v; *q; q++) tags += *q == ',';
                badge_tags_total += tags < CHAT_BADGES_MAX ? tags : CHAT_BADGES_MAX;
                badges_resolved += m.nbadge;
            }
            if (msgs <= 12) print_msg(&m);
        }
    }
    if (argc >= 6)
        CHECK(badge_tags_total > 0 && badges_resolved * 100 / badge_tags_total >= 90,
              "real chat: %d of %d badges resolved (%d messages with badges)", badges_resolved,
              badge_tags_total, with_badge_tag);
    CHECK(roomstate == 71092938, "room-id from ROOMSTATE (%d)", roomstate);
    CHECK(msgs > 20, "%d messages from %d lines, %d with emotes", msgs, lines, with_emotes);
    CHECK(with_emotes > 0, "emotes recognised in real chat");

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
