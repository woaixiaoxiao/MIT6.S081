#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define SIZE 1

int main() {
  // p1 父写子读
  int p1[2];
  pipe(p1);
  // p2 子写父读
  int p2[2];
  pipe(p2);
  // 子进程
  if (fork() == 0) {
    // 子进程阻塞p1的写端，p2的读端
    close(p1[1]);
    close(p2[0]);
    char buf[SIZE + 1];
    read(p1[0], buf, SIZE);
    printf("%d: received ping\n", getpid());
    write(p2[1], buf, SIZE);
    exit(0);
  } else {
    // 父进程
    close(p1[0]);
    close(p2[1]);
    char buf[SIZE] = "1";
    write(p1[1], buf, SIZE);
    read(p2[0], buf, SIZE);
    printf("%d: received pong\n", getpid());
  }
  exit(0);
}
