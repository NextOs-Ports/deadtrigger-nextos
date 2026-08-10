/*
 * imports.c — resolvedor de simbolos UND do FF V.
 * ff5_resolve (fallback do so_resolve): overrides -> pthread/sem *_fake ->
 * softfp_resolve -> dlsym(RTLD_DEFAULT) -> stub. Tabela cresce por demanda.
 */
#include "imports.h"
#include "bionic_shim.h"
#include "opensl.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// softfp (softfp.c) — thunk p/ libm/libc que passa float/double
extern void *softfp_resolve(const char *name);
// setjmp/longjmp bridge (sigjmp.S) — buffer bionic menor que o glibc
extern int  ff5_setjmp(void *);
extern int  ff5__setjmp(void *);
extern int  ff5_sigsetjmp(void *, int);
extern int  ff5___sigsetjmp(void *, int);
extern void ff5_longjmp(void *, int);
extern void ff5__longjmp(void *, int);
extern void ff5_siglongjmp(void *, int);

// ---- __sF: tabela stdio do bionic (DATA). O guest faz &__sF[i] com stride
// de 84 bytes (stdin +0, stdout +84, stderr +168). Esses enderecos nao sao
// FILE* da glibc; cada import stdio deve traduzi-los antes de chamar o host. -
static char ff5_sF[3 * 512];

static FILE *stdio_host(FILE *f) {
    uintptr_t p = (uintptr_t)f, b = (uintptr_t)ff5_sF;
    if (p == b)       return stdin;
    if (p == b + 84)  return stdout;
    if (p == b + 168) return stderr;
    return f;
}

static int my_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vfprintf(stdio_host(f), fmt, ap);
    va_end(ap);
    return rc;
}
static int my_vfprintf(FILE *f, const char *fmt, va_list ap) {
    return vfprintf(stdio_host(f), fmt, ap);
}
static int my_fflush(FILE *f) { return fflush(f ? stdio_host(f) : NULL); }
static size_t my_fread_stdio(void *p, size_t s, size_t n, FILE *f) {
    return fread(p, s, n, stdio_host(f));
}
static size_t my_fwrite_stdio(const void *p, size_t s, size_t n, FILE *f) {
    return fwrite(p, s, n, stdio_host(f));
}
static int my_fseek_stdio(FILE *f, long off, int whence) {
    return fseek(stdio_host(f), off, whence);
}
static int my_fseeko_stdio(FILE *f, off_t off, int whence) {
    return fseeko(stdio_host(f), off, whence);
}
static long my_ftell_stdio(FILE *f) { return ftell(stdio_host(f)); }
static off_t my_ftello_stdio(FILE *f) { return ftello(stdio_host(f)); }
static char *my_fgets_stdio(char *s, int n, FILE *f) {
    return fgets(s, n, stdio_host(f));
}
static int my_fputs_stdio(const char *s, FILE *f) {
    return fputs(s, stdio_host(f));
}
static int my_fputc_stdio(int c, FILE *f) {
    return fputc(c, stdio_host(f));
}
static int my_fscanf_stdio(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vfscanf(stdio_host(f), fmt, ap);
    va_end(ap);
    return rc;
}
static int my_feof_stdio(FILE *f) { return feof(stdio_host(f)); }
static int my_ferror_stdio(FILE *f) { return ferror(stdio_host(f)); }
static void my_clearerr_stdio(FILE *f) { clearerr(stdio_host(f)); }
static int my_setvbuf_stdio(FILE *f, char *buf, int mode, size_t size) {
    return setvbuf(stdio_host(f), buf, mode, size);
}
static int my_fclose_stdio(FILE *f) {
    FILE *host = stdio_host(f);
    if (host == stdin || host == stdout || host == stderr) return fflush(host);
    return fclose(host);
}

// pthread_cond_timedwait_relative_np (bionic, tempo RELATIVO) -> rota p/ o fake
// de tempo absoluto (reltime pequeno = timeout imediato; guest reitera). ------
extern int pthread_cond_timedwait_fake(void *cond, void *mutex, const void *abstime);
static int cond_timedwait_relative_np(void *cond, void *mutex, const void *reltime) {
    return pthread_cond_timedwait_fake(cond, mutex, reltime);
}

