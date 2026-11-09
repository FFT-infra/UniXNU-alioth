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
#include <sys/proc_internal.h>
#include <kern/task.h>
#include <sys/vnode.h>
#include <sys/mount_internal.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

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
  // The widget line's open question: what state makes installd run the system-app LaunchServices
  // migration at boot. Its database directory holds the system-app state and the last LS map
  // (the "rebuild system" step a shell would run), and the system data containers carry installd's
  // own. All are small, so they are funded before the unified log (diag_rank below).
  "/private/var/mobile/Library/MobileInstallation",
  "/private/var/containers/Data/System",
  "/private/var/db/datamigrator",
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
// The MobileInstallation state and the system data containers answer a question no other file can,
// and they are kilobytes: rank them with the persisted log so the log's own files cannot starve
// them. Their roots are listed first, so a rank tie falls their way.
static int diag_mi_state(const char *path) {
  return !strncmp(path, "/private/var/mobile/Library/MobileInstallation", 46) ||
         !strncmp(path, "/private/var/containers/Data/System", 35) ||
         !strncmp(path, "/private/var/db/datamigrator", 28);
}
static int diag_rank(const struct diag_file *f) {
  if (diag_dsc_max && f->size == diag_dsc_max && !strncmp(f->path, "/private/var/db/uuidtext/dsc/", 29)) return 0;
  if (diag_mi_state(f->path)) return 1;
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


// lsfix=<seconds>: print the owner daemon's LaunchServices repair log
// (/private/var/ngpu/lsfix.log). The daemon is the only userland process this image adds; it runs
// as root outside every sandbox through the addon's launchd overlay, and this row is how its result
// reaches the host without a shell. Lines print as they are, 200 bytes each, first 16 KiB.
// Which processes carry a bootstrap port: the owner helper has to copy one because it was forked
// before launchd ran, and task_for_pid plus task_get_special_port could not find it from userland
// (in6). Read here from the task itself, so the next round knows whether the port exists at all or
// only the copy is blocked.
static void ls_bootstraps(void) {
  int shown = 0;
  for (int pid = 1; pid <= 300 && shown < 24; ++pid) {
    proc_t p = proc_find(pid);
    if (!p) continue;
    task_t task = proc_task(p);
    mach_port_t bootstrap = MACH_PORT_NULL;
    const int found = task && task_get_special_port(task, TASK_BOOTSTRAP_PORT, &bootstrap) == KERN_SUCCESS;
    const int boot = found && bootstrap != MACH_PORT_NULL;
    if (&qcom_usb_debug_write) {
      char line[96];
      const int n = snprintf(line, sizeof line, "LSBOOT pid=%d name=%s boot=%d\n", pid, p->p_name, boot);
      if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    }
    ++shown;
    proc_rele(p);
  }
}

// The owner helper is a child of the init trampoline and was forked before launchd ran, so its task
// has no bootstrap port and it cannot copy one from userland: task_for_pid reaches pid 1 only, and
// launchd's own task carries none (in5/in6). The kernel can hand it one from a process launchd
// already started, which is what this does while the helper waits. One gift per boot; the helper
// adopts the port and calls LaunchServices.
static uint32_t lsgift_at, lsgift_done;
static void ls_bootstrap_gift(unsigned now_ms) {
  if (lsgift_done || (lsgift_at && now_ms - lsgift_at < 1000u)) return;
  lsgift_at = now_ms ? now_ms : 1;
  proc_t worker = NULL;
  for (int pid = 2; pid <= 400 && !worker; ++pid) {
    proc_t p = proc_find(pid);
    if (!p) continue;
    if (p->p_name[0] == 'l' && !strcmp(p->p_name, "lsfixd")) worker = p;
    else proc_rele(p);
  }
  if (!worker) return;
  task_t wt = proc_task(worker);
  mach_port_t have = MACH_PORT_NULL;
  if (wt && task_get_special_port(wt, TASK_BOOTSTRAP_PORT, &have) == KERN_SUCCESS && have != MACH_PORT_NULL) {
    lsgift_done = 1;
    proc_rele(worker);
    return;
  }
  for (int pid = 2; pid <= 400; ++pid) {
    proc_t p = proc_find(pid);
    if (!p) continue;
    if (p == worker) { proc_rele(p); continue; }
    task_t t = proc_task(p);
    mach_port_t b = MACH_PORT_NULL;
    const int ok = t && task_get_special_port(t, TASK_BOOTSTRAP_PORT, &b) == KERN_SUCCESS && b != MACH_PORT_NULL;
    if (!ok) { proc_rele(p); continue; }
    const kern_return_t kr = wt ? task_set_special_port(wt, TASK_BOOTSTRAP_PORT, b) : KERN_FAILURE;
    if (&qcom_usb_debug_write) {
      char line[96];
      const int n = snprintf(line, sizeof line, "LSGIFT from pid=%d name=%s kr=%d\n", pid, p->p_name, kr);
      if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    }
    lsgift_done = kr == KERN_SUCCESS;
    proc_rele(p);
    break;
  }
  proc_rele(worker);
}

// Print a text file line by line, tag first, the first 16 KiB. Used for the logs a launchd cache
// question is decided by: the trampoline's own log, launchd_cache_loader's log (it names the CDHash
// it validated or the failure), and launchd's job log (a spawn refusal is written there, not in the
// exec observer, because posix_spawn never creates the process).
static void ls_dump_text(const char *path, const char *tag) {
  vfs_context_t ctx = vfs_context_kernel();
  vnode_t vp = NULLVP;
  const int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
  if (error) {ls_reset_note(path, error); return;}
  char *buf = IOMallocData(4096);
  if (!buf) {vnode_close(vp, FREAD, ctx); return;}
  off_t off = 0;
  while (off < (off_t)(16 * 1024) && &qcom_usb_debug_write) {
    uio_t uio = uio_create(1, off, UIO_SYSSPACE, UIO_READ);
    if (!uio) break;
    uio_addiov(uio, CAST_USER_ADDR_T(buf), 4096);
    const int read_error = VNOP_READ(vp, uio, 0, ctx);
    const size_t got = 4096 - (size_t)uio_resid(uio);
    uio_free(uio);
    if (read_error || !got) break;
    for (size_t i = 0; i < got;) {
      size_t end = i;
      while (end < got && buf[end] != '\n') ++end;
      char line[240];
      size_t n = end - i;
      if (n > 200) n = 200;
      const int prefix = snprintf(line, sizeof line, "%s ", tag);
      memcpy(line + prefix, buf + i, n);
      line[prefix + n] = 0;
      qcom_usb_debug_write(line, (size_t)prefix + n);
      qcom_usb_debug_write("\n", 1);
      i = end + 1;
      if ((off + (off_t)i) >= (off_t)(16 * 1024)) break;
    }
    off += (off_t)got;
  }
  IOFreeData(buf, 4096);
  vnode_close(vp, FREAD, ctx);
}

// Print only the lines of a text file that contain needle, tag first, the first 512 KiB scanned.
// launchd's job log is far larger than a dump budget and the evidence for one label is a handful
// of lines inside it; a full dump loses them past the cut.
static void ls_grep_text(const char *path, const char *needle, const char *tag, unsigned limit) {
  vfs_context_t ctx = vfs_context_kernel();
  vnode_t vp = NULLVP;
  if (vnode_open(path, FREAD, 0, 0, &vp, ctx)) return;
  char *buf = IOMallocData(4096);
  if (!buf) {vnode_close(vp, FREAD, ctx); return;}
  char line[512];
  size_t used = 0;
  unsigned printed = 0;
  off_t off = 0;
  while (off < (off_t)(512 * 1024) && printed < limit && &qcom_usb_debug_write) {
    uio_t uio = uio_create(1, off, UIO_SYSSPACE, UIO_READ);
    if (!uio) break;
    uio_addiov(uio, CAST_USER_ADDR_T(buf), 4096);
    const int read_error = VNOP_READ(vp, uio, 0, ctx);
    const size_t got = 4096 - (size_t)uio_resid(uio);
    uio_free(uio);
    if (read_error || !got) break;
    for (size_t i = 0; i < got && printed < limit; ++i) {
      if (buf[i] != '\n') {
        if (used < sizeof line - 1) line[used++] = buf[i];
        continue;
      }
      line[used] = 0;
      used = 0;
      if (!strnstr(line, needle, sizeof line)) continue;
      char out[560];
      const int n = snprintf(out, sizeof out, "%s %s\n", tag, line);
      if (n > 0) {
        qcom_usb_debug_write(out, (size_t)n < sizeof out ? (size_t)n : sizeof out - 1);
        ++printed;
      }
    }
    off += (off_t)got;
  }
  IOFreeData(buf, 4096);
  vnode_close(vp, FREAD, ctx);
}

// The GPU trace's problem records (event 65): the GMEM colour-order test (sheng-swaptest=1) reports
// each of its A/B/C/D regions through this event, and the trace ring is otherwise only readable
// from the persistent log through a TWRP round. Copying the newest records here puts the lines on
// the USB console in the same boot. The snapshot returns the newest records that fit, so it is
// taken twice: once just after the test window and once at the lsfix dump.
extern uint32_t native_gpu_trace_snapshot(void *out, uint32_t bytes) __attribute__((weak_import));
struct diag_trace_record {uint64_t sequence, us; uint32_t event, pid; uint64_t value[12];};
static void gpu_problem_dump(const char *tag) {
  enum {TraceBytes = 4 * 1024 * 1024};
  if (!&native_gpu_trace_snapshot || !&qcom_usb_debug_write) return;
  char *out = IOMallocData(TraceBytes);
  if (!out) return;
  const uint32_t got = native_gpu_trace_snapshot(out, TraceBytes);
  if (got >= 32 + sizeof(struct diag_trace_record)) {
    const uint32_t count = *(const uint32_t *)(void *)(out + 12);
    const struct diag_trace_record *rows = (const struct diag_trace_record *)(void *)(out + 32);
    char line[240];
    unsigned printed = 0;
    for (uint32_t i = 0; i < count && printed < 40; ++i) {
      if (rows[i].event != 65) continue;
      const char *text = (const char *)&rows[i].value[1];
      const int n = snprintf(line, sizeof line, "%s %.96s\n", tag, text);
      if (n > 0) {
        qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        ++printed;
      }
    }
  }
  IOFreeData(out, TraceBytes);
}

// The UI scale the payload actually shipped: /chosen display-scale is the number of scanout pixels
// per logical point (LegacyDisplayProbe.cpp), so 3 reads 360x800 points on this panel and 2 reads
// 540x1200. Printed because a board round with no screenshot otherwise cannot tell the two apart.
static void display_scale_row(void) {
  DTEntry chosen = NULL;
  const void *value = NULL;
  unsigned int size = 0;
  if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != 0 || chosen == NULL) return;
  if (SecureDTGetProperty(chosen, "display-scale", &value, &size) != 0 || value == NULL || size != 4) return;
  uint32_t scale = 0;
  memcpy(&scale, value, sizeof scale);
  if (&qcom_usb_debug_write) {
    char line[64];
    const int n = snprintf(line, sizeof line, "LSSCALE display-scale=%u\n", scale);
    if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
}

// Size of one path, or nothing when it does not exist.
static void ls_view_size(const char *path) {
  vnode_t vp = NULLVP;
  if (vnode_lookup(path, 0, &vp, vfs_context_kernel()) != 0 || vp == NULLVP) return;
  struct vnode_attr va;
  VATTR_INIT(&va);
  VATTR_WANTED(&va, va_data_size);
  if (vnode_getattr(vp, &va, vfs_context_kernel()) == 0 && &qcom_usb_debug_write) {
    char line[192];
    const int n = snprintf(line, sizeof line, "LSVIEW %s size=%lld\n", path, (long long)va.va_data_size);
    if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
  vnode_put(vp);
}

static uint32_t lsfix_at, lsfix_done, lsfix_first_ms;
// Fixed checkpoints, independent of the lsfix argument: the colour-order test runs about twenty
// seconds after the render server publishes its device, and that time moves with boot health, so
// three snapshots are taken and each prints only what is in the ring at that moment.
static uint32_t trace_marks;
void boot_diag_lsfix_ms(unsigned now_ms) {
  static const uint32_t marks[3] = {60000, 120000, 180000};
  static const char *const tags[3] = {"GTPROB1", "GTPROB2", "GTPROB3"};
  while (trace_marks < 3 && now_ms >= marks[trace_marks]) {
    gpu_problem_dump(tags[trace_marks]);
    ++trace_marks;
  }
  if (lsfix_done) return;
  ls_bootstrap_gift(now_ms);
  if (!lsfix_at && (!PE_parse_boot_argn("lsfix", &lsfix_at, sizeof lsfix_at) || !lsfix_at)) {lsfix_done = 1; return;}
  if (!lsfix_first_ms) {lsfix_first_ms = now_ms ? now_ms : 1; return;}
  if ((now_ms - lsfix_first_ms) / 1000u < lsfix_at) return;
  lsfix_done = 1;
  ls_bootstraps();
  /* The trampoline's own log (it writes /dev/console, which does not reach the USB capture) and the
   * cache file's size: 887525 means our merged cache is mounted over /System/Library/xpc, 1640626 the
   * sealed one. The two .sig sizes separate a mounted merged signature (11674) from the sealed one
   * (21346); a job plist that is in the cache but whose Program is absent fails in launchd, not in the
   * exec observer, so its path is measured too. */
  ls_view_size("/private/var/ngpu/lsinit.log");
  ls_view_size("/System/Library/xpc/launchd.plist");
  ls_view_size("/System/Library/xpc/launchd.plist.sig");
  ls_view_size("/private/var/ngpu/xpc/launchd.plist");
  ls_view_size("/private/var/ngpu/xpc/launchd.plist.sig");
  ls_view_size("/private/var/ngpu/bin/adrenoprobed");
  ls_view_size("/private/var/ngpu/bin/lsfixd");
  /* The store's size and modify time say whether anything rewrote it this boot (the seed carries
   * mtime 64); the DataClassMigrator route is judged by this row alone until the widgets show. */
  {
    char stores[8][DiagPath];
    const unsigned n = ls_store_candidates(stores, 8, vfs_context_kernel());
    for (unsigned i = 0; i < n; ++i) ls_stat_store(stores[i], vfs_context_kernel());
  }
  ls_dump_text("/private/var/ngpu/lsfix.log", "LSFIX");
  ls_dump_text("/private/var/ngpu/lsinit.log", "LSINITLOG");
  ls_dump_text("/var/log/com.apple.xpc.launchd/launchd_cache_loader.log", "LSCACHELOG");
  ls_view_size("/var/log/com.apple.xpc.launchd/launchd.log");
  ls_grep_text("/var/log/com.apple.xpc.launchd/launchd.log", "adrenoprobed", "LSJOB", 40);
  display_scale_row();
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
  {
    /* sheng-ls-late was written before the container UUID was known to change: its two baked-in
     * paths read ENOENT on every volume created after the first (hdp18 printed rc=2 for both and
     * truncated nothing). Read the containers first and poke the store in whichever one carries it. */
    vfs_context_t ctx = vfs_context_kernel();
    ls_reset_poke = 1;
    char paths[8][DiagPath];
    const unsigned count = ls_store_candidates(paths, 8, ctx);
    for (unsigned i = 0; i < count; ++i) {
      const int rc = ls_reset_one(paths[i], ctx);
      if (rc) ++boot_ls_reset_errors; else ++boot_ls_reset_files;
      ls_reset_note(paths[i], rc);
    }
  }
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
