// Custom levels for a Beat Saber guest whose own loader was stripped.
//
// The Quest build keeps the custom-level DATA model whole — CustomPreviewBeatmapLevel,
// CustomBeatmapLevelPack, CustomBeatmapLevelCollection, their constructors, and
// BeatmapLevelsModel's _customLevelPackCollection with the code that folds it
// into the list of all packs — and loses the half that fills it:
// CustomLevelLoader is down to its .ctor (KL_PROBE_METHODS=CustomLevelLoader,
// 1.28.0). Managed stripping removed every method that read a song off disk,
// because on a Quest nothing called them.
//
// So this is that half, on the host side of the fence. It reads
// <files>/CustomLevels/<song>/Info.dat, has the GUEST parse it
// (StandardLevelInfoSaveData.DeserializeFromJSONString survived), builds the
// preview objects with the guest's own constructors through the IL2CPP
// embedding API, and hands BeatmapLevelsModel the pack collection its loader
// would have. Nothing in the guest image is written: these are calls, made
// between frames on the thread that runs them, which is why this works on a
// headset where a hook cannot.
//
// STAGE 1 ONLY: the levels are LISTED. Selecting one asks BeatmapLevelsModel for
// the playable level (audio clip + beatmap data), which is the other thing the
// stripped loader did and is not here yet.
//
// Knobs: KL_SONGS=0 turns this off. KL_SONGS_CATEGORY=0 leaves
// LevelFilteringNavigationController._enableCustomLevels alone, i.e. hides the
// game's own "Custom Levels" tab again (the levels are then under "All" only).
// On by default because the one thing that tab calls into the model,
// ReloadCustomLevelPackCollectionAsync, was measured on the host with
// KL_SONGS_TEST_RELOAD=1: the task runs to completion and the collection is
// kept. What the tab's own view code does on a headset is not measured yet.
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <CommonCrypto/CommonDigest.h>

#include "klepton.h"
#include "kl_env.h"
#include "kl_jni.h"
#include "kl_songs.h"

typedef struct Il2CppDomain   Il2CppDomain;
typedef struct Il2CppAssembly Il2CppAssembly;
typedef struct Il2CppImage    Il2CppImage;
typedef struct Il2CppClass    Il2CppClass;
typedef struct MethodInfo     MethodInfo;
typedef struct Il2CppObject   Il2CppObject;

static struct {
    Il2CppDomain     *(*domain_get)(void);
    const Il2CppAssembly **(*domain_get_assemblies)(const Il2CppDomain *, size_t *);
    const Il2CppImage *(*assembly_get_image)(const Il2CppAssembly *);
    Il2CppClass      *(*class_from_name)(const Il2CppImage *, const char *, const char *);
    const MethodInfo *(*class_get_method_from_name)(Il2CppClass *, const char *, int);
    void             *(*class_get_field_from_name)(Il2CppClass *, const char *);
    size_t            (*field_get_offset)(void *);
    Il2CppObject     *(*runtime_invoke)(const MethodInfo *, void *, void **, Il2CppObject **);
    Il2CppObject     *(*object_new)(Il2CppClass *);
    Il2CppObject     *(*array_new)(Il2CppClass *, uintptr_t);
    void             *(*object_unbox)(Il2CppObject *);
    Il2CppObject     *(*string_new)(const char *);
    uint32_t          (*array_length)(Il2CppObject *);
    Il2CppClass      *(*object_get_class)(Il2CppObject *);
    const char       *(*class_get_name)(Il2CppClass *);
    void             *(*class_get_type)(Il2CppClass *);
    Il2CppObject     *(*type_get_object)(void *);
    // Optional: only an incremental collector needs it, and a build without
    // one may not export it.
    void              (*gc_wbarrier_set_field)(Il2CppObject *, void **, void *);
} il;

// v24 / Unity 2019.4, LP64 — the same three offsets kl_mprobe.c reads.
#define STR_LEN   16
#define STR_CHARS 20
#define ARR_DATA  32

