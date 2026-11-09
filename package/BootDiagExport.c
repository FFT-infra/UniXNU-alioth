// SPDX-License-Identifier: MIT
// Diagnostic file export (int-217): the unified log that logd persisted (/private/var/db/diagnostics, with the format strings in
// /private/var/db/uuidtext) and the crash reports, read by the kernel itself, so neither a sandbox nor a launchd job that never runs
// (adrenoprobed) stands in the way. BootLog.c asks for the stream once (sheng-diag-export=<seconds>) and writes it into the half of the
// export region the pipeline-cache blob does not hold; scripts/read-sheng-log.py unpacks it into the run's diag/ directory.
// Stream: "SHEXR001", u32 stream bytes, u32 0, then the cache export's stream (drivers/AdrenoMetal/CacheExport.h, "M2VCACH1") with root
// "/" and every path relative to it. The scan lists every file first and spends the budget in the order of worth (the dyld shared
// cache's format-string catalog, then the persisted log's newest file, then the rest), so neither the log nor its format strings are
// pushed out by what is expendable: the catalog composes the system frameworks' messages, and the other files keep changing only with
// the binaries the host already has.
// screen.bgrx is the panel's scanout at half resolution (16-byte header "SHOT", u32 width, height, stride, then BGRX rows), so the
// run shows what was on the screen. Files that do not fit the budget are named in diag-skipped.txt, the stream's last file.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/mount_internal.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

enum {DiagMaxFiles = 6144, DiagPath = 192, DiagDepth = 6, DiagChunk = 256 * 1024, DiagNote = 32768};
struct diag_file {char path[DiagPath]; uint64_t size; uint32_t mode, uid, gid, root, keep;};
static struct diag_file *diag_files;
static uint32_t diag_count, diag_skipped_count;
static char *diag_note;   // diag-skipped.txt
static size_t diag_note_used;
volatile uint32_t boot_diag_files, boot_diag_skipped, boot_diag_errors;

// In the order of their worth: the format strings of earlier runs are on the host already, and the crash reports are rarely needed.
// A root may be a single file: the render server's shader priming state (PrimeShaders.h) in its persistent cache directory, small and
// the only record of how far priming got (backboardd runs as mobile; the root home is the fallback).
static const char *const diag_roots[] = {
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/prime.log",
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/prime-bad",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/prime.log",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/prime-bad",
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/shadow.log",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/shadow.log",
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/frames.trace",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/frames.trace",
  "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/cpu.profile",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/cpu.profile",
  // Safari's built-in data lists (int-269: a nil built-in list URL aborted MobileSafari): what a directory read of the framework
  // returns next to what a lookup of each file finds. Small, so they go before the log.
  "/System/Library/PrivateFrameworks/SafariShared.framework/UserAgentQuirks.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework/SitesAllowedToAutoplay.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework/SitesAllowedToUseAutoplayQuirks.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework/WBSOfflineSearchRemoteDisablementDefaultConfig.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework/WBSPasswordBreachConfigurationBag.plist",
  "/System/Library/PrivateFrameworks/SafariCore.framework/WBSAutoFillQuirks.plist",
  "/System/Library/PrivateFrameworks/SafariCore.framework/WBSPasswordWarningTopFraudTargets.plist",
  "/System/Library/PrivateFrameworks/SafariShared.framework",
  "/System/Library/PrivateFrameworks/SafariCore.framework",
  "/private/var/db/diagnostics",
  "/private/var/db/uuidtext",
  "/private/var/mobile/Library/Logs/CrashReporter",
};
extern volatile uint64_t native_gop_virtual __attribute__((weak_import));
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
static uint32_t shot_width, shot_height;   // the half-resolution screenshot; 0 when there is no scanout

static void note(const char *text, const char *path, int error) {
  if (!diag_note) return;
  int n = snprintf(diag_note + diag_note_used, DiagNote - diag_note_used, "%s %s %d\n", text, path, error);
  if (n > 0 && diag_note_used + (size_t)n < DiagNote) diag_note_used += (size_t)n;
}

