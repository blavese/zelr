/* math.h -- Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Worked out here rather than taken from anywhere, which is why the list is
 * short: each of these is a series or an iteration written out, and one
 * that has not been written is not declared. */
#pragma once

#define M_PI  3.14159265358979323846
#define M_E   2.71828182845904523536
#define HUGE_VAL (__builtin_huge_val())
#define NAN      (__builtin_nanf(""))
#define INFINITY (__builtin_inff())

double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double a, double b);
double sqrt(double x);
double pow(double base, double e);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
int    isnan(double x);
int    isinf(double x);
