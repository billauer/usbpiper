#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include "cuse.h"
#include "usbpiper.h"

static int max_size;
static int bufsize;

static void *buf, *complbuf;

// timerfd_settime() clears any pending timer events, so there's no need
// for any dummy read() cleanup after calling this function.

static int timer_disarm(struct piperusbfile *xusb) {
  struct itimerspec t = {
    .it_interval = { 0, 0 },
    .it_value = { 0, 0 },
  };

  if (timerfd_settime(xusb->timerfd, 0, &t, NULL)) {
    perror("timerfd_settime");
    return 1;
  }

  xusb->timer_armed = 0;
  return 0;
}

static int timer_arm(struct piperusbfile *xusb,
		     struct timespec *delta) {
  struct itimerspec t = {
    .it_interval = { 0, 0 },
    .it_value = *delta,
  };

  if (timerfd_settime(xusb->timerfd, 0, &t, NULL)) {
    perror("timerfd_settime");
    return 1;
  }
  xusb->timer_armed = 1;
  return 0;
}

static int send_response(struct piperusbfile *xusb, void *buf) {
  int rc;
  struct fuse_out_header *h = buf;

  while (1) {
    rc = write(xusb->fd, buf, h->len);

    if ((rc < 0) && (errno == EINTR))
      continue;

    if (rc < 0) {
      perror("write response");
      return 1;
    }

    if (rc != h->len) {
      fprintf(stderr, "Huh? Wrote %d bytes, only %d accepted!\n",
	      h->len, rc);
      return 1;
    }
    return 0;
  }
}

static int complete_status_only(struct piperusbfile *xusb,
				uint64_t unique,
				int32_t	error) {
  struct fuse_out_header h;

  h.len = sizeof(h);
  h.error = error;
  h.unique = unique;

  return send_response(xusb, &h);
}

static int complete_init(struct piperusbfile *xusb,
			 struct fuse_in_header *inh) {
  char *devname = xusb->name;
  struct cuse_init_in *init_in = (void *) &inh[1];

  struct {
    struct fuse_out_header h;
    struct cuse_init_out r;
    char devname[64];
  } compl;

  const char assignment[] = "DEVNAME=";

  if ((strlen(assignment) + strlen(devname)) > 63) {
    BUG("Device name %s too long\n", devname);
    return 1;
  }

  if ((init_in->major != 7) || (init_in->minor < 21)) {
    ERR("FUSE revision %d.%d inadequate: 7.21 and later is required\n",
	init_in->major, init_in->minor);
    return 1;
  }

  compl.h.len = sizeof(struct fuse_out_header) +
    sizeof(struct cuse_init_out) +
    strlen(assignment) + strlen(devname) + 1;

  compl.h.error = 0;
  compl.h.unique = inh->unique;

  compl.r.major = 7;
  compl.r.minor = 21; // 7.21 is required to get the events in POLL requests

  compl.r.max_read = compl.r.max_write = max_size;
  compl.r.dev_major = 456;
  compl.r.dev_minor = xusb->fd; // fd is unique per daemon execution
  compl.r.flags = 0;

  strcpy(compl.devname, assignment);
  strcat(compl.devname, devname);

  return send_response(xusb, &compl);
}

static int complete_open(struct piperusbfile *xusb,
			 struct fuse_in_header *inh) {
  struct fuse_open_in *arg = (void *) &inh[1];

  struct {
    struct fuse_out_header h;
    struct fuse_open_out resp;
  } compl;

  // Note that Linux doesn't forward read() calls to a file not opened
  // for read, and same for write().

  // It so happens that O_RDONLY=0, O_WRONLY=1 and O_RDWR=2, which
  // is why the tests are written a bit weirdly.

  boolean open_for_read = !(arg->flags & O_WRONLY);
  boolean open_for_write = ((arg->flags & (O_WRONLY | O_RDWR)) != 0);

  DEBUG("OPEN %s flags = %08x\n", xusb->name, arg->flags);

  if (xusb->state != XUSB_CLOSED) {
    ERR("Rejected attempt to double-open %s\n", xusb->name);
    return complete_status_only(xusb, inh->unique, -EBUSY);
  }

  if ((open_for_read && !xusb->source) ||
      (open_for_write && !xusb->sink))
    return complete_status_only(xusb, inh->unique, -ENODEV);

  if (open_for_read && try_queue_bulkin(xusb->source))
    return 1;

  xusb->state = XUSB_OPEN;

  compl.h.len = sizeof(compl);
  compl.h.error = 0;
  compl.h.unique = inh->unique;

  // It appears like the kernel ignores the argument completely, so
  // imitate libfuse's answer blindly.
  compl.resp.fh = 0;
  compl.resp.open_flags = FOPEN_DIRECT_IO | FOPEN_NONSEEKABLE;

  return send_response(xusb, &compl);
}

