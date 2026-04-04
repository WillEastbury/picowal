#include "web_server.h"
#include "kv_flash.h"
#include "metadata_dict.h"
#include "user_auth.h"
#include "wal_defs.h"
#include "wal_fence.h"
#include "sd_card.h"
#include "kv_sd.h"
#include "query.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// ============================================================
// Minimal HTTP server with jump-table routing
//
// Routes:
//   GET  /              → embedded root page
//   GET  /{slug}        → embedded page by slug
//   GET  /0/{type}/{id} → read KV record (requires Auth header)
//   POST /0/{type}/{id} → write KV record (requires Auth header)
//
// Session-based auth via cookies. PSK auth removed.
// type = uint10 (0-1023), id = uint22 (0-4194303)
// ============================================================

#define HTTP_PORT 80
#define HTTP_BUF_SIZE 4096  // max request size (headers + body up to 4KB value)
#define HTTP_CONN_COUNT 6

static wal_state_t *g_wal;
static volatile uint32_t g_http_last_activity_ms = 0;

// ============================================================
// Debug log ring buffer — accessible via GET /admin/log
// ============================================================

#define LOG_BUF_SIZE 2048
static char g_log_buf[LOG_BUF_SIZE];
static uint16_t g_log_pos = 0;

void web_log(const char *fmt, ...) {
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    // Also printf for serial
    printf("%s", line);
    // Append to ring buffer
    for (int i = 0; i < n && i < (int)sizeof(line) - 1; i++) {
        g_log_buf[g_log_pos] = line[i];
        g_log_pos = (g_log_pos + 1) % LOG_BUF_SIZE;
    }
}

static uint16_t log_read(char *out, uint16_t max) {
    uint16_t n = 0;
    // Read from oldest to newest
    for (uint16_t i = 0; i < LOG_BUF_SIZE && n < max - 1; i++) {
        uint16_t idx = (g_log_pos + i) % LOG_BUF_SIZE;
        if (g_log_buf[idx] == 0) continue;
        out[n++] = g_log_buf[idx];
    }
    out[n] = '\0';
    return n;
}

// Forward declarations
static void dispatch(struct tcp_pcb *pcb, const char *req, uint16_t req_len);

// ============================================================
// Minimal client JS — login, save card, delete, logout
// ============================================================

static const char APP_JS[] =
"(function(){"
"function api(m,u,b){return fetch(u,{method:m,credentials:'same-origin',body:b})}"
"function $(id){return document.getElementById(id)}"
/* Login form */
"var lf=$('loginForm');if(lf)lf.onsubmit=function(e){e.preventDefault();"
"var u=$('lu').value,p=$('lp').value;"
"var b=new Uint8Array(64);b[0]=u.length;for(var i=0;i<u.length&&i<31;i++)b[1+i]=u.charCodeAt(i);"
"b[32]=p.length;for(var i=0;i<p.length&&i<31;i++)b[33+i]=p.charCodeAt(i);"
"api('POST','/login',b.buffer).then(function(r){if(r.ok)location.href='/';else $('lerr').textContent='Login failed ('+r.status+')'});}"
/* Save card form */
";var sf=$('cardForm');if(sf)sf.onsubmit=function(e){e.preventDefault();"
"var pack=sf.dataset.pack,card=sf.dataset.card;"
"var parts=[0x7D,0xCA,1,0];"
"sf.querySelectorAll('[data-ord]').forEach(function(el){"
"var ord=parseInt(el.dataset.ord),type=el.dataset.ftype,val=el.value;"
"var data=[];"
"switch(type){"
"case 'bool':data=[/^true$|^1$/i.test(val)?1:0];break;"
"case 'uint8':data=[(parseInt(val)||0)&255];break;"
"case 'uint16':{var v=parseInt(val)||0;data=[v&255,(v>>8)&255];break}"
"case 'uint32':case 'lookup':{var v=parseInt(val)||0;data=[v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255];break}"
"case 'int16':{var ab=new ArrayBuffer(2);new DataView(ab).setInt16(0,parseInt(val)||0,true);data=Array.from(new Uint8Array(ab));break}"
"case 'int32':{var ab=new ArrayBuffer(4);new DataView(ab).setInt32(0,parseInt(val)||0,true);data=Array.from(new Uint8Array(ab));break}"
"case 'array_u16':{var a=val.trim()?val.split(',').map(function(s){return parseInt(s.trim())||0}):[];data=[a.length*2];a.forEach(function(v){data.push(v&255,(v>>8)&255)});break}"
"default:{var s=String(val||'');data=[s.length];for(var i=0;i<s.length;i++)data.push(s.charCodeAt(i)&255)}}"
"parts.push(ord&0x1F,data.length);data.forEach(function(b){parts.push(b)})});"
"api('POST','/pack/'+pack+'/'+card,new Uint8Array(parts)).then(function(r){"
"$('saveMsg').textContent=r.ok?'Saved!':'Error '+r.status;"
"$('saveMsg').style.color=r.ok?'#27ae60':'#e94560'});}"
/* Delete card */
";var db=$('delBtn');if(db)db.onclick=function(){"
"if(!confirm('Delete this card?'))return;"
"var sf=$('cardForm');api('DELETE','/pack/'+sf.dataset.pack+'/'+sf.dataset.card).then(function(r){"
"if(r.ok)location.href='/pack/'+sf.dataset.pack;else alert('Delete failed')});}"
/* Logout */
";var lo=$('logoutBtn');if(lo)lo.onclick=function(){location.href='/logout';}"
/* Create user form */
";var uf=$('userForm');if(uf)uf.onsubmit=function(e){e.preventDefault();"
"var body=JSON.stringify({username:$('nu').value,password:$('np').value,flags:parseInt($('nf').value)||0,"
"readPacks:$('nr').value.trim()?$('nr').value.split(',').map(Number):[],"
"writePacks:$('nw').value.trim()?$('nw').value.split(',').map(Number):[],"
"deletePacks:$('nd').value.trim()?$('nd').value.split(',').map(Number):[]});"
"api('POST','/admin/users',body).then(function(r){return r.text().then(function(t){"
"if(r.ok)location.reload();else alert('Error: '+t)})});}"
/* Add field form */
";var ff=$('fieldForm');if(ff)ff.onsubmit=function(e){e.preventDefault();"
"var body=$('fn').value.trim()+'|'+$('ft').value+'|'+$('fm').value.trim();"
"api('POST','/admin/meta/'+ff.dataset.pack,body).then(function(r){return r.text().then(function(t){"
"if(r.ok)location.reload();else alert('Error: '+t)})});}"
/* New pack form */
";var pf=$('packForm');if(pf)pf.onsubmit=function(e){e.preventDefault();"
"api('POST','/admin/meta/new',JSON.stringify({ordinal:parseInt($('po').value),name:$('pn').value.trim()})).then(function(r){return r.text().then(function(t){"
"if(r.ok)location.reload();else alert('Error: '+t)})});}"
/* Change password */
";var cp=$('passForm');if(cp)cp.onsubmit=function(e){e.preventDefault();"
"var o=$('cpOld').value,n=$('cpNew').value,c=$('cpConfirm').value;"
"if(n!==c){$('cpMsg').textContent='Passwords do not match';return}"
"var b=new Uint8Array(64);b[0]=o.length;for(var i=0;i<o.length&&i<31;i++)b[1+i]=o.charCodeAt(i);"
"b[32]=n.length;for(var i=0;i<n.length&&i<31;i++)b[33+i]=n.charCodeAt(i);"
"api('POST','/0/1/'+document.body.dataset.uc+'/_passwd',b.buffer).then(function(r){"
"$('cpMsg').textContent=r.ok?'Password changed':'Failed ('+r.status+')';$('cpMsg').style.color=r.ok?'#27ae60':'#e94560'});}"
"})();";

// ============================================================
// SSR helpers — render HTML into a buffer
// ============================================================

// Parse a schema card from Pack 0 into field definitions.
// Returns field count. Fills names[], types[], maxlens[], ords[].
static uint8_t parse_schema(const uint8_t *card, uint16_t card_len,
                            char names[][32], uint8_t *types, uint8_t *maxlens, uint8_t *ords,
                            char *pack_name, uint8_t max_fields) {
    if (card_len < 4 || card[0] != 0x7D || card[1] != 0xCA) return 0;
    uint16_t off = 4;
    uint8_t field_count = 0;
    char name_buf[256]; uint16_t name_buf_len = 0;

    while (off + 1 < card_len) {
        uint8_t ord = card[off] & 0x1F;
        uint8_t flen = card[off + 1];
        off += 2;
        if (off + flen > card_len) break;

        if (ord == 0 && flen >= 1 && pack_name) {
            uint8_t n = card[off]; if (n > flen - 1) n = flen - 1;
            if (n > 31) n = 31;
            memcpy(pack_name, card + off + 1, n);
            pack_name[n] = '\0';
        }
        if (ord == 1 && flen >= 1) field_count = card[off];
        if (ord == 2) {
            for (uint8_t i = 0; i < field_count && i < max_fields && i * 3 + 2 < flen; i++) {
                ords[i] = card[off + i * 3] & 0x1F;
                types[i] = card[off + i * 3 + 1];
                maxlens[i] = card[off + i * 3 + 2];
            }
        }
        if (ord == 5 && flen > 0) {
            memcpy(name_buf, card + off, flen);
            name_buf_len = flen;
        }
        off += flen;
    }

    // Parse null-separated names
    if (name_buf_len > 0) {
        uint8_t ni = 0; uint16_t si = 0;
        for (uint16_t i = 0; i < name_buf_len && ni < field_count && ni < max_fields; i++) {
            if (name_buf[i] == '\0') {
                uint8_t len = (uint8_t)(i - si); if (len > 31) len = 31;
                memcpy(names[ni], name_buf + si, len);
                names[ni][len] = '\0';
                ni++; si = i + 1;
            }
        }
    }
    return field_count < max_fields ? field_count : max_fields;
}

static const char *type_name(uint8_t code) {
    // Type codes match picowal.js / user_auth.c FT_* constants
    switch (code) {
        case 0x00: return "bool";      // metadata_dict enum value
        case 0x01: return "uint8";
        case 0x02: return "uint16";
        case 0x03: return "uint32";
        case 0x04: return "int8";
        case 0x05: return "int16";
        case 0x06: return "int32";
        case 0x07: return "bool";      // picowal.js T.BOOL
        case 0x08: return "ascii";
        case 0x09: return "utf8";
        case 0x0A: return "date";
        case 0x0B: return "time";
        case 0x0C: return "datetime";
        case 0x0D: return "ipv4";
        case 0x0E: return "mac";
        case 0x0F: return "enum";
        case 0x10: return "array_u16";
        case 0x11: return "blob";
        case 0x12: return "lookup";
        default:   return "?";
    }
}

// Decode a field value from a card into a display string
// Type codes match picowal.js / user_auth.c FT_* constants
static int decode_field_str(char *out, int max, uint8_t type_code,
                            const uint8_t *data, uint8_t dlen) {
    switch (type_code) {
        case 0x07: // bool
            return snprintf(out, max, "%s", dlen && data[0] ? "true" : "false");
        case 0x01: // uint8
            return snprintf(out, max, "%u", dlen ? data[0] : 0);
        case 0x04: // int8
            return snprintf(out, max, "%d", dlen ? (int8_t)data[0] : 0);
        case 0x02: { // uint16
            uint16_t v = 0; if (dlen >= 2) memcpy(&v, data, 2);
            return snprintf(out, max, "%u", v);
        }
        case 0x05: { // int16
            int16_t v = 0; if (dlen >= 2) memcpy(&v, data, 2);
            return snprintf(out, max, "%d", (int)v);
        }
        case 0x03: { // uint32
            uint32_t v = 0; if (dlen >= 4) memcpy(&v, data, 4);
            return snprintf(out, max, "%lu", (unsigned long)v);
        }
        case 0x06: { // int32
            int32_t v = 0; if (dlen >= 4) memcpy(&v, data, 4);
            return snprintf(out, max, "%ld", (long)v);
        }
        case 0x12: { // lookup — stored as uint32
            uint32_t v = 0; if (dlen >= 4) memcpy(&v, data, 4);
            return snprintf(out, max, "%lu", (unsigned long)v);
        }
        case 0x08: case 0x09: // ascii, utf8
        case 0x0A: case 0x0B: case 0x0C: // date, time, datetime
            // length-prefixed string
            if (dlen >= 1) {
                uint8_t slen = data[0]; if (slen > dlen - 1) slen = dlen - 1;
                if (slen > (uint8_t)(max - 1)) slen = (uint8_t)(max - 1);
                memcpy(out, data + 1, slen);
                out[slen] = '\0'; return slen;
            }
            out[0] = '\0'; return 0;
        case 0x10: { // array_u16
            int n = 0;
            if (dlen >= 1) {
                uint8_t bc = data[0];
                for (uint8_t i = 0; i + 1 < bc && i + 2 <= dlen - 1; i += 2) {
                    uint16_t v = data[1 + i] | ((uint16_t)data[2 + i] << 8);
                    if (n > 0) { out[n++] = ','; }
                    n += snprintf(out + n, max - n, "%u", v);
                }
            }
            out[n] = '\0'; return n;
        }
        case 0x11: // blob
            return snprintf(out, max, "(%u bytes)", dlen);
        default:
            out[0] = '\0'; return 0;
    }
}

// Parse all fields from a data card, output values by ordinal.
// vals[ord] is filled with the display string.
static void parse_card_values(const uint8_t *card, uint16_t card_len,
                              char vals[][64], uint8_t *field_types, uint8_t nfields) {
    if (card_len < 4 || card[0] != 0x7D || card[1] != 0xCA) return;
    uint16_t off = 4;
    while (off + 1 < card_len) {
        uint8_t ord = card[off] & 0x1F;
        uint8_t flen = card[off + 1];
        off += 2;
        if (off + flen > card_len) break;
        for (uint8_t i = 0; i < nfields; i++) {
            // Match by ordinal position (field_types array is indexed by schema order)
            // We need to find this ord in the schema
        }
        // Direct: decode into vals[ord] if ord < 32
        if (ord < 32) {
            decode_field_str(vals[ord], 64, field_types[ord], card + off, flen);
        }
        off += flen;
    }
}


typedef struct {
    uint8_t buf[HTTP_BUF_SIZE];
    uint16_t len;
    bool response_pending_close;
    bool keep_alive;
    uint8_t close_polls;
    uint16_t bytes_in_flight;
    uint8_t slot_idx;
    bool in_use;
} http_conn_t;

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);

bool web_server_recent_activity(uint32_t quiet_ms) {
    uint32_t last = g_http_last_activity_ms;
    if (last == 0) return false;
    return (to_ms_since_boot(get_absolute_time()) - last) < quiet_ms;
}

