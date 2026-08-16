// Unity's on-screen keyboard: the gate, the channel, the drain
//
// One family of the synthetic JNIEnv's Java classes. The mechanism (registries,
// dispatch, id interning) is kl_jni.c; this file owns implementations and the
// binding table that names them. See runtime/jni/kl_jni_int.h for the seam.
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_target.h"
#include "kl_env.h"
#include "kl_ovrp.h"
#include "kl_avdec.h"
#include "kl_egl.h"
#include "kl_ndk.h"
#include "kl_va.h"
#include "kl_jni_int.h"

// ---- the on-screen keyboard (Unity's soft input) ---------------------------
//
// A text field is the one UI element a guest cannot draw for itself: on Android
// it hands the job to the platform, and the platform hands the result back. Six
// Java methods go out and eight natives come back, and NONE of it is invented
// here — every rule below is read out of this APK's own
// com/unity3d/player/UnityPlayer.smali and the dialog class B it builds, so the
// guest sees the sequence its own Java would have produced:
//
//   showSoftInput(text, type, autocorrect, multiline, secure, alert,
//                 placeholder, charLimit, hideInputField, ...)
//        -> constructs the dialog. Its layout listener (class x) then reports
//           visible=true and the area the keyboard covers.
//   hideSoftInput()
//        -> area = the EMPTY rect, visible=false, and if a dialog existed,
//           dismiss it and nativeReportKeyboardConfigChanged (UnityPlayer$d).
//   reportSoftInputStr(s, action, canceled)   [dialog -> player]
//        -> action 1 hides first; then canceled ? nativeSoftInputCanceled
//           : s ? nativeSetInputString(s); then action 1 -> nativeSoftInputClosed
//           (UnityPlayer$i, and the ORDER is that method's, not a choice).
//   setSoftInputStr(s)   -> the dialog's setText, whose TextWatcher reports the
//                           string and the selection straight back out.
//   setHideInputField(b) -> dialog-only (UnityPlayer$g touches nothing native).
//
// The three booleans are named from the inputType bits the dialog ORs from
// them: 0x8000 AUTO_CORRECT / 0x80000 NO_SUGGESTIONS, 0x20000 MULTI_LINE, and
// 0x80 VARIATION_PASSWORD. `secure` is the one with a consequence outside the
// guest — a frontend must not echo a password into a log.
//
// The reports are queued rather than dispatched inline, because on Android they
// are queueGLThreadEvent()s: they run on the RENDER thread, not on whichever
// thread asked. Here the caller is a frontend on its own thread (the viewer's
// SDL loop, the visionOS UI), so dispatching inline would call the guest's
// natives from a thread the engine never expects them on. kl_jni_drain_ui_tasks
// runs the queue, and both Unity drivers already call that from the pump thread
// immediately after nativeRender — the UI thread and the GL thread are the same
// thread here, which is what makes one drain point correct for both queues.
#define KLJ_SOFT_TEXT_MAX 1024
#define KLJ_SOFT_QUEUE    32

enum {
    KLJ_SOFT_STR = 1,   // nativeSetInputString(s)
    KLJ_SOFT_SEL,       // nativeSetInputSelection(start, length)
    KLJ_SOFT_AREA,      // nativeSetInputArea(l, t, r, b)
    KLJ_SOFT_VISIBLE,   // nativeSetKeyboardIsVisible(b)
    KLJ_SOFT_CLOSED,    // nativeSoftInputClosed()
    KLJ_SOFT_CANCELED,  // nativeSoftInputCanceled()
    KLJ_SOFT_CFG,       // nativeReportKeyboardConfigChanged()
};

typedef struct {
    int  kind, a, b, c, d;
    char s[KLJ_SOFT_TEXT_MAX];
} klj_soft_ev;

static struct {
    pthread_mutex_t mu;
    void       *player;          // the UnityPlayer the guest called us on
    int         open;            // showSoftInput seen, not yet closed
    int         visible;
    int         hide_field, secure, multiline, autocorrect;
    int         type, limit;
    char        text[KLJ_SOFT_TEXT_MAX];
    char        placeholder[256];
    klj_soft_ev q[KLJ_SOFT_QUEUE];
    int         qh, qn;
    unsigned    dropped;
    unsigned    opens;
} g_soft = { .mu = PTHREAD_MUTEX_INITIALIZER };

// Caller holds g_soft.mu. A full queue drops the OLDEST report, and says so:
// the newest string is the one the guest needs, and a silent drop here is a
// text field that stops updating for no visible reason.
static void klj_soft_push(int kind, int a, int b, int c, int d, const char *s) {
    if (g_soft.qn == KLJ_SOFT_QUEUE) {
        g_soft.qh = (g_soft.qh + 1) % KLJ_SOFT_QUEUE;
        g_soft.qn--;
        if (!g_soft.dropped++)
            KLJ_LOG("soft input: report queue full — dropping the oldest. "
                    "Nothing is draining it; is the pump running?");
    }
    klj_soft_ev *e = &g_soft.q[(g_soft.qh + g_soft.qn) % KLJ_SOFT_QUEUE];
    e->kind = kind; e->a = a; e->b = b; e->c = c; e->d = d;
    e->s[0] = '\0';
    if (s) { strncpy(e->s, s, sizeof e->s - 1); e->s[sizeof e->s - 1] = '\0'; }
    g_soft.qn++;
}

