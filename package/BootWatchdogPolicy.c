// SPDX-License-Identifier: MIT
// Explicit temporary bring-up policy for an unavailable watchdog backend.
// Runs after writable scratch Data mounts, before the unchanged launchd exec.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

static const char policy_head[] =
 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
 "<plist version=\"1.0\"><dict>";
static const char policy_watchdogd[]="<key>com.apple.watchdogd</key><true/>";
// Bring-up isolation: hardware stacks show identityservicesd, contextstored and locationd in a
// cycle of synchronous XPC waits. A disabled job makes its Mach service lookups fail instead of hang.
// sheng-disable-jobs=<label>[,<label>...] (restricted label characters, bounded length).
static const char policy_tail[]="</dict></plist>\n";
// sheng-lite=1: jobs that only reach for the network, iCloud or the photo library and have nothing to do on this tablet (no radios, no accounts),
// all of which start at the first boot of a fresh Data and compete with SpringBoard for the CPU and for memory. The first four are the
// cycle of synchronous XPC waits the isolation work found. Labels are the ones in the target's own LaunchDaemons.
static const char lite_jobs[]=
 "com.apple.identityservicesd,com.apple.locationd,com.apple.CommCenter,com.apple.bluetoothd,"
 "com.apple.photoanalysisd,com.apple.mediaanalysisd,com.apple.mediastream.mstreamd,com.apple.icloud.searchpartyd,"
 "com.apple.findmy.findmybeaconingd,com.apple.familycircled,com.apple.financed,com.apple.softwareupdateservicesd,"
 "com.apple.ndoagent,com.apple.biomesyncd,com.apple.email.maild,com.apple.amsengagementd,com.apple.adid,com.apple.eyereliefd,"
 // Crash or respawn loops seen on hardware (int-176): fairplayd.A2 is KeepAlive and exits at once (22 launches in 6 minutes), passd crashes,
 // obliteration is a data-wipe daemon with nothing to do here. iconservicesagent also dies in CI::GLContext::init, but disabling it leaves
 // the icons missing (seen on hardware), so it stays enabled.
 //
 // The two posters are the wallpaper extensions that fault with SIGBUS (ESR 0x82000007) on a userspace pointer every boot
 // (fix76 CRASH p262/p263/p264, fix78 CRASH p267/p269). Their labels are the launchd ones the OS volume declares, read out of the
 // running system's msgbuf: com.apple.NanoUniverse.AegirProxyApp.AegirPoster and com.apple.WatchFacesWallpaperSupport.KaleidoscopePoster.
 // The short names com.apple.AegirPoster / com.apple.KaleidoscopePoster are the process names, not the labels, and a disabled key that
 // names nothing disables nothing (b0c316f did exactly that and the crash stayed).
 "com.apple.fairplayd.A2,com.apple.passd,com.apple.mobile.obliteration";

// The two wallpaper extensions, kept apart from the list above because sheng-wallpaper-posters=1 puts
// them back on their own. They faulted with SIGBUS (ESR 0x82000007) on a userspace pointer every boot
// when the list above was written; that was 2026-11-06 and every boot then lost a core about seven
// seconds in, so they are worth one round with the core loss fixed before they are written off.
static const char lite_posters[]="com.apple.NanoUniverse.AegirProxyApp.AegirPoster,com.apple.WatchFacesWallpaperSupport.KaleidoscopePoster";
static char policy[2048],jobs[200];
volatile int sheng_watchdog_policy_status=-1;

// sheng-setup=1 or sheng-data-reset=2 (the install entry's "show Hello" choice): remove Setup's completion flags once, so this boot shows the Hello page.
// iPadOS writes them again when Setup finishes, so later boots do not show it. Absent files are fine.
static int sheng_boot_setup_reset(proc_t p,uint64_t scratch) {
 static const char *const files[]={
  "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist",
  "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist"};
 for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i) {
  int32_t result=0;
  int error=copyout(files[i],scratch,strlen(files[i])+1);
  if(error)return error;
  struct unlink_args rm={.path=scratch};
  error=unlink(p,&rm,&result);
  if(error && error!=ENOENT)return error;
 }
 return 0;
}

/* The desktop's wallpaper is not re-read from the sealed .ca on every boot: PosterBoard keeps the chosen poster and a rendered
 * snapshot in its own container and reuses them. This board's Data volume has been carried since before the wallpaper overlay
 * existed, so a snapshot taken while the images could not be decoded outlives every later change to them. fix142 is that state:
 * a solid red 2732x2732 J120.png was written into the mounted .ca (A files=15 wr=2, mount3=5/0) and the picture did not change.
 * sheng-wallpaper-reset=1 removes the stored configuration once, so this boot builds it again from the collection and the overlay.
 * [0] is how many candidates there are; [i+1] records the unlink of candidate i: 0 removed, ENOENT (2) absent, else the errno. */
