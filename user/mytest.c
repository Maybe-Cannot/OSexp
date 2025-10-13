//该头文件包含了POSIX操作系统API的声明
#include <unistd.h>

#define BUFFER_SIZE 1024

int main() {
    //构造读取容器
    char buffer[BUFFER_SIZE];
    //定义读取和写入的字节数
    ssize_t bytes_read, bytes_written;
    
    // 循环读取标准输入并写入标准输出
    while ((bytes_read = read(STDIN_FILENO, buffer, BUFFER_SIZE)) > 0) {
        //这个是退出的逻辑
        if (buffer[0] == 'e'&&buffer[1] == 'x'&&buffer[2] == 'i'&&buffer[3] == 't') {
            break; // 遇到 'q' 字符时退出循环
        }
        //将读取的数据写入标准输出
        bytes_written = write(STDOUT_FILENO, buffer, bytes_read);
        
        // 检查写入是否成功
        if (bytes_written != bytes_read) {
            // 写入失败，退出程序
            return 1;
        }
    }
    
    // 检查读取是否出错
    if (bytes_read < 0) {
        return 1;
    }
    
    return 0;
}