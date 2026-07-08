/* diag.h -- thread registry + stall watchdog for the frame-1 black-hang hunt.
 *
 * Purpose: a parked thread prints nothing, so we make the hang self-report.
 * Every thread that can block forever publishes a "wait beacon" (what kind of
 * wait, on which object, since when) into a small registry. An independent
 * libnx watchdog thread (NOT routed through the pthread shim we are debugging)
 * notices when frame progress stops and dumps every thread's state to the log.
 *
 * Overhead is near-zero on the happy path: a wait beacon is a few volatile
 * stores, and nothing is logged until a stall is actually detected.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */
#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>
#include "config.h"   /* DEBUG_LOG */

#ifdef __cplusplus
extern "C" {
#endif

/* wait kinds, kept in sync with diag_wait_kind_name() in diag.c */
enum {
  DIAG_W_NONE = 0,
  DIAG_W_COND,        /* pthread_cond_wait / timedwait                       */
  DIAG_W_JOIN,        /* pthread_join                                        */
  DIAG_W_SEM,         /* sem_wait / semaphoreWait                            */
  DIAG_W_MUTEX,       /* contended pthread_mutex_lock (uncontended skips it) */
  DIAG_W_RWLOCK,      /* rwlock read/write lock                              */
  DIAG_W_FUTEX,       /* infinite FUTEX_WAIT (re-poll spin)                  */
};

/* Diagnostic beacons + stall watchdog. Real functions only in a debug build; in a
 * release build (DEBUG_LOG 0) they compile to nothing and diag.c is empty. */
#if DEBUG_LOG
void diag_thread_register(const void *entry, int is_main_engine);
void diag_thread_unregister(void);
void diag_set_name(void *target_pthread, const char *name);
void diag_wait_enter(int kind, const void *obj);
void diag_wait_exit(void);
void diag_futex_spin(const void *obj);
void diag_frame(int frame);
void diag_watchdog_start(void);
#else
#define diag_thread_register(a,b) ((void)0)
#define diag_thread_unregister()  ((void)0)
#define diag_set_name(a,b)        ((void)0)
#define diag_wait_enter(a,b)      ((void)0)
#define diag_wait_exit()          ((void)0)
#define diag_futex_spin(a)        ((void)0)
#define diag_frame(f)             ((void)0)
#define diag_watchdog_start()     ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* DIAG_H */
