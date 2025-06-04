# lab07_lock

> [!IMPORTANT]  
> The following hint may contain a decisive solution. If you wish to solve the problem by yourself, **DO NOT** view it. If you find it helpful, consider staring this repository!

## Memory allocator

The basic idea and implementation are quite simple: just assign one lock to each CPU. 

Be careful to avoid deadlocks: they can occur if you don't release the current CPU's lock before attempting to find another CPU to borrow free memory from. (The other may also tries to borrow from this cpu.)

```cpp
struct {
  struct spinlock lock;
  struct run *freelist;
  int freelist_len;
  char name[10];
  int is_active;
} kmem[NCPU];
```

However, there's a particularly annoying detail: if one CPU needs to borrow free memory from others, how much should it borrow? This detail cost me a lot of time.

I tried borrowing one, half, or 1024 units, and experimented with several tricks to improve speed and hit rate, but none of them worked. All of them frequently fails to pass the test.

To improve the hit rate when borrowing, I even sorted the other CPUs by the length of their freelists beforehand:

```cpp
void kalloc(void) {
    // ...

    acquire(&kmem[id].lock);
    if (kmem[id].freelist_len == 0) {
        // To avoid deadlock
        release(&kmem[id].lock);

        // Sort freelist_len in descending order and keep track of the indices.
        // No need to lock — a bit of inaccuracy is fine, since this is just for improving the hit rate.
        int choices[NCPU];
        for (int i = 0; i < NCPU; i++) {
            choices[i] = i;
        }
        for (int i = 0; i < NCPU - 1; i++) {
            for (int j = 0; j < NCPU - 1 - i; j++) {
                if (kmem[choices[j]].freelist_len < kmem[choices[j+1]].freelist_len) {
                    int temp = choices[j];
                    choices[j] = choices[j+1];
                    choices[j+1] = temp;
                }
            }
        }
        for (int i = 0; i < NCPU; i++) {
            int nowid = choices[i];
            // ...
        }
    }
}
```

In the end, the solution turned out to be surprisingly simple: just borrow it all from the other CPU... OMG.
```cpp
void kalloc(void) {
    // ...
        acquire(&kmem[nowid].lock)
        if (kmem[nowid].freelist_len > 0) {
            acquire(&kmem[id].lock);

            // Just borrow it all !!!
            kmem[id].freelist = kmem[nowid].freelist;
            kmem[id].freelist_len = kmem[nowid].freelist_len;

            kmem[nowid].freelist = 0;
            kmem[nowid].freelist_len = 0;
            release(kmem[nowid].lock)
            break
        }
    // ...
}
```


## Buffer cache

First, we cannot use the same scheme as the memory allocator, because the disk is not accessed by only one CPU at a time. 

`bcache` is a copy of one disk unit, multiple CPUs may attempt to access the unit concurrently. I think understanding this idea is very important.

The previous locking scheme was time-consuming because it locked the entire disk cache list. To improve this, we can split the disk cache list into multiple parts — effectively implementing a hash table.

When accessing a specific disk unit, we compute its hash ID and use it to select the corresponding list. I chose 31 as the size of the hash table.

By the way, I think using a doubly linked list as before works really well and is very convenient.

```cpp
void binit(void) {
    // ...

    for(int i = 0; i < NBUF; ++i) {
        int idx = i % HASH_SIZE;
        struct buf *head = &bcache.head[idx];
        struct buf *now = &bcache.buf[i];

        now->next = head->next;
        now->prev = head;

        head->next->prev = now;
        head->next = now;

        initsleeplock(&now->lock, "buffer");

        printf("head->next = %p\n", head->next);
        printf("head->prev = %p\n", head->prev);
    }
}
```

### Key 1

The key idea arises when the corresponding list is empty in `bget`. To establish a mapping for the disk unit, we must borrow a `bcache` entry from another hash table bucket.

```cpp
static struct buf* bget(uint dev, uint blockno) {
  int idx = bhash(dev, blockno);

  acquire(&bcache.lock[idx]);
  // Is the block already cached? (same as before, skip it in this demo)
  // Not cached. Recycle the least recently used (LRU) unused buffer. (similar as before, skip it in this demo)
  // Must release, otherwise get deadlock. (same as the previous problem)
  release(&bcache.lock[idx]);

  // Now we want to borrow a bucket from another list.
```

Suppose that we have found which list we will borrow from, then before we want to borrow, the other thread changes our list:

1. OurThread: `bget(0)`, The 0th list is empty, so we prepare to borrow from another list. No lock now.
2. Thread1: `brelease(0)`, It successfully acquire and release `lock(0)`. So 0th list is no longer empty.
3. Thread1: `bget(0)`, Since the 0th list is now non-empty, it successfully gets a bucket and maps the disk content to it.
4. OurThread: During this window, actually begins the borrowing process and ends up taking a bucket from another list.
5. Now there are **two buckets** mapped to the same disk block. If one is modified, the other becomes stale.


