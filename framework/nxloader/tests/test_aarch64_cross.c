/*
 * AArch64 cross-safe ABI, loader-hook and instruction-cache gate.
 *
 * This executable builds one sectionless synthetic ELF entirely in memory.
 * It never opens an external guest, never calls an initializer/JNI entry and
 * executes only code owned by this test process.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE

#include "nxloader.h"

#include <elf.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "test_aarch64_cross.c must be compiled for AArch64"
#endif

#if UINTPTR_MAX != UINT64_MAX
#error "the AArch64 cross gate requires a 64-bit uintptr_t"
#endif

#if ULONG_MAX != UINT64_MAX
#error "the AArch64 cross gate requires the LP64 data model"
#endif

typedef char nx_pointer_must_be_64_bits[(sizeof(void *) == 8) ? 1 : -1];
typedef char nx_long_must_be_64_bits[(sizeof(long) == 8) ? 1 : -1];
typedef char nx_size_must_be_64_bits[(sizeof(size_t) == 8) ? 1 : -1];

typedef uint64_t (*nx_preservation_target_fn)(uint64_t);
typedef int (*nx_cache_function)(void);
typedef uint64_t (*nx_synthetic_function)(uint64_t);

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
#ifndef R_AARCH64_RELATIVE
#define R_AARCH64_RELATIVE 1027
#endif

#define NX_SYNTHETIC_FILE_SIZE 0x2000u
#define NX_SYNTHETIC_ENTRY_VMA 0x0800u
#define NX_SYNTHETIC_DYNAMIC_VMA 0x1100u
#define NX_SYNTHETIC_STRING_VMA 0x1200u
#define NX_SYNTHETIC_SYMBOL_VMA 0x1280u
#define NX_SYNTHETIC_HASH_VMA 0x1300u
#define NX_SYNTHETIC_RELA_VMA 0x1380u
#define NX_SYNTHETIC_SLOT_VMA 0x1400u

typedef union nx_synthetic_elf {
  uint64_t alignment;
  unsigned char bytes[NX_SYNTHETIC_FILE_SIZE];
} nx_synthetic_elf;

static int failures;
static volatile uint64_t synthetic_destination_calls;
static size_t synthetic_initializer_filter_calls;

#define CHECK(expression)                                                   \
  do {                                                                      \
    if (!(expression)) {                                                    \
      fprintf(stderr, "aarch64-cross: CHECK failed at %s:%d: %s\n",       \
              __FILE__, __LINE__, #expression);                             \
      failures++;                                                           \
    }                                                                       \
  } while (0)

static uint32_t read_u32(const void *pointer) {
  uint32_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

static uint64_t read_u64(const void *pointer) {
  uint64_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

static int bytes_are_zero(const void *pointer, size_t size) {
  const unsigned char *bytes = (const unsigned char *)pointer;
  size_t index;
  for (index = 0; index < size; ++index) {
    if (bytes[index] != 0)
      return 0;
  }
  return 1;
}

static uintptr_t nx_synthetic_function_address(nx_synthetic_function function) {
  uintptr_t address = 0;
  if (sizeof(function) != sizeof(address))
    return 0;
  memcpy(&address, &function, sizeof(address));
  return address;
}

static nx_synthetic_function nx_synthetic_address_function(uintptr_t address) {
  nx_synthetic_function function = NULL;
  if (sizeof(function) == sizeof(address))
    memcpy(&function, &address, sizeof(function));
  return function;
}

static int64_t nx_aarch64_branch_displacement(uint32_t instruction) {
  int64_t immediate = (int64_t)(instruction & UINT32_C(0x03ffffff));
  if (immediate & (INT64_C(1) << 25))
    immediate -= INT64_C(1) << 26;
  return immediate * 4;
}

static uintptr_t nx_add_branch_displacement(uintptr_t address,
                                            int64_t displacement) {
  if (displacement >= 0)
    return address + (uintptr_t)displacement;
  return address - (uintptr_t)(-displacement);
}

static nxloader_initializer_action nx_forbid_synthetic_initializer(
    void *userdata, const nxloader_module *module,
    const nxloader_initializer_info *initializer) {
  (void)userdata;
  (void)module;
  (void)initializer;
  synthetic_initializer_filter_calls++;
  return NXLOADER_INITIALIZER_REJECT;
}

uint64_t __attribute__((noinline, aligned(4)))
nx_synthetic_hook_destination(uint64_t value) {
  synthetic_destination_calls++;
  return value ^ UINT64_C(0xa64f00d5c0decafe);
}

static void nx_build_synthetic_test_elf(nx_synthetic_elf *fixture) {
  static const uint32_t entry_code[2] = {
      UINT32_C(0xd503201f), /* NOP */
      UINT32_C(0xd65f03c0)  /* RET */
  };
  Elf64_Ehdr *header;
  Elf64_Phdr *programs;
  Elf64_Dyn *dynamic;
  Elf64_Sym *symbols;
  Elf64_Rela *relocation;
  uint32_t *hash;
  size_t dynamic_index = 0;

  memset(fixture, 0, sizeof(*fixture));
  header = (Elf64_Ehdr *)fixture->bytes;
  memcpy(header->e_ident, ELFMAG, SELFMAG);
  header->e_ident[EI_CLASS] = ELFCLASS64;
  header->e_ident[EI_DATA] = ELFDATA2LSB;
  header->e_ident[EI_VERSION] = EV_CURRENT;
  header->e_type = ET_DYN;
  header->e_machine = EM_AARCH64;
  header->e_version = EV_CURRENT;
  header->e_entry = NX_SYNTHETIC_ENTRY_VMA;
  header->e_ehsize = sizeof(*header);
  header->e_phoff = sizeof(*header);
  header->e_phentsize = sizeof(Elf64_Phdr);
  header->e_phnum = 3;
  header->e_shoff = 0;
  header->e_shentsize = 0;
  header->e_shnum = 0;
  header->e_shstrndx = SHN_UNDEF;

  programs = (Elf64_Phdr *)(fixture->bytes + header->e_phoff);
  programs[0].p_type = PT_LOAD;
  programs[0].p_offset = 0;
  programs[0].p_vaddr = 0;
  programs[0].p_filesz = 0x1000;
  programs[0].p_memsz = 0x1000;
  programs[0].p_flags = PF_R | PF_X;
  programs[0].p_align = 0x1000;
  programs[1].p_type = PT_LOAD;
  programs[1].p_offset = 0x1000;
  programs[1].p_vaddr = 0x1000;
  programs[1].p_filesz = 0x1000;
  programs[1].p_memsz = 0x1000;
  programs[1].p_flags = PF_R | PF_W;
  programs[1].p_align = 0x1000;
  programs[2].p_type = PT_DYNAMIC;
  programs[2].p_offset = NX_SYNTHETIC_DYNAMIC_VMA;
  programs[2].p_vaddr = NX_SYNTHETIC_DYNAMIC_VMA;
  programs[2].p_filesz = 0xa0;
  programs[2].p_memsz = 0xa0;
  programs[2].p_flags = PF_R | PF_W;
  programs[2].p_align = 8;

  memcpy(fixture->bytes + NX_SYNTHETIC_ENTRY_VMA, entry_code,
         sizeof(entry_code));
  fixture->bytes[NX_SYNTHETIC_STRING_VMA] = '\0';

  symbols = (Elf64_Sym *)(fixture->bytes + NX_SYNTHETIC_SYMBOL_VMA);
  memset(&symbols[0], 0, sizeof(symbols[0]));

  hash = (uint32_t *)(fixture->bytes + NX_SYNTHETIC_HASH_VMA);
  hash[0] = 1; /* nbucket */
  hash[1] = 1; /* nchain, containing only the mandatory null symbol */
  hash[2] = 0;
  hash[3] = 0;

  relocation = (Elf64_Rela *)(fixture->bytes + NX_SYNTHETIC_RELA_VMA);
  relocation->r_offset = NX_SYNTHETIC_SLOT_VMA;
  relocation->r_info = ELF64_R_INFO(0, R_AARCH64_RELATIVE);
  relocation->r_addend = NX_SYNTHETIC_ENTRY_VMA;

  dynamic = (Elf64_Dyn *)(fixture->bytes + NX_SYNTHETIC_DYNAMIC_VMA);
