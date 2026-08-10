/*
 * bionic_shim.c -- bionic ABI helpers for the so-loader (ARM32 / glibc host).
 *
 * The __*_chk / strlcpy / strlcat / my_* helpers are exported (build with
 * -rdynamic) so so_resolve's dlsym(RTLD_DEFAULT) fallback finds the bionic-named
 * ones, and imports.c can wire the my_* ones by name.
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/syscall.h>

#include "bionic_shim.h"
#include "so_util.h"

/* ARM (32-bit EABI) syscall numbers -- NOT the arm64/asm-generic ones. The
   toolchain's <sys/syscall.h> already resolves these correctly for the target;
   the guards just document intent and protect against a broken include. */
#ifndef SYS_futex
#define SYS_futex 240
#endif
#ifndef SYS_rt_sigprocmask
#define SYS_rt_sigprocmask 175
#endif
#ifndef SYS_gettid
#define SYS_gettid 224
#endif

/* ============================ allocator ============================ */

void *my_memalign(size_t align, size_t size) {
  /* posix_memalign requires align to be a power of two multiple of sizeof(void*) */
  size_t a = sizeof(void *);
  if (align < a) align = a;
  while (a < align) a <<= 1;   /* round up to a power of two */
  void *p = NULL;
  if (posix_memalign(&p, a, size ? size : 1) != 0) return NULL;
  return p;
}

/* ============================ FORTIFY _chk ============================ */

void *__memcpy_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memcpy(d, s, n); }
void *__memmove_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memmove(d, s, n); }
void *__memset_chk(void *d, int c, size_t n, size_t dn) { (void)dn; return memset(d, c, n); }
char *__strcpy_chk(char *d, const char *s, size_t dn) { (void)dn; return strcpy(d, s); }
char *__strcat_chk(char *d, const char *s, size_t dn) { (void)dn; return strcat(d, s); }
size_t __strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
size_t __strlcpy_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strlcpy(d, s, n); }
size_t __strlcat_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strlcat(d, s, n); }
int __vsnprintf_chk(char *d, size_t n, int flag, size_t dn, const char *f, va_list ap) {
  (void)flag; (void)dn; return vsnprintf(d, n, f, ap);
}
int __snprintf_chk(char *d, size_t n, int flag, size_t dn, const char *f, ...) {
  (void)flag; (void)dn; va_list ap; va_start(ap, f); int r = vsnprintf(d, n, f, ap); va_end(ap); return r;
}
int __vsprintf_chk(char *d, int flag, size_t dn, const char *f, va_list ap) {
  (void)flag; (void)dn; return vsprintf(d, f, ap);
}
int __sprintf_chk(char *d, int flag, size_t dn, const char *f, ...) {
  (void)flag; (void)dn; va_list ap; va_start(ap, f); int r = vsprintf(d, f, ap); va_end(ap); return r;
}
void __FD_SET_chk(int fd, void *set, size_t n) { (void)n; if (fd >= 0) FD_SET(fd, (fd_set *)set); }

/* ============================ strlcpy / strlcat ============================ */
/* bionic funcs that the glibc sysroot does not provide; a passthrough stub that
   does NOT copy corrupts the heap ("free(): invalid size"). Real impls. */
size_t strlcpy(char *dst, const char *src, size_t sz) {
  size_t n = strlen(src);
  if (sz) { size_t c = n < sz - 1 ? n : sz - 1; memcpy(dst, src, c); dst[c] = 0; }
  return n;
}
size_t strlcat(char *dst, const char *src, size_t sz) {
  size_t dl = strnlen(dst, sz);
  size_t sl = strlen(src);
  if (dl == sz) return sz + sl;                 /* no NUL in dst */
  size_t room = sz - dl - 1;
  size_t c = sl < room ? sl : room;
  memcpy(dst + dl, src, c);
  dst[dl + c] = 0;
  return dl + sl;
}

/* ============================ raw syscall ============================ */
extern long syscall(long, ...);

