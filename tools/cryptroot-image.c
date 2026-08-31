#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include "../user/lib/libinstall/cryptroot_crypto.h"
#define ROUNDS 100000u
static void put32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int main(int argc,char**argv){if(argc!=4){fprintf(stderr,"usage: %s <ext2> <encrypted> <password>\n",argv[0]);return 2;}FILE*in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");if(!in||!out){perror("open");return 1;}uint8_t h[4096]={0},key[64],buf[512];memcpy(h,"LORICRY1",8);put32(h+8,1);put32(h+12,8);put32(h+16,ROUNDS);if(getrandom(h+20,16,0)!=16){perror("getrandom");return 1;}install_pbkdf2((uint8_t*)argv[3],strlen(argv[3]),h+20,ROUNDS,key);install_key_verifier(key,h+36);install_xts_ctx_t c;install_xts_init(&c,key);memset(key,0,sizeof(key));if(fwrite(h,1,sizeof(h),out)!=sizeof(h))return 1;uint64_t sector=0;size_t n;while((n=fread(buf,1,sizeof(buf),in))!=0){if(n!=sizeof(buf)){fprintf(stderr,"input is not sector aligned\n");return 1;}install_xts(&c,buf,sizeof(buf),sector++,1);if(fwrite(buf,1,sizeof(buf),out)!=sizeof(buf))return 1;}if(ferror(in)||fclose(out)){perror("I/O");return 1;}fclose(in);memset(buf,0,sizeof(buf));return 0;}
