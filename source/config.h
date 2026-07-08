/* config.h -- VERY LITTLE NIGHTMARES Switch wrapper configuration
 * (forked from the ZOOKEEPER DX port, itself from cr3_nx / max_nx).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// The engine + libc++ + il2cpp managed heap need a generous newlib heap; the
// rest of system memory is handed to the .so loader (see __libnx_initheap).
#define MEMORY_MB 768

// mmap arena. Unity reserves big aligned pools by over-mmapping then munmapping the
// unaligned head/tail; a plain malloc/free-per-mmap would corrupt the kept middle,
// so anonymous mmaps come from a dedicated aligned arena with a per-page used bitmap
// (carved in __libnx_initheap). MMAP_ARENA_ALIGN matches the 64MB region granule the
// nx_patch_unity_regions patch shrinks libunity to (256MB unpatched won't fit 4GB).
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)    // == patched region granularity
// Heap-backed arena cap (1792MB = 28x64MB blocks). The stack-region OC window takes
// most reservations lazily; this arena is the large-aligned backstop + non-region
// maps. On a 4GB Switch: so_zone(192) + arena(1792) leaves newlib ~1.1GB.
#define MMAP_ARENA_RESERVE  ((size_t)1792 * 1024 * 1024)  // heap-backed cap (28x64MB)

// Stack-region overcommit (OC) arena (see libc_shim.c). svcMapMemory can only
// alias into the ~2GB stack region, so we hold the big PROT_NONE reservations
// in a stack-region window and back the committed pages from a small heap pool.
#define OC_WINDOW_BYTES     ((size_t)1536 * 1024 * 1024)  // cheap PROT_NONE reservation in the stack hole (24x64MB)
#define OC_POOL_BYTES       ((size_t) 448 * 1024 * 1024)  // commit-pool: VLN commits ~204MB live + gameplay headroom

// Overcommit (alias-region) mode window (unused on title-override, kept for
// parity with the ZOOKEEPER base -- see overcommit_setup in main.c).
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)  // 6 GB virtual reservation window
#define OVERCOMMIT_HEAP_MB  608u

// --- CR3 leftovers (dead code inherited from the base; NOT used by Unity) ---
#define SO_NAME      "libcrx.so"
#define SO_CPP_NAME  "libc++_shared.so"
#define MAIN_MVGL    "main.10007.android.mvgl"

// --- VERY LITTLE NIGHTMARES identity (Bandai Namco, eu.bandainamcoent.*) ----
// Used by the JNI Context shim (getPackageName / getObbDir / versionCode) so
// the split-binary engine builds the right OBB path:
//   <obbDir>/main.<VLN_VERSION_CODE>.<VLN_PACKAGE>.obb
#define VLN_PACKAGE       "eu.bandainamcoent.verylittlenightmares"
#define VLN_VERSION_CODE  147          // APK versionCode == OBB "main.147.*"
#define VLN_VERSION_NAME  "1.2.6"
#define VLN_OBB_NAME      "main.147.eu.bandainamcoent.verylittlenightmares.obb"

#define CONFIG_NAME "config.txt"
#define LOG_NAME    "sdmc:/switch/vln_nx/debug.log"

// Game data root == the .nro's own folder, matching the SD convention used by
// the sibling SoLoader ports (cr3_nx/, swordigo/, ...): nro + libs + assets +
// OBB all live in sdmc:/switch/vln_nx/. Returned for getenv("HOME")/getpwuid()
// and the managed data path.
#define GAME_HOME   "sdmc:/switch/vln_nx"

// flip to 1 to write debug.log (and enable the diag watchdog) for on-hardware debugging
#define DEBUG_LOG 0

extern int screen_width;
extern int screen_height;

// Language. 0 = follow the Switch system language.
#define LANG_AUTO 0
#define LANG_JA   1
#define LANG_EN   2

typedef struct {
  int screen_width;
  int screen_height;
  int language;
  // Portrait rotation to fill the landscape panel:
  //   1 = rotate 90 CW (default, right Joy-Con up), 2 = rotate 90 CCW (left up).
  int portrait;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