// The string and the caret together, which is what every text change reports:
// the dialog's TextWatcher fires reportSoftInputStr and then
// reportSoftInputSelection, and a caret left where it was would put the next
// character in the middle of the old text. Caller holds the lock.
static void klj_soft_push_text(void) {
    int len = (int)strlen(g_soft.text);
    klj_soft_push(KLJ_SOFT_STR, 0, 0, 0, 0, g_soft.text);
    klj_soft_push(KLJ_SOFT_SEL, len, 0, 0, 0, NULL);
}

// Run the queue. Called from kl_jni_drain_ui_tasks, i.e. on the pump thread
// between frames — see the section comment. One event is taken per turn of the
// loop and the lock is DROPPED before the guest runs: nativeSetInputString
// reaches managed code, which is allowed to call setSoftInputStr straight back
// at us, and that would deadlock against a lock held across the dispatch.
unsigned klj_drain_soft_input(void) {
    unsigned ran = 0;
    for (;;) {
        klj_soft_ev e;
        pthread_mutex_lock(&g_soft.mu);
        if (!g_soft.qn) { pthread_mutex_unlock(&g_soft.mu); break; }
        e = g_soft.q[g_soft.qh];
        g_soft.qh = (g_soft.qh + 1) % KLJ_SOFT_QUEUE;
        g_soft.qn--;
        void *self = g_soft.player;
        pthread_mutex_unlock(&g_soft.mu);

        // Resolved per event and not cached: the cache would have to be
        // populated at the FIRST event, and a lookup that ran before the guest
        // reached RegisterNatives would pin a NULL for the rest of the run —
        // a keyboard that is silently dead, from one early miss. The scan is a
        // few dozen entries and this runs at typing speed.
        static const char *const names[] = {
            [KLJ_SOFT_STR]      = "nativeSetInputString",
            [KLJ_SOFT_SEL]      = "nativeSetInputSelection",
            [KLJ_SOFT_AREA]     = "nativeSetInputArea",
            [KLJ_SOFT_VISIBLE]  = "nativeSetKeyboardIsVisible",
            [KLJ_SOFT_CLOSED]   = "nativeSoftInputClosed",
            [KLJ_SOFT_CANCELED] = "nativeSoftInputCanceled",
            [KLJ_SOFT_CFG]      = "nativeReportKeyboardConfigChanged",
        };
        void *f = (e.kind >= KLJ_SOFT_STR && e.kind <= KLJ_SOFT_CFG)
                ? kl_jni_native("com/unity3d/player/UnityPlayer", names[e.kind], NULL)
                : NULL;
        if (!f || !self) {
            KLJ_LOG("soft input: report %d has no registered native (or no player) "
                    "— dropped", e.kind);
            continue;
        }
        void *env = kl_jni_env();
        // The JVM gives every native call its own local frame; the drain is a
        // host->guest entry like any other, so it plays the same half (kl_jni.h).
        kl_jni_local_frame_push();
        switch (e.kind) {
        case KLJ_SOFT_STR:
            ((void (*)(void *, void *, void *))f)(env, self, kl_jni_new_string(e.s));
            break;
        case KLJ_SOFT_SEL:
            ((void (*)(void *, void *, kl_jint, kl_jint))f)(env, self, e.a, e.b);
            break;
        case KLJ_SOFT_AREA:
            ((void (*)(void *, void *, kl_jint, kl_jint, kl_jint, kl_jint))f)
                (env, self, e.a, e.b, e.c, e.d);
            break;
        case KLJ_SOFT_VISIBLE:
            // jboolean is a BYTE, not an int: the guest reads w2 as an 8-bit
            // value, so passing an int would leave the high bits to chance.
            ((void (*)(void *, void *, uint8_t))f)(env, self, (uint8_t)(e.a != 0));
            break;
        default:
            ((void (*)(void *, void *))f)(env, self);
            break;
        }
        kl_jni_local_frame_pop();
        ran++;
    }
    return ran;
}

// UnityPlayer.hideSoftInput() — UnityPlayer$d, transcribed: the empty area, not
// visible, and then the dialog's own teardown. The config-changed report is
// inside the "there was a dialog" arm in the guest's code, so a hide with no
// keyboard open is the first two reports and nothing else.
static klj_val klj_UnityPlayer_hideSoftInput(void *env, void *self,
                                             const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    pthread_mutex_lock(&g_soft.mu);
    if (self) g_soft.player = self;
    int was_open = g_soft.open;
    g_soft.open = 0;
    g_soft.visible = 0;
    klj_soft_push(KLJ_SOFT_AREA, 0, 0, 0, 0, NULL);
    klj_soft_push(KLJ_SOFT_VISIBLE, 0, 0, 0, 0, NULL);
    if (was_open) klj_soft_push(KLJ_SOFT_CFG, 0, 0, 0, 0, NULL);
    pthread_mutex_unlock(&g_soft.mu);
    KLJ_LOG("UnityPlayer.hideSoftInput()%s", was_open ? "" : " — none was open");
    return (klj_val){.j = 0};
}

