// The decrypted-metadata dump. See kl_metadump.h for why it exists.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include "kl_metadump.h"
#include "kl_env.h"
#include "kl_fault.h"

// IL2CPP's own. A target that does not protect its metadata answers to this one
// and nothing below about the on-disk magic ever matters.
#define KLMD_SANITY 0xFAB11BAFu

static long klmd_page_size(void) {
    static long ps;
    if (!ps) ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? ps : 4096;
}

// An IL2CPP global-metadata header is `sanity`, `version`, and then nothing but
// (offset, size) pairs — one per table. That shape is what makes a candidate
// checkable without knowing the version, and checkable is the whole difference
// between this and grepping for four bytes: the magic is four bytes and this
// process maps gigabytes, so a bare match is a coincidence waiting to be written
// to disk as a corrupt dump.
//
// The first pair's OFFSET is where the first table starts, i.e. the end of the
// header, so it says how many pairs there are. Every pair then has to be inside
// the blob and the whole thing has to be a plausible size. The total is the
// furthest any table reaches, which is also the file's length.
static int klmd_measure(const uint32_t *p, size_t avail, size_t *out_size,
                        uint32_t *out_version, int *out_tables, int require_version) {
    if (avail < 32) return 0;
    uint32_t version = p[1];
    // 16 is Unity 5.3's; 31 is 2023's. Anything outside that is not a version.
    // Not required of a SHAPE candidate: a header recovered from a protected
    // file need not have had its version field put back either.
    if (require_version && (version < 16 || version > 40)) return 0;

    uint32_t header_end = p[2];
    if (header_end < 16 || header_end > 4096 || (header_end % 8) != 0) return 0;
    int pairs = (int)((header_end - 8) / 8);
    if (pairs < 8 || pairs > 128) return 0;
    if (avail < header_end) return 0;

    uint64_t end = header_end;
    for (int i = 0; i < pairs; i++) {
        uint32_t off = p[2 + i * 2], size = p[3 + i * 2];
        // A table may legitimately be empty, and an empty one is allowed to
        // carry a nonsense offset — some writers leave it at 0. Only non-empty
        // tables constrain anything.
        if (!size) continue;
        if (off < header_end) return 0;               // overlaps the header
        if (size > (256u << 20) || off > (512u << 20)) return 0;
        if ((uint64_t)off + size > end) end = (uint64_t)off + size;
    }
    if (end < 4096 || end > (512u << 20)) return 0;
    if (out_size) *out_size = (size_t)end;
    if (out_version) *out_version = version;
    if (out_tables) *out_tables = pairs;
    return 1;
}

// Is the page holding `p` actually in core?
//
// `pages_resident` on the region says only that SOME page is, and a region can
// be tens of gigabytes with a handful of hot pages in it — so the region-level
// test is a filter and this is the answer. mincore is cached one page at a time
// because the scan walks forward: every word after the first in a page hits the
// cache, and a page that is not resident is left alone entirely.
static int klmd_resident(uintptr_t p) {
    static uintptr_t cached_page = (uintptr_t)-1;
    static int cached_answer;
    long ps = klmd_page_size();
    uintptr_t page = p & ~(uintptr_t)(ps - 1);
    if (page == cached_page) return cached_answer;
    char vec = 0;
    cached_page = page;
    // mincore fails for an address that is not mapped at all, which the region
    // walk should have excluded — treat it as "do not touch" either way.
    cached_answer = mincore((void *)page, (size_t)ps, &vec) == 0 && (vec & MINCORE_INCORE);
    return cached_answer;
}

// The word index at which the NEXT page begins, given a region base and a word
// index inside it. Returns one before it, because the caller's loop increments.
static size_t klmd_next_page_word(uintptr_t base, size_t i) {
    long ps = klmd_page_size();
    uintptr_t here = base + i * 4;
    uintptr_t next = (here & ~(uintptr_t)(ps - 1)) + (uintptr_t)ps;
    return (next - base) / 4 - 1;
}

