// picowal.js — Binary codec + KnockoutJS binding library for PicoWAL FPGA KV engine
// All data over the wire is raw binary (ArrayBuffer), never JSON.
// Depends on: KnockoutJS (ko global)

var PicoWAL = (function() {
  "use strict";

  // ── Field Type Constants ──
  var T = {
    UINT8:0x01, UINT16:0x02, UINT32:0x03,
    INT8:0x04, INT16:0x05, INT32:0x06,
    BOOL:0x07, ASCII:0x08, UTF8:0x09,
    DATE:0x0A, TIME:0x0B, DATETIME:0x0C,
    IPV4:0x0D, MAC:0x0E, ENUM:0x0F,
    ARRAY_U16:0x10, BLOB:0x11
  };

  // Type names for UI labels
  var TYPE_NAMES = {};
  TYPE_NAMES[0x01]='uint8'; TYPE_NAMES[0x02]='uint16'; TYPE_NAMES[0x03]='uint32';
  TYPE_NAMES[0x04]='int8'; TYPE_NAMES[0x05]='int16'; TYPE_NAMES[0x06]='int32';
  TYPE_NAMES[0x07]='bool'; TYPE_NAMES[0x08]='ascii'; TYPE_NAMES[0x09]='utf8';
  TYPE_NAMES[0x0A]='date'; TYPE_NAMES[0x0B]='time'; TYPE_NAMES[0x0C]='datetime';
  TYPE_NAMES[0x0D]='ipv4'; TYPE_NAMES[0x0E]='mac'; TYPE_NAMES[0x0F]='enum';
  TYPE_NAMES[0x10]='array_u16'; TYPE_NAMES[0x11]='blob';

  // Fixed sizes by type (length-prefixed types not listed here)
  var FIXED = {};
  FIXED[0x01]=1; FIXED[0x02]=2; FIXED[0x03]=4;
  FIXED[0x04]=1; FIXED[0x05]=2; FIXED[0x06]=4;
  FIXED[0x07]=1; FIXED[0x0A]=4; FIXED[0x0B]=3;
  FIXED[0x0C]=7; FIXED[0x0D]=4; FIXED[0x0E]=6; FIXED[0x0F]=1;

  var CARD_MAGIC = 0xCA7D;

  // ── Decode a single field value from DataView at offset ──
  function decodeValue(type, dv, off, len) {
    switch(type) {
      case 0x01: return dv.getUint8(off);
      case 0x02: return dv.getUint16(off, true);
      case 0x03: return dv.getUint32(off, true);
      case 0x04: return dv.getInt8(off);
      case 0x05: return dv.getInt16(off, true);
      case 0x06: return dv.getInt32(off, true);
      case 0x07: return dv.getUint8(off) !== 0;
      case 0x08: case 0x09: {
        var plen = dv.getUint8(off);
        var bytes = new Uint8Array(dv.buffer, dv.byteOffset + off + 1, plen);
        return String.fromCharCode.apply(null, bytes);
      }
      case 0x0A: return {
        y: dv.getUint16(off, true), m: dv.getUint8(off + 2), d: dv.getUint8(off + 3)
      };
      case 0x0B: return {
        h: dv.getUint8(off), m: dv.getUint8(off + 1), s: dv.getUint8(off + 2)
      };
      case 0x0C: return {
        y: dv.getUint16(off, true), mo: dv.getUint8(off + 2), d: dv.getUint8(off + 3),
        h: dv.getUint8(off + 4), mi: dv.getUint8(off + 5), s: dv.getUint8(off + 6)
      };
      case 0x0D:
        return dv.getUint8(off) + '.' + dv.getUint8(off + 1) + '.' +
               dv.getUint8(off + 2) + '.' + dv.getUint8(off + 3);
      case 0x0E:
        return Array.from(new Uint8Array(dv.buffer, dv.byteOffset + off, 6),
          function(b) { return ('0' + b.toString(16)).slice(-2); }).join(':');
      case 0x0F: return dv.getUint8(off);
      case 0x10: {
        var plen = dv.getUint8(off), arr = [];
        for (var i = 0; i < plen; i += 2) arr.push(dv.getUint16(off + 1 + i, true));
        return arr;
      }
      case 0x11: {
        var plen = dv.getUint8(off);
        return Array.from(new Uint8Array(dv.buffer, dv.byteOffset + off + 1, plen));
      }
      default:
        return Array.from(new Uint8Array(dv.buffer, dv.byteOffset + off, len));
    }
  }

  // ── Encode a single field value into byte array ──
  function encodeValue(type, val) {
    var buf, dv;
    switch(type) {
      case 0x01: return [val & 0xFF];
      case 0x02:
        buf = new ArrayBuffer(2); new DataView(buf).setUint16(0, val, true);
        return Array.from(new Uint8Array(buf));
      case 0x03:
        buf = new ArrayBuffer(4); new DataView(buf).setUint32(0, val, true);
        return Array.from(new Uint8Array(buf));
      case 0x04: return [(val < 0 ? val + 256 : val) & 0xFF];
      case 0x05:
        buf = new ArrayBuffer(2); new DataView(buf).setInt16(0, val, true);
        return Array.from(new Uint8Array(buf));
      case 0x06:
        buf = new ArrayBuffer(4); new DataView(buf).setInt32(0, val, true);
        return Array.from(new Uint8Array(buf));
      case 0x07: return [val ? 1 : 0];
      case 0x08: case 0x09: {
        var bytes = [];
        for (var i = 0; i < val.length; i++) bytes.push(val.charCodeAt(i));
        return [bytes.length].concat(bytes);
      }
      case 0x0A:
        buf = new ArrayBuffer(4); dv = new DataView(buf);
        dv.setUint16(0, val.y, true); dv.setUint8(2, val.m); dv.setUint8(3, val.d);
        return Array.from(new Uint8Array(buf));
      case 0x0B: return [val.h, val.m, val.s];
      case 0x0C:
        buf = new ArrayBuffer(7); dv = new DataView(buf);
        dv.setUint16(0, val.y, true); dv.setUint8(2, val.mo); dv.setUint8(3, val.d);
        dv.setUint8(4, val.h); dv.setUint8(5, val.mi); dv.setUint8(6, val.s);
        return Array.from(new Uint8Array(buf));
      case 0x0D:
        return val.split('.').map(Number);
      case 0x0E:
        return val.split(':').map(function(h) { return parseInt(h, 16); });
      case 0x0F: return [val & 0xFF];
      case 0x10: {
        var bytes = [val.length * 2];
        val.forEach(function(v) { bytes.push(v & 0xFF, (v >> 8) & 0xFF); });
        return bytes;
      }
      case 0x11:
        return [val.length].concat(val);
      default: return Array.isArray(val) ? val : [val];
    }
  }

  // ── Parse card binary (ArrayBuffer) → {magic, version, fields} ──
  function parseCard(ab) {
    var dv = new DataView(ab);
    if (ab.byteLength < 4) return null;
    var magic = dv.getUint16(0, true);
    if (magic !== CARD_MAGIC) return null;
    var version = dv.getUint16(2, true);
    var fields = {};
    var off = 4;
    while (off + 1 < ab.byteLength) {
      var ordByte = dv.getUint8(off);
      var len = dv.getUint8(off + 1);
      if (ordByte === 0 && len === 0 && off > 4) break;
      var ord = ordByte & 0x1F;
      var flags = (ordByte >> 5) & 0x07;
      if (off + 2 + len > ab.byteLength) break;
      fields[ord] = { flags: flags, data: new DataView(ab, off + 2, len) };
      off += 2 + len;
    }
    return { magic: magic, version: version, fields: fields };
  }

  // ── Build delta binary for PUT (only changed fields) ──
  function buildDelta(version, fieldArray) {
    var buf = new Uint8Array(2048);
    buf[0] = 0x7D; buf[1] = 0xCA;
    buf[2] = version & 0xFF; buf[3] = (version >> 8) & 0xFF;
    var off = 4;
    fieldArray.sort(function(a, b) { return a.ord - b.ord; });
    fieldArray.forEach(function(f) {
      buf[off] = f.ord & 0x1F;
      buf[off + 1] = f.data.length;
      buf.set(f.data, off + 2);
      off += 2 + f.data.length;
    });
    return buf.slice(0, off).buffer;
  }

  // ── Build full card binary (all fields, zero-padded to 2KB) ──
  function buildCard(version, fieldArray) {
    var buf = new Uint8Array(2048);
    buf[0] = 0x7D; buf[1] = 0xCA;
    buf[2] = version & 0xFF; buf[3] = (version >> 8) & 0xFF;
    var off = 4;
    fieldArray.sort(function(a, b) { return a.ord - b.ord; });
    fieldArray.forEach(function(f) {
      buf[off] = f.ord & 0x1F;
      buf[off + 1] = f.data.length;
      buf.set(f.data, off + 2);
      off += 2 + f.data.length;
    });
    return buf.buffer;
  }

  // ── Parse metadata field_defs (ordinal 2 of a Pack 0 card) ──
  function parseFieldDefs(data, count) {
    var defs = [];
    for (var i = 0; i < count; i++) {
      defs.push({
        ord:    data.getUint8(i * 3) & 0x1F,
        flags:  (data.getUint8(i * 3) >> 5) & 0x07,
        type:   data.getUint8(i * 3 + 1),
        maxlen: data.getUint8(i * 3 + 2)
      });
    }
    return defs;
  }

  // ── Parse null-separated field names ──
  function parseFieldNames(dv) {
    var bytes = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
    return String.fromCharCode.apply(null, bytes).split('\0').filter(Boolean);
  }

  // ── HTML input type for a field type code ──
  function inputType(fieldType) {
    switch(fieldType) {
      case 0x07: return 'checkbox';
      case 0x08: case 0x09: return 'text';
      case 0x01: case 0x02: case 0x03:
      case 0x04: case 0x05: case 0x06: return 'number';
      case 0x0A: return 'date';
      case 0x0B: return 'time';
      case 0x0C: return 'datetime-local';
      case 0x0D: return 'text'; // IP address as text
      case 0x0E: return 'text'; // MAC as text
      case 0x0F: return 'number'; // enum index
      case 0x10: return 'text'; // array_u16 as comma-separated
      case 0x11: return 'password'; // blob hidden by default
      default: return 'text';
    }
  }

  // ── Format a value for display in an input field ──
  function formatForInput(type, val) {
    if (val === null || val === undefined) return '';
    switch(type) {
      case 0x07: return val;
      case 0x0A: return val.y + '-' + ('0'+val.m).slice(-2) + '-' + ('0'+val.d).slice(-2);
      case 0x0B: return ('0'+val.h).slice(-2) + ':' + ('0'+val.m).slice(-2) + ':' + ('0'+val.s).slice(-2);
      case 0x0C: return val.y + '-' + ('0'+val.mo).slice(-2) + '-' + ('0'+val.d).slice(-2) +
                        'T' + ('0'+val.h).slice(-2) + ':' + ('0'+val.mi).slice(-2);
      case 0x10: return val.map(function(v) { return v === 0xFFFF ? 'ALL' : v; }).join(', ');
      case 0x11: return '(' + val.length + ' bytes)';
      default: return String(val);
    }
  }

  // ── Parse a display string back to a typed value ──
  function parseFromInput(type, str) {
    switch(type) {
      case 0x01: case 0x02: case 0x03: case 0x0F: return parseInt(str, 10) || 0;
      case 0x04: case 0x05: case 0x06: return parseInt(str, 10) || 0;
      case 0x07: return str === true || str === 'true' || str === '1';
      case 0x08: case 0x09: return str;
      case 0x0A: { var p = str.split('-'); return {y:+p[0], m:+p[1], d:+p[2]}; }
      case 0x0B: { var p = str.split(':'); return {h:+p[0], m:+p[1], s:+p[2]||0}; }
      case 0x0C: {
        var dt = str.split('T'), d = dt[0].split('-'), t = (dt[1]||'').split(':');
        return {y:+d[0], mo:+d[1], d:+d[2], h:+t[0]||0, mi:+t[1]||0, s:+t[2]||0};
      }
      case 0x0D: return str;
      case 0x0E: return str;
      case 0x10:
        return str.split(',').map(function(s) {
          s = s.trim(); return s === 'ALL' ? 0xFFFF : parseInt(s, 10);
        }).filter(function(v) { return !isNaN(v); });
      case 0x11: return []; // blob not editable from text
      default: return str;
    }
  }

  // ── Session-aware fetch wrapper ──
  var _onSessionExpired = null;

  function onSessionExpired(handler) {
    _onSessionExpired = handler;
  }

  function apiFetch(method, url, body) {
    var opts = { method: method, credentials: 'same-origin' };
    if (body) opts.body = body;
    return fetch(url, opts).then(function(r) {
      if (r.status === 401 && _onSessionExpired) {
        _onSessionExpired();
      }
      return r;
    });
  }

  // ── Login → returns session info ──
  function login(username, password, callback) {
    var body = new Uint8Array(64);
    body[0] = username.length;
    for (var i = 0; i < username.length && i < 31; i++) body[1 + i] = username.charCodeAt(i);
    body[32] = password.length;
    for (var i = 0; i < password.length && i < 31; i++) body[33 + i] = password.charCodeAt(i);

    fetch('/login', {
      method: 'POST',
      body: body.buffer,
      credentials: 'same-origin'
    }).then(function(r) {
      if (!r.ok) return callback(null, r.status);
      return r.arrayBuffer().then(function(ab) {
        var userCard = new DataView(ab).getUint32(0, true);
        callback({ userCard: userCard }, 0);
      });
    }).catch(function() { callback(null, -1); });
  }

  // ── Load own user profile + build navigation from ACLs ──
  function loadProfile(userCard, callback) {
    loadCard(1, userCard, function(vm) {
      if (!vm) return callback(null);

      var profile = { userCard: userCard, username: '', packs: [] };

      vm.props().forEach(function(p) {
        if (p.ord === 0) profile.username = p.rawVal || '';
        if (p.ord === 5) profile.readPacks = p.rawVal || [];
        if (p.ord === 6) profile.writePacks = p.rawVal || [];
        if (p.ord === 7) profile.deletePacks = p.rawVal || [];
      });

      profile.isAdmin = (profile.readPacks || []).indexOf(0xFFFF) >= 0;

      var packOrds = profile.isAdmin
        ? [0,1,2,3,4,5,6,7,8,9,10]
        : profile.readPacks.filter(function(p) { return p !== 0xFFFF; });

      var remaining = packOrds.length;
      if (remaining === 0) return callback(profile);

      profile.packs = [];
      packOrds.forEach(function(packOrd) {
        loadMeta(packOrd, function(schema) {
          if (schema) {
            profile.packs.push({
              ord: packOrd,
              name: schema.packName,
              canRead: profile.isAdmin || (profile.readPacks||[]).indexOf(packOrd) >= 0,
              canWrite: profile.isAdmin || (profile.writePacks||[]).indexOf(packOrd) >= 0,
              canDelete: profile.isAdmin || (profile.deletePacks||[]).indexOf(packOrd) >= 0
            });
          }
          remaining--;
          if (remaining <= 0) {
            profile.packs.sort(function(a,b) { return a.ord - b.ord; });
            callback(profile);
          }
        });
      });
    });
  }

  // ── Logout ──
  function logout(callback) {
    fetch('/logout', { method: 'POST', credentials: 'same-origin' })
    .then(function() {
      metaCache = {};
      callback && callback();
    });
  }

  // ── Password change (FPGA hashes internally) ──
  function changePassword(userCard, oldPass, newPass, callback) {
    var body = new Uint8Array(64);
    body[0] = oldPass.length;
    for (var i = 0; i < oldPass.length && i < 31; i++) body[1 + i] = oldPass.charCodeAt(i);
    body[32] = newPass.length;
    for (var i = 0; i < newPass.length && i < 31; i++) body[33 + i] = newPass.charCodeAt(i);

    apiFetch('POST', '/0/1/' + userCard + '/_passwd', body.buffer)
    .then(function(r) { callback(r.ok, r.status); });
  }
  var metaCache = {};

  // ── Load pack metadata (schema) — cached ──
  function loadMeta(pack, callback) {
    if (metaCache[pack]) return callback(metaCache[pack]);

    fetch('/0/0/' + pack, { credentials: 'same-origin' })
    .then(function(r) {
      if (!r.ok) return callback(null);
      return r.arrayBuffer();
    })
    .then(function(metaAB) {
      if (!metaAB) return;
      var meta = parseCard(metaAB);
      if (!meta) return callback(null);

      var fieldCount = decodeValue(T.UINT8, meta.fields[1].data, 0, 1);
      var fieldDefs  = parseFieldDefs(meta.fields[2].data, fieldCount);
      var fieldNames = parseFieldNames(meta.fields[5].data);
      var packName   = decodeValue(T.ASCII, meta.fields[0].data, 0);

      var schema = {
        pack:       pack,
        packName:   packName,
        fieldCount: fieldCount,
        fieldDefs:  fieldDefs,
        fieldNames: fieldNames
      };

      metaCache[pack] = schema;
      callback(schema);
    });
  }

  // ── Invalidate metadata cache (after schema change) ──
  function clearMeta(pack) {
    if (pack !== undefined) delete metaCache[pack];
    else metaCache = {};
  }

  // ── Load card using pre-loaded or cached metadata ──
  function loadCard(pack, card, callback) {
    loadMeta(pack, function(schema) {
      if (!schema) return callback(null);

      fetch('/0/' + pack + '/' + card, { credentials: 'same-origin' })
      .then(function(r) {
        if (r.status === 404) return null;
        return r.arrayBuffer();
      })
      .then(function(cardAB) {
        var cardData = cardAB ? parseCard(cardAB) : null;
        var version = cardData ? cardData.version : 0;

        var props = schema.fieldDefs.map(function(def, i) {
          var rawVal = cardData && cardData.fields[def.ord]
            ? decodeValue(def.type, cardData.fields[def.ord].data, 0,
                          cardData.fields[def.ord].data.byteLength)
            : null;

          var displayVal = formatForInput(def.type, rawVal);
          var obs = ko.observable(displayVal);
          var dirty = ko.observable(false);
          obs.subscribe(function() { dirty(true); });

          return {
            ord:     def.ord,
            name:    schema.fieldNames[i] || ('field_' + def.ord),
            type:    def.type,
            typeName: TYPE_NAMES[def.type] || 'unknown',
            maxlen:  def.maxlen,
            input:   inputType(def.type),
            val:     obs,
            dirty:   dirty,
            rawVal:  rawVal
          };
        });

        callback({
          pack:     pack,
          card:     card,
          packName: schema.packName,
          version:  version,
          isNew:    !cardData,
          props:    ko.observableArray(props)
        });
      });
    });
  }

  // ── Save only dirty fields as binary delta PUT ──
  function saveCard(vm, callback) {
    var dirty = vm.props().filter(function(p) { return p.dirty(); });
    if (!dirty.length) return callback && callback(true);

    var fields = dirty.map(function(p) {
      var typedVal = parseFromInput(p.type, p.val());
      return { ord: p.ord, data: new Uint8Array(encodeValue(p.type, typedVal)) };
    });

    var buf = buildDelta(vm.version + 1, fields);

    fetch('/0/' + vm.pack + '/' + vm.card, {
      method: 'PUT',
      body: buf,
      credentials: 'same-origin'
    }).then(function(r) {
      if (r.ok) {
        dirty.forEach(function(p) { p.dirty(false); });
        vm.version++;
      }
      callback && callback(r.ok);
    });
  }

  // ── Load list of cards in a pack ──
  function listCards(pack, start, limit, callback) {
    var url = '/0/' + pack + '?start=' + (start || 0) + '&limit=' + (limit || 50);
    fetch(url, { credentials: 'same-origin' })
    .then(function(r) { return r.arrayBuffer(); })
    .then(function(ab) {
      var dv = new DataView(ab);
      var items = [], off = 0;
      while (off + 5 < ab.byteLength) {
        var cardOrd = dv.getUint32(off, true);
        if (cardOrd === 0xFFFFFFFF) break;
        var payloadLen = dv.getUint16(off + 4, true);
        var payload = ab.slice(off + 6, off + 6 + payloadLen);
        items.push({ id: cardOrd, data: parseCard(payload) });
        off += 6 + payloadLen;
      }
      callback(items);
    });
  }

  // ── Multi-get specific cards ──
  function mgetCards(pack, cardOrdinals, callback) {
    var buf = new ArrayBuffer(cardOrdinals.length * 4);
    var dv = new DataView(buf);
    cardOrdinals.forEach(function(id, i) { dv.setUint32(i * 4, id, true); });

    fetch('/0/' + pack + '/_mget', {
      method: 'POST',
      body: buf,
      credentials: 'same-origin'
    })
    .then(function(r) { return r.arrayBuffer(); })
    .then(function(ab) {
      var dv = new DataView(ab);
      var items = [], off = 0;
      while (off + 5 < ab.byteLength) {
        var cardOrd = dv.getUint32(off, true);
        if (cardOrd === 0xFFFFFFFF) break;
        var payloadLen = dv.getUint16(off + 4, true);
        var payload = ab.slice(off + 6, off + 6 + payloadLen);
        items.push({ id: cardOrd, data: parseCard(payload) });
        off += 6 + payloadLen;
      }
      callback(items);
    });
  }

  // Public API
  return {
    T: T,
    TYPE_NAMES: TYPE_NAMES,
    CARD_MAGIC: CARD_MAGIC,
    parseCard: parseCard,
    buildCard: buildCard,
    buildDelta: buildDelta,
    decodeValue: decodeValue,
    encodeValue: encodeValue,
    parseFieldDefs: parseFieldDefs,
    parseFieldNames: parseFieldNames,
    inputType: inputType,
    formatForInput: formatForInput,
    parseFromInput: parseFromInput,
    onSessionExpired: onSessionExpired,
    apiFetch: apiFetch,
    login: login,
    logout: logout,
    loadProfile: loadProfile,
    changePassword: changePassword,
    loadMeta: loadMeta,
    clearMeta: clearMeta,
    loadCard: loadCard,
    saveCard: saveCard,
    listCards: listCards,
    mgetCards: mgetCards
  };
})();
