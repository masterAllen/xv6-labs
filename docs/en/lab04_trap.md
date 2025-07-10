[中文](./../zh/lab04_trap_zh.md) | English

# lab04_Trap

> [!IMPORTANT]  
> The following hint may contain a decisive solution. If you wish to solve the problem by yourself, **DO NOT** view it. If you find it helpful, consider staring this repository!

## add backtrace syscall

Too easy. Remember to check the stack structure of RISC-V -- it is a bit different from x86.

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

The reason we can access the current frame pointer is that it's stored in a register. The hint already points this out -- we just need to add a small snippet of code to tell the compiler that we can read this register (s0) using a function.

```cpp
// We can use this function to read s0. Because we read it by asm, so we should use the following format to tell the compiler that we can use asm in a function.
// https://ttzytt.com/2022/07/xv6_lab4_record/index.html

#ifndef __ASSEMBLER__
static inline uint64 r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}
```

## add alarm syscall

If you understand the principle behind this system call, implementing it is straightforward. I think a key question is: 

**why a call to `sigreturn` is necessary after `sigalarm`.**

In theory, we could save the current context onto the stack and allocate a new stack space when `sigalarm` is triggered. After the handler finishes executing (i.e., after the function returns), the return address would be restored from the stack as usual, allowing execution to resume at the point where the signal was triggered.

This approach is feasible and resembles how signal handling works in some systems (e.g., `signal()` in Linux). However, xv6 dont adopt this desing -- perhaps because it has some drawbacks, or because it would be more diffcult for students to implement. Anyway, it is simply a design decision made by the authors of xv6.

So let’s get back to implementing the alarm in xv6. It is straightforward, we need a countdown that decreases with each clock tick. When it reaches zero, the saved handler is executed.

```cpp
// proc.c, Some data structure for alarm
  // Allocate a trapframe page.
  if((p->alarm_saved_trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  p->alarm_handler = 0;
  p->alarm_ticks_left = 0;
  p->alarm_ticks_total = 0;
```

The clock-triggered function resides in the interrupt handling code, specifically in `trap.c`. In xv6, the `devintr()` can be used to determine the type of external interrupt -- such as a timer, disk, or UART interrupt. A return value of 2 indicates that a timer interrupt has occurred.

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

        // save trapframe
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

When `sigreturn` is called, it will restore the saved trapframe, so we can continue the code when `sigalarm` is called.

```cpp
uint64 sys_sigreturn(void) {
  struct proc* p = myproc();
  *(p->trapframe) = *(p->alarm_saved_trapframe);
  p->alarm_canrun = 1;
  return p->trapframe->a0;
}
```

There is one scenario that requires special attention: what if `sigalarm` is triggered again while its handler is still executing? To handle this, we need to introduce a flag. While the sigalarm handler is running, the countdown should be paused.

```cpp
// trap.c
void usertrap(void) {
  // ...
    // a new flag, only if it is set, the countdown can be done.
    if(p->alarm_canrun == 1 && p->alarm_ticks_total > 0) {
      // ...
      if(p->alarm_ticks_left == 0) {
        // ...
        // where run the alarm handler, set the flag false to avoid executing again when handling.
        p->alarm_canrun = 0;
      }
    }
}

uint64 sys_sigreturn(void) {
  // ...
  // reset the alarm_canrun...
  p->alarm_canrun = 1;
}
```