// The guest's own metadata file, read once: its first word is the second magic
// this scan looks for, and its length is how big a blob carrying that magic must
// be. Both are needed because a PROTECTED header cannot be measured — the offset
// pairs are ciphertext — so the only statement left about the blob's extent is
// "the same length as the file it was loaded from".
static struct { uint32_t magic; size_t size; uint8_t *bytes; int tried; } g_disk;

static void klmd_load_disk(const char *path) {
    if (g_disk.tried) return;
    g_disk.tried = 1;
    if (!path || !*path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 64 || n > (512L << 20)) { fclose(f); return; }
    uint8_t *b = malloc((size_t)n);
    if (!b) { fclose(f); return; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(b); return; }
    memcpy(&g_disk.magic, b, 4);
    g_disk.size = (size_t)n;
    g_disk.bytes = b;
    if (g_disk.magic != KLMD_SANITY)
        fprintf(stderr, "  [metadump] %s is PROTECTED: it begins 0x%08x, not IL2CPP's "
                        "0x%08x — also looking for a %zu-byte blob carrying the "
                        "guest's own magic\n",
                path, g_disk.magic, KLMD_SANITY, g_disk.size);
}

// How much of a candidate differs from the file it was loaded from. This is the
// measurement the whole exercise turns on: a copy identical to disk is the
// ciphertext sitting where it was read, and a copy that differs is the plaintext
// this instrument exists to capture. Reporting "found a blob" without it would
// be a 35 MB file of ciphertext presented as the answer.
// Returns 1 if the candidate is worth writing — i.e. it is not simply the file
// again.
static int klmd_compare_disk(const uint8_t *mem, size_t n) {
    if (!g_disk.bytes || n != g_disk.size) return 1;
    size_t diff = 0, first = (size_t)-1;
    for (size_t i = 0; i < n; i++)
        if (mem[i] != g_disk.bytes[i]) {
            if (first == (size_t)-1) first = i;
            diff++;
        }
    if (!diff) {
        // NOT written. A file named "the decrypted metadata" that is a copy of
        // the encrypted one the caller already has is an instrument presenting
        // its own failure as its result — and it would be found later, by
        // someone feeding it to Il2CppDumper and being told the sanity value is
        // wrong, several steps from here.
        fprintf(stderr, "  [metadump]   ...and it is BYTE-IDENTICAL to the file — this "
                        "is the ciphertext as loaded, not a decrypted copy. NOT "
                        "written\n");
        return 0;
    }
    fprintf(stderr, "  [metadump]   ...and %zu of %zu bytes differ from the file "
                    "(%.1f%%), first at 0x%zx — something decrypted it in place\n",
            diff, n, 100.0 * (double)diff / (double)n, first);
    return 1;
}

// Write the first candidate and only the first: later ones are reported so the
// log says how many there were, but a file silently replaced by a second
// candidate would make "which one is this?" unanswerable after the run.
static void blob_write(const char *path, const uint8_t *mem, size_t n, size_t *wrote) {
    if (*wrote) return;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "  [metadump] cannot write %s\n", path);
        return;
    }
    size_t got = fwrite(mem, 1, n, f);
    fclose(f);
    if (got != n) {
        fprintf(stderr, "  [metadump] short write to %s (%zu of %zu)\n", path, got, n);
        return;
    }
    *wrote = n;
    // The addresses Il2CppDumper wants are file offsets into libil2cpp.so, so
    // the caller's own load-address report is the other half; say so here rather
    // than leave it to be discovered.
    fprintf(stderr, "  [metadump] wrote %zu bytes to %s — feed it to Il2CppDumper "
                    "with the guest's own libil2cpp.so (the file on disk, not this "
                    "mapping)\n", n, path);
}

