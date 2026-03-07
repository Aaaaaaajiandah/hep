/* src/core/sha1.c — public domain SHA-1 */
#include "sha1.h"
#include <string.h>
#include <stdint.h>

#define ROL32(x,n) (((x)<<(n))|((x)>>(32-(n))))

void sha1_init(sha1_ctx *ctx) {
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->count = 0;
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t a,b,c,d,e,W[80];
    int i;
    for(i=0;i<16;i++)
        W[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|
             ((uint32_t)block[i*4+2]<<8)|(uint32_t)block[i*4+3];
    for(i=16;i<80;i++)
        W[i]=ROL32(W[i-3]^W[i-8]^W[i-14]^W[i-16],1);
    a=state[0];b=state[1];c=state[2];d=state[3];e=state[4];
#define R0(v,w,x,y,z,n) z+=((w&(x^y))^y)+W[n]+0x5A827999u+ROL32(v,5);w=ROL32(w,30);
#define R2(v,w,x,y,z,n) z+=(w^x^y)+W[n]+0x6ED9EBA1u+ROL32(v,5);w=ROL32(w,30);
#define R3(v,w,x,y,z,n) z+=(((w|x)&y)|(w&x))+W[n]+0x8F1BBCDCu+ROL32(v,5);w=ROL32(w,30);
#define R4(v,w,x,y,z,n) z+=(w^x^y)+W[n]+0xCA62C1D6u+ROL32(v,5);w=ROL32(w,30);
    for(i= 0;i<20;i+=5){R0(a,b,c,d,e,i);R0(e,a,b,c,d,i+1);R0(d,e,a,b,c,i+2);R0(c,d,e,a,b,i+3);R0(b,c,d,e,a,i+4);}
    for(i=20;i<40;i+=5){R2(a,b,c,d,e,i);R2(e,a,b,c,d,i+1);R2(d,e,a,b,c,i+2);R2(c,d,e,a,b,i+3);R2(b,c,d,e,a,i+4);}
    for(i=40;i<60;i+=5){R3(a,b,c,d,e,i);R3(e,a,b,c,d,i+1);R3(d,e,a,b,c,i+2);R3(c,d,e,a,b,i+3);R3(b,c,d,e,a,i+4);}
    for(i=60;i<80;i+=5){R4(a,b,c,d,e,i);R4(e,a,b,c,d,i+1);R4(d,e,a,b,c,i+2);R4(c,d,e,a,b,i+3);R4(b,c,d,e,a,i+4);}
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;
}

void sha1_update(sha1_ctx *ctx, const uint8_t *data, size_t len) {
    size_t i, index = (size_t)(ctx->count & 63);
    ctx->count += len;
    size_t part = 64 - index;
    if (len >= part) {
        memcpy(&ctx->buf[index], data, part);
        sha1_transform(ctx->state, ctx->buf);
        for (i = part; i + 63 < len; i += 64)
            sha1_transform(ctx->state, data + i);
        index = 0;
    } else { i = 0; }
    memcpy(&ctx->buf[index], data + i, len - i);
}

void sha1_final(sha1_ctx *ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->count * 8;
    uint8_t pad = 0x80;
    sha1_update(ctx, &pad, 1);
    pad = 0;
    while ((ctx->count & 63) != 56)
        sha1_update(ctx, &pad, 1);
    uint8_t b[8];
    for (int i=7;i>=0;i--) { b[i]=bits&0xff; bits>>=8; }
    sha1_update(ctx, b, 8);
    for (int i=0;i<5;i++) {
        digest[i*4]   = (ctx->state[i]>>24)&0xff;
        digest[i*4+1] = (ctx->state[i]>>16)&0xff;
        digest[i*4+2] = (ctx->state[i]>> 8)&0xff;
        digest[i*4+3] = (ctx->state[i]    )&0xff;
    }
}

void sha1_buf(const uint8_t *data, size_t len, uint8_t digest[20]) {
    sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
}

void sha1_to_hex(const uint8_t sha[20], char hex[41]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        hex[i*2]   = h[(sha[i]>>4)&0xf];
        hex[i*2+1] = h[sha[i]&0xf];
    }
    hex[40] = '\0';
}

int sha1_from_hex(const char *hex, uint8_t sha[20]) {
    for (int i = 0; i < 20; i++) {
        unsigned hi, lo;
        char c = hex[i*2];
        if      (c>='0'&&c<='9') hi=c-'0';
        else if (c>='a'&&c<='f') hi=c-'a'+10;
        else if (c>='A'&&c<='F') hi=c-'A'+10;
        else return -1;
        c = hex[i*2+1];
        if      (c>='0'&&c<='9') lo=c-'0';
        else if (c>='a'&&c<='f') lo=c-'a'+10;
        else if (c>='A'&&c<='F') lo=c-'A'+10;
        else return -1;
        sha[i] = (uint8_t)((hi<<4)|lo);
    }
    return 0;
}
