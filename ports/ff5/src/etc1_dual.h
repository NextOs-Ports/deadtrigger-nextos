#ifndef FF5_ETC1_DUAL_H
#define FF5_ETC1_DUAL_H

#include <stddef.h>

/* ETC1 dupla camada (regra NextOS): RGB vem ETC1 no bundle; o alpha vem num
 * sidecar (ff5_alpha.pak) casado pelo hash do blob RGB. No upload ETC1 o
 * loader cria a textura GEMEA de alpha; o shader patchado amostra
 * c.a = texture2D(u_ff5_atex, uv).r. Espelha ports/summertimesaga (ss_etc1_*),
 * com encode offline porque o FF5 tem 4000+ texturas. Sem gemea/sem patch a
 * textura degrada pra OPACA (alpha=1), nunca lixo. */

int  ff5_dual_enabled(void);                 /* FF5_DUAL_ALPHA=1 (experimental) */
int  ff5_dual_init(const char *gamedir);     /* carrega ff5_alpha.pak; 0 se ausente */

/* notificacoes dos intercepts GL (main.c) */
void ff5_dual_on_active_texture(unsigned unit_enum);       /* GL_TEXTURE0+i */
void ff5_dual_on_bind_texture(unsigned target, unsigned tex);
void ff5_dual_on_delete_textures(int n, const unsigned *ids);
void ff5_dual_on_compressed_upload(unsigned target, int level, unsigned ifmt,
                                   int w, int h, int size, const void *data);
void ff5_dual_on_link_program(unsigned prog);
void ff5_dual_on_use_program(unsigned prog);
void ff5_dual_on_uniform1i(int location, int value);
void ff5_dual_before_draw(void);

/* reescreve o fragment shader (texture2D(_MainTex,..) -> helper com a gemea);
 * devolve buffer malloc'd (caller libera) ou NULL se nao mudou */
char *ff5_dual_patch_shader(const char *src, size_t len, size_t *out_len);

#endif
