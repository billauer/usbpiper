#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>

#include "usbpiper.h"

#define FIFOSIZE 262144
const uint16_t vendorID = 0x1234, prodID = 0x5678;
const uint8_t int_idx = 0; // Interface Number
const uint8_t alt_idx = 0; // Alternate setting

const static int td_bufsize = 1 << 16;
const int numtd = 10;

static libusb_context *ctx = NULL; // A libusb session
static int global_pollfd;
struct pipercallback usb_callback_info;

// A few simple list functions. One entry is the header, and the rest are
// payload entries. Linux kernel style lists, that is.

void insert_list(struct pipertd *new_entry,
		 struct pipertd *after_entry) {
  new_entry->prev = after_entry;
  new_entry->next = after_entry->next;

  after_entry->next->prev = new_entry;
  after_entry->next = new_entry;
}

void remove_list(struct pipertd *entry) {
  entry->prev->next = entry->next;
  entry->next->prev = entry->prev;
}

static void shutdown_endpoint_on_fail(struct piperendpoint *xep,
				      int rc) {
  if (rc)
    exit(1); // For now, terminate completely
}

static void transfer_in_callback(struct libusb_transfer *transfer) {
  struct pipertd *td = transfer->user_data;
  struct piperendpoint *xep = td->xep;
  int len = transfer->actual_length;
  enum xusb_state state = xep->dev->state;

  // td is moved back to pool before its data is processed. This is OK
  // because try_queue_bulkin() is called only after the data has been reaped,
  // and this runs single threaded.

  remove_list(td); // Remove entry from td_queued
  insert_list(td, xep->td_pool->prev); // Last entry in list

  xep->num_queued_tds--;

  switch(transfer->status) {
  case LIBUSB_TRANSFER_COMPLETED:
    if (piperfifo_write(xep->fifo, transfer->buffer, len) != len) {
      BUG("Overflow on BULK IN FIFO of %s\n", xep->dev->name);
      shutdown_endpoint_on_fail(xep, 1);
      return;
    }

    if (xep->dev->unique_up)
      shutdown_endpoint_on_fail(xep, try_complete_read(xep->dev));

    if (state == XUSB_OPEN)
      shutdown_endpoint_on_fail(xep, try_queue_bulkin(xep));

    break;

  case LIBUSB_TRANSFER_CANCELLED:
    break;

  default:
    ERR("On BULK IN endpoint %d (%s)\n", xep->ep, xep->dev->name);
    print_xfererr(transfer->status, "Transfer result");
    shutdown_endpoint_on_fail(xep, 1);
  }

  if (state == XUSB_RELEASING)
    shutdown_endpoint_on_fail(xep,
			      try_complete_release(xep->dev));
}

static void transfer_out_callback(struct libusb_transfer *transfer) {
  struct pipertd *td = transfer->user_data;
  struct piperendpoint *xep = td->xep;
  enum xusb_state state = xep->dev->state;

  remove_list(td); // Remove entry from td_queued
  insert_list(td, xep->td_pool->prev); // Last entry in list

  xep->num_queued_tds--;

  switch(transfer->status) {
  case LIBUSB_TRANSFER_COMPLETED:
    if (transfer->actual_length != transfer->length) {
      ERR("On BULK OUT endpoint %d (%s): "
	  "Attempted to send %d bytes, sent only %d.\n",
	  xep->ep, xep->dev->name,
	  transfer->length, transfer->actual_length);
      shutdown_endpoint_on_fail(xep, 1);
      return;
    }

    // Queue TDs for BULK OUT even in XUSB_RELEASING state, as this is part
    // of flushing existing data.
    shutdown_endpoint_on_fail(xep, try_queue_bulkout(xep, false));

    break;

  case LIBUSB_TRANSFER_CANCELLED:
    break;

  default:
    ERR("On BULK OUT endpoint %d (%s)\n", xep->ep, xep->dev->name);
    print_xfererr(transfer->status, "Transfer result");
    shutdown_endpoint_on_fail(xep, 1);
  }

  if (state == XUSB_RELEASING)
    shutdown_endpoint_on_fail(xep,
			      try_complete_release(xep->dev));
}

