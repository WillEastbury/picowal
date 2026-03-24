#include "web_server.h"
#include "kv_flash.h"
#include "wal_defs.h"

#include "pico/stdlib.h"
#include "lwip/tcp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ============================================================
// Minimal HTTP server with jump-table routing
//
// Routes:
//   GET  /              → embedded root page
//   GET  /{slug}        → embedded page by slug
//   GET  /0/{type}/{id} → read KV record (requires Auth header)
//   POST /0/{type}/{id} → write KV record (requires Auth header)
//
// Auth: "Authorization: PSK <64-char hex>" header required for /0/
// Only GET and POST supported. Everything else → 405.
// type = uint10 (0-1023), id = uint22 (0-4194303)
// ============================================================

#define HTTP_PORT 80
#define HTTP_BUF_SIZE 4096  // max request size (headers + body up to 4KB value)

static wal_state_t *g_wal;

// Runtime PSK for HTTP auth (set from net_core)
static uint8_t g_http_psk[32];
static bool g_http_psk_set = false;

void web_server_set_psk(const uint8_t psk[32]) {
    memcpy(g_http_psk, psk, 32);
    g_http_psk_set = true;
}

// ============================================================
// Embedded pages — add pages here as {slug, content, len}
// ============================================================

static const char PAGE_ROOT[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>PicoWAL</title>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font:14px/1.5 monospace;background:#1a1a2e;color:#e0e0e0;padding:16px}"
"h1{color:#0ff;margin-bottom:8px}h2{color:#0f0;margin:12px 0 4px}"
".c{background:#16213e;padding:12px;border-radius:8px;margin:8px 0}"
"input,button{font:inherit;padding:4px 8px;margin:2px;border:1px solid #444;"
"background:#0d1117;color:#e0e0e0;border-radius:4px}"
"button{background:#0ff;color:#000;cursor:pointer;font-weight:bold}"
"pre{background:#0d1117;padding:8px;border-radius:4px;overflow-x:auto;margin:4px 0}"
".s{display:inline-block;margin:0 8px;padding:4px 8px;background:#0d1117;border-radius:4px}"
".s b{color:#0ff}"
"</style></head><body>"
"<h1>PicoWAL</h1>"
"<div class=c id=st></div>"
"<div class=c><h2>READ</h2>"
"Type:<input id=gt size=5 value=0> ID:<input id=gi size=8 value=0> "
"<button onclick=doGet()>GET</button><pre id=go></pre></div>"
"<div class=c><h2>WRITE</h2>"
"Type:<input id=pt size=5 value=0> ID:<input id=pi size=8 value=0><br>"
"Value:<input id=pv size=40> <button onclick=doPut()>POST</button>"
"<pre id=po></pre></div>"
"<div class=c><h2>LIST</h2>"
"Type:<input id=lt size=5 value=0> <button onclick=doList()>LIST</button>"
"<pre id=lo></pre></div>"
"<script>"
"const K=document.getElementById.bind(document),"
"P=(t,i)=>'/0/'+t+'/'+i,"
"H={'Authorization':'PSK '+localStorage.getItem('psk')||''};"
"function S(){fetch('/0/stats/0',{headers:H}).then(r=>r.ok?r.text():'{}')"
".then(t=>{try{let d=JSON.parse(t);K('st').innerHTML="
"'<span class=s>Active:<b>'+d.a+'</b></span>'"
"+'<span class=s>Dead:<b>'+d.d+'</b></span>'"
"+'<span class=s>Free:<b>'+d.f+'</b></span>'}catch(e){}})}"
"function doGet(){fetch(P(K('gt').value,K('gi').value),{headers:H})"
".then(r=>r.text()).then(t=>K('go').textContent=t)}"
"function doPut(){fetch(P(K('pt').value,K('pi').value),"
"{method:'POST',headers:H,body:K('pv').value})"
".then(r=>r.text()).then(t=>{K('po').textContent=t;S()})}"
"function doList(){fetch('/0/'+K('lt').value+'/0',{headers:H})"
".then(r=>r.text()).then(t=>K('lo').textContent=t)}"
"if(!localStorage.getItem('psk'))"
"{let p=prompt('Enter PSK (64 hex chars):');if(p)localStorage.setItem('psk',p);"
"H.Authorization='PSK '+p}"
"S();setInterval(S,5000);"
"</script></body></html>";

typedef struct {
    const char *slug;     // URL path (without leading /)
    const char *content;
    uint16_t    len;
    const char *mime;
} embedded_page_t;

static const embedded_page_t PAGES[] = {
    {"",           PAGE_ROOT, sizeof(PAGE_ROOT) - 1, "text/html"},
    {"index.html", PAGE_ROOT, sizeof(PAGE_ROOT) - 1, "text/html"},
    // Add more pages here: {"about", ABOUT_HTML, sizeof(ABOUT_HTML)-1, "text/html"},
    {NULL, NULL, 0, NULL}
};

// ============================================================
// HTTP helpers
// ============================================================

static void http_respond(struct tcp_pcb *pcb, const char *status,
                         const char *ctype, const uint8_t *body, uint16_t blen) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        status, ctype, blen);
    tcp_write(pcb, hdr, n, TCP_WRITE_FLAG_COPY);
    if (blen > 0) tcp_write(pcb, body, blen, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

static void http_json(struct tcp_pcb *pcb, const char *status, const char *json) {
    http_respond(pcb, status, "application/json", (const uint8_t *)json, strlen(json));
}

// ============================================================
// Auth: parse "Authorization: PSK <64 hex chars>" from headers
// Returns true if valid.
// ============================================================

static bool hex_decode(const char *hex, uint8_t *out, int len) {
    for (int i = 0; i < len; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        uint8_t v = 0;
        if (hi >= '0' && hi <= '9') v = (hi - '0') << 4;
        else if (hi >= 'a' && hi <= 'f') v = (hi - 'a' + 10) << 4;
        else if (hi >= 'A' && hi <= 'F') v = (hi - 'A' + 10) << 4;
        else return false;
        if (lo >= '0' && lo <= '9') v |= lo - '0';
        else if (lo >= 'a' && lo <= 'f') v |= lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') v |= lo - 'A' + 10;
        else return false;
        out[i] = v;
    }
    return true;
}

static bool check_auth(const char *headers) {
    if (!g_http_psk_set) return false;
    const char *auth = strstr(headers, "Authorization: PSK ");
    if (!auth) auth = strstr(headers, "authorization: PSK ");
    if (!auth) return false;
    auth += 19;  // skip "Authorization: PSK "

    uint8_t provided[32];
    if (!hex_decode(auth, provided, 32)) return false;

    // Constant-time compare
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= provided[i] ^ g_http_psk[i];
    return diff == 0;
}

// ============================================================
// Route handlers — jump table
// ============================================================

typedef enum { VERB_GET, VERB_POST, VERB_UNKNOWN } http_verb_t;

typedef void (*route_handler_t)(struct tcp_pcb *pcb, http_verb_t verb,
                                uint16_t type_id, uint32_t record_id,
                                const uint8_t *body, uint16_t body_len);

// /0/{type}/{id} — KV operations
static void handle_kv(struct tcp_pcb *pcb, http_verb_t verb,
                      uint16_t type_id, uint32_t record_id,
                      const uint8_t *body, uint16_t body_len) {
    uint32_t key = ((uint32_t)(type_id & 0x3FF) << 22) | (record_id & 0x3FFFFF);

    if (verb == VERB_GET) {
        uint16_t len = 0;
        const uint8_t *val = kv_get(key, &len);
        if (!val) {
            http_json(pcb, "404 Not Found", "{\"error\":\"not found\"}");
        } else {
            http_respond(pcb, "200 OK", "application/octet-stream", val, len);
        }
    } else {
        // POST — write
        if (body_len > KV_MAX_VALUE) {
            http_json(pcb, "413 Payload Too Large", "{\"error\":\"too large\"}");
            return;
        }
        if (kv_put(key, body, body_len)) {
            http_json(pcb, "200 OK", "{\"ok\":true}");
        } else {
            http_json(pcb, "507 Insufficient Storage", "{\"error\":\"full\"}");
        }
    }
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
    else {
        http_json(pcb, "405 Method Not Allowed", "{\"error\":\"only GET and POST\"}");
        return;
    }

    // Extract path (up to space or ?)
    char path[128];
    int pi = 0;
    while (*path_start && *path_start != ' ' && *path_start != '?' && pi < 127)
        path[pi++] = *path_start++;
    path[pi] = '\0';

    // Find headers end + body
    const char *hdr_end = strstr(req, "\r\n\r\n");
    const uint8_t *body = NULL;
    uint16_t body_len = 0;
    if (hdr_end) {
        body = (const uint8_t *)(hdr_end + 4);
        body_len = req_len - (uint16_t)(body - (const uint8_t *)req);
    }

    // ---- Route: GET / or GET /{slug} → embedded pages ----
    if (verb == VERB_GET && (path[0] == '/' && (path[1] == '\0' || path[1] != '0' || path[2] != '/'))) {
        const char *slug = path[1] ? path + 1 : "";
        for (const embedded_page_t *p = PAGES; p->slug; p++) {
            if (strcmp(slug, p->slug) == 0) {
                http_respond(pcb, "200 OK", p->mime, (const uint8_t *)p->content, p->len);
                return;
            }
        }
        http_json(pcb, "404 Not Found", "{\"error\":\"page not found\"}");
        return;
    }

    // ---- Route: /0/{type}/{id} → KV operations (auth required) ----
    if (path[0] == '/' && path[1] == '0' && path[2] == '/') {
        if (!check_auth(req)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"invalid PSK\"}");
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

        handle_kv(pcb, verb, (uint16_t)type_val, (uint32_t)id_val, body, body_len);
        return;
    }

    http_json(pcb, "404 Not Found", "{\"error\":\"unknown route\"}");
}

// ============================================================
// TCP connection plumbing
// ============================================================

typedef struct {
    uint8_t buf[HTTP_BUF_SIZE];
    uint16_t len;
} http_conn_t;

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_conn_t *conn = (http_conn_t *)arg;
    if (!p) { if (conn) free(conn); tcp_close(pcb); return ERR_OK; }

    uint16_t copy = p->tot_len;
    if (conn->len + copy > HTTP_BUF_SIZE) copy = HTTP_BUF_SIZE - conn->len;
    pbuf_copy_partial(p, conn->buf + conn->len, copy, 0);
    conn->len += copy;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (!strstr((char *)conn->buf, "\r\n\r\n")) return ERR_OK;

    dispatch(pcb, (const char *)conn->buf, conn->len);

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    free(conn);
    tcp_close(pcb);
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) return err;
    http_conn_t *conn = calloc(1, sizeof(http_conn_t));
    if (!conn) { tcp_abort(pcb); return ERR_MEM; }
    tcp_arg(pcb, conn);
    tcp_recv(pcb, http_recv);
    return ERR_OK;
}

void web_server_init(wal_state_t *wal) {
    g_wal = wal;
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, HTTP_PORT);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, http_accept);
    printf("[http] Listening on port %d\n", HTTP_PORT);
}