int try_complete_release(struct piperusbfile *xusb) {
  struct piperfifo *sink_fifo = xusb->sink ? xusb->sink->fifo : NULL;
  unsigned int sink_fill = sink_fifo ? fifo_fill(sink_fifo) : 0;
  int rc = 0;
  boolean ok_to_release = true;

  // If there are outstanding USB TDs, don't release no matter what, because
  // then the device file can be reopened and get leftovers.
  if (xusb->source && !empty_list(xusb->source->td_queued))
    ok_to_release = false;

  if (xusb->sink && !empty_list(xusb->sink->td_queued))
    ok_to_release = false;

  // Wait until data towards device has reached destination
  if ((sink_fill != 0) && !xusb->timed_out && !xusb->interrupted_down)
    ok_to_release = false;

  if (ok_to_release) {
    if (xusb->timer_armed) {
      rc = timer_disarm(xusb);
      if (rc)
	return rc;
    }

    if ((sink_fill != 0) && (xusb->timed_out))
      WARN("Timed out while flushing. Lost at least %d bytes of data on %s.\n",
	   sink_fill, xusb->name);

    if (xusb->sink)
      piperfifo_limit(xusb->sink->fifo, 0);
    if (xusb->source)
      piperfifo_limit(xusb->source->fifo, 0);

    xusb->state = XUSB_CLOSED;
    rc = complete_status_only(xusb, xusb->unique_down,
			      xusb->interrupted_down ? -EINTR : 0);
    xusb->unique_down = 0;
    return rc;
  }

  // Couldn't complete the RELEASE request, maybe help to give it a push.

  if (xusb->sink && xusb->timed_out && !xusb->bulkout_canceled) {
    xusb->bulkout_canceled = 1;
    piperfifo_limit(sink_fifo, 0); // Also prevents queuing of BULK OUT TDs
    rc |= cancel_all(xusb->sink);
  }

  // If this is the first failure to complete immediately, set the timer
  if (!xusb->timer_armed) {
    struct timespec delta = { .tv_sec = 1, .tv_nsec = 0 };
    rc |= timer_arm(xusb, &delta);
  }

  return rc; // Didn't release, but no error unless something failed here
}

static int process_release(struct piperusbfile *xusb,
			   struct fuse_in_header *inh) {
  DEBUG("RELEASE %s\n", xusb->name);

  if (xusb->state != XUSB_OPEN)
    BUG("Huh? %s is not open, and yet it got a RELEASE request!\n",
	xusb->name);

  // RELEASE can be sent only when there are no more references to the
  // file descriptor. In particular, no outstanding request.

  if ((xusb->unique_down) || (xusb->unique_up)) {
    BUG("Huh? %s received a RELEASE request, but there's still outstanding I/O!\n",
	xusb->name);
    return complete_status_only(xusb, inh->unique, -EBADF);
  }

  if (xusb->timer_armed) {
    BUG("Huh? %s received a RELEASE request, with the timer armed!\n",
	xusb->name);
    (void) timer_disarm(xusb);
  }

  xusb->unique_down = inh->unique;
  xusb->state = XUSB_RELEASING;
  xusb->timed_out = 0;
  xusb->interrupted_down = 0;
  xusb->bulkout_canceled = 0;

  if (xusb->source)
    cancel_all(xusb->source);

  return try_complete_release(xusb);
}

// try_complete_write() assumes unique_down is non-zero (i.e. there's
// a blocking WRITE request.

// TODO: Support synchronous streams