// bionic __assert2(file, line, func, msg) — denuncia e sai (nao continuar cego)
static void my_assert2(const char *file, int line, const char *func, const char *msg) {
    fprintf(stderr, "[ff5] __assert2: %s:%d %s: %s\n",
            file ? file : "?", line, func ? func : "?", msg ? msg : "?");
    _exit(70);
}

// ---- trap de memclr/memset com ponteiro pequeno (caca ponteiro NULL+off) --
extern uintptr_t ff5_unity_base(void);
static void report_bad(const char *fn, void *dest, size_t n, void *ret) {
    uintptr_t ub = ff5_unity_base();
    fprintf(stderr, "[ff5] %s dest=%p n=%zu caller=%p", fn, dest, n, ret);
    if (ub && (uintptr_t)ret >= ub) fprintf(stderr, " (libunity+0x%lx)", (unsigned long)((uintptr_t)ret - ub));
    fprintf(stderr, "\n");
    _exit(71);
}
static void *my_memset(void *d, int c, size_t n) {
    if (n && (uintptr_t)d < 0x10000) report_bad("memset", d, n, __builtin_return_address(0));
    return memset(d, c, n);
}
static void my_aeabi_memclr8(void *d, size_t n) {
    if (n && (uintptr_t)d < 0x10000) report_bad("__aeabi_memclr8", d, n, __builtin_return_address(0));
    if (!n) return;
    memset(d, 0, n);
}
static void my_aeabi_memclr(void *d, size_t n) {
    if (n && (uintptr_t)d < 0x10000) report_bad("__aeabi_memclr", d, n, __builtin_return_address(0));
    if (!n) return;
    memset(d, 0, n);
}

// trap de raise/abort/pthread_kill: revela quem dispara o crash deliberado
static void report_raise(const char *fn, int sig, void *ret) {
    uintptr_t ub = ff5_unity_base();
    fprintf(stderr, "[ff5] %s(sig=%d) caller=%p", fn, sig, ret);
    if (ub && (uintptr_t)ret >= ub) fprintf(stderr, " (libunity+0x%lx)", (unsigned long)((uintptr_t)ret - ub));
    fprintf(stderr, "\n");
    _exit(72);
}
static int my_raise(int sig) { report_raise("raise", sig, __builtin_return_address(0)); return 0; }
static void my_abort_trap(void) { report_raise("abort", 6, __builtin_return_address(0)); _exit(72); }
static int my_pthread_kill(void *t, int sig) { report_raise("pthread_kill", sig, __builtin_return_address(0)); return 0; }

// _Unwind_RaiseException: loga o caller (excecao C++?) e passa adiante ao real.
static int (*real_unwind_raise)(void *);
static int my_unwind_raise(void *exc) {
    uintptr_t ub = ff5_unity_base();
    void *ret = __builtin_return_address(0);
    fprintf(stderr, "[ff5] _Unwind_RaiseException caller=%p", ret);
    if (ub && (uintptr_t)ret >= ub) fprintf(stderr, " (libunity+0x%lx)", (unsigned long)((uintptr_t)ret - ub));
    fprintf(stderr, "\n");
    if (!real_unwind_raise) real_unwind_raise = dlsym(RTLD_DEFAULT, "_Unwind_RaiseException");
    return real_unwind_raise ? real_unwind_raise(exc) : 0;
}
static void (*real_unwind_resume)(void *);
static void my_unwind_resume(void *exc) {
    fprintf(stderr, "[ff5] _Unwind_Resume caller=%p\n", __builtin_return_address(0));
    if (!real_unwind_resume) real_unwind_resume = dlsym(RTLD_DEFAULT, "_Unwind_Resume");
    if (real_unwind_resume) real_unwind_resume(exc);
}

