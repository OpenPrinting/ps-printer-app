#
# Makefile for the PostScript Printer Application
#
# Copyright © 2020-2021 by Till Kamppeter
# Copyright © 2020 by Michael R Sweet
#
# Licensed under Apache License v2.0.  See the file "LICENSE" for more
# information.
#

# Version and
VERSION		=	1.0
prefix		=	$(DESTDIR)/usr
localstatedir	=	$(DESTDIR)/var
includedir	=	$(prefix)/include
bindir		=	$(prefix)/bin
libdir		=	$(prefix)/lib
mandir		=	$(prefix)/share/man
ppddir		=	$(prefix)/share/ppd
statedir	=	$(localstatedir)/lib/ps-printer-app
spooldir	=	$(localstatedir)/spool/ps-printer-app
serverbin	=	$(prefix)/lib/ps-printer-app
resourcedir	=	$(prefix)/share/ps-printer-app
cupsserverbin	=	`cups-config  --serverbin`
unitdir 	:=	$(DESTDIR)`pkg-config --variable=systemdsystemunitdir systemd`


# Compiler/linker options...
OPTIM		=	-Os -g
CFLAGS		+=	`pkg-config --cflags pappl` `cups-config --cflags` `pkg-config --cflags libppd` `pkg-config --cflags libcupsfilters` `pkg-config --cflags libpappl-retrofit` $(OPTIM)
ifdef VERSION
CFLAGS		+=	-DSYSTEM_VERSION_STR="\"$(VERSION)\""
ifndef MAJOR
MAJOR		=	`echo $(VERSION) | perl -p -e 's/^(\d+).*$$/\1/'`
endif
ifndef MINOR
MINOR		=	`echo $(VERSION) | perl -p -e 's/^\d+\D+(\d+).*$$/\1/'`
endif
ifndef PATCH
PATCH		=	`echo $(VERSION) | perl -p -e 's/^\d+\D+\d+\D+(\d+).*$$/\1/'`
endif
ifndef PACKAGE
PACKAGE		=	`echo $(VERSION) | perl -p -e 's/^\d+\D+\d+\D+\d+\D+(\d+).*$$/\1/'`
endif
endif
ifdef MAJOR
CFLAGS		+=	-DSYSTEM_VERSION_ARR_0=$(MAJOR)
endif
ifdef MINOR
CFLAGS		+=	-DSYSTEM_VERSION_ARR_1=$(MINOR)
endif
ifdef PATCH
CFLAGS		+=	-DSYSTEM_VERSION_ARR_2=$(PATCH)
endif
ifdef PACKAGE
CFLAGS		+=	-DSYSTEM_VERSION_ARR_3=$(PACKAGE)
endif
LDFLAGS		+=	$(OPTIM) `cups-config --ldflags`
LIBS		+=	`pkg-config --libs pappl` `cups-config --image --libs` `pkg-config --libs libppd` `pkg-config --libs libcupsfilters` `pkg-config --libs libpappl-retrofit`


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
	mkdir -p $(statedir)/ppd
	mkdir -p $(spooldir)
	mkdir -p $(resourcedir)
	cp testpage.ps $(resourcedir)
	if test "x$(cupsserverbin)" != x; then \
	  mkdir -p $(libdir); \
	  touch $(serverbin) 2> /dev/null || :; \
	  if rm $(serverbin) 2> /dev/null; then \
	    ln -s $(cupsserverbin) $(serverbin); \
	  fi; \
	else \
	  mkdir -p $(serverbin)/filter; \
	  mkdir -p $(serverbin)/backend; \
	fi
	if test "x$(unitdir)" != x; then \
	  mkdir -p $(unitdir); \
	  cp ps-printer-app.service $(unitdir); \
	fi

ps-printer-app:	$(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(OBJS):	Makefile
