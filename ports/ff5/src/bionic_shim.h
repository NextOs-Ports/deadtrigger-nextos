/*
 * bionic_shim.h -- bionic ABI helpers for the so-loader (ARM32).
 *
 * - my_memalign: normalized posix_memalign.
 * - FORTIFY __*_chk: strip the dest-size arg, call plain libc.
 * - real strlcpy/strlcat.
 * - my_syscall: raw syscall passthrough with ARM32 numbers.
 * - file-path redirect helpers (my_open/my_fopen/my_stat/...): map
 *   /data/local/tmp -> /tmp and /data/data/<pkg> -> a redirect base.
 * - ff5_sigjmp_save_mask/restore_mask: signal-mask helpers for sigjmp.S.
 */

#ifndef __BIONIC_SHIM_H__
#define __BIONIC_SHIM_H__

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>

/* ---- allocator ---- */
void *my_memalign(size_t align, size_t size);

/* ---- FORTIFY _chk (unchecked -> plain libc) ---- */
void *__memcpy_chk(void *d, const void *s, size_t n, size_t dn);
void *__memmove_chk(void *d, const void *s, size_t n, size_t dn);
void *__memset_chk(void *d, int c, size_t n, size_t dn);
char *__strcpy_chk(char *d, const char *s, size_t dn);
char *__strcat_chk(char *d, const char *s, size_t dn);
size_t __strlcpy_chk(char *d, const char *s, size_t n, size_t dn);
size_t __strlcat_chk(char *d, const char *s, size_t n, size_t dn);
size_t __strlen_chk(const char *s, size_t n);
int __vsnprintf_chk(char *d, size_t n, int flag, size_t dn, const char *f, va_list ap);
int __snprintf_chk(char *d, size_t n, int flag, size_t dn, const char *f, ...);
int __vsprintf_chk(char *d, int flag, size_t dn, const char *f, va_list ap);
int __sprintf_chk(char *d, int flag, size_t dn, const char *f, ...);
void __FD_SET_chk(int fd, void *set, size_t n);

/* ---- real strlcpy/strlcat (bionic; glibc lacks them on this sysroot) ---- */
size_t strlcpy(char *dst, const char *src, size_t sz);
size_t strlcat(char *dst, const char *src, size_t sz);

/* ---- raw syscall (ARM32 numbers) ---- */
long my_syscall(long nr, ...);

/* ---- file-path redirect ---- */
void bionic_set_redirect(const char *gamedir);
int my_open(const char *path, int flags, ...);
int my_open64(const char *path, int flags, ...);
int my_close(int fd);
FILE *my_fopen(const char *path, const char *mode);
FILE *my_fopen64(const char *path, const char *mode);
int   my_sched_getaffinity(int pid, size_t setsize, void *mask);
int   ff5_ncpu(void);
int my_stat(const char *path, void *st);
int my_lstat(const char *path, void *st);
int my_fstat(int fd, void *st);
int my_stat64(const char *path, void *st);
int my_lstat64(const char *path, void *st);
int my_fstat64(int fd, void *st);
int my_access(const char *path, int mode);
int my_mkdir(const char *path, unsigned mode);

/* ---- signal-mask helpers used by sigjmp.S ---- */
int ff5_sigjmp_save_mask(uint32_t *mask32);
int ff5_sigjmp_restore_mask(const uint32_t *mask32);

#endif
