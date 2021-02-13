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

struct {
    struct spinlock lock;
    struct buf buf[NBUF];
    struct {
        struct spinlock bucket_lock;
        struct buf head;
    } bucket[NBUCKET];
} bcache;

void remove_from_bucket(int bucket_idx,struct buf* target) {
    struct buf *b;
    for(b = bcache.bucket[bucket_idx].head.next; b != &bcache.bucket[bucket_idx].head; b = b->next){
        if(b == target){
            b->next->prev = b->prev;
            b->prev->next = b->next;
            b->prev = b->next = 0;
            break;
        }
    }
}

void insert_into_bucket(int bucket_idx, struct buf* b) {
    struct buf* head;
    // insert it after head
    head = &bcache.bucket[bucket_idx].head;
    b->next = head->next;
    b->prev = head;
    head->next->prev = b;
    head->next = b;
}

void
binit(void)
{
    struct buf *b;
    initlock(&bcache.lock,"bcache");
    for (int i = 0; i < NBUCKET; i++) {
        initlock(&bcache.bucket[i].bucket_lock,"bcache.bucket");
        bcache.bucket[i].head.prev = &bcache.bucket[i].head;
        bcache.bucket[i].head.next = &bcache.bucket[i].head;
    }
    for (int i = 0; i < NBUF; i++) {
      b = &bcache.buf[i];
      initsleeplock(&b->lock, "buffer");
      b->last_used = 0;
      b->blockno = i;
      b->dev = 0x3f3f3f3f;
      insert_into_bucket(b->blockno % NBUCKET,b);
    }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
    struct buf* b;
    struct buf* victim_b = 0;
    int bucket_idx;
    int min_ticks = 0;
    // Is the block already cached?
    // look up in hash table
    bucket_idx = blockno % NBUCKET;
    acquire(&bcache.bucket[bucket_idx].bucket_lock);
    for(b = bcache.bucket[bucket_idx].head.next; b != &bcache.bucket[bucket_idx].head; b = b->next){
        if(b->dev == dev && b->blockno == blockno){
            // found in hash table
            // increase ref count
            b->refcnt++;
            release(&bcache.bucket[bucket_idx].bucket_lock);
            // acquire sleep lock
            acquiresleep(&b->lock);
            return b;
        }
    }
    release(&bcache.bucket[bucket_idx].bucket_lock);
    acquire(&bcache.lock);
    acquire(&bcache.bucket[bucket_idx].bucket_lock);
    for(b = bcache.bucket[bucket_idx].head.next; b != &bcache.bucket[bucket_idx].head; b = b->next){
        if(b->dev == dev && b->blockno == blockno){
            // found in hash table
            // increase ref count
            b->refcnt++;
            release(&bcache.bucket[bucket_idx].bucket_lock);
            release(&bcache.lock);
            // acquire sleep lock
            acquiresleep(&b->lock);
            return b;
        }
    }
    // not cached, first try to find a LRU block from current bucket
    for (b = bcache.bucket[bucket_idx].head.next; b != &bcache.bucket[bucket_idx].head; b = b->next) {
        if (b->refcnt == 0 && (victim_b == 0 || b->last_used < min_ticks)) {
          min_ticks = b->last_used;
          victim_b = b;
        }
        // victim's last used time = 0, which is min, no need to do further search
        if (victim_b != 0 && min_ticks == 0) break;
    }
    // found victim from current bucket
    if (victim_b) {
      victim_b->dev = dev;
      victim_b->blockno = blockno;
      victim_b->valid = 0;
      victim_b->refcnt = 1;
      release(&bcache.bucket[bucket_idx].bucket_lock);
      release(&bcache.lock);
      acquiresleep(&victim_b->lock);
      return victim_b;
    }
    // try to find a LRU block from other bucket
    for (int i = 0; i < NBUCKET; i++) {
        if (i == bucket_idx) continue;
        victim_b = 0, min_ticks = 0;
        acquire(&bcache.bucket[i].bucket_lock);
        for (b = bcache.bucket[i].head.next; b != &bcache.bucket[i].head; b = b->next) {
            if (b->refcnt == 0 && (victim_b == 0 || b->last_used < min_ticks)) {
            min_ticks = b->last_used;
            victim_b = b;
            }
            // victim's last used time = 0, which is min, no need to do further search
            if (victim_b != 0 && min_ticks == 0) break;
        }
        // found victim from bucket i
        if (victim_b) {
            // move victim_b from its origin bucket
            remove_from_bucket(i,victim_b);
            release(&bcache.bucket[i].bucket_lock);
            insert_into_bucket(bucket_idx,victim_b);
            victim_b->dev = dev;
            victim_b->blockno = blockno;
            victim_b->valid = 0;
            victim_b->refcnt = 1;
            release(&bcache.bucket[bucket_idx].bucket_lock);
            release(&bcache.lock);
            acquiresleep(&victim_b->lock);
            if (victim_b->refcnt != 1) {
                panic("bad refcnt");
            }
            return victim_b;
        }
        // not found in bucket i
        release(&bcache.bucket[i].bucket_lock);
    }
    panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
    struct buf *b;

    b = bget(dev, blockno);
    if(!b->valid) {
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
    if(!holdingsleep(&b->lock))
        panic("bwrite");
    virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
    if(!holdingsleep(&b->lock))
        panic("brelse");
    int i = b->blockno % NBUCKET;
    releasesleep(&b->lock);
    acquire(&bcache.bucket[i].bucket_lock);
    b->refcnt--;
    if (b->refcnt == 0) {
      b->last_used = get_ticks();
    }
    release(&bcache.bucket[i].bucket_lock);
}

void
bpin(struct buf *b) {
    acquire(&bcache.lock);
    b->refcnt++;
    release(&bcache.lock);
}

void
bunpin(struct buf *b) {
    acquire(&bcache.lock);
    b->refcnt--;
    release(&bcache.lock);
}


