中文 | [English](./../en/lab07_lock.md)

# lab07_lock

> [!IMPORTANT]  
> 以下文章可能会对您的解决有决定性的帮助。如果您希望自己独立解决，**请勿**查看。如果您觉得有帮助，请考虑 Star 这个仓库！

## memory alloc

基本思想和实现都很简单：为每个 CPU 分配一个锁。

**小心避免死锁**：如果尝试从另一个 CPU 借用空闲内存之前，没有释放当前 CPU 的锁，那么很有可能发生死锁。（即其他人可能也正在尝试借自己）

```cpp
struct {
  struct spinlock lock;
  struct run *freelist;
  int freelist_len;
  char name[10];
  int is_active;
} kmem[NCPU];
```

但是，有一个特别烦人的细节：**从其他 CPU 借空闲内存时，应该借多少？**这个花了我巨多的时间。

我尝试借用一、一半或 1024，而且尝试了几种技巧来提高速度和命中率，但都经常无法通过测试。

比如为了提高借用时的命中率，我甚至对其他 CPU 进行了排序：

```cpp
void kalloc(void) {
    // ...

    acquire(&kmem[id].lock);
    if (kmem[id].freelist_len == 0) {
        // 避免死锁
        release(&kmem[id].lock);

        // 按降序排序 freelist_len 并跟踪索引。
        // 不需要锁定——稍微不准确是可以的，因为这只是为了提高命中率。
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
            // 尝试借用
        }
    }
}
```

**最终解决了，答案出人意料地简单：借全部... 我突然释怀地笑**
```cpp
void kalloc(void) {
    // ...
        acquire(&kmem[nowid].lock)
        if (kmem[nowid].freelist_len > 0) {
            acquire(&kmem[id].lock);

            // 只是借用全部！！！
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

## buffer cache

首先，我们不能使用上一节地方案，**因为磁盘不是一次只被一个 CPU 访问。** `bcache` 是一个磁盘单元的副本，多个 CPU 可能同时尝试访问。我认为理解这两个之间的差别非常重要。

之前的锁定方案很耗时，因为它锁定了整个磁盘缓存列表。解决方案时，将磁盘缓存列表分成多个部分，用一个哈希表维护。

访问每个磁盘单元时，计算 HASH 值，然后在对应的哈希表中分配。很明显，使用链表型的哈希表超级合适，因为此时可以只锁对应哈希值的那个链表就可以了；与之对比，线性开型明显要锁整个哈希表。

实现中，可以像现在的代码一样，使用双向链表，并且哨兵机制。效果很好，而且非常方便。

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

### 关键点 1

关键想法出现在 `bget` 中相应列表为空时。为了为磁盘单元建立映射，我们必须从另一个哈希表桶借用 `bcache` 条目。

```cpp
static struct buf* bget(uint dev, uint blockno) {
  int idx = bhash(dev, blockno);

  acquire(&bcache.lock[idx]);
  // 块是否已经被缓存？（与之前相同，跳过）
  // ..

  // 未缓存。找到哈希表上对应的链表，使用 LRU 得到 unused buffer。
  // ...
  release(&bcache.lock[idx]); // 释放掉自己的锁

  // 现在我们要从另一个列表借用桶。
```

假设我们已经找到了要从哪个列表借用，然后在我们想要借用之前，另一个线程改变了我们的列表：

1. 我们的线程：`bget(0)`，第 0 个列表为空，所以我们准备从另一个列表借用。现在没有锁。
2. 他人的线程：`brelease(0)`，它成功获取并释放 `lock(0)`。所以第 0 个列表不再为空。
3. 他人的线程：`bget(0)`，由于第 0 个列表现在非空，它成功获取一个桶并将磁盘内容映射到它。
4. 我们的线程：由于代码已经是开始借用了，唤醒之后最终从另一个列表取走一个桶。
5. 现在有**两个桶**映射到同一个磁盘块。同步失败，即一个被修改，另一个信息是过时的。

如何解决？我想出的解决方案：借用的时候要持有自己的锁的同时，然后借用前再次确认自己现在是否已经不需要借了。

```cpp
static struct buf* bget(uint dev, uint blockno) {
    // 持有我们自己的锁
    acquire(&bcache.lock[idx]);