#define LOG(...) fprintf(stderr, "  [songs] " __VA_ARGS__)

static int songs_bind(void) {
    static int state;                 // 0 untried, 1 up, -1 unavailable
    if (state) return state > 0;
    state = -1;
    kl_image *img = kl_find_image("libil2cpp.so");
    if (!img) return 0;
#define BIND(field, sym) \
    do { *(void **)&il.field = kl_sym(img, sym); \
         if (!il.field) { LOG("libil2cpp exports no %s\n", sym); return 0; } } while (0)
    BIND(domain_get,                 "il2cpp_domain_get");
    BIND(domain_get_assemblies,      "il2cpp_domain_get_assemblies");
    BIND(assembly_get_image,         "il2cpp_assembly_get_image");
    BIND(class_from_name,            "il2cpp_class_from_name");
    BIND(class_get_method_from_name, "il2cpp_class_get_method_from_name");
    BIND(class_get_field_from_name,  "il2cpp_class_get_field_from_name");
    BIND(field_get_offset,           "il2cpp_field_get_offset");
    BIND(runtime_invoke,             "il2cpp_runtime_invoke");
    BIND(object_new,                 "il2cpp_object_new");
    BIND(array_new,                  "il2cpp_array_new");
    BIND(object_unbox,               "il2cpp_object_unbox");
    BIND(string_new,                 "il2cpp_string_new");
    BIND(array_length,               "il2cpp_array_length");
    BIND(object_get_class,           "il2cpp_object_get_class");
    BIND(class_get_name,             "il2cpp_class_get_name");
    BIND(class_get_type,             "il2cpp_class_get_type");
    BIND(type_get_object,            "il2cpp_type_get_object");
#undef BIND
    *(void **)&il.gc_wbarrier_set_field = kl_sym(img, "il2cpp_gc_wbarrier_set_field");
    state = 1;
    return 1;
}

static Il2CppClass *cls(const char *ns, const char *name) {
    Il2CppDomain *dom = il.domain_get();
    if (!dom) return NULL;
    size_t n = 0;
    const Il2CppAssembly **as = il.domain_get_assemblies(dom, &n);
    for (size_t i = 0; i < n; i++) {
        const Il2CppImage *im = il.assembly_get_image(as[i]);
        Il2CppClass *k = im ? il.class_from_name(im, ns, name) : NULL;
        if (k) return k;
    }
    return NULL;
}

static void str_ascii(Il2CppObject *s, char *out, size_t cap) {
    out[0] = 0;
    if (!s) return;
    int32_t len = *(int32_t *)((uint8_t *)s + STR_LEN);
    const uint16_t *ch = (const uint16_t *)((uint8_t *)s + STR_CHARS);
    size_t o = 0;
    for (int32_t i = 0; i < len && o + 1 < cap; i++)
        out[o++] = (ch[i] >= 0x20 && ch[i] < 0x7f) ? (char)ch[i] : '?';
    out[o] = 0;
}

// Every managed call goes through here, so a throw is a named line in the log
// and a NULL to the caller instead of an unwinding guest.
static Il2CppObject *call(Il2CppObject *self, Il2CppClass *k, const char *name,
                          int argc, void **args) {
    const MethodInfo *m = k ? il.class_get_method_from_name(k, name, argc) : NULL;
    if (!m) {
        LOG("%s has no %s/%d\n", k ? il.class_get_name(k) : "(no class)", name, argc);
        return NULL;
    }
    Il2CppObject *exc = NULL;
    Il2CppObject *r = il.runtime_invoke(m, self, args, &exc);
    if (exc) {
        Il2CppClass *ek = il.object_get_class(exc);
        char msg[200] = "";
        const MethodInfo *gm = il.class_get_method_from_name(ek, "get_Message", 0);
        Il2CppObject *exc2 = NULL;
        if (gm) str_ascii(il.runtime_invoke(gm, exc, NULL, &exc2), msg, sizeof msg);
        LOG("%s.%s threw %s: %s\n", il.class_get_name(k), name, il.class_get_name(ek), msg);
        return NULL;
    }
    return r;
}

