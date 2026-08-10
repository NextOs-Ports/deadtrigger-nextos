/*
 * sem_shim.c -- own POSIX semaphores for the so-loader (bionic->glibc).
 *
 * The bionic `sem_t` (Android) is 4 bytes; glibc's is 32 bytes. Unity embeds the
 * sem_t at bionic size; the glibc sem_* (resolved via PLT) operate on 32 bytes ->
 * corrupt adjacent memory and the counter never works -> sem_post does not wake
 * sem_wait -> the preload thread never runs -> boot deadlock.
 *
 * FIX (same idea as the pthread bridge): intercept sem_* and implement them with
 * our own pthread mutex+cond, indexed by the sem POINTER (opaque handle) -- the
 * Unity sem_t layout becomes irrelevant.
 *
 * This is an ALTERNATIVE semaphore backend to pthread_fake.c's sem_*_fake; wire
 * exactly ONE from main.c (Terraria wired these sh_sem_* for sem_*).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <signal.h>

static int stid(void) { return (int)syscall(SYS_gettid); }

/* GC-SAFE WAIT (defined in pthread_fake.c): unblock SIGPWR/SIGXCPU around the real
   wait so the GC stop-the-world can suspend the thread while it is blocked here. */
extern void gc_wait_unblock(void *oldp);
extern void gc_wait_restore(void *oldp);

#define MAX_SEMS 8192
struct mysem { void *key; pthread_mutex_t m; pthread_cond_t c; int count; int used; };
static struct mysem g_sems[MAX_SEMS];
static pthread_mutex_t g_sems_lock = PTHREAD_MUTEX_INITIALIZER;

static struct mysem *sem_lookup(void *s, int create, unsigned initval) {
  pthread_mutex_lock(&g_sems_lock);
  struct mysem *r = NULL, *freeslot = NULL;
  for (int i = 0; i < MAX_SEMS; i++) {
    if (g_sems[i].used && g_sems[i].key == s) { r = &g_sems[i]; break; }
    if (!g_sems[i].used && !freeslot) freeslot = &g_sems[i];
  }
  if (!r && create && freeslot) {
    r = freeslot;
    r->used = 1; r->key = s; r->count = (int)initval;
    pthread_mutex_init(&r->m, NULL);
    pthread_cond_init(&r->c, NULL);
  }
  pthread_mutex_unlock(&g_sems_lock);
  return r;
}

/* FF5_SEMPOLL=ms: sem_wait on NON-main threads returns periodically (timeout) even
   without a post -> the thread "wakes", checks its queue and re-waits. Works around
   the Unity job-scheduler lost-wakeup race. Opt-in and disabled unless the main tid
   is registered (sh_sem_set_main_tid). */
static int g_poll_ms = 0;
static int g_main_tid = 0;
void sh_sem_set_poll(int ms) { g_poll_ms = ms; }
void sh_sem_set_main_tid(int tid) { g_main_tid = tid; }

int sh_sem_init(void *s, int pshared, unsigned value) {
  (void)pshared;
  pthread_mutex_lock(&g_sems_lock);
  struct mysem *r = NULL, *freeslot = NULL;
  for (int i = 0; i < MAX_SEMS; i++) {
    if (g_sems[i].used && g_sems[i].key == s) { r = &g_sems[i]; break; }
    if (!g_sems[i].used && !freeslot) freeslot = &g_sems[i];
  }
  if (!r && freeslot) {
    r = freeslot; r->used = 1; r->key = s;
    pthread_mutex_init(&r->m, NULL); pthread_cond_init(&r->c, NULL);
  }
  if (r) r->count = (int)value;
  pthread_mutex_unlock(&g_sems_lock);
  return 0;
}

int sh_sem_wait(void *s) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  int poll = (g_poll_ms > 0 && g_main_tid && stid() != g_main_tid);
  pthread_mutex_lock(&m->m);
  while (m->count <= 0) {
    sigset_t o; gc_wait_unblock(&o);
    if (poll) {
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += (long)g_poll_ms * 1000000L;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L; }
      int rc = pthread_cond_timedwait(&m->c, &m->m, &ts);
      gc_wait_restore(&o);
      if (rc == ETIMEDOUT) { pthread_mutex_unlock(&m->m); return 0; } /* poll: return without decrement */
    } else {
      pthread_cond_wait(&m->c, &m->m);
      gc_wait_restore(&o);
    }
  }
  m->count--;
  pthread_mutex_unlock(&m->m);
  return 0;
}

int sh_sem_trywait(void *s) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  int rc;
  pthread_mutex_lock(&m->m);
  if (m->count > 0) { m->count--; rc = 0; }
  else { errno = EAGAIN; rc = -1; }
  pthread_mutex_unlock(&m->m);
  return rc;
}

int sh_sem_timedwait(void *s, const struct timespec *abs) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  int rc = 0;
  pthread_mutex_lock(&m->m);
  while (m->count <= 0) {
    sigset_t o; gc_wait_unblock(&o);
    if (abs) {
      rc = pthread_cond_timedwait(&m->c, &m->m, abs);
      gc_wait_restore(&o);
      if (rc == ETIMEDOUT) { errno = ETIMEDOUT; rc = -1; break; }
    } else {
      pthread_cond_wait(&m->c, &m->m);
      gc_wait_restore(&o);
    }
  }
  if (rc == 0) m->count--;
  pthread_mutex_unlock(&m->m);
  return rc;
}

int sh_sem_post(void *s) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  pthread_mutex_lock(&m->m);
  m->count++;
  pthread_cond_signal(&m->c);
  pthread_mutex_unlock(&m->m);
  return 0;
}

int sh_sem_getvalue(void *s, int *sval) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  pthread_mutex_lock(&m->m);
  if (sval) *sval = m->count;
  pthread_mutex_unlock(&m->m);
  return 0;
}

int sh_sem_destroy(void *s) {
  pthread_mutex_lock(&g_sems_lock);
  for (int i = 0; i < MAX_SEMS; i++)
    if (g_sems[i].used && g_sems[i].key == s) { g_sems[i].used = 0; g_sems[i].key = NULL; break; }
  pthread_mutex_unlock(&g_sems_lock);
  return 0;
}