size_t kl_metadump_run(const char *disk_metadata) {
    const char *path = kl_env_str("KL_DUMP_METADATA", NULL);
    if (!path || !*path) return 0;
    klmd_load_disk(kl_env_str("KL_DUMP_METADATA_SRC", disk_metadata));

    // Only regions this process can read AND has written to. The blob is
    // decrypted in memory, so it is either heap or an anonymous mapping; the
    // file-backed read-only mapping of a library is exactly what we do not want
    // to find. KL_DUMP_METADATA_RO=1 drops the write requirement for a guest
    // that turns out to decrypt into a read-only mapping.
    int want_write = !kl_env_on("KL_DUMP_METADATA_RO", 0);

    mach_port_t task = mach_task_self();
    mach_vm_address_t addr = 0;
    uint64_t scanned = 0, regions = 0, skipped_cold = 0;
    int found = 0;
    size_t wrote = 0;

    fprintf(stderr, "  [metadump] scanning this process for a decrypted IL2CPP "
                    "metadata blob (magic 0x%08x%s)...\n", KLMD_SANITY,
            g_disk.magic && g_disk.magic != KLMD_SANITY ? " and the guest's own" : "");

    // `depth` OUTSIDE the loop. mach_vm_region_recurse descends by being asked
    // again at the same address one level deeper, so a depth that is re-zeroed
    // each time round asks the identical question forever the moment it meets a
    // submap — an instrument that hangs the process it was added to measure.
    natural_t depth = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        // The FULL submap info, not the short form, because the field that
        // filters this scan down to something finite is in it: `pages_resident`.
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = mach_vm_region_recurse(task, &addr, &size, &depth,
                                                  (vm_region_recurse_info_t)&info, &cnt);
        if (kr != KERN_SUCCESS) break;
        if (info.is_submap) { depth++; continue; }

        mach_vm_address_t next = addr + size;
        int readable = (info.protection & VM_PROT_READ) != 0;
        int writable = (info.protection & VM_PROT_WRITE) != 0;
        int wanted = readable && (writable || !want_write) && size >= 4096;
        if (wanted && !info.pages_resident) {
            skipped_cold++;
        } else if (wanted) {
            regions++;
            const uint32_t *w = (const uint32_t *)(uintptr_t)addr;
            size_t words = (size_t)(size / 4);
            for (size_t i = 0; i + 8 < words; i++) {
                if (!klmd_resident((uintptr_t)(w + i))) {
                    // Skip to the next page rather than the next word. This is
                    // what keeps the scan finite AND keeps it from costing the
                    // guest its own memory: a word read out of a cold anonymous
                    // page faults the page in, so a naive walk of a reserved
                    // range does not merely take forever, it allocates it.
                    i = klmd_next_page_word(addr, i);
                    continue;
                }
                scanned += 4;
                int own = g_disk.magic && w[i] == g_disk.magic && g_disk.magic != KLMD_SANITY;
                if (w[i] != KLMD_SANITY && !own) {
                    // The magic-independent search, and the reason it exists:
                    // a loader that decrypts a protected file has no obligation
                    // to put IL2CPP's sanity value back — it was patched out of
                    // the check on the way in — so a plaintext header can be
                    // sitting in memory with four arbitrary bytes in front of
                    // it and the search above will never see it. What CANNOT be
                    // arbitrary is the shape: everything after the first two
                    // words is (offset, size), the first offset is the header's
                    // own length, and the tables have to reach exactly as far as
                    // the file they were loaded from. That last equality is what
                    // makes this near-impossible to hit by chance.
                    //
                    // **The tables must reach EXACTLY the length of the file
                    // they were loaded from.** That single equality is what
                    // makes this trustworthy, and the weaker test was tried and
                    // measured: accepting any shape whose version word reads as
                    // a plausible metadata version (16..40) produced **more than
                    // thirty** hits in one VRChat run — pair-structured data is
                    // common, so the structure alone proves nothing and the
                    // instrument would have written 116 MB of something else out
                    // as the answer. If a protector ever strips a wrapper and
                    // changes the length, this goes quiet and says so rather
                    // than guessing.
                    //
                    // Cheap gate first: the first table offset is the header's
                    // own length, so it is 8-aligned and small, which rejects
                    // almost every word in the process for two compares.
                    if (!g_disk.size) continue;
                    uint32_t he = w[i + 2];
                    if ((he & 7) || he < 16 || he > 4096) continue;
                    size_t sh_blob = 0; uint32_t sh_ver = 0; int sh_tables = 0;
                    if (!klmd_measure(w + i, (words - i) * 4, &sh_blob, &sh_ver,
                                      &sh_tables, 0)) continue;
                    if (sh_blob != g_disk.size) continue;
                    found++;
                    fprintf(stderr, "  [metadump]   a header SHAPE at %p with no known "
                                    "magic (first word 0x%08x, version word %u): %d "
                                    "tables reaching exactly %zu bytes, the file's own "
                                    "length — this is a decrypted header\n",
                            (void *)(w + i), w[i], sh_ver, sh_tables, sh_blob);
                    blob_write(path, (const uint8_t *)(w + i), sh_blob, &wrote);
                    if (wrote == sh_blob) i += sh_blob / 4 - 1;
                    continue;
                }

                size_t avail = (words - i) * 4;
                size_t blob = 0; uint32_t version = 0; int tables = 0;
                if (own) {
                    // A protected header cannot be measured, so the file's own
                    // length is the claim — and klmd_compare_disk is what turns
                    // that claim into evidence.
                    blob = g_disk.size;
                    if (blob > avail) continue;
                    found++;
                    fprintf(stderr, "  [metadump]   the guest's own magic 0x%08x at %p, "
                                    "%zu bytes (the file's length)\n",
                            g_disk.magic, (void *)(w + i), blob);
                    if (!klmd_compare_disk((const uint8_t *)(w + i), blob)) {
                        i += blob / 4 - 1;      // past it; the magic recurs inside
                        continue;
                    }
                } else if (!klmd_measure(w + i, avail, &blob, &version, &tables, 1)) {
                    // Named, because a rejected candidate is the difference
                    // between "the metadata is still encrypted in memory" and
                    // "it is there and this validator is too strict".
                    fprintf(stderr, "  [metadump]   magic at %p, but the header does "
                                    "not measure (version %u, first table at %u) — "
                                    "not a metadata blob\n",
                            (void *)(w + i), w[i + 1], w[i + 2]);
                    continue;
                } else if (blob > avail) {
                    fprintf(stderr, "  [metadump]   magic at %p measures %zu bytes but "
                                    "only %zu are in this region — skipped\n",
                            (void *)(w + i), blob, avail);
                    continue;
                } else {
                    found++;
                    fprintf(stderr, "  [metadump]   metadata at %p: version %u, %d "
                                    "tables, %zu bytes (%.1f MB)\n",
                            (void *)(w + i), version, tables, blob,
                            (double)blob / (1024 * 1024));
                }

                size_t before = wrote;
                blob_write(path, (const uint8_t *)(w + i), blob, &wrote);
                // Past the blob, not one word into it: the tables are full of
                // arbitrary integers and either magic can appear inside them.
                if (wrote && !before) i += blob / 4 - 1;
            }
        }
        addr = next;
        if (!addr) break;
    }

    if (!found)
        fprintf(stderr, "  [metadump] no metadata blob in %llu region(s), %.1f MB of "
                        "%sRESIDENT memory (%llu region(s) entirely cold, untouched) "
                        "— either init has not decrypted it yet, or it is never "
                        "decrypted whole (try KL_DUMP_METADATA_RO=1)\n",
                (unsigned long long)regions, (double)scanned / (1024 * 1024),
                want_write ? "writable " : "",
                (unsigned long long)skipped_cold);
    else
        fprintf(stderr, "  [metadump] %d candidate(s) over %.1f MB of %sresident "
                        "memory\n", found, (double)scanned / (1024 * 1024),
                want_write ? "writable " : "");
    return wrote;
}

