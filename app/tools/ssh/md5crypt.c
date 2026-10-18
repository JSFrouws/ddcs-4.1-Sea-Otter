/* crypt() for a static dropbear on the DDCS V4.1: the cross toolchain has no libcrypt, so
 * password login needs its own. Supports the "$1$" MD5-crypt format (what the unit's
 * BusyBox `chpasswd -m` / `passwd -a m` write). Anything else (e.g. the vendor's old DES
 * hash) never matches, so set a password with `chpasswd -m` first - see docs/ssh-setup.md.
 * MD5 per RFC 1321, MD5-crypt per Poul-Henning Kamp's original algorithm. */
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t s[4]; uint64_t n; uint8_t b[64]; } md5_t;

#define ROL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))
static const uint32_t K[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
static const uint8_t R[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

static void md5_block(md5_t *m, const uint8_t *p)
{
	uint32_t w[16], a = m->s[0], b = m->s[1], c = m->s[2], d = m->s[3];
	for (int i = 0; i < 16; i++) w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
	for (int i = 0; i < 64; i++) {
		uint32_t f; int g;
		if (i < 16) { f = (b & c) | (~b & d); g = i; }
		else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
		else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
		else { f = c ^ (b | ~d); g = (7 * i) % 16; }
		uint32_t t = d; d = c; c = b;
		b = b + ROL(a + f + K[i] + w[g], R[i]);
		a = t;
	}
	m->s[0] += a; m->s[1] += b; m->s[2] += c; m->s[3] += d;
}

static void md5_init(md5_t *m) { m->s[0] = 0x67452301; m->s[1] = 0xefcdab89; m->s[2] = 0x98badcfe; m->s[3] = 0x10325476; m->n = 0; }

static void md5_add(md5_t *m, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len--) { m->b[m->n++ % 64] = *p++; if (m->n % 64 == 0) md5_block(m, m->b); }
}

static void md5_end(md5_t *m, uint8_t out[16])
{
	uint64_t bits = m->n * 8;
	uint8_t pad = 0x80, z = 0;
	md5_add(m, &pad, 1);
	while (m->n % 64 != 56) md5_add(m, &z, 1);
	for (int i = 0; i < 8; i++) { uint8_t c = (uint8_t)(bits >> (8 * i)); md5_add(m, &c, 1); }
	for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(m->s[i] >> (8 * j));
}

static char *to64(char *s, uint32_t v, int n)
{
	static const char a64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	while (n--) { *s++ = a64[v & 0x3f]; v >>= 6; }
	return s;
}

char *crypt(const char *pw, const char *setting)
{
	static char out[64];
	static const char magic[] = "$1$";
	if (!pw || !setting || strncmp(setting, magic, 3)) { out[0] = '*'; out[1] = 0; return out; }   /* never matches */
	const char *salt = setting + 3;
	size_t sl = 0, pl = strlen(pw);
	while (sl < 8 && salt[sl] && salt[sl] != '$') sl++;
	uint8_t f[16];
	md5_t c, a;
	md5_init(&c); md5_add(&c, pw, pl); md5_add(&c, magic, 3); md5_add(&c, salt, sl);
	md5_init(&a); md5_add(&a, pw, pl); md5_add(&a, salt, sl); md5_add(&a, pw, pl); md5_end(&a, f);
	for (size_t n = pl; n > 0; n -= n > 16 ? 16 : n) md5_add(&c, f, n > 16 ? 16 : n);
	for (size_t i = pl; i; i >>= 1) { if (i & 1) md5_add(&c, "", 1); else md5_add(&c, pw, 1); }
	md5_end(&c, f);
	for (int i = 0; i < 1000; i++) {
		md5_init(&c);
		if (i & 1) md5_add(&c, pw, pl); else md5_add(&c, f, 16);
		if (i % 3) md5_add(&c, salt, sl);
		if (i % 7) md5_add(&c, pw, pl);
		if (i & 1) md5_add(&c, f, 16); else md5_add(&c, pw, pl);
		md5_end(&c, f);
	}
	char *s = out;
	memcpy(s, magic, 3); s += 3; memcpy(s, salt, sl); s += sl; *s++ = '$';
	s = to64(s, (uint32_t)f[0] << 16 | f[6] << 8 | f[12], 4);
	s = to64(s, (uint32_t)f[1] << 16 | f[7] << 8 | f[13], 4);
	s = to64(s, (uint32_t)f[2] << 16 | f[8] << 8 | f[14], 4);
	s = to64(s, (uint32_t)f[3] << 16 | f[9] << 8 | f[15], 4);
	s = to64(s, (uint32_t)f[4] << 16 | f[10] << 8 | f[5], 4);
	s = to64(s, f[11], 2);
	*s = 0;
	return out;
}

#ifdef MD5CRYPT_TEST
#include <stdio.h>
int main(int argc, char **argv) { puts(crypt(argv[1], argv[2])); return 0; }
#endif
