# lab03 PageTable

> [!IMPORTANT]  
> The following hint may contain a decisive solution. If you wish to solve the problem by yourself, **DO NOT** view it. If you find it helpful, consider staring this repository!

## add ugetid system calls

It aleray has ugetid in `user/ulib.c`, we only need implement the related data structure in kernel.

**user/ulib.c**
```c
int ugetpid(void) {
  struct usyscall *u = (struct usyscall *)USYSCALL;
  return u->pid;
}
```

Implementing it is too easy, just refer to the trapframe handling.  Here are some examples:

**kernel/proc.c/allocproc**
```c  
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Allocate a usyscall page.
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

  // just refer it like trapframe
  if(mappages(pagetable, USYSCALL, PGSIZE, (uint64)(p->usyscall), PTE_V | PTE_U | PTE_R) < 0){
    printf("map usyscall error.\n");
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }
```

## print page table

Too too too too ... easy. Just use a recursive function. You should know the structure of pagetable in xv6 and the usage of `pte`. Its high bits represent the jump address, and the low bits represent some flags. 

We can use these flags to judge whether it points to the final address or the starting address of the next-level page table.

```c
void vmprint_one(pagetable_t pagetable, int level) {
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if (pte & PTE_V) {
      for (int j = 0; j < level; j++) {
        printf(" ..");
      }
      uint64 idx = (uint64)(i*PGSIZE);
      printf("%p: pte %p pa %p\n", (void*)(idx), (void*)pte, (void*)PTE2PA(pte));

      if((pte & (PTE_R|PTE_W|PTE_X)) == 0){
        // this PTE points to a lower-level page table.
        uint64 child = PTE2PA(pte);
        vmprint_one((pagetable_t)child, level + 1);
      }
    }
  }

}
void vmprint(pagetable_t pagetable) {
  // your code here
  printf("page table %p\n", pagetable);
  vmprint_one(pagetable, 1);
}
```

## use super pages

**3rd most time-consuming problem**

### The first question
How can a page table be designed to allocate both normal pages and superpages? Can normal pages and superpages be mixed in the same allocation?

We cannot simply mix them, because certain functions like `free` require address alignment, and we have no way to determine whether a given address belongs to a normal page or a superpage. One possible solution is to introduce a special flag in the page table entries (pte) when allocating superpages; all pages within a superpage should carry this special flag.

However, even if we can determine whether a page is a superpage, it remains difficult to compute the base (starting) address of the page. The previous alignment strategy was effective because each page was 4KB in size, allowing the base address to be calculated by masking out the lower bits of the virtual address.  This method fails when normal pages and superpages are mixed, as it cannot reliably determine page boundaries under varying page sizes.

```c
void freerange(void *pa_start, void *pa_end) {
  char *p;
  // #define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1))
  p = (char*)PGROUNDUP((uint64)pa_start); // <-- this
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}
```

### Split two parts

So for simplicity, we partition the memory into two regions: one for normal pages and the other for superpages. When allocating or freeing a page, we must first determine whether the target address belongs to a superpage.

**memlayout.h**
```c
#define KERNBASE 0x80000000L
#define PHYSTOP_SUPERPAGE (KERNBASE + 32*(2*1024*1024)) // 2MB Superpage
#define PHYSTOP (KERNBASE + 128*1024*1024)
```

### Allocate memory

It is straightforward when allocating: just check the requested size, then set the corresponding value in the pagetable. The OS creates a superpage by setting the PTE_V and PTE_R bits in the level-1 PTE and setting the physical address to the start of a two-megabyte region of physical memory.

Following the approach used for allocating normal pages, the overall flow is as follows:
```
sbrk(n) --> growproc(n) --> uvmalloc_super() --> kalloc_super(request the physical memory) + mappages_super(set the value in the pagetable)
```

The details of the functions involved are as follows:

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

In `uvmalloc_super`, it performs two steps: use `kalloc_super` to allocate physical memory for the user and use `mappage_super` to update the pagetable.

It is straightforward in `kalloc_super`, we can just follow the logic of `kalloc`. After it returns, we have the physical address of the memory. In `mappage_super`, since superpage are mapped using level-1 page table, so we can make the code clearly.

```cpp
// va --> pa in pagetable
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

// This function is only for superpage, it will find the level-1 pagetable for the va.
pte_t * walk_super(pagetable_t pagetable, uint64 va, int alloc)
{
  // ...

  // level-2
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

  // level-1
  pte = &pagetable[PX(1, va)];
  return pte;
}
```

### Free memory

We should add basic support for superpages in functions like `kfree`, `uvmdealloc`, and `uvmfree`.

In addition, we need to understand the memory freeing process to identify any other functions that may need to be modified.

The flow of freeing memory is as follows:
```
freeproc --> proc_freepagetable(traverse pagetable and free each used memory) -->
uvmfree(pagetable) --> uvmunmap(pagetable, va, npages) (free specific address) --> kfree
```

**vm.c**
```cpp
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  // freewalk just frees the page table itself; it is unrelated to superpages.
  freewalk(pagetable);
}
```

We can add a new function called `uvmunmap_super`, and call either it or `uvmunmap` within `uvmfree`. However, `uvmfree` is currently very simple, it only calls `uvmunmap` once and relies on `uvmunmap` to handle the detailed cleanup. In other words, `uvmunmap` is responsible for traversing each page and calling `kfree` to free them.

Therefore, it is more reasonable to make the changes in `uvmunmap`. As it traverses each page, it can check whether the page belongs to a superpage and handle it accordingly.

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

How can we check it? We can use the previously defined `walk_super` function to access the level-1 page table. Recall that the superpage's level-1 PTE will have both the PTE_V and PTE_R bits set. Otherwise, the PTE_R bit will not be set in the level-1 PTE.

```cpp
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  // ...

  for(a = va; a < va + npages*PGSIZE; a += sz){
    // Use walk_super to check whether it is in a superpage.
    pte = walk_super(pagetable, a);
    if(pte && (*pte & PTE_R) && (*pte & PTE_V)) {
      // Free the super page
      sz = SUPERPGSIZE;

      if(do_free){
        uint64 pa = PTE2PA(*pte);
        kfree_super((void*)pa);
      }
      *pte = 0;
    } else {
      // Free the normal page
    }
  }
}
```

So, is that all? Not quite. There's a subtle and easily overlooked modification that can be quite tricky to debug.

Superpages may cause virtual addresses to be non-contiguous. Consider the following situation: if a user's size is `0x5000` now, then the user requests a superpage, the starting virtual address of the new memory will be aligned to `0x200000` (since a superpage is 2MB). As a result, there will be a gap between `0x5000` and `0x200000`: a hole in the virtual address space.

In the previous implementation, it was considered illegal if a page's PTE did not have the `PTE_V` flag set. We should remove this restriction.

```cpp
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  // ...

  for(a = va; a < va + npages*PGSIZE; a += sz){
    // Check whether it is a superpage
    pte = walk_super(pagetable, a);
    if(pte && (*pte & PTE_R) && (*pte & PTE_V)) {
      // Free the superpage
    } else {
      // Free the normal page
      // ...

      if((*pte & PTE_V) == 0) {
        // --> Remove the restriction where a page must have `PTE_V` flags.
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

Lastly, we need to modify `uvmcopy` to support superpages when use `fork`. The approach is similar to the previous section: traverse the old page table, use `walk_super` to check whether a page is the start of a superpage, and handle it accordingly.