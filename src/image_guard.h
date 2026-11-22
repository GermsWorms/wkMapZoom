/* Exact local executable identity gate; no provider DLLs or game calls. */
#ifndef WA_IMAGE_GUARD_H
#define WA_IMAGE_GUARD_H
#include <stdint.h>
#include <string.h>
typedef struct {uint32_t h[8];uint64_t bytes;unsigned used;unsigned char block[64];} wa_sha256;
static uint32_t wa_ror(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void wa_sha_block(wa_sha256 *s,const unsigned char *p){
 static const uint32_t k[64]={
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
 uint32_t w[64],a,b,c,d,e,f,g,h,t,u;unsigned i;
 for(i=0;i<16;i++)w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
 for(i=16;i<64;i++)w[i]=w[i-16]+(wa_ror(w[i-15],7)^wa_ror(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(wa_ror(w[i-2],17)^wa_ror(w[i-2],19)^(w[i-2]>>10));
 a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
 for(i=0;i<64;i++){t=h+(wa_ror(e,6)^wa_ror(e,11)^wa_ror(e,25))+((e&f)^((~e)&g))+k[i]+w[i];u=(wa_ror(a,2)^wa_ror(a,13)^wa_ror(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
 s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void wa_sha_init(wa_sha256 *s){
 const uint32_t initial[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
 memset(s,0,sizeof(*s));memcpy(s->h,initial,sizeof(initial));
}
static void wa_sha_update(wa_sha256 *s,const unsigned char *p,size_t n){
 s->bytes+=n;
 while(n){size_t take=64-s->used;if(take>n)take=n;memcpy(s->block+s->used,p,take);s->used+=(unsigned)take;p+=take;n-=take;if(s->used==64){wa_sha_block(s,s->block);s->used=0;}}
}
static void wa_sha_final(wa_sha256 *s,unsigned char digest[32]){
 uint64_t bits=s->bytes*8;unsigned i;
 s->block[s->used++]=0x80;
 if(s->used>56){memset(s->block+s->used,0,64-s->used);wa_sha_block(s,s->block);s->used=0;}
 memset(s->block+s->used,0,56-s->used);
 for(i=0;i<8;i++)s->block[63-i]=(unsigned char)(bits>>(i*8));
 wa_sha_block(s,s->block);
 for(i=0;i<32;i++)digest[i]=(unsigned char)(s->h[i/4]>>(24-(i%4)*8));
}
static int wa_supported_image(const char *path){
 const unsigned char expected[32]={0xd6,0xc4,0x49,0xc7,0x59,0x16,0xa0,0x94,0x1d,0x11,0x63,0xaf,0xc2,0xa9,0x1f,0x1e,0x72,0xa3,0x16,0x00,0x60,0x9b,0xb9,0xb7,0x66,0xf2,0x30,0x2c,0xa8,0x01,0x64,0xeb};
 unsigned char buffer[16384],digest[32];DWORD n;BOOL ok;LARGE_INTEGER size;wa_sha256 hash;
 HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
 if(file==INVALID_HANDLE_VALUE)return 0;
 if(!GetFileSizeEx(file,&size)||size.QuadPart!=3600384){CloseHandle(file);return 0;}
 wa_sha_init(&hash);
 while((ok=ReadFile(file,buffer,sizeof(buffer),&n,NULL)) && n)wa_sha_update(&hash,buffer,n);
 CloseHandle(file);if(!ok)return 0;wa_sha_final(&hash,digest);
 return memcmp(digest,expected,32)==0;
}
#endif
