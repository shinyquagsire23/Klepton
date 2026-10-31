// The web pages the DreamSpace web window may show (KL_JNI_OPEN_URL):
// kl_mono_web_url, the one rule for the page an intent opens and for every
// navigation inside the window afterwards (the app's navigation delegate calls
// the same function), and kl_jni_intent_web, which turns an intent into the
// address that rule is asked about. https only, http upgraded, listed hosts
// only, plain DNS names only. Pure string checks; no guest, no network. In
// `make check`.
#include <stdio.h>
#include <string.h>
#include "guest/kl_mono.h"
#include "jni/kl_jni_int.h"

static int fails;
static void report(int good, const char *what, const char *in, const char *res) {
    if (!good) fails++;
    printf("  %s  %-6s %.70s -> %s\n", good ? "ok  " : "FAIL", what, in ? in : "(null)", res);
}
// want NULL: refused (with a reason); else the address to load.
static void check(int navigation, const char *in, const char *want) {
    char out[2048];
    const char *why = NULL;
    int ok = kl_mono_web_url(in, out, sizeof out, navigation, &why);
    int good = want ? ok && !strcmp(out, want) : !ok && why;
    report(good, navigation ? "[nav]" : "[open]", in, ok ? out : why ? why : "(refused, no reason)");
}
static void expect(const char *in, const char *want) { check(0, in, want); check(1, in, want); }
// An intent, through kl_jni_intent_web and then the rule.
static void intent(const char *act, const char *uri, const char *pkg, const char *want) {
    char web[2048], out[2048];
    const char *why = NULL;
    int ok = kl_jni_intent_web(act, uri, pkg, web, sizeof web) &&
             kl_mono_web_url(web, out, sizeof out, 0, &why);
    int good = want ? ok && !strcmp(out, want) : !ok;
    char in[512];
    snprintf(in, sizeof in, "%s %s pkg=%s", act ? act : "-", uri ? uri : "-", pkg ? pkg : "-");
    report(good, "[int]", in, ok ? out : why ? why : "no web address");
}