So how do we solve this? Here’s the solution I came up with. While holding our own lock, when we attempt to make changes, we should first check whether a bucket has already been mapped to this disk unit.

```cpp
static struct buf* bget(uint dev, uint blockno) {
    // Hold our own lock
    acquire(&bcache.lock[idx]);

    // Check whether a bucket has already been mapped to this disk unit. (because of the other thread.)
    head = &bcache.head[idx];
    for(b = head->next; b != head; b = b->next){
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        release(&bcache.lock[idx]);
        acquiresleep(&b->lock);
        return b;
      }
    }

    // Borrowing ....
```

Holding our own lock is crucial. You can verify that, in this case, step 3 above will no longer cause any data consistency issues.

### Key 2

The next key is how to lock and unlock when borrowing. The intuitive idea is to first hold our own lock, and then acquire the other lock.

```cpp
static struct buf* bget(uint dev, uint blockno) {
    // Hold our own lock
    acquire(&bcache.lock[idx]);

    // Check whether a bucket has already been mapped to this disk unit. (explained in key 1)

    // Borrowing ....
    acquire(&bcache.lock[other_idx]);
    // ...
```

However, this is the typical deadlock situtation:

1. OurThread: `bget(0)`, Since the 0th list is empty, it plans to borrow from the 1st list. It now holds `lock(0)` and attempts to acquire `lock(1)`.
2. Thread1: `bget(1)`, It successfully acquire and release `lock(1)`, making the 1st list empty.
3. Thread1: `bget(1+HASH_SIZE)`, It tries to borrow from the 1st list, but since it is empty, now this thread holds `lock(1)` and starts looking for another list to borrow from.
4. Now ourthread holds `lock(0)` and try to hold `lock(1)`; thread1 holds `lock(1)` and try to hold `lock(0)`. **Oops, Deadlock!**

So the core idea is: we should avoid holding two locks at the same time whenever possible. By this idea, we can split the actions:

```cpp
    // We choose other_idx to borrow from. Now unlink the bucket
    acquire(&bcache.lock[other_idx]);
    // unlink the bucket
    min_b->next->prev = min_b->prev;
    min_b->prev->next = min_b->next;
    release(&bcache.lock[other_idx]);

    // We want to put the bucket in our list. Now hold our own lock
    acquire(&bcache.lock[idx]);
    // Check whether a bucket has already been mapped to this disk unit. (explained in key 1)
    if (...) {
        release(&bcache.loc[idx]);
        return;
    }

    // Put the bucket in our list.
    head = &bcache.head[idx];
    min_b->prev = head->prev;
    head->prev->next = min_b;
    head->prev = min_b;
    min_b->next = head;
    // ...
    release(&bcache.loc[idx]);

```

Here's a tricky case: what if the list already has a bucket mapped to the disk block (because another thread did it, as explained in Key 1)? Then what should we do with the one we just unlinked from somewhere else?

Sure, you could put it back where it came from — but to keep things simple, I just throw it into our list anyway:

```cpp
    // We choose other_idx to borrow from. Now unlink the bucket
    acquire(&bcache.lock[other_idx]);
    // ...
    release(&bcache.lock[other_idx]);

    // We want to put the bucket in our list. Now hold our own lock
    acquire(&bcache.lock[idx]);

    // Put the bucket in our list anyway...
    // ...

    // Now check whether a bucket has already been mapped to this disk unit and choose what to return.
    for (...)
        if (b.dev == dev && b->blockno == blockno) {
            release(&bcache.loc[idx]);
            return b;
        }
    }
    b = min_b;
    b->dev = dev;
    b->blockno = blockno;
    // ...
    release(&bcache.loc[idx]);
    return b;
```

### Key 3

The next important question is: how do we choose the right list to borrow from? If each list maintains its own LRU order, it's actually quite simple — we just compare the first free bucket in each LRU list:

```cpp
    uint min_ticks = 0xFFFF;
    int min_idx = idx;
    struct buf *min_b;
    for (int i = 0; i < HASH_SIZE; ++i) {
        if (i == idx) continue;

        acquire(&bcache.lock[i]);
        head = &(bcache.head[i]);
        for(b = head->prev; b != head; b = b->prev){
            // Because each list is also LRU, so we just compare the first **free** bucket (refcnt == 0).
            if(b->refcnt == 0) {
                if(b->ticks < min_ticks) {
                    min_ticks = b->ticks;
                    min_idx = i;
                    min_b = b;
                }
                break;
            }
        }
        release(&bcache.lock[i]);
    }
```