long my_syscall(long nr, ...) {
  va_list ap; va_start(ap, nr);
  long a1 = va_arg(ap, long), a2 = va_arg(ap, long), a3 = va_arg(ap, long);
  long a4 = va_arg(ap, long), a5 = va_arg(ap, long), a6 = va_arg(ap, long);
  va_end(ap);
  /* FF5_RTFILTER (default OFF): remove SIGPWR/SIGXCPU from a raw rt_sigprocmask
     BLOCK/SETMASK so bionic-static GC threads stay suspendable by the stop-the-
     world. Enable only if the Terraria-class GC deadlock reappears. */
  if (getenv("FF5_RTFILTER") && nr == SYS_rt_sigprocmask && a2 &&
      (a1 == 0 /*SIG_BLOCK*/ || a1 == 2 /*SIG_SETMASK*/)) {
    unsigned long m = *(const unsigned long *)a2;
    m &= ~(1UL << (SIGPWR - 1));
    m &= ~(1UL << (SIGXCPU - 1));
    unsigned long tmp = m;
    return syscall(nr, a1, (long)&tmp, a3, a4, a5, a6);
  }
  return syscall(nr, a1, a2, a3, a4, a5, a6);
}

/* ============================ file-path redirect ============================ */

#define FF5_PKG "com.square_enix.android_googleplay.FFPR5"
#define DD_PREFIX "/data/data/" FF5_PKG

static char g_redir_base[1024] = ".";
static volatile int g_catalog_fd = -1;

void bionic_set_redirect(const char *gamedir) {
  if (gamedir && gamedir[0]) {
    snprintf(g_redir_base, sizeof g_redir_base, "%s", gamedir);
    size_t n = strlen(g_redir_base);
    while (n > 1 && g_redir_base[n - 1] == '/') g_redir_base[--n] = 0;
  } else {
    strcpy(g_redir_base, ".");
  }
}

/* Return the mapped path in buf, or NULL if p needs no redirect. */
static const char *redirect_path(const char *p, char *buf, size_t bufsz) {
  if (!p) return NULL;
  /* Application.streamingAssetsPath no Android e um URI dentro do APK:
     jar:file://<apk>!/assets. O falso APK do so-loader nao existe como ZIP;
     seus assets ja estao materializados abaixo de g_redir_base. Preserve o
     caminho que a Unity/Addressables construiu e intercepte apenas a fronteira
     de I/O, exatamente como o AssetManager faria no aparelho Android. Isso
     tambem cobre o packageCodePath vazio usado durante o bring-up
     ("jar:file://!/assets/..."). */
  const char *asset = strstr(p, "!/assets/");
  if (asset) {
    asset += sizeof("!/assets/") - 1;
    snprintf(buf, bufsz, "%s/%s", g_redir_base, asset);
    return buf;
  }
  /* Os bundles on-demand nao ficam dentro do APK: no Android, o Google Play
     Asset Delivery devolve ao provider o diretorio instalado do asset pack.
     No pacote NextOS eles ja estao materializados em <gamedir>/assetpack.
     Preserve primeiro qualquer bundle local real de aa/Android; somente para
     um caminho inexistente e um basename realmente instalado, apresente a
     localizacao que o PAD entregaria. */
  const char *aa_android = strstr(p, "/aa/Android/");
  if (aa_android && access(p, F_OK) != 0) {
    const char *basename = strrchr(aa_android, '/');
    if (basename && basename[1]) {
      snprintf(buf, bufsz, "%s/assetpack/%s", g_redir_base, basename + 1);
      if (access(buf, F_OK) == 0) return buf;
    }
  }
  /* O virtual filesystem Android apresenta os arquivos do APK sob
     assets/bin/Data. No pacote do port eles ja estao materializados em
     bin/Data. Managed fica fora desta regra: a etapa nativa de extracao usa
     assets/bin/Data/Managed como origem e userdata/il2cpp como destino; mapear
     ambos para o mesmo arquivo faria uma copia sobre si mesma. */
  static const char apk_data[] = "assets/bin/Data";
  size_t adl = sizeof(apk_data) - 1;
  if (!strncmp(p, apk_data, adl) && (p[adl] == '/' || p[adl] == 0) &&
      strncmp(p + adl, "/Managed", sizeof("/Managed") - 1)) {
    snprintf(buf, bufsz, "%s/bin/Data%s", g_redir_base, p + adl);
    return buf;
  }
  /* O APK monta assets/bin/Data/Managed em <files>/il2cpp durante a etapa de
     extracao. No port os mesmos arquivos ja estao desempacotados no layout
     final; apresente esse diretorio sob o caminho privado que o IL2CPP recebeu
     em il2cpp_set_data_dir/il2cpp_set_config_dir. */
  char unpacked[1200];
  snprintf(unpacked, sizeof unpacked, "%s/userdata/il2cpp", g_redir_base);
  size_t ul = strlen(unpacked);
  if (!strncmp(p, unpacked, ul) && (p[ul] == '/' || p[ul] == 0)) {
    snprintf(buf, bufsz, "%s/bin/Data/Managed%s", g_redir_base, p + ul);
    return buf;
  }
  if (!strncmp(p, "/data/local/tmp", 15)) {   /* -> writable tmpfs */
    snprintf(buf, bufsz, "/tmp%s", p + 15);
    return buf;
  }
  size_t dl = sizeof(DD_PREFIX) - 1;
  if (!strncmp(p, DD_PREFIX, dl)) {            /* app private dir -> redirect base */
    snprintf(buf, bufsz, "%s%s", g_redir_base, p + dl);
    return buf;
  }
  return NULL;
}

