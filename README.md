# usbpiper

Based upon CUSE, libusb and the kernel's epoll capability,
this is a single-threaded utility which generates one /dev/usbpiper_* device
file for each bulk / interrupt endpoint on a USB device.

It is *not* a driver for the
[XillyUSB FPGA IP Core](http://xillybus.com/xillyusb).

This is an unfinished project. Explanations, some howto and other information
on this utility can be found on
[this page](http://billauer.co.il/blog/2020/02/usbpiper-cuse-epoll-libusb/).
