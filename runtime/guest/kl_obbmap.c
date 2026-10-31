// UE4 OBB read-through VFS.
//
// A UE4 Android title keeps its content in OBBs; on a real device the engine's
// FAndroidPlatformFile mounts them as a zip-union and serves the guest's reads
// of <UE4Game>/<Project>/Content/Paks/foo.pak straight out of the archive.
// Klepton has no such union, so the guest's open() of the loose path failed and
// the game (Asgard's Wrath 2, TWD2) sat forever on its loading screen.
//
// This serves those reads DIRECTLY from the OBB, without extracting anything.
// Every entry in these OBBs is STORED (uncompressed) — verified across all 82
// of AW2's archives — so a loose pak is just a contiguous byte range inside its
// OBB, and "opening" it is opening the OBB and remembering that range. Reads are
// translated into it. Nothing is copied, nothing is deleted, disk never doubles,
// and a rotated app container needs only the OBBs that staging always provides —
// which is why this replaced the earlier extract-and-delete approach (that lost
// data when the container rotated between runs).
//
// UE4-only: kl_ue4_configure is the sole caller of kl_obbmap_init, so the index
// is empty for every other guest and the open/read/stat hooks are branches
// never taken. Unity reaches its OBB through the Java ZipFile shim (kl_jni_io.c),
// a separate path this does not touch.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include "kl_obbmap.h"
#include "kl_extension.h"
#include <zlib.h>
#include "kl_env.h"

#define EOCD_SIG 0x06054b50u
#define CEN_SIG  0x02014b50u

typedef struct {
    char     loose[1024];   // container-absolute path the guest opens
    char     obb[1024];     // archive it lives in
    uint64_t lhdr, size;    // local-header offset and (uncompressed) size
    uint16_t method;        // 0 stored (served); 8 deflate (refused, see below)
    uint8_t  is_loose;      // 1: `obb` IS the data file (a loose container in the
                            // OBB dir), data at offset 0, no ZIP local header
    uint8_t  is_abs;        // 1: lhdr IS the absolute data offset in `obb` — an
                            // entry INSIDE a pak (the ICU read-through below),
                            // where no ZIP local header exists to parse
    uint8_t  is_zpak;       // 1: a ZLIB-compressed pak-internal entry; lhdr is
                            // the absolute offset of the pak ENTRY (its 53-byte
                            // header, which carries the block ranges), size the
                            // uncompressed size. Served by inflating into an
                            // unlinked tempfile at open (kl_obbmap_open).
} obb_entry;

static obb_entry *g_ent;
static unsigned   g_n, g_cap;
// Audio banks indexed by BASENAME, for sound engines that read their banks by a
// native path unrelated to the OBB layout (Wwise fopen(<files>/Init.bnk), FMOD).
// The banks live in the OBB (zix: assets/Audio/GeneratedSoundBanks/Android/*.bnk
// + Media/*.wem; vampire: Content/.../FMOD/Banks/Mobile/*.bank), all STORED, so
// they serve through the same virtual-fd window as the loose paks.
//
// By basename — and, where several archives carry a file of the same name, by
// the FOLDER it sits in. Batman: Arkham Shadow ships its voice-over as one
// archive per language, each with the same bank names under a folder named
// after the language (assets/Android/de-DE/VO_GameOver.bnk, …/en-US/…), and
// Wwise asks for a localised bank under that folder name. Served by basename
// alone, every such request got the file of whichever archive had been read
// first: with German chosen in the game and both packs on the device, the
// English banks came back, and with them the English lines (seen in a device
// log, 2026-10-11: the 19.9 MB English VO_GameOver.bnk where the German one is
// 21.9 MB). `dir` is that folder's name.
//
// One folder name is not always enough. Mission: ISS ships every Wwise media
// file twice in ONE archive: .../Android/<id>.wem from an older export and
// .../Android/Media/<id>.wem, which the game's Wwise 2024 asks for (it carries
// the `hash` chunk; the old copy has `JUNK` there). For the voice lines both
// copies sit in a folder named English(US) (Android/English(US) and
// Android/Media/English(US)), so the one-name match served the old copy, and
// Mission Control never spoke — music, asked for under "Media", was right.
// `dir2` is the last two folder names; a request whose two match one file's
// gets that file before a one-name match is considered.
typedef struct { char base[128]; char dir[48]; char dir2[96]; char obb[1024]; uint64_t lhdr, size; uint16_t method; } obb_bank;
static obb_bank *g_bank;
static unsigned  g_bankn, g_bankcap;

static int name_is_bank(const char *base) {
    size_t L = strlen(base);
    const char *suf[] = { ".bnk", ".wem", ".bank" };   // .strings.bank ends in .bank
    for (unsigned k = 0; k < sizeof suf/sizeof suf[0]; k++) {
        size_t sl = strlen(suf[k]);
        if (L > sl && strcmp(base + L - sl, suf[k]) == 0) return 1;
    }
    return 0;
}
// The name of the folder a file sits in: "de-DE" for ".../Android/de-DE/x.bnk",
// "" for a bare name.
static void parent_name(const char *path, const char *base, char *out, size_t cap) {
    out[0] = 0;
    if (base <= path) return;
    const char *end = base - 1;                       // the '/' before the basename
    const char *start = end;
    while (start > path && start[-1] != '/') start--;
    size_t n = (size_t)(end - start);
    if (n == 0 || n >= cap) return;
    memcpy(out, start, n); out[n] = 0;
}
// The last two folder names: "Media/English(US)" for
// ".../Android/Media/English(US)/x.wem", "" when there are fewer than two.
static void parent2_name(const char *path, const char *base, char *out, size_t cap) {
    out[0] = 0;
    if (base <= path) return;
    const char *end = base - 1;                       // the '/' before the basename
    const char *start = end;
    while (start > path && start[-1] != '/') start--; // start of the parent
    if (start <= path + 1) return;
    start--;                                          // the '/' before the parent
    while (start > path && start[-1] != '/') start--; // start of the grandparent
    size_t n = (size_t)(end - start);
    if (n == 0 || n >= cap || *start == '/') return;
    memcpy(out, start, n); out[n] = 0;
}
static void bank_add(const char *rel, const char *obbpath, uint16_t method,
                     uint64_t size, uint64_t lhdr) {
    const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
    if (!name_is_bank(base) || method != 0) return;   // STORED only
    if (strlen(base) >= sizeof g_bank[0].base) return;
    if (g_bankn == g_bankcap) {
        g_bankcap = g_bankcap ? g_bankcap * 2 : 64;
        g_bank = realloc(g_bank, g_bankcap * sizeof *g_bank);
    }
    obb_bank *b = &g_bank[g_bankn++];
    snprintf(b->base, sizeof b->base, "%s", base);
    parent_name(rel, base, b->dir, sizeof b->dir);
    parent2_name(rel, base, b->dir2, sizeof b->dir2);
    snprintf(b->obb, sizeof b->obb, "%s", obbpath);
    b->method = method; b->size = size; b->lhdr = lhdr;
}
static obb_bank *bank_find(const char *loose) {
    const char *base = strrchr(loose, '/'); base = base ? base + 1 : loose;
    if (!name_is_bank(base)) return NULL;
    char dir[48], dir2[96];
    parent_name(loose, base, dir, sizeof dir);
    parent2_name(loose, base, dir2, sizeof dir2);
    // The file of that name in the folder asked for, if there is one; otherwise
    // the first of that name, as it always was. KL_OBB_BANK_DIR=0 restores the
    // plain first match.
    static int by_dir = -1;
    if (by_dir < 0) by_dir = kl_env_on("KL_OBB_BANK_DIR", 1);
    obb_bank *first = NULL;
    unsigned same = 0;
    if (by_dir && dir2[0])
        for (unsigned i = 0; i < g_bankn; i++)
            if (strcmp(g_bank[i].base, base) == 0 && strcmp(g_bank[i].dir2, dir2) == 0) {
                static int said;
                if (said < 3) { said++;
                    fprintf(stderr, "  [obb] %s asked for under '%s' -> the file in that "
                                    "folder (%llu bytes)\n", base, dir2,
                            (unsigned long long)g_bank[i].size); }
                return &g_bank[i];
            }
    for (unsigned i = 0; i < g_bankn; i++) {
        if (strcmp(g_bank[i].base, base) != 0) continue;
        if (by_dir && dir[0] && strcmp(g_bank[i].dir, dir) == 0) {
            if (first && first != &g_bank[i]) {
                static int said;
                if (said < 6) { said++;
                    const char *a = strrchr(g_bank[i].obb, '/');
                    fprintf(stderr, "  [obb] %s is in more than one archive; asked for under "
                                    "'%s' -> served from %s\n", base, dir, a ? a + 1 : g_bank[i].obb); }
            }
            return &g_bank[i];
        }
        if (!first) first = &g_bank[i];
        same++;
    }
    if (first && same > 1) {
        // Several of that name and none in the folder asked for: said, because the
        // choice is then the order the archives were read in and nothing better.
        static int said;
        if (said < 6) { said++;
            const char *a = strrchr(first->obb, '/');
            fprintf(stderr, "  [obb] %s is in %u archives and none has it under '%s' "
                            "-> the first one, %s (folder '%s')\n",
                    base, same, dir, a ? a + 1 : first->obb, first->dir); }
    }
    return first;
}
static char       g_base[1024];
static size_t     g_base_len;
static int        g_verbose;

