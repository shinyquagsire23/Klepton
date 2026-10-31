// The installation's asset-file list (runtime/xr/kl_assetcfg.c). `make
// assetcfg`, in `make check`.
//
// Built from that one file and nothing else: what is checked is the parser and
// the disk test — which lines are entries, which are refused, and that an entry
// is installed only as a regular file of exactly its size inside the OBB
// directory. kl_ovrplat.c answers the platform from exactly these entries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../runtime/xr/kl_assetcfg.h"

static int ok = 1;
#define CHECK(c, ...) do { if (!(c)) { ok = 0; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int parse(const char *line, kl_assetcfg_entry *e) {
    const char *why = NULL;
    return kl_assetcfg_parse_line(line, e, &why);
}

static void write_file(const char *path, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    for (size_t i = 0; i < size; i++) fputc('x', f);
    fclose(f);
}

int main(void) {
    printf("=== Asset-file list: parse, refuse, installed only at the listed size ===\n");
    kl_assetcfg_entry e;

    // Entries.
    CHECK(parse("asset 1234567890123456 de.lang 412345678 de\n", &e) == 1 &&
          e.id == 1234567890123456ull && strcmp(e.file, "de.lang") == 0 &&
          e.size == 412345678ull && strcmp(e.tag, "de") == 0, "a full line");
    CHECK(parse("  asset\t7 fr.lang\t10\r\n", &e) == 1 && strcmp(e.tag, "fr") == 0 && e.id == 7,
          "blanks and tabs, CRLF, the tag from the file name");
    CHECK(parse("asset 9 Content/pt-BR.lang 5", &e) == 1 && strcmp(e.tag, "pt-BR") == 0,
          "a subdirectory; the tag from the base name");
    CHECK(parse("asset 18446744073709551615 x.lang 1", &e) == 1 && e.id == 18446744073709551615ull,
          "the largest id");
    CHECK(parse("asset 26412782438311256 de.lang 3256791074 de meta=1.0.40.356975.Quest", &e) == 1 &&
          strcmp(e.tag, "de") == 0 && strcmp(e.meta, "1.0.40.356975.Quest") == 0,
          "Deadpool VR's line: tag and the store's metadata (its build version)");
    CHECK(parse("asset 5 fr.lang 10 meta=1.0.40.356975.Quest", &e) == 1 &&
          strcmp(e.tag, "fr") == 0 && strcmp(e.meta, "1.0.40.356975.Quest") == 0,
          "meta= without a tag; the tag from the file name");
    CHECK(parse("asset 5 fr.lang 10 meta=v2 fr", &e) == 1 && strcmp(e.meta, "v2") == 0 &&
          strcmp(e.tag, "fr") == 0, "meta= before the tag");
    CHECK(parse("asset 5 fr.lang 10", &e) == 1 && e.meta[0] == 0, "no meta= is \"\"");
    CHECK(parse("", &e) == 0 && parse("\n", &e) == 0 && parse("   \t\n", &e) == 0 &&
          parse("# asset 1 de.lang 2", &e) == 0 && parse("   # x", &e) == 0,
          "blank and comment lines are no entry");

    // Malformed.
    static const char *const bad[] = {
        "assets 1 de.lang 2",                    // not the keyword
        "asset 1 de.lang",                       // too few fields
        "asset 1 de.lang 2 de extra",            // too many
        "asset 1 de.lang 2 de extra more",
        "asset 0 de.lang 2",                     // id 0
        "asset -1 de.lang 2",
        "asset 0x10 de.lang 2",
        "asset 12a de.lang 2",
        "asset 18446744073709551616 de.lang 2",  // past 64 bits
        "asset 123456789012345678901 de.lang 2",
        "asset 1 de.lang 0",                     // size 0
        "asset 1 de.lang -5",
        "asset 1 de.lang 1.5",
        "asset 1 ../de.lang 2",                  // traversal
        "asset 1 a/../../de.lang 2",
        "asset 1 sub/.. 2",
        "asset 1 ./de.lang 2",
        "asset 1 a//de.lang 2",
        "asset 1 sub/ 2",
        "asset 1 / 2",
        "asset 1 a\\..\\de.lang 2",               // backslash
        "asset 1 de.lang 2 d/e",                 // tag
        "asset 1 de.lang 2 ''",
        "asset 1 de.lang 2 abcdefghijklmnopqrstuvwxyz0123456",
        "asset 1 .lang 2",                       // no tag in the name
        "asset 1 de\x01.lang 2",
        "asset 1 de.lang 2 meta=",                // meta
        "asset 1 de.lang 2 meta=a;b",
        "asset 1 de.lang 2 meta=a meta=b",
        "asset 1 de.lang 2 de fr",
        "asset 1 de.lang 2 meta=0123456789012345678901234567890123456789012345678901234567890123",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(parse(bad[i], &e) == -1, "malformed line accepted: \"%s\"", bad[i]);
    char longline[1200];
    memset(longline, 'a', sizeof longline - 1);
    longline[sizeof longline - 1] = 0;
    memcpy(longline, "asset 1 ", 8);
    CHECK(parse(longline, &e) == -1, "an over-long line");
    const char *why = NULL;
    CHECK(kl_assetcfg_parse_line(NULL, &e, &why) == -1 && why, "no line, with a reason");

    // The disk: a scratch OBB directory, and a file outside it.
    char base[] = "/tmp/kl_assetcfg.XXXXXX";
    if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }
    char real_base[PATH_MAX];
    if (!realpath(base, real_base)) { perror("realpath"); return 2; }
    char obb[PATH_MAX], out[PATH_MAX], p[PATH_MAX + 64], q[PATH_MAX + 64];
    snprintf(obb, sizeof obb, "%s/obb", real_base);
    snprintf(out, sizeof out, "%s/outside", real_base);
    mkdir(obb, 0755);
    mkdir(out, 0755);
    snprintf(p, sizeof p, "%s/de.lang", obb);       write_file(p, 100);
    snprintf(p, sizeof p, "%s/fr.lang", obb);       write_file(p, 99);
    snprintf(p, sizeof p, "%s/secret.lang", out);   write_file(p, 7);
    snprintf(p, sizeof p, "%s/dir.lang", obb);      mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sub", obb);           mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sub/it.lang", obb);   write_file(p, 3);
    snprintf(p, sizeof p, "%s/secret.lang", out);
    snprintf(q, sizeof q, "%s/evil.lang", obb);     symlink(p, q);
    snprintf(q, sizeof q, "%s/esc", obb);           symlink(out, q);
    snprintf(p, sizeof p, "%s/de.lang", obb);
    snprintf(q, sizeof q, "%s/alias.lang", obb);    symlink(p, q);

    struct { const char *line; int rc; int installed; } disk[] = {
        { "asset 1 de.lang 100",        0, 1 },     // there, right size
        { "asset 2 fr.lang 100",        0, 0 },     // there, wrong size
        { "asset 3 es.lang 100",        0, 0 },     // not there
        { "asset 4 dir.lang 100",       0, 0 },     // a directory
        { "asset 5 sub/it.lang 3",      0, 1 },     // in a subdirectory
        { "asset 6 evil.lang 7",       -1, 0 },     // a link out of the directory
        { "asset 7 esc/secret.lang 7", -1, 0 },     // a directory link out of it
        { "asset 8 alias.lang 100",     0, 1 },     // a link that stays inside
    };
    for (size_t i = 0; i < sizeof disk / sizeof disk[0]; i++) {
        int pr = parse(disk[i].line, &e);
        int rc = pr == 1 ? kl_assetcfg_resolve(&e, obb, &why) : -2;
        CHECK(rc == disk[i].rc && e.installed == disk[i].installed,
              "\"%s\": resolve %d installed %d, want %d and %d",
              disk[i].line, rc, e.installed, disk[i].rc, disk[i].installed);
    }
    parse("asset 1 de.lang 100", &e);
    kl_assetcfg_resolve(&e, obb, NULL);
    snprintf(p, sizeof p, "%s/de.lang", obb);
    CHECK(e.installed && strcmp(e.path, p) == 0, "installed at its absolute path (%s)", e.path);
    parse("asset 8 alias.lang 100", &e);
    kl_assetcfg_resolve(&e, obb, NULL);
    snprintf(q, sizeof q, "%s/alias.lang", obb);
    CHECK(e.installed && strcmp(e.path, q) == 0, "a link inside keeps the name it is listed under (%s)", e.path);
    // The OBB directory as the guest spells it, through a symbolic link
    // (/tmp -> /private/tmp here, /var -> /private/var on the headset): the
    // path reported is in that spelling, not the resolved one, because the
    // game compares it with the path its own engine built.
    char obb_link[PATH_MAX + 8];
    snprintf(obb_link, sizeof obb_link, "%s/obb", base);
    parse("asset 1 de.lang 100", &e);
    snprintf(q, sizeof q, "%s/de.lang", obb_link);
    CHECK(kl_assetcfg_resolve(&e, obb_link, NULL) == 0 && e.installed && strcmp(e.path, q) == 0,
          "installed under the OBB directory's own spelling (%s, want %s)", e.path, q);
    parse("asset 3 es.lang 100", &e);
    kl_assetcfg_resolve(&e, obb, NULL);
    snprintf(q, sizeof q, "%s/es.lang", obb);
    CHECK(!e.installed && strcmp(e.path, q) == 0, "a missing file keeps its absolute path (%s)", e.path);

    // Absolute paths: inside the directory, or refused.
    snprintf(q, sizeof q, "asset 1 %s/de.lang 100", obb);
    CHECK(parse(q, &e) == 1 && kl_assetcfg_resolve(&e, obb, NULL) == 0 && e.installed,
          "an absolute path inside the OBB directory");
    snprintf(q, sizeof q, "asset 1 %s/secret.lang 7", out);
    CHECK(parse(q, &e) == 1 && kl_assetcfg_resolve(&e, obb, NULL) == -1, "an absolute path outside it");
    snprintf(q, sizeof q, "asset 1 %s 7", obb);
    CHECK(parse(q, &e) == 1 && kl_assetcfg_resolve(&e, obb, NULL) == -1, "the directory itself");
    snprintf(q, sizeof q, "asset 1 %sx/de.lang 100", obb);
    CHECK(parse(q, &e) == 1 && kl_assetcfg_resolve(&e, obb, NULL) == -1, "a sibling with the same prefix");
    parse("asset 1 de.lang 100", &e);
    CHECK(kl_assetcfg_resolve(&e, "relative/obb", NULL) == -1 &&
          kl_assetcfg_resolve(&e, NULL, NULL) == -1, "an OBB directory that is not absolute");
    snprintf(q, sizeof q, "%s/", obb);
    CHECK(kl_assetcfg_resolve(&e, q, NULL) == 0 && e.installed, "a trailing '/' on the OBB directory");

    // A whole list.
    char cfg[PATH_MAX + 32];
    snprintf(cfg, sizeof cfg, "%s/klepton-asset-files.txt", real_base);
    FILE *f = fopen(cfg, "w");
    if (!f) { perror(cfg); return 2; }
    fprintf(f, "# Deadpool VR\n\n"
               "asset 11 de.lang 100 de\n"
               "asset 12 fr.lang 100\n"           // wrong size: listed, not installed
               "asset 13 ../outside/secret.lang 7\n"
               "asset 11 es.lang 5\n"             // id twice
               "asset 14 de.lang 100\n"           // file twice
               "garbage\n"
               "asset 15 evil.lang 7\n");
    for (int i = 0; i < 2000; i++) fputc('a', f); // an over-long line
    fprintf(f, "\nasset 16 sub/it.lang 3\n");
    fclose(f);
    kl_assetcfg_entry list[KL_ASSETCFG_MAX];
    FILE *log = tmpfile();
    int n = kl_assetcfg_load(cfg, obb, list, KL_ASSETCFG_MAX, log);
    int rejected = 0;
    if (log) {
        rewind(log);
        char l[512];
        while (fgets(l, sizeof l, log)) rejected += strstr(l, "rejected") != NULL;
        fclose(log);
    }
    CHECK(n == 3 && list[0].id == 11 && list[0].installed &&
          list[1].id == 12 && !list[1].installed &&
          list[2].id == 16 && list[2].installed && strcmp(list[2].tag, "it") == 0,
          "load: %d entries, want de (installed), fr (not), it (installed)", n);
    CHECK(rejected == 6, "load: %d lines said as rejected, want 6", rejected);
    CHECK(kl_assetcfg_load(cfg, obb, list, 2, NULL) == 2, "load: no more than `max`");
    snprintf(q, sizeof q, "%s/none.txt", real_base);
    CHECK(kl_assetcfg_load(q, obb, list, KL_ASSETCFG_MAX, NULL) == -1, "load: no file");
    CHECK(kl_assetcfg_load(cfg, "obb", list, KL_ASSETCFG_MAX, NULL) == -1, "load: a relative OBB directory");

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", real_base);
    if (system(cmd) != 0) printf("(scratch directory %s left behind)\n", real_base);

    printf("%s: entries parsed, malformed lines and paths out of the OBB directory refused, "
           "installed only at the listed size\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
