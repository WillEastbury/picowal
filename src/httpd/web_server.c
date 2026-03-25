#include "web_server.h"
#include "kv_flash.h"
#include "metadata_dict.h"
#include "wal_defs.h"
#include "wal_fence.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "lwip/netif.h"
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
#define HTTP_CONN_COUNT 2

static wal_state_t *g_wal;
static volatile uint32_t g_http_last_activity_ms = 0;

// Runtime PSK for HTTP auth (set from net_core)
static uint8_t g_http_psk[32];
static bool g_http_psk_set = false;

static const char GUI_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>Storage Appliance GUI</title>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<link rel=stylesheet href=/gui.css></head><body>"
"<h1>Storage Appliance GUI</h1>"
"<p><a href=/w/0/0>OPEN METADATA EDITOR</a></p>"
"<label>PSK</label><input id=psk placeholder='64 hex chars'>"
"<div class=row><div><label>TYPE</label><input id=type value=0></div><div><label>ID</label><input id=id value=0></div></div>"
"<button id=loadBtn>LOAD</button><button id=saveBtn>SAVE</button>"
"<label>VALUE (JSON object keyed by field name)</label><textarea id=value>{\n  \"example\": true\n}</textarea>"
"<h2>Metadata</h2><button id=metaBtn>LOAD METADATA</button><pre id=meta>NOT LOADED</pre>"
"<h2>Seed Data</h2>"
"<div class=row3><div><label>RECORDS</label><input id=seedCount type=number min=1 max=5000 value=5000></div><div><label>START ID</label><input id=seedStartId type=number min=0 max=4194303 value=1></div><div><label>BATCH SIZE</label><input id=seedBatch type=number min=1 max=32 value=8></div></div>"
"<button id=seedBtn>SEED FROM METADATA</button><pre id=status>READY</pre>"
"<script src=/gui_codec.js></script><script src=/gui_app.js></script></body></html>";

static const char META_EDITOR_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>Metadata Editor</title>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<link rel=stylesheet href=/gui.css></head><body>"
"<h1>Metadata Editor</h1>"
"<p><a href=/gui>BACK TO RECORD GUI</a></p>"
"<label>PSK</label><input id=psk placeholder='64 hex chars'>"
"<label>OBJECT ID</label><input id=objId readonly>"
"<h2>Type</h2>"
"<div class=row><div><label>TYPE ORDINAL</label><input id=typeOrd value=0></div><div><label>TYPE NAME</label><input id=typeName></div></div>"
"<button id=saveTypeBtn>SAVE TYPE</button>"
"<h2>Field</h2>"
"<div class=row3><div><label>FIELD ORDINAL</label><input id=fieldOrd value=1></div><div><label>FIELD NAME</label><input id=fieldName></div><div><label>FIELD TYPE</label><input id=fieldType placeholder='utf8'></div></div>"
"<div class=row><div><label>MAX LEN</label><input id=fieldMaxLen value=32></div><div></div></div>"
"<button id=saveFieldBtn>SAVE FIELD</button>"
"<h2>Metadata Snapshot</h2><button id=reloadMetaBtn>RELOAD</button><pre id=meta>NOT LOADED</pre><pre id=status>READY</pre>"
"<script src=/meta_editor.js></script></body></html>";

static const char GUI_CSS[] =
"body{font:14px monospace;background:#111;color:#eee;padding:16px;max-width:900px}"
"input,textarea,button{font:inherit;width:100%;margin:6px 0;padding:8px}"
"button{background:#0ff;color:#000;border:0;font-weight:bold;cursor:pointer}"
"textarea{min-height:180px}pre{white-space:pre-wrap;background:#222;padding:10px}"
".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px}";

