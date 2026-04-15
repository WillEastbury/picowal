#include "web_server.h"
#include "kv_flash.h"
#define KV_STORE_REDIRECT
#include "kv_store.h"
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
"api('POST','/admin/meta/new',JSON.stringify({ordinal:parseInt($('po').value),name:$('pn').value.trim(),module:($('pm')?$('pm').value.trim():'')})).then(function(r){return r.text().then(function(t){"
"if(r.ok)location.reload();else alert('Error: '+t)})});}"
/* Change password */
";var cp=$('passForm');if(cp)cp.onsubmit=function(e){e.preventDefault();"
"var o=$('cpOld').value,n=$('cpNew').value,c=$('cpConfirm').value;"
"if(n!==c){$('cpMsg').textContent='Passwords do not match';return}"
"var b=new Uint8Array(64);b[0]=o.length;for(var i=0;i<o.length&&i<31;i++)b[1+i]=o.charCodeAt(i);"
"b[32]=n.length;for(var i=0;i<n.length&&i<31;i++)b[33+i]=n.charCodeAt(i);"
"api('POST','/0/1/'+document.body.dataset.uc+'/_passwd',b.buffer).then(function(r){"
"$('cpMsg').textContent=r.ok?'Password changed':'Failed ('+r.status+')';$('cpMsg').style.color=r.ok?'#27ae60':'#e94560'});}"
/* Grid save: collect all rows, build batch binary, POST /batch */
";window.saveGrid=function(cp,linkOrd,parentId){"
"var rows=document.querySelectorAll('table[data-cp=\"'+cp+'\"] tbody tr');"
"var cards=[];"
"rows.forEach(function(tr){"
"var cid=parseInt(tr.dataset.cid)||0;"
"var inputs=tr.querySelectorAll('[data-ord]');"
"var hasVal=false;inputs.forEach(function(el){if(el.value)hasVal=true});"
"if(!hasVal)return;"
"var parts=[0x7D,0xCA,1,0];"
/* Write the parent link field (lookup ord) */
"parts.push(linkOrd&0x1F,4,parentId&255,(parentId>>8)&255,(parentId>>16)&255,(parentId>>24)&255);"
"inputs.forEach(function(el){"
"var ord=parseInt(el.dataset.ord),type=el.dataset.ftype,val=el.value;"
"var data=[];"
"switch(type){"
"case 'bool':data=[parseInt(val)?1:0];break;"
"case 'uint8':data=[(parseInt(val)||0)&255];break;"
"case 'uint16':{var v=parseInt(val)||0;data=[v&255,(v>>8)&255];break}"
"case 'uint32':case 'lookup':{var v=parseInt(val)||0;data=[v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255];break}"
"case 'int16':{var ab=new ArrayBuffer(2);new DataView(ab).setInt16(0,parseInt(val)||0,true);data=Array.from(new Uint8Array(ab));break}"
"case 'int32':{var ab=new ArrayBuffer(4);new DataView(ab).setInt32(0,parseInt(val)||0,true);data=Array.from(new Uint8Array(ab));break}"
"default:{var s=String(val||'');data=[s.length];for(var i=0;i<s.length;i++)data.push(s.charCodeAt(i)&255)}}"
"parts.push(ord&0x1F,data.length);data.forEach(function(b){parts.push(b)})});"
"cards.push({pack:cp,cid:cid,data:new Uint8Array(parts)})});"
"if(cards.length==0)return;"
/* Build batch binary: 0xBA7C + count(u16) + [pack(u16) card(u32) len(u16) data]... */
"var total=4;cards.forEach(function(c){total+=8+c.data.length});"
"var buf=new Uint8Array(total);var o=0;"
"buf[o++]=0xBA;buf[o++]=0x7C;buf[o++]=cards.length&255;buf[o++]=(cards.length>>8)&255;"
"cards.forEach(function(c){"
"buf[o++]=c.pack&255;buf[o++]=(c.pack>>8)&255;"
"buf[o++]=c.cid&255;buf[o++]=(c.cid>>8)&255;buf[o++]=(c.cid>>16)&255;buf[o++]=(c.cid>>24)&255;"
"buf[o++]=c.data.length&255;buf[o++]=(c.data.length>>8)&255;"
"buf.set(c.data,o);o+=c.data.length});"
"api('POST','/batch',buf.buffer).then(function(r){"
"var msg=document.getElementById('gridMsg'+cp);"
"if(msg){msg.textContent=r.ok?'Saved!':'Error '+r.status;msg.style.color=r.ok?'#40906a':'#b04050'}"
"if(r.ok)setTimeout(function(){location.reload()},500)});}"
"})();";

// ============================================================
// SSR helpers — render HTML into a buffer
// ============================================================

// Parse a schema card from Pack 0 into field definitions.
// Returns field count. Fills names[], types[], maxlens[], ords[].

// Full schema parser including child packs (ord 6)
static uint8_t parse_schema_full(const uint8_t *card, uint16_t card_len,
                            char names[][32], uint8_t *types, uint8_t *maxlens, uint8_t *ords,
                            char *pack_name, uint8_t max_fields, char *module,
                            uint8_t *children, uint8_t *child_count) {
    if (card_len < 4 || card[0] != 0x7D || card[1] != 0xCA) return 0;
    uint16_t off = 4;
    uint8_t field_count = 0;
    char name_buf[256]; uint16_t name_buf_len = 0;
    if (module) module[0] = '\0';
    if (child_count) *child_count = 0;

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
        if (ord == 4 && flen >= 1 && module) {
            uint8_t n = card[off]; if (n > flen - 1) n = flen - 1;
            if (n > 31) n = 31;
            memcpy(module, card + off + 1, n);
            module[n] = '\0';
        }
        if (ord == 5 && flen > 0) {
            memcpy(name_buf, card + off, flen);
            name_buf_len = flen;
        }
        if (ord == 6 && children && child_count) {
            uint8_t nc = flen > 8 ? 8 : flen;
            for (uint8_t i = 0; i < nc; i++) children[i] = card[off + i];
            *child_count = nc;
        }
        off += flen;
    }

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

static uint8_t parse_schema_ex(const uint8_t *card, uint16_t card_len,
                            char names[][32], uint8_t *types, uint8_t *maxlens, uint8_t *ords,
                            char *pack_name, uint8_t max_fields, char *module) {
    return parse_schema_full(card, card_len, names, types, maxlens, ords,
                             pack_name, max_fields, module, NULL, NULL);
}

static uint8_t parse_schema(const uint8_t *card, uint16_t card_len,
                            char names[][32], uint8_t *types, uint8_t *maxlens, uint8_t *ords,
                            char *pack_name, uint8_t max_fields) {
    return parse_schema_ex(card, card_len, names, types, maxlens, ords, pack_name, max_fields, NULL);
}

static const char *type_name(uint8_t code) {
    // Authoritative PicoWAL type codes
    switch (code) {
        case 0x01: return "uint8";
        case 0x02: return "uint16";
        case 0x03: return "uint32";
        case 0x04: return "int8";
        case 0x05: return "int16";
        case 0x06: return "int32";
        case 0x07: return "bool";
        case 0x08: return "ascii";
        case 0x09: return "utf8";
        case 0x0A: return "date";
        case 0x0B: return "time";
        case 0x0C: return "datetime";
        case 0x10: return "array_u16";
        case 0x11: return "blob";
        case 0x12: return "lookup";
        default:   return "?";
    }
}

static const char *type_friendly(uint8_t code) {
    switch (code) {
        case 0x01: return "Small number (0-255)";
        case 0x02: return "Number (0-65k)";
        case 0x03: return "Large number";
        case 0x04: return "Small number (+/-)";
        case 0x05: return "Number (+/-)";
        case 0x06: return "Large number (+/-)";
        case 0x07: return "Yes / No";
        case 0x08: return "Text";
        case 0x09: return "Text";
        case 0x0A: return "Date";
        case 0x0B: return "Time";
        case 0x0C: return "Date & Time";
        case 0x10: return "Number list";
        case 0x11: return "Binary data";
        case 0x12: return "Link";
        default:   return "";
    }
}

// Prettify a field name: "in_stock" -> "In Stock"
static void pretty_name(char *out, int max, const char *raw) {
    int o = 0;
    bool cap = true;
    for (int i = 0; raw[i] && o < max - 1; i++) {
        if (raw[i] == '_') {
            out[o++] = ' ';
            cap = true;
        } else if (cap && raw[i] >= 'a' && raw[i] <= 'z') {
            out[o++] = raw[i] - 32;
            cap = false;
        } else {
            out[o++] = raw[i];
            cap = false;
        }
    }
    out[o] = '\0';
}

// Cardinality bucket: 3-bit log10 estimate stored in schema flags bits 1-3
// 0=<10, 1=10-99, 2=100-999, 3=1K-9K, 4=10K-99K, 5=100K-999K, 6=1M-9M, 7=10M+
#define SCHEMA_FLAG_PUBLIC_READ  0x01
#define SCHEMA_CARD_BUCKET(flags) (((flags) >> 1) & 0x07)
#define SCHEMA_CARD_BUCKET_SET(flags, b) (((flags) & 0xF1) | (((b) & 0x07) << 1))

static uint8_t card_count_bucket(uint32_t count) {
    if (count < 10) return 0;
    if (count < 100) return 1;
    if (count < 1000) return 2;
    if (count < 10000) return 3;
    if (count < 100000) return 4;
    if (count < 1000000) return 5;
    if (count < 10000000) return 6;
    return 7;
}

// SRAM cardinality cache — avoids kv_range just to check pack size
// Lazy: populated on first access, updated when card list is viewed
// Flushed to schema flags on idle (poll loop)
#define CARD_CACHE_MAX 32
static struct {
    uint16_t pack;
    uint8_t  bucket;   // 0-7
    bool     dirty;    // needs flush to schema flags
    bool     valid;
} s_card_cache[CARD_CACHE_MAX];
static uint8_t s_card_cache_count = 0;

static int card_cache_find(uint16_t pack) {
    for (uint8_t i = 0; i < s_card_cache_count; i++)
        if (s_card_cache[i].valid && s_card_cache[i].pack == pack) return i;
    return -1;
}

