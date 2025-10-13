#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "user.h"
#include "../kernel/syscall.h"

int
main(int argc, char *argv[])
{
  if(argc < 2){
    fprintf(2, "usage: trace mask command [args]\n");
    exit(1);
  }

  int mask = atoi(argv[1]);
  trace(mask);  // 开启系统调用跟踪

  exec(argv[2], &argv[2]);
  exit(0);
}