// Virtual fds: a guest fd that is really an OBB fd, plus the byte window inside
// it. read/pread/lseek/fstat/close consult this table before the real call.
typedef struct { int fd; uint64_t base, len; off_t pos; int inuse;
                 char name[48]; unsigned nread; } obb_vfd;
// One slot per pak the guest keeps open AT ONCE. A UE4 title mounts every
// pakchunk and holds each pak's fd open for on-demand file reads, so this must
// exceed the total pak count, not "a few". Wrath2 ships 80 paks; at 64 slots the
// table filled during mount and the LAST paks to be processed — pakchunk0/1/2,
// which hold the global shader library and engine content — silently failed to
// open as windows, so UE4 never mounted them and died with "Missing global
// shader … make sure cooking was successful". 256 covered 80 paks + optional/split
// variants with headroom; the table-full path now shouts instead of failing mute.
// Medal of Honor: Above and Beyond (x3647259232032222) mounts more than 400 paks
// from its OBBs and content bundles: at 256 the last 169 — pakchunk0 among them —
// did not mount, the game asked to quit and faulted at start (2026-10-18). 2048
// slots, and the lookups only walk up to the highest slot ever taken
// (g_vfd_hi), so a title with few paks pays what it paid before.
#define KL_OBB_MAX_VFD 2048
static obb_vfd  g_vfd[KL_OBB_MAX_VFD];
static unsigned g_vfd_hi;   // slots [0, g_vfd_hi) have been used; under g_vfd_lock
// KL_TRACE_PAKREAD: log the first reads the guest makes through each .pak window
// fd. This is the ONE path we cannot otherwise see — the guest parses a pak's own
// index and reads files (e.g. the global shader library) straight from the window,
// unlike the ICU files we pre-index and serve. The offsets reveal whether the
// guest reaches the index region and the file offsets, or reads nothing/garbage.
static int pakread_trace(void) {
    static int v = -1;
    if (v < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        int dflt = t && strcmp(t, "wrath2") == 0;   // env can't be set on device
        v = kl_env_on("KL_TRACE_PAKREAD", dflt);
    }
    return v;
}
static pthread_mutex_t g_vfd_lock = PTHREAD_MUTEX_INITIALIZER;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void index_one(const char *obbpath) {
    FILE *f = fopen(obbpath, "rb");
    if (!f) return;
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return; }
    off_t fsz = ftello(f);
    size_t tail = (fsz < 66000) ? (size_t)fsz : 66000;
    uint8_t *buf = malloc(tail ? tail : 1);
    if (!buf) { fclose(f); return; }
    if (fseeko(f, fsz - (off_t)tail, SEEK_SET) != 0 || fread(buf, 1, tail, f) != tail) {
        free(buf); fclose(f); return;
    }
    ssize_t eocd = -1;
    for (ssize_t i = (ssize_t)tail - 22; i >= 0; i--)
        if (rd32(buf + i) == EOCD_SIG) { eocd = i; break; }
    if (eocd < 0) { free(buf); fclose(f); return; }
    uint32_t count = rd16(buf + eocd + 10);
    uint32_t cdsz  = rd32(buf + eocd + 12);
    uint32_t cdoff = rd32(buf + eocd + 16);
    free(buf);
    if (count == 0xffffu || cdsz == 0xffffffffu || cdoff == 0xffffffffu) {
        fprintf(stderr, "  [obb] %s is ZIP64 — cannot index it\n", obbpath);
        fclose(f); return;
    }
    uint8_t *cd = malloc(cdsz ? cdsz : 1);
    if (!cd) { fclose(f); return; }
    if (fseeko(f, (off_t)cdoff, SEEK_SET) != 0 || fread(cd, 1, cdsz, f) != cdsz) {
        free(cd); fclose(f); return;
    }
    unsigned added = 0;
    size_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= cdsz; i++) {
        if (rd32(cd + p) != CEN_SIG) break;
        uint16_t nlen = rd16(cd + p + 28), xlen = rd16(cd + p + 30), clen = rd16(cd + p + 32);
        if (p + 46 + nlen > cdsz) break;
        char name[768];
        size_t take = nlen < sizeof name - 1 ? nlen : sizeof name - 1;
        memcpy(name, cd + p + 46, take);
        name[take] = '\0';
        // Some UE4 OBBs pack entries at their build-machine STAGED path —
        // "<junk>/Saved/StagedBuilds/<Flavor>/Engine/Content/..." (TWD2) —
        // rather than at the clean deployment-relative path wrath2 uses
        // ("Wrath2/Content/..."). The engine reads them relative to the staged
        // root, so strip everything up to and including "StagedBuilds/<Flavor>/"
        // when present; an entry without the marker is already relative and is
        // left as-is. Without this, prefixed entries index under a path the
        // engine never probes and the game boots to a black screen with no paks.
        const char *rel = name;
        char *sb = strstr(name, "StagedBuilds/");
        if (sb) {
            char *flav = sb + strlen("StagedBuilds/");
            char *slash = strchr(flav, '/');
            if (slash) rel = slash + 1;
        }
        size_t rlen = strlen(rel);
        if (rlen && rel[rlen - 1] != '/') {
            if (g_n == g_cap) {
                g_cap = g_cap ? g_cap * 2 : 128;
                g_ent = realloc(g_ent, g_cap * sizeof *g_ent);
            }
            obb_entry *e = &g_ent[g_n++];
            memset(e, 0, sizeof *e);   // is_loose MUST start 0: g_ent is realloc'd,
                                       // and a garbage is_loose makes data_offset
                                       // return base 0 (past-header skipped) — the
                                       // pak then mounts at the wrong offset and the
                                       // guest finds no content (UE4 dies in ICU init).
            snprintf(e->loose, sizeof e->loose, "%s/%s", g_base, rel);
            snprintf(e->obb, sizeof e->obb, "%s", obbpath);
            e->method = rd16(cd + p + 10);
            e->size   = rd32(cd + p + 24);
            e->lhdr   = rd32(cd + p + 42);
            bank_add(rel, obbpath, e->method, e->size, e->lhdr);
            added++;
        }
        p += 46u + nlen + xlen + clen;
    }
    free(cd);
    fclose(f);
    if (g_verbose)
        fprintf(stderr, "  [obb] indexed %u entr%s from %s\n", added, added == 1 ? "y" : "ies",
                strrchr(obbpath, '/') ? strrchr(obbpath, '/') + 1 : obbpath);
}

// mkdir -p every directory that an indexed entry sits in, so the guest's
// directory probes (access/opendir of Content/Paks) succeed even though the
// files themselves are virtual — UE4 checks the directory before the paks.
static void make_dirs(void) {
    for (unsigned i = 0; i < g_n; i++) {
        char d[1024];
        snprintf(d, sizeof d, "%s", g_ent[i].loose);
        char *slash = strrchr(d, '/');
        if (!slash) continue;
        *slash = 0;
        for (char *q = d + 1; *q; q++)
            if (*q == '/') { *q = 0; mkdir(d, 0777); *q = '/'; }
        mkdir(d, 0777);
    }
}

