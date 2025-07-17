中文 | [English](./../en/lab05_cow.md)

# lab05_copyonwrite

> [!IMPORTANT]  
> 以下文章可能会对您的解决有决定性的帮助。如果您希望自己独立解决，**请勿**查看。如果您觉得有帮助，请考虑 Star 这个仓库！

## 引入新标志

太直接了，没什么值得说的，修改 `uvmcopy` 就行。

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

        // 原来需要分配物理内存，现在取消
        // if((mem = kalloc()) == 0)

        // 如果一个页面可以写入，这个页面应该由 COW 机制处理。
        if (flags & PTE_W) {
            flags = flags & ~PTE_W;
            flags = flags | PTE_COW_W;
        }
        // ...

        // 我们需要将旧的物理地址与这个虚拟地址结合起来。
        if (mappages(new, i, PGSIZE, pa, flags) != 0) {
            // ...
        }
    }
}
```

然后在 `trap.c` 中处理写时复制引发的错误，也很直接，检查标志，然后申请物理内存、从旧内存中复制、更新页表。

```cpp
// trap.c
void usertrap(void) {
    // ...
    // r_scause -> 返回 CSR（控制和状态寄存器），表示 trap 的类型。
    // 存储访问故障是 0xf。
    uint64 scause = r_scause();
    // ...
    else if (scause == 0xf) {
        // 获取虚拟地址
        uint64 oldpage_va = r_stavl();
        oldpage_va = PGROUNDDOWN(oldpage_va);
    
        pagetable_t pagetable = p->pagetable;
        pte_t *pte = walk(pagetable, oldpage_va, 0);

        uint flags = PTE_FLAGS(*pte);

        // 通过检查 cow 标志来检查它是否是写时复制页面。
        if ((flags & PTE_V) && (flags & PTE_COW_W)) {
            // 分配新的物理内存并从旧物理内存复制内容
            uint64 newpage_pa = (uint64)kalloc();
            uint64 oldpage_pa = PTE2PA(*pte);
            memmove((void*)newpage_pa, (void*)oldpage_pa, PGSIZE);

            // 清除标志
            flags = flags & ~PTE_COW_W;
            flags = flags | PTE_W;

            // 更新页表
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

## 释放和分配物理内存

COW 引入了一个新问题：当释放某个页面时，我们需要确定是否应该真正释放物理内存，万一这个页面是写时复制的，此时还被其他进程用呢。

提示已经告诉我们用计数即可。为了简单起见，使用数组，忽略掉这些空间开销。

```cpp
// kalloc.c
struct {
  struct spinlock lock;
  // 这个实现浪费了一些空间（更精确的大小应该是 PHYSTOP - KERNELBASE），但为了简单起见，我们忽略它。
  uint ref_count[PHYSTOP / PGSIZE];
} ref_count;

// kinit() 和 freerange() 需要更改，为了简洁起见，这里没有显示。

void* kalloc() {
    // ...
    acquire(&ref_count.lock);
    ref_count.ref_count[(uint64)r / PGSIZE] = 1;
    release(&ref_count.lock);
    // ...
}
```

然后添加两个函数来修改计数，并在处理写时复制页面时使用，即前一节的内容。

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
// fork → uvmcopy：页面可能在这个函数中被标记为写时复制。
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
    // ...
    // 当页面被标记为写时复制时，增加旧物理地址的引用计数
    kadd_ref_count((void*)pa);
}

// trap.c
// 当尝试在写时复制页面上写入时调用此函数。
void usertrap(void) {
    // ...
    // 在成功分配新内存并从旧物理地址复制数据后，
    // 我们应该递减旧物理页面的引用计数。
    ksub_ref_count((void*)oldpage_pa);
}
```

解决了？不！前面显示的代码片段中有一个错误。在 `usertrap` 中，子进程处理写时复制，此时会释放掉旧内存，我们用 `ksub_ref_count`，只是递减计数而不释放这块内存。**但有可能在子进程执行真正的写时复制前，父进程已经释放了它的地址**，也就是说，现在这个地址只有我们子进程在用了，那我们应该要真正释放掉这片物理内存。

因此，**我们应该调整 `kfree` 来检查是否真正释放地址，并在 `usertrap` 中使用它**。

```cpp
void usertrap(void) {
    // ...
    // 使用 kfree 而不是 ksub_ref_count
    kfree((void*)oldpage_pa);
}

// kalloc.c
void kfree(void *pa) {
    // ...
    acquire(&ref_count.lock);
    ref_count.ref_count[page_index]--;

    // 如果引用计数为 0，真正释放它。
    if (ref_count.ref_count[page_index] == 0) {
        // ...
    }
    release(&ref_count.lock);
}
```

## copyout

最后，如 Hint 所说，修改 `copyout()`，即遇到 COW 页面时使用与 `usertrap` 相同的方案。

```cpp
// copyput：src 是物理地址，将 [src:src+len] 复制到新内存，并将其虚拟地址映射为 dstva
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    // ...
    while(len > 0){
        va0 = PGROUNDDOWN(dstva);

        // 检查 COW（现在不显示）
        if (is_cow) {
            uint64 newpage_pa = (uint64)kalloc();
            uint64 oldpage_pa = PTE2PA(*pte);
            memmove((void*)newpage_pa, (void*)oldpage_pa, PGSIZE);

            // 更新标志
            uint flags = PTE_FLAGS(*pte);
            flags = flags & ~PTE_COW_W;
            flags = flags | PTE_W;

            // 更新页表
            if(mappages(pagetable, va0, PGSIZE, newpage_pa, flags) != 0){
              // ...
            }
            *pte = PA2PTE(newpage_pa) | flags;

            // 与 usertrap 相同
            kfree((void*)oldpage_pa);
            pa0 = newpage_pa;
        }
        // copyout。将 src 内存复制到新分配的内存。
        memmove((void *)(pa0 + (dstva - va0)), src, n);
        // ...
    }
    return 0;
}
```

但是，有一个细微的差别。在 `copyout` 中，为 COW 页面新分配的内存很快就会被最终的 `memmove` 覆盖。因此，并不总是需要将旧物理内存的内容复制到新页面。

更具体地说，我们只需要在不会从 `src` 覆盖整个页面的情况下复制旧内容。这种情况只发生在范围的开始或结束。

```cpp
// copyput：src 是物理地址，将 [src:src+len] 复制到新内存，并将其虚拟地址映射为 dstva
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    // ...
    while(len > 0){
        va0 = PGROUNDDOWN(dstva);
        pa0 = PTE2PA(*pte);

        // n 是这次从 src 复制的大小
        n = PGSIZE - (dstva - va0);
        if(n > len)
            n = len;

        // 检查 COW（现在不显示）
        if (is_cow) {
            uint64 newpage_pa = (uint64)kalloc();
            uint64 oldpage_pa = PTE2PA(*pte);

            // 关键思想：只有在我们不会从 src 覆盖整个页面的情况下才需要复制旧内容。
            // memmove((void*)newpage_pa, (void*)oldpage_pa, PGSIZE);
            if (n != PGSIZE)
                memmove((void*)newpage_pa, (void*)oldpage_pa, PGSIZE);

            // ...
            pa0 = newpage_pa;
        }
        // copyout。将 src 内存复制到新分配的内存。
        memmove((void *)(pa0 + (dstva - va0)), src, n);
        // ...

        // 更新一些东西
        len -= n; 
        src += n; 
        dstva = va0 + PGSIZE; // 不是 dstva += PGSIZE。va0 必须是一个页面的开始。
    }
    return 0;
}
```