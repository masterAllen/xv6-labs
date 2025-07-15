中文 | [English](./../en/lab04_trap.md)

# lab04_Trap

> [!IMPORTANT]  
> 以下文章可能会对您的解决有决定性的帮助。如果您希望自己独立解决，**请勿**查看。如果您觉得有帮助，请考虑 Star 这个仓库！

## add backtrace syscall

太简单了。记住检查 RISC-V 的栈结构——和 x86 有点不同。

```cpp
void backtrace(void)
{
  printf("backtrace:\n");

  uint64 cur_fp = r_fp();

  uint64 top = PGROUNDUP(cur_fp);
  uint64 bot = PGROUNDDOWN(cur_fp);

  while(cur_fp < top && cur_fp > bot){
    uint64 return_addr = *(uint64*)(cur_fp - 1 * sizeof(uint64));
    printf("%p\n", (void*)(return_addr));
    cur_fp = *(uint64*)(cur_fp - 2 * sizeof(uint64));
  }
}
```

有一个小知识：为什么我们可以访问当前帧指针？因为它存储在寄存器 s0 中。Hint 已经告诉我们了：只需要添加一小段代码来告诉编译器我们可以使用函数读取这个寄存器（s0）。

```cpp
// 我们可以使用这个函数来读取 s0。因为我们通过 asm 读取它，所以我们应该使用以下格式来告诉编译器我们可以在函数中使用 asm。
// https://ttzytt.com/2022/07/xv6_lab4_record/index.html

#ifndef __ASSEMBLER__
static inline uint64 r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}
```

## add sigalarm syscall

如果你理解这个系统调用背后的原理，实现起来就很直接了。我认为这个题目的最最关键问题是：

**为什么在 `sigalarm` 之后需要调用 `sigreturn`。**

当时思考了半天，理论上，明明可以在 `sigalarm` 被触发时将当前上下文保存到栈上并分配新的栈空间。在处理器执行完成后（即函数返回后），返回地址会像往常一样从栈中恢复，这样就从触发前的位置继续执行。

实际上，这种方法是可行的，类似于某些系统的处理方式（比如 Linux 中的 `signal()`）。但是，xv6 没有采用这种设计，因为种种原因吧，可能是因为这方法确实会有一些缺点，也可能对学生来说更难实现。**总而言之，这只是 xv6 作者做出的设计决定。**

xv6 是固定将要触发的代码起始地址保存到一个叫 trapframe 的结构上，触发后执行它，并且把触发前的地址替换到 trapframe 上，触发结束就能回去了。

现在回到主线，如何实现 `sigalarm`。很容易想到，我们需要一个计时器。当它达到零时，就执行事先保存要执行的代码段。

```cpp
// proc.c，alarm 的一些数据结构
  // 分配一个 trapframe 页面。
  if((p->alarm_saved_trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  p->alarm_handler = 0;
  p->alarm_ticks_left = 0;
  p->alarm_ticks_total = 0;
```

时钟触发的函数位于中断处理代码中，主要在 `trap.c` 中。在 xv6 中，`devintr()` 可以用来确定外部中断的类型，比如定时器、磁盘或 UART 中断。返回值为 2 为定时器中断。

```cpp
// trap.c
void usertrap(void) {
  // ...
  int which_dev = devintr();
  if(which_dev == 2) {
    if(p->alarm_ticks_total > 0) {
      p->alarm_ticks_left--;

      // 计时器是否结束
      if(p->alarm_ticks_left == 0) {
        // 周期执行，所以计时器重新开始
        p->alarm_ticks_left = p->alarm_ticks_total;

        // 保存 trapframe
        *(p->alarm_saved_trapframe) = *(p->trapframe);

        p->trapframe->epc = p->alarm_handler;
        goto make_usertrap;
      }
    }

    // 没进入上面最后的条件，说明之前的程序正常走，即内核 yield，跳转回用户
    yield();
  }
}
```

当调用 `sigreturn` 时，恢复保存的 trapframe，所以我们可以继续 `sigalarm` 被调用时的代码。

```cpp
uint64 sys_sigreturn(void) {
  struct proc* p = myproc();
  *(p->trapframe) = *(p->alarm_saved_trapframe);
  p->alarm_canrun = 1;
  return p->trapframe->a0;
}
```

有一个场景需要特别注意：如果 `sigalarm` 正在执行时再次被触发怎么办？所以我们需要引入一个标志。当 sigalarm 正处理时，倒计时应该暂停。

```cpp
// trap.c
void usertrap(void) {
  // ...
    // 一个新的标志，只有当它被设置时，倒计时才能进行。
    if(p->alarm_canrun == 1 && p->alarm_ticks_total > 0) {
      // ...
      if(p->alarm_ticks_left == 0) {
        // ...
        // 运行 alarm 处理器的地方，设置标志为 false 以避免在处理时再次执行。
        p->alarm_canrun = 0;
      }
    }
}

uint64 sys_sigreturn(void) {
  // ...
  // 重置 alarm_canrun...
  p->alarm_canrun = 1;
}
``` 