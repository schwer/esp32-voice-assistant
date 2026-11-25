// SPDX-License-Identifier: MIT
//
// Local setup page: http://<device-ip>/ or http://assistant.local/
// Lets the user set the Gemini API key, time zone and assistant name.
// The API key is write-only: it is never sent back to the browser.
//
// TODO: admin PIN / physical-button gate, /appointments.ics export,
//       OAuth callback + app-password forms (docs/integrations.md)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "mdns.h"
#include "settings.h"
#include "net.h"
#include "setup_web.h"
#include "web_util.h"
#include "diag.h"
#include "leds.h"
#include "diag_page.h"
#include "prompts.h"
#include "audio.h"
#include "selftest.h"
#include "voicepack.h"
#include "clock.h"
#include "ota.h"
#include "intercom.h"
#include "intercom_page.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_system.h"

static const char *TAG = "setup_web";
static httpd_handle_t s_server;
static volatile int64_t s_last_activity_us;

int64_t setup_web_last_activity_us(void) { return s_last_activity_us; }

typedef struct { const char *label; const char *posix; } tz_option_t;

static const tz_option_t TZ_OPTIONS[] = {
    { "UTC",                          "UTC0" },
    { "US Eastern (New York)",        "EST5EDT,M3.2.0,M11.1.0" },
    { "US Central (Chicago)",         "CST6CDT,M3.2.0,M11.1.0" },
    { "US Mountain (Denver)",         "MST7MDT,M3.2.0,M11.1.0" },
    { "US Arizona (Phoenix)",         "MST7" },
    { "US Pacific (Los Angeles)",     "PST8PDT,M3.2.0,M11.1.0" },
    { "US Alaska (Anchorage)",        "AKST9AKDT,M3.2.0,M11.1.0" },
    { "US Hawaii (Honolulu)",         "HST10" },
    { "Brazil (São Paulo)",           "<-03>3" },
    { "UK / Ireland (London)",        "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Central Europe (Berlin, Paris, Amsterdam)", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Eastern Europe (Athens, Helsinki)",         "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "India (Kolkata)",              "IST-5:30" },
    { "China (Shanghai)",             "CST-8" },
    { "Japan (Tokyo)",                "JST-9" },
    { "Australia East (Sydney)",      "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "New Zealand (Auckland)",       "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};

// ---------- helpers ----------

static void send_escaped(httpd_req_t *req, const char *s)
{
    char buf[128];
    size_t n = 0;
    for (; *s; s++) {
        const char *rep = web_html_entity(*s);
        if (n + 8 >= sizeof(buf)) {
            httpd_resp_send_chunk(req, buf, n);
            n = 0;
        }
        if (rep) {
            size_t l = strlen(rep);
            memcpy(&buf[n], rep, l);
            n += l;
        } else {
            buf[n++] = *s;
        }
    }
    if (n) {
        httpd_resp_send_chunk(req, buf, n);
    }
}

#define SEND(req, s) httpd_resp_sendstr_chunk((req), (s))

static bool form_field(const char *body, const char *key, char *out, size_t len)
{
    if (httpd_query_key_value(body, key, out, len) != ESP_OK) {
        return false;
    }
    web_url_decode(out);
    web_trim(out);
    return true;
}

/**
 * Reject cross-site requests: another website open in the user's browser must not
 * be able to change settings or reboot the device. The browser always sends Origin
 * on cross-site POSTs, so it must match the Host we were reached on.
 */
static bool same_origin(httpd_req_t *req)
{
    char origin[96] = "", host[64] = "";
    bool has_origin = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK;
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    return web_same_origin(has_origin ? origin : NULL, host);
}

static esp_err_t forbid(httpd_req_t *req)
{
    ESP_LOGW(TAG, "blocked cross-site request to %s", req->uri);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request blocked");
    return ESP_FAIL;
}

// ---------- voice recording progress (setup + diagnostics pages) ----------

// Card with a progress bar; polls /voice/progress while a recording job runs.
static const char VOICE_PROGRESS_HTML[] =
"<div class=card id=vp hidden><strong id=vptitle>Recording voice messages</strong>"
"<div style='height:10px;background:var(--border);border-radius:99px;overflow:hidden;margin:12px 0 8px'>"
"<div id=vpbar style='height:100%;width:0;background:var(--accent);transition:width .4s'></div></div>"
"<p id=vptext class=note style='margin:0'></p></div>"
"<script>(function(){const $=i=>document.getElementById(i);let seen=false;"
"async function poll(){let p;try{p=await (await fetch('/voice/progress',{cache:'no-store'})).json()}"
"catch(e){return setTimeout(poll,2000)}"
"const active=p.busy||p.pending;"
"if(active)seen=true;"
"if(!active&&!seen&&!location.search.includes('rec=1'))return;"
"$('vp').hidden=false;"
"const pct=p.total?Math.round(100*p.done/p.total):0;$('vpbar').style.width=(active?pct:(p.error?pct:100))+'%';"
"if(active){$('vptitle').textContent='Recording voice messages in '+p.voice+'\\u2019s voice\\u2026';"
"$('vptext').textContent=p.done+' of '+p.total+' messages recorded. '+(p.busy?'Keep the board online; this takes about a minute.':p.status);"
"setTimeout(poll,1000)}"
"else if(p.error){$('vptitle').textContent='Voice recording stopped';$('vptext').className='note bad';$('vptext').textContent=p.status}"
"else if(seen||location.search.includes('rec=1')){if(!seen){seen=true;return setTimeout(poll,1000)}"
"$('vptitle').textContent='\\u2714 Finished: everything now uses '+p.active_voice+'\\u2019s voice';"
"$('vptext').innerHTML='';const a=document.createElement('a');a.href='/diag#voice-messages';"
"a.textContent='Hear the new messages on the diagnostics page \\u2192';$('vptext').append(a)}}"
"poll()})()</script>";

static esp_err_t get_voice_progress(httpd_req_t *req)
{
    voicepack_progress_t p;
    voicepack_progress(&p);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "busy", p.busy);
    cJSON_AddBoolToObject(r, "pending", p.pending);
    cJSON_AddNumberToObject(r, "done", p.done);
    cJSON_AddNumberToObject(r, "total", p.total);
    cJSON_AddStringToObject(r, "voice", p.voice);
    cJSON_AddStringToObject(r, "active_voice", p.active_voice);
    cJSON_AddStringToObject(r, "error", p.error);
    cJSON_AddStringToObject(r, "status", p.status);
    char *json = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return err;
}