// syscall guard: o engine instala handler de SIGSEGV via rt_sigaction CRU
// (nr 174 ARM), foge do nosso override de sigaction, pega a falha real e
// re-raise. Bloqueamos rt_sigaction dos sinais fatais p/ NOSSO handler pegar
// o crash original. Demais syscalls seguem pro my_syscall.
extern long my_syscall(long nr, ...);
#define ARM_NR_rt_sigaction 174
static int sig_fatal(int s) { return s==4||s==6||s==7||s==8||s==11; }
static long my_syscall_guard(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    if (nr == ARM_NR_rt_sigaction && sig_fatal((int)a1)) {
        static int once = 0;
        if (once++ < 8) fprintf(stderr, "[ff5] bloqueado rt_sigaction(sig=%ld) do engine\n", a1);
        return 0;
    }
    long r = my_syscall(nr, a1, a2, a3, a4, a5, a6);
    // mmap2=192, munmap=91, mprotect=125 (ARM32): loga p/ achar reserva de heap
    if (nr == 192 || nr == 125) {
        if ((unsigned long)r >= 0xfffff000UL || (size_t)a2 >= 8u * 1024 * 1024)
            fprintf(stderr, "[ff5] syscall(%s len=%zu KB prot=%ld flags=0x%lx) = 0x%lx%s\n",
                    nr == 192 ? "mmap2" : "mprotect", (size_t)a2 / 1024, a3, a4,
                    (unsigned long)r, (unsigned long)r >= 0xfffff000UL ? " ERRO" : "");
    }
    return r;
}

// ---- tabela de overrides (nome -> fn) ------------------------------------
typedef struct { const char *name; void *fn; } ovr_t;
static ovr_t g_ovr[512];
static int g_novr;

void ff5_add_override(const char *name, void *fn) {
    if (g_novr < 512) { g_ovr[g_novr].name = name; g_ovr[g_novr].fn = fn; g_novr++; }
}
static void add(const char *name, void *fn) { ff5_add_override(name, fn); }

