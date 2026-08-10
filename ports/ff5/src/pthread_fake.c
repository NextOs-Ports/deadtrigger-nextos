/* pthread_fake.c -- bridge pthread bionic(armhf) -> glibc for the so-loader.
 *
 * ABI problem (ARM 32-bit): bionic stores mutex/cond/sem in 4 bytes, but glibc
 * uses 24/48/16 bytes. If glibc writes into the game's 4-byte slot it corrupts
 * memory. Solution: the game's 4-byte slot holds a POINTER to a glibc object we
 * allocate (lazy-init covers the static PTHREAD_*_INITIALIZER, which come zeroed).
 *
 * pthread_t / pthread_key_t / pthread_once_t are 4 bytes in both on armhf, so
 * pthread_create/key/once map ~directly. pthread_attr_t does NOT: translate its
 * 24-byte Bionic LP32 layout before handing it to glibc.
 *
 * CRITICAL (armv7): the fast path is a PLAIN pointer read (NO 8-byte atomic).
 * bionic slots can be 4-byte-aligned and an 8-byte LDREXD/LDAR would SIGBUS.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* lazy-init RACE-SAFE: the slot holds a pointer to the glibc object. If 2 threads
 * touch a STATIC primitive (PTHREAD_*_INITIALIZER=zeroed) at once, each would
 * create its own object and the 2nd would clobber the 1st -> lost wakeup. Fix:
 * a GLOBAL lock only on the CREATE path (serializes, prevents double-create).
 * Fast-path = plain slot read (NO atomic; see note above). The lock is only
 * contended on the 1st touch of each primitive; afterwards, zero contention. */
static pthread_mutex_t g_lazy_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- mutex ---------- */
static int pf_dbg(void) { static int v=-1; if(v<0)v=!!getenv("FF5_DBG"); return v; }
/* Um slot bionic estatico inicializado como RECURSIVE/ERRORCHECK NAO vem zerado
   (bionic guarda o tipo nos bits do valor). Nesse caso *slot != NULL e nao e um
   PONTEIRO — e o valor-inicializador bionic. Detectamos valores pequenos (< 64K,
   nao e um endereco valido de heap/mmap) e tratamos como "precisa alocar". */
static int is_bionic_static_init(void *cur) {
  return ((uintptr_t)cur) < 0x10000u;   /* valor de tipo bionic, nao um ponteiro */
}
static pthread_mutex_t *mtx_get(void **slot) {
  void *cur = *slot;                         /* fast-path: plain read (no atomic) */
  if (cur && !is_bionic_static_init(cur)) return (pthread_mutex_t *)cur;
  pthread_mutex_lock(&g_lazy_lock);
  cur = *slot;
  if (!cur || is_bionic_static_init(cur)) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t *m = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    if (pf_dbg()) fprintf(stderr, "[pf] mtx alloc slot=%p old=%p -> M=%p\n", (void*)slot, cur, (void*)m);
    *slot = cur = m;
  }
  pthread_mutex_unlock(&g_lazy_lock);
  return (pthread_mutex_t *)cur;
}
int pthread_mutex_init_fake(void **slot, const void *attr) {
  (void)attr; *slot = NULL; mtx_get(slot); return 0;
}
int pthread_mutex_destroy_fake(void **slot) {
  if (*slot && !is_bionic_static_init(*slot)) { pthread_mutex_destroy((pthread_mutex_t *)*slot); free(*slot); }
  *slot = NULL;
  return 0;
}
int pthread_mutex_lock_fake(void **slot) {
  if (pf_dbg()) fprintf(stderr, "[pf] lock   slot=%p *slot=%p\n", (void*)slot, *slot);
  return pthread_mutex_lock(mtx_get(slot));
}
int pthread_mutex_unlock_fake(void **slot) {
  if (pf_dbg()) fprintf(stderr, "[pf] unlock slot=%p *slot=%p\n", (void*)slot, *slot);
  if (!*slot || is_bionic_static_init(*slot)) return 0;
  return pthread_mutex_unlock((pthread_mutex_t *)*slot);
}
int pthread_mutex_trylock_fake(void **slot) { return pthread_mutex_trylock(mtx_get(slot)); }

