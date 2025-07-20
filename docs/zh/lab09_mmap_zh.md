中文 | [English](./../en/lab09_mmap.md)

# lab09_mmap

> [!IMPORTANT]  
> 以下文章可能会对您的解决有决定性的帮助。如果您希望自己独立解决，**请勿**查看。如果您觉得有帮助，请考虑 Star 这个仓库！

## 实现 `mmap`

### 1. 选择正确的数据结构

我们需要解决的第一个问题是：使用什么数据结构来实现 `mmap`？

一开始，可能会想进行正常的虚拟到物理地址映射，然后将区域标记为一个新的标志呗。但是，`mmap` 有一个独特的特征：它是和文件有绑定关系，所以不需要这样处理。

其实关键点是发现原来进程里面已经有打开的文件（`struct file *ofile[NOFILE]`）这样的数据结构了。

就像 Hint 的建议，使用一个映射表就像了，即 VMA(Virtual Memory Area)，将虚拟地址与文件对应起来。我们在进程中为 VMA 分配一个区域，每个 VMA 对应一个 mmap。

```cpp
#define MAX_VMA 10

// 自己创建 vma 这种数据结构用来映射
struct vma {
  uint64 start;      // 起始虚拟地址
  uint64 end;        // 结束虚拟地址
  int prot;          // 保护（读/写）
  int flags;         // MAP_SHARED 或 MAP_PRIVATE
  struct file *f;    // 指向映射文件的指针
  int offset;        // 文件偏移
  int valid;         // 指示此 VMA 是否有效的标志
};

// 每个进程的状态
struct proc {
    // ...
    struct file *ofile[NOFILE];  // 打开的文件 <-- 稍后将使用
    // ...
    struct vma vmas[MAX_VMA];
};
```

### 2. 在 `sys_mmap` 中获取参数

然后就开始实现 `sys_mmap`。首先解决参数。`mmap` 的定义是：`void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)`。

这里就直接给出实现：

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

### 3. 选择虚拟地址

实现 `sys_mmap` 的其余部分很直接。搜索一个空的 VMA 然后在里面保存 mmap 信息。

```cpp
  acquire(&p->lock);
  // 找到一个空的 VMA
  for (int i = 0; i < MAX_VMA; i++) {
    if (p->vmas[i].valid == 0) {
        // ...
    }
  }
```

分配新的虚拟地址区域，一开始不知道怎么选。后来参考 `sys_sbrk` 中使用的方法，发现它就是使用 `sz` 作为新的虚拟区域的起始地址。这个虽然可能会创建空洞（中间的地址被释放后就不会再被分配了），不过为了简单起见，就这样用就可以：