// A packaged UE4 build ships no .uproject, and most engines run monolithic
// without one — but some (TWD2) fatally "Failed to open descriptor file
// ../../../<Proj>/<Proj>.uproject" when it is absent. A UE4 project descriptor
// is just JSON, and for a monolithic build its Modules/Plugins lists are
// informational (the code is compiled in), so a minimal valid one clears the
// gate. Written once at init to a temp file; any *.uproject the guest opens
// under the game base is served from it (the resolved path varies, the suffix
// does not).
static char g_uproject[1024];
static void write_synth_uproject(void) {
    snprintf(g_uproject, sizeof g_uproject, "%s/.klepton_synth.uproject", g_base);
    FILE *f = fopen(g_uproject, "wb");
    if (!f) { g_uproject[0] = 0; return; }
    fputs("{\n\t\"FileVersion\": 3,\n\t\"EngineAssociation\": \"\",\n"
          "\t\"Category\": \"\",\n\t\"Description\": \"\"\n}\n", f);
    fclose(f);
}
static int is_uproject(const char *loose) {
    if (!g_uproject[0] || !loose) return 0;
    if (strncmp(loose, g_base, g_base_len) != 0) return 0;
    size_t L = strlen(loose);
    return L >= 9 && strcmp(loose + L - 9, ".uproject") == 0;
}

static obb_entry *find(const char *loose);   // defined below

// The container directory the paks live in, relative to g_base. Derived from an
// already-indexed .pak entry so a loose container joins its siblings at the exact
// path the engine probes; falls back to "<Project>/Content/Paks" (Project =
// basename of the UE4Game/<Project> base) when no pak was indexed from a .obb.
static void paks_dir_from_index(char *out, size_t out_sz) {
    for (unsigned i = 0; i < g_n; i++) {
        const char *lp = g_ent[i].loose;
        size_t L = strlen(lp);
        if (L >= 4 && strcmp(lp + L - 4, ".pak") == 0) {
            const char *slash = strrchr(lp, '/');
            if (slash) { snprintf(out, out_sz, "%.*s", (int)(slash - lp), lp); return; }
        }
    }
    const char *proj = strrchr(g_base, '/');
    proj = proj ? proj + 1 : g_base;
    snprintf(out, out_sz, "%s/%s/Content/Paks", g_base, proj);
}

// Index a loose container file that sits directly in the OBB directory rather
// than inside a *.obb ZIP. UE 4.27 IoStore titles (Wrath2) can ship their
// global.utoc/global.ucas — and the .ucas that hold the engine i18n data ICU
// needs — this way; the *.obb-only scan below skipped them, so the engine found
// no cultures and FICUInternationalization crashed on a null culture. Served
// STORED, whole-file (window base 0, size = file size).
static void index_loose_container(const char *fullpath, const char *paks_dir,
                                  const char *name) {
    struct stat st;
    if (stat(fullpath, &st) != 0 || st.st_size <= 0) return;
    char loose_path[1024];
    snprintf(loose_path, sizeof loose_path, "%s/%s", paks_dir, name);
    if (find(loose_path)) return;      // already served from a .obb ZIP — don't duplicate
    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 128;
        g_ent = realloc(g_ent, g_cap * sizeof *g_ent);
    }
    obb_entry *e = &g_ent[g_n++];
    memset(e, 0, sizeof *e);
    snprintf(e->loose, sizeof e->loose, "%s", loose_path);
    snprintf(e->obb, sizeof e->obb, "%s", fullpath);
    e->size = (uint64_t)st.st_size;
    e->lhdr = 0;
    e->method = 0;              // a bare file is "stored"
    e->is_loose = 1;
}

// Loose files worth serving from the OBB directory: pak + IoStore containers.
static int is_loose_container_name(const char *n) {
    size_t L = strlen(n);
    return (L >= 5 && strcmp(n + L - 5, ".utoc") == 0)
        || (L >= 5 && strcmp(n + L - 5, ".ucas") == 0)
        || (L >= 4 && strcmp(n + L - 4, ".pak")  == 0)
        || (L >= 4 && strcmp(n + L - 4, ".sig")  == 0);
}

static void icu_pak_index(void);   // below kl_obbmap_stat, which it builds on

void kl_obbmap_init(const char *obbdir, const char *ue4game_base) {
    if (!obbdir) return;
    g_verbose = kl_env_on("KL_TRACE_OBB", 0);
    // A UE4 guest passes its UE4Game/<Project> base for loose-path pak serving.
    // A Unity guest passes NULL/"" — there is no such base; only the basename
    // audio-bank index is wanted (Wwise/FMOD read banks by a native path).
    if (ue4game_base && *ue4game_base) {
        snprintf(g_base, sizeof g_base, "%s", ue4game_base);
        g_base_len = strlen(g_base);
    } else {
        g_base[0] = 0; g_base_len = 0;
    }
    DIR *d = opendir(obbdir);
    if (!d) return;
    struct dirent *e;
    unsigned files = 0;
    while ((e = readdir(d)) != NULL) {
        size_t L = strlen(e->d_name);
        // ...and the store's language packs, `<tag>.lang`: the same ZIP as an
        // OBB under another name (Asgard's Wrath 2's de.lang is 73 stored FMOD
        // banks, Wrath2/Content/FMOD/Desktop/VO_*_DE.bank). Unreal's own OBB
        // visitor mounts them beside the .obb files; left out here, the banks
        // of a pack that was on the device were not there for the sound engine.
        int is_obb  = L >= 4 && strcmp(e->d_name + L - 4, ".obb") == 0;
        int is_lang = L >= 5 && strcmp(e->d_name + L - 5, ".lang") == 0;
        if (!is_obb && !is_lang) continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", obbdir, e->d_name);
        index_one(p);
        files++;
    }
    closedir(d);
    // Second pass: loose container files (pak + IoStore .utoc/.ucas) that live
    // directly in the OBB directory rather than inside a *.obb ZIP. The scan
    // above only opened *.obb, so these were staged but never served — which is
    // how a UE 4.27 IoStore title loses the engine i18n data ICU needs. Mapped
    // beside the paks the engine already probes.
    unsigned loose = 0;
    if (g_base_len && (d = opendir(obbdir)) != NULL) {
        char paks_dir[1024];
        paks_dir_from_index(paks_dir, sizeof paks_dir);
        while ((e = readdir(d)) != NULL) {
            if (!is_loose_container_name(e->d_name)) continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", obbdir, e->d_name);
            struct stat st;
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;   // dirs/.obb handled above
            index_loose_container(p, paks_dir, e->d_name);
            loose++;
        }
        closedir(d);
        if (loose)
            fprintf(stderr, "  [obb] %u loose container file(s) (pak/utoc/ucas) served "
                            "from the OBB dir at %s\n", loose, paks_dir);
    }
    if (g_n && g_base_len) {
        make_dirs();
        write_synth_uproject();
        fprintf(stderr, "  [obb] read-through VFS ready: %u entr%s across %u archive(s), "
                        "served from %s (no extraction — read straight from the OBBs)\n",
                g_n, g_n == 1 ? "y" : "ies", files, g_base);
        // IoStore + internationalisation inventory. UE 4.27 IoStore builds cook
        // engine content — including the ICU i18n data FICUInternationalization
        // needs — into .utoc/.ucas containers rather than .pak. Wrath2 dies in
        // FICUInternationalization::Initialize (null FCulture) because it never
        // finds Content/Internationalization: its i18n is in an IoStore global
        // container UE4 can't reach. This line answers the one open question on
        // the next run — are the containers PRESENT here but not reaching the
        // engine (a serving/mount bug) or ABSENT from the staged OBB set (a
        // staging/data gap)? Named entries help pin which .ucas holds the i18n.
        unsigned n_utoc = 0, n_ucas = 0, n_pak = 0, n_i18n = 0;
        char first_utoc[256] = "", first_i18n[256] = "";
        for (unsigned i = 0; i < g_n; i++) {
            const char *lp = g_ent[i].loose;
            size_t L = strlen(lp);
            if (L >= 5 && strcmp(lp + L - 5, ".utoc") == 0) {
                if (!n_utoc) snprintf(first_utoc, sizeof first_utoc, "%s", lp);
                n_utoc++;
            } else if (L >= 5 && strcmp(lp + L - 5, ".ucas") == 0) {
                n_ucas++;
            } else if (L >= 4 && strcmp(lp + L - 4, ".pak") == 0) {
                n_pak++;
            }
            if (strstr(lp, "Internationalization") || strstr(lp, "icudt")) {
                if (!n_i18n) snprintf(first_i18n, sizeof first_i18n, "%s", lp);
                n_i18n++;
            }
        }
        fprintf(stderr, "  [obb] container inventory: %u .pak, %u .utoc, %u .ucas, "
                        "%u internationalisation entr%s%s%s%s%s\n",
                n_pak, n_utoc, n_ucas, n_i18n, n_i18n == 1 ? "y" : "ies",
                n_utoc ? " | first .utoc: " : "", first_utoc,
                n_i18n ? " | first i18n: " : "", first_i18n);
    }
    if (g_bankn)
        fprintf(stderr, "  [obb] audio-bank index: %u bank/media file(s) across %u "
                        "archive(s), served by basename to the sound engine\n",
                g_bankn, files);
    // wrath2's ICU files, served from inside pakchunk0 (per-target; see the
    // function). After the OBB index, so the pak's own window is known.
    icu_pak_index();
}

