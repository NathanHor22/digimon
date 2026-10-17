#pragma once
#include <time.h>
struct timeval { time_t tv_sec; long tv_usec; };
int settimeofday(const struct timeval *, const void *);
