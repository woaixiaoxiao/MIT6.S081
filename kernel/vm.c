#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

void init_kernel_pgtbl(pagetable_t pgtbl) {
    memset(pgtbl, 0, PGSIZE);

    // uart registers
    kvmmap(pgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

    // virtio mmio disk interface
    kvmmap(pgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

    // PLIC
    kvmmap(pgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

    // map kernel text executable and read-only.
    kvmmap(pgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

    // map kernel data and the physical RAM we'll make use of.
    kvmmap(pgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // map the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    kvmmap(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

pagetable_t new_kernel_pgtbl() {
    pagetable_t pgtbl = (pagetable_t)kalloc();
    init_kernel_pgtbl(pgtbl);
    return pgtbl;
}

/*
 * create a direct-map page table for the kernel.
 */
void kvminit() {
    kernel_pagetable = new_kernel_pgtbl();
    // CLINT
    kvmmap(kernel_pagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);
}

// void dfs_clone(pagetable_t old_pt, pagetable_t new_pt, int depth) {
//     for (int i = 0; i < 512; i++) {
//         pte_t *old_pte = &old_pt[i];
//         pte_t *new_pte = &new_pt[i];
//         // 如果这个pte有效，则需要复制
//         if (*old_pte & PTE_V) {
//             // 0或者1级页表
//             if (depth < 2) {
//                 // 为这下一级的某个页表申请一个页面
//                 pagetable_t child_pt = (pagetable_t)kalloc();
//                 *new_pte = PA2PTE(child_pt) | PTE_V;
//                 dfs_clone((pagetable_t)PTE2PA(*old_pte), (pagetable_t)PTE2PA(*new_pte), depth + 1);
//             } else {
//                 // 2级页表，就要复制真正的物理地址了
//                 *new_pte = *old_pte;
//             }
//         }
//     }
// }

// pagetable_t clone_k_pgtbl() {
//     pagetable_t new_k_pgtbl = (pagetable_t)kalloc();
//     dfs_clone(kernel_pagetable, new_k_pgtbl, 0);
//     return new_k_pgtbl;
// }

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart() {
    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.

// 根据页表，查询虚拟地址va对应的最终的页表项，如果在查询的过程中页表项不存在，那就创建页表项
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc) {
    if (va >= MAXVA)
        panic("walk");

    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
                return 0;
            memset(pagetable, 0, PGSIZE);
            *pte = PA2PTE(pagetable) | PTE_V;
        }
    }
    return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.

// 根据虚拟地址va去查页表，得到对应的物理地址，如果不存在这个映射，返回0
uint64
walkaddr(pagetable_t pagetable, uint64 va) {
    pte_t *pte;
    uint64 pa;

    if (va >= MAXVA)
        return 0;

    pte = walk(pagetable, va, 0);
    if (pte == 0)
        return 0;
    if ((*pte & PTE_V) == 0)
        return 0;
    if ((*pte & PTE_U) == 0)
        return 0;
    pa = PTE2PA(*pte);
    return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t pgtbl, uint64 va, uint64 pa, uint64 sz, int perm) {
    if (mappages(pgtbl, va, sz, pa, perm) != 0)
        panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(pagetable_t pgtbl, uint64 va) {
    uint64 off = va % PGSIZE;
    pte_t *pte;
    uint64 pa;

    pte = walk(pgtbl, va, 0);
    if (pte == 0)
        panic("kvmpa");
    if ((*pte & PTE_V) == 0)
        panic("kvmpa");
    pa = PTE2PA(*pte);
    return pa + off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.

// 将从va开始size字节的虚拟页面的映射都加入到页表中
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
    uint64 a, last;
    pte_t *pte;

    a = PGROUNDDOWN(va);
    last = PGROUNDDOWN(va + size - 1);
    for (;;) {
        if ((pte = walk(pagetable, a, 1)) == 0)
            return -1;
        if (*pte & PTE_V)
            panic("remap");
        *pte = PA2PTE(pa) | perm | PTE_V;
        if (a == last)
            break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.

// 移除从虚拟地址va开始的npages个页面的虚拟映射，即将页表项变成0，根据dofree判断是否需要释放物理页面
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
    uint64 a;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
        if ((pte = walk(pagetable, a, 0)) == 0)
            panic("uvmunmap: walk");
        if ((*pte & PTE_V) == 0)
            panic("uvmunmap: not mapped");
        if (PTE_FLAGS(*pte) == PTE_V)
            panic("uvmunmap: not a leaf");
        if (do_free) {
            uint64 pa = PTE2PA(*pte);
            kfree((void *)pa);
        }
        *pte = 0;
    }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate() {
    pagetable_t pagetable;
    pagetable = (pagetable_t)kalloc();
    if (pagetable == 0)
        return 0;
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.

// 将initcode加载到虚拟地址开始的第一个页面，首先通过mappages创造页表，然后将数据挪进去
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
    char *mem;

    if (sz >= PGSIZE)
        panic("inituvm: more than a page");
    mem = kalloc();
    memset(mem, 0, PGSIZE);
    mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
    memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.

// 将某个进程的虚拟地址空间从oldsize增加到newsz，并将增加的部分更新到页表
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
    char *mem;
    uint64 a;

    if (newsz < oldsz)
        return oldsz;

    oldsz = PGROUNDUP(oldsz);
    for (a = oldsz; a < newsz; a += PGSIZE) {
        mem = kalloc();
        if (mem == 0) {
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
        memset(mem, 0, PGSIZE);
        if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
            kfree(mem);
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
    }
    return newsz;
}

uint64 copy_to_kernal(pagetable_t user, pagetable_t kernel, uint64 oldsize, uint64 newsize) {
    uint64 va, pa;
    pte_t *pte;
    uint flags;
    for (va = PGROUNDUP(oldsize); va < newsize; va += PGSIZE) {
        pte = walk(user, va, 0);
        pa = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);
        flags &= ~PTE_U;
        if (mappages(kernel, va, PGSIZE, pa, flags) != 0) {
            uvmunmap(kernel, PGROUNDUP(oldsize), (va - PGROUNDUP(oldsize)) / PGSIZE, 0);
            return -1;
        }
    }
    return newsize;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.

// 释放虚拟内存，从oldsize编程newsize，涉及到删除页表项以及删除物理页
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
    if (newsz >= oldsz)
        return oldsz;

    if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
        int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    }

    return newsz;
}

uint64 dealloc_kernal(pagetable_t kernel, uint64 oldsz, uint64 newsz) {
    if (newsz >= oldsz)
        return oldsz;

    if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
        int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        uvmunmap(kernel, PGROUNDUP(newsz), npages, 0);
    }

    return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable) {
    // there are 2^9 = 512 PTEs in a page table.
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            // this PTE points to a lower-level page table.
            uint64 child = PTE2PA(pte);
            freewalk((pagetable_t)child);
            pagetable[i] = 0;
        } else if (pte & PTE_V) {
            panic("freewalk: leaf");
        }
    }
    kfree((void *)pagetable);
}

int depth = 0;
void print_prefix(int i) {
    for (int i = 0; i <= depth; i++) {
        printf("..");
        if (i < depth) {
            printf(" ");
        }
    }
    printf("%d: ", i);
}
void vmprint(pagetable_t pagetable) {
    printf("page table %p\n", pagetable);
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        // 如果这一项有效
        if (pte & PTE_V) {
            print_prefix(i);
            printf("pte %p ", pte);
            printf("pa %p", PTE2PA(pte));
            printf("\n");
            if (depth < 2) {
                depth++;
                vmprint((pagetable_t)PTE2PA(pte));
                depth--;
            }
        }
    }
}

// Free user memory pages,
// then free page-table pages.

// 先通过uvmunmap将所有的物理页面和最低一级的页表给释放，再通过freewalk将第一第二级页表给释放
void uvmfree(pagetable_t pagetable, uint64 sz) {
    if (sz > 0)
        uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
    freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.

int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
    pte_t *pte;
    uint64 pa, i;
    uint flags;
    char *mem;

    for (i = 0; i < sz; i += PGSIZE) {
        if ((pte = walk(old, i, 0)) == 0)
            panic("uvmcopy: pte should exist");
        if ((*pte & PTE_V) == 0)
            panic("uvmcopy: page not present");
        pa = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);
        if ((mem = kalloc()) == 0)
            goto err;
        memmove(mem, (char *)pa, PGSIZE);
        if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
            kfree(mem);
            goto err;
        }
    }
    return 0;

err:
    uvmunmap(new, 0, i / PGSIZE, 1);
    return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
    pte_t *pte;

    pte = walk(pagetable, va, 0);
    if (pte == 0)
        panic("uvmclear");
    *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    uint64 n, va0, pa0;

    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0)
            return -1;
        n = PGSIZE - (dstva - va0);
        if (n > len)
            n = len;
        memmove((void *)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
    return copyin_new(pagetable, dst, srcva, len);
    // uint64 n, va0, pa0;

    // while (len > 0) {
    //     va0 = PGROUNDDOWN(srcva);
    //     pa0 = walkaddr(pagetable, va0);
    //     if (pa0 == 0)
    //         return -1;
    //     n = PGSIZE - (srcva - va0);
    //     if (n > len)
    //         n = len;
    //     memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    //     len -= n;
    //     dst += n;
    //     srcva = va0 + PGSIZE;
    // }
    // return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
    return copyinstr_new(pagetable, dst, srcva, max);
    // uint64 n, va0, pa0;
    // int got_null = 0;

    // while (got_null == 0 && max > 0) {
    //     va0 = PGROUNDDOWN(srcva);
    //     pa0 = walkaddr(pagetable, va0);
    //     if (pa0 == 0)
    //         return -1;
    //     n = PGSIZE - (srcva - va0);
    //     if (n > max)
    //         n = max;

    //     char *p = (char *)(pa0 + (srcva - va0));
    //     while (n > 0) {
    //         if (*p == '\0') {
    //             *dst = '\0';
    //             got_null = 1;
    //             break;
    //         } else {
    //             *dst = *p;
    //         }
    //         --n;
    //         --max;
    //         p++;
    //         dst++;
    //     }

    //     srcva = va0 + PGSIZE;
    // }
    // if (got_null) {
    //     return 0;
    // } else {
    //     return -1;
    // }
}