// ---------- page ----------

static const char PAGE_HEAD[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Assistant Setup</title><style>"
":root{--bg:#f7f7f5;--fg:#1d1d1b;--muted:#5f5f5a;--card:#fff;--border:#e2e2dc;"
"--accent:#2f6fde;--ok-bg:#e7f5ea;--ok-fg:#1d5b2c;--warn-bg:#fff6e0;--warn-fg:#6b4b00}"
"@media(prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ececea;--muted:#a3a39d;"
"--card:#1f1f1e;--border:#33332f;--accent:#7aa7ff;--ok-bg:#16301d;--ok-fg:#a6e2b3;"
"--warn-bg:#2e2716;--warn-fg:#f0d58c}}"
"body{margin:0;background:var(--bg);color:var(--fg);"
"font:16px/1.5 system-ui,-apple-system,'Segoe UI',sans-serif}"
"main{max-width:560px;margin:0 auto;padding:32px 16px 48px}"
"h1{font-size:1.5rem;margin:0 0 4px}.muted{color:var(--muted)}"
".card{background:var(--card);border:1px solid var(--border);border-radius:12px;"
"padding:20px;margin:20px 0}"
"label{display:block;font-weight:600;margin:16px 0 6px}"
"label.check{display:flex;align-items:center;gap:10px}label.check input{width:auto;margin:0}"
"input,select{width:100%;box-sizing:border-box;padding:10px 12px;font:inherit;"
"color:var(--fg);background:var(--bg);border:1px solid var(--border);border-radius:8px}"
"button{margin-top:20px;padding:10px 18px;font:inherit;font-weight:600;border:0;"
"border-radius:8px;background:var(--accent);color:#fff;cursor:pointer}"
".note{font-size:.9rem;color:var(--muted);margin-top:6px}"
".ok,.warn{border-radius:8px;padding:12px 14px}"
".ok{background:var(--ok-bg);color:var(--ok-fg)}.warn{background:var(--warn-bg);color:var(--warn-fg)}"
"dl{display:grid;grid-template-columns:auto 1fr;gap:4px 16px;margin:0}dt{color:var(--muted)}"
"dd{margin:0;overflow-wrap:anywhere}a{color:var(--accent)}"
"</style></head><body><main>";

static esp_err_t get_root(httpd_req_t *req)
{
    s_last_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "GET %s", req->uri);
    char q[32] = {0}, url[64] = "", hint[8] = "", now[40] = "";
    bool saved = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strstr(q, "saved=1");
    bool failed = strstr(q, "error=1") != NULL;
    net_device_url(url, sizeof(url));
    settings_api_key_hint(hint, sizeof(hint));

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year > 120) {
        strftime(now, sizeof(now), "%a %d %b %Y, %H:%M", &tm);
    } else {
        strlcpy(now, "not synced yet", sizeof(now));
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    SEND(req, PAGE_HEAD);
    SEND(req, "<h1>Voice Assistant Setup</h1><p class=muted>Settings are stored only on this device.</p>");

    if (saved) SEND(req, "<p class=ok>Saved.</p>");
    SEND(req, VOICE_PROGRESS_HTML);
    if (failed) SEND(req, "<p class=warn>Couldn't save. Check the values and try again.</p>");
    if (strstr(q, "wifi=added")) SEND(req, "<p class=ok>Network added.</p>");
    if (strstr(q, "wifi=forgot")) SEND(req, "<p class=ok>Network forgotten.</p>");
    if (strstr(q, "wifi=switching")) SEND(req, "<p class=ok>Switching networks. If it's a different network, the "
                                              "board will be at a new address there.</p>");
    if (strstr(q, "wifi=bad")) SEND(req, "<p class=warn>Couldn't add that network: the name must be 1-32 "
                                         "characters and the password blank or 8-63 characters.</p>");
    if (!settings_has_api_key()) {
        SEND(req, "<p class=warn>Add your Gemini API key to start using the assistant. "
                  "<a href='https://aistudio.google.com/apikey' target=_blank rel=noopener>Get a key</a></p>");
    }

    SEND(req, "<form class=card method=post action=/save>");

    SEND(req, "<label for=api_key>Gemini API key</label>"
              "<input id=api_key name=api_key type=password autocomplete=off spellcheck=false placeholder='");
    if (hint[0]) {
        SEND(req, "Saved (ends in ");
        send_escaped(req, hint);
        SEND(req, "). Leave blank to keep it.");
    } else {
        SEND(req, "AIza...");
    }
    SEND(req, "'><p class=note>Only sent to Google. This page never shows it again.</p>");

    SEND(req, "<label for=tz>Time zone</label><select id=tz name=tz>");
    const char *cur = settings_get_tz();
    bool matched = false;
    for (size_t i = 0; i < sizeof(TZ_OPTIONS) / sizeof(TZ_OPTIONS[0]); i++) {
        bool sel = strcmp(cur, TZ_OPTIONS[i].posix) == 0;
        matched |= sel;
        SEND(req, "<option value=\"");
        send_escaped(req, TZ_OPTIONS[i].posix);
        SEND(req, sel ? "\" selected>" : "\">");
        send_escaped(req, TZ_OPTIONS[i].label);
        SEND(req, "</option>");
    }
    if (!matched) {
        SEND(req, "<option value=\"");
        send_escaped(req, cur);
        SEND(req, "\" selected>Custom: ");
        send_escaped(req, cur);
        SEND(req, "</option>");
    }
    SEND(req, "</select>");

    SEND(req, "<label for=name>Assistant name</label><input id=name name=name maxlength=31 value=\"");
    send_escaped(req, settings_get_name());
    SEND(req, "\"><p class=note>What the assistant calls itself. The wake word stays \"Jarvis\".</p>");

    SEND(req, "<label for=location>Home location</label><input id=location name=location maxlength=63 "
              "placeholder='e.g. Boston, MA' value=\"");
    send_escaped(req, settings_get_location());
    SEND(req, "\"><p class=note>Used when you ask about the weather without naming a place.</p>");
    SEND(req, "<label for=units>Temperature</label><select id=units name=units>");
    SEND(req, settings_get_fahrenheit() ? "<option value=F selected>Fahrenheit</option><option value=C>Celsius</option>"
                                        : "<option value=F>Fahrenheit</option><option value=C selected>Celsius</option>");
    SEND(req, "</select>");

    static const char *const VOICES[] = { "Puck", "Charon", "Kore", "Fenrir", "Aoede", "Leda", "Orus", "Zephyr" };
    SEND(req, "<label for=voice>Voice</label><select id=voice name=voice>");
    const char *cur_voice = settings_get_voice();
    bool voice_found = false;
    for (size_t i = 0; i < sizeof(VOICES) / sizeof(VOICES[0]); i++) {
        bool sel = strcmp(cur_voice, VOICES[i]) == 0;
        voice_found |= sel;
        SEND(req, "<option");
        if (sel) SEND(req, " selected");
        SEND(req, ">");
        SEND(req, VOICES[i]);
        SEND(req, "</option>");
    }
    if (!voice_found) {
        SEND(req, "<option selected>");
        send_escaped(req, cur_voice);
        SEND(req, "</option>");
    }
    SEND(req, "</select><p class=note>The board uses one voice for everything. When you save a new voice, "
              "the board records all of its own messages (\"Yes?\", \"Settings saved\", ...) in it right away; "
              "you'll see the progress at the top of this page.</p>");

    SEND(req, "<label for=model>Gemini model</label><input id=model name=model maxlength=63 "
              "spellcheck=false autocomplete=off value=\"");
    send_escaped(req, settings_get_model());
    SEND(req, "\"><p class=note>Leave as is unless Google retires it. Use <em>Test Gemini</em> "
              "on the diagnostics page to check it.</p>");

    SEND(req, "<label class=check><input type=checkbox name=barge value=1");
    if (settings_get_barge_in()) SEND(req, " checked");
    SEND(req, "> Let me interrupt him</label>"
              "<p class=note>Start talking while he's answering and he stops to listen. If he keeps cutting "
              "himself off, the board's echo cancellation needs adjusting: it then pauses interruptions by itself.</p>");
    SEND(req, "<label class=check><input type=checkbox name=proactive value=1");
    if (settings_get_proactive()) SEND(req, " checked");
    SEND(req, "> Only answer when spoken to</label>"
              "<p class=note>He ignores the TV, background noise and people talking to each other. "
              "Turns itself off if the selected model doesn't support it.</p>");

    SEND(req, "<label class=check><input type=checkbox name=saver value=1");
    if (settings_get_battery_saver()) SEND(req, " checked");
    SEND(req, "> Battery saver</label>"
              "<p class=note>On battery, Wi-Fi turns off after 3 quiet minutes and back on when you say "
              "\"Jarvis\". This page is unreachable while it's off. On USB power Wi-Fi always stays on.</p>");

    SEND(req, "<label class=check><input type=checkbox name=open_wifi value=1");
    if (settings_get_open_wifi()) SEND(req, " checked");
    SEND(req, "> Use free Wi-Fi with no password</label>"
              "<p class=note>When none of your networks (below) is around, join an open network, but only "
              "if it really reaches the internet: cafés and airports that want you to click \"Accept\" on a "
              "page are skipped. On a public network this page switches itself off so strangers can't "
              "change your settings.</p>");

    SEND(req, "<label class=check><input type=checkbox name=search value=1");
    if (settings_get_search()) SEND(req, " checked");
    SEND(req, "> Let Gemini search the web</label>"
              "<p class=note>For news, sports, opening hours and other current information. "
              "Turns itself off if the selected model doesn't support it.</p>");

    SEND(req, "<input type=hidden name=spoken_present value=1>"
              "<label class=check><input type=checkbox name=spoken value=1");
    if (settings_get_spoken_status()) SEND(req, " checked");
    SEND(req, "> Speak status messages</label>"
              "<p class=note>Say what the lights mean, e.g. \"I need Wi-Fi\" or \"Settings saved\".</p>");

    SEND(req, "<button type=submit>Save</button></form>");

    // Wi-Fi networks: home, phone hotspot, work, ...
    SEND(req, "<div class=card id=wifi><h2 style='margin-top:0'>Wi-Fi networks</h2>"
              "<p class=note style='margin-top:0'>The board joins whichever of these is around, strongest first "
              "(<em>Use now</em> switches straight away). Passwords are stored encrypted. If none is around, "
              "the board opens its own <em>Jarvis-Setup</em> network so you can set it up from a phone.</p>");
    char (*nets)[NET_SSID_MAX] = calloc(NET_MAX_SAVED, NET_SSID_MAX);
    int nn = nets ? net_saved_networks(nets, NET_MAX_SAVED) : 0;
    char cur_ssid[NET_SSID_MAX] = "";
    net_current_ssid(cur_ssid, sizeof(cur_ssid));
    for (int i = 0; i < nn; i++) {
        SEND(req, "<form method=post action=/wifi/forget class=netrow "
                  "style='display:flex;justify-content:space-between;align-items:center;gap:8px;margin:6px 0'>"
                  "<span>");
        send_escaped(req, nets[i]);
        if (!strcmp(nets[i], cur_ssid)) SEND(req, " <small class=ok>(connected)</small>");
        SEND(req, "</span><input type=hidden name=ssid value=\"");
        send_escaped(req, nets[i]);
        SEND(req, "\"><span style='display:flex;gap:6px;flex:none'>");
        if (strcmp(nets[i], cur_ssid) != 0) {
            SEND(req, "<button type=submit formaction=/wifi/select style='margin:0'>Use now</button>");
        }
        SEND(req, "<button type=submit style='margin:0'"
                  " onclick=\"return confirm('Forget this network?')\">Forget</button></span></form>");
    }
    free(nets);
    if (!nn) SEND(req, "<p class=note>None saved yet.</p>");
    SEND(req, "<form method=post action=/wifi/add style='margin-top:12px'>"
              "<label for=nssid>Add a network</label>"
              "<input id=nssid name=ssid maxlength=32 required placeholder='Network name' autocomplete=off "
              "autocapitalize=none spellcheck=false>"
              "<input name=pass type=password maxlength=63 placeholder='Password (blank if none)' "
              "autocomplete=new-password style='margin-top:6px'>"
              "<p class=note>It doesn't need to be switched on now: the board uses it whenever it's around. "
              "Names are case-sensitive; the board only sees 2.4 GHz networks (on iPhone, turn on "
              "\"Maximise Compatibility\").</p>"
              "<button type=submit>Add network</button></form></div>");

    const esp_app_desc_t *app = esp_app_get_description();
    SEND(req, "<div class=card><dl><dt>Address</dt><dd>");
    send_escaped(req, url[0] ? url : "-");
    SEND(req, "</dd><dt>Device time</dt><dd>");
    send_escaped(req, now);
    SEND(req, "</dd><dt>Firmware</dt><dd>");
    send_escaped(req, app->version);
    SEND(req, "</dd></dl><p style='margin:14px 0 0'><a href='/diag'>Diagnostics &rarr;</a></p>"
              "</div></main></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/** Read a small form body. Returns false (and replies) if it's too large or broken. */
static bool read_form(httpd_req_t *req, char *body, size_t len)
{
    if (req->content_len >= len) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Too large");
        return false;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return false;
        got += r;
    }
    body[got] = 0;
    return true;
}

static esp_err_t redirect(httpd_req_t *req, const char *to)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", to);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t post_wifi_add(httpd_req_t *req)
{
    if (!same_origin(req)) return forbid(req);
    char body[256], ssid[NET_SSID_MAX] = "", pass[NET_PASS_MAX] = "";
    if (!read_form(req, body, sizeof(body))) return ESP_FAIL;
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    esp_err_t err = net_add_network(ssid, pass);
    memset(body, 0, sizeof(body));
    memset(pass, 0, sizeof(pass));
    if (err == ESP_OK) {
        leds_event(LEDS_EVENT_SAVED);
        prompts_say(PROMPT_SETTINGS_SAVED);
    }
    return redirect(req, err == ESP_OK ? "/?wifi=added#wifi" : "/?wifi=bad#wifi");
}

static esp_err_t post_wifi_forget(httpd_req_t *req)
{
    if (!same_origin(req)) return forbid(req);
    char body[160], ssid[NET_SSID_MAX] = "";
    if (!read_form(req, body, sizeof(body))) return ESP_FAIL;
    form_field(body, "ssid", ssid, sizeof(ssid));
    if (net_forget(ssid) == ESP_OK) leds_event(LEDS_EVENT_DELETED);
    return redirect(req, "/?wifi=forgot#wifi");
}

static esp_err_t post_wifi_select(httpd_req_t *req)
{
    if (!same_origin(req)) return forbid(req);
    char body[160], ssid[NET_SSID_MAX] = "";
    if (!read_form(req, body, sizeof(body))) return ESP_FAIL;
    form_field(body, "ssid", ssid, sizeof(ssid));
    esp_err_t err = net_select(ssid);
    if (err == ESP_OK) leds_event(LEDS_EVENT_SAVED);
    return redirect(req, err == ESP_OK ? "/?wifi=switching#wifi" : "/?wifi=bad#wifi");
}

static esp_err_t post_save(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /save (%d bytes)", (int)req->content_len);
    if (!same_origin(req)) {
        return forbid(req);
    }
    char body[1024];
    if (req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Too large");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = 0;

    esp_err_t err = ESP_OK;
    char val[SETTINGS_API_KEY_MAX + 16];

    if (form_field(body, "api_key", val, sizeof(val)) && val[0]) {
        err |= settings_set_api_key(val);
    }
    if (form_field(body, "tz", val, sizeof(val)) && val[0]) {
        if (settings_set_tz(val) == ESP_OK) {
            setenv("TZ", val, 1);
            tzset();
        } else {
            err = ESP_FAIL;
        }
    }
    if (form_field(body, "name", val, sizeof(val)) && val[0]) {
        err |= settings_set_name(val);
    }
    if (form_field(body, "voice", val, sizeof(val)) && val[0] && strcmp(val, settings_get_voice()) != 0) {
        err |= settings_set_voice(val);
    }
    if (form_field(body, "model", val, sizeof(val)) && val[0]) {
        const char *m = strncmp(val, "models/", 7) == 0 ? val + 7 : val;   // accept either form
        err |= settings_set_model(m);
    }
    if (form_field(body, "location", val, sizeof(val))) {
        err |= settings_set_location(val);                // empty clears it
    }
    if (form_field(body, "units", val, sizeof(val)) && val[0]) {
        err |= settings_set_fahrenheit(val[0] == 'F');
    }
    if (form_field(body, "spoken_present", val, sizeof(val))) {
        bool on = form_field(body, "spoken", val, sizeof(val)) && strcmp(val, "1") == 0;
        err |= settings_set_spoken_status(on);
        bool search = form_field(body, "search", val, sizeof(val)) && strcmp(val, "1") == 0;
        err |= settings_set_search(search);
        bool saver = form_field(body, "saver", val, sizeof(val)) && strcmp(val, "1") == 0;
        err |= settings_set_battery_saver(saver);
        bool open_wifi = form_field(body, "open_wifi", val, sizeof(val)) && strcmp(val, "1") == 0;
        err |= settings_set_open_wifi(open_wifi);
        net_set_allow_open(open_wifi);
        bool barge = form_field(body, "barge", val, sizeof(val)) && strcmp(val, "1") == 0;
        err |= settings_set_barge_in(barge);
        bool proactive = form_field(body, "proactive", val, sizeof(val)) && strcmp(val, "1") == 0;
        err |= settings_set_proactive(proactive);
    }
    memset(body, 0, sizeof(body));
    memset(val, 0, sizeof(val));
    // One voice: if the board's messages aren't in the selected voice (a new voice, or the
    // first save with an API key), record them now. The page shows the progress.
    bool recording = false;
    if (err == ESP_OK && settings_has_api_key() && !voicepack_matches_selected() &&
        !(voicepack_pending() && strcmp(voicepack_progress_voice(), settings_get_voice()) == 0)) {
        recording = voicepack_request(false) == ESP_OK;
    } else if (voicepack_pending()) {
        recording = true;                              // already recording this voice
    }
    if (err == ESP_OK) {
        leds_event(LEDS_EVENT_SAVED);
        prompts_say(PROMPT_SETTINGS_SAVED);
        if (leds_get() == LEDS_SETUP_NEEDED && settings_has_api_key() && net_is_connected()) {
            leds_set(LEDS_OFF);
        }
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", err != ESP_OK ? "/?error=1" : recording ? "/?saved=1&rec=1" : "/?saved=1");
    return httpd_resp_send(req, NULL, 0);
}

// ---------- diagnostics ----------

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *s = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

/** Diagnostics actions must come from our own page (custom header => no cross-site). */
static bool diag_action_allowed(httpd_req_t *req)
{
    char v[4];
    return same_origin(req) &&
           httpd_req_get_hdr_value_str(req, "X-Assistant", v, sizeof(v)) == ESP_OK;
}

static esp_err_t get_diag(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, DIAG_PAGE, sizeof(DIAG_PAGE) - 1);
}

static esp_err_t get_diag_json(httpd_req_t *req)
{
    s_last_activity_us = esp_timer_get_time();
    cJSON *d = diag_snapshot();
    cJSON_AddItemToObject(d, "clock", clock_snapshot());
    ota_status_t st = ota_last_status();
    cJSON *up = cJSON_AddObjectToObject(d, "updates");
    cJSON_AddStringToObject(up, "this_version", st.current);
    if (st.ok) {
        cJSON_AddStringToObject(up, "latest_version", st.latest);
        if (st.notes[0]) cJSON_AddStringToObject(up, "whats_new", st.notes);
        cJSON_AddStringToObject(up, "status", st.available ? "update available: press Install update, or say \"Jarvis, update yourself\""
                                              : st.needs_usb ? st.why : "up to date");
    } else {
        cJSON_AddStringToObject(up, "status", st.checked ? st.why : "not checked yet");
    }
    return send_json(req, d);
}

static esp_err_t get_diag_logs(httpd_req_t *req)
{
    const size_t len = 8 * 1024 + 1;
    char *buf = malloc(len);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    size_t n = diag_logs(buf, len);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static esp_err_t post_test_gemini(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    return send_json(req, diag_test_gemini());
}

static esp_err_t get_leds_json(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *states = cJSON_AddArrayToObject(r, "states");
    for (int i = 0; i < LEDS_STATE_COUNT; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", leds_state_name((leds_state_t)i));
        cJSON_AddStringToObject(o, "description", leds_state_description((leds_state_t)i));
        cJSON_AddItemToArray(states, o);
    }
    cJSON *events = cJSON_AddArrayToObject(r, "events");
    for (int i = 0; i < LEDS_EVENT_COUNT; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", leds_event_name((leds_event_t)i));
        cJSON_AddStringToObject(o, "description", leds_event_description((leds_event_t)i));
        cJSON_AddItemToArray(events, o);
    }
    cJSON_AddStringToObject(r, "current", leds_state_name(leds_get()));
    return send_json(req, r);
}

static esp_err_t post_led_preview(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    char q[48] = "", name[24] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "name", name, sizeof(name));
    bool ok = leds_preview(name);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddStringToObject(r, "message", ok ? "Showing it on the ring for a few seconds."
                                             : "Unknown light pattern.");
    return send_json(req, r);
}

static esp_err_t post_speaker_test(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    prompts_say_always(PROMPT_SPEAKER_TEST);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "message",
        "Playing a test message. If you hear nothing, check the Speaker line under Modules.");
    return send_json(req, r);
}

static esp_err_t post_update_check(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    ota_status_t st;
    ota_check(&st);
    char msg[800];
    if (!st.ok) snprintf(msg, sizeof(msg), "Couldn't check: %s.", st.why);
    else if (st.available) snprintf(msg, sizeof(msg), "Version %s is available (this is %s). %s%s%sPress Install update to install it.",
                                    st.latest, st.current, st.notes[0] ? "What's new: " : "", st.notes,
                                    st.notes[0] ? " " : "");
    else if (st.needs_usb) snprintf(msg, sizeof(msg), "Version %s is available, but %s: install it from the web install page with a USB cable.", st.latest, st.why);
    else snprintf(msg, sizeof(msg), "Up to date (%s).", st.current);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", st.ok);
    cJSON_AddStringToObject(r, "message", msg);
    return send_json(req, r);
}

static esp_err_t post_update_install(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    ota_status_t st;
    ota_check(&st);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", st.available);
    if (st.available) {
        ota_request_install();
        cJSON_AddStringToObject(r, "message", "Installing. The board says when it starts and when it's done "
                                "(about a minute); this page reconnects by itself.");
    } else {
        cJSON_AddStringToObject(r, "message", st.needs_usb ? "This update needs the web install page (USB)."
                                              : st.ok ? "Already up to date." : "Couldn't reach the update site.");
    }
    return send_json(req, r);
}

// ---------- intercom

static esp_err_t get_intercom(httpd_req_t *req)
{
    s_last_activity_us = esp_timer_get_time();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INTERCOM_PAGE, sizeof(INTERCOM_PAGE) - 1);
}

