#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"

#define O_RDONLY 0
#define MAX_PATH 512

void search(char* dir, char* filename) {
  // 获取当前目录的文件描述符
  int fd = open(dir, O_RDONLY);
  // 不断读取目录项，进行判断
  // 这个p是用来拼接这个目录中的子项的路径的
  char buf[MAX_PATH];
  strcpy(buf, dir);
  char* p = buf + strlen(buf);
  *p = '/';
  p++;
  // 正式读取每一行，并根据目录还是文件进行讨论
  struct dirent de;
  struct stat st;
  while (read(fd, &de, sizeof(de)) == sizeof(de)) {
    // 无效
    if (de.inum == 0) {
      continue;
    }
    // 拼接出目录的这一项的path
    memmove(p, de.name, DIRSIZ);
    p[DIRSIZ] = 0;
    // 取出这一项的信息
    stat(buf, &st);
    // 如果是文件
    if (st.type == T_FILE) {
      // 文件名相同，打印path
      if (strcmp(de.name, filename) == 0) {
        printf("%s\n", buf);
      }
    } else if (st.type == T_DIR) {
      // 这一项是目录，只要不是 . 或者 .. 那就递归进去
      if (strcmp(de.name, ".") != 0 && strcmp(de.name, "..") != 0) {
        search(buf, filename);
      }
    }
  }
  // 记得关闭文件描述符
  close(fd);
}

int main(int argc, char** argv) {
  if (argc != 3) {
    printf("usage: find dir filename\n");
    exit(1);
  }
  char* dir = argv[1];
  char* filename = argv[2];
  search(dir, filename);
  exit(0);
}
