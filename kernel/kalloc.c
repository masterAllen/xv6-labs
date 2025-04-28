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

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// void *
// kalloc(void)
// {
//   struct run *r;

//   push_off();
//   int steal_num = 0;

//   int id = cpuid();
//   acquire(&kmem[id].lock);

//   if (kmem[id].freelist_len == 0) {

//     // 选择其他空闲页最多的核心偷一半过来
//     int max_freelist_len = 0;
//     int max_freelist_id = 0;
//     for (int i = 0; i < NCPU; i++) {
//       if (i == id) continue;

//       acquire(&kmem[i].lock);
//       if (kmem[i].freelist_len > max_freelist_len) {
//         max_freelist_len = kmem[i].freelist_len;
//         max_freelist_id = i;
//       }
//       release(&kmem[i].lock);
//     }

//     // 最大的就这个，那没办法了，只能返回 0
//     if (max_freelist_id == id) {
//       release(&kmem[id].lock);
//       pop_off();
//       return 0;
//     }

//     // 如果锁在里面，会造成并发错误了... 比如离开这个函数之后，可能 max_freelist_len 就减一了..
//     acquire(&kmem[max_freelist_id].lock);

//     max_freelist_len = kmem[max_freelist_id].freelist_len;
//     printf("now id = %d, steal_num = %d\n", id, steal_num);
//     for (int i = 0; i < NCPU; i++) {
//       printf("kmem[%d].freelist_len: %d\n", i, kmem[i].freelist_len);
//     }
//     if (max_freelist_len > 0) {

//       if (max_freelist_len == 1) {
//         // 如果被偷的人只有一个，则直接偷
//         kmem[id].freelist = kmem[max_freelist_id].freelist;
//         kmem[id].freelist_len = 1;
//         kmem[max_freelist_id].freelist = 0;
//         kmem[max_freelist_id].freelist_len = 0;
//         steal_num = 1;

//       } else {
//         // 偷一半过来
//         int half_freelist_len = max_freelist_len / 2;
//         // 开始偷
//         struct run *nowmem = kmem[max_freelist_id].freelist;
//         struct run *premem = nowmem;
//         for (int i = 0; i < half_freelist_len; i++) {
//           premem = nowmem;
//           nowmem = nowmem->next;
//         }

//         steal_num = max_freelist_len - half_freelist_len;

//         // 处理偷的人
//         kmem[id].freelist = nowmem;
//         kmem[id].freelist_len += steal_num;

//         // 处理被偷的人，一个是把结尾链表置空，一个是减去数量
//         premem->next = 0;
//         kmem[max_freelist_id].freelist_len = half_freelist_len;
//       }

//     } else {
//       // 整个系统完全没有空闲
//       // panic("kalloc: no free page");
//       printf("kalloc: no free page\n");

//       // 结束返回
//       release(&kmem[max_freelist_id].lock);
//       release(&kmem[id].lock);
//       pop_off();
//       return 0;
//     }
//     release(&kmem[max_freelist_id].lock);
//   }

//   r = kmem[id].freelist;
//   if (r == 0) {
//     // printf("now id = %d, steal_num = %d\n", id, steal_num);
//     // for (int i = 0; i < NCPU; i++) {
//     //   printf("kmem[%d].freelist_len: %d\n", i, kmem[i].freelist_len);
//     // }
//   }

//   kmem[id].freelist = r->next;
//   kmem[id].freelist_len--;
//   memset((char*)r, 5, PGSIZE); // fill with junk

//   release(&kmem[id].lock);
//   pop_off();
//   return (void*)r;
// }

// void *
// kalloc(void)
// {
//   struct run *r;

//   push_off();
//   int id = cpuid();
//   pop_off();

//   acquire(&kmem[id].lock);

//   if (kmem[id].freelist_len == 0) {
//     release(&kmem[id].lock);

//     // 选择其他空闲页最多的核心偷一半过来
//     for (int i = 1; i < NCPU; i++) {
//       int nowid = (id + i) % NCPU;

//       if (kmem[nowid].freelist_len > 0) {
//         acquire(&kmem[nowid].lock);
//         int now_freelist_len = kmem[nowid].freelist_len;
//         if (now_freelist_len == 0) {
//           release(&kmem[nowid].lock);
//           continue;
//         }

//         acquire(&kmem[id].lock);
//         if (now_freelist_len == 1) {
//           // 如果被偷的人只有一个，则直接偷
//           kmem[id].freelist = kmem[nowid].freelist;
//           kmem[id].freelist_len = 1;
//           kmem[nowid].freelist = 0;
//           kmem[nowid].freelist_len = 0;

//         } else {

//           // 偷一半过来
//           int half_freelist_len = now_freelist_len / 2;
//           // 开始偷
//           struct run *nowmem = kmem[nowid].freelist;
//           struct run *premem = nowmem;
//           for (int i = 0; i < half_freelist_len; i++) {
//             premem = nowmem;
//             nowmem = nowmem->next;
//           }

//           // 处理偷的人
//           kmem[id].freelist = nowmem;
//           kmem[id].freelist_len += (now_freelist_len - half_freelist_len);

//           // 处理被偷的人，一个是把结尾链表置空，一个是减去数量
//           premem->next = 0;
//           kmem[nowid].freelist_len = half_freelist_len;
//         }
//         release(&kmem[nowid].lock);
//         break;
//       }
//     }
//   }

//   if (kmem[id].freelist_len > 0) {
//     r = kmem[id].freelist;
//     kmem[id].freelist = r->next;
//     kmem[id].freelist_len--;
//     memset((char*)r, 5, PGSIZE); // fill with junk
//   } else {
//     return 0;
//   }

//   release(&kmem[id].lock);
//   return (void*)r;
// }

void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();
  pop_off();

  // while (atomic_read4((int*) &(kmem[id].lock.locked)) == 1) {
  //   printf("kmem[%d].lock.locked\n", id);
  // }

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
      int nowid = choices[i];
      if (nowid == id) continue;

      acquire2(&kmem[nowid].lock);

      if (kmem[nowid].freelist_len > 0) {
        // int steal_num = 1;
        // int steal_num = kmem[nowid].freelist_len > 1024 ? 1024 : kmem[nowid].freelist_len;
        // int steal_num = (kmem[nowid].freelist_len) / 2;
        // steal_num = steal_num == 0 ? 1 : steal_num;
        // printf("%d steal %d, num = %d\n", id, nowid, steal_num);

        acquire3(&kmem[id].lock);

        kmem[id].freelist = kmem[nowid].freelist;
        kmem[id].freelist_len = kmem[nowid].freelist_len;

        kmem[nowid].freelist = 0;
        kmem[nowid].freelist_len = 0;

        // struct run *nowmem = kmem[nowid].freelist;
        // kmem[id].freelist = nowmem;
        // for (int j = 0; j < steal_num; j++) {
        //   nowmem = nowmem->next;
        // }
        // kmem[nowid].freelist = nowmem;

        // kmem[id].freelist_len = steal_num;
        // kmem[nowid].freelist_len -= steal_num;

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