static esp_err_t intercom_reply(httpd_req_t *req, esp_err_t err, const char *fail_msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", err == ESP_OK);
    if (err != ESP_OK) cJSON_AddStringToObject(r, "message", fail_msg);
    return send_json(req, r);
}

static esp_err_t post_intercom_say(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    s_last_activity_us = esp_timer_get_time();
    char body[INTERCOM_TEXT_BYTES * 3 + 16], text[INTERCOM_TEXT_BYTES + 1] = "";
    if (!read_form(req, body, sizeof(body))) return ESP_FAIL;
    form_field(body, "text", text, sizeof(text));
    esp_err_t err = intercom_say(text);
    return intercom_reply(req, err, err == ESP_ERR_INVALID_ARG ? "Type a message of up to 300 characters."
                                                                : "Jarvis is still busy with the last message.");
}

static int form_id(httpd_req_t *req)
{
    char body[32], v[12] = "";
    if (!read_form(req, body, sizeof(body))) return -1;
    form_field(body, "id", v, sizeof(v));
    return atoi(v);
}

static esp_err_t post_intercom_replay(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    esp_err_t err = intercom_replay(form_id(req));
    return intercom_reply(req, err, err == ESP_ERR_NOT_FOUND ? "That message is gone." : "Jarvis is busy. Try again in a moment.");
}

