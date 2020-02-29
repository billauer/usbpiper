#include <stdio.h>
#include <sys/epoll.h>

#include "usbpiper.h"
#include "cuse.h"

static const int max_size = 0x20000;
#define ARRAYSIZE 10

static void eventloop(int pollfd) {
  struct epoll_event event_array[ARRAYSIZE];
  int num, i;

  while (1) {
    if ((num = epoll_wait(pollfd, event_array, ARRAYSIZE, -1)) < 0) {
      perror("epoll_wait");
      break;
    }

    for (i=0; i<num; i++) {
      struct pipercallback *c = event_array[i].data.ptr;
      if ((*c->callback)(event_array[i].events, c->private))
	return;
    }
  }
}

int main(int argc, char **argv) {
  int pollfd;

  WARN("\nNote: This utility is NOT a driver for the XillyUSB FPGA IP core.\n\n");

  if (init_devfile(max_size))
    return 1;

  pollfd = epoll_create1(0);

  if (pollfd < 0) {
    perror("epoll_create1");
    return 1;
  }

  if (init_usb(pollfd, max_size))
    return 1;

  eventloop(pollfd);

  return 0;
}