static klj_val klj_UnityPlayer_showSoftInput(void *env, void *self,
                                             const klj_val *a, int n) {
    (void)env;
    const char *text = n > 0 ? klj_str(a[0].l) : NULL;
    const char *hint = n > 6 ? klj_str(a[6].l) : NULL;
    pthread_mutex_lock(&g_soft.mu);
    if (self) g_soft.player = self;
    g_soft.open        = 1;
    g_soft.visible     = 1;
    g_soft.type        = n > 1 ? (int)a[1].j : 0;
    g_soft.autocorrect = n > 2 ? (int)a[2].j : 0;
    g_soft.multiline   = n > 3 ? (int)a[3].j : 0;
    g_soft.secure      = n > 4 ? (int)a[4].j : 0;
    g_soft.limit       = n > 7 ? (int)a[7].j : 0;
    g_soft.hide_field  = n > 8 ? (int)a[8].j : 0;
    g_soft.text[0] = '\0';
    if (text) { strncpy(g_soft.text, text, sizeof g_soft.text - 1);
                g_soft.text[sizeof g_soft.text - 1] = '\0'; }
    g_soft.placeholder[0] = '\0';
    if (hint) { strncpy(g_soft.placeholder, hint, sizeof g_soft.placeholder - 1);
                g_soft.placeholder[sizeof g_soft.placeholder - 1] = '\0'; }
    g_soft.opens++;
    // What the dialog's layout listener (class x) reports once it is up: the
    // keyboard is visible, and it covers an area. The area is the guest's own
    // idea of a keyboard's height and it uses it to move the field out from
    // under it — there is no on-screen keyboard here at all, so the honest rect
    // is the empty one, which is what it gets when the dialog goes away too.
    klj_soft_push(KLJ_SOFT_VISIBLE, 1, 0, 0, 0, NULL);
    klj_soft_push(KLJ_SOFT_AREA, 0, 0, 0, 0, NULL);
    pthread_mutex_unlock(&g_soft.mu);
    KLJ_LOG("UnityPlayer.showSoftInput(type=%d%s%s%s limit=%d placeholder=\"%s\") "
            "— a frontend can type into it now",
            g_soft.type, g_soft.secure ? " secure" : "",
            g_soft.multiline ? " multiline" : "",
            g_soft.autocorrect ? " autocorrect" : "", g_soft.limit,
            g_soft.placeholder);
    return (klj_val){.j = 0};
}

// The guest setting the text itself (TouchScreenKeyboard.text = "..."). The
// dialog's setText fires its own TextWatcher, so this reports straight back
// out — a round trip that looks redundant and is the guest's own shape.
static klj_val klj_UnityPlayer_setSoftInputStr(void *env, void *self,
                                               const klj_val *a, int n) {
    (void)env;
    const char *s = n > 0 ? klj_str(a[0].l) : NULL;
    if (!s) return (klj_val){.j = 0};          // UnityPlayer$e drops a null
    pthread_mutex_lock(&g_soft.mu);
    if (self) g_soft.player = self;
    if (!g_soft.open) {                        // ...and so it does with no dialog
        pthread_mutex_unlock(&g_soft.mu);
        return (klj_val){.j = 0};
    }
    strncpy(g_soft.text, s, sizeof g_soft.text - 1);
    g_soft.text[sizeof g_soft.text - 1] = '\0';
    klj_soft_push_text();
    pthread_mutex_unlock(&g_soft.mu);
    return (klj_val){.j = 0};
}

static klj_val klj_UnityPlayer_setHideInputField(void *env, void *self,
                                                 const klj_val *a, int n) {
    (void)env;
    pthread_mutex_lock(&g_soft.mu);
    if (self) g_soft.player = self;
    g_soft.hide_field = n > 0 ? (int)a[0].j : 0;
    pthread_mutex_unlock(&g_soft.mu);
    // Recorded and nothing else, which is the whole of UnityPlayer$g: it asks
    // the dialog to hide its own EditText so the guest can draw the text
    // itself. No native is reached, so there is nothing here to get wrong.
    KLJ_LOG("UnityPlayer.setHideInputField(%d) — recorded; the guest draws the "
            "field itself", g_soft.hide_field);
    return (klj_val){.j = 0};
}

