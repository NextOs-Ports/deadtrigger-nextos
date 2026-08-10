#include "etc1_dual.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_ETC1_RGB8_OES 0x8D64
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

#define DUAL_MAX_TEX 65536
#define DUAL_MAX_UNITS 8
#define DUAL_MAX_PROGS 256
/* Mali-450 tem 8 units de fragment; a Unity 2019 GLES2 usa as primeiras. */
#define DUAL_ALPHA_UNIT 7

/* ---- sidecar: FF5ALPHA | u32 count | {u32 hash,u16 w,u16 h,u32 off,u32 sz} ---- */
typedef struct { uint32_t hash; uint16_t w, h; uint32_t off, sz; } PakEntry;

static const uint8_t *g_pak;
static size_t g_pak_size;
static const PakEntry *g_pak_entries;
static uint32_t g_pak_count;

static unsigned g_twin[DUAL_MAX_TEX];        /* tex RGB -> tex gemea de alpha */
static unsigned g_active_unit;               /* 0..N */
static unsigned g_unit_tex[DUAL_MAX_UNITS];  /* textura 2D ligada por unit */
static unsigned g_current_prog;
static unsigned g_white_tex;                 /* 1x1 branco: fallback da gemea */

typedef struct {
    unsigned prog;
    int loc_atex, loc_dual, loc_maintex;
    int unit_maintex;
    int last_dual;                           /* cache pro uniform (evita glUniform por draw) */
    unsigned last_twin;
} ProgInfo;
static ProgInfo g_progs[DUAL_MAX_PROGS];
static ProgInfo *g_current_pi;

static void (*p_glGenTextures)(int, unsigned *);
static void (*p_glBindTexture)(unsigned, unsigned);
static void (*p_glActiveTexture)(unsigned);
static void (*p_glTexParameteri)(unsigned, unsigned, int);
static void (*p_glCompressedTexImage2D)(unsigned, int, unsigned, int, int, int, int, const void *);
static void (*p_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*p_glDeleteTextures)(int, const unsigned *);
static int (*p_glGetUniformLocation)(unsigned, const char *);
static void (*p_glUniform1i)(int, int);
static void (*p_glUniform1f)(int, float);

static int dual_gl_ready(void) {
    if (p_glUniform1f) return 1;
    p_glGenTextures = dlsym(RTLD_DEFAULT, "glGenTextures");
    p_glBindTexture = dlsym(RTLD_DEFAULT, "glBindTexture");
    p_glActiveTexture = dlsym(RTLD_DEFAULT, "glActiveTexture");
    p_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
    p_glCompressedTexImage2D = dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
    p_glTexImage2D = dlsym(RTLD_DEFAULT, "glTexImage2D");
    p_glDeleteTextures = dlsym(RTLD_DEFAULT, "glDeleteTextures");
    p_glGetUniformLocation = dlsym(RTLD_DEFAULT, "glGetUniformLocation");
    p_glUniform1i = dlsym(RTLD_DEFAULT, "glUniform1i");
    p_glUniform1f = dlsym(RTLD_DEFAULT, "glUniform1f");
    return p_glUniform1f != NULL;
}

int ff5_dual_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("FF5_DUAL_ALPHA");
        v = e && *e && strcmp(e, "0") != 0;
    }
    return v;
}

int ff5_dual_init(const char *gamedir) {
    if (!ff5_dual_enabled()) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/ff5_alpha.pak", gamedir);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[dual] sem %s — gemeas de alpha desativadas\n", path);
        return 0;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 16) { close(fd); return 0; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return 0;
    const uint8_t *p = m;
    if (memcmp(p, "FF5ALPHA", 8) != 0) { munmap(m, (size_t)st.st_size); return 0; }
    uint32_t count; memcpy(&count, p + 8, 4);
    if (12 + (size_t)count * sizeof(PakEntry) > (size_t)st.st_size) {
        munmap(m, (size_t)st.st_size); return 0;
    }
    g_pak = p; g_pak_size = (size_t)st.st_size;
    g_pak_entries = (const PakEntry *)(p + 12);
    g_pak_count = count;
    fprintf(stderr, "[dual] ff5_alpha.pak: %u camadas de alpha (%zu KB)\n",
            count, g_pak_size / 1024);
    return 1;
}

