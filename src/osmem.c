// SPDX-License-Identifier: BSD-3-Clause

#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "osmem.h"
#include "block_meta.h"

#define ALIGNMENT	8
#define ALIGN(size)	(((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define META_SIZE	ALIGN(sizeof(struct block_meta))
#define MMAP_THRESHOLD	(128 * 1024)
#define MIN_SPLIT	(META_SIZE + ALIGN(1))

/* Список блоків на heap (лише brk-блоки, впорядковані за адресою) */
static struct block_meta *heap_head;
static int heap_preallocated;

static inline void *payload(struct block_meta *b)
{
	return (char *)b + META_SIZE;
}

static inline struct block_meta *meta(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

static struct block_meta *heap_last(void)
{
	struct block_meta *b = heap_head;

	while (b && b->next)
		b = b->next;
	return b;
}

/* Об'єднує блок з наступним (обидва мають бути суміжні). */
static void merge_with_next(struct block_meta *b)
{
	struct block_meta *n = b->next;

	b->size += META_SIZE + n->size;
	b->next = n->next;
	if (n->next)
		n->next->prev = b;
}

/* Злиття всіх сусідніх вільних блоків. */
static void coalesce_all(void)
{
	struct block_meta *b = heap_head;

	while (b && b->next) {
		if (b->status == STATUS_FREE && b->next->status == STATUS_FREE)
			merge_with_next(b);
		else
			b = b->next;
	}
}

/* Розбиває блок, якщо залишку вистачає на новий блок (метадані + 1 байт). */
static void split_block(struct block_meta *b, size_t size)
{
	if (b->size < size + MIN_SPLIT)
		return;

	struct block_meta *rest = (struct block_meta *)((char *)payload(b) + size);

	rest->size = b->size - size - META_SIZE;
	rest->status = STATUS_FREE;
	rest->prev = b;
	rest->next = b->next;
	if (b->next)
		b->next->prev = rest;
	b->next = rest;
	b->size = size;
}

/* Find best: найменший вільний блок, у який поміщається size. */
static struct block_meta *find_best(size_t size)
{
	struct block_meta *best = NULL;

	for (struct block_meta *b = heap_head; b; b = b->next)
		if (b->status == STATUS_FREE && b->size >= size &&
		    (!best || b->size < best->size))
			best = b;
	return best;
}

static void *sbrk_checked(size_t increment)
{
	void *p = sbrk(increment);

	DIE(p == (void *)-1, "sbrk");
	return p;
}

static struct block_meta *alloc_mapped(size_t size)
{
	void *p = mmap(NULL, META_SIZE + size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	DIE(p == MAP_FAILED, "mmap");

	struct block_meta *b = p;

	b->size = size;
	b->status = STATUS_MAPPED;
	b->prev = NULL;
	b->next = NULL;
	return b;
}

/* Виділення блоку на heap для вирівняного розміру size. */
static struct block_meta *alloc_heap(size_t size)
{
	struct block_meta *b;

	if (!heap_preallocated) {
		/* Попереднє виділення 128 КБ при першому використанні heap */
		heap_preallocated = 1;
		b = sbrk_checked(MMAP_THRESHOLD);
		b->size = MMAP_THRESHOLD - META_SIZE;
		b->status = STATUS_FREE;
		b->prev = NULL;
		b->next = NULL;
		heap_head = b;
	}

	coalesce_all();
	b = find_best(size);
	if (b) {
		split_block(b, size);
		b->status = STATUS_ALLOC;
		return b;
	}

	struct block_meta *last = heap_last();

	if (last && last->status == STATUS_FREE) {
		/* Розширюємо останній вільний блок */
		sbrk_checked(size - last->size);
		last->size = size;
		last->status = STATUS_ALLOC;
		return last;
	}

	b = sbrk_checked(META_SIZE + size);
	b->size = size;
	b->status = STATUS_ALLOC;
	b->prev = last;
	b->next = NULL;
	if (last)
		last->next = b;
	else
		heap_head = b;
	return b;
}

static void *do_alloc(size_t size, size_t threshold)
{
	if (size == 0)
		return NULL;

	size = ALIGN(size);
	if (META_SIZE + size >= threshold)
		return payload(alloc_mapped(size));
	return payload(alloc_heap(size));
}

void *os_malloc(size_t size)
{
	return do_alloc(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	if (!ptr)
		return;

	struct block_meta *b = meta(ptr);

	if (b->status == STATUS_MAPPED) {
		int ret = munmap(b, META_SIZE + b->size);

		DIE(ret == -1, "munmap");
	} else if (b->status == STATUS_ALLOC) {
		b->status = STATUS_FREE;
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;
	if (nmemb > (size_t)-1 / size)
		return NULL;

	size_t total = nmemb * size;
	void *ptr = do_alloc(total, (size_t)getpagesize());

	memset(ptr, 0, total);
	return ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	if (!ptr)
		return os_malloc(size);
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *b = meta(ptr);

	if (b->status == STATUS_FREE)
		return NULL;

	size_t new_size = ALIGN(size);

	/* Блок або новий розмір працюють через mmap: виділити, скопіювати, звільнити */
	if (b->status == STATUS_MAPPED || META_SIZE + new_size >= MMAP_THRESHOLD) {
		void *np = os_malloc(size);

		memcpy(np, ptr, b->size < new_size ? b->size : new_size);
		os_free(ptr);
		return np;
	}

	/* Зменшення (або той самий розмір) */
	if (new_size <= b->size) {
		split_block(b, new_size);
		return ptr;
	}

	/* Останній блок на heap: розширюємо через sbrk */
	if (!b->next) {
		sbrk_checked(new_size - b->size);
		b->size = new_size;
		return ptr;
	}

	/* Зливаємо з наступними вільними блоками по одному */
	while (b->next && b->next->status == STATUS_FREE) {
		merge_with_next(b);
		if (b->size >= new_size) {
			split_block(b, new_size);
			return ptr;
		}
	}

	/* Якщо після злиття блок став останнім — розширюємо його */
	if (!b->next) {
		sbrk_checked(new_size - b->size);
		b->size = new_size;
		return ptr;
	}

	void *np = os_malloc(size);

	memcpy(np, ptr, b->size);
	os_free(ptr);
	return np;
}