/* CPU count reportado ao guest. Unity dimensiona os Job.Worker por aqui
   (/proc/cpuinfo, /sys/.../cpu, sched_getaffinity, sysconf). Por padrao preserve
   a topologia real do host; FF5_NCPU=n existe apenas para diagnostico. */
int ff5_ncpu(void) {
  static int n = -1;
  if (n < 0) {
    const char *e = getenv("FF5_NCPU");
    n = (e && *e) ? atoi(e) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 32) n = 32;
  }
  return n;
}
/* paths de contagem de CPU que o Unity le (via open E fopen). */
static int is_cpu_path(const char *p) {
  return p && (!strcmp(p, "/proc/cpuinfo") ||
               !strcmp(p, "/sys/devices/system/cpu/possible") ||
               !strcmp(p, "/sys/devices/system/cpu/present") ||
               !strcmp(p, "/sys/devices/system/cpu/online"));
}
/* fd anonimo com conteudo fake de N cores (mkstemp+unlink; kernel 3.14 ok). */
static int cpu_fake_fd(const char *path) {
  char tmpl[] = "/tmp/ff5cpuXXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return -1;
  unlink(tmpl);
  int n = ff5_ncpu();
  if (!strcmp(path, "/proc/cpuinfo")) {
    for (int i = 0; i < n; i++) {
      char b[288]; int len = snprintf(b, sizeof b,
        "processor\t: %d\nmodel name\t: ARMv7 Processor rev 1 (v7l)\n"
        "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
        "CPU implementer\t: 0x41\nCPU architecture: 7\n\n", i);
      if (write(fd, b, len) != len) { close(fd); return -1; }
    }
  } else {                                   /* present/possible/online: "0" ou "0-(n-1)" */
    char b[16]; int len = (n <= 1) ? snprintf(b, sizeof b, "0\n")
                                   : snprintf(b, sizeof b, "0-%d\n", n - 1);
    if (write(fd, b, len) != len) { close(fd); return -1; }
  }
  lseek(fd, 0, SEEK_SET);
  return fd;
}

/* Android/Boehm descobre segmentos de dados estaticos lendo /proc/self/maps.
 * Os ELF carregados por so_load existem no mapa real apenas como mmaps anonimos;
 * sem o pathname e as permissoes de cada PT_LOAD, o GC nao registra os globals
 * nativos que guardam referencias gerenciadas (por exemplo o cache System.Type).
 * Exponha uma visao equivalente a do linker Android tambem para open()/read(),
 * que e o caminho usado pelo coletor, e preserve o mapa real logo depois. */
