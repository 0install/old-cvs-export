#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>

#include "global.h"
#include "support.h"
#include "zero-install.h"

void *my_malloc(size_t size)
{
	void *new;

	new = malloc(size);
	if (!new) {
		fprintf(stderr, "zero-install: Out of memory");
		return NULL;
	}

	return new;
}

void *my_realloc(void *old, size_t size)
{
	void *new;

	new = realloc(old, size);
	if (!new) {
		fprintf(stderr, "zero-install: Out of memory");
		return NULL;
	}

	return new;
}

char *my_strdup(const char *str)
{
	int l = strlen(str) + 1;
	char *new;

	new = my_malloc(l);
	if (!new)
		return NULL;
		
	memcpy(new, str, l);

	return new;
}

void set_blocking(int fd, int blocking)
{
	if (fcntl(fd, F_SETFL, blocking ? 0 : O_NONBLOCK))
		perror("fcntl() failed");
}

/* If uri is relative, convert to absolute path, using the given base URI.
 * 'http://foo.org/dir', 'leaf.html' -> 'http://foo.org/dir/leaf.html'
 * 'http://foo.org/dir', 'http://bar.org/leaf' -> 'http://bar.org/leaf'
 *
 * 0 on success.
 */
int uri_ensure_absolute(const char *uri, const char *base,
			char *result, int result_len)
{
	int base_len, uri_len;

	if (strncmp(uri, "ftp://", 6) == 0 ||
	    strncmp(uri, "http://", 7) == 0 ||
	    strncmp(uri, "https://", 8) == 0) {
		int len = strlen(uri) + 1;
		if (len > result_len) {
			fprintf(stderr, "URI too long\n");
			return 0;
		}
		memcpy(result, uri, len);
		return 1;
	}

	/* Relative path */
	/* printf("Join '%s' + '%s'\n", base, uri); */

	uri_len = strlen(uri);
	base_len = strlen(base);

	if (uri_len + base_len + 2 > result_len) {
		fprintf(stderr, "URI too long\n");
		return 0;
	}

	memcpy(result, base, base_len);
	result[base_len] = '/';
	memcpy(result + base_len + 1, uri, uri_len + 1);

	return 1;
}

/* /www.foo.org/some/path, one, two to
 * http://www.foo.org/some/path/one/two
 *
 * The two 'leaf' components are appended, if non-NULL.
 * 0 on error, which will have been already reported.
 */
int build_uri(char *buffer, int len, const char *path,
		     const char *leaf1, const char *leaf2)
{
	assert(path[0] == '/');

	if (snprintf(buffer, len, "http:/%s%s%s%s%s", path,
			leaf1 ? "/" : "", leaf1 ? leaf1 : "",
			leaf2 ? "/" : "", leaf2 ? leaf2 : "") > len - 1)
		goto too_big;

	return 1;
too_big:
	fprintf(stderr, "Path '%s' too long to convert to URI\n", path);
	return 0;
}

/* Ensure that 'path' is a directory, creating it if not.
 * If 'path' already exists as a non-directory, it is unlinked.
 * As a sanity check, 'path' must start with cache_dir.
 * Returns 1 on success.
 */
int ensure_dir(const char *path)
{
	struct stat info;

	if (strncmp(path, cache_dir, strlen(cache_dir)) != 0) {
		fprintf(stderr, "'%s' is not in cache directory!\n", path);
		exit(1);
	}
	
	if (lstat(path, &info) == 0) {
		if (S_ISDIR(info.st_mode))
			return 1;	/* Already exists */
		fprintf(stderr, "%s should be a directory... unlinking!\n",
				path);
		unlink(path);
	}

	if (mkdir(path, 0755)) {
		perror("mkdir");
		fprintf(stderr, "(while creating %s)\n", path);
		return 0;
	}

	return 1;
}

/* Set the close-on-exec flag for this FD.
 * TRUE means that an exec()'d process will not get the FD.
 */
void close_on_exec(int fd, int close)
{
	if (fcntl(fd, F_SETFD, close)) {
		perror("fcntl() failed");
		exit(EXIT_FAILURE);
	}
}

/*
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest. The original code was
 * written by Colin Plumb in 1993, and put in the public domain.
 * 
 * Modified to use glib datatypes. Put under GPL to simplify
 * licensing for ROX-Filer. Taken from Debian's dpkg package.
 */

#define md5byte unsigned char

static void MD5Transform(u_int32_t buf[4], u_int32_t const in[16]);

typedef struct _MD5Context MD5Context;

struct _MD5Context {
	u_int32_t buf[4];
	u_int32_t bytes[2];
	u_int32_t in[16];
};