// ============================================================
// HTTP helpers
// ============================================================

static uint16_t http_respond(struct tcp_pcb *pcb, const char *status,
                             const char *ctype, const uint8_t *body, uint16_t blen) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\n",
        status, ctype, blen);
    if (n <= 0 || n >= (int)sizeof(hdr)) return 0;
    if (tcp_write(pcb, hdr, (uint16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return 0;
    if (blen > 0 && tcp_write(pcb, body, blen, TCP_WRITE_FLAG_COPY) != ERR_OK) return 0;
    tcp_output(pcb);
    return (uint16_t)n + blen;
}

static void http_json(struct tcp_pcb *pcb, const char *status, const char *json) {
    (void)http_respond(pcb, status, "application/json", (const uint8_t *)json, strlen(json));
}

// HTTP response with extra headers (e.g. Set-Cookie)
static uint16_t http_respond_with_headers(struct tcp_pcb *pcb, const char *status,
                                          const char *ctype, const char *extra_hdrs,
                                          const uint8_t *body, uint16_t blen) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "%s"
        "Access-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\n",
        status, ctype, blen, extra_hdrs ? extra_hdrs : "");
    if (n <= 0 || n >= (int)sizeof(hdr)) return 0;
    if (tcp_write(pcb, hdr, (uint16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return 0;
    if (blen > 0 && tcp_write(pcb, body, blen, TCP_WRITE_FLAG_COPY) != ERR_OK) return 0;
    tcp_output(pcb);
    return (uint16_t)n + blen;
}

// Check auth: try cookie session first, then fall back to PSK.
// If session found, fills *session and returns true.
// If PSK valid but no session, session is zeroed but returns true.
// Otherwise returns false.
static bool check_auth_session(const char *req, user_session_t *session) {
    memset(session, 0, sizeof(*session));

    uint8_t token[SESSION_TOKEN_LEN];
    if (user_auth_parse_cookie(req, token)) {
        if (user_auth_check(token, session)) return true;
    }

    return false;
}

static bool parse_metadata_field_body(const uint8_t *body, uint16_t body_len,
                                      char name[META_NAME_MAX + 1], uint8_t *field_type, uint8_t *max_len) {
    char buf[96];
    if (body_len == 0 || body_len >= sizeof(buf)) return false;
    memcpy(buf, body, body_len);
    buf[body_len] = '\0';

    char *p1 = strchr(buf, '|');
    if (!p1) return false;
    char *p2 = strchr(p1 + 1, '|');
    if (!p2) return false;
    *p1 = '\0';
    *p2 = '\0';

    if (strlen(buf) == 0 || strlen(buf) > META_NAME_MAX) return false;
    strcpy(name, buf);

    if (!metadata_field_type_parse(p1 + 1, field_type)) return false;

    char *end = NULL;
    unsigned long n = strtoul(p2 + 1, &end, 10);
    if (!end || *end != '\0' || n > 255u) return false;
    *max_len = (uint8_t)n;
    return true;
}

static bool request_wants_keep_alive(const char *req) {
    const char *line_end = strstr(req, "\r\n");
    if (!line_end) return false;
    bool http11 = (strstr(req, "HTTP/1.1") != NULL && strstr(req, "HTTP/1.1") < line_end);
    const char *conn = strstr(req, "\r\nConnection:");
    if (!conn) conn = strstr(req, "\r\nconnection:");
    if (conn) {
        return strstr(conn, "keep-alive") != NULL || strstr(conn, "Keep-Alive") != NULL;
    }
    return http11;
}

// ============================================================
// Shared page chrome — all HTML pages use this shell
// ============================================================

static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>PicoWAL</title><style>"
    "body{font:14px monospace;background:#1a1a2e;color:#e0e0e0;padding:0;margin:0}"
    ".page{max-width:960px;margin:0 auto;padding:16px}"
    "h1{color:#e94560;margin:0}h2{color:#0ff;border-bottom:1px solid #0f3460;padding-bottom:4px}"
    "a{color:#0ff}a:hover{color:#e94560}"
    "input,select,button,textarea{font:inherit;padding:6px 10px;margin:4px 0;background:#0f3460;color:#e0e0e0;border:1px solid #1a1a4e;border-radius:4px}"
    "button{background:#e94560;color:#fff;border:none;cursor:pointer;font-weight:bold}button:hover{background:#c73652}"
    ".btn-sm{padding:3px 8px;font-size:12px}.btn-del{background:#c0392b}.btn-ok{background:#27ae60}"
    "table{width:100%;border-collapse:collapse;margin:8px 0}th,td{padding:6px 8px;border:1px solid #0f3460;text-align:left}"
    "th{background:#0f3460}.row{display:flex;gap:8px;flex-wrap:wrap}.row>*{flex:1;min-width:120px}"
    ".card{background:#16213e;border:1px solid #0f3460;border-radius:8px;padding:16px;margin:12px 0}"
    ".badge{background:#27ae60;color:#fff;padding:2px 6px;border-radius:3px;font-size:11px}"
    ".badge-admin{background:#e94560}"
    "label{display:block;font-size:12px;color:#888;margin-top:6px}"
    "pre{background:#222;padding:10px;border-radius:4px;white-space:pre-wrap;max-height:400px;overflow-y:auto}"
    ".tabs{display:flex;gap:0;margin-bottom:0}.tab{padding:10px 20px;cursor:pointer;background:#0f3460;border:1px solid #1a1a4e;border-bottom:none;border-radius:8px 8px 0 0;color:#888}"
    ".tab.active{background:#16213e;color:#0ff;border-color:#0f3460}.tab-body{display:none}.tab-body.active{display:block}"
    "#loginBox{max-width:300px;margin:80px auto}"
    "#status{padding:8px;background:#222;border-radius:4px;white-space:pre-wrap;margin-top:8px}"
    "nav{background:#0f3460;padding:10px 20px;display:flex;justify-content:space-between;align-items:center}"
    "nav a{color:#e0e0e0;text-decoration:none;margin:0 10px}nav a:hover{color:#0ff}"
    ".nav-brand{color:#e94560!important;font-weight:700;letter-spacing:1px;font-size:16px}"
    ".dropdown{position:relative;display:inline-block}"
    ".dropdown>span{color:#e0e0e0;cursor:pointer;margin:0 10px}.dropdown>span:hover{color:#0ff}"
    ".dropdown-menu{display:none;position:absolute;top:100%;left:0;background:#0f3460;border:1px solid #1a1a4e;border-radius:4px;min-width:140px;z-index:10;padding:4px 0}"
    ".dropdown:hover .dropdown-menu{display:block}"
    ".dropdown-menu a{display:block;padding:6px 16px;margin:0;white-space:nowrap}.dropdown-menu a:hover{background:#1a1a4e}"
    "</style></head><body>"
    "<nav><span class=nav-brand>&#x1F5C3; PicoWAL</span>"
    "<div><a href=/>Home</a><a href=/status>Status</a><a href=/query>Query</a>";

static const char PAGE_NAV_TAIL[] =
    "</div></nav><div class=page>";

static const char PAGE_TAIL[] = "</div></body></html>";

// Build dynamic nav links for the user's accessible packs
static uint16_t build_nav(char *buf, uint16_t bufsize, const char *req) {
    int n = 0;
    uint8_t token[SESSION_TOKEN_LEN];
    user_session_t session;
    if (user_auth_parse_cookie(req, token) && user_auth_check(token, &session)) {
        uint32_t keys[16];
        uint32_t count = kv_range(0, 0xFFC00000u, keys, NULL, 16);

        // First pass: user packs (non-public, non-system)
        for (uint32_t i = 0; i < count && n < (int)bufsize - 100; i++) {
            uint32_t pack_ord = keys[i] & 0x3FFFFF;
            if (pack_ord == 0 || pack_ord == 1) continue; // skip metadata + users
            if (!user_auth_can_read(&session, (uint16_t)pack_ord)) continue;

            // Check if public-read (system pack)
            uint8_t sbuf[128]; uint16_t slen = sizeof(sbuf);
            if (!kv_get_copy(keys[i], sbuf, &slen, NULL)) continue;
            if (slen < 6 || sbuf[0] != 0x7D || sbuf[1] != 0xCA) continue;

            // Parse flags (ord 3) and name (ord 0)
            char pname[16] = "?"; uint8_t pflags = 0;
            uint16_t off = 4;
            while (off + 1 < slen) {
                uint8_t ord = sbuf[off] & 0x1F, flen = sbuf[off + 1]; off += 2;
                if (off + flen > slen) break;
                if (ord == 0 && flen >= 1) {
                    uint8_t nl = sbuf[off]; if (nl > flen - 1) nl = flen - 1; if (nl > 15) nl = 15;
                    memcpy(pname, sbuf + off + 1, nl); pname[nl] = '\0';
                    if (pname[0] >= 'a' && pname[0] <= 'z') pname[0] -= 32;
                }
                if (ord == 3 && flen >= 1) pflags = sbuf[off];
                off += flen;
            }

            if (pflags & 0x01) continue; // skip public packs — they go in System dropdown
            n += snprintf(buf + n, bufsize - n,
                "<a href='/pack/%lu'>%s</a>", (unsigned long)pack_ord, pname);
        }

        // System dropdown: public-read packs
        char sys[512]; int sn = 0;
        for (uint32_t i = 0; i < count && sn < (int)sizeof(sys) - 100; i++) {
            uint32_t pack_ord = keys[i] & 0x3FFFFF;
            if (pack_ord == 0 || pack_ord == 1) continue;
            if (!user_auth_can_read(&session, (uint16_t)pack_ord)) continue;

            uint8_t sbuf[128]; uint16_t slen = sizeof(sbuf);
            if (!kv_get_copy(keys[i], sbuf, &slen, NULL)) continue;
            if (slen < 6 || sbuf[0] != 0x7D || sbuf[1] != 0xCA) continue;

            char pname[16] = "?"; uint8_t pflags = 0;
            uint16_t off = 4;
            while (off + 1 < slen) {
                uint8_t ord = sbuf[off] & 0x1F, flen = sbuf[off + 1]; off += 2;
                if (off + flen > slen) break;
                if (ord == 0 && flen >= 1) {
                    uint8_t nl = sbuf[off]; if (nl > flen - 1) nl = flen - 1; if (nl > 15) nl = 15;
                    memcpy(pname, sbuf + off + 1, nl); pname[nl] = '\0';
                    if (pname[0] >= 'a' && pname[0] <= 'z') pname[0] -= 32;
                }
                if (ord == 3 && flen >= 1) pflags = sbuf[off];
                off += flen;
            }

            if (!(pflags & 0x01)) continue; // only public packs
            sn += snprintf(sys + sn, sizeof(sys) - sn,
                "<a href='/pack/%lu'>%s</a>", (unsigned long)pack_ord, pname);
        }

        if (sn > 0) {
            n += snprintf(buf + n, bufsize - n,
                "<div class=dropdown><span>System &#x25BE;</span><div class=dropdown-menu>%s</div></div>", sys);
        }

        if (user_auth_is_admin(&session))
            n += snprintf(buf + n, bufsize - n, "<a href=/admin>Admin</a><a href='/admin/meta'>Schema</a><a href='/admin/log'>Log</a><a href='/admin/flash'>Flash</a><a href='/admin/ram'>RAM</a><a href=/update>OTA</a>");
        n += snprintf(buf + n, bufsize - n, "<a href=/logout>Logout</a>");
    }
    return (uint16_t)n;
}

static void http_page_req(struct tcp_pcb *pcb, const char *req,
                          const char *content, uint16_t content_len) {
    uint16_t head_len = sizeof(PAGE_HEAD) - 1;
    uint16_t nav_tail_len = sizeof(PAGE_NAV_TAIL) - 1;
    uint16_t tail_len = sizeof(PAGE_TAIL) - 1;
    char nav[512];
    uint16_t nav_len = build_nav(nav, sizeof(nav), req);
    uint32_t total = (uint32_t)head_len + nav_len + nav_tail_len + content_len + tail_len;
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %lu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\n",
        (unsigned long)total);
    if (n <= 0) return;
    tcp_write(pcb, hdr, (uint16_t)n, TCP_WRITE_FLAG_COPY);
    tcp_write(pcb, PAGE_HEAD, head_len, 0);
    if (nav_len > 0) tcp_write(pcb, nav, nav_len, TCP_WRITE_FLAG_COPY);
    tcp_write(pcb, PAGE_NAV_TAIL, nav_tail_len, 0);
    if (content_len > 0) tcp_write(pcb, content, content_len, TCP_WRITE_FLAG_COPY);
    tcp_write(pcb, PAGE_TAIL, tail_len, 0);
    tcp_output(pcb);
}

static void http_page(struct tcp_pcb *pcb, const char *content, uint16_t content_len) {
    http_page_req(pcb, "", content, content_len);
}
// ============================================================
// /status — appliance stats inside page chrome
// ============================================================

static void http_status_page(struct tcp_pcb *pcb, const char *req) {
    kv_stats_t st = kv_stats();
    uint32_t records = kv_record_count();
    uint16_t types[16];
    uint32_t counts[16];
    uint32_t n_types = kv_type_counts(types, counts, 16);
    const char *ip_text = netif_list ? ip4addr_ntoa(netif_ip4_addr(netif_list)) : "0.0.0.0";
    uint32_t used_pages = st.total - st.free;
    uint32_t used_bytes = used_pages * KV_SECTOR_SIZE;
    uint32_t free_bytes = st.free * KV_SECTOR_SIZE;
    uint32_t usage_tenths = (st.total > 0) ? (used_pages * 1000u) / st.total : 0;

    char body[1200];
    int n = snprintf(body, sizeof(body),
        "<h2>Appliance Status</h2>"
        "<div class=card><pre>"
        "HTTP:         %s:80\n"
        "RECORDS:      %lu\n"
        "USED BYTES:   %lu\n"
        "FREE BYTES:   %lu\n"
        "USAGE:        %lu.%lu%%\n"
        "DEAD PAGES:   %lu\n"
        "REQUESTS:     %lu\n"
        "WRITES:       %lu\n"
        "READS:        %lu\n"
        "COMPACTIONS:  %lu\n"
        "RECLAIMED:    %lu\n"
        "</pre></div>",
        ip_text,
        (unsigned long)records,
        (unsigned long)used_bytes,
        (unsigned long)free_bytes,
        (unsigned long)(usage_tenths / 10u),
        (unsigned long)(usage_tenths % 10u),
        (unsigned long)st.dead,
        (unsigned long)g_wal->req_total,
        (unsigned long)g_wal->req_appends,
        (unsigned long)g_wal->req_reads,
        (unsigned long)g_wal->compactions,
        (unsigned long)g_wal->slots_reclaimed);
    if (n <= 0 || n >= (int)sizeof(body)) return;

    // SD card info
    {
        sd_info_t sdi;
        if (sd_get_info(&sdi)) {
            kvsd_stats_t kst = kvsd_stats();
            n += snprintf(body + n, sizeof(body) - n,
                "<div class=card><h2>SD Card</h2><pre>"
                "TYPE:         %s\n"
                "CAPACITY:     %lu MB\n"
                "BLOCKS:       %lu\n"
                "KV RECORDS:   %lu / %lu\n"
                "MAX CARDS:    %lu\n"
                "</pre></div>",
                sdi.sdhc ? "SDHC" : "SDSC",
                (unsigned long)sdi.capacity_mb,
                (unsigned long)sdi.block_count,
                (unsigned long)kst.active, (unsigned long)kst.index_max,
                (unsigned long)(sdi.block_count / 4));
        } else {
            n += snprintf(body + n, sizeof(body) - n,
                "<div class=card><h2>SD Card</h2><pre style='color:#e94560'>NOT DETECTED\n%s</pre>"
                "<form method=POST action='/admin/sd/init' style='margin-top:8px'>"
                "<button type=submit>Init SD Card</button></form></div>",
                sd_get_debug());
        }
    }

    if (n_types > 0) {
        int wrote = snprintf(body + n, sizeof(body) - (size_t)n,
            "<h2>Packs</h2>"
            "<table><thead><tr><th>Pack</th><th>Name</th><th>Cards</th></tr></thead><tbody>");
        if (wrote > 0 && wrote < (int)(sizeof(body) - (size_t)n)) n += wrote;

        for (uint32_t i = 0; i < n_types && i < 16; i++) {
            char pack_name[24];
            memset(pack_name, 0, sizeof(pack_name));
            uint32_t schema_key = ((uint32_t)0 << 22) | (types[i] & 0x3FFFFF);
            uint8_t schema_buf[256];
            uint16_t slen = sizeof(schema_buf);
            if (kv_get_copy(schema_key, schema_buf, &slen, NULL) && slen >= 6 &&
                schema_buf[0] == 0x7D && schema_buf[1] == 0xCA) {
                uint16_t off = 4;
                if (off + 2 <= slen && (schema_buf[off] & 0x1F) == 0) {
                    uint8_t flen = schema_buf[off + 1];
                    off += 2;
                    if (off + flen <= slen && flen >= 1) {
                        uint8_t nlen = schema_buf[off];
                        if (nlen > flen - 1) nlen = flen - 1;
                        if (nlen > sizeof(pack_name) - 1) nlen = sizeof(pack_name) - 1;
                        memcpy(pack_name, schema_buf + off + 1, nlen);
                        pack_name[nlen] = '\0';
                    }
                }
            }
            if (pack_name[0] == '\0') snprintf(pack_name, sizeof(pack_name), "-");

            wrote = snprintf(body + n, sizeof(body) - (size_t)n,
                "<tr><td>%u</td><td>%s</td><td>%lu</td></tr>",
                (unsigned int)types[i], pack_name, (unsigned long)counts[i]);
            if (wrote > 0 && wrote < (int)(sizeof(body) - (size_t)n)) n += wrote;
        }

        wrote = snprintf(body + n, sizeof(body) - (size_t)n, "</tbody></table>");
        if (wrote > 0 && wrote < (int)(sizeof(body) - (size_t)n)) n += wrote;
    }

    http_page_req(pcb, req, body, (uint16_t)n);
}

static bool parse_content_length(const char *req, uint32_t *out_len) {
    const char *p = strstr(req, "\r\nContent-Length:");
    if (!p) p = strstr(req, "\r\ncontent-length:");
    if (!p) {
        *out_len = 0;
        return true;
    }

    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    uint32_t len = 0;
    bool saw_digit = false;
    while (*p >= '0' && *p <= '9') {
        saw_digit = true;
        len = len * 10u + (uint32_t)(*p - '0');
        if (len > KV_MAX_VALUE) return false;
        p++;
    }
    if (!saw_digit) return false;

    *out_len = len;
    return true;
}

static int http_alloc_req_id(void) {
    for (int i = 0; i < REQ_RING_SIZE; i++) {
        if (g_wal->requests[i].ready == REQ_EMPTY) return i;
    }
    return -1;
}

static int http_alloc_slot(void) {
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (g_wal->slot_free[i]) {
            g_wal->slot_free[i] = 0;
            return i;
        }
    }
    return -1;
}

static bool http_wait_req(uint8_t req_id) {
    wal_request_t *req = &g_wal->requests[req_id];
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 2000u;
    while (req->ready != REQ_DONE) {
        if ((int32_t)(to_ms_since_boot(get_absolute_time()) - deadline) >= 0) return false;
        tight_loop_contents();
    }
    return true;
}

static bool http_kv_get(uint32_t key, uint8_t *out, uint16_t *len) {
    int rid = http_alloc_req_id();
    if (rid < 0) return false;

    wal_request_t *req = &g_wal->requests[rid];
    wal_response_t *resp = &g_wal->responses[rid];
    memset(resp, 0, sizeof(*resp));
    req->op = WAL_OP_KV_GET;
    req->slot = 0;
    req->len = 0;
    req->key_hash = key;
    req->zc_data = NULL;
    req->zc_len = 0;
    req->zc_pbuf = NULL;
    wal_dmb();
    req->ready = REQ_PENDING;
    multicore_fifo_push_blocking(fifo_signal((uint8_t)rid));

    if (!http_wait_req((uint8_t)rid)) {
        req->ready = REQ_EMPTY;
        return false;
    }

    bool ok = false;
    if (resp->status == WAL_RESP_OK && resp->result_len <= *len) {
        memcpy(out, g_wal->data[resp->result_slot], resp->result_len);
        *len = resp->result_len;
        g_wal->slot_free[resp->result_slot] = 1;
        ok = true;
    } else if (resp->status == WAL_RESP_OK) {
        g_wal->slot_free[resp->result_slot] = 1;
    }

    wal_dmb();
    req->ready = REQ_EMPTY;
    return ok;
}

static bool http_kv_put(uint32_t key, const uint8_t *body, uint16_t body_len) {
    int rid = http_alloc_req_id();
    if (rid < 0) return false;
    int slot = http_alloc_slot();
    if (slot < 0) return false;

    memcpy(g_wal->data[slot], body, body_len);

    wal_request_t *req = &g_wal->requests[rid];
    wal_response_t *resp = &g_wal->responses[rid];
    memset(resp, 0, sizeof(*resp));
    req->op = WAL_OP_KV_PUT;
    req->slot = (uint8_t)slot;
    req->len = body_len;
    req->key_hash = key;
    req->zc_data = NULL;
    req->zc_len = 0;
    req->zc_pbuf = NULL;
    wal_dmb();
    req->ready = REQ_PENDING;
    multicore_fifo_push_blocking(fifo_signal((uint8_t)rid));

    if (!http_wait_req((uint8_t)rid)) {
        g_wal->slot_free[slot] = 1;
        req->ready = REQ_EMPTY;
        return false;
    }

    bool ok = (resp->status == WAL_RESP_OK);
    wal_dmb();
    req->ready = REQ_EMPTY;
    return ok;
}

// ============================================================
// Route handlers — jump table
// ============================================================

typedef enum { VERB_GET, VERB_POST, VERB_PUT, VERB_DELETE, VERB_UNKNOWN } http_verb_t;

typedef void (*route_handler_t)(struct tcp_pcb *pcb, http_verb_t verb,
                                uint16_t type_id, uint32_t record_id,
                                const uint8_t *body, uint16_t body_len);

// /0/{type}/{id} — KV operations
static void handle_kv(struct tcp_pcb *pcb, http_verb_t verb,
                      uint16_t type_id, uint32_t record_id,
                      const uint8_t *body, uint16_t body_len) {
    uint32_t key = ((uint32_t)(type_id & 0x3FF) << 22) | (record_id & 0x3FFFFF);

    if (verb == VERB_GET) {
        uint8_t val[KV_MAX_VALUE];
        uint16_t len = KV_MAX_VALUE;
        if (!kv_get_copy(key, val, &len, NULL)) {
            http_json(pcb, "404 Not Found", "{\"error\":\"not found\"}");
        } else {
            http_respond(pcb, "200 OK", "application/octet-stream", val, len);
        }
    } else if (verb == VERB_DELETE) {
        if (kv_delete(key)) {
            http_json(pcb, "200 OK", "{\"ok\":true}");
        } else {
            http_json(pcb, "404 Not Found", "{\"error\":\"not found\"}");
        }
    } else {
        // POST or PUT — write
        if (body_len > KV_MAX_VALUE) {
            http_json(pcb, "413 Payload Too Large", "{\"error\":\"too large\"}");
            return;
        }
        if (kv_put(key, body, body_len)) {
            http_json(pcb, "200 OK", "{\"ok\":true}");
        } else {
            http_json(pcb, "500 Internal Server Error", "{\"error\":\"write failed\"}");
        }
    }
}

static void handle_metadata(struct tcp_pcb *pcb, http_verb_t verb, const char *path,
                            const uint8_t *body, uint16_t body_len, const char *req) {
    if (strncmp(path, "/meta/types", 11) == 0) {
        if (strcmp(path, "/meta/types") == 0 && verb == VERB_GET) {
            metadata_type_def_t defs[64];
            uint32_t count = metadata_list_types(defs, 64);
            char json[2048];
            int n = snprintf(json, sizeof(json), "{\"types\":[");
            if (n < 0 || n >= (int)sizeof(json)) {
                http_json(pcb, "500 Internal Server Error", "{\"error\":\"meta types overflow\"}");
                return;
            }
            for (uint32_t i = 0; i < count; i++) {
                int wrote = snprintf(json + n, sizeof(json) - (size_t)n,
                                     "%s{\"ordinal\":%u,\"name\":\"%s\"}",
                                     (i == 0) ? "" : ",",
                                     (unsigned int)defs[i].ordinal,
                                     defs[i].name);
                if (wrote < 0 || wrote >= (int)(sizeof(json) - (size_t)n)) {
                    http_json(pcb, "500 Internal Server Error", "{\"error\":\"meta types overflow\"}");
                    return;
                }
                n += wrote;
            }
            if (n + 3 >= (int)sizeof(json)) {
                http_json(pcb, "500 Internal Server Error", "{\"error\":\"meta types overflow\"}");
                return;
            }
            strcpy(json + n, "]}");
            http_json(pcb, "200 OK", json);
            return;
        }

        if (strncmp(path, "/meta/types/by-name/", 20) == 0 && verb == VERB_GET) {
            metadata_type_def_t def;
            if (!metadata_find_type(path + 20, &def)) {
                http_json(pcb, "404 Not Found", "{\"error\":\"type not found\"}");
                return;
            }
            char json[128];
            snprintf(json, sizeof(json), "{\"ordinal\":%u,\"name\":\"%s\"}",
                     (unsigned int)def.ordinal, def.name);
            http_json(pcb, "200 OK", json);
            return;
        }

        if (strncmp(path, "/meta/types/", 12) == 0) {
            unsigned int ordinal = 0;
            if (sscanf(path, "/meta/types/%u", &ordinal) != 1 || ordinal > 1023u) {
                http_json(pcb, "400 Bad Request", "{\"error\":\"invalid type ordinal\"}");
                return;
            }
            if (verb == VERB_GET) {
                metadata_type_def_t def;
                if (!metadata_get_type((uint16_t)ordinal, &def)) {
                    http_json(pcb, "404 Not Found", "{\"error\":\"type not found\"}");
                    return;
                }
                char json[128];
                snprintf(json, sizeof(json), "{\"ordinal\":%u,\"name\":\"%s\"}",
                         (unsigned int)def.ordinal, def.name);
                http_json(pcb, "200 OK", json);
                return;
            }
            {
                user_session_t session;
                if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
                    http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
                    return;
                }
            }
            char name[META_NAME_MAX + 1];
            if (body_len == 0 || body_len > META_NAME_MAX) {
                http_json(pcb, "400 Bad Request", "{\"error\":\"type name required\"}");
                return;
            }
            memcpy(name, body, body_len);
            name[body_len] = '\0';
            if (!metadata_set_type((uint16_t)ordinal, name)) {
                http_json(pcb, "500 Internal Server Error", "{\"error\":\"type write failed\"}");
                return;
            }
            http_json(pcb, "200 OK", "{\"ok\":true}");
            return;
        }
    }

    if (strncmp(path, "/meta/fields", 12) == 0) {
        if (strcmp(path, "/meta/fields") == 0 && verb == VERB_GET) {
            metadata_field_def_t defs[64];
            uint32_t count = metadata_list_fields(defs, 64);
            char json[3072];
            int n = snprintf(json, sizeof(json), "{\"fields\":[");
            if (n < 0 || n >= (int)sizeof(json)) {
                http_json(pcb, "500 Internal Server Error", "{\"error\":\"meta fields overflow\"}");
                return;
            }
            for (uint32_t i = 0; i < count; i++) {
                int wrote = snprintf(json + n, sizeof(json) - (size_t)n,
                                     "%s{\"ordinal\":%u,\"name\":\"%s\",\"type\":\"%s\",\"max_len\":%u}",
                                     (i == 0) ? "" : ",",
                                     (unsigned int)defs[i].ordinal,
                                     defs[i].name,
                                     metadata_field_type_name(defs[i].field_type),
                                     (unsigned int)defs[i].max_len);
                if (wrote < 0 || wrote >= (int)(sizeof(json) - (size_t)n)) {
                    http_json(pcb, "500 Internal Server Error", "{\"error\":\"meta fields overflow\"}");
                    return;
                }
                n += wrote;
            }
            if (n + 3 >= (int)sizeof(json)) {
                http_json(pcb, "500 Internal Server Error", "{\"error\":\"meta fields overflow\"}");
                return;
            }
            strcpy(json + n, "]}");
            http_json(pcb, "200 OK", json);
            return;
        }

        if (strncmp(path, "/meta/fields/by-name/", 21) == 0 && verb == VERB_GET) {
            metadata_field_def_t def;
            if (!metadata_find_field(path + 21, &def)) {
                http_json(pcb, "404 Not Found", "{\"error\":\"field not found\"}");
                return;
            }
            char json[192];
            snprintf(json, sizeof(json),
                     "{\"ordinal\":%u,\"name\":\"%s\",\"type\":\"%s\",\"max_len\":%u}",
                     (unsigned int)def.ordinal,
                     def.name,
                     metadata_field_type_name(def.field_type),
                     (unsigned int)def.max_len);
            http_json(pcb, "200 OK", json);
            return;
        }

        if (strncmp(path, "/meta/fields/", 13) == 0) {
            unsigned int ordinal = 0;
            if (sscanf(path, "/meta/fields/%u", &ordinal) != 1 || ordinal > 1023u) {
                http_json(pcb, "400 Bad Request", "{\"error\":\"invalid field ordinal\"}");
                return;
            }
            if (verb == VERB_GET) {
                metadata_field_def_t def;
                if (!metadata_get_field((uint16_t)ordinal, &def)) {
                    http_json(pcb, "404 Not Found", "{\"error\":\"field not found\"}");
                    return;
                }
                char json[192];
                snprintf(json, sizeof(json),
                         "{\"ordinal\":%u,\"name\":\"%s\",\"type\":\"%s\",\"max_len\":%u}",
                         (unsigned int)def.ordinal,
                         def.name,
                         metadata_field_type_name(def.field_type),
                         (unsigned int)def.max_len);
                http_json(pcb, "200 OK", json);
                return;
            }
            {
                user_session_t session;
                if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
                    http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
                    return;
                }
            }
            char name[META_NAME_MAX + 1];
            uint8_t field_type = 0;
            uint8_t max_len = 0;
            if (!parse_metadata_field_body(body, body_len, name, &field_type, &max_len)) {
                http_json(pcb, "400 Bad Request", "{\"error\":\"expected NAME|TYPE|MAXLEN\"}");
                return;
            }
            if (!metadata_set_field((uint16_t)ordinal, name, field_type, max_len)) {
                http_json(pcb, "500 Internal Server Error", "{\"error\":\"field write failed\"}");
                return;
            }
            http_json(pcb, "200 OK", "{\"ok\":true}");
            return;
        }
    }

    http_json(pcb, "404 Not Found", "{\"error\":\"unknown metadata route\"}");
}

