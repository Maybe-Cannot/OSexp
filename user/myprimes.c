#include <unistd.h>
#include <stdlib.h>
#define MAX_PRIME 40 // 上限是40

void generate_numbers(int write_fd) {
    // 向管道中写入2-40的数字流
    for (int i = 2; i <= MAX_PRIME; i++) {
        ssize_t w = write(write_fd, &i, sizeof(i));
    if (w != sizeof(i)) return;
    }
    int c = close(write_fd);
    if (c < 0) return;
}

void filter_process(int read_fd) {
    int prime;
    int num;

    // 读取第一个数字，它一定是素数
    ssize_t r = read(read_fd, &prime, sizeof(prime));
    if (r != sizeof(prime)) return;
    char buf[32];
    char *b = buf;
    int tmp = prime;
    if (tmp == 0) *b++ = '0';
    else {
        int digits[10], cnt = 0;
        while (tmp) {
            digits[cnt++] = tmp % 10;
            tmp /= 10;
        }
        for (int i = cnt - 1; i >= 0; i--)
            *b++ = digits[i] + '0';
    }
    *b++ = '\n';
    ssize_t w = write(1, buf, b - buf);
    if (w != (b - buf)) return;

    int p[2];
    int pret = pipe(p);
    if (pret < 0) return;

    int pid = fork();
    if (pid == 0) {
        // 子进程：成为下一个过滤器
        int c1 = close(p[1]);
        if (c1 < 0) return;
        filter_process(p[0]); // 递归创建下一个过滤进程
        return;
    } else {
        // 父进程：继续过滤当前素数的倍数
        int c2 = close(p[0]);
        if (c2 < 0) return;
        ssize_t rnum;
        while ((rnum = read(read_fd, &num, sizeof(num))) == sizeof(num)) {
            if (num % prime != 0) {
                ssize_t w2 = write(p[1], &num, sizeof(num));
                if (w2 != sizeof(num)) return;
            }
        }
        int c3 = close(read_fd);
        int c4 = close(p[1]);
        if (c3 < 0 || c4 < 0) return;
    }
    return;
}

int main(int argc, char *argv[]) {
    // 创建管道
    int p[2];
    int pret = pipe(p);
    if (pret < 0) return 1;
    int pid = fork();
    if (pid == 0) {
        int c1 = close(p[1]);
        if (c1 < 0) return 1;
        filter_process(p[0]); // 开始过滤
        return 0;
    } else {
        int c2 = close(p[0]);
        if (c2 < 0) return 1;
        generate_numbers(p[1]); // 生成数字序列
        int c3 = close(p[1]);
        if (c3 < 0) return 1;
    }
    return 0;
}