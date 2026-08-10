/* softfp.c -- float-ABI bridge.
 *
 * libunity.so / libil2cpp.so are SOFTFP (base AAPCS): float/double pass in
 * INTEGER registers (r0:r1 ...). The device glibc libm/libc is HARDFP
 * (Tag_ABI_VFP_args=VFP): float/double in d0..d7. Calling libm directly from
 * the Unity softfp code puts the args in the wrong registers -> e.g.
 * modf(512.0,iptr) reads iptr from the low word of 512.0 = 0 -> writes to NULL
 * (SEGV in modfl).
 *
 * Fix: wrappers declared pcs("aapcs") (softfp) -> they receive args the way the
 * Unity code passes them and call the glibc (hardfp) func; GCC performs the
 * register translation. imports.c routes float-passing symbols through
 * softfp_resolve() BEFORE dlsym(RTLD_DEFAULT).
 */
#include <math.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#define SF __attribute__((pcs("aapcs")))

/* 1 arg double->double */
#define W1D(n)  SF double sf_##n(double x){ return n(x); }
/* 1 arg float->float */
#define W1F(n)  SF float  sf_##n(float x){ return n(x); }
/* 2 arg double,double->double */
#define W2D(n)  SF double sf_##n(double a,double b){ return n(a,b); }
/* 2 arg float,float->float */
#define W2F(n)  SF float  sf_##n(float a,float b){ return n(a,b); }

/* trig / hyperbolic */
W1D(acos) W1D(asin) W1D(atan) W1D(cos) W1D(sin) W1D(tan)
W1D(cosh) W1D(sinh) W1D(tanh) W1D(acosh) W1D(asinh) W1D(atanh)
W1F(acosf) W1F(asinf) W1F(atanf) W1F(cosf) W1F(sinf) W1F(tanf)
W1F(coshf) W1F(sinhf) W1F(tanhf)
/* exp / log */
W1D(exp) W1D(exp2) W1D(log) W1D(log2) W1D(log10) W1D(expm1) W1D(log1p)
W1F(expf) W1F(exp2f) W1F(logf) W1F(log2f) W1F(log10f)
/* roots / misc */
W1D(sqrt) W1D(cbrt) W1D(fabs)
W1F(sqrtf) W1F(cbrtf) W1F(fabsf)
/* rounding */
W1D(ceil) W1D(floor) W1D(round) W1D(trunc) W1D(rint) W1D(nearbyint)
W1F(ceilf) W1F(floorf) W1F(roundf) W1F(truncf) W1F(rintf) W1F(nearbyintf)
/* 2-arg double */
W2D(atan2) W2D(fmod) W2D(pow) W2D(remainder) W2D(hypot)
W2D(fmax) W2D(fmin) W2D(copysign) W2D(fdim) W2D(nextafter)
/* 2-arg float */
W2F(atan2f) W2F(fmodf) W2F(powf) W2F(remainderf) W2F(hypotf)
W2F(fmaxf) W2F(fminf) W2F(copysignf) W2F(fdimf)

/* special signatures (pointer / int args) */
SF double sf_modf(double x, double *iptr){ return modf(x, iptr); }
SF float  sf_modff(float x, float *iptr){ return modff(x, iptr); }
SF double sf_frexp(double x, int *e){ return frexp(x, e); }
SF float  sf_frexpf(float x, int *e){ return frexpf(x, e); }
SF double sf_ldexp(double x, int e){ return ldexp(x, e); }
SF float  sf_ldexpf(float x, int e){ return ldexpf(x, e); }
SF double sf_scalbn(double x, int e){ return scalbn(x, e); }
SF float  sf_scalbnf(float x, int e){ return scalbnf(x, e); }
SF double sf_scalbln(double x, long e){ return scalbln(x, e); }
SF double sf_strtod(const char *s, char **end){ return strtod(s, end); }
SF float  sf_strtof(const char *s, char **end){ return strtof(s, end); }

/* GLES do aparelho e hard-float, enquanto libunity Android e softfp. Funcoes
 * que recebem somente inteiros/ponteiros compartilham a mesma ABI e podem ir
 * direto ao driver. TODAS as entradas GLES2 que recebem float por valor
 * precisam de thunk: deixar glUniform*f de fora entrega cor/alfa/coordenadas
 * ao Mali pelos registradores errados e faz sprites sumirem ou se deslocarem. */
static void (*r_glBlendColor)(float, float, float, float);
static void (*r_glClearColor)(float, float, float, float);
static void (*r_glClearDepthf)(float);
static void (*r_glDepthRangef)(float, float);
static void (*r_glLineWidth)(float);
static void (*r_glPolygonOffset)(float, float);
static void (*r_glSampleCoverage)(float, unsigned char);
static void (*r_glTexParameterf)(unsigned, unsigned, float);
static void (*r_glUniform1f)(int, float);
static void (*r_glUniform2f)(int, float, float);
static void (*r_glUniform3f)(int, float, float, float);
static void (*r_glUniform4f)(int, float, float, float, float);
static void (*r_glVertexAttrib1f)(unsigned, float);
static void (*r_glVertexAttrib2f)(unsigned, float, float);
static void (*r_glVertexAttrib3f)(unsigned, float, float, float);
static void (*r_glVertexAttrib4f)(unsigned, float, float, float, float);

