中文 | [English](./../en/lab03_pagetable.md)

# lab03 PageTable

> [!IMPORTANT]  
> 以下文章可能会对您的解决有决定性的帮助。如果您希望自己独立解决，**请勿**查看。如果您觉得有帮助，请考虑 Star 这个仓库！

## add ugetpid syscall

`user/ulib.c` 中已经有 ugetid，我们只需要在内核中实现相关的数据结构。

**user/ulib.c**
```c
int ugetpid(void) {
  struct usyscall *u = (struct usyscall *)USYSCALL;
  return u->pid;
}
```

实现起来太简单了，只需参考 trapframe 的处理。以下是一些示例：

**kernel/proc.c/allocproc**
```c  
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 分配一个 usyscall 页面。
  if((p->usyscall = (struct usyscall *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
```

**kernel/proc.c/proc_pagetable**
```c  
  if(mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  // 就像 trapframe 一样引用它
  if(mappages(pagetable, USYSCALL, PGSIZE, (uint64)(p->usyscall), PTE_V | PTE_U | PTE_R) < 0){
    printf("map usyscall error.\n");
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }
```

## print pagetable

太太太太...简单了，只需使用递归函数。需要知道 xv6 中页表的结构和 `pte` 的用法。它的高位表示跳转地址，低位表示一些标志。

可以使用这些标志来判断它是指向最终地址还是下一级页表的起始地址。

```c
void vmprint_one(pagetable_t pagetable, int level) {
  // 一个页表中有 2^9 = 512 个 PTE。
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if (pte & PTE_V) {
      for (int j = 0; j < level; j++) {
        printf(" ..");
      }
      uint64 idx = (uint64)(i*PGSIZE);
      printf("%p: pte %p pa %p\n", (void*)(idx), (void*)pte, (void*)PTE2PA(pte));

      if((pte & (PTE_R|PTE_W|PTE_X)) == 0){
        // 这个 PTE 指向一个低级页表。
        uint64 child = PTE2PA(pte);
        vmprint_one((pagetable_t)child, level + 1);
      }
    }
  }

}
void vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  vmprint_one(pagetable, 1);
}
```

## use superpages

**第三耗时的问题**

### 第一个问题：两种页是否混合分配

页表如何设计才能同时分配普通页和超级页？普通页和超级页可以在同一分配中混合吗？

一开始花了些时间去实现混合，但很难，因为某些函数如 `free` 需要地址对齐，无法确定传入的地址属于普通页还是超级页。一个可能的方案是在分配超级页时，在页表项（pte）中引入特殊标志；超级页内的所有页面都应该携带这个特殊标志。

但即使我们可以确定页面是否为超级页，计算页面的基址（起始）地址仍然很困难。之前的对齐策略是有效的原因，是因为每个页面都是 4KB 大小，所以直接清空虚拟地址的低位就能得到基址。当普通页和超级页混合时，对于超级页而言，没办法直接清空低位就得到基地址了。

```c
void freerange(void *pa_start, void *pa_end) {
  char *p;
  // #define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1))
  p = (char*)PGROUNDUP((uint64)pa_start); // <-- 之前可直接利用 PGROUNDUP 来获取基地址
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}
```

### 内存分成两部分

所以为了简单起见，我们将内存分为两个区域：一个用于普通页，另一个用于超级页。当分配或释放页面时，我们必须首先确定目标地址是否属于超级页。

**memlayout.h**
```c
#define KERNBASE 0x80000000L
#define PHYSTOP_SUPERPAGE (KERNBASE + 32*(2*1024*1024)) // 2MB 超级页
#define PHYSTOP (KERNBASE + 128*1024*1024)
```

### 分配内存

分配时很简单：只需检查请求的大小，然后在页表中设置相应的值。通过在1级 PTE 中设置 PTE_V 和 PTE_R 位并将物理地址设置为2兆字节物理内存区域的开始来创建超级页。

按照分配普通页的方法，整体流程如下：
```
sbrk(n) --> growproc(n) --> uvmalloc_super() --> kalloc_super(请求物理内存) + mappages_super(在页表中设置值)
```

涉及函数的详细信息如下：

**proc.c**
```cpp
int growproc(int n) {
  // ...
  if (n > SUPERPGSIZE) {
    sz = p->sz;
    if((sz = uvmalloc_super(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    // ...
}
```

**vm.c**
```cpp
uint64 uvmalloc_super(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm) {
  // ...
  sz = SUPERPGSIZE;
  oldsz = SUPERPGROUNDUP(oldsz);

  for(a = oldsz; a < newsz; a += sz){
    mem = kalloc_super();
    // ...
    if(mappages_super(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree_super(mem);
      uvmdealloc_super(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}
```

在 `uvmalloc_super` 中，它执行两个步骤：使用 `kalloc_super` 为用户分配物理内存，使用 `mappage_super` 更新页表。

