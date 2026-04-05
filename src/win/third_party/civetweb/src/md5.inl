#include <stdint.h>
#include <string.h>

typedef uint8_t md5_byte_t;
typedef uint32_t md5_word_t;

typedef struct md5_state_s {
    md5_word_t count[2];
    md5_word_t abcd[4];
    md5_byte_t buf[64];
} md5_state_t;

static void md5_process(md5_state_t* pms, const md5_byte_t* data);

static void md5_init(md5_state_t* pms) {
    pms->count[0] = pms->count[1] = 0;
    pms->abcd[0] = 0x67452301;
    pms->abcd[1] = 0xefcdab89;
    pms->abcd[2] = 0x98badcfe;
    pms->abcd[3] = 0x10325476;
}

static void md5_append(md5_state_t* pms, const md5_byte_t* data, int nbytes) {
    const md5_byte_t* p = data;
    int left = nbytes;
    int offset = (int)((pms->count[0] >> 3) & 63);
    md5_word_t nbits = (md5_word_t)(nbytes << 3);

    if (nbytes <= 0) return;

    pms->count[1] += (md5_word_t)(nbytes >> 29);
    pms->count[0] += nbits;
    if (pms->count[0] < nbits) pms->count[1]++;

    if (offset) {
        int copy = 64 - offset;
        if (copy > left) copy = left;
        memcpy(pms->buf + offset, p, (size_t)copy);
        if (offset + copy < 64) return;
        md5_process(pms, pms->buf);
        p += copy;
        left -= copy;
    }

    while (left >= 64) {
        md5_process(pms, p);
        p += 64;
        left -= 64;
    }

    if (left) memcpy(pms->buf, p, (size_t)left);
}

static void md5_finish(md5_state_t* pms, md5_byte_t digest[16]) {
    static const md5_byte_t pad[64] = {0x80};
    md5_byte_t data[8];
    int i;

    for (i = 0; i < 8; i++) {
        data[i] = (md5_byte_t)((pms->count[i >> 2] >> ((i & 3) << 3)) & 0xff);
    }

    md5_append(pms, pad, (int)(1 + ((119 - ((pms->count[0] >> 3) & 63)) & 63)));
    md5_append(pms, data, 8);

    for (i = 0; i < 16; i++) {
        digest[i] = (md5_byte_t)((pms->abcd[i >> 2] >> ((i & 3) << 3)) & 0xff);
    }
}

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))

#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define STEP(f, a, b, c, d, x, t, s) \
    (a) += f((b), (c), (d)) + (x) + (t); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b);

