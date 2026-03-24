#include "web_server.h"
#include "kv_flash.h"
#include "wal_defs.h"

#include "pico/stdlib.h"
#include "lwip/tcp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ============================================================
// Minimal HTTP/1.0 server — no dependencies beyond raw lwIP TCP
//
// Routes:
//   GET  /           → admin shell HTML
//   GET  /api/stats  → JSON KV stats
//   GET  /api/kv?key=<uint32>  → JSON value for key
//   POST /api/kv?key=<uint32>  → write value (body = raw bytes)
//   DELETE /api/kv?key=<uint32> → delete key
//   GET  /api/keys?type=<uint16> → JSON list of keys for type
// ============================================================

#define HTTP_PORT 80
#define HTTP_BUF_SIZE 2048

static wal_state_t *g_wal;

// ---- Embedded HTML shell ----
static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>PicoWAL</title>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font:14px/1.5 monospace;background:#1a1a2e;color:#e0e0e0;padding:16px}"
"h1{color:#0ff;margin-bottom:8px}h2{color:#0f0;margin:12px 0 4px}"
".card{background:#16213e;padding:12px;border-radius:8px;margin:8px 0}"
"input,button{font:inherit;padding:4px 8px;margin:2px;border:1px solid #444;background:#0d1117;color:#e0e0e0;border-radius:4px}"
"button{background:#0ff;color:#000;cursor:pointer;font-weight:bold}button:hover{background:#0a0}"
"pre{background:#0d1117;padding:8px;border-radius:4px;overflow-x:auto;margin:4px 0}"
".stat{display:inline-block;margin:0 12px;padding:4px 8px;background:#0d1117;border-radius:4px}"
".stat b{color:#0ff}"
"</style></head><body>"
"<h1>PicoWAL Admin</h1>"
"<div class=card id=stats></div>"
"<div class=card><h2>GET</h2>"
"Type:<input id=gt size=6 value=0> ID:<input id=gi size=8 value=0> "
"<button onclick=doGet()>Read</button>"
"<pre id=gout></pre></div>"
"<div class=card><h2>PUT</h2>"
"Type:<input id=pt size=6 value=0> ID:<input id=pi size=8 value=0><br>"
"Value:<input id=pv size=40 placeholder='text value'> "
"<button onclick=doPut()>Write</button>"
"<pre id=pout></pre></div>"
"<div class=card><h2>DELETE</h2>"
"Type:<input id=dt size=6 value=0> ID:<input id=di size=8 value=0> "
"<button onclick=doDel()>Delete</button>"
"<pre id=dout></pre></div>"
"<div class=card><h2>LIST KEYS</h2>"
"Type:<input id=lt size=6 value=0> "
"<button onclick=doList()>List</button>"
"<pre id=lout></pre></div>"
"<script>"
"const TB=10,IB=22,TM=(1<<TB)-1,IM=(1<<IB)-1;"
"function pk(t,i){return((t&TM)<<IB)|(i&IM)}"
"function S(){fetch('/api/stats').then(r=>r.json()).then(d=>{"
"document.getElementById('stats').innerHTML="
"'<span class=stat>Active: <b>'+d.active+'</b></span>'"
"+'<span class=stat>Dead: <b>'+d.dead+'</b></span>'"
"+'<span class=stat>Free: <b>'+d.free+'</b></span>'"
"+'<span class=stat>Total: <b>'+d.total+'</b></span>'})}"
"function doGet(){let k=pk(+document.getElementById('gt').value,+document.getElementById('gi').value);"
"fetch('/api/kv?key='+k).then(r=>r.text()).then(t=>document.getElementById('gout').textContent=t)}"
"function doPut(){let k=pk(+document.getElementById('pt').value,+document.getElementById('pi').value);"
"fetch('/api/kv?key='+k,{method:'POST',body:document.getElementById('pv').value})"
".then(r=>r.text()).then(t=>{document.getElementById('pout').textContent=t;S()})}"
"function doDel(){let k=pk(+document.getElementById('dt').value,+document.getElementById('di').value);"
"fetch('/api/kv?key='+k,{method:'DELETE'}).then(r=>r.text()).then(t=>{document.getElementById('dout').textContent=t;S()})}"
"function doList(){let t=+document.getElementById('lt').value,p=(t&TM)<<IB,m=TM<<IB;"
"fetch('/api/keys?prefix='+p+'&mask='+m).then(r=>r.text()).then(t=>document.getElementById('lout').textContent=t)}"
"S();setInterval(S,5000);"
"</script></body></html>";

// ---- HTTP response helpers ----

