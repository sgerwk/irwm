PROGS=irwm

all: $(PROGS)

install: all
	mkdir -p $(DESTDIR)/usr/bin
	cp $(PROGS) $(DESTDIR)/usr/bin

CFLAGS=-Wall -Wextra

CFLAGS+=-I/usr/X11R6/include
LDFLAGS+=-L/usr/X11R6/lib
LDLIBS+=-lX11

irwm: CFLAGS+=-DLIRC
irwm: CFLAGS+=-DXFT -I/usr/include/freetype2
irwm: LDLIBS+=-llirc_client
irwm: LDLIBS+=-lXft

clean:
	rm -f $(PROGS) *~

