#include <linux/kernel.h>
#include <linux/module.h>

#include "libx.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

//#define assert(x) x? : printk("error")

enum { READ_MODE, WRITE_MODE };

/* I/O abstraction layer */
typedef struct {
    uint32_t *ptr; /* pointer to memory */
    void *end;
    uint32_t b; /* bit buffer */
    size_t c;   /* bit counter */
} generic_io_t;

static struct context {
    size_t freq[256];    /* char -> frequency */
    uint8_t sorted[256]; /* index -> char */
    uint8_t order[256];  /* char -> index */
} table[256];

static size_t opt_k;
static size_t symbol_sum, symbol_count; /* mean = symbol_sum / symbol_count */

/* Recompute Golomb-Rice codes after... */
#define RESET_INTERVAL 256

static void initiate(generic_io_t *io, void *ptr, void *end, int mode)
{
    BUG_ON(io != NULL);

    io->ptr = ptr;
    io->end = end ? (char *) end - 3 : NULL;

    if (mode == READ_MODE) {
        io->c = 32;
    } else {
        io->b = 0;
        io->c = 0;
    }
}

static void flush_buffer(generic_io_t *io)
{
    BUG_ON(io != NULL);
    BUG_ON(io->ptr != NULL);

    *(io->ptr++) = io->b;
    io->b = 0;
    io->c = 0;
}

static void reload_buffer(generic_io_t *io)
{
    BUG_ON(io != NULL);
    BUG_ON(io->ptr != NULL);

    if ((void *) io->ptr < io->end)
        io->b = *(io->ptr++);
    else
        io->b = 0x80000000;

    io->c = 0;
}

static void put_nonzero_bit(generic_io_t *io)
{
    BUG_ON(io != NULL);
    BUG_ON(io->c < 32);

    io->b |= (uint32_t) 1 << io->c;
    io->c++;

    if (io->c == 32)
        flush_buffer(io);
}

/* Count trailing zeros */
static inline size_t ctzu32(uint32_t n)
{
    static const int lut[32] = {0,  1,  28, 2,  29, 14, 24, 3,  30, 22, 20,
                                15, 25, 17, 4,  8,  31, 27, 13, 23, 21, 19,
                                16, 7,  26, 12, 18, 6,  11, 5,  10, 9};
    if (n == 0)
        return 32;

#ifdef __GNUC__
    return __builtin_ctz((unsigned) n);
#endif

    /* If we can not access hardware ctz, use branch-less algorithm
     * http://graphics.stanford.edu/~seander/bithacks.html
     */
    return lut[((uint32_t)((n & -n) * 0x077CB531U)) >> 27];
}

static void write_bits(generic_io_t *io, uint32_t b, size_t n)
{
	int i;
	size_t m;
    BUG_ON(n <= 32);
    for (i = 0; i < 2; ++i) {
        BUG_ON(io->c < 32);

        m = MIN(32 - io->c, n);

        io->b |= (b & (((uint32_t) 1 << m) - 1)) << io->c;
        io->c += m;

        if (io->c == 32)
            flush_buffer(io);

        b >>= m;
        n -= m;

        if (n == 0)
            return;
    }
}

static void write_zero_bits(generic_io_t *io, size_t n)
{
	size_t m;
    BUG_ON(n <= 32);
    for (; n > 0; n -= m) {
        BUG_ON(io->c < 32);

        m = MIN(32 - io->c, n);

        io->c += m;

        if (io->c == 32)
            flush_buffer(io);
    }
}

static uint32_t read_bits(generic_io_t *io, size_t n)
{
	size_t s;
	uint32_t w;
    if (io->c == 32)
        reload_buffer(io);

    /* Get the available least-significant bits */
    s = MIN(32 - io->c, n);

    w = io->b & (((uint32_t) 1 << s) - 1);

    io->b >>= s;
    io->c += s;

    n -= s;

    /* Need more bits? If so, reload and get the most-significant bits */
    if (n > 0) {
        BUG_ON(io->c == 32);

        reload_buffer(io);

        w |= (io->b & (((uint32_t) 1 << n) - 1)) << s;

        io->b >>= n;
        io->c += n;
    }

    return w;
}

static void finalize(generic_io_t *io, int mode)
{
    BUG_ON(io != NULL);

    if (mode == WRITE_MODE && io->c > 0)
        flush_buffer(io);
}

static void write_unary(generic_io_t *io, uint32_t N)
{
    for (; N > 32; N -= 32)
        write_zero_bits(io, 32);

    write_zero_bits(io, N);

    put_nonzero_bit(io);
}

