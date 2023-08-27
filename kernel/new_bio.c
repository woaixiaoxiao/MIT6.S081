// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define prime 13

struct {
    struct buf buf[NBUF];
} bcache;

struct {
    struct spinlock lock;
    struct buf head;
} ht[prime];

#define LOCK(i) (acquire(&ht[i].lock));
#define UNLOCK(i) (release(&ht[i].lock));

void update_time(struct buf *b) {
    acquire(&tickslock);
    b->timestamp = ticks;
    release(&tickslock);
}

void insert_into_ht(struct buf *b, int key) {
    b->next = ht[key].head.next;
    b->prev = &ht[key].head;
    ht[key].head.next->prev = b;
    ht[key].head.next = b;
}

void delete_from_ht(struct buf *b) {
    b->next->prev = b->prev;
    b->prev->next = b->next;
}

void binit(void) {
    struct buf *b;

    char a[20];
    for (int i = 0; i < prime; i++) {
        snprintf(a, sizeof(a), "bcache_%d", i);
        initlock(&ht[i].lock, a);
        ht[i].head.prev = &ht[i].head;
        ht[i].head.next = &ht[i].head;
    }

    // Create linked list of buffers
    for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
        initsleeplock(&b->lock, "buffer");
        insert_into_ht(b, 0);
    }
}

struct buf *search_in_ht(uint dev, uint blockno, int key) {
    struct buf *b;
    for (b = ht[key].head.next; b != &ht[key].head; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            update_time(b);
            return b;
        }
    }
    return 0;
}

struct buf *search_lru_free_in_ht(uint dev, uint blockno, int key) {
    struct buf *b;
    struct buf *lru_b = 0;
    for (b = ht[key].head.next; b != &ht[key].head; b = b->next) {
        if (b->refcnt == 0 && (lru_b == 0 || lru_b->timestamp > b->timestamp)) {
            lru_b = b;
        }
    }
    return lru_b;
}

struct buf *search_in_other(uint dev, uint blockno, int key) {
    struct buf *b;
    for (int i = key, cycle = 0; cycle < prime; cycle++, i = (i + 1) % prime) {
        // 如果不是自己，则给这个兄弟上个锁
        if (i != key) {
            LOCK(i);
        }
        // 在这个兄弟里去找一下
        b = search_lru_free_in_ht(dev, blockno, i);
        // 这个兄弟里没有空闲页面
        if (!b) {
            if (i != key) {
                UNLOCK(i);
            }
            continue;
        }
        // 在这个兄弟里找到了空闲页面
        // 先更新属性
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        update_time(b);
        // 如果不是自己的哈希槽里的，将这个页面放到自己哈希表槽中
        if (i != key) {
            delete_from_ht(b);
            insert_into_ht(b, key);
        }
        // 释放哈希表的锁
        if (i != key) {
            UNLOCK(i);
        }
        return b;
    }
    panic("no free buf");
}

static struct buf *
bget(uint dev, uint blockno) {
    struct buf *b;
    int key = blockno % prime;
    // 尝试去对应的哈希表槽查找
    LOCK(key);
    b = search_in_ht(dev, blockno, key);
    if (b) {
        UNLOCK(key);
        acquiresleep(&b->lock);
        return b;
    }
    // 至此，没有在对应的表槽找到，遍历所有哈希表的表槽，不过优先处理自己表槽的
    // 这里是带着key对应的锁去查找的
    b = search_in_other(dev, blockno, key);
    // 这个b不可能为0，否则直接panic了
    UNLOCK(key);
    acquiresleep(&b->lock);
    return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno) {
    struct buf *b;

    b = bget(dev, blockno);
    if (!b->valid) {
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b) {
    if (!holdingsleep(&b->lock))
        panic("bwrite");
    virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b) {
    releasesleep(&b->lock);
    int key = b->blockno % prime;
    LOCK(key);
    b->refcnt -= 1;
    UNLOCK(key);
}

void bpin(struct buf *b) {
    int key = b->blockno % prime;
    LOCK(key);
    b->refcnt++;
    UNLOCK(key);
}

void bunpin(struct buf *b) {
    int key = b->blockno % prime;
    LOCK(key);
    b->refcnt--;
    UNLOCK(key);
}