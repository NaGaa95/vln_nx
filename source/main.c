/* main.c -- VERY LITTLE NIGHTMARES Switch wrapper entry point.
 *
 * Unity 2021.3.45f2 / IL2CPP. Loads libmain + libunity + libil2cpp, then drives
 * the lifecycle the Java UnityPlayer normally runs (JNI_OnLoad -> initJni ->
 * recreate GFX state -> surface changed -> resume/focus -> render loop) via the
 * native entry points from unity_entrypoints.h. The engine owns its own EGL/GLES3
 * context; SDL is audio/HID only. Forked from the ZOOKEEPER DX port.
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "diag.h"
#include "obb_stage.h"

#define DATA_ROOT  GAME_HOME          /* sdmc:/switch/vln */
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"

/* Force FMOD's native OpenSL output (type 22) instead of its Java AudioTrack output
 * (type 21, silent with no JVM): rewrite `mov w1,w21` to `movz w1,#22` at the
 * setOutput call site. Self-verifying (patched only if the original word matches).
 * Sites verified via the Unity 2021.3.45f2 Release symbols. */
#define VLN_FMOD_OPENSL_PATCH  1   /* audio ON: OpenSL output at 48kHz */
#define VLN_FMOD_SETOUTPUT_SITE 0x7a498c
#define VLN_FMOD_SETOUTPUT_FROM 0x2A1503E1u   /* mov w1, w21 (requested type) */
#define VLN_FMOD_BUFGEO_SITE    0             /* 0 = don't patch (VLN buffer likely sane) */

void unity_environment_init(const char *data_root);   /* unity_glue.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;
u64    g_alias_base = 0, g_alias_size = 0;
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;

so_module main_mod, unity_mod, il2cpp_mod;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* NATIVE ENGINE CLOCK FIX (the headline black-screen fix). The port drives the
 * player loop itself and never delivers Choreographer frame callbacks, so
 * TimeManager::Update's newTime is frozen, deltaTime collapses to the 1e-5 clamp,
 * and async scene loads never integrate. Fix: hook Update's entry, replay its
 * prologue, and re-enter the frameless body with newTime = startupRef + wallclock
 * so realtimeSinceStartup advances at wall rate. Offsets in unity_entrypoints.h. */
static void (*g_unity_update_body)(void *, double) = NULL; /* 0x4410f8 Update body */
static uint64_t g_clk_base_ns = 0;
static void   *g_tm = NULL;          /* captured TimeManager instance (for the clock thread) */
static Mutex   g_clock_lock;         /* serialize main-hook vs background clock-thread ticks */
/* Unity's vsync/frame-presented counter (libunity global @0x19059a0). No real
 * vsync here, so the clock thread bumps it to unblock frame-pacing waits. */
static volatile uint64_t *g_vsync_counter = NULL;
/* Wall-clock ns of the last main-thread Update hook; the clock thread drives the
 * engine clock only when this goes stale (main thread parked in a scene-load job). */
static volatile uint64_t g_last_main_tick_ns = 0;
#define CLOCK_STALL_NS 100000000ULL   /* 100ms of main-thread silence => treat as stalled */
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* advance the engine clock by re-running Update's body with a wall-clock newTime.
 * Shared by the main-thread Update hook and the background clock thread. */
static void nx_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double wall   = (double)(now - g_clk_base_ns) / 1e9;       /* seconds since first tick */
  double sref   = *(volatile double *)((char *)tm + 0xe8);   /* m_StartupRef (set at init) */
  double newTime = sref + wall;                              /* body: elapsed = newTime-sref = wall */
  if (g_unity_update_body) g_unity_update_body(tm, newTime);
}
static void nx_time_update_hook(void *tm) {
  g_tm = tm;                                                 /* capture for the clock thread */
  g_last_main_tick_ns = nx_now_ns();                         /* main thread is driving the clock */
  *(volatile uint64_t *)((char *)tm + 0xc8) += 1;            /* frameCount++ (prologue) */
  *(volatile uint32_t *)((char *)tm + 0xd0) += 1;            /* aux counter++           */
  if (*(volatile uint8_t *)((char *)tm + 0xf8) != 0) return; /* paused -> early return  */
  mutexLock(&g_clock_lock);
  nx_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}
/* Keep the engine clock ticking from an independent thread while nativeRender is
 * parked in a synchronous scene-load job (else the clock freezes -> black screen).
 * trylock so we never block the main hook or invert lock order. */