// getKeyboardLayout() — the IME's layout, from the dialog. The guest's own code
// answers null when there is no dialog, and there is no IME here to name even
// when there is one, so null is the answer it is already written to accept.
static klj_val klj_UnityPlayer_getKeyboardLayout(void *env, void *self,
                                                 const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// The clipboard, which is a text field's other half — GUIUtility.systemCopyBuffer
// on the managed side, and the way a password manager gets a password into a
// login box. APP-LOCAL: this is what setClipboardText put there, not the host
// pasteboard, because reaching the real one means AppKit here and UIKit on
// device and this file is shared. A copy then a paste inside the guest works,
// which is the pair the guest itself exercises; pasting from the host does not,
// and that is a named gap rather than a silent one.
static char g_clipboard[KLJ_SOFT_TEXT_MAX];

static klj_val klj_UnityPlayer_setClipboardText(void *env, void *self,
                                                const klj_val *a, int n) {
    (void)env; (void)self;
    const char *s = n > 0 ? klj_str(a[0].l) : NULL;
    pthread_mutex_lock(&g_soft.mu);
    g_clipboard[0] = '\0';
    if (s) { strncpy(g_clipboard, s, sizeof g_clipboard - 1);
             g_clipboard[sizeof g_clipboard - 1] = '\0'; }
    pthread_mutex_unlock(&g_soft.mu);
    return (klj_val){.j = 0};
}

static klj_val klj_UnityPlayer_getClipboardText(void *env, void *self,
                                                const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    pthread_mutex_lock(&g_soft.mu);
    char copy[KLJ_SOFT_TEXT_MAX];
    memcpy(copy, g_clipboard, sizeof copy);
    pthread_mutex_unlock(&g_soft.mu);
    // Android's own answer for an empty clipboard is null (getPrimaryClip
    // returns null), and the guest's code tests for it.
    return (klj_val){.l = copy[0] ? kl_jni_new_string(copy) : NULL};
}

int kl_jni_soft_input_active(void) {
    pthread_mutex_lock(&g_soft.mu);
    int open = g_soft.open;
    pthread_mutex_unlock(&g_soft.mu);
    return open;
}

int kl_jni_soft_input_get(char *buf, size_t cap, int *secure) {
    pthread_mutex_lock(&g_soft.mu);
    int open = g_soft.open;
    if (buf && cap) {
        strncpy(buf, g_soft.text, cap - 1);
        buf[cap - 1] = '\0';
    }
    if (secure) *secure = g_soft.secure;
    pthread_mutex_unlock(&g_soft.mu);
    return open;
}

void kl_jni_soft_input_set(const char *utf8) {
    if (!utf8) utf8 = "";
    pthread_mutex_lock(&g_soft.mu);
    if (!g_soft.open) { pthread_mutex_unlock(&g_soft.mu); return; }
    // The dialog's LengthFilter, which is the guest's own limit and not ours:
    // a frontend that ignored it would report text the field would have
    // refused, and the guest trusts the report.
    size_t max = sizeof g_soft.text - 1;
    if (g_soft.limit > 0 && (size_t)g_soft.limit < max) max = (size_t)g_soft.limit;
    strncpy(g_soft.text, utf8, max);
    g_soft.text[max] = '\0';
    klj_soft_push_text();
    pthread_mutex_unlock(&g_soft.mu);
}

void kl_jni_soft_input_close(int canceled) {
    pthread_mutex_lock(&g_soft.mu);
    if (!g_soft.open) { pthread_mutex_unlock(&g_soft.mu); return; }
    // reportSoftInputStr(text, 1, canceled), whose order is UnityPlayer$i's:
    // the hide happens FIRST (action 1), then either the cancellation or the
    // final string, then the close. A guest that gets the close before the
    // string commits the previous one.
    g_soft.open = 0;
    g_soft.visible = 0;
    klj_soft_push(KLJ_SOFT_AREA, 0, 0, 0, 0, NULL);
    klj_soft_push(KLJ_SOFT_VISIBLE, 0, 0, 0, 0, NULL);
    klj_soft_push(KLJ_SOFT_CFG, 0, 0, 0, 0, NULL);
    if (canceled) klj_soft_push(KLJ_SOFT_CANCELED, 0, 0, 0, 0, NULL);
    else          klj_soft_push(KLJ_SOFT_STR, 0, 0, 0, 0, g_soft.text);
    klj_soft_push(KLJ_SOFT_CLOSED, 0, 0, 0, 0, NULL);
    int secure = g_soft.secure;
    pthread_mutex_unlock(&g_soft.mu);
    KLJ_LOG("soft input %s", canceled ? "canceled"
                                      : secure ? "committed (secure — text not logged)"
                                               : "committed");
}

// ---- ...and the gate for it -------------------------------------------------
//
// `make softinput`, in `make check`. It exists because every failure in this
// seam returns success and shows a correct-looking screen: a report that is
// never drained, an order that puts the close before the string, a caret left
// behind the text — each one is a login box that quietly does not work, and the
// only other instrument is a person clicking a field in a running guest, which
// on this target costs a viewer session and cannot be repeated identically.
//
// It drives the REAL bindings (they are static in this file, which is why the
// check is here rather than in tests/) against natives it registers through the
// real RegisterNatives, so what it measures is the path a guest takes.
static struct {
    int   n;
    char  log[64][256];
} g_soft_seen;

static void klj_soft_seen(const char *fmt, ...) {
    if (g_soft_seen.n >= (int)(sizeof g_soft_seen.log / sizeof g_soft_seen.log[0])) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_soft_seen.log[g_soft_seen.n++], sizeof g_soft_seen.log[0], fmt, ap);
    va_end(ap);
}

static void klj_soft_t_str(void *env, void *self, void *s) {
    (void)env; (void)self;
    klj_soft_seen("str \"%s\"", klj_str(s) ? klj_str(s) : "(null)");
}
static void klj_soft_t_sel(void *env, void *self, kl_jint a, kl_jint b) {
    (void)env; (void)self; klj_soft_seen("sel %d,%d", a, b);
}
static void klj_soft_t_area(void *env, void *self, kl_jint l, kl_jint t,
                            kl_jint r, kl_jint b) {
    (void)env; (void)self; klj_soft_seen("area %d,%d,%d,%d", l, t, r, b);
}
static void klj_soft_t_vis(void *env, void *self, uint8_t v) {
    (void)env; (void)self; klj_soft_seen("visible %d", v);
}
static void klj_soft_t_closed(void *env, void *self)   { (void)env; (void)self; klj_soft_seen("closed"); }
static void klj_soft_t_cancel(void *env, void *self)   { (void)env; (void)self; klj_soft_seen("canceled"); }
static void klj_soft_t_cfg(void *env, void *self)      { (void)env; (void)self; klj_soft_seen("cfg"); }