/* ---------- cond ---------- */
static pthread_cond_t *cond_get(void **slot) {
  void *cur = *slot;
  if (cur) return (pthread_cond_t *)cur;
  pthread_mutex_lock(&g_lazy_lock);
  cur = *slot;
  if (!cur) { pthread_cond_t *c = malloc(sizeof(pthread_cond_t)); pthread_cond_init(c, NULL); *slot = cur = c; }
  pthread_mutex_unlock(&g_lazy_lock);
  return (pthread_cond_t *)cur;
}
int pthread_cond_init_fake(void **slot, const void *attr) {
  (void)attr; *slot = NULL; cond_get(slot); return 0;
}
int pthread_cond_destroy_fake(void **slot) {
  if (*slot) { pthread_cond_destroy((pthread_cond_t *)*slot); free(*slot); *slot = NULL; }
  return 0;
}
int pthread_cond_signal_fake(void **slot) { return pthread_cond_signal(cond_get(slot)); }
int pthread_cond_broadcast_fake(void **slot) { return pthread_cond_broadcast(cond_get(slot)); }

/* FF5_CONDPOLL=ms: defense against lost-wakeup on condition variables. Instead of
   waiting forever, do a short timedwait and RETURN (spurious wakeup, allowed by
   POSIX) -> the caller re-acquires the mutex and re-checks its predicate. */
static long g_cond_poll_ms = 0;
void cond_set_poll(int ms) { g_cond_poll_ms = ms; }
__attribute__((constructor)) static void cond_poll_env(void) {
  const char *e = getenv("FF5_CONDPOLL"); if (e && *e) g_cond_poll_ms = atoi(e);
}

/* GC-SAFE WAIT: while a thread is BLOCKED in one of our cond/sem waits it is at a
   SAFE point. If the guest's GC does a stop-the-world via SIGPWR/SIGXCPU while the
   thread has them masked, it deadlocks. Unblocking those signals only around the
   real wait lets the GC's suspend handler run. Default OFF (opt-in FF5_GCSAFEWAIT):
   bionic-static GC threads mask SIGPWR via inline syscalls that bypass our shims. */
int g_gc_safe_wait = 0;
void set_gc_safe_wait(int on) { g_gc_safe_wait = on; }
__attribute__((constructor)) static void gc_safe_wait_env(void) { if (getenv("FF5_GCSAFEWAIT")) g_gc_safe_wait = 1; }
void gc_wait_unblock(void *oldp) {   /* oldp = sigset_t* */
  if (!g_gc_safe_wait) return;
  sigset_t un; sigemptyset(&un); sigaddset(&un, SIGPWR); sigaddset(&un, SIGXCPU);
  pthread_sigmask(SIG_UNBLOCK, &un, (sigset_t *)oldp);
}
void gc_wait_restore(void *oldp) {
  if (!g_gc_safe_wait) return;
  pthread_sigmask(SIG_SETMASK, (sigset_t *)oldp, NULL);
}

static void trace_wait(const char *kind, void **cslot, void **mslot, void *ret) {
  if (!getenv("FF5_WAIT_TRACE")) return;
  static unsigned count;
  unsigned n = __atomic_fetch_add(&count, 1, __ATOMIC_RELAXED);
  if (n >= 256) return;
  char name[20] = "?";
  pthread_getname_np(pthread_self(), name, sizeof name);
  fprintf(stderr,
          "[waittrace] %-8s tid=%ld name=%s caller=%p cslot=%p cond=%p mslot=%p mutex=%p\n",
          kind, syscall(SYS_gettid), name, ret, cslot,
          cslot ? *cslot : NULL, mslot, mslot ? *mslot : NULL);
}