static void md5_process(md5_state_t* pms, const md5_byte_t* data) {
    md5_word_t a = pms->abcd[0], b = pms->abcd[1], c = pms->abcd[2], d = pms->abcd[3];
    md5_word_t x[16];
    int i;

    for (i = 0; i < 16; i++) {
        x[i] = (md5_word_t)data[i * 4] |
               ((md5_word_t)data[i * 4 + 1] << 8) |
               ((md5_word_t)data[i * 4 + 2] << 16) |
               ((md5_word_t)data[i * 4 + 3] << 24);
    }

    STEP(F, a, b, c, d, x[0], 0xd76aa478, 7)
    STEP(F, d, a, b, c, x[1], 0xe8c7b756, 12)
    STEP(F, c, d, a, b, x[2], 0x242070db, 17)
    STEP(F, b, c, d, a, x[3], 0xc1bdceee, 22)
    STEP(F, a, b, c, d, x[4], 0xf57c0faf, 7)
    STEP(F, d, a, b, c, x[5], 0x4787c62a, 12)
    STEP(F, c, d, a, b, x[6], 0xa8304613, 17)
    STEP(F, b, c, d, a, x[7], 0xfd469501, 22)
    STEP(F, a, b, c, d, x[8], 0x698098d8, 7)
    STEP(F, d, a, b, c, x[9], 0x8b44f7af, 12)
    STEP(F, c, d, a, b, x[10], 0xffff5bb1, 17)
    STEP(F, b, c, d, a, x[11], 0x895cd7be, 22)
    STEP(F, a, b, c, d, x[12], 0x6b901122, 7)
    STEP(F, d, a, b, c, x[13], 0xfd987193, 12)
    STEP(F, c, d, a, b, x[14], 0xa679438e, 17)
    STEP(F, b, c, d, a, x[15], 0x49b40821, 22)

    STEP(G, a, b, c, d, x[1], 0xf61e2562, 5)
    STEP(G, d, a, b, c, x[6], 0xc040b340, 9)
    STEP(G, c, d, a, b, x[11], 0x265e5a51, 14)
    STEP(G, b, c, d, a, x[0], 0xe9b6c7aa, 20)
    STEP(G, a, b, c, d, x[5], 0xd62f105d, 5)
    STEP(G, d, a, b, c, x[10], 0x02441453, 9)
    STEP(G, c, d, a, b, x[15], 0xd8a1e681, 14)
    STEP(G, b, c, d, a, x[4], 0xe7d3fbc8, 20)
    STEP(G, a, b, c, d, x[9], 0x21e1cde6, 5)
    STEP(G, d, a, b, c, x[14], 0xc33707d6, 9)
    STEP(G, c, d, a, b, x[3], 0xf4d50d87, 14)
    STEP(G, b, c, d, a, x[8], 0x455a14ed, 20)
    STEP(G, a, b, c, d, x[13], 0xa9e3e905, 5)
    STEP(G, d, a, b, c, x[2], 0xfcefa3f8, 9)
    STEP(G, c, d, a, b, x[7], 0x676f02d9, 14)
    STEP(G, b, c, d, a, x[12], 0x8d2a4c8a, 20)

    STEP(H, a, b, c, d, x[5], 0xfffa3942, 4)
    STEP(H, d, a, b, c, x[8], 0x8771f681, 11)
    STEP(H, c, d, a, b, x[11], 0x6d9d6122, 16)
    STEP(H, b, c, d, a, x[14], 0xfde5380c, 23)
    STEP(H, a, b, c, d, x[1], 0xa4beea44, 4)
    STEP(H, d, a, b, c, x[4], 0x4bdecfa9, 11)
    STEP(H, c, d, a, b, x[7], 0xf6bb4b60, 16)
    STEP(H, b, c, d, a, x[10], 0xbebfbc70, 23)
    STEP(H, a, b, c, d, x[13], 0x289b7ec6, 4)
    STEP(H, d, a, b, c, x[0], 0xeaa127fa, 11)
    STEP(H, c, d, a, b, x[3], 0xd4ef3085, 16)
    STEP(H, b, c, d, a, x[6], 0x04881d05, 23)
    STEP(H, a, b, c, d, x[9], 0xd9d4d039, 4)
    STEP(H, d, a, b, c, x[12], 0xe6db99e5, 11)
    STEP(H, c, d, a, b, x[15], 0x1fa27cf8, 16)
    STEP(H, b, c, d, a, x[2], 0xc4ac5665, 23)

    STEP(I, a, b, c, d, x[0], 0xf4292244, 6)
    STEP(I, d, a, b, c, x[7], 0x432aff97, 10)
    STEP(I, c, d, a, b, x[14], 0xab9423a7, 15)
    STEP(I, b, c, d, a, x[5], 0xfc93a039, 21)
    STEP(I, a, b, c, d, x[12], 0x655b59c3, 6)
    STEP(I, d, a, b, c, x[3], 0x8f0ccc92, 10)
    STEP(I, c, d, a, b, x[10], 0xffeff47d, 15)
    STEP(I, b, c, d, a, x[1], 0x85845dd1, 21)
    STEP(I, a, b, c, d, x[8], 0x6fa87e4f, 6)
    STEP(I, d, a, b, c, x[15], 0xfe2ce6e0, 10)
    STEP(I, c, d, a, b, x[6], 0xa3014314, 15)
    STEP(I, b, c, d, a, x[13], 0x4e0811a1, 21)
    STEP(I, a, b, c, d, x[4], 0xf7537e82, 6)
    STEP(I, d, a, b, c, x[11], 0xbd3af235, 10)
    STEP(I, c, d, a, b, x[2], 0x2ad7d2bb, 15)
    STEP(I, b, c, d, a, x[9], 0xeb86d391, 21)

    pms->abcd[0] += a;
    pms->abcd[1] += b;
    pms->abcd[2] += c;
    pms->abcd[3] += d;
}

#undef F
#undef G
#undef H
#undef I
#undef ROTATE_LEFT
#undef STEP

