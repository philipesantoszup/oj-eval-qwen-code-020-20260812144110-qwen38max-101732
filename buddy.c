#include <stddef.h>
#include <stdlib.h>

#include "buddy.h"

#define PAGE_SIZE 4096UL
#define MAX_RANK 16
#define MAX_RANK_PAGES (1UL << (MAX_RANK - 1)) /* 2^15 pages per block */

typedef unsigned long long bm_word_t;
#define BM_BITS ((long)(sizeof(bm_word_t) * 8))

/*
 * State is kept as two bitmaps per rank.  A block of rank r covers
 * 2^(r-1) consecutive pages and is identified by its index b within
 * that rank (it spans pages [b * 2^(r-1), (b+1) * 2^(r-1))).
 *
 *   free_bits[r][b]  - block is a maximal free block (buddy lists)
 *   alloc_bits[r][b] - block is currently allocated
 *
 * Invariant: for every managed page, exactly one block on the path from
 * the page (rank 1) up to rank MAX_RANK carries a bit, and that bit is
 * the block's current state.  Ancestor/descendant blocks carry no bits.
 */
static char *pool_start;
static long pool_pages;
static long rank_blocks[MAX_RANK + 1];           /* block slots per rank */
static long free_count[MAX_RANK + 1];            /* free blocks per rank */
static bm_word_t *free_bits[MAX_RANK + 1];
static bm_word_t *alloc_bits[MAX_RANK + 1];

static long bm_words(long nbits) {
    return (nbits + BM_BITS - 1) / BM_BITS;
}

static int bm_test(const bm_word_t *bm, long b) {
    return (int)((bm[b / BM_BITS] >> (b % BM_BITS)) & 1ULL);
}

static void bm_set(bm_word_t *bm, long b) {
    bm[b / BM_BITS] |= 1ULL << (b % BM_BITS);
}

static void bm_clear(bm_word_t *bm, long b) {
    bm[b / BM_BITS] &= ~(1ULL << (b % BM_BITS));
}

/* lowest set bit index, or -1 */
static long bm_first(const bm_word_t *bm, long nbits) {
    long words = bm_words(nbits);
    for (long w = 0; w < words; w++) {
        if (bm[w]) {
            long b = (long)w * BM_BITS + __builtin_ctzll(bm[w]);
            return b < nbits ? b : -1;
        }
    }
    return -1;
}

/* Validate that p is a page boundary inside the pool; returns the page
 * index, or -1 when invalid. */
static long page_index_of(const void *p) {
    long off;

    if (p == NULL || pool_start == NULL)
        return -1;
    off = (const char *)p - pool_start;
    if (off < 0 || off >= pool_pages * (long)PAGE_SIZE)
        return -1;
    if ((off & (long)(PAGE_SIZE - 1)) != 0)
        return -1;
    return off / (long)PAGE_SIZE;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0)
        return -EINVAL;

    pool_start = (char *)p;
    pool_pages = (long)pgcount;

    for (int r = 1; r <= MAX_RANK; r++) {
        long pages_per_block = 1L << (r - 1);
        size_t bytes;

        free(free_bits[r]);
        free(alloc_bits[r]);
        free_bits[r] = NULL;
        alloc_bits[r] = NULL;
        free_count[r] = 0;

        rank_blocks[r] = (pool_pages + pages_per_block - 1) / pages_per_block;
        bytes = (size_t)bm_words(rank_blocks[r]) * sizeof(bm_word_t);
        free_bits[r] = (bm_word_t *)calloc(1, bytes);
        alloc_bits[r] = (bm_word_t *)calloc(1, bytes);
        if (free_bits[r] == NULL || alloc_bits[r] == NULL)
            return -EINVAL;
    }

    /* Decompose the pool into maximal aligned blocks (rank <= MAX_RANK).
     * For a power-of-2 sized pool this is exactly one rank-16-style root
     * block; other sizes are covered without wasting any pages. */
    long off = 0;
    while (off < pool_pages) {
        long rem = pool_pages - off;
        long s = 1;
        long lb;
        int r = 1;

        while ((s << 1) <= rem && (s << 1) <= (long)MAX_RANK_PAGES)
            s <<= 1;
        if (off != 0) {
            lb = off & (-off); /* largest power of 2 dividing off */
            if (lb < s)
                s = lb;
        }
        while ((1L << r) <= s)
            r++;
        bm_set(free_bits[r], off >> (r - 1));
        free_count[r]++;
        off += s;
    }

    return OK;
}

void *alloc_pages(int rank) {
    int r;
    long b = -1;

    if (rank < 1 || rank > MAX_RANK)
        return ERR_PTR(-EINVAL);
    if (pool_start == NULL)
        return ERR_PTR(-ENOSPC);

    /* Smallest rank that still has free blocks; within that rank take
     * the lowest-addressed block so allocations are first-fit. */
    for (r = rank; r <= MAX_RANK; r++) {
        if (free_count[r] > 0) {
            b = bm_first(free_bits[r], rank_blocks[r]);
            break;
        }
    }
    if (b < 0)
        return ERR_PTR(-ENOSPC);

    bm_clear(free_bits[r], b);
    free_count[r]--;

    /* Split down to the requested rank, releasing right halves. */
    while (r > rank) {
        r--;
        b <<= 1;
        bm_set(free_bits[r], b + 1);
        free_count[r]++;
    }

    bm_set(alloc_bits[r], b);
    return (void *)(pool_start +
                    ((unsigned long)b << (rank - 1)) * PAGE_SIZE);
}

int return_pages(void *p) {
    long i = page_index_of(p);
    int r;
    long b = 0;

    if (i < 0)
        return -EINVAL;

    /* Find the smallest block containing the page that carries state. */
    for (r = 1; r <= MAX_RANK; r++) {
        b = i >> (r - 1);
        if (bm_test(alloc_bits[r], b))
            break;
        if (bm_test(free_bits[r], b))
            return -EINVAL; /* already free (double return) */
    }
    if (r > MAX_RANK)
        return -EINVAL;

    bm_clear(alloc_bits[r], b);

    /* Merge with free buddies as far up the tree as possible. */
    while (r < MAX_RANK) {
        long buddy = b ^ 1;

        if (buddy >= rank_blocks[r] || !bm_test(free_bits[r], buddy))
            break;
        bm_clear(free_bits[r], buddy);
        free_count[r]--;
        b >>= 1;
        r++;
    }

    bm_set(free_bits[r], b);
    free_count[r]++;
    return OK;
}

int query_ranks(void *p) {
    long i = page_index_of(p);

    if (i < 0)
        return -EINVAL;

    for (int r = 1; r <= MAX_RANK; r++) {
        long b = i >> (r - 1);

        if (bm_test(alloc_bits[r], b) || bm_test(free_bits[r], b))
            return r;
    }
    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK)
        return -EINVAL;
    if (pool_start == NULL)
        return 0;
    return (int)free_count[rank];
}
