// SPDX-License-Identifier: MIT
// Updates over Wi-Fi: reading the manifest and deciding (components/ota/ota_manifest.c).
#include <string.h>
#include "test.h"
#include "ota_manifest.h"

static const char *MANIFEST =
    "{\"name\":\"ESP32 Voice Assistant\",\"version\":\"2026.11.20-abc1234\",\"build\":\"abc1234\","
    "\"builds\":[{\"chipFamily\":\"ESP32-S3\",\"parts\":[]}],"
    "\"ota\":{\"layout\":2,"
    "\"app\":{\"path\":\"firmware/esp32-voice-assistant.bin\",\"size\":4024272,\"sha256\":\"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\"},"
    "\"model\":{\"path\":\"firmware/srmodels.bin\",\"size\":291168,\"sha256\":"
    "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}}}";

static void parses_the_manifest(void)
{
    ota_manifest_t m;
    CHECK(ota_parse_manifest(MANIFEST, &m));
    CHECK(!strcmp(m.version, "2026.11.20-abc1234"));
    CHECK(m.layout == OTA_LAYOUT);
    CHECK(!strcmp(m.app_path, "firmware/esp32-voice-assistant.bin") && m.app_size == 4024272);
    CHECK(!strncmp(m.app_sha256, "fedcba98", 8) && strlen(m.app_sha256) == 64);
    CHECK(!strcmp(m.model_path, "firmware/srmodels.bin") && m.model_size == 291168);
    CHECK(strlen(m.model_sha256) == 64);
}

static void rejects_manifests_without_an_update(void)
{
    ota_manifest_t m;
    CHECK(!ota_parse_manifest("{\"version\":\"2026.11.20-abc1234\",\"builds\":[]}", &m));   // older site
    CHECK(!ota_parse_manifest("{\"ota\":{\"layout\":2,\"app\":{\"path\":\"a.bin\"}}}", &m));  // no version
    CHECK(!ota_parse_manifest("{\"version\":\"x\",\"ota\":{\"app\":{\"path\":\"a.bin\"}}}", &m)); // no layout
    CHECK(!ota_parse_manifest("<html>not found</html>", &m));
    CHECK(!ota_parse_manifest("", &m));
}

static void decides_what_is_newer(void)
{
    CHECK(ota_is_newer("2026.11.17-1111111", "2026.11.20-2222222"));
    CHECK(ota_is_newer("2026.11.20-1111111", "2026.11.20-2222222"));   // same day, new build
    CHECK(!ota_is_newer("2026.11.20-2222222", "2026.11.20-2222222"));  // already on it
    CHECK(!ota_is_newer("2026.11.20-2222222", "2026.11.17-1111111"));  // no downgrades
    CHECK(!ota_is_newer("2026.12.01-1111111", "2026.11.30-2222222"));
    CHECK(ota_is_newer("2026.12.31-1111111", "2027.01.01-2222222"));   // across the year
    CHECK(ota_is_newer("717e4e5-dirty", "2026.11.20-2222222"));         // development build
    CHECK(!ota_is_newer("2026.11.20-1111111", "garbage"));
    CHECK(!ota_is_newer("2026.11.20-1111111", ""));
    CHECK(!ota_is_newer("2026.11.20-1111111", NULL));
}

static void builds_urls_next_to_the_manifest(void)
{
    char url[128];
    CHECK(ota_url_join("https://example.github.io/repo/manifest.json?t=123", "firmware/app.bin", url, sizeof(url)));
    CHECK(!strcmp(url, "https://example.github.io/repo/firmware/app.bin"));
    CHECK(ota_url_join("https://x.io/manifest.json", "a.bin", url, sizeof(url)) && !strcmp(url, "https://x.io/a.bin"));
    // Only files next to the manifest: no other sites or parent folders.
    CHECK(!ota_url_join("https://x.io/m.json", "https://evil.example/a.bin", url, sizeof(url)));
    CHECK(!ota_url_join("https://x.io/m.json", "/a.bin", url, sizeof(url)));
    CHECK(!ota_url_join("https://x.io/r/m.json", "../a.bin", url, sizeof(url)));
    CHECK(!ota_url_join("https://x.io/m.json", "", url, sizeof(url)));
    char tiny[16];
    CHECK(!ota_url_join("https://x.io/m.json", "firmware/app.bin", tiny, sizeof(tiny)));   // too long
}

int main(void)
{
    RUN(parses_the_manifest);
    RUN(rejects_manifests_without_an_update);
    RUN(decides_what_is_newer);
    RUN(builds_urls_next_to_the_manifest);
    TEST_MAIN_END("ota");
}
