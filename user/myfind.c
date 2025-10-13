/*
功能：编写一个简化版的 UNIX find 程序：在目录树中查找所有名称与指定名称匹配的文件 
提示： 
    利用open、read、fstat 系统调用实现本功能；
    参考 user/ls.c 的代码，学习如何读取目录内容；
    使用递归实现 find 程序，使其能够进入子目录（遍历目录树）；需使用 strcmp() 函数进行字符串比较
mysixfive

myfind . sixfives.txt
myfind . sixfives.txt -exec mysixfive
*/

#include "../kernel/types.h"
#include "user.h"
#include "../kernel/stat.h"
#include "../kernel/fs.h"   // for struct dirent and DIRSIZ
#define Max_buf 1024

char *
strncpy(char *dest, const char *src, int n) {
    int i = 0;
    while (i < n && src[i]) {
        dest[i] = src[i];
        i++;
    }
    while (i < n) {
        dest[i] = 0;
        i++;
    }
    return dest;
}


//e_cmd: 实现exec操作的函数
void 
e_cmd(char *cmd, char *file_path){
    // child: exec cmd with file_path as single argument
    char *argv[3];
    argv[0] = cmd;
    argv[1] = file_path;
    argv[2] = 0;
    exec(cmd, argv);
    fprintf(2, "find: exec %s failed\n", cmd);
    exit(1);
}

void
findyou(char* sp, char* fn, int isexec, char* cmd){
    char buf[Max_buf];//用来存放文件；
    int fd;
    //struct stat：存放文件状态（通过 fstat 填充），包括 type (T_FILE/T_DIR)、大小等。
    //struct dirent：目录项（每个条目表示一个文件/子目录），包含 inum（inode号）、
    //                  name（文件名，长度固定 DIRSIZ）。
    struct stat st;       // 存储 path 对应 inode 的元信息（类型、大小等）
    struct dirent de;     // 目录项结构，用于逐条读取目录内容
    char *p;              // 指针，用来指向 buf 中路径末尾，方便拼接子路径
    //打开目录项
    if ((fd = open(sp, 0)) < 0) {
        fprintf(2, "find: cannot open %s\n", sp);
        return;
    }
    // 获取文件/目录的元信息
    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: fstat %s failed\n", sp);
        close(fd);
        return;
    }
    // 如果是文件：取出它的 basename
    if (st.type == T_FILE) {
        char *base = sp;
        for (p = sp; *p; p++)               //取出文件最基本的名字
            if (*p == '/')
                base = p+1;
        if (strcmp(base, fn) == 0) {        // 如果文件名与目标匹配
            if (isexec) {                   // -exec 模式：执行命令
            if (fork() == 0) {
                close(fd);
                e_cmd(cmd, sp);
            } else {
                wait(0);
            }
            } else {                        // 普通模式：打印路径
                printf("%s\n", sp);
            }
        }
        close(fd);
        return;
    } else if (st.type != T_DIR) {          // 既不是文件也不是目录（可能是设备），直接跳过
        close(fd);
        return;
    }                                       //接下来一定是目录
    // 如果是目录：复制路径到 buf，并把 p 指向末尾
    strncpy(buf, sp, sizeof(buf));
    buf[sizeof(buf)-1] = 0;
    p = buf + strlen(buf);

    // 如果末尾不是 '/'，补一个 '/'，确保路径正确
    if (p != buf && *(p-1) != '/') {
        *p++ = '/';
    }
    // 遍历目录条目
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0)
            continue; // 空条目跳过

        // 取出文件名（注意 DIRSIZ 固定长度，需要手动加 '\0'）
        char name[DIRSIZ+1];
        memmove(name, de.name, DIRSIZ);
        name[DIRSIZ] = 0;

        // 跳过 "." 和 ".."
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        // 检查拼接后路径是否过长
        int len = strlen(name);
        if ((p - buf) + len + 1 >= sizeof(buf)) {
            fprintf(2, "find: path too long: %s%s\n", buf, name);
            continue;
        }

        // 拼接完整路径
        memmove(p, name, len);
        p[len] = 0;
        // 打开子路径，获取类型
        int fd2;
        struct stat st2;
        if ((fd2 = open(buf, 0)) < 0) {
            fprintf(2, "find: cannot open %s\n", buf);
            p[0] = 0; // 恢复 buf 状态
            continue;
        }
        if (fstat(fd2, &st2) < 0) {
            fprintf(2, "find: fstat %s failed\n", buf);
            close(fd2);
            p[0] = 0;
            continue;
        }
        close(fd2);
        if (st2.type == T_DIR) {
            // 如果是目录，递归调用
            findyou(buf, fn, isexec, cmd);
        } else if (st2.type == T_FILE) {
            // 如果是文件，比较文件名
            if (strcmp(name, fn) == 0) {
                if (isexec) {
                    if (fork() == 0) {
                        e_cmd(cmd, buf);
                        exit(1);
                    } else {
                        wait(0);
                    }
                } else {
                    printf("%s\n", buf);
                }
            }
        }
        // 恢复路径（移除刚拼接的子项）
        p[0] = 0;
    }

}


int
main(int argc,char* argv[]){
    if (argc != 3 && argc != 5) {
        //这个地方是传入的不正确，不按规则来传
        fprintf(2, "Usage: find <start-path> <name> [-exec <cmd-path>]\n");
        exit(1);
    }
    char* start_path = argv[1];
    char* file_name = argv[2];

    int is_exec = 0;
    char* my_cmd = argv[0];
    if(argc == 5){
        if(strcmp("-exec", argv[3]) != 0){
            fprintf(2, "Usage: find <start-path> <name> [-exec <cmd-path>]\n");
            exit(1);
        }
        is_exec = 1;
        my_cmd = argv[4];
    }

    findyou(start_path, file_name, is_exec, my_cmd);

}