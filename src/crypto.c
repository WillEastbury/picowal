#include "crypto.h"
#include "mbedtls/sha256.h"
#include <string.h>

// ============================================================
// ChaCha20 quarter-round and block function (RFC 8439)
// ============================================================

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define QR(a, b, c, d) do { \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d,  8); \
    c += d; b ^= c; b = ROTL32(b,  7); \
} while (0)

void chacha20_block(uint32_t out[16], const uint32_t key[8],
                    uint32_t counter, const uint32_t nonce[3]) {
    uint32_t s[16];
    // "expand 32-byte k"
    s[0]=0x61707865; s[1]=0x3320646e; s[2]=0x79622d32; s[3]=0x6b206574;
    s[4]=key[0]; s[5]=key[1]; s[6]=key[2]; s[7]=key[3];
    s[8]=key[4]; s[9]=key[5]; s[10]=key[6]; s[11]=key[7];
    s[12]=counter; s[13]=nonce[0]; s[14]=nonce[1]; s[15]=nonce[2];

    for (int i = 0; i < 16; i++) out[i] = s[i];

    for (int i = 0; i < 10; i++) {
        QR(out[0],out[4],out[ 8],out[12]);
        QR(out[1],out[5],out[ 9],out[13]);
        QR(out[2],out[6],out[10],out[14]);
        QR(out[3],out[7],out[11],out[15]);
        QR(out[0],out[5],out[10],out[15]);
        QR(out[1],out[6],out[11],out[12]);
        QR(out[2],out[7],out[ 8],out[13]);
        QR(out[3],out[4],out[ 9],out[14]);
    }
    for (int i = 0; i < 16; i++) out[i] += s[i];
}

void chacha20_encrypt(uint8_t *out, const uint8_t *in, uint32_t len,
                      const uint8_t key[32], uint32_t counter,
                      const uint8_t nonce[12]) {
    uint32_t k[8], n[3], block[16];
    memcpy(k, key, 32);
    memcpy(n, nonce, 12);

    for (uint32_t off = 0; off < len; off += 64) {
        chacha20_block(block, k, counter++, n);
        uint8_t *ks = (uint8_t *)block;
        uint32_t chunk = len - off;
        if (chunk > 64) chunk = 64;
        for (uint32_t i = 0; i < chunk; i++)
            out[off + i] = in[off + i] ^ ks[i];
    }
}

// ============================================================
// Poly1305 MAC (RFC 8439)
// ============================================================

// 130-bit arithmetic using 5 × 26-bit limbs
typedef struct { uint32_t h[5]; uint32_t r[5]; uint32_t pad[4]; } poly1305_ctx;

static void poly1305_init(poly1305_ctx *ctx, const uint8_t key[32]) {
    // r = key[0..15] clamped
    uint32_t t0 = key[0]|((uint32_t)key[1]<<8)|((uint32_t)key[2]<<16)|((uint32_t)key[3]<<24);
    uint32_t t1 = key[4]|((uint32_t)key[5]<<8)|((uint32_t)key[6]<<16)|((uint32_t)key[7]<<24);
    uint32_t t2 = key[8]|((uint32_t)key[9]<<8)|((uint32_t)key[10]<<16)|((uint32_t)key[11]<<24);
    uint32_t t3 = key[12]|((uint32_t)key[13]<<8)|((uint32_t)key[14]<<16)|((uint32_t)key[15]<<24);
    ctx->r[0] = t0 & 0x3ffffff;
    ctx->r[1] = ((t0>>26)|(t1<<6)) & 0x3ffff03;
    ctx->r[2] = ((t1>>20)|(t2<<12)) & 0x3ffc0ff;
    ctx->r[3] = ((t2>>14)|(t3<<18)) & 0x3f03fff;
    ctx->r[4] = (t3>>8) & 0x00fffff;
    // pad = key[16..31]
    ctx->pad[0] = key[16]|((uint32_t)key[17]<<8)|((uint32_t)key[18]<<16)|((uint32_t)key[19]<<24);
    ctx->pad[1] = key[20]|((uint32_t)key[21]<<8)|((uint32_t)key[22]<<16)|((uint32_t)key[23]<<24);
    ctx->pad[2] = key[24]|((uint32_t)key[25]<<8)|((uint32_t)key[26]<<16)|((uint32_t)key[27]<<24);
    ctx->pad[3] = key[28]|((uint32_t)key[29]<<8)|((uint32_t)key[30]<<16)|((uint32_t)key[31]<<24);
    memset(ctx->h, 0, sizeof(ctx->h));
}