int try_complete_write(struct piperusbfile *xusb) {
  uint32_t count;
  struct piperfifo *fifo = xusb->sink->fifo;
  struct {
    struct fuse_out_header h;
    struct fuse_write_out resp;
  } compl;
  int rc;

  if (!xusb->interrupted_down && (fifo_vacant(fifo) < max_size))
    return 0; // Didn't complete, and this is no error. So success.

  count = xusb->write_size;

  // If completion is forced by interrupt, ensure there's max_size bytes
  // vacant in the FIFO for the next WRITE, by possibly unwinding data

  if (xusb->interrupted_down)
    count -= piperfifo_limit(fifo, fifo->size - max_size);

  if ((count == 0) && (xusb->write_size != 0)) {
    rc = complete_status_only(xusb, xusb->unique_down, -EINTR);
    xusb->unique_down = 0;
    return rc;
  }

  compl.h.unique = xusb->unique_down;
  compl.h.len = sizeof(compl);
  compl.h.error = 0;
  compl.resp.size = count;

  xusb->unique_down = 0;

  return send_response(xusb, &compl);
}

int try_complete_read(struct piperusbfile *xusb) {
  struct piperfifo *fifo = xusb->source->fifo;
  uint32_t count = fifo_fill(fifo);

  struct fuse_out_header *compl = complbuf;
  int rc = 0;
  int bufsize = sizeof(*compl);

  if (xusb->interrupted_up && (count == 0)) {
    rc = complete_status_only(xusb, xusb->unique_up, -EINTR);
    xusb->unique_up = 0;
    return rc;
  }

  if ((count == 0) ||
      // Partial completion: Only on timeout or interrupt
      ((count < xusb->read_size) &&
       !(xusb->timed_out || xusb->interrupted_up))) {

    if (!xusb->timer_armed && !xusb->timed_out) {
      struct timespec delta = { .tv_sec = 0, .tv_nsec = 10000000 }; // 10 ms
      return timer_arm(xusb, &delta);
    }

    return 0; // Didn't complete, but no error (and the clock is ticking)
  }

  if (xusb->timer_armed) {
    rc = timer_disarm(xusb);
    if (rc)
      return rc;
  }

  if (count > xusb->read_size)
    count = xusb->read_size;

  bufsize += count;

  memset(compl, 0, sizeof(*compl));

  if (piperfifo_read(fifo, &compl[1], count) != count) {
    BUG("There's a bug with piperfifo_read()\n");
    return 1;
  }

  compl->unique = xusb->unique_up;
  compl->len = bufsize;
  compl->error = 0;

  xusb->unique_up = 0;

  rc = send_response(xusb, compl);

  // After getting some data off the FIFO, maybe a BULK IN TD can be queued
  rc |= try_queue_bulkin(xusb->source);

  return rc;
}

static int process_write(struct piperusbfile *xusb,
			 struct fuse_in_header *inh) {
  struct fuse_write_in *arg = (void *) &inh[1];
  int count;

  DEBUG("WRITE fh=%ld, offset=%ld, size=%d, write_flags=0x%08x,\n"
       "lock_owner=%ld, flags=0x%08x\n",
       arg->fh, arg->offset, arg->size, arg->write_flags,
       arg->lock_owner, arg->flags);

  // It's really not expected to happen
  if (!xusb->sink) {
    BUG("Huh? WRITE request to %s, which isn't writable\n", xusb->name);
    return complete_status_only(xusb, inh->unique, -EBADF);
  }

  // An already pending WRITE request is only possible in a multi-threaded
  // or forked process doing nonsense. So stop it now, even though the error
  // isn't per spec.
  if (xusb->unique_down)
    return complete_status_only(xusb, inh->unique, -EINVAL);

  // The FIFO should always have enough room for the entire buffer, since
  // a previous WRITE request must block until then. Or clean up.
  count = piperfifo_write(xusb->sink->fifo, &arg[1], arg->size);

  if (count != arg->size) {
    BUG("Huh? FIFO for %s was unable to accept %d bytes (only %d bytes)\n",
	xusb->name, arg->size, count);
    return 1;
  }

  xusb->unique_down = inh->unique;
  xusb->write_size = arg->size;
  xusb->interrupted_down = 0;

  return try_queue_bulkout(xusb->sink, true); // Calls try_complete_write()
}