// ---------------------------------------------------------------------------
// KL_META_WATCH=1 — WHO reads the metadata, found by faulting on the mapping
// ---------------------------------------------------------------------------
//
// The dump above answers "is there a plaintext image?" and for VRChat the
// answer is no: the file is read once and never decrypted whole, so the only
// way to the algorithm is the code that does the decrypting. That code cannot
// be found statically in this target — `libil2cpp.so`'s runtime `.text` makes
// ZERO direct PLT calls (every import is reached indirectly) and the exported
// il2cpp_* entry points are trampolines through a table filled at load, so
// neither a string xref nor a call-graph walk from `open`/`mmap` has anything
// to walk from.
//
// It can be found dynamically, because the mapping is OURS. mmap the file for
// the guest, take the pages away with `mprotect(PROT_NONE)`, and the first
// instruction that touches metadata announces itself as a fault whose pc is
// the reader. The handler opens the region, notes the site, and a watchdog
// thread closes it again a moment later, so one run collects a census of
// reader sites rather than a single one.
//
// A site is `<image>+0x<off>` and a metadata offset, which is what makes the
// static pass possible afterwards: the offset says WHICH table the site was
// reading, so a site that faults at the header is the header parse and a site
// that faults inside a table is that table's accessor.
//
// Diagnostic only, and it costs a fault per site per re-arm — never leave it on.