int pthread_cond_wait_fake(void **cslot, void **mslot) {
  trace_wait("cond", cslot, mslot, __builtin_return_address(0));
  if (g_cond_poll_ms <= 0) {
    sigset_t old; gc_wait_unblock(&old);
    int r = pthread_cond_wait(cond_get(cslot), mtx_get(mslot));
    gc_wait_restore(&old);
    return r;
  }
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += g_cond_poll_ms * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  int r = pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), &ts);
  return (r == ETIMEDOUT) ? 0 : r;   /* timeout becomes a spurious wakeup */
}
int pthread_cond_timedwait_fake(void **cslot, void **mslot, const struct timespec *ts) {
  trace_wait("timed", cslot, mslot, __builtin_return_address(0));
  sigset_t old; gc_wait_unblock(&old);
  int r = pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), ts);
  gc_wait_restore(&old);
  return r;
}
/* bionic: pthread_cond_timeout_np(cond, mutex, ms) -- relative wait in ms */
int pthread_cond_timeout_np_fake(void **cslot, void **mslot, unsigned ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  return pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), &ts);
}

/* ---------- sem (slot->ptr glibc) ---------- */
static sem_t *sem_get(void **slot) {
  void *cur = *slot;
  if (cur) return (sem_t *)cur;
  pthread_mutex_lock(&g_lazy_lock);
  cur = *slot;
  if (!cur) { sem_t *s = malloc(sizeof(sem_t)); sem_init(s, 0, 0); *slot = cur = s; }
  pthread_mutex_unlock(&g_lazy_lock);
  return (sem_t *)cur;
}
int sem_init_fake(void **slot, int pshared, unsigned value) {
  (void)pshared; sem_t *s = malloc(sizeof(sem_t)); sem_init(s, 0, value); *slot = s; return 0;
}
int sem_destroy_fake(void **slot) {
  if (*slot) { sem_destroy((sem_t *)*slot); free(*slot); *slot = NULL; } return 0;
}
int sem_post_fake(void **slot) { return sem_post(sem_get(slot)); }
int sem_wait_fake(void **slot) { return sem_wait(sem_get(slot)); }
int sem_trywait_fake(void **slot) { return sem_trywait(sem_get(slot)); }
int sem_getvalue_fake(void **slot, int *v) { return sem_getvalue(sem_get(slot), v); }
int sem_timedwait_fake(void **slot, const struct timespec *ts) { return sem_timedwait(sem_get(slot), ts); }

/* ---------- rwlock (slot->ptr glibc, like mutex) ---------- */
static pthread_rwlock_t *rwl_get(void **slot) {
  void *cur = *slot;
  if (cur) return (pthread_rwlock_t *)cur;
  pthread_mutex_lock(&g_lazy_lock);
  cur = *slot;
  if (!cur) { pthread_rwlock_t *r = malloc(sizeof(pthread_rwlock_t)); pthread_rwlock_init(r, NULL); *slot = cur = r; }
  pthread_mutex_unlock(&g_lazy_lock);
  return (pthread_rwlock_t *)cur;
}
int pthread_rwlock_init_fake(void **slot, const void *a) { (void)a; *slot = NULL; rwl_get(slot); return 0; }
int pthread_rwlock_destroy_fake(void **slot) { if (*slot) { pthread_rwlock_destroy((pthread_rwlock_t *)*slot); free(*slot); *slot = NULL; } return 0; }
int pthread_rwlock_rdlock_fake(void **slot) { return pthread_rwlock_rdlock(rwl_get(slot)); }
int pthread_rwlock_wrlock_fake(void **slot) { return pthread_rwlock_wrlock(rwl_get(slot)); }
int pthread_rwlock_tryrdlock_fake(void **slot) { return pthread_rwlock_tryrdlock(rwl_get(slot)); }
int pthread_rwlock_trywrlock_fake(void **slot) { return pthread_rwlock_trywrlock(rwl_get(slot)); }
int pthread_rwlock_unlock_fake(void **slot) { if (!*slot) return 0; return pthread_rwlock_unlock((pthread_rwlock_t *)*slot); }

/* ---------- create / attr ---------- */
/* Bionic LP32 pthread_attr_t (24 bytes):
 *   +0 flags, +4 stack_base, +8 stack_size, +12 guard_size,
 *   +16 sched_policy, +20 sched_priority.
 * Android's 32-bit default stack is 1 MiB. Ignoring this object made glibc use
 * its 8 MiB default for every Unity/GC thread; GC stack scans then faulted much
 * of those mappings into RAM and swap on the 1 GiB device. */