// ============================================================
// OTA firmware update — A/B slot scheme
//
// Slot A: 0x000000–0x07FFFF (512KB) — default boot location
// Slot B: 0x080000–0x0FFFFF (512KB) — OTA staging area
//
// Normal boot: RP2350 boots from 0x000000 (slot A).
//
// OTA when running from slot A:
//   1. begin: prepare to write slot B
//   2. chunk: write new firmware to slot B (safe — not running code)
//   3. commit: erase first sector of slot A, write a tiny trampoline
//      that loads SP/PC from slot B's vector table and jumps there.
//      Then reboot — boots the trampoline which jumps to slot B.
//
// OTA when running from slot B (trampoline active in A):
//   1. begin: prepare to write slot A
//   2. chunk: write new firmware directly to slot A (safe — slot B is running)
//   3. commit: reboot — boots directly from slot A (real firmware).
//
// The trampoline is ~64 bytes: a Cortex-M33 vector table whose
// reset handler loads SP/PC from slot B and branches there.
// ============================================================

#define OTA_SLOT_A      0x000000
#define OTA_SLOT_B      (512 * 1024)   // 0x080000
#define OTA_SLOT_SIZE   (512 * 1024)
#define OTA_XIP_BASE    0x10000000

// Detect which slot we're running from by checking our own PC
static bool running_from_slot_b(void) {
    uint32_t pc;
    __asm volatile ("mov %0, pc" : "=r" (pc));
    return pc >= (OTA_XIP_BASE + OTA_SLOT_B);
}

