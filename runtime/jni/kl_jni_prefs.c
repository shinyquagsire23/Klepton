// SharedPreferences and the collection view getAll() forces
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

// ---- SharedPreferences ----
// Unity's PlayerPrefs. This one has to persist: a value written this run must
// read back the next, so it is a real file rather than an in-memory map. The
// file is Android's own shared_prefs/<name>.xml, which costs a small serialiser
// and buys inspectability — and a prefs file pulled off a Quest drops straight
// in. Set<String> is absent because nothing has asked for it; the primitive
// types are all one shape, so they come as a group.
typedef struct { klj_pref *v; unsigned n, cap; } klj_pref_set;

typedef struct {
    char         name[128];
    char         path[1024];
    klj_pref_set kv;
} klj_prefs;

// An Editor batches: Android does not publish a put until commit()/apply(), and
// a read through the SharedPreferences in between still sees the old value.
typedef struct {
    klj_prefs   *store;
    klj_pref_set pending;
    int          clear;
} klj_editor;

static klj_pref *klj_pref_find(klj_pref_set *s, const char *key) {
    for (unsigned i = 0; i < s->n; i++)
        if (strcmp(s->v[i].key, key) == 0) return &s->v[i];
    return NULL;
}

// Find or create, and reset whatever value was there — every caller is about to
// overwrite it.
static klj_pref *klj_pref_slot(klj_pref_set *s, const char *key) {
    klj_pref *p = klj_pref_find(s, key);
    if (p) { free(p->sval); p->sval = NULL; p->ival = 0; p->fval = 0; return p; }
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v   = realloc(s->v, s->cap * sizeof *s->v);
    }
    p = &s->v[s->n++];
    memset(p, 0, sizeof *p);
    p->key = strdup(key);
    return p;
}

static void klj_pref_erase(klj_pref_set *s, const char *key) {
    klj_pref *p = klj_pref_find(s, key);
    if (!p) return;
    free(p->key);
    free(p->sval);
    *p = s->v[--s->n];   // order is not part of the API
}

static void klj_pref_clear(klj_pref_set *s) {
    for (unsigned i = 0; i < s->n; i++) { free(s->v[i].key); free(s->v[i].sval); }
    s->n = 0;
}

