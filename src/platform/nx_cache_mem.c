/* nx_cache_mem.c -- the game's cache (/dev_hdd1) in memory, not on the SD card.
 *
 * The game copies the files it loads into its cache and reads the copies on later loads; the
 * runtime empties the cache at every start, as the PS3 does at the game's mount (Soulbrandt's
 * docs/101). On the PS3 the copies spared the disc drive its seeks. Here the disc's folder and the
 * cache are on the same SD card, and its writes are slow: to the title screen, 39 MB of copies
 * took 10.4 s of writing (4 MB/s), where the 103 MB read from the disc took 5.5 s (stage 11's
 * count, nx_game_io.c).
 *
 * So the cache is a drive of its own in memory, "hdd1:", in the C library's device table
 * (libsysbase's devoptab): the runtime's file layer (runtime/ppu/ppu_fs.cpp) opens, reads, lists
 * and removes its files through the C library as before, with PS3_HDD1_ROOT naming this drive.
 * Nothing is kept from one start to the next, which is what the PS3's mount promises the game.
 *
 * It holds files up to DESNX_CACHE_MEMORY_MB (settings.txt; 512 by default, 0 leaves the cache on
 * the SD card). A file created past that is kept on the SD card instead, in NX_HDD1_SPILL_DIR,
 * under its own name in this drive's folders: the game sees one cache either way.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>

#include "nx_cache_mem.h"
#include "nx_log.h"
#include "nx_paths.h"

#define DEFAULT_MB  512
#define NAME_CAP    256         /* a name's bytes and its end: readdir's d_name */
#define BLOCK       16384       /* st_blksize, the C library's buffer for each open file */
#define GROW_MIN    65536       /* a file written without its length set first grows by this or more */

typedef struct Node Node;
struct Node {
    Node *parent, *first, *next;    /* its folder, a folder's first entry, the next in its folder */
    char *name;
    bool dir;
    unsigned opened;                /* descriptors open on the file */
    time_t mtime;
    uint8_t *data;                  /* a file in memory: its bytes, */
    size_t size, held;              /* its length and the memory held for it */
    int spill;                      /* a file on the SD card: its number in NX_HDD1_SPILL_DIR, else -1 */
};

typedef struct {
    Node *node;
    off_t pos;
    int flags;
    int fd;                         /* a spilled file's own descriptor on the SD card, else -1 */
} OpenFile;

typedef struct {
    char *path;
    char **names;                   /* the folder's entries as it was opened */
    size_t count, at;
} DirState;

static Mutex s_lock;
static Node s_root = {.dir = true, .name = "", .spill = -1};
static size_t s_cap, s_held, s_peak;
static unsigned s_files, s_spilled, s_spills_made;
static bool s_on;

static int fail(struct _reent *r, int error)
{
    r->_errno = error;
    return -1;
}

static Node *child(const Node *dir, const char *name)
{
    for (Node *n = dir->first; n; n = n->next)
        if (strcmp(n->name, name) == 0)
            return n;
    return NULL;
}

/* path: "hdd1:/cache/11/name", or a part of it. *node is what it names, NULL when only its last
 * part is missing; *parent the folder that holds it or would (NULL for the root); leaf its last
 * part. */
static int resolve(const char *path, Node **node, Node **parent, char leaf[NAME_CAP])
{
    const char *colon = strchr(path, ':');
    if (colon)
        path = colon + 1;
    Node *at = &s_root, *up = NULL;
    leaf[0] = '\0';
    for (;;) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        const char *end = strchr(path, '/');
        const size_t n = end ? (size_t)(end - path) : strlen(path);
        if (!at)
            return ENOENT;
        if (!at->dir)
            return ENOTDIR;
        if (n >= NAME_CAP)
            return ENAMETOOLONG;
        if (n == 2 && path[0] == '.' && path[1] == '.')
            return EINVAL;  /* the runtime cleans its paths first (vfs_path.h) */
        const char *part = path;
        path += n;
        if (n == 1 && part[0] == '.')
            continue;
        memcpy(leaf, part, n);
        leaf[n] = '\0';
        up = at;
        at = child(at, leaf);
    }
    *node = at;
    *parent = up;
    return 0;
}