static void add(const char *path, uint32_t root, vfs_context_t ctx) {
  vnode_t file = NULLVP;
  int error = vnode_lookup(path, 0, &file, ctx);
  if (error) {note("lookup", path, error); ++boot_diag_errors; return;}
  struct vnode_attr va;
  VATTR_INIT(&va);
  VATTR_WANTED(&va, va_data_size);
  VATTR_WANTED(&va, va_mode);
  VATTR_WANTED(&va, va_uid);
  VATTR_WANTED(&va, va_gid);
  error = vnode_getattr(file, &va, ctx);
  vnode_put(file);
  if (error) {note("getattr", path, error); ++boot_diag_errors; return;}
  if (diag_count >= DiagMaxFiles) {note("unlisted", path, (int)(va.va_data_size >> 10)); ++diag_skipped_count; return;}
  struct diag_file *f = &diag_files[diag_count++];
  strlcpy(f->path, path, sizeof f->path);
  f->size = va.va_data_size; f->mode = va.va_mode; f->uid = va.va_uid; f->gid = va.va_gid; f->root = root; f->keep = 0;
}

static void scan(const char *path, unsigned depth, uint32_t root, vfs_context_t ctx) {
  vnode_t dir = NULLVP;
  int error = vnode_lookup(path, 0, &dir, ctx);
  if (error) {note("lookup", path, error); ++boot_diag_errors; return;}
  if (vnode_vtype(dir) == VREG) {vnode_put(dir); add(path, root, ctx); return;}
  if (vnode_vtype(dir) != VDIR) {vnode_put(dir); return;}
  char *buffer = IOMallocData(8192);
  char *child = IOMallocData(DiagPath);
  if (!buffer || !child) {vnode_put(dir); if (buffer) IOFreeData(buffer, 8192); if (child) IOFreeData(child, DiagPath); return;}
  off_t offset = 0;
  for (int eof = 0; !eof;) {
    uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
    if (!uio) break;
    uio_addiov(uio, CAST_USER_ADDR_T(buffer), 8192);
    int count = 0;
    error = VNOP_READDIR(dir, uio, 0, &eof, &count, ctx);
    const size_t got = 8192 - (size_t)uio_resid(uio);
    offset = uio_offset(uio);
    uio_free(uio);
    if (error) {note("readdir", path, error); ++boot_diag_errors; break;}
    if (!got) break;
    for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got;) {
      const struct dirent *d = (const struct dirent *)(buffer + at);
      if (!d->d_reclen) {eof = 1; break;}
      at += d->d_reclen;
      if (!d->d_ino || (d->d_namlen == 1 && d->d_name[0] == '.') || (d->d_namlen == 2 && d->d_name[0] == '.' && d->d_name[1] == '.')) continue;
      if ((size_t)snprintf(child, DiagPath, "%s/%.*s", path, (int)d->d_namlen, d->d_name) >= DiagPath) {note("long", path, 0); continue;}
      if (d->d_type == DT_DIR) {
        if (depth < DiagDepth) scan(child, depth + 1, root, ctx);
        continue;
      }
      if (d->d_type != DT_REG) continue;
      add(child, root, ctx);
    }
  }
  IOFreeData(child, DiagPath);
  IOFreeData(buffer, 8192);
  vnode_put(dir);
}

// The order the budget is spent in. Three classes, worth first:
//
// 0. The dyld shared cache's format-string catalog (/private/var/db/uuidtext/dsc, the one entry an
//    order of magnitude larger than the per-image catalogs). Without it every message a system
//    framework logged composes as "compose failure [shared UUID]": the last export held the whole
//    unified log and not this file, so SpringBoard's and backboardd's own lines were unreadable.
//    It is picked as the largest dsc entry the scan listed rather than by its UUID, which changes
//    with the OS build.
// 1. The persisted unified log's own files, the later name first: logd writes the live boot into
//    the highest-numbered Persist file (unlock1 2026-11-09 listed 0001..0009 and the live one was
//    0009). Ordering them by modification time picked an older, already-flushed file and the live
//    one whose chunks were not final yet, and the latter decoded to zero events.
// 2. Everything else in root order; inside a root the later name first (log files are numbered
//    upward, so the newest comes first).
static uint64_t diag_dsc_max;
static int diag_rank(const struct diag_file *f) {
  if (diag_dsc_max && f->size == diag_dsc_max && !strncmp(f->path, "/private/var/db/uuidtext/dsc/", 29)) return 0;
  if (!strncmp(f->path, "/private/var/db/diagnostics/Persist/", 36)) return 1;
  return 2;
}
static int worth(const void *a, const void *b) {
  const struct diag_file *x = a, *y = b;
  const int rx = diag_rank(x), ry = diag_rank(y);
  if (rx != ry) return rx < ry ? -1 : 1;
  if (x->root != y->root) return x->root < y->root ? -1 : 1;
  return -strcmp(x->path, y->path);
}

