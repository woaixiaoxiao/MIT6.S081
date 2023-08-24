// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include <stdlib.h>

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
    struct run *next;
};

struct {
    struct spinlock lock;
    struct run *freelist;
} kmem;

// KERNBASE 不是 end
#define PA2INDEX(pa) ((((uint64)pa) - KERNBASE) / PGSIZE)

struct {
    struct spinlock lock;
    int count[PA2INDEX(PHYSTOP)];
} ref_count;

#define PA2REFCOUNT(pa) (ref_count.count[PA2INDEX(pa)])
#define PAINITRC(pa) (ref_count.count[PA2INDEX(pa)] = 1)
#define PADEC(pa) (ref_count.count[PA2INDEX(pa)]--)
#define PAINC(pa) (ref_count.count[PA2INDEX(pa)]++)

void kinit() {
    initlock(&kmem.lock, "kmem");
    // 初始化计数数组的锁
    initlock(&ref_count.lock, "ref_count");
    freerange(end, (void *)PHYSTOP);
}

int flag = 1;
void freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
        kfree(p);
    flag = 0;
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa) {
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

    acquire(&ref_count.lock);
    if (!flag) {
        PADEC(pa);
    }
    if (PA2REFCOUNT(pa) == 0) {
        // Fill with junk to catch dangling refs.
        memset(pa, 1, PGSIZE);

        r = (struct run *)pa;

        acquire(&kmem.lock);
        r->next = kmem.freelist;
        kmem.freelist = r;
        release(&kmem.lock);
    }
    // 必须放在最后，防止被释放两次
    release(&ref_count.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void) {
    struct run *r;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r)
        kmem.freelist = r->next;
    release(&kmem.lock);

    if (r) {
        memset((char *)r, 5, PGSIZE); // fill with junk
        // 初始化这个物理地址的引用数
        PAINITRC((char *)r);
    }
    return (void *)r;
}

// 发生了对cow页面的写操作，必须要分配一个物理页面了
void *kcopy(void *pa) {
    acquire(&ref_count.lock);
    // 如果自己就是唯一的拥有者了，那么就不用申请页面，直接用就完事了
    if (PA2REFCOUNT(pa) == 1) {
        release(&ref_count.lock);
        return pa;
    }
    void *npa = kalloc();
    // 没有可用页面，返回0
    if (npa == 0) {
        release(&ref_count.lock);
        return NULL;
    }
    // 将当前页面的计数减1，并复制新的页面
    PADEC(pa);
    memmove(npa, pa, PGSIZE);
    release(&ref_count.lock);
    return npa;
}

// 给某个页面增加一个计数
void kinc(void *pa) {
    acquire(&ref_count.lock);
    PAINC(pa);
    release(&ref_count.lock);
}