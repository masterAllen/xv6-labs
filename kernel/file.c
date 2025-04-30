//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}


void*
filepa(struct file *f, uint off)
{
  return pai(f->ip, off);
}

// 从文件中搬运数据到物理地址，不改变偏移量
int
file_to_pa(struct file *f, uint64 addr, uint off, int n)
{
  int r = 0;

  // printf("file_to_pa: f->ip->size: %d, addr: %p, off: %d, n: %d\n", f->ip->size, (void*)addr, off, n);
  // 有可能大于文件大小，甚至偏移量都有可能大于文件大小
  if(off > f->ip->size)
    return 0;
  if(off + n > f->ip->size)
    n = f->ip->size - off;

  if(f->readable == 0)
    return -1;

  if(f->type != FD_INODE)
    return -1;

  ilock(f->ip);
  r = readi(f->ip, 0, addr, off, n);
  iunlock(f->ip);
  return r;
}

// 从物理地址读取数据到文件，不改变偏移量
int
file_from_pa(struct file *f, uint64 addr, uint off, int n)
{
  // 参考 filewrite
  int r, ret = 0;

  // printf("file_from_pa: f->ip->size: %d, addr: %p, off: %d, n: %d\n", f->ip->size, (void*)addr, off, n);

  if(f->writable == 0)
    return -1;

  if(f->type != FD_INODE)
    return -1;
  
  if(off+n > f->ip->size)
    n = f->ip->size - off;

  int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
  int i = 0;
  // 这里很关键！必须分块写入，否则 log 写入有问题，明明知道参考 filewrite，该函数的确是分块写入，但我一开始没好好照着写，导致一直出错
  while(i < n){
    int n1 = n - i;
    if(n1 > max)
      n1 = max;

    // printf("file_from_pa: n1=%d, i=%d, off=%d\n", n1, i, off);
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