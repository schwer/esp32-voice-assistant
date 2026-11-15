// SPDX-License-Identifier: MIT
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "web_util.h"

void web_url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

void web_trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

const char *web_html_entity(char c)
{
    switch (c) {
    case '&': return "&amp;";
    case '<': return "&lt;";
    case '>': return "&gt;";
    case '"': return "&quot;";
    case '\'': return "&#39;";
    default: return NULL;
    }
}

bool web_same_origin(const char *origin, const char *host)
{
    if (!origin) return true;                   // no Origin header: not a cross-site browser request
    if (!host || !host[0]) return false;
    const char *o = strstr(origin, "://");
    return o && strcmp(o + 3, host) == 0;
}