static esp_err_t post_intercom_delete(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    esp_err_t err = intercom_delete(form_id(req));
    if (err == ESP_OK) leds_event(LEDS_EVENT_DELETED);
    return intercom_reply(req, err, "That message is gone.");
}

static esp_err_t get_intercom_status(httpd_req_t *req) { return send_json(req, intercom_status()); }
static esp_err_t get_intercom_messages(httpd_req_t *req) { return send_json(req, intercom_list()); }

static esp_err_t post_stop_ringing(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    bool was = clock_ringing();
    clock_stop_ringing();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "message", was ? "Stopped." : "Nothing is ringing.");
    return send_json(req, r);
}

static esp_err_t mic_test_json(httpd_req_t *req, esp_err_t start_err)
{
    float secs = 0;
    selftest_state_t st = selftest_mic_state(&secs);
    cJSON *r = cJSON_CreateObject();
    const char *state = st == SELFTEST_RUNNING ? "running"
                      : st == SELFTEST_HEARD ? "heard"
                      : st == SELFTEST_NOTHING ? "nothing" : "idle";
    cJSON_AddStringToObject(r, "state", state);
    cJSON_AddNumberToObject(r, "seconds", secs);
    if (start_err == ESP_ERR_NOT_SUPPORTED) {
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "message", "The microphones aren't running. Check the log.");
    } else if (start_err == ESP_ERR_INVALID_STATE) {
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "message", "The board is busy (in a conversation or another test). Try again in a moment.");
    } else if (start_err != ESP_OK) {
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "message", "Couldn't start the test.");
    }
    return send_json(req, r);
}