#define NX_SYNTHETIC_DYN(tag_value, value)                                  \
  do {                                                                      \
    dynamic[dynamic_index].d_tag = (tag_value);                             \
    dynamic[dynamic_index].d_un.d_val = (value);                            \
    dynamic_index++;                                                        \
  } while (0)
  NX_SYNTHETIC_DYN(DT_STRTAB, NX_SYNTHETIC_STRING_VMA);
  NX_SYNTHETIC_DYN(DT_STRSZ, 1);
  NX_SYNTHETIC_DYN(DT_SYMTAB, NX_SYNTHETIC_SYMBOL_VMA);
  NX_SYNTHETIC_DYN(DT_SYMENT, sizeof(Elf64_Sym));
  NX_SYNTHETIC_DYN(DT_HASH, NX_SYNTHETIC_HASH_VMA);
  NX_SYNTHETIC_DYN(DT_RELA, NX_SYNTHETIC_RELA_VMA);
  NX_SYNTHETIC_DYN(DT_RELASZ, sizeof(Elf64_Rela));
  NX_SYNTHETIC_DYN(DT_RELAENT, sizeof(Elf64_Rela));
  NX_SYNTHETIC_DYN(DT_NULL, 0);
#undef NX_SYNTHETIC_DYN
}

static uintptr_t read_stack_pointer(void) {
  uintptr_t value;
  __asm__ volatile("mov %0, sp" : "=r"(value));
  return value;
}