// A method on the object's own class — for interface-typed values and nested
// types, where the class is easier to ask the object for than to name.
static Il2CppObject *ocall(Il2CppObject *self, const char *name, int argc, void **args) {
    return self ? call(self, il.object_get_class(self), name, argc, args) : NULL;
}

static float ofloat(Il2CppObject *self, const char *name) {
    Il2CppObject *r = ocall(self, name, 0, NULL);
    return r ? *(float *)il.object_unbox(r) : 0.0f;
}

static void **field_slot(Il2CppObject *o, const char *name) {
    if (!o) return NULL;
    void *f = il.class_get_field_from_name(il.object_get_class(o), name);
    if (!f) { LOG("%s has no field %s\n", il.class_get_name(il.object_get_class(o)), name);
              return NULL; }
    return (void **)((uint8_t *)o + il.field_get_offset(f));
}

static Il2CppObject *field_obj(Il2CppObject *o, const char *name) {
    void **slot = field_slot(o, name);
    return slot ? (Il2CppObject *)*slot : NULL;
}

static void store_ref(Il2CppObject *owner, void **slot, Il2CppObject *v) {
    if (il.gc_wbarrier_set_field) il.gc_wbarrier_set_field(owner, slot, v);
    else *slot = v;
}

static Il2CppObject *ref_array(Il2CppClass *elem, Il2CppObject **items, unsigned n) {
    Il2CppObject *a = il.array_new(elem, n);
    if (!a) return NULL;
    void **data = (void **)((uint8_t *)a + ARR_DATA);
    for (unsigned i = 0; i < n; i++) store_ref(a, &data[i], items[i]);
    return a;
}

static Il2CppObject *type_of(Il2CppClass *k) {
    void *t = k ? il.class_get_type(k) : NULL;
    return t ? il.type_get_object(t) : NULL;
}

// Resources.FindObjectsOfTypeAll: instances AND loaded prefabs, which is the
// point — the loader's defaults live on a prefab nothing instantiates here.
static Il2CppObject *find_all(Il2CppClass *k) {
    static Il2CppClass *res;
    if (!res) res = cls("UnityEngine", "Resources");
    void *args[1] = { type_of(k) };
    return args[0] ? call(NULL, res, "FindObjectsOfTypeAll", 1, args) : NULL;
}

static Il2CppObject *array_at(Il2CppObject *a, uint32_t i) {
    return (a && i < il.array_length(a))
         ? ((Il2CppObject **)((uint8_t *)a + ARR_DATA))[i] : NULL;
}

// ---- reading a song folder -------------------------------------------------

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (n >= 0 && n < (8 << 20)) ? malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[n] = 0; if (len) *len = (size_t)n; }
    return buf;
}

// The id the game keys scores and favourites on. The PC loader's is
// "custom_level_" + a SHA-1 over the level's files; this is the same shape
// from Info.dat alone, which is stable for a folder nobody edits and all
// Stage 1 needs.
static void level_id(const char *info, size_t len, char out[64]) {
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(info, (CC_LONG)len, d);
    int o = snprintf(out, 64, "custom_level_");
    for (int i = 0; i < 20; i++) o += snprintf(out + o, 64 - (size_t)o, "%02X", d[i]);
}

static int difficulty_value(const char *name) {
    static const char *const N[] = { "Easy", "Normal", "Hard", "Expert", "ExpertPlus" };
    for (int i = 0; i < 5; i++) if (!strcmp(name, N[i])) return i;
    return -1;
}

// The loader's serialized defaults, off its prefab: what a level falls back to
// for its environment and what the pack shows as its cover.
static struct {
    Il2CppObject *characteristics, *env, *env360, *cover, *small_cover, *sprites;
} g_def;