// Find an indexed entry by its loose path. NULL if not one of ours.
static obb_entry *find(const char *loose) {
    if (!g_n || !loose || !g_base_len) return NULL;   // no base -> basename index only
    if (strncmp(loose, g_base, g_base_len) != 0) return NULL;
    for (unsigned i = 0; i < g_n; i++)
        if (strcmp(g_ent[i].loose, loose) == 0) return &g_ent[i];
    return NULL;
}

// access()/stat() answer: is this loose path an indexed entry, and how big?
int kl_obbmap_stat(const char *loose, long long *size) {
    if (is_uproject(loose)) {
        struct stat st;
        if (stat(g_uproject, &st) == 0) { if (size) *size = (long long)st.st_size; return 1; }
    }
    obb_entry *e = find(loose);
    if (!e) {
        obb_bank *b = bank_find(loose);
        if (b) { if (size) *size = (long long)b->size; return 1; }
        return 0;
    }
    if (size) *size = (long long)e->size;
    return 1;
}

// The shared per-target gate for BOTH halves of the ICU answer: the directory
// synthesis (kl_obbmap_icu_dir) and the pak read-through index (icu_pak_index).
// Default wrath2 only; KL_ICU_DIR_SYNTH overrides either way. See the two users
// for why this must never apply to a title whose ICU already works.
static int icu_synth_on(void) {
    static int on = -1;
    if (on < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        int def = (t && strcmp(t, "wrath2") == 0) ? 1 : 0;
        on = kl_env_on("KL_ICU_DIR_SYNTH", def);
    }
    return on;
}