static int maps_fake_fd(const char *path) {
  if (!path || strcmp(path, "/proc/self/maps")) return -1;

  int real_fd = open(path, O_RDONLY);
  char tmpl[] = "/tmp/ff5mapsXXXXXX";
  int view_fd = mkstemp(tmpl);
  if (view_fd < 0) {
    if (real_fd >= 0) close(real_fd);
    return -1;
  }
  unlink(tmpl);

  const uintptr_t page_mask = 4095u;
  for (int i = 0; i < g_so_nmods; i++) {
    const struct so_phdr_mod *m = &g_so_mods[i];
    for (int j = 0; j < m->phnum; j++) {
      const Elf32_Phdr *ph = &m->ph[j];
      if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
      uintptr_t lo = (m->base + ph->p_vaddr) & ~page_mask;
      uintptr_t hi = (m->base + ph->p_vaddr + ph->p_memsz + page_mask) &
                     ~page_mask;
      unsigned long off = (unsigned long)ph->p_offset & ~4095ul;
      char perm[5] = {
        (ph->p_flags & PF_R) ? 'r' : '-',
        (ph->p_flags & PF_W) ? 'w' : '-',
        (ph->p_flags & PF_X) ? 'x' : '-',
        'p', 0
      };
      char line[1024];
      int len = snprintf(line, sizeof line,
                         "%08lx-%08lx %s %08lx 00:00 0 %s/%s\n",
                         (unsigned long)lo, (unsigned long)hi, perm, off,
                         g_redir_base, m->name);
      if (len <= 0 || len >= (int)sizeof line ||
          write(view_fd, line, (size_t)len) != len) {
        close(view_fd);
        if (real_fd >= 0) close(real_fd);
        return -1;
      }
    }
  }

  if (real_fd >= 0) {
    char buf[4096];
    ssize_t n;
    while ((n = read(real_fd, buf, sizeof buf)) > 0) {
      const char *p = buf;
      ssize_t left = n;
      while (left > 0) {
        ssize_t w = write(view_fd, p, (size_t)left);
        if (w <= 0) break;
        p += w;
        left -= w;
      }
    }
    close(real_fd);
  }
  lseek(view_fd, 0, SEEK_SET);
  return view_fd;
}

int my_open(const char *path, int flags, ...) {
  if (is_cpu_path(path)) { int fd = cpu_fake_fd(path); if (fd >= 0) return fd; }
  if (path && !strcmp(path, "/proc/self/maps") && !(flags & (O_WRONLY | O_RDWR))) {
    int fd = maps_fake_fd(path);
    if (fd >= 0) return fd;
  }
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  int mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
  int fd = open(r ? r : path, flags, mode);
  if (fd >= 0 && path && strstr(path, "/aa/catalog.json"))
    g_catalog_fd = fd;
  if (getenv("FF5_PATH_TRACE") ||
      (getenv("FF5_BUNDLE_TRACE") && path && strstr(path, ".bundle")))
    fprintf(stderr, "[path] open %s%s%s flags=0x%x -> %d errno=%d\n",
            path ? path : "(null)", r ? " => " : "", r ? r : "",
            flags, fd, fd < 0 ? errno : 0);
  return fd;
}
int my_open64(const char *path, int flags, ...) {
  if (is_cpu_path(path)) { int fd = cpu_fake_fd(path); if (fd >= 0) return fd; }
  if (path && !strcmp(path, "/proc/self/maps") && !(flags & (O_WRONLY | O_RDWR))) {
    int fd = maps_fake_fd(path);
    if (fd >= 0) return fd;
  }
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  int mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
  int fd = open64(r ? r : path, flags, mode);
  if (fd >= 0 && path && strstr(path, "/aa/catalog.json"))
    g_catalog_fd = fd;
  if (getenv("FF5_PATH_TRACE") ||
      (getenv("FF5_BUNDLE_TRACE") && path && strstr(path, ".bundle")))
    fprintf(stderr, "[path] open64 %s%s%s flags=0x%x -> %d errno=%d\n",
            path ? path : "(null)", r ? " => " : "", r ? r : "",
            flags, fd, fd < 0 ? errno : 0);
  return fd;
}