static char g_first_id[64];           // one id to look up afterwards, as the proof

// StandardLevelInfoSaveData.difficultyBeatmapSets -> PreviewDifficultyBeatmapSet[].
// A characteristic this build does not know (a mod's "Lawless") is skipped,
// as the PC loader skips it.
static Il2CppObject *preview_sets(Il2CppObject *save) {
    static Il2CppClass *k_set, *k_diff;
    if (!k_set)  k_set  = cls("", "PreviewDifficultyBeatmapSet");
    if (!k_diff) k_diff = cls("", "BeatmapDifficulty");
    Il2CppObject *sets = ocall(save, "get_difficultyBeatmapSets", 0, NULL);
    uint32_t n = sets ? il.array_length(sets) : 0;
    Il2CppObject *out[16];
    unsigned no = 0;
    for (uint32_t i = 0; i < n && no < 16; i++) {
        Il2CppObject *set = array_at(sets, i);
        char cname[64];
        str_ascii(field_obj(set, "_beatmapCharacteristicName"), cname, sizeof cname);
        void *a1[1] = { il.string_new(cname) };
        Il2CppObject *ch = ocall(g_def.characteristics,
                                 "GetBeatmapCharacteristicBySerializedName", 1, a1);
        if (!ch) { LOG("    characteristic '%s' unknown to this build — skipped\n", cname);
                   continue; }
        Il2CppObject *maps = field_obj(set, "_difficultyBeatmaps");
        uint32_t nm = maps ? il.array_length(maps) : 0;
        int32_t vals[8];
        unsigned nv = 0;
        for (uint32_t j = 0; j < nm && nv < 8; j++) {
            char dname[32];
            str_ascii(field_obj(array_at(maps, j), "_difficulty"), dname, sizeof dname);
            int v = difficulty_value(dname);
            if (v >= 0) vals[nv++] = v;
        }
        if (!nv) continue;
        Il2CppObject *darr = il.array_new(k_diff, nv);
        memcpy((uint8_t *)darr + ARR_DATA, vals, nv * sizeof vals[0]);
        Il2CppObject *ps = il.object_new(k_set);
        void *a2[2] = { ch, darr };
        call(ps, k_set, ".ctor", 2, a2);
        out[no++] = ps;
    }
    return ref_array(k_set, out, no);
}

static Il2CppObject *preview_level(const char *dir) {
    static Il2CppClass *k_save, *k_prev;
    if (!k_save) k_save = cls("", "StandardLevelInfoSaveData");
    if (!k_prev) k_prev = cls("", "CustomPreviewBeatmapLevel");

    char path[1200];
    size_t len = 0;
    snprintf(path, sizeof path, "%s/Info.dat", dir);
    char *info = slurp(path, &len);
    if (!info) { snprintf(path, sizeof path, "%s/info.dat", dir); info = slurp(path, &len); }
    if (!info) return NULL;                    // not a song folder; not worth a line

    char id[64];
    level_id(info, len, id);
    void *a1[1] = { il.string_new(info) };
    free(info);
    Il2CppObject *save = call(NULL, k_save, "DeserializeFromJSONString", 1, a1);
    if (!save) { LOG("  %s: Info.dat did not parse\n", dir); return NULL; }

    float bpm = ofloat(save, "get_beatsPerMinute"),  off = ofloat(save, "get_songTimeOffset"),
          shf = ofloat(save, "get_shuffle"),         shp = ofloat(save, "get_shufflePeriod"),
          pst = ofloat(save, "get_previewStartTime"), pdu = ofloat(save, "get_previewDuration");
    Il2CppObject *name = ocall(save, "get_songName", 0, NULL);
    Il2CppObject *sets = preview_sets(save);
    if (!sets || !il.array_length(sets)) { LOG("  %s: no playable difficulty\n", dir);
                                           return NULL; }
    void *args[18] = {
        g_def.cover, save, il.string_new(dir), g_def.sprites, il.string_new(id),
        name, ocall(save, "get_songSubName", 0, NULL),
        ocall(save, "get_songAuthorName", 0, NULL),
        ocall(save, "get_levelAuthorName", 0, NULL),
        &bpm, &off, &shf, &shp, &pst, &pdu,
        g_def.env, g_def.env360, sets,
    };
    Il2CppObject *lvl = il.object_new(k_prev);
    const MethodInfo *ctor = il.class_get_method_from_name(k_prev, ".ctor", 18);
    Il2CppObject *exc = NULL;
    if (ctor) il.runtime_invoke(ctor, lvl, args, &exc);
    if (!ctor || exc) { LOG("  %s: CustomPreviewBeatmapLevel..ctor failed\n", dir); return NULL; }

    if (!g_first_id[0]) snprintf(g_first_id, sizeof g_first_id, "%s", id);
    char nm[96];
    str_ascii(name, nm, sizeof nm);
    LOG("  + \"%s\"  %.0f bpm, %u characteristic(s)  %s\n", nm, bpm,
        il.array_length(sets), id);
    return lvl;
}