static esp_err_t post_mic_test(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    return mic_test_json(req, selftest_mic_start());
}

static esp_err_t get_mic_test(httpd_req_t *req)
{
    return mic_test_json(req, ESP_OK);
}

static esp_err_t get_messages_json(httpd_req_t *req)
{
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < PROMPT_COUNT; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", prompts_id((prompt_t)i));
        cJSON_AddStringToObject(o, "text", prompts_text((prompt_t)i));
        char src[64];
        if (prompts_is_recorded((prompt_t)i)) snprintf(src, sizeof(src), "%s's voice", voicepack_active_voice());
        else snprintf(src, sizeof(src), "built-in clip");
        cJSON_AddStringToObject(o, "source", src);
        cJSON_AddItemToArray(list, o);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "messages", list);
    cJSON_AddStringToObject(r, "voice", voicepack_active_voice());
    char vs[160];
    voicepack_status(vs, sizeof(vs));
    cJSON_AddStringToObject(r, "status", vs);
    return send_json(req, r);
}

static volatile bool s_playing_all;

static void play_all_task(void *arg)
{
    for (int i = 0; i < PROMPT_COUNT && s_playing_all; i++) {
        prompts_say_always((prompt_t)i);
        audio_wait_idle(10000);
    }
    s_playing_all = false;
    vTaskDelete(NULL);
}