// sheng-ls-reset=1: empty the LaunchServices store before userland starts.
//
// 2026-11-09: the home screen's three widget slots are blank because pkd rejects every
// /Applications/<App>.app/PlugIns/*.appex with "plug-ins outside containing apps must be in
// /System", and the rest of that message names the remedy: "Maybe run 'mobile_install rebuild
// system user' to register the app containing your plugin with LS?". This board has no shell, so
// the store is emptied instead of rebuilt on request: lsd cannot use an invalid store and builds
// a new one. One-shot on the argument, and every path it touches is printed, so a run that does
// not change the home screen is as visible as one that does.
extern size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));
volatile uint32_t boot_ls_reset_files, boot_ls_reset_errors;
static uint32_t ls_reset_logged;

static void ls_reset_note(const char *path, int error) {
  if (!&qcom_usb_debug_write) return;
  char line[160];
  int n = snprintf(line, sizeof line, "LSRESET %s rc=%d\n", path, error);
  if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

// Report mode (the default) reads a store file and prints its size and mtime: whether lsd rebuilt
// the store since the last boot is the whole question a poking round leaves behind. Only
// sheng-ls-reset=1 empties what it finds; the late tick must not touch the store it measures.
static int ls_reset_poke;
static int ls_reset_one(const char *path, vfs_context_t ctx) {
  vnode_t vp = NULLVP;
  int error = vnode_lookup(path, 0, &vp, ctx);
  if (error) return error;
  struct vnode_attr va;
  VATTR_INIT(&va);
  if (!ls_reset_poke) {
    VATTR_WANTED(&va, va_data_size);
    VATTR_WANTED(&va, va_modify_time);
    error = vnode_getattr(vp, &va, ctx);
    vnode_put(vp);
    if (error) return error;
    char line[192];
    int n = snprintf(line, sizeof line, "LSSTORE %s size=%lld mtime=%ld\n",
                     path, (long long)va.va_data_size, (long)va.va_modify_time.tv_sec);
    if (n > 0 && &qcom_usb_debug_write) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    return 0;
  }
  VATTR_SET(&va, va_data_size, (off_t)0);
  error = vnode_setattr(vp, &va, ctx);
  vnode_put(vp);
  return error;
}

// com.apple.LaunchServices-<n>-v2.csstore, the form lsd names its store with (the number is the
// user id class: 5028 in the InternalDaemon container, 501 for the mobile user).
static int ls_reset_match(const char *name) {
  const size_t n = strlen(name);
  return n > 33 && !strncmp(name, "com.apple.LaunchServices-", 25) && !strcmp(name + n - 8, ".csstore");
}

static void ls_reset_walk(const char *path, unsigned depth, vfs_context_t ctx) {
  vnode_t dir = NULLVP;
  unsigned walked = 0;
  int lookup = vnode_lookup(path, 0, &dir, ctx);
  if (lookup) {ls_reset_note(path, lookup); return;}
  if (vnode_vtype(dir) != VDIR) {vnode_put(dir); return;}
  char *buffer = IOMallocData(8192);
  char *child = IOMallocData(DiagPath);
  if (!buffer || !child) {vnode_put(dir); if (buffer) IOFreeData(buffer, 8192); if (child) IOFreeData(child, DiagPath); return;}
  off_t offset = 0;
  for (int eof = 0; !eof;) {
    uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
    if (!uio) break;
    uio_addiov(uio, CAST_USER_ADDR_T(buffer), 8192);
    int count = 0;
    int error = VNOP_READDIR(dir, uio, 0, &eof, &count, ctx);
    const size_t got = 8192 - (size_t)uio_resid(uio);
    offset = uio_offset(uio);
    uio_free(uio);
    if (error || !got) break;
    for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got;) {
      const struct dirent *d = (const struct dirent *)(buffer + at);
      if (!d->d_reclen) {eof = 1; break;}
      at += d->d_reclen;
      if (!d->d_ino || (d->d_namlen == 1 && d->d_name[0] == '.') || (d->d_namlen == 2 && d->d_name[0] == '.' && d->d_name[1] == '.')) continue;
      if ((size_t)snprintf(child, DiagPath, "%s/%.*s", path, (int)d->d_namlen, d->d_name) >= DiagPath) continue;
      ++walked;
      if (d->d_type == DT_DIR) {
        if (depth < 4) ls_reset_walk(child, depth + 1, ctx);
        continue;
      }
      if (d->d_type != DT_REG || !ls_reset_match(d->d_name)) continue;
      if (!ls_reset_poke) continue;   // report mode reads fixed paths, never a walked match
      int rc = ls_reset_one(child, ctx);
      if (rc) ++boot_ls_reset_errors; else ++boot_ls_reset_files;
      ls_reset_note(child, rc);
    }
  }
  if (depth <= 2) ls_reset_note(path, -(int)walked);   // negative: entries seen, not an error
  IOFreeData(child, DiagPath);
  IOFreeData(buffer, 8192);
  vnode_put(dir);
}