// ---- the model ---------------------------------------------------------------

// The one BeatmapLevelsModel that has run Init — its prefab twin, which
// FindObjectsOfTypeAll also returns, has no pack collection yet.
static Il2CppObject *live_model(void) {
    static Il2CppClass *k;
    if (!k) k = cls("", "BeatmapLevelsModel");
    Il2CppObject *all = find_all(k);
    uint32_t n = all ? il.array_length(all) : 0;
    for (uint32_t i = 0; i < n; i++) {
        Il2CppObject *m = array_at(all, i);
        if (ocall(m, "get_allLoadedBeatmapLevelPackCollection", 0, NULL)) return m;
    }
    return NULL;
}

static unsigned pack_count(Il2CppObject *model) {
    Il2CppObject *c = ocall(model, "get_allLoadedBeatmapLevelPackCollection", 0, NULL);
    Il2CppObject *a = field_obj(c, "_beatmapLevelPacks");
    return a ? il.array_length(a) : 0;
}

// "All loaded packs", with ours in it.
//
// The PC build's UpdateAllLoadedBeatmapLevelPacks appends _customLevelPackCollection
// itself. This one does not (measured: 24 packs before the call and 24 after,
// with the field set) — the append went with the loader. So the list is
// rebuilt here, from the game's own array plus the one pack, and handed back
// through the same field.
//
// The id index is filled by hand as well, and NOT by UpdateLoadedPreviewLevels:
// that method starts by rebuilding the very list this just replaced (measured
// too — calling it put the count back to 24 every time), so it can only ever
// index a list without ours.
//
// Repeatable, because the game rebuilds that list whenever its DLC state
// changes and ours is not in what it builds. Returns 1 if it had to add it.
static int fold_into_all(Il2CppObject *model) {
    Il2CppObject *mine = array_at(field_obj(field_obj(model, "_customLevelPackCollection"),
                                            "_beatmapLevelPacks"), 0);
    void **slot = field_slot(model, "_allLoadedBeatmapLevelPackCollection");
    Il2CppObject *packs = slot ? field_obj((Il2CppObject *)*slot, "_beatmapLevelPacks") : NULL;
    if (!mine || !packs) return 0;
    uint32_t n = il.array_length(packs);
    for (uint32_t i = 0; i < n; i++) if (array_at(packs, i) == mine) return 0;

    static Il2CppClass *k_ipack, *k_pcoll;
    if (!k_ipack) k_ipack = cls("", "IBeatmapLevelPack");
    if (!k_pcoll) k_pcoll = cls("", "BeatmapLevelPackCollection");
    Il2CppObject *items[512];
    if (n + 1 > 512) return 0;
    for (uint32_t i = 0; i < n; i++) items[i] = array_at(packs, i);
    items[n] = mine;
    Il2CppObject *all = il.object_new(k_pcoll);
    void *a[1] = { ref_array(k_ipack, items, n + 1) };
    call(all, k_pcoll, ".ctor", 1, a);
    store_ref(model, slot, all);

    // _loadedPreviewBeatmapLevels[levelID] = level, for each of ours. The
    // collection's IReadOnlyList is the array inject() built, so it reads as one.
    Il2CppObject *index  = field_obj(model, "_loadedPreviewBeatmapLevels");
    Il2CppObject *levels = field_obj(ocall(mine, "get_beatmapLevelCollection", 0, NULL),
                                     "_customPreviewBeatmapLevels");
    for (uint32_t i = 0; index && levels && i < il.array_length(levels); i++) {
        Il2CppObject *lvl = array_at(levels, i);
        void *kv[2] = { ocall(lvl, "get_levelID", 0, NULL), lvl };
        if (kv[0]) ocall(index, "set_Item", 2, kv);
    }
    return 1;
}