static esp_err_t post_play(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    char q[48] = "", id[24] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "id", id, sizeof(id));
    cJSON *r = cJSON_CreateObject();
    if (strcmp(id, "all") == 0) {
        if (!s_playing_all) {
            s_playing_all = true;
            if (xTaskCreate(play_all_task, "play_all", 3072, NULL, 3, NULL) != pdPASS) s_playing_all = false;
        }
        cJSON_AddBoolToObject(r, "ok", s_playing_all);
        cJSON_AddStringToObject(r, "message", "Playing every message in order.");
        return send_json(req, r);
    }
    if (strcmp(id, "stop") == 0) {
        s_playing_all = false;
        audio_flush_playback();
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddStringToObject(r, "message", "Stopped.");
        return send_json(req, r);
    }
    for (int i = 0; i < PROMPT_COUNT; i++) {
        if (strcmp(id, prompts_id((prompt_t)i)) == 0) {
            prompts_say_always((prompt_t)i);
            cJSON_AddBoolToObject(r, "ok", true);
            cJSON_AddStringToObject(r, "message", "Playing.");
            return send_json(req, r);
        }
    }
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "message", "Unknown message.");
    return send_json(req, r);
}

static esp_err_t post_voice_rerecord(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    esp_err_t err = settings_has_api_key() ? voicepack_request(true) : ESP_ERR_INVALID_STATE;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", err == ESP_OK);
    char msg[160];
    snprintf(msg, sizeof(msg), err == ESP_ERR_INVALID_STATE ? "Add your Gemini API key on the setup page first."
             : "Recording every message again in %s's voice.", settings_get_voice());
    cJSON_AddStringToObject(r, "message", err == ESP_OK ? msg : "Couldn't start right now. Try again in a moment.");
    return send_json(req, r);
}

