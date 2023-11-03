#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    const char *filename = "server_file.txt";
    const char *port = "80";  // HTTP默认端口号

    // 打开包含IP地址的文件
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        perror("fopen");
        exit(1);
    }

    // 读取文件中的IP地址行数
    int num_lines = 0;
    char buffer[100]; 

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        num_lines++;
    }

    // 重新定位文件指针到文件开头
    fseek(fp, 0, SEEK_SET);

    // 分配二维字符数组来存储IP地址
    char **ip_addresses = (char **)malloc(num_lines * sizeof(char *));
    if (ip_addresses == NULL) {
        perror("malloc");
        fclose(fp);
        exit(1);
    }

    for (int i = 0; i < num_lines; i++) {
        ip_addresses[i] = (char *)malloc(100 * sizeof(char));
        if (ip_addresses[i] == NULL) {
            perror("malloc");
            fclose(fp);
            exit(1);
        }
    }

    // 读取文件中的IP地址并存储到二维数组
    for (int i = 0; i < num_lines; i++) {
        if (fgets(ip_addresses[i], 100, fp) == NULL) {
            perror("fgets");
            fclose(fp);
            exit(1);
        }
        // 去除末尾换行符
        size_t ip_len = strlen(ip_addresses[i]);
        if (ip_addresses[i][ip_len - 1] == '\n') {
            ip_addresses[i][ip_len - 1] = '\0';
        }
    }

    fclose(fp);

    // 现在你有一个二维数组ip_addresses，其中存储了每行的IP地址
    for (int i = 0; i < num_lines; i++) {
        printf("IP address %d: %s\n", i + 1, ip_addresses[i]);
        // 在这里可以执行连接和其他操作
    }

    // 释放分配的内存
    for (int i = 0; i < num_lines; i++) {
        free(ip_addresses[i]);
    }
    free(ip_addresses);

    return 0;
}
