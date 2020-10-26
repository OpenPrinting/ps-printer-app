# PostScript Printer Application

This repository contains a Printer Application for PostScript printers
that uses [PAPPL](https://www.msweet.org/pappl) to support IPP
printing from multiple operating systems. In addition, it uses the resources
of [cups-filters 2.x](https://github.com/OpenPrinting/cups-filters) (filter
functions in libcupsfilters, libppd). This work is derived from the
[hp-printer-app](https://github.com/michaelrsweet/hp-printer-app).

This Printer Application is a working model for

- A non-raster Printer Application: Destination format is PostScript,
  a high-level/vector format. Input data in PostScript or PDF is
  accepted and needed conversion is not done through an inbetween
  raster step.

- A Printer Application which uses the new filter functions of
  cups-filters 2.x. Filter functions are library functions derived
  from cups-filters and contain decades of development and refinement
  starting from the introduction of CUPS in 2000.

- A retro-fit Printer Application for classic CUPS drivers, in this
  case the simplest form of only PPD files for PostScript printers. It
  lists PPD files from repositories included in the Snap, loads the
  PPD needed for the actual printer, extracts options from the PPD to
  display them in the web interface, accepts job settings as IPP
  attributes, and inserts the PostScript code provided by the PPD
  correctly into the output data stream.

- A Printer Application which does not pass through raw (input format
  is printer's native format) jobs. To assure that always teh
  PostScript code of the PPD file is inserted into the output stream,
  we call the printer's native format
  "application/vnd.printer-specific" which does not exist as input
  format, so "application/postscript" input is forced through the
  pstops() filter function.

- To do not need to re-invent the code for forking so that we can pass
  data through a sequence of filters, we create a filter function to
  send the data off to the printer and form a chain of the actually
  converting filter function (one of pstops(), pdftops(), imagetops(),
  rastertops()) and the with this filter function using the
  filterChain() filter function.


## KNOWN ISSUES

- Only auto-detected USB printers work, if selecting a discovered
  nework printer you can set it up and configure its default settings
  bug jobs do not get printed. So select USB printers for now. This
  seems to be a problem of PAPPL

- This Printer Application and its snapping is derived from the
  hp-printer-app, which uses the "avahi-observe" Snap interface to
  access DNS-SD. This does not work for registering printers. In this
  Printer Application this is already corrected to "avahi-control".


## TODO:

- Automatic start of the Printer Application as a daemon

- Add printer auto-setup for all supported printers

- Add driver named "auto" which selects the PPD automatically based on
  device ID.

- Correct locations for the state file and a log file

- Access to PPD options not representable by IPP via the web
  interface, so that they can be set as printer default at least.

- Add PPD files via web interface.

- If in a future version the PPD repository changed and an installed
  printer does not find its PPD any more on startup, let it fall back
  to the "auto" driver so that it finds a new, suitable PPD.

- Build options for cups-filters, to build withour libqpdf and/or
  withour libppd, the former will allow to create the Snap of this
  Printer Application without downloading and building QPDF

- Better way to download HPLIP for grabbing the PostScript PPD files
  for HP printers.


## BUILDING AND INSTALLING

NOTE: There is a bug in Ubuntu Groovy (20.10) that prevents it from
building Snaps, see

https://forum.snapcraft.io/t/build-fails-with-a-mksquashfs-error-cannot-pack-root-prime/

Any older Ubuntu version (like 20.04) should work.

In the main directory of this repository run

```
snapcraft snap
```

This will download all needed packages and build the PostScript
Printer Application. Note that PAPPL (upcoming 1.0) and cups-filters
(upcoming 2.0) are pulled directly from their GIT repositories, as
there are no appropriate releases yet. This can also lead to the fact
that this Printer Application will suddenly not build any more.

To install the resulting Snap run

```
sudo snap install --dangerous ps-printer-app_1.0_amd64.snap
```

Then connect the interfaces:

```
sudo snap connect ps-printer-app:avahi-control
sudo snap connect ps-printer-app:log-observe
sudo snap connect ps-printer-app:network-manager
sudo snap connect ps-printer-app:raw-usb
```

Now fire up the Printer Application as a server daemon:

```
ps-printer-app server
```

Enter the web interface

```
http://localhost:8000/
```

Use the web interface to add a printer. Supply a name, select the
discovered printer, then select make and model. Also set the loaded
media and the option defaults.

Then print PDF, PostScript, JPEG, Apple Raster, or PWG Raster files
with

```
ps-printer-app FILE
```

See

```
ps-printer-app --help
```

for more options, use the "--debug" info for verbose logging in your
terminal window.

You can also do a "quick-and-dirty" build wothout snapping. You need a directory with the latest GIT snapshot of PAPPL and the latest GIT snapshot of cups-filters (master branch). Both PAPPL and cups-filters need to be compiled (./cnfigure; make), installing not needed. Also install the header files of all needed libraries (installing "libcups2-dev" should do it).

In the directory with the attached ps-printer-app.c run the command line

```
gcc -o ps-printer-app ps-printer-app.c $PAPPL_SRC/pappl/libpappl.a $CUPS_FILTERS_SRC/.libs/libppd.a $CUPS_FILTERS_SRC/.libs/libcupsfilters.a -ldl -lpthread  -lppd -lcups -lavahi-common -lavahi-client -lgnutls -ljpeg -lpng16 -ltiff -lz -lm -lusb-1.0 -lpam -lqpdf -lstdc++ -I. -I$PAPPL_SRC/pappl -I$CUPS_FILTERS_SRC/ppd -I$CUPS_FILTERS_SRC/cupsfilters
```

The run

```
./ps-printer-app --help
```


## LEGAL STUFF

The PostScript Printer Application is Copyright © 2020 by Till Kamppeter.

It is derived from the HP PCL Printer Application, a first working model of
a raster Printer Application using PAPPL. It is available here:

https://github.com/michaelrsweet/hp-printer-app

The HP PCL Printer Application is Copyright © 2019-2020 by Michael R Sweet.

This software is licensed under the Apache License Version 2.0 with an exception
to allow linking against GPL2/LGPL2 software (like older versions of CUPS).  See
the files "LICENSE" and "NOTICE" for more information.