#define BA_FLAG_DETACHED   0x00000001u
#define BA_FLAG_USERSTACK  0x00000002u
#define BA_SIZE            24u
#define BA_DEFAULT_STACK   (1024u * 1024u)

static uint32_t ba_u32(const void *a, size_t off) {
  uint32_t v = 0;
  if (a) memcpy(&v, (const unsigned char *)a + off, sizeof v);
  return v;
}
static void ba_put_u32(void *a, size_t off, uint32_t v) {
  memcpy((unsigned char *)a + off, &v, sizeof v);
}

/* trampoline: install a per-thread sigaltstack before calling the real start.
 * Without it, a SIGSEGV in a Unity thread runs the crash handler on that thread's
 * own (possibly corrupted/full) stack -> re-fault -> silent death. */
struct thr_boot { void *(*start)(void *); void *arg; };
struct thr_altstack { void *mem; size_t size; };
static void thr_altstack_cleanup(void *p) {
  struct thr_altstack *a = (struct thr_altstack *)p;
  if (!a->mem || a->mem == MAP_FAILED) return;
  stack_t off;
  memset(&off, 0, sizeof off);
  off.ss_flags = SS_DISABLE;
  sigaltstack(&off, NULL);
  munmap(a->mem, a->size);
}
static void *thr_trampoline(void *p) {
  struct thr_boot b = *(struct thr_boot *)p;
  free(p);
  struct thr_altstack alt = { MAP_FAILED, 64u * 1024u };
  alt.mem = mmap(NULL, alt.size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (alt.mem != MAP_FAILED) {
    stack_t ss; ss.ss_sp = alt.mem; ss.ss_size = alt.size; ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
  }
  void *r;
  pthread_cleanup_push(thr_altstack_cleanup, &alt);
  if (pf_dbg()) fprintf(stderr, "[pf] thread ENTER start=%p\n", (void*)b.start);
  r = b.start(b.arg);
  if (pf_dbg()) fprintf(stderr, "[pf] thread EXIT  start=%p ret=%p\n", (void*)b.start, r);
  pthread_cleanup_pop(1);
  return r;
}
int pthread_create_fake(pthread_t *t, const void *attr, void *(*start)(void *), void *arg) {
  if (!t || !start) return EINVAL;

  pthread_attr_t host;
  int rc = pthread_attr_init(&host);
  if (rc) return rc;

  uint32_t flags = attr ? ba_u32(attr, 0) : 0;
  void *stack_base = attr ? (void *)(uintptr_t)ba_u32(attr, 4) : NULL;
  size_t stack_size = attr ? (size_t)ba_u32(attr, 8) : BA_DEFAULT_STACK;
  size_t guard_size = attr ? (size_t)ba_u32(attr, 12) : (size_t)sysconf(_SC_PAGESIZE);
  if (!stack_size) stack_size = BA_DEFAULT_STACK;
  if (!guard_size) guard_size = (size_t)sysconf(_SC_PAGESIZE);

  if ((flags & BA_FLAG_USERSTACK) && stack_base) {
    rc = pthread_attr_setstack(&host, stack_base, stack_size);
  } else {
    rc = pthread_attr_setstacksize(&host, stack_size);
  }
  if (!rc) rc = pthread_attr_setguardsize(&host, guard_size);
  if (!rc && (flags & BA_FLAG_DETACHED))
    rc = pthread_attr_setdetachstate(&host, PTHREAD_CREATE_DETACHED);
  if (rc) {
    pthread_attr_destroy(&host);
    return rc;
  }

  if (pf_dbg() || getenv("FF5_STACK_TRACE"))
    fprintf(stderr, "[stack] pthread_create start=%p stack=%zu guard=%zu detached=%u user=%u\n",
            (void*)start, stack_size, guard_size,
            !!(flags & BA_FLAG_DETACHED), !!(flags & BA_FLAG_USERSTACK));
  struct thr_boot *b = malloc(sizeof *b);
  if (!b) {
    rc = pthread_create(t, &host, start, arg);
    pthread_attr_destroy(&host);
    return rc;
  }
  b->start = start; b->arg = arg;
  rc = pthread_create(t, &host, thr_trampoline, b);
  pthread_attr_destroy(&host);
  if (rc) {
    fprintf(stderr, "[pf] pthread_create FALHOU rc=%d start=%p stack=%zu\n",
            rc, (void*)start, stack_size);
    free(b);
  }
  return rc;
}
int pthread_attr_init_fake(void *a) {
  if (!a) return EINVAL;
  memset(a, 0, BA_SIZE);
  ba_put_u32(a, 8, BA_DEFAULT_STACK);
  long page = sysconf(_SC_PAGESIZE);
  ba_put_u32(a, 12, page > 0 ? (uint32_t)page : 4096u);
  if (pf_dbg()) fprintf(stderr, "[pf] attr_init stack=%u\n", BA_DEFAULT_STACK);
  return 0;
}
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) {
  if (!a || (s != PTHREAD_CREATE_JOINABLE && s != PTHREAD_CREATE_DETACHED)) return EINVAL;
  uint32_t flags = ba_u32(a, 0);
  if (s == PTHREAD_CREATE_DETACHED) flags |= BA_FLAG_DETACHED;
  else flags &= ~BA_FLAG_DETACHED;
  ba_put_u32(a, 0, flags);
  if (pf_dbg()) fprintf(stderr, "[pf] attr_setdetach(%d)\n", s);
  return 0;
}
int pthread_attr_setstacksize_fake(void *a, size_t s) {
  if (!a || s < PTHREAD_STACK_MIN || s > UINT32_MAX) return EINVAL;
  ba_put_u32(a, 8, (uint32_t)s);
  if (pf_dbg()) fprintf(stderr, "[pf] attr_setstacksize(%zu)\n", s);
  return 0;
}
int pthread_attr_getdetachstate_fake(const void *a, int *s) {
  if (!a || !s) return EINVAL;
  *s = (ba_u32(a, 0) & BA_FLAG_DETACHED) ?
       PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
  return 0;
}
int pthread_attr_setschedparam_fake(void *a, const void *p) {
  if (!a || !p) return EINVAL;
  ba_put_u32(a, 20, (uint32_t)((const struct sched_param *)p)->sched_priority);
  return 0;
}
int pthread_setschedparam_fake(pthread_t t, int p, const void *s) { (void)t; (void)p; (void)s; return 0; }
int pthread_setname_np_fake(pthread_t t, const char *n) { (void)t; (void)n; return 0; }

