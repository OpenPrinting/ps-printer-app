# PostScript Printer Application

## INTRODUCTION

This repository contains a Printer Application for PostScript printers
that uses [PAPPL](https://www.msweet.org/pappl) to support IPP
printing from multiple operating systems. In addition, it uses the resources
of [cups-filters 2.x](https://github.com/OpenPrinting/cups-filters) (filter
functions in libcupsfilters, libppd). This work is derived from the
[hp-printer-app](https://github.com/michaelrsweet/hp-printer-app).

Your contributions are welcome. Please post [issues and pull
requests](https://github.com/OpenPrinting/ps-printer-app).


### This Printer Application is a working model for

- A non-raster Printer Application: Destination format is PostScript,
  a high-level/vector format. Input data in PostScript or PDF is
  accepted and needed conversion is done without any inbetween raster
  steps.

- A Printer Application which uses the new filter functions of
  cups-filters 2.x. Filter functions are library functions derived
  from the CUPS filters and contain decades of development and
  refinement starting from the introduction of CUPS in 2000.

- A retro-fit Printer Application for classic CUPS drivers, in this
  case the simplest form of only PPD files for PostScript printers. It
  lists PPD files from repositories included in the Snap, loads the
  PPD needed for the actual printer, extracts options from the PPD to
  display them in the web interface, accepts job settings as IPP
  attributes, and inserts the PostScript code provided by the PPD
  correctly into the output data stream.

- A Printer Application which does not pass through raw (input format
  is printer's native format) jobs. To assure that always the
  PostScript code of the PPD file is inserted into the output stream,
  we call the printer's native format
  "application/vnd.printer-specific" which does not exist as input
  format, so "application/postscript" input is forced through the
  pstops() filter function.

- An expandable Printer Application: The user can add PPD files via the
  administration web interface to support additional printer models.

Further properties are:

- To do not need to re-invent the code for forking into sub-processes
  so that we can pass data through a sequence of filters, we create a
  filter function to send the data off to the printer and form a chain
  of the actually converting filter function (one of pstops() and
  pdftops()) with this filter function using the filterChain() filter
  function.

- For PWG/Apple Raster input we use raster callbacks so that the
  processing is streaming, allowing for large and even infinitely long
  jobs. We use libppd functions to insert the PPD option's PostScript
  code in the output stream and the filterPOpen() function to create a
  file descriptor for the libppd functions to send data off to the
  device.

- The PostScript Printer Application Snap has all PostScript PPD files
  of the [foomatic-db](https://github.com/OpenPrinting/foomatic-db)
  and [HPLIP](https://developers.hp.com/hp-linux-imaging-and-printing)
  projects built-in, so most PostScript printer PPDs which usually
  come with Linux Distributions. To avoid that this vast number of
  PPDs blows up the size of the Snap, we highly compress them using
  [pyppd](https://github.com/OpenPrinting/pyppd). Note that some PPDs
  use certain CUPS filters for extra functionality. These filters are
  not included, so only the basic functionality is supported then (in
  most cases only PIN-protected printing gets lost by this). The user
  can add additional PPDs without needing to rebuild the Snap (see
  below).

- We use the printer's IEEE-1284 device ID to identify at first that
  it is a PostScript printer (via CMD: field) to see whether it is
  supported at all and only then check via make and model whether we
  explicitly support it with a PPD. PostScript printers for which
  there is no PPD get a generic PPD assigned. By the check of the CMD:
  field before make/model lookup we assure that if PostScript is
  provided by an add-on module that the module is actually installed.

- The printer capabilities for a given printer model (a "driver" in
  the Printer Application) are not static throughout the life of the
  print queue set up in the Printer Application. The user can
  configure via a page in the web admin interface which hardware
  accessories (extra paper trays, duplex unit, finishers, ...) are
  installed on the printer and the Printer Application updates the
  driver data structure and with this the printer capabilities. The
  response to a get-printer-attributes IPP request gets updated
  appropriately.

- PostScript is a full-fledged programming language and many PostScript
  printers allow querying settings of options and the presence of
  installable hardware accessories executing appropriate PostScript
  code. If a setting can get queried, the manufacturer puts the needed
  PostScript code into the PPD file, together with the queriable option.
  These queries are supported by the web interface of the Printer
  Application.


### Remark

- This Printer Application and its snapping is derived from the
  hp-printer-app, which uses the "avahi-observe" Snap interface to
  access DNS-SD. This does not work for registering printers. In this
  Printer Application this is already corrected to "avahi-control".


### To Do

- On the "Add PPD files" page in the list of already added user PPD
  files mark which ones are actually used by a printer which got set
  up in the Printer Application, to avoid that the user removes these
  files.

- Human-readable strings for vendor options (Needs support by PAPPL:
  [Issue #58: Localization
  support](https://github.com/michaelrsweet/pappl/issues/58))

- Internationalization/Localization (Needs support by PAPPL: [Issue
  #58: Localization
  support](https://github.com/michaelrsweet/pappl/issues/58))

- SNMP Ink level check via ps_status() function (Needs support by PAPPL:
  [Issue #83: CUPS does IPP and SNMP ink level polls via backends,
  PAPPL should have functions for
  this](https://github.com/michaelrsweet/pappl/issues/83))

- In `ps-printer-app.c` some places are marked with `TODO`. These are
  points to be improved or where functionality in PAPPL is still
  needed.

- Build options for cups-filters, to build without libqpdf and/or
  without libppd, the former will allow to create the Snap of this
  Printer Application without downloading and building QPDF

- Better way to download HPLIP for grabbing the PostScript PPD files
  for HP printers


## THE SNAP

### Installing and building

To just run and use this Printer Application, simply install it from
the Snap Store:

```
sudo snap install --edge ps-printer-app
```

Then follow the instructions below for setting it up.

To build the Snap by yourself, in the main directory of this
repository run

```
snapcraft snap
```

This will download all needed packages and build the PostScript
Printer Application. Note that PAPPL (upcoming 1.0) and cups-filters
(upcoming 2.0) are pulled directly from their GIT repositories, as
there are no appropriate releases yet. This can also lead to the fact
that this Printer Application will suddenly not build any more.

NOTE: There is a bug in Ubuntu Groovy (20.10) that prevents it from
building Snaps, see [this discussion on the Snapcraft
forum](https://forum.snapcraft.io/t/build-fails-with-a-mksquashfs-error-cannot-pack-root-prime/). The
problem is already solved but did not make it into Groovy yet.

Any older (like 20.04) or newer (like 21.04) Ubuntu version should work.

To install the resulting Snap run

```
sudo snap install --dangerous ps-printer-app_1.0_amd64.snap
```


### Setting up

The Printer Application will automatically be started as a server daemon.

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

or print with CUPS, CUPS (and also cups-browsed) discover and treat
the printers set up with this Printer Application as driverless IPP
printers (IPP Everywhere and AirPrint).

You can also add PPD files without rebuilding the Snap, either by
using the "Add PPD files" button in the web interface or by manually
copying PPD files:

```
sudo cp PPDFILE /var/snap/ps-printer-app/common/ppd/
```

After manually copying (or removing) PPD files you need to restart the
server or in the web interface, on the "Add PPD files" page click the
"Refresh" button at the bottom. This adds the changes to the internal
driver list.

On the "Add Printer" page in the drop-down to select the driver,
user-added PPD files are marked "USER-ADDED". When setting up a
printer with automatic driver selection, user-added PPD files are
preferred.

`PPDFILE` in the command line above cannot only be a single PPD file
but any number of single PPD files, `.tar.gz` files containing PPDs
(in arbitrary directory structure) and PPD-gemerating executables
which are usually put into `/usr/lib/cups/driver`. You can also create
arbitrary sub-directory structures in
`/var/snap/ps-printer-app/current/ppd/` containing the mentioned types
of files. Only make sure to not put any executables there which do
anything else than listing and generating PPD files.

Note that with the web interface you can only manage individual PPDs
(uncompressed or compressed with `gzip`) in the
`/var/snap/ps-printer-app/current/ppd/` itself. Archives, executables,
or sub-directories are not shown and appropriate uploads not
accepted. This especially prevents adding executables without root
rights.

Any added PPD file must be for PostScript printers, as non-PostScript
PPD files are for CUPS drivers and so they would need additional files
in order to work and such files are not supported by this Printer
Application. The "Add PPD files" page shows warnings if such files get
uploaded.

See

```
ps-printer-app --help
```

for more options.

Use the "--debug" argument for verbose logging in your terminal window.


## BUILDING WITHOUT SNAP

You can also do a "quick-and-dirty" build without snapping and without
needing to install PAPPL and cups-filters 2.x into your system. You
need a directory with the latest GIT snapshot of PAPPL and the latest
GIT snapshot of cups-filters (master branch). Both PAPPL and
cups-filters need to be compiled (./cnfigure; make), installing not
needed. Also install the header files of all needed libraries
(installing "libcups2-dev" should do it).

In the directory with the attached ps-printer-app.c run the command
line

```
gcc -o ps-printer-app ps-printer-app.c $PAPPL_SRC/pappl/libpappl.a $CUPS_FILTERS_SRC/.libs/libppd.a $CUPS_FILTERS_SRC/.libs/libcupsfilters.a -ldl -lpthread  -lppd -lcups -lavahi-common -lavahi-client -lgnutls -ljpeg -lpng16 -ltiff -lz -lm -lusb-1.0 -lpam -lqpdf -lstdc++ -I. -I$PAPPL_SRC/pappl -I$CUPS_FILTERS_SRC/ppd -I$CUPS_FILTERS_SRC/cupsfilters -L$CUPS_FILTERS_SRC/.libs/
```

There is also a Makefile, but this needs PAPPL and cups-filters 2.x to be installed into your system.

Run

```
./ps-printer-app --help
```

When running the non-snapped version, by default, PPD files are
searched for in

```
/usr/share/ppd/
/usr/lib/cups/driver/
/var/lib/ps-printer-app/ppd/
```

The last path is used when adding PPD files using the "Add PPD files"
page in the web interface.

You can set the `PPD_PATHS` environment variable to search other
places instead:

```
PPD_PATHS=/path/to/my/ppds:/my/second/place ./ps-printer-app server
```

Simply put a colon-separated list of any amount of paths into the
variable, always the last being used by the "Add PPD files"
page. Creating a wrapper script is recommended.

For access to the test page `testpage.ps` use the TESTPAGE_DIR
environment variable:

```
TESTPAGE_DIR=`pwd` PPD_PATHS=/path/to/my/ppds:/my/second/place ./ps-printer-app server
```

or for your own creation of a test page (PostScript, PDF, PNG, JPEG,
Apple Raster, PWG Raster):

```
TESTPAGE=/path/to/my/testpage/my_testpage.ps PPD_PATHS=/path/to/my/ppds:/my/second/place ./ps-printer-app server
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