/* File-scope assembly works on the pinned GCC 8.3 compiler, whose AArch64
 * backend predates support for the naked function attribute. */
uint64_t nx_entry_sp_alignment(void);
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".global nx_entry_sp_alignment\n"
    ".type nx_entry_sp_alignment, %function\n"
    "nx_entry_sp_alignment:\n"
    "mov x9, sp\n"
    "and x0, x9, #15\n"
    "ret\n"
    ".size nx_entry_sp_alignment, .-nx_entry_sp_alignment\n");

static uint64_t __attribute__((noinline)) nx_nested_sp_alignment(void) {
  uintptr_t before = read_stack_pointer();
  uint64_t entry = nx_entry_sp_alignment();
  uintptr_t after = read_stack_pointer();
  return ((uint64_t)before | entry | (uint64_t)after) & UINT64_C(15);
}

/* Ten integer arguments cover x0-x7 and the stack argument area. */
static uint64_t __attribute__((noinline)) nx_integer_arguments(
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
    uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9) {
  if (a0 != UINT64_C(0x101) || a1 != UINT64_C(0x202) ||
      a2 != UINT64_C(0x303) || a3 != UINT64_C(0x404) ||
      a4 != UINT64_C(0x505) || a5 != UINT64_C(0x606) ||
      a6 != UINT64_C(0x707) || a7 != UINT64_C(0x808) ||
      a8 != UINT64_C(0x909) || a9 != UINT64_C(0xa0a))
    return 0;
  return UINT64_C(0xa64ca11ab1a0c0de);
}

/* Ten FP arguments cover v0-v7 and the stack argument area. */
static double __attribute__((noinline)) nx_fp_arguments(
    double a0, double a1, double a2, double a3, double a4, double a5,
    double a6, double a7, double a8, double a9) {
  if (a0 != 0.5 || a1 != 1.0 || a2 != 1.5 || a3 != 2.0 || a4 != 2.5 ||
      a5 != 3.0 || a6 != 3.5 || a7 != 4.0 || a8 != 4.5 || a9 != 5.0)
    return -1.0;
  return 4096.5;
}

/* Volatile work prevents this normal C callee from collapsing into a leaf.
 * Its only contract with the assembly probe is ordinary AAPCS64. */
static uint64_t __attribute__((noinline)) nx_preservation_target(
    uint64_t seed) {
  volatile uint64_t words[12];
  uint64_t result = seed;
  size_t index;
  /* A compiler-owned clobber forces a real AAPCS64 save/restore path for all
   * nonvolatile registers checked by the outer assembly probe. */
  __asm__ volatile("" : "+r"(result) :
                   : "x19", "x20", "x21", "x22", "x23", "x24", "x25",
                     "x26", "x27", "x28", "v8", "v9", "v10", "v11",
                     "v12", "v13", "v14", "v15", "memory");
  for (index = 0; index < sizeof(words) / sizeof(words[0]); ++index) {
    words[index] = result ^ (UINT64_C(0x9e3779b97f4a7c15) + index);
    result += words[index] ^ (result >> 7);
  }
  return result;
}

/* Save the caller's AAPCS64 nonvolatile state, seed x19-x28 and the lower
 * 64 bits of d8-d15, perform a real BLR to a normal C function, then verify
 * and restore everything. Bit 0 reports GPR preservation; bit 1 reports FP
 * preservation. x18 is intentionally never touched. */
