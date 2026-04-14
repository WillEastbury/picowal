#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// ChaCha20-Poly1305 AEAD — standalone, no mbedTLS dependency
// Used for UDP WAL session encryption
// ============================================================

// ChaCha20 stream cipher
void chacha20_block(uint32_t out[16], const uint32_t key[8],
                    uint32_t counter, const uint32_t nonce[3]);
void chacha20_encrypt(uint8_t *out, const uint8_t *in, uint32_t len,
                      const uint8_t key[32], uint32_t counter,
                      const uint8_t nonce[12]);

// Poly1305 MAC
void poly1305_mac(uint8_t tag[16], const uint8_t *msg, uint32_t msg_len,
                  const uint8_t key[32]);

// ChaCha20-Poly1305 AEAD
// Encrypts plaintext, appends 16-byte auth tag.
// aad = additional authenticated data (e.g. header), not encrypted.
// Returns ciphertext_len + 16 (tag).
uint32_t aead_encrypt(uint8_t *out,
                      const uint8_t *plaintext, uint32_t pt_len,
                      const uint8_t *aad, uint32_t aad_len,
                      const uint8_t key[32], const uint8_t nonce[12]);

// Decrypts and verifies. Returns plaintext length or 0 if auth fails.
uint32_t aead_decrypt(uint8_t *out,
                      const uint8_t *ciphertext, uint32_t ct_len,
                      const uint8_t *aad, uint32_t aad_len,
                      const uint8_t key[32], const uint8_t nonce[12]);

// HKDF-SHA256 key derivation (using hardware SHA-256)
void hkdf_sha256(uint8_t *out, uint32_t out_len,
                 const uint8_t *ikm, uint32_t ikm_len,
                 const uint8_t *salt, uint32_t salt_len,
                 const uint8_t *info, uint32_t info_len);

#endif
