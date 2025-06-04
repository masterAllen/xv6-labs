# lab05_copyonwrite

> [!IMPORTANT]  
> The following hint may contain a decisive solution. If you wish to solve the problem by yourself, **DO NOT** view it. If you find it helpful, consider staring this repository!

## introduce a new flag

Too straightforward. Nothing worth to say.

```cpp
// riscv.h
#define PTE_COW_W (1L << 5)

// vm.c
// fork --> uvmcopy
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
    // ...
    for(i = 0; i < sz; i += PGSIZE) {
        pa = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);

        // disable it
        // if((mem = kalloc()) == 0)

        // If a page can be written, this page should be handled by the COW mechanism.
        if (flags & PTE_W) {
            flags = flags & ~PTE_W;
            flags = flags | PTE_COW_W;
        }
        // ...

        // We need to combine the old physical address with this virtual address.
        if (mappages(new, i, PGSIZE, pa, flags) != 0) {
            // ...
        }
    }
}
```

Handle the copy-on-write fault in the exception.

```cpp
// trap.c
void usertrap(void) {
    // ...
    // r_scause -> return CSR(control and status register) which indicates the type of trap. 
    // Store access fault is 0xf.
    uint64 scause = r_scause();
    // ...
    else if (scause == 0xf) {
        // get the virutal address
        uint64 oldpage_va = r_stavl();
        oldpage_va = PGROUNDDOWN(oldpage_va);
    
        pagetable_t pagetable = p->pagetable;
        pte_t *pte = walk(pagetable, oldpage_va, 0);

        uint flags = PTE_FLAGS(*pte);

        // check whether it is a copy-on-write page by checking the cow flag.
        if ((flags & PTE_V) && (flags & PTE_COW_W)) {
            // allocate a new physical memory and copy the content from old physical memory
            uint64 newpage_pa = (uint64)kalloc();
            uint64 oldpage_pa = PTE2PA(*pte);
            memmove((void*)newpage_pa, (void*)oldpage_pa, PGSIZE);

            // Clear Flags
            flags = flags & ~PTE_COW_W;
            flags = flags | PTE_W;

            // Update pagetable
            if (mappages(pagetable, oldpage_va, PGSIZE, newpage_pa, flags) != 0){
                // ...
            }
            *pte = PA2PTE(newpage_pa) | flags;

            // ...
        }
        // ...
    }
}
```

## free and malloc physical memory

COW introduce a new problem: when freeing a page marked as copy-on-write, we need to carefully determine whether the physical memory should actually be freed.

The hint already tells us to use a reference count. For simplicity, we'll use an array-based implementation and ignore any potential space overhead caused by definition.

```cpp
// kalloc.c
struct {
  struct spinlock lock;
  // This implementation wastes some space (a more precise size would be PHYSTOP - KERNELBASE), but for simplicity, we ignore it.
  uint ref_count[PHYSTOP / PGSIZE];
} ref_count;

// kinit() and freerange() require changes, which are not shown here for brevity.

void* kalloc() {
    // ...
    acquire(&ref_count.lock);
    ref_count.ref_count[(uint64)r / PGSIZE] = 1;
    release(&ref_count.lock);
    // ...
}
```

Add two functions to modify the reference count, and use them when handling copy-on-write pages as described in the previous section.

```cpp
// kalloc.c
void kadd_ref_count(void *pa) {
  acquire(&ref_count.lock);
  ref_count.ref_count[(uint64)pa / PGSIZE]++;
  release(&ref_count.lock);
}

void ksub_ref_count(void *pa) {
  acquire(&ref_count.lock);
  ref_count.ref_count[(uint64)pa / PGSIZE]--;
  release(&ref_count.lock);
}

// vm.c
// fork → uvmcopy: pages may be marked as copy-on-write in this function.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
    // ...
    // when a page is marked as copy-on-write, add the reference count of the old physical address
    kadd_ref_count((void*)pa);
}

// trap.c
// This function is called when a write is attempted on a copy-on-write page.
void usertrap(void) {
    // ...
    // After successfully allocating new memory and copying data from the old physical address,
    // we should decrement the reference count of the old physical page.
    ksub_ref_count((void*)oldpage_pa);
}
```