// Runs from the display late probe, which is before launchd starts userland: a store emptied any
// later would already be open in lsd and mapped.
void boot_diag_ls_reset(void) {
  unsigned on = 0;
  if (ls_reset_logged) return;
  if (!PE_parse_boot_argn("sheng-ls-reset", &on, sizeof on) || on != 1) return;
  ls_reset_logged = 1;
  ls_reset_poke = 1;
  vfs_context_t ctx = vfs_context_kernel();
  ls_reset_walk("/private/var/mobile/Containers/Data/InternalDaemon", 0, ctx);
  ls_reset_walk("/private/var/mobile/Library/Caches", 0, ctx);
  if (&qcom_usb_debug_write) {
    char line[80];
    int n = snprintf(line, sizeof line, "LSRESET done files=%u errors=%u\n", boot_ls_reset_files, boot_ls_reset_errors);
    if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
}

// sheng-ls-late=<seconds>: report the LaunchServices store once the Data volume exists. The early
// call above runs before userland and finds nothing (hdp6: vnode_lookup rc=2 for both roots).
//
// Report by fixed path, never by walking: hdp9 walked the whole InternalDaemon tree from this same
// tick thread at t=80 and the thread never came back (mon froze at 80, the exec log stopped, the
// desktop never started). lsd maps and rewrites that tree at exactly this time, so the walk is one
// more reader of the files it is rebuilding. One lookup and one getattr on the store this device
// actually uses (the UUID is the one the hdp7 walk printed) answers the same question with no
// directory reads at all.
static const char *const ls_store_paths[] = {
  "/private/var/mobile/Containers/Data/InternalDaemon/2E637D83-1834-4994-8755-84173239AA65/Library/Caches/com.apple.LaunchServices-5028-v2.csstore",
  "/private/var/mobile/Library/Caches/com.apple.LaunchServices-501-v2.csstore",
};
static void ls_store_report(vfs_context_t ctx) {
  for (unsigned i = 0; i < sizeof(ls_store_paths) / sizeof(ls_store_paths[0]); ++i) {
    vnode_t vp = NULLVP;
    int error = vnode_lookup(ls_store_paths[i], 0, &vp, ctx);
    if (error) {ls_reset_note(ls_store_paths[i], error); continue;}
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    VATTR_WANTED(&va, va_modify_time);
    error = vnode_getattr(vp, &va, ctx);
    vnode_put(vp);
    if (error) {ls_reset_note(ls_store_paths[i], error); continue;}
    char line[224];
    int n = snprintf(line, sizeof line, "LSSTORE %s size=%lld mtime=%ld\n", ls_store_paths[i],
                     (long long)va.va_data_size, (long)va.va_modify_time.tv_sec);
    if (n > 0 && &qcom_usb_debug_write) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
}
// sheng-ls-report=<seconds>: the store's size and the root mount's read-only bit, run from the
// touch loop instead of the boot monitor. hdp9 (a directory walk at t=80) and hdp10 (one lookup at
// t=240) both stopped the monitor thread for the rest of the boot while lsd was rewriting the
// store; the touch loop is a thread whose stall costs touch and nothing else. One directory level
// only: the InternalDaemon container UUID changed when the Data volume was reset in hdp11, so the
// baked-in path reads ENOENT now.
static void ls_stat_store(const char *path, vfs_context_t ctx) {
  vnode_t vp = NULLVP;
  int error = vnode_lookup(path, 0, &vp, ctx);
  if (error) {ls_reset_note(path, error); return;}
  struct vnode_attr va;
  VATTR_INIT(&va);
  VATTR_WANTED(&va, va_data_size);
  VATTR_WANTED(&va, va_modify_time);
  error = vnode_getattr(vp, &va, ctx);
  vnode_put(vp);
  if (error) {ls_reset_note(path, error); return;}
  char line[224];
  int n = snprintf(line, sizeof line, "LSSTORE %s size=%lld mtime=%ld\n", path,
                   (long long)va.va_data_size, (long)va.va_modify_time.tv_sec);
  if (n > 0 && &qcom_usb_debug_write) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

// Every candidate store path: one level of InternalDaemon containers plus the mobile user's own
// cache. The container UUID changes when the Data volume is reset (hdp11), so the path is read,
// not baked in.
// Entry count of one directory, no recursion, names not used. The LS store names 382
// PluginKitPlugin containers and 208 /Applications bundles; whether those containers exist on this
// Data volume is the one thing that decides whether the store describes this machine or the one it
// was built on. A directory that cannot be read reports -errno.
static int ls_count_dir(const char *path, vfs_context_t ctx) {
  vnode_t dir = NULLVP;
  int error = vnode_lookup(path, 0, &dir, ctx);
  if (error) return -error;
  if (vnode_vtype(dir) != VDIR) {vnode_put(dir); return -ENOTDIR;}
  char *buffer = IOMallocData(4096);
  if (!buffer) {vnode_put(dir); return -ENOMEM;}
  int total = 0;
  off_t offset = 0;
  for (int eof = 0; !eof;) {
    uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
    if (!uio) break;
    uio_addiov(uio, CAST_USER_ADDR_T(buffer), 4096);
    int count = 0;
    int read_error = VNOP_READDIR(dir, uio, 0, &eof, &count, ctx);
    const size_t got = 4096 - (size_t)uio_resid(uio);
    offset = uio_offset(uio);
    uio_free(uio);
    if (read_error) {total = -read_error; break;}
    if (!got) break;
    for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got;) {
      const struct dirent *d = (const struct dirent *)(buffer + at);
      if (!d->d_reclen) {eof = 1; break;}
      at += d->d_reclen;
      if (!d->d_ino || (d->d_namlen == 1 && d->d_name[0] == '.') ||
          (d->d_namlen == 2 && d->d_name[0] == '.' && d->d_name[1] == '.')) continue;
      ++total;
    }
  }
  IOFreeData(buffer, 4096);
  vnode_put(dir);
  return total;
}

static unsigned ls_store_candidates(char (*paths)[DiagPath], unsigned max, vfs_context_t ctx) {
  static const char *const root = "/private/var/mobile/Containers/Data/InternalDaemon";
  unsigned count = 0;
  vnode_t dir = NULLVP;
  int lookup = vnode_lookup(root, 0, &dir, ctx);
  if (lookup) {
    ls_reset_note(root, lookup);
  } else if (vnode_vtype(dir) != VDIR) {
    vnode_put(dir);
  } else {
    char *buffer = IOMallocData(4096);
    if (buffer) {
      uio_t uio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
      if (uio) {
        uio_addiov(uio, CAST_USER_ADDR_T(buffer), 4096);
        int eof = 0, n = 0;
        int error = VNOP_READDIR(dir, uio, 0, &eof, &n, ctx);
        const size_t got = 4096 - (size_t)uio_resid(uio);
        uio_free(uio);
        if (error) {
          ls_reset_note(root, error);
        } else {
          for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got && count < max;) {
            const struct dirent *d = (const struct dirent *)(buffer + at);
            if (!d->d_reclen) break;
            at += d->d_reclen;
            if (d->d_type != DT_DIR || d->d_namlen < 2 || d->d_name[0] == '.') continue;
            snprintf(paths[count], DiagPath, "%s/%.*s/Library/Caches/com.apple.LaunchServices-5028-v2.csstore",
                     root, (int)d->d_namlen, d->d_name);
            ++count;
          }
        }
      }
      IOFreeData(buffer, 4096);
    }
    vnode_put(dir);
  }
  if (count < max) {
    strlcpy(paths[count], "/private/var/mobile/Library/Caches/com.apple.LaunchServices-501-v2.csstore", DiagPath);
    ++count;
  }
  return count;
}

static uint32_t ls_report_at, ls_report_done, ls_report_first_ms;
void boot_diag_ls_report_ms(unsigned now_ms) {
  if (ls_report_done) return;
  if (!ls_report_at && (!PE_parse_boot_argn("sheng-ls-report", &ls_report_at, sizeof ls_report_at) || !ls_report_at)) {
    ls_report_done = 1;
    return;
  }
  if (!ls_report_first_ms) {ls_report_first_ms = now_ms ? now_ms : 1; return;}
  if ((now_ms - ls_report_first_ms) / 1000u < ls_report_at) return;
  ls_report_done = 1;
  vfs_context_t ctx = vfs_context_kernel();
  {
    vnode_t root_vnode = vfs_rootvnode();
    mount_t mount = root_vnode ? vnode_mount(root_vnode) : NULL;
    const unsigned long flag = mount ? (unsigned long)mount->mnt_flag : 0;
    if (root_vnode) vnode_put(root_vnode);
    if (&qcom_usb_debug_write) {
      char line[80];
      int n = snprintf(line, sizeof line, "ROOTFS rdonly=%u flag=%lx\n", (flag & MNT_RDONLY) ? 1u : 0u, flag);
      if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    }
  }
  {
    char paths[8][DiagPath];
    const unsigned count = ls_store_candidates(paths, 8, ctx);
    for (unsigned i = 0; i < count; ++i) ls_stat_store(paths[i], ctx);
  }
  {
    /* The store names 382 plugin containers and 208 apps (hdp16). If the directories hold the same
     * order of magnitude the store describes this machine; if they are empty, it describes the
     * machine it was built on and every container path in it is stale. The two UUIDs are read off
     * that store's own strings, so their lookup result says the same thing for one concrete name. */
    const int plugins = ls_count_dir("/private/var/mobile/Containers/Data/PluginKitPlugin", ctx);
    const int apps = ls_count_dir("/private/var/mobile/Containers/Data/Application", ctx);
    vnode_t probe = NULLVP;
    int rc_plugin = vnode_lookup("/private/var/mobile/Containers/Data/PluginKitPlugin/0CCC2140-A8BC-4365-8C4E-DFE241F40423", 0, &probe, ctx);
    if (!rc_plugin) vnode_put(probe);
    probe = NULLVP;
    int rc_app = vnode_lookup("/private/var/mobile/Containers/Data/Application/300A91FA-3F52-4530-B57B-88364EB53DF7", 0, &probe, ctx);
    if (!rc_app) vnode_put(probe);
    if (&qcom_usb_debug_write) {
      char line[160];
      int n = snprintf(line, sizeof line, "LSCONT plugin=%d app=%d uuid_plugin=%d uuid_app=%d\n",
                       plugins, apps, rc_plugin, rc_app);
      if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    }
  }
}

// sheng-ls-dump=<seconds>: print the printable strings of the store itself, so what the database
// holds is readable on the host without a shell. Same thread as the report, bounded to the first
// 8 MiB read and 96 KiB printed. A 6,295,552-byte store (hdp13) answered "is it fresh or empty";
// only its strings can say which applications and plugins it carries, and the first round of this
// (hdp14) printed 167 runs of index-table noise because every eight printable bytes qualified.
// A run worth printing: at least ten characters, one of them a dot or a slash, and nothing
// outside what an identifier or a path uses. The store's index tables are full of eight-character
// runs of printable bytes (" ,(0(4(8(<(@(") and those filled the budget before any identifier did.
static int ls_texty(const char *s, size_t n) {
  if (n < 10) return 0;
  int mark = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (c == '.' || c == '/') {mark = 1; continue;}
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '+' || c == ':' || c == ' ')) return 0;
  }
  return mark;
}

