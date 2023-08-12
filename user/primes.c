#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define INT_SIZE 4
#define RD 0
#define WR 1
#define NULL 0

void recursion(int p[]) {
  // 首先关闭写端
  close(p[WR]);
  // 读取base数字
  int base;
  int temp = read(p[RD], &base, INT_SIZE);
  // 压根没有输入到这个进程
  if (temp == 0) {
    return;
  }
  printf("prime %d\n", base);
  // 创建管道，并和子进程联动
  int p2[2];
  pipe(p2);
  if (fork() == 0) {
    recursion(p2);
    exit(0);
  }
  // 关闭读端
  close(p2[RD]);
  // 开始不断接受父进程的输入，判断之后传递给子进程
  int rec, res;
  while (1) {
    res = read(p[RD], &rec, INT_SIZE);
    // 父进程结束写了
    if (res == 0) {
      break;
    }
    // 如果这个数字不是base的倍数，那么写给子进程
    if (rec % base != 0) {
      write(p2[WR], &rec, INT_SIZE);
    }
  }
  // 写完了，关闭自己的写端
  close(p2[WR]);
  // 关闭父进程给的管道的读端
  close(p[RD]);
  wait(NULL);
}

int main() {

  int p[2];
  pipe(p);
  // 子进程
  if (fork() == 0) {
    recursion(p);
    exit(0);
  }
  // 父进程
  close(p[RD]);
  for (int i = 2;i <= 35;i++) {
    write(p[WR], &i, INT_SIZE);
  }
  close(p[WR]);
  // 等待子进程结束
  wait(NULL);

  exit(0);
}