int my_close(int fd) {
  if (fd == g_catalog_fd) {
    const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    const char *module = "host";
    unsigned long offset = (unsigned long)caller;
    for (int i = 0; i < g_so_nmods; i++) {
      const struct so_phdr_mod *m = &g_so_mods[i];
      uintptr_t lo = UINTPTR_MAX, hi = 0;
      for (int j = 0; j < m->phnum; j++) {
        const Elf32_Phdr *ph = &m->ph[j];
        if (ph->p_type != PT_LOAD) continue;
        uintptr_t a = m->base + ph->p_vaddr;
        uintptr_t b = a + ph->p_memsz;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
      }
      if (caller >= lo && caller < hi) {
        module = m->name;
        offset = (unsigned long)(caller - m->base);
        break;
      }
    }
    if (getenv("FF5_PATH_TRACE"))
      fprintf(stderr,
              "[path] close catalog fd=%d tid=%ld caller=%s+0x%lx\n",
              fd, syscall(SYS_gettid), module, offset);
    g_catalog_fd = -1;
  }
  return close(fd);
}
FILE *my_fopen(const char *path, const char *mode) {
  if (is_cpu_path(path)) { int fd = cpu_fake_fd(path); if (fd >= 0) { FILE *f = fdopen(fd, "r"); if (f) return f; close(fd); } }
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  FILE *f = NULL;
  if (path && mode && !strcmp(path, "/proc/self/maps") && mode[0] == 'r') {
    /* O linker Android mapeia cada .so a partir do arquivo e /proc/self/maps
     * preserva seu pathname. Nosso loader copia os segmentos para um mmap
     * anonimo; a libRMS consulta maps para achar a propria base ELF e falha se
     * enxergar apenas a linha anonima. Exponha os modulos realmente registrados
     * e, em seguida, mantenha integralmente o mapa real do processo. */
    FILE *real_maps = fopen(path, mode);
    FILE *view = tmpfile();
    if (real_maps && view) {
      for (int i = 0; i < g_so_nmods; i++) {
        uintptr_t lo = UINTPTR_MAX, hi = 0;
        const struct so_phdr_mod *m = &g_so_mods[i];
        for (int j = 0; j < m->phnum; j++) {
          const Elf32_Phdr *ph = &m->ph[j];
          if (ph->p_type != PT_LOAD) continue;
          uintptr_t a = m->base + ph->p_vaddr;
          uintptr_t b = a + ph->p_memsz;
          if (a < lo) lo = a;
          if (b > hi) hi = b;
        }
        if (lo < hi)
          fprintf(view, "%08lx-%08lx r-xp 00000000 00:00 0 %s/%s\n",
                  (unsigned long)lo, (unsigned long)hi, g_redir_base, m->name);
      }
      char line[1024];
      while (fgets(line, sizeof line, real_maps)) fputs(line, view);
      rewind(view);
      f = view;
    } else if (view) {
      fclose(view);
    }
    if (real_maps) fclose(real_maps);
  }
  if (!f) f = fopen(r ? r : path, mode);
  if (getenv("FF5_PATH_TRACE") ||
      (getenv("FF5_BUNDLE_TRACE") && path && strstr(path, ".bundle")))
    fprintf(stderr, "[path] fopen %s%s%s mode=%s -> %p errno=%d\n",
            path ? path : "(null)", r ? " => " : "", r ? r : "",
            mode ? mode : "(null)", (void *)f, f ? 0 : errno);
  return f;
}
FILE *my_fopen64(const char *path, const char *mode) {
  if (is_cpu_path(path)) { int fd = cpu_fake_fd(path); if (fd >= 0) { FILE *f = fdopen(fd, "r"); if (f) return f; close(fd); } }
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  FILE *f = fopen64(r ? r : path, mode);
  if (getenv("FF5_PATH_TRACE") ||
      (getenv("FF5_BUNDLE_TRACE") && path && strstr(path, ".bundle")))
    fprintf(stderr, "[path] fopen64 %s%s%s mode=%s -> %p errno=%d\n",
            path ? path : "(null)", r ? " => " : "", r ? r : "",
            mode ? mode : "(null)", (void *)f, f ? 0 : errno);
  return f;
}
/* Unity/glibc hardware_concurrency conta os bits do mask. sched_getaffinity
   devolve ZERO em sucesso; devolver o numero de CPUs faz a Unity tratar a
   consulta como falha e desabilitar a criacao dos Job.Worker. */