static const char GUI_CODEC_JS[] =
"(()=>{const MAGIC=[77,68,66,49],FLAG_HEAP=1,utf8enc=new TextEncoder(),utf8dec=new TextDecoder('utf-8');"
"function clipText(s,maxLen){const n=Math.max(1,Math.min(maxLen||255,255));return String(s).slice(0,n)}"
"function latin1Encode(s,maxLen){const text=clipText(String(s),maxLen||255),out=new Uint8Array(text.length);for(let i=0;i<text.length;i++)out[i]=text.charCodeAt(i)&255;return out}"
"function latin1Decode(bytes){let s='';for(const b of bytes)s+=String.fromCharCode(b);return s}"
"function u16le(n){return Uint8Array.of(n&255,(n>>>8)&255)}function u32le(n){return Uint8Array.of(n&255,(n>>>8)&255,(n>>>16)&255,(n>>>24)&255)}"
"function inlineU32(bytes){let v=0;for(let i=0;i<bytes.length&&i<4;i++)v|=(bytes[i]<<(i*8));return v>>>0}"
"function inlineBytes(len,value){return Uint8Array.of(value&255,(value>>>8)&255,(value>>>16)&255,(value>>>24)&255).slice(0,len)}"
"function i16le(n){const b=new Uint8Array(2);new DataView(b.buffer).setInt16(0,Number(n)||0,true);return b}"
"function i32le(n){const b=new Uint8Array(4);new DataView(b.buffer).setInt32(0,Number(n)||0,true);return b}"
"function concatBytes(parts){let len=0;for(const p of parts)len+=p.length;const out=new Uint8Array(len);let off=0;for(const p of parts){out.set(p,off);off+=p.length}return out}"
"function isHeapField(field){switch(field.type){case 'char[]':case 'byte[]':case 'isodate':case 'isotime':case 'isodatetime':case 'utf8':case 'latin1':return true;default:return false}}"
"function encodeFieldValue(field,value){const safe=(value===undefined||value===null)?'':value;switch(field.type){case 'bool':return Uint8Array.of(value?1:0);case 'char':return Uint8Array.of(String(value||' ').charCodeAt(0)&255);case 'char[]':return latin1Encode(safe,field.max_len||255);case 'utf8':return utf8enc.encode(clipText(safe,field.max_len||255));case 'latin1':return latin1Encode(safe,field.max_len||255);case 'byte':return Uint8Array.of((Number(value)||0)&255);case 'byte[]':{const arr=Array.isArray(value)?value:[],len=Math.min(arr.length,field.max_len||255),out=new Uint8Array(len);for(let i=0;i<len;i++)out[i]=(Number(arr[i])||0)&255;return out}case 'uint8':return Uint8Array.of((Number(value)||0)&255);case 'int8':return Uint8Array.of((Number(value)||0)&255);case 'int16':return i16le(value);case 'int32':return i32le(value);case 'uint16':return u16le((Number(value)||0)&65535);case 'uint32':return u32le((Number(value)||0)>>>0);case 'isodate':case 'isotime':case 'isodatetime':return utf8enc.encode(clipText(safe,field.max_len||32));default:return utf8enc.encode(String(safe))}}"
"function decodeFieldValue(field,bytes){const dv=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);switch(field.type){case 'bool':return bytes.length?bytes[0]!==0:false;case 'char':return bytes.length?String.fromCharCode(bytes[0]):'';case 'char[]':return latin1Decode(bytes);case 'utf8':return utf8dec.decode(bytes);case 'latin1':return latin1Decode(bytes);case 'byte':return bytes.length?bytes[0]:0;case 'byte[]':return Array.from(bytes);case 'uint8':return bytes.length?bytes[0]:0;case 'int8':return bytes.length?dv.getInt8(0):0;case 'int16':return bytes.length>=2?dv.getInt16(0,true):0;case 'int32':return bytes.length>=4?dv.getInt32(0,true):0;case 'uint16':return bytes.length>=2?dv.getUint16(0,true):0;case 'uint32':return bytes.length>=4?dv.getUint32(0,true):0;case 'isodate':case 'isotime':case 'isodatetime':return utf8dec.decode(bytes);default:return Array.from(bytes)}}"
"function encodeBinaryRecord(meta,obj){const entries=[],heap=[];let heapLen=0;for(const field of meta.fields){if(!Object.prototype.hasOwnProperty.call(obj,field.name))continue;const payload=encodeFieldValue(field,obj[field.name]),heapField=isHeapField(field),flags=heapField?FLAG_HEAP:0,data=heapField?heapLen:inlineU32(payload);entries.push(concatBytes([u16le(field.ordinal),Uint8Array.of(field.field_type),Uint8Array.of(flags),u16le(payload.length),u32le(data)]));if(heapField){heap.push(payload);heapLen+=payload.length}}return concatBytes([Uint8Array.from(MAGIC),u16le(entries.length),u16le(heapLen),...entries,...heap])}"
"function decodeBinaryRecord(meta,bytes){if(bytes.length<8||bytes[0]!==MAGIC[0]||bytes[1]!==MAGIC[1]||bytes[2]!==MAGIC[2]||bytes[3]!==MAGIC[3])return null;const dv=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);let off=4;const count=dv.getUint16(off,true);off+=2;const heapLen=dv.getUint16(off,true);off+=2;const heapBase=off+count*10;if(heapBase+heapLen>bytes.length)throw new Error('truncated heap');const obj={};for(let i=0;i<count;i++){if(off+10>bytes.length)throw new Error('truncated record');const ord=dv.getUint16(off,true);off+=2;const storedType=bytes[off++],flags=bytes[off++],len=dv.getUint16(off,true);off+=2;const data=dv.getUint32(off,true);off+=4;const field=meta.fieldsByOrdinal[ord],payload=(flags&FLAG_HEAP)?bytes.slice(heapBase+data,heapBase+data+len):inlineBytes(len,data);if((flags&FLAG_HEAP)&&(data+len>heapLen))throw new Error('heap range out of bounds');if(!field)continue;obj[field.name]=decodeFieldValue(field,payload);obj['_'+field.name+'_ordinal']=ord;obj['_'+field.name+'_heap']=(flags&FLAG_HEAP)!==0;if(field.field_type!==storedType)obj['_'+field.name+'_type_mismatch']={stored:storedType,meta:field.field_type}}return obj}"
"window.guiCodec={utf8dec,encodeBinaryRecord,decodeBinaryRecord,clipText}})();";

