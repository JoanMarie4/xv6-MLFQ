#include "types.h"
#include "stat.h"
#include "user.h"

/* CS350 ATTENTION: to ensure correct compilation of the base code, 
   stub functions for the system call user space wrapper functions are provided. 
   REMEMBER to disable the stub functions (by commenting the following macro) to 
   allow your implementation to work properly. */
//#define STUB_FUNCS
#ifdef STUB_FUNCS
void setwaitingticks(void) {}
#endif


int 
main(int argc, char * argv[])
{
    setwaitingticks(atoi(argv[1]));
    exit(); //return 0;
}