static void attach(Node *dir, Node *n)
{
    n->parent = dir;
    n->next = dir->first;
    dir->first = n;
}

static void detach(Node *n)
{
    Node **link = &n->parent->first;
    while (*link != n)
        link = &(*link)->next;
    *link = n->next;
    n->parent = n->next = NULL;
}

static Node *make(Node *dir, const char *name, bool is_dir)
{
    Node *n = calloc(1, sizeof *n);
    char *copy = strdup(name);
    if (!n || !copy) {
        free(n);
        free(copy);
        return NULL;
    }
    n->name = copy;
    n->dir = is_dir;
    n->spill = -1;
    n->mtime = time(NULL);
    attach(dir, n);
    return n;
}

static void spill_path(char *out, size_t size, int number)
{
    snprintf(out, size, NX_HDD1_SPILL_DIR "/%d.bin", number);
}

/* Frees a node taken out of its folder; a file must not be open. */
static void drop(Node *n)
{
    if (!n->dir) {
        if (n->spill >= 0) {
            char sd[128];
            spill_path(sd, sizeof sd, n->spill);
            unlink(sd);
            s_spilled--;
        } else {
            s_held -= n->held;
        }
        s_files--;
        free(n->data);
    }
    free(n->name);
    free(n);
}

/* Memory for `need` bytes of a file: just that for a length the game sets (it sets each copy's
 * length before writing it), and some to spare for a file that grows as it is written. */
static int hold(Node *f, size_t need, bool exact)
{
    if (need <= f->held)
        return 0;
    size_t want = exact ? need : need + need / 4;
    if (!exact && want < GROW_MIN)
        want = GROW_MIN;
    uint8_t *data = realloc(f->data, want);
    if (!data && want > need)
        data = realloc(f->data, want = need);
    if (!data)
        return ENOSPC;
    s_held += want - f->held;
    if (s_held > s_peak)
        s_peak = s_held;
    f->data = data;
    f->held = want;
    return 0;
}

static int set_size(Node *f, size_t size)
{
    if (size > f->size) {
        const int error = hold(f, size, true);
        if (error)
            return error;
        memset(f->data + f->size, 0, size - f->size);
    }
    f->size = size;
    f->mtime = time(NULL);
    return 0;
}