static const char GUI_APP_JS[] =
"(()=>{const $=id=>document.getElementById(id);let metaCache=null;$('psk').value=localStorage.getItem('psk')||'';"
"function hdr(extra){const p=$('psk').value.trim();localStorage.setItem('psk',p);return Object.assign({'Authorization':'PSK '+p},extra||{})}"
"function path(){return '/0/'+$('type').value.trim()+'/'+$('id').value.trim()}"
"async function readText(r){const t=await r.text();if(!r.ok)throw new Error(r.status+' '+t);return t}"
"async function readBytes(r){const b=new Uint8Array(await r.arrayBuffer());if(!r.ok)throw new Error(r.status+' '+window.guiCodec.utf8dec.decode(b));return b}"
"async function readJson(r){return JSON.parse(await readText(r))}function setStatus(t){$('status').textContent=t}"
"function normalizeMeta(){if(!metaCache)return;metaCache.fields=(metaCache.fields||[]).slice().sort((a,b)=>a.ordinal-b.ordinal);metaCache.fieldsByName={};metaCache.fieldsByOrdinal={};for(const f of metaCache.fields){metaCache.fieldsByName[f.name]=f;metaCache.fieldsByOrdinal[f.ordinal]=f}}"
"function makeFieldValue(field,seq,typeOrd,recordId){const base=field.name+'_'+typeOrd+'_'+recordId+'_'+seq;switch(field.type){case 'bool':return((seq+typeOrd+recordId)&1)===0;case 'char':return String.fromCharCode(65+((seq+typeOrd+recordId)%26));case 'char[]':return window.guiCodec.clipText(base,field.max_len||24);case 'utf8':return window.guiCodec.clipText('utf8_\\u00e9_\\u03a9_'+base,field.max_len||24);case 'latin1':return window.guiCodec.clipText('\\u00c4\\u00d6\\u00dc_'+base,field.max_len||24);case 'byte':return(seq+recordId+typeOrd)&255;case 'byte[]':{const len=Math.max(1,Math.min(field.max_len||8,16)),out=[];for(let i=0;i<len;i++)out.push((seq+recordId+typeOrd+i)&255);return out}case 'uint8':return(seq+recordId+typeOrd)&255;case 'int8':return((seq+recordId+typeOrd)%127)-63;case 'int16':return((seq*31+recordId+typeOrd)%32767)-16384;case 'int32':return((seq*1009)+recordId+typeOrd)|0;case 'uint16':return(seq*17+recordId+typeOrd)&65535;case 'uint32':return((seq*4099)+recordId+typeOrd)>>>0;case 'isodate':return new Date(Date.UTC(2026,0,1+((seq+typeOrd)%28))).toISOString().slice(0,10);case 'isotime':return new Date(Date.UTC(2026,0,1,(seq+typeOrd)%24,(recordId+seq)%60,(seq*7)%60)).toISOString().slice(11,19);case 'isodatetime':return new Date(Date.UTC(2026,0,1+((seq+typeOrd)%28),(seq+typeOrd)%24,(recordId+seq)%60,(seq*7)%60)).toISOString().slice(0,19)+'Z';default:return window.guiCodec.clipText(base,24)}}"
"function buildSeedObject(typeDef,fields,seq,recordId){const obj={};for(const field of fields)obj[field.name]=makeFieldValue(field,seq,typeDef.ordinal,recordId);return obj}"
"async function loadMetadata(){setStatus('LOADING METADATA...');try{const [typesRes,fieldsRes]=await Promise.all([fetch('/meta/types',{headers:hdr()}),fetch('/meta/fields',{headers:hdr()})]);const types=await readJson(typesRes),fields=await readJson(fieldsRes);metaCache={types:types.types||[],fields:fields.fields||[]};normalizeMeta();$('meta').textContent=JSON.stringify(metaCache,null,2);setStatus('METADATA '+metaCache.types.length+' TYPES, '+metaCache.fields.length+' FIELDS')}catch(e){setStatus('METADATA ERROR '+e)}}"
"async function loadValue(){setStatus('LOADING...');try{if(!metaCache)await loadMetadata();const r=await fetch(path(),{headers:hdr()}),bytes=await readBytes(r),obj=window.guiCodec.decodeBinaryRecord(metaCache,bytes);if(obj){$('value').value=JSON.stringify(obj,null,2);setStatus('LOAD 200 BINARY '+bytes.length+'B')}else{$('value').value=window.guiCodec.utf8dec.decode(bytes);setStatus('LOAD 200 RAW '+bytes.length+'B')}}catch(e){setStatus('LOAD ERROR '+e)}}"
"async function saveValue(){setStatus('SAVING...');try{if(!metaCache)await loadMetadata();const obj=JSON.parse($('value').value),body=window.guiCodec.encodeBinaryRecord(metaCache,obj),r=await fetch(path(),{method:'POST',headers:hdr({'Content-Type':'application/octet-stream'}),body});setStatus('SAVE '+r.status+' '+await r.text())}catch(e){setStatus('SAVE ERROR '+e)}}"
"async function seedRecords(){if(!metaCache)await loadMetadata();if(!metaCache||!metaCache.types||metaCache.types.length===0){setStatus('SEED ERROR no metadata types');return}if(!metaCache.fields||metaCache.fields.length===0){setStatus('SEED ERROR no metadata fields');return}const total=Math.max(1,Math.min(parseInt($('seedCount').value||'5000',10),5000)),startId=Math.max(0,Math.min(parseInt($('seedStartId').value||'1',10),4194303)),batchSize=Math.max(1,Math.min(parseInt($('seedBatch').value||'8',10),32));let ok=0,fail=0;setStatus('SEEDING 0/'+total);for(let base=0;base<total;base+=batchSize){const jobs=[];for(let offset=0;offset<batchSize&&(base+offset)<total;offset++){const seq=base+offset,typeDef=metaCache.types[seq%metaCache.types.length],recordId=startId+seq,body=window.guiCodec.encodeBinaryRecord(metaCache,buildSeedObject(typeDef,metaCache.fields,seq,recordId));jobs.push(fetch('/0/'+typeDef.ordinal+'/'+recordId,{method:'POST',headers:hdr({'Content-Type':'application/octet-stream'}),body}).then(async r=>{if(!r.ok)throw new Error(await r.text());ok++}).catch(async()=>{fail++}))}await Promise.all(jobs);setStatus('SEEDING '+Math.min(base+batchSize,total)+'/'+total+' OK='+ok+' FAIL='+fail)}setStatus('SEED DONE OK='+ok+' FAIL='+fail)}"
"$('loadBtn').addEventListener('click',loadValue);$('saveBtn').addEventListener('click',saveValue);$('metaBtn').addEventListener('click',loadMetadata);$('seedBtn').addEventListener('click',seedRecords)})();";