static struct {
    bool     active;
    uint32_t target_base;  // flash offset where we're writing (slot A or B)
    uint32_t offset;       // bytes written so far
    uint32_t total_written;
    uint32_t erased_up_to; // absolute flash offset erased up to
} g_ota;

// These MUST run from RAM — they touch flash
static void __no_inline_not_in_flash_func(ota_flash_erase)(uint32_t offset) {
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
}

static void __no_inline_not_in_flash_func(ota_flash_program)(uint32_t offset, const uint8_t *data) {
    flash_range_program(offset, data, 256);
}

// OTA commit: copy slot B → slot A.
// CRITICAL: XIP reads from slot B can fail while flash is in erase/program mode.
// Solution: read a large chunk of B into SRAM, then erase+write that chunk to A.
// We use 128KB chunks (fits in SRAM alongside everything else).
#define OTA_CHUNK_SIZE (16 * 1024)
static uint8_t g_ota_chunk_buf[OTA_CHUNK_SIZE];

static void __no_inline_not_in_flash_func(ota_erase_write_chunk)(
    uint32_t dest_off, const uint8_t *ram_data, uint32_t len) {
    for (uint32_t off = 0; off < len; off += FLASH_SECTOR_SIZE) {
        uint32_t slen = len - off;
        if (slen > FLASH_SECTOR_SIZE) slen = FLASH_SECTOR_SIZE;

        flash_range_erase(dest_off + off, FLASH_SECTOR_SIZE);
        for (uint32_t p = 0; p < slen; p += 256)
            flash_range_program(dest_off + off + p, ram_data + off + p, 256);
        // Pad remainder of sector with 0xFF
        if (slen < FLASH_SECTOR_SIZE) {
            uint8_t pad[256];
            for (int i = 0; i < 256; i++) pad[i] = 0xFF;
            for (uint32_t p = slen; p < FLASH_SECTOR_SIZE; p += 256)
                flash_range_program(dest_off + off + p, pad, 256);
        }
    }
}

static void ota_write_chunk(const uint8_t *data, uint16_t len) {
    uint8_t page_buf[256];
    uint32_t abs_off = g_ota.target_base + g_ota.offset;

    while (len > 0 && g_ota.offset < OTA_SLOT_SIZE) {
        // Erase sector if needed
        if (abs_off >= g_ota.erased_up_to) {
            uint32_t irq = save_and_disable_interrupts();
            ota_flash_erase(g_ota.erased_up_to);
            restore_interrupts(irq);
            g_ota.erased_up_to += FLASH_SECTOR_SIZE;
        }

        uint16_t chunk = (len > 256) ? 256 : len;
        memcpy(page_buf, data, chunk);
        if (chunk < 256) memset(page_buf + chunk, 0xFF, 256 - chunk);

        uint32_t irq = save_and_disable_interrupts();
        ota_flash_program(abs_off, page_buf);
        restore_interrupts(irq);

        abs_off += chunk;
        g_ota.offset += chunk;
        data += chunk;
        len -= chunk;
    }

    g_ota.total_written = g_ota.offset;
}

// ============================================================
// Request dispatcher
// ============================================================