uint64_t nx_callee_saved_probe(nx_preservation_target_fn target);
__asm__(
      ".text\n"
      ".p2align 2\n"
      ".global nx_callee_saved_probe\n"
      ".type nx_callee_saved_probe, %function\n"
      "nx_callee_saved_probe:\n"
      "sub sp, sp, #160\n\t"
      "stp x19, x20, [sp, #0]\n\t"
      "stp x21, x22, [sp, #16]\n\t"
      "stp x23, x24, [sp, #32]\n\t"
      "stp x25, x26, [sp, #48]\n\t"
      "stp x27, x28, [sp, #64]\n\t"
      "stp x29, x30, [sp, #80]\n\t"
      "stp d8, d9, [sp, #96]\n\t"
      "stp d10, d11, [sp, #112]\n\t"
      "stp d12, d13, [sp, #128]\n\t"
      "stp d14, d15, [sp, #144]\n\t"
      "mov x9, x0\n\t"
      "mov x19, #0x119\n\t"
      "mov x20, #0x120\n\t"
      "mov x21, #0x121\n\t"
      "mov x22, #0x122\n\t"
      "mov x23, #0x123\n\t"
      "mov x24, #0x124\n\t"
      "mov x25, #0x125\n\t"
      "mov x26, #0x126\n\t"
      "mov x27, #0x127\n\t"
      "mov x28, #0x128\n\t"
      "mov x10, #0x208\n\t"
      "fmov d8, x10\n\t"
      "mov x10, #0x209\n\t"
      "fmov d9, x10\n\t"
      "mov x10, #0x210\n\t"
      "fmov d10, x10\n\t"
      "mov x10, #0x211\n\t"
      "fmov d11, x10\n\t"
      "mov x10, #0x212\n\t"
      "fmov d12, x10\n\t"
      "mov x10, #0x213\n\t"
      "fmov d13, x10\n\t"
      "mov x10, #0x214\n\t"
      "fmov d14, x10\n\t"
      "mov x10, #0x215\n\t"
      "fmov d15, x10\n\t"
      "mov x0, #0x55\n\t"
      "blr x9\n\t"
      "mov x12, #3\n\t"
      "cmp x19, #0x119\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "cmp x20, #0x120\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "cmp x21, #0x121\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "cmp x22, #0x122\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "cmp x23, #0x123\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "cmp x24, #0x124\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "cmp x25, #0x125\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "cmp x26, #0x126\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "cmp x27, #0x127\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "cmp x28, #0x128\n\t"
      "b.eq 1f\n\t"
      "and x12, x12, #2\n"
      "1:\n\t"
      "fmov x9, d8\n\t"
      "mov x10, #0x208\n\t"
      "cmp x9, x10\n\t"
      "b.eq 2f\n\t"
      "and x12, x12, #1\n"
      "2:\n\t"
      "fmov x9, d9\n\t"
      "mov x10, #0x209\n\t"
      "cmp x9, x10\n\t"
      "b.eq 2f\n\t"
      "and x12, x12, #1\n"
      "2:\n\t"
      "fmov x9, d10\n\t"
      "mov x10, #0x210\n\t"
      "cmp x9, x10\n\t"
      "b.eq 2f\n\t"
      "and x12, x12, #1\n"
      "2:\n\t"
      "fmov x9, d11\n\t"
      "mov x10, #0x211\n\t"
      "cmp x9, x10\n\t"
      "b.eq 2f\n\t"
      "and x12, x12, #1\n"
      "2:\n\t"
      "fmov x9, d12\n\t"
      "mov x10, #0x212\n\t"
      "cmp x9, x10\n\t"
      "b.eq 2f\n\t"
      "and x12, x12, #1\n"
      "2:\n\t"
      "fmov x9, d13\n\t"
      "mov x10, #0x213\n\t"
      "cmp x9, x10\n\t"
      "b.eq 2f\n\t"
      "and x12, x12, #1\n"
      "2:\n\t"
      "fmov x9, d14\n\t"
      "mov x10, #0x214\n\t"
      "cmp x9, x10\n\t"
      "b.eq 2f\n\t"
      "and x12, x12, #1\n"
      "2:\n\t"
      "fmov x9, d15\n\t"
      "mov x10, #0x215\n\t"
      "cmp x9, x10\n\t"
      "b.eq 2f\n\t"
      "and x12, x12, #1\n"
      "2:\n\t"
      "mov x0, x12\n\t"
      "ldp d14, d15, [sp, #144]\n\t"
      "ldp d12, d13, [sp, #128]\n\t"
      "ldp d10, d11, [sp, #112]\n\t"
      "ldp d8, d9, [sp, #96]\n\t"
      "ldp x29, x30, [sp, #80]\n\t"
      "ldp x27, x28, [sp, #64]\n\t"
      "ldp x25, x26, [sp, #48]\n\t"
      "ldp x23, x24, [sp, #32]\n\t"
      "ldp x21, x22, [sp, #16]\n\t"
      "ldp x19, x20, [sp, #0]\n\t"
      "add sp, sp, #160\n\t"
      "ret\n"
      ".size nx_callee_saved_probe, .-nx_callee_saved_probe\n");

static int read_mapping_permissions(const void *address, char permissions[5]) {
  FILE *stream;
  char line[512];
  uintptr_t target = (uintptr_t)address;
  int found = 0;

  stream = fopen("/proc/self/maps", "r");
  if (!stream)
    return 0;
  while (fgets(line, sizeof(line), stream)) {
    unsigned long first;
    unsigned long last;
    char current[5];
    if (sscanf(line, "%lx-%lx %4s", &first, &last, current) != 3)
      continue;
    if (target >= (uintptr_t)first && target < (uintptr_t)last) {
      memcpy(permissions, current, sizeof(current));
      found = 1;
      break;
    }
  }
  fclose(stream);
  return found;
}

static int verify_mapping(const void *address, int writable, int executable,
                          int *rwx_seen) {
  char permissions[5] = {0, 0, 0, 0, 0};
  if (!read_mapping_permissions(address, permissions))
    return 0;
  if (permissions[1] == 'w' && permissions[2] == 'x')
    *rwx_seen = 1;
  return permissions[0] == 'r' &&
         (permissions[1] == 'w') == writable &&
         (permissions[2] == 'x') == executable;
}

