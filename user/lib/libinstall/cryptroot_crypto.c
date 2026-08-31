#include "cryptroot_crypto.h"
#include <string.h>

static void hmac(const uint8_t *key,uint32_t keylen,const uint8_t *msg,uint32_t len,uint8_t out[32])
{
    br_hmac_key_context keyctx;br_hmac_context ctx;
    br_hmac_key_init(&keyctx,&br_sha256_vtable,key,keylen);
    br_hmac_init(&ctx,&keyctx,32);br_hmac_update(&ctx,msg,len);br_hmac_out(&ctx,out);
    memset(&keyctx,0,sizeof(keyctx));memset(&ctx,0,sizeof(ctx));
}

void install_pbkdf2(const uint8_t *pw,uint32_t plen,const uint8_t salt[16],uint32_t rounds,uint8_t key[64])
{
    uint8_t msg[20],u[32],t[32];memcpy(msg,salt,16);if(!rounds)rounds=1;
    for(uint32_t block=1;block<=2;block++){
        msg[16]=block>>24;msg[17]=block>>16;msg[18]=block>>8;msg[19]=block;
        hmac(pw,plen,msg,20,u);memcpy(t,u,32);
        for(uint32_t r=1;r<rounds;r++){hmac(pw,plen,u,32,u);for(int i=0;i<32;i++)t[i]^=u[i];}
        memcpy(key+32*(block-1),t,32);
    }
    memset(u,0,sizeof(u));memset(t,0,sizeof(t));
}

void install_key_verifier(const uint8_t key[64],uint8_t out[32])
{
    static const uint8_t label[]="LoricaOS encrypted root";
    hmac(key,64,label,sizeof(label)-1,out);
}

void install_xts_init(install_xts_ctx_t *ctx,const uint8_t key[64])
{
    br_aes_ct64_cbcenc_init(&ctx->data_enc,key,32);
    br_aes_ct64_cbcdec_init(&ctx->data_dec,key,32);
    br_aes_ct64_cbcenc_init(&ctx->tweak_enc,key+32,32);
}

static void aes_enc(const br_aes_ct64_cbcenc_keys *ctx,uint8_t block[16])
{ uint8_t iv[16]={0};br_aes_ct64_cbcenc_run(ctx,iv,block,16); }
static void aes_dec(const br_aes_ct64_cbcdec_keys *ctx,uint8_t block[16])
{ uint8_t iv[16]={0};br_aes_ct64_cbcdec_run(ctx,iv,block,16); }
static void mulx(uint8_t *t){uint8_t carry=0;for(int i=0;i<16;i++){uint8_t n=t[i]>>7;t[i]=(uint8_t)((t[i]<<1)|carry);carry=n;}if(carry)t[0]^=0x87;}

void install_xts(install_xts_ctx_t *ctx,uint8_t *data,uint32_t len,uint64_t sector,int encrypt)
{
    uint8_t tweak[16]={0};for(int i=0;i<8;i++)tweak[i]=(uint8_t)(sector>>(8*i));aes_enc(&ctx->tweak_enc,tweak);
    for(uint32_t off=0;off+16<=len;off+=16){for(int i=0;i<16;i++)data[off+i]^=tweak[i];if(encrypt)aes_enc(&ctx->data_enc,data+off);else aes_dec(&ctx->data_dec,data+off);for(int i=0;i<16;i++)data[off+i]^=tweak[i];mulx(tweak);}
    memset(tweak,0,sizeof(tweak));
}
