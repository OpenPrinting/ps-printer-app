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

#include <base.h>


//
// Constants...
//

// Name and version

#define SYSTEM_NAME "PostScript Printer Application"
#define SYSTEM_PACKAGE_NAME "ps-printer-app"
#define SYSTEM_VERSION_STR "1.0"
#define SYSTEM_VERSION_ARR_0 1
#define SYSTEM_VERSION_ARR_1 0
#define SYSTEM_VERSION_ARR_2 0
#define SYSTEM_VERSION_ARR_3 0
#define SYSTEM_WEB_IF_FOOTER "Copyright &copy; 2020 by Till Kamppeter. Provided under the terms of the <a href=\"https://www.apache.org/licenses/LICENSE-2.0\">Apache License 2.0</a>."

// Test page

#define TESTPAGE "testpage.ps"


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
  if (pr_supports_postscript(device_id))
    // Printer supports PostScript, so find the best-matching PPD file
    ret = pr_best_matching_ppd(device_id, global_data);
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
  cupsArrayAdd(spooling_conversions, &pr_convert_ps_to_ps);
  cupsArrayAdd(spooling_conversions, &pr_convert_pdf_to_ps);

  // Array of stream formats, most desirables first
  stream_formats = cupsArrayNew(NULL, NULL);
  cupsArrayAdd(stream_formats, &pr_stream_postscript);

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
    PR_COPTIONS_QUERY_PS_DEFAULTS | PR_COPTIONS_WEB_ADD_PPDS,
    ps_autoadd,
    pr_identify,
    pr_testpage,
    spooling_conversions,
    stream_formats,
    "",
    "",
    TESTPAGE
  };

  return (pr_retrofit_printer_app(&printer_app_config, argc, argv));
}