// ---- ICU pak read-through (wrath2) ----
// wrath2's ICU reads its data with PLAIN open() — not through UE4's pak layer —
// so the files must be reachable as loose paths. They exist only inside
// pakchunk0 (Engine/Content/Internationalization/...). Rather than extracting
// them to disk (mutating the staged data), this indexes the pak's OWN directory
// index at init and adds each STORED file as an obb_entry whose data window is
// the file's bytes inside the pak inside the OBB — the same read-through that
// serves the paks themselves, one level deeper. Compressed entries (zlib/oodle:
// cnvalias.icu, brkitr/*) are skipped and logged once; the locale .res set that
// culture creation needs is stored uncompressed.
//
// Pak format (v11, index unencrypted — verified against this pak): footer at
// EOF-221 = guid(16) encrypted(1) magic(4=0x5A6F12E1) version(4) indexOff(8)
// indexSize(8) hash(20) methods(5x32). Primary index: FString mount, u32 count,
// u64 seed, u32 hasPathHash {off8 sz8 hash20}, u32 hasFullDir {off8 sz8 hash20},
// s32 encodedSize, encoded entries. Full directory index: u32 dirs { FString
// dir, u32 files { FString file, s32 encodedOffset } }. Encoded entry: u32 bits
// (cmi=bits>>23&0x3f; bit31/30/29 = offset/uncompressed/size are 32-bit), then
// offset, uncompressed[, size]. Every accepted entry is validated against the
// duplicated on-disk header at its offset (Offset==0, cmi and sizes echo) so a
// misparse indexes nothing rather than serving garbage. Data begins after that
// 53-byte header.
static int data_offset(FILE *f, const obb_entry *e, uint64_t *out);
static uint32_t icu_r32(const uint8_t *p) { return rd32(p); }
static uint64_t icu_r64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
// FString: >0 utf8 length incl NUL; <0 utf16 count. Returns bytes consumed, 0 on bounds.
static size_t icu_fstr(const uint8_t *b, size_t sz, size_t p, char *out, size_t cap) {
    if (p + 4 > sz) return 0;
    int32_t n = (int32_t)icu_r32(b + p); p += 4;
    if (n > 0) {
        if ((size_t)n > sz - p || (size_t)n > cap) return 0;
        memcpy(out, b + p, (size_t)n); out[n - 1] = 0;
        return 4 + (size_t)n;
    }
    if (n < 0) {                       // utf16: skip (no ICU path is non-ascii)
        size_t bytes = (size_t)(-n) * 2;
        if (bytes > sz - p) return 0;
        out[0] = 0;
        return 4 + bytes;
    }
    out[0] = 0; return 4;
}
static void ent_add_abs(const char *rel, const char *obbpath,
                        uint64_t abs_off, uint64_t size) {
    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_ent = realloc(g_ent, g_cap * sizeof *g_ent);
    }
    obb_entry *e = &g_ent[g_n++];
    memset(e, 0, sizeof *e);
    snprintf(e->loose, sizeof e->loose, "%s/%s", g_base, rel);
    snprintf(e->obb, sizeof e->obb, "%s", obbpath);
    e->lhdr = abs_off; e->size = size; e->method = 0; e->is_abs = 1;
}
static void icu_pak_index(void) {
    if (!icu_synth_on()) return;
    // The engine-content pak: pakchunk0*. Find its data window first.
    char obbpath[1024] = {0};
    uint64_t pak_base = 0, pak_size = 0;
    for (unsigned i = 0; i < g_n; i++) {
        const char *bn = strrchr(g_ent[i].loose, '/');
        bn = bn ? bn + 1 : g_ent[i].loose;
        if (strncmp(bn, "pakchunk0-", 10) == 0 && strstr(bn, ".pak")) {
            FILE *f = fopen(g_ent[i].obb, "rb");
            if (!f) return;
            uint64_t base;
            int ok = data_offset(f, &g_ent[i], &base);
            fclose(f);
            if (!ok) return;
            snprintf(obbpath, sizeof obbpath, "%s", g_ent[i].obb);
            pak_base = base; pak_size = g_ent[i].size;
            break;
        }
    }
    if (!obbpath[0] || pak_size < 221) return;
    int fd = open(obbpath, O_RDONLY);
    if (fd < 0) return;
    uint8_t foot[221];
    unsigned added = 0, skipped = 0, rejected = 0;
    uint8_t *idx = NULL, *fdir = NULL;
    if (pread(fd, foot, 221, (off_t)(pak_base + pak_size - 221)) != 221) goto out;
    if (foot[16] != 0 || icu_r32(foot + 17) != 0x5A6F12E1u) goto out;   // encrypted / not a pak
    {
    uint64_t ioff = icu_r64(foot + 25), isz = icu_r64(foot + 33);
    if (isz < 32 || isz > (64u << 20) || ioff + isz > pak_size) goto out;
    idx = malloc(isz);
    if (!idx || pread(fd, idx, isz, (off_t)(pak_base + ioff)) != (ssize_t)isz) goto out;
    char sbuf[512];
    size_t p = icu_fstr(idx, isz, 0, sbuf, sizeof sbuf);            // mount
    if (!p || p + 16 > isz) goto out;
    p += 4 + 8;                                                     // count, seed
    if (p + 4 > isz) goto out;
    if (icu_r32(idx + p)) { p += 4 + 8 + 8 + 20; } else p += 4;     // path-hash idx
    if (p + 4 > isz || !icu_r32(idx + p)) goto out;                 // need full dir
    p += 4;
    if (p + 16 + 20 + 4 > isz) goto out;
    uint64_t fdo = icu_r64(idx + p), fds = icu_r64(idx + p + 8);
    p += 16 + 20;
    uint32_t enc_sz = icu_r32(idx + p); p += 4;
    if (enc_sz > isz - p) goto out;
    const uint8_t *enc = idx + p;
    if (fds < 4 || fds > (64u << 20) || fdo + fds > pak_size) goto out;
    fdir = malloc(fds);
    if (!fdir || pread(fd, fdir, fds, (off_t)(pak_base + fdo)) != (ssize_t)fds) goto out;

    size_t q = 0;
    uint32_t ndir = icu_r32(fdir); q += 4;
    char dname[512], fname[256];
    for (uint32_t d = 0; d < ndir; d++) {
        size_t c = icu_fstr(fdir, fds, q, dname, sizeof dname);
        if (!c) goto out;
        q += c;
        if (q + 4 > fds) goto out;
        uint32_t nf = icu_r32(fdir + q); q += 4;
        int want = strstr(dname, "Engine/Content/Internationalization") != NULL;
        for (uint32_t k = 0; k < nf; k++) {
            c = icu_fstr(fdir, fds, q, fname, sizeof fname);
            if (!c) goto out;
            q += c;
            if (q + 4 > fds) goto out;
            int32_t eo = (int32_t)icu_r32(fdir + q); q += 4;
            if (!want || eo < 0 || (uint32_t)eo + 4 > enc_sz) continue;
            // decode the encoded entry (validated below, so a misread is inert).
            // Field order per UnrealPak's DecodeEntry, confirmed against this
            // pak's bytes: bits, then CompressionBlockSize IMMEDIATELY (only
            // when the 6-bit field saturates at 0x3f), then offset,
            // uncompressed size, and (compressed only) size.
            size_t ep = (size_t)eo;
            uint32_t bits = icu_r32(enc + ep); ep += 4;
            uint32_t cmi = (bits >> 23) & 0x3f;
            uint64_t off, usz, csz = 0;
            if ((bits & 0x3f) == 0x3f) { if (ep + 4 > enc_sz) continue; ep += 4; }
            if (ep + 8 > enc_sz) continue;
            if (bits & (1u << 31)) { off = icu_r32(enc + ep); ep += 4; }
            else                   { off = icu_r64(enc + ep); ep += 8; }
            if (ep + 8 > enc_sz) continue;
            if (bits & (1u << 30)) { usz = icu_r32(enc + ep); ep += 4; }
            else                   { usz = icu_r64(enc + ep); ep += 8; }
            if (cmi) {
                if (ep + 8 > enc_sz) continue;
                if (bits & (1u << 29)) { csz = icu_r32(enc + ep); ep += 4; }
                else                   { csz = icu_r64(enc + ep); ep += 8; }
            }
            if (cmi > 1) { skipped++; continue; }      // oodle: cannot inflate
            // validate against the duplicated on-disk header (offset field 0,
            // sizes and method echoed) — a misparse indexes nothing
            uint8_t hdr[28];
            if (off + 53 > pak_size || usz > (256u << 20) ||
                pread(fd, hdr, 28, (off_t)(pak_base + off)) != 28 ||
                icu_r64(hdr) != 0 ||
                icu_r64(hdr + 8) != (cmi ? csz : usz) ||
                icu_r64(hdr + 16) != usz || icu_r32(hdr + 24) != cmi) {
                rejected++; continue;
            }
            char rel[768];
            // dname is mount-relative ("../../../Engine/..."); strip leading ../
            const char *dn = dname;
            while (strncmp(dn, "../", 3) == 0) dn += 3;
            snprintf(rel, sizeof rel, "%s%s", dn, fname);
            if (cmi == 0) {
                ent_add_abs(rel, obbpath, pak_base + off + 53, usz);
            } else {
                ent_add_abs(rel, obbpath, pak_base + off, usz);
                g_ent[g_n - 1].is_abs = 0; g_ent[g_n - 1].is_zpak = 1;
            }
            added++;
        }
    }
    }
out:
    free(idx); free(fdir); close(fd);
    if (added || skipped || rejected)
        fprintf(stderr, "  [obb] ICU pak read-through: %u file(s) served from "
                        "pakchunk0 (zlib inflated at open), %u oodle skipped, %u "
                        "failed validation\n", added, skipped, rejected);
    // One sample path, so a lookup miss is diagnosable from the log alone: the
    // guest's open and this line must PRINT the same loose path or nothing else
    // matters.
    if (added)
        fprintf(stderr, "  [obb]   e.g. %s\n", g_ent[g_n - 1].loose);
}

// Is `loose` UE4's ICU data directory (or a subpath of it)? Answered YES while
// the OBB map is live, because the truth the guest is asking about lives where
// it cannot look. FICUInternationalization::Initialize probes
// DirectoryExists(<Project|Engine>/Content/Internationalization) before it will
// load ANY locale data, and picks the first directory that exists. The data is
// really there — cooked into pakchunk0 (wrath2 v5513: 20 icudt / 29
// Internationalization index hits) — but that pak's index is path-hash-only:
// file lookups (FileExists/OpenRead, what ICU uses to actually READ the data)
// resolve, directory queries do not, so the pak layer says no and falls through
// to a loose access()/stat() here, where no loose tree exists either. The "no"
// that results is not a missing feature: with no data directory every
// FindOrMakeCulture returns null and the first HandleLanguageChanged
// dereferences it — signal 11 at +0x30, wrath2's boot crash. Saying "that
// directory exists" here is the merged-view truth, and everything after it goes
// through per-file opens the pak index really serves. Subpaths are matched too
// (…/Internationalization/icudt64l) for the versioned-subdir probe.
int kl_obbmap_icu_dir(const char *loose) {
    // PER-TARGET, default wrath2 only. This lie is load-bearing for wrath2
    // (path-hash-only pak index, see above) but it REGRESSED olar the moment it
    // was global: olar's ICU was already finding its data through the pak layer,
    // and a loose directory that suddenly "exists" changed which data path its
    // init picked — it then open()ed every icudt64l/*.res loose, all ENOENT, and
    // died signal 11 where it previously booted. A title whose ICU already works
    // must not see this answer. KL_ICU_DIR_SYNTH overrides either way.
    if (!icu_synth_on()) return 0;
    if (!g_n || !loose) return 0;
    if (strncmp(loose, g_base, g_base_len) != 0) return 0;
    // ENGINE dir only. UE4 probes <Project>/Content/Internationalization FIRST
    // and uses the first directory that exists — but wrath2's pak carries the
    // ICU tree ONLY under Engine/Content/Internationalization (verified against
    // pakchunk0's index). Blessing the project dir too made ICU pick it, every
    // file read missed the pak and fell through to loose open()s that also
    // failed, and the culture came out null — the same crash the fix exists to
    // end. The project probe must FAIL so UE4 falls to the Engine dir the pak
    // really serves.
    const char *s = strstr(loose, "/Engine/Content/Internationalization");
    if (!s) return 0;
    const char *end = s + strlen("/Engine/Content/Internationalization");
    return *end == 0 || *end == '/';
}

// Resolve where the entry's DATA begins: the local header's own name/extra
// lengths (which can differ from the central directory's) sit before it.
static int data_offset(FILE *f, const obb_entry *e, uint64_t *out) {
    // A loose container is its own file: the data starts at byte 0, there is no
    // ZIP local header to skip.
    if (e->is_loose) { *out = 0; return 1; }
    // A pak-internal entry recorded its absolute data offset directly.
    if (e->is_abs) { *out = e->lhdr; return 1; }
    uint8_t h[30];
    if (fseeko(f, (off_t)e->lhdr, SEEK_SET) != 0 || fread(h, 1, 30, f) != 30) return 0;
    *out = e->lhdr + 30u + rd16(h + 26) + rd16(h + 28);
    return 1;
}

