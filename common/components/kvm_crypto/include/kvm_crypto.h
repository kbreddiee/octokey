/*
 * espkvm — pairing crypto primitives (thin wrappers over mbedTLS, which
 * ships with ESP-IDF; no external dependency).
 *
 *  - X25519 ECDH with ephemeral keys, generated fresh every time pairing
 *    mode is entered.
 *  - HKDF-SHA256 to derive independent keys from the shared secret:
 *        LMK   = HKDF(shared, "espkvm lmk v1")   -> ESP-NOW per-peer key
 *        KEK   = HKDF(shared, "espkvm kek v1")   -> wraps the PMK blob
 *        PROOF = HKDF(shared, "espkvm done v1")  -> PAIR_DONE possession proof
 *  - AES-128-GCM to transport the hub's PMK to the dongle.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "kvm_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque ephemeral ECDH context (heap-allocated; free with kvm_ecdh_free). */
typedef struct kvm_ecdh kvm_ecdh_t;

/* Generate an ephemeral X25519 keypair; writes the public key (32 bytes,
 * little-endian per RFC 7748). Returns NULL on failure. */
kvm_ecdh_t *kvm_ecdh_new(uint8_t pub_out[KVM_PUBKEY_LEN]);

/* Compute the raw shared secret with a peer's public key. */
esp_err_t kvm_ecdh_secret(kvm_ecdh_t *ctx,
                          const uint8_t peer_pub[KVM_PUBKEY_LEN],
                          uint8_t secret_out[32]);

void kvm_ecdh_free(kvm_ecdh_t *ctx);

/* HKDF-SHA256(secret, info=label) -> out[len]. */
esp_err_t kvm_kdf(const uint8_t secret[32], const char *label,
                  uint8_t *out, size_t len);

/* AES-128-GCM seal/open (no AAD; the pairing session id is bound via the
 * surrounding handshake instead). */
esp_err_t kvm_gcm_seal(const uint8_t key[KVM_KEY_LEN],
                       const uint8_t nonce[KVM_NONCE_LEN],
                       const uint8_t *pt, size_t len,
                       uint8_t *ct, uint8_t tag[KVM_TAG_LEN]);

esp_err_t kvm_gcm_open(const uint8_t key[KVM_KEY_LEN],
                       const uint8_t nonce[KVM_NONCE_LEN],
                       const uint8_t *ct, size_t len,
                       const uint8_t tag[KVM_TAG_LEN],
                       uint8_t *pt);

/* Hardware RNG. */
void kvm_rand(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