So is this okay? NO! There is a mistake in the previously shown code snippet. In the `usertrap`, when the old physical address has been called `kfree` in the parent process, we should free this address truly. But `ksub_ref_count` just decrement the count and do not free it.

Therefore, we should adjust `kfree` to check whether free the address truly and use it in `usertrap`.

```cpp
void usertrap(void) {
    // ...
    // use kfree instead of ksub_ref_count
    kfree((void*)oldpage_pa);
}

// kalloc.c
void kfree(void *pa) {
    // ...
    acquire(&ref_count.lock);
    ref_count.ref_count[page_index]--;

    // if reference count is 0, free it truly.
    if (ref_count.ref_count[page_index] == 0) {
        // ...
    }
    release(&ref_count.lock);
}
```

## copyout

Lastly, as the hints say, modify `copyout()` to use the same scheme as `usertrap` when it encounters a COW page.

```cpp
// copyput: src is physical address, copy [src:src+len] to a new memory, add map its virtual address as dstva
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    // ...
    while(len > 0){
        va0 = PGROUNDDOWN(dstva);

        // check COW (dont show it now)
        if (is_cow) {
            uint64 newpage_pa = (uint64)kalloc();
            uint64 oldpage_pa = PTE2PA(*pte);
            memmove((void*)newpage_pa, (void*)oldpage_pa, PGSIZE);

            // Update Flags
            uint flags = PTE_FLAGS(*pte);
            flags = flags & ~PTE_COW_W;
            flags = flags | PTE_W;

            // Update Pagetable
            if(mappages(pagetable, va0, PGSIZE, newpage_pa, flags) != 0){
              // ...
            }
            *pte = PA2PTE(newpage_pa) | flags;

            // Same as usertrap 
            kfree((void*)oldpage_pa);
            pa0 = newpage_pa;
        }
        // copyout. Copy the src memmory to the new allocated memory.
        memmove((void *)(pa0 + (dstva - va0)), src, n);
        // ...
    }
    return 0;
}
```

However, there is a slight difference. In `copyout`, the newly allocated memory for a COW page will soon be overwritten by the final `memmove`. Therefore, it's not always necessary to copy the contents of the old physical memory to the new page.  

More specifically, we only need to copy the old content if we're not going to overwrite the entire page from `src`. This situation only occurs at the beginning or the end of the range.

```cpp
// copyput: src is physical address, copy [src:src+len] to a new memory, add map its virtual address as dstva
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    // ...
    while(len > 0){
        va0 = PGROUNDDOWN(dstva);
        pa0 = PTE2PA(*pte);

        // n is the copy size (from src) in this time
        n = PGSIZE - (dstva - va0);
        if(n > len)
            n = len;

        // check COW (dont show it now)
        if (is_cow) {
            uint64 newpage_pa = (uint64)kalloc();
            uint64 oldpage_pa = PTE2PA(*pte);

            // Key idea: only need to copy the old content if we will not overwrite entire page from src.
            // memmove((void*)newpage_pa, (void*)oldpage_pa, PGSIZE);
            if (n != PGSIZE)
                memmove((void*)newpage_pa, (void*)oldpage_pa, PGSIZE);

            // ...
            pa0 = newpage_pa;
        }
        // copyout. Copy the src memmory to the new allocated memory.
        memmove((void *)(pa0 + (dstva - va0)), src, n);
        // ...

        // update some thing
        len -= n; 
        src += n; 
        dstva = va0 + PGSIZE; // not dstva += PGSIZE. va0 must be the start of one page.
    }
    return 0;
}
```

To sum up, the points above represent the key takeaways I found valuable in this lab.