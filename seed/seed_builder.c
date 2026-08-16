 















#include <stdio.h>
#include <stdlib.h>
#include <string.h>

 
typedef struct {
    unsigned char buf[64];
    unsigned long long len;    
    unsigned int h[8];
} Sha256;

static unsigned int sha_rotr(unsigned int x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(Sha256 *s, const unsigned char *p) {
    static const unsigned int K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    unsigned int w[64];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) |
               ((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
    for (i = 16; i < 64; i++) {
        unsigned int s0 = sha_rotr(w[i-15],7) ^ sha_rotr(w[i-15],18) ^ (w[i-15] >> 3);
        unsigned int s1 = sha_rotr(w[i-2],17) ^ sha_rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned int a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (i = 0; i < 64; i++) {
        unsigned int S1 = sha_rotr(e,6) ^ sha_rotr(e,11) ^ sha_rotr(e,25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = h + S1 + ch + K[i] + w[i];
        unsigned int S0 = sha_rotr(a,2) ^ sha_rotr(a,13) ^ sha_rotr(a,22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void sha_init(Sha256 *s) {
    s->len = 0;
    s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372; s->h[3]=0xa54ff53a;
    s->h[4]=0x510e527f; s->h[5]=0x9b05688c; s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
}

static void sha_update(Sha256 *s, const unsigned char *p, size_t n) {
    while (n > 0) {
        size_t off = (size_t)(s->len & 63);
        size_t take = 64 - off;
        if (take > n) take = n;
        memcpy(s->buf + off, p, take);
        s->len += take;
        p += take;
        n -= take;
        if ((s->len & 63) == 0) sha_block(s, s->buf);
    }
}

static void sha_final(Sha256 *s, unsigned char out[32]) {
    unsigned long long bits = s->len * 8;
    unsigned char pad = 0x80;
    sha_update(s, &pad, 1);
    unsigned char zero = 0;
    while ((s->len & 63) != 56) sha_update(s, &zero, 1);
    unsigned char lenb[8];
    int i;
    for (i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - i*8));
    sha_update(s, lenb, 8);
    for (i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(s->h[i] >> 24);
        out[i*4+1] = (unsigned char)(s->h[i] >> 16);
        out[i*4+2] = (unsigned char)(s->h[i] >> 8);
        out[i*4+3] = (unsigned char)(s->h[i]);
    }
}

 
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

 




static int materialize(const char *hexpath, const char *outpath) {
    FILE *f = fopen(hexpath, "rb");
    if (!f) { fprintf(stderr, "seed_builder: 打不开 %s\n", hexpath); return 1; }

     
    char expect_sha[65] = {0};
    int header_lines = 0;
    char line[4096];
     
    while (header_lines < 5 && fgets(line, sizeof line, f)) {
        header_lines++;
        if (strncmp(line, "; sha256:", 9) == 0) {
            const char *p = line + 9;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            int n = 0;
            while (n < 64 && hexval(p[n]) >= 0) { expect_sha[n] = p[n]; n++; }
            expect_sha[n] = 0;
        }
    }
    if (expect_sha[0] == 0) {
        fprintf(stderr, "seed_builder: %s 头注释缺少 sha256 行\n", hexpath);
        fclose(f);
        return 1;
    }

     
    size_t cap = 1 << 20, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { fclose(f); return 1; }
    while (fgets(line, sizeof line, f)) {
        size_t i = 0, n = strlen(line);
        while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i < n && line[i] == ';') continue;    
        i = 0;
        while (i < n && line[i] != '\n' && line[i] != '\r') {
            int hi = hexval(line[i]);
            if (hi < 0) { i++; continue; }         
            int lo = hexval(line[i+1]);
            if (lo < 0) { fprintf(stderr, "seed_builder: %s 数据行 hex 残缺 @%zu: [%s]\n", hexpath, i, line); free(buf); fclose(f); return 1; }
            if (len >= cap) { cap *= 2; buf = (unsigned char *)realloc(buf, cap); }
            buf[len++] = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
    }
    fclose(f);

     
    Sha256 s;
    unsigned char digest[32];
    sha_init(&s);
    sha_update(&s, buf, len);
    sha_final(&s, digest);
    char got[65];
    for (int i = 0; i < 32; i++) sprintf(got + i*2, "%02x", digest[i]);
    got[64] = 0;
    if (strcmp(got, expect_sha) != 0) {
        fprintf(stderr, "seed_builder: %s sha256 不匹配! 期望 %s 实得 %s\n", hexpath, expect_sha, got);
        free(buf);
        return 1;
    }

     
    FILE *o = fopen(outpath, "wb");
    if (!o) { fprintf(stderr, "seed_builder: 写不出 %s\n", outpath); free(buf); return 1; }
    if (fwrite(buf, 1, len, o) != len) { fprintf(stderr, "seed_builder: 写入失败 %s\n", outpath); fclose(o); free(buf); return 1; }
    fclose(o);
    free(buf);
    printf("seed_builder: %s <- %s (%zu 字节, sha256 校验通过)\n", outpath, hexpath, len);
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= materialize("seed/atlv1_seed.hex", "atlv1.exe");
    rc |= materialize("seed/atl_rt_seed.hex", "atl_rt.obj");
    if (rc != 0) {
        fprintf(stderr, "seed_builder: 种子物化失败\n");
        return 1;
    }
    printf("seed_builder: 种子物化完成 — atlv1.exe + atl_rt.obj 已就位\n");
    return 0;
}
