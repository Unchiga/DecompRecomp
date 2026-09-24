#ifndef MEMORIES_SDK_MATH_H
#define MEMORIES_SDK_MATH_H
/* A mod's math.h: x87 floating point, as the game's own code uses. */

double sqrt(double x);
double sin(double x);
double cos(double x);
double atan2(double y, double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double pow(double x, double y);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double exp(double x);
double log(double x);
double log10(double x);
float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float atan2f(float y, float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float fmodf(float x, float y);
float powf(float x, float y);
float tanf(float x);
float expf(float x);
float logf(float x);

#define sqrt(x) __builtin_sqrt(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define fabs(x) __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)

#define M_PI 3.14159265358979323846

#endif