int kl_jni_soft_input_selftest(FILE *out) {
    int fails = 0, checks = 0;
#define SOFT_CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fprintf(out, "  FAIL: " __VA_ARGS__); fprintf(out, "\n"); fails++; } \
    } while (0)

    const kl_jni_method m[] = {
        {"nativeSetInputString",            "(Ljava/lang/String;)V", (void *)klj_soft_t_str},
        {"nativeSetInputSelection",         "(II)V",                 (void *)klj_soft_t_sel},
        {"nativeSetInputArea",              "(IIII)V",               (void *)klj_soft_t_area},
        {"nativeSetKeyboardIsVisible",      "(Z)V",                  (void *)klj_soft_t_vis},
        {"nativeSoftInputClosed",           "()V",                   (void *)klj_soft_t_closed},
        {"nativeSoftInputCanceled",         "()V",                   (void *)klj_soft_t_cancel},
        {"nativeReportKeyboardConfigChanged", "()V",                 (void *)klj_soft_t_cfg},
    };
    void *cls    = klj_class_object("com/unity3d/player/UnityPlayer");
    void *player = klj_new_object_data("com/unity3d/player/UnityPlayer", NULL);
    klj_RegisterNatives(kl_jni_env(), cls, m, (kl_jint)(sizeof m / sizeof m[0]));

    // The guest opens a password field with a 12-character limit. The argument
    // order is showSoftInput's own, transcribed in the section above.
    klj_val a[10] = {0};
    a[0].l = kl_jni_new_string("seed");
    a[1].j = 0;          // keyboard type
    a[2].j = 0;          // autocorrect
    a[3].j = 0;          // multiline
    a[4].j = 1;          // secure
    a[5].j = 0;          // alert
    a[6].l = kl_jni_new_string("Username");
    a[7].j = 12;         // character limit
    a[8].j = 1;          // hide input field
    g_soft_seen.n = 0;
    klj_UnityPlayer_showSoftInput(kl_jni_env(), player, a, 10);

    // Nothing may have run yet: these are the guest's GL-thread events, and a
    // frontend calling in from its own thread must not reach the engine there.
    SOFT_CHECK(g_soft_seen.n == 0, "showSoftInput dispatched %d report(s) before "
                                   "the drain — the queue is what defers them",
               g_soft_seen.n);
    char text[256]; int secure = 0;
    SOFT_CHECK(kl_jni_soft_input_get(text, sizeof text, &secure) == 1,
               "a field is open and the frontend cannot see it");
    SOFT_CHECK(secure == 1, "a password field did not report itself secure — a "
                            "frontend that echoes it has no way to know");
    SOFT_CHECK(strcmp(text, "seed") == 0,
               "the field's initial text is \"%s\", not the guest's \"seed\"", text);

    klj_drain_soft_input();
    SOFT_CHECK(g_soft_seen.n == 2 && strcmp(g_soft_seen.log[0], "visible 1") == 0
                                  && strcmp(g_soft_seen.log[1], "area 0,0,0,0") == 0,
               "opening reported %d event(s), first \"%s\" — expected visible then area",
               g_soft_seen.n, g_soft_seen.n ? g_soft_seen.log[0] : "(none)");

    // Typing. The caret follows the text, because the dialog's TextWatcher
    // reports both and a stale caret puts the next character in the middle.
    g_soft_seen.n = 0;
    kl_jni_soft_input_set("hello");
    klj_drain_soft_input();
    SOFT_CHECK(g_soft_seen.n == 2 && strcmp(g_soft_seen.log[0], "str \"hello\"") == 0
                                  && strcmp(g_soft_seen.log[1], "sel 5,0") == 0,
               "typing reported \"%s\" / \"%s\"", g_soft_seen.n > 0 ? g_soft_seen.log[0] : "",
               g_soft_seen.n > 1 ? g_soft_seen.log[1] : "");

    // The guest's own character limit is the frontend's too: it is the dialog's
    // LengthFilter, and text past it is text the field would have refused.
    g_soft_seen.n = 0;
    kl_jni_soft_input_set("0123456789abcdefg");
    kl_jni_soft_input_get(text, sizeof text, NULL);
    SOFT_CHECK(strlen(text) == 12, "the 12-character limit let %zu through", strlen(text));
    klj_drain_soft_input();

    // The guest setting the text — TouchScreenKeyboard.text = "..." — and the
    // frontend must then be editing THAT string, not the one it last sent.
    g_soft_seen.n = 0;
    klj_val s1 = {.l = kl_jni_new_string("fromguest")};
    klj_UnityPlayer_setSoftInputStr(kl_jni_env(), player, &s1, 1);
    kl_jni_soft_input_get(text, sizeof text, NULL);
    SOFT_CHECK(strcmp(text, "fromguest") == 0,
               "the guest set the text and the frontend still reads \"%s\"", text);
    klj_drain_soft_input();
    SOFT_CHECK(g_soft_seen.n == 2 && strcmp(g_soft_seen.log[0], "str \"fromguest\"") == 0,
               "setSoftInputStr did not report back out (%d event(s))", g_soft_seen.n);

    // Done. UnityPlayer$i's order: the string, and THEN the close — a guest
    // that is told the field closed before it is given the final text commits
    // the previous one.
    g_soft_seen.n = 0;
    kl_jni_soft_input_close(0);
    klj_drain_soft_input();
    int i_str = -1, i_closed = -1, i_vis0 = -1;
    for (int i = 0; i < g_soft_seen.n; i++) {
        if (!strcmp(g_soft_seen.log[i], "str \"fromguest\"")) i_str = i;
        if (!strcmp(g_soft_seen.log[i], "closed"))            i_closed = i;
        if (!strcmp(g_soft_seen.log[i], "visible 0"))         i_vis0 = i;
    }
    SOFT_CHECK(i_str >= 0 && i_closed >= 0 && i_str < i_closed,
               "commit reported str at %d and closed at %d", i_str, i_closed);
    SOFT_CHECK(i_vis0 >= 0, "commit never reported the keyboard gone");
    SOFT_CHECK(!kl_jni_soft_input_active(),
               "the field is still open after it was committed");
    // ...and a committed field is not a cancelled one.
    for (int i = 0; i < g_soft_seen.n; i++)
        SOFT_CHECK(strcmp(g_soft_seen.log[i], "canceled") != 0,
                   "a commit reported a cancellation");

    // Dismissed. The text must NOT be reported: a cancellation the guest reads
    // as a commit is the wrong string typed into a login box.
    a[4].j = 0;
    klj_UnityPlayer_showSoftInput(kl_jni_env(), player, a, 10);
    kl_jni_soft_input_set("abandoned");
    klj_drain_soft_input();
    g_soft_seen.n = 0;
    kl_jni_soft_input_close(1);
    klj_drain_soft_input();
    int saw_cancel = 0, saw_str = 0;
    for (int i = 0; i < g_soft_seen.n; i++) {
        if (!strcmp(g_soft_seen.log[i], "canceled"))        saw_cancel = 1;
        if (!strncmp(g_soft_seen.log[i], "str ", 4))        saw_str = 1;
    }
    SOFT_CHECK(saw_cancel, "a dismissal did not report a cancellation");
    SOFT_CHECK(!saw_str, "a dismissal reported the text as well");
    SOFT_CHECK(!kl_jni_soft_input_active(), "the field survived its dismissal");

    // The guest hiding the keyboard itself closes the frontend's field too —
    // without this a viewer keeps swallowing every keystroke for the rest of
    // the run, which reads as a wedged window.
    klj_UnityPlayer_showSoftInput(kl_jni_env(), player, a, 10);
    klj_UnityPlayer_hideSoftInput(kl_jni_env(), player, NULL, 0);
    SOFT_CHECK(!kl_jni_soft_input_active(),
               "hideSoftInput left the frontend holding the keyboard");
    klj_drain_soft_input();
    g_soft_seen.n = 0;

    // A frontend that types with no field open must reach nothing at all.
    kl_jni_soft_input_set("nowhere");
    kl_jni_soft_input_close(0);
    klj_drain_soft_input();
    SOFT_CHECK(g_soft_seen.n == 0,
               "%d report(s) escaped from a frontend with no field open", g_soft_seen.n);

    // The clipboard: empty is NULL, which is what Android answers and what the
    // guest's own code tests for.
    klj_val got = klj_UnityPlayer_getClipboardText(kl_jni_env(), player, NULL, 0);
    SOFT_CHECK(got.l == NULL, "an empty clipboard answered something");
    klj_val cb = {.l = kl_jni_new_string("pasted")};
    klj_UnityPlayer_setClipboardText(kl_jni_env(), player, &cb, 1);
    got = klj_UnityPlayer_getClipboardText(kl_jni_env(), player, NULL, 0);
    SOFT_CHECK(got.l && klj_str(got.l) && strcmp(klj_str(got.l), "pasted") == 0,
               "the clipboard did not keep what was put in it");

    fprintf(out, "  %d assertion(s), %d failed\n", checks, fails);
    return fails == 0;
