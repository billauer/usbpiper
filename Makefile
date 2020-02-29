CC= gcc
ALL= usbpiper
OBJECTS=devfile.o usb.o usberrors.o fifo.o
LIBFLAGS=-fno-strict-aliasing -lusb-1.0
FLAGS= -Wall -O3 -g -fno-strict-aliasing
HFILES=cuse.h usbpiper.h

all:    $(ALL)

clean:
	rm -f *.o $(ALL)
	rm -f `find . -name "*~"`

%.o:    %.c $(HFILES)
	$(CC) -c $(FLAGS) -o $@ $<

$(ALL) : %: %.o Makefile $(OBJECTS)
	$(CC) $< $(OBJECTS) -o $@ $(LIBFLAGS)
