/* Test 6 - testing setwaitingticks and setrunningticks in the middle of a program

Processes A, B, and C are created. C is pinned to queue0.
WAITING_THRESHOLD and RUNNING_THRESHOLD are set to very large numbers.

A, B, and C execute in round-robin in queue0 since nothing can be demoted.
Then, RUNNING_THRESHOLD is set to 4, so A and B drop into queue1.
Then, WAITING_THRESHOLD is set to 2, so A and B rejoin C in queue0.

Initially:
...ABCABCABCABC...

After decreasing running threshold:
...CCCCCC...

After decreasing waiting threshold:
...ABCABCABC...CCC...ABCABCABC...CCC...
*/
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

void loop()
{
  int i;
  int j=0;
  for(i=0;i<1000000;i++)
    j=j+1;
}
void
forktest(void)
{
  int pid1;
  int ret;
  int fds1[2];

  ret = setrunningticks(1000000);
  if (ret < 0)
  {
    printf(1, "cannot set running ticks\n");
    exit();
  }
  ret = setwaitingticks(1000000);
    if (ret < 0)
  {
    printf(1, "cannot set waiting ticks\n");
    exit();
  }

  ret = pipe(fds1);
  if ( ret < 0)
  {
    printf(1, "cannot create a pipe\n");
    exit();
  }

  pid1 = fork();
  if(pid1 < 0)
    return;
    
  if(pid1 == 0){
      int i;
      char buf[256];
      // block here
      close(fds1[1]);
      read(fds1[0], buf, 1);
      printf(1, "\nstart process A [%d]\n", getpid());

      for (i=0;i<50;i++)
      {
        //printf(1, "Program C[%d] %d\n", getpid(), i);
	loop();
      }
  }
  else
  {
    int pid2;
    int fds2[2];
    ret = pipe(fds2);

    ret = pipe(fds2);
    if (ret < 0)
    {
        printf(1, "cannot create the second pipe\n");
    }

    pid2 = fork();
    if(pid2 < 0)
      return;
    
    if(pid2 == 0){
      int i;
      char buf[256];
      close(fds1[0]);
      close(fds2[1]);
      //block here
      read(fds2[0], buf, 1);
      write(fds1[1], "Done", 5);
      printf(1, "\nstart process B [%d]\n", getpid());

      for (i=0;i<50;i++)
      {
	//printf(1, "Program B[%d] %d\n", getpid(), i);
        loop();
      }
    }
    else
    {
      int i;
      close(fds1[0]);
      close(fds1[1]);
      close(fds2[0]);
      write(fds2[1],"Done", 5);
      printf(1, "\nstart process C [%d]\n", getpid());

      setpriority(getpid(), 0);
      printf(1, "\n C pinned\n");

      for (i=0;i<25;i++)
      {
        //printf(1, "Program A[%d] %d\n", getpid(), i);
        loop();
      }

      ret = setrunningticks(4);
      if (ret < 0)
      {
        printf(1, "cannot set running ticks\n");
        exit();
      }
      printf(1, "\n Running threshold = 4\n");

      for (i=0;i<25;i++)
      {
        //printf(1, "Program A[%d] %d\n", getpid(), i);
        loop();
      }

      ret = setwaitingticks(2);
      if (ret < 0)
      {
        printf(1, "cannot set waiting ticks\n");
        exit();
      }
      printf(1, "\n Waiting threshold = 2\n");

      for (i=0;i<25;i++)
      {
        //printf(1, "Program A[%d] %d\n", getpid(), i);
        loop();
      }

      wait();
      wait();
    }
  }
}

int
main(void)
{
  forktest();
  exit();
}