/* ---------- thread misc (forward direct; pthread_t = 4B on armhf) ---------- */
int pthread_detach_fake(pthread_t t) { return pthread_detach(t); }
int pthread_join_fake(pthread_t t, void **r) { return pthread_join(t, r); }
pthread_t pthread_self_fake(void) { return pthread_self(); }
int pthread_getschedparam_fake(pthread_t t, int *pol, void *par) { return pthread_getschedparam(t, pol, (struct sched_param *)par); }

/* pthread_sigmask: ABI bionic != glibc. bionic sigset_t (armv7, classic) = 4 BYTES
   (bit s-1 = signal s, signals 1-32); glibc sigset_t = 128 BYTES. Passing the bionic
   set straight to glibc makes glibc READ 128B (124B of stack garbage) and WRITE 128B
   into the 4-byte `old` (corruption). FIX: translate 4B bionic <-> glibc sigset_t. */
int pthread_sigmask_fake(int how, const void *set, void *old) {
  sigset_t gset, gold; sigset_t *pset = NULL, *pold = NULL;
  if (set) {
    sigemptyset(&gset);
    uint32_t bm = *(const uint32_t *)set;   /* bionic: bit (s-1) = signal s */
    /* FF5_SIGFILTER (default OFF): drop SIGPWR/SIGXCPU from BLOCK/SETMASK so every
       thread stays suspendable by the GC stop-the-world. Enable only if the GC
       deadlock from the Terraria class reappears. */
    if (getenv("FF5_SIGFILTER") && (how == SIG_BLOCK || how == SIG_SETMASK)) {
      bm &= ~(1U << (SIGPWR - 1));
      bm &= ~(1U << (SIGXCPU - 1));
    }
    for (int s = 1; s <= 32; s++) if (bm & (1U << (s - 1))) sigaddset(&gset, s);
    pset = &gset;
  }
  if (old) pold = &gold;
  int r = pthread_sigmask(how, pset, pold);
  if (old) {
    uint32_t bm = 0;
    for (int s = 1; s <= 32; s++) if (sigismember(&gold, s)) bm |= (1U << (s - 1));
    *(uint32_t *)old = bm;                   /* return the 4-byte bionic mask */
  }
  return r;
}

