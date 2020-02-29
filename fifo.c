#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "usbpiper.h"
struct piperfifo *piperfifo_new(unsigned int size) {
  struct piperfifo *fifo;

  if (!(fifo = malloc(sizeof(*fifo)))) {
    ERR("Failed to allocate memory for FIFO struct\n");
    return NULL;
  }

  if (!(fifo->mem = malloc(size))) {
    free(fifo);
    ERR("Failed to allocate memory for FIFO\n");
    return NULL;
  }

  if (mlock(fifo->mem, size)) {
    unsigned int i;
    unsigned char *buf = fifo->mem;

    WARN("Warning: Failed to lock RAM, so FIFO's memory may swap to disk.\n"
	 "(You may want to use ulimit -l)\n");

    // Write something every 1024 bytes (4096 should be OK, actually).
    // Hopefully all pages are in real RAM after this. Better than nothing.

    for (i=0; i<size; i+=1024)
      buf[i] = 0;
  }

  fifo->size = size;
  fifo->fill = 0;
  fifo->readpos = 0;
  fifo->writepos = 0;

  return fifo;
}

void piperfifo_destroy(struct piperfifo *fifo) {
  if (!fifo)
    return; // Better safe than SEGV

  munlock(fifo->mem, fifo->size);

  free(fifo->mem);
  free(fifo);
}

unsigned int piperfifo_write(struct piperfifo *fifo,
			     void *data, unsigned int len) {
  unsigned int done = 0;
  unsigned int todo = len;

  while (1) {
    unsigned int nmax = fifo->size - fifo->fill;
    unsigned int nrail = fifo->size - fifo->writepos;
    unsigned int n = (todo > nmax) ? nmax : todo;

    if (n == 0)
      return done;

    if (n > nrail)
      n = nrail;

    memcpy(fifo->mem + fifo->writepos, data + done, n);

    done += n;
    todo -= n;

    fifo->writepos += n;
    fifo->fill += n;

    if (fifo->writepos == fifo->size)
      fifo->writepos = 0;
  }
}

unsigned int piperfifo_read(struct piperfifo *fifo,
			    void *data, unsigned int len) {
  unsigned int done = 0;
  unsigned int todo = len;

  while (1) {
    unsigned int nrail = fifo->size - fifo->readpos;
    unsigned int n = (todo > fifo->fill) ? fifo->fill : todo;

    if (n == 0)
      return done;

    if (n > nrail)
      n = nrail;

    memcpy(data + done, fifo->mem + fifo->readpos, n);

    done += n;
    todo -= n;

    fifo->readpos += n;
    fifo->fill -= n;

    if (fifo->readpos == fifo->size)
      fifo->readpos = 0;
  }
}

// Reduce the number of elements in FIFO to @len, return the number of
// bytes removed.
unsigned int piperfifo_limit(struct piperfifo *fifo,
			     unsigned int len) {
  unsigned int n;

  if (fifo->fill <= len)
    return 0;

  n = fifo->fill - len;

  if (fifo->writepos < n)
    fifo->writepos += fifo->size;

  fifo->writepos -= n;
  fifo->fill -= n;

  return n;
}
