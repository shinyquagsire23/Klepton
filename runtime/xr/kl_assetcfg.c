// The configured asset-file list — see kl_assetcfg.h for the format and for
// what is rejected.
#include "kl_assetcfg.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define KL_ASSETCFG_LINE 1024

// A plain decimal number above zero, nothing around it, no overflow.
static int assetcfg_number(const char *s, unsigned long long *out) {
    if (!s[0] || strlen(s) > 20) return 0;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || !end || *end || v == 0) return 0;
    *out = v;
    return 1;
}

// The store's metadata of an asset file, as the installer copied it: a short
// token, no blanks (Deadpool VR's is its build's version, 1.0.40.356975.Quest).
static int assetcfg_meta_ok(const char *t) {
    size_t n = strlen(t);
    if (n == 0 || n >= sizeof ((kl_assetcfg_entry *)0)->meta) return 0;
    for (const char *p = t; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-' || *p == '+'))
            return 0;
    return 1;
}

static int assetcfg_tag_ok(const char *t) {
    size_t n = strlen(t);
    if (n == 0 || n > 31) return 0;
    for (const char *p = t; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return 0;
    return 1;
}

// The path as written: no control character or backslash, and every component
// a real name — not empty, not "." and not "..". A leading '/' is allowed here;
// whether that absolute path is inside the data directory is resolve's to say.
static const char *assetcfg_path_bad(const char *f) {
    if (!f[0]) return "empty file name";
    for (const unsigned char *p = (const unsigned char *)f; *p; p++)
        if (*p < 0x20 || *p == 0x7f || *p == '\\') return "control character or backslash in the file name";
    const char *c = f[0] == '/' ? f + 1 : f;
    for (;;) {
        const char *e = strchr(c, '/');
        size_t n = e ? (size_t)(e - c) : strlen(c);
        if (n == 0) return "empty path component";
        if ((n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.'))
            return "'.' or '..' in the path";
        if (!e) break;
        c = e + 1;
    }
    return NULL;
}

int kl_assetcfg_parse_line(const char *line, kl_assetcfg_entry *e, const char **why) {
    const char *dummy;
    if (!why) why = &dummy;
    *why = NULL;
    if (!line || !e) { *why = "no line"; return -1; }
    if (strlen(line) >= KL_ASSETCFG_LINE) { *why = "line too long"; return -1; }
    char buf[KL_ASSETCFG_LINE];
    snprintf(buf, sizeof buf, "%s", line);
    buf[strcspn(buf, "\r\n")] = 0;
    char *s = buf + strspn(buf, " \t");
    if (!*s || *s == '#') return 0;

    char *tok[6];
    int n = 0;
    for (char *save = NULL, *t = strtok_r(s, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
        if (n == 6) { *why = "too many fields"; return -1; }
        tok[n++] = t;
    }
    if (strcmp(tok[0], "asset") != 0) { *why = "not an 'asset' line"; return -1; }
    if (n < 4) { *why = "want: asset <id> <file> <size> [<tag>] [meta=<text>]"; return -1; }

    memset(e, 0, sizeof *e);
    unsigned long long id;
    if (!assetcfg_number(tok[1], &id)) { *why = "asset id is not a decimal number above 0"; return -1; }
    e->id = (uint64_t)id;
    if ((*why = assetcfg_path_bad(tok[2])) != NULL) return -1;
    if (strlen(tok[2]) >= sizeof e->file) { *why = "file name too long"; return -1; }
    snprintf(e->file, sizeof e->file, "%s", tok[2]);
    if (!assetcfg_number(tok[3], &e->size)) { *why = "size is not a decimal number above 0"; return -1; }

    // After the size: a language tag and a meta=<text>, each at most once, in
    // either order.
    const char *tag = NULL;
    for (int i = 4; i < n; i++) {
        if (strncmp(tok[i], "meta=", 5) == 0) {
            if (e->meta[0]) { *why = "meta= given twice"; return -1; }
            if (!assetcfg_meta_ok(tok[i] + 5)) { *why = "meta is not [A-Za-z0-9._+-]{1,63}"; return -1; }
            snprintf(e->meta, sizeof e->meta, "%s", tok[i] + 5);
        } else {
            if (tag) { *why = "want: asset <id> <file> <size> [<tag>] [meta=<text>]"; return -1; }
            if (!assetcfg_tag_ok(tok[i])) { *why = "language tag is not [A-Za-z0-9_-]{1,31}"; return -1; }
            tag = tok[i];
        }
    }
    if (tag) {
        snprintf(e->tag, sizeof e->tag, "%s", tag);
    } else {
        const char *base = strrchr(e->file, '/');
        base = base ? base + 1 : e->file;
        char t[64];
        snprintf(t, sizeof t, "%.*s", (int)strcspn(base, "."), base);
        if (!assetcfg_tag_ok(t)) { *why = "no language tag, and none in the file name"; return -1; }
        snprintf(e->tag, sizeof e->tag, "%s", t);
    }
    return 1;
}

int kl_assetcfg_resolve(kl_assetcfg_entry *e, const char *data_dir, const char **why) {
    const char *dummy;
    if (!why) why = &dummy;
    *why = NULL;
    e->installed = 0;
    e->path[0] = 0;
    if (!data_dir || data_dir[0] != '/') { *why = "the OBB directory is not an absolute path"; return -1; }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", data_dir);
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dir[--dl] = 0;

    // Lexically inside: parse_line has already ruled out "." and "..".
    const char *rel = e->file;
    if (rel[0] == '/') {
        if (strncmp(rel, dir, dl) != 0 || rel[dl] != '/' || !rel[dl + 1]) {
            *why = "absolute path outside the OBB directory";
            return -1;
        }
        rel += dl + 1;
    }
    int w = snprintf(e->path, sizeof e->path, "%s/%s", dl == 1 ? "" : dir, rel);
    if (w < 0 || (size_t)w >= sizeof e->path) { e->path[0] = 0; *why = "path too long"; return -1; }

    struct stat st;
    if (stat(e->path, &st) != 0) return 0;             // listed, not installed
    // ...and really inside: a symbolic link out of the directory is not one
    // of its files.
    char real[PATH_MAX], realdir[PATH_MAX];
    if (!realpath(e->path, real) || !realpath(dir, realdir)) return 0;
    size_t rl = strlen(realdir);
    if (strncmp(real, realdir, rl) != 0 || (rl > 1 && real[rl] != '/')) {
        *why = "the file leads out of the OBB directory";
        return -1;
    }
    // The path stays the one under data_dir as the guest knows it, not the
    // resolved one: on visionOS the container is /var/mobile/..., which
    // realpath spells /private/var/mobile/..., and a game that compares the
    // path we report with the one its own engine built from its OBB directory
    // (Deadpool VR does) finds no match in the second spelling.
    if (S_ISREG(st.st_mode) && (unsigned long long)st.st_size == e->size)
        e->installed = 1;
    return 0;
}

int kl_assetcfg_load(const char *cfg_path, const char *data_dir,
                     kl_assetcfg_entry *out, int max, FILE *log) {
    if (!cfg_path || !data_dir || data_dir[0] != '/' || !out || max <= 0) return -1;
    FILE *f = fopen(cfg_path, "r");
    if (!f) return -1;
    char line[KL_ASSETCFG_LINE + 2];
    int n = 0, no = 0;
    while (fgets(line, sizeof line, f)) {
        no++;
        const char *why = NULL;
        kl_assetcfg_entry e;
        int r;
        if (!strchr(line, '\n') && !feof(f)) {         // longer than any line we take
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            r = -1; why = "line too long";
        } else {
            r = kl_assetcfg_parse_line(line, &e, &why);
        }
        if (r == 1 && kl_assetcfg_resolve(&e, data_dir, &why) != 0) r = -1;
        if (r == 1)
            for (int i = 0; i < n; i++)
                if (out[i].id == e.id || strcmp(out[i].file, e.file) == 0) {
                    r = -1; why = "id or file listed twice";
                    break;
                }
        if (r == 1 && n >= max) { r = -1; why = "list full"; }
        if (r < 0) {
            if (log) fprintf(log, "  [plat] %s line %d rejected: %s\n", cfg_path, no, why ? why : "malformed");
            continue;
        }
        if (r == 1) out[n++] = e;
    }
    fclose(f);
    return n;
}
