#ifndef LORICA_CRYPTROOT_CRYPTO_H
#define LORICA_CRYPTROOT_CRYPTO_H
#include <stdint.h>
#include <bearssl.h>
typedef struct {
    br_aes_ct64_cbcenc_keys data_enc, tweak_enc;
    br_aes_ct64_cbcdec_keys data_dec;
} install_xts_ctx_t;
void install_pbkdf2(const uint8_t *,uint32_t,const uint8_t[16],uint32_t,uint8_t[64]);
void install_key_verifier(const uint8_t[64],uint8_t[32]);
void install_xts_init(install_xts_ctx_t *,const uint8_t[64]);
void install_xts(install_xts_ctx_t *,uint8_t *,uint32_t,uint64_t,int);
#endif
