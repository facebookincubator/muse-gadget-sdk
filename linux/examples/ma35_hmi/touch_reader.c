// SPDX-License-Identifier: Apache-2.0
#include <linux/input.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
int main(void){
 int fd=open("/dev/input/event0",O_RDONLY); if(fd<0){perror("touch open");return 1;}
 struct input_absinfo ax,ay;
 if(ioctl(fd,EVIOCGABS(ABS_X),&ax)||ioctl(fd,EVIOCGABS(ABS_Y),&ay)){perror("touch range");return 1;}
 setvbuf(stdout,NULL,_IOLBF,0);printf("RANGE %d %d %d %d\n",ax.minimum,ax.maximum,ay.minimum,ay.maximum);
 int x=0,y=0,down=0,last=0;struct input_event e;
 while(read(fd,&e,sizeof(e))==sizeof(e)){
  if(e.type==EV_ABS&&e.code==ABS_X)x=e.value;
  if(e.type==EV_ABS&&e.code==ABS_Y)y=e.value;
  if(e.type==EV_KEY&&e.code==BTN_TOUCH)down=e.value;
  if(e.type==EV_SYN&&e.code==SYN_REPORT&&down!=last){printf("%s %d %d\n",down?"DOWN":"UP",x,y);last=down;}
 }
 return 0;
}
