#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char **argv){
    if(argc != 1){
        fprintf(2, "usage: pingpong\n");
        exit(1);
    }
    int p1[2], p2[2];       // p1: parent->child, p2: child->parent
    if(pipe(p1) < 0 || pipe(p2) < 0){
        fprintf(2, "Create Pipe Failed!\n");
        exit(1);
    }
    if(fork() > 0){
        // Parent
        close(p1[0]);       // Parent do not read from p1
        close(p2[1]);       // Parent do not write to p2

        write(p1[1], "0", 1);
        char recv;
        int status = 0;
        if(read(p2[0], &recv, 1) > 0){
            printf("%d: received pong\n", getpid());
        }
        else{
            fprintf(2, "Read from child failed!\n");
            status = 1;
        }
        close(p1[1]);
        close(p2[0]);
        exit(status);
    }
    else{
        // Child
        close(p1[1]);       // Child do not write to p1
        close(p2[0]);       // Child do not read from p2
        char recv;
        int status = 0;
        if(read(p1[0], &recv, 1) > 0){
            printf("%d: received ping\n", getpid());
            write(p2[1], "1", 1);
        }
        else{
            fprintf(2, "Read from parent failed!\n");
            status = 1;
        }
        close(p1[0]);
        close(p2[1]);
        exit(status);
    }

    exit(0);
}