static void poly1305_blocks(poly1305_ctx *ctx, const uint8_t *m, uint32_t len, uint8_t hibit) {
    uint32_t r0=ctx->r[0], r1=ctx->r[1], r2=ctx->r[2], r3=ctx->r[3], r4=ctx->r[4];
    uint32_t s1=r1*5, s2=r2*5, s3=r3*5, s4=r4*5;
    uint32_t h0=ctx->h[0], h1=ctx->h[1], h2=ctx->h[2], h3=ctx->h[3], h4=ctx->h[4];

    while (len >= 16) {
        uint32_t t0 = m[0]|((uint32_t)m[1]<<8)|((uint32_t)m[2]<<16)|((uint32_t)m[3]<<24);
        uint32_t t1 = m[4]|((uint32_t)m[5]<<8)|((uint32_t)m[6]<<16)|((uint32_t)m[7]<<24);
        uint32_t t2 = m[8]|((uint32_t)m[9]<<8)|((uint32_t)m[10]<<16)|((uint32_t)m[11]<<24);
        uint32_t t3 = m[12]|((uint32_t)m[13]<<8)|((uint32_t)m[14]<<16)|((uint32_t)m[15]<<24);

        h0 += t0 & 0x3ffffff;
        h1 += ((t0>>26)|(t1<<6)) & 0x3ffffff;
        h2 += ((t1>>20)|(t2<<12)) & 0x3ffffff;
        h3 += ((t2>>14)|(t3<<18)) & 0x3ffffff;
        h4 += (t3>>8) | ((uint32_t)hibit<<24);

        uint64_t d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        uint64_t d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        uint64_t d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        uint64_t d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        uint64_t d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;

        uint32_t c;
        c = (uint32_t)(d0>>26); h0=(uint32_t)d0&0x3ffffff; d1+=c;
        c = (uint32_t)(d1>>26); h1=(uint32_t)d1&0x3ffffff; d2+=c;
        c = (uint32_t)(d2>>26); h2=(uint32_t)d2&0x3ffffff; d3+=c;
        c = (uint32_t)(d3>>26); h3=(uint32_t)d3&0x3ffffff; d4+=c;
        c = (uint32_t)(d4>>26); h4=(uint32_t)d4&0x3ffffff; h0+=c*5;
        c = h0>>26; h0 &= 0x3ffffff; h1+=c;

        m += 16; len -= 16;
    }
    ctx->h[0]=h0; ctx->h[1]=h1; ctx->h[2]=h2; ctx->h[3]=h3; ctx->h[4]=h4;
}

static void poly1305_finish(poly1305_ctx *ctx, uint8_t tag[16]) {
    uint32_t h0=ctx->h[0], h1=ctx->h[1], h2=ctx->h[2], h3=ctx->h[3], h4=ctx->h[4];
    uint32_t c;
    c=h1>>26; h1&=0x3ffffff; h2+=c;
    c=h2>>26; h2&=0x3ffffff; h3+=c;
    c=h3>>26; h3&=0x3ffffff; h4+=c;
    c=h4>>26; h4&=0x3ffffff; h0+=c*5;
    c=h0>>26; h0&=0x3ffffff; h1+=c;

    uint32_t g0=h0+5; c=g0>>26; g0&=0x3ffffff;
    uint32_t g1=h1+c; c=g1>>26; g1&=0x3ffffff;
    uint32_t g2=h2+c; c=g2>>26; g2&=0x3ffffff;
    uint32_t g3=h3+c; c=g3>>26; g3&=0x3ffffff;
    uint32_t g4=h4+c-(1u<<26);

    uint32_t mask = (g4>>31)-1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0&mask)|g0; h1 = (h1&mask)|g1; h2 = (h2&mask)|g2;
    h3 = (h3&mask)|g3; h4 = (h4&mask)|g4;

    uint64_t f;
    f = (uint64_t)(h0|(h1<<26)) + ctx->pad[0]; uint32_t r0 = (uint32_t)f;
    f = (uint64_t)(h1>>6|(h2<<20)) + ctx->pad[1] + (f>>32); uint32_t r1 = (uint32_t)f;
    f = (uint64_t)(h2>>12|(h3<<14)) + ctx->pad[2] + (f>>32); uint32_t r2 = (uint32_t)f;
    f = (uint64_t)(h3>>18|(h4<<8)) + ctx->pad[3] + (f>>32); uint32_t r3 = (uint32_t)f;

    tag[0]=(uint8_t)r0; tag[1]=(uint8_t)(r0>>8); tag[2]=(uint8_t)(r0>>16); tag[3]=(uint8_t)(r0>>24);
    tag[4]=(uint8_t)r1; tag[5]=(uint8_t)(r1>>8); tag[6]=(uint8_t)(r1>>16); tag[7]=(uint8_t)(r1>>24);
    tag[8]=(uint8_t)r2; tag[9]=(uint8_t)(r2>>8); tag[10]=(uint8_t)(r2>>16); tag[11]=(uint8_t)(r2>>24);
    tag[12]=(uint8_t)r3; tag[13]=(uint8_t)(r3>>8); tag[14]=(uint8_t)(r3>>16); tag[15]=(uint8_t)(r3>>24);
}

