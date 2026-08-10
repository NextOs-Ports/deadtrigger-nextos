/*
 * so_util.h -- utils to load and hook .so modules  [ARM 32-bit / armhf]
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

extern void *text_base, *data_base;
extern size_t text_size, data_size;

void hook_arm(uintptr_t addr, uintptr_t dst);
void hook_arm64(uintptr_t addr, uintptr_t dst);

void so_make_text_writable(void);
void so_make_text_executable(void);
void so_flush_caches(void);
void so_free_temp(void);
int so_load(const char *filename, void *base, size_t max_size);
int so_relocate(void);
int so_resolve(DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
void so_execute_init_array(void);
uintptr_t so_find_addr(const char *symbol);
uintptr_t so_find_addr_safe(const char *symbol);
uintptr_t so_find_addr_rx(const char *symbol);
uintptr_t so_find_rel_addr(const char *symbol);
uintptr_t so_find_rel_addr_safe(const char *symbol);
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs,
                               const char *name);
void so_finalize(void);
int so_unload(void);

typedef struct so_module so_module;
so_module *so_save(void);
void so_use(so_module *m);

/* snapshot dos phdr de cada modulo p/ o dl_iterate_phdr custom (unwind C++).
 * Nossos modulos sao mapeados a mao -> invisiveis ao dl_iterate_phdr da libc. */
#include <elf.h>
struct so_phdr_mod {
  uintptr_t base;        /* dlpi_addr = load_virtbase */
  Elf32_Phdr ph[20];     /* phdrs com p_vaddr ORIGINAL (offset) */
  int phnum;
  char name[32];
};
#define SO_MAX_MODULES 8
extern struct so_phdr_mod g_so_mods[SO_MAX_MODULES];
extern int g_so_nmods;
void so_record_phdr(const char *name);

#endif