static uint32_t fnv1a(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static const PakEntry *pak_find(uint32_t hash, int w, int h) {
    for (uint32_t i = 0; i < g_pak_count; i++) {
        const PakEntry *e = &g_pak_entries[i];
        if (e->hash == hash && e->w == (uint16_t)w && e->h == (uint16_t)h)
            return e;
    }
    return NULL;
}

void ff5_dual_on_active_texture(unsigned unit_enum) {
    unsigned u = unit_enum - GL_TEXTURE0;
    if (u < DUAL_MAX_UNITS) g_active_unit = u;
}

void ff5_dual_on_bind_texture(unsigned target, unsigned tex) {
    if (target == GL_TEXTURE_2D && g_active_unit < DUAL_MAX_UNITS)
        g_unit_tex[g_active_unit] = tex;
}

void ff5_dual_on_delete_textures(int n, const unsigned *ids) {
    if (!ids) return;
    for (int i = 0; i < n; i++) {
        unsigned id = ids[i];
        if (id < DUAL_MAX_TEX && g_twin[id]) {
            unsigned twin = g_twin[id];
            g_twin[id] = 0;
            if (p_glDeleteTextures) p_glDeleteTextures(1, &twin);
            for (int u = 0; u < DUAL_MAX_UNITS; u++)
                if (g_unit_tex[u] == twin) g_unit_tex[u] = 0;
        }
        for (int u = 0; u < DUAL_MAX_UNITS; u++)
            if (g_unit_tex[u] == id) g_unit_tex[u] = 0;
    }
}

void ff5_dual_on_compressed_upload(unsigned target, int level, unsigned ifmt,
                                   int w, int h, int size, const void *data) {
    if (!g_pak || target != GL_TEXTURE_2D || level != 0 ||
        ifmt != GL_ETC1_RGB8_OES || !data || size <= 0)
        return;
    unsigned orig = g_active_unit < DUAL_MAX_UNITS ? g_unit_tex[g_active_unit] : 0;
    if (!orig || orig >= DUAL_MAX_TEX || g_twin[orig]) return;
    const PakEntry *e = pak_find(fnv1a(data, (size_t)size), w, h);
    if (!e || (size_t)e->off + e->sz > g_pak_size) return;
    if (!dual_gl_ready()) return;

    unsigned twin = 0;
    p_glGenTextures(1, &twin);
    if (!twin) return;
    p_glBindTexture(GL_TEXTURE_2D, twin);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_ETC1_RGB8_OES, w, h, 0,
                             (int)e->sz, g_pak + e->off);
    p_glBindTexture(GL_TEXTURE_2D, orig);   /* restaura o bind da Unity */
    g_twin[orig] = twin;
    static unsigned logged;
    if (logged++ < 6)
        fprintf(stderr, "[dual] gemea de alpha #%u: tex=%u %dx%d\n", logged, orig, w, h);
}

static ProgInfo *prog_find(unsigned prog, int create) {
    if (!prog) return NULL;
    ProgInfo *empty = NULL;
    for (int i = 0; i < DUAL_MAX_PROGS; i++) {
        if (g_progs[i].prog == prog) return &g_progs[i];
        if (!g_progs[i].prog && !empty) empty = &g_progs[i];
    }
    if (!create || !empty) return NULL;
    memset(empty, 0, sizeof *empty);
    empty->prog = prog;
    empty->loc_atex = empty->loc_dual = empty->loc_maintex = -1;
    empty->last_dual = -1;
    return empty;
}

void ff5_dual_on_link_program(unsigned prog) {
    if (!g_pak || !dual_gl_ready()) return;
    ProgInfo *pi = prog_find(prog, 1);
    if (!pi) return;
    pi->loc_atex = p_glGetUniformLocation(prog, "u_ff5_atex");
    pi->loc_dual = p_glGetUniformLocation(prog, "u_ff5_dual");
    pi->loc_maintex = p_glGetUniformLocation(prog, "_MainTex");
    pi->unit_maintex = 0;
    pi->last_dual = -1;
    pi->last_twin = 0;
    if (prog == g_current_prog) g_current_pi = pi;
}

void ff5_dual_on_use_program(unsigned prog) {
    static unsigned seen;
    if (seen++ < 3)
        fprintf(stderr, "[dual] glUseProgram interceptado (#%u prog=%u)\n", seen, prog);
    g_current_prog = prog;
    g_current_pi = prog_find(prog, 0);
}

void ff5_dual_on_uniform1i(int location, int value) {
    ProgInfo *pi = prog_find(g_current_prog, 0);
    if (pi && location >= 0 && location == pi->loc_maintex &&
        value >= 0 && value < DUAL_MAX_UNITS)
        pi->unit_maintex = value;
}

