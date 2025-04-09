#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"


int
main(int argc, char *argv[])
{
  if(argc != 2){
    printf("Usage: secret the-secret\n");
    exit(1);
  }
  char *end = sbrk(PGSIZE*32);
  printf("PGSIZE: %d\n", PGSIZE);
  printf("end: %p\n", end);
  end = end + 9 * PGSIZE;
  printf("end: %p\n", end);
  strcpy(end, "my very very very secret pw is:   ");
  printf("end: %s\n", end);
  strcpy(end+32, argv[1]);

  for (int i = 0; i < 4; i++) {
    printf("%x ", end[32+i]);
  }
  printf("\n");
  exit(0);
}