#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>
#include "klepton.h"

#define KLMD_SITES 512

static struct {
    int      armed;              // the watch is on (KL_META_WATCH)
    int      fd;                 // the fd global-metadata.dat was opened on
    uint8_t *base;               // ...and where it got mapped
    size_t   len;
    int      open;               // pages are readable right now
    unsigned rearm_ms;
    unsigned faults;
    unsigned reads;
    size_t   read_bytes;
    unsigned sites_n;
    unsigned ranges_n;
    struct { void *pc; size_t off; unsigned hits; } sites[KLMD_SITES];
    struct { size_t off, len; void *dst; } ranges[KLMD_SITES];
    struct sigaction prev_segv, prev_bus;
} g_watch;

static void klmd_note_site(void *pc, size_t off) {
    for (unsigned i = 0; i < g_watch.sites_n; i++)
        if (g_watch.sites[i].pc == pc) { g_watch.sites[i].hits++; return; }
    if (g_watch.sites_n >= KLMD_SITES) return;
    unsigned i = g_watch.sites_n++;
    g_watch.sites[i].pc = pc;
    g_watch.sites[i].off = off;
    g_watch.sites[i].hits = 1;

    // Printed as it happens rather than only in the report: a site found on the
    // way to a crash is the interesting one, and a report at exit is exactly
    // what a crash loses.
    size_t imgoff = 0;
    const char *img = kl_addr_image(pc, &imgoff);
    char buf[160];
    int n = snprintf(buf, sizeof buf, "  [metawatch] read at metadata+0x%zx from %s+0x%zx\n",
                     off, img ? img : "<host>", imgoff);
    if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
}

