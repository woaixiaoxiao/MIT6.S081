#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include <stdlib.h>

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void trapinit(void) {
    initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void trapinithart(void) {
    w_stvec((uint64)kernelvec);
}
#define NVMA 16
//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
// int mmap_handler(int va, int cause) {
//     int i;
//     struct proc *p = myproc();
//     // 根据地址查找属于哪一个VMA
//     for (i = 0; i < NVMA; ++i) {
//         if (p->vmas[i].valid && p->vmas[i].addr <= va && va <= p->vmas[i].addr + p->vmas[i].length - 1) {
//             break;
//         }
//     }
//     if (i == NVMA)
//         return -1;

//     int pte_flags = PTE_U;
//     if (p->vmas[i].prot & PROT_READ) pte_flags |= PTE_R;
//     if (p->vmas[i].prot & PROT_WRITE) pte_flags |= PTE_W;
//     if (p->vmas[i].prot & PROT_EXEC) pte_flags |= PTE_X;

//     struct file *vf = p->vmas[i].file;
//     // 读导致的页面错误
//     if (cause == 13 && vf->readable == 0) return -1;
//     // 写导致的页面错误
//     if (cause == 15 && vf->writable == 0) return -1;

//     void *pa = kalloc();
//     if (pa == 0)
//         return -1;
//     memset(pa, 0, PGSIZE);

//     // 读取文件内容
//     ilock(vf->ip);
//     // 计算当前页面读取文件的偏移量，实验中p->vma[i].offset总是0
//     // 要按顺序读读取，例如内存页面A,B和文件块a,b
//     // 则A读取a，B读取b，而不能A读取b，B读取a
//     int offset = PGROUNDDOWN(va - p->vmas[i].addr);
//     int readbytes = readi(vf->ip, 0, (uint64)pa, offset, PGSIZE);
//     // 什么都没有读到
//     if (readbytes == 0) {
//         iunlock(vf->ip);
//         kfree(pa);
//         return -1;
//     }
//     iunlock(vf->ip);

//     // 添加页面映射
//     if (mappages(p->pagetable, PGROUNDDOWN(va), PGSIZE, (uint64)pa, pte_flags) != 0) {
//         kfree(pa);
//         return -1;
//     }

//     return 0;
// }
int mmap_handler(uint64 va, uint64 r_cause) {
    struct proc *p = myproc();
    // 地址不合法
    if (va >= p->sz || va < p->trapframe->sp) {
        return -1;
    }
    // 是否和vma有关系
    struct vma *vma = 0;
    for (int i = 0; i < 16; i++) {
        if (p->vmas[i].valid == 1 && (va >= p->vmas[i].addr && va < p->vmas[i].addr + p->vmas[i].length)) {
            vma = &p->vmas[i];
            break;
        }
    }
    // 和vma没关系
    if (vma == 0) {
        return -1;
    }
    // 和vma是有关的
    // 看看权限是否正确
    struct file *file = vma->file;
    // 不能读，但是你读了
    if (!file->readable && r_cause == 13) {
        return -1;
    }
    // 不能写，但是你写了
    if (!file->writable && r_cause == 15) {
        return -1;
    }
    // 构建pte的标志位
    int pte_flag = PTE_U;
    if (vma->prot & PROT_READ) {
        pte_flag |= PTE_R;
    }
    if (vma->prot & PROT_WRITE) {
        pte_flag |= PTE_W;
    }
    if (vma->prot & PROT_EXEC) {
        pte_flag |= PTE_X;
    }
    // 先分配一个物理页面
    uint64 pa = (uint64)kalloc();
    if (pa == 0) {
        return -1;
    }
    memset((void *)pa, 0, PGSIZE);
    // 成功分配物理页面，从文件中读
    struct inode *ip = file->ip;
    ilock(ip);
    // 读取失败
    if (readi(ip, 0, pa, PGROUNDDOWN(va - vma->addr), PGSIZE) == 0) {
        iunlock(ip);
        kfree((void *)pa);
        return -1;
    }
    iunlock(ip);
    // 加入映射
    if (mappages(p->pagetable, PGROUNDDOWN(va), PGSIZE, pa, pte_flag) != 0) {
        kfree((void *)pa);
        return -1;
    }

    return 0;
}

void usertrap(void) {
    int which_dev = 0;

    if ((r_sstatus() & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // send interrupts and exceptions to kerneltrap(),
    // since we're now in the kernel.
    w_stvec((uint64)kernelvec);

    struct proc *p = myproc();

    // save user program counter.
    p->trapframe->epc = r_sepc();

    if (r_scause() == 8) {
        // system call

        if (p->killed)
            exit(-1);

        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        p->trapframe->epc += 4;

        // an interrupt will change sstatus &c registers,
        // so don't enable until done with those registers.
        intr_on();

        syscall();
    } else if (r_scause() == 13 || r_scause() == 15) {
        // #ifdef LAB_MMAP
        if (mmap_handler(r_stval(), r_scause()) == -1) {
            p->killed = -1;
        }
        // #endif
    } else if ((which_dev = devintr()) != 0) {
        // ok
    } else {
        printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
        printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
        p->killed = 1;
    }

    if (p->killed)
        exit(-1);

    // give up the CPU if this is a timer interrupt.
    if (which_dev == 2)
        yield();

    usertrapret();
}

//
// return to user space
//
void usertrapret(void) {
    struct proc *p = myproc();

    // we're about to switch the destination of traps from
    // kerneltrap() to usertrap(), so turn off interrupts until
    // we're back in user space, where usertrap() is correct.
    intr_off();

    // send syscalls, interrupts, and exceptions to trampoline.S
    w_stvec(TRAMPOLINE + (uservec - trampoline));

    // set up trapframe values that uservec will need when
    // the process next re-enters the kernel.
    p->trapframe->kernel_satp = r_satp();         // kernel page table
    p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
    p->trapframe->kernel_trap = (uint64)usertrap;
    p->trapframe->kernel_hartid = r_tp(); // hartid for cpuid()

    // set up the registers that trampoline.S's sret will use
    // to get to user space.

    // set S Previous Privilege mode to User.
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE; // enable interrupts in user mode
    w_sstatus(x);

    // set S Exception Program Counter to the saved user pc.
    w_sepc(p->trapframe->epc);

    // tell trampoline.S the user page table to switch to.
    uint64 satp = MAKE_SATP(p->pagetable);

    // jump to trampoline.S at the top of memory, which
    // switches to the user page table, restores user registers,
    // and switches to user mode with sret.
    uint64 fn = TRAMPOLINE + (userret - trampoline);
    ((void (*)(uint64, uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void kerneltrap() {
    int which_dev = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0)
        panic("kerneltrap: not from supervisor mode");
    if (intr_get() != 0)
        panic("kerneltrap: interrupts enabled");

    if ((which_dev = devintr()) == 0) {
        printf("scause %p\n", scause);
        printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
        panic("kerneltrap");
    }

    // give up the CPU if this is a timer interrupt.
    if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
        yield();

    // the yield() may have caused some traps to occur,
    // so restore trap registers for use by kernelvec.S's sepc instruction.
    w_sepc(sepc);
    w_sstatus(sstatus);
}

void clockintr() {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int devintr() {
    uint64 scause = r_scause();

    if ((scause & 0x8000000000000000L) && (scause & 0xff) == 9) {
        // this is a supervisor external interrupt, via PLIC.

        // irq indicates which device interrupted.
        int irq = plic_claim();

        if (irq == UART0_IRQ) {
            uartintr();
        } else if (irq == VIRTIO0_IRQ) {
            virtio_disk_intr();
        } else if (irq) {
            printf("unexpected interrupt irq=%d\n", irq);
        }

        // the PLIC allows each device to raise at most one
        // interrupt at a time; tell the PLIC the device is
        // now allowed to interrupt again.
        if (irq)
            plic_complete(irq);

        return 1;
    } else if (scause == 0x8000000000000001L) {
        // software interrupt from a machine-mode timer interrupt,
        // forwarded by timervec in kernelvec.S.

        if (cpuid() == 0) {
            clockintr();
        }

        // acknowledge the software interrupt by clearing
        // the SSIP bit in sip.
        w_sip(r_sip() & ~2);

        return 2;
    } else {
        return 0;
    }
}