static int process_read(struct piperusbfile *xusb,
			struct fuse_in_header *inh) {
  struct fuse_read_in *arg = (void *) &inh[1];

  DEBUG("READ fh=%ld, offset=%ld, size=%d, read_flags=0x%08x,\n"
       "lock_owner=%ld, flags=0x%08x\n",
       arg->fh, arg->offset, arg->size, arg->read_flags,
       arg->lock_owner, arg->flags);

  // It's really not expected to happen
  if (!xusb->source) {
    BUG("Huh? READ request to %s, which isn't readable\n", xusb->name);
    return complete_status_only(xusb, inh->unique, -EBADF);
  }

  // An already pending READ request is only possible in a multi-threaded
  // or forked process doing nonsense. So stop it now, even though the error
  // isn't per spec.
  if (xusb->unique_up)
    return complete_status_only(xusb, inh->unique, -EINVAL);

  if (xusb->timer_armed) {
    BUG("Huh? %s received a READ request with the timer armed!\n",
	xusb->name);
    (void) timer_disarm(xusb);
  }

  xusb->unique_up = inh->unique;
  xusb->read_size = arg->size;
  xusb->timed_out = 0;
  xusb->interrupted_up = 0;

  return try_complete_read(xusb);
}

static int process_interrupt(struct piperusbfile *xusb,
			     struct fuse_in_header *inh) {
  struct fuse_interrupt_in *arg = (void *) &inh[1];

  DEBUG("INTERRUPT unique=%ld\n", arg->unique);

  if (arg->unique == xusb->unique_down) {
    xusb->interrupted_down = 1;

    if (xusb->state == XUSB_OPEN)
      return try_complete_write(xusb);
    else
      return try_complete_release(xusb);
  }

  if (arg->unique == xusb->unique_up) {
    xusb->interrupted_up = 1;
    return try_complete_read(xusb);
  }

  // It's pefectly possible that an INTERRUPT request arrives after its
  // completion has been submitted due to a race condition. So do nothing.
  return 0;
}

static int read_from_cuse(uint32_t events, void *private) {
  int rc;
  struct piperusbfile *xusb = private;
  struct fuse_in_header *inh = buf;

  rc = read(xusb->fd, buf, bufsize);

  if ((rc < 0) && (errno == EINTR))
    return 0;

  if (rc < 0) {
    perror("CUSE read");

    if (events & EPOLLERR)
      ERR("(this error is probably related to the CUSE session in general)\n");

    return 1;
  }

  if (rc != inh->len) {
    BUG("CUSE read loop: Mismatch between read length = %d and len field %d\n",
	rc, inh->len);
    return 1;
  }

  DEBUG("len = %d, opcode = %d, unique = %ld, nodeid = %ld\n"
       "uid = %d, gid = %d, pid = %d\n",
       inh->len, inh->opcode, inh->unique, inh->nodeid,
       inh->uid, inh->gid, inh->pid);

  switch (inh->opcode) {
  case CUSE_INIT:
    return complete_init(xusb, inh);

  case FUSE_OPEN:
    return complete_open(xusb, inh);

  case FUSE_READ:
    return process_read(xusb, inh);

  case FUSE_WRITE:
    return process_write(xusb, inh);

  case FUSE_RELEASE:
    return process_release(xusb, inh);

  case FUSE_INTERRUPT:
    return process_interrupt(xusb, inh);

  case FUSE_IOCTL:
    // No ioctl() is supported
    return complete_status_only(xusb, inh->unique, -EINVAL);

  default:
    LOG("Unsupported opcode %d\n", inh->opcode);
    return complete_status_only(xusb, inh->unique, -ENOSYS);
  }
  return 1; // This is never reached; silence possible warning
}

