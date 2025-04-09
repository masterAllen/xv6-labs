#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
  // your code here.  you should write the secret to fd 2 using write
  // (e.g., write(2, secret, 8)

  for(int i = 0; i < 3000; i++) {
    char *end = sbrk(1 * PGSIZE);

    int ok_flag = 0;
    for (int k = 0; k < 32-5; k++) {
      if (end[k] && end[k+1] && end[k+2] && end[k+3] && end[k+4]) { 
        ok_flag = 1;
        printf("end[%d]: %s\n", k, end+k);
        k += 5;
      }
    }
    if (ok_flag) {
      printf("end[%d]: %p\n", i, end);
    }
  }

  // char *end = sbrk(PGSIZE*32);
  // printf("PGSIZE: %d\n", PGSIZE);
  // printf("end: %p\n", end);
  // end = end + 9 * PGSIZE;
  // printf("end: %p\n", end);

  // printf("end: %s\n", end);
  // for (int i = 0; i < 4; i++) {
  //   printf("%x ", end[32+i]);
  // }
  // printf("\n");
  exit(0);
}