static void do_restart(TimerHandle_t t) { esp_restart(); }

static esp_err_t post_reboot(httpd_req_t *req)
{
    if (!diag_action_allowed(req)) return forbid(req);
    ESP_LOGW(TAG, "reboot requested from diagnostics page");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "message", "Rebooting... this page reconnects automatically.");
    esp_err_t err = send_json(req, r);
    TimerHandle_t t = xTimerCreate("reboot", pdMS_TO_TICKS(500), pdFALSE, NULL, do_restart);
    if (t) xTimerStart(t, 0);
    return err;
}

// ---------- lifecycle ----------

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable");
        return;
    }
    mdns_hostname_set("assistant");
    mdns_instance_name_set("Voice Assistant");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t setup_web_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 10240;              // the API key test does a TLS handshake here
    cfg.lru_purge_enable = true;         // a new browser connection closes the oldest idle one
    cfg.max_open_sockets = 5;            // leave sockets for Gemini, weather, time and mDNS
    cfg.max_uri_handlers = 40;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_root };
    static const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = post_save };
    static const httpd_uri_t handlers[] = {
        { .uri = "/diag",             .method = HTTP_GET,  .handler = get_diag },
        { .uri = "/diag.json",        .method = HTTP_GET,  .handler = get_diag_json },
        { .uri = "/diag/logs",        .method = HTTP_GET,  .handler = get_diag_logs },
        { .uri = "/diag/test-gemini", .method = HTTP_POST, .handler = post_test_gemini },
        { .uri = "/diag/leds.json",   .method = HTTP_GET,  .handler = get_leds_json },
        { .uri = "/diag/led-preview", .method = HTTP_POST, .handler = post_led_preview },
        { .uri = "/diag/reboot",      .method = HTTP_POST, .handler = post_reboot },
        { .uri = "/diag/speaker-test",.method = HTTP_POST, .handler = post_speaker_test },
        { .uri = "/diag/mic-test",    .method = HTTP_POST, .handler = post_mic_test },
        { .uri = "/diag/stop-ringing",.method = HTTP_POST, .handler = post_stop_ringing },
        { .uri = "/diag/update-check",.method = HTTP_POST, .handler = post_update_check },
        { .uri = "/diag/update-install",.method = HTTP_POST, .handler = post_update_install },
        { .uri = "/intercom",         .method = HTTP_GET,  .handler = get_intercom },
        { .uri = "/intercom/say",     .method = HTTP_POST, .handler = post_intercom_say },
        { .uri = "/intercom/replay",  .method = HTTP_POST, .handler = post_intercom_replay },
        { .uri = "/intercom/delete",  .method = HTTP_POST, .handler = post_intercom_delete },
        { .uri = "/intercom/status",  .method = HTTP_GET,  .handler = get_intercom_status },
        { .uri = "/intercom/messages.json", .method = HTTP_GET, .handler = get_intercom_messages },
        { .uri = "/wifi/add",         .method = HTTP_POST, .handler = post_wifi_add },
        { .uri = "/wifi/select",      .method = HTTP_POST, .handler = post_wifi_select },
        { .uri = "/wifi/forget",      .method = HTTP_POST, .handler = post_wifi_forget },
        { .uri = "/diag/messages.json", .method = HTTP_GET, .handler = get_messages_json },
        { .uri = "/diag/play",        .method = HTTP_POST, .handler = post_play },
        { .uri = "/diag/voice-record",.method = HTTP_POST, .handler = post_voice_rerecord },
        { .uri = "/voice/progress",   .method = HTTP_GET,  .handler = get_voice_progress },
        { .uri = "/diag/mic-test",    .method = HTTP_GET,  .handler = get_mic_test },
    };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &save);
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        httpd_register_uri_handler(s_server, &handlers[i]);
    }
    start_mdns();
    char url[64] = "";
    net_device_url(url, sizeof(url));
    ESP_LOGI(TAG, "setup page running: %s  (or http://assistant.local/)", url[0] ? url : "(no IP yet)");
    return ESP_OK;
}

esp_err_t setup_web_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    mdns_free();
    return ESP_OK;
}