// 1 = injected, 0 = the game is not that far yet, -1 = nothing to do, ever.
static int inject(const char *root) {
    Il2CppObject *model = live_model();
    if (!model) return 0;

    // The loader's prefab, not an instance: nothing instantiates it in this
    // build, and BeatmapLevelsModel holds no reference to it (the field that
    // names it, _customLevelLoaderPrefab, is on another class). A prefab some
    // loaded object references is itself loaded, which is all this needs.
    Il2CppObject *loaders = find_all(cls("", "CustomLevelLoader")), *loader = NULL;
    for (uint32_t i = 0; loaders && i < il.array_length(loaders) && !loader; i++)
        if (field_obj(array_at(loaders, i), "_defaultEnvironmentInfo"))
            loader = array_at(loaders, i);
    if (!loader) { LOG("no CustomLevelLoader prefab is loaded — no defaults to build with\n");
                   return -1; }
    g_def.characteristics = field_obj(loader, "_beatmapCharacteristicCollection");
    g_def.env             = field_obj(loader, "_defaultEnvironmentInfo");
    g_def.env360          = field_obj(loader, "_defaultAllDirectionsEnvironmentInfo");
    g_def.cover           = field_obj(loader, "_defaultPackCover");
    g_def.small_cover     = field_obj(loader, "_smallDefaultPackCover");
    g_def.sprites         = array_at(find_all(cls("", "CachedMediaAsyncLoader")), 0);
    if (!g_def.characteristics || !g_def.env) {
        LOG("the loader prefab has lost its defaults — cannot build levels\n");
        return -1;
    }

    DIR *d = opendir(root);
    if (!d) return -1;
    Il2CppObject *levels[256];
    unsigned n = 0;
    LOG("scanning %s\n", root);
    for (struct dirent *e; (e = readdir(d)) != NULL && n < 256; ) {
        if (e->d_name[0] == '.') continue;
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/%s", root, e->d_name);
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        Il2CppObject *lvl = preview_level(dir);
        if (lvl) levels[n++] = lvl;
    }
    closedir(d);
    if (!n) { LOG("no song folders with an Info.dat\n"); return -1; }

    Il2CppClass *k_prev = cls("", "CustomPreviewBeatmapLevel");
    Il2CppClass *k_coll = cls("", "CustomBeatmapLevelCollection");
    Il2CppClass *k_pack = cls("", "CustomBeatmapLevelPack");
    Il2CppClass *k_ipack = cls("", "IBeatmapLevelPack");
    Il2CppClass *k_pcoll = cls("", "BeatmapLevelPackCollection");
    if (!k_coll || !k_pack || !k_ipack || !k_pcoll) return -1;

    Il2CppObject *coll = il.object_new(k_coll);
    void *a1[1] = { ref_array(k_prev, levels, n) };
    call(coll, k_coll, ".ctor", 1, a1);

    Il2CppObject *pack = il.object_new(k_pack);
    void *a6[6] = { il.string_new("custom_levelPack_CustomLevels"),
                    il.string_new("Custom Levels"), il.string_new("Custom Levels"),
                    g_def.cover, g_def.small_cover ? g_def.small_cover : g_def.cover, coll };
    call(pack, k_pack, ".ctor", 6, a6);

    Il2CppObject *pcoll = il.object_new(k_pcoll);
    void *a2[1] = { ref_array(k_ipack, &pack, 1) };
    call(pcoll, k_pcoll, ".ctor", 1, a2);

    void **slot = field_slot(model, "_customLevelPackCollection");
    if (!slot) return -1;
    unsigned before = pack_count(model);
    store_ref(model, slot, pcoll);
    fold_into_all(model);
    // The proof that the game can SEE it, rather than that we stored it: its
    // own lookup, by the id a level list or a score table would use.
    void *aid[1] = { il.string_new(g_first_id) };
    Il2CppObject *found = ocall(model, "GetLevelPreviewForLevelId", 1, aid);
    LOG("%u level(s) in pack \"Custom Levels\"; the model lists %u pack(s), was %u; "
        "lookup by id %s\n", n, pack_count(model), before, found ? "FINDS it" : "finds NOTHING");

    // KL_SONGS_TEST_RELOAD: the measurement the Custom Levels tab is waiting
    // on. Call what that tab calls and see what is left of the collection.
    if (kl_env_on("KL_SONGS_TEST_RELOAD", 0)) {
        void *token[2] = { NULL, NULL };       // default(CancellationToken)
        void *a[1] = { token };
        Il2CppObject *task = ocall(model, "ReloadCustomLevelPackCollectionAsync", 1, a);
        Il2CppObject *st = task ? ocall(task, "get_Status", 0, NULL) : NULL;
        LOG("reload test: task %s, status %d; collection %s; the model lists %u pack(s)\n",
            task ? "returned" : "NULL", st ? *(int32_t *)il.object_unbox(st) : -1,
            *slot == (void *)pcoll ? "KEPT" : (*slot ? "REPLACED" : "CLEARED"),
            pack_count(model));
    }
    return 1;
}