// Only the five entities Android's XmlSerializer emits — which is exactly the
// set our own reader has to understand, since we write every file we read.
static void klj_xml_escape(FILE *f, const char *s) {
    for (; s && *s; s++) switch (*s) {
    case '&':  fputs("&amp;",  f); break;
    case '<':  fputs("&lt;",   f); break;
    case '>':  fputs("&gt;",   f); break;
    case '"':  fputs("&quot;", f); break;
    case '\'': fputs("&apos;", f); break;
    default:   fputc(*s, f);
    }
}
static char *klj_xml_unescape(const char *s, size_t len) {
    static const char *const ent[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"};
    static const char        rep[] = {'&', '<', '>', '"', '\''};
    char *out = malloc(len + 1), *w = out;
    for (size_t i = 0; i < len; ) {
        if (s[i] != '&') { *w++ = s[i++]; continue; }
        unsigned k = 0;
        for (; k < sizeof rep; k++) {
            size_t el = strlen(ent[k]);
            if (i + el <= len && strncmp(s + i, ent[k], el) == 0) { *w++ = rep[k]; i += el; break; }
        }
        if (k == sizeof rep) *w++ = s[i++];   // not an entity we emit — take it literally
    }
    *w = '\0';
    return out;
}

static const struct { const char *tag; char kind; } g_pref_tags[] = {
    {"string", 'S'}, {"int", 'I'}, {"long", 'J'}, {"float", 'F'}, {"boolean", 'Z'}, {NULL, 0},
};

static void klj_prefs_save(klj_prefs *p) {
    FILE *f = fopen(p->path, "wb");
    if (!f) { KLJ_LOG("SharedPreferences[%s]: cannot write %s", p->name, p->path); return; }
    fputs("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n", f);
    for (unsigned i = 0; i < p->kv.n; i++) {
        klj_pref *e = &p->kv.v[i];
        if (e->kind == 'S') {
            fputs("    <string name=\"", f);
            klj_xml_escape(f, e->key);
            fputs("\">", f);
            klj_xml_escape(f, e->sval ? e->sval : "");
            fputs("</string>\n", f);
            continue;
        }
        const char *tag = "boolean";
        for (int t = 0; g_pref_tags[t].tag; t++)
            if (g_pref_tags[t].kind == e->kind) { tag = g_pref_tags[t].tag; break; }
        char val[64];
        if (e->kind == 'F')      snprintf(val, sizeof val, "%.9g", (double)e->fval);
        else if (e->kind == 'Z') snprintf(val, sizeof val, "%s", e->ival ? "true" : "false");
        else                     snprintf(val, sizeof val, "%lld", (long long)e->ival);
        fprintf(f, "    <%s name=\"", tag);
        klj_xml_escape(f, e->key);
        fprintf(f, "\" value=\"%s\" />\n", val);
    }
    fputs("</map>\n", f);
    fclose(f);
}

// A reader for exactly the subset above, not an XML parser. Every '<' that is
// not one of our five tags is skipped, so the prolog and <map> need no cases.
static void klj_prefs_load(klj_prefs *p) {
    FILE *f = fopen(p->path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return; }
    char  *buf = malloc((size_t)sz + 1);
    size_t len = fread(buf, 1, (size_t)sz, f);
    buf[len] = '\0';
    fclose(f);

    for (char *q = buf; (q = strchr(q, '<')) != NULL; ) {
        q++;
        char kind = 0;
        for (int i = 0; g_pref_tags[i].tag; i++) {
            size_t tl = strlen(g_pref_tags[i].tag);
            if (strncmp(q, g_pref_tags[i].tag, tl) == 0 && (q[tl] == ' ' || q[tl] == '\t')) {
                kind = g_pref_tags[i].kind;
                break;
            }
        }
        if (!kind) continue;
        char *close = strchr(q, '>');
        char *nm    = strstr(q, "name=\"");
        if (!close || !nm || nm > close) continue;
        nm += 6;
        char *ne = strchr(nm, '"');
        if (!ne || ne > close) continue;
        char *key = klj_xml_unescape(nm, (size_t)(ne - nm));

        if (kind == 'S') {
            // <string name="k" /> is how a null value is written; that is
            // indistinguishable from absent to every reader, so drop it.
            char *end = close[-1] == '/' ? NULL : strstr(close + 1, "</string>");
            if (end) {
                char     *val = klj_xml_unescape(close + 1, (size_t)(end - close - 1));
                klj_pref *e   = klj_pref_slot(&p->kv, key);
                e->kind = 'S';
                e->sval = val;
            }
        } else {
            char *vs = strstr(q, "value=\"");
            char *ve = vs && vs < close ? strchr(vs + 7, '"') : NULL;
            if (ve) {
                char val[64];
                size_t vl = (size_t)(ve - vs - 7);
                if (vl >= sizeof val) vl = sizeof val - 1;
                memcpy(val, vs + 7, vl);
                val[vl] = '\0';
                klj_pref *e = klj_pref_slot(&p->kv, key);
                e->kind = kind;
                if (kind == 'F')      e->fval = strtof(val, NULL);
                else if (kind == 'Z') e->ival = strcmp(val, "true") == 0;
                else                  e->ival = strtoll(val, NULL, 10);
            }
        }
        free(key);
        q = close;
    }
    free(buf);
}

// One SharedPreferences object per name, as Android does — callers hold on to
// the reference and expect writes through one to be visible through another.
#define KLJ_MAX_PREF_FILES 8
static klj_prefs g_prefs_files[KLJ_MAX_PREF_FILES];
static void     *g_prefs_objs[KLJ_MAX_PREF_FILES];
static unsigned  g_nprefs_files;

// Activity.getPreferences(mode) is getSharedPreferences() with the activity's
// own class name as the file — Android's per-activity default store. Named
// here rather than pointed at "default", because the name IS the file on disk
// and a guest that later asks for it by name must find the same one.
static klj_val klj_Context_getSharedPreferences(void *env, void *self, const klj_val *a, int n);
static klj_val klj_Activity_getPreferences(void *env, void *self, const klj_val *a, int n) {
    klj_val args[2] = { {.l = kl_jni_new_string("com.unity3d.player.UnityPlayerActivity")},
                        { .j = n > 0 ? a[0].j : 0 } };
    return klj_Context_getSharedPreferences(env, self, args, 2);
}

static klj_val klj_Context_getSharedPreferences(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *name = n > 0 ? klj_str(a[0].l) : NULL;
    if (!name) name = "default";
    for (unsigned i = 0; i < g_nprefs_files; i++)
        if (strcmp(g_prefs_files[i].name, name) == 0) return (klj_val){.l = g_prefs_objs[i]};
    if (g_nprefs_files == KLJ_MAX_PREF_FILES) {
        KLJ_LOG("SharedPreferences: file table full, refusing \"%s\"", name);
        return (klj_val){.l = NULL};
    }
    klj_prefs *p = &g_prefs_files[g_nprefs_files];
    snprintf(p->name, sizeof p->name, "%s", name);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/shared_prefs", kl_jni_files_dir());
    klj_mkdir_p(dir);
    snprintf(p->path, sizeof p->path, "%s/%s.xml", dir, name);
    klj_prefs_load(p);
    // The mode is ignored: MODE_PRIVATE is the only one not deprecated, and
    // there is no second process here to share with.
    KLJ_LOG("getSharedPreferences(\"%s\", %d) -> %s (%u entries)",
            name, n > 1 ? (int)a[1].j : 0, p->path, p->kv.n);
    void *obj = kl_jni_new_object("android/content/SharedPreferences");
    klj_as_object(obj)->data = p;
    g_prefs_objs[g_nprefs_files++] = obj;
    return (klj_val){.l = obj};
}

static klj_prefs *klj_prefs_of(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}

static klj_pref *klj_prefs_lookup(void *self, const klj_val *a, int n, char kind) {
    klj_prefs  *p   = klj_prefs_of(self);
    const char *key = n > 0 ? klj_str(a[0].l) : NULL;
    if (!p || !key) return NULL;
    klj_pref *e = klj_pref_find(&p->kv, key);
    // Android throws ClassCastException on a type mismatch. Nothing here can
    // throw, so a mismatch reads as absent — the caller gets its own default,
    // which is the closest non-throwing answer. Logged, because it means the
    // file disagrees with the code that wrote it.
    if (e && e->kind != kind) {
        KLJ_LOG("SharedPreferences[%s]: \"%s\" is '%c' but was read as '%c' — using the default",
                p->name, key, e->kind, kind);
        return NULL;
    }
    return e;
}

static klj_val klj_SP_getString(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_pref *e = klj_prefs_lookup(self, a, n, 'S');
    if (!e) return (klj_val){.l = n > 1 ? a[1].l : NULL};
    return (klj_val){.l = kl_jni_new_string(e->sval ? e->sval : "")};
}
#define KLJ_SP_GET(Sfx, kind, Field, Slot)                                        \
    static klj_val klj_SP_get##Sfx(void *env, void *self, const klj_val *a, int n) { \
        (void)env;                                                                \
        klj_pref *e = klj_prefs_lookup(self, a, n, kind);                         \
        if (!e) return (klj_val){.Slot = n > 1 ? a[1].Slot : 0};                  \
        return (klj_val){.Slot = e->Field};                                       \
    }
