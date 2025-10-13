/*
功能：
    通过一对管道（每个方向一个管道）在父子进程间 “往返传递” 一个字节。
提示：
    通过 fork 创建子进程，形成父子进程关系，
    使用 pipe 创建半双工通信信道，通过两个管道分别处理不同方向的数据流。
*/

#include <unistd.h>

int main(){
    int p1[2], p2[2];
    int pret1 = pipe(p1);
    int pret2 = pipe(p2);
    if(pret1 || pret2)
        return 1;
    if(fork() != 0){
        // father
        close(p1[0]);
        close(p2[1]);
        char package = 'h';
        ssize_t w1 = write(p1[1],&package,1);
        char getson;
        ssize_t r1 = read(p2[0], &getson, 1);
        if(getson != package)
            return 1;
        else {
            ssize_t w2 = write(1, "pingpong success\n", 17);
            if(!w2)
                return 1;
        }
        close(p1[1]);
        close(p2[0]);
        if(!w1&&!r1)
            return 1;
    }
    else{
        // son
        close(p1[1]);
        close(p2[0]);
        char getfather;
        ssize_t r2 = read(p1[0], &getfather, 1);
        ssize_t w3 = write(p2[1], &getfather, 1);
        close(p1[0]);
        close(p2[1]);
        if(!w3&&!r2)
            return 1;
    }
    return 0;
}