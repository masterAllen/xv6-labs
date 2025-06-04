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
  int freelist_len;
  char name[10];
  int is_active;
} kmem[NCPU];

void
kinit()
{
  for(int i = 0; i < NCPU; i++){
    snprintf(kmem[i].name, 10, "kmem%d", i);
    initlock(&kmem[i].lock, kmem[i].name);
    kmem[i].freelist = 0;
    kmem[i].freelist_len = 0;
  }
  freerange(end, (void*)PHYSTOP);

}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();

  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  kmem[id].freelist_len++;
  release(&kmem[id].lock);

  pop_off();
}

void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();
  pop_off();

  acquire(&kmem[id].lock);
  if (kmem[id].freelist_len == 0) {
    release(&kmem[id].lock);

    // 选择其他空闲页最多的核心偷一些过来

    // 对 freelist_len 排序，按照值从大到小，记录索引
    // 不需要加锁，这里只是为了提高 hit rate
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

    // printf("choices: ");
    // for (int i = 0; i < NCPU; i++) {
    //   printf("%d(%d) ", choices[i], kmem[choices[i]].freelist_len);
    // }
    // printf("\n");
    
    for (int i = 0; i < NCPU; i++) {
      // int nowid = choices[i];
      int nowid = (i + 1) % NCPU;
      if (nowid == id) continue;

      acquire(&kmem[nowid].lock);

      if (kmem[nowid].freelist_len > 0) {
        acquire(&kmem[id].lock);

        kmem[id].freelist = kmem[nowid].freelist;
        kmem[id].freelist_len = kmem[nowid].freelist_len;

        kmem[nowid].freelist = 0;
        kmem[nowid].freelist_len = 0;

        release(&kmem[nowid].lock);
        break;
      }

      kmem[nowid].is_active = 0;
      release(&kmem[nowid].lock);
    }
  }

  if (kmem[id].freelist_len > 0) {
    r = kmem[id].freelist;
    kmem[id].freelist = r->next;
    kmem[id].freelist_len--;
    memset((char*)r, 5, PGSIZE); // fill with junk
  } else {
    // printf("kalloc: no free page\n");
    return 0;
  }

  release(&kmem[id].lock);
  return (void*)r;
}