KLJ_SP_GET(Int,     'I', ival, j)
KLJ_SP_GET(Long,    'J', ival, j)
KLJ_SP_GET(Boolean, 'Z', ival, j)
KLJ_SP_GET(Float,   'F', fval, d)
#undef KLJ_SP_GET

static klj_val klj_SP_contains(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_prefs  *p   = klj_prefs_of(self);
    const char *key = n > 0 ? klj_str(a[0].l) : NULL;
    return (klj_val){.j = p && key && klj_pref_find(&p->kv, key) != NULL};
}

// getAll() hands back a *copy* in Android, and that is not incidental here: the
// path that calls it is Unity migrating one prefs file into another, so a live
// alias would be read while its source is being written.
static klj_pref_set *klj_pref_snapshot(const klj_pref_set *src) {
    klj_pref_set *s = calloc(1, sizeof *s);
    for (unsigned i = 0; i < src->n; i++) {
        klj_pref *d = klj_pref_slot(s, src->v[i].key);
        d->kind = src->v[i].kind;
        d->ival = src->v[i].ival;
        d->fval = src->v[i].fval;
        d->sval = src->v[i].sval ? strdup(src->v[i].sval) : NULL;
    }
    return s;
}

static klj_val klj_SP_getAll(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_prefs *p = klj_prefs_of(self);
    void      *obj = kl_jni_new_object("java/util/Map");
    klj_as_object(obj)->data = p ? klj_pref_snapshot(&p->kv) : calloc(1, sizeof(klj_pref_set));
    KLJ_LOG("SharedPreferences[%s]: getAll() -> %u entries",
            p ? p->name : "?", p ? p->kv.n : 0);
    return (klj_val){.l = obj};
}

