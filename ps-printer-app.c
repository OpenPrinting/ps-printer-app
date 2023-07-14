//
// PostScript Printer Application based on PAPPL and libpappl-retrofit
//
// Copyright © 2020 by Till Kamppeter.
// Copyright © 2020 by Michael R Sweet.
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

//
// Include necessary headers...
//

#include <pappl-retrofit.h>


//
// Constants...
//

// Name and version

#define SYSTEM_NAME "PostScript Printer Application"
#define SYSTEM_PACKAGE_NAME "ps-printer-app"
#ifndef SYSTEM_VERSION_STR
#  define SYSTEM_VERSION_STR "1.0"
#endif
#ifndef SYSTEM_VERSION_ARR_0
#  define SYSTEM_VERSION_ARR_0 1
#endif
#ifndef SYSTEM_VERSION_ARR_1
#  define SYSTEM_VERSION_ARR_1 0
#endif
#ifndef SYSTEM_VERSION_ARR_2
#  define SYSTEM_VERSION_ARR_2 0
#endif
#ifndef SYSTEM_VERSION_ARR_3
#  define SYSTEM_VERSION_ARR_3 0
#endif
#define SYSTEM_WEB_IF_FOOTER "Copyright &copy; 2020 by Till Kamppeter. Provided under the terms of the <a href=\"https://www.apache.org/licenses/LICENSE-2.0\">Apache License 2.0</a>."

// Test page

#define TESTPAGE "testpage.pdf"


//
// 'ps_autoadd()' - Auto-add PostScript printers.
//

const char *			// O - Driver name or `NULL` for none
ps_autoadd(const char *device_info,	// I - Device name (unused)
	   const char *device_uri,	// I - Device URI (unused)
	   const char *device_id,	// I - IEEE-1284 device ID
	   void       *data)            // I - Global data
{
  pr_printer_app_global_data_t *global_data =
    (pr_printer_app_global_data_t *)data;
  const char	*ret = NULL;		// Return value
  char          *p;


  (void)device_info;
  (void)device_uri;

  if (device_id == NULL || global_data == NULL)
    return (NULL);

  // Look at the COMMAND SET (CMD) key for the list of printer languages...
  //
  // There are several printers for which PostScript is available in an
  // add-on module, so there are printers with the same model name but
  // with and without PostScript support. So we auto-add printers only
  // if their device ID explicitly tells that they do PostScript
  // We only make an exception on device IDs which do not inform about
  // the printer's PDLs at all (no "CMD:" or "COMMAND SET:" fields)
  // allowing them always. This is because some backends do not
  // obtain the device ID from the printer but obtain make and model
  // by another method and make an artificial device ID from them.
  if ((strncmp(device_id, "CMD:", 4) &&
       strncmp(device_id, "COMMAND SET:", 12) &&
       strstr(device_id, ";CMD:") == NULL &&
       strstr(device_id, ";COMMAND SET:") == NULL) ||
      prSupportsPostScript(device_id))
  {
    // Printer supports PostScript, so find the best-matching PPD file
    ret = prBestMatchingPPD(device_id, global_data);
    if (strcmp(ret, "generic") == 0 && !prSupportsPostScript(device_id))
      ret = NULL;
  }
  else
    // Printer does not support PostScript, it is not supported by this
    // Printer Application
    ret = NULL;

  return (ret);
}


//
// 'main()' - Main entry for the ps-printer-app.
//

int
main(int  argc,				// I - Number of command-line arguments
     char *argv[])			// I - Command-line arguments
{
  cups_array_t *spooling_conversions,
               *stream_formats;
  int          ret;

  // Array of spooling conversions, most desirables first
  spooling_conversions = cupsArrayNew(NULL, NULL);
  cupsArrayAdd(spooling_conversions, (void *)&PR_CONVERT_PS_TO_PS);
  cupsArrayAdd(spooling_conversions, (void *)&PR_CONVERT_PDF_TO_PS);

  // Array of stream formats, most desirables first
  stream_formats = cupsArrayNew(NULL, NULL);
  cupsArrayAdd(stream_formats, (void *)&PR_STREAM_POSTSCRIPT);

  // Configuration record of the PostScript Printer Application
  pr_printer_app_config_t printer_app_config =
  {
    SYSTEM_NAME,
    SYSTEM_PACKAGE_NAME,
    SYSTEM_VERSION_STR,
    {
      SYSTEM_VERSION_ARR_0,
      SYSTEM_VERSION_ARR_1,
      SYSTEM_VERSION_ARR_2,
      SYSTEM_VERSION_ARR_3
    },
    SYSTEM_WEB_IF_FOOTER,
    PR_COPTIONS_QUERY_PS_DEFAULTS |
    PR_COPTIONS_WEB_ADD_PPDS |
    PR_COPTIONS_NO_PAPPL_BACKENDS |
    PR_COPTIONS_CUPS_BACKENDS,
    ps_autoadd,
    prIdentify,
    prTestPage,
    prSetupAddPPDFilesPage,
    prSetupDeviceSettingsPage,
    spooling_conversions,
    stream_formats,
    "",
    "snmp,dnssd,usb",
    TESTPAGE,
    NULL,
    NULL
  };

  return (prRetroFitPrinterApp(&printer_app_config, argc, argv));
}