volatile int32_t sheng_wallpaper_reset[8] = {0};
// The container scan's own reading: [0] vnode_lookup result, [1] last VNOP_READDIR result, [2] directory entries walked, [3] candidate
// containers that matched. A scan that unlinks nothing is one of these numbers, and the row below says which.
volatile int32_t sheng_wallpaper_scan[4] = {0};
// The container the last walk matched. The configuration read builds its path from this instead of
// a UUID baked into the source, which read ENOENT once the container had been recreated.
char sheng_wallpaper_container[192];
// sheng-wp=<1|2>: the entry names under the directories that can hold a chosen poster. 1 is the
// Data container, 2 the system volume's wallpaper collections. The name is short because the
// merged command line is capped at 1000 bytes and this argument competes with the board's own.
// The plist in Library/Preferences turned out to be CoreAnimation settings, so the next thing to
// look at is the directory listing rather than another guessed file.
volatile int32_t sheng_wallpaper_list_ran = 0;
char sheng_wallpaper_names[256];
// sheng-wallpaper-db=1: the printable runs in the first page of the poster store's SQLite file.
// SQLite keeps its schema in page 1, so the CREATE TABLE names come out without a SQLite reader.
volatile int32_t sheng_wallpaper_db_ran = 0;
char sheng_wallpaper_db[400];
extern vfs_context_t vfs_context_kernel(void);

static int sheng_unlink_one(proc_t p, uint64_t scratch, const char *file) {
 int32_t result = 0;
 int error = copyout(file, scratch, strlen(file) + 1);
 if (error) return error;
 struct unlink_args rm = {.path = scratch};
 error = unlink(p, &rm, &result);
 return error ? error : 0;
}

// PosterBoard's store is not at a path this board can assume: the container UUID changes with the Data
// volume, and the file that names the chosen poster is not one of the three fixed preferences. So find
// the container by its own metadata, which names the identifier in every one, then walk that container
// and remove the files that carry the selection by BASENAME. Measured on fix173: the Application
// directory answers (157 entries, 157 directories) but all three fixed paths were ENOENT, so the
// selection lives somewhere else in that container and only a walk finds it.
static const char *const sheng_wallpaper_targets[] = {
 "com.apple.PosterBoard.plist", "SelectedConfigurationIdentifier.plist", "com.apple.PaperBoard.plist"};
static char sheng_scan_path[512];
static char sheng_scan_buf[4096];
volatile int32_t sheng_wallpaper_unlinked[8];
volatile unsigned sheng_wallpaper_walked;

/* Read one 4096-byte page of a file at a byte offset, through lseek + read. The wallpaper database
 * keeps its rows in pages after the first, and the schema-only read this probe started with never
 * reached them. */
static int sheng_read_page(proc_t p, uint64_t scratch, const char *path, uint64_t offset, char *buf, unsigned cap) {
 if (copyout(path, scratch, strlen(path) + 1)) return -1;
 struct open_args op = {.path = scratch, .flags = O_RDONLY};
 int32_t result = 0;
 int error = open(p, &op, &result);
 if (error) return -error;
 const int32_t fd = result;
 struct lseek_args ls = {.fd = fd, .offset = (off_t)offset, .whence = 0};
 off_t where = 0;
 error = lseek(p, &ls, &where);
 if (error) {struct close_args cl = {.fd = fd}; int32_t r = 0; (void)sys_close(p, &cl, &r); return -error;}
 user_ssize_t got = 0;
 struct read_args rd = {.fd = fd, .cbuf = scratch, .nbyte = cap};
 error = read(p, &rd, &got);
 struct close_args cl = {.fd = fd};
 int32_t close_result = 0;
 (void)sys_close(p, &cl, &close_result);
 if (error) return -error;
 if (got < 0) return -1;
 if ((unsigned)got > cap) got = (user_ssize_t)cap;
 if (copyin(scratch, buf, (size_t)got)) return -1;
 return (int)got;
}

