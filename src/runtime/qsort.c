#include "qsort.h"
/*
quick sort
Our interface to qsort_r looks like this:
    void dew_qsort(
        void* base, usize nmemb, usize size,
        int(*cmp)(const void* x, const void* y, void* ctx),
        void* ctx);

qsort_r is not a libc standard function; its signature differs:
    ISO TR 24731-1
        errno_t qsort_s(
            void* base, usize nmemb, usize size,
            int (*cmp)(const void* x, const void* y, void* ctx),
            void* ctx);
    GNU libc, musl libc (and probably all other Linux-oriented libcs)
        void qsort_r(
            void* base, usize nmemb, usize size,
            int (*cmp)(const void* x, const void* y, void* ctx),
            void* ctx);
    Microsoft Windows
        void qsort_s(
            void* base, usize nmemb, usize size,
            int (__cdecl*cmp)(void* ctx, const void* x, const void* y),  ← ctx position
            void* ctx);
    BSD
        void qsort_r(
            void* base, usize nmemb, usize size,
            void* ctx,                                             ← ctx position
            int (*cmp)(void* ctx, const void* x, const void* y));  ← cmp & ctx position

For this reason we use qsort from musl, licensed as follows:

    Copyright (C) 2011 by Valentin Ochs

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.

Minor changes by Rich Felker for integration in musl, 2011-04-27.

Minor changes by Rasmus Andersson, 2022-02-27.
    - Comparison with matching signed/unsigned integers
    - Use of __builtin_ctz (rsm_ctz) instead of a_ctz_{32,64} asm implementations

Smoothsort, an adaptive variant of Heapsort.  Memory usage: O(1).
    Run time: Worst case O(n log n), close to O(n) in the mostly-sorted case.

*/

typedef usize size_t;

static inline int pntz(size_t p[2]) {
    int r = dew_ctz(p[0] - 1);
    if(r != 0 || (r = 8*sizeof(size_t) + dew_ctz(p[1])) != 8*sizeof(size_t)) {
        return r;
    }
    return 0;
}

static void cycle(size_t width, u8* ar[], int n) {
    u8 tmp[256];
    size_t l;
    int i;

    if(n < 2) {
        return;
    }

    ar[n] = tmp;
    while(width) {
        l = sizeof(tmp) < width ? sizeof(tmp) : width;
        memcpy(ar[n], ar[0], l);
        for(i = 0; i < n; i++) {
            memcpy(ar[i], ar[i + 1], l);
            ar[i] += l;
        }
        width -= l;
    }
}

/* shl() and shr() need n > 0 */
static inline void shl(size_t p[2], int n) {
    if(n >= (int)(8 * sizeof(size_t))) {
        n -= (int)(8 * sizeof(size_t));
        p[1] = p[0];
        p[0] = 0;
    }
    p[1] <<= n;
    p[1] |= p[0] >> (sizeof(size_t) * 8 - n);
    p[0] <<= n;
}

static inline void shr(size_t p[2], int n) {
    if(n >= (int)(8 * sizeof(size_t))) {
        n -= (int)(8 * sizeof(size_t));
        p[0] = p[1];
        p[1] = 0;
    }
    p[0] >>= n;
    p[0] |= p[1] << (sizeof(size_t) * 8 - n);
    p[1] >>= n;
}

static void sift(u8 *head, size_t width, dew_qsort_cmp cmp, void *arg, int pshift, size_t lp[]) {
    u8 *rt, *lf;
    u8 *ar[14 * sizeof(size_t) + 1];
    int i = 1;

    ar[0] = head;
    while(pshift > 1) {
        rt = head - width;
        lf = head - width - lp[pshift - 2];

        if(cmp(ar[0], lf, arg) >= 0 && cmp(ar[0], rt, arg) >= 0) {
            break;
        }
        if(cmp(lf, rt, arg) >= 0) {
            ar[i++] = lf;
            head = lf;
            pshift -= 1;
        } else {
            ar[i++] = rt;
            head = rt;
            pshift -= 2;
        }
    }
    cycle(width, ar, i);
}

static void trinkle(
    u8 *head,
    size_t width,
    dew_qsort_cmp cmp,
    void *arg,
    size_t pp[2],
    int pshift,
    int trusty,
    size_t lp[])
{
    u8 *stepson, *rt, *lf;
    size_t p[2];
    u8 *ar[14 * sizeof(size_t) + 1];
    int i = 1;
    int trail;

    p[0] = pp[0];
    p[1] = pp[1];

    ar[0] = head;
    while(p[0] != 1 || p[1] != 0) {
        stepson = head - lp[pshift];
        if(cmp(stepson, ar[0], arg) <= 0) {
            break;
        }
        if(!trusty && pshift > 1) {
            rt = head - width;
            lf = head - width - lp[pshift - 2];
            if(cmp(rt, stepson, arg) >= 0 || cmp(lf, stepson, arg) >= 0) {
                break;
            }
        }

        ar[i++] = stepson;
        head = stepson;
        trail = pntz(p);
        shr(p, trail);
        pshift += trail;
        trusty = 0;
    }
    if(!trusty) {
        cycle(width, ar, i);
        sift(head, width, cmp, arg, pshift, lp);
    }
}

void dew_qsort(void *base, size_t nel, size_t width, dew_qsort_cmp cmp, void* nullable arg) {
    size_t lp[12*sizeof(size_t)];
    size_t i, size = width * nel;
    u8 *head, *high;
    size_t p[2] = {1, 0};
    int pshift = 1;
    int trail;

    if (!size) return;

    head = base;
    high = head + size - width;

    /* Precompute Leonardo numbers, scaled by element width */
    for(lp[0]=lp[1]=width, i=2; (lp[i]=lp[i-2]+lp[i-1]+width) < size; i++);

    while(head < high) {
        if((p[0] & 3) == 3) {
            sift(head, width, cmp, arg, pshift, lp);
            shr(p, 2);
            pshift += 2;
        } else {
            if ((isize)lp[pshift - 1] >= (isize)(high - head)) {
                trinkle(head, width, cmp, arg, p, pshift, 0, lp);
            } else {
                sift(head, width, cmp, arg, pshift, lp);
            }

            if(pshift == 1) {
                shl(p, 1);
                pshift = 0;
            } else {
                shl(p, pshift - 1);
                pshift = 1;
            }
        }

        p[0] |= 1;
        head += width;
    }

    trinkle(head, width, cmp, arg, p, pshift, 0, lp);

    while(pshift != 1 || p[0] != 1 || p[1] != 0) {
        if(pshift <= 1) {
            trail = pntz(p);
            shr(p, trail);
            pshift += trail;
        } else {
            shl(p, 2);
            pshift -= 2;
            p[0] ^= 7;
            shr(p, 1);
            trinkle(head - lp[pshift] - width, width, cmp, arg, p, pshift + 1, 1, lp);
            shl(p, 1);
            p[0] |= 1;
            trinkle(head - width, width, cmp, arg, p, pshift, 1, lp);
        }
        head -= width;
    }
}
