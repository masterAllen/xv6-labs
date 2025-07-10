[中文](./../zh/lab09_mmap_zh.md) | English

# lab09_mmap

> [!IMPORTANT]  
> The following hint may contain a decisive solution. If you wish to solve the problem by yourself, **DO NOT** view it. If you find it helpful, consider staring this repository!

## Implementing `mmap`

### 1. Choosing the Right Data Structure

The first question we need to address is: what data structure should we use to implement `mmap`?

Initially, one might think about implementing normal virtual-to-physical address mapping and marking the area as mapped. However, `mmap` has a unique characteristic - it deals with files, so we don't need to handle it that way.

As the hints suggest, we can use VMA (Virtual Memory Area) to combine virtual addresses with files. We allocate an area in the process for VMA, with each VMA corresponding to one mmap.

```cpp
#define MAX_VMA 10

struct vma {
  uint64 start;      // Starting virtual address
  uint64 end;        // Ending virtual address
  int prot;          // Protection (read/write)
  int flags;         // MAP_SHARED or MAP_PRIVATE
  struct file *f;    // Pointer to mapped file
  int offset;        // File offset
  int valid;         // Flag indicating if this VMA is valid
};

// Per-process state
struct proc {
    // ...
    struct file *ofile[NOFILE];  // Open files <-- Will be used later
    // ...
    struct vma vmas[MAX_VMA];
};
```

### 2. Getting args in `sys_mmap`

Let's implement `sys_mmap`. First, we need to get the arguments. The definition of `mmap` is: `void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)`.

Here's the implementation for getting the arguments:

```cpp
uint64 sys_mmap(void) {
  // mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
  uint64 addr;
  size_t len;
  int prot;
  int flags;
  int fd;
  int offset;
  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  // ...
}
```

### 3. Choosing Virtual Address

Implementing the rest of `sys_mmap` is straightforward. We just need to search for an empty VMA and save the mmap information in that area.

```cpp
  acquire(&p->lock);
  // Find an empty VMA
  for (int i = 0; i < MAX_VMA; i++) {
    if (p->vmas[i].valid == 0) {
        // ...
    }
  }
```

For allocating a new virtual address area, we can follow the approach used in `sys_sbrk`, which uses `sz` as the starting address for the new virtual area. This scheme might create holes and leave virtual areas unused after being freed, but for simplicity, we'll use this approach:

```cpp
      // Following sys_sbrk's approach
      uint64 va = PGROUNDUP(p->sz);
      for (int j = 0; j < append_sz; j += PGSIZE) {
        // ...
        if(mappages(p->pagetable, va+j, PGSIZE, pa, flags) != 0){
          release(&p->lock);
          return -1;
        }
      }
      // ...
```

### 4. Choosing Physical Address

Next, we need to map these new virtual addresses to the correct physical addresses. We should find the file's address and set the physical address accordingly.

Is it that simple? No! We **DON'T** have the physical address of the file! To be more precise, when we use `open(fname)`, we don't load the file content into memory! **I think this is the key insight for implementing `mmap`!**

What we should do is similar to copy-on-write - lazy mapping. If a page is accessed, the kernel will trigger a trap, and then we handle it by mapping to the real physical address.

### 5. Setting Correct Flags

Next, we need to choose appropriate flags in the page table. Initially, I thought we should set the `prot` from `mmap` as the flags. For example, if `mmap(..., prot=PROT_READ)`, then set the flag as `PTE_R`.

However, this is incorrect. Remember that the kernel checks flags in copy-on-write - whether reading an unreadable page or writing an unwriteable page. If we mark the page as `PTE_R`, the kernel will consider it a valid operation when reading it.

We'll follow the copy-on-write approach by creating a new flag and marking the page with this flag. When the page is accessed, the kernel will check the flags and trigger a trap, allowing us to handle it.

```cpp
      uint64 va = PGROUNDUP(p->sz);
      uint64 pa = 0;

      uint64 append_sz = len + len%PGSIZE;
      for (int j = 0; j < append_sz; j += PGSIZE) {
        // Set correct flags
        uint flags = PTE_V | PTE_U | PTE_M;

        if(mappages(p->pagetable, va+j, PGSIZE, pa, flags) != 0){
          // ...
        }
      }
      p->sz += append_sz;

      p->vmas[i].valid = 1;
      p->vmas[i].prot = prot;
      p->vmas[i].flags = flags;
      p->vmas[i].f = p->ofile[fd];
      p->vmas[i].offset = offset;
      // ...

      // Add file reference
      filedup(p->vmas[i].f);
```

### 6. Handling Traps: Loading File to Destination Physical Address

Now we need to handle traps by actually loading the file to the destination. We can follow the approach used in `filewrite` in `file.c`.

There's an important detail: we should not modify the file's offset after our operation.

I suggest creating `file_to_memory` function. Besiding making code more clear, it also helps us avoid changing the header in `trap.c`. (If you just use the same code in the `usertrap` in `trap.c`, the compiler will throw include errors.)

**A key trick in lazy mapping: Only load the page that triggered the trap.**

```cpp
int fileread(struct file *f, uint64 addr, int n) {
  // ...
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  // ...
}

int file_to_pa(struct file *f, uint64 addr, uint off, int n) {
  // ...
  ilock(f->ip);
  // We don't change the offset
  r = readi(f->ip, 0, addr, off, n);
  iunlock(f->ip);
}
```

### 7. Other Trap Operations

The other operations in the trap handler are straightforward. We allocate a new physical area, use the previous function to load the content, and then set the correct flags.

An important detail: because we use `readi` to load file content, which ultimately uses `copyin`, we need to **mark the page as `PTE_W` anyway**, then set the final flags after `readi`.