/* ---------- key / specific (pthread_key_t compat) ---------- */
int pthread_key_create_fake(unsigned *k, void (*d)(void *)) { return pthread_key_create((pthread_key_t *)k, d); }
int pthread_key_delete_fake(unsigned k) { return pthread_key_delete((pthread_key_t)k); }
void *pthread_getspecific_fake(unsigned k) { return pthread_getspecific((pthread_key_t)k); }
int pthread_setspecific_fake(unsigned k, const void *v) { return pthread_setspecific((pthread_key_t)k, v); }
/* ---------- once (bionic once_t = int) ---------- */
int pthread_once_fake(int *o, void (*f)(void)) { return pthread_once((pthread_once_t *)o, f); }
/* ---------- mutexattr (no-op; mtx_get uses RECURSIVE) ---------- */
int pthread_mutexattr_init_fake(void *a) { (void)a; return 0; }
int pthread_mutexattr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_mutexattr_settype_fake(void *a, int t) { (void)a; (void)t; return 0; }
/* ---------- condattr ---------- */
int pthread_condattr_init_fake(void *a){ (void)a; return 0; }
int pthread_condattr_destroy_fake(void *a){ (void)a; return 0; }
int pthread_condattr_setclock_fake(void *a, int clk){ (void)a; (void)clk; return 0; }
/* Unity/Boehm usa getattr_np+attr_getstack para descobrir os limites reais da
 * pilha de cada thread e varrer referencias managed. O pthread_attr_t Bionic
 * LP32 tem 24 bytes (stack_base em +4, stack_size em +8), enquanto o objeto da
 * glibc e maior e tem outro layout. A antiga faixa inventada de 8 MiB abaixo do
 * SP fazia o GC perder pilhas de workers ou varrer mapeamentos alheios. Traduza
 * o attr real da thread consultada e grave somente os 24 bytes do guest. */
int pthread_getattr_np_fake(pthread_t t, void *attr) {
  if (!attr) return EINVAL;
  pthread_attr_t host;
  void *base = NULL;
  size_t size = 0;
  int rc = pthread_getattr_np(t, &host);
  if (!rc) {
    rc = pthread_attr_getstack(&host, &base, &size);
    pthread_attr_destroy(&host);
  }
  memset(attr, 0, 24);
  if (!rc) {
    *(void **)((unsigned char *)attr + 4) = base;
    *(size_t *)((unsigned char *)attr + 8) = size;
  }
  if (getenv("FF5_STACK_TRACE"))
    fprintf(stderr, "[stack] getattr thread=%lx base=%p size=%zu rc=%d\n",
            (unsigned long)t, base, size, rc);
  return rc;
}
int pthread_attr_getstack_fake(void *attr, void **addr, size_t *size) {
  if (!attr) return EINVAL;
  void *base = *(void **)((unsigned char *)attr + 4);
  size_t bytes = *(size_t *)((unsigned char *)attr + 8);
  if (addr) *addr = base;
  if (size) *size = bytes;
  if (getenv("FF5_STACK_TRACE"))
    fprintf(stderr, "[stack] attr_getstack base=%p size=%zu\n", base, bytes);
  return base && bytes ? 0 : EINVAL;
}