#undef SOFT_CHECK
}

// WebView's message channel — the two-way bridge between the guest's C++ and
// the page it would have loaded. Android returns a pair of entangled ports; the
// guest keeps port 0 and posts port 1 into the document, which is where ipc.js
// picks it up (`onmessage` takes e.ports[0] and calls OnConnectCallback).
//
// One pair per WebView, not one pair for the process: three WebViews are set up
// (streampreflight, streamloading, streamanimation) and each opens its own
// channel, so a shared static would cross their wires.
static klj_val klj_WebView_createWebMessageChannel(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_webdoc *d = klj_webdoc_of(self);
    void *ports = klj_new_array('L', "android/webkit/WebMessagePort", 2);
    klj_as_object(ports)->pinned = 1;
    klj_array *arr = klj_arr(ports);
    for (int i = 0; i < 2; i++) {
        void *p = klj_new_object_data("android/webkit/WebMessagePort", d);
        klj_as_object(p)->pinned = 1;
        ((void **)arr->data)[i] = p;
    }
    KLJ_LOG("WebView.createWebMessageChannel(%s) — two ports",
            d && d->url ? d->url : "(no document)");
    return (klj_val){.l = ports};
}

// WebView.getProgress — the page load percentage, and the ONE thing gating the
// whole in-headset UI.
//
// libvrlink_scene's WebViewThread spins at 5 ms posting
// WebView::UIThread_InitializeMessageChannels to the UI thread, and that
// function (+0x14ba5c) does exactly one test before doing its work
// unconditionally: `if (getProgress() != 100) return`. Answering 0 forever is
// what parks the guest on "Waiting for message channels to initialize...".
//
// So this is not a licence to claim a document rendered. It reports the LOAD,
// which is a fact we hold: loadUrl resolved the `file:///android_asset/` URL
// against the same assets root AssetManager.open() serves, and stat() said
// whether the file is there. A document that is present and fully read is at
// 100% loaded; that it is then handed to no renderer is the separate, declared
// gap above. A URL we cannot find stays at 0, so a wrong path still shows up as
// a stall rather than as a lie.
static klj_val klj_WebView_getProgress(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_webdoc *d = klj_webdoc_of(self);
    int pct = (d && d->found) ? 100 : 0;
    if (d && !d->logged) {
        d->logged = 1;
        KLJ_LOG("WebView.getProgress(%s) -> %d", d->url ? d->url : "(no document)", pct);
    }
    return (klj_val){.j = pct};
}