static void dispatch(struct tcp_pcb *pcb, const char *req, uint16_t req_len) {
    // Parse method
    http_verb_t verb = VERB_UNKNOWN;
    const char *path_start;
    if (strncmp(req, "GET ", 4) == 0) { verb = VERB_GET; path_start = req + 4; }
    else if (strncmp(req, "POST ", 5) == 0) { verb = VERB_POST; path_start = req + 5; }
    else if (strncmp(req, "PUT ", 4) == 0) { verb = VERB_PUT; path_start = req + 4; }
    else if (strncmp(req, "DELETE ", 7) == 0) { verb = VERB_DELETE; path_start = req + 7; }
    else {
        http_json(pcb, "405 Method Not Allowed", "{\"error\":\"only GET, POST, PUT, DELETE\"}");
        return;
    }

    // Extract path (up to space or ?)
    char path[128];
    int pi = 0;
    while (*path_start && *path_start != ' ' && *path_start != '?' && pi < 127)
        path[pi++] = *path_start++;
    path[pi] = '\0';

    // Capture query string (after ?)
    const char *query = NULL;
    if (*path_start == '?') query = path_start;

    // Find headers end + body
    const char *hdr_end = strstr(req, "\r\n\r\n");
    const uint8_t *body = NULL;
    uint16_t body_len = 0;
    if (hdr_end) {
        body = (const uint8_t *)(hdr_end + 4);
        body_len = req_len - (uint16_t)(body - (const uint8_t *)req);
    }

    if (verb == VERB_GET && strcmp(path, "/0/stats/0") == 0) {
        kv_stats_t stats = kv_stats();
        char json[96];
        int n = snprintf(json, sizeof(json),
                         "{\"a\":%lu,\"d\":%lu,\"f\":%lu,\"t\":%lu}",
                         (unsigned long)stats.active,
                         (unsigned long)stats.dead,
                         (unsigned long)stats.free,
                         (unsigned long)stats.total);
        if (n < 0 || n >= (int)sizeof(json)) {
            http_json(pcb, "500 Internal Server Error", "{\"error\":\"stats overflow\"}");
            return;
        }
        http_json(pcb, "200 OK", json);
        return;
    }

    if (verb == VERB_GET && strcmp(path, "/app.js") == 0) {
        (void)http_respond(pcb, "200 OK", "application/javascript",
                          (const uint8_t *)APP_JS, sizeof(APP_JS) - 1);
        return;
    }

    if (verb == VERB_GET && strcmp(path, "/gui") == 0) {
        (void)http_respond_with_headers(pcb, "302 Found", "text/plain",
                                        "Location: /\r\n",
                                        (const uint8_t *)"Redirecting", 11);
        return;
    }

    if (verb == VERB_GET && strncmp(path, "/w/0/", 5) == 0) {
        (void)http_respond_with_headers(pcb, "302 Found", "text/plain",
                                        "Location: /admin\r\n",
                                        (const uint8_t *)"Redirecting", 11);
        return;
    }

    if ((strncmp(path, "/meta/types", 11) == 0 || strncmp(path, "/meta/fields", 12) == 0) &&
        (verb == VERB_GET || verb == VERB_POST)) {
        handle_metadata(pcb, verb, path, body, body_len, req);
        return;
    }

    if (verb == VERB_GET && strcmp(path, "/status") == 0) {
        http_status_page(pcb, req);
        return;
    }

    // ---- Route: GET / — SSR home: login form or pack links ----
    if (verb == VERB_GET && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        user_session_t session;
        char pg[4096];
        int n = 0;

        if (!check_auth_session(req, &session)) {
            // Login form
            n = snprintf(pg, sizeof(pg),
                "<div style='max-width:300px;margin:80px auto'>"
                "<h1 style='color:#e94560'>&#x1F5C3; PicoWAL</h1>"
                "<p style='color:#888'>Hardware KV Storage</p>"
                "<form id=loginForm>"
                "<label>Username</label><input id=lu style='width:100%%'>"
                "<label>Password</label><input id=lp type=password style='width:100%%'>"
                "<button style='width:100%%;margin-top:8px'>Login</button>"
                "<div id=lerr style='color:#e94560;margin-top:4px'></div>"
                "</form></div>"
                "<script src=/app.js></script>");
        } else {
            // Home — list available packs
            n = snprintf(pg, sizeof(pg),
                "<h2>Welcome</h2>"
                "<div style='display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:12px'>");

            // Scan Pack 0 for schema cards
            uint32_t keys[32];
            uint32_t count = kv_range(0, 0xFFC00000u, keys, NULL, 32);
            for (uint32_t i = 0; i < count && n < (int)sizeof(pg) - 200; i++) {
                uint32_t pack_ord = keys[i] & 0x3FFFFF;
                if (pack_ord == 0) continue; // skip Pack 0 itself
                uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
                if (!kv_get_copy(keys[i], sbuf, &slen, NULL)) continue;
                char pname[32] = "?";
                uint8_t ords[32], types[32], maxlens[32]; char names[32][32];
                uint8_t fc = parse_schema(sbuf, slen, names, types, maxlens, ords, pname, 32);

                // Count cards in this pack
                uint32_t ckeys[2]; uint32_t cc = kv_range(((uint32_t)pack_ord << 22), 0xFFC00000u, ckeys, NULL, 2);

                n += snprintf(pg + n, sizeof(pg) - n,
                    "<a href='/pack/%lu' style='text-decoration:none'>"
                    "<div class=card style='margin:0'>"
                    "<div style='font-size:11px;color:#888'>Pack %lu</div>"
                    "<div style='font-size:18px;color:#0ff;margin:4px 0'>%s</div>"
                    "<div style='font-size:12px;color:#888'>%u fields &middot; %lu+ cards</div>"
                    "</div></a>",
                    (unsigned long)pack_ord, (unsigned long)pack_ord,
                    pname, (unsigned int)fc, (unsigned long)cc);
            }
            n += snprintf(pg + n, sizeof(pg) - n,
                "</div>"
                "<div style='margin-top:16px'>"
                "<button id=logoutBtn class=btn-sm>Logout</button>%s</div>"
                "<script src=/app.js></script>",
                user_auth_is_admin(&session)
                    ? " <a href='/admin' style='margin-left:8px' class=btn-sm>Admin</a>"
                      " <a href='/admin/meta' style='margin-left:8px' class=btn-sm>Schema</a>"
                    : "");
        }
        if (n > 0) http_page_req(pcb, req, pg, (uint16_t)n);
        return;
    }

    // ---- Route: GET /pack/{n} — SSR card list ----
    if (verb == VERB_GET && strncmp(path, "/pack/", 6) == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"login required\"}");
            return;
        }
        unsigned int pack_ord = 0, card_ord = 0;
        int parts = sscanf(path, "/pack/%u/%u", &pack_ord, &card_ord);

        if (parts == 2) {
            // SSR card form
            if (!user_auth_can_read(&session, (uint16_t)pack_ord)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no access\"}");
                return;
            }

            // Load schema
            uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
            char pname[32] = "?";
            uint8_t ords[32], ftypes[32], maxlens[32]; char names[32][32];
            memset(names, 0, sizeof(names));
            uint8_t fc = 0;
            if (kv_get_copy(((uint32_t)0 << 22) | pack_ord, sbuf, &slen, NULL))
                fc = parse_schema(sbuf, slen, names, ftypes, maxlens, ords, pname, 32);

            // Load data card
            uint32_t key = ((uint32_t)(pack_ord & 0x3FF) << 22) | (card_ord & 0x3FFFFF);
            uint8_t dbuf[KV_MAX_VALUE]; uint16_t dlen = KV_MAX_VALUE;
            bool exists = kv_get_copy(key, dbuf, &dlen, NULL);

            // Parse data values by ordinal
            char vals[32][64]; memset(vals, 0, sizeof(vals));
            // Map: ftypes_by_ord[ord] = type_code for decode
            uint8_t ftypes_by_ord[32]; memset(ftypes_by_ord, 0, sizeof(ftypes_by_ord));
            for (uint8_t i = 0; i < fc; i++) ftypes_by_ord[ords[i]] = ftypes[i];
            if (exists) parse_card_values(dbuf, dlen, vals, ftypes_by_ord, 32);

            char pg[4096]; int n = 0;
            n += snprintf(pg + n, sizeof(pg) - n,
                "<h2><a href='/pack/%u' style='color:#0ff'>%s</a> / Card %u%s</h2>"
                "<form id=cardForm data-pack='%u' data-card='%u'>",
                pack_ord, pname, card_ord,
                exists ? "" : " <span class=badge>NEW</span>",
                pack_ord, card_ord);

            for (uint8_t i = 0; i < fc && n < (int)sizeof(pg) - 500; i++) {
                const char *tn = type_name(ftypes[i]);
                // Skip password_hash and salt for display (ords 1,2 in pack 1)
                if (pack_ord == 1 && (ords[i] == 1 || ords[i] == 2)) continue;

                n += snprintf(pg + n, sizeof(pg) - n,
                    "<div style='margin-bottom:8px'>"
                    "<label style='display:flex;justify-content:space-between'>"
                    "<span>%s</span>"
                    "<span class=badge style='font-size:10px'>%s</span></label>",
                    names[i], tn);

                uint8_t tc = ftypes[i];
                if (tc == 0x07) {
                    // bool — checkbox
                    bool checked = vals[ords[i]][0] == 't' || vals[ords[i]][0] == '1';
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<select data-ord='%u' data-ftype='bool' style='width:100%%'>"
                        "<option value='false'%s>false</option>"
                        "<option value='true'%s>true</option></select>",
                        (unsigned)ords[i], checked ? "" : " selected", checked ? " selected" : "");
                } else if (tc == 18) {
                    // lookup — dropdown from target pack
                    uint8_t target_pack = maxlens[i];
                    uint32_t cur_val = 0;
                    if (vals[ords[i]][0]) cur_val = (uint32_t)strtoul(vals[ords[i]], NULL, 10);

                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<select data-ord='%u' data-ftype='lookup' style='width:100%%'>"
                        "<option value='0'>-- none --</option>",
                        (unsigned)ords[i]);

                    // Scan target pack for cards
                    uint32_t lkeys[32];
                    uint32_t lcount = kv_range(((uint32_t)(target_pack & 0x3FFu) << 22),
                                               0xFFC00000u, lkeys, NULL, 32);
                    for (uint32_t li = 0; li < lcount && n < (int)sizeof(pg) - 200; li++) {
                        uint32_t lid = lkeys[li] & 0x3FFFFF;
                        // Read field 0 for display name
                        char lname[32] = "";
                        uint8_t lbuf[256]; uint16_t llen = sizeof(lbuf);
                        if (kv_get_copy(lkeys[li], lbuf, &llen, NULL) && llen >= 4 &&
                            lbuf[0] == 0x7D && lbuf[1] == 0xCA) {
                            uint16_t lo = 4;
                            if (lo + 1 < llen) {
                                uint8_t lord = lbuf[lo] & 0x1F;
                                uint8_t lflen = lbuf[lo + 1];
                                if (lord == 0 && lo + 2 + lflen <= llen && lflen >= 1) {
                                    uint8_t sn = lbuf[lo + 2];
                                    if (sn > lflen - 1) sn = lflen - 1;
                                    if (sn > 31) sn = 31;
                                    memcpy(lname, lbuf + lo + 3, sn);
                                    lname[sn] = '\0';
                                }
                            }
                        }
                        n += snprintf(pg + n, sizeof(pg) - n,
                            "<option value='%lu'%s>%lu: %s</option>",
                            (unsigned long)lid, lid == cur_val ? " selected" : "",
                            (unsigned long)lid, lname[0] ? lname : "?");
                    }
                    n += snprintf(pg + n, sizeof(pg) - n, "</select>");
                } else if (tc == 0x11) {
                    // blob — read-only
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input data-ord='%u' data-ftype='blob' value='%s' style='width:100%%' readonly "
                        "title='Blob fields are read-only'>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else if (tc == 0x0A) {
                    // isodate — date picker
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input type=date data-ord='%u' data-ftype='isodate' value='%s' style='width:100%%'>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else if (tc == 0x0B) {
                    // isotime — time picker
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input type=time data-ord='%u' data-ftype='isotime' value='%s' style='width:100%%' step=1>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else if (tc == 0x0C) {
                    // isodatetime — datetime-local picker
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input type=datetime-local data-ord='%u' data-ftype='isodatetime' value='%s' style='width:100%%' step=1>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else if (tc == 0x01 || tc == 0x02 || tc == 0x03 || tc == 0x04 || tc == 0x05 || tc == 0x06) {
                    // numeric types — number input with min/max
                    const char *minmax = "";
                    if (tc == 0x01) minmax = " min=0 max=255";         // uint8
                    else if (tc == 0x04) minmax = " min=-128 max=127"; // int8
                    else if (tc == 0x02) minmax = " min=0 max=65535";  // uint16
                    else if (tc == 0x05) minmax = " min=-32768 max=32767"; // int16
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input type=number data-ord='%u' data-ftype='%s' value='%s' style='width:100%%'%s>",
                        (unsigned)ords[i], tn, vals[ords[i]], minmax);
                } else if (tc == 0x08 || tc == 0x09) {
                    // ascii, utf8 — text input with maxlength
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input data-ord='%u' data-ftype='%s' value='%s' style='width:100%%' maxlength='%u'>",
                        (unsigned)ords[i], tn, vals[ords[i]], (unsigned)maxlens[i]);
                } else if (tc == 0x10) {
                    // array_u16 — comma-separated
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input data-ord='%u' data-ftype='array_u16' value='%s' style='width:100%%' "
                        "placeholder='comma-separated numbers'>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else {
                    // fallback text input
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input data-ord='%u' data-ftype='%s' value='%s' style='width:100%%'>",
                        (unsigned)ords[i], tn, vals[ords[i]]);
                }

                n += snprintf(pg + n, sizeof(pg) - n, "</div>");
            }

            n += snprintf(pg + n, sizeof(pg) - n,
                "<div style='display:flex;gap:8px;margin-top:12px'>"
                "<button type=submit class=btn-ok>&#x1F4BE; Save</button>"
                "%s</div>"
                "<div id=saveMsg style='margin-top:8px'></div>"
                "</form><script src=/app.js></script>",
                user_auth_can_delete(&session, (uint16_t)pack_ord)
                    ? "<button type=button id=delBtn class=btn-del>Delete</button>" : "");

            http_page_req(pcb, req, pg, (uint16_t)n);
            return;
        }

        if (parts >= 1) {
            // SSR card list for pack
            if (!user_auth_can_read(&session, (uint16_t)pack_ord)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no access\"}");
                return;
            }

            // Load schema for pack name + field 0 name
            uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
            char pname[32] = "?";
            uint8_t ords[32], ftypes[32], maxlens[32]; char names[32][32];
            memset(names, 0, sizeof(names));
            uint8_t fc = 0;
            if (kv_get_copy(((uint32_t)0 << 22) | pack_ord, sbuf, &slen, NULL))
                fc = parse_schema(sbuf, slen, names, ftypes, maxlens, ords, pname, 32);

            char pg[4096]; int n = 0;
            n += snprintf(pg + n, sizeof(pg) - n,
                "<h2>%s <span style='color:#888'>(Pack %u)</span></h2>"
                "<table><thead><tr><th>Card</th><th>%s</th><th></th></tr></thead><tbody>",
                pname, pack_ord, fc > 0 ? names[0] : "Value");

            uint32_t keys[64];
            uint32_t count = kv_range(((uint32_t)(pack_ord & 0x3FFu) << 22), 0xFFC00000u, keys, NULL, 64);
            for (uint32_t i = 0; i < count && n < (int)sizeof(pg) - 300; i++) {
                uint32_t cid = keys[i] & 0x3FFFFF;
                // Read field 0 value for display
                char display[48] = "";
                uint8_t cbuf[512]; uint16_t clen = sizeof(cbuf);
                if (kv_get_copy(keys[i], cbuf, &clen, NULL) && clen >= 4 &&
                    cbuf[0] == 0x7D && cbuf[1] == 0xCA) {
                    uint16_t coff = 4;
                    if (coff + 1 < clen) {
                        uint8_t cord = cbuf[coff] & 0x1F;
                        uint8_t cflen = cbuf[coff + 1];
                        if (cord == ords[0] && coff + 2 + cflen <= clen && fc > 0) {
                            decode_field_str(display, sizeof(display), ftypes[0],
                                           cbuf + coff + 2, cflen);
                        }
                    }
                }
                n += snprintf(pg + n, sizeof(pg) - n,
                    "<tr><td>%lu</td><td><a href='/pack/%u/%lu'>%s</a></td>"
                    "<td style='width:40px;text-align:center'><a href='/pack/%u/%lu'>&#x270E;</a></td></tr>",
                    (unsigned long)cid, pack_ord, (unsigned long)cid,
                    display[0] ? display : "(empty)",
                    pack_ord, (unsigned long)cid);
            }

            n += snprintf(pg + n, sizeof(pg) - n,
                "</tbody></table>"
                "<div style='margin-top:12px'>"
                "<a href='/pack/%u/%lu' class=btn-sm style='text-decoration:none;padding:6px 16px;background:#e94560;color:#fff;border-radius:4px'>"
                "+ New Card</a></div>",
                pack_ord, (unsigned long)(count > 0 ? (keys[count-1] & 0x3FFFFF) + 1 : 0));

            http_page_req(pcb, req, pg, (uint16_t)n);
            return;
        }
    }

    // ---- Route: GET /admin — SSR user management ----
    if (verb == VERB_GET && strcmp(path, "/admin") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }

        char pg[4096]; int n = 0;
        n += snprintf(pg + n, sizeof(pg) - n,
            "<h2>Users</h2>"
            "<table><thead><tr><th>Card</th><th>Username</th><th>Read</th><th>Write</th><th>Delete</th></tr></thead><tbody>");

        uint32_t keys[64];
        uint32_t count = kv_range(((uint32_t)1 << 22), 0xFFC00000u, keys, NULL, 64);
        for (uint32_t i = 0; i < count && n < (int)sizeof(pg) - 300; i++) {
            uint32_t cid = keys[i] & 0x3FFFFF;
            uint8_t cbuf[512]; uint16_t clen = sizeof(cbuf);
            if (!kv_get_copy(keys[i], cbuf, &clen, NULL)) continue;

            char vals[32][64]; memset(vals, 0, sizeof(vals));
            uint8_t ftbo[32]; memset(ftbo, 0, sizeof(ftbo));
            ftbo[0] = 14; ftbo[3] = 5; ftbo[5] = 0x10; ftbo[6] = 0x10; ftbo[7] = 0x10;
            parse_card_values(cbuf, clen, vals, ftbo, 32);

            n += snprintf(pg + n, sizeof(pg) - n,
                "<tr><td>%lu</td><td><a href='/pack/1/%lu'>%s</a>%s</td>"
                "<td>%s</td><td>%s</td><td>%s</td></tr>",
                (unsigned long)cid, (unsigned long)cid,
                vals[0][0] ? vals[0] : "?",
                (vals[3][0] == '1' || (vals[3][0] && atoi(vals[3]) & 1)) ? " <span class='badge badge-admin'>ADMIN</span>" : "",
                vals[5], vals[6], vals[7]);
        }

        n += snprintf(pg + n, sizeof(pg) - n,
            "</tbody></table>"
            "<div class=card><h2>Create User</h2>"
            "<form id=userForm>"
            "<div class=row>"
            "<div><label>Username</label><input id=nu></div>"
            "<div><label>Password</label><input id=np type=password></div>"
            "<div><label>Flags</label><select id=nf><option value=0>Normal</option><option value=1>Admin</option></select></div></div>"
            "<div class=row>"
            "<div><label>Read Packs (comma-sep)</label><input id=nr value='4,5'></div>"
            "<div><label>Write Packs</label><input id=nw value='4,5'></div>"
            "<div><label>Delete Packs</label><input id=nd></div></div>"
            "<button style='margin-top:8px'>Create User</button>"
            "</form></div>"
            "<div class=card><h2>Change Password</h2>"
            "<form id=passForm>"
            "<div style='max-width:400px'>"
            "<label>Current</label><input id=cpOld type=password>"
            "<label>New</label><input id=cpNew type=password>"
            "<label>Confirm</label><input id=cpConfirm type=password>"
            "<button style='margin-top:8px;width:100%%'>Change</button>"
            "<div id=cpMsg style='margin-top:4px'></div>"
            "</div></form></div>"
            "<script src=/app.js></script>");

        http_page_req(pcb, req, pg, (uint16_t)n);
        return;
    }

    // ---- Route: GET /admin/meta — SSR pack schema list ----
    if (verb == VERB_GET && strcmp(path, "/admin/meta") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }

        char pg[4096]; int n = 0;
        n += snprintf(pg + n, sizeof(pg) - n,
            "<h2>Schema Editor</h2>"
            "<div style='display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:12px'>");

        uint32_t keys[32];
        uint32_t count = kv_range(0, 0xFFC00000u, keys, NULL, 32);
        for (uint32_t i = 0; i < count && n < (int)sizeof(pg) - 300; i++) {
            uint32_t pack_ord = keys[i] & 0x3FFFFF;
            uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
            if (!kv_get_copy(keys[i], sbuf, &slen, NULL)) continue;
            char pname[32] = "?";
            uint8_t ords[32], types[32], maxlens[32]; char names[32][32];
            uint8_t fc = parse_schema(sbuf, slen, names, types, maxlens, ords, pname, 32);

            n += snprintf(pg + n, sizeof(pg) - n,
                "<a href='/admin/meta/%lu' style='text-decoration:none'>"
                "<div class=card style='margin:0'>"
                "<div style='font-size:11px;color:#888'>Pack %lu</div>"
                "<div style='font-size:18px;color:#0ff;margin:4px 0'>%s</div>"
                "<div style='font-size:12px;color:#888'>%u fields</div>"
                "</div></a>",
                (unsigned long)pack_ord, (unsigned long)pack_ord, pname, (unsigned int)fc);
        }

        n += snprintf(pg + n, sizeof(pg) - n,
            "</div>"
            "<div class=card style='margin-top:16px'><h2>+ New Pack</h2>"
            "<form id=packForm><div class=row>"
            "<div><label>Ordinal</label><input id=po type=number min=0 max=1023></div>"
            "<div><label>Name</label><input id=pn placeholder='e.g. devices'></div>"
            "<div style='align-self:end'><button>Create</button></div>"
            "</div></form></div>"
            "<script src=/app.js></script>");

        http_page_req(pcb, req, pg, (uint16_t)n);
        return;
    }

    // ---- Route: GET /admin/meta/{n} — SSR schema editor for pack N ----
    if (verb == VERB_GET && strncmp(path, "/admin/meta/", 12) == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        unsigned int pack_ord = 0;
        sscanf(path, "/admin/meta/%u", &pack_ord);

        uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
        char pname[32] = "?";
        uint8_t ords[32], ftypes[32], maxlens[32]; char names[32][32];
        memset(names, 0, sizeof(names));
        uint8_t fc = 0;
        if (kv_get_copy(((uint32_t)0 << 22) | pack_ord, sbuf, &slen, NULL))
            fc = parse_schema(sbuf, slen, names, ftypes, maxlens, ords, pname, 32);

        char pg[4096]; int n = 0;
        n += snprintf(pg + n, sizeof(pg) - n,
            "<h2><a href='/admin/meta' style='color:#0ff'>&laquo; Schema</a> / Pack %u: %s</h2>"
            "<table><thead><tr><th>#</th><th>Field</th><th>Type</th><th>Max</th></tr></thead><tbody>",
            pack_ord, pname);

        for (uint8_t i = 0; i < fc && n < (int)sizeof(pg) - 200; i++) {
            const char *tn = type_name(ftypes[i]);
            n += snprintf(pg + n, sizeof(pg) - n,
                "<tr><td>%u</td><td>%s</td><td>%s%s</td><td>%u</td></tr>",
                (unsigned int)ords[i], names[i], tn,
                ftypes[i] == 18 ? " &rarr; pack" : "",
                (unsigned int)maxlens[i]);
        }
        if (fc == 0) n += snprintf(pg + n, sizeof(pg) - n,
            "<tr><td colspan=4 style='color:#888'>No fields yet</td></tr>");

        n += snprintf(pg + n, sizeof(pg) - n,
            "</tbody></table>"
            "<div class=card><h2>Add / Edit Field</h2>"
            "<form id=fieldForm data-pack='%u'><div class=row>"
            "<div><label>Ordinal</label><input id=fo type=number value='%u' min=0 max=31></div>"
            "<div><label>Name</label><input id=fn placeholder='e.g. name'></div>"
            "<div><label>Type</label><select id=ft>",
            pack_ord, (unsigned int)fc);

        // Field type options
        static const char *ft_names[] = {
            "bool","char","char[]","byte","byte[]","uint8","int8","int16",
            "int32","uint16","uint32","isodate","isotime","isodatetime",
            "utf8","latin1","array_u16","blob","lookup"
        };
        for (int i = 0; i < 19 && n < (int)sizeof(pg) - 100; i++) {
            n += snprintf(pg + n, sizeof(pg) - n,
                "<option value='%s'%s>%s</option>",
                ft_names[i], i == 14 ? " selected" : "", ft_names[i]);
        }

        n += snprintf(pg + n, sizeof(pg) - n,
            "</select></div></div>"
            "<div class=row>"
            "<div><label>Max Length</label><input id=fm type=number value=32 min=1 max=255></div>"
            "<div style='align-self:end'><button>Save Field</button></div>"
            "</div></form></div>"
            "<div class=card style='border-color:#c0392b'>"
            "<h2 style='color:#e94560'>Danger Zone</h2>"
            "<p>Delete this pack and all its cards.</p>"
            "<button onclick=\"if(confirm('Delete pack %u and ALL its cards?'))fetch('/admin/meta/%u',{method:'DELETE',credentials:'same-origin'}).then(function(r){if(r.ok)location.href='/admin/meta';else alert('Failed')})\" "
            "style='background:#c0392b'>Delete Pack %u</button></div>"
            "<script src=/app.js></script>",
            pack_ord, pack_ord, pack_ord);

        http_page_req(pcb, req, pg, (uint16_t)n);
        return;
    }

    // ---- Route: DELETE /admin/meta/{n} — delete pack schema + all cards ----
    if (verb == VERB_DELETE && strncmp(path, "/admin/meta/", 12) == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        unsigned int pack_ord = 0;
        sscanf(path, "/admin/meta/%u", &pack_ord);

        // Delete all cards in this pack
        uint32_t keys[256];
        uint32_t count = kv_range(((uint32_t)(pack_ord & 0x3FFu) << 22), 0xFFC00000u, keys, NULL, 256);
        for (uint32_t i = 0; i < count; i++) {
            kv_delete(keys[i]);
        }

        // Delete the schema card from Pack 0
        kv_delete(((uint32_t)0 << 22) | pack_ord);

        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"deleted\":%lu}", (unsigned long)(count + 1));
        http_json(pcb, "200 OK", resp);
        return;
    }

    // ---- Route: POST /admin/meta/new — create new pack schema ----
    if (verb == VERB_POST && strcmp(path, "/admin/meta/new") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        // Parse JSON body: {"ordinal":N,"name":"..."}
        char jbuf[256];
        uint16_t jlen = body_len < sizeof(jbuf) - 1 ? body_len : sizeof(jbuf) - 1;
        memcpy(jbuf, body, jlen); jbuf[jlen] = '\0';

        unsigned int ord = 0; char name[32] = "";
        char *p = strstr(jbuf, "\"ordinal\":");
        if (p) ord = (unsigned int)atoi(p + 10);
        p = strstr(jbuf, "\"name\":\"");
        if (p) { p += 8; char *end = strchr(p, '"'); if (end && end - p < 32) { memcpy(name, p, end - p); name[end - p] = '\0'; } }

        if (name[0] == '\0') {
            http_json(pcb, "400 Bad Request", "{\"error\":\"name required\"}");
            return;
        }

        // Build minimal schema card (no fields yet)
        user_auth_schema_field_t empty[1];
        if (user_auth_seed_schema((uint16_t)ord, name, empty, 0)) {
            http_json(pcb, "200 OK", "{\"ok\":true}");
        } else {
            http_json(pcb, "500 Internal Server Error", "{\"error\":\"schema write failed\"}");
        }
        return;
    }

    // ---- Route: POST /admin/meta/{n} — add/update field in pack schema ----
    if (verb == VERB_POST && strncmp(path, "/admin/meta/", 12) == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        unsigned int pack_ord = 0;
        sscanf(path, "/admin/meta/%u", &pack_ord);

        // Parse body: "name|type|maxlen"
        char fbuf[96];
        uint16_t flen = body_len < sizeof(fbuf) - 1 ? body_len : sizeof(fbuf) - 1;
        memcpy(fbuf, body, flen); fbuf[flen] = '\0';

        char *p1 = strchr(fbuf, '|'); if (!p1) { http_json(pcb, "400 Bad Request", "{\"error\":\"expected name|type|maxlen\"}"); return; }
        char *p2 = strchr(p1 + 1, '|'); if (!p2) { http_json(pcb, "400 Bad Request", "{\"error\":\"expected name|type|maxlen\"}"); return; }
        *p1 = '\0'; *p2 = '\0';
        char *fname = fbuf; char *ftname = p1 + 1; uint8_t fmaxlen = (uint8_t)atoi(p2 + 1);

        // Resolve type code
        uint8_t ftype_code = 255;
        if (!metadata_field_type_parse(ftname, &ftype_code)) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"unknown field type\"}");
            return;
        }

        // Load current schema card
        uint8_t sbuf[512]; uint16_t slen = sizeof(sbuf);
        char pname[32] = "";
        uint8_t ords[32], ftypes[32], maxlens_arr[32]; char snames[32][32];
        memset(snames, 0, sizeof(snames));
        uint8_t fc = 0;
        uint32_t schema_key = ((uint32_t)0 << 22) | pack_ord;
        if (kv_get_copy(schema_key, sbuf, &slen, NULL))
            fc = parse_schema(sbuf, slen, snames, ftypes, maxlens_arr, ords, pname, 32);

        if (pname[0] == '\0') snprintf(pname, sizeof(pname), "pack_%u", pack_ord);

        // Find next free ordinal or update existing by name
        int8_t target_idx = -1;
        uint8_t next_ord = 0;
        for (uint8_t i = 0; i < fc; i++) {
            if (strcmp(snames[i], fname) == 0) { target_idx = (int8_t)i; break; }
            if (ords[i] >= next_ord) next_ord = ords[i] + 1;
        }

        user_auth_schema_field_t fields[32];
        uint8_t new_fc = 0;
        // Copy existing fields
        for (uint8_t i = 0; i < fc && new_fc < 31; i++) {
            if (target_idx >= 0 && i == (uint8_t)target_idx) {
                // Update this field
                fields[new_fc].ord = ords[i];
                fields[new_fc].type = ftype_code;
                fields[new_fc].maxlen = fmaxlen;
                fields[new_fc].name = fname;
                new_fc++;
            } else {
                fields[new_fc].ord = ords[i];
                fields[new_fc].type = ftypes[i];
                fields[new_fc].maxlen = maxlens_arr[i];
                fields[new_fc].name = snames[i];
                new_fc++;
            }
        }
        // Add new field if not updating
        if (target_idx < 0 && new_fc < 32) {
            fields[new_fc].ord = next_ord;
            fields[new_fc].type = ftype_code;
            fields[new_fc].maxlen = fmaxlen;
            fields[new_fc].name = fname;
            new_fc++;
        }

        if (user_auth_seed_schema((uint16_t)pack_ord, pname, fields, new_fc)) {
            http_json(pcb, "200 OK", "{\"ok\":true}");
        } else {
            http_json(pcb, "500 Internal Server Error", "{\"error\":\"schema write failed\"}");
        }
        return;
    }

    // ---- Route: POST /admin/users → create user (server-side hashing) ----
    if (verb == VERB_POST && strcmp(path, "/admin/users") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"not authenticated\"}");
            return;
        }
        if (!user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }

        // Parse JSON-ish body: {"username":"x","password":"y","flags":N,
        //   "readPacks":[...],"writePacks":[...],"deletePacks":[...]}
        // Simple extraction — not a full JSON parser
        char jbuf[512];
        uint16_t jlen = body_len < sizeof(jbuf) - 1 ? body_len : sizeof(jbuf) - 1;
        memcpy(jbuf, body, jlen);
        jbuf[jlen] = '\0';

        // Extract string values
        char username[32] = "", password[32] = "";
        uint8_t flags = 0;
        uint16_t rpacks[20] = {0}, wpacks[20] = {0}, dpacks[20] = {0};
        uint8_t rc = 0, wc = 0, dc = 0;

        // Username
        char *p = strstr(jbuf, "\"username\":\"");
        if (p) {
            p += 12;
            char *end = strchr(p, '"');
            if (end && end - p < 32) { memcpy(username, p, end - p); username[end - p] = '\0'; }
        }
        // Password
        p = strstr(jbuf, "\"password\":\"");
        if (p) {
            p += 12;
            char *end = strchr(p, '"');
            if (end && end - p < 32) { memcpy(password, p, end - p); password[end - p] = '\0'; }
        }
        // Flags
        p = strstr(jbuf, "\"flags\":");
        if (p) flags = (uint8_t)atoi(p + 8);

        // Parse pack arrays helper
        #define PARSE_PACKS(key, arr, cnt) do { \
            p = strstr(jbuf, "\"" key "\":["); \
            if (p) { \
                p = strchr(p, '[') + 1; \
                while (*p && *p != ']' && cnt < 20) { \
                    while (*p == ' ' || *p == ',') p++; \
                    if (*p >= '0' && *p <= '9') { \
                        arr[cnt++] = (uint16_t)atoi(p); \
                        while (*p >= '0' && *p <= '9') p++; \
                    } else break; \
                } \
            } \
        } while(0)

        PARSE_PACKS("readPacks", rpacks, rc);
        PARSE_PACKS("writePacks", wpacks, wc);
        PARSE_PACKS("deletePacks", dpacks, dc);
        #undef PARSE_PACKS

        if (strlen(username) == 0 || strlen(password) == 0) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"username and password required\"}");
            return;
        }

        int32_t card_id = user_auth_create_user(
            username, (uint8_t)strlen(username),
            password, (uint8_t)strlen(password),
            flags,
            rpacks, rc, wpacks, wc, dpacks, dc);

        if (card_id >= 0) {
            char resp[64];
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"card\":%ld}", (long)card_id);
            http_json(pcb, "200 OK", resp);
        } else {
            http_json(pcb, "409 Conflict", "{\"error\":\"username taken or create failed\"}");
        }
        return;
    }

    // ---- Route: POST/PUT/DELETE /pack/{n}/{card} → KV write/delete ----
    if ((verb == VERB_POST || verb == VERB_PUT || verb == VERB_DELETE) &&
        strncmp(path, "/pack/", 6) == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"login required\"}");
            return;
        }
        unsigned int pack_val = 0, card_val = 0;
        if (sscanf(path, "/pack/%u/%u", &pack_val, &card_val) < 2) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"expected /pack/{n}/{card}\"}");
            return;
        }
        if (verb == VERB_DELETE) {
            if (!user_auth_can_delete(&session, (uint16_t)pack_val)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no delete access\"}");
                return;
            }
        } else {
            if (!user_auth_can_write(&session, (uint16_t)pack_val)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no write access\"}");
                return;
            }
        }
        handle_kv(pcb, verb, (uint16_t)pack_val, (uint32_t)card_val, body, body_len);
        return;
    }

    // ---- Route: POST /login ----
    if (verb == VERB_POST && strcmp(path, "/login") == 0) {
        if (!body || body_len < 64) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"login body must be 64 bytes\"}");
            return;
        }
        uint8_t ulen = body[0];
        if (ulen > 31) ulen = 31;
        const char *username = (const char *)(body + 1);
        uint8_t plen = body[32];
        if (plen > 31) plen = 31;
        const char *password = (const char *)(body + 33);

        uint8_t token[SESSION_TOKEN_LEN];
        int32_t card_id = user_auth_login(username, ulen, password, plen, token);
        if (card_id < 0) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"invalid credentials\"}");
            return;
        }

        // Build Set-Cookie header
        char token_hex[33];
        user_auth_format_token(token, token_hex);
        char cookie_hdr[128];
        snprintf(cookie_hdr, sizeof(cookie_hdr),
                 "Set-Cookie: sid=%s; Path=/; HttpOnly; SameSite=Lax\r\n",
                 token_hex);

        // Response body: 4 bytes LE user_card
        uint8_t resp_body[4];
        resp_body[0] = (uint8_t)(card_id & 0xFF);
        resp_body[1] = (uint8_t)((card_id >> 8) & 0xFF);
        resp_body[2] = (uint8_t)((card_id >> 16) & 0xFF);
        resp_body[3] = (uint8_t)((card_id >> 24) & 0xFF);

        http_respond_with_headers(pcb, "200 OK", "application/octet-stream",
                                  cookie_hdr, resp_body, 4);
        return;
    }

    // ---- Route: GET/POST /logout ----
    if ((verb == VERB_GET || verb == VERB_POST) && strcmp(path, "/logout") == 0) {
        uint8_t token[SESSION_TOKEN_LEN];
        if (user_auth_parse_cookie(req, token)) {
            user_auth_logout(token);
        }
        // Clear cookie and redirect to login
        http_respond_with_headers(pcb, "302 Found", "text/plain",
                                  "Set-Cookie: sid=; Path=/; Max-Age=0\r\nLocation: /\r\n",
                                  (const uint8_t *)"Logged out", 10);
        return;
    }

    // ---- Route: POST /0/1/{id}/_passwd ----
    if (verb == VERB_POST && strncmp(path, "/0/1/", 5) == 0 && strstr(path, "/_passwd")) {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"not authenticated\"}");
            return;
        }

        unsigned int card_id = 0;
        sscanf(path, "/0/1/%u/", &card_id);

        // Only self or admin can change password
        if (session.user_card != card_id && !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"not authorized\"}");
            return;
        }

        if (!body || body_len < 64) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"passwd body must be 64 bytes\"}");
            return;
        }

        uint8_t old_len = body[0]; if (old_len > 31) old_len = 31;
        uint8_t new_len = body[32]; if (new_len > 31) new_len = 31;

        if (user_auth_change_password(card_id,
                                       (const char *)(body + 1), old_len,
                                       (const char *)(body + 33), new_len)) {
            http_json(pcb, "200 OK", "{\"ok\":true}");
        } else {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"wrong password\"}");
        }
        return;
    }

    // ---- Route: /0/{type}?start=N&limit=N → list cards ----
    if (verb == VERB_GET && path[0] == '/' && path[1] == '0' && path[2] == '/') {
        // Check if this is a list query (has query string) or a card read
        unsigned int type_val = 0, id_val = 0;
        int parsed = sscanf(path, "/0/%u/%u", &type_val, &id_val);

        // If only type parsed and there's a query string, it's a list
        if (parsed == 1 && query) {
            user_session_t session;
            if (!check_auth_session(req, &session)) {
                http_json(pcb, "401 Unauthorized", "{\"error\":\"not authenticated\"}");
                return;
            }
            if (!user_auth_can_read(&session, (uint16_t)type_val)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no read access\"}");
                return;
            }

            // Parse start and limit from query string
            unsigned int start = 0, limit = 50;
            const char *sp = strstr(query, "start=");
            if (sp) start = (unsigned int)atoi(sp + 6);
            const char *lp = strstr(query, "limit=");
            if (lp) limit = (unsigned int)atoi(lp + 6);
            if (limit > 100) limit = 100;

            uint32_t keys[256];
            uint32_t total = kv_range(((uint32_t)(type_val & 0x3FFu) << 22),
                                       0xFFC00000u, keys, NULL, 256);

            // Build binary response: [cardOrd:4][payloadLen:2][payload:N]...
            uint8_t resp[4000];
            uint16_t roff = 0;
            uint32_t skip = start, sent = 0;

            for (uint32_t i = 0; i < total && sent < limit; i++) {
                if (skip > 0) { skip--; continue; }
                uint32_t card_ord = keys[i] & 0x3FFFFFu;

                uint8_t val[KV_MAX_VALUE];
                uint16_t vlen = KV_MAX_VALUE;
                if (!kv_get_copy(keys[i], val, &vlen, NULL)) continue;

                if (roff + 6 + vlen > sizeof(resp)) break;

                // Card ordinal (LE u32)
                resp[roff++] = (uint8_t)(card_ord & 0xFF);
                resp[roff++] = (uint8_t)((card_ord >> 8) & 0xFF);
                resp[roff++] = (uint8_t)((card_ord >> 16) & 0xFF);
                resp[roff++] = (uint8_t)((card_ord >> 24) & 0xFF);
                // Payload length (LE u16)
                resp[roff++] = (uint8_t)(vlen & 0xFF);
                resp[roff++] = (uint8_t)((vlen >> 8) & 0xFF);
                // Payload
                memcpy(resp + roff, val, vlen);
                roff += vlen;
                sent++;
            }

            // Sentinel: 0xFFFFFFFF
            if (roff + 4 <= sizeof(resp)) {
                resp[roff++] = 0xFF; resp[roff++] = 0xFF;
                resp[roff++] = 0xFF; resp[roff++] = 0xFF;
            }

            http_respond(pcb, "200 OK", "application/octet-stream", resp, roff);
            return;
        }
    }

    if (verb == VERB_GET && path[0] == '/' && path[1] == 'I' && path[2] == 'd' && path[3] == 's' && path[4] == '/') {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"not authenticated\"}");
            return;
        }

        unsigned int type_val = 0;
        if (sscanf(path, "/Ids/%u/", &type_val) == 1) {
            uint32_t keys[2049];
            uint8_t id_buf[4096];
            uint32_t count = kv_range(((uint32_t)(type_val & 0x3FFu) << 22), 0xFFC00000u, keys, NULL, 2049u);
            if (count > 2048u) {
                http_json(pcb, "413 Payload Too Large", "{\"error\":\"too many ids\"}");
                return;
            }

            for (uint32_t i = 0; i < count; i++) {
                uint32_t id_val = keys[i] & 0x3FFFFFu;
                if (id_val > 0xFFFFu) {
                    http_json(pcb, "422 Unprocessable Entity", "{\"error\":\"id exceeds uint16\"}");
                    return;
                }
                id_buf[i * 2u] = (uint8_t)(id_val & 0xFFu);
                id_buf[i * 2u + 1u] = (uint8_t)((id_val >> 8) & 0xFFu);
            }

            (void)http_respond(pcb, "200 OK", "application/octet-stream", id_buf, (uint16_t)(count * 2u));
            return;
        }
    }

    if (verb == VERB_GET && path[0] == '/' && path[1] == 'w' && path[2] == '/') {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"not authenticated\"}");
            return;
        }

        unsigned int type_val = 0;
        if (sscanf(path, "/w/%u/", &type_val) == 1) {
            uint32_t keys[257];
            uint32_t count = kv_range(((uint32_t)(type_val & 0x3FFu) << 22), 0xFFC00000u, keys, NULL, 257u);
            if (count > 256u) {
                http_json(pcb, "413 Payload Too Large", "{\"error\":\"too many instances\"}");
                return;
            }

            char body[4096];
            int n = snprintf(body, sizeof(body), "COUNT=%lu\n", (unsigned long)count);
            if (n < 0 || n >= (int)sizeof(body)) {
                http_json(pcb, "500 Internal Server Error", "{\"error\":\"instance list overflow\"}");
                return;
            }

            for (uint32_t i = 0; i < count; i++) {
                uint32_t id_val = keys[i] & 0x3FFFFFu;
                int wrote = snprintf(body + n, sizeof(body) - (size_t)n, "%lu\n", (unsigned long)id_val);
                if (wrote < 0 || wrote >= (int)(sizeof(body) - (size_t)n)) {
                    http_json(pcb, "500 Internal Server Error", "{\"error\":\"instance list overflow\"}");
                    return;
                }
                n += wrote;
            }

            (void)http_respond(pcb, "200 OK", "text/plain", (const uint8_t *)body, (uint16_t)n);
            return;
        }
    }

    // ---- Route: /0/{type}/{id} → KV operations (session or PSK auth + RBAC) ----
    if (path[0] == '/' && path[1] == '0' && path[2] == '/') {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"not authenticated\"}");
            return;
        }

        // Parse /0/{type}/{id}
        unsigned int type_val = 0, id_val = 0;
        if (sscanf(path, "/0/%u/%u", &type_val, &id_val) < 2) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"expected /0/{type}/{id}\"}");
            return;
        }

        if (type_val > 1023) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"type must be 0-1023\"}");
            return;
        }
        if (id_val > 4194303) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"id must be 0-4194303\"}");
            return;
        }

        // RBAC check
        if (verb == VERB_GET) {
            if (!user_auth_can_read(&session, (uint16_t)type_val)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no read access to this pack\"}");
                return;
            }
        } else if (verb == VERB_DELETE) {
            if (!user_auth_can_delete(&session, (uint16_t)type_val)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no delete access to this pack\"}");
                return;
            }
        } else {
            if (!user_auth_can_write(&session, (uint16_t)type_val)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no write access to this pack\"}");
                return;
            }
        }

        handle_kv(pcb, verb, (uint16_t)type_val, (uint32_t)id_val, body, body_len);
        return;
    }

    // ---- Route: GET /admin/log — debug log (HTML or ?raw for plain text) ----
    if (verb == VERB_GET && strcmp(path, "/admin/log") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        char logbuf[LOG_BUF_SIZE];
        uint16_t len = log_read(logbuf, sizeof(logbuf));

        if (query && strstr(query, "raw")) {
            http_respond(pcb, "200 OK", "text/plain", (const uint8_t *)logbuf, len);
            return;
        }

        char pg[LOG_BUF_SIZE + 512];
        int n = snprintf(pg, sizeof(pg),
            "<h2>Debug Log</h2>"
            "<div class=card>"
            "<div style='display:flex;gap:8px;margin-bottom:8px'>"
            "<button onclick='location.reload()'>Refresh</button>"
            "<button onclick=\"fetch('/admin/reboot',{method:'POST',credentials:'same-origin'}).then(function(){document.getElementById('logpre').textContent='Rebooting...'})\" style='background:#c0392b'>Reboot</button>"
            "<button onclick=\"fetch('/admin/sd/init',{method:'POST',credentials:'same-origin'}).then(function(r){return r.text()}).then(function(t){alert(t);location.reload()})\" >Init SD</button>"
            "</div>"
            "<pre id=logpre style='max-height:600px;overflow-y:auto;font-size:12px'>%s</pre>"
            "</div>", logbuf);
        http_page_req(pcb, req, pg, (uint16_t)n);
        return;
    }

    // ---- Route: POST /admin/reboot — remote reboot ----
    if (verb == VERB_POST && strcmp(path, "/admin/reboot") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        http_json(pcb, "200 OK", "{\"ok\":true,\"rebooting\":true}");
        tcp_output(pcb);
        sleep_ms(500);
        watchdog_reboot(0, 0, 0);
        while (1) tight_loop_contents();
        return;
    }

    // ---- Route: GET /admin/flash?addr=N&len=N — flash/XIP hex dump ----
    if (verb == VERB_GET && strcmp(path, "/admin/flash") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }

        uint32_t addr = 0x10000000; uint32_t rlen = 256;
        if (query) {
            const char *a = strstr(query, "addr="); if (a) addr = strtoul(a + 5, NULL, 0);
            const char *l = strstr(query, "len="); if (l) rlen = strtoul(l + 4, NULL, 0);
        }
        if (rlen > 1024) rlen = 1024;

        // Validate: must be in XIP range (0x10000000–0x103FFFFF) 
        if (addr < 0x10000000 || addr + rlen > 0x10400000) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"addr must be in 0x10000000-0x103FFFFF\"}");
            return;
        }

        if (query && strstr(query, "raw")) {
            http_respond(pcb, "200 OK", "application/octet-stream",
                        (const uint8_t *)addr, (uint16_t)rlen);
            return;
        }

        char pg[4096]; int n = 0;
        n += snprintf(pg + n, sizeof(pg) - n,
            "<h2>Flash Inspector</h2>"
            "<div class=card>"
            "<form method=GET><div class=row>"
            "<div><label>Address (hex)</label><input name=addr value='0x%08lx'></div>"
            "<div><label>Length</label><input name=len type=number value='%lu' max=1024></div>"
            "<div style='align-self:end'><button>Read</button></div>"
            "</div></form>"
            "<pre style='font-size:12px'>",
            (unsigned long)addr, (unsigned long)rlen);

        const uint8_t *mem = (const uint8_t *)addr;
        for (uint32_t i = 0; i < rlen && n < (int)sizeof(pg) - 80; i += 16) {
            n += snprintf(pg + n, sizeof(pg) - n, "%08lx: ", (unsigned long)(addr + i));
            for (uint32_t j = 0; j < 16 && i + j < rlen; j++)
                n += snprintf(pg + n, sizeof(pg) - n, "%02x ", mem[i + j]);
            n += snprintf(pg + n, sizeof(pg) - n, " ");
            for (uint32_t j = 0; j < 16 && i + j < rlen; j++) {
                uint8_t c = mem[i + j];
                pg[n++] = (c >= 0x20 && c < 0x7F) ? c : '.';
            }
            pg[n++] = '\n';
        }

        n += snprintf(pg + n, sizeof(pg) - n, "</pre></div>");
        http_page_req(pcb, req, pg, (uint16_t)n);
        return;
    }

    // ---- Route: GET /admin/ram?addr=N&len=N — SRAM hex dump ----
    if (verb == VERB_GET && strcmp(path, "/admin/ram") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }

        uint32_t addr = 0x20000000; uint32_t rlen = 256;
        if (query) {
            const char *a = strstr(query, "addr="); if (a) addr = strtoul(a + 5, NULL, 0);
            const char *l = strstr(query, "len="); if (l) rlen = strtoul(l + 4, NULL, 0);
        }
        if (rlen > 1024) rlen = 1024;

        // Validate: must be in SRAM range (0x20000000–0x20082000)
        if (addr < 0x20000000 || addr + rlen > 0x20082000) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"addr must be in 0x20000000-0x20082000\"}");
            return;
        }

        if (query && strstr(query, "raw")) {
            http_respond(pcb, "200 OK", "application/octet-stream",
                        (const uint8_t *)addr, (uint16_t)rlen);
            return;
        }

        char pg[4096]; int n = 0;
        n += snprintf(pg + n, sizeof(pg) - n,
            "<h2>RAM Inspector</h2>"
            "<div class=card>"
            "<form method=GET><div class=row>"
            "<div><label>Address (hex)</label><input name=addr value='0x%08lx'></div>"
            "<div><label>Length</label><input name=len type=number value='%lu' max=1024></div>"
            "<div style='align-self:end'><button>Read</button></div>"
            "</div></form>"
            "<pre style='font-size:12px'>",
            (unsigned long)addr, (unsigned long)rlen);

        const uint8_t *mem = (const uint8_t *)addr;
        for (uint32_t i = 0; i < rlen && n < (int)sizeof(pg) - 80; i += 16) {
            n += snprintf(pg + n, sizeof(pg) - n, "%08lx: ", (unsigned long)(addr + i));
            for (uint32_t j = 0; j < 16 && i + j < rlen; j++)
                n += snprintf(pg + n, sizeof(pg) - n, "%02x ", mem[i + j]);
            n += snprintf(pg + n, sizeof(pg) - n, " ");
            for (uint32_t j = 0; j < 16 && i + j < rlen; j++) {
                uint8_t c = mem[i + j];
                pg[n++] = (c >= 0x20 && c < 0x7F) ? c : '.';
            }
            pg[n++] = '\n';
        }

        n += snprintf(pg + n, sizeof(pg) - n, "</pre></div>");
        http_page_req(pcb, req, pg, (uint16_t)n);
        return;
    }

    // ---- Route: POST /admin/sd/init — trigger SD card init ----
    if (verb == VERB_POST && strcmp(path, "/admin/sd/init") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        bool ok = sd_init();
        if (ok) {
            sd_info_t sdi; sd_get_info(&sdi);
            char resp[256];
            snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"type\":\"%s\",\"mb\":%lu,\"blocks\":%lu,\"debug\":\"%s\"}",
                     sdi.sdhc ? "SDHC" : "SDSC", (unsigned long)sdi.capacity_mb,
                     (unsigned long)sdi.block_count, sd_get_debug());
            http_json(pcb, "200 OK", resp);
        } else {
            char resp[192];
            snprintf(resp, sizeof(resp), "{\"ok\":false,\"debug\":\"%s\"}", sd_get_debug());
            http_json(pcb, "200 OK", resp);
        }
        return;
    }

    // ---- OTA: POST /update/begin — prepare inactive slot ----
    if (verb == VERB_POST && strcmp(path, "/update/begin") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        if (g_ota.active) {
            http_json(pcb, "409 Conflict", "{\"error\":\"OTA already in progress\"}");
            return;
        }

        bool from_b = running_from_slot_b();
        g_ota.active = true;
        g_ota.target_base = from_b ? OTA_SLOT_A : OTA_SLOT_B;
        g_ota.offset = 0;
        g_ota.total_written = 0;
        g_ota.erased_up_to = g_ota.target_base;

        printf("[ota] BEGIN — running from slot %c, writing to slot %c (0x%lx)\n",
               from_b ? 'B' : 'A', from_b ? 'A' : 'B',
               (unsigned long)g_ota.target_base);

        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"max\":%lu,\"slot\":\"%c\",\"target\":\"%c\"}",
                 (unsigned long)OTA_SLOT_SIZE,
                 from_b ? 'B' : 'A', from_b ? 'A' : 'B');
        http_json(pcb, "200 OK", resp);
        return;
    }

    // ---- OTA: POST /update/chunk — write firmware chunk ----
    if (verb == VERB_POST && strcmp(path, "/update/chunk") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        if (!g_ota.active) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"call /update/begin first\"}");
            return;
        }
        if (!body || body_len == 0) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"empty chunk\"}");
            return;
        }
        if (g_ota.offset + body_len > OTA_SLOT_SIZE) {
            http_json(pcb, "413 Payload Too Large", "{\"error\":\"exceeds slot size (512KB)\"}");
            g_ota.active = false;
            return;
        }

        ota_write_chunk(body, body_len);

        char resp[80];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"written\":%lu}", (unsigned long)g_ota.total_written);
        http_json(pcb, "200 OK", resp);
        return;
    }

    // ---- OTA: POST /update/commit — reboot ----
    if (verb == VERB_POST && strcmp(path, "/update/commit") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        if (!g_ota.active) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"no OTA in progress\"}");
            return;
        }

        bool from_b = running_from_slot_b();

        printf("[ota] COMMIT — %lu bytes to slot %c\n",
               (unsigned long)g_ota.total_written,
               from_b ? 'A' : 'B');

        // Send response before the destructive part
        char resp[80];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"total\":%lu,\"rebooting\":true}",
                 (unsigned long)g_ota.total_written);
        http_json(pcb, "200 OK", resp);
        tcp_output(pcb);
        sleep_ms(500);

        if (!from_b) {
            // Running from A, wrote to B — copy B→A in 128KB chunks.
            // Read each chunk from XIP BEFORE touching flash, then erase+write from SRAM.
            printf("[ota] Copying slot B → A (%lu bytes) in 128KB chunks...\n",
                   (unsigned long)g_ota.total_written);

            hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);

            uint32_t size = g_ota.total_written;
            for (uint32_t coff = 0; coff < size; coff += OTA_CHUNK_SIZE) {
                uint32_t clen = size - coff;
                if (clen > OTA_CHUNK_SIZE) clen = OTA_CHUNK_SIZE;

                // Phase 1: Read from slot B via XIP into SRAM (interrupts enabled, XIP works)
                const uint8_t *src = (const uint8_t *)(OTA_XIP_BASE + OTA_SLOT_B + coff);
                memcpy(g_ota_chunk_buf, src, clen);

                // Phase 2: Write to slot A from SRAM (interrupts disabled, no XIP needed)
                uint32_t irq = save_and_disable_interrupts();
                ota_erase_write_chunk(OTA_SLOT_A + coff, g_ota_chunk_buf, clen);
                restore_interrupts(irq);
            }
        }
        // If from B, wrote directly to A — just reboot

        watchdog_reboot(0, 0, 0);
        while (1) tight_loop_contents();
        return;
    }

    // ---- OTA: GET /update — admin upload page ----
    if (verb == VERB_GET && strcmp(path, "/update") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }

        bool from_b = running_from_slot_b();
        char pg[2048]; int n = 0;
        n += snprintf(pg + n, sizeof(pg) - n,
            "<h2>Firmware Update</h2>"
            "<div class=card>"
            "<p>Running from <strong>Slot %c</strong> (0x%s). "
            "OTA will write to <strong>Slot %c</strong>.</p>"
            "<p>Select a <code>.bin</code> firmware file. Max 512KB.</p>"
            "<input type=file id=fw accept='.bin'>"
            "<button onclick=doOTA() style='margin-top:8px;width:100%%'>Upload &amp; Flash</button>"
            "<pre id=otaLog>Ready</pre>"
            "</div>"
            "<script>"
            "async function doOTA(){"
            "var f=document.getElementById('fw').files[0];"
            "if(!f){document.getElementById('otaLog').textContent='No file selected';return}"
            "var log=document.getElementById('otaLog');"
            "log.textContent='Starting OTA ('+f.size+' bytes)...';"
            "var r=await fetch('/update/begin',{method:'POST',credentials:'same-origin'});"
            "if(!r.ok){log.textContent='BEGIN failed: '+r.status;return}"
            "var info=await r.json();log.textContent='Writing to slot '+info.target+'...';"
            "var buf=new Uint8Array(await f.arrayBuffer());"
            "var chunk=1024,off=0;"
            "while(off<buf.length){"
            "var end=Math.min(off+chunk,buf.length);"
            "var r=await fetch('/update/chunk',{method:'POST',credentials:'same-origin',body:buf.slice(off,end)});"
            "if(!r.ok){log.textContent='CHUNK failed at '+off+': '+r.status;return}"
            "off=end;log.textContent='Written '+off+'/'+buf.length+' ('+Math.round(100*off/buf.length)+'%%)'}"
            "log.textContent='Committing...';"
            "await fetch('/update/commit',{method:'POST',credentials:'same-origin'}).catch(function(){});"
            "log.textContent='Rebooting! Page will reload in 15s...';"
            "setTimeout(function(){location.reload()},15000)}"
            "</script>",
            from_b ? 'B' : 'A', from_b ? "080000" : "000000",
            from_b ? 'A' : 'B');

        http_page_req(pcb, req, pg, (uint16_t)n);
        return;
    }

    // ---- Route: POST /query — execute a query ----
    if (verb == VERB_POST && strcmp(path, "/query") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"login required\"}");
            return;
        }
        if (!body || body_len == 0) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"query body required\"}");
            return;
        }

        // Null-terminate body
        char qbuf[512];
        uint16_t qlen = body_len < sizeof(qbuf) - 1 ? body_len : sizeof(qbuf) - 1;
        memcpy(qbuf, body, qlen); qbuf[qlen] = '\0';

        query_t q = query_parse(qbuf);
        char result[4096];
        int rlen = query_execute(&q, result, sizeof(result));
        http_respond(pcb, "200 OK", "application/json",
                    (const uint8_t *)result, (uint16_t)rlen);
        return;
    }

    // ---- Route: GET /query — query form page ----
    if (verb == VERB_GET && strcmp(path, "/query") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"login required\"}");
            return;
        }

        static const char qpage[] =
            "<h2>Query</h2>"
            "<div class=card>"
            "<textarea id=qtext style='width:100%;min-height:120px;font-family:monospace'"
            " placeholder='S:name,code\nF:countries\nW:code|IN|GB,US,DE'>S:name,code\nF:countries</textarea>"
            "<button onclick=runQuery() style='margin-top:8px;width:100%'>Run Query</button>"
            "<pre id=qresult style='margin-top:8px;max-height:400px;overflow-y:auto'>Results will appear here</pre>"
            "</div>"
            "<script>"
            "function runQuery(){"
            "var q=document.getElementById('qtext').value;"
            "fetch('/query',{method:'POST',credentials:'same-origin',body:q})"
            ".then(function(r){return r.text()})"
            ".then(function(t){"
            "try{document.getElementById('qresult').textContent=JSON.stringify(JSON.parse(t),null,2)}"
            "catch(e){document.getElementById('qresult').textContent=t}})}"
            "</script>";

        http_page_req(pcb, req, qpage, sizeof(qpage) - 1);
        return;
    }

    http_json(pcb, "404 Not Found", "{\"error\":\"unknown route\"}");
}