```cpp
  } else if(r_scause() == 0xd || r_scause() == 0xf){
    // ... (get the va, flags...)

    // Check if it's a mmap page
    if ((flags & PTE_V) && (flags & PTE_M)) {
      for (int i = 0; i < MAX_VMA; i++) {
        if (p->vmas[i].valid == 1 && p->vmas[i].start <= va && p->vmas[i].end > va) {
          // Allocate a new physical area
          char *pa = kalloc();

          // We must mark PTE_W first!
          if(mappages(pagetable, va, PGSIZE, (uint64)pa, flags | PTE_W) != 0){
            // ...
          }

          // Load file into the physical area
          memset(pa, 0, PGSIZE);
          struct file *f = p->vmas[i].f;
          uint off = va - p->vmas[i].start;
          int n = (va + PGSIZE) > p->vmas[i].end ? p->vmas[i].end - va : PGSIZE;
          file_to_pa(f, (uint64)pa, off, n);

          // Set the final flags
          flags = (flags | PTE_R) & ~PTE_M;
          flags = (p->vmas[i].prot & PROT_WRITE) ? flags | PTE_W : flags & ~PTE_W;
          update_pte(pagetable, va, flags); // My custom function
        }
      }
    }
```

## Implementing `unmmap`

### 1. Finding the Corresponding VMA

The first step is finding the corresponding VMA. This is straightforward:

```cpp
  for (int i = 0; i < MAX_VMA; i++) {
    if (p->vmas[i].valid == 1 && p->vmas[i].start <= start && p->vmas[i].end >= end) {
```

### 2. Updating the Mapped Address (Initial Attempt)

It's important to understand that `unmmap` can unmap a part of the area. This lab ensures not unmapping the center portion of an area, which would split the area. Given this constraint, updating is simple:

```cpp
      if (p->vmas[i].start == start) {
        p->vmas[i].start = end;
      } else if (p->vmas[i].end == end) {
        p->vmas[i].end = start;
      }
```

Actually, this isn't quite correct. But let's continue and we'll address this later.

### 3. Writing Back to the File

When unmapping an area, we need to write the changes back to the file. A question arises: If a file is mapped twice and both mappings write something, how do we ensure concurrency?

However, as the webpage states, "It's OK if processes that map the same MAP_SHARED file do not share physical pages." We don't need to handle this situation.

So it's straightforward. We check some flags and create a function to write back to the file:

```cpp
      if ((p->vmas[i].flags & MAP_SHARED)) {
        for (int j = start; j < end; j += PGSIZE) {
          // ...
          // Don't forget some checks (e.g., PTE_V? PTE_W?)

          // Write back to the file...
          if((flags & PTE_D)){ 
            uint offset = va - p->vmas[i].start;
            // file_from_pa is my custom function that writes to the file using the passed physical address
            if (file_from_pa(p->vmas[i].f, PTE2PA(*pte), offset, PGSIZE) == -1) {
            }
          }
        }
      }
```

The `file_from_pa` function is implemented as follows, following the pattern of `filewrite`:

```cpp
int file_from_pa(struct file *f, uint64 addr, uint off, int n) {
  // ...
  
  int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
  int i = 0;
  while(i < n){
    int n1 = n - i;
    if(n1 > max)
      n1 = max;

    begin_op();
    ilock(f->ip);
    r = writei(f->ip, 0, addr + i, off + i, n1);
    iunlock(f->ip);
    end_op();

    if(r != n1){
      // error from writei
      break;
    }
    i += r;
  }
  ret = (i == n ? n : -1);
  return ret;
}
```

### 4. Updating the Mapped Address (Corrected Version)

Actually, there's a bug in the above section.

We update `start` when unmapping, but when writing back to the file, the `offset` we compute is incorrect!

```cpp
  uint offset = va - p->vmas[i].start;
  if (file_from_pa(p->vmas[i].f, PTE2PA(*pte), offset, PGSIZE) == -1)
```

For example, if we map from `0x1000` to `0x3000`, then unmap from `0x1000` to `0x2000`, the address `0x3000` corresponds to offset `0x2000` in the mapped file. But because we updated `start`, we compute the offset as `0x1000`.

We should create two variables to record the original mapped range and use them to compute the correct offset:

```cpp
struct vma {
  uint64 start;      // Starting virtual address
  uint64 end;        // Ending virtual address
  uint64 raw_start;  // Original starting address
  uint64 raw_end;    // Original ending address
  //...
}

uint offset = va - p->vmas[i].raw_start;
```

Also, don't forget to check if the mapped area has been completely unmapped. If so, reset some data structures, such as decreasing the file's reference count:

```cpp
      if(p->vmas[i].start == p->vmas[i].end) {
        p->vmas[i].valid = 0;
        fileclose(p->vmas[i].f);
      }
```

### 5. Freeing Memory

The next step is to free the corresponding physical memory. I initially used `uvmunmap` for this, but it failed because we shouldn't free memory that hasn't actually been allocated.

We need to walk the page table ourselves and check if the page was allocated using our custom `PTE_M` flag:

```cpp
      // uvmunmap(p->pagetable, start, len/PGSIZE, 1);
      for (int j = start; j < end; j += PGSIZE) {
        uint64 va = j;
        pte_t *pte = walk(p->pagetable, va, 0);
        uint flags = PTE_FLAGS(*pte);

        // Check if the page was allocated
        if ((flags & PTE_M) == 0) {
          uint64 pa = PTE2PA(*pte);
          kfree((void*)pa);
        }

        // Clear the page table. We should mark PTE_M so it won't be freed by uvmfree.
        // uvmfree also needs to be adjusted to check PTE_M first.
        *pte = 0 | PTE_M;
      }
```

The above is my note about this lab. There are some other requirements such as `uvmfree` or `uvmcopy`, but I think this covers the main implementation details.