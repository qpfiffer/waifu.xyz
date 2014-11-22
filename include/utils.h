// vim: noet ts=4 sw=4
#pragma once
#include <stdlib.h>
#include <math.h>

#define INT_LEN(x) floor(log10(abs(x))) + 1

int endswith(const char *string, const char *suffix);
char *strnstr(const char *haystack, const char *needle, size_t len);

const char *webm_location();
const char *db_location();
