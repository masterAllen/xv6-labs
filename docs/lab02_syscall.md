
> [!IMPORTANT]  
> The following hint may contain a decisive solution. If you wish to solve the problem by yourself, **DO NOT** view it. If you find it helpful, consider staring this repository!

## add trace syscall

Too easy, nothing worth to record.
`ecall`: Performs some operations, then jumps to `trap.c`, where `syscall` is invoked."


## attack xv6

The first few bytes in `my secret secret ..` are modified after `secret` exits. Why? Use GDB to find out. Here's how:

* You cannot set a watchpoint on the target address directly in `secret.c`, because at that time you're still in user mode.
* Define a syscall (we can temporarily modify the `trace` syscall from the prev problem). Change `secret.c`, pass the address of interest to this syscall.
* Set a breakpoint at the beginning of the syscall. Now, we are in kernel mode and can access the address passed from user mode.
* However, we still **cannot** set a watchpoint on this address yet. It’s a virtual address in the user process's space (e.g., `0xd000`).
* To proceed, retrieve the **physical address** inside the syscall. How? Ask the LLM and you'll get the correct answer:

  ```c
  uint64 pa = walkaddr(myproc()->pagetable, va);
  ```
* Now you can set a watchpoint on the **physical** address (that is to say, `pa`).
* You'll discover that the content is being changed because the first few bytes are used to store the pointer to the **next** page in the free memory list.

Here’s the relevant part of the code:

```c
// freeproc --> ... --> kfree
// pa is the address of each page
void kfree(void *pa) {
  // ...

  struct run *r;
  r = (struct run*)pa;
  // modifies the first few bytes...
  r->next = kmem.freelist;
  kmem.freelist = r;
  // ...
}
```