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

#define HASH_SIZE 13


struct {
  struct spinlock lock[HASH_SIZE];
  struct buf head[HASH_SIZE];

  struct buf buf[NBUF];
} bcache;

void
binit(void)
{
  printf("NBUF = %d\n", NBUF);
  printf("HASH_SIZE = %d\n", HASH_SIZE);

  for(int i = 0; i < HASH_SIZE; i++){
    char name[10];
    snprintf(name, 10, "bcache%d", i);
    initlock(&bcache.lock[i], name);

    struct buf *head = &bcache.head[i];
    head->next = head;
    head->prev = head;
  }

  for(int i = 0; i < NBUF; ++i) {
    int idx = i % HASH_SIZE;
    struct buf *head = &bcache.head[idx];
    struct buf *now = &bcache.buf[i];

    now->next = head->next;
    now->prev = head;

    head->next->prev = now;
    head->next = now;

    initsleeplock(&now->lock, "buffer");
  }
}

int bhash(uint dev, uint blockno) {
  // 这里为了尽量分散，dev和blockno都参与hash计算
  int hash = (dev%2) ? blockno % HASH_SIZE : (HASH_SIZE - blockno % HASH_SIZE);
  return hash;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  int idx = bhash(dev, blockno);

  acquire(&bcache.lock[idx]);
  struct buf *b;
  struct buf *head = &(bcache.head[idx]);

  // Is the block already cached?
  for(b = head->next; b != head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = head->prev; b != head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.lock[idx]);

  // 假如 release
  // 1. bget(0) myself: acquire-release, 没找到, 准备开始大锁
  // 2. brelse(0) other: acquire-release, OK...
  // 3. bget(0) other: acquire-release, 找到一个空的
  // 4. bget(0) myself: 大锁之后，从别的地方拿过来一个
  // 5. 出错，有两个了..

  // 假如 release，但是大锁之后会检测
  // 那也不对，第 4 步中如果发生在检测之后，还是会从别的地方拿一个

  // 假如不 release: 死锁

  // 假如 release，但是大锁最后要改变的时候才会检测？
  // 似乎可以 --> 没错，就是可以

  // 本次哈希没有，所以要从其他桶中借一个过来
  uint min_ticks = 0xFFFF;
  int min_idx = idx;
  struct buf *min_b;
  for (int i = 0; i < HASH_SIZE; ++i) {
    if (i == idx) continue;

    acquire(&bcache.lock[i]);
    head = &(bcache.head[i]);
    for(b = head->prev; b != head; b = b->prev){
      // 因为每个桶自己也维护了顺序，所以只要找到一个，就是当前桶的 LRU 了
      if(b->refcnt == 0 && b->ticks < min_ticks) {
        min_ticks = b->ticks;
        min_idx = i;
        min_b = b;
        break;
      }
    }
    release(&bcache.lock[i]);
  }

  if (min_ticks != 0xFFFF) {
    // 要开始改变了，原则：不要同时拿两个锁

    // 有一个规定：
    // 先强制删掉别人的块，即使后面发现不需要了，也还是放在我们这里不归还
    // 其实可以归还，只不过代码改一下就好了

    // 删掉别人的块，等会我们可能用它 (unlink)
    acquire(&bcache.lock[min_idx]);
    min_b->next->prev = min_b->prev;
    min_b->prev->next = min_b->next;
    release(&bcache.lock[min_idx]);

    acquire(&bcache.lock[idx]);
    head = &bcache.head[idx];

    // 不管最后要不要别人的块，这个块放在这里，并且放在最后一个中
    min_b->prev = head->prev;
    head->prev->next = min_b;
    head->prev = min_b;
    min_b->next = head;

    // 需要先检测目标是否已经分配了
    for(b = head->next; b != head; b = b->next){
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        release(&bcache.lock[idx]);
        acquiresleep(&b->lock);
        return b;
      }
    }

    // 没有分配，就用这个块
    b = min_b;
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;
    release(&bcache.lock[idx]);
    acquiresleep(&b->lock);
    return b;
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

  int idx = bhash(b->dev, b->blockno);

  acquire(&bcache.lock[idx]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head[idx].next;
    b->prev = &bcache.head[idx];
    bcache.head[idx].next->prev = b;
    bcache.head[idx].next = b;

    b->ticks = ticks;
  }
  release(&bcache.lock[idx]);
}

void
bpin(struct buf *b) {
  int idx = bhash(b->dev, b->blockno);
  acquire(&bcache.lock[idx]);
  b->refcnt++;
  release(&bcache.lock[idx]);
}

void
bunpin(struct buf *b) {
  int idx = bhash(b->dev, b->blockno);
  acquire(&bcache.lock[idx]);
  b->refcnt--;
  release(&bcache.lock[idx]);
}