static klj_val klj_SP_edit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_editor *ed = calloc(1, sizeof *ed);
    ed->store = klj_prefs_of(self);
    void *obj = kl_jni_new_object("android/content/SharedPreferences$Editor");
    klj_as_object(obj)->data = ed;
    return (klj_val){.l = obj};
}

static klj_editor *klj_editor_of(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}

// Every Editor mutator returns `this` — that is what makes the chaining Unity
// writes work, not a convenience.
static klj_val klj_editor_put(void *self, const klj_val *a, int n, char kind) {
    klj_editor *ed  = klj_editor_of(self);
    const char *key = n > 0 ? klj_str(a[0].l) : NULL;
    if (!ed || !key) return (klj_val){.l = self};
    klj_pref *e = klj_pref_slot(&ed->pending, key);
    e->kind = kind;
    if (kind == 'S') {
        const char *v = n > 1 ? klj_str(a[1].l) : NULL;
        if (v) e->sval = strdup(v);
        else   e->kind = '-';     // putString(key, null) is documented to remove
    } else if (kind == 'F') {
        e->fval = n > 1 ? (float)a[1].d : 0.0f;
    } else {
        e->ival = n > 1 ? (int64_t)a[1].j : 0;
    }
    return (klj_val){.l = self};
}
#define KLJ_ED_PUT(Sfx, kind)                                                     \
    static klj_val klj_ED_put##Sfx(void *env, void *self, const klj_val *a, int n) { \
        (void)env; return klj_editor_put(self, a, n, kind);                       \
    }
KLJ_ED_PUT(String, 'S') KLJ_ED_PUT(Int,   'I') KLJ_ED_PUT(Long, 'J')
KLJ_ED_PUT(Float,  'F') KLJ_ED_PUT(Boolean, 'Z')
#undef KLJ_ED_PUT

static klj_val klj_ED_remove(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_editor *ed  = klj_editor_of(self);
    const char *key = n > 0 ? klj_str(a[0].l) : NULL;
    if (ed && key) klj_pref_slot(&ed->pending, key)->kind = '-';
    return (klj_val){.l = self};
}
static klj_val klj_ED_clear(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_editor *ed = klj_editor_of(self);
    if (ed) ed->clear = 1;
    return (klj_val){.l = self};
}

// clear() runs before the puts in the same Editor regardless of call order —
// that ordering is the documented contract, not an implementation detail.
// commit() writes synchronously and apply() does not; with no other process to
// race, both reduce to the same flush.
static void klj_editor_flush(void *self) {
    klj_editor *ed = klj_editor_of(self);
    if (!ed || !ed->store) return;
    klj_prefs *p = ed->store;
    if (ed->clear) klj_pref_clear(&p->kv);
    for (unsigned i = 0; i < ed->pending.n; i++) {
        klj_pref *s = &ed->pending.v[i];
        if (s->kind == '-') { klj_pref_erase(&p->kv, s->key); continue; }
        klj_pref *d = klj_pref_slot(&p->kv, s->key);
        d->kind = s->kind;
        d->ival = s->ival;
        d->fval = s->fval;
        d->sval = s->sval ? strdup(s->sval) : NULL;
    }
    KLJ_LOG("SharedPreferences[%s]: %s%u change%s -> %u entries in %s", p->name,
            ed->clear ? "clear + " : "", ed->pending.n,
            ed->pending.n == 1 ? "" : "s", p->kv.n, p->path);
    klj_prefs_save(p);
    klj_pref_clear(&ed->pending);
    ed->clear = 0;
}
static klj_val klj_ED_commit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_editor_flush(self);
    return (klj_val){.j = 1};
}
static klj_val klj_ED_apply(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_editor_flush(self);
    return (klj_val){0};
}

// ---- the collection view getAll() forces ----
// entrySet() -> iterator() -> Map.Entry is the only route out of a Map over JNI,
// so the whole chain follows from getAll() rather than being speculative. It is
// read-only throughout: nothing mutates one of our snapshots, so there is no
// remove() or setValue() here.
// `coll` is one of { klj_pref_set, klj_list } selected by is_set; both expose
// the same { count, void** } shape the two walkers below read.
typedef struct { int is_set; void *coll; unsigned pos; } klj_iter;

static klj_pref_set *klj_pset(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}

