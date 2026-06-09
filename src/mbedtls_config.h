// Minimal mbedtls configuration for PicoWAL — SHA-256 only
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

// We only need SHA-256 for password hashing
#define MBEDTLS_SHA256_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

// Required by SHA-256 internally
#define MBEDTLS_NO_PLATFORM_ENTROPY

#endif