// Get cardinality bucket for a pack (from SRAM cache, or scan + cache)
uint8_t get_cardinality(uint16_t pack) {
    int idx = card_cache_find(pack);
    if (idx >= 0) return s_card_cache[idx].bucket;

    // Cache miss — count cards and cache
    uint32_t keys[1];
    uint32_t count = kv_range(((uint32_t)(pack & 0x3FFu) << 22), 0xFFC00000u, keys, NULL, 0);
    // kv_range with limit 0 may not work — use a reasonable scan
    // Actually kv_range returns up to limit, so use a large limit for count
    uint32_t keys2[128];
    count = kv_range(((uint32_t)(pack & 0x3FFu) << 22), 0xFFC00000u, keys2, NULL, 128);
    uint8_t bucket = card_count_bucket(count);

    if (s_card_cache_count < CARD_CACHE_MAX) {
        idx = s_card_cache_count++;
    } else {
        idx = 0; // evict oldest
    }
    s_card_cache[idx].pack = pack;
    s_card_cache[idx].bucket = bucket;
    s_card_cache[idx].dirty = true;
    s_card_cache[idx].valid = true;
    return bucket;
}

// Update cardinality from a known count (called when card list already has the count)
void set_cardinality(uint16_t pack, uint32_t count) {
    uint8_t bucket = card_count_bucket(count);
    int idx = card_cache_find(pack);
    if (idx >= 0) {
        if (s_card_cache[idx].bucket != bucket) {
            s_card_cache[idx].bucket = bucket;
            s_card_cache[idx].dirty = true;
        }
        return;
    }
    if (s_card_cache_count < CARD_CACHE_MAX) {
        idx = s_card_cache_count++;
    } else {
        idx = 0;
    }
    s_card_cache[idx].pack = pack;
    s_card_cache[idx].bucket = bucket;
    s_card_cache[idx].dirty = true;
    s_card_cache[idx].valid = true;
}