static const char META_EDITOR_JS[] =
"(()=>{const $=id=>document.getElementById(id);const objId=(location.pathname.split('/').pop()||'0').trim();$('objId').value=objId;$('typeOrd').value=objId;$('psk').value=localStorage.getItem('psk')||'';"
"function hdr(){const p=$('psk').value.trim();localStorage.setItem('psk',p);return {'Authorization':'PSK '+p}}"
"function setStatus(t){$('status').textContent=t}"
"async function readText(r){const t=await r.text();if(!r.ok)throw new Error(r.status+' '+t);return t}"
"async function readJson(r){return JSON.parse(await readText(r))}"
"async function reload(){setStatus('LOADING METADATA...');try{const [typesRes,fieldsRes]=await Promise.all([fetch('/meta/types',{headers:hdr()}),fetch('/meta/fields',{headers:hdr()})]);const meta={types:(await readJson(typesRes)).types||[],fields:(await readJson(fieldsRes)).fields||[]};$('meta').textContent=JSON.stringify(meta,null,2);setStatus('METADATA LOADED')}catch(e){setStatus('LOAD ERROR '+e)}}"
"async function saveType(){setStatus('SAVING TYPE...');try{const r=await fetch('/meta/types/'+$('typeOrd').value.trim(),{method:'POST',headers:hdr(),body:$('typeName').value.trim()});setStatus('TYPE '+r.status+' '+await r.text());await reload()}catch(e){setStatus('TYPE ERROR '+e)}}"
"async function saveField(){setStatus('SAVING FIELD...');try{const body=$('fieldName').value.trim()+'|'+$('fieldType').value.trim()+'|'+$('fieldMaxLen').value.trim();const r=await fetch('/meta/fields/'+$('fieldOrd').value.trim(),{method:'POST',headers:hdr(),body});setStatus('FIELD '+r.status+' '+await r.text());await reload()}catch(e){setStatus('FIELD ERROR '+e)}}"
"$('reloadMetaBtn').addEventListener('click',reload);$('saveTypeBtn').addEventListener('click',saveType);$('saveFieldBtn').addEventListener('click',saveField)})();";

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