static Thread g_clock_thr;
static void nx_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  while (!jni_quit_requested) {
    svcSleepThread(8000000ULL);                  /* ~8ms => ~120Hz clock keep-alive */
    /* simulate a vsync tick so libunity's frame-pacing waits advance (always) */
    if (g_vsync_counter) __atomic_add_fetch(g_vsync_counter, 1, __ATOMIC_RELAXED);
    /* Drive the clock here ONLY while the main thread is stalled; ticking during
     * normal play would fragment Time.deltaTime into slow motion. */
    void *tm = g_tm;
    if (tm && g_unity_update_body &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {
      nx_clock_tick(tm);
      mutexUnlock(&g_clock_lock);
    }
  }
}
static void nx_start_clock_thread(void) {
  if (R_SUCCEEDED(threadCreate(&g_clock_thr, nx_clock_thread, NULL, NULL, 0x8000, 0x2C, -2)))
    threadStart(&g_clock_thr);
}
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  g_vsync_counter = (volatile uint64_t *)(ub + 0x19059a0);   /* vsync/frame-presented counter */
  g_unity_update_body = (void (*)(void *, double))(ub + OFF_TimeManager_Update_body);
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  so_patch_code((void *)(ub + OFF_TimeManager_Update_entry), stub, sizeof stub);
  debugPrintf("[boot] installed TimeManager::Update hook @libunity+0x%x "
              "(newTime <- startupRef + wallclock)\n", OFF_TimeManager_Update_entry);
}

/* audio warmup gate for opensles.c (frames since boot) */
static volatile uint32_t g_frame_count = 0;
uint32_t port_frame_count(void) { return g_frame_count; }

/* libunity ~25M + libil2cpp ~42M + headroom for relocated segments */
#define SO_REGION_BYTES (192u * 1024 * 1024)

static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { best_l = he - hs; best_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Reserve a slice of address space for the .so images; the rest is the newlib
 * heap the engine mallocs from. (Verbatim from cr3_nx / ZOOKEEPER.) */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  /* Fallback: fully heap-backed aligned arena (no overcommit). */
  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 448 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    /* Cap the arena at 30% of usable heap so newlib (il2cpp/mono/GC malloc) keeps
     * the majority; on a 4GB Switch the fixed 1792MB arena would starve it into OOM.
     * No-op on 8GB (1792MB < 30%); on 4GB the arena drops to ~832MB. */
    size_t usable    = size - so_zone - big_align;
    size_t arena_cap = ((usable * 30) / 100) & ~(big_align - 1);
    if (arena_sz > arena_cap) arena_sz = arena_cap;
    fake_heap_size = size - so_zone - arena_sz - big_align;
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* Verify the essential data files exist (obb_stage() has already produced the merged
 * data.unity3d and consumed the OBB, so the OBB need not be present). */
static void check_data(void) {
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    "assets/bin/Data/data.unity3d",
  };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0)
      fatal_error("Missing data file:\n%s\nCheck your SD card layout (see BUILD.md).", files[i]);
  }
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  crx_resolve_imports(mod);
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;

/* Shrink Unity's memory-region granularity 256MB -> 64MB in-memory (after load,
 * before any allocator runs) so a 4GB Switch fits and the SD ships stock game data.
 * 21 immediate-operand sites; verify every original word first and patch nothing on
 * any mismatch, so a different libunity is left stock. Offsets: Unity 2021.3.45f2. */