void poly1305_mac(uint8_t tag[16], const uint8_t *msg, uint32_t msg_len,
                  const uint8_t key[32]) {
    poly1305_ctx ctx;
    poly1305_init(&ctx, key);
    poly1305_blocks(&ctx, msg, msg_len & ~15u, 1);
    if (msg_len & 15) {
        uint8_t pad[16];
        memset(pad, 0, 16);
        memcpy(pad, msg + (msg_len & ~15u), msg_len & 15);
        pad[msg_len & 15] = 1;
        poly1305_blocks(&ctx, pad, 16, 0);
    }
    poly1305_finish(&ctx, tag);
}

// ============================================================
// ChaCha20-Poly1305 AEAD (RFC 8439)
// ============================================================

static void pad16(uint8_t *buf, uint32_t *pos, uint32_t len) {
    uint32_t rem = len & 15;
    if (rem) {
        uint8_t z[16]; memset(z, 0, 16);
        memcpy(buf + *pos, z, 16 - rem);
        *pos += 16 - rem;
    }
}

uint32_t aead_encrypt(uint8_t *out,
                      const uint8_t *plaintext, uint32_t pt_len,
                      const uint8_t *aad, uint32_t aad_len,
                      const uint8_t key[32], const uint8_t nonce[12]) {
    // Generate Poly1305 key from ChaCha20 block 0
    uint8_t poly_key[32];
    uint8_t zeros[32]; memset(zeros, 0, 32);
    chacha20_encrypt(poly_key, zeros, 32, key, 0, nonce);

    // Encrypt plaintext with ChaCha20 (counter starts at 1)
    chacha20_encrypt(out, plaintext, pt_len, key, 1, nonce);

    // Construct Poly1305 input: aad || pad || ciphertext || pad || lengths
    // We compute MAC incrementally
    poly1305_ctx ctx;
    poly1305_init(&ctx, poly_key);

    // MAC aad
    if (aad_len > 0) {
        poly1305_blocks(&ctx, aad, aad_len & ~15u, 1);
        if (aad_len & 15) {
            uint8_t p[16]; memset(p, 0, 16);
            memcpy(p, aad + (aad_len & ~15u), aad_len & 15);
            poly1305_blocks(&ctx, p, 16, 1);
        }
    }

    // MAC ciphertext
    poly1305_blocks(&ctx, out, pt_len & ~15u, 1);
    if (pt_len & 15) {
        uint8_t p[16]; memset(p, 0, 16);
        memcpy(p, out + (pt_len & ~15u), pt_len & 15);
        poly1305_blocks(&ctx, p, 16, 1);
    }

    // MAC lengths (aad_len || pt_len as little-endian uint64)
    uint8_t lens[16];
    memset(lens, 0, 16);
    lens[0]=(uint8_t)aad_len; lens[1]=(uint8_t)(aad_len>>8);
    lens[2]=(uint8_t)(aad_len>>16); lens[3]=(uint8_t)(aad_len>>24);
    lens[8]=(uint8_t)pt_len; lens[9]=(uint8_t)(pt_len>>8);
    lens[10]=(uint8_t)(pt_len>>16); lens[11]=(uint8_t)(pt_len>>24);
    poly1305_blocks(&ctx, lens, 16, 1);

    // Append tag
    poly1305_finish(&ctx, out + pt_len);
    return pt_len + 16;
}

