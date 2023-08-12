#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

#define stdin 0
#define NULL 0
#define MAX_SIZE 1024
#define CHAR_SIZE 1

char* strdup(char* p) {
  char* temp = (char*)malloc(strlen(p) + 1);
  memcpy(temp, p, strlen(p));
  return temp;
}

int main(int argc, char* argv[]) {
  // 初始化我们要传递给execute函数的参数
  char* args[MAXARG + 2];
  for (int i = 1;i < argc;i++) {
    args[i - 1] = argv[i];
  }
  int arg_count = argc - 1;
  // 从标准输入一行一行地读
  char input_line[MAX_SIZE];
  while (1) {
    // 最外层的这个while循环，每循环一次代表要execute一次
    // 因此arg_count和p都应该重置，其中p用来操作input_line
    arg_count = argc - 1;
    char* p = input_line;
    // 用read从标准输入端读
    int res;
    while ((res = read(stdin, p, CHAR_SIZE)) > 0) {
      // 如果读入的既不是空格也不是换行，那就让p++，继续读入这个参数
      if (*p != ' ' && *p != '\n') {
        p++;
        // 读入的是空格，说明第一个参数已经完成了
        // 那么我们可以将这个参数写入args，并且修改p指针
      } else if (*p == ' ') {
        *p = '\0';
        args[arg_count] = strdup(input_line);
        arg_count++;
        p = input_line;
        // 读入的是换行，说明已经完全读完了
        // 存当前的这个参数到args，并且为args增加一个结尾标志null
        // 然后fork，exec
      } else if (*p == '\n') {
        *p = '\0';
        args[arg_count] = strdup(input_line);
        arg_count++;
        args[arg_count] = NULL;
        // 子进程，去exec
        if (fork() == 0) {
          exec(args[0], args);
          exit(0);
        }
        // 父进程，现在应该去读新的一行了，break
        break;
      }
    }
    // 读完了
    if (res <= 0) {
      break;
    }
  }
  // 等待所有子进程结束
  while (wait(0) != -1) {
  }
  exit(0);
}
