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

#define NUMBUCKET 13

struct {
  struct spinlock lock;

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache[NUMBUCKET];

struct spinlock global_lock;
struct buf bufs[NBUF];

void
binit(void)
{
  struct buf *b;

  initlock(&global_lock, "global_lock");

  for(int i = 0; i < NUMBUCKET; i++) {
    char name[10];
    snprintf(name, 10, "bcache%d", i);
    initlock(&bcache[i].lock, name);
    bcache[i].head.prev = &bcache[i].head;
    bcache[i].head.next = &bcache[i].head;
  }

  // Create linked list of buffers
  for(b = bufs; b < bufs+NBUF; b++){
    int id = (b-bufs) % NUMBUCKET;
    b->next = bcache[id].head.next;
    b->prev = &bcache[id].head;
    initsleeplock(&b->lock, "buffer");
    bcache[id].head.next->prev = b;
    bcache[id].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int id = blockno % NUMBUCKET;

  acquire(&global_lock);

  // Is the block already cached?
  acquire(&bcache[id].lock);
  for(b = bcache[id].head.next; b != &bcache[id].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      acquire(&tickslock);
      b->time_stamp = ticks;
      release(&tickslock);
      release(&bcache[id].lock);
      release(&global_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache[id].lock);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  int min = 0x7fffffff;
  struct buf *minb = 0;
  for (int i = 0; i < NUMBUCKET; i++) {
    acquire(&bcache[i].lock);
    for(b = bcache[i].head.prev; b != &bcache[i].head; b = b->prev){
      if(b->refcnt == 0) {
        if (b->time_stamp < min) {
          min = b->time_stamp;
          minb = b;
        }
      }
    }
    release(&bcache[i].lock);
  }
  if(minb){
      int id = minb->blockno % NUMBUCKET;
      acquire(&bcache[id].lock);
      minb->dev = dev;
      minb->valid = 0;
      minb->refcnt = 1;
      acquire(&tickslock);
      minb->time_stamp = ticks;
      release(&tickslock);
      if(blockno % NUMBUCKET == id){
        minb->blockno = blockno;
      }
      else{
        int new_id = blockno % NUMBUCKET;
        // acquire(&global_lock);
        minb->blockno = blockno;
        minb->next->prev = minb->prev;
        minb->prev->next = minb->next;
        minb->next = bcache[new_id].head.next;
        minb->prev = &bcache[new_id].head;
        bcache[new_id].head.next->prev = minb;
        bcache[new_id].head.next = minb;
        // release(&global_lock);
      }
      release(&bcache[id].lock);
      release(&global_lock);
      acquiresleep(&minb->lock);
      return minb;
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

  releasesleep(&b->lock);

  int id = b->blockno % NUMBUCKET;
  acquire(&bcache[id].lock);
  b->refcnt--;
  // if (b->refcnt == 0) {
  //   // no one is waiting for it.
  //   b->next->prev = b->prev;
  //   b->prev->next = b->next;
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }
  
  release(&bcache[id].lock);
}

void
bpin(struct buf *b) {
  int id = b->blockno % NUMBUCKET;
  acquire(&bcache[id].lock);
  b->refcnt++;
  release(&bcache[id].lock);
}

void
bunpin(struct buf *b) {
  int id = b->blockno % NUMBUCKET;
  acquire(&bcache[id].lock);
  b->refcnt--;
  release(&bcache[id].lock);
}