static klj_val klj_Map_entrySet(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    void *obj = kl_jni_new_object("java/util/Set");
    klj_as_object(obj)->data = klj_pset(self);
    return (klj_val){.l = obj};
}
static klj_val klj_Coll_size(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_pref_set *s = klj_pset(self);
    return (klj_val){.j = s ? s->n : 0};
}
static klj_val klj_Coll_isEmpty(void *env, void *self, const klj_val *a, int n) {
    return (klj_val){.j = klj_Coll_size(env, self, a, n).j == 0};
}
static klj_val klj_Set_iterator(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_iter *it = calloc(1, sizeof *it);
    it->is_set = 1;
    it->coll = klj_pset(self);
    void *obj = kl_jni_new_object("java/util/Iterator");
    klj_as_object(obj)->data = it;
    return (klj_val){.l = obj};
}
static klj_val klj_List_iterator(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_list *l = NULL;
    klj_object *o = klj_as_object(self);
    if (o) l = o->data;
    klj_iter *it = calloc(1, sizeof *it);
    it->is_set = 0;
    it->coll = l;
    void *obj = kl_jni_new_object("java/util/Iterator");
    klj_as_object(obj)->data = it;
    return (klj_val){.l = obj};
}
static unsigned klj_iter_len(const klj_iter *it) {
    if (!it || !it->coll) return 0;
    return it->is_set ? ((klj_pref_set *)it->coll)->n : ((klj_list *)it->coll)->count;
}
static void *klj_iter_at(const klj_iter *it, unsigned pos) {
    if (!it || !it->coll) return NULL;
    if (it->is_set) return &((klj_pref_set *)it->coll)->v[pos];
    return ((klj_list *)it->coll)->items[pos];
}
static klj_val klj_Iterator_hasNext(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_iter   *it = o ? o->data : NULL;
    return (klj_val){.j = it && it->pos < klj_iter_len(it)};
}
static klj_val klj_Iterator_next(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o  = klj_as_object(self);
    klj_iter   *it = o ? o->data : NULL;
    if (!it || it->pos >= klj_iter_len(it)) return (klj_val){.l = NULL};
    void *obj = kl_jni_new_object("java/util/Map$Entry");
    klj_as_object(obj)->data = klj_iter_at(it, it->pos++);
    return (klj_val){.l = obj};
}

// Boxed primitives. getValue() returns Object, so the caller's only way back to
// the number is the box's own accessor — and the box's *class* is how it decides
// which one to call, which is what makes IsInstanceOf's name match sufficient.
static void *klj_box(const klj_pref *e) {
    if (!e) return NULL;
    if (e->kind == 'S') return kl_jni_new_string(e->sval ? e->sval : "");
    const char *cls = e->kind == 'I' ? "java/lang/Integer"
                    : e->kind == 'J' ? "java/lang/Long"
                    : e->kind == 'F' ? "java/lang/Float"
                    :                  "java/lang/Boolean";
    klj_pref *copy = malloc(sizeof *copy);
    *copy      = *e;
    copy->key  = NULL;      // the box holds the value, not the mapping
    copy->sval = NULL;
    void *obj = kl_jni_new_object(cls);
    klj_as_object(obj)->data = copy;
    return obj;
}
static klj_val klj_Entry_getKey(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_pref   *e = o ? o->data : NULL;
    return (klj_val){.l = kl_jni_new_string(e && e->key ? e->key : "")};
}
static klj_val klj_Entry_getValue(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    return (klj_val){.l = klj_box(o ? o->data : NULL)};
}
// The boxed constructors — new Boolean(z), new Integer(i) and friends. Boxes have
// existed here since SharedPreferences needed them, but only ever built from the
// inside (klj_box); the guest can also build one directly, and it unboxes through
// the same accessors, so it has to land in the same representation.
static klj_val klj_Box_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env;
    const char *cls = klj_class_name(clazz);
    klj_pref   *e   = calloc(1, sizeof *e);
    if (!e || !cls) return (klj_val){.l = NULL};
    if      (strcmp(cls, "java/lang/Integer") == 0) e->kind = 'I';
    else if (strcmp(cls, "java/lang/Long")    == 0) e->kind = 'J';
    else if (strcmp(cls, "java/lang/Float")   == 0) e->kind = 'F';
    else                                            e->kind = 'Z';
    if (e->kind == 'F') e->fval = n > 0 ? (float)a[0].d : 0.0f;
    else                e->ival = n > 0 ? (int64_t)a[0].j : 0;
    void *obj = kl_jni_new_object(cls);
    klj_as_object(obj)->data = e;
    return (klj_val){.l = obj};
}