static int sheng_read_head(proc_t p, uint64_t scratch, const char *path, char *buf, unsigned cap) {
 if (copyout(path, scratch, strlen(path) + 1)) return -1;
 struct open_args op = {.path = scratch, .flags = O_RDONLY};
 int32_t result = 0;
 int error = open(p, &op, &result);
 if (error) return -error;
 const int32_t fd = result;
 user_ssize_t got = 0;
 struct read_args rd = {.fd = fd, .cbuf = scratch, .nbyte = cap};
 error = read(p, &rd, &got);
 struct close_args cl = {.fd = fd};
 int32_t close_result = 0;
 (void)sys_close(p, &cl, &close_result);
 if (error) return -error;
 if (got < 0) return -1;
 if ((unsigned)got > cap) got = (user_ssize_t)cap;
 if (copyin(scratch, buf, (size_t)got)) return -1;
 return (int)got;
}

static int sheng_has_text(const char *hay, int len, const char *needle) {
 const int n = (int)strlen(needle);
 for (int i = 0; i + n <= len; ++i) if (!memcmp(hay + i, needle, (size_t)n)) return 1;
 return 0;
}

// Unlink every file below root whose basename is one of the targets. Depth-bounded and counted: the row
// says how many the walk removed, so an empty walk is a reading and not a silent no-op.
static void sheng_wallpaper_walk(proc_t p, uint64_t scratch, const char *root, unsigned depth, volatile unsigned *found) {
 vfs_context_t ctx = vfs_context_kernel();
 vnode_t dir = NULLVP;
 if (vnode_lookup(root, 0, &dir, ctx)) return;
 if (vnode_vtype(dir) != VDIR) {vnode_put(dir); return;}
 off_t offset = 0;
 for (int eof = 0; !eof;) {
  uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
  if (!uio) break;
  uio_addiov(uio, CAST_USER_ADDR_T(sheng_scan_buf), sizeof sheng_scan_buf);
  int nentries = 0;
  const int error = VNOP_READDIR(dir, uio, 0, &eof, &nentries, ctx);
  const size_t got = sizeof sheng_scan_buf - (size_t)uio_resid(uio);
  offset = uio_offset(uio);
  uio_free(uio);
  if (error || !got) break;
  for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got;) {
   const struct dirent *d = (const struct dirent *)(sheng_scan_buf + at);
   if (!d->d_reclen) {eof = 1; break;}
   at += d->d_reclen;
   if (!d->d_ino || (d->d_namlen == 1 && d->d_name[0] == '.') ||
       (d->d_namlen == 2 && d->d_name[0] == '.' && d->d_name[1] == '.')) continue;
   if ((size_t)snprintf(sheng_scan_path, sizeof sheng_scan_path, "%s/%.*s", root, (int)d->d_namlen, d->d_name) >= sizeof sheng_scan_path) continue;
   if (d->d_type == DT_DIR && depth > 1) {sheng_wallpaper_walk(p, scratch, sheng_scan_path, depth - 1, found); continue;}
   if (d->d_type != DT_REG) continue;
   for (unsigned t = 0; t < sizeof sheng_wallpaper_targets / sizeof sheng_wallpaper_targets[0]; ++t) {
    const size_t n = strlen(sheng_wallpaper_targets[t]);
    if ((size_t)d->d_namlen != n || memcmp(d->d_name, sheng_wallpaper_targets[t], n)) continue;
    if (sheng_unlink_one(p, scratch, sheng_scan_path) == 0 && *found < 8) sheng_wallpaper_unlinked[(*found)++] = (int32_t)d->d_namlen;
    break;
   }
  }
 }
 vnode_put(dir);
}

// Every container below containers_root whose metadata plist names com.apple.PosterBoard, walked for the
// selection files. Returns how many containers matched.
static unsigned sheng_wallpaper_containers(proc_t p, uint64_t scratch, const char *root) {
 vfs_context_t ctx = vfs_context_kernel();
 vnode_t dir = NULLVP;
 if (vnode_lookup(root, 0, &dir, ctx)) return 0;
 if (vnode_vtype(dir) != VDIR) {vnode_put(dir); return 0;}
 unsigned matched = 0;
 off_t offset = 0;
 for (int eof = 0; !eof;) {
  uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
  if (!uio) break;
  uio_addiov(uio, CAST_USER_ADDR_T(sheng_scan_buf), sizeof sheng_scan_buf);
  int nentries = 0;
  const int error = VNOP_READDIR(dir, uio, 0, &eof, &nentries, ctx);
  const size_t got = sizeof sheng_scan_buf - (size_t)uio_resid(uio);
  offset = uio_offset(uio);
  uio_free(uio);
  if (error || !got) break;
  for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got;) {
   const struct dirent *d = (const struct dirent *)(sheng_scan_buf + at);
   if (!d->d_reclen) {eof = 1; break;}
   at += d->d_reclen;
   if (!d->d_ino || (d->d_namlen == 1 && d->d_name[0] == '.') ||
       (d->d_namlen == 2 && d->d_name[0] == '.' && d->d_name[1] == '.')) continue;
   if (d->d_type != DT_DIR) continue;
   if ((size_t)snprintf(sheng_scan_path, sizeof sheng_scan_path, "%s/%.*s", root, (int)d->d_namlen, d->d_name) >= sizeof sheng_scan_path) continue;
   static const char meta[] = "/.com.apple.mobile_container_manager.metadata.plist";
   if (strlen(sheng_scan_path) + sizeof meta >= sizeof sheng_scan_path) continue;
   char container[512];
   strlcpy(container, sheng_scan_path, sizeof container);
   strlcat(sheng_scan_path, meta, sizeof sheng_scan_path);
   char head[2048];
   const int n = sheng_read_head(p, scratch, sheng_scan_path, head, sizeof head);
   if (n <= 0 || !sheng_has_text(head, n, "com.apple.PosterBoard")) continue;
   ++matched;
   strlcpy(sheng_wallpaper_container, container, sizeof sheng_wallpaper_container);
   sheng_wallpaper_walk(p, scratch, container, 5, &sheng_wallpaper_walked);
  }
 }
 vnode_put(dir);
 return matched;
}