// A FILE* over an OBB entry's byte window, for guests that read banks through
// STDIO (Wwise fopen) rather than open(). A virtual fd cannot back a host FILE*
// — host stdio calls the host read()/lseek(), not Klepton's window-translating
// shims — so this uses funopen with callbacks that pread the OBB window directly.
typedef struct { int fd; uint64_t base, size, pos; } klobb_cookie;
static int klobb_cookie_read(void *c_, char *buf, int n) {
    klobb_cookie *c = c_;
    if (n < 0) return -1;
    if (c->pos >= c->size) return 0;
    uint64_t left = c->size - c->pos;
    size_t want = (uint64_t)n < left ? (size_t)n : (size_t)left;
    ssize_t got = pread(c->fd, buf, want, (off_t)(c->base + c->pos));
    if (got > 0) c->pos += (uint64_t)got;
    return (int)got;
}
static fpos_t klobb_cookie_seek(void *c_, fpos_t off, int whence) {
    klobb_cookie *c = c_;
    int64_t np = whence == SEEK_SET ? off
               : whence == SEEK_CUR ? (int64_t)c->pos + off
               : (int64_t)c->size + off;               // SEEK_END
    if (np < 0) np = 0;
    if ((uint64_t)np > c->size) np = (int64_t)c->size;
    c->pos = (uint64_t)np;
    return (fpos_t)c->pos;
}
static int klobb_cookie_close(void *c_) {
    klobb_cookie *c = c_;
    if (c->fd >= 0) close(c->fd);
    free(c);
    return 0;
}
FILE *kl_obbmap_fopen(const char *loose, const char *mode) {
    if (!loose || !mode || mode[0] != 'r') return NULL;       // read-only
    obb_entry *e = find(loose);
    obb_entry tmp;
    if (!e) {
        obb_bank *b = bank_find(loose);
        if (!b) return NULL;
        memset(&tmp, 0, sizeof tmp);
        snprintf(tmp.obb, sizeof tmp.obb, "%s", b->obb);
        tmp.method = b->method; tmp.size = b->size; tmp.lhdr = b->lhdr;
        e = &tmp;
    }
    if (e->method != 0) return NULL;                          // STORED only
    FILE *probe = fopen(e->obb, "rb");
    if (!probe) return NULL;
    uint64_t base;
    int ok = data_offset(probe, e, &base);
    fclose(probe);
    if (!ok) return NULL;
    int fd = open(e->obb, O_RDONLY);
    if (fd < 0) return NULL;
    klobb_cookie *c = calloc(1, sizeof *c);
    if (!c) { close(fd); return NULL; }
    c->fd = fd; c->base = base; c->size = e->size; c->pos = 0;
    FILE *f = funopen(c, klobb_cookie_read, NULL, klobb_cookie_seek, klobb_cookie_close);
    if (!f) { close(fd); free(c); return NULL; }
    static int said; if (said < 16) { said++;
        const char *bn = strrchr(loose,'/'); bn = bn?bn+1:loose;
        fprintf(stderr, "  [obb] fopen bank %s from the OBB (%llu bytes)\n",
                bn, (unsigned long long)e->size); }
    return f;
}

// open(): if the path is a stored indexed entry, open the OBB, register the
// byte window, and hand the guest that fd. Returns the fd, or -1 for "not ours"
// (the caller then does its normal open, which is correct for real files).
// Serve a ZLIB pak-internal entry: read its on-disk header (authoritative for
// the block ranges), inflate every block, and hand back a real fd over an
// UNLINKED tempfile holding the plain bytes — read/lseek/fstat/close all work
// natively on it, nothing persists, and the staged data is never touched.
static int zpak_open(const obb_entry *e) {
    int ofd = open(e->obb, O_RDONLY);
    if (ofd < 0) return -1;
    int out = -1;
    uint8_t hdr[28];
    uint8_t *cbuf = NULL, *ubuf = NULL;
    if (pread(ofd, hdr, 28, (off_t)e->lhdr) != 28) goto fail;
    uint64_t csz = icu_r64(hdr + 8), usz = icu_r64(hdr + 16);
    if (icu_r32(hdr + 24) != 1 || usz != e->size || usz > (256u << 20)) goto fail;
    uint8_t nbb[4];
    if (pread(ofd, nbb, 4, (off_t)(e->lhdr + 48)) != 4) goto fail;
    uint32_t nb = icu_r32(nbb);
    if (nb == 0 || nb > 4096) goto fail;
    uint64_t (*blk)[2] = calloc(nb, sizeof *blk);
    if (!blk) goto fail;
    for (uint32_t i = 0; i < nb; i++) {
        uint8_t r[16];
        if (pread(ofd, r, 16, (off_t)(e->lhdr + 52 + 16u * i)) != 16) { free(blk); goto fail; }
        uint64_t s = icu_r64(r), en = icu_r64(r + 8);
        // v>=5 stores the ranges relative to the entry; older paks absolute.
        if (s >= e->lhdr) { s -= e->lhdr; en -= e->lhdr; }
        if (en <= s || en - s > csz + 64) { free(blk); goto fail; }
        blk[i][0] = s; blk[i][1] = en;
    }
    ubuf = malloc(usz ? usz : 1);
    if (!ubuf) { free(blk); goto fail; }
    uint64_t done = 0;
    for (uint32_t i = 0; i < nb && done < usz; i++) {
        uint64_t bs = blk[i][1] - blk[i][0];
        cbuf = realloc(cbuf, bs);
        if (!cbuf || pread(ofd, cbuf, bs, (off_t)(e->lhdr + blk[i][0])) != (ssize_t)bs) { free(blk); goto fail; }
        uLongf dl = (uLongf)(usz - done);
        if (uncompress(ubuf + done, &dl, cbuf, (uLong)bs) != Z_OK) { free(blk); goto fail; }
        done += dl;
    }
    free(blk);
    if (done != usz) goto fail;
    {
        const char *td = getenv("TMPDIR");
        char t[1024];
        snprintf(t, sizeof t, "%s/klepton-icu-XXXXXX", td && *td ? td : "/tmp");
        out = mkstemp(t);
        if (out < 0) goto fail;
        unlink(t);                                     // nothing persists
        if (write(out, ubuf, usz) != (ssize_t)usz) { close(out); out = -1; goto fail; }
        lseek(out, 0, SEEK_SET);
    }
fail:
    free(cbuf); free(ubuf); close(ofd);
    return out;
}

int kl_obbmap_locate(const char *loose, char *host, size_t cap, long long *base, long long *size) {
    obb_entry *e = find(loose);
    if (!e || e->is_zpak || (!e->is_loose && !e->is_abs && e->method != 0)) return 0;
    FILE *probe = fopen(e->obb, "rb");
    if (!probe) return 0;
    uint64_t at = 0;
    int ok = data_offset(probe, e, &at);
    fclose(probe);
    if (!ok) return 0;
    if (host) snprintf(host, cap, "%s", e->obb);
    if (base) *base = (long long)at;
    if (size) *size = (long long)e->size;
    return 1;
}

