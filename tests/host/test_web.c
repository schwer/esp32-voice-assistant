// SPDX-License-Identifier: MIT
// Setup page helpers: form decoding, escaping, and the cross-site request check.
#include <string.h>
#include "web_util.h"
#include "test.h"

static void url_decoding(void)
{
    char a[] = "Boston%2C+MA";
    web_url_decode(a);
    CHECK(strcmp(a, "Boston, MA") == 0);
    char b[] = "a%2Bb%20c";
    web_url_decode(b);
    CHECK(strcmp(b, "a+b c") == 0);
    char c[] = "100%25";
    web_url_decode(c);
    CHECK(strcmp(c, "100%") == 0);
    char d[] = "bad%G1%2";                 // malformed escapes are kept as-is
    web_url_decode(d);
    CHECK(strcmp(d, "bad%G1%2") == 0);
    char e[] = "%E2%9C%93";                // UTF-8 bytes pass through
    web_url_decode(e);
    CHECK(strcmp(e, "\xE2\x9C\x93") == 0);
}

static void trimming(void)
{
    char a[] = "  AIzaKey \t\n";
    web_trim(a);
    CHECK(strcmp(a, "AIzaKey") == 0);
    char b[] = "   ";
    web_trim(b);
    CHECK(b[0] == 0);
    char c[] = "no-space";
    web_trim(c);
    CHECK(strcmp(c, "no-space") == 0);
}

static void html_escaping(void)
{
    CHECK(strcmp(web_html_entity('<'), "&lt;") == 0);
    CHECK(strcmp(web_html_entity('>'), "&gt;") == 0);
    CHECK(strcmp(web_html_entity('&'), "&amp;") == 0);
    CHECK(strcmp(web_html_entity('"'), "&quot;") == 0);
    CHECK(strcmp(web_html_entity('\''), "&#39;") == 0);
    CHECK(web_html_entity('a') == NULL && web_html_entity(' ') == NULL);
}

static void cross_site_requests_are_blocked(void)
{
    CHECK(web_same_origin(NULL, "192.168.1.50"));                         // curl, scripts
    CHECK(web_same_origin("http://192.168.1.50", "192.168.1.50"));
    CHECK(web_same_origin("http://assistant.local", "assistant.local"));
    CHECK(!web_same_origin("https://evil.example", "192.168.1.50"));       // another website
    CHECK(!web_same_origin("http://192.168.1.50:8080", "192.168.1.50"));  // another port
    CHECK(!web_same_origin("null", "192.168.1.50"));                      // sandboxed / file://
    CHECK(!web_same_origin("http://192.168.1.50", ""));                    // no Host
    CHECK(!web_same_origin("http://192.168.1.5", "192.168.1.50"));
}

int main(void)
{
    RUN(url_decoding);
    RUN(trimming);
    RUN(html_escaping);
    RUN(cross_site_requests_are_blocked);
    TEST_MAIN_END("web");
}
