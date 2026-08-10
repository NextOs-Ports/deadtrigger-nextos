/*
 * imports.h — cadeia de resolucao de simbolos UND do FF V.
 * ff5_resolve e o fallback do so_resolve: overrides -> softfp -> dlsym -> stub.
 * A tabela cresce por demanda (comeca so com os bionic-isms conhecidos).
 */
#ifndef FF5_IMPORTS_H
#define FF5_IMPORTS_H

// Fallback chamado pelo so_resolve p/ cada UND fora da tabela explicita.
void *ff5_resolve(const char *name);

// Registra um override de simbolo (main.c usa p/ glGetString/exit/dlopen/...).
void ff5_add_override(const char *name, void *fn);

// Pre-registra os aliases bionic->glibc (memalign, stat, syscall, setjmp,
// pthread cond/mutex, *_chk, ...). Chamar ANTES de carregar as libs do jogo.
void ff5_imports_init(void);

#endif