// ---- the channel, once it is up --------------------------------------------
//
// A WebMessage is a string plus any ports being transferred with it. We keep
// the string, because it is the entire IPC: the protocol ships in the APK
// (assets/webui/ipc.js) and is plain text, "<mailbox> <json>", in both
// directions. Logging what the guest sends is how the far half of this UI gets
// measured instead of guessed at.
static klj_val klj_WebMessage_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *s = n > 0 ? klj_str(a[0].l) : NULL;
    return (klj_val){.l = klj_new_object_data("android/webkit/WebMessage",
                                              strdup(s ? s : ""))};
}

static const char *klj_webmessage_data(void *msg) {
    klj_object *o = klj_as_object(msg);
    return (o && o->cls && strcmp(o->cls, "android/webkit/WebMessage") == 0) ? o->data : NULL;
}

// postWebMessage(msg, uri): the guest handing port 1 to the document. On
// Android this is what fires the page's `onmessage`. There is no page, so the
// port lands nowhere — but the guest does not wait for an acknowledgement, it
// just marks its channel up and carries on.
static klj_val klj_WebView_postWebMessage(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_webdoc *d = klj_webdoc_of(self);
    const char *s = n > 0 ? klj_webmessage_data(a[0].l) : NULL;
    KLJ_LOG("WebView.postWebMessage(\"%s\") to %s — the port reaches no document",
            s ? s : "", d && d->url ? d->url : "(no document)");
    return (klj_val){.j = 0};
}

// WebMessagePort.postMessage: native -> page. Nothing receives it, but the text
// is the guest telling us what its UI is being asked to show, so it is printed.
static klj_val klj_WebMessagePort_postMessage(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *s = n > 0 ? klj_webmessage_data(a[0].l) : NULL;
    KLJ_LOG("WebMessagePort.postMessage: %s", s ? s : "(not a WebMessage)");
    return (klj_val){.j = 0};
}

// setWebMessageCallback(cb, handler): the page -> native direction. Worth a line
// because of what is measured in it — libvrlink_scene passes a NULL callback
// (+0x149d1c is `mov x3, xzr`, and the descriptor string has exactly one xref,
// so there is no second registration anywhere). A null callback drops incoming
// messages on Android too, so nothing we could deliver here would be read. That
// rules out "deliver the page's Continue click" as the way past the preflight,
// and it is the kind of thing that costs a day if it is inferred rather than
// printed.
static klj_val klj_WebMessagePort_setWebMessageCallback(void *env, void *self,
                                                        const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("WebMessagePort.setWebMessageCallback(%s) — page->native is %s",
            (n > 0 && a[0].l) ? "callback" : "null",
            (n > 0 && a[0].l) ? "registered" : "unreadable by the guest's own choice");
    return (klj_val){.j = 0};
}

// Uri.EMPTY — the target the guest passes to postWebMessage. An identity, and
// the only thing it is used for is being passed straight back to us.
 klj_val klj_Uri_EMPTY(void) {
    static void *empty;
    return klj_singleton("android/net/Uri", &empty);
}