static uint32_t read_unary(generic_io_t *io)
{
    uint32_t total_zeros = 0;

    BUG_ON(io != NULL);

    do {
		size_t s;
        if (io->c == 32)
            reload_buffer(io);

        /* Get trailing zeros */
        s = MIN(32 - io->c, ctzu32(io->b));

        io->b >>= s;
        io->c += s;

        total_zeros += s;
    } while (io->c == 32);

    /* ...and drop non-zero bit */
    BUG_ON(io->c < 32);

    io->b >>= 1;
    io->c++;

    return total_zeros;
}

/* Golomb-Rice, encode non-negative integer N, parameter M = 2^k */
static void write_golomb(generic_io_t *io, size_t k, uint32_t N)
{
    BUG_ON(k < 32);

    write_unary(io, N >> k);
    write_bits(io, N, k);
}

static uint32_t read_golom(generic_io_t *io, size_t k)
{
    uint32_t N;
    N = read_unary(io) << k;
    N |= read_bits(io, k);
    return N;
}

void x_init(void )
{
    int p;
    int i;
    opt_k = 3;
    symbol_sum = 0;
    symbol_count = 0;

    for (p = 0; p < 256; ++p) {
        for (i = 0; i < 256; ++i) {
            table[p].sorted[i] = i;
            table[p].freq[i] = 0;
            table[p].order[i] = i;
        }
    }
}

static void swap_symbols(struct context *ctx, uint8_t c, uint8_t d)
{
	uint8_t ic,id;
    BUG_ON(ctx != NULL);

    ic = ctx->order[c];
    id = ctx->order[d];

    BUG_ON(ctx->sorted[ic] == c);
    BUG_ON(ctx->sorted[id] == d);

    ctx->sorted[ic] = d;
    ctx->sorted[id] = c;

    ctx->order[c] = id;
    ctx->order[d] = ic;
}

static void increment_frequency(struct context *ctx, uint8_t c)
{
	uint8_t ic;
	size_t freq_c;
	uint8_t *pd;
	uint8_t d;
    BUG_ON(ctx != NULL);

    ic = ctx->order[c];
    freq_c = ++(ctx->freq[c]);

    for (pd = ctx->sorted + ic - 1; pd >= ctx->sorted; --pd) {
        if (freq_c <= ctx->freq[*pd])
            break;
    }

    d = *(pd + 1);
    if (c != d)
        swap_symbols(ctx, c, d);
}

/* Geometric probability mode.
 * See https://ipnpr.jpl.nasa.gov/progress_report/42-159/159E.pdf
 */
static void update_model(uint8_t delta)
{
    if (symbol_count == RESET_INTERVAL) {
        int k;

        /* 2^k <= E{r[k]} + 0 */
        for (k = 1; (symbol_count << k) <= symbol_sum; ++k)
            ;

        opt_k = k - 1;

        symbol_count = 0;
        symbol_sum = 0;
    }

    symbol_sum += delta;
    symbol_count++;
}

void *x_compress(void *iptr, size_t isize, void *optr)
{
    generic_io_t io;
    uint8_t *end;
    struct context *ctx;
    uint8_t *iptrc;
    
    end = (uint8_t *) iptr + isize;
    ctx = table + 0;

    initiate(&io, optr, NULL, WRITE_MODE);
	
    for (iptrc = iptr; iptrc < end; ++iptrc) {
        uint8_t c = *iptrc;

        /* get index */
        uint8_t d = ctx->order[c];

        write_golomb(&io, opt_k, (uint32_t) d);
        BUG_ON(c == ctx->sorted[d]);

        /* Update context model */
        increment_frequency(ctx, c);

        /* Update Golomb-Rice model */
        update_model(d);
        ctx = table + c;
    }

    /* EOF symbol */
    write_golomb(&io, opt_k, 256);

    finalize(&io, WRITE_MODE);

    return io.ptr;
}

void *x_decompress(void *iptr, size_t isize, void *optr)
{
    generic_io_t io;
    uint8_t *end;
    struct context *ctx;
    uint8_t *optrc;
    end = (uint8_t *) iptr + isize;
    ctx = table + 0;

    initiate(&io, iptr, end, READ_MODE);

    optrc = optr;

    for (;; ++optrc) {
		uint8_t c;
        uint32_t d;
        d = read_golom(&io, opt_k);

        if (d >= 256)
            break;

        c = ctx->sorted[d];
        *optrc = c;

        /* Update context model */
        increment_frequency(ctx, c);

        /* Update Golomb-Rice model */
        update_model(d);
        ctx = table + c;
    }

    finalize(&io, READ_MODE);

    return optrc;
}