SF void sf_glBlendColor(float r, float g, float b, float a) {
  if (!r_glBlendColor) r_glBlendColor = dlsym(RTLD_DEFAULT, "glBlendColor");
  if (r_glBlendColor) r_glBlendColor(r, g, b, a);
}
SF void sf_glClearColor(float r, float g, float b, float a) {
  if (!r_glClearColor) r_glClearColor = dlsym(RTLD_DEFAULT, "glClearColor");
  if (r_glClearColor) r_glClearColor(r, g, b, a);
}
SF void sf_glClearDepthf(float depth) {
  if (!r_glClearDepthf) r_glClearDepthf = dlsym(RTLD_DEFAULT, "glClearDepthf");
  if (r_glClearDepthf) r_glClearDepthf(depth);
}
SF void sf_glDepthRangef(float near_value, float far_value) {
  if (!r_glDepthRangef) r_glDepthRangef = dlsym(RTLD_DEFAULT, "glDepthRangef");
  if (r_glDepthRangef) r_glDepthRangef(near_value, far_value);
}
SF void sf_glLineWidth(float width) {
  if (!r_glLineWidth) r_glLineWidth = dlsym(RTLD_DEFAULT, "glLineWidth");
  if (r_glLineWidth) r_glLineWidth(width);
}
SF void sf_glPolygonOffset(float factor, float units) {
  if (!r_glPolygonOffset) r_glPolygonOffset = dlsym(RTLD_DEFAULT, "glPolygonOffset");
  if (r_glPolygonOffset) r_glPolygonOffset(factor, units);
}
SF void sf_glSampleCoverage(float value, unsigned char invert) {
  if (!r_glSampleCoverage) r_glSampleCoverage = dlsym(RTLD_DEFAULT, "glSampleCoverage");
  if (r_glSampleCoverage) r_glSampleCoverage(value, invert);
}
SF void sf_glTexParameterf(unsigned target, unsigned pname, float value) {
  if (!r_glTexParameterf) r_glTexParameterf = dlsym(RTLD_DEFAULT, "glTexParameterf");
  if (r_glTexParameterf) r_glTexParameterf(target, pname, value);
}
SF void sf_glUniform1f(int location, float x) {
  if (!r_glUniform1f) r_glUniform1f = dlsym(RTLD_DEFAULT, "glUniform1f");
  if (r_glUniform1f) r_glUniform1f(location, x);
}
SF void sf_glUniform2f(int location, float x, float y) {
  if (!r_glUniform2f) r_glUniform2f = dlsym(RTLD_DEFAULT, "glUniform2f");
  if (r_glUniform2f) r_glUniform2f(location, x, y);
}
SF void sf_glUniform3f(int location, float x, float y, float z) {
  if (!r_glUniform3f) r_glUniform3f = dlsym(RTLD_DEFAULT, "glUniform3f");
  if (r_glUniform3f) r_glUniform3f(location, x, y, z);
}
SF void sf_glUniform4f(int location, float x, float y, float z, float w) {
  if (!r_glUniform4f) r_glUniform4f = dlsym(RTLD_DEFAULT, "glUniform4f");
  if (r_glUniform4f) r_glUniform4f(location, x, y, z, w);
}
SF void sf_glVertexAttrib1f(unsigned index, float x) {
  if (!r_glVertexAttrib1f) r_glVertexAttrib1f = dlsym(RTLD_DEFAULT, "glVertexAttrib1f");
  if (r_glVertexAttrib1f) r_glVertexAttrib1f(index, x);
}
SF void sf_glVertexAttrib2f(unsigned index, float x, float y) {
  if (!r_glVertexAttrib2f) r_glVertexAttrib2f = dlsym(RTLD_DEFAULT, "glVertexAttrib2f");
  if (r_glVertexAttrib2f) r_glVertexAttrib2f(index, x, y);
}
SF void sf_glVertexAttrib3f(unsigned index, float x, float y, float z) {
  if (!r_glVertexAttrib3f) r_glVertexAttrib3f = dlsym(RTLD_DEFAULT, "glVertexAttrib3f");
  if (r_glVertexAttrib3f) r_glVertexAttrib3f(index, x, y, z);
}
SF void sf_glVertexAttrib4f(unsigned index, float x, float y, float z, float w) {
  if (!r_glVertexAttrib4f) r_glVertexAttrib4f = dlsym(RTLD_DEFAULT, "glVertexAttrib4f");
  if (r_glVertexAttrib4f) r_glVertexAttrib4f(index, x, y, z, w);
}