uint32_t aead_decrypt(uint8_t *out,
                      const uint8_t *ciphertext, uint32_t ct_len,
                      const uint8_t *aad, uint32_t aad_len,
                      const uint8_t key[32], const uint8_t nonce[12]) {
    if (ct_len < 16) return 0;
    uint32_t pt_len = ct_len - 16;
    const uint8_t *tag = ciphertext + pt_len;

    // Generate Poly1305 key
    uint8_t poly_key[32];
    uint8_t zeros[32]; memset(zeros, 0, 32);
    chacha20_encrypt(poly_key, zeros, 32, key, 0, nonce);

    // Verify tag
    poly1305_ctx ctx;
    poly1305_init(&ctx, poly_key);

    if (aad_len > 0) {
        poly1305_blocks(&ctx, aad, aad_len & ~15u, 1);
        if (aad_len & 15) {
            uint8_t p[16]; memset(p, 0, 16);
            memcpy(p, aad + (aad_len & ~15u), aad_len & 15);
            poly1305_blocks(&ctx, p, 16, 1);
        }
    }

    poly1305_blocks(&ctx, ciphertext, pt_len & ~15u, 1);
    if (pt_len & 15) {
        uint8_t p[16]; memset(p, 0, 16);
        memcpy(p, ciphertext + (pt_len & ~15u), pt_len & 15);
        poly1305_blocks(&ctx, p, 16, 1);
    }

    uint8_t lens[16]; memset(lens, 0, 16);
    lens[0]=(uint8_t)aad_len; lens[1]=(uint8_t)(aad_len>>8);
    lens[2]=(uint8_t)(aad_len>>16); lens[3]=(uint8_t)(aad_len>>24);
    lens[8]=(uint8_t)pt_len; lens[9]=(uint8_t)(pt_len>>8);
    lens[10]=(uint8_t)(pt_len>>16); lens[11]=(uint8_t)(pt_len>>24);
    poly1305_blocks(&ctx, lens, 16, 1);

    uint8_t computed_tag[16];
    poly1305_finish(&ctx, computed_tag);

    // Constant-time compare
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= computed_tag[i] ^ tag[i];
    if (diff) return 0;

    // Decrypt
    chacha20_encrypt(out, ciphertext, pt_len, key, 1, nonce);
    return pt_len;
}

// ============================================================
// HKDF-SHA256 (RFC 5869) — uses mbedTLS SHA-256
// ============================================================

static void hmac_sha256(uint8_t out[32],
                        const uint8_t *key, uint32_t key_len,
                        const uint8_t *msg, uint32_t msg_len) {
    uint8_t ipad[64], opad[64];
    uint8_t kbuf[32];

    if (key_len > 64) {
        mbedtls_sha256_context c;
        mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
        mbedtls_sha256_update(&c, key, key_len);
        mbedtls_sha256_finish(&c, kbuf); mbedtls_sha256_free(&c);
        key = kbuf; key_len = 32;
    }

    memset(ipad, 0x36, 64); memset(opad, 0x5c, 64);
    for (uint32_t i = 0; i < key_len; i++) { ipad[i] ^= key[i]; opad[i] ^= key[i]; }

    mbedtls_sha256_context c;
    uint8_t inner[32];
    mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, ipad, 64);
    mbedtls_sha256_update(&c, msg, msg_len);
    mbedtls_sha256_finish(&c, inner); mbedtls_sha256_free(&c);

    mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, opad, 64);
    mbedtls_sha256_update(&c, inner, 32);
    mbedtls_sha256_finish(&c, out); mbedtls_sha256_free(&c);
}

void hkdf_sha256(uint8_t *out, uint32_t out_len,
                 const uint8_t *ikm, uint32_t ikm_len,
                 const uint8_t *salt, uint32_t salt_len,
                 const uint8_t *info, uint32_t info_len) {
    // Extract
    uint8_t prk[32];
    if (!salt || salt_len == 0) {
        uint8_t z[32]; memset(z, 0, 32);
        hmac_sha256(prk, z, 32, ikm, ikm_len);
    } else {
        hmac_sha256(prk, salt, salt_len, ikm, ikm_len);
    }

    // Expand
    uint8_t t[32]; uint32_t t_len = 0;
    uint8_t counter = 1;
    uint32_t pos = 0;
    while (pos < out_len) {
        uint8_t buf[32 + 256 + 1]; // t || info || counter
        uint32_t blen = 0;
        memcpy(buf, t, t_len); blen += t_len;
        if (info_len > 0 && info_len <= 256) { memcpy(buf + blen, info, info_len); blen += info_len; }
        buf[blen++] = counter++;
        hmac_sha256(t, prk, 32, buf, blen);
        t_len = 32;
        uint32_t chunk = out_len - pos;
        if (chunk > 32) chunk = 32;
        memcpy(out + pos, t, chunk);
        pos += chunk;
    }
}