// The tab is a serialized bool on a menu controller that is rebuilt with the
// menu scene, so it is raised again whenever an instance turns up without it.
static void raise_category(void) {
    static Il2CppClass *k;
    if (!k) k = cls("", "LevelFilteringNavigationController");
    Il2CppObject *all = find_all(k);
    uint32_t n = all ? il.array_length(all) : 0;
    for (uint32_t i = 0; i < n; i++) {
        void **slot = field_slot(array_at(all, i), "_enableCustomLevels");
        if (slot && !*(uint8_t *)slot) {
            *(uint8_t *)slot = 1;
            LOG("Custom Levels tab enabled on a LevelFilteringNavigationController\n");
        }
    }
}

void kl_songs_tick(unsigned frame) {
    static int state;                 // 0 waiting, 1 injected, -1 off
    static char root[1024];
    if (state < 0) return;
    if (state == 0) {
        // Not on the first frames: the app's own installers are still running
        // and BeatmapLevelsModel.Init has not built its collections.
        if (frame < 90 || frame % 30) return;
        if (!root[0]) {
            const char *files = kl_jni_files_dir();
            struct stat st;
            if (!kl_env_on("KL_SONGS", 1) || !files) { state = -1; return; }
            snprintf(root, sizeof root, "%s/CustomLevels", files);
            if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) { state = -1; return; }
            if (!songs_bind() || !cls("", "CustomPreviewBeatmapLevel")) {
                LOG("%s exists, but this guest has no custom-level classes\n", root);
                state = -1; return;
            }
        }
        int r = inject(root);
        if (r) state = r;
        return;
    }
    if (frame % 90) return;
    Il2CppObject *model = live_model();
    if (model && fold_into_all(model))
        LOG("the game rebuilt its pack list without ours — folded back in\n");
    if (kl_env_on("KL_SONGS_CATEGORY", 1)) raise_category();
}