// int, long and boolean all unbox from the same integral field; only float
// comes out of the FP bank.
static klj_val klj_Box_integral(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_pref   *e = o ? o->data : NULL;
    return (klj_val){.j = e ? (uint64_t)e->ival : 0};
}
static klj_val klj_Box_floatValue(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    klj_pref   *e = o ? o->data : NULL;
    return (klj_val){.d = e ? (double)e->fval : 0.0};
}

const klj_binding klj_bind_prefs[] = {
    {"android/app/Activity", "getPreferences", "(I)Landroid/content/SharedPreferences;",
     klj_Activity_getPreferences},
    {"android/content/Context", "getSharedPreferences",
     "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", klj_Context_getSharedPreferences},

    {"android/content/SharedPreferences", "getString",
     "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", klj_SP_getString},
    {"android/content/SharedPreferences", "getInt",     "(Ljava/lang/String;I)I", klj_SP_getInt},
    {"android/content/SharedPreferences", "getLong",    "(Ljava/lang/String;J)J", klj_SP_getLong},
    {"android/content/SharedPreferences", "getFloat",   "(Ljava/lang/String;F)F", klj_SP_getFloat},
    {"android/content/SharedPreferences", "getBoolean", "(Ljava/lang/String;Z)Z", klj_SP_getBoolean},
    {"android/content/SharedPreferences", "contains",   "(Ljava/lang/String;)Z",  klj_SP_contains},
    {"android/content/SharedPreferences", "getAll",     "()Ljava/util/Map;",      klj_SP_getAll},
    {"android/content/SharedPreferences", "edit",
     "()Landroid/content/SharedPreferences$Editor;", klj_SP_edit},

#define KLJ_ED(name, args, fn) \
    {"android/content/SharedPreferences$Editor", name, \
     "(" args ")Landroid/content/SharedPreferences$Editor;", fn}
    KLJ_ED("putString",  "Ljava/lang/String;Ljava/lang/String;", klj_ED_putString),
    KLJ_ED("putInt",     "Ljava/lang/String;I",                  klj_ED_putInt),
    KLJ_ED("putLong",    "Ljava/lang/String;J",                  klj_ED_putLong),
    KLJ_ED("putFloat",   "Ljava/lang/String;F",                  klj_ED_putFloat),
    KLJ_ED("putBoolean", "Ljava/lang/String;Z",                  klj_ED_putBoolean),
    KLJ_ED("remove",     "Ljava/lang/String;",                   klj_ED_remove),
    KLJ_ED("clear",      "",                                     klj_ED_clear),
#undef KLJ_ED
    {"android/content/SharedPreferences$Editor", "commit", "()Z", klj_ED_commit},
    {"android/content/SharedPreferences$Editor", "apply",  "()V", klj_ED_apply},

    {"java/util/Map", "entrySet", "()Ljava/util/Set;", klj_Map_entrySet},
    {"java/util/Map", "size",     "()I",               klj_Coll_size},
    {"java/util/Map", "isEmpty",  "()Z",               klj_Coll_isEmpty},
    {"java/util/Set", "iterator", "()Ljava/util/Iterator;", klj_Set_iterator},
    {"java/util/Set", "size",     "()I",               klj_Coll_size},
    {"java/util/Set", "isEmpty",  "()Z",               klj_Coll_isEmpty},
    {"java/util/Iterator", "hasNext", "()Z",                  klj_Iterator_hasNext},
    {"java/util/Iterator", "next",    "()Ljava/lang/Object;", klj_Iterator_next},
    {"java/util/Map$Entry", "getKey",   "()Ljava/lang/Object;", klj_Entry_getKey},
    {"java/util/Map$Entry", "getValue", "()Ljava/lang/Object;", klj_Entry_getValue},
    {"java/lang/Boolean", "<init>", "(Z)V", klj_Box_init},
    {"java/lang/Integer", "<init>", "(I)V", klj_Box_init},
    {"java/lang/Long",    "<init>", "(J)V", klj_Box_init},
    {"java/lang/Float",   "<init>", "(F)V", klj_Box_init},
    {"java/lang/Integer", "intValue",     "()I", klj_Box_integral},
    {"java/lang/Long",    "longValue",    "()J", klj_Box_integral},
    {"java/lang/Boolean", "booleanValue", "()Z", klj_Box_integral},
    {"java/lang/Float",   "floatValue",   "()F", klj_Box_floatValue},
    {"java/util/List", "iterator", "()Ljava/util/Iterator;", klj_List_iterator},
    {0}
};
