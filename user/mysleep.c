/*
日期：20250922
功能: 调用sleep/pause系统调用实现暂停功能，暂停时间ticks通过命令行传入；
原理：
    xv6 内核通过时钟中断维护 ticks 计数，
    sleep/pause系统调用会将进程置于阻塞状态，直到指定 ticks 数耗尽后由内核唤醒。
关键代码位置说明：
    kernel/sysproc.c（内核实现）
    user/user.h（用户态声明）
    user/usys.S（汇编跳转）
*/

#include <unistd.h>
#define maxtime 32

int main(){
    char inputtime[maxtime];
    ssize_t income;
    if((income = read(STDIN_FILENO, inputtime, maxtime - 1)) < 0)
        return 1;
    
    // 确保字符串以null结尾
    inputtime[income] = '\0';
    
    // 将字符串转换为整数
    int ticks = 0;
    for(int i = 0; i < income && inputtime[i] >= '0' && inputtime[i] <= '9'; i++) {
        ticks = ticks * 10 + (inputtime[i] - '0');
    }
    
    // 调用sleep系统调用（需要在xv6环境中实现）
    sleep(ticks);
    return 0;
}