static void ls_dump_run(char *runbuf, size_t *run, unsigned *runs, size_t *used) {
  if (!ls_texty(runbuf, *run)) {*run = 0; return;}
  runbuf[*run] = 0;
  if (&qcom_usb_debug_write && *runs < 1200 && *used < 96 * 1024) {
    char line[160];
    int n = snprintf(line, sizeof line, "LSTR %s\n", runbuf);
    if (n > 0) {
      qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
      *used += (size_t)n;
      ++*runs;
    }
  }
  *run = 0;
}

static void ls_dump_file(const char *path, vfs_context_t ctx) {
  vnode_t vp = NULLVP;
  int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
  if (error) {ls_reset_note(path, error); return;}
  char *buf = IOMallocData(4096);
  if (!buf) {vnode_close(vp, FREAD, ctx); return;}
  if (&qcom_usb_debug_write) {
    char line[192];
    int n = snprintf(line, sizeof line, "LSDMP %s\n", path);
    if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
  off_t off = 0;
  size_t used = 0, run = 0;
  unsigned runs = 0;
  char runbuf[128];
  while (off < (off_t)(8 * 1024 * 1024) && used < 96 * 1024 && runs < 1200) {
    uio_t uio = uio_create(1, off, UIO_SYSSPACE, UIO_READ);
    if (!uio) break;
    uio_addiov(uio, CAST_USER_ADDR_T(buf), 4096);
    int read_error = VNOP_READ(vp, uio, 0, ctx);
    const size_t got = 4096 - (size_t)uio_resid(uio);
    uio_free(uio);
    if (read_error || !got) break;
    for (size_t i = 0; i < got;) {
      const unsigned char c = (unsigned char)buf[i];
      /* CSStore keeps some of its text as UTF-16LE: one printable byte followed by a zero.
       * Reading that as plain bytes would break every run at length one, so a printable byte
       * with a zero after it counts as one wide character and is consumed two bytes at a time. */
      const int wide = (i + 1 < got) && (unsigned char)buf[i + 1] == 0;
      if (c >= 32 && c < 127) {
        if (run < sizeof runbuf - 1) runbuf[run++] = (char)c;
        else ls_dump_run(runbuf, &run, &runs, &used);
        i += wide ? 2 : 1;
      } else {
        ls_dump_run(runbuf, &run, &runs, &used);
        ++i;
      }
    }
    off += (off_t)got;
  }
  ls_dump_run(runbuf, &run, &runs, &used);
  IOFreeData(buf, 4096);
  vnode_close(vp, FREAD, ctx);
  if (&qcom_usb_debug_write) {
    char line[96];
    int n = snprintf(line, sizeof line, "LSDMP done bytes=%lld runs=%u\n", (long long)off, runs);
    if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
}

static uint32_t ls_dump_at, ls_dump_done, ls_dump_first_ms;
void boot_diag_ls_dump_ms(unsigned now_ms) {
  if (ls_dump_done) return;
  if (!ls_dump_at && (!PE_parse_boot_argn("sheng-ls-dump", &ls_dump_at, sizeof ls_dump_at) || !ls_dump_at)) {
    ls_dump_done = 1;
    return;
  }
  if (!ls_dump_first_ms) {ls_dump_first_ms = now_ms ? now_ms : 1; return;}
  if ((now_ms - ls_dump_first_ms) / 1000u < ls_dump_at) return;
  ls_dump_done = 1;
  vfs_context_t ctx = vfs_context_kernel();
  char paths[8][DiagPath];
  const unsigned count = ls_store_candidates(paths, 8, ctx);
  for (unsigned i = 0; i < count; ++i) ls_dump_file(paths[i], ctx);
}


static uint32_t ls_late_at, ls_late_done;
void boot_diag_ls_reset_tick(unsigned now) {
  if (ls_late_done) return;
  if (!ls_late_at && (!PE_parse_boot_argn("sheng-ls-late", &ls_late_at, sizeof ls_late_at) || !ls_late_at)) {
    ls_late_done = 1;
    return;
  }
  if (now < ls_late_at) return;
  ls_late_done = 1;
  ls_store_report(vfs_context_kernel());
  if (&qcom_usb_debug_write) {
    char line[96];
    int n = snprintf(line, sizeof line, "LSRESET late t=%us files=%u errors=%u\n", now, boot_ls_reset_files, boot_ls_reset_errors);
    if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
}

// The stream's length (without the 16-byte blob header) for at most budget bytes, after scanning the roots; 0 when nothing could be read.
uint64_t boot_diag_scan(uint64_t budget) {
  if (!diag_files) diag_files = IOMallocData(sizeof(struct diag_file) * DiagMaxFiles);
  if (!diag_note) diag_note = IOMallocData(DiagNote);
  if (!diag_files || !diag_note) return 0;
  diag_count = diag_skipped_count = 0; diag_note_used = 0; diag_note[0] = 0;
  const char *note_name = "diag-skipped.txt";
  uint64_t used = 8 + 4 + 1 + 4 + 24 + strlen(note_name) + DiagNote;   // head with root "/", the note file at its full size
  PE_Video video = {};
  shot_width = shot_height = 0;
  if (&native_gop_virtual && native_gop_virtual && !PE_current_console(&video) && video.v_depth == 32 && video.v_width >= 2 && video.v_height >= 2 &&
      video.v_width <= 16384 && video.v_height <= 16384 && video.v_rowBytes >= video.v_width * 4) {
    const uint64_t cost = 24 + strlen("screen.bgrx") + 16 + (uint64_t)(video.v_width / 2) * (video.v_height / 2) * 4;
    if (used + cost <= budget) {shot_width = (uint32_t)video.v_width / 2; shot_height = (uint32_t)video.v_height / 2; used += cost;}
  }
  vfs_context_t ctx = vfs_context_kernel();
  for (unsigned i = 0; i < sizeof diag_roots / sizeof diag_roots[0]; ++i) scan(diag_roots[i], 0, i, ctx);
  diag_dsc_max = 0;
  for (uint32_t i = 0; i < diag_count; ++i)
    if (diag_files[i].size > diag_dsc_max && !strncmp(diag_files[i].path, "/private/var/db/uuidtext/dsc/", 29))
      diag_dsc_max = diag_files[i].size;
  qsort(diag_files, diag_count, sizeof diag_files[0], worth);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < diag_count; ++i) {
    struct diag_file *f = &diag_files[i];
    const uint64_t cost = 24 + strlen(f->path) - 1 + f->size;
    if (used + cost > budget) {note("skipped", f->path, (int)(f->size >> 10)); ++diag_skipped_count; continue;}
    f->keep = 1; used += cost; ++kept;
  }
  boot_diag_files = kept; boot_diag_skipped = diag_skipped_count;
  return used;
}

typedef int (*boot_diag_emit_t)(const void *, uint32_t, void *);
static int put_head(boot_diag_emit_t emit, void *arg, const char *path, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t size) {
  uint8_t head[24];
  const uint32_t plen = (uint32_t)strlen(path), fields[4] = {plen, mode, uid, gid};
  memcpy(head, fields, 16); memcpy(head + 16, &size, 8);
  return emit(head, sizeof head, arg) || emit(path, plen, arg);
}

// Emits the stream the last scan measured; each file goes out at its scanned size (zero-filled when it shrank or could not be read, cut
// when it grew), so the length always equals boot_diag_scan's. Returns the first nonzero emit result.
int boot_diag_stream(boot_diag_emit_t emit, void *arg) {
  uint8_t *chunk = IOMallocData(DiagChunk);
  if (!chunk) return -1;
  vfs_context_t ctx = vfs_context_kernel();
  int rc = emit("M2VCACH1", 8, arg);
  const uint32_t root_len = 1, count = boot_diag_files + 1 + (shot_width ? 1 : 0);
  if (!rc) rc = emit(&root_len, 4, arg);
  if (!rc) rc = emit("/", 1, arg);
  if (!rc) rc = emit(&count, 4, arg);
  if (!rc && shot_width) {
    // Every second pixel of every second row, read from the scanout mapping the display presenter set up.
    PE_Video video = {};
    const uint32_t row = shot_width * 4, head[4] = {'S' | 'H' << 8 | 'O' << 16 | (uint32_t)'T' << 24, shot_width, shot_height, row};
    const bool live = !PE_current_console(&video) && video.v_width / 2 == shot_width && video.v_height / 2 == shot_height;
    rc = put_head(emit, arg, "screen.bgrx", 0100644, 0, 0, 16 + (uint64_t)row * shot_height);
    if (!rc) rc = emit(head, sizeof head, arg);
    for (uint32_t y = 0; !rc && y < shot_height; ++y) {
      uint32_t *out = (uint32_t *)chunk;
      const uint32_t *in = (const uint32_t *)(uintptr_t)(native_gop_virtual + (uint64_t)y * 2 * video.v_rowBytes);
      for (uint32_t x = 0; x < shot_width; ++x) out[x] = live ? in[x * 2] : 0;
      rc = emit(chunk, row, arg);
    }
  }
  for (uint32_t i = 0; !rc && i < diag_count; ++i) {
    const struct diag_file *f = &diag_files[i];
    if (!f->keep) continue;
    rc = put_head(emit, arg, f->path + 1, f->mode, f->uid, f->gid, f->size);
    vnode_t vp = NULLVP;
    int error = rc ? 0 : vnode_open(f->path, FREAD, 0, 0, &vp, ctx);
    if (error) {note("open", f->path, error); ++boot_diag_errors; vp = NULLVP;}
    for (uint64_t at = 0; !rc && at < f->size;) {
      const uint32_t want = f->size - at < DiagChunk ? (uint32_t)(f->size - at) : DiagChunk;
      int resid = (int)want;
      memset(chunk, 0, want);
      if (vp && (error = vn_rdwr(UIO_READ, vp, (caddr_t)chunk, (int)want, (off_t)at, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
                                 vfs_context_proc(ctx)))) {
        note("read", f->path, error); ++boot_diag_errors;
        vnode_close(vp, FREAD, ctx); vp = NULLVP;
      }
      rc = emit(chunk, want, arg);
      at += want;
    }
    if (vp) vnode_close(vp, FREAD, ctx);
  }
  // The note last, at the size the scan reserved.
  if (!rc) rc = put_head(emit, arg, "diag-skipped.txt", 0100644, 0, 0, DiagNote);
  if (!rc) {memset(diag_note + diag_note_used, 0, DiagNote - diag_note_used); rc = emit(diag_note, DiagNote, arg);}
  IOFreeData(chunk, DiagChunk);
  return rc;
}
