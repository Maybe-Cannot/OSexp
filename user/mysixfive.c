/*
功能：将文件sixfives.txt中被5或6整除的数字打印出来
提示：
使用read系统调用读取文件，进一步判断两个分隔符（" -\r\t\n./,"）间的是否为数字，
将符合要求的数字打印；
使用strchr()函数检查字符是否为分隔符。

cat > sixfives.txt
1/30/3232/434/545/53464/243235.432.47657.5.6
crtl + D

mysixfive

*/
typedef unsigned int uint;
#include "user.h"

#define stuff 128

void endl(){
  int no = write(1, "\n", 1);
  if(!no)
    return;
  return;
}

int main(){
    int fd = open("sixfives.txt", 0); // 只读方式打开
    if (fd < 0) {
        return 1;
    }
    char buf[stuff];
    int rn,wn;
    rn = read(fd, buf, sizeof(buf));
    wn = write(1,buf,rn);
    endl();
    if(rn != wn){
      return 1;
    }
    int i, count;
    count = 0;
    char temp[stuff];
    for(i=0;i<=rn;i++){
      if(i == (rn) || strchr(" -\\r\\t\\n./,", buf[i])){
        int j, sum;
        for(sum = 0, j = 0; j<count;j++){
          sum *= 10;
          sum += (temp[j] - 48);
        };
        if(!(sum%6)||!(sum%5)){
          int s = write(1,temp,count);
          endl();
          if(!s){
            return 1;
          }
        }
        memset(temp, 0, stuff);
        count = 0;
      }
      else{
        temp[count]=buf[i];
        count++;
      }
    }

    //        if (strchr(" -\r\t\n./,", ch)) {
              
    close(fd);
    return 0;
}