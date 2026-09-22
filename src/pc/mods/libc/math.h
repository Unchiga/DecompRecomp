#ifndef MEMORIES_MOD_MATH_H
#define MEMORIES_MOD_MATH_H
/* A mod's <math.h> (README.md). */

#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_SQRT2 1.41421356237309504880
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

double sin(double x); float sinf(float x);
double cos(double x); float cosf(float x);
double tan(double x); float tanf(float x);
double asin(double x); float asinf(float x);
double acos(double x); float acosf(float x);
double atan(double x); float atanf(float x);
double atan2(double y, double x); float atan2f(float y, float x);
double sinh(double x); float sinhf(float x);
double cosh(double x); float coshf(float x);
double tanh(double x); float tanhf(float x);
double exp(double x); float expf(float x);
double exp2(double x); float exp2f(float x);
double log(double x); float logf(float x);
double log10(double x); float log10f(float x);
double log2(double x); float log2f(float x);
double pow(double x, double y); float powf(float x, float y);
double sqrt(double x); float sqrtf(float x);
double cbrt(double x); float cbrtf(float x);
double hypot(double x, double y); float hypotf(float x, float y);
double fabs(double x); float fabsf(float x);
double floor(double x); float floorf(float x);
double ceil(double x); float ceilf(float x);
double round(double x); float roundf(float x);
double trunc(double x); float truncf(float x);
long lround(double x); long lroundf(float x);
long long llround(double x); long long llroundf(float x);
double fmod(double x, double y); float fmodf(float x, float y);
double fmin(double x, double y); float fminf(float x, float y);
double fmax(double x, double y); float fmaxf(float x, float y);
double copysign(double x, double y); float copysignf(float x, float y);
double ldexp(double x, int e); float ldexpf(float x, int e);
double frexp(double x, int *e); float frexpf(float x, int *e);
double modf(double x, double *whole); float modff(float x, float *whole);

#endif