static void http_send(struct tcp_pcb *pcb, const char *status,
                      const char *content_type, const uint8_t *body, uint16_t body_len) {
    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET,POST,DELETE,OPTIONS\r\n"
        "Connection: close\r\n\r\n",
        status, content_type, body_len);

    tcp_write(pcb, hdr, hdr_len, TCP_WRITE_FLAG_COPY);
    if (body_len > 0)
        tcp_write(pcb, body, body_len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

static void http_send_str(struct tcp_pcb *pcb, const char *status,
                          const char *content_type, const char *body) {
    http_send(pcb, status, content_type, (const uint8_t *)body, strlen(body));
}

// ---- URL parsing ----

static uint32_t parse_uint_param(const char *url, const char *name) {
    char search[32];
    snprintf(search, sizeof(search), "%s=", name);
    const char *p = strstr(url, search);
    if (!p) return 0;
    return (uint32_t)strtoul(p + strlen(search), NULL, 10);
}

// ---- Request handler ----

static void handle_request(struct tcp_pcb *pcb, const char *method,
                           const char *url, const uint8_t *body, uint16_t body_len) {

    // OPTIONS (CORS preflight)
    if (strcmp(method, "OPTIONS") == 0) {
        http_send_str(pcb, "204 No Content", "text/plain", "");
        return;
    }

    // GET / → admin shell
    if (strcmp(method, "GET") == 0 && (strcmp(url, "/") == 0 || strcmp(url, "/index.html") == 0)) {
        http_send(pcb, "200 OK", "text/html", (const uint8_t *)INDEX_HTML, sizeof(INDEX_HTML) - 1);
        return;
    }

    // GET /api/stats
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/stats", 10) == 0) {
        kv_stats_t s = kv_stats();
        char json[128];
        snprintf(json, sizeof(json),
            "{\"active\":%lu,\"dead\":%lu,\"free\":%lu,\"total\":%lu}",
            (unsigned long)s.active, (unsigned long)s.dead,
            (unsigned long)s.free, (unsigned long)s.total);
        http_send_str(pcb, "200 OK", "application/json", json);
        return;
    }

    // GET /api/kv?key=N
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/kv", 7) == 0) {
        uint32_t key = parse_uint_param(url, "key");
        uint16_t len = 0;
        const uint8_t *val = kv_get(key, &len);
        if (!val) {
            http_send_str(pcb, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
        } else {
            // Return raw value with length header
            char hdr_extra[64];
            snprintf(hdr_extra, sizeof(hdr_extra), "application/octet-stream");
            http_send(pcb, "200 OK", hdr_extra, val, len);
        }
        return;
    }

    // POST /api/kv?key=N (body = value)
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/kv", 7) == 0) {
        uint32_t key = parse_uint_param(url, "key");
        if (kv_put(key, body, body_len)) {
            http_send_str(pcb, "200 OK", "application/json", "{\"ok\":true}");
        } else {
            http_send_str(pcb, "507 Insufficient Storage", "application/json", "{\"error\":\"store full\"}");
        }
        return;
    }

    // DELETE /api/kv?key=N
    if (strcmp(method, "DELETE") == 0 && strncmp(url, "/api/kv", 7) == 0) {
        uint32_t key = parse_uint_param(url, "key");
        if (kv_delete(key)) {
            http_send_str(pcb, "200 OK", "application/json", "{\"ok\":true}");
        } else {
            http_send_str(pcb, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
        }
        return;
    }

    // GET /api/keys?prefix=N&mask=M
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/keys", 9) == 0) {
        uint32_t prefix = parse_uint_param(url, "prefix");
        uint32_t mask = parse_uint_param(url, "mask");
        uint32_t keys[64];
        uint16_t sectors[64];
        uint32_t count = kv_range(prefix, mask, keys, sectors, 64);

        // Build JSON array
        char json[1024];
        int pos = snprintf(json, sizeof(json), "{\"count\":%lu,\"keys\":[", (unsigned long)count);
        for (uint32_t i = 0; i < count && pos < (int)sizeof(json) - 20; i++) {
            if (i > 0) json[pos++] = ',';
            pos += snprintf(json + pos, sizeof(json) - pos, "%lu", (unsigned long)keys[i]);
        }
        pos += snprintf(json + pos, sizeof(json) - pos, "]}");
        http_send_str(pcb, "200 OK", "application/json", json);
        return;
    }

    http_send_str(pcb, "404 Not Found", "text/plain", "Not Found");
}

// ---- TCP connection state ----

typedef struct {
    uint8_t buf[HTTP_BUF_SIZE];
    uint16_t len;
} http_conn_t;

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_conn_t *conn = (http_conn_t *)arg;

    if (!p) {
        // Connection closed
        if (conn) free(conn);
        tcp_close(pcb);
        return ERR_OK;
    }

    // Accumulate data
    uint16_t copy = p->tot_len;
    if (conn->len + copy > HTTP_BUF_SIZE)
        copy = HTTP_BUF_SIZE - conn->len;
    pbuf_copy_partial(p, conn->buf + conn->len, copy, 0);
    conn->len += copy;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    // Check for end of HTTP headers
    char *hdr_end = strstr((char *)conn->buf, "\r\n\r\n");
    if (!hdr_end) return ERR_OK;  // keep accumulating

    // Parse method + URL
    char method[8] = {0};
    char url[128] = {0};
    sscanf((char *)conn->buf, "%7s %127s", method, url);

    // Find body
    uint8_t *body = (uint8_t *)(hdr_end + 4);
    uint16_t body_len = conn->len - (uint16_t)(body - conn->buf);

    handle_request(pcb, method, url, body, body_len);

    // Close after response
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

// ============================================================
// Init
// ============================================================

void web_server_init(wal_state_t *wal) {
    g_wal = wal;

    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, HTTP_PORT);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, http_accept);

    printf("[http] Web server listening on port %d\n", HTTP_PORT);
}
