#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


int main(int argc, char *argv[]) {
  int p[2]; 
  int pid; 
  char content;
  if(argc > 1){
    fprintf(2, "usage: pingpong\n");
    exit(1);
  }
  pipe(p);
  if (fork() == 0) {
      // child
      pid = getpid();
      // waiting 1 byte from parent
      read(p[0],&content,1);
      close(p[0]);
      printf("%d: received ping\n",pid);
      // send 1 byte to parent 
      write(p[1],"0",1);
      close(p[1]);
  } else {
      // parent
      pid = getpid();
      // send 1 byte to child
      write(p[1],"0",1);
      close(p[1]);
      // IMPORTANT
      // should wait for child here
      // or the parent will read the data write by itself
      wait(0);
      // waiting 1 byte from child
      read(p[0],&content,1);
      close(p[0]);
      printf("%d: received pong\n",pid);
  }
  exit(0);
}