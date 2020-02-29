#ifndef _USBPIPER_H
#define _USBPIPER_H

#include <stdio.h>
#include <stdint.h>
#include <libusb-1.0/libusb.h>

#define BUG(...) { fprintf(stderr, __VA_ARGS__); }
#define ERR(...) { fprintf(stderr, __VA_ARGS__); }
#define WARN(...) { fprintf(stderr, __VA_ARGS__); }
#define LOG(...) { fprintf(stderr, __VA_ARGS__); }
#define DEBUG(...) { fprintf(stderr, __VA_ARGS__); }
#define INFO(...) { fprintf(stderr, __VA_ARGS__); }

enum xusb_state {
  XUSB_CLOSED = 0,
  XUSB_OPEN,
  XUSB_RELEASING,
};

typedef enum { false = 0, true = 1 } boolean;

struct piperfifo {
  unsigned int size; // In bytes
  unsigned int fill; // Number of bytes in the FIFO
  unsigned int readpos;
  unsigned int writepos;
  void *mem;
};

struct piperusbfile;
struct piperendpoint;

struct pipertd {
  struct pipertd *prev;
  struct pipertd *next;
  struct piperendpoint *xep;
  struct libusb_transfer *transfer;
};

struct piperendpoint {
  struct piperusbfile *dev;
  struct piperfifo *fifo;
  int ep;
  libusb_device_handle *usbdevice;
  struct pipertd *td_pool; // List header of unused TDs
  struct pipertd *td_queued; // List header of TDs submitted to libusb
  int num_queued_tds;
  int transfer_type;
};

struct pipercallback {
  int (*callback)(uint32_t, void *);
  void *private;
};

struct piperusbfile {
  libusb_device_handle *usbdevice;
  int fd;
  int timerfd;
  char *name;
  enum xusb_state state;
  uint64_t unique_up;
  uint64_t unique_down; // Also for release
  struct piperendpoint *sink;
  struct piperendpoint *source;
  struct pipercallback *callback;
  struct pipercallback *timer_callback;
  uint32_t read_size;
  uint32_t write_size;
  int timer_armed:1;
  int timed_out:1;
  int interrupted_up:1;
  int interrupted_down:1;
  int bulkout_canceled:1;

  // Temporary, for simple loopback
  struct piperusbfile *counterpart;
};

static inline unsigned int fifo_fill(struct piperfifo *fifo) {
  return fifo->fill;
}

static inline unsigned int fifo_vacant(struct piperfifo *fifo) {
  return fifo->size - fifo->fill;
}

// Headers for devfile.c:
struct piperusbfile *devfile_init(int pollfd, char *name);
void devfile_destroy(struct piperusbfile *xusb);
int init_devfile(int max_size);
void deinit_devfile(void);
int try_complete_release(struct piperusbfile *xusb);
int try_complete_write(struct piperusbfile *xusb);
int try_complete_read(struct piperusbfile *xusb);

// Headers for fifo.c:

struct piperfifo *piperfifo_new(unsigned int size);
void piperfifo_destroy(struct piperfifo *fifo);
unsigned int piperfifo_write(struct piperfifo *fifo,
			     void *data, unsigned int len);
unsigned int piperfifo_read(struct piperfifo *fifo,
			    void *data, unsigned int len);
unsigned int piperfifo_limit(struct piperfifo *fifo,
			     unsigned int len);

// Headers for usb.c:
int init_usb(int pollfd, int max_size);
int cancel_all(struct piperendpoint *xep);
int try_queue_bulkin(struct piperendpoint *xep);
int try_queue_bulkout(struct piperendpoint *xep,
		      boolean try_complete);

void insert_list(struct pipertd *new_entry,
		 struct pipertd *after_entry);
void remove_list(struct pipertd *entry);

static inline int empty_list(struct pipertd *header) {
  return header->next == header;
}

// Headers for usberrors.c:
void print_usberr(int errnum, char *msg);
void print_xfererr(int errnum, char *msg);

#endif