static void klmd_fault(int sig, siginfo_t *si, void *uctx) {
    uint8_t *a = si ? (uint8_t *)si->si_addr : NULL;
    if (g_watch.armed && a && a >= g_watch.base && a < g_watch.base + g_watch.len) {
        ucontext_t *uc = uctx;
        void *pc = uc ? (void *)uc->uc_mcontext->__ss.__pc : NULL;
        g_watch.faults++;
        klmd_note_site(pc, (size_t)(a - g_watch.base));
        // Open the whole mapping and let the instruction retry. Opening only
        // the faulting page would be a finer instrument and a much slower one;
        // the watchdog re-closes it, so the granularity is time, not pages.
        mprotect(g_watch.base, g_watch.len, PROT_READ);
        g_watch.open = 1;
        return;
    }
    // Not ours. The fault reporter installed these signals first, and a real
    // crash has to keep reaching it.
    struct sigaction *p = (sig == SIGBUS) ? &g_watch.prev_bus : &g_watch.prev_segv;
    if (p->sa_flags & SA_SIGINFO) { if (p->sa_sigaction) p->sa_sigaction(sig, si, uctx); return; }
    if (p->sa_handler && p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN) {
        p->sa_handler(sig); return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void klmd_arm(void *p, size_t n);

// The call chain, without frame pointers.
//
// kl_fault_print_frames walks x29, and in this guest that walk stops at the
// first frame: the runtime `.text` of libil2cpp is obfuscated and does not keep
// a frame chain, so the one thing the whole investigation needs — WHO asked for
// the metadata — is exactly what the ordinary instrument cannot say.
//
// A conservative scan says it anyway. Every saved return address is a word on
// the stack pointing into some image's text, so printing all of them prints the
// chain with some junk mixed in; a wrong answer here is a stale word, not a
// misleading claim, and the addresses are checkable by disassembling them.
static void klmd_stack_xray(const char *what, void *sp, size_t words) {
    fprintf(stderr, "  [metawatch] stack x-ray at %s (conservative — some of these "
                    "are stale words):\n", what);
    uintptr_t *p = (uintptr_t *)(((uintptr_t)sp) & ~(uintptr_t)7);
    unsigned shown = 0;
    for (size_t i = 0; i < words && shown < 40; i++) {
        uintptr_t v = p[i];
        if (v < 0x1000) continue;
        size_t off = 0;
        const char *img = kl_addr_image((void *)v, &off);
        if (!img) continue;
        fprintf(stderr, "      [sp+0x%04zx] %s+0x%zx\n", i * 8, img, off);
        shown++;
    }
    fflush(stderr);
}

// Where the guest ASSEMBLED the file. This guest does not map its metadata and
// does not read it into one buffer either — libunity streams it through a 7 KB
// staging buffer — so the thing to watch is whatever holds the finished copy,
// and the only description we have of it is that it begins with the same bytes
// the file does. Which is exactly the property kl_metadump_run reports as
// "BYTE-IDENTICAL to the file, NOT written": the useless answer to the dump
// question is the address the watch needs.
//
// Matched on 4 KB rather than on the magic: the magic is four bytes and this
// process maps gigabytes.
static uint8_t *klmd_find_loaded(size_t *out_len) {
    if (!g_disk.bytes || g_disk.size < 4096) return NULL;
    mach_port_t task = mach_task_self();
    mach_vm_address_t addr = 0;
    natural_t depth = 0;                        // OUTSIDE the loop — see above
    for (;;) {
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(task, &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        if (info.is_submap) { depth++; continue; }
        mach_vm_address_t next = addr + size;
        if ((info.protection & VM_PROT_READ) && info.pages_resident &&
            size >= g_disk.size) {
            const uint8_t *b = (const uint8_t *)(uintptr_t)addr;
            for (size_t i = 0; i + g_disk.size <= (size_t)size; i += 4096) {
                if (!klmd_resident((uintptr_t)(b + i))) continue;
                if (memcmp(b + i, g_disk.bytes, 4096) != 0) continue;
                if (out_len) *out_len = g_disk.size;
                return (uint8_t *)(uintptr_t)(b + i);
            }
        }
        addr = next;
        if (!addr) break;
    }
    return NULL;
}

// The two file events the watch needs: which fd carries the metadata, and — if
// this guest maps it rather than reading it — where it landed.
static void klmd_watch_file(const char *op, const char *path, int fd,
                            void *addr, size_t len, long long off, void *caller) {
    if (!g_watch.armed) return;
    if (strcmp(op, "open") == 0) {
        if (path && strstr(path, "global-metadata.dat")) {
            g_watch.fd = fd;
            // The path the GUEST opened, which is the only statement anywhere
            // about which file this run is actually reading — the staged copy,
            // not the tree's.
            klmd_load_disk(path);
            fprintf(stderr, "  [metawatch] global-metadata.dat is fd %d, %zu bytes on disk\n",
                    fd, g_disk.size);
        }
        return;
    }
    if (strcmp(op, "mmap") != 0) return;
    // Every file-backed mapping, named by asking the FD rather than by matching
    // the fd the open trace recorded: a guest that maps its metadata through a
    // door we do not trace (openat, open64, a dup) would be invisible to the
    // match and is not invisible to F_GETPATH.
    char fpath[1024] = "";
    if (fcntl(fd, F_GETPATH, fpath) != 0) fpath[0] = 0;
    size_t imgoff = 0;
    const char *img = kl_addr_image(caller, &imgoff);
    fprintf(stderr, "  [metawatch] mmap(fd %d \"%s\", %zu bytes, off %lld) -> %p, from %s+0x%zx\n",
            fd, fpath, len, off, addr, img ? img : "<host>", imgoff);
    if (strstr(fpath, "global-metadata.dat")) {
        int here = 0;
        klmd_stack_xray("the metadata mmap", &here, 512);
        klmd_arm(addr, len);
    }
}

static void *klmd_watchdog(void *unused) {
    (void)unused;
    for (;;) {
        usleep(g_watch.rearm_ms * 1000);
        if (!g_watch.armed || !g_watch.base) continue;
        if (g_watch.open) {
            g_watch.open = 0;
            mprotect(g_watch.base, g_watch.len, PROT_NONE);
        }
    }
    return NULL;
}

// Take the pages away and let the readers announce themselves. `p`/`n` are the
// raw extent; it is trimmed to whole pages, because the ends of a malloc'd
// buffer share their page with whatever the allocator put next to them and
// taking those away is a fault storm with nothing to do with the metadata.
static void klmd_arm(void *p, size_t n) {
    if (g_watch.base) return;                    // the first region wins
    long ps = klmd_page_size();
    uintptr_t lo = ((uintptr_t)p + (uintptr_t)ps - 1) & ~((uintptr_t)ps - 1);
    uintptr_t hi = ((uintptr_t)p + n) & ~((uintptr_t)ps - 1);
    if (hi <= lo) return;
    g_watch.base = (uint8_t *)lo;
    g_watch.len  = (size_t)(hi - lo);

    struct sigaction sa, prev;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = klmd_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &prev); g_watch.prev_segv = prev;
    sigaction(SIGBUS,  &sa, &prev); g_watch.prev_bus  = prev;

    pthread_t t;
    pthread_create(&t, NULL, klmd_watchdog, NULL);
    pthread_detach(t);

    mprotect(g_watch.base, g_watch.len, PROT_NONE);
    g_watch.open = 0;
    fprintf(stderr, "  [metawatch] armed over %p..%p, re-arming every %u ms\n",
            (void *)g_watch.base, (void *)(g_watch.base + g_watch.len), g_watch.rearm_ms);
}

// The guest does not map this file, it READS it — so the buffer the bytes land
// in is the thing to watch, and `read` is the only call that says where that is.
// It is a plain forward for every other fd, which is all of them.
static ssize_t klmd_read(int fd, void *buf, size_t n) {
    // The file offset BEFORE the read, which is the only thing that says which
    // part of the metadata a chunk is: this guest seeks around the file and
    // takes 154 KB of 35 MB, so "23 reads" on its own says nothing about what
    // was read.
    off_t at = (g_watch.armed && fd == g_watch.fd && g_watch.fd)
                   ? lseek(fd, 0, SEEK_CUR) : -1;
    ssize_t r = read(fd, buf, n);
    if (at >= 0 && r > 0) {
        if (g_watch.ranges_n < KLMD_SITES) {
            unsigned i = g_watch.ranges_n++;
            g_watch.ranges[i].off = (size_t)at;
            g_watch.ranges[i].len = (size_t)r;
            g_watch.ranges[i].dst = buf;
        }
    }
    if (!g_watch.armed || fd != g_watch.fd || !g_watch.fd || r <= 0) return r;

    size_t imgoff = 0;
    const char *img = kl_addr_image(__builtin_return_address(0), &imgoff);
    g_watch.reads++;
    g_watch.read_bytes += (size_t)r;
    // Only the first few, and only their frames: the interesting caller is not
    // the one that issued the read (this guest streams the file through Unity's
    // own file layer) but whoever above it is assembling and decrypting it.
    if (g_watch.reads <= (unsigned)kl_env_int("KL_META_WATCH_FRAMES", 3)) {
        fprintf(stderr, "  [metawatch] read(fd %d, %zu) -> %zd into %p, from %s+0x%zx\n",
                fd, n, r, buf, img ? img : "<host>", imgoff);
        kl_fault_print_frames(stderr, NULL);
    }
    // Whole-file in one call is the shape worth arming on. A chunked read into a
    // rolling window would be a different instrument, and saying so beats
    // arming over a buffer that is about to be reused for the next chunk.
    if ((size_t)r >= (16u << 20)) { klmd_arm(buf, (size_t)r); return r; }

    // Streamed in chunks. The buffer each chunk lands in is a staging buffer
    // that is about to be reused, so there is nothing to arm on until the whole
    // file has gone past — then the assembled copy is somewhere in this process
    // and can be found by its first 4 KB.
    if (!g_watch.base && g_disk.size && g_watch.read_bytes >= g_disk.size) {
        size_t len = 0;
        uint8_t *p = klmd_find_loaded(&len);
        if (p) {
            fprintf(stderr, "  [metawatch] the assembled file is at %p (%zu bytes), after "
                            "%u read(s) totalling %zu bytes\n",
                    (void *)p, len, g_watch.reads, g_watch.read_bytes);
            klmd_arm(p, len);
        } else {
            fprintf(stderr, "  [metawatch] the whole file has been read (%zu bytes) and no "
                            "copy of it is anywhere in this process — it is consumed as it "
                            "arrives, not assembled\n", g_watch.read_bytes);
            g_disk.size = 0;                     // ask once
        }
    }
    return r;
}

static void *klmd_shim_override(const char *name) {
    if (g_watch.armed && strcmp(name, "read") == 0) return (void *)klmd_read;
    return NULL;
}

void kl_metadump_watch_install(void) {
    if (!kl_env_int("KL_META_WATCH", 0)) return;
    g_watch.armed = 1;
    g_watch.rearm_ms = (unsigned)kl_env_int("KL_META_WATCH_MS", 50);
    if (!g_watch.rearm_ms) g_watch.rearm_ms = 1;
    kl_file_watch = klmd_watch_file;
    kl_shim_override = klmd_shim_override;
    fprintf(stderr, "  [metawatch] on — the metadata buffer will be taken away and "
                    "every reader named\n");
}

void kl_metadump_watch_report(FILE *out) {
    if (!g_watch.armed) return;
    fprintf(out, "\n=== metadata readers (KL_META_WATCH) ===\n");
    fprintf(out, "  %u read(s) of fd %d, %zu of %zu bytes\n",
            g_watch.reads, g_watch.fd, g_watch.read_bytes, g_disk.size);
    for (unsigned i = 0; i < g_watch.ranges_n; i++)
        fprintf(out, "    file 0x%08zx .. 0x%08zx (%zu) -> %p\n",
                g_watch.ranges[i].off, g_watch.ranges[i].off + g_watch.ranges[i].len,
                g_watch.ranges[i].len, g_watch.ranges[i].dst);
    if (!g_watch.base) {
        fprintf(out, "  never armed — no buffer to watch\n");
        return;
    }
    fprintf(out, "  %u fault(s), %u distinct site(s)\n", g_watch.faults, g_watch.sites_n);
    for (unsigned i = 0; i < g_watch.sites_n; i++) {
        size_t off = 0;
        const char *img = kl_addr_image(g_watch.sites[i].pc, &off);
        fprintf(out, "  %-28s+0x%-10zx first read at metadata+0x%-10zx %u hit(s)\n",
                img ? img : "<host>", off, g_watch.sites[i].off, g_watch.sites[i].hits);
    }
    fflush(out);
}
