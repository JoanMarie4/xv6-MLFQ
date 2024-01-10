/* Test 5 -- testing pinning a process while it is in queue1
In this test, we set waiting ticks to be a large number

We have processes A, B, and C run in round robin until they are all in queue1
Then, we pin process C, and it should jump to queue0.
Then we unpin C so it should drop to queue1 and the processes should execute by priority

Before pinning C:
...ABCABCABCABCABC...

After pinnning C:
...CCCCCCC...

After unpinning C:
...ABABAB...ABCABCABC...
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

  ret = setrunningticks(2);
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
      printf(1, "\n start process A [%d]\n", getpid());

      for (i=0;i<100;i++)
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
      printf(1, "\n start process B [%d]\n", getpid());

      for (i=0;i<100;i++)
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
      printf(1, "\n start process C [%d]\n", getpid());

      for (i=0;i<25;i++)
      {
        //printf(1, "Program A[%d] %d\n", getpid(), i);
        loop();
      }
      setpriority(getpid(), 0);
      printf(1, "\n C pinned\n");

      for (i=0;i<20;i++)
      {
        //printf(1, "Program A[%d] %d\n", getpid(), i);
        loop();
      }

      setpriority(getpid(), 1);
      printf(1, "\n C unpinned\n");

      for (i=0;i<20;i++)
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