static int nx_patch_unity_regions(uintptr_t ub) {
  static const struct { uint32_t off, from, to; } P[] = {
    {0x36046c, 0x12be0009, 0x12bf8009}, {0x360474, 0x92648d36, 0x92669536},
    {0x360d18, 0xd35cfc28, 0xd35afc28}, {0x360d1c, 0x52a20009, 0x52a08009},
    {0x3620cc, 0x52a20009, 0x52a08009}, {0x364484, 0xd35cfd29, 0xd35afd29},
    {0x364488, 0x52a2000a, 0x52a0800a}, {0x364924, 0x12be000a, 0x12bf800a},
    {0x36492c, 0x92648d36, 0x92669536}, {0x366c70, 0xd35cdc33, 0xd35ad433},
    {0x366c74, 0xd35cfd15, 0xd35afd15}, {0x366d74, 0x52a20008, 0x52a08008},
    {0x367278, 0xd35cfc28, 0xd35afc28}, {0x367288, 0x92646c28, 0x92667428},
    {0x367290, 0xd35c9c2a, 0xd35a942a}, {0x3672a4, 0xb25c6feb, 0xb25e77eb},
    {0x3672a8, 0xd35cdc29, 0xd35ad429}, {0x3672ac, 0xf2a2000b, 0xf2a0800b},
    {0x3672e8, 0xcb0a7108, 0xcb0a6908}, {0x367310, 0xd35c9c29, 0xd35a9429},
    {0x368fac, 0xd35c9e89, 0xd35a9689},
  };
  const int N = (int)(sizeof P / sizeof P[0]);
  for (int i = 0; i < N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + P[i].off);
    if (cur != P[i].from) {
      debugPrintf("[region] libunity mismatch @+0x%x: have 0x%08x want 0x%08x -> SKIP all (stock libunity, 256MB regions)\n",
                  (unsigned)P[i].off, cur, P[i].from);
      return 0;
    }
  }
  for (int i = 0; i < N; i++)
    so_patch_code((void *)(ub + P[i].off), &P[i].to, sizeof P[i].to);
  debugPrintf("[region] libunity memory granularity 256MB->64MB patched in-memory (%d sites)\n", N);
  return 1;
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  socketInitializeDefault();
  debugPrintf("[boot] === vln_nx start (Unity 2021.3.45f2, build v28 obb-onboard-extract) ===\n");

  /* chdir into DATA_ROOT: hbloader leaves cwd elsewhere and the engine reads many
   * files through relative paths ("assets/bin/..."). */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    char cwd2[256] = {0};
    getcwd(cwd2, sizeof cwd2);
    struct stat st;
    int reach_meta = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_obb  = stat(VLN_OBB_NAME, &st) == 0;
    debugPrintf("[boot] cwd was '%s' -> chdir(%s)=%d -> '%s'\n", cwd, DATA_ROOT, rc, cwd2);
    debugPrintf("[boot] reachable(rel): metadata=%d obb=%d\n", reach_meta, reach_obb);
  }

  /* Load config.txt (portrait rotation, language) from the game folder; create it
   * with defaults on first run so the options are discoverable/editable on the SD. */
  {
    const char *cfg = DATA_ROOT "/" CONFIG_NAME;
    int rc = read_config(cfg);
    if (rc != 0) write_config(cfg);   /* -1 missing or 1 stale keys -> (re)write */
    if (config.portrait != 2) config.portrait = 1;   /* only 1 (CW) or 2 (CCW) */
    debugPrintf("[cfg] portrait=%d (1=ROT90 default, 2=ROT270)\n", config.portrait);
  }

  /* Use complete loose assets directly, or stage the original split APK + OBB layout. */
  if (obb_stage(VLN_OBB_NAME) != 0)
    fatal_error("Game data not ready.\n\nPlace either complete extracted assets in:\n  %s/assets\n\nor the split-release OBB:\n  %s\n\nSee README.md for the required layout.", DATA_ROOT, VLN_OBB_NAME);

  /* Force libunity to re-extract il2cpp resources every boot: our shim never flushes
   * a writable file-backed mmap to disk, so a previously-extracted global-metadata.dat
   * lands truncated. Deleting the extraction markers makes libunity redo it. */
  {
    int a = unlink(DATA_ROOT "/il2cpp/unity.ver");
    int b = unlink(DATA_ROOT "/il2cpp/Metadata/global-metadata.dat");
    int c = unlink(DATA_ROOT "/il2cpp/Resources/mscorlib.dll-resources.dat");
    debugPrintf("[boot] force re-extract: unlink unity.ver=%d metadata=%d resources=%d\n", a, b, c);
  }

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem layout: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    debugPrintf("[boot] OVERCOMMIT off (heap-backed): system_resource=%u MB map_hint=%d alias=%u MB\n",
                (unsigned)(g_oc_sysres >> 20), g_oc_hint_map, g_oc_alias_mb);
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[boot] phys: total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20), (unsigned)((tot - used) >> 20));
  }

  /* Arm the stack-region overcommit arena (aliases heap pages into the stack region
   * for Unity's big PROT_NONE reservations). Any failure falls back to heap-backed. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      pool = memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES))
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p, heap-backed arena %u MB\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool,
                    (unsigned)(g_mmap_arena_size >> 20));
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole (win=%p sz=%u MB rv=%p) -> heap-backed only\n",
                  win, (unsigned)(winsz >> 20), (void *)rv);
    }
  }

  /* Portrait game: report a portrait surface (W<H) so the engine renders upright.
   * Handheld 720x1280 / docked 1080x1920 (see android_native_unity.c). */
  if (appletGetOperationMode() == AppletOperationMode_Console) { screen_width = 1080; screen_height = 1920; }
  else                                                         { screen_width = 720;  screen_height = 1280; }

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  check_data();

  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  debugPrintf("[boot] loaded libmain   @ virtbase %p\n", (void *)main_mod.load_virtbase);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  debugPrintf("[boot] loaded libunity  @ virtbase %p\n", (void *)unity_mod.load_virtbase);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  debugPrintf("[boot] loaded libil2cpp @ virtbase %p\n", (void *)il2cpp_mod.load_virtbase);
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed\n");

