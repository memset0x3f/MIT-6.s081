#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

#define MAXLINE 512

char *readline(){
    static char buf[MAXLINE];
    int cnt = 0;
    while(read(0, &buf[cnt], 1) == 1){
        if(buf[cnt] == '\n') break;
        cnt++;
    }
    buf[cnt] = '\0';
    return buf;
}

void xargs(char *program, int argc, char **argv){
    char *args[MAXARG];
    for(int i = 0; i < argc; i++){
        args[i] = (char*)malloc((strlen(argv[i])+1)*sizeof(char));
        strcpy(args[i], argv[i]);
    }
    while(1){
        char *arg = readline();
        int len = strlen(arg);
        if(len == 0) break;
        args[argc] = (char*)malloc((len+1)*sizeof(char));
        strcpy(args[argc], arg);
        argc++;
    }
    if(fork() > 0){
        // Parent
        int stat;
        wait(&stat);
        for(int i = 0; i < argc; i++){
            free(args[i]);
        }
        if(stat){
            fprintf(2, "Error during execution!");
            exit(1);
        }
    }
    else{
        int stat = exec(program, args);
        if(stat) exit(1);
    }
}

int main(int argc, char **argv){
    if(argc < 2){
        fprintf(2, "Usage: xargs <program> [args]");
        exit(1);
    }
    xargs(argv[1], argc-1, argv+1);
    exit(0);
}