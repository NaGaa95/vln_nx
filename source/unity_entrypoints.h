/* unity_entrypoints.h -- UnityPlayer native methods recovered from
 * libunity.so's RegisterNatives table (VERY LITTLE NIGHTMARES,
 * Unity 2021.3.45f2, arm64, build hash 88f88f591b2e).
 *
 * Recovered by tools/re/jnitables.py (a Unity-version-agnostic scan of the
 * R_AARCH64_RELATIVE relocations that build the {name,sig,fn} method tables).
 * The UnityPlayer table sits at .data.rel.ro 0x187b620 (27 entries).
 *
 * These offsets are LINK-TIME addresses for THIS exact libunity.so. If the game
 * is updated, re-run tools/re/jnitables.py against the new binary.
 *
 * Runtime address = unity_mod.load_virtbase + offset (the .so links at base 0;
 * for the executable segment file-offset == vaddr, so RVA == VA here).
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

/* ---- UnityPlayer native method offsets (link-time vaddr) ----------------
 * VLN table @0x187b620.  Compare with ZOOKEEPER (2022.3, 0x5ddxxx): the set is
 * the same modulo two renamed keyboard helpers; every address differs (engine
 * version), and nativeInjectEvent LOSES its trailing int arg (see below). */
/* drive-critical */
#define OFF_JNI_OnLoad                    0x6284e8 /* (JavaVM*,reserved)->jint  caches VM, registers natives */
#define OFF_initJni                       0x6278d4 /* (env,thiz,Context)                 */
#define OFF_nativeRecreateGfxState        0x627b20 /* (env,thiz,int,Surface)  set surface*/
#define OFF_nativeSendSurfaceChangedEvent 0x627b84 /* (env,thiz)                         */
#define OFF_nativeRender                  0x627bd0 /* (env,thiz)->Z   per-frame; false=stop */
#define OFF_nativeInjectEvent             0x627c24 /* (env,thiz,InputEvent)->Z  *** 3-arg, NO trailing int *** */
#define OFF_nativePause                   0x6279bc /* (env,thiz)->Z                      */
#define OFF_nativeResume                  0x627a14 /* (env,thiz)->V                      */
#define OFF_nativeFocusChanged            0x627ad0 /* (env,thiz,Z)                       */
#define OFF_nativeDone                    0x62793c /* (env,thiz)->Z   shutdown           */
#define OFF_nativeApplicationUnload       0x627a8c /* (env,thiz)                         */
#define OFF_nativeLowMemory               0x627a50 /* (env,thiz)                         */
#define OFF_nativeOrientationChanged      0x628494 /* (env,thiz,int,int)                 */
/* secondary / usually unused for a port */
#define OFF_nativeUnitySendMessage        0x628084 /* (env,thiz,String,String,byte[])    */
#define OFF_nativeMuteMasterAudio         0x628284 /* (env,thiz,Z)                       */
#define OFF_nativeIsAutorotationOn        0x628228 /* (env,thiz)->Z                      */
#define OFF_nativeGetSoftInputType        0x627504 /* (env,thiz)->I                      */
#define OFF_nativeRestartActivityIndicator 0x6282dc/* (env,thiz)                         */
#define OFF_nativeSetLaunchURL            0x628318 /* (env,thiz,String)                  */
#define OFF_nativeHidePreservedContent    0x628458 /* (env,thiz)                         */
/* soft keyboard (route via SoftInputProvider stub; not needed for first boot) */
#define OFF_nativeSetInputArea            0x627dac
#define OFF_nativeSetKeyboardIsVisible    0x627e20
#define OFF_nativeSetInputString          0x627e74
#define OFF_nativeSetInputSelection       0x627f20
#define OFF_nativeSoftInputClosed         0x628040
#define OFF_nativeSoftInputCanceled       0x627f7c
#define OFF_nativeSoftInputLostFocus      0x627fc0
#define OFF_nativeReportKeyboardConfigChanged 0x628004

/* ---- FMOD natives (libunity RegisterNatives table @0x188bae0) -----------
 * Captured for the audio path (opensles.c). Same shape as ZOOKEEPER. */
#define OFF_fmodGetInfo                   0x13453c0 /* (I)I                               */
#define OFF_fmodProcess                   0x1345488 /* (Ljava/nio/ByteBuffer;)I           */
#define OFF_fmodProcessMicData            0x1345514 /* (Ljava/nio/ByteBuffer;I)I          */

/* ---- Native engine clock fix (THE black-screen fix; see main.c) ----------
 * TimeManager::Update(TimeManager*) located by its 1e-5 deltaTime-clamp
 * fingerprint (tools/re/scan.py + func.py). Field layout is identical to
 * 2022.3: frameCount u64 @0xc8, aux u32 @0xd0, pause byte @0xf8, startupRef
 * double @0xe8, elapsed @0x70. Entry runs the tiny prologue then falls through
 * to the body which recomputes deltaTime = (newTime - startupRef) - elapsed. */
#define OFF_TimeManager_Update_entry      0x4410d4 /* prologue: [+0xc8]++;[+0xd0]++;if([+0xf8])ret */
#define OFF_TimeManager_Update_body       0x4410f8 /* frameless; re-reads x0, expects newTime in d0 */

/* ---- JNI native signatures: ret (*)(JNIEnv*, jobject thiz, args...) -----
 * NOTE VLN nativeInjectEvent is (env,thiz,InputEvent) -- 3 args. We keep the
 * fn_inject typedef 4-arg (env,thiz,ev,deviceId) so android_native_feed_hid can
 * pass a deviceId uniformly; the engine's 3-arg function simply ignores w3
 * (harmless on AArch64: caller-cleaned, extra arg register unread). */
typedef void     (*fn_initJni)(void*,void*,void*);
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);

#define UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

/* ===========================================================================
 * Drive sequence (what the Java UnityPlayer does; done in main.c):
 *
 *   JNI_OnLoad(fake_vm, NULL);                          // cache VM, reg natives
 *   initJni(env, thiz, fake_context);                   // early init
 *   nativeRecreateGfxState(env, thiz, 0, fake_surface); // give it the surface
 *   nativeSendSurfaceChangedEvent(env, thiz);           // engine builds GL state
 *   nativeResume(env, thiz); nativeFocusChanged(env,thiz,1);  // UNPAUSE the loop
 *   for (;;) {
 *       feed_hid(nativeInjectEvent, env, thiz);         // touch/stick -> MotionEvent
 *       if (!nativeRender(env, thiz)) break;            // false == engine wants out
 *   }
 *   nativeApplicationUnload(env, thiz);  nativeDone(env, thiz);
 * =========================================================================== */

#endif /* UNITY_ENTRYPOINTS_H */