static int sheng_wallpaper_reset_files(proc_t p, uint64_t scratch) {
 int count = 0;
 sheng_wallpaper_reset[1 + count] = sheng_unlink_one(p, scratch, "/private/var/mobile/Library/Preferences/com.apple.PosterBoard.plist");
 count++;
 sheng_wallpaper_reset[1 + count] = sheng_unlink_one(p, scratch, "/private/var/mobile/Library/Preferences/com.apple.PaperBoard.plist");
 count++;
 sheng_wallpaper_reset[1 + count] = sheng_unlink_one(p, scratch, "/private/var/mobile/Library/Preferences/com.apple.paperboard.plist");
 count++;

 vfs_context_t ctx = vfs_context_kernel();
 vnode_t dir = NULLVP;
 int lookup = vnode_lookup("/private/var/mobile/Containers/Data/Application", 0, &dir, ctx);
 sheng_wallpaper_scan[0] = lookup;
 if (!lookup && vnode_vtype(dir) == VDIR) {
  {
   char *buffer = (char *)IOMallocData(4096);
   if (buffer) {
    off_t offset = 0;
    for (int eof = 0; !eof;) {
     uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
     if (!uio) break;
     uio_addiov(uio, CAST_USER_ADDR_T(buffer), 4096);
     int nentries = 0;
     int error = VNOP_READDIR(dir, uio, 0, &eof, &nentries, ctx);
     const size_t got = 4096 - (size_t)uio_resid(uio);
     offset = uio_offset(uio);
     uio_free(uio);
     sheng_wallpaper_scan[1] = error;
     if (error || !got) break;
     for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got;) {
      const struct dirent *d = (const struct dirent *)(buffer + at);
      if (!d->d_reclen) { eof = 1; break; }
      at += d->d_reclen;
      if (!d->d_ino || (d->d_namlen == 1 && d->d_name[0] == '.') || (d->d_namlen == 2 && d->d_name[0] == '.' && d->d_name[1] == '.')) continue;
      sheng_wallpaper_scan[2] = sheng_wallpaper_scan[2] + 1;
      if (d->d_type == DT_DIR && d->d_namlen > 10) {
       sheng_wallpaper_scan[3] = sheng_wallpaper_scan[3] + 1;
       char target[256];
       snprintf(target, sizeof target, "/private/var/mobile/Containers/Data/Application/%.*s/Library/Preferences/com.apple.PosterBoard.plist", (int)d->d_namlen, d->d_name);
       int rc = sheng_unlink_one(p, scratch, target);
       if (rc != ENOENT && count < 6) sheng_wallpaper_reset[1 + count++] = rc;
       snprintf(target, sizeof target, "/private/var/mobile/Containers/Data/Application/%.*s/Library/Application Support/PRBPosterExtensionDataStore/SelectedConfigurationIdentifier.plist", (int)d->d_namlen, d->d_name);
       rc = sheng_unlink_one(p, scratch, target);
       if (rc != ENOENT && count < 6) sheng_wallpaper_reset[1 + count++] = rc;
       snprintf(target, sizeof target, "/private/var/mobile/Containers/Data/Application/%.*s/Library/Application Support/PosterBoard/SelectedConfigurationIdentifier.plist", (int)d->d_namlen, d->d_name);
       rc = sheng_unlink_one(p, scratch, target);
       if (rc != ENOENT && count < 6) sheng_wallpaper_reset[1 + count++] = rc;
      }
     }
    }
    IOFreeData(buffer, 4096);
   }
  }
  vnode_put(dir);
 }
 // The paths above are guesses and measured ENOENT on fix173. Find PosterBoard's container by the identifier
 // its metadata plist carries, then walk that container for the files that name the chosen poster.
 sheng_wallpaper_scan[0] = (int32_t)sheng_wallpaper_containers(p, scratch, "/private/var/mobile/Containers/Data/Application");
 sheng_wallpaper_scan[1] = (int32_t)(sheng_wallpaper_containers(p, scratch, "/private/var/mobile/Containers/Shared/AppGroup") + 0x100);
 sheng_wallpaper_reset[0] = count;
 return 0;
}