static int test_instruction_cache(int *rwx_seen) {
  static const uint32_t return_one[] = {
      UINT32_C(0x52800020), UINT32_C(0xd65f03c0)};
  static const uint32_t return_two[] = {
      UINT32_C(0x52800040), UINT32_C(0xd65f03c0)};
  long page_size = sysconf(_SC_PAGESIZE);
  void *mapping;
  nx_cache_function function = NULL;
  int first_result = -1;
  int second_result = -1;
  int ok = 1;

  CHECK(page_size > 0);
  if (page_size <= 0)
    return 0;
  mapping = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(mapping != MAP_FAILED);
  if (mapping == MAP_FAILED)
    return 0;

  ok = verify_mapping(mapping, 1, 0, rwx_seen);
  CHECK(ok);
  memcpy(mapping, return_one, sizeof(return_one));
  ok = mprotect(mapping, (size_t)page_size, PROT_READ | PROT_EXEC);
  CHECK(ok == 0);
  if (ok != 0) {
    CHECK(munmap(mapping, (size_t)page_size) == 0);
    return 0;
  }
  ok = verify_mapping(mapping, 0, 1, rwx_seen);
  CHECK(ok);
  __builtin___clear_cache((char *)mapping,
                          (char *)mapping + sizeof(return_one));
  CHECK(sizeof(function) == sizeof(mapping));
  if (sizeof(function) == sizeof(mapping))
    memcpy(&function, &mapping, sizeof(function));
  CHECK(function != NULL);
  if (function)
    first_result = function();
  CHECK(first_result == 1);

  ok = mprotect(mapping, (size_t)page_size, PROT_READ | PROT_WRITE);
  CHECK(ok == 0);
  if (ok != 0) {
    CHECK(munmap(mapping, (size_t)page_size) == 0);
    return 0;
  }
  ok = verify_mapping(mapping, 1, 0, rwx_seen);
  CHECK(ok);
  memcpy(mapping, return_two, sizeof(return_two));
  ok = mprotect(mapping, (size_t)page_size, PROT_READ | PROT_EXEC);
  CHECK(ok == 0);
  if (ok != 0) {
    CHECK(munmap(mapping, (size_t)page_size) == 0);
    return 0;
  }
  ok = verify_mapping(mapping, 0, 1, rwx_seen);
  CHECK(ok);
  __builtin___clear_cache((char *)mapping,
                          (char *)mapping + sizeof(return_two));
  if (function)
    second_result = function();
  CHECK(second_result == 2);
  CHECK(*rwx_seen == 0);
  CHECK(munmap(mapping, (size_t)page_size) == 0);
  return first_result == 1 && second_result == 2 && *rwx_seen == 0;
}