在 `kalloc_super` 中很简单，我们可以直接遵循 `kalloc` 的逻辑。它返回后，我们就有了内存的物理地址。在 `mappage_super` 中，由于超级页使用1级页表映射，所以我们可以使代码更清晰。

```cpp
// 在页表中 va --> pa
int mappages_super(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
  // ...
  
  a = va;
  last = va + size - SUPERPGSIZE;
  for(;;){
    pte = walk_super(pagetable, a, 1);
    if(pte == 0)
      return -1;

    *pte = PA2PTE(pa) | perm | PTE_V | PTE_R;
    if(a == last)
      break;
    a += SUPERPGSIZE;
    pa += SUPERPGSIZE;
  }
  return 0;
}

// 这个函数只用于超级页，它会为 va 找到1级页表。
pte_t * walk_super(pagetable_t pagetable, uint64 va, int alloc)
{
  // ...

  // 2级
  pte_t *pte = &pagetable[PX(2, va)];
  if(!(*pte) && alloc) {
    pagetable = (pde_t*)kalloc();
    memset(pagetable, 0, PGSIZE);
    *pte = PA2PTE(pagetable) | PTE_V;
  }
  if(!(*pte & PTE_V) && alloc) {
    return 0;
  }
  pagetable = (pagetable_t)PTE2PA(*pte);

  // 1级
  pte = &pagetable[PX(1, va)];
  return pte;
}
```

### 释放内存

我们应该在 `kfree`、`uvmdealloc` 和 `uvmfree` 等函数中添加对超级页的基本支持。

此外，我们需要理解内存释放过程，以识别可能需要修改的其他函数。

释放内存的流程如下：
```
freeproc --> proc_freepagetable(遍历页表并释放每个使用的内存) -->
uvmfree(pagetable) --> uvmunmap(pagetable, va, npages)(释放特定地址) --> kfree
```

**vm.c**
```cpp
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  // freewalk 只是释放页表本身；它与超级页无关。
  freewalk(pagetable);
}
```

我们可以添加一个名为 `uvmunmap_super` 的新函数，并在 `uvmfree` 中调用它或 `uvmunmap`。但是，`uvmfree` 目前非常简单，它只调用 `uvmunmap` 一次，并依赖 `uvmunmap` 来处理详细的清理。换句话说，`uvmunmap` 负责遍历每个页面并调用 `kfree` 来释放它们。

因此，在 `uvmunmap` 中进行更改更合理。当它遍历每个页面时，可以检查页面是否属于超级页并相应地处理。

```cpp
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  // ...

  for(a = va; a < va + npages*PGSIZE; a += sz){
    sz = PGSIZE;
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0) {
      printf("va=%ld pte=%ld\n", a, *pte);
      panic("uvmunmap: not mapped");
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}
```

我们如何检查它？我们可以使用之前定义的 `walk_super` 函数来访问1级页表。回想一下，超级页的1级 PTE 将同时设置 PTE_V 和 PTE_R 位。否则，1级 PTE 中不会设置 PTE_R 位。

```cpp
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  // ...

  for(a = va; a < va + npages*PGSIZE; a += sz){
    // 使用 walk_super 检查它是否在超级页中。
    pte = walk_super(pagetable, a);
    if(pte && (*pte & PTE_R) && (*pte & PTE_V)) {
      // 释放超级页
      sz = SUPERPGSIZE;

      if(do_free){
        uint64 pa = PTE2PA(*pte);
        kfree_super((void*)pa);
      }
      *pte = 0;
    } else {
      // 释放普通页
    }
  }
}
```

### 一个微妙的细节：允许虚拟地址中的空洞

那么就结束了？有一个微妙且容易被忽视的修改，调试起来可能很棘手。

超级页可能导致虚拟地址不连续。考虑以下情况：如果用户现在的大小是 `0x5000`，然后用户请求一个超级页，新内存的起始虚拟地址将对齐到 `0x200000`（因为超级页是2MB）。结果，`0x5000` 和 `0x200000` 之间会有一个间隙：虚拟地址空间中的一个空洞。

在之前的实现中，如果页面的 PTE 没有设置 `PTE_V` 标志，就被认为是非法的。所以应该移除这个限制。

```cpp
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  // ...

  for(a = va; a < va + npages*PGSIZE; a += sz){
    // 检查它是否是超级页
    pte = walk_super(pagetable, a);
    if(pte && (*pte & PTE_R) && (*pte & PTE_V)) {
      // 释放超级页
    } else {
      // 释放普通页
      // ...

      if((*pte & PTE_V) == 0) {
        // --> 移除页面必须具有 `PTE_V` 标志的限制。
        // panic("uvmunmap: not mapped");
        continue;
      }
      if(PTE_FLAGS(*pte) == PTE_V)
        panic("uvmunmap: not a leaf");
      // ...
    }
  }
}
```

### Fork

最后，修改 `uvmcopy` 让使用 `fork` 时支持超级页。方法与前一节类似：遍历旧页表，使用 `walk_super` 检查页面是否是超级页的开始，并相应地处理。 