/* Which wallpaper the desktop actually picks. PosterBoard stores its choice in this plist, so its bytes name the collection and
 * the variant without any guesswork. [0] status (0 read, else the errno, -1 no open), [1] bytes read, [2..] the bytes, four to a
 * word, little endian as they sit in the file. A binary plist keeps its strings verbatim, so the identifier is readable in the
 * dump. sheng-wallpaper-probe=1. */
volatile int32_t sheng_wallpaper_cfg[2 + 128] = {0};
// 0 until sheng_wallpaper_read_cfg is entered, so a probe that never ran and one that read nothing
// stop looking the same on the console. The path it opens carries a container UUID that changes
// when the app's container is recreated, which is a silent failure otherwise.
volatile int32_t sheng_wallpaper_probe_ran = 0;
// sheng-wallpaper-scan=1: how many containers carry PosterBoard in their metadata plist, walked from
// the two container roots. The configuration probe opens a path with a container UUID baked in and now
// reads ENOENT, so the first question is whether a PosterBoard container exists at all.
volatile int32_t sheng_wallpaper_scan_ran = 0;
volatile int32_t sheng_wallpaper_scan_app = -1;
volatile int32_t sheng_wallpaper_scan_group = -1;
// Entry names under one directory, appended to sheng_wallpaper_names with '|' between them.
static void sheng_wallpaper_list(proc_t p, uint64_t scratch, const char *root, char tag) {
 (void)p; (void)scratch;
 vfs_context_t ctx = vfs_context_kernel();
 vnode_t dir = NULLVP;
 if (vnode_lookup(root, 0, &dir, ctx)) return;
 if (vnode_vtype(dir) != VDIR) {vnode_put(dir); return;}
 off_t offset = 0;
 for (int eof = 0; !eof;) {
  uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
  if (!uio) break;
  uio_addiov(uio, CAST_USER_ADDR_T(sheng_scan_buf), sizeof sheng_scan_buf);
  int nentries = 0;
  const int error = VNOP_READDIR(dir, uio, 0, &eof, &nentries, ctx);
  const size_t got = sizeof sheng_scan_buf - (size_t)uio_resid(uio);
  offset = uio_offset(uio);
  uio_free(uio);
  if (error || !got) break;
  for (size_t at = 0; at + offsetof(struct dirent, d_name) <= got;) {
   const struct dirent *d = (const struct dirent *)(sheng_scan_buf + at);
   if (!d->d_reclen) {eof = 1; break;}
   at += d->d_reclen;
   if (!d->d_ino || (d->d_namlen == 1 && d->d_name[0] == '.') ||
       (d->d_namlen == 2 && d->d_name[0] == '.' && d->d_name[1] == '.')) continue;
   size_t used = strlen(sheng_wallpaper_names);
   if (used + 3 >= sizeof sheng_wallpaper_names) {vnode_put(dir); return;}
   if (used) sheng_wallpaper_names[used++] = '|';
   sheng_wallpaper_names[used++] = tag;
   sheng_wallpaper_names[used++] = ':';
   if (used + (size_t)d->d_namlen + 2 >= sizeof sheng_wallpaper_names) {vnode_put(dir); return;}
   memcpy(sheng_wallpaper_names + used, d->d_name, d->d_namlen);
   sheng_wallpaper_names[used + d->d_namlen] = 0;
  }
 }
 vnode_put(dir);
}