int my_sched_getaffinity(int pid, size_t setsize, void *mask) {
  (void)pid;
  if (mask && setsize >= sizeof(unsigned long)) {
    memset(mask, 0, setsize);
    int n = ff5_ncpu(); int maxb = (int)(8 * sizeof(unsigned long));
    if (n > maxb) n = maxb;
    unsigned long m = 0; for (int i = 0; i < n; i++) m |= (1UL << i);
    *(unsigned long *)mask = m;
    return 0;
  }
  return -1;
}
/* No ARM32, o struct stat publico da Bionic usa o layout stat64 do kernel
 * (104 bytes; st_size em +48). O struct stat da glibc armhf tem 88 bytes e
 * st_size em +44. Escrever o formato glibc direto no buffer Android fazia o
 * IL2CPP ler um tamanho gigantesco, aceitar um read curto e deixar a cauda de
 * global-metadata.dat sem inicializar. struct stat64 da glibc armhf tem o
 * mesmo layout de 104 bytes esperado pela Bionic, entao ele e a ponte ABI. */
int my_stat(const char *path, void *st) {
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  int rc = stat64(r ? r : path, (struct stat64 *)st);
  if (getenv("FF5_PATH_TRACE"))
    fprintf(stderr, "[path] stat %s%s%s -> %d errno=%d\n",
            path ? path : "(null)", r ? " => " : "", r ? r : "",
            rc, rc ? errno : 0);
  return rc;
}
int my_lstat(const char *path, void *st) {
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  return lstat64(r ? r : path, (struct stat64 *)st);
}
int my_fstat(int fd, void *st) {
  int rc = fstat64(fd, (struct stat64 *)st);
  if (getenv("FF5_PATH_TRACE"))
    fprintf(stderr, "[path] fstat fd=%d -> %d errno=%d\n", fd, rc,
            rc ? errno : 0);
  return rc;
}
int my_stat64(const char *path, void *st) {
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  return stat64(r ? r : path, (struct stat64 *)st);
}
int my_lstat64(const char *path, void *st) {
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  return lstat64(r ? r : path, (struct stat64 *)st);
}
int my_fstat64(int fd, void *st) { return fstat64(fd, (struct stat64 *)st); }
int my_access(const char *path, int mode) {
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  return access(r ? r : path, mode);
}
int my_mkdir(const char *path, unsigned mode) {
  char rb[2048]; const char *r = redirect_path(path, rb, sizeof rb);
  return mkdir(r ? r : path, (mode_t)mode);
}

/* ============================ sigjmp mask helpers ============================ */
/* Tail helpers for sigjmp.S. No malloc/locks/logging: siglongjmp may restore the
   mask while still inside a signal handler. The Bionic mask slot is 4 bytes
   (signals 1-32), matching the armv7 sigset_t. */
int ff5_sigjmp_save_mask(uint32_t *mask32) {
  sigset_t host;
  int caller_errno = errno;
  if (!mask32) { errno = EINVAL; return -1; }
  int rc = sigprocmask(SIG_SETMASK, NULL, &host);
  if (!rc) {
    uint32_t bm = 0;
    for (int s = 1; s <= 32; s++) if (sigismember(&host, s)) bm |= (1U << (s - 1));
    *mask32 = bm;
    errno = caller_errno;
  }
  return rc;
}
int ff5_sigjmp_restore_mask(const uint32_t *mask32) {
  sigset_t host;
  int caller_errno = errno;
  if (!mask32) { errno = EINVAL; return -1; }
  sigemptyset(&host);
  for (int s = 1; s <= 32; s++) if (*mask32 & (1U << (s - 1))) sigaddset(&host, s);
  int rc = sigprocmask(SIG_SETMASK, &host, NULL);
  if (!rc) errno = caller_errno;
  return rc;
}
