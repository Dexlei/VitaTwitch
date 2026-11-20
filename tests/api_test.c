/* Host tests for the JSON parser, the Twitch list queries and the login flow.
 *   gcc -DVT_NO_NET -Isrc tests/api_test.c src/json.c src/twitch.c src/auth.c src/m3u8.c -o api_test
 *   api_test bodies <outdir>        writes the GQL request bodies built by the C code
 *   api_test parse top.json search.json followed_anon.json device.json poll.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "auth.h"
#include "json.h"
#include "twitch.h"

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

static void write_file(const char *dir, const char *name, const char *text)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static void test_json(void)
{
    printf("[json]\n");
    json_t *j = json_parse("{\"a\":{\"b\":[1,2,{\"c\":\"x\\u00e9\\ud83d\\ude00\\\"y\"}]},\"t\":true,\"n\":null,\"f\":-1.5e2}");
    CHECK(j != NULL, "parses nested document");
    CHECK(json_num(j, "a.b.1", 0) == 2, "array index in path");
    CHECK(!strcmp(json_str(j, "a.b.2.c", ""), "x\xC3\xA9\xF0\x9F\x98\x80\"y"), "unicode escapes + surrogate pair");
    CHECK(json_num(j, "t", 0) == 1 && json_num(j, "f", 0) == -150, "bool and number");
    CHECK(json_path(j, "n") && json_path(j, "n")->type == JSON_NULL, "null");
    CHECK(json_str(j, "missing.x", "def")[0] == 'd', "missing path -> default");
    json_free(j);
    CHECK(json_parse("{\"a\":[1,2") == NULL, "truncated input rejected");
    CHECK(json_parse("[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]") == NULL,
          "excessive nesting rejected");

    char esc[64];
    twitch_json_escape("a\"b\\c\n", esc, sizeof(esc));
    CHECK(!strcmp(esc, "a\\\"b\\\\c\\n"), "json escape (%s)", esc);
    json_t *round = NULL;
    char body[1024];
    twitch_build_list_body(TW_LIST_SEARCH, "ib\"ai", body, sizeof(body));
    round = json_parse(body);
    CHECK(round && !strcmp(json_str(round, "variables.q", ""), "ib\"ai"), "search body is valid JSON with the query");
    json_free(round);
    for (int k = 0; k < 3; k++) {
        twitch_build_list_body((twitch_list_t)k, "x", body, sizeof(body));
        round = json_parse(body);
        CHECK(round && json_str(round, "query", NULL), "list body %d is valid JSON", k);
        json_free(round);
    }
    twitch_build_gql_body("kaicenat", body, sizeof(body));
    round = json_parse(body);
    CHECK(round && !strcmp(json_str(round, "variables.login", ""), "kaicenat"), "playback body is valid JSON");
    json_free(round);
}

static void print_list(const twitch_stream_t *s, int n, int max)
{
    for (int i = 0; i < n && i < max; i++)
        printf("    %-16s live=%d %7d  %-24.24s %.40s\n", s[i].login, s[i].live, s[i].viewers, s[i].game, s[i].title);
}

int main(int argc, char **argv)
{
    char err[256];
    static twitch_stream_t list[100];

    if (argc >= 3 && !strcmp(argv[1], "bodies")) {
        char body[1024];
        twitch_build_list_body(TW_LIST_TOP, NULL, body, sizeof(body));
        write_file(argv[2], "body_top.json", body);
        twitch_build_list_body(TW_LIST_SEARCH, "ibai", body, sizeof(body));
        write_file(argv[2], "body_search.json", body);
        twitch_build_list_body(TW_LIST_FOLLOWED, NULL, body, sizeof(body));
        write_file(argv[2], "body_followed.json", body);
        twitch_build_gql_body("kaicenat", body, sizeof(body));
        write_file(argv[2], "body_playback.json", body);
        return 0;
    }
    if (argc < 7 || strcmp(argv[1], "parse")) {
        fprintf(stderr, "usage: api_test bodies <dir> | api_test parse top search followed device poll\n");
        return 2;
    }

    test_json();

    printf("[explorar]\n");
    int n = twitch_parse_list(read_file(argv[2]), TW_LIST_TOP, list, 100, err, sizeof(err));
    print_list(list, n, 4);
    CHECK(n >= 20, "%d top streams", n);
    CHECK(n > 0 && list[0].live && list[0].viewers > 0 && list[0].login[0], "first entry is live with viewers");
    CHECK(n > 0 && strstr(list[0].preview, "320x180") != NULL, "320x180 thumbnail url");
    CHECK(n > 0 && !strncmp(list[0].avatar, "https://", 8), "avatar url");

    printf("[buscar]\n");
    n = twitch_parse_list(read_file(argv[3]), TW_LIST_SEARCH, list, 100, err, sizeof(err));
    print_list(list, n, 4);
    CHECK(n > 0 && !strcmp(list[0].login, "ibai"), "search 'ibai' -> first result ibai (%d results)", n);

    printf("[siguiendo sin login]\n");
    n = twitch_parse_list(read_file(argv[4]), TW_LIST_FOLLOWED, list, 100, err, sizeof(err));
    CHECK(n == -2, "anonymous followed -> needs login (%s)", err);

    printf("[siguiendo: errores de Twitch]\n");
    n = twitch_parse_list("{\"errors\":[{\"message\":\"service error\",\"path\":[\"currentUser\",\"followedLiveUsers\"]}],"
                          "\"data\":{\"currentUser\":{\"followedLiveUsers\":null}}}",
                          TW_LIST_FOLLOWED, list, 100, err, sizeof(err));
    CHECK(n == -1 && strstr(err, "currentUser.followedLiveUsers"), "service error -> -1 with path (%s)", err);
    n = twitch_parse_list("{\"errors\":[{\"message\":\"service error\",\"path\":[\"currentUser\",\"followedLiveUsers\",\"edges\",1,\"node\",\"stream\",\"game\"]}],"
                          "\"data\":{\"currentUser\":{\"followedLiveUsers\":{\"edges\":["
                          "{\"node\":{\"login\":\"a\",\"displayName\":\"A\",\"stream\":{\"viewersCount\":5,\"title\":\"t\"}}},"
                          "{\"node\":{\"login\":\"b\",\"displayName\":\"B\",\"stream\":{\"viewersCount\":9,\"game\":null}}}]}}}}",
                          TW_LIST_FOLLOWED, list, 100, err, sizeof(err));
    CHECK(n == 2 && list[1].viewers == 9, "partial error keeps the list (%d channels)", n);
    n = twitch_parse_list("{\"errors\":[{\"message\":\"service error\"}],\"data\":{\"currentUser\":null}}",
                          TW_LIST_FOLLOWED, list, 100, err, sizeof(err));
    CHECK(n == -1, "error + null user is an error, not 'log in again' (%d)", n);

    json_t *round;
    char cursor[256] = "", fbody[1536];
    int cnt = 0, has_next = 0;
    twitch_build_follows_body(NULL, fbody, sizeof(fbody));
    round = json_parse(fbody);
    CHECK(round && json_path(round, "variables.after") && json_path(round, "variables.after")->type == JSON_NULL,
          "follows body, first page");
    json_free(round);
    twitch_build_follows_body("abc\"1", fbody, sizeof(fbody));
    round = json_parse(fbody);
    CHECK(round && !strcmp(json_str(round, "variables.after", ""), "abc\"1"), "follows body with cursor");
    json_free(round);
    int r0 = twitch_parse_follows("{\"data\":{\"currentUser\":{\"follows\":{\"edges\":["
                                  "{\"cursor\":\"c1\",\"node\":{\"login\":\"off\",\"stream\":null}},"
                                  "{\"cursor\":\"c2\",\"node\":{\"login\":\"low\",\"stream\":{\"viewersCount\":3}}},"
                                  "{\"cursor\":\"c3\",\"node\":{\"login\":\"high\",\"stream\":{\"viewersCount\":300}}}],"
                                  "\"pageInfo\":{\"hasNextPage\":true}}}}}",
                                  list, 100, &cnt, cursor, sizeof(cursor), &has_next, err, sizeof(err));
    twitch_sort_by_viewers(list, cnt);
    CHECK(r0 == 0 && cnt == 2 && has_next && !strcmp(cursor, "c3") && !strcmp(list[0].login, "high"),
          "follows page: only live, sorted, next cursor %s", cursor);

    printf("[login]\n");
    auth_device_t dev;
    CHECK(auth_parse_device(read_file(argv[5]), &dev, err, sizeof(err)) == 0, "device code parsed: %s at %s every %ds",
          dev.user_code, dev.uri, dev.interval);
    auth_t a;
    memset(&a, 0, sizeof(a));
    int r = auth_parse_token(read_file(argv[6]), &a, err, sizeof(err));
    CHECK(r == AUTH_PENDING, "poll before activation -> pending (%d)", r);
    r = auth_parse_token("{\"access_token\":\"abc\",\"refresh_token\":\"def\",\"scope\":[\"chat:read\"],\"token_type\":\"bearer\"}",
                         &a, err, sizeof(err));
    CHECK(r == AUTH_OK && !strcmp(a.access, "abc") && !strcmp(a.refresh, "def"), "token response parsed");
    r = auth_parse_token("{\"status\":400,\"message\":\"expired_token\"}", &a, err, sizeof(err));
    CHECK(r < 0, "expired code -> error (%s)", err);
    CHECK(auth_parse_validate("{\"client_id\":\"x\",\"login\":\"someone\",\"scopes\":[],\"user_id\":\"123\",\"expires_in\":100}", &a) == 0 &&
          !strcmp(a.login, "someone") && !strcmp(a.user_id, "123"), "validate response parsed");
    strcpy(a.display, "Some One");
    strcpy(a.avatar, "https://x/y.png");
    auth_save(&a, "auth_test.txt");
    auth_t b;
    CHECK(auth_load(&b, "auth_test.txt") == 0 && !memcmp(&a, &b, sizeof(a)), "auth file round trip");
    remove("auth_test.txt");

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