int try_queue_bulkin(struct piperendpoint *xep) {
  int fifo_left = fifo_vacant(xep->fifo) - xep->num_queued_tds * td_bufsize;
  int rc;

  while ((fifo_left >= td_bufsize) && !empty_list(xep->td_pool)) {
    struct pipertd *td = xep->td_pool->next;

    switch (xep->transfer_type) {
    case LIBUSB_TRANSFER_TYPE_BULK:
      libusb_fill_bulk_transfer(td->transfer, td->xep->usbdevice,
				(td->xep->ep | LIBUSB_ENDPOINT_IN),
				td->transfer->buffer, td_bufsize,
				transfer_in_callback, td, 0);
      break;
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:
      libusb_fill_interrupt_transfer(td->transfer, td->xep->usbdevice,
				     (td->xep->ep | LIBUSB_ENDPOINT_IN),
				     td->transfer->buffer, td_bufsize,
				     transfer_in_callback, td, 0);
      break;
    default:
      BUG("try_queue_bulkin: Unexpected transfer type %d\n",
	  xep->transfer_type);
      return 1;
    }

    rc = libusb_submit_transfer(td->transfer);

    if (rc < 0) {
      ERR("While queueing BULK IN TD on endpoint %d on behalf of %s:\n",
	  xep->ep, xep->dev->name);
      print_usberr(rc, "libusb_submit_transfer");
      return 1;
    }

    remove_list(td); // Remove entry from td_pool
    insert_list(td, xep->td_queued->prev); // Last entry in list

    xep->num_queued_tds++;
    fifo_left -= td_bufsize;
  }

  return 0;
}

int try_queue_bulkout(struct piperendpoint *xep,
		      boolean try_complete) {
  int rc;
  struct piperfifo *fifo = xep->fifo;
  boolean try_write = try_complete;

  while (!empty_list(xep->td_pool)) {
    struct pipertd *td = xep->td_pool->next;
    unsigned int fill = fifo_fill(fifo);
    unsigned int len;

    if (!fill)
      break;

    // Send a partially filled TD only if there's no other one currently
    // queued. This is a balance between fairly low latency an not wasting
    // too much resources on low-bandwidth data sources.

    if ((fill < td_bufsize) && !empty_list(xep->td_queued))
      break;

    len = piperfifo_read(fifo, td->transfer->buffer, td_bufsize);

    switch (xep->transfer_type) {
    case LIBUSB_TRANSFER_TYPE_BULK:
      libusb_fill_bulk_transfer(td->transfer, td->xep->usbdevice,
				(td->xep->ep | LIBUSB_ENDPOINT_OUT),
				td->transfer->buffer, len,
				transfer_out_callback, td, 0);
      break;
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:
      libusb_fill_interrupt_transfer(td->transfer, td->xep->usbdevice,
				     (td->xep->ep | LIBUSB_ENDPOINT_OUT),
				     td->transfer->buffer, len,
				     transfer_out_callback, td, 0);
      break;
    default:
      BUG("try_queue_bulkout: Unexpected transfer type %d\n",
	  xep->transfer_type);
      return 1;
    }

    rc = libusb_submit_transfer(td->transfer);

    if (rc < 0) {
      ERR("While queueing BULK OUT TD on endpoint %d on behalf of %s:\n",
	  xep->ep, xep->dev->name);
      print_usberr(rc, "libusb_submit_transfer");
      return 1;
    }

    remove_list(td); // Remove entry from td_pool
    insert_list(td, xep->td_queued->prev); // Last entry in list

    xep->num_queued_tds++;

    try_write = true;
  }

  if (try_write && xep->dev->unique_down && (xep->dev->state == XUSB_OPEN))
     return try_complete_write(xep->dev);

  // try_complete_release() is called elsewhere when relevant

  return 0;
}

static int usb_epoll_callback(uint32_t events, void *private) {
  static libusb_context *cb_ctx;
  struct timeval zero_tv = { 0, 0 };
  int rc;

  cb_ctx = private;

  rc = libusb_handle_events_timeout(cb_ctx, &zero_tv);

  if (!rc)
    return 0;

  print_usberr(rc, "libusb_handle_events_timeout");
  return 1;
}

static void usb_epoll_add(int fd, short events, void *user_data) {
  int cb_pollfd = *(int *)user_data;
  struct epoll_event event;

  event.events = events;
  event.data.ptr = &usb_callback_info;

  if (epoll_ctl(cb_pollfd, EPOLL_CTL_ADD, fd, &event)) {
    ERR("While attempting to add epoll event for libusb:\n");
    perror("epoll_ctl");
    exit(1);
  }

  DEBUG("callback: libusb fd %d added to pollfd\n", fd);
}

static void usb_epoll_remove(int fd, void *user_data) {
  int cb_pollfd = *(int *)user_data;

  if (epoll_ctl(cb_pollfd, EPOLL_CTL_DEL, fd, NULL)) {
    ERR("While attempting to remove epoll event for libusb:\n");
    perror("epoll_ctl");
    exit(1);
  }

  DEBUG("callback: libusb fd %d removed from pollfd\n", fd);
}