int main(void) {
    printf("=== allowed, as given or upgraded ===\n");
    expect("https://open.spotify.com/",            "https://open.spotify.com/");
    expect("http://open.spotify.com/track/1",      "https://open.spotify.com/track/1");
    expect("https://accounts.spotify.com/login",   "https://accounts.spotify.com/login");
    expect("https://spotify.com",                  "https://spotify.com");
    expect("https://www.youtube.com/watch?v=x",    "https://www.youtube.com/watch?v=x");
    expect("https://m.youtube.com/#frag",          "https://m.youtube.com/#frag");
    expect("https://youtu.be/abc",                 "https://youtu.be/abc");
    expect("HTTPS://WWW.Meta.com/quest",           "https://WWW.Meta.com/quest");
    expect("https://www.oculus.com:443/x",         "https://www.oculus.com:443/x");
    expect("https://facebook.com/x",               "https://facebook.com/x");
    expect("https://youtube.com./",                "https://youtube.com./");
    expect("https://xn--80ak6aa92e.youtube.com/",  "https://xn--80ak6aa92e.youtube.com/");   // punycode label UNDER a listed host

    printf("=== refused: schemes, hosts off the list ===\n");
    expect("ovrweb://pwa?packageName=x",           NULL);
    expect("javascript:alert(1)",                  NULL);
    expect("file:///etc/passwd",                   NULL);
    expect("data:text/html,hi",                    NULL);
    expect("https://example.com/",                 NULL);
    expect("https://www.google.com/",              NULL);   // removed from the list
    expect("https://accounts.google.com/",         "https://accounts.google.com/");   // exact host only
    expect("https://google.com/",                  NULL);
    expect("https://x.accounts.google.com/",       NULL);
    expect("https://accounts.google.com.evil.com/", NULL);
    expect("https://www.facebook.com/",            NULL);   // facebook.com: exact only
    expect("https://m.facebook.com/",              NULL);
    expect("https://notyoutube.com/",              NULL);
    expect("https://youtube.com.example.com/",     NULL);

    printf("=== refused: tricks ===\n");
    expect("https://spotify.com@example.com/",     NULL);
    expect("https://user:pw@open.spotify.com/",    NULL);
    expect("https://evil.com\\@youtube.com/",      NULL);
    expect("https://youtube.com\\evil.com/",       NULL);   // bare backslash
    expect("https://you\\tube.com/",               NULL);
    expect("https://\\youtube.com/",               NULL);
    expect("https://you%74ube.com/",               NULL);
    expect("https://yout\xc3\xbc" "be.com/",       NULL);   // IDN, raw UTF-8
    expect("https://www.y\xd0\xbeutube.com/",      NULL);   // Cyrillic o look-alike
    expect("https://xn--yutube-wqf.com/",          NULL);   // punycode look-alike, not under a listed host
    expect("https://142.250.185.78/",              NULL);   // IPv4 literal
    expect("https://142.250.185.78:443/x",         NULL);
    expect("https://2130706433/",                  NULL);   // a bare number (decimal IPv4)
    expect("https://127.0.0.1.youtube.com.evil/",  NULL);
    expect("https://[::1]/",                       NULL);   // IPv6 literals
    expect("https://[::1]:443/",                   NULL);
    expect("https://[2001:db8::1]/",               NULL);
    expect("https://[::ffff:142.250.185.78]/",     NULL);
    expect("https://www..youtube.com/",            NULL);   // empty labels
    expect("https://.youtube.com/",                NULL);
    expect("https://youtube.com../",               NULL);
    expect("https://-a.youtube.com/",              NULL);   // dash labels
    expect("https://a-.youtube.com/",              NULL);
    expect("https://www.-youtube.com/",            NULL);
    expect("https:///path",                        NULL);
    expect("https://",                             NULL);
    expect("https://:443/",                        NULL);
    expect("",                                     NULL);
    expect(NULL,                                   NULL);

    printf("=== lengths ===\n");
    {
        char in[1024], want[1024], label[80];
        memset(label, 'a', 63); label[63] = 0;
        snprintf(in, sizeof in, "https://%s.youtube.com/", label);            // 63: fine
        expect(in, in);
        memset(label, 'a', 64); label[64] = 0;
        snprintf(in, sizeof in, "https://%s.youtube.com/", label);            // 64: too long
        expect(in, NULL);
        // 253 characters in all: fine; 254 (and anything longer): refused.
        char a61[62], b[60];
        memset(a61, 'a', 61); a61[61] = 0;
        memset(b, 'b', 55); b[55] = 0;
        snprintf(in, sizeof in, "https://%s.%s.%s.%s.youtube.com/", a61, a61, a61, b);
        snprintf(want, sizeof want, "%s", in);
        expect(in, want);
        memset(b, 'b', 56); b[56] = 0;
        snprintf(in, sizeof in, "https://%s.%s.%s.%s.youtube.com/", a61, a61, a61, b);
        expect(in, NULL);
        char huge[700];
        memset(huge, 'a', sizeof huge - 1); huge[sizeof huge - 1] = 0;
        for (int i = 50; i < (int)sizeof huge - 1; i += 51) huge[i] = '.';
        snprintf(in, sizeof in, "https://%s.youtube.com/", huge);
        expect(in, NULL);
        // An address that does not fit the caller's buffer is refused, not cut.
        char small[16];
        const char *why = NULL;
        int ok = kl_mono_web_url("https://open.spotify.com/a/long/path", small, sizeof small, 0, &why);
        report(!ok && why, "[open]", "https://open.spotify.com/a/long/path (16-byte buffer)",
               ok ? small : why ? why : "?");
    }

    printf("=== navigation only ===\n");
    check(1, "about:blank",                        "about:blank");
    check(1, "about:srcdoc",                       "about:srcdoc");
    check(0, "about:blank",                        NULL);      // never opened by itself
    check(1, "about:config",                       NULL);
    check(1, "blob:https://www.youtube.com/1234",  NULL);
    check(1, "http://www.youtube.com/redirected",  "https://www.youtube.com/redirected");   // a redirect, upgraded
    check(1, "https://consent.youtube.com/m?x=1",  "https://consent.youtube.com/m?x=1");
    check(1, "https://accounts.google.com/ServiceLogin", "https://accounts.google.com/ServiceLogin");   // YouTube sign-in redirect
    check(1, "https://www.google.com/",            NULL);
    check(1, "https://x.accounts.google.com/",     NULL);
    check(1, "https://accounts.google.com.evil.com/", NULL);

    printf("=== intents (kl_jni_intent_web, then the rule) ===\n");
    const char *V = "android.intent.action.VIEW";
    intent(V, "https://open.spotify.com/", NULL,    "https://open.spotify.com/");
    intent(V, "http://youtu.be/x", NULL,            "https://youtu.be/x");
    intent(V, "https://example.com/", NULL,         NULL);
    intent(V, "ovrweb://pwa?packageName=com.spotify.horizon", "com.oculus.browser",
           "https://open.spotify.com/");
    intent(V, "ovrweb://pwa?url=https%3A%2F%2Fwww.youtube.com%2Fwatch%3Fv%3Dabc", NULL,
           "https://www.youtube.com/watch?v=abc");                // url= decoded, round trip
    intent(V, "ovrweb://pwa?x=1&url=http%3A%2F%2Fopen.spotify.com%2F", NULL,
           "https://open.spotify.com/");
    intent(V, "ovrweb://pwa?url=https%3A%2F%2Fevil.com%2F", NULL, NULL);
    intent(V, "ovrweb://pwa?url=https%3A%2F%2Fspotify.com%40evil.com%2F", NULL, NULL);
    intent(V, "ovrweb://pwa?url=javascript%3Aalert(1)", NULL, NULL);
    intent(V, "ovrweb://pwa?packageName=com.oculus.vrshell", NULL, NULL);
    intent(NULL, NULL, "com.google.android.apps.youtube.vr.oculus", "https://www.youtube.com/");
    intent(NULL, NULL, "com.oculus.browser",        NULL);      // no longer mapped (google.com is off the list)
    intent(NULL, NULL, "com.microsoft.rdc",         NULL);
    intent("android.intent.action.MAIN", "https://open.spotify.com/", NULL, NULL);   // not VIEW

    printf(fails ? "FAIL: %d web-address case(s)\n"
                 : "PASS: web window addresses: https only, listed hosts only, plain names only.\n", fails);
    return fails != 0;
}