static int sheng_wallpaper_read_cfg(proc_t p, uint64_t scratch) {
 sheng_wallpaper_probe_ran=1;
 // The container id is asked for, not assumed: the one written here before read ENOENT on the
 // 2026-11-06 image while a container naming PosterBoard did exist (wallpaper scan app=1).
 if(!sheng_wallpaper_container[0])
  (void)sheng_wallpaper_containers(p,scratch,"/private/var/mobile/Containers/Data/Application");
 char path[256]="";
 if(sheng_wallpaper_container[0])
  snprintf(path,sizeof path,"%s/Library/Preferences/com.apple.PosterBoard.plist",sheng_wallpaper_container);
 if(!path[0]) {sheng_wallpaper_cfg[0]=ENOENT;sheng_wallpaper_cfg[1]=0;return 0;}
 int error=copyout(path,scratch,strlen(path)+1);
 if(error)return error;
 int32_t fd=-1,result=0;
 struct open_args op={.path=scratch,.flags=O_RDONLY};
 error=open(p,&op,&result);
 if(error) {sheng_wallpaper_cfg[0]=error;return 0;}
 fd=result;
 user_ssize_t got_ss=0;
 struct read_args rd={.fd=fd,.cbuf=scratch,.nbyte=512};
 error=read(p,&rd,&got_ss);
 struct close_args cl={.fd=fd};
 (void)sys_close(p,&cl,&result);
 if(error) {sheng_wallpaper_cfg[0]=error;return 0;}
 const int32_t got=(int32_t)got_ss;
 if(got<0) {sheng_wallpaper_cfg[0]=got;return 0;}
 if(got>512) {sheng_wallpaper_cfg[0]=-2;return 0;}
 unsigned char local[512];
 error=copyin(scratch,local,sizeof local);
 if(error)return error;
 sheng_wallpaper_cfg[0]=0;
 sheng_wallpaper_cfg[1]=got;
 for(int32_t i=0;i<got;++i)sheng_wallpaper_cfg[2+i/4]|=(int32_t)((uint32_t)local[i]<<((i%4)*8));
 return 0;
}

// One comma-separated label at *c into the policy as a disabled key; advances *c past it.
static int append_job(const char **cursor) {
 const char *c=*cursor,*e=c;while(*e && *e!=',')++e;
 if(e>c) {
  if(strlcat(policy,"<key>",sizeof(policy))>=sizeof(policy))return EINVAL;
  size_t at=strlen(policy);if(at+(size_t)(e-c)+1>=sizeof(policy))return EINVAL;
  memcpy(policy+at,c,(size_t)(e-c));policy[at+(size_t)(e-c)]=0;
  if(strlcat(policy,"</key><true/>",sizeof(policy))>=sizeof(policy))return EINVAL;
 }
 *cursor=*e?e+1:e;return 0;
}