#if VLN_FMOD_OPENSL_PATCH
  /* Apply the FMOD OpenSL output patch before init runs (see VLN_FMOD_SETOUTPUT_SITE). */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *site = (volatile uint32_t *)(ub + VLN_FMOD_SETOUTPUT_SITE);
    if (*site == VLN_FMOD_SETOUTPUT_FROM) {
      uint32_t req_opensl = 0x528002C1u;    /* movz w1, #22 (FMOD_OUTPUTTYPE_OPENSL) */
      so_patch_code((void *)site, &req_opensl, sizeof req_opensl);
      debugPrintf("[fmod] output request forced to OpenSL(22) @libunity+0x%x\n",
                  (unsigned)VLN_FMOD_SETOUTPUT_SITE);
    }
  }
#endif

  /* Region-granularity 256MB->64MB in-memory before any allocator/init runs. */
  nx_patch_unity_regions((uintptr_t)unity_mod.load_virtbase);

  /* Main thread runs init_array + the engine lifecycle; give it its own stable
   * bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays done\n");

  /* fake JNI + our environment, then HID */
  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  /* resolve UnityPlayer natives (load_virtbase + recovered offsets) */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);
  debugPrintf("[boot] entry points resolved (initJni=%p render=%p inject=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender, (void *)Unity_nativeInjectEvent);

  install_bionic_tls(main_tls);

  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) first: it caches the JavaVM into
   * libunity's JNI manager, without which initJni's ScopedJNI gets a NULL env. */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    debugPrintf("[boot] calling libunity JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with il2cpp so managed AndroidJNI/AndroidJavaObject calls
   * resolve. We can't call libil2cpp's JNI_OnLoad (a GOT-indirect PLT call our loader
   * mis-binds -> Instruction Abort), so replicate its two essential global stores
   * directly: cache the VM and store the JNI-handler fn-ptr. */
  {
    uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
    *(void **)(b + 0x282d4e8) = fake_vm;                 /* JavaVM cache            */
    *(void **)(b + 0x282c678) = (void *)(b + 0x7acca4);  /* JNI handler fn pointer  */
    debugPrintf("[boot] il2cpp JavaVM global set (vm=%p, handler=%p)\n",
                fake_vm, (void *)(b + 0x7acca4));
  }

  debugPrintf("[boot] calling initJni...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  debugPrintf("[boot] initJni returned; nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  debugPrintf("[boot] gfx state created; sendSurfaceChanged...\n");
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface change sent; resuming + focusing player loop\n");

  /* The player loop only advances Update when the app is RESUMED and FOCUSED;
   * issue those transitions before the loop or it renders a frozen frame forever. */
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
  debugPrintf("[boot] resumed + focus=true\n");

  /* Install the GC-disable + clock fix BEFORE the first nativeRender: VLN allocates
   * on its first frame, triggering a Boehm GC that stops the world with POSIX signals
   * Switch never delivers, so nativeRender would never return. */
  {
    /* Disable the Boehm GC entirely (its signal-based stop-the-world is broken on
     * Switch); set_mode(DISABLED) also turns off the incremental collector. */
    typedef void (*fn_set_mode)(int);
    typedef void (*fn_void)(void);
    fn_set_mode il2cpp_gc_set_mode = (fn_set_mode)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    fn_void     il2cpp_gc_disable  = (fn_void)    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
    if (il2cpp_gc_set_mode) { il2cpp_gc_set_mode(1); debugPrintf("[boot] il2cpp_gc_set_mode(DISABLED)\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_set_mode not found\n");
    if (il2cpp_gc_disable)  { il2cpp_gc_disable();   debugPrintf("[boot] il2cpp_gc_disable() -> GC OFF\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_disable not found\n");

    /* Install the native engine-clock fix (see nx_install_time_fix). */
    nx_install_time_fix();

    /* Frame-2 black-screen fix: nativeRender's UnityChoreographer ctor blocks in a
     * spin-loop (libunity 0x6290fc..0x629118) waiting for a vsync-ready flag a Java
     * Choreographer would set. Let the object construct (singleton stays valid) but
     * skip the WAIT: patch the loop exit test at 0x629108 to an unconditional branch. */
    {
      uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
      uint32_t br = 0x14000005u;   /* b 0x62911c (was cbnz x8,0x62911c = 0xB50000A8) */
      so_patch_code((void *)(ub + 0x629108), &br, sizeof br);
      debugPrintf("[boot] Choreographer VSYNC wait skipped (libunity+0x629108 -> b 0x62911c)\n");
    }
  }
  /* keep the engine clock advancing while nativeRender blocks -- see nx_clock_thread. */
  nx_start_clock_thread();
  debugPrintf("[boot] GC off + clock fix installed + clock thread started; entering render loop\n");

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");
  diag_watchdog_start();

  int frame = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);
    g_frame_count++;
    android_native_update_mode();
    android_native_feed_hid((uint8_t (*)(void*,void*,void*,int))Unity_nativeInjectEvent,
                            fake_env, fake_unityplayer_thiz);
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
    if (frame < 5 || (frame % 120) == 0) debugPrintf("[boot] frame %d rendered\n", frame);
    frame++;
  }

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