```cpp
      // 遵循 sys_sbrk 的方法
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

### 4. 选择物理地址

然后就是将这些新的虚拟地址映射到正确物理地址。这样读取地址，内核才能到对于地址读取内容呀。所以找到文件的地址，然后进行映射。

所以吗？不！其实我们**没有**文件的物理地址！更准确地说，使用 `open(fname)` 时，并不会将文件内容加载到内存中！**个人感觉这是实现 `mmap` 的一个关键理解！**

是啊，你想想如果映射一个非常大的文件，那要加载可得了！我们应该做的是类似于 COW，即延迟映射。如果访问了一个页面，内核有 trap，此时我们在处理它。

### 5. 设置正确的标志

那就如 COW，在页表中选择适当的标志。一开始我以为应该将 `mmap` 中的 `prot` 设置为标志就行了。例如，如果 `mmap(..., prot=PROT_READ)`，则将标志设置为 `PTE_R`。

但其实还是没理解对。回想 COW，触发 trap 是因为读取 unwritable 或者写入 unreadable。如果我们将页面标记为 `PTE_R`，那这样读取它的时候，其实不会触发 trap。

所以就像 COW 那样，创建一个新标志并用其标记页面。当访问页面时，内核将检查标志并触发 trap。

```cpp
      uint64 va = PGROUNDUP(p->sz);
      uint64 pa = 0;

      uint64 append_sz = len + len%PGSIZE;
      for (int j = 0; j < append_sz; j += PGSIZE) {
        // 设置正确的标志
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

      // 添加文件引用
      filedup(p->vmas[i].f);
```

### 6. 处理 trap：将文件加载到目标物理地址

现在触发 trap 了，我们需要通过实际将文件加载到目标。首先就是申请物理地址，然后映射虚拟地址和物理地址，最后文件加载内容放在物理地址中。

这里先说最后一项，即文件内容放到物理地址中，可以遵循 `file.c` 中 `filewrite` 使用的方法。

**有一个重要的细节：操作后文件的偏移不能变。**

强烈建议在 `file.c` 中创建 `file_to_pa` 函数，即文件加载到物理地址中。除了使代码更清晰外，还能避免在 `trap.c` 中更改头文件。（如果在 `trap.c` 的 `usertrap` 中使用相同的代码，编译器会抛出包含错误。）

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
  // 我们不改变偏移
  r = readi(f->ip, 0, addr, off, n);
  iunlock(f->ip);
}
```

### 7. 其他陷阱操作

上一点已经说了触发 trap 后的操作：首先申请物理地址，然后映射虚拟地址和物理地址，最后文件加载内容放在物理地址中。

最后一步就是上一点的内容。前面两点就仿照 COW 实验的内容就可以了。

一个重要的细节：因为我们调用 `readi` 来加载文件内容，它最终会调用 `copyin`，这个函数会检查 `PTE_W`，所以我们**无论如何将页面标上 `PTE_W`**，然后在 `readi` 后，再去设置最终标志。

**另一个重要细节：延迟映射中的一个关键技巧，只加载触发 trap 的页面。**

```cpp
  } else if(r_scause() == 0xd || r_scause() == 0xf){
    // ...（获取 va、flags...）

    // 检查它是否是 mmap 页面
    if ((flags & PTE_V) && (flags & PTE_M)) {
      for (int i = 0; i < MAX_VMA; i++) {
        if (p->vmas[i].valid == 1 && p->vmas[i].start <= va && p->vmas[i].end > va) {
          // 分配一个新的物理区域
          char *pa = kalloc();

          // 我们必须先标记 PTE_W！
          if(mappages(pagetable, va, PGSIZE, (uint64)pa, flags | PTE_W) != 0){
            // ...
          }

          // 将文件加载到物理区域
          memset(pa, 0, PGSIZE);
          struct file *f = p->vmas[i].f;
          uint off = va - p->vmas[i].start;
          int n = (va + PGSIZE) > p->vmas[i].end ? p->vmas[i].end - va : PGSIZE;
          file_to_pa(f, (uint64)pa, off, n);

          // 设置最终标志
          flags = (flags | PTE_R) & ~PTE_M;
          flags = (p->vmas[i].prot & PROT_WRITE) ? flags | PTE_W : flags & ~PTE_W;
          update_pte(pagetable, va, flags); // 我的自定义函数
        }
      }
    }
```

## 实现 `unmmap`

### 1. 找到相应的 VMA

第一步是找到相应的 VMA。这很直接：

```cpp
  for (int i = 0; i < MAX_VMA; i++) {
    if (p->vmas[i].valid == 1 && p->vmas[i].start <= start && p->vmas[i].end >= end) {
```

### 2. 更新映射地址（初始尝试）

重要的是要理解 `unmmap` 可以取消映射区域的一部分。实验已经确保了不释放中心部分，即不会造成分割。考虑到这个约束，更新很简单：

```cpp
      if (p->vmas[i].start == start) {
        p->vmas[i].start = end;
      } else if (p->vmas[i].end == end) {
        p->vmas[i].end = start;
      }
```

实际上，这个不太对。但先继续，后面会 callback。

### 3. 写回文件

释放一个区域时，我们需要将更改写回文件。然后出现了一个问题：如果一个文件被映射两次，两个映射都写了一些东西，我们如何确保并发性？

但是，正如网页所述，我们不需要处理这种情况：
> "It's OK if processes that map the same MAP_SHARED file do not share physical pages."

所以很直接。我们检查一些 flags 并创建一个函数来写回文件：

```cpp
      if ((p->vmas[i].flags & MAP_SHARED)) {
        for (int j = start; j < end; j += PGSIZE) {
          // ...
          // 不要忘记一些检查（例如，PTE_V？PTE_W？）

          // 写回文件...
          if((flags & PTE_D)){ 
            uint offset = va - p->vmas[i].start;
            // file_from_pa 是我的自定义函数，使用传递的物理地址写入文件
            if (file_from_pa(p->vmas[i].f, PTE2PA(*pte), offset, PGSIZE) == -1) {
            }
          }
        }
      }
```

`file_from_pa` 函数实现如下，遵循 `filewrite` 的模式：

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
      // writei 的错误
      break;
    }
    i += r;
  }
  ret = (i == n ? n : -1);
  return ret;
}
```

### 4. 更新映射地址（修正版本）

现在揭晓一个错误，我们在取消映射时更新 `start`，但在写回文件时，我们计算的 `offset` 是不正确的！

```cpp
  uint offset = va - p->vmas[i].start;
  if (file_from_pa(p->vmas[i].f, PTE2PA(*pte), offset, PGSIZE) == -1)
```

例如，从 `0x1000` 映射到 `0x3000`，然后从 `0x1000` 释放到 `0x2000`，地址 `0x3000` 对应于映射文件中的偏移 `0x2000`。但是因为我们更新了 `start`，我们计算的偏移是 `0x1000`。

所以应该创建两个变量来记录原始映射范围，并使用它们来计算正确的偏移：

```cpp
struct vma {
  uint64 start;      // 起始虚拟地址
  uint64 end;        // 结束虚拟地址
  uint64 raw_start;  // 原始起始地址
  uint64 raw_end;    // 原始结束地址
  //...
}

uint offset = va - p->vmas[i].raw_start;
```

另外，不要忘记检查映射区域是否已完全被释放。如果是，重置一些数据结构，例如减少文件的引用计数：

```cpp
      if(p->vmas[i].start == p->vmas[i].end) {
        p->vmas[i].valid = 0;
        fileclose(p->vmas[i].f);
      }
```

### 5. 释放内存

下一步是释放相应的物理内存。我最初使用 `uvmunmap`，但失败了，因为有的地址没有被真正分配，此时这个函数会失败。

所以自己遍历页表并检查页面是否使用了自定义 `PTE_M` 标志分配：

```cpp
      // uvmunmap(p->pagetable, start, len/PGSIZE, 1);
      for (int j = start; j < end; j += PGSIZE) {
        uint64 va = j;
        pte_t *pte = walk(p->pagetable, va, 0);
        uint flags = PTE_FLAGS(*pte);

        // 检查页面是否已分配
        if ((flags & PTE_M) == 0) {
          uint64 pa = PTE2PA(*pte);
          kfree((void*)pa);
        }

        // 清除页表。我们应该标记 PTE_M 这样它就不会被 uvmfree 释放。
        // uvmfree 也需要调整以首先检查 PTE_M。
        *pte = 0 | PTE_M;
      }
```

以上就是我关于这个实验的笔记。还有一些其他要求，如 `uvmfree` 或 `uvmcopy`，但也都大差不差了额，我认为上面涵盖了主要的实现细节。 