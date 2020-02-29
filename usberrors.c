#include <stdio.h>
#include <libusb-1.0/libusb.h>

#include "usbpiper.h"

void print_usberr(int errnum, char *msg) {
  int i;

  const struct {
    int num;
    char *symbol;
    char *text;
  } errs[] = {
    { LIBUSB_SUCCESS, "LIBUSB_SUCCESS", "Success (no error)" },
    { LIBUSB_ERROR_IO, "LIBUSB_ERROR_IO", "Input/output error" },
    { LIBUSB_ERROR_INVALID_PARAM, "LIBUSB_ERROR_INVALID_PARAM", "Invalid parameter" },
    { LIBUSB_ERROR_ACCESS, "LIBUSB_ERROR_ACCESS", "Access denied (insufficient permissions)" },
    { LIBUSB_ERROR_NO_DEVICE, "LIBUSB_ERROR_NO_DEVICE", "No such device (it may have been disconnected)" },
    { LIBUSB_ERROR_NOT_FOUND, "LIBUSB_ERROR_NOT_FOUND", "Entity not found" },
    { LIBUSB_ERROR_BUSY, "LIBUSB_ERROR_BUSY", "Resource busy" },
    { LIBUSB_ERROR_TIMEOUT, "LIBUSB_ERROR_TIMEOUT", "Operation timed out" },
    { LIBUSB_ERROR_OVERFLOW, "LIBUSB_ERROR_OVERFLOW", "Overflow" },
    { LIBUSB_ERROR_PIPE, "LIBUSB_ERROR_PIPE", "Pipe error" },
    { LIBUSB_ERROR_INTERRUPTED, "LIBUSB_ERROR_INTERRUPTED", "System call interrupted (perhaps due to signal)" },
    { LIBUSB_ERROR_NO_MEM, "LIBUSB_ERROR_NO_MEM", "Insufficient memory" },
    { LIBUSB_ERROR_NOT_SUPPORTED, "LIBUSB_ERROR_NOT_SUPPORTED", "Operation not supported or unimplemented on this platform" },
    { LIBUSB_ERROR_OTHER, "LIBUSB_ERROR_OTHER", "Other error" },
    {}
  };

  for (i=0; errs[i].text; i++) {
    if (errs[i].num == errnum) {
      ERR("%s: %s (%s)\n", msg, errs[i].text, errs[i].symbol);
      return;
    }
  }

  ERR("%s: Unknown error %d\n", msg, errnum);
}

void print_xfererr(int errnum, char *msg) {
  int i;

  const struct {
    int num;
    char *symbol;
    char *text;
  } errs[] = {
    { LIBUSB_TRANSFER_COMPLETED, "LIBUSB_TRANSFER_COMPLETED", "Transfer completed" },
    { LIBUSB_TRANSFER_ERROR, "LIBUSB_TRANSFER_ERROR", "Transfer failed" },
    { LIBUSB_TRANSFER_NO_DEVICE, "LIBUSB_TRANSFER_NO_DEVICE", "No such device (it may have been disconnected)" },
    { LIBUSB_TRANSFER_TIMED_OUT, "LIBUSB_TRANSFER_TIMED_OUT", "Operation timed out" },
    { LIBUSB_TRANSFER_OVERFLOW, "LIBUSB_TRANSFER_OVERFLOW", "Overflow" },
    { LIBUSB_TRANSFER_STALL, "LIBUSB_TRANSFER_STALL", "Endpoint returned STALL" },
    { LIBUSB_TRANSFER_CANCELLED, "LIBUSB_TRANSFER_CANCELLED", "Transfer was cancelled" },
    {}
  };

  for (i=0; errs[i].text; i++) {
    if (errs[i].num == errnum) {
      ERR("%s: %s (%s)\n", msg, errs[i].text, errs[i].symbol);
      return;
    }
  }

  ERR("%s: Unknown error %d\n", msg, errnum);
}
