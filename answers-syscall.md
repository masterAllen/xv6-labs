1. Looking at the backtrace output, which function called syscall?
Use `backtrace` in gdb, get answer: `usertrap`.

2. What is the value of p->trapframe->a7 and what does that value represent? (Hint: look user/initcode.S, the first user program xv6 starts.)
0x7. Look up `kernel/syscall.h`, it means SYS_exit.

3. What was the previous mode that the CPU was in?
在 RISC-V 架构中，`sstatus` 寄存器的第 [31:0] 位中，第 0 位到第 2 位是 `SPP` (Previous Supervisor Mode)，用于保存处理器在进入中断或异常处理程序前的模式。 0 表示之前的模式是用户模式，为 1 表示之前的模式是监督模式。
在给出的输出中，`sstatus` 的值是 `100010`，所以，处理器在进入当前模式之前是在用户模式下运行的。
From: https://gitlab.eduxiji.net/pku2100013145/expProject266346-203747/-/blob/lab2/answers-syscall.txt

4. Write down the assembly instruction the kernel is panicing at. Which register corresponds to the varialable num?
```
// num = p->trapframe->a7;
num = *(int *) 0;
80001c82:	00002683          	lw	a3,0(zero) # 0 <_entry-0x80000000>
```
Register `a3` is `num`.

5. Why does the kernel crash? Hint: look at figure 3-3 in the text; is address 0 mapped in the kernel address space? Is that confirmed by the value in scause above? (See description of scause in RISC-V privileged instructions)
地址 0 并不映射到内核空间中（从 0x80000000 开始），内核加载中的报错信息里面有 `scause`，这是是 RISC-V 架构中一个 CSR（控制状态寄存器），用于报告异常或中断的原因，通常在 S（Supervisor）模式下使用，`0xd` 表示 加载页错误（Load Page Fault）。
```
scause=0xd sepc=0x80001c82 stval=0x0
```

6. What is the name of the binary that was running when the kernel paniced? What is its process id (pid)?
Use `p p->name` in gdb, get `initcode`. Use `p *p` in gdb, we can get `pid=1`.
```
(gdb)p *p
$3 = {
    lock = {locked = 0, name = 0x800071c8 "proc", cpu = 0x0}, 
    state = RUNNING, chan = 0x0, killed = 0, xstate = 0, pid = 1, parent = 0x0，kstack=274877894656,
    s7 = 4096, pagetable= 0x87f55000, trapframe = 0x87f56000,
    p->
    context = {
        ra = 2147488430, sp = 274877898368, s0 = 274877898416, s1= 214526240, s2= 2147525168. s3= 1
        s4 = 214755040, s5 = 3, s6 = 2147595008, s7= 1, s8 = 2147595304, s9 = 4, s10 = 0, s11 = 0
    }, 
    ofile = {0x0 <repeats 16 times>}, cwd= 0x8018770 <itable+24>,
    name ="initcode\000\000\000\000\000\000\000"
}
```