static bool writes(int flags)
{
    return (flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR;
}

static void fill(const Node *n, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_ino = (ino_t)(uintptr_t)n;
    st->st_mode = n->dir ? S_IFDIR | 0777 : S_IFREG | 0666;
    st->st_nlink = 1;
    st->st_size = n->dir ? 0 : (off_t)n->size;
    st->st_blksize = BLOCK;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_atime = st->st_mtime = st->st_ctime = n->mtime;
}

/* a spilled file's length is the SD card's */
static void spill_size(int spill, struct stat *st)
{
    char sd[128];
    struct stat on_sd;
    spill_path(sd, sizeof sd, spill);
    if (stat(sd, &on_sd) == 0) {
        st->st_size = on_sd.st_size;
        st->st_blocks = on_sd.st_blocks;
    }
}

/* ---- the drive's calls (sys/iosupport.h) ---------------------------------------------------- */

static int mem_open(struct _reent *r, void *file, const char *path, int flags, int mode)
{
    (void)mode;
    OpenFile *h = file;
    h->node = NULL;
    h->pos = 0;
    h->flags = flags;
    h->fd = -1;
    char leaf[NAME_CAP];
    Node *node, *parent;
    bool made = false;
    mutexLock(&s_lock);
    int error = resolve(path, &node, &parent, leaf);
    if (!error && node && (flags & O_CREAT) && (flags & O_EXCL))
        error = EEXIST;
    else if (!error && node && node->dir)
        error = EISDIR;
    else if (!error && !node) {
        if (!(flags & O_CREAT) || !parent)
            error = ENOENT;
        else if (!(node = make(parent, leaf, false)))
            error = ENOMEM;
        else {
            made = true;
            s_files++;
            if (s_held >= s_cap) {
                node->spill = (int)s_spills_made++;
                s_spilled++;
            }
        }
    }
    if (!error && node->spill < 0 && writes(flags) && (flags & O_TRUNC))
        set_size(node, 0);
    if (!error)
        node->opened++;     /* from here no unlink takes it */
    mutexUnlock(&s_lock);
    if (error)
        return fail(r, error);

    if (node->spill >= 0) {
        char sd[128];
        spill_path(sd, sizeof sd, node->spill);
        if (made)
            mkdir(NX_HDD1_SPILL_DIR, 0777);
        const int cut = made || (writes(flags) && (flags & O_TRUNC));
        h->fd = open(sd, O_RDWR | (made ? O_CREAT : 0) | (cut ? O_TRUNC : 0), 0666);
        if (h->fd < 0) {
            error = errno ? errno : EIO;
            mutexLock(&s_lock);
            node->opened--;
            if (made) {
                detach(node);
                drop(node);
            }
            mutexUnlock(&s_lock);
            return fail(r, error);
        }
    }
    h->node = node;
    return 0;
}

static int mem_close(struct _reent *r, void *file)
{
    (void)r;
    OpenFile *h = file;
    if (h->fd >= 0)
        close(h->fd);
    mutexLock(&s_lock);
    if (h->node)
        h->node->opened--;
    mutexUnlock(&s_lock);
    h->node = NULL;
    h->fd = -1;
    return 0;
}

static ssize_t mem_write(struct _reent *r, void *file, const char *ptr, size_t len)
{
    OpenFile *h = file;
    if (!writes(h->flags))
        return fail(r, EBADF);
    if (!len)
        return 0;
    if (h->fd >= 0) {
        const off_t at = (h->flags & O_APPEND) ? lseek(h->fd, 0, SEEK_END) : lseek(h->fd, h->pos, SEEK_SET);
        const ssize_t n = at < 0 ? -1 : write(h->fd, ptr, len);
        if (n < 0)
            return fail(r, errno);
        h->pos = at + n;
        return n;
    }
    Node *f = h->node;
    mutexLock(&s_lock);
    const size_t pos = (h->flags & O_APPEND) ? f->size : (size_t)h->pos;
    const size_t end = pos + len;
    int error = 0;
    if (end > f->size) {
        error = hold(f, end, false);
        if (!error && pos > f->size)
            memset(f->data + f->size, 0, pos - f->size);
    }
    if (!error) {
        memcpy(f->data + pos, ptr, len);
        if (end > f->size)
            f->size = end;
    }
    mutexUnlock(&s_lock);
    if (error)
        return fail(r, error);
    h->pos = (off_t)end;
    return (ssize_t)len;
}

static ssize_t mem_read(struct _reent *r, void *file, char *ptr, size_t len)
{
    OpenFile *h = file;
    if ((h->flags & O_ACCMODE) == O_WRONLY)
        return fail(r, EBADF);
    if (h->fd >= 0) {
        const ssize_t n = lseek(h->fd, h->pos, SEEK_SET) < 0 ? -1 : read(h->fd, ptr, len);
        if (n < 0)
            return fail(r, errno);
        h->pos += n;
        return n;
    }
    const Node *f = h->node;
    size_t n = 0;
    mutexLock(&s_lock);
    if ((size_t)h->pos < f->size) {
        n = f->size - (size_t)h->pos;
        if (n > len)
            n = len;
        memcpy(ptr, f->data + h->pos, n);
    }
    mutexUnlock(&s_lock);
    h->pos += (off_t)n;
    return (ssize_t)n;
}

static off_t mem_seek(struct _reent *r, void *file, off_t pos, int dir)
{
    OpenFile *h = file;
    off_t base;
    if (dir == SEEK_SET)
        base = 0;
    else if (dir == SEEK_CUR)
        base = h->pos;
    else if (dir == SEEK_END && h->fd >= 0) {
        struct stat st;
        if (fstat(h->fd, &st) != 0)
            return fail(r, errno);
        base = st.st_size;
    } else if (dir == SEEK_END) {
        mutexLock(&s_lock);
        base = (off_t)h->node->size;
        mutexUnlock(&s_lock);
    } else
        return fail(r, EINVAL);
    if (base + pos < 0)
        return fail(r, EINVAL);
    return h->pos = base + pos;
}

static int mem_fstat(struct _reent *r, void *file, struct stat *st)
{
    (void)r;
    const OpenFile *h = file;
    mutexLock(&s_lock);
    fill(h->node, st);
    const int spill = h->node->spill;
    mutexUnlock(&s_lock);
    if (spill >= 0)
        spill_size(spill, st);
    return 0;
}

static int mem_stat(struct _reent *r, const char *path, struct stat *st)
{
    char leaf[NAME_CAP];
    Node *node, *parent;
    int spill = -1;
    mutexLock(&s_lock);
    int error = resolve(path, &node, &parent, leaf);
    if (!error && !node)
        error = ENOENT;
    if (!error) {
        fill(node, st);
        spill = node->spill;
    }
    mutexUnlock(&s_lock);
    if (error)
        return fail(r, error);
    if (spill >= 0)
        spill_size(spill, st);
    return 0;
}

/* want_dir: 1 for rmdir; 0 for unlink, which takes an empty folder too, since remove() may come
 * here for one */
static int remove_path(struct _reent *r, const char *path, bool want_dir)
{
    char leaf[NAME_CAP];
    Node *node, *parent;
    mutexLock(&s_lock);
    int error = resolve(path, &node, &parent, leaf);
    if (!error && !node)
        error = ENOENT;
    else if (!error && !parent)
        error = EBUSY;          /* the drive's root */
    else if (!error && want_dir && !node->dir)
        error = ENOTDIR;
    else if (!error && node->dir && node->first)
        error = ENOTEMPTY;
    else if (!error && node->opened)
        error = EBUSY;          /* as on the SD card: an open file stays */
    if (!error) {
        detach(node);
        drop(node);
    }
    mutexUnlock(&s_lock);
    return error ? fail(r, error) : 0;
}

static int mem_unlink(struct _reent *r, const char *path)
{
    return remove_path(r, path, false);
}

static int mem_rmdir(struct _reent *r, const char *path)
{
    return remove_path(r, path, true);
}

static int mem_mkdir(struct _reent *r, const char *path, int mode)
{
    (void)mode;
    char leaf[NAME_CAP];
    Node *node, *parent;
    mutexLock(&s_lock);
    int error = resolve(path, &node, &parent, leaf);
    if (!error && node)
        error = EEXIST;
    else if (!error && !make(parent, leaf, true))
        error = ENOMEM;
    mutexUnlock(&s_lock);
    return error ? fail(r, error) : 0;
}

/* dir is n, or in it */
static bool within(const Node *n, const Node *dir)
{
    for (; dir; dir = dir->parent)
        if (dir == n)
            return true;
    return false;
}

static int mem_rename(struct _reent *r, const char *from, const char *to)
{
    char from_leaf[NAME_CAP], to_leaf[NAME_CAP];
    Node *src, *src_parent, *dst, *dst_parent;
    char *name = NULL;
    mutexLock(&s_lock);
    int error = resolve(from, &src, &src_parent, from_leaf);
    if (!error && !src)
        error = ENOENT;
    if (!error)
        error = resolve(to, &dst, &dst_parent, to_leaf);
    if (!error && (!src_parent || !dst_parent))
        error = EBUSY;          /* the drive's root */
    else if (!error && within(src, dst_parent))
        error = EINVAL;         /* a folder into itself */
    else if (!error && dst && dst != src) {
        if (dst->dir != src->dir)
            error = dst->dir ? EISDIR : ENOTDIR;
        else if (dst->dir && dst->first)
            error = ENOTEMPTY;
        else if (dst->opened)
            error = EBUSY;
    }
    if (!error && dst != src && !(name = strdup(to_leaf)))
        error = ENOMEM;
    if (!error && dst != src) {
        if (dst) {
            detach(dst);
            drop(dst);
        }
        detach(src);
        free(src->name);
        src->name = name;
        attach(dst_parent, src);
    }
    mutexUnlock(&s_lock);
    return error ? fail(r, error) : 0;
}

static void forget(DirState *d)
{
    for (size_t i = 0; i < d->count; i++)
        free(d->names[i]);
    free(d->names);
    d->names = NULL;
    d->count = d->at = 0;
}

/* the folder's names now; the lock is held */
static int list(DirState *d)
{
    char leaf[NAME_CAP];
    Node *dir, *parent;
    int error = resolve(d->path, &dir, &parent, leaf);
    if (!error && !dir)
        error = ENOENT;
    else if (!error && !dir->dir)
        error = ENOTDIR;
    if (error)
        return error;
    size_t count = 0;
    for (const Node *n = dir->first; n; n = n->next)
        count++;
    d->names = calloc(count ? count : 1, sizeof *d->names);
    if (!d->names)
        return ENOMEM;
    for (const Node *n = dir->first; n; n = n->next) {
        if (!(d->names[d->count] = strdup(n->name))) {
            forget(d);
            return ENOMEM;
        }
        d->count++;
    }
    return 0;
}

static DIR_ITER *mem_diropen(struct _reent *r, DIR_ITER *it, const char *path)
{
    DirState *d = it->dirStruct;
    memset(d, 0, sizeof *d);
    if (!(d->path = strdup(path))) {
        r->_errno = ENOMEM;
        return NULL;
    }
    mutexLock(&s_lock);
    const int error = list(d);
    mutexUnlock(&s_lock);
    if (error) {
        free(d->path);
        d->path = NULL;
        r->_errno = error;
        return NULL;
    }
    return it;
}

static int mem_dirreset(struct _reent *r, DIR_ITER *it)
{
    DirState *d = it->dirStruct;
    forget(d);
    mutexLock(&s_lock);
    const int error = list(d);
    mutexUnlock(&s_lock);
    return error ? fail(r, error) : 0;
}

/* Each entry as it is now: one removed since the folder was opened is left out. */
static int mem_dirnext(struct _reent *r, DIR_ITER *it, char *filename, struct stat *st)
{
    DirState *d = it->dirStruct;
    char leaf[NAME_CAP];
    Node *dir, *parent;
    mutexLock(&s_lock);
    const bool there = resolve(d->path, &dir, &parent, leaf) == 0 && dir && dir->dir;
    while (there && d->at < d->count) {
        const char *name = d->names[d->at++];
        const Node *n = child(dir, name);
        if (!n)
            continue;
        fill(n, st);
        memcpy(filename, name, strlen(name) + 1);   /* every name is shorter than NAME_CAP */
        mutexUnlock(&s_lock);
        return 0;
    }
    mutexUnlock(&s_lock);
    return fail(r, ENOENT);     /* the end, as the SD card's drive says it */
}

static int mem_dirclose(struct _reent *r, DIR_ITER *it)
{
    (void)r;
    DirState *d = it->dirStruct;
    forget(d);
    free(d->path);
    d->path = NULL;
    return 0;
}

static int mem_statvfs(struct _reent *r, const char *path, struct statvfs *buf)
{
    (void)r;
    (void)path;
    memset(buf, 0, sizeof *buf);
    mutexLock(&s_lock);
    const size_t used = s_held < s_cap ? s_held : s_cap;
    buf->f_bsize = buf->f_frsize = 4096;
    buf->f_blocks = s_cap / 4096;
    buf->f_bfree = buf->f_bavail = (s_cap - used) / 4096;
    buf->f_files = s_files;
    buf->f_namemax = NAME_CAP - 1;
    mutexUnlock(&s_lock);
    return 0;
}

static int mem_ftruncate(struct _reent *r, void *file, off_t len)
{
    OpenFile *h = file;
    if (len < 0)
        return fail(r, EINVAL);
    if (!writes(h->flags))
        return fail(r, EBADF);
    if (h->fd >= 0)
        return ftruncate(h->fd, len) == 0 ? 0 : fail(r, errno);
    mutexLock(&s_lock);
    const int error = set_size(h->node, (size_t)len);
    mutexUnlock(&s_lock);
    return error ? fail(r, error) : 0;
}

static int mem_fsync(struct _reent *r, void *file)
{
    (void)r;
    (void)file;
    return 0;
}

static const devoptab_t s_device = {
    .name = "hdd1",             /* NX_HDD1_MEM_DIR */
    .structSize = sizeof(OpenFile),
    .open_r = mem_open,
    .close_r = mem_close,
    .write_r = mem_write,
    .read_r = mem_read,
    .seek_r = mem_seek,
    .fstat_r = mem_fstat,
    .stat_r = mem_stat,
    .unlink_r = mem_unlink,
    .rename_r = mem_rename,
    .mkdir_r = mem_mkdir,
    .dirStateSize = sizeof(DirState),
    .diropen_r = mem_diropen,
    .dirreset_r = mem_dirreset,
    .dirnext_r = mem_dirnext,
    .dirclose_r = mem_dirclose,
    .statvfs_r = mem_statvfs,
    .ftruncate_r = mem_ftruncate,
    .fsync_r = mem_fsync,
    .rmdir_r = mem_rmdir,
    .lstat_r = mem_stat,
};

/* What an earlier start spilled, if it got that far. */
static void clear_spills(void)
{
    DIR *d = opendir(NX_HDD1_SPILL_DIR);
    if (!d)
        return;
    char path[512];
    for (const struct dirent *e; (e = readdir(d));) {
        snprintf(path, sizeof path, NX_HDD1_SPILL_DIR "/%s", e->d_name);
        unlink(path);
    }
    closedir(d);
}

bool nx_cache_mem_init(void)
{
    const char *setting = getenv("DESNX_CACHE_MEMORY_MB");
    const long mb = setting && *setting ? strtol(setting, NULL, 10) : DEFAULT_MB;
    if (mb <= 0) {
        nx_log("cache: /dev_hdd1 on the SD card, " NX_HDD1_DIR " (DESNX_CACHE_MEMORY_MB=%s)\n", setting);
        return false;
    }
    s_cap = (size_t)mb << 20;
    s_root.mtime = time(NULL);
    /* the folder the game's mount gives it, empty (cellSysCacheMount) */
    if (!make(&s_root, "cache", true) || AddDevice(&s_device) < 0) {
        nx_log("cache: its drive in memory could not be set up; /dev_hdd1 stays on the SD card\n");
        return false;
    }
    setenv("PS3_HDD1_ROOT", NX_HDD1_MEM_DIR, 1);
    clear_spills();
    s_on = true;
    nx_log("cache: /dev_hdd1 in memory, up to %ld MB (DESNX_CACHE_MEMORY_MB); files past that on the SD card, "
           NX_HDD1_SPILL_DIR "\n", mb);
    return true;
}

void nx_cache_mem_report(void)
{
    static unsigned last_files, last_spilled;
    static size_t last_held;
    if (!s_on)
        return;
    mutexLock(&s_lock);
    const unsigned files = s_files, spilled = s_spilled;
    const size_t held = s_held, peak = s_peak;
    mutexUnlock(&s_lock);
    if (files == last_files && spilled == last_spilled && held == last_held)
        return;
    last_files = files;
    last_spilled = spilled;
    last_held = held;
    char past[96] = "";
    if (spilled)
        snprintf(past, sizeof past, "; %u of them on the SD card, past the memory it may use", spilled);
    nx_log("cache in memory: %u files, %zu MB of %zu MB (at most %zu MB so far)%s\n", files, held >> 20, s_cap >> 20,
           peak >> 20, past);
}