static int find_device(struct libusb_device **dev,
		       libusb_device_handle **dev_handle,
		       struct libusb_device_descriptor *desc) {
  int rc;
  int i;

  struct libusb_device **devs;

  rc = libusb_get_device_list(ctx, &devs);
  if (rc < 0) {
    print_usberr(rc, "Failed to get device list");
    return 1;
  }

  for (i=0; devs[i]; i++) {
    rc = libusb_get_device_descriptor(devs[i], desc);
    if (rc) {
      print_usberr(rc, "Failed to get device descriptor");
      libusb_free_device_list(devs, 1);
      return 1;
    }

    if ((desc->idVendor == vendorID) && (desc->idProduct == prodID)) {
      rc = libusb_open(devs[i], dev_handle);

      if (rc) {
	print_usberr(rc, "Device found, but failed to open it");
	libusb_free_device_list(devs, 1);
	return 1;
      }

      *dev = devs[i];
      return 0;
    }
  }
  ERR("Failed to find USB device %04x/%04x\n", vendorID, prodID);
  return 1;
}

static int setup_streams(libusb_device_handle *dev_handle,
			 const struct libusb_endpoint_descriptor *ep,
			 int num_ep, int max_size) {
  int i, ii;
  char n[32];

  for (ii=0; ii<num_ep; ii++, ep++) {
    struct piperendpoint *xep;
    int fifo_size, d, e, transfer_type;
    struct pipertd *td_array, *last_td;

    DEBUG("bEndpointAddress: %02x,  bmAttributes: %02x\n",
	  ep->bEndpointAddress, ep->bmAttributes);

    // Prepare endpoints

    d = (ep->bEndpointAddress & 0x80) ? 1 : 0;
    e = ep->bEndpointAddress & 0x0f;
    transfer_type = ep->bmAttributes & 0x3;

    if (transfer_type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
      WARN("Isochronous endpoints not supported, skipping %02x\n",
	   ep->bEndpointAddress);
      continue;
    }

    if (transfer_type == LIBUSB_TRANSFER_TYPE_CONTROL) {
      WARN("Control endpoints (?!!) not expected, skipping %02x\n",
	   ep->bEndpointAddress);
      continue;
    }

    fifo_size = d ? FIFOSIZE : FIFOSIZE + max_size;

    if (!(xep = malloc(sizeof(*xep)))) {
      ERR("Failed to allocate memory for struct piperendpoint\n");
      return 1;
    }

    if (!(td_array = malloc((numtd + 2) * sizeof(*td_array)))) {
      ERR("Failed to allocate memory for array of TDs\n");
      return 1;
    }

    if (!(xep->fifo = piperfifo_new(fifo_size)))
      return 1;

    sprintf(n, "usbpiper_%s_%s_%02d",
	    transfer_type == LIBUSB_TRANSFER_TYPE_BULK ? "bulk" : "interrupt",
	    d ? "in" : "out",
	    e);

    if (!(xep->dev = devfile_init(global_pollfd, n)))
      return 1;

    if (d) {
      xep->dev->source = xep;
      xep->dev->sink = NULL;
    } else {
      xep->dev->source = NULL;
      xep->dev->sink = xep;
    }

    xep->usbdevice = dev_handle;
    xep->ep = e;
    xep->transfer_type = transfer_type;
    xep->td_pool = td_array++;
    xep->td_queued = td_array++;

    xep->td_pool->prev = xep->td_pool->next = xep->td_pool;
    xep->td_pool->transfer = NULL;

    xep->td_queued->prev = xep->td_queued->next = xep->td_queued;
    xep->td_queued->transfer = NULL;
    xep->num_queued_tds = 0;

    last_td = xep->td_pool;

    for (i=0; i<numtd; i++) {
      struct pipertd *td = td_array++;

      void *tdbuf;

      td->transfer = libusb_alloc_transfer(0);
      tdbuf = malloc(td_bufsize);

      if (!tdbuf || !td->transfer) {
	ERR("Failed to allocate memory for transfer struct\n");
	return 1;
      }

      td->xep = xep;
      td->transfer->user_data = td;
      td->transfer->buffer = tdbuf;

      insert_list(td, last_td);
      last_td = td;
    }
  }
  return 0;
}