int __attribute__((noinline))
nx_synthetic_loader_hook_gate(int *rwx_seen) {
  static const uint32_t pool_sentinel_code[4] = {
      UINT32_C(0x52800aa0), /* MOV W0, #0x55 */
      UINT32_C(0xd65f03c0), /* RET */
      UINT32_C(0xd503201f), /* NOP */
      UINT32_C(0xd503201f)  /* NOP */
  };
  nx_synthetic_elf fixture;
  const Elf64_Ehdr *header;
  const Elf64_Dyn *dynamic;
  nxloader_config config;
  nxloader_module *module = NULL;
  nxloader_registry *empty_registry = NULL;
  nxloader_module_info info;
  nxloader_resolution_report report;
  nx_synthetic_function destination_function = nx_synthetic_hook_destination;
  nx_synthetic_function entry_function = NULL;
  nx_synthetic_function pool_function = NULL;
  uintptr_t destination_address;
  uintptr_t entry_address;
  void *entry_page = NULL;
  void *pool_page = NULL;
  uint8_t *pool = NULL;
  void *entry = NULL;
  void *slot = NULL;
  uint32_t branch;
  uint64_t relocated_entry;
  uint64_t primed_entry_result;
  uint64_t primed_pool_result;
  uint64_t execution_result;
  uint64_t pool_execution_result;
  const uint64_t execution_input = UINT64_C(0x0123456789abcdef);
  long page_size;
  int initial_failures = failures;
  int completed = 0;
  size_t index;

#define NX_SYNTHETIC_REQUIRE(expression)                                    \
  do {                                                                      \
    int nx_synthetic_condition = (expression) != 0;                         \
    CHECK(nx_synthetic_condition);                                          \
    if (!nx_synthetic_condition)                                            \
      goto cleanup;                                                         \
  } while (0)

  NX_SYNTHETIC_REQUIRE(rwx_seen != NULL);
  nx_build_synthetic_test_elf(&fixture);
  header = (const Elf64_Ehdr *)fixture.bytes;
  dynamic = (const Elf64_Dyn *)(fixture.bytes + NX_SYNTHETIC_DYNAMIC_VMA);
  NX_SYNTHETIC_REQUIRE(header->e_entry == NX_SYNTHETIC_ENTRY_VMA);
  NX_SYNTHETIC_REQUIRE(header->e_shoff == 0);
  NX_SYNTHETIC_REQUIRE(header->e_shentsize == 0);
  NX_SYNTHETIC_REQUIRE(header->e_shnum == 0);
  NX_SYNTHETIC_REQUIRE(header->e_shstrndx == SHN_UNDEF);
  for (index = 0; index < 8; ++index) {
    NX_SYNTHETIC_REQUIRE(dynamic[index].d_tag != DT_INIT);
    NX_SYNTHETIC_REQUIRE(dynamic[index].d_tag != DT_INIT_ARRAY);
    NX_SYNTHETIC_REQUIRE(dynamic[index].d_tag != DT_INIT_ARRAYSZ);
  }
  NX_SYNTHETIC_REQUIRE(dynamic[8].d_tag == DT_NULL);

  synthetic_destination_calls = 0;
  synthetic_initializer_filter_calls = 0;
  nxloader_config_init(&config);
  config.expected_arch = NXLOADER_ARCH_AARCH64;
  config.trampoline_pool_size = 16;
  config.initializer_filter = nx_forbid_synthetic_initializer;
  NX_SYNTHETIC_REQUIRE(nxloader_module_create(&config, &module) ==
                       NXLOADER_OK);
  NX_SYNTHETIC_REQUIRE(nxloader_module_load_memory(
                           module, fixture.bytes, sizeof(fixture.bytes),
                           "owned-sectionless-aarch64-test.so") ==
                       NXLOADER_OK);
  NX_SYNTHETIC_REQUIRE(nxloader_module_get_state(module) ==
                       NXLOADER_STATE_LOADED);

  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  NX_SYNTHETIC_REQUIRE(nxloader_module_get_info(module, &info) ==
                       NXLOADER_OK);
  NX_SYNTHETIC_REQUIRE(info.arch == NXLOADER_ARCH_AARCH64);
  NX_SYNTHETIC_REQUIRE(info.segment_count == 2);
  NX_SYNTHETIC_REQUIRE(info.symbol_count == 1);
  NX_SYNTHETIC_REQUIRE(info.relocation_count == 1);
  NX_SYNTHETIC_REQUIRE(info.needed_count == 0);
  NX_SYNTHETIC_REQUIRE(info.minimum_vma == 0);
  NX_SYNTHETIC_REQUIRE(info.image_size == NX_SYNTHETIC_FILE_SIZE);
  page_size = sysconf(_SC_PAGESIZE);
  NX_SYNTHETIC_REQUIRE(page_size > 0);
  NX_SYNTHETIC_REQUIRE(((unsigned long)page_size &
                        ((unsigned long)page_size - 1ul)) == 0);
  NX_SYNTHETIC_REQUIRE(info.mapping_size ==
                       NX_SYNTHETIC_FILE_SIZE + (size_t)page_size);

  entry = nxloader_module_vma_to_pointer(module, header->e_entry, 8);
  slot = nxloader_module_vma_to_pointer(module, NX_SYNTHETIC_SLOT_VMA, 8);
  NX_SYNTHETIC_REQUIRE(entry != NULL);
  NX_SYNTHETIC_REQUIRE(slot != NULL);
  pool = (uint8_t *)info.mapping_base + info.image_size;
  NX_SYNTHETIC_REQUIRE(read_u32(entry) == UINT32_C(0xd503201f));
  NX_SYNTHETIC_REQUIRE(read_u32((const uint8_t *)entry + 4) ==
                       UINT32_C(0xd65f03c0));
  NX_SYNTHETIC_REQUIRE(read_u64(slot) == 0);
  NX_SYNTHETIC_REQUIRE(bytes_are_zero(pool, 16));
  NX_SYNTHETIC_REQUIRE(verify_mapping(entry, 1, 0, rwx_seen));
  NX_SYNTHETIC_REQUIRE(verify_mapping(slot, 1, 0, rwx_seen));
  NX_SYNTHETIC_REQUIRE(verify_mapping(pool, 1, 0, rwx_seen));

  NX_SYNTHETIC_REQUIRE(nxloader_module_relocate(module) == NXLOADER_OK);
  NX_SYNTHETIC_REQUIRE(nxloader_module_get_state(module) ==
                       NXLOADER_STATE_RELOCATED);
  entry_address = (uintptr_t)entry;
  relocated_entry = read_u64(slot);
  NX_SYNTHETIC_REQUIRE(relocated_entry == (uint64_t)entry_address);

  NX_SYNTHETIC_REQUIRE(nxloader_registry_create(&empty_registry) ==
                       NXLOADER_OK);
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  NX_SYNTHETIC_REQUIRE(nxloader_module_resolve(module, empty_registry, 0,
                                               &report) == NXLOADER_OK);
  NX_SYNTHETIC_REQUIRE(report.imports_resolved == 0);
  NX_SYNTHETIC_REQUIRE(report.weak_imports_zeroed == 0);
  NX_SYNTHETIC_REQUIRE(report.unresolved_strong == 0);
  NX_SYNTHETIC_REQUIRE(report.first_unresolved == NULL);
  NX_SYNTHETIC_REQUIRE(nxloader_module_get_state(module) ==
                       NXLOADER_STATE_RESOLVED);

  /* Prime both instruction-cache ranges before they are patched. The RX->RW
   * transitions never create RWX mappings. After the writes below, this test
   * performs no manual cache maintenance: nxloader_module_finalize() is the
   * only cache invalidation before both exact addresses are executed again. */
  entry_function = nx_synthetic_address_function(entry_address);
  pool_function = nx_synthetic_address_function((uintptr_t)pool);
  NX_SYNTHETIC_REQUIRE(entry_function != NULL);
  NX_SYNTHETIC_REQUIRE(pool_function != NULL);
  entry_page = (void *)(entry_address &
                        ~((uintptr_t)page_size - (uintptr_t)1));
  pool_page = (void *)((uintptr_t)pool &
                       ~((uintptr_t)page_size - (uintptr_t)1));
  NX_SYNTHETIC_REQUIRE(entry_page != pool_page);
  memcpy(pool, pool_sentinel_code, sizeof(pool_sentinel_code));
  NX_SYNTHETIC_REQUIRE(read_u32(pool) == UINT32_C(0x52800aa0));
  NX_SYNTHETIC_REQUIRE(read_u32(pool + 4) == UINT32_C(0xd65f03c0));
  NX_SYNTHETIC_REQUIRE(mprotect(entry_page, (size_t)page_size,
                                PROT_READ | PROT_EXEC) == 0);
  NX_SYNTHETIC_REQUIRE(verify_mapping(entry, 0, 1, rwx_seen));
  NX_SYNTHETIC_REQUIRE(mprotect(pool_page, (size_t)page_size,
                                PROT_READ | PROT_EXEC) == 0);
  NX_SYNTHETIC_REQUIRE(verify_mapping(pool, 0, 1, rwx_seen));
  NX_SYNTHETIC_REQUIRE(*rwx_seen == 0);
  primed_entry_result = entry_function(execution_input);
  primed_pool_result = pool_function(execution_input);
  NX_SYNTHETIC_REQUIRE(primed_entry_result == execution_input);
  NX_SYNTHETIC_REQUIRE(primed_pool_result == UINT64_C(0x55));
  NX_SYNTHETIC_REQUIRE(synthetic_destination_calls == 0);

  NX_SYNTHETIC_REQUIRE(mprotect(entry_page, (size_t)page_size,
                                PROT_READ | PROT_WRITE) == 0);
  NX_SYNTHETIC_REQUIRE(verify_mapping(entry, 1, 0, rwx_seen));
  NX_SYNTHETIC_REQUIRE(mprotect(pool_page, (size_t)page_size,
                                PROT_READ | PROT_WRITE) == 0);
  NX_SYNTHETIC_REQUIRE(verify_mapping(pool, 1, 0, rwx_seen));
  NX_SYNTHETIC_REQUIRE(*rwx_seen == 0);

  destination_address =
      nx_synthetic_function_address(destination_function);
  NX_SYNTHETIC_REQUIRE(destination_address != 0);
  NX_SYNTHETIC_REQUIRE((destination_address & (uintptr_t)3u) == 0);
  NX_SYNTHETIC_REQUIRE(nxloader_module_install_hook(
                           module, entry_address, destination_address, 4) ==
                       NXLOADER_OK);
  branch = read_u32(entry);
  NX_SYNTHETIC_REQUIRE((branch & UINT32_C(0xfc000000)) ==
                       UINT32_C(0x14000000));
  NX_SYNTHETIC_REQUIRE(nx_add_branch_displacement(
                           entry_address,
                           nx_aarch64_branch_displacement(branch)) ==
                       (uintptr_t)pool);
  NX_SYNTHETIC_REQUIRE(read_u32(pool) == UINT32_C(0x58000051));
  NX_SYNTHETIC_REQUIRE(read_u32(pool + 4) == UINT32_C(0xd61f0220));
  NX_SYNTHETIC_REQUIRE(read_u64(pool + 8) ==
                       (uint64_t)destination_address);
  NX_SYNTHETIC_REQUIRE(verify_mapping(entry, 1, 0, rwx_seen));
  NX_SYNTHETIC_REQUIRE(verify_mapping(pool, 1, 0, rwx_seen));

  NX_SYNTHETIC_REQUIRE(nxloader_module_finalize(module) == NXLOADER_OK);
  NX_SYNTHETIC_REQUIRE(nxloader_module_get_state(module) ==
                       NXLOADER_STATE_FINALIZED);
  NX_SYNTHETIC_REQUIRE(verify_mapping(entry, 0, 1, rwx_seen));
  NX_SYNTHETIC_REQUIRE(verify_mapping(slot, 1, 0, rwx_seen));
  NX_SYNTHETIC_REQUIRE(verify_mapping(pool, 0, 1, rwx_seen));
  NX_SYNTHETIC_REQUIRE(*rwx_seen == 0);
  NX_SYNTHETIC_REQUIRE(synthetic_initializer_filter_calls == 0);

  NX_SYNTHETIC_REQUIRE((uintptr_t)relocated_entry == entry_address);
  execution_result = entry_function(execution_input);
  NX_SYNTHETIC_REQUIRE(
      execution_result ==
      (execution_input ^ UINT64_C(0xa64f00d5c0decafe)));
  NX_SYNTHETIC_REQUIRE(execution_result != primed_entry_result);
  pool_execution_result = pool_function(execution_input);
  NX_SYNTHETIC_REQUIRE(
      pool_execution_result ==
      (execution_input ^ UINT64_C(0xa64f00d5c0decafe)));
  NX_SYNTHETIC_REQUIRE(pool_execution_result != primed_pool_result);
  NX_SYNTHETIC_REQUIRE(synthetic_destination_calls == 2);
  NX_SYNTHETIC_REQUIRE(synthetic_initializer_filter_calls == 0);
  completed = 1;

cleanup:
  CHECK(synthetic_initializer_filter_calls == 0);
  nxloader_registry_destroy(empty_registry);
  nxloader_module_destroy(module);
#undef NX_SYNTHETIC_REQUIRE
  return completed && failures == initial_failures;
}