void ff5_dual_before_draw(void) {
    if (!g_pak) return;
    ProgInfo *pi = g_current_pi;
    if (!pi || pi->loc_atex < 0 || pi->loc_dual < 0) return;
    unsigned base = pi->unit_maintex < DUAL_MAX_UNITS ? g_unit_tex[pi->unit_maintex] : 0;
    unsigned twin = base && base < DUAL_MAX_TEX ? g_twin[base] : 0;
    int want_dual = twin != 0;

    /* toda manipulacao acontece com a unit reservada ativa; o branco 1x1 e
     * criado ali mesmo pra NUNCA clobberar o binding da unit do draw */
    unsigned bind = twin;
    if (!bind) {
        if (!g_white_tex) {
            static const unsigned char px[4] = {255, 255, 255, 255};
            p_glActiveTexture(GL_TEXTURE0 + DUAL_ALPHA_UNIT);
            p_glGenTextures(1, &g_white_tex);
            if (!g_white_tex) { p_glActiveTexture(GL_TEXTURE0 + g_active_unit); return; }
            p_glBindTexture(GL_TEXTURE_2D, g_white_tex);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                           GL_UNSIGNED_BYTE, px);
            g_unit_tex[DUAL_ALPHA_UNIT] = g_white_tex;
            p_glActiveTexture(GL_TEXTURE0 + g_active_unit);
        }
        bind = g_white_tex;
    }
    if (!bind) return;
    if (g_unit_tex[DUAL_ALPHA_UNIT] != bind) {
        p_glActiveTexture(GL_TEXTURE0 + DUAL_ALPHA_UNIT);
        p_glBindTexture(GL_TEXTURE_2D, bind);
        g_unit_tex[DUAL_ALPHA_UNIT] = bind;
        p_glActiveTexture(GL_TEXTURE0 + g_active_unit);
    }
    if (pi->last_dual != want_dual || pi->last_twin != bind) {
        p_glUniform1i(pi->loc_atex, DUAL_ALPHA_UNIT);
        p_glUniform1f(pi->loc_dual, want_dual ? 1.0f : 0.0f);
        pi->last_dual = want_dual;
        pi->last_twin = bind;
    }
}

/* ---- patch de fragment shader -------------------------------------------- */

static const char DUAL_DECL[] =
    "\nuniform sampler2D u_ff5_atex;\n"
    "uniform mediump float u_ff5_dual;\n"
    "mediump vec4 ff5_tex2D(mediump vec2 ff5_uv){\n"
    "  mediump vec4 ff5_c = texture2D(_MainTex, ff5_uv);\n"
    "  ff5_c.a = mix(ff5_c.a, texture2D(u_ff5_atex, ff5_uv).r, u_ff5_dual);\n"
    "  return ff5_c;\n}\n";

char *ff5_dual_patch_shader(const char *src, size_t len, size_t *out_len) {
    if (!g_pak || !src || !len) return NULL;
    if (!memmem(src, len, "gl_FragColor", 12) &&
        !memmem(src, len, "gl_FragData", 11))
        return NULL;                                   /* so fragment */
    const char *decl = memmem(src, len, "sampler2D _MainTex;", 19);
    if (!decl) return NULL;
    /* ja patchado? (recompilacao do mesmo fonte) */
    if (memmem(src, len, "u_ff5_atex", 10)) return NULL;

    /* 1) troca as chamadas; 2) insere o helper depois da declaracao */
    size_t cap = len + sizeof(DUAL_DECL) + 4096;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    size_t decl_end = (size_t)(decl - src) + 19;
    int replaced = 0;
    while (i < len) {
        if (i == decl_end) {
            memcpy(out + o, DUAL_DECL, sizeof(DUAL_DECL) - 1);
            o += sizeof(DUAL_DECL) - 1;
        }
        /* "texture2D (_MainTex," e "texture2D(_MainTex," */
        if (i + 20 <= len && memcmp(src + i, "texture2D", 9) == 0) {
            size_t j = i + 9;
            while (j < len && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < len && src[j] == '(') {
                size_t k = j + 1;
                while (k < len && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k + 9 <= len && memcmp(src + k, "_MainTex", 8) == 0) {
                    size_t m = k + 8;
                    while (m < len && (src[m] == ' ' || src[m] == '\t')) m++;
                    if (m < len && src[m] == ',') {
                        if (o + 12 >= cap) { free(out); return NULL; }
                        memcpy(out + o, "ff5_tex2D(", 10); o += 10;
                        i = m + 1;                     /* pula ate a virgula */
                        replaced = 1;
                        continue;
                    }
                }
            }
        }
        if (o + 1 >= cap) { free(out); return NULL; }
        out[o++] = src[i++];
    }
    if (!replaced) { free(out); return NULL; }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}