int sheng_boot_watchdog_policy(void *process, uint64_t scratch) {
 proc_t p=process;
 int enabled=0,error=0,fd=-1;int32_t result=0;
 PE_parse_boot_argn("sheng-disable-watchdogd",&enabled,sizeof(enabled));
 if(!PE_parse_boot_argn("sheng-disable-jobs",jobs,sizeof(jobs)))jobs[0]=0;jobs[sizeof(jobs)-1]=0;
 int lite=0;PE_parse_boot_argn("sheng-lite",&lite,sizeof(lite));if(lite!=1)lite=0;
 if(enabled!=1)enabled=0;
 for(const char *c=jobs;*c;++c)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='-'||*c==','))return EINVAL;
 int setup=0,reset=0;
 PE_parse_boot_argn("sheng-setup",&setup,sizeof(setup));
 PE_parse_boot_argn("sheng-data-reset",&reset,sizeof(reset));   // 2 = erase Data and show the Hello page; 1 = erase Data, Setup stays skipped
 if(reset==2)setup=1;
 if(setup==1 && (error=sheng_boot_setup_reset(p,scratch))) {
  sheng_watchdog_policy_status=error;
  printf("sheng setup flags reset failed: %d\n",error);
  return error;
 }
 if(setup==1)printf("sheng setup flags reset: Hello page on this boot\n");
 {
  int wallpaper=0;
  PE_parse_boot_argn("sheng-wallpaper-reset",&wallpaper,sizeof(wallpaper));
  if(wallpaper==1) {
   if((error=sheng_wallpaper_reset_files(p,scratch))) sheng_watchdog_policy_status=error;
   else printf("sheng wallpaper configuration reset\n");
  }
  int wscan=0;
  PE_parse_boot_argn("sheng-wallpaper-scan",&wscan,sizeof(wscan));
  // This function runs on every exec, and the walk reads a metadata plist in every container under
  // two roots, so it cannot run every time. It also cannot run only once: the first exec is far too
  // early for PosterBoard to have a container. Every two hundredth exec is late enough to see one and
  // cheap enough to repeat, and the row prints whatever the last scan found.
  static unsigned scan_tick;
  if(wscan==1 && (scan_tick++ % 200)==0) {
   sheng_wallpaper_scan_ran=1;
   sheng_wallpaper_scan_app=(int32_t)sheng_wallpaper_containers(p,scratch,"/private/var/mobile/Containers/Data/Application");
   sheng_wallpaper_scan_group=(int32_t)sheng_wallpaper_containers(p,scratch,"/private/var/mobile/Containers/Shared/AppGroup");
  }
  int wdb=0;
  PE_parse_boot_argn("sheng-wallpaper-db",&wdb,sizeof(wdb));
  if(wdb==1 && !sheng_wallpaper_db_ran) {
   sheng_wallpaper_db_ran=1;
   if(!sheng_wallpaper_container[0])
    (void)sheng_wallpaper_containers(p,scratch,"/private/var/mobile/Containers/Data/Application");
   if(sheng_wallpaper_container[0]) {
    char path[320], head[2048];
    snprintf(path,sizeof path,"%s/Library/Application Support/PRBPosterExtensionDataStore/61/PBFPosterExtensionDataStoreSQLiteDatabase.sqlite3",sheng_wallpaper_container);
    const int n=sheng_read_head(p,scratch,path,head,sizeof head);
    if(n>0) {
     size_t used=0; unsigned run=0;
     for(int i=0;i<n;++i) {
      const unsigned char c=(unsigned char)head[i];
      if(c>=0x20 && c<0x7f) {if(run+1<sizeof head) head[run++]= (char)c; continue;}
      if(run>=4 && used+run+2<sizeof sheng_wallpaper_db) {
       if(used) sheng_wallpaper_db[used++]='|';
       memcpy(sheng_wallpaper_db+used,head,run); used+=run; sheng_wallpaper_db[used]=0;
      }
      run=0;
     }
     /* The schema names the tables but not the rows, and the selection this port needs is a row.
      * Reading pages and pulling out their text runs is enough: a provider name and a wallpaper
      * identifier are ASCII, and the first pages hold both the schema and the first table leaves.
      * A full b-tree walk is not needed to answer "did PosterBoard ever store a selection". */
     {
      unsigned pageSize = 4096;
      if (n >= 18) {unsigned ps = ((unsigned char)head[16] << 8) | (unsigned char)head[17]; if (ps >= 512 && ps <= 65536) pageSize = ps;}
      char page[4096], run[256];
      for (unsigned pg = 1; pg <= 3; ++pg) {
       const int m = sheng_read_page(p, scratch, path, (uint64_t)pg * pageSize, page, sizeof page);
       if (m <= 0) break;
       unsigned rrun = 0, kept = 0;
       for (int i = 0; i < m; ++i) {
        const unsigned char c = (unsigned char)page[i];
        if (c >= 0x20 && c < 0x7f) {if (rrun + 1 < sizeof run) run[rrun++] = (char)c; continue;}
        /* Rows carry long binary runs; keep only text long enough to be a name or an identifier.
         * The run goes to its own buffer: writing it back over page[] would overwrite the bytes
         * this loop is still reading. */
        if (rrun >= 6 && kept < 6 && used + rrun + 3 < sizeof sheng_wallpaper_db) {
         sheng_wallpaper_db[used++] = '|';
         memcpy(sheng_wallpaper_db + used, run, rrun); used += rrun; sheng_wallpaper_db[used] = 0;
         ++kept;
        }
        rrun = 0;
       }
      }
     }
    }
   }
  }
  int wlist=0;
  PE_parse_boot_argn("sheng-wp",&wlist,sizeof(wlist));
  if(wlist==2 && !sheng_wallpaper_list_ran) {
   /* The system volume, not the Data container: which wallpaper collections and which variants
    * this image really carries. The overlay mount targets a path inside one of these. */
   sheng_wallpaper_list_ran=1;
   sheng_wallpaper_list(p,scratch,"/Library/Wallpaper/Collections",'C');
   sheng_wallpaper_list(p,scratch,"/Library/Wallpaper/Collections/iPadOS_17~ipad.wallpaperCollection/Wallpapers",'W');
  }
  if(wlist==1 && !sheng_wallpaper_list_ran) {
   if(!sheng_wallpaper_container[0])
    (void)sheng_wallpaper_containers(p,scratch,"/private/var/mobile/Containers/Data/Application");
   if(sheng_wallpaper_container[0]) {
    char root[256];
    sheng_wallpaper_list_ran=1;
    snprintf(root,sizeof root,"%s/Library/Application Support",sheng_wallpaper_container);
    sheng_wallpaper_list(p,scratch,root,'A');
    snprintf(root,sizeof root,"%s/Library/Application Support/PRBPosterExtensionDataStore",sheng_wallpaper_container);
    sheng_wallpaper_list(p,scratch,root,'P');
    snprintf(root,sizeof root,"%s/Library/Application Support/ArchivedDataStores",sheng_wallpaper_container);
    sheng_wallpaper_list(p,scratch,root,'R');
    snprintf(root,sizeof root,"%s/Library/Application Support/PRBPosterExtensionDataStore/61",sheng_wallpaper_container);
    sheng_wallpaper_list(p,scratch,root,'S');
   }
  }
  int wprobe=0;
  PE_parse_boot_argn("sheng-wallpaper-probe",&wprobe,sizeof(wprobe));
  if(wprobe==1) {
   if((error=sheng_wallpaper_read_cfg(p,scratch))) sheng_watchdog_policy_status=error;
  }
 }
 if(!enabled && !jobs[0] && !lite)return 0;
 policy[0]=0;strlcat(policy,policy_head,sizeof(policy));
 if(enabled)strlcat(policy,policy_watchdogd,sizeof(policy));
 if(lite)for(const char *c=lite_jobs;*c;) {if((error=append_job(&c)))return error;}
 {int posters=0;PE_parse_boot_argn("sheng-wallpaper-posters",&posters,sizeof posters);
  if(lite && posters!=1)for(const char *c=lite_posters;*c;) {if((error=append_job(&c)))return error;}}
 for(const char *c=jobs;*c;) {if((error=append_job(&c)))return error;}
 strlcat(policy,policy_tail,sizeof(policy));
 const char *directory="/private/var/db/com.apple.xpc.launchd";
 const char *path="/private/var/db/com.apple.xpc.launchd/disabled.plist";
 if((error=copyout(directory,scratch,strlen(directory)+1)))goto done;
 struct mkdir_args md={.path=scratch,.mode=0755};
 error=mkdir(p,&md,&result);
 if(error && error!=EEXIST)goto done;
 if((error=copyout(path,scratch,strlen(path)+1)))goto done;
 // With persistent Data the previous boot's file is there: this policy is rewritten in place on every boot (it must follow the
 // boot arguments). Symlinks are still refused (O_NOFOLLOW) and the fstat check below insists on a root-owned regular file.
 struct open_args op={.path=scratch,.flags=O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,.mode=0644};
 if((error=open(p,&op,&result)))goto done;
 fd=result;
 // The scratch page keeps the cache-mount environment slots at +0x400, so the policy (the lite list makes it longer than 0x200 bytes) goes
 // out in pieces through the 0x200 bytes at +0x200.
 for(size_t sent=0,total=strlen(policy);sent<total;) {
  size_t piece=total-sent<0x1f0?total-sent:0x1f0;
  if((error=copyout(policy+sent,scratch+0x200,piece)))goto done;
  struct write_args wr={.fd=fd,.cbuf=scratch+0x200,.nbyte=piece};
  user_ssize_t written=0;
  if((error=write(p,&wr,&written)))goto done;
  if(written!=(user_ssize_t)piece){error=EIO;goto done;}
  sent+=piece;
 }
 struct fchown_args owner={.fd=fd,.uid=0,.gid=0};
 if((error=fchown(p,&owner,&result)))goto done;
 struct fchmod_args mode={.fd=fd,.mode=0644};
 if((error=fchmod(p,&mode,&result)))goto done;
 struct fsync_args sync={.fd=fd};
 if((error=fsync(p,&sync,&result)))goto done;
 // Preserve the cache-mount environment slots at +0x400, +0x500, +0x600.
 struct fstat64_args sa={.fd=fd,.ub=scratch+0x800};struct stat64 st={0};
 if((error=sys_fstat64(p,&sa,&result)) || (error=copyin(sa.ub,&st,sizeof(st))))goto done;
 if(st.st_uid!=0 || st.st_gid!=0 || !S_ISREG(st.st_mode) || (st.st_mode&0777)!=0644 || st.st_size!=(off_t)(strlen(policy)))error=EINVAL;
 done:
 if(fd>=0){struct close_args cl={.fd=fd};int e=sys_close(p,&cl,&result);if(!error)error=e;}
 // A real empty migration marker selects the qualified modern override loader.
 if(!error) {
  const char *marker="/private/var/db/com.apple.xpc.launchd/disabled.migrated";
  error=copyout(marker,scratch,strlen(marker)+1);
  if(!error) {
   struct open_args mark={.path=scratch,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=0644};
   error=open(p,&mark,&result);
   if(!error){struct close_args cl={.fd=result};error=sys_close(p,&cl,&result);}
   else if(error==EEXIST)error=0;
  }
 }
 sheng_watchdog_policy_status=error;
 printf("sheng bring-up watchdogd launch policy: %d (hardware watchdog not implemented)\n",error);
 return error;
}