int main(void) {
  volatile uint64_t integers[10] = {
      UINT64_C(0x101), UINT64_C(0x202), UINT64_C(0x303), UINT64_C(0x404),
      UINT64_C(0x505), UINT64_C(0x606), UINT64_C(0x707), UINT64_C(0x808),
      UINT64_C(0x909), UINT64_C(0xa0a)};
  volatile double fp[10] = {
      0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};
  uint64_t integer_result;
  double fp_result;
  uint64_t preserved;
  int rwx_seen = 0;
  int cache_rewrite;
  int loader_hook_execution;

  CHECK(sizeof(void *) == 8);
  CHECK(sizeof(long) == 8);
  CHECK(sizeof(size_t) == 8);
  CHECK(sizeof(uintptr_t) == 8);
  CHECK(nxloader_process_arch() == NXLOADER_ARCH_AARCH64);

  integer_result = nx_integer_arguments(
      integers[0], integers[1], integers[2], integers[3], integers[4],
      integers[5], integers[6], integers[7], integers[8], integers[9]);
  CHECK(integer_result == UINT64_C(0xa64ca11ab1a0c0de));
  fp_result = nx_fp_arguments(fp[0], fp[1], fp[2], fp[3], fp[4], fp[5],
                              fp[6], fp[7], fp[8], fp[9]);
  CHECK(fp_result == 4096.5);

  CHECK((read_stack_pointer() & (uintptr_t)15u) == 0);
  CHECK(nx_entry_sp_alignment() == 0);
  CHECK(nx_nested_sp_alignment() == 0);

  preserved = nx_callee_saved_probe(nx_preservation_target);
  CHECK((preserved & UINT64_C(1)) != 0);
  CHECK((preserved & UINT64_C(2)) != 0);

  cache_rewrite = test_instruction_cache(&rwx_seen);
  CHECK(cache_rewrite != 0);
  CHECK(rwx_seen == 0);

  loader_hook_execution = nx_synthetic_loader_hook_gate(&rwx_seen);
  CHECK(loader_hook_execution != 0);
  CHECK(rwx_seen == 0);

  if (failures != 0) {
    fprintf(stderr, "aarch64-cross: FAIL failures=%d\n", failures);
    return 1;
  }
  puts("aarch64-cross: PASS lp64=1 integer_args=1 fp_args=1 "
       "stack_align_16=1 callee_saved_gpr=1 callee_saved_fp=1 "
       "cache_rewrite=1 loader_lifecycle=1 sectionless=1 "
       "relative_relocation=1 hook_veneer=1 hook_execution=1 "
       "icache_primed_entry=1 icache_primed_pool=1 "
       "post_finalize_reexecution=1 finalize_only_loader_cache_clear=1 "
       "loader_cache_finalize=1 wx_mapping=0 "
       "synthetic_test_elf_loaded=1 external_guest_elf_loaded=0 "
       "guest_elf_loaded=0 guest_initializers_executed=0 "
       "device_access=0 hardware_ran=0");
  return 0;
}