void ff5_imports_init(void) {
    // alocador real (Enlighten/GI quebram com memalign stubado)
    add("memalign", (void *)my_memalign);
    // I/O com redirect de path (asset/save)
    add("open", (void *)my_open);       add("open64", (void *)my_open64);
    add("close", (void *)my_close);
    add("fopen", (void *)my_fopen);     add("fopen64", (void *)my_fopen64);
    add("stat", (void *)my_stat);       add("stat64", (void *)my_stat64);
    add("lstat", (void *)my_lstat);     add("lstat64", (void *)my_lstat64);
    add("fstat", (void *)my_fstat);     add("fstat64", (void *)my_fstat64);
    add("access", (void *)my_access);   add("mkdir", (void *)my_mkdir);
    // CPU count -> 1 (Unity cria 0 Job.Workers, roda jobs INLINE; senao a main
    // trava em WaitForJobGroup esperando workers que nunca executam). ver ff5_ncpu.
    add("sched_getaffinity", (void *)my_sched_getaffinity);
    // syscall cru (job-system usa futex direto) — numeros ARM32.
    // Guard bloqueia rt_sigaction dos sinais fatais (engine sequestra SIGSEGV).
    add("syscall", (void *)my_syscall_guard);
    // str bionic (glibc do sysroot nao tem)
    add("strlcpy", (void *)strlcpy);    add("strlcat", (void *)strlcat);
    // FORTIFY (nossas defs vencem a glibc no link)
    add("__memcpy_chk", (void *)__memcpy_chk);   add("__memmove_chk", (void *)__memmove_chk);
    add("__memset_chk", (void *)__memset_chk);   add("__strcpy_chk", (void *)__strcpy_chk);
    add("__strcat_chk", (void *)__strcat_chk);   add("__strlcpy_chk", (void *)__strlcpy_chk);
    add("__strlcat_chk", (void *)__strlcat_chk); add("__strlen_chk", (void *)__strlen_chk);
    add("__vsnprintf_chk", (void *)__vsnprintf_chk); add("__snprintf_chk", (void *)__snprintf_chk);
    add("__vsprintf_chk", (void *)__vsprintf_chk);   add("__sprintf_chk", (void *)__sprintf_chk);
    add("__FD_SET_chk", (void *)__FD_SET_chk);
    // setjmp/longjmp (buffer bionic menor)
    add("setjmp", (void *)ff5_setjmp);       add("_setjmp", (void *)ff5__setjmp);
    add("sigsetjmp", (void *)ff5_sigsetjmp); add("__sigsetjmp", (void *)ff5___sigsetjmp);
    add("longjmp", (void *)ff5_longjmp);     add("_longjmp", (void *)ff5__longjmp);
    add("siglongjmp", (void *)ff5_siglongjmp);
    // OpenSL: IIDs sao DATA (endereco da variavel), engine = shim
    add("slCreateEngine", (void *)slCreateEngine_shim);
    add("SL_IID_ENGINE", (void *)&sl_IID_ENGINE);
    add("SL_IID_PLAY", (void *)&sl_IID_PLAY);
    add("SL_IID_BUFFERQUEUE", (void *)&sl_IID_BUFFERQUEUE);
    add("SL_IID_VOLUME", (void *)&sl_IID_VOLUME);
    add("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (void *)&sl_IID_ANDROIDSIMPLEBUFFERQUEUE);
    // errno bionic -> glibc
    add("__errno", (void *)dlsym(RTLD_DEFAULT, "__errno_location"));
    // stdio bionic (DATA) + cond relativo bionic
    add("__sF", (void *)ff5_sF);
    add("fprintf", (void *)my_fprintf);
    add("vfprintf", (void *)my_vfprintf);
    add("fflush", (void *)my_fflush);
    add("fread", (void *)my_fread_stdio);
    add("fwrite", (void *)my_fwrite_stdio);
    add("fseek", (void *)my_fseek_stdio);
    add("fseeko", (void *)my_fseeko_stdio);
    add("ftell", (void *)my_ftell_stdio);
    add("ftello", (void *)my_ftello_stdio);
    add("fgets", (void *)my_fgets_stdio);
    add("fputs", (void *)my_fputs_stdio);
    add("fputc", (void *)my_fputc_stdio);
    add("fscanf", (void *)my_fscanf_stdio);
    add("feof", (void *)my_feof_stdio);
    add("ferror", (void *)my_ferror_stdio);
    add("clearerr", (void *)my_clearerr_stdio);
    add("setvbuf", (void *)my_setvbuf_stdio);
    add("fclose", (void *)my_fclose_stdio);
    add("pthread_cond_timedwait_relative_np", (void *)cond_timedwait_relative_np);
    add("__assert2", (void *)my_assert2);
    // traps p/ achar o ponteiro NULL+offset (crash de ctor)
    add("memset", (void *)my_memset);
    add("__aeabi_memclr8", (void *)my_aeabi_memclr8);
    add("__aeabi_memclr4", (void *)my_aeabi_memclr);
    add("__aeabi_memclr", (void *)my_aeabi_memclr);
    // Traps de sinal eram uteis no bring-up, mas nao podem substituir o fluxo
    // normal: o GC usa pthread_kill para suspender/reiniciar workers. Opt-in.
    if (getenv("FF5_SIGNALTRAP")) {
        add("raise", (void *)my_raise);
        add("gsignal", (void *)my_raise);
        add("abort", (void *)my_abort_trap);
        add("pthread_kill", (void *)my_pthread_kill);
    }
    add("_Unwind_RaiseException", (void *)my_unwind_raise);
    add("_Unwind_Resume", (void *)my_unwind_resume);
}

// ---- stub p/ imports genuinamente ausentes -------------------------------
static intptr_t stub_generic(void) { return 0; }

void *ff5_resolve(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_novr; i++)
        if (!strcmp(g_ovr[i].name, name)) return g_ovr[i].fn;
    // pthread_*/sem_* bionic->glibc: casa <name>_fake do pthread_fake.c
    if (!strncmp(name, "pthread_", 8) || !strncmp(name, "sem_", 4)) {
        char fk[96];
        snprintf(fk, sizeof fk, "%s_fake", name);
        void *f = dlsym(RTLD_DEFAULT, fk);
        if (f) return f;
    }
    void *f = softfp_resolve(name);   // libm que passa float (ABI softfp)
    if (f) return f;
    f = dlsym(RTLD_DEFAULT, name);    // glibc + nossos -rdynamic (Android/EGL/OpenSL)
    if (f) return f;
    fprintf(stderr, "[ff5] UND stub: %s\n", name);
    return (void *)stub_generic;
}