// Flush one dirty entry to schema flags. Call from idle/poll loop.
// Returns true if work was done.
bool flush_cardinality_one(void) {
    for (uint8_t i = 0; i < s_card_cache_count; i++) {
        if (!s_card_cache[i].valid || !s_card_cache[i].dirty) continue;
        uint32_t skey = ((uint32_t)0 << 22) | s_card_cache[i].pack;
        uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
        if (!kv_get_copy(skey, sbuf, &slen, NULL) || slen < 4) {
            s_card_cache[i].dirty = false;
            continue;
        }
        if (sbuf[0] != 0x7D || sbuf[1] != 0xCA) { s_card_cache[i].dirty = false; continue; }
        uint16_t off = 4;
        while (off + 1 < slen) {
            uint8_t ord = sbuf[off] & 0x1F, flen = sbuf[off + 1];
            if (off + 2 + flen > slen) break;
            if (ord == 3 && flen >= 1) {
                uint8_t old = SCHEMA_CARD_BUCKET(sbuf[off + 2]);
                if (old != s_card_cache[i].bucket) {
                    sbuf[off + 2] = SCHEMA_CARD_BUCKET_SET(sbuf[off + 2], s_card_cache[i].bucket);
                    kv_put(skey, sbuf, slen);
                }
                break;
            }
            off += 2 + flen;
        }
        s_card_cache[i].dirty = false;
        return true;
    }
    return false;
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

// Check auth: cookie session only.
// If session found, fills *session and returns true, otherwise false.
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
    "*{box-sizing:border-box}"
    "body{font:15px/1.6 -apple-system,system-ui,sans-serif;background:#1c1e26;color:#d5d8e0;padding:0;margin:0}"
    ".page{max-width:900px;margin:0 auto;padding:20px 24px}"
    "h1{color:#e06070;margin:0}h2{color:#a8c8ff;border-bottom:1px solid #2a3040;padding-bottom:8px;margin-bottom:16px;font-weight:600}"
    "a{color:#7eb8f0;text-decoration:none}a:hover{color:#c0d8ff}"
    "input,select,button,textarea{font:inherit;padding:10px 14px;margin:4px 0;background:#252830;color:#d5d8e0;border:1px solid #3a3f50;border-radius:8px;box-sizing:border-box}"
    "input:focus,select:focus,textarea:focus{border-color:#7eb8f0;outline:none;box-shadow:0 0 0 3px rgba(126,184,240,.12)}"
    "button{background:#5080c0;color:#fff;border:none;cursor:pointer;font-weight:600;padding:10px 22px;border-radius:8px;transition:background .15s}button:hover{background:#4070b0}"
    ".btn-sm{padding:6px 14px;font-size:13px}.btn-del{background:#b04050}.btn-del:hover{background:#903040}"
    ".btn-ok{background:#40906a}.btn-ok:hover{background:#308058}"
    ".btn-ghost{background:transparent;border:1px solid #3a3f50;color:#7eb8f0;padding:8px 16px}.btn-ghost:hover{background:#2a3040}"
    "table{width:100%;border-collapse:collapse;margin:10px 0}th,td{padding:10px 12px;border-bottom:1px solid #2a3040;text-align:left}"
    "th{font-size:11px;text-transform:uppercase;letter-spacing:.8px;color:#6a7080;font-weight:600;border-bottom:2px solid #3a3f50}"
    "td{font:14px/1.4 monospace}tr:hover td{background:#22252e}"
    ".row{display:flex;gap:10px;flex-wrap:wrap}.row>*{flex:1;min-width:120px}"
    ".card{background:#22252e;border:1px solid #2a3040;border-radius:12px;padding:24px;margin:14px 0}"
    ".badge{background:#40906a;color:#fff;padding:2px 10px;border-radius:12px;font-size:10px;font-weight:600}"
    ".badge-admin{background:#b04050}"
    "label{display:block;font-size:13px;color:#8090a0;margin-bottom:4px;font-weight:500}"
    ".fg{margin-bottom:20px}.fg label{margin-bottom:6px}"
    ".fg input,.fg select,.fg textarea{width:100%}"
    "pre{font:13px/1.5 monospace;background:#1a1c22;padding:14px;border-radius:8px;white-space:pre-wrap;max-height:400px;overflow-y:auto;border:1px solid #2a3040}"
    ".tabs{display:flex;gap:0;margin-bottom:0}.tab{padding:10px 20px;cursor:pointer;background:#252830;border:1px solid #2a3040;border-bottom:none;border-radius:8px 8px 0 0;color:#6a7080}"
    ".tab.active{background:#22252e;color:#a8c8ff;border-color:#3a3f50}.tab-body{display:none}.tab-body.active{display:block}"
    "#loginBox{max-width:320px;margin:80px auto}"
    "#status{padding:10px;background:#1a1c22;border-radius:8px;white-space:pre-wrap;margin-top:8px;border:1px solid #2a3040}"
    "nav{background:#22252e;padding:12px 24px;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #2a3040}"
    "nav a{color:#b0b8c8;text-decoration:none;margin:0 12px;font-size:14px}nav a:hover{color:#a8c8ff}"
    ".nav-brand{color:#e06070!important;font-weight:700;letter-spacing:1px;font-size:16px}"
    ".dropdown{position:relative;display:inline-block}"
    ".dropdown>span{color:#b0b8c8;cursor:pointer;margin:0 12px;font-size:14px}.dropdown>span:hover{color:#a8c8ff}"
    ".dropdown-menu{display:none;position:absolute;top:100%;left:0;background:#22252e;border:1px solid #3a3f50;border-radius:8px;min-width:150px;z-index:10;padding:6px 0;box-shadow:0 4px 12px rgba(0,0,0,.3)}"
    ".dropdown:hover .dropdown-menu{display:block}"
    ".dropdown-menu a{display:block;padding:8px 18px;margin:0;white-space:nowrap;font-size:14px}.dropdown-menu a:hover{background:#2a3040}"
    ".pager{display:flex;justify-content:space-between;align-items:center;margin:16px 0;font-size:13px;color:#6a7080}"
    ".pager a{padding:8px 16px;background:#252830;border:1px solid #3a3f50;border-radius:8px;text-decoration:none;font-size:13px}"
    ".search-box{display:flex;gap:8px;margin-bottom:14px}"
    ".search-box input{flex:1;font:13px monospace}.search-box button{white-space:nowrap}"
    ".card-nav{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}"
    ".card-nav a{font-size:13px;padding:6px 14px;background:#252830;border:1px solid #3a3f50;border-radius:8px}"
    ".crumb{font-size:13px;color:#6a7080;margin-bottom:8px}.crumb a{font-size:13px}"
    ".actions{display:flex;gap:10px;margin-top:20px;padding-top:20px;border-top:1px solid #2a3040}"
    ".grid td{padding:2px 4px;border-bottom:1px solid #22252e}.grid input,.grid select{font:13px/1.3 monospace;color:#d5d8e0;padding:4px 6px;margin:0;border:1px solid transparent;background:transparent;width:100%}"
    ".grid input:focus,.grid select:focus{border-color:#7eb8f0;background:#252830}"
    ".grid tr:hover td{background:#1c1e26}.grid .new-row input{border:1px solid #3a3f50}"
    "</style></head><body>"
    "<nav><span class=nav-brand>&#x1F5C3; PicoWAL</span>"
    "<div><a href=/>Home</a><a href=/status>Status</a><a href=/query>Query</a>";

static const char PAGE_NAV_TAIL[] =
    "</div></nav><div class=page>";

static const char PAGE_TAIL[] = "</div></body></html>";

// Build dynamic nav links for the user's accessible packs
// Packs with a module (ord 4) are grouped into dropdown menus.
// Packs without a module appear as flat links.
static uint16_t build_nav(char *buf, uint16_t bufsize, const char *req) {
    int n = 0;
    uint8_t token[SESSION_TOKEN_LEN];
    user_session_t session;
    if (user_auth_parse_cookie(req, token) && user_auth_check(token, &session)) {
        uint32_t keys[32];
        uint32_t count = kv_range(0, 0xFFC00000u, keys, NULL, 32);

        // Collect pack info in one pass
        typedef struct { uint32_t ord; char name[16]; char module[16]; } nav_pack_t;
        nav_pack_t packs[24];
        uint8_t np = 0;

        for (uint32_t i = 0; i < count && np < 24; i++) {
            uint32_t pack_ord = keys[i] & 0x3FFFFF;
            if (pack_ord == 0 || pack_ord == 1) continue;
            if (!user_auth_can_read(&session, (uint16_t)pack_ord)) continue;

            uint8_t sbuf[128]; uint16_t slen = sizeof(sbuf);
            if (!kv_get_copy(keys[i], sbuf, &slen, NULL)) continue;
            if (slen < 6 || sbuf[0] != 0x7D || sbuf[1] != 0xCA) continue;

            nav_pack_t *p = &packs[np];
            p->ord = pack_ord;
            p->name[0] = '?'; p->name[1] = '\0';
            p->module[0] = '\0';

            uint16_t off = 4;
            while (off + 1 < slen) {
                uint8_t ord = sbuf[off] & 0x1F, flen = sbuf[off + 1]; off += 2;
                if (off + flen > slen) break;
                if (ord == 0 && flen >= 1) {
                    uint8_t nl = sbuf[off]; if (nl > flen - 1) nl = flen - 1; if (nl > 15) nl = 15;
                    memcpy(p->name, sbuf + off + 1, nl); p->name[nl] = '\0';
                    if (p->name[0] >= 'a' && p->name[0] <= 'z') p->name[0] -= 32;
                }
                if (ord == 4 && flen >= 1) {
                    uint8_t ml = sbuf[off]; if (ml > flen - 1) ml = flen - 1; if (ml > 15) ml = 15;
                    memcpy(p->module, sbuf + off + 1, ml); p->module[ml] = '\0';
                    if (p->module[0] >= 'a' && p->module[0] <= 'z') p->module[0] -= 32;
                }
                off += flen;
            }
            np++;
        }

        // Flat links: packs with no module
        for (uint8_t i = 0; i < np && n < (int)bufsize - 100; i++) {
            if (packs[i].module[0]) continue;
            n += snprintf(buf + n, bufsize - n,
                "<a href='/pack/%lu'>%s</a>", (unsigned long)packs[i].ord, packs[i].name);
        }

        // Grouped dropdowns: collect unique modules, render a dropdown each
        char seen[8][16]; uint8_t nseen = 0;
        for (uint8_t i = 0; i < np && nseen < 8; i++) {
            if (!packs[i].module[0]) continue;
            // Check if already seen
            bool dup = false;
            for (uint8_t s = 0; s < nseen; s++) {
                if (strcmp(seen[s], packs[i].module) == 0) { dup = true; break; }
            }
            if (dup) continue;
            strncpy(seen[nseen], packs[i].module, 15); seen[nseen][15] = '\0';
            nseen++;

            // Build dropdown items for this module
            char items[384]; int sn = 0;
            for (uint8_t j = 0; j < np && sn < (int)sizeof(items) - 80; j++) {
                if (strcmp(packs[j].module, packs[i].module) != 0) continue;
                sn += snprintf(items + sn, sizeof(items) - sn,
                    "<a href='/pack/%lu'>%s</a>", (unsigned long)packs[j].ord, packs[j].name);
            }
            if (sn > 0 && n < (int)bufsize - 200) {
                n += snprintf(buf + n, bufsize - n,
                    "<div class=dropdown><span>%s &#x25BE;</span>"
                    "<div class=dropdown-menu>%s</div></div>",
                    packs[i].module, items);
            }
        }

        if (user_auth_is_admin(&session) && n < (int)bufsize - 300)
            n += snprintf(buf + n, bufsize - n,
                "<div class=dropdown><span>Admin &#x25BE;</span><div class=dropdown-menu>"
                "<a href=/admin>Users</a>"
                "<a href='/admin/meta'>Schema</a>"
                "<a href='/admin/log'>Log</a>"
                "<a href='/admin/flash'>Flash</a>"
                "<a href='/admin/ram'>RAM</a>"
                "<a href=/update>OTA Update</a>"
                "</div></div>");
        if (n < (int)bufsize - 40)
            n += snprintf(buf + n, bufsize - n, "<a href=/logout>Logout</a>");
    }
    return (uint16_t)n;
}

static void http_page_req(struct tcp_pcb *pcb, const char *req,
                          const char *content, uint16_t content_len) {
    uint16_t head_len = sizeof(PAGE_HEAD) - 1;
    uint16_t nav_tail_len = sizeof(PAGE_NAV_TAIL) - 1;
    uint16_t tail_len = sizeof(PAGE_TAIL) - 1;
    char nav[768];
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
    uint32_t sd_records = kvsd_ready() ? kvsd_record_count() : 0;
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
        "RECORDS:      %lu (flash) + %lu (SD) = %lu\n"
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
        (unsigned long)records, (unsigned long)sd_records,
        (unsigned long)(records + sd_records),
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
    fifo_push_timeout(fifo_signal((uint8_t)rid));

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
    fifo_push_timeout(fifo_signal((uint8_t)rid));

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
// OTA firmware update — SD-staged
//
// Upload chunks → SD staging area (600KB reserved blocks 1-1200)
// Commit → read from SD → SRAM → erase+write flash slot A
// No XIP contention: SD reads via SPI, independent of flash
// ============================================================

#define OTA_SLOT_A      0x000000
#define OTA_SLOT_SIZE   (600 * 1024)
#define OTA_XIP_BASE    0x10000000

static struct {
    bool     active;
    uint32_t offset;       // bytes written to SD so far
    uint32_t total_written;
    uint32_t sd_base;      // SD block where staging starts
} g_ota;

static void __no_inline_not_in_flash_func(ota_flash_erase)(uint32_t offset) {
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
}

static void __no_inline_not_in_flash_func(ota_flash_program)(uint32_t offset, const uint8_t *data) {
    flash_range_program(offset, data, 256);
}

#define OTA_CHUNK_SIZE FLASH_SECTOR_SIZE
static uint8_t g_ota_chunk_buf[OTA_CHUNK_SIZE];

static void __no_inline_not_in_flash_func(ota_erase_write_chunk)(
    uint32_t dest_off, const uint8_t *ram_data, uint32_t len) {
    for (uint32_t off = 0; off < len; off += FLASH_SECTOR_SIZE) {
        uint32_t slen = len - off;
        if (slen > FLASH_SECTOR_SIZE) slen = FLASH_SECTOR_SIZE;
        flash_range_erase(dest_off + off, FLASH_SECTOR_SIZE);
        for (uint32_t p = 0; p < slen; p += 256)
            flash_range_program(dest_off + off + p, ram_data + off + p, 256);
        if (slen < FLASH_SECTOR_SIZE) {
            uint8_t pad[256];
            for (int i = 0; i < 256; i++) pad[i] = 0xFF;
            for (uint32_t p = slen; p < FLASH_SECTOR_SIZE; p += 256)
                flash_range_program(dest_off + off + p, pad, 256);
        }
    }
}

// OTA commit: copy from SD to flash entirely from SRAM.
// Writes sectors 1..N first, then sector 0 last — so XIP remains
// valid until the final moment before reboot.
static void __no_inline_not_in_flash_func(ota_commit_from_sd)(
    uint32_t sd_base, uint32_t size) {

    // Pass 1: write all chunks EXCEPT the first sector (keeps boot2/vectors valid)
    uint32_t first_chunk = FLASH_SECTOR_SIZE;  // skip first 4KB
    for (uint32_t coff = first_chunk; coff < size; coff += OTA_CHUNK_SIZE) {
        uint32_t clen = size - coff;
        if (clen > OTA_CHUNK_SIZE) clen = OTA_CHUNK_SIZE;

        uint32_t blks = (clen + 511) / 512;
        sd_read_blocks(sd_base + (coff / 512), g_ota_chunk_buf, blks);

        uint32_t irq = save_and_disable_interrupts();
        ota_erase_write_chunk(OTA_SLOT_A + coff, g_ota_chunk_buf, clen);
        restore_interrupts(irq);
    }

    // Pass 2: write the first sector last (overwrites boot2 + vector table)
    {
        uint32_t clen = first_chunk < size ? first_chunk : size;
        uint32_t blks = (clen + 511) / 512;
        sd_read_blocks(sd_base, g_ota_chunk_buf, blks);

        // Point of no return — after this, XIP is invalid
        uint32_t irq = save_and_disable_interrupts();
        ota_erase_write_chunk(OTA_SLOT_A, g_ota_chunk_buf, clen);
        // Don't restore interrupts — go straight to reboot
        watchdog_reboot(0, 0, 0);
        while (1) tight_loop_contents();
    }
}
static void ota_write_chunk_sd(const uint8_t *data, uint16_t len) {
    uint8_t blk_buf[512];
    while (len > 0 && g_ota.offset < OTA_SLOT_SIZE) {
        uint32_t blk_off = g_ota.offset / 512;
        uint32_t byte_off = g_ota.offset % 512;
        uint16_t space = (uint16_t)(512 - byte_off);
        uint16_t chunk = len < space ? len : space;

        if (byte_off == 0 && chunk == 512) {
            sd_write_block(g_ota.sd_base + blk_off, data);
        } else {
            if (byte_off > 0) sd_read_block(g_ota.sd_base + blk_off, blk_buf);
            else memset(blk_buf, 0xFF, 512);
            memcpy(blk_buf + byte_off, data, chunk);
            sd_write_block(g_ota.sd_base + blk_off, blk_buf);
        }

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
        (void)http_respond_with_headers(pcb, "200 OK", "application/javascript",
                          "Cache-Control: public, max-age=86400\r\n",
                          (const uint8_t *)APP_JS, sizeof(APP_JS) - 1);
        return;
    }

    // Favicon — return 204 with long cache to stop browser re-requesting
    if (verb == VERB_GET && (strcmp(path, "/favicon.ico") == 0 || strcmp(path, "/favicon.png") == 0)) {
        (void)http_respond_with_headers(pcb, "204 No Content", "image/x-icon",
                          "Cache-Control: public, max-age=604800\r\n",
                          NULL, 0);
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
            // SSR card form — redesigned
            if (!user_auth_can_read(&session, (uint16_t)pack_ord)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no access\"}");
                return;
            }

            // Load schema including children list
            uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
            char pname[32] = "?";
            uint8_t ords[32], ftypes[32], maxlens[32]; char names[32][32];
            memset(names, 0, sizeof(names));
            uint8_t fc = 0;
            uint8_t child_packs[8]; uint8_t child_count = 0;
            if (kv_get_copy(((uint32_t)0 << 22) | pack_ord, sbuf, &slen, NULL))
                fc = parse_schema_full(sbuf, slen, names, ftypes, maxlens, ords,
                                       pname, 32, NULL, child_packs, &child_count);

            // Load data card
            uint32_t key = ((uint32_t)(pack_ord & 0x3FF) << 22) | (card_ord & 0x3FFFFF);
            uint8_t dbuf[KV_MAX_VALUE]; uint16_t dlen = KV_MAX_VALUE;
            bool exists = kv_get_copy(key, dbuf, &dlen, NULL);

            // Find prev/next card IDs for navigation
            uint32_t nav_keys[64];
            uint32_t nav_count = kv_range(((uint32_t)(pack_ord & 0x3FFu) << 22),
                                          0xFFC00000u, nav_keys, NULL, 64);
            int32_t prev_card = -1, next_card = -1;
            for (uint32_t ni = 0; ni < nav_count; ni++) {
                uint32_t nid = nav_keys[ni] & 0x3FFFFF;
                if (nid == card_ord) {
                    if (ni > 0) prev_card = (int32_t)(nav_keys[ni-1] & 0x3FFFFF);
                    if (ni + 1 < nav_count) next_card = (int32_t)(nav_keys[ni+1] & 0x3FFFFF);
                    break;
                }
            }

            // Parse data values by ordinal
            char vals[32][64]; memset(vals, 0, sizeof(vals));
            uint8_t ftypes_by_ord[32]; memset(ftypes_by_ord, 0, sizeof(ftypes_by_ord));
            for (uint8_t i = 0; i < fc; i++) ftypes_by_ord[ords[i]] = ftypes[i];
            if (exists) parse_card_values(dbuf, dlen, vals, ftypes_by_ord, 32);

            char pg[8192]; int n = 0;

            // Breadcrumb
            n += snprintf(pg + n, sizeof(pg) - n,
                "<div class=crumb><a href='/'>Home</a> &rsaquo; "
                "<a href='/pack/%u'>%s</a> &rsaquo; Card %u</div>",
                pack_ord, pname, card_ord);

            // Card navigation
            n += snprintf(pg + n, sizeof(pg) - n, "<div class=card-nav>");
            if (prev_card >= 0)
                n += snprintf(pg + n, sizeof(pg) - n,
                    "<a href='/pack/%u/%ld'>&larr; Card %ld</a>",
                    pack_ord, (long)prev_card, (long)prev_card);
            else
                n += snprintf(pg + n, sizeof(pg) - n, "<span></span>");
            n += snprintf(pg + n, sizeof(pg) - n,
                "<span style='color:#0ff;font-weight:bold'>Card %u%s</span>",
                card_ord, exists ? "" : " <span class=badge>NEW</span>");
            if (next_card >= 0)
                n += snprintf(pg + n, sizeof(pg) - n,
                    "<a href='/pack/%u/%ld'>Card %ld &rarr;</a>",
                    pack_ord, (long)next_card, (long)next_card);
            else
                n += snprintf(pg + n, sizeof(pg) - n, "<span></span>");
            n += snprintf(pg + n, sizeof(pg) - n, "</div>");

            // Card form inside styled container
            n += snprintf(pg + n, sizeof(pg) - n,
                "<div class=card><form id=cardForm data-pack='%u' data-card='%u'>",
                pack_ord, card_ord);

            for (uint8_t i = 0; i < fc && n < (int)sizeof(pg) - 500; i++) {
                const char *tn = type_name(ftypes[i]);
                // Skip password_hash and salt for display (ords 1,2 in pack 1)
                if (pack_ord == 1 && (ords[i] == 1 || ords[i] == 2)) continue;

                char plabel[32];
                pretty_name(plabel, sizeof(plabel), names[i]);
                n += snprintf(pg + n, sizeof(pg) - n,
                    "<div class=fg><label>%s <span style='font-weight:400;color:#505868;font-size:11px'>%s</span></label>",
                    plabel, type_friendly(ftypes[i]));

                uint8_t tc = ftypes[i];
                if (tc == 0x07) {
                    // bool — dropdown
                    bool checked = vals[ords[i]][0] == 't' || vals[ords[i]][0] == '1';
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<select data-ord='%u' data-ftype='bool'>"
                        "<option value='false'%s>No</option>"
                        "<option value='true'%s>Yes</option></select>",
                        (unsigned)ords[i], checked ? "" : " selected", checked ? " selected" : "");
                } else if (tc == 18) {
                    // lookup — dropdown showing resolved names
                    uint8_t target_pack = maxlens[i];
                    uint32_t cur_val = 0;
                    if (vals[ords[i]][0]) cur_val = (uint32_t)strtoul(vals[ords[i]], NULL, 10);

                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<select data-ord='%u' data-ftype='lookup'>"
                        "<option value='0'>— select —</option>",
                        (unsigned)ords[i]);

                    uint32_t lkeys[16];
                    uint32_t lcount = kv_range(((uint32_t)(target_pack & 0x3FFu) << 22),
                                               0xFFC00000u, lkeys, NULL, 16);
                    for (uint32_t li = 0; li < lcount && n < (int)sizeof(pg) - 300; li++) {
                        uint32_t lid = lkeys[li] & 0x3FFFFF;
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
                            "<option value='%lu'%s>%s</option>",
                            (unsigned long)lid, lid == cur_val ? " selected" : "",
                            lname[0] ? lname : "?");
                    }
                    n += snprintf(pg + n, sizeof(pg) - n, "</select>");
                } else if (tc == 0x11) {
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input data-ord='%u' data-ftype='blob' value='%s' readonly "
                        "title='Blob fields are read-only' style='opacity:.6'>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else if (tc == 0x0A) {
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input type=date data-ord='%u' data-ftype='isodate' value='%s'>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else if (tc == 0x0B) {
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input type=time data-ord='%u' data-ftype='isotime' value='%s' step=1>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else if (tc == 0x0C) {
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input type=datetime-local data-ord='%u' data-ftype='isodatetime' value='%s' step=1>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else if (tc == 0x01 || tc == 0x02 || tc == 0x03 || tc == 0x04 || tc == 0x05 || tc == 0x06) {
                    const char *minmax = "";
                    if (tc == 0x01) minmax = " min=0 max=255";
                    else if (tc == 0x04) minmax = " min=-128 max=127";
                    else if (tc == 0x02) minmax = " min=0 max=65535";
                    else if (tc == 0x05) minmax = " min=-32768 max=32767";
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input type=number data-ord='%u' data-ftype='%s' value='%s'%s>",
                        (unsigned)ords[i], tn, vals[ords[i]], minmax);
                } else if (tc == 0x08 || tc == 0x09) {
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input data-ord='%u' data-ftype='%s' value='%s' maxlength='%u'"
                        " placeholder='Up to %u characters'>",
                        (unsigned)ords[i], tn, vals[ords[i]], (unsigned)maxlens[i],
                        (unsigned)maxlens[i]);
                } else if (tc == 0x10) {
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input data-ord='%u' data-ftype='array_u16' value='%s' "
                        "placeholder='comma-separated numbers'>",
                        (unsigned)ords[i], vals[ords[i]]);
                } else {
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<input data-ord='%u' data-ftype='%s' value='%s'>",
                        (unsigned)ords[i], tn, vals[ords[i]]);
                }

                n += snprintf(pg + n, sizeof(pg) - n, "</div>");
            }

            n += snprintf(pg + n, sizeof(pg) - n,
                "<div class=actions>"
                "<button type=submit class=btn-ok>&#x1F4BE; Save</button>"
                "%s"
                "<a href='/pack/%u' class=btn-ghost style='text-decoration:none'>Cancel</a>"
                "</div>"
                "<div id=saveMsg style='margin-top:8px'></div>"
                "</form></div>",
                user_auth_can_delete(&session, (uint16_t)pack_ord)
                    ? "<button type=button id=delBtn class=btn-del>&#x1F5D1; Delete</button>" : "",
                pack_ord);

            // ---- Master-Child: inline grid editor for child packs ----
            for (uint8_t ci = 0; ci < child_count && exists && n < (int)sizeof(pg) - 400; ci++) {
                uint8_t cp = child_packs[ci];
                uint8_t csb[256]; uint16_t csl = sizeof(csb);
                char cpname[32] = "?";
                uint8_t cords[32], ctypes[32], cmaxl[32]; char cnames[32][32];
                memset(cnames, 0, sizeof(cnames));
                uint8_t cfc = 0;
                if (kv_get_copy(((uint32_t)0 << 22) | cp, csb, &csl, NULL))
                    cfc = parse_schema(csb, csl, cnames, ctypes, cmaxl, cords, cpname, 32);
                if (cfc == 0) continue;

                // Find lookup field pointing to master pack
                int8_t link_fi = -1;
                for (uint8_t fi = 0; fi < cfc; fi++) {
                    if (ctypes[fi] == 0x12 && cmaxl[fi] == pack_ord) {
                        link_fi = (int8_t)fi; break;
                    }
                }
                if (link_fi < 0) continue;
                uint8_t link_ord = cords[link_fi];

                // Editable columns (skip lookup-to-parent)
                uint8_t dcols[6]; uint8_t ndc = 0;
                for (uint8_t fi = 0; fi < cfc && ndc < 6; fi++) {
                    if (fi == (uint8_t)link_fi) continue;
                    dcols[ndc++] = fi;
                }

                // Pre-load lookup options for each column that's a lookup
                // lk_keys[col][0..lk_count-1] = card keys, lk_names[col][i] = field 0 name
                typedef struct { uint32_t id; char name[24]; } lk_opt_t;
                lk_opt_t lk_opts[6][16];
                uint8_t lk_counts[6]; bool lk_is_search[6];
                memset(lk_counts, 0, sizeof(lk_counts));
                memset(lk_is_search, 0, sizeof(lk_is_search));
                for (uint8_t d = 0; d < ndc; d++) {
                    if (ctypes[dcols[d]] != 0x12) continue;
                    uint8_t tp = cmaxl[dcols[d]];
                    uint8_t bucket = get_cardinality(tp);
                    if (bucket >= 2) { lk_is_search[d] = true; continue; } // >=100 cards
                    uint32_t tkeys[17];
                    uint32_t tcount = kv_range(((uint32_t)(tp & 0x3FFu) << 22),
                                               0xFFC00000u, tkeys, NULL, 17);
                    if (tcount > 16) { lk_is_search[d] = true; set_cardinality(tp, tcount); continue; }
                    for (uint32_t ti = 0; ti < tcount && ti < 16; ti++) {
                        lk_opts[d][ti].id = tkeys[ti] & 0x3FFFFF;
                        lk_opts[d][ti].name[0] = '?'; lk_opts[d][ti].name[1] = '\0';
                        uint8_t tb[128]; uint16_t tl = sizeof(tb);
                        if (kv_get_copy(tkeys[ti], tb, &tl, NULL) && tl >= 6 &&
                            tb[0]==0x7D && tb[1]==0xCA) {
                            uint8_t to = tb[4] & 0x1F, tfl = tb[5];
                            if (to == 0 && 6 + tfl <= tl && tfl >= 1) {
                                uint8_t sl = tb[6]; if (sl>tfl-1) sl=tfl-1; if (sl>23) sl=23;
                                memcpy(lk_opts[d][ti].name, tb+7, sl);
                                lk_opts[d][ti].name[sl] = '\0';
                            }
                        }
                        lk_counts[d] = (uint8_t)(ti + 1);
                    }
                }

                char plabel[32]; pretty_name(plabel, sizeof(plabel), cpname);
                n += snprintf(pg + n, sizeof(pg) - n,
                    "<div class=card style='margin-top:8px'>"
                    "<div style='display:flex;justify-content:space-between;align-items:center'>"
                    "<h2 style='margin:0;font-size:16px'>%s</h2>"
                    "<button class=btn-sm onclick='saveGrid(%u,%u,%u)' "
                    "style='background:#40906a'>Save All</button></div>"
                    "<table class=grid data-cp='%u' data-link='%u' data-parent='%u'>"
                    "<thead><tr>",
                    plabel, (unsigned)cp, (unsigned)link_ord, card_ord,
                    (unsigned)cp, (unsigned)link_ord, card_ord);

                for (uint8_t d = 0; d < ndc && n < (int)sizeof(pg) - 80; d++) {
                    char ph[32]; pretty_name(ph, sizeof(ph), cnames[dcols[d]]);
                    n += snprintf(pg + n, sizeof(pg) - n, "<th>%s</th>", ph);
                }
                n += snprintf(pg + n, sizeof(pg) - n, "<th style='width:30px'></th></tr></thead><tbody>");

                // Scan matching child cards
                uint32_t ckeys[64];
                uint32_t ccount = kv_range(((uint32_t)(cp & 0x3FFu) << 22), 0xFFC00000u, ckeys, NULL, 64);
                uint16_t shown = 0;
                uint32_t max_cid = 0;
                for (uint32_t ri = 0; ri < ccount && shown < 15 && n < (int)sizeof(pg) - 400; ri++) {
                    uint32_t child_cid = ckeys[ri] & 0x3FFFFF;
                    if (child_cid > max_cid) max_cid = child_cid;
                    uint8_t rb[256]; uint16_t rl = sizeof(rb);
                    if (!kv_get_copy(ckeys[ri], rb, &rl, NULL) || rl < 4) continue;
                    if (rb[0] != 0x7D || rb[1] != 0xCA) continue;

                    // Check link field matches
                    uint16_t ro = 4; bool match = false;
                    while (ro + 1 < rl) {
                        uint8_t ford = rb[ro] & 0x1F, flen = rb[ro+1]; ro += 2;
                        if (ro + flen > rl) break;
                        if (ford == link_ord && flen >= 4) {
                            uint32_t lv = rb[ro] | ((uint32_t)rb[ro+1]<<8) |
                                          ((uint32_t)rb[ro+2]<<16) | ((uint32_t)rb[ro+3]<<24);
                            if (lv == card_ord) match = true;
                        }
                        ro += flen;
                    }
                    if (!match) continue;

                    // Render editable row
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<tr data-cid='%lu'>", (unsigned long)child_cid);
                    for (uint8_t d = 0; d < ndc && n < (int)sizeof(pg) - 200; d++) {
                        char fv[48] = "";
                        uint16_t fo = 4;
                        while (fo + 1 < rl) {
                            uint8_t ford = rb[fo] & 0x1F, flen = rb[fo+1]; fo += 2;
                            if (fo + flen > rl) break;
                            if (ford == cords[dcols[d]])
                                decode_field_str(fv, sizeof(fv), ctypes[dcols[d]], rb + fo, flen);
                            fo += flen;
                        }
                        uint8_t dtc = ctypes[dcols[d]];
                        const char *dtn = type_name(dtc);
                        if (dtc == 0x12 && !lk_is_search[d]) {
                            // Lookup with <=16 options: select dropdown
                            uint32_t cur = fv[0] ? (uint32_t)strtoul(fv, NULL, 10) : 0;
                            n += snprintf(pg + n, sizeof(pg) - n,
                                "<td><select data-ord='%u' data-ftype='lookup'>"
                                "<option value='0'>—</option>",
                                (unsigned)cords[dcols[d]]);
                            for (uint8_t li = 0; li < lk_counts[d] && n < (int)sizeof(pg) - 80; li++) {
                                n += snprintf(pg + n, sizeof(pg) - n,
                                    "<option value='%lu'%s>%s</option>",
                                    (unsigned long)lk_opts[d][li].id,
                                    lk_opts[d][li].id == cur ? " selected" : "",
                                    lk_opts[d][li].name);
                            }
                            n += snprintf(pg + n, sizeof(pg) - n, "</select></td>");
                        } else if (dtc == 0x12 && lk_is_search[d]) {
                            // Lookup with >16 options: search input
                            char resolved[24] = "";
                            if (fv[0]) {
                                uint32_t lid = (uint32_t)strtoul(fv, NULL, 10);
                                uint8_t tp2 = cmaxl[dcols[d]];
                                uint32_t lk2 = ((uint32_t)(tp2&0x3FFu)<<22)|(lid&0x3FFFFF);
                                uint8_t lb2[64]; uint16_t ll2 = sizeof(lb2);
                                if (kv_get_copy(lk2, lb2, &ll2, NULL) && ll2>=6 && lb2[0]==0x7D && lb2[1]==0xCA) {
                                    uint8_t lo2=lb2[4]&0x1F, lfl2=lb2[5];
                                    if (lo2==0 && 6+lfl2<=ll2 && lfl2>=1) {
                                        uint8_t sl2=lb2[6]; if(sl2>lfl2-1) sl2=lfl2-1; if(sl2>23) sl2=23;
                                        memcpy(resolved,lb2+7,sl2); resolved[sl2]='\0';
                                    }
                                }
                            }
                            n += snprintf(pg + n, sizeof(pg) - n,
                                "<td><input data-ord='%u' data-ftype='lookup' value='%s'"
                                " data-lid='%s' placeholder='Search...' list='lk%u_%u'></td>",
                                (unsigned)cords[dcols[d]], fv, resolved, (unsigned)cp, (unsigned)cords[dcols[d]]);
                        } else {
                            n += snprintf(pg + n, sizeof(pg) - n,
                                "<td><input data-ord='%u' data-ftype='%s' value='%s'",
                                (unsigned)cords[dcols[d]], dtn, fv);
                            if (dtc == 0x02 || dtc == 0x03) n += snprintf(pg + n, sizeof(pg) - n, " type=number");
                            else if (dtc == 0x07) n += snprintf(pg + n, sizeof(pg) - n, " type=number min=0 max=1");
                            else if (dtc == 0x0A) n += snprintf(pg + n, sizeof(pg) - n, " type=date");
                            n += snprintf(pg + n, sizeof(pg) - n, "></td>");
                        }
                    }
                    n += snprintf(pg + n, sizeof(pg) - n,
                        "<td><a href='#' onclick='this.closest(\"tr\").remove();return false' "
                        "style='color:#b04050;font-size:14px' title='Remove'>&#x2715;</a></td></tr>");
                    shown++;
                }

                // Empty row for adding new child
                uint32_t next_id = max_cid + 1;
                n += snprintf(pg + n, sizeof(pg) - n,
                    "<tr data-cid='%lu' class='new-row' style='opacity:.6'>",
                    (unsigned long)next_id);
                for (uint8_t d = 0; d < ndc && n < (int)sizeof(pg) - 150; d++) {
                    uint8_t dtc = ctypes[dcols[d]];
                    const char *dtn = type_name(dtc);
                    char ph[32]; pretty_name(ph, sizeof(ph), cnames[dcols[d]]);
                    if (dtc == 0x12 && !lk_is_search[d]) {
                        n += snprintf(pg + n, sizeof(pg) - n,
                            "<td><select data-ord='%u' data-ftype='lookup'>"
                            "<option value='0'>—</option>",
                            (unsigned)cords[dcols[d]]);
                        for (uint8_t li = 0; li < lk_counts[d] && n < (int)sizeof(pg) - 80; li++) {
                            n += snprintf(pg + n, sizeof(pg) - n,
                                "<option value='%lu'>%s</option>",
                                (unsigned long)lk_opts[d][li].id, lk_opts[d][li].name);
                        }
                        n += snprintf(pg + n, sizeof(pg) - n, "</select></td>");
                    } else if (dtc == 0x12 && lk_is_search[d]) {
                        n += snprintf(pg + n, sizeof(pg) - n,
                            "<td><input data-ord='%u' data-ftype='lookup' value='' "
                            "placeholder='Search %s...' list='lk%u_%u'></td>",
                            (unsigned)cords[dcols[d]], ph, (unsigned)cp, (unsigned)cords[dcols[d]]);
                    } else {
                        n += snprintf(pg + n, sizeof(pg) - n,
                            "<td><input data-ord='%u' data-ftype='%s' value='' placeholder='%s'",
                            (unsigned)cords[dcols[d]], dtn, ph);
                        if (dtc == 0x02 || dtc == 0x03) n += snprintf(pg + n, sizeof(pg) - n, " type=number");
                        else if (dtc == 0x07) n += snprintf(pg + n, sizeof(pg) - n, " type=number min=0 max=1");
                        else if (dtc == 0x0A) n += snprintf(pg + n, sizeof(pg) - n, " type=date");
                        n += snprintf(pg + n, sizeof(pg) - n, "></td>");
                    }
                }
                n += snprintf(pg + n, sizeof(pg) - n, "<td></td></tr>");

                n += snprintf(pg + n, sizeof(pg) - n,
                    "</tbody></table>"
                    "<div id='gridMsg%u' style='margin-top:4px;font-size:13px'></div></div>",
                    (unsigned)cp);
            }

            n += snprintf(pg + n, sizeof(pg) - n, "<script src=/app.js></script>");

            http_page_req(pcb, req, pg, (uint16_t)n);
            return;
        }

        if (parts >= 1) {
            // SSR card list for pack — paginated, multi-column
            if (!user_auth_can_read(&session, (uint16_t)pack_ord)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no access\"}");
                return;
            }

            // Parse page from query string ?p=N
            unsigned int page = 0;
            if (query) {
                const char *pp = strstr(query, "p=");
                if (pp) page = (unsigned int)strtoul(pp + 2, NULL, 10);
            }
            const unsigned int per_page = 10;

            // Load schema
            uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
            char pname[32] = "?";
            uint8_t ords[32], ftypes[32], maxlens[32]; char names[32][32];
            memset(names, 0, sizeof(names));
            uint8_t fc = 0;
            if (kv_get_copy(((uint32_t)0 << 22) | pack_ord, sbuf, &slen, NULL))
                fc = parse_schema(sbuf, slen, names, ftypes, maxlens, ords, pname, 32);

            // Show up to 4 columns
            uint8_t ncols = fc > 4 ? 4 : fc;
            if (ncols == 0) ncols = 1;

            // Get total card count
            uint32_t keys[128];
            uint32_t total = kv_range(((uint32_t)(pack_ord & 0x3FFu) << 22), 0xFFC00000u, keys, NULL, 128);
            set_cardinality((uint16_t)pack_ord, total);
            uint32_t total_pages = (total + per_page - 1) / per_page;
            if (total_pages == 0) total_pages = 1;
            if (page >= total_pages) page = total_pages - 1;
            uint32_t start = page * per_page;
            uint32_t end = start + per_page;
            if (end > total) end = total;

            char pg[4096]; int n = 0;

            // Header with count
            n += snprintf(pg + n, sizeof(pg) - n,
                "<h2>%s <span style='color:#556;font-size:14px'>Pack %u &middot; %lu cards</span></h2>",
                pname, pack_ord, (unsigned long)total);

            // Search bar
            n += snprintf(pg + n, sizeof(pg) - n,
                "<div class=search-box>"
                "<input id=qSearch placeholder='S:* F:%s W:%s|==|...' value=''>"
                "<button onclick=\"location.href='/query?q='+encodeURIComponent(document.getElementById('qSearch').value)\" class=btn-ghost>"
                "&#x1F50D; Query</button></div>",
                pname, fc > 0 ? names[0] : "field");

            // Table header with multiple columns
            n += snprintf(pg + n, sizeof(pg) - n,
                "<table><thead><tr><th style='width:50px'>#</th>");
            for (uint8_t c = 0; c < ncols && n < (int)sizeof(pg) - 200; c++) {
                char ph[32]; pretty_name(ph, sizeof(ph), names[c]);
                n += snprintf(pg + n, sizeof(pg) - n, "<th>%s</th>", ph);
            }
            n += snprintf(pg + n, sizeof(pg) - n, "</tr></thead><tbody>");

            // Render rows for this page
            for (uint32_t i = start; i < end && n < (int)sizeof(pg) - 400; i++) {
                uint32_t cid = keys[i] & 0x3FFFFF;
                // Decode all visible fields
                char fvals[4][48];
                memset(fvals, 0, sizeof(fvals));
                uint8_t cbuf[256]; uint16_t clen = sizeof(cbuf);
                if (kv_get_copy(keys[i], cbuf, &clen, NULL) && clen >= 4 &&
                    cbuf[0] == 0x7D && cbuf[1] == 0xCA) {
                    // Walk card binary, decode only the columns we need
                    uint16_t coff = 4;
                    while (coff + 1 < clen) {
                        uint8_t cord = cbuf[coff] & 0x1F;
                        uint8_t cflen = cbuf[coff + 1];
                        coff += 2;
                        if (coff + cflen > clen) break;
                        for (uint8_t c = 0; c < ncols && c < fc; c++) {
                            if (ords[c] == cord)
                                decode_field_str(fvals[c], sizeof(fvals[c]), ftypes[c], cbuf + coff, cflen);
                        }
                        coff += cflen;
                    }
                    for (uint8_t c = 0; c < ncols && c < fc; c++) {
                        // Resolve lookup display name
                        if (ftypes[c] == 0x12 && fvals[c][0]) {
                            uint32_t lid = (uint32_t)strtoul(fvals[c], NULL, 10);
                            uint8_t tp = maxlens[c];
                            uint32_t lk = ((uint32_t)(tp & 0x3FFu) << 22) | (lid & 0x3FFFFF);
                            uint8_t lb[128]; uint16_t ll = sizeof(lb);
                            if (kv_get_copy(lk, lb, &ll, NULL) && ll >= 6 &&
                                lb[0]==0x7D && lb[1]==0xCA) {
                                uint8_t lo = lb[4] & 0x1F, lfl = lb[5];
                                if (lo == 0 && 6 + lfl <= ll && lfl >= 1) {
                                    uint8_t sl = lb[6]; if (sl > lfl-1) sl=lfl-1; if (sl>46) sl=46;
                                    memcpy(fvals[c], lb+7, sl); fvals[c][sl]='\0';
                                }
                            }
                        }
                    }
                }

                n += snprintf(pg + n, sizeof(pg) - n,
                    "<tr><td><a href='/pack/%u/%lu'>%lu</a></td>",
                    pack_ord, (unsigned long)cid, (unsigned long)cid);
                for (uint8_t c = 0; c < ncols && n < (int)sizeof(pg) - 100; c++) {
                    if (c == 0)
                        n += snprintf(pg + n, sizeof(pg) - n,
                            "<td><a href='/pack/%u/%lu' style='color:#e0e0e0'>%s</a></td>",
                            pack_ord, (unsigned long)cid, fvals[c][0] ? fvals[c] : "-");
                    else
                        n += snprintf(pg + n, sizeof(pg) - n,
                            "<td>%s</td>", fvals[c][0] ? fvals[c] : "-");
                }
                n += snprintf(pg + n, sizeof(pg) - n, "</tr>");
            }
            n += snprintf(pg + n, sizeof(pg) - n, "</tbody></table>");

            // Pagination bar
            n += snprintf(pg + n, sizeof(pg) - n, "<div class=pager>");
            if (page > 0)
                n += snprintf(pg + n, sizeof(pg) - n,
                    "<a href='/pack/%u?p=%u'>&larr; Prev</a>", pack_ord, page - 1);
            else
                n += snprintf(pg + n, sizeof(pg) - n, "<span></span>");
            n += snprintf(pg + n, sizeof(pg) - n,
                "<span>Page %u of %lu</span>", page + 1, (unsigned long)total_pages);
            if (page + 1 < total_pages)
                n += snprintf(pg + n, sizeof(pg) - n,
                    "<a href='/pack/%u?p=%u'>Next &rarr;</a>", pack_ord, page + 1);
            else
                n += snprintf(pg + n, sizeof(pg) - n, "<span></span>");
            n += snprintf(pg + n, sizeof(pg) - n, "</div>");

            // New card button
            n += snprintf(pg + n, sizeof(pg) - n,
                "<div style='margin-top:8px'>"
                "<a href='/pack/%u/%lu' style='text-decoration:none'>"
                "<button type=button>+ New Card</button></a></div>",
                pack_ord, (unsigned long)(total > 0 ? (keys[total-1] & 0x3FFFFF) + 1 : 0));

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
            "<div><label>Module</label><input id=pm placeholder='e.g. Sales'></div>"
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
        char pname[32] = "?"; char pmodule[32] = "";
        uint8_t ords[32], ftypes[32], maxlens[32]; char names[32][32];
        memset(names, 0, sizeof(names));
        uint8_t fc = 0;
        uint8_t child_packs[8]; uint8_t child_count = 0;
        if (kv_get_copy(((uint32_t)0 << 22) | pack_ord, sbuf, &slen, NULL))
            fc = parse_schema_full(sbuf, slen, names, ftypes, maxlens, ords,
                                   pname, 32, pmodule, child_packs, &child_count);

        char pg[4096]; int n = 0;
        n += snprintf(pg + n, sizeof(pg) - n,
            "<h2><a href='/admin/meta' style='color:#7eb8f0'>&laquo; Schema</a> / Pack %u: %s",
            pack_ord, pname);
        if (pmodule[0])
            n += snprintf(pg + n, sizeof(pg) - n,
                " <span style='font-size:13px;color:#6a7080'>(%s)</span>", pmodule);
        n += snprintf(pg + n, sizeof(pg) - n, "</h2>"
            "<table><thead><tr><th>#</th><th>Field</th><th>Type</th><th>Max</th></tr></thead><tbody>");

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
            "uint8","uint16","uint32","int8","int16","int32",
            "bool","ascii","utf8","date","time","datetime",
            "array_u16","blob","lookup"
        };
        for (int i = 0; i < 15 && n < (int)sizeof(pg) - 100; i++) {
            n += snprintf(pg + n, sizeof(pg) - n,
                "<option value='%s'%s>%s</option>",
                ft_names[i], strcmp(ft_names[i],"utf8")==0 ? " selected" : "", ft_names[i]);
        }

        n += snprintf(pg + n, sizeof(pg) - n,
            "</select></div></div>"
            "<div class=row>"
            "<div><label>Max Length</label><input id=fm type=number value=32 min=1 max=255></div>"
            "<div style='align-self:end'><button>Save Field</button></div>"
            "</div></form></div>"
            "<div class=card><h2>Detail Packs (1:Many)</h2>"
            "<p style='font-size:13px;color:#6a7080'>Child packs shown as detail tables when editing a card in this pack.</p>"
            "<div style='margin-bottom:8px'>Current: ",
            pack_ord, (unsigned int)fc);

        if (child_count > 0) {
            for (uint8_t ci = 0; ci < child_count; ci++) {
                // Resolve child pack name
                char cpn[16] = "?";
                uint8_t csb[64]; uint16_t csl = sizeof(csb);
                if (kv_get_copy(((uint32_t)0 << 22) | child_packs[ci], csb, &csl, NULL) && csl > 4) {
                    uint8_t co = 4;
                    if (co + 1 < csl && (csb[co] & 0x1F) == 0) {
                        uint8_t fl = csb[co+1]; if (co+2+fl <= csl && fl >= 1) {
                            uint8_t nl = csb[co+2]; if (nl > fl-1) nl=fl-1; if (nl>15) nl=15;
                            memcpy(cpn, csb+co+3, nl); cpn[nl]='\0';
                        }
                    }
                }
                if (ci > 0) n += snprintf(pg + n, sizeof(pg) - n, ", ");
                n += snprintf(pg + n, sizeof(pg) - n, "<b>%s</b> (%u)", cpn, (unsigned)child_packs[ci]);
            }
        } else {
            n += snprintf(pg + n, sizeof(pg) - n, "<i style='color:#6a7080'>None</i>");
        }
        n += snprintf(pg + n, sizeof(pg) - n,
            "</div>"
            "<div class=row>"
            "<div><label>Child Pack IDs</label>"
            "<input id=cpInput placeholder='e.g. 6,8' value='");
        // Pre-fill current children
        for (uint8_t ci = 0; ci < child_count; ci++) {
            if (ci > 0) n += snprintf(pg + n, sizeof(pg) - n, ",");
            n += snprintf(pg + n, sizeof(pg) - n, "%u", (unsigned)child_packs[ci]);
        }
        n += snprintf(pg + n, sizeof(pg) - n,
            "'></div>"
            "<div style='align-self:end'>"
            "<button onclick=\"fetch('/admin/children/%u',{method:'POST',credentials:'same-origin',"
            "body:document.getElementById('cpInput').value}).then(function(r){"
            "if(r.ok)location.reload();else alert('Failed')})\">Set Children</button>"
            "</div></div></div>"
            "<div class=card style='border-color:#b04050'>"
            "<h2 style='color:#e06070'>Danger Zone</h2>",
            pack_ord);

        n += snprintf(pg + n, sizeof(pg) - n,
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
        // Parse JSON body: {"ordinal":N,"name":"...","module":"..."}
        char jbuf[256];
        uint16_t jlen = body_len < sizeof(jbuf) - 1 ? body_len : sizeof(jbuf) - 1;
        memcpy(jbuf, body, jlen); jbuf[jlen] = '\0';
        web_log("[meta/new] body_len=%u jbuf='%.60s'", (unsigned)body_len, jbuf);

        unsigned int ord = 0; char name[32] = ""; char module[32] = "";
        // Parse JSON values — handle optional whitespace after colons
        char *p = strstr(jbuf, "\"ordinal\":");
        if (p) { p += 10; while (*p == ' ') p++; ord = (unsigned int)atoi(p); }
        p = strstr(jbuf, "\"name\":");
        if (p) { p += 7; while (*p == ' ') p++; if (*p == '"') { p++; char *end = strchr(p, '"'); if (end && end - p < 32) { memcpy(name, p, end - p); name[end - p] = '\0'; } } }
        p = strstr(jbuf, "\"module\":");
        if (p) { p += 9; while (*p == ' ') p++; if (*p == '"') { p++; char *end = strchr(p, '"'); if (end && end - p < 32) { memcpy(module, p, end - p); module[end - p] = '\0'; } } }

        if (name[0] == '\0') {
            web_log("[meta/new] name empty, ord=%u module='%s'", ord, module);
            http_json(pcb, "400 Bad Request", "{\"error\":\"name required\"}");
            return;
        }

        // Build minimal schema card (no fields yet), with optional module
        user_auth_schema_field_t empty[1];
        bool ok;
        if (module[0])
            ok = user_auth_seed_schema_module((uint16_t)ord, name, empty, 0, module);
        else
            ok = user_auth_seed_schema((uint16_t)ord, name, empty, 0);
        if (ok) {
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

        // Validate field name: A-Za-z0-9_+-*/.  only
        for (char *v = fname; *v; v++) {
            char c = *v;
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '+' ||
                  c == '-' || c == '*' || c == '/' || c == '.')) {
                http_json(pcb, "400 Bad Request", "{\"error\":\"field name: A-Za-z0-9_+-*/. only\"}");
                return;
            }
        }

        // Resolve type name to picowal.js type code (NOT metadata_dict enum)
        uint8_t ftype_code = 255;
        static const struct { const char *name; uint8_t code; } type_map[] = {
            {"bool",0x07}, {"char",0x08}, {"char[]",0x08},
            {"uint8",0x01}, {"int8",0x04}, {"int16",0x05}, {"int32",0x06},
            {"uint16",0x02}, {"uint32",0x03},
            {"isodate",0x0A}, {"isotime",0x0B}, {"isodatetime",0x0C},
            {"utf8",0x09}, {"ascii",0x08}, {"latin1",0x08},
            {"array_u16",0x10}, {"blob",0x11}, {"lookup",0x12},
        };
        for (unsigned ti = 0; ti < sizeof(type_map)/sizeof(type_map[0]); ti++) {
            if (strcmp(ftname, type_map[ti].name) == 0) { ftype_code = type_map[ti].code; break; }
        }
        if (ftype_code == 255) {
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

    // ---- Route: POST /batch — atomic multi-card write ----
    // Binary: 0xBA 0x7C count(u16) then [pack(u16) card(u32) len(u16) data[len]]...
    if (verb == VERB_POST && strcmp(path, "/batch") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session)) {
            http_json(pcb, "401 Unauthorized", "{\"error\":\"login required\"}");
            return;
        }
        if (body_len < 4 || body[0] != 0xBA || body[1] != 0x7C) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"bad batch magic\"}");
            return;
        }
        uint16_t count = body[2] | ((uint16_t)body[3] << 8);
        if (count == 0 || count > 32) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"batch count 1-32\"}");
            return;
        }

        // First pass: validate all entries and check RBAC
        uint16_t off = 4;
        for (uint16_t i = 0; i < count; i++) {
            if (off + 8 > body_len) {
                http_json(pcb, "400 Bad Request", "{\"error\":\"truncated batch\"}");
                return;
            }
            uint16_t bpack = body[off] | ((uint16_t)body[off+1] << 8);
            // skip card_id (4 bytes)
            uint16_t blen = body[off+6] | ((uint16_t)body[off+7] << 8);
            off += 8;
            if (off + blen > body_len || blen > KV_MAX_VALUE) {
                http_json(pcb, "400 Bad Request", "{\"error\":\"bad entry size\"}");
                return;
            }
            if (!user_auth_can_write(&session, bpack)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no write access\"}");
                return;
            }
            // Validate card magic
            if (blen >= 2 && (body[off] != 0x7D || body[off+1] != 0xCA)) {
                http_json(pcb, "400 Bad Request", "{\"error\":\"bad card magic\"}");
                return;
            }
            off += blen;
        }

        // Second pass: write all cards
        off = 4;
        uint16_t ok_count = 0;
        for (uint16_t i = 0; i < count; i++) {
            uint16_t bpack = body[off] | ((uint16_t)body[off+1] << 8);
            uint32_t bcard = body[off+2] | ((uint32_t)body[off+3]<<8) |
                             ((uint32_t)body[off+4]<<16) | ((uint32_t)body[off+5]<<24);
            uint16_t blen = body[off+6] | ((uint16_t)body[off+7] << 8);
            off += 8;
            uint32_t key = ((uint32_t)(bpack & 0x3FF) << 22) | (bcard & 0x3FFFFF);
            if (kv_put(key, body + off, blen)) ok_count++;
            off += blen;
        }

        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"count\":%u}", (unsigned)ok_count);
        http_json(pcb, ok_count == count ? "200 OK" : "207 Multi-Status", resp);
        return;
    }

    // ---- Route: POST /admin/children/{n} — set child packs for master pack ----
    // Body: comma-separated pack ordinals, e.g. "6,8,9"
    if (verb == VERB_POST && strncmp(path, "/admin/children/", 16) == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        unsigned int pack_ord = 0;
        sscanf(path, "/admin/children/%u", &pack_ord);

        // Parse comma-separated child pack ordinals
        char cbuf[64];
        uint16_t clen = body_len < sizeof(cbuf) - 1 ? body_len : sizeof(cbuf) - 1;
        memcpy(cbuf, body, clen); cbuf[clen] = '\0';
        uint8_t cpacks[8]; uint8_t nc = 0;
        char *tok = cbuf;
        while (*tok && nc < 8) {
            while (*tok == ' ' || *tok == ',') tok++;
            if (*tok == '\0') break;
            cpacks[nc++] = (uint8_t)atoi(tok);
            while (*tok && *tok != ',') tok++;
        }

        // Read existing schema card, rebuild with ord 6 appended/updated
        uint32_t skey = ((uint32_t)0 << 22) | pack_ord;
        uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
        if (!kv_get_copy(skey, sbuf, &slen, NULL) || slen < 4) {
            http_json(pcb, "404 Not Found", "{\"error\":\"pack schema not found\"}");
            return;
        }

        // Copy existing schema, stripping any existing ord 6
        uint8_t nbuf[256]; uint16_t noff = 0;
        if (slen >= 4) { memcpy(nbuf, sbuf, 4); noff = 4; } // magic+version
        uint16_t roff = 4;
        while (roff + 1 < slen) {
            uint8_t ord = sbuf[roff] & 0x1F;
            uint8_t flen = sbuf[roff + 1];
            if (roff + 2 + flen > slen) break;
            if (ord != 6 && noff + 2 + flen <= sizeof(nbuf)) {
                memcpy(nbuf + noff, sbuf + roff, 2 + flen);
                noff += 2 + flen;
            }
            roff += 2 + flen;
        }

        // Append new ord 6 if we have children
        if (nc > 0 && noff + 2 + nc <= sizeof(nbuf)) {
            nbuf[noff++] = 6;
            nbuf[noff++] = nc;
            memcpy(nbuf + noff, cpacks, nc);
            noff += nc;
        }

        if (kv_put(skey, nbuf, noff)) {
            http_json(pcb, "200 OK", "{\"ok\":true}");
        } else {
            http_json(pcb, "500 Internal Server Error", "{\"error\":\"write failed\"}");
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
            if (!user_auth_can_read(&session, (uint16_t)type_val)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no read access to this pack\"}");
                return;
            }
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
            if (!user_auth_can_read(&session, (uint16_t)type_val)) {
                http_json(pcb, "403 Forbidden", "{\"error\":\"no read access to this pack\"}");
                return;
            }
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
            "<button onclick=\"if(confirm('WIPE ALL DATA? This erases flash KV + SD and reboots.')){fetch('/admin/wipe',{method:'POST',credentials:'same-origin'}).then(function(){document.getElementById('logpre').textContent='Wiping...'})}\" style='background:#800'>Wipe All</button>"
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

    // ---- Route: POST /admin/wipe — erase all user data (flash KV + SD + flash index) ----
    if (verb == VERB_POST && strcmp(path, "/admin/wipe") == 0) {
        user_session_t session;
        if (!check_auth_session(req, &session) || !user_auth_is_admin(&session)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"admin required\"}");
            return;
        }
        web_log("[admin] WIPE: erasing all user data\n");

        // Erase flash KV (keeps firmware, erases KV region)
        kv_wipe();
        web_log("[admin] Flash KV wiped\n");

        // Erase flash index tier (768KB–1MB region)
        {
            uint32_t irq = save_and_disable_interrupts();
            for (uint32_t s = 0; s < FIDX_SECTORS; s++) {
                flash_range_erase(FIDX_FLASH_OFFSET + s * FIDX_SECTOR_SIZE, FIDX_SECTOR_SIZE);
            }
            restore_interrupts(irq);
        }
        web_log("[admin] Flash index wiped\n");

        // Reinitialise flash KV (empty)
        kv_init();
        web_log("[admin] Flash KV reinitialised\n");

        // Wipe SD superblock (forces reinit on next boot)
        if (kvsd_ready()) {
            uint8_t zero[512];
            memset(zero, 0, sizeof(zero));
            sd_write_block(0, zero);
            web_log("[admin] SD superblock wiped\n");
        }

        // Re-seed default admin + system packs
        user_auth_init();
        web_log("[admin] Admin user + system packs re-seeded\n");

        http_json(pcb, "200 OK", "{\"ok\":true,\"wiped\":true,\"rebooting\":true}");
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

    // ---- OTA: POST /update/begin — prepare SD staging ----
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
        if (!kvsd_ready()) {
            http_json(pcb, "503 Service Unavailable", "{\"error\":\"SD card not ready\"}");
            return;
        }

        g_ota.active = true;
        g_ota.sd_base = kvsd_ota_start_block();
        g_ota.offset = 0;
        g_ota.total_written = 0;

        web_log("[ota] BEGIN — staging to SD block %lu\n", (unsigned long)g_ota.sd_base);

        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"max\":%lu,\"staging\":\"SD\",\"block\":%lu}",
                 (unsigned long)OTA_SLOT_SIZE, (unsigned long)g_ota.sd_base);
        http_json(pcb, "200 OK", resp);
        return;
    }

    // ---- OTA: POST /update/chunk — write firmware chunk to SD ----
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
            http_json(pcb, "413 Payload Too Large", "{\"error\":\"exceeds 600KB\"}");
            g_ota.active = false;
            return;
        }

        ota_write_chunk_sd(body, body_len);

        char resp[80];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"written\":%lu}", (unsigned long)g_ota.total_written);
        http_json(pcb, "200 OK", resp);
        return;
    }

    // ---- OTA: POST /update/commit — read SD → flash, reboot ----
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

        web_log("[ota] COMMIT — %lu bytes from SD → flash\n",
                (unsigned long)g_ota.total_written);

        // Send response before destructive flash operations
        char resp[80];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"total\":%lu,\"rebooting\":true}",
                 (unsigned long)g_ota.total_written);
        http_json(pcb, "200 OK", resp);
        tcp_output(pcb);
        sleep_ms(500);

        // Don't flush index — flash is about to be rewritten.
        // SD hash table + flash index will be rebuilt on next boot.

        // Park Core 1 — set halt flag and wait for it to stop touching flash
        g_wal->ota_halt_core1 = true;
        __sev();  // wake Core 1 if sleeping
        sleep_ms(50);  // let Core 1 finish any in-flight flash op

        hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);

        // Copy SD → flash from SRAM function (writes sector 0 last, then reboots)
        ota_commit_from_sd(g_ota.sd_base, g_ota.total_written);
        // Never reaches here — ota_commit_from_sd reboots
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

        char pg[2048]; int n = 0;
        bool sd_ok = kvsd_ready();
        n += snprintf(pg + n, sizeof(pg) - n,
            "<h2>Firmware Update</h2>"
            "<div class=card>"
            "<p>Staging via <strong>%s</strong>. Flash target: <strong>Slot A</strong> (0x000000).</p>"
            "<p>%s</p>"
            "<p>Select a <code>.bin</code> firmware file. Max 600KB.</p>"
            "<input type=file id=fw accept='.bin'>"
            "<button onclick=doOTA() style='margin-top:8px;width:100%%'%s>Upload &amp; Flash</button>"
            "<pre id=otaLog>%s</pre>"
            "</div>"
            "<script>"
            "async function doOTA(){"
            "var f=document.getElementById('fw').files[0];"
            "if(!f){document.getElementById('otaLog').textContent='No file selected';return}"
            "var log=document.getElementById('otaLog');"
            "log.textContent='Uploading to SD ('+f.size+' bytes)...';"
            "var r=await fetch('/update/begin',{method:'POST',credentials:'same-origin'});"
            "if(!r.ok){log.textContent='BEGIN failed: '+r.status;return}"
            "var info=await r.json();log.textContent='Staging to SD block '+info.block+'...';"
            "var buf=new Uint8Array(await f.arrayBuffer());"
            "var chunk=1024,off=0;"
            "while(off<buf.length){"
            "var end=Math.min(off+chunk,buf.length);"
            "var r=await fetch('/update/chunk',{method:'POST',credentials:'same-origin',body:buf.slice(off,end)});"
            "if(!r.ok){log.textContent='CHUNK failed at '+off+': '+r.status;return}"
            "off=end;log.textContent='SD: '+off+'/'+buf.length+' ('+Math.round(100*off/buf.length)+'%%)'}"
            "log.textContent='Committing: SD → Flash...';"
            "await fetch('/update/commit',{method:'POST',credentials:'same-origin'}).catch(function(){});"
            "log.textContent='Rebooting! Page will reload in 15s...';"
            "setTimeout(function(){location.reload()},15000)}"
            "</script>",
            sd_ok ? "SD Card" : "Not available",
            sd_ok ? "Firmware uploads to SD staging area, then flashes on commit." : "<strong style='color:#b04050'>SD card not available — OTA disabled.</strong>",
            sd_ok ? "" : " disabled",
            sd_ok ? "Ready — SD staging available" : "ERROR: SD card required for OTA");

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

        // RBAC: check read access on all FROM packs before executing
        for (uint8_t fi = 0; fi < q.from_count; fi++) {
            // Resolve pack name to ordinal for RBAC check
            uint32_t skeys[64];
            uint32_t scount = kv_range(0, 0xFFC00000u, skeys, NULL, 64);
            for (uint32_t si = 0; si < scount; si++) {
                uint8_t sb[256]; uint16_t sl = sizeof(sb);
                if (!kv_get_copy(skeys[si], sb, &sl, NULL)) continue;
                if (sl < 6 || sb[0] != 0x7D || sb[1] != 0xCA) continue;
                char pn[32] = ""; uint16_t off = 4;
                while (off + 1 < sl) {
                    uint8_t ord = sb[off] & 0x1F, fl = sb[off+1]; off += 2;
                    if (off + fl > sl) break;
                    if (ord == 0 && fl >= 1) {
                        uint8_t nl = sb[off]; if (nl > 31) nl = 31;
                        memcpy(pn, sb+off+1, nl); pn[nl] = '\0';
                    }
                    off += fl;
                }
                // Case-insensitive match
                bool match = (strlen(pn) > 0 && strlen(pn) == strlen(q.from_decks[fi]));
                for (int ci = 0; match && pn[ci]; ci++) {
                    char a = pn[ci], b = q.from_decks[fi][ci];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) match = false;
                }
                if (match) {
                    uint16_t pack_ord = (uint16_t)(skeys[si] & 0x3FFFFF);
                    if (!user_auth_can_read(&session, pack_ord)) {
                        http_json(pcb, "403 Forbidden", "{\"error\":\"no read access to queried pack\"}");
                        return;
                    }
                    break;
                }
            }
        }

        char result[4096];
        const char *pack_name = "";
        int result_count = 0;
        int rlen = query_execute(&q, result, sizeof(result), &pack_name, &result_count);

        // Pack name and count in headers, body is pipe-delimited rows
        char extra_hdrs[128];
        snprintf(extra_hdrs, sizeof(extra_hdrs),
                 "X-Pack: %s\r\nX-Count: %d\r\n", pack_name, result_count);
        http_respond_with_headers(pcb, "200 OK", "text/plain",
                                  extra_hdrs, (const uint8_t *)result, (uint16_t)rlen);
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
            "<div id=qhdrs style='margin-top:8px;font-size:12px;color:#888'></div>"
            "<pre id=qresult style='margin-top:4px;max-height:400px;overflow-y:auto'>Results will appear here</pre>"
            "</div>"
            "<script>"
            "function runQuery(){"
            "var q=document.getElementById('qtext').value;"
            "fetch('/query',{method:'POST',credentials:'same-origin',body:q})"
            ".then(function(r){"
            "var pack=r.headers.get('X-Pack')||'?';"
            "var count=r.headers.get('X-Count')||'?';"
            "document.getElementById('qhdrs').textContent='Pack: '+pack+' | Count: '+count;"
            "return r.text()})"
            ".then(function(t){document.getElementById('qresult').textContent=t})}"
            "</script>";

        http_page_req(pcb, req, qpage, sizeof(qpage) - 1);
        return;
    }

    // ---- Route: GET /notes/{pack}/{card}?writenotes={text} — anonymous notes ----
    // No auth required. ONLY pack 99 allowed (anonymous notes sentinel).
    if (verb == VERB_GET && strncmp(path, "/notes/", 7) == 0) {
        unsigned int npack = 0, ncard = 0;
        if (sscanf(path, "/notes/%u/%u", &npack, &ncard) < 2) {
            http_json(pcb, "400 Bad Request", "{\"error\":\"expected /notes/{pack}/{card}\"}");
            return;
        }
        if (npack != 99) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"notes only allowed on pack 99\"}");
            return;
        }
        uint32_t key = ((uint32_t)(npack & 0x3FF) << 22) | (ncard & 0x3FFFFF);

        // Check for ?writenotes= query parameter
        const char *wn = query ? strstr(query, "writenotes=") : NULL;
        if (wn) {
            wn += 11;  // skip "writenotes="
            // Build a minimal card: magic + version + utf8 field
            uint8_t card[512];
            uint16_t tlen = 0;
            for (const char *p2 = wn; *p2 && *p2 != '&' && tlen < 480; p2++) {
                // URL decode: + → space, %XX → byte
                if (*p2 == '+') { card[6 + tlen++] = ' '; }
                else if (*p2 == '%' && p2[1] && p2[2]) {
                    uint8_t hi = p2[1], lo = p2[2];
                    uint8_t v = 0;
                    if (hi >= '0' && hi <= '9') v = (hi-'0')<<4;
                    else if (hi >= 'a' && hi <= 'f') v = (hi-'a'+10)<<4;
                    else if (hi >= 'A' && hi <= 'F') v = (hi-'A'+10)<<4;
                    if (lo >= '0' && lo <= '9') v |= lo-'0';
                    else if (lo >= 'a' && lo <= 'f') v |= lo-'a'+10;
                    else if (lo >= 'A' && lo <= 'F') v |= lo-'A'+10;
                    card[6 + tlen++] = v;
                    p2 += 2;
                } else { card[6 + tlen++] = (uint8_t)*p2; }
            }
            // Header: magic + version
            card[0] = 0x7D; card[1] = 0xCA; card[2] = 1; card[3] = 0;
            // Field 0 (ord 0, type utf8): the note text
            card[4] = 0x00;  // ord byte
            card[5] = (uint8_t)tlen;
            uint16_t total = 6 + tlen;

            if (kv_store_put(key, card, total)) {
                http_json(pcb, "200 OK", "{\"ok\":true}");
            } else {
                http_json(pcb, "500 Internal Server Error", "{\"error\":\"write failed\"}");
            }
            return;
        }

        // Read note
        uint8_t nbuf[512];
        uint16_t nlen = sizeof(nbuf);
        if (kv_store_get_copy(key, nbuf, &nlen, NULL) && nlen > 6 &&
            nbuf[0] == 0x7D && nbuf[1] == 0xCA) {
            uint8_t flen = nbuf[5];
            if (flen > nlen - 6) flen = nlen - 6;
            http_respond(pcb, "200 OK", "text/plain", nbuf + 6, flen);
        } else {
            http_json(pcb, "404 Not Found", "{\"error\":\"note not found\"}");
        }
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