int kl_obbmap_open(const char *loose, int flags) {
    if (is_uproject(loose) && (flags & O_ACCMODE) == O_RDONLY) {
        int fd = open(g_uproject, O_RDONLY);
        if (fd >= 0 && g_verbose)
            fprintf(stderr, "  [obb] synthesized %s -> the minimal descriptor\n",
                    strrchr(loose,'/')?strrchr(loose,'/')+1:loose);
        return fd;   // real file fd — normal read/close, no window translation
    }
    if ((flags & O_ACCMODE) != O_RDONLY) return -1;   // read-only path only
    obb_entry *e = find(loose);
    obb_entry tmp;
    if (!e) {
        obb_bank *b = bank_find(loose);
        if (!b) return -1;
        memset(&tmp, 0, sizeof tmp);
        snprintf(tmp.obb, sizeof tmp.obb, "%s", b->obb);
        tmp.method = b->method; tmp.size = b->size; tmp.lhdr = b->lhdr;
        e = &tmp;
        static int said; if (said < 16) { said++;
            fprintf(stderr, "  [obb] serving audio bank %s from the OBB (%llu bytes) "
                            "— the sound engine reads it by a native path\n",
                    strrchr(loose,'/')?strrchr(loose,'/')+1:loose,
                    (unsigned long long)b->size); }
    }
    if (e->is_zpak) {
        int zfd = zpak_open(e);
        if (zfd >= 0 && g_verbose)
            fprintf(stderr, "  [obb] inflated %s from the pak (%llu bytes)\n",
                    strrchr(loose,'/')?strrchr(loose,'/')+1:loose,
                    (unsigned long long)e->size);
        return zfd;    // a real fd over the plain bytes — no window translation
    }
    if (e->method != 0) {
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [obb] %s is DEFLATE inside the OBB — read-through only "
                            "handles stored entries; letting the open fail\n", loose); }
        return -1;
    }
    FILE *probe = fopen(e->obb, "rb");
    if (!probe) return -1;
    uint64_t base;
    int ok = data_offset(probe, e, &base);
    fclose(probe);
    if (!ok) return -1;
    int fd = open(e->obb, O_RDONLY);
    if (fd < 0) return -1;
    pthread_mutex_lock(&g_vfd_lock);
    for (unsigned i = 0; i < sizeof g_vfd / sizeof g_vfd[0]; i++) {
        if (g_vfd[i].inuse) continue;
        g_vfd[i] = (obb_vfd){ .fd = fd, .base = base, .len = e->size, .pos = 0, .inuse = 1 };
        if (i >= g_vfd_hi) g_vfd_hi = i + 1;
        { const char *bn = strrchr(loose, '/'); bn = bn ? bn + 1 : loose;
          snprintf(g_vfd[i].name, sizeof g_vfd[i].name, "%s", bn); }
        pthread_mutex_unlock(&g_vfd_lock);
        // Unconditional, once per audio bank: "did the guest's sound engine
        // actually open its banks through the read-through" is the question that
        // separates a bank-not-found silence from a mixer-produced one, and it
        // has to be answerable without a trace knob.
        {
            size_t L = strlen(loose);
            const char *suf[] = { ".bank", ".bnk", ".fsb", ".pck", ".wem" };
            for (unsigned k = 0; k < sizeof suf/sizeof suf[0]; k++) {
                size_t sl = strlen(suf[k]);
                if (L >= sl && strcmp(loose + L - sl, suf[k]) == 0) {
                    static const char *said[48]; static unsigned nsaid;
                    int seen = 0;
                    for (unsigned j = 0; j < nsaid; j++)
                        if (strcmp(said[j], loose) == 0) { seen = 1; break; }
                    if (!seen && nsaid < 48) {
                        said[nsaid++] = strdup(loose);
                        fprintf(stderr, "  [obb] audio bank served: %s (%llu bytes)\n",
                                strrchr(loose,'/')?strrchr(loose,'/')+1:loose,
                                (unsigned long long)e->size);
                    }
                    break;
                }
            }
        }
        if (g_verbose)
            fprintf(stderr, "  [obb] open %s -> fd %d (window %llu..%llu in %s)\n",
                    strrchr(loose,'/')?strrchr(loose,'/')+1:loose, fd,
                    (unsigned long long)base, (unsigned long long)(base + e->size),
                    strrchr(e->obb,'/')?strrchr(e->obb,'/')+1:e->obb);
        return fd;
    }
    pthread_mutex_unlock(&g_vfd_lock);
    // Table full — a real failure mode for many-pak titles: the guest can no
    // longer mount this pak, so its files vanish. Shout (a missing pak reads as
    // "missing global shader" or worse, three subsystems away) rather than hand
    // back an untracked fd whose reads would land at the zip header.
    fprintf(stderr, "  [obb] VFD TABLE FULL (%d slots) opening %s — this pak will "
                    "NOT mount; raise KL_OBB_MAX_VFD\n",
            KL_OBB_MAX_VFD, strrchr(loose,'/')?strrchr(loose,'/')+1:loose);
    close(fd);
    return -1;
}

static obb_vfd *vfd_find(int fd) {
    for (unsigned i = 0; i < g_vfd_hi; i++)
        if (g_vfd[i].inuse && g_vfd[i].fd == fd) return &g_vfd[i];
    return NULL;
}

ssize_t kl_obbmap_pread(int fd, void *buf, size_t n, off_t off, int *handled) {
    pthread_mutex_lock(&g_vfd_lock);
    obb_vfd *v = vfd_find(fd);
    if (!v) { pthread_mutex_unlock(&g_vfd_lock); *handled = 0; return -1; }
    uint64_t base = v->base, len = v->len;
    int trace = pakread_trace() && v->nread < 40 && strstr(v->name, ".pak");
    char nm[48]; if (trace) { snprintf(nm, sizeof nm, "%s", v->name); v->nread++; }
    pthread_mutex_unlock(&g_vfd_lock);
    *handled = 1;
    if (trace)
        fprintf(stderr, "  [pakread] %s pread off=%lld n=%zu (len=%llu)\n",
                nm, (long long)off, n, (unsigned long long)len);
    if (off < 0 || (uint64_t)off >= len) return 0;
    if (off + n > len) n = (size_t)(len - off);
    return pread(fd, buf, n, (off_t)(base + off));
}

ssize_t kl_obbmap_read(int fd, void *buf, size_t n, int *handled) {
    pthread_mutex_lock(&g_vfd_lock);
    obb_vfd *v = vfd_find(fd);
    if (!v) { pthread_mutex_unlock(&g_vfd_lock); *handled = 0; return -1; }
    off_t pos = v->pos; uint64_t base = v->base, len = v->len;
    int trace = pakread_trace() && v->nread < 40 && strstr(v->name, ".pak");
    char nm[48]; if (trace) { snprintf(nm, sizeof nm, "%s", v->name); v->nread++; }
    if (pos < 0 || (uint64_t)pos >= len) { pthread_mutex_unlock(&g_vfd_lock); *handled = 1;
        if (trace) fprintf(stderr, "  [pakread] %s read pos=%lld n=%zu -> EOF (len=%llu)\n",
                           nm, (long long)pos, n, (unsigned long long)len);
        return 0; }
    if ((uint64_t)pos + n > len) n = (size_t)(len - pos);
    ssize_t r = pread(fd, buf, n, (off_t)(base + pos));
    if (r > 0) v->pos = pos + r;
    pthread_mutex_unlock(&g_vfd_lock);
    if (trace)
        fprintf(stderr, "  [pakread] %s read pos=%lld n=%zu -> %zd (len=%llu)\n",
                nm, (long long)pos, n, r, (unsigned long long)len);
    *handled = 1;
    return r;
}

off_t kl_obbmap_lseek(int fd, off_t off, int whence, int *handled) {
    pthread_mutex_lock(&g_vfd_lock);
    obb_vfd *v = vfd_find(fd);
    if (!v) { pthread_mutex_unlock(&g_vfd_lock); *handled = 0; return -1; }
    off_t np;
    switch (whence) {
        case SEEK_SET: np = off; break;
        case SEEK_CUR: np = v->pos + off; break;
        case SEEK_END: np = (off_t)v->len + off; break;
        default: pthread_mutex_unlock(&g_vfd_lock); *handled = 1; errno = EINVAL; return -1;
    }
    if (np < 0) { pthread_mutex_unlock(&g_vfd_lock); *handled = 1; errno = EINVAL; return -1; }
    v->pos = np;
    pthread_mutex_unlock(&g_vfd_lock);
    *handled = 1;
    return np;
}

int kl_obbmap_fstat_size(int fd, long long *size, int *handled) {
    pthread_mutex_lock(&g_vfd_lock);
    obb_vfd *v = vfd_find(fd);
    if (!v) { pthread_mutex_unlock(&g_vfd_lock); *handled = 0; return -1; }
    if (size) *size = (long long)v->len;
    pthread_mutex_unlock(&g_vfd_lock);
    *handled = 1;
    return 0;
}

