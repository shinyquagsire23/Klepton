// Keep the pinned kuba--/zip parser/decompressor. Constrain only its extraction
// boundary: remote maps may not create links, escape their folder or replace
// an existing song. All entries are checked before writing into a private stage.
#define zip_stream_extract sc2_unrestricted_zip_stream_extract
#include "zip.c"
#undef zip_stream_extract
#include <limits.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define SC2_ZIP_MAX_BYTES (512ULL * 1024 * 1024)
#define SC2_ZIP_MAX_FILES 4096

static int sc2_zip_name(const char *name) {
  if (!name || !*name || *name == '/' || strlen(name) > 1024) return 0;
  const char *part = name; unsigned depth=0;
  for (const char *p = name;; ++p) {
    if (*p == '\\' || *p == ':' || (unsigned char)*p < 32 && *p) return 0;
    if (*p == '/' || !*p) {
      if (++depth>32) return 0;
      size_t n = (size_t)(p - part);
      if ((!n && *p) || (n == 1 && *part == '.') ||
          (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
      if (!*p) return n || p > name;
      part = p + 1;
    }
  }
}
static int sc2_zip_valid(mz_zip_archive *archive) {
  mz_uint n = mz_zip_reader_get_num_files(archive);
  if (!n || n > SC2_ZIP_MAX_FILES) return 0;
  mz_uint64 total = 0;
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat info;
    if (!mz_zip_reader_file_stat(archive, i, &info) ||
        mz_zip_reader_get_filename(archive, i, NULL, 0) > sizeof info.m_filename ||
        !sc2_zip_name(info.m_filename) || (info.m_bit_flag & 1) ||
        info.m_uncomp_size > SC2_ZIP_MAX_BYTES - total) return 0;
    total += info.m_uncomp_size;
    unsigned mode = info.m_external_attr >> 16;
    unsigned type = mode & 0170000;
    if (type && type != 0100000 && type != 0040000) return 0;
    if (info.m_comp_size == 0 ? info.m_uncomp_size != 0 :
        info.m_uncomp_size / info.m_comp_size > 1000) return 0;
    // Reject duplicate and case-colliding paths (the target FS may fold case).
    for (mz_uint j = 0; j < i; ++j) {
      mz_zip_archive_file_stat prev;
      if (!mz_zip_reader_file_stat(archive, j, &prev) ||
          !strcasecmp(prev.m_filename, info.m_filename)) return 0;
    }
  }
  return 1;
}
struct sc2_zip_sink { int fd; unsigned long long bytes, limit; };
static size_t sc2_zip_write(void *arg, unsigned long long offset, const void *data, size_t size) {
  struct sc2_zip_sink *sink = arg;
  if (offset != sink->bytes || size > sink->limit - sink->bytes) return 0;
  const char *p = data; size_t left = size;
  while (left) {
    ssize_t n = write(sink->fd, p, left);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return 0;
    p += n; left -= (size_t)n;
  }
  sink->bytes += size; return size;
}
// The stage is new (0700) and we never extract symlinks. Walk it by fd, refusing
// links even if another app thread happens to modify it during extraction.
static int sc2_zip_entry(mz_zip_archive *archive, mz_uint i, int root) {
  mz_zip_archive_file_stat info;
  if (!mz_zip_reader_file_stat(archive, i, &info)) return -1;
  char path[1025]; strcpy(path, info.m_filename);
  size_t len = strlen(path); int directory = mz_zip_reader_is_file_a_directory(archive, i);
  if (len && path[len-1] == '/') path[len-1] = 0;
  int fd = dup(root); if (fd < 0) return -1;
  char *part = path, *slash;
  while ((slash = strchr(part, '/'))) {
    *slash = 0;
    if (mkdirat(fd, part, 0700) && errno != EEXIST) { close(fd); return -1; }
    int next = openat(fd, part, O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    close(fd); if (next < 0) return -1; fd = next; part = slash + 1;
  }
  int result = -1;
  if (directory) {
    if (!mkdirat(fd, part, 0700) || errno == EEXIST) {
      int sub = openat(fd, part, O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
      if (sub >= 0) { close(sub); result = 0; }
    }
  } else {
    int file = openat(fd, part, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW, 0600);
    if (file >= 0) {
      struct sc2_zip_sink sink = {file, 0, info.m_uncomp_size};
      int ok = mz_zip_reader_extract_to_callback(archive, i, sc2_zip_write, &sink, 0);
      int synced = fsync(file); int closed = close(file);
      if (ok && sink.bytes == info.m_uncomp_size && !synced && !closed) result = 0;
    }
  }
  close(fd); return result;
}
// Cleanup is limited to our freshly created stage; never follow a link.
static void sc2_zip_remove_contents(int fd) {
  int duplicate=dup(fd);if(duplicate<0)return;
  DIR *dir=fdopendir(duplicate);if(!dir){close(duplicate);return;}
  struct dirent *e;
  while ((e=readdir(dir))) {
    if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
    // Klepton's readdir bridge uses a thread-local entry. Keep this name across
    // recursion rather than retaining a pointer that the child walk replaces.
    char name[256];size_t len=strlen(e->d_name);if(len>=sizeof name)continue;
    memcpy(name,e->d_name,len+1);
    struct stat st;
    if(!fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode)) {
      int child=openat(fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
      if(child>=0){sc2_zip_remove_contents(child);close(child);unlinkat(fd,name,AT_REMOVEDIR);}
    } else unlinkat(fd,name,0);
  }
  closedir(dir);
}
static void sc2_zip_remove(const char *path) {
  int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return;
  sc2_zip_remove_contents(fd);close(fd);rmdir(path);
}

int zip_stream_extract(const char *stream, size_t size, const char *dir,
                       int (*on_extract)(const char *, void *), void *arg) {
  if (!stream || !dir || !*dir || !size || size > SC2_ZIP_MAX_BYTES) return ZIP_ENOINIT;
  mz_zip_archive archive; memset(&archive,0,sizeof archive);
  if (!mz_zip_reader_init_mem(&archive,stream,size,0)) return ZIP_ENOINIT;
  if (!sc2_zip_valid(&archive)) { mz_zip_reader_end(&archive); return ZIP_EINVENTNAME; }
  // Canonicalize only the parent, leaving the final component untrusted until
  // lstat/mkdir. Never overwrite existing songs, even an empty directory.
  char parent[PATH_MAX], target[PATH_MAX], stage[PATH_MAX];
  if (strlen(dir) >= sizeof parent) { mz_zip_reader_end(&archive); return ZIP_EINVENTNAME; }
  strcpy(parent,dir); char *last = strrchr(parent,'/');
  const char *leaf = last ? last+1 : dir;
  if (!*leaf || !strcmp(leaf,".") || !strcmp(leaf,"..")) { mz_zip_reader_end(&archive); return ZIP_EINVENTNAME; }
  char name[PATH_MAX]; strcpy(name,leaf);
  if (last) { if (last == parent) last[1]=0; else *last=0; } else strcpy(parent,".");
  char *canonical = realpath(parent,NULL);
  if (!canonical) { mz_zip_reader_end(&archive); return ZIP_ENOFILE; }
  int n = snprintf(target,sizeof target,"%s/%s",canonical,name);
  int m = snprintf(stage,sizeof stage,"%s/.sc2-download-XXXXXX",canonical); free(canonical);
  if (n < 0 || n >= (int)sizeof target || m < 0 || m >= (int)sizeof stage || !mkdtemp(stage)) {
    mz_zip_reader_end(&archive); return ZIP_ENOFILE;
  }
  int root = open(stage,O_RDONLY|O_DIRECTORY|O_NOFOLLOW), result=0;
  if (root < 0) result=ZIP_ENOFILE;
  for (mz_uint i=0; !result && i<mz_zip_reader_get_num_files(&archive); ++i)
    if (sc2_zip_entry(&archive,i,root)) result=ZIP_ENOFILE;
  if (root>=0) close(root);
  mz_zip_reader_end(&archive);
  // Reserve destination with mkdir: unlike rename(), it atomically rejects any
  // existing directory or symlink. Files are complete before publication.
  if (!result) {
    if (mkdir(target,0700)) result=ZIP_ENOFILE;
    else if (rename(stage,target)) { rmdir(target); result=ZIP_ENOFILE; }
  }
  if (result) sc2_zip_remove(stage);
  if (!result && on_extract && on_extract(target,arg)<0) return ZIP_ENOFILE;
  return result;
}
