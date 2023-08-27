// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
    struct run *next;
};

struct {
    struct spinlock lock;
    struct run *freelist;
    int count;
} kmem[NCPU];

#define block ((PHYSTOP - KERNBASE) / NCPU)
#define start_ad(a) (KERNBASE + a * block)
#define end_ad(a) (start_ad(a) + block)

struct spinlock b_lock;

void kinit() {
    push_off();
    for (int i = 0; i < NCPU; i++) {
        initlock(&kmem[i].lock, "kmem");
        kmem[i].count = 0;
    }
    initlock(&b_lock, "borrow");

    freerange(end, (void *)PHYSTOP);
    pop_off();
}

void freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
        kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa) {
    struct run *r;
    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
    r = (struct run *)pa;

    push_off();

    int id = cpuid();
    acquire(&kmem[id].lock);
    r->next = kmem[id].freelist;
    kmem[id].freelist = r;
    kmem[id].count++;
    release(&kmem[id].lock);

    pop_off();
}

void *borrow(int id) {
    for (int i = 0; i < NCPU; i++) {
        acquire(&kmem[i].lock);
        if (kmem[i].count != 0) {
            int b_count = (kmem[i].count + 1) / 2;
            struct run *r = kmem[i].freelist;
            struct run *temp = r;
            for (int i = 0; i < b_count - 1; i++) {
                temp = temp->next;
            }
            kmem[i].freelist = temp->next;
            kmem[i].count -= b_count;
            acquire(&kmem[id].lock);
            if (b_count != 1) {
                temp->next = kmem[id].freelist;
                kmem[id].freelist = r->next;
                kmem[id].count += b_count - 1;
            }
            release(&kmem[id].lock);
            release(&kmem[i].lock);
            return r;
        }
        release(&kmem[i].lock);
    }
    return 0;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void) {
    struct run *r;

    push_off();
    int id = cpuid();
    acquire(&kmem[id].lock);
    r = kmem[id].freelist;
    if (r) {
        kmem[id].freelist = r->next;
        kmem[id].count--;
    }
    release(&kmem[id].lock);
    if (!r) {
        acquire(&b_lock);
        r = borrow(id);
        release(&b_lock);
    }
    if (r)
        memset((char *)r, 5, PGSIZE); // fill with junk
    pop_off();
    return (void *)r;
}
