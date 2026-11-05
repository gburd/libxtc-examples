/*-
 * 16_l2book/gorand.h -- Go's math/rand, in C (see gorand.c for the
 * license: the algorithm and its table are The Go Authors').
 *
 *	The console market draws from this so a seed replays the market
 *	intrepidkarthi's console shows for the same seed: same generator,
 *	same draws, same orders.  Pure C, no libc, no allocation.
 */
#ifndef L2BOOK_GORAND_H
#define L2BOOK_GORAND_H

#include <stdint.h>

#define GORAND_LEN 607

typedef struct {
	int		tap, feed;
	uint64_t	vec[GORAND_LEN];
} gorand_t;

/* rand.NewSource(seed). */
void	gorand_seed(gorand_t *g, int64_t seed);
/* (*Rand).Int63, .Int31n (Intn for n < 2^31), .Float64. */
int64_t	gorand_int63(gorand_t *g);
int32_t	gorand_int31n(gorand_t *g, int32_t n);
double	gorand_float64(gorand_t *g);

#endif /* L2BOOK_GORAND_H */