#if G_BYTE_ORDER == G_BIG_ENDIAN
void byteSwap(u_int32_t *buf, unsigned words)
{
	md5byte *p = (md5byte *)buf;

	do {
		*buf++ = (u_int32_t)((unsigned)p[3] << 8 | p[2]) << 16 |
			((unsigned)p[1] << 8 | p[0]);
		p += 4;
	} while (--words);
}
#else
#define byteSwap(buf,words)
#endif

/*
 * Start MD5 accumulation. Set bit count to 0 and buffer to mysterious
 * initialization constants.
 */
static void MD5Init(MD5Context *ctx)
{
	ctx->buf[0] = 0x67452301;
	ctx->buf[1] = 0xefcdab89;
	ctx->buf[2] = 0x98badcfe;
	ctx->buf[3] = 0x10325476;

	ctx->bytes[0] = 0;
	ctx->bytes[1] = 0;
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
static void MD5Update(MD5Context *ctx, md5byte const *buf, unsigned len)
{
	u_int32_t t;

	/* Update byte count */

	t = ctx->bytes[0];
	if ((ctx->bytes[0] = t + len) < t)
		ctx->bytes[1]++;	/* Carry from low to high */

	t = 64 - (t & 0x3f);	/* Space available in ctx->in (at least 1) */
	if (t > len) {
		memcpy((md5byte *)ctx->in + 64 - t, buf, len);
		return;
	}
	/* First chunk is an odd size */
	memcpy((md5byte *)ctx->in + 64 - t, buf, t);
	byteSwap(ctx->in, 16);
	MD5Transform(ctx->buf, ctx->in);
	buf += t;
	len -= t;

	/* Process data in 64-byte chunks */
	while (len >= 64) {
		memcpy(ctx->in, buf, 64);
		byteSwap(ctx->in, 16);
		MD5Transform(ctx->buf, ctx->in);
		buf += 64;
		len -= 64;
	}

	/* Handle any remaining bytes of data. */
	memcpy(ctx->in, buf, len);
}

/*
 * Final wrapup - pad to 64-byte boundary with the bit pattern 
 * 1 0* (64-bit count of bits processed, MSB-first)
 * Returns the newly allocated string of the hash.
 */
static char *MD5Final(MD5Context *ctx)
{
	char *retval;
	int i;
	int count = ctx->bytes[0] & 0x3f;	/* Number of bytes in ctx->in */
	md5byte *p = (md5byte *)ctx->in + count;
	u_int8_t	*bytes;

	/* Set the first char of padding to 0x80.  There is always room. */
	*p++ = 0x80;

	/* Bytes of padding needed to make 56 bytes (-8..55) */
	count = 56 - 1 - count;

	if (count < 0) {	/* Padding forces an extra block */
		memset(p, 0, count + 8);
		byteSwap(ctx->in, 16);
		MD5Transform(ctx->buf, ctx->in);
		p = (md5byte *)ctx->in;
		count = 56;
	}
	memset(p, 0, count);
	byteSwap(ctx->in, 14);

	/* Append length in bits and transform */
	ctx->in[14] = ctx->bytes[0] << 3;
	ctx->in[15] = ctx->bytes[1] << 3 | ctx->bytes[0] >> 29;
	MD5Transform(ctx->buf, ctx->in);

	byteSwap(ctx->buf, 4);

	retval = my_malloc(33);
	bytes = (u_int8_t *) ctx->buf;
	for (i = 0; i < 16; i++)
		sprintf(retval + (i * 2), "%02x", bytes[i]);
	retval[32] = '\0';
	
	return retval;
}

# ifndef ASM_MD5

/* The four core functions - F1 is optimized somewhat */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

/* This is the central step in the MD5 algorithm. */
#define MD5STEP(f,w,x,y,z,in,s) \
	 (w += f(x,y,z) + in, w = (w<<s | w>>(32-s)) + x)

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to
 * reflect the addition of 16 longwords of new data.  MD5Update blocks
 * the data and converts bytes into longwords for this routine.
 */
static void MD5Transform(u_int32_t buf[4], u_int32_t const in[16])
{
	register u_int32_t a, b, c, d;

	a = buf[0];
	b = buf[1];
	c = buf[2];
	d = buf[3];

	MD5STEP(F1, a, b, c, d, in[0] + 0xd76aa478, 7);
	MD5STEP(F1, d, a, b, c, in[1] + 0xe8c7b756, 12);
	MD5STEP(F1, c, d, a, b, in[2] + 0x242070db, 17);
	MD5STEP(F1, b, c, d, a, in[3] + 0xc1bdceee, 22);
	MD5STEP(F1, a, b, c, d, in[4] + 0xf57c0faf, 7);
	MD5STEP(F1, d, a, b, c, in[5] + 0x4787c62a, 12);
	MD5STEP(F1, c, d, a, b, in[6] + 0xa8304613, 17);
	MD5STEP(F1, b, c, d, a, in[7] + 0xfd469501, 22);
	MD5STEP(F1, a, b, c, d, in[8] + 0x698098d8, 7);
	MD5STEP(F1, d, a, b, c, in[9] + 0x8b44f7af, 12);
	MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1, 17);
	MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7be, 22);
	MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122, 7);
	MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193, 12);
	MD5STEP(F1, c, d, a, b, in[14] + 0xa679438e, 17);
	MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821, 22);

	MD5STEP(F2, a, b, c, d, in[1] + 0xf61e2562, 5);
	MD5STEP(F2, d, a, b, c, in[6] + 0xc040b340, 9);
	MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51, 14);
	MD5STEP(F2, b, c, d, a, in[0] + 0xe9b6c7aa, 20);
	MD5STEP(F2, a, b, c, d, in[5] + 0xd62f105d, 5);
	MD5STEP(F2, d, a, b, c, in[10] + 0x02441453, 9);
	MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681, 14);
	MD5STEP(F2, b, c, d, a, in[4] + 0xe7d3fbc8, 20);
	MD5STEP(F2, a, b, c, d, in[9] + 0x21e1cde6, 5);
	MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6, 9);
	MD5STEP(F2, c, d, a, b, in[3] + 0xf4d50d87, 14);
	MD5STEP(F2, b, c, d, a, in[8] + 0x455a14ed, 20);
	MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905, 5);
	MD5STEP(F2, d, a, b, c, in[2] + 0xfcefa3f8, 9);
	MD5STEP(F2, c, d, a, b, in[7] + 0x676f02d9, 14);
	MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8a, 20);

	MD5STEP(F3, a, b, c, d, in[5] + 0xfffa3942, 4);
	MD5STEP(F3, d, a, b, c, in[8] + 0x8771f681, 11);
	MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122, 16);
	MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380c, 23);
	MD5STEP(F3, a, b, c, d, in[1] + 0xa4beea44, 4);
	MD5STEP(F3, d, a, b, c, in[4] + 0x4bdecfa9, 11);
	MD5STEP(F3, c, d, a, b, in[7] + 0xf6bb4b60, 16);
	MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70, 23);
	MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6, 4);
	MD5STEP(F3, d, a, b, c, in[0] + 0xeaa127fa, 11);
	MD5STEP(F3, c, d, a, b, in[3] + 0xd4ef3085, 16);
	MD5STEP(F3, b, c, d, a, in[6] + 0x04881d05, 23);
	MD5STEP(F3, a, b, c, d, in[9] + 0xd9d4d039, 4);
	MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5, 11);
	MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8, 16);
	MD5STEP(F3, b, c, d, a, in[2] + 0xc4ac5665, 23);

	MD5STEP(F4, a, b, c, d, in[0] + 0xf4292244, 6);
	MD5STEP(F4, d, a, b, c, in[7] + 0x432aff97, 10);
	MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7, 15);
	MD5STEP(F4, b, c, d, a, in[5] + 0xfc93a039, 21);
	MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3, 6);
	MD5STEP(F4, d, a, b, c, in[3] + 0x8f0ccc92, 10);
	MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47d, 15);
	MD5STEP(F4, b, c, d, a, in[1] + 0x85845dd1, 21);
	MD5STEP(F4, a, b, c, d, in[8] + 0x6fa87e4f, 6);
	MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0, 10);
	MD5STEP(F4, c, d, a, b, in[6] + 0xa3014314, 15);
	MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1, 21);
	MD5STEP(F4, a, b, c, d, in[4] + 0xf7537e82, 6);
	MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235, 10);
	MD5STEP(F4, c, d, a, b, in[2] + 0x2ad7d2bb, 15);
	MD5STEP(F4, b, c, d, a, in[9] + 0xeb86d391, 21);

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}

# endif /* ASM_MD5 */

/* Calculate the MD5 sum for 'path' and compare it with 'md5'.
 * Returns 1 if they match, 0 if not.
 */
int check_md5(const char *path, const char *md5)
{
	char *real;
	MD5Context ctx;
	int retval;
	char buffer[512];
	int fd;

	MD5Init(&ctx);

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		perror("open");
		return 0;
	}

	while (1) {
		int got;

		got = read(fd, buffer, sizeof(buffer));
		if (got < 0)
			perror("read");
		if (got == 0)
			break;
		MD5Update(&ctx, buffer, got);
	}

	if (close(fd))
		perror("close");
	
	real = MD5Final(&ctx);

	retval = strcmp(real, md5) == 0;
	free(real);

	return retval;
}