// Uri.decode — percent-decoding, implemented rather than stubbed because it is a
// pure function with one right answer.
//
// Note it is *not* URLDecoder.decode: Android's Uri.decode leaves '+' alone rather
// than turning it into a space. Getting that backwards would silently corrupt any
// path containing a plus, which is the kind of bug that surfaces as a missing file
// a long way from here.
static int klj_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static klj_val klj_Uri_decode(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *s = n > 0 ? klj_str(a[0].l) : NULL;
    if (!s) return (klj_val){.l = NULL};
    size_t len = strlen(s);
    char  *out = malloc(len + 1);
    if (!out) return (klj_val){.l = NULL};
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        int hi, lo;
        if (s[i] == '%' && i + 2 < len &&
            (hi = klj_hexval(s[i + 1])) >= 0 && (lo = klj_hexval(s[i + 2])) >= 0) {
            out[o++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = 0;
    klj_val r = {.l = kl_jni_new_string(out)};
    free(out);
    return r;
}

// The single activity. t_boot hands this same object to initJni as the
// Context, and Unity reads it back through the static UnityPlayer.currentActivity
// — so it has to be one instance, not two of the same class. Created lazily
// because whichever of the two paths runs first should win, and they are the same
// object either way. The class is the title's manifest activity: Unity's by
// default, SDLActivity for the SDL3 target (kl_jni_set_activity_class, called
// before the first kl_jni_activity()).
static const char *g_activity_class = "com/unity3d/player/UnityPlayerActivity";
void kl_jni_set_activity_class(const char *cls) { if (cls) g_activity_class = cls; }
const char *klj_activity_class(void) { return g_activity_class; }

void *kl_jni_activity(void) {
    static void *activity;
    if (!activity) {
        activity = kl_jni_new_object(g_activity_class);
        ((klj_object *)activity)->pinned = 1;   // singleton; survives any frame pop
    }
    return activity;
}

// SensorManager.getDefaultSensor(type) -> null, for every type.
//
// This is not a gap, it is the SAME answer kl_ndk.c's ASensorManager already
// gives through the other door, and the two must not disagree: Vision Pro
// exposes no Android-shaped accelerometer or gyro to the guest, and a VR title
// does not want one — head and controller poses arrive through the XR runtime,
// not through Input.acceleration. An empty sensor set is a configuration real
// Android devices ship, so the engine already handles it, whereas a fabricated
// sensor would feed it invented motion.
//
// Null is also what makes the guest STOP asking: Unity keeps the non-null ones
// and never registers a listener for the rest, so nothing downstream waits on
// an event that cannot come.
static klj_val klj_SensorManager_getDefaultSensor(void *env, void *self,
                                                  const klj_val *a, int n) {
    (void)env; (void)self;
    static int said;
    if (!said++)
        KLJ_LOG("SensorManager.getDefaultSensor(%d) -> null; no Android sensors are "
                "presented (the NDK ASensor list is empty for the same reason)",
                n > 0 ? (int)a[0].j : -1);
    return (klj_val){.l = NULL};
}

const klj_binding klj_bind_softinput[] = {
    {"android/hardware/SensorManager", "getDefaultSensor",
     "(I)Landroid/hardware/Sensor;",              klj_SensorManager_getDefaultSensor},
    // There is no soft keyboard here; Unity calls hide unconditionally while
    // tearing down text input, so silence is correct rather than a stub.
    // The on-screen keyboard. See "the on-screen keyboard" above — every one of
    // these is transcribed from this APK's own UnityPlayer.smali, and
    // hideSoftInput was a no-op until a guest opened a text field: it reports
    // the close, so silence there is a keyboard that never goes away.
    {"com/unity3d/player/UnityPlayer", "hideSoftInput", "()V", klj_UnityPlayer_hideSoftInput},
    {"com/unity3d/player/UnityPlayer", "showSoftInput",
     "(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V", klj_UnityPlayer_showSoftInput},
    {"com/unity3d/player/UnityPlayer", "setSoftInputStr", "(Ljava/lang/String;)V",
     klj_UnityPlayer_setSoftInputStr},
    {"com/unity3d/player/UnityPlayer", "setHideInputField", "(Z)V",
     klj_UnityPlayer_setHideInputField},
    {"com/unity3d/player/UnityPlayer", "getKeyboardLayout", "()Ljava/lang/String;",
     klj_UnityPlayer_getKeyboardLayout},
    {"com/unity3d/player/UnityPlayer", "setClipboardText", "(Ljava/lang/String;)V",
     klj_UnityPlayer_setClipboardText},
    {"com/unity3d/player/UnityPlayer", "getClipboardText", "()Ljava/lang/String;",
     klj_UnityPlayer_getClipboardText},
    {"android/net/Uri", "decode", "(Ljava/lang/String;)Ljava/lang/String;", klj_Uri_decode},
    {"android/webkit/WebView", "getProgress", "()I", klj_WebView_getProgress},
    {"android/webkit/WebView", "createWebMessageChannel", "()[Landroid/webkit/WebMessagePort;",
     klj_WebView_createWebMessageChannel},
    {"android/webkit/WebMessagePort", "setWebMessageCallback",
     "(Landroid/webkit/WebMessagePort$WebMessageCallback;Landroid/os/Handler;)V",
     klj_WebMessagePort_setWebMessageCallback},
    {"android/webkit/WebMessage", "<init>",
     "(Ljava/lang/String;[Landroid/webkit/WebMessagePort;)V", klj_WebMessage_init},
    {"android/webkit/WebMessage", "<init>", "(Ljava/lang/String;)V", klj_WebMessage_init},
    {"android/webkit/WebView", "postWebMessage",
     "(Landroid/webkit/WebMessage;Landroid/net/Uri;)V", klj_WebView_postWebMessage},
    {"android/webkit/WebMessagePort", "postMessage", "(Landroid/webkit/WebMessage;)V",
     klj_WebMessagePort_postMessage},
    {0}
};
