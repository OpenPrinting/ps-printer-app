#
# Makefile for the PostScript Printer Application
#
# Copyright © 2020 by Till Kamppeter
# Copyright © 2020 by Michael R Sweet
#
# Licensed under Apache License v2.0.  See the file "LICENSE" for more
# information.
#

# Version and
VERSION		=	1.0
prefix		=	$(DESTDIR)/usr
includedir	=	$(prefix)/include
bindir		=	$(prefix)/bin
libdir		=	$(prefix)/lib
mandir		=	$(prefix)/share/man
ppddir		=	$(prefix)/share/ppd
unitdir 	:=	`pkg-config --variable=systemdsystemunitdir systemd`


# Compiler/linker options...
OPTIM		=	-Os -g
CFLAGS		+=	`pkg-config --cflags pappl` `pkg-config --cflags libppd` `pkg-config --cflags libcupsfilters` $(OPTIM)
LDFLAGS		+=	$(OPTIM)
LIBS		+=	`pkg-config --libs pappl` `pkg-config --libs libppd` `pkg-config --libs libcupsfilters`


# Targets...
OBJS		=	ps-printer-app.o
TARGETS		=	ps-printer-app


# General build rules...
.SUFFIXES:	.c .o
.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<


# Targets...
all:		$(TARGETS)

clean:
	rm -f $(TARGETS) $(OBJS)

install:	$(TARGETS)
	mkdir -p $(bindir)
	cp $(TARGETS) $(bindir)
	mkdir -p $(mandir)/man1
	cp ps-printer-app.1 $(mandir)/man1
	mkdir -p $(ppddir)
	cp generic-ps-printer.ppd $(ppddir)
	if test "x$(unitdir)" != x; then \
	mkdir -p $(unitdir); \
	cp ps-printer-app.service $(unitdir); \
	fi

ps-printer-app:	$(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(OBJS):	Makefile
