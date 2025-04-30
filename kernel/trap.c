#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fcntl.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions tn kerneltrap(),
  // since we're now in the kerne/.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();

  // save user program counter.
  p->trapframe->epc = r_sepc();

  if(r_scause() == 8){
    // system call

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } else if(r_scause() == 0xd || r_scause() == 0xf){
    // 0xd: Load Page Fault
    // 0xf: Store Page Fault
    // stval 是一个 CSR（Control and Status Register），当陷入（trap）发生时，它会保存与陷阱相关的额外信息
    // 如果是 访问无效地址（例如页错误），stval 保存那个无效的虚拟地址。
    uint64 va = r_stval();
    va = PGROUNDDOWN(va);
    
    pagetable_t pagetable = p->pagetable;
    pte_t *pte = walk(pagetable, va, 0);

    uint flags = PTE_FLAGS(*pte);
    // printf("\nusertrap(): r_scause: %ld, va: %p, pte: %p\n", r_scause(), (void*)va, (void*)pte);
    // printf("PTE_V = %ld, PTE_M = %ld\n", (flags & PTE_V), (flags & PTE_M));

    // 处理 mmap：页表项存在、Valid、标记了 PTE_M
    if ((flags & PTE_V) && (flags & PTE_M)) {
      // 遍历 vmas
      for (int i = 0; i < MAX_VMA; i++) {
        if (p->vmas[i].valid == 1 && p->vmas[i].start <= va && p->vmas[i].end > va) {
          // 要检测权限
          if (r_scause() == 0xd && !(p->vmas[i].prot & PROT_READ)) {
            continue;
          }
          if (r_scause() == 0xf && !(p->vmas[i].prot & PROT_WRITE)) {
            continue;
          }

          // Try 2
          // // 找到文件的物理地址
          // struct file *f = p->vmas[i].f;
          // void* pa = filepa(f, va - p->vmas[i].start);
          // void* pa_aligned = pa - (va - va_aligned);

          // // 映射虚拟地址和物理地址
          // flags = (flags | PTE_R) & ~PTE_M;
          // printf("usertrap map: pid: %d, va: %p, pa: %p\n", p->pid, (void*)va_aligned, (void*)pa_aligned);

          // if(mappages(pagetable, va_aligned, PGSIZE, (uint64)pa_aligned, flags) != 0){
          //   printf("mappages failed\n");
          //   goto trap_exit;
          // }


          // Try 1
          // 先分配物理内存，然后根据记录的文件，把内容写进去
          char *pa = kalloc();
          // 一开始一定要给写权限，因为等会要写内容，最终的 copyin 里面会判断这个地址是不是可写的...
          if(mappages(pagetable, va, PGSIZE, (uint64)pa, flags | PTE_W) != 0){
            printf("mappages failed\n");
            goto trap_exit;
          }

          // 根据文件指针，把内容写进去
          memset(pa, 0, PGSIZE);
          struct file *f = p->vmas[i].f;
          uint off = va - p->vmas[i].start;
          int n = (va + PGSIZE) > p->vmas[i].end ? p->vmas[i].end - va : PGSIZE;

          // printf("vma id: %d, pid: %d, va: %p, pa: %p, write n: %d\n", i, p->pid, (void*)va, (void*)pa, n);
          // printf("vmas[i].start: %p, vmas[i].end: %p\n", (void*)p->vmas[i].start, (void*)p->vmas[i].end);

          // 这里一开始想用 readi，即前面需要 ilock(f->ip)，发现一直编译不通过，后来发现可以直接用这个函数呀
          // 但是这个函数会改变文件的 offset
          file_to_pa(f, (uint64)pa, off, n);

          // 更新 PTE 权限：取消 Mmap 标识、增加读权限；根据 mmap 的权限决定是否写权限
          flags = (flags | PTE_R) & ~PTE_M;
          flags = (p->vmas[i].prot & PROT_WRITE) ? flags | PTE_W : flags & ~PTE_W;
          update_pte(pagetable, va, flags);

          goto trap_last;
        }
      }
      printf("No VMAs can contain this va!!\n");
    } else {
      printf("Flag is not correct!! PTEV is %ld, PTEM is %ld\n", flags&PTE_V, flags&PTE_M);
    }
    // printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    // printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);

  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
  trap_exit:
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

trap_last:
  if(killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

