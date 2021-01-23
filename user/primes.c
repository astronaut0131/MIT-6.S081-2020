#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
void child_process(int p[2]) {
    int len;
    int prime;
    int num;
    int pp[2];
    // no need to write to left neighbor
    close(p[1]);
    len = read(p[0],&prime,sizeof(int));
    if (len == 0) {
        close(p[0]);
        exit(0);
    }
    printf("prime %d\n",prime);
    pipe(pp);
    // create right neighbor
    if (fork() == 0) {
        // right neighbor don't need p
        close(p[0]);
        child_process(pp);
    } else {
        // no need to read from right
        close(pp[0]);
        while (1) {
            // read from left neighbor
            len = read(p[0],&num,sizeof(int));
            if (len == 0) {
                break;
            }
            // filter the number read
            if (num % prime != 0) {
                // write to right neighbor
                write(pp[1],&num,sizeof(int));
            }
        }
        close(p[0]);
        close(pp[1]);
        wait(0);
    }
    exit(0);
}
int main(int argc, char *argv[])
{
  int i;
  int p[2];
  if(argc > 1){
    fprintf(2, "usage: primes\n");
    exit(1);
  }
  pipe(p);
  // first round
  // generate 2 through 35 to child process
  if (fork() == 0) {
      child_process(p);
  } else {
      // no need to read from right
      close(p[0]);
      for (i = 2; i <= 35; i++) {
          write(p[1],&i,sizeof(int));
      }
      close(p[1]);
      wait(0);
  }
  exit(0);
}