struct sfent { const char *nm; void *fn; };
static const struct sfent SFTAB[] = {
  {"acos",sf_acos},{"asin",sf_asin},{"atan",sf_atan},{"cos",sf_cos},{"sin",sf_sin},{"tan",sf_tan},
  {"cosh",sf_cosh},{"sinh",sf_sinh},{"tanh",sf_tanh},{"acosh",sf_acosh},{"asinh",sf_asinh},{"atanh",sf_atanh},
  {"acosf",sf_acosf},{"asinf",sf_asinf},{"atanf",sf_atanf},{"cosf",sf_cosf},{"sinf",sf_sinf},{"tanf",sf_tanf},
  {"coshf",sf_coshf},{"sinhf",sf_sinhf},{"tanhf",sf_tanhf},
  {"exp",sf_exp},{"exp2",sf_exp2},{"log",sf_log},{"log2",sf_log2},{"log10",sf_log10},{"expm1",sf_expm1},{"log1p",sf_log1p},
  {"expf",sf_expf},{"exp2f",sf_exp2f},{"logf",sf_logf},{"log2f",sf_log2f},{"log10f",sf_log10f},
  {"sqrt",sf_sqrt},{"cbrt",sf_cbrt},{"fabs",sf_fabs},
  {"sqrtf",sf_sqrtf},{"cbrtf",sf_cbrtf},{"fabsf",sf_fabsf},
  {"ceil",sf_ceil},{"floor",sf_floor},{"round",sf_round},{"trunc",sf_trunc},{"rint",sf_rint},{"nearbyint",sf_nearbyint},
  {"ceilf",sf_ceilf},{"floorf",sf_floorf},{"roundf",sf_roundf},{"truncf",sf_truncf},{"rintf",sf_rintf},{"nearbyintf",sf_nearbyintf},
  {"atan2",sf_atan2},{"fmod",sf_fmod},{"pow",sf_pow},{"remainder",sf_remainder},{"hypot",sf_hypot},
  {"fmax",sf_fmax},{"fmin",sf_fmin},{"copysign",sf_copysign},{"fdim",sf_fdim},{"nextafter",sf_nextafter},
  {"atan2f",sf_atan2f},{"fmodf",sf_fmodf},{"powf",sf_powf},{"remainderf",sf_remainderf},{"hypotf",sf_hypotf},
  {"fmaxf",sf_fmaxf},{"fminf",sf_fminf},{"copysignf",sf_copysignf},{"fdimf",sf_fdimf},
  {"modf",sf_modf},{"modff",sf_modff},{"frexp",sf_frexp},{"frexpf",sf_frexpf},
  {"ldexp",sf_ldexp},{"ldexpf",sf_ldexpf},{"scalbn",sf_scalbn},{"scalbnf",sf_scalbnf},{"scalbln",sf_scalbln},
  {"strtod",sf_strtod},{"strtof",sf_strtof},
};

void *softfp_resolve(const char *nm){
  if(!nm) return 0;
  for(unsigned i=0;i<sizeof(SFTAB)/sizeof(SFTAB[0]);i++)
    if(!strcmp(nm,SFTAB[i].nm)) return SFTAB[i].fn;
  return 0;
}

void *softfp_gl_resolve(const char *nm){
  if(!nm) return 0;
  if(!strcmp(nm,"glBlendColor")) return sf_glBlendColor;
  if(!strcmp(nm,"glClearColor")) return sf_glClearColor;
  if(!strcmp(nm,"glClearDepthf")) return sf_glClearDepthf;
  if(!strcmp(nm,"glClearDepthfOES")) return sf_glClearDepthf;
  if(!strcmp(nm,"glDepthRangef")) return sf_glDepthRangef;
  if(!strcmp(nm,"glDepthRangefOES")) return sf_glDepthRangef;
  if(!strcmp(nm,"glLineWidth")) return sf_glLineWidth;
  if(!strcmp(nm,"glPolygonOffset")) return sf_glPolygonOffset;
  if(!strcmp(nm,"glSampleCoverage")) return sf_glSampleCoverage;
  if(!strcmp(nm,"glTexParameterf")) return sf_glTexParameterf;
  if(!strcmp(nm,"glUniform1f")) return sf_glUniform1f;
  if(!strcmp(nm,"glUniform2f")) return sf_glUniform2f;
  if(!strcmp(nm,"glUniform3f")) return sf_glUniform3f;
  if(!strcmp(nm,"glUniform4f")) return sf_glUniform4f;
  if(!strcmp(nm,"glVertexAttrib1f")) return sf_glVertexAttrib1f;
  if(!strcmp(nm,"glVertexAttrib2f")) return sf_glVertexAttrib2f;
  if(!strcmp(nm,"glVertexAttrib3f")) return sf_glVertexAttrib3f;
  if(!strcmp(nm,"glVertexAttrib4f")) return sf_glVertexAttrib4f;
  return 0;
}