static int read_from_timer(uint32_t events, void *private) {
  struct piperusbfile *xusb = private;
  int rc;
  uint64_t ticks;

  rc = read(xusb->timerfd, &ticks, sizeof(ticks));

  if (rc == sizeof(ticks)) { // Properly received timer
    xusb->timer_armed = 0;

    DEBUG("read_from_timer: ticks = %ld\n", ticks);

    xusb->timed_out = 1;
    if ((xusb->state == XUSB_OPEN) && xusb->unique_up)
      return try_complete_read(xusb);
    else if ((xusb->state == XUSB_RELEASING) && xusb->unique_down)
      return try_complete_release(xusb);

    // We should never reach this point, because the completion of any
    // timed request also disarms the timer, making the read() request
    // above return EAGAIN. No point killing the daemon on this, just warn.
    WARN("Unexpected timer event for %s\n", xusb->name);
    return 0;
  }

  // The read() is non-blocking, and may return EAGAIN if timerfd_settime()
  // was called while handling a file descriptor listed earlier in the poll
  // list, hence canceling the pending event. So just return.
  if (errno == EAGAIN)
    return 0;

  ERR("While attempting to read timerfd of %s:\n", xusb->name);
  perror("read");
  return 1;
}

struct piperusbfile *devfile_init(int pollfd, char *name) {
  struct piperusbfile *xusb;
  struct pipercallback *c, *timer_callback;
  struct epoll_event event;

  size_t namelen = strlen(name) + 1;

  if (!(xusb = malloc(sizeof(*xusb)))) {
    ERR("Failed to allocate memory for xusb\n");
    return NULL;
  }

  if (!(c = malloc(sizeof(*c)))) {
    ERR("Failed to allocate memory for callback struct\n");
    goto err1;
  }

  if (!(xusb->name = malloc(namelen))) {
    ERR("Failed to allocate memory for file name\n");
    goto err2;
  }

  memcpy(xusb->name, name, namelen);

  xusb->unique_up = 0;
  xusb->unique_down = 0;
  xusb->state = XUSB_CLOSED;
  xusb->callback = c;

  xusb->fd = open("/dev/cuse", O_RDWR);

  if (xusb->fd < 0) {
    ERR("While opening /dev/cuse for %s:\n", name);
    perror("open");
    goto err3;
  }

  c->callback = read_from_cuse;
  c->private = xusb;

  // read() is guaranteed to return immediately on EPOLLERR
  event.events = EPOLLIN | EPOLLERR;
  event.data.ptr = c;

  if (epoll_ctl(pollfd, EPOLL_CTL_ADD, xusb->fd, &event)) {
    perror("epoll_ctl");
    goto err3;
  }

  xusb->timer_armed = 0;
  xusb->timed_out = 0;
  xusb->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

  if (xusb->timerfd < 0) {
    perror("timerfd_create");
    goto err4;
  }

  if (!(timer_callback = malloc(sizeof(*timer_callback)))) {
    ERR("Failed to allocate memory for callback struct\n");
    goto err5;
  }
  xusb->timer_callback = timer_callback;

  timer_callback->callback = read_from_timer;
  timer_callback->private = xusb;

  event.events = EPOLLIN;
  event.data.ptr = timer_callback;

  if (epoll_ctl(pollfd, EPOLL_CTL_ADD, xusb->timerfd, &event)) {
    perror("epoll_ctl");
    goto err6;
  }

  return xusb;

 err6:
  free(timer_callback);
 err5:
  close(xusb->timerfd);
 err4:
  close(xusb->fd);
 err3:
  free(xusb->name);
 err2:
  free(c);
 err1:
  free(xusb);

  return NULL;
}

void devfile_destroy(struct piperusbfile *xusb) {
  close(xusb->timerfd);
  close(xusb->fd);
  free(xusb->timer_callback);
  free(xusb->callback);
  free(xusb->name);
  free(xusb);
}

int init_devfile(int global_max_size) {
  int max_in = sizeof(struct fuse_in_header) + sizeof(struct fuse_read_in);
  int max_out =  sizeof(struct fuse_out_header) +
    sizeof(struct fuse_write_out);
  int max_inout = (max_in > max_out) ? max_in : max_out;

  max_size = global_max_size;

  bufsize = max_size + max_inout;

  if (!(buf = malloc(bufsize))) {
    ERR("Failed to allocate memory for request buffer\n");
    return 1;
  }

  if (!(complbuf = malloc(max_size + sizeof(struct fuse_out_header)))) {
    ERR("Failed to allocate memory for completion buffer\n");
    free(buf);
    return 1;
  }

  return 0;
}

void deinit_devfile(void) {
  free(buf);
  free(complbuf);
}
