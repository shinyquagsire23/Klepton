#include <stdio.h>
// UE4 OBB read-through VFS. See kl_obbmap.c.
#ifndef KL_OBBMAP_H
#define KL_OBBMAP_H
#include <sys/types.h>

// Index every *.obb under obbdir, recording the loose path each entry occupies
// below ue4game_base ("<files>/UE4Game/<Project>"). UE4-only; call once from
// kl_ue4_configure. Also mkdir -p's the directories the entries live in, so the
// guest's directory probes succeed while the files stay virtual.
void kl_obbmap_init(const char *obbdir, const char *ue4game_base);

// access()/stat(): 1 (+ size) if loose is an indexed entry, else 0.
int kl_obbmap_stat(const char *loose, long long *size);
// Where an indexed entry's bytes really are: the host file and the offset of
// its first byte in it. For a reader that is handed a path and a byte range
// and opens the file ITSELF, past the guest's open() — a media player given
// "(this pak, offset, length)" when the pak is an entry inside the OBB. 1 on
// success; 0 when the path is not an entry that is stored as plain bytes.
int kl_obbmap_locate(const char *loose, char *host, size_t cap, long long *base, long long *size);

// 1 if loose is UE4's Content/Internationalization directory (or under it) and
// the OBB map is live — the ICU data is in a mounted pak whose path-hash index
// cannot answer directory queries, so the loose probe must say "exists" or UE4
// never loads any locale and crashes on a null culture. See kl_obbmap.c.
int kl_obbmap_icu_dir(const char *loose);

// open(): fd serving the entry straight from the OBB, or -1 if not ours (the
// caller then does its normal open). Read-only, stored entries only.
int kl_obbmap_open(const char *loose, int flags);
// A FILE* over an OBB bank/entry, for guests that read via stdio (Wwise).
FILE *kl_obbmap_fopen(const char *loose, const char *mode);

// Per-fd operations. Each sets *handled=1 and returns the result when fd is one
// of ours, or *handled=0 (return value ignored) when it is a normal fd.
ssize_t kl_obbmap_pread(int fd, void *buf, size_t n, off_t off, int *handled);
ssize_t kl_obbmap_read (int fd, void *buf, size_t n, int *handled);
off_t   kl_obbmap_lseek(int fd, off_t off, int whence, int *handled);
int     kl_obbmap_fstat_size(int fd, long long *size, int *handled);
int     kl_obbmap_close(int fd, int *handled);
int     kl_obbmap_is_vfd(int fd);

// Directory synthesis: UE4 lists Content/Paks to find its paks, but the files
// are virtual. opendir returns a synthetic handle (or NULL if not ours); the
// caller routes readdir/closedir to these while is_dirhandle is true. readdir
// fills name_out/is_dir and returns 1 (more), 0 (end), or -1 (not ours).
void *kl_obbmap_opendir(const char *dirpath);
int   kl_obbmap_readdir(void *h, char *name_out, size_t cap, int *is_dir);
int   kl_obbmap_closedir(void *h);

#endif
