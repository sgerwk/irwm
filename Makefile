PROGS=irwm
MANS=irwm.1

all: $(PROGS)

install: all
	mkdir -p $(DESTDIR)/usr/bin
	cp $(PROGS) $(DESTDIR)/usr/bin
	mkdir -p $(DESTDIR)/usr/share/man/man1
	cp $(MANS) $(DESTDIR)/usr/share/man/man1

CFLAGS=-Wall -Wextra

CFLAGS+=-I/usr/X11R6/include
LDFLAGS+=-L/usr/X11R6/lib
LDLIBS+=-lX11

irwm: CFLAGS+=-DLIRC
irwm: CFLAGS+=-DXFT -I/usr/include/freetype2
irwm: LDLIBS+=-llirc_client
irwm: LDLIBS+=-lXft

clean:
	rm -f $(PROGS) *~ irwm.log