static int setup_device(struct libusb_device *dev,
			libusb_device_handle *dev_handle,
			struct libusb_device_descriptor *desc,
			int max_size) {
  int rc;
  struct libusb_config_descriptor *config;
  const struct libusb_interface *interface;
  const struct libusb_interface_descriptor *setting;

  uint8_t num_int, num_alt;

  rc = libusb_get_active_config_descriptor(dev, &config);

  if (rc) {
    print_usberr(rc, "Failed to obtain config descriptor");
    return 1;
  }

  num_int = config->bNumInterfaces;

  if (num_int <= int_idx) {
    ERR("There are only %d interfaces, hence requested interface %d is illegal.\n",
	num_int, int_idx);
    goto err;
  }

  interface = &config->interface[int_idx];
  num_alt = interface->num_altsetting;

  if (num_alt <= alt_idx) {
    ERR("Interface %d has only %d alternate settings.\n"
	"Hence requested alternate setting %d is illegal.\n",
	int_idx, num_alt, alt_idx);
    goto err;
  }

  rc = libusb_kernel_driver_active(dev_handle, int_idx);

  if (rc == 1) {
    INFO("A kernel driver is active on the device. Taking control instead.\n");

    rc = libusb_detach_kernel_driver(dev_handle, int_idx);

    if (rc) {
      print_usberr(rc, "Failed to detach kernel driver");
      goto err;
    }
  } else if (rc) {
     print_usberr(rc, "Failed to access device");
     goto err;
  }

  rc = libusb_claim_interface(dev_handle, int_idx);

  if (rc) {
    print_usberr(rc, "Failed to claim interface");
    goto err;
  }

  rc = libusb_set_interface_alt_setting(dev_handle, int_idx, alt_idx);

  if (rc) {
    print_usberr(rc, "Failed to set interface / alternate setting");
    goto err;
  }

  setting = &interface->altsetting[alt_idx];

  if (setup_streams(dev_handle, setting->endpoint, setting->bNumEndpoints,
		    max_size))
    goto err;

  libusb_free_config_descriptor(config);
  return 0;

 err:
  libusb_free_config_descriptor(config);
  return 1;
}

int init_usb(int pollfd, int max_size) {
  struct libusb_device *dev;
  libusb_device_handle *dev_handle;
  struct libusb_device_descriptor desc;
  const struct libusb_pollfd **fdarray, **entry;
  struct epoll_event event;

  int rc;

  rc = libusb_init(&ctx);

  if (rc) {
    print_usberr(rc, "Failed to init libusb");
    return 1;
  }

  libusb_set_debug(ctx, 3); // Set verbosity level to 3, as suggested in the documentation

  fdarray = libusb_get_pollfds(ctx);

  if (!fdarray) {
    ERR("The OS doesn't support epoll (this isn't Linux, is it?)\n");
    return 1;
  }

  if (!libusb_pollfds_handle_timeouts(ctx)) {
    ERR("The OS is too old to run this program\n");
    return 1;
  }

  usb_callback_info.callback = usb_epoll_callback;
  usb_callback_info.private = ctx;

  for (entry = fdarray; *entry; entry++) {
    const struct libusb_pollfd *p = *entry;
    event.events = p->events;
    event.data.ptr = &usb_callback_info;

    if (epoll_ctl(pollfd, EPOLL_CTL_ADD, p->fd, &event)) {
      ERR("While attempting to add epoll event for libusb:\n");
      perror("epoll_ctl");
      return 1;
    }

    DEBUG("init: libusb fd %d added to pollfd\n", p->fd);
  }

  // This isn't very pretty, but since ctx is global, it's pointless
  // pretending that the pollfd is anything but global.
  global_pollfd = pollfd;

  libusb_set_pollfd_notifiers(ctx, usb_epoll_add, usb_epoll_remove,
			      &global_pollfd);

  // libusb_free_pollfds(fdarray); -- Commented out, not always supported

  if (find_device(&dev, &dev_handle, &desc))
    return 1;

  return setup_device(dev, dev_handle, &desc, max_size);
}

int cancel_all(struct piperendpoint *xep) {
  struct pipertd *td;
  int rc;
  int rc_out = 0;

  for (td = xep->td_queued->next; td != xep->td_queued; td = td->next) {
    rc = libusb_cancel_transfer(td->transfer);

    if (rc && (rc != LIBUSB_ERROR_NOT_FOUND)) {
      ERR("While canceling a transfer on endpoint %d on behalf of %s:\n",
	  xep->ep, xep->dev->name);
      print_usberr(rc, "libusb_cancel_transfer");
      rc_out = 1;
    }
  }

  return rc_out;
}