// ============================================================
// TCP connection plumbing
// ============================================================

static http_conn_t g_http_conns[HTTP_CONN_COUNT];

static http_conn_t *http_conn_alloc(void) {
    for (uint8_t i = 0; i < HTTP_CONN_COUNT; i++) {
        if (!g_http_conns[i].in_use) {
            memset(&g_http_conns[i], 0, sizeof(g_http_conns[i]));
            g_http_conns[i].in_use = true;
            g_http_conns[i].slot_idx = i;
            return &g_http_conns[i];
        }
    }
    return NULL;
}

static void http_close_conn(struct tcp_pcb *pcb, http_conn_t *conn) {
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
        }
    }
    if (conn) {
        conn->in_use = false;
    }
}

static void http_err(void *arg, err_t err) {
    http_conn_t *conn = (http_conn_t *)arg;
    if (conn) {
        (void)err;
        conn->in_use = false;
    }
}

static err_t http_poll(void *arg, struct tcp_pcb *pcb) {
    http_conn_t *conn = (http_conn_t *)arg;
    if (conn->response_pending_close) {
        if (conn->bytes_in_flight == 0) {
            http_close_conn(pcb, conn);
            return ERR_OK;
        }
        if (conn->close_polls < 8) {
            conn->close_polls++;
            return ERR_OK;
        }
        http_close_conn(pcb, conn);
    }
    return ERR_OK;
}