int kl_obbmap_close(int fd, int *handled) {
    pthread_mutex_lock(&g_vfd_lock);
    obb_vfd *v = vfd_find(fd);
    if (!v) { pthread_mutex_unlock(&g_vfd_lock); *handled = 0; return -1; }
    v->inuse = 0;
    pthread_mutex_unlock(&g_vfd_lock);
    *handled = 1;
    return close(fd);
}

// ---- directory synthesis ----
//
// UE4 discovers its paks by LISTING Content/Paks, not by opening them by name:
// the pak files are virtual (served from the OBBs on open), so the directory is
// physically empty and readdir returns nothing — the engine mounts no paks,
// finds no content or localization data, and ForceQuits (TWD2 died exactly so).
// This makes readdir yield the virtual entries: the immediate children (files
// and subdirectories) of the queried directory that exist in the index, merged
// with whatever real entries the directory has, deduplicated.
typedef struct {
    DIR   *real;                 // real directory, if it exists (NULL otherwise)
    char **names;                // virtual immediate-child names
    char  *types;                // 1 = subdir, 0 = file
    unsigned n, i;               // count, and readdir cursor into names
    int    magic;                // identifies this as one of ours
} obb_dir;
#define OBB_DIR_MAGIC 0x0BBD1234

// Register live dir handles so readdir/closedir can tell ours from a real DIR*.
static obb_dir *g_dirs[64];
static pthread_mutex_t g_dir_lock = PTHREAD_MUTEX_INITIALIZER;

static void dir_add_name(obb_dir *d, const char *name, char type) {
    for (unsigned k = 0; k < d->n; k++)
        if (strcmp(d->names[k], name) == 0) return;   // dedup
    d->names = realloc(d->names, (d->n + 1) * sizeof *d->names);
    d->types = realloc(d->types, (d->n + 1) * sizeof *d->types);
    d->names[d->n] = strdup(name);
    d->types[d->n] = type;
    d->n++;
}

void *kl_obbmap_opendir(const char *dirpathin) {
    // Unity indexes OBB audio banks with no Unreal loose-file base. An empty
    // prefix must not claim real Unity directories as synthetic UE4 handles.
    // Only with the mods for now; other games keep the old answer.
    if (!g_n || (!g_base_len && kl_extension_active()) || !dirpathin) return NULL;
    // UE4 lists its pak folder with a trailing slash ("<Project>/Content/Paks/");
    // matching children as "<dir>/<child>" then compares against the slash that
    // is already part of <dir>, and every entry is rejected — the synthesis
    // hands back an empty listing, no pak is discovered, and the title dies at
    // ICU init on content it never mounted. Normalise it away once, up front.
    char dbuf[1024];
    snprintf(dbuf, sizeof dbuf, "%s", dirpathin);
    size_t dl = strlen(dbuf);
    while (dl > 1 && dbuf[dl - 1] == '/') dbuf[--dl] = 0;
    const char *dirpath = dbuf;
    if (strncmp(dirpath, g_base, g_base_len) != 0) return NULL;
    // Collect indexed entries whose loose path is dirpath + "/" + <child...>.
    obb_dir tmp; memset(&tmp, 0, sizeof tmp);
    for (unsigned e = 0; e < g_n; e++) {
        const char *L = g_ent[e].loose;
        size_t Ll = strlen(L);
        if (Ll <= dl + 1 || strncmp(L, dirpath, dl) != 0 || L[dl] != '/') continue;
        const char *rest = L + dl + 1;
        const char *slash = strchr(rest, '/');
        if (slash) {                                   // a subdirectory child
            char sub[256];
            size_t sl = (size_t)(slash - rest);
            if (sl >= sizeof sub) sl = sizeof sub - 1;
            memcpy(sub, rest, sl); sub[sl] = 0;
            dir_add_name(&tmp, sub, 1);
        } else {
            dir_add_name(&tmp, rest, 0);               // a file child
        }
    }
    // the synthetic .uproject, if it belongs directly in this dir
    if (g_uproject[0]) {
        char up[1024];
        snprintf(up, sizeof up, "%s", g_uproject);
        // it is served for any *.uproject under base; expose one entry named
        // "<dir-leaf>.uproject" is not knowable here, so skip — the engine opens
        // it by the command-line path, not by listing. (No dir entry needed.)
    }
    DIR *real = opendir(dirpath);
    if (!tmp.n && !real) return NULL;                  // nothing to offer
    // Unconditional, once per distinct directory: "did the guest list this dir,
    // and how many virtual entries did it get" is the question that says whether
    // UE4 is discovering its paks — and it must be answerable without a trace
    // knob, because a pak set that never mounts is the difference between a game
    // that boots and one that ForceQuits on missing content.
    if (tmp.n) {
        static const char *said[32]; static unsigned nsaid;
        int seen = 0;
        for (unsigned k = 0; k < nsaid; k++)
            if (strcmp(said[k], dirpath) == 0) { seen = 1; break; }
        if (!seen && nsaid < 32) {
            said[nsaid++] = strdup(dirpath);
            fprintf(stderr, "  [obb] guest listed %s -> %u virtual entr%s served\n",
                    dirpath, tmp.n, tmp.n == 1 ? "y" : "ies");
        }
    }
    obb_dir *d = malloc(sizeof *d);
    *d = tmp; d->real = real; d->i = 0; d->magic = OBB_DIR_MAGIC;
    pthread_mutex_lock(&g_dir_lock);
    for (unsigned k = 0; k < sizeof g_dirs / sizeof g_dirs[0]; k++)
        if (!g_dirs[k]) { g_dirs[k] = d; break; }
    pthread_mutex_unlock(&g_dir_lock);
    return d;
}

static int is_our_dir(void *h) {
    if (!h) return 0;
    pthread_mutex_lock(&g_dir_lock);
    int yes = 0;
    for (unsigned k = 0; k < sizeof g_dirs / sizeof g_dirs[0]; k++)
        if (g_dirs[k] == h) { yes = 1; break; }
    pthread_mutex_unlock(&g_dir_lock);
    return yes && ((obb_dir *)h)->magic == OBB_DIR_MAGIC;
}

// Fill one entry. name/type out; returns 1 while entries remain, 0 at end.
int kl_obbmap_readdir(void *h, char *name_out, size_t cap, int *is_dir) {
    if (!is_our_dir(h)) return -1;                     // not ours (sentinel)
    obb_dir *d = h;
    // Drain the real directory first, skipping names we will yield virtually.
    if (d->real) {
        struct dirent *e;
        while ((e = readdir(d->real)) != NULL) {
            int dup = 0;
            for (unsigned k = 0; k < d->n; k++)
                if (strcmp(d->names[k], e->d_name) == 0) { dup = 1; break; }
            if (dup) continue;
            snprintf(name_out, cap, "%s", e->d_name);
            *is_dir = (e->d_type == DT_DIR);
            return 1;
        }
        d->real = NULL;   // exhausted (closedir handles the actual close)
    }
    if (d->i < d->n) {
        snprintf(name_out, cap, "%s", d->names[d->i]);
        *is_dir = d->types[d->i];
        d->i++;
        return 1;
    }
    return 0;
}

int kl_obbmap_closedir(void *h) {
    if (!is_our_dir(h)) return -1;
    obb_dir *d = h;
    pthread_mutex_lock(&g_dir_lock);
    for (unsigned k = 0; k < sizeof g_dirs / sizeof g_dirs[0]; k++)
        if (g_dirs[k] == h) { g_dirs[k] = NULL; break; }
    pthread_mutex_unlock(&g_dir_lock);
    if (d->real) closedir(d->real);
    for (unsigned k = 0; k < d->n; k++) free(d->names[k]);
    free(d->names); free(d->types); free(d);
    return 0;
}

int kl_obbmap_is_vfd(int fd) {
    pthread_mutex_lock(&g_vfd_lock);
    int yes = vfd_find(fd) != NULL;
    pthread_mutex_unlock(&g_vfd_lock);
    return yes;
}
