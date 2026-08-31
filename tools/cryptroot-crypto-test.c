#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../user/lib/libinstall/cryptroot_crypto.h"
static int hx(const uint8_t*p,const char*w,int n){static const char h[]="0123456789abcdef";for(int i=0;i<n;i++)if(h[p[i]>>4]!=w[2*i]||h[p[i]&15]!=w[2*i+1])return 0;return 1;}
int main(void){uint8_t k[64],s[16]={0},d[32]="0123456789abcdef0123456789abcdef";install_xts_ctx_t c;memcpy(s,"salt",4);install_pbkdf2((uint8_t*)"password",8,s,1,k);if(!hx(k,"3267a06614b9d090bc3e684eebdb6af8ee753cf80b7f7f7cf5e676c65422d054",32))return 1;for(int i=0;i<64;i++)k[i]=i;install_xts_init(&c,k);install_xts(&c,d,32,5,1);if(!hx(d,"a8f5c3ecbc0111003e7cdb3b0678bb8844f42eee4a7ec348da7d35ac4a75fef5",32))return 2;install_xts(&c,d,32,5,0);if(memcmp(d,"0123456789abcdef0123456789abcdef",32))return 3;puts("cryptroot-crypto: PASS (PBKDF2 and AES-256-XTS vectors)");return 0;}
