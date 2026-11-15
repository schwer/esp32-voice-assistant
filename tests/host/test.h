// SPDX-License-Identifier: MIT
// Minimal test helpers for the host unit tests (no framework needed).
#pragma once
#include <math.h>
#include <stdio.h>

static int t_checks, t_failures;
static const char *t_current;

#define CHECK(cond) do { t_checks++; if (!(cond)) { t_failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, t_current, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { t_checks++; double _a = (a), _b = (b); if (fabs(_a - _b) > (eps)) { \
    t_failures++; printf("  FAIL %s:%d in %s: %s = %g, expected %g +/- %g\n", __FILE__, __LINE__, \
    t_current, #a, _a, _b, (double)(eps)); } } while (0)
#define RUN(fn) do { t_current = #fn; fn(); } while (0)
#define TEST_MAIN_END(name) do { printf("%s: %d checks, %d failed\n", name, t_checks, t_failures); \
    return t_failures ? 1 : 0; } while (0)
