#include "array.h"


void array_free(struct Array* a) {
	if (a->v != (u8*)a+sizeof(*a))
		free(a->v);
}


static void* nullable _array_resize(struct Array* a, u32 elemsize, u32 newcap) {
	assert(newcap >= a->len);
	if (a->cap != newcap) {
		usize newsize;
		if (check_mul_overflow((usize)newcap, (usize)elemsize, &newsize))
			return NULL;
		// dlog("resize array %p: %zu -> %zu B", a, (usize)elemsize*a->cap, newsize);
		void* newv;
		if (a->v == (u8*)a+sizeof(*a)) { // embedded
			newv = malloc(newsize);
			if (newv)
				memcpy(newv, a->v, (usize)elemsize * a->len);
		} else {
			newv = realloc(a->v, newsize);
		}
		if (!newv)
			return NULL;
		a->v = newv;
		a->cap = (u32)(newsize / (usize)elemsize);
	}
	return a->v + (usize)a->len*(usize)elemsize;
}


void* nullable _array_reserve(struct Array* a, u32 elemsize, u32 minavail) {
	u32 avail = a->cap - a->len;
	assert(avail < minavail);
	u32 extracap = minavail - avail;

	u32 newcap;
	if (a->cap == 0) {
		newcap = MAX(minavail, 64 / elemsize);
	} else {
		/*
		if (newcap > 0 && newcap < USIZE_MAX - sizeof(void*))
			newcap = ALIGN2(newcap, sizeof(void*));
		*/
		usize currsize = (usize)a->cap * (usize)elemsize;
		usize extrasize;
		if (check_mul_overflow((usize)extracap, (usize)elemsize, &extrasize))
			return NULL;
		if (currsize < 65536 && extrasize < 65536/2) {
			// double capacity until we hit 64KiB
			newcap = (a->cap >= extracap) ? a->cap * 2 : a->cap + extracap;
		} else {
			u32 addlcap = MAX(65536u / elemsize, CEIL_POW2(extracap));
			if (check_add_overflow(a->cap, addlcap, &newcap)) {
				// try adding exactly what is needed (extracap)
				if (check_add_overflow(a->cap, extracap, &newcap))
					return NULL;
			}
		}
	}

	assert(newcap - a->cap >= extracap);
	return _array_resize(a, elemsize, newcap);
}


bool array_append(struct Array* a, u32 elemsize, const void* elemv, u32 elemc) {
	void* p = array_reserve(a, elemsize, elemc);
	if (!p)
		return false;
	memcpy(p, elemv, (usize)elemsize*elemc);
	a->len += elemc;
	return true;
}