void web_server_set_psk(const uint8_t psk[32]) {
    memcpy(g_http_psk, psk, 32);
    g_http_psk_set = true;
}

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

static void http_root_stats(struct tcp_pcb *pcb) {
    kv_stats_t st = kv_stats();
    uint32_t records = kv_record_count();
    uint16_t types[8];
    uint32_t counts[8];
    uint32_t n_types = kv_type_counts(types, counts, 8);
    const char *ip_text = netif_list ? ip4addr_ntoa(netif_ip4_addr(netif_list)) : "0.0.0.0";
    uint32_t used_pages = st.total - st.free;
    uint32_t used_bytes = used_pages * KV_SECTOR_SIZE;
    uint32_t free_bytes = st.free * KV_SECTOR_SIZE;
    uint32_t usage_tenths = (st.total > 0) ? (used_pages * 1000u) / st.total : 0;

    char body[768];
    int n = snprintf(body, sizeof(body),
                     "STORAGE APPLIANCE\n"
                     "HTTP: %s:80\n"
                     "RECORDS: %lu\n"
                     "USED BYTES: %lu  FREE BYTES: %lu\n"
                     "USAGE: %lu.%lu%%  DEAD PAGES: %lu\n"
                     "REQUESTS: %lu\n"
                     "WRITES: %lu  READS: %lu\n"
                     "COMPACTIONS: %lu  RECLAIMED: %lu\n"
                     "TYPE     COUNT\n",
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
    if (n <= 0 || n >= (int)sizeof(body)) {
        http_json(pcb, "500 Internal Server Error", "{\"error\":\"root stats overflow\"}");
        return;
    }

    for (uint32_t i = 0; i < n_types && i < 8; i++) {
        int wrote = snprintf(body + n, sizeof(body) - (size_t)n,
                             "%-8u %lu\n",
                             (unsigned int)types[i],
                             (unsigned long)counts[i]);
        if (wrote <= 0 || wrote >= (int)(sizeof(body) - (size_t)n)) {
            http_json(pcb, "500 Internal Server Error", "{\"error\":\"root types overflow\"}");
            return;
        }
        n += wrote;
    }
    if (n_types == 0) {
        int wrote = snprintf(body + n, sizeof(body) - (size_t)n, "(EMPTY)\n");
        if (wrote <= 0 || wrote >= (int)(sizeof(body) - (size_t)n)) {
            http_json(pcb, "500 Internal Server Error", "{\"error\":\"root empty overflow\"}");
            return;
        }
        n += wrote;
    }

    (void)http_respond(pcb, "200 OK", "text/plain", (const uint8_t *)body, (uint16_t)n);
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

static void format_psk_hex(char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[g_http_psk[i] >> 4];
        out[i * 2 + 1] = hex[g_http_psk[i] & 0x0F];
    }
    out[64] = '\0';
}

static bool is_same_subnet(const struct tcp_pcb *pcb) {
    if (!netif_list) return false;
    if (!IP_IS_V4_VAL(pcb->remote_ip)) return false;

    const ip4_addr_t *local_ip = netif_ip4_addr(netif_list);
    const ip4_addr_t *netmask = netif_ip4_netmask(netif_list);
    const ip4_addr_t *remote_ip = ip_2_ip4(&pcb->remote_ip);
    uint32_t local_masked = ip4_addr_get_u32(local_ip) & ip4_addr_get_u32(netmask);
    uint32_t remote_masked = ip4_addr_get_u32(remote_ip) & ip4_addr_get_u32(netmask);
    return local_masked == remote_masked;
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
        uint8_t val[KV_MAX_VALUE];
        uint16_t len = KV_MAX_VALUE;
        if (!kv_get_copy(key, val, &len, NULL)) {
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
            if (!check_auth(req)) {
                http_json(pcb, "401 Unauthorized", "{\"error\":\"invalid PSK\"}");
                return;
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
            if (!check_auth(req)) {
                http_json(pcb, "401 Unauthorized", "{\"error\":\"invalid PSK\"}");
                return;
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

    // ---- Route: GET / and /index.html → appliance stats ----
    if (verb == VERB_GET && (strcmp(path, "/key") == 0 || strcmp(path, "/psk") == 0)) {
        if (!g_http_psk_set) {
            http_json(pcb, "503 Service Unavailable", "{\"error\":\"psk unavailable\"}");
            return;
        }
        if (!is_same_subnet(pcb)) {
            http_json(pcb, "403 Forbidden", "{\"error\":\"same subnet required\"}");
            return;
        }

        char psk_hex[65];
        format_psk_hex(psk_hex);
        http_respond(pcb, "200 OK", "text/plain", (const uint8_t *)psk_hex, 64);
        return;
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

    if (verb == VERB_GET && strcmp(path, "/gui") == 0) {
        (void)http_respond(pcb, "200 OK", "text/html", (const uint8_t *)GUI_HTML, sizeof(GUI_HTML) - 1);
        return;
    }

    if (verb == VERB_GET && strncmp(path, "/w/0/", 5) == 0) {
        (void)http_respond(pcb, "200 OK", "text/html", (const uint8_t *)META_EDITOR_HTML, sizeof(META_EDITOR_HTML) - 1);
        return;
    }

    if (verb == VERB_GET && strcmp(path, "/gui.css") == 0) {
        (void)http_respond(pcb, "200 OK", "text/css", (const uint8_t *)GUI_CSS, sizeof(GUI_CSS) - 1);
        return;
    }

    if (verb == VERB_GET && strcmp(path, "/gui_codec.js") == 0) {
        (void)http_respond(pcb, "200 OK", "application/javascript", (const uint8_t *)GUI_CODEC_JS, sizeof(GUI_CODEC_JS) - 1);
        return;
    }

    if (verb == VERB_GET && strcmp(path, "/gui_app.js") == 0) {
        (void)http_respond(pcb, "200 OK", "application/javascript", (const uint8_t *)GUI_APP_JS, sizeof(GUI_APP_JS) - 1);
        return;
    }

    if (verb == VERB_GET && strcmp(path, "/meta_editor.js") == 0) {
        (void)http_respond(pcb, "200 OK", "application/javascript", (const uint8_t *)META_EDITOR_JS, sizeof(META_EDITOR_JS) - 1);
        return;
    }

    if ((strncmp(path, "/meta/types", 11) == 0 || strncmp(path, "/meta/fields", 12) == 0) &&
        (verb == VERB_GET || verb == VERB_POST)) {
        handle_metadata(pcb, verb, path, body, body_len, req);
        return;
    }

    if (verb == VERB_GET && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        http_root_stats(pcb);
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
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, HTTP_PORT);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, http_accept);
    printf("[http] Listening on port %d\n", HTTP_PORT);
}
