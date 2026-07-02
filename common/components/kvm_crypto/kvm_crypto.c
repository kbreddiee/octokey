/* espkvm pairing crypto — X25519 + HKDF-SHA256 + AES-128-GCM via mbedTLS. */
/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <string.h>

#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "esp_random.h"
#include "esp_log.h"

#include "kvm_crypto.h"

static const char *TAG = "kvm_crypto";

struct kvm_ecdh {
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;   /* our ephemeral private scalar */
    mbedtls_ecp_point Q;   /* our ephemeral public point   */
};

/* mbedTLS-style RNG callback backed by the ESP32 hardware RNG. */
static int hw_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

void kvm_rand(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}

kvm_ecdh_t *kvm_ecdh_new(uint8_t pub_out[KVM_PUBKEY_LEN])
{
    kvm_ecdh_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    mbedtls_ecp_group_init(&ctx->grp);
    mbedtls_mpi_init(&ctx->d);
    mbedtls_ecp_point_init(&ctx->Q);

    int rc = mbedtls_ecp_group_load(&ctx->grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) {
        rc = mbedtls_ecdh_gen_public(&ctx->grp, &ctx->d, &ctx->Q, hw_rng, NULL);
    }
    if (rc == 0) {
        /* For Montgomery curves mbedTLS serialises the point as the x
         * coordinate, 32 bytes little-endian — exactly RFC 7748 format. */
        size_t olen = 0;
        rc = mbedtls_ecp_point_write_binary(&ctx->grp, &ctx->Q,
                                            MBEDTLS_ECP_PF_COMPRESSED, &olen,
                                            pub_out, KVM_PUBKEY_LEN);
        if (rc == 0 && olen != KVM_PUBKEY_LEN) {
            rc = -1;
        }
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "ecdh keygen failed: -0x%04x", (unsigned)-rc);
        kvm_ecdh_free(ctx);
        return NULL;
    }
    return ctx;
}

esp_err_t kvm_ecdh_secret(kvm_ecdh_t *ctx,
                          const uint8_t peer_pub[KVM_PUBKEY_LEN],
                          uint8_t secret_out[32])
{
    mbedtls_ecp_point Qp;
    mbedtls_mpi z;
    mbedtls_ecp_point_init(&Qp);
    mbedtls_mpi_init(&z);

    int rc = mbedtls_ecp_point_read_binary(&ctx->grp, &Qp,
                                           peer_pub, KVM_PUBKEY_LEN);
    if (rc == 0) {
        rc = mbedtls_ecdh_compute_shared(&ctx->grp, &z, &Qp, &ctx->d,
                                         hw_rng, NULL);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary_le(&z, secret_out, 32);
    }

    mbedtls_ecp_point_free(&Qp);
    mbedtls_mpi_free(&z);

    if (rc != 0) {
        ESP_LOGE(TAG, "ecdh shared failed: -0x%04x", (unsigned)-rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void kvm_ecdh_free(kvm_ecdh_t *ctx)
{
    if (!ctx) {
        return;
    }
    mbedtls_ecp_point_free(&ctx->Q);
    mbedtls_mpi_free(&ctx->d);       /* zeroises the private scalar */
    mbedtls_ecp_group_free(&ctx->grp);
    free(ctx);
}

esp_err_t kvm_kdf(const uint8_t secret[32], const char *label,
                  uint8_t *out, size_t len)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = mbedtls_hkdf(md, NULL, 0, secret, 32,
                          (const unsigned char *)label, strlen(label),
                          out, len);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t kvm_gcm_seal(const uint8_t key[KVM_KEY_LEN],
                       const uint8_t nonce[KVM_NONCE_LEN],
                       const uint8_t *pt, size_t len,
                       uint8_t *ct, uint8_t tag[KVM_TAG_LEN])
{
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len,
                                       nonce, KVM_NONCE_LEN, NULL, 0,
                                       pt, ct, KVM_TAG_LEN, tag);
    }
    mbedtls_gcm_free(&g);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t kvm_gcm_open(const uint8_t key[KVM_KEY_LEN],
                       const uint8_t nonce[KVM_NONCE_LEN],
                       const uint8_t *ct, size_t len,
                       const uint8_t tag[KVM_TAG_LEN],
                       uint8_t *pt)
{
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&g, len, nonce, KVM_NONCE_LEN,
                                      NULL, 0, tag, KVM_TAG_LEN, ct, pt);
    }
    mbedtls_gcm_free(&g);
    return rc == 0 ? ESP_OK : ESP_FAIL;   /* tag mismatch -> ESP_FAIL */
}