    // 检查是否已经有桶映射到这个磁盘单元。（因为其他线程）
    head = &bcache.head[idx];
    for(b = head->next; b != head; b = b->next){
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        release(&bcache.lock[idx]);
        acquiresleep(&b->lock);
        return b;
      }
    }

    // 借用中....
```

持有自己的锁是至关重要的。可以自行验证，上面的步骤 3 将不再导致数据一致性问题。

当然这个又会引发死锁问题，这个得看关键点 2 了。

### 关键点 2

下一个关键是如何在借用时上锁和解锁。直观的想法是先持有我们自己的锁，然后获取另一个锁。

```cpp
static struct buf* bget(uint dev, uint blockno) {
    // 持有我们自己的锁
    acquire(&bcache.lock[idx]);

    // 检查是否已经有桶映射到这个磁盘单元。（在关键点 1 中解释）

    // 借用中....
    acquire(&bcache.lock[other_idx]);
    // ...
```

然而，这是典型的死锁情况：

1. 我们的线程：`bget(0)`，由于第 0 个列表为空，它计划从第 1 个列表借用。它现在持有 `lock(0)` 并尝试获取 `lock(1)`。
2. 他人的线程：`bget(1)`，它成功获取并释放 `lock(1)`，使第 1 个列表为空。
3. 他人的线程：`bget(1+HASH_SIZE)`，它尝试从第 1 个列表借用，此时为空，现在这个线程持有 `lock(1)` 并开始寻找另一个列表来借用。
4. 现在我们的线程持有 `lock(0)` 并尝试持有 `lock(1)`；他人的线程 持有 `lock(1)` 并尝试持有 `lock(0)`。**死锁！**

所以核心想法是：**应该尽可能避免同时持有两个锁**。通过这个想法，我们可以分割操作：先找到借哪个，然后再自己上自己的锁。

你可能会担心，我找到借的人了，但是我之后又把他解锁了，万一在我操作前他又被其他人借了咋整？所以看代码，**找到借的人时，直接就顺手把他的空桶给拿掉。**

```cpp
    // 我们选择从 other_idx 借用
    acquire(&bcache.lock[other_idx]);
    // 把想借的桶拿走，这样就不用担心 release(other) 之后，other 又被另一个人借完了
    min_b->next->prev = min_b->prev;
    min_b->prev->next = min_b->next;
    release(&bcache.lock[other_idx]);

    // 我们想要将桶放入我们的列表。现在持有我们自己的锁
    acquire(&bcache.lock[idx]);
    // 检查是否已经有桶映射到这个磁盘单元。（在关键点 1 中解释）
    if (...) {
        release(&bcache.loc[idx]);
        return;
    }

    // 将桶放入我们的列表。
    head = &bcache.head[idx];
    min_b->prev = head->prev;
    head->prev->next = min_b;
    head->prev = min_b;
    min_b->next = head;
    // ...
    release(&bcache.loc[idx]);
```


这里有一个情况：如果自己发现自己已经加载成功了（因为另一个线程这样做了，关键点 1 中解释的），怎么办？我们手里还拿着别人的桶哩。

当然，可以还回去。但为了保持简单，我选择仍然放进我们的列表：

```cpp
    // 我们选择从 other_idx 借用。现在取消链接桶
    acquire(&bcache.lock[other_idx]);
    // ...
    release(&bcache.lock[other_idx]);

    // 我们想要将桶放入我们的列表。现在持有我们自己的锁
    acquire(&bcache.lock[idx]);

    // 无论如何将桶放入我们的列表...
    // ...

    // 现在检查是否已经有桶映射到这个磁盘单元并选择要返回的内容。
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

### 关键点 3

下一个重要问题是：我们如何选择正确的列表来借用？如果每个列表维护自己的 LRU 顺序，实际上很简单，我们只需要比较每个 LRU 列表中的第一个空闲桶：

```cpp
    uint min_ticks = 0xFFFF;
    int min_idx = idx;
    struct buf *min_b;
    for (int i = 0; i < HASH_SIZE; ++i) {
        if (i == idx) continue;

        acquire(&bcache.lock[i]);
        head = &(bcache.head[i]);
        for(b = head->prev; b != head; b = b->prev){
            // 因为每个列表也是 LRU，所以我们只需要比较第一个**空闲**桶（refcnt == 0）。
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

而且这样也顺带完成了这个实验的挑战要求（在网页的最后）。