static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    http_conn_t *conn = (http_conn_t *)arg;
    if (!conn) return ERR_OK;
    if (conn->bytes_in_flight > len) conn->bytes_in_flight -= len;
    else conn->bytes_in_flight = 0;
    if (conn->response_pending_close && conn->bytes_in_flight == 0) {
        http_close_conn(pcb, conn);
    } else if (conn->keep_alive && conn->bytes_in_flight == 0) {
        conn->len = 0;
        memset(conn->buf, 0, sizeof(conn->buf));
        tcp_recv(pcb, http_recv);
    }
    return ERR_OK;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_conn_t *conn = (http_conn_t *)arg;
    if (!p) {
        http_close_conn(pcb, conn);
        return ERR_OK;
    }
    if (!conn || err != ERR_OK) {
        pbuf_free(p);
        http_close_conn(pcb, conn);
        return ERR_OK;
    }

    g_http_last_activity_ms = to_ms_since_boot(get_absolute_time());
    uint16_t copy = p->tot_len;
    if ((uint32_t)conn->len + copy >= HTTP_BUF_SIZE) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        conn->bytes_in_flight = http_respond(pcb, "413 Payload Too Large", "application/json",
                                             (const uint8_t *)"{\"error\":\"request too large\"}",
                                             sizeof("{\"error\":\"request too large\"}") - 1);
        conn->response_pending_close = true;
        conn->close_polls = 0;
        tcp_recv(pcb, NULL);
        return ERR_OK;
    }
    pbuf_copy_partial(p, conn->buf + conn->len, copy, 0);
    conn->len += copy;
    conn->buf[conn->len] = 0;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    char *hdr_end = strstr((char *)conn->buf, "\r\n\r\n");
    if (!hdr_end) return ERR_OK;

    uint32_t content_length = 0;
    if (!parse_content_length((const char *)conn->buf, &content_length)) {
        conn->bytes_in_flight = http_respond(pcb, "400 Bad Request", "application/json",
                                             (const uint8_t *)"{\"error\":\"invalid content-length\"}",
                                             sizeof("{\"error\":\"invalid content-length\"}") - 1);
        conn->response_pending_close = true;
        conn->close_polls = 0;
        tcp_recv(pcb, NULL);
        return ERR_OK;
    }

    uint32_t header_len = (uint32_t)((hdr_end + 4) - (char *)conn->buf);
    if (header_len + content_length > HTTP_BUF_SIZE - 1u) {
        conn->bytes_in_flight = http_respond(pcb, "413 Payload Too Large", "application/json",
                                             (const uint8_t *)"{\"error\":\"request too large\"}",
                                             sizeof("{\"error\":\"request too large\"}") - 1);
        conn->response_pending_close = true;
        conn->close_polls = 0;
        tcp_recv(pcb, NULL);
        return ERR_OK;
    }

    if ((uint32_t)conn->len < header_len + content_length) return ERR_OK;

    char method[8] = {0};
    char path[64] = {0};
    sscanf((const char *)conn->buf, "%7s %63s", method, path);
    conn->keep_alive = request_wants_keep_alive((const char *)conn->buf);
    uint16_t before = tcp_sndbuf(pcb);
    dispatch(pcb, (const char *)conn->buf, conn->len);
    uint16_t after = tcp_sndbuf(pcb);
    conn->bytes_in_flight = (before >= after) ? (before - after) : 0;
    conn->response_pending_close = !conn->keep_alive;
    conn->close_polls = 0;
    if (conn->keep_alive || conn->bytes_in_flight > 0) {
        tcp_recv(pcb, NULL);
    }
    if (conn->bytes_in_flight == 0 && conn->keep_alive) {
        conn->len = 0;
        memset(conn->buf, 0, sizeof(conn->buf));
        tcp_recv(pcb, http_recv);
    } else if (conn->bytes_in_flight == 0) {
        http_close_conn(pcb, conn);
    }
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) return err;
    http_conn_t *conn = http_conn_alloc();
    if (!conn) {
        static const char busy[] = "{\"error\":\"server busy\"}";
        http_respond(pcb, "503 Service Unavailable", "application/json",
                     (const uint8_t *)busy, sizeof(busy) - 1);
        tcp_output(pcb);
        tcp_abort(pcb);
        return ERR_MEM;
    }
    tcp_nagle_disable(pcb);
    tcp_arg(pcb, conn);
    tcp_recv(pcb, http_recv);
    tcp_err(pcb, http_err);
    tcp_sent(pcb, http_sent);
    tcp_poll(pcb, http_poll, 1);
    return ERR_OK;
}

void web_server_init(wal_state_t *wal) {
    g_wal = wal;

    // Initialize user auth (seeds admin on first boot)
    user_auth_init();

    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, HTTP_PORT);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, http_accept);
    printf("[http] Listening on port %d\n", HTTP_PORT);
}
