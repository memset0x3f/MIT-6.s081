#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MAX 35
#define DATA_SIZE sizeof(int)

int sieve(int fd){
    int p[2];
    pipe(p);
    int base, num, stat;
    stat = read(fd, &num, DATA_SIZE);
    if(stat <= 0) return 0;

    base = num;
    printf("prime %d\n", base);
    while(read(fd, &num, DATA_SIZE) > 0){
        if(num % base == 0) continue;
        write(p[1], &num, DATA_SIZE);
    }
    close(fd);
    close(p[1]);
    if(fork() > 0){
        // Parent
        close(p[0]);
        int stat;
        wait(&stat);
        return stat;
    }
    else{
        int stat = sieve(p[0]);
        close(p[0]);
        return stat;
    }
}

int main(){
    int p[2];
    pipe(p);
    for(int i = 2; i <= 35; i++){
        write(p[1], &i, DATA_SIZE);
    }
    close(p[1]);
    int stat = sieve(p[0]);
    if(stat != 0) fprintf(2, "Error!");

    exit(0);
}