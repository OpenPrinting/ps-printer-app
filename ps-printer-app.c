//
// PostScript Printer Application for the Printer Application Framework
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

#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <pappl/pappl.h>
#include <ppd/ppd.h>
#include <cupsfilters/log.h>
#include <cupsfilters/filter.h>
#include <cupsfilters/ieee1284.h>
#include <cups/cups.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <malloc.h>


//
// Types...
//

typedef struct ps_ppd_path_s		// Driver-name/PPD-path pair 
{
  const char *driver_name;              // Driver name
  const char *ppd_path;	                // PPD path in collections
} ps_ppd_path_t;

typedef struct ps_filter_data_s		// Filter data
{
  filter_function_t filter_function;	// Filter function to use
  void		    *filter_parameters;	// Filter parameters
} ps_filter_data_t;

typedef struct ps_job_data_s		// Job data
{
  ppd_file_t            *ppd;           // PPD file loaded from collection
  int		        num_options;    // Number of PPD print options
  cups_option_t	        *options;       // PPD print options
  int                   device_fd;      // File descriptor to pipe output
                                        // to the device
  int                   device_pid;     // Process ID for device output
                                        // sub-process
  FILE                  *device_file;   // File pointer for output to
                                        // device
  int                   line_count;     // Raster lines actually received for
                                        // this page
} ps_job_data_t;


//
// Local globals...
//

// Name and version

#define SYSTEM_NAME "PostScript Printer Application"
#define SYSTEM_VERSION_STR "1.0"
#define SYSTEM_VERSION_ARR_0 1
#define SYSTEM_VERSION_ARR_1 0
#define SYSTEM_VERSION_ARR_2 0
#define SYSTEM_VERSION_ARR_3 0


// PPD collections used as drivers

static const char * const col_paths[] =   // PPD collection dirs
{
 "/usr/lib/cups/driver",
 "/usr/share/ppd"
};

static  int               num_drivers = 0; // Number of drivers (from the PPDs)
static  pappl_pr_driver_t *drivers = NULL; // Driver index (for menu and
                                           // auto-add)


//
// Local functions...
//

static const char *ps_autoadd(const char *device_info, const char *device_uri,
			      const char *device_id, void *data);
static void   ps_ascii85(FILE *outputfp, const unsigned char *data, int length,
			 int last_data);
static bool   ps_callback(pappl_system_t *system, const char *driver_name,
			  const char *device_uri, const char *device_id,
			  pappl_pr_driver_data_t *driver_data,
			  ipp_t **driver_attrs, void *data);
static int    ps_compare_ppd_paths(void *a, void *b, void *data); 
bool          ps_filter(pappl_job_t *job, pappl_device_t *device, void *data);
static void   ps_free_job_data(ps_job_data_t *job_data);
static bool   ps_have_force_gray(ppd_file_t *ppd,
				 const char **optstr, const char **choicestr);
static void   ps_identify(pappl_printer_t *printer,
			  pappl_identify_actions_t actions,
			  const char *message);
static void   ps_job_log(void *data, filter_loglevel_t level,
			 const char *message, ...);
static ps_job_data_t *ps_create_job_data(pappl_job_t *job,
					 pappl_pr_options_t *job_options);
static void   ps_media_col(pwg_size_t *pwg_size, const char *def_source,
			   const char *def_type, int left_offset,
			   int top_ofsset, pappl_media_tracking_t tracking,
			   pappl_media_col_t *col);
static void   ps_one_bit_dither_on_draft(pappl_job_t *job,
					 pappl_pr_options_t *options);
int           ps_print_filter_function(int inputfd, int outputfd,
				       int inputseekable, int *jobcanceled,
				       filter_data_t *data, void *parameters);
static bool   ps_rendjob(pappl_job_t *job, pappl_pr_options_t *options,
			 pappl_device_t *device);
static bool   ps_rendpage(pappl_job_t *job, pappl_pr_options_t *options,
			  pappl_device_t *device, unsigned page);
static bool   ps_rstartjob(pappl_job_t *job, pappl_pr_options_t *options,
			   pappl_device_t *device);
static bool   ps_rstartpage(pappl_job_t *job, pappl_pr_options_t *options,
			    pappl_device_t *device, unsigned page);
static bool   ps_rwriteline(pappl_job_t *job, pappl_pr_options_t *options,
			    pappl_device_t *device, unsigned y,
			    const unsigned char *pixels);
static void   ps_setup(pappl_system_t *system);
static bool   ps_status(pappl_printer_t *printer);
static pappl_system_t   *system_cb(int num_options, cups_option_t *options,
				   void *data);


//
// 'main()' - Main entry for the ps-printer-app.
//

int
main(int  argc,				// I - Number of command-line arguments
     char *argv[])			// I - Command-line arguments
{
  return (papplMainloop(argc, argv,     // Command line arguments
			"1.0",          // Version number
			NULL,           // HTML Footer for web interface
			0,              // Number of drivers for built-in setup
			NULL,           // Driver list for built-in setup
			NULL,           // Setup callback for selected driver
			ps_autoadd,     // Printer auto-addition callback
			NULL,           // Sub-command name
			NULL,           // Callback for sub-command
			system_cb,      // System creation callback
			NULL,           // Usage info output callback
			NULL));         // Data
}


//
// 'ps_ascii85()' - Print binary data as a series of base-85 numbers.
//                  4 binary bytes are encoded into 5 printable
//                  characters. If the supplied data cannot be divided
//                  into groups of 4, the remaining 1, 2, or 3 bytes
//                  will be held by the function and on the next call
//                  the data will get preceded by these bytes. This
//                  way the data to be encoded can be supplied in
//                  arbitrary portions. On the last call the last_data
//                  bit has to be set to also encode a remainder of
//                  less than 4 bytes. A held remainder can get flushed
//                  out without needing to supply further data by calling
//                  with data set to NULL, length to 0 and last_data to 1.
//

static void
ps_ascii85(FILE                *outputfp,
	   const unsigned char *data,		// I - Data to encode
	   int                 length,		// I - Number of bytes to encode
	   int                 last_data)	// I - Last portion of data?
{
  unsigned              i;
  unsigned	        b;			// Binary data word */
  unsigned char	        c[5];			// ASCII85 encoded chars */
  static int	        col = 0;		// Current column */
  static unsigned char  remaining[3];           // Remaining bytes which do
                                                // not complete 4 to be encoded
                                                // Keep them for next call
  static unsigned int   num_remaining = 0;


  while (num_remaining + length > 0)
  {
    if (num_remaining > 0 || length < 4)
    {
      for (b = 0, i = 0; i < 4; i ++)
      {
	b << 8;
	if (i < num_remaining)
	  b |= remaining[i];
	else if (i - num_remaining < length) 
	  b |= data[i - num_remaining];
	else if (!last_data)
	{
	  if (length)
	    memcpy(remaining + num_remaining, data, length);
	  num_remaining = i;
	  return;
	}
      }
      i = 4 - num_remaining;
      if (length < i)
	i = length;
      num_remaining = 0;
    }
    else
    {
      b = (((((data[0] << 8) | data[1]) << 8) | data[2]) << 8) | data[3];
      i = 4;
    }

    if (b == 0)
    {
      fputc('z', outputfp);
      col ++;
    }
    else
    {
      c[4] = (b % 85) + '!';
      b /= 85;
      c[3] = (b % 85) + '!';
      b /= 85;
      c[2] = (b % 85) + '!';
      b /= 85;
      c[1] = (b % 85) + '!';
      b /= 85;
      c[0] = b + '!';

      fwrite(c, 5, 1, outputfp);
      col += 5;
    }

    if (data)
      data += i;
    length -= i;

    if (col >= 75)
    {
      fputc('\n', outputfp);
      col = 0;
    }
  }

  if (last_data)
  {
    fputs("~>\n", outputfp);
    col = 0;
    num_remaining = 0;
  }
}


//
// 'ps_autoadd()' - Auto-add PostScript printers.
//

static const char *			// O - Driver name or `NULL` for none
ps_autoadd(const char *device_info,	// I - Device name (unused)
	   const char *device_uri,	// I - Device URI (unused)
	   const char *device_id,	// I - IEEE-1284 device ID
	   void       *data)		// I - Callback data (not used)
{
  int           i;
  const char	*ret = NULL;		// Return value
  int		num_did;		// Number of device ID key/value pairs
  cups_option_t	*did = NULL;		// Device ID key/value pairs
  int		num_ddid;		// Device ID of driver list entry
  cups_option_t	*ddid = NULL;		// Device ID of driver list entry
  const char	*cmd,			// Command set value
                *mfg, *dmfg, *mdl, *dmdl, // Device ID fields
		*ps;			// PostScript command set pointer
  char          buf[1024];


  (void)device_info;
  (void)device_uri;
  (void)data;

  if (device_id == NULL)
    return (NULL);

  // Parse the IEEE-1284 device ID to see if this is a printer we support...
  num_did = papplDeviceParseID(device_id, &did);
  if (num_did == 0 || did == NULL)
    return (NULL);

  // Look at the COMMAND SET (CMD) key for the list of printer languages...
  //
  // There are several printers for which PostScript is available in an
  // add-on module, so there are printers with the same model name but
  // with and without PostScript support. So we auto-add printers only
  // if their device ID explicitly tells that they do PostScript
  if ((cmd = cupsGetOption("COMMAND SET", num_did, did)) == NULL)
    cmd = cupsGetOption("CMD", num_did, did);

  if (cmd == NULL ||
      ((ps = strcasestr(cmd, "POSTSCRIPT")) == NULL &&
       (ps = strcasestr(cmd, "BRSCRIPT")) == NULL &&
       ((ps = strcasestr(cmd, "PS")) == NULL ||
	(ps[2] != ',' && ps[2])) &&
       ((ps = strcasestr(cmd, "PS2")) == NULL ||
	(ps[3] != ',' && ps[3])) &&
       ((ps = strcasestr(cmd, "PS3")) == NULL ||
	(ps[3] != ',' && ps[3]))) ||
      (ps != cmd && *(ps - 1) != ','))
  {
    // Printer does not support PostScript, it is not supported by this
    // Printer Application
    ret = NULL;
    goto done;
  }

  // Make and model
  if ((mfg = cupsGetOption("MANUFACTURER", num_did, did)) == NULL)
    mfg = cupsGetOption("MFG", num_did, did);
  if ((mdl = cupsGetOption("MODEL", num_did, did)) == NULL)
    mdl = cupsGetOption("MDL", num_did, did);

  if (mfg && mdl)
  {
    // Match make and model with device ID of driver list entry
    for (i = 1; i < num_drivers; i ++)
    {
      if (drivers[i].device_id[0])
      {
	num_ddid = papplDeviceParseID(drivers[i].device_id, &ddid);
	if (num_ddid == 0 || ddid == NULL)
	  continue;
	if ((dmfg = cupsGetOption("MANUFACTURER", num_ddid, ddid)) == NULL)
	  dmfg = cupsGetOption("MFG", num_ddid, ddid);
	if ((dmdl = cupsGetOption("MODEL", num_ddid, ddid)) == NULL)
	  dmdl = cupsGetOption("MDL", num_ddid, ddid);
	if (dmfg && dmdl &&
	    strcasecmp(mfg, dmfg) == 0 &&
	    strcasecmp(mdl, dmdl) == 0)
	  // Match
	  ret = drivers[i].name;
	cupsFreeOptions(num_ddid, ddid);
	if (ret)
	  goto done;
      }
    }

    // Normalize device ID to format of driver name and match
    ieee1284NormalizeMakeAndModel(device_id, NULL,
				  IEEE1284_NORMALIZE_IPP,
				  buf, sizeof(buf),
				  NULL, NULL);
    for (i = 1; i < num_drivers; i ++)
    {
      if (strncmp(buf, drivers[i].name, strlen(buf)) == 0)
      {
	// Match
	ret = drivers[i].name;
	goto done;
      }
    }
  }

  // PostScript printer but none of the PPDs matches? Assign the generic PPD
  // if we have one
  if (strcasecmp(drivers[1].name, "generic"))
    ret = "generic";

 done:
  cupsFreeOptions(num_did, did);

  return (ret);
}


//
// 'ps_callback()' - PostScript callback.
//

static bool				   // O - `true` on success, `false`
                                           //     on failure
ps_callback(
    pappl_system_t       *system,	   // I - System
    const char           *driver_name,     // I - Driver name
    const char           *device_uri,	   // I - Device URI
    const char           *device_id,	   // I - Device ID
    pappl_pr_driver_data_t *driver_data,   // O - Driver data
    ipp_t                **driver_attrs,   // O - Driver attributes
    void                 *data)	           // I - Callback data
{
  int          i, j;                       // Looping variables
  cups_array_t *ppd_paths = (cups_array_t *)data;
  ps_ppd_path_t *ppd_path,
               search_ppd_path;
  ppd_file_t   *ppd = NULL;		   // PPD file loaded from collection
  ppd_cache_t  *pc;
  char         *keyword;
  ipp_res_t    units;			   // Resolution units
  const char   *def_source,
               *def_type,
               *def_media;
  int          def_left, def_right, def_top, def_bottom;
  ppd_option_t *option;
  ppd_choice_t *choice;
  ppd_attr_t   *ppd_attr;
  pwg_map_t    *pwg_map;
  pwg_size_t   *pwg_size;
  ppd_pwg_finishings_t *finishings;
  int          count;

  if (!driver_name || !device_uri || !driver_data || !driver_attrs)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Driver callback called without required information.");
    return (false);
  }

  if (!ppd_paths || cupsArrayCount(ppd_paths) == 0)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Driver callback did not find PPD indices.");
    return (false);
  }

  //
  // Load assigned PPD file from the PPD collection, mark defaults, create
  // cache
  //

 retry:
  if (strcasecmp(driver_name, "auto") == 0)
  {
    // Auto-select driver
    search_ppd_path.driver_name = ps_autoadd(NULL, device_uri, device_id, NULL);
    if (search_ppd_path.driver_name)
      papplLog(system, PAPPL_LOGLEVEL_INFO,
	       "Automatic printer driver selection, using \"%s\".",
	       search_ppd_path.driver_name);
    else
    {
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "Automatic printer driver selection for printer "
	       "\"%s\" with device ID \"%s\" failed.",
	       device_uri, device_id);
      return (false);
    }
  }
  else
    search_ppd_path.driver_name = driver_name;

  ppd_path = (ps_ppd_path_t *)cupsArrayFind(ppd_paths, &search_ppd_path);

  if (ppd_path == NULL)
  {
    if (strcasecmp(driver_name, "auto") == 0)
    {
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "For the printer driver \"%s\" got auto-selected which does not "
	       "exist in this Printer Application.",
	       search_ppd_path.driver_name);
      return (false);
    }
    else
    {
      papplLog(system, PAPPL_LOGLEVEL_WARN,
	       "Printer uses driver \"%s\" which does not exist in this "
	       "Printer Application, switching to \"auto\".", driver_name);
      driver_name = "auto";
      goto retry;
    }
  }

  if ((ppd = ppdOpen2(ppdCollectionGetPPD(ppd_path->ppd_path, NULL,
					  (filter_logfunc_t)papplLog,
					  system))) == NULL)
  {
    ppd_status_t	err;		// Last error in file
    int			line;		// Line number in file

    err = ppdLastError(&line);
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "PPD %s: %s on line %d\n", ppd_path->ppd_path,
	     ppdErrorString(err), line);
    return (false);
  }

  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Using PPD %s: %s\n", ppd_path->ppd_path, ppd->nickname);

  ppdMarkDefaults(ppd);
  if ((pc = ppdCacheCreateWithPPD(ppd)) != NULL)
    ppd->cache = pc;

  //
  // Populate driver data record
  //

  // Callback functions end general properties
  driver_data->extension          = strdup(ppd_path->ppd_path);
  driver_data->identify_cb        = ps_identify;
  driver_data->identify_default   = PAPPL_IDENTIFY_ACTIONS_SOUND;
  driver_data->identify_supported = PAPPL_IDENTIFY_ACTIONS_DISPLAY |
    PAPPL_IDENTIFY_ACTIONS_SOUND;
  driver_data->printfile_cb       = NULL;
  driver_data->rendjob_cb         = ps_rendjob;
  driver_data->rendpage_cb        = ps_rendpage;
  driver_data->rstartjob_cb       = ps_rstartjob;
  driver_data->rstartpage_cb      = ps_rstartpage;
  driver_data->rwriteline_cb      = ps_rwriteline;
  driver_data->status_cb          = ps_status;
  driver_data->testpage_cb        = NULL; // XXX Add ps_testpage() function
  driver_data->format             = "application/vnd.printer-specific";
  driver_data->orient_default     = IPP_ORIENT_NONE;
  driver_data->quality_default    = IPP_QUALITY_NORMAL;

  // Make and model
  strncpy(driver_data->make_and_model,
	  ppd->nickname,
	  sizeof(driver_data->make_and_model));

  // Print speed in pages per minute (PPDs do not show different values for
  // Grayscale and Color)
  driver_data->ppm = ppd->throughput;
  if (driver_data->ppm <= 1)
    driver_data->ppm = 1;
  if (ppd->color_device)
    driver_data->ppm_color = driver_data->ppm;
  else
    driver_data->ppm_color = 0;

  // Properties bot supported by the PPD
  driver_data->has_supplies = false;
  driver_data->input_face_up = false;

  // Pages face-up or face-down in output bin?
  if (pc->num_bins > 0)
    driver_data->output_face_up =
      (strstr(pc->bins->pwg, "face-up") != NULL);
  else
    driver_data->output_face_up = false;

  // No orientationrequested by default
  driver_data->orient_default = IPP_ORIENT_NONE;

  // Supported color modes
  if (ppd->color_device)
  {
    driver_data->color_supported =
      PAPPL_COLOR_MODE_AUTO | PAPPL_COLOR_MODE_COLOR |
      PAPPL_COLOR_MODE_MONOCHROME;
    driver_data->color_default = PAPPL_COLOR_MODE_AUTO;
  }
  else
  {
    driver_data->color_supported = PAPPL_COLOR_MODE_MONOCHROME;
    driver_data->color_default = PAPPL_COLOR_MODE_MONOCHROME;
  }

  // These parameters are usually not defined in PPDs
  driver_data->content_default = PAPPL_CONTENT_AUTO;
  driver_data->quality_default = IPP_QUALITY_NORMAL; // XXX Check with presets/
                                                     // resolution
  driver_data->scaling_default = PAPPL_SCALING_AUTO;

  // Raster graphics modes fo PWG Raster input
  if (ppd->color_device)
    driver_data->raster_types =
      PAPPL_PWG_RASTER_TYPE_BLACK_1 | PAPPL_PWG_RASTER_TYPE_SGRAY_8 |
      PAPPL_PWG_RASTER_TYPE_SRGB_8;
  else
    driver_data->raster_types =
      PAPPL_PWG_RASTER_TYPE_BLACK_1 | PAPPL_PWG_RASTER_TYPE_SGRAY_8;
  driver_data->force_raster_type = 0;

  // Duplex
  driver_data->sides_supported = PAPPL_SIDES_ONE_SIDED;
  driver_data->duplex = PAPPL_DUPLEX_NONE;
  driver_data->sides_default = PAPPL_SIDES_ONE_SIDED;
  if (pc->sides_2sided_long)
  {
    driver_data->sides_supported |= PAPPL_SIDES_TWO_SIDED_LONG_EDGE;
    driver_data->duplex = PAPPL_DUPLEX_NORMAL;
    if ((choice = ppdFindMarkedChoice(ppd, pc->sides_option)) != NULL &&
	strcmp(choice->choice, pc->sides_2sided_long) == 0)
      driver_data->sides_default = PAPPL_SIDES_TWO_SIDED_LONG_EDGE;
  }
  if (pc->sides_2sided_short)
  {
    driver_data->sides_supported |= PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
    driver_data->duplex = PAPPL_DUPLEX_NORMAL;
    if ((choice = ppdFindMarkedChoice(ppd, pc->sides_option)) != NULL &&
	strcmp(choice->choice, pc->sides_2sided_short) == 0)
      driver_data->sides_default = PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
  }

  // Finishings
  driver_data->finishings = PAPPL_FINISHINGS_NONE;
  for (i = 1,
	 finishings = (ppd_pwg_finishings_t *)cupsArrayFirst(pc->finishings);
       finishings;
       i ++,
	 finishings = (ppd_pwg_finishings_t *)cupsArrayNext(pc->finishings))
  {
    if (finishings->value == IPP_FINISHINGS_STAPLE)
      driver_data->finishings |= PAPPL_FINISHINGS_STAPLE;
    else  if (finishings->value == IPP_FINISHINGS_PUNCH)
      driver_data->finishings |= PAPPL_FINISHINGS_PUNCH;
    else if (finishings->value == IPP_FINISHINGS_TRIM)
      driver_data->finishings |= PAPPL_FINISHINGS_TRIM;
  }

  // Resolution
  if ((option = ppdFindOption(ppd, "Resolution")) != NULL &&
      (count = option->num_choices) > 0) {
    // Valid "Resolution" option, make a sorted list of resolutions.
    if (count > PAPPL_MAX_RESOLUTION)
      count = PAPPL_MAX_RESOLUTION;
    driver_data->num_resolution = count;

    for (i = 0, choice = option->choices; i < count; i ++, choice ++)
    {
      if ((j = sscanf(choice->choice, "%dx%d",
		      &(driver_data->x_resolution[i]),
		      &(driver_data->y_resolution[i]))) == 1)
	driver_data->y_resolution[i] = driver_data->x_resolution[i];
      else if (j <= 0)
      {
	papplLog(system, PAPPL_LOGLEVEL_ERROR,
		 "Invalid resolution: %s\n", choice->choice);
	i --;
	count --;
	continue;
      }
      if (choice->marked) // Default resolution
      {
	driver_data->x_default = driver_data->x_resolution[i];
	driver_data->y_default = driver_data->y_resolution[i];
      }
      for (j = i - 1; j >= 0; j --)
      {
	int       x1, y1,               // First X and Y resolution
	          x2, y2,               // Second X and Y resolution
	          temp;                 // Swap variable
	x1 = driver_data->x_resolution[j];
	y1 = driver_data->y_resolution[j];
	x2 = driver_data->x_resolution[j + 1];
	y2 = driver_data->y_resolution[j + 1];
	if (x2 < x1 || (x2 == x1 && y2 < y1))
	{
	  temp                             = driver_data->x_resolution[j];
	  driver_data->x_resolution[j]     = driver_data->x_resolution[j + 1];
	  driver_data->x_resolution[j + 1] = temp;
	  temp                             = driver_data->y_resolution[j];
	  driver_data->y_resolution[j]     = driver_data->y_resolution[j + 1];
	  driver_data->y_resolution[j + 1] = temp;
	}
      }
    }
  }
  else
  {
    if ((ppd_attr = ppdFindAttr(ppd, "DefaultResolution", NULL)) != NULL)
    {
      // Use the PPD-defined default resolution...
      if ((j = sscanf(ppd_attr->value, "%dx%d",
		      &(driver_data->x_resolution[0]),
		      &(driver_data->y_resolution[0]))) == 1)
	driver_data->y_resolution[0] = driver_data->x_resolution[0];
      else if (j <= 0)
      {
	papplLog(system, PAPPL_LOGLEVEL_ERROR,
		 "Invalid resolution: %s, using 300 dpi\n", ppd_attr->value);
	driver_data->x_resolution[0] = 300;
	driver_data->y_resolution[0] = 300;
      }
    }
    else
    {
      papplLog(system, PAPPL_LOGLEVEL_WARN,
	       "No default resolution, using 300 dpi\n");
      driver_data->x_resolution[0] = 300;
      driver_data->y_resolution[0] = 300;
    }
    driver_data->num_resolution = 1;
    driver_data->x_default = driver_data->x_resolution[0];
    driver_data->y_default = driver_data->y_resolution[0];
  }

  // Media source
  if ((count = pc->num_sources) > 0)
  {
    if (count > PAPPL_MAX_SOURCE)
      count = PAPPL_MAX_SOURCE;
    driver_data->num_source = count;
    choice = ppdFindMarkedChoice(ppd, pc->source_option);
    def_source = NULL;
    for (i = 0, pwg_map = pc->sources; i < count; i ++, pwg_map ++)
    {
      driver_data->source[i] = strdup(pwg_map->pwg);
      if (choice && !strcmp(pwg_map->ppd, choice->choice))
	def_source = driver_data->source[i];
    }
    if (def_source == NULL)
      def_source = driver_data->source[0];
  }
  else
  {
    driver_data->num_source = 1;
    driver_data->source[0] = "auto";
    def_source = driver_data->source[0];
  }

  // Media type
  if ((count = pc->num_types) > 0)
  {
    if (count > PAPPL_MAX_SOURCE)
      count = PAPPL_MAX_SOURCE;
    driver_data->num_type = count;
    choice = ppdFindMarkedChoice(ppd, "MediaType");
    def_type = NULL;
    for (i = 0, pwg_map = pc->types; i < count; i ++, pwg_map ++)
    {
      driver_data->type[i] = strdup(pwg_map->pwg);
      if (choice && !strcmp(pwg_map->ppd, choice->choice))
	def_type = driver_data->type[i];
    }
    if (def_type == NULL)
      def_type = driver_data->type[0];
  }
  else
  {
    driver_data->num_type = 1;
    driver_data->type[0] = "auto";
    def_type = driver_data->type[0];
  }

  // Media size, margins
  def_left = def_right = def_top = def_bottom = -1;
  driver_data->borderless = false;
  count = pc->num_sizes;
  if (count > PAPPL_MAX_MEDIA)
    count = PAPPL_MAX_MEDIA;
  driver_data->num_media = count;
  choice = ppdFindMarkedChoice(ppd, "PageSize");
  for (i = 0, pwg_size = pc->sizes; i < pc->num_sizes; i ++, pwg_size ++)
  {
    if (i < count)
    {
      driver_data->media[i] =
	strdup(pwg_size->map.pwg);
      if (choice && !strcmp(pwg_size->map.ppd, choice->choice))
	ps_media_col(pwg_size, def_source, def_type, 0, 0, 0,
		     &(driver_data->media_default));
    }
    if (pwg_size->left > def_left)
      def_left = pwg_size->left;
    if (pwg_size->right > def_right)
      def_right = pwg_size->right;
    if (pwg_size->top > def_top)
      def_top = pwg_size->top;
    if (pwg_size->bottom > def_bottom)
      def_bottom = pwg_size->bottom;
    if (pwg_size->left == 0 && pwg_size->right == 0 &&
	pwg_size->top == 0 && pwg_size->bottom == 0)
      driver_data->borderless = true;
  }
  if (def_left < 0)
    def_left = 635;
  if (def_right < 0)
    def_right = 635;
  if (def_top < 0)
    def_top = 1270;
  if (def_bottom < 0)
    def_bottom = 1270;
  driver_data->left_right = (def_left > def_right ? def_left : def_right);
  driver_data->bottom_top = (def_bottom > def_top ? def_bottom : def_right);

  // "media-ready" not defined in PPDs
  // For "media-ready" we need to actually query the printer XXX
  for (i = 0; i < driver_data->num_source; i ++)
  {
    memcpy(&(driver_data->media_ready[i]), &(driver_data->media_default),
	   sizeof(pappl_media_col_t));
    strncpy(driver_data->media_ready[i].source, driver_data->source[i],
	    sizeof(driver_data->media_ready[i].source));
  }

  // Offsets not defined in PPDs
  driver_data->left_offset_supported[0] = 0;
  driver_data->left_offset_supported[1] = 0;
  driver_data->top_offset_supported[0] = 0;
  driver_data->top_offset_supported[1] = 0;

  // Media tracking not defined in PPDs
  driver_data->tracking_supported = 0;

  // Output bins
  if ((count = pc->num_bins) > 0)
  {
    if (count > PAPPL_MAX_BIN)
      count = PAPPL_MAX_BIN;
    driver_data->num_bin = count;
    choice = ppdFindMarkedChoice(ppd, "OutputBin");
    driver_data->bin_default = 0;
    for (i = 0, pwg_map = pc->bins; i < count; i ++, pwg_map ++)
    {
      driver_data->bin[i] = strdup(pwg_map->pwg);
      if (choice && !strcmp(pwg_map->ppd, choice->choice))
	driver_data->bin_default = i;
    }
  }
  else
  {
    driver_data->num_bin = 1;
    driver_data->bin[0] = "auto";
    driver_data->bin_default = 0;
  }

  // Properties not defined in PPDs
  driver_data->mode_configured = 0;
  driver_data->mode_supported = 0;
  driver_data->tear_offset_configured = 0;
  driver_data->tear_offset_supported[0] = 0;
  driver_data->tear_offset_supported[1] = 0;
  driver_data->speed_supported[0] = 0;
  driver_data->speed_supported[1] = 0;
  driver_data->speed_default = 0;
  driver_data->darkness_default = 0;
  driver_data->darkness_configured = 0;
  driver_data->darkness_supported = 0;
  driver_data->num_features = 0;
  driver_data->num_vendor = 0;

  ppdClose(ppd);

  return (true);
}


//
// 'ps_compare_ppd_paths()' - Compare function for sorting PPD path array
//

static int
ps_compare_ppd_paths(void *a,
		     void *b,
		     void *data)
{
  ps_ppd_path_t *aa = (ps_ppd_path_t *)a;
  ps_ppd_path_t *bb = (ps_ppd_path_t *)b;

  (void)data;
  return (strcmp(aa->driver_name, bb->driver_name));
}



//
// 'ps_create_job_data()' - Load the printer's PPD file and set the PPD options
//                          according to the job options
//

static ps_job_data_t *ps_create_job_data(pappl_job_t *job,
					 pappl_pr_options_t *job_options)
{
  int                   i, j, count;
  ps_job_data_t         *job_data;      // PPD data for job
  ppd_cache_t           *pc;
  pappl_pr_driver_data_t driver_data;   // Printer driver data
  const char            *ppd_path;
  cups_option_t         *opt;
  ipp_t                 *driver_attrs;  // Printer (driver) IPP attributes
  char                  buf[1024];      // Buffer for building strings
  const char            *optstr,        // Option name from PPD option
                        *choicestr,     // Choice name from PPD option
                        *val;           // Value string from IPP option
  ipp_t                 *attrs;         // IPP Attributes structure
  ipp_t		        *media_col,	// media-col IPP structure
                        *media_size;	// media-size IPP structure
  int                   xres, yres;
  ipp_attribute_t       *attr;
  int                   pcm;            // Print color mode: 0: mono,
                                        // 1: color (for presets)
  int                   pq;             // IPP Print quality value (for presets)
  int		        num_presets;	// Number of presets
  cups_option_t	        *presets;       // Presets of PPD options
  ppd_option_t          *option;        // PPD option
  ppd_choice_t          *choice;        // Choice in PPD option
  ppd_attr_t            *ppd_attr;
  pwg_map_t             *pwg_map;
  int                   jobcanceled = 0;// Is job canceled?
  pappl_printer_t       *printer = papplJobGetPrinter(job);

  //
  // Load the printer's assigned PPD file, mark the defaults, and create the
  // cache
  //

  job_data = (ps_job_data_t *)calloc(1, sizeof(ps_job_data_t));

  papplPrinterGetDriverData(printer, &driver_data);
  ppd_path = (const char *)driver_data.extension;
  if ((job_data->ppd =
       ppdOpen2(ppdCollectionGetPPD(ppd_path, NULL,
				    (filter_logfunc_t)papplLogJob, job))) ==
      NULL)
  {
    ppd_status_t	err;		// Last error in file
    int			line;		// Line number in file

    err = ppdLastError(&line);
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
		"PPD %s: %s on line %d\n", ppd_path,
		ppdErrorString(err), line);
    return (NULL);
  }

  ppdMarkDefaults(job_data->ppd);
  if ((pc = ppdCacheCreateWithPPD(job_data->ppd)) != NULL)
    job_data->ppd->cache = pc;

  driver_attrs = papplPrinterGetDriverAttributes(printer);

  //
  // Find the PPD (or filter) options corresponding to the job options
  //

  // Job options without PPD equivalent
  //  - print-content-optimize
  //  - print-darkness
  //  - darkness-configured
  //  - print-speed

  // page-ranges (filter option)
  if (job_options->first_page == 0)
    job_options->first_page = 1;
  if (job_options->last_page == 0)
    job_options->last_page = INT_MAX;
  if (job_options->first_page > 1 || job_options->last_page < INT_MAX)
  {
    snprintf(buf, sizeof(buf), "%d-%d",
	     job_options->first_page, job_options->last_page);
    job_data->num_options = cupsAddOption("page-ranges", buf,
					  job_data->num_options,
					  &(job_data->options));
  }

  // Finishings
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding options for finishings");
  if (job_options->finishings & PAPPL_FINISHINGS_PUNCH)
    job_data->num_options = ppdCacheGetFinishingOptions(pc, NULL,
							IPP_FINISHINGS_PUNCH,
							job_data->num_options,
							&(job_data->options));
  if (job_options->finishings & PAPPL_FINISHINGS_STAPLE)
    job_data->num_options = ppdCacheGetFinishingOptions(pc, NULL,
							IPP_FINISHINGS_STAPLE,
							job_data->num_options,
							&(job_data->options));
  if (job_options->finishings & PAPPL_FINISHINGS_TRIM)
    job_data->num_options = ppdCacheGetFinishingOptions(pc, NULL,
							IPP_FINISHINGS_TRIM,
							job_data->num_options,
							&(job_data->options));

  // PageSize/media/media-size/media-size-name
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: PageSize");
  attrs = ippNew();
  media_col = ippNew();
  media_size = ippNew();
  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"x-dimension", job_options->media.size_width);
  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"y-dimension", job_options->media.size_length);
  ippAddCollection(media_col, IPP_TAG_PRINTER, "media-size", media_size);
  ippDelete(media_size);
  ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-size-name",
	       NULL, job_options->media.size_name);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"media-left-margin", job_options->media.left_margin);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"media-right-margin", job_options->media.right_margin);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"media-top-margin", job_options->media.top_margin);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"media-bottom-margin", job_options->media.bottom_margin);
  ippAddCollection(attrs, IPP_TAG_PRINTER, "media-col", media_col);
  ippDelete(media_col);
  if ((choicestr = ppdCacheGetPageSize(pc, attrs, NULL, NULL)) != NULL)
    job_data->num_options = cupsAddOption("PageSize", choicestr,
					  job_data->num_options,
					  &(job_data->options));
  ippDelete(attrs);

  // InputSlot/media-source
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: InputSlot");
  if ((choicestr = ppdCacheGetInputSlot(pc, NULL,
					job_options->media.source)) !=
      NULL)
    job_data->num_options = cupsAddOption("InputSlot", choicestr,
					  job_data->num_options,
					  &(job_data->options));

  // MediaType/media-type
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: MediaType");
  if ((choicestr = ppdCacheGetMediaType(pc, NULL,
					job_options->media.type)) != NULL)
    job_data->num_options = cupsAddOption("MediaType", choicestr,
					  job_data->num_options,
					  &(job_data->options));

  // orientation-requested (filter option)
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
	      "Adding option: orientation-requested");
  if (job_options->orientation_requested >= IPP_ORIENT_PORTRAIT &&
      job_options->orientation_requested <  IPP_ORIENT_NONE)
  {
    snprintf(buf, sizeof(buf), "%d", job_options->orientation_requested);
    job_data->num_options = cupsAddOption("orientation-requested", buf,
					  job_data->num_options,
					  &(job_data->options));
  }

  // Presets, selected by color/bw and print quality
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
	      "Adding option presets depending on requested print quality");
  if (job_data->ppd->color_device &&
      (job_options->print_color_mode &
       (PAPPL_COLOR_MODE_AUTO | PAPPL_COLOR_MODE_COLOR)) != 0)
    pcm = 1;
  else
    pcm = 0;
  if (job_options->print_quality == IPP_QUALITY_DRAFT)
    pq = 0;
  else if (job_options->print_quality == IPP_QUALITY_HIGH)
    pq = 2;
  else
    pq = 1;
  num_presets = pc->num_presets[pcm][pq];
  presets     = pc->presets[pcm][pq];
  for (i = 0; i < num_presets; i ++)
    job_data->num_options = cupsAddOption(presets[i].name, presets[i].value,
					  job_data->num_options,
					  &(job_data->options));

  // Do we have a way to force grayscale printing?
  if (pcm == 0)
  {
    // Find suitable option in the PPD file and set it if available
    if (ps_have_force_gray(job_data->ppd, &optstr, &choicestr) &&
	cupsGetOption(optstr, job_data->num_options,
		      job_data->options) == NULL)
      job_data->num_options = cupsAddOption(optstr, choicestr,
					    job_data->num_options,
					    &(job_data->options));
    // Add "ColorModel=Gray" to make filters converting color
    // input to grayscale
    if (cupsGetOption("ColorModel", job_data->num_options,
		      job_data->options) == NULL)
      job_data->num_options = cupsAddOption("ColorModel", "Gray",
					    job_data->num_options,
					    &(job_data->options));
  }

  // print-scaling (filter option)
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: print-scaling");
  if (job_options->print_scaling)
  {
    if (job_options->print_scaling & PAPPL_SCALING_AUTO)
      job_data->num_options = cupsAddOption("print-scaling", "auto",
					    job_data->num_options,
					    &(job_data->options));
    if (job_options->print_scaling & PAPPL_SCALING_AUTO_FIT)
      job_data->num_options = cupsAddOption("print-scaling", "auto-fit",
					    job_data->num_options,
					    &(job_data->options));
    if (job_options->print_scaling & PAPPL_SCALING_FILL)
      job_data->num_options = cupsAddOption("print-scaling", "fill",
					    job_data->num_options,
					    &(job_data->options));
    if (job_options->print_scaling & PAPPL_SCALING_FIT)
      job_data->num_options = cupsAddOption("print-scaling", "fit",
					    job_data->num_options,
					    &(job_data->options));
    if (job_options->print_scaling & PAPPL_SCALING_NONE)
      job_data->num_options = cupsAddOption("print-scaling", "none",
					    job_data->num_options,
					    &(job_data->options));
  }

  // Resolution/printer-resolution
  // Only add a "Resolution" option if there is none yet (from presets)
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: Resolution");
  if (cupsGetOption("Resolution", job_data->num_options,
		    job_data->options) == NULL)
  {
    if ((job_options->printer_resolution[0] &&
	 ((attr = papplJobGetAttribute(job, "printer-resolution")) != NULL ||
	  (attr = papplJobGetAttribute(job, "Resolution")) != NULL) &&
	 (option = ppdFindOption(job_data->ppd, "Resolution")) != NULL &&
	 (count = option->num_choices) > 0))
    {
      for (i = 0, choice = option->choices; i < count; i ++, choice ++)
      {
	if ((j = sscanf(choice->choice, "%dx%d", &xres, &yres)) == 1)
	  yres = xres;
	else if (j <= 0)
	  continue;
	if (job_options->printer_resolution[0] == xres &&
	    (job_options->printer_resolution[1] == yres ||
	     (job_options->printer_resolution[1] == 0 && xres == yres)))
	  break;
      }
      if (i < count)
	job_data->num_options = cupsAddOption("Resolution", choice->choice,
					      job_data->num_options,
					      &(job_data->options));
    }
    else if (job_options->printer_resolution[0])
    {
      if (job_options->printer_resolution[1] &&
	  job_options->printer_resolution[0] !=
	  job_options->printer_resolution[1])
	snprintf(buf, sizeof(buf) - 1, "%dx%ddpi",
		 job_options->printer_resolution[0],
		 job_options->printer_resolution[1]);
      else
	snprintf(buf, sizeof(buf) - 1, "%ddpi",
		 job_options->printer_resolution[0]);
      job_data->num_options = cupsAddOption("Resolution", buf,
					    job_data->num_options,
					    &(job_data->options));
    }
    else if ((ppd_attr = ppdFindAttr(job_data->ppd, "DefaultResolution",
				     NULL)) != NULL)
      job_data->num_options = cupsAddOption("Resolution", ppd_attr->value,
					    job_data->num_options,
					    &(job_data->options));
  }

  // Duplex/sides
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: Duplex");
  if (job_options->sides && pc->sides_option)
  {
    if (job_options->sides & PAPPL_SIDES_ONE_SIDED &&
	pc->sides_1sided)
      job_data->num_options = cupsAddOption(pc->sides_option,
					    pc->sides_1sided,
					    job_data->num_options,
					    &(job_data->options));
    else if (job_options->sides & PAPPL_SIDES_TWO_SIDED_LONG_EDGE &&
	     pc->sides_2sided_long)
      job_data->num_options = cupsAddOption(pc->sides_option,
					    pc->sides_2sided_long,
					    job_data->num_options,
					    &(job_data->options));
    else if (job_options->sides & PAPPL_SIDES_TWO_SIDED_SHORT_EDGE &&
	     pc->sides_2sided_short)
      job_data->num_options = cupsAddOption(pc->sides_option,
					    pc->sides_2sided_short,
					    job_data->num_options,
					    &(job_data->options));
  }

  //
  // Find further options directly from the job IPP attributes
  //

  // OutputBin/output-bin
  if ((count = pc->num_bins) > 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: OutputBin");
    if ((attr = papplJobGetAttribute(job, "output-bin")) == NULL)
      attr = ippFindAttribute(driver_attrs, "output-bin-default",
			      IPP_TAG_ZERO);
    choicestr = NULL;
    if (attr)
    {
      val = ippGetString(attr, 0, NULL);
      for (i = 0, pwg_map = pc->bins; i < count; i ++, pwg_map ++)
	if (!strcmp(pwg_map->pwg, val))
	  choicestr = pwg_map->ppd;
    }
    if (choicestr == NULL)
    {
      val = driver_data.bin[driver_data.bin_default];
      for (i = 0, pwg_map = pc->bins; i < count; i ++, pwg_map ++)
	if (!strcmp(pwg_map->pwg, val))
	  choicestr = pwg_map->ppd;
    }
    if (choicestr != NULL)
      job_data->num_options = cupsAddOption("OutputBin", choicestr,
					    job_data->num_options,
					    &(job_data->options));
  }

  // XXX "Collate" option

  // Mark options in the PPD file
  ppdMarkOptions(job_data->ppd, job_data->num_options, job_data->options);

  // Log the option settings which will get used
  snprintf(buf, sizeof(buf) - 1, "PPD options to be used:");
  for (i = job_data->num_options, opt = job_data->options; i > 0; i --, opt ++)
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1,
	     " %s=%s", opt->name, opt->value);
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "%s", buf);  

  return (job_data);
}


//
// 'ps_filter()' - PAPPL generic filter function wrapper
//

bool
ps_filter(
    pappl_job_t    *job,		// I - Job
    pappl_device_t *device,		// I - Device
    void           *data)		// I - Filter data
{
  int                   i, j, count;
  ps_job_data_t         *job_data;      // PPD data for job
  ps_filter_data_t	*psfd = (ps_filter_data_t *)data;
  const char		*filename;	// Input filename
  int			fd;		// Input file descriptor
  int                   nullfd;         // File descriptor for /dev/null
  pappl_pr_options_t	*job_options;	// Job options
  bool			ret = false;	// Return value
  filter_data_t         filter_data;    // Data for supplying to filter
                                        // function
  char                  buf[1024];      // Buffer for building strings
  cups_array_t          *chain;         // Filter function chain
  filter_filter_in_chain_t *filter,     // Filter function call for filtering
                           *print;      // Filter function call for printing
  int                   jobcanceled = 0;// Is job canceled?
  pappl_printer_t       *printer = papplJobGetPrinter(job);

  //
  // Load the printer's assigned PPD file, and find out which PPD option
  // seetings correspond to our job options
  //

  job_options = papplJobCreatePrintOptions(job, INT_MAX, 1);
  job_data = ps_create_job_data(job, job_options);

  //
  // Open the input file...
  //

  filename = papplJobGetFilename(job);
  if ((fd = open(filename, O_RDONLY)) < 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to open JPEG file '%s': %s",
		filename, strerror(errno));
    return (false);
  }

  //
  // Create data record to call filter functions
  //

  filter_data.job_id = papplJobGetID(job);
  filter_data.job_user = strdup(papplJobGetUsername(job));
  filter_data.job_title = strdup(papplJobGetName(job));
  filter_data.copies = job_options->copies;
  filter_data.job_attrs = NULL;     // We use PPD/filter options
  filter_data.printer_attrs = NULL; // We use the printer's PPD file
  filter_data.num_options = job_data->num_options;
  filter_data.options = job_data->options; // PPD/filter options
  filter_data.ppdfile = NULL;
  filter_data.ppd = job_data->ppd;
  filter_data.logfunc = ps_job_log; // Job log function catching page counts
                                    // ("PAGE: XX YY" messages)
  filter_data.logdata = job;

  //
  // Set up filter function chain
  //

  chain = cupsArrayNew(NULL, NULL);
  filter =
    (filter_filter_in_chain_t *)calloc(1, sizeof(filter_filter_in_chain_t));
  filter->function = psfd->filter_function;
  filter->parameters = psfd->filter_parameters;
  filter->name = "Filtering";
  cupsArrayAdd(chain, filter);
  print =
    (filter_filter_in_chain_t *)calloc(1, sizeof(filter_filter_in_chain_t));
  print->function = ps_print_filter_function;
  print->parameters = device;
  print->name = "Printing";
  cupsArrayAdd(chain, print);

  //
  // Fire up the filter functions
  //

  papplJobSetImpressions(job, 1);

  // The filter chain has no output, data is going to the device
  nullfd = open("/dev/null", O_RDWR);

  if (filterChain(fd, nullfd, 1, &jobcanceled, &filter_data, chain) == 0)
    ret = true;

  //
  // Clean up
  //

  papplJobDeletePrintOptions(job_options);
  free(filter_data.job_user);
  free(filter_data.job_title);
  free(filter);
  free(print);
  cupsArrayDelete(chain);
  ps_free_job_data(job_data);
  close(fd);
  close(nullfd);

  return (ret);
}


//
// 'ps_free_job_data()' - Clean up PPD and PPD options.
//

static void   ps_free_job_data(ps_job_data_t *job_data)
{
  ppdClose(job_data->ppd);
  cupsFreeOptions(job_data->num_options, job_data->options);
  free(job_data);
}


//
// 'ps_have_force_gray()' - Check PPD file whether there is an option setting
//                          which forces grayscale output. Return the first
//                          suitable one as pair of option name and value.
//

static bool                                // O - True if suitable setting found
ps_have_force_gray(ppd_file_t *ppd,        // I - PPD file to check
		   const char **optstr,    // I - Option name of found option
		   const char **choicestr) // I - Choice name to force grayscale
{
  ppd_option_t *option;


  if ((option = ppdFindOption(ppd, "ColorModel")) != NULL &&
      ppdFindChoice(option, "Gray"))
  {
    if (optstr)
      *optstr = "ColorModel";
    if (choicestr)
      *choicestr = "Gray";
    return (true);
  }
  else if ((option = ppdFindOption(ppd, "ColorModel")) != NULL &&
	   ppdFindChoice(option, "Grayscale"))
  {
    if (optstr)
      *optstr = "ColorModel";
    if (choicestr)
      *choicestr = "Grayscale";
    return (true);
  }
  else if ((option = ppdFindOption(ppd, "HPColorMode")) != NULL &&
	   ppdFindChoice(option, "grayscale"))
  {
    if (optstr)
      *optstr = "HPColorMode";
    if (choicestr)
      *choicestr = "grayscale";
    return (true);
  }
  else if ((option = ppdFindOption(ppd, "BRMonoColor")) != NULL &&
	   ppdFindChoice(option, "Mono"))
  {
    if (optstr)
      *optstr = "BRMonoColor";
    if (choicestr)
      *choicestr = "Mono";
    return (true);
  }
  else if ((option = ppdFindOption(ppd, "CNIJSGrayScale")) != NULL &&
	   ppdFindChoice(option, "1"))
  {
    if (optstr)
      *optstr = "CNIJSGrayScale";
    if (choicestr)
      *choicestr = "1";
    return (true);
  }
  else if ((option = ppdFindOption(ppd, "HPColorAsGray")) != NULL &&
	   ppdFindChoice(option, "True"))
  {
    if (optstr)
      *optstr = "HPColorAsGray";
    if (choicestr)
      *choicestr = "True";
    return (true);
  }

  if (optstr)
    *optstr = NULL;
  if (choicestr)
    *choicestr = NULL;

  return (false);
}


//
// 'ps_identify()' - Identify the printer.
//

static void
ps_identify(
    pappl_printer_t          *printer,	// I - Printer
    pappl_identify_actions_t actions, 	// I - Actions to take
    const char               *message)	// I - Message, if any
{
  (void)printer;
  (void)actions;

  // Identify a printer using display, flash, sound or speech. XXX
}


//
// 'ps_job_log()' - Job log function which calls
//                  papplJobSetImpressionsCompleted() on page logs of
//                  filter functions
//

static void
ps_job_log(void *data,
	   filter_loglevel_t level,
	   const char *message,
	   ...)
{
  va_list arglist;
  pappl_job_t *job = (pappl_job_t *)data;
  char buf[1024];
  int page, copies;

  va_start(arglist, message);
  vsnprintf(buf, sizeof(buf) - 1, message, arglist);
  fflush(stdout);
  va_end(arglist);
  if (level == FILTER_LOGLEVEL_CONTROL)
  {
    if (sscanf(buf, "PAGE: %d %d", &page, &copies) == 2)
    {
      papplJobSetImpressionsCompleted(job, copies);
      papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Printing page %d, %d copies",
		  page, copies);
    }
    else
      papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Unused control message: %s",
		  buf);
  }
  else
    papplLogJob(job, (pappl_loglevel_t)level, "%s", buf);
}


//
// 'ps_media_col()' - Create a media-col entry
//

static void
ps_media_col(pwg_size_t *pwg_size,            // I - Media size entry from PPD
	                                      //     cache
	     const char *def_source,          // I - Default media source
	     const char *def_type,            // I - Default media type
	     int left_offset,                 // I - Left offset
	     int top_offset,                  // I - Top offset
	     pappl_media_tracking_t tracking, // I - Media tracking
	     pappl_media_col_t *col)          // O - PAPPL media col entry
{
  strncpy(col->size_name, pwg_size->map.pwg, sizeof(col->size_name));
  col->size_width = pwg_size->width;
  col->size_length = pwg_size->length;
  col->left_margin = pwg_size->left;
  col->right_margin = pwg_size->right;
  col->top_margin = pwg_size->top;
  col->bottom_margin = pwg_size->bottom;
  strncpy(col->source, def_source, sizeof(col->source));
  strncpy(col->type, def_type, sizeof(col->type));
  col->left_offset = left_offset;
  col->top_offset = top_offset;
  col->tracking = tracking;
}


//
// 'ps_one_bit_dither_on_draft()' - If a PWG/Apple-Raster or image job
//                                  is printed in grayscale in draft mode
//                                  switch to 1-bit dithering mode to get
//                                  printing as fast as possible
//

static void
ps_one_bit_dither_on_draft(
    pappl_job_t      *job,       // I   - Job
    pappl_pr_options_t *options) // I/O - Job options
{
  pappl_pr_driver_data_t driver_data;


  papplPrinterGetDriverData(papplJobGetPrinter(job), &driver_data);
  if (options->print_quality == IPP_QUALITY_DRAFT &&
      options->print_color_mode != PAPPL_COLOR_MODE_COLOR &&
      options->header.cupsNumColors == 1)
  {
    cupsRasterInitPWGHeader(&options->header,
			    pwgMediaForPWG(options->media.size_name),
			    "black_1",
			    options->printer_resolution[0],
			    options->printer_resolution[1],
			    (options->header.Duplex ?
			     (options->header.Tumble ?
			      "two-sided-short-edge" : "two-sided-long-edge") :
			     "one-sided"),
			    "normal");
    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
		"Monochrome draft quality job -> 1-bit dithering for speed-up");
    if (options->print_content_optimize == PAPPL_CONTENT_PHOTO ||
	!strcmp(papplJobGetFormat(job), "image/jpeg") ||
	!strcmp(papplJobGetFormat(job), "image/png"))
    {
      memcpy(options->dither, driver_data.pdither, sizeof(options->dither));
      papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
		  "Photo/Image-optimized dither matrix");
    }
    else
    {
      memcpy(options->dither, driver_data.gdither, sizeof(options->dither));
      papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
		  "General-purpose dither matrix");
    }
  }
}


//
// 'ps_print_filter_function()' - Print file.
//                                This function has the format of a filter
//                                function of libcupsfilters, so we can chain
//                                it with other filter functions using the
//                                special filter function filterChain() and
//                                so we do not need to care with forking. As
//                                we send off the data to the device instead
//                                of filtering, it behaves more like a
//                                backend than a filter, and sends nothing
//                                to its output FD. Therefore it must always
//                                be in the end of a chain.
//                                This function does not do any filtering or
//                                conversion, this has to be done by filters
//                                applied to the data before.
//

int                                           // O - Error status
ps_print_filter_function(int inputfd,         // I - File descriptor input 
			                      //     stream
			 int outputfd,        // I - File descriptor output
			                      //     stream (unused)
			 int inputseekable,   // I - Is input stream
			                      //     seekable? (unused)
		         int *jobcanceled,    // I - Pointer to integer
                                              //     marking whether job is
			                      //     canceled
			 filter_data_t *data, // I - Job and printer data
			 void *parameters)    // I - PAPPL output device
{
  ssize_t	       bytes;	              // Bytes read/written
  char	               buffer[65536];         // Read/write buffer
  pappl_device_t       *device = (pappl_device_t *)parameters;
                                              // PAPPL output device
  filter_logfunc_t     log = data->logfunc;   // Log function
  void                 *ld = data->logdata;   // log function data

  (void)inputseekable;

  //int fd = open("/tmp/printout", O_CREAT | O_WRONLY);
  while ((bytes = read(inputfd, buffer, sizeof(buffer))) > 0 &&
	 !*jobcanceled)
  {
    //write(fd, buffer, (size_t)bytes);
    if (papplDeviceWrite(device, buffer, (size_t)bytes) < 0)
    {
      if (log)
	log(ld, FILTER_LOGLEVEL_ERROR,
	    "Output to device: Unable to send %d bytes to printer.\n",
	    (int)bytes);
      close(inputfd);
      close(outputfd);
      return (1);
    }
  }
  papplDeviceFlush(device);
  //close(fd);
  close(inputfd);
  close(outputfd);
  return (0);
}


//
// 'ps_rendjob()' - End a job.
//

static bool                     // O - `true` on success, `false` on failure
ps_rendjob(
    pappl_job_t      *job,      // I - Job
    pappl_pr_options_t *options,// I - Options
    pappl_device_t   *device)   // I - Device
{
  ps_job_data_t *job_data;      // PPD data for job
  FILE *devout;
  filter_data_t filter_data;
  int num_pages;


  (void)options;
  job_data = (ps_job_data_t *)papplJobGetData(job);
  devout = job_data->device_file;

  fputs("%%Trailer\n", devout);
  if ((num_pages = papplJobGetImpressionsCompleted(job)) > 0)
    fprintf(devout,"%%%%Pages: %d\n", num_pages);
  fputs("%%EOF\n", devout);

  if (job_data->ppd->jcl_end)
    ppdEmitJCLEnd(job_data->ppd, devout);
  else
    fputc(0x04, devout);

  //
  // Clean up
  //

  filter_data.logfunc = ps_job_log;
  filter_data.logdata = job;
  fclose(job_data->device_file);
  filterPClose(job_data->device_fd, job_data->device_pid, &filter_data);

  ps_free_job_data(job_data);
  papplJobSetData(job, NULL);

  return (true);
}


//
// 'ps_rendpage()' - End a page.
//

static bool                     // O - `true` on success, `false` on failure
ps_rendpage(
    pappl_job_t      *job,      // I - Job
    pappl_pr_options_t *options,// I - Job options
    pappl_device_t   *device,   // I - Device
    unsigned         page)      // I - Page number
{
  ps_job_data_t         *job_data;      // PPD data for job
  FILE *devout;
  unsigned char *pixels;


  job_data = (ps_job_data_t *)papplJobGetData(job);
  devout = job_data->device_file;

  // If we got too few raster lines pad with blank lines
  if (job_data->line_count < options->header.cupsHeight)
  {
    pixels = (unsigned char *)malloc(options->header.cupsBytesPerLine);
    if (options->header.cupsColorSpace == CUPS_CSPACE_K ||
	options->header.cupsColorSpace == CUPS_CSPACE_CMYK)
      memset(pixels, 0x00, options->header.cupsBytesPerLine);
    else
      memset(pixels, 0xff, options->header.cupsBytesPerLine);
    for (; job_data->line_count < options->header.cupsHeight;
	 job_data->line_count ++)
      ps_ascii85(devout, pixels, options->header.cupsBytesPerLine, 0);
    free (pixels);
  }

  // Flush out remaining bytes of the bitmap 
  ps_ascii85(devout, NULL, 0, 1);

  // Finish page and get it printed
  fprintf(devout, "grestore\n");
  fprintf(devout, "showpage\n");
  fprintf(devout, "%%%%PageTrailer\n");

  papplDeviceFlush(device);

  return (true);
}


//
// 'ps_rstartjob()' - Start a job.
//

static bool                     // O - `true` on success, `false` on failure
ps_rstartjob(
    pappl_job_t      *job,      // I - Job
    pappl_pr_options_t *options,// I - Job options
    pappl_device_t   *device)   // I - Device
{
  ps_job_data_t      *job_data;      // PPD data for job
  const char	     *job_name;
  int                nullfd;
  int                jobcanceled = 0;
  filter_data_t      filter_data;
  FILE               *devout;

  // Log function for output to device
  filter_data.logfunc = ps_job_log; // Job log function catching page counts
                                    // ("PAGE: XX YY" messages)
  filter_data.logdata = job;
  // Load PPD file and determine the PPD options equivalent to the job options
  job_data = ps_create_job_data(job, options);
  // The filter has no output, data is going directly to the device
  nullfd = open("/dev/null", O_RDWR);
  // Create file descriptor/pipe to which the functions of libppd can send
  // the data so that it gets passed on to the device
  job_data->device_fd = filterPOpen(ps_print_filter_function, -1, nullfd,
				    0, &jobcanceled, &filter_data, device,
				    &(job_data->device_pid));
  if (job_data->device_fd < 0)
    return (false);

  job_data->device_file = fdopen(job_data->device_fd, "w");
  devout = job_data->device_file;

  // Save data for the other raster callback functions
  papplJobSetData(job, job_data);

  // Print 1 bit per pixel for monochrome draft printing
  ps_one_bit_dither_on_draft(job, options);

  // DSC header
  job_name = papplJobGetName(job);

  ppdEmitJCL(job_data->ppd, devout, papplJobGetID(job),
	     papplJobGetUsername(job), job_name ? job_name : "Unknown");

  fputs("%!PS-Adobe-3.0\n", devout);
  fprintf(devout, "%%%%LanguageLevel: %d\n", job_data->ppd->language_level);
  fprintf(devout, "%%%%Creator: %s/%d.%d.%d.%d\n", SYSTEM_NAME,
	  SYSTEM_VERSION_ARR_0, SYSTEM_VERSION_ARR_1,
	  SYSTEM_VERSION_ARR_2, SYSTEM_VERSION_ARR_3);
  if (job_name)
  {
    fputs("%%Title: ", devout);
    while (*job_name)
    {
      if (*job_name >= 0x20 && *job_name < 0x7f)
        fputc(*job_name, devout);
      else
        fputc('?', devout);

      job_name ++;
    }
    fputc('\n', devout);
  }
  fprintf(devout, "%%%%BoundingBox: 0 0 %d %d\n",
	  options->header.PageSize[0], options->header.PageSize[1]);
  fputs("%%Pages: (atend)\n", devout);
  fputs("%%EndComments\n", devout);

  fputs("%%BeginProlog\n", devout);

  // Number of copies (uncollated and hardware only due to job
  // not being spooled and infinite job supported
  if (job_data->ppd->language_level == 1)
    fprintf(devout, "/#copies %d def\n", options->copies);
  else
    fprintf(devout, "<</NumCopies %d>>setpagedevice\n", options->copies);

  if (job_data->ppd->patches)
  {
    fputs("%%BeginFeature: *JobPatchFile 1\n", devout);
    fputs(job_data->ppd->patches, devout);
    fputs("\n%%EndFeature\n", devout);
  }
  ppdEmit(job_data->ppd, devout, PPD_ORDER_PROLOG);
  fputs("%%EndProlog\n", devout);

  fputs("%%BeginSetup\n", devout);
  ppdEmit(job_data->ppd, devout, PPD_ORDER_DOCUMENT);
  ppdEmit(job_data->ppd, devout, PPD_ORDER_ANY);
  fputs("%%EndSetup\n", devout);

  return (true);
}


//
// 'ps_rstartpage()' - Start a page.
//

static bool                      // O - `true` on success, `false` on failure
ps_rstartpage(
    pappl_job_t       *job,       // I - Job
    pappl_pr_options_t  *options, // I - Job options
    pappl_device_t    *device,    // I - Device
    unsigned          page)       // I - Page number
{
  ps_job_data_t          *job_data;      // PPD data for job
  FILE *devout;
  int bpc;


  job_data = (ps_job_data_t *)papplJobGetData(job);
  devout = job_data->device_file;
  job_data->line_count = 0;

  // Print 1 bit per pixel for monochrome draft printing
  ps_one_bit_dither_on_draft(job, options);

  // DSC header
  fprintf(devout, "%%%%Page: (%d) %d\n", page, page);
  fputs("%%BeginPageSetup\n", devout);
  ppdEmit(job_data->ppd, devout, PPD_ORDER_PAGE);
  fputs("%%EndPageSetup\n", devout);

  // Start raster image output
  fprintf(devout, "gsave\n");

  switch (options->header.cupsColorSpace)
  {
  case CUPS_CSPACE_RGB:
  case CUPS_CSPACE_SRGB:
  case CUPS_CSPACE_ADOBERGB:
    fprintf(devout, "/DeviceRGB setcolorspace\n");
    break;

  case CUPS_CSPACE_CMYK:
    fprintf(devout, "/DeviceCMYK setcolorspace\n");
    break;

  default:
  case CUPS_CSPACE_K:
  case CUPS_CSPACE_W:
  case CUPS_CSPACE_SW:
    fprintf(devout, "/DeviceGray setcolorspace\n");
    break;
  }

  fprintf(devout, "%d %d scale\n",
	  options->header.PageSize[0], options->header.PageSize[1]);
  fprintf(devout, "<< \n"
	 "/ImageType 1\n"
	 "/Width %d\n"
	 "/Height %d\n"
	 "/BitsPerComponent %d\n",
	  options->header.cupsWidth, options->header.cupsHeight,
	  options->header.cupsBitsPerColor);

  switch (options->header.cupsColorSpace)
  {
  case CUPS_CSPACE_RGB:
  case CUPS_CSPACE_SRGB:
  case CUPS_CSPACE_ADOBERGB:
    fprintf(devout, "/Decode [0 1 0 1 0 1]\n");
    break;

  case CUPS_CSPACE_CMYK:
    fprintf(devout, "/Decode [0 1 0 1 0 1 0 1]\n");
    break;

  case CUPS_CSPACE_SW:
    fprintf(devout, "/Decode [0 1]\n");
    break;

  default:
  case CUPS_CSPACE_K:
  case CUPS_CSPACE_W:
    fprintf(devout, "/Decode [1 0]\n");
    break;
  }

  fprintf(devout, "/DataSource currentfile /ASCII85Decode filter\n");

  fprintf(devout, "/ImageMatrix [%d 0 0 %d 0 %d]\n",
	  options->header.cupsWidth, -1 * options->header.cupsHeight,
	  options->header.cupsHeight);
  fprintf(devout, ">> image\n");

  return (true);
}


//
// 'ps_rwriteline()' - Write a line.
//

static bool				// O - `true` on success, `false` on failure
ps_rwriteline(
    pappl_job_t         *job,		// I - Job
    pappl_pr_options_t  *options,	// I - Job options
    pappl_device_t      *device,	// I - Device
    unsigned            y,		// I - Line number
    const unsigned char *pixels)	// I - Line
{
  ps_job_data_t         *job_data;      // PPD data for job
  FILE *devout;
  int out_length;

  job_data = (ps_job_data_t *)papplJobGetData(job);
  devout = job_data->device_file;

  if (job_data->line_count < options->header.cupsHeight)
    ps_ascii85(devout, pixels, options->header.cupsBytesPerLine, 0);
  job_data->line_count ++;

  return (true);
}


//
// 'ps_setup()' - Setup PostScript driver.
//

static void
ps_setup(pappl_system_t *system)      // I - System
{
  int              i, j, k;
  const char       *col_paths_env;
  char             *ptr1, *ptr2;
  char             *generic_ppd, *mfg_mdl, *mdl, *dev_id;
  cups_array_t     *ppd_paths,
                   *ppd_collections;
  ppd_collection_t *col;
  ps_ppd_path_t    *ppd_path;
  int              num_options = 0;
  cups_option_t    *options = NULL;
  cups_array_t     *ppds;
  ppd_info_t       *ppd;
  char             buf1[1024], buf2[1024];
  int              pre_normalized;
  pappl_pr_driver_t swap;
  ps_filter_data_t *ps_filter_data,
                   *pdf_filter_data;

  //
  // Create PPD collection idex data structure
  //

  ppd_paths = cupsArrayNew(ps_compare_ppd_paths, NULL);
  ppd_collections = cupsArrayNew(NULL, NULL);

  //
  // Build PPD list from all repositories
  //

  if ((col_paths_env = getenv("PPD_PATHS")) != NULL)
  {
    strncpy(buf1, col_paths_env, sizeof(buf1));
    ptr1 = buf1;
    while (ptr1 && *ptr1)
    {
      ptr2 = strchr(ptr1, ':');
      if (ptr2)
	*ptr2 = '\0';
      col = (ppd_collection_t *)calloc(1, sizeof(ppd_collection_t));
      col->name = NULL;
      col->path = ptr1;
      cupsArrayAdd(ppd_collections, col);
      if (ptr2)
	ptr1 = ptr2 + 1;
      else
	ptr1 = NULL;
    }
  }
  else
    for (i = 0; i < sizeof(col_paths)/sizeof(col_paths[0]); i ++)
    {
      col = (ppd_collection_t *)calloc(1, sizeof(ppd_collection_t));
      col->name = NULL;
      col->path = (char *)col_paths[i];
      cupsArrayAdd(ppd_collections, col);
    }

  ppds = ppdCollectionListPPDs(ppd_collections, 0,
			       num_options, options,
			       (filter_logfunc_t)papplLog, system);
  cupsArrayDelete(ppd_collections);

  //
  // Create driver list from the PPD list and submit it
  //
  
  if (ppds)
  {
    i = 0;
    num_drivers = cupsArrayCount(ppds);
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Found %d PPD files.", num_drivers);
    num_drivers ++; // For "Auto"
    // Search for a generic PPD to use as generic PostScript driver
    generic_ppd = NULL;
    for (ppd = (ppd_info_t *)cupsArrayFirst(ppds);
	 ppd;
	 ppd = (ppd_info_t *)cupsArrayNext(ppds))
    {
      if (!strcasecmp(ppd->record.make, "Generic") ||
	  !strncasecmp(ppd->record.make_and_model, "Generic", 7) ||
	  !strncasecmp(ppd->record.products[0], "Generic", 7))
      {
	generic_ppd = ppd->record.name;
	break;
      }
    }
    if (generic_ppd)
      papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	       "Found generic PPD file: %s", generic_ppd);
    else
      papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	       "No generic PPD file found, "
	       "Printer Application will only support printers "
	       "explicitly supported by the PPD files");
    // Create driver indices
    drivers = (pappl_pr_driver_t *)calloc(num_drivers + PPD_MAX_PROD,
					  sizeof(pappl_pr_driver_t));
    // Add entry for "auto" driver selection (not yet implemented)
    drivers[i].name = strdup("auto");
    drivers[i].description = strdup("Automatic Selection");
    drivers[i].device_id = strdup("CMD:POSTSCRIPT;");
    drivers[i].extension = strdup(" auto");
    i ++;
    if (generic_ppd)
    {
      drivers[i].name = strdup("generic");
      drivers[i].description = strdup("Generic PostScript Printer");
      drivers[i].device_id = strdup("CMD:POSTSCRIPT;");
      drivers[i].extension = strdup(" generic");
      i ++;
      ppd_path = (ps_ppd_path_t *)calloc(1, sizeof(ps_ppd_path_t));
      ppd_path->driver_name = strdup("generic");
      ppd_path->ppd_path = strdup(generic_ppd);
      cupsArrayAdd(ppd_paths, ppd_path);
    }
    for (ppd = (ppd_info_t *)cupsArrayFirst(ppds);
	 ppd;
	 ppd = (ppd_info_t *)cupsArrayNext(ppds))
    {
      if (!generic_ppd || strcmp(ppd->record.name, generic_ppd))
      {
	// Note: The last entry in the product list is the ModelName of the
	// PPD not an actual Product entry. Therefore we ignore it
	// (Hidden feature of ppdCollectionListPPDs())
        for (j = -1; j < PPD_MAX_PROD - 1; j ++)
	{
	  // End of product list;
          if (j >= 0 &&
	      (!ppd->record.products[j][0] || !ppd->record.products[j + 1][0]))
            break;
	  // If there is only 1 product, ignore it, it is either the
	  // model of the PPD itself or something weird
	  if (j == 0 &&
	      (!ppd->record.products[1][0] || !ppd->record.products[2][0]))
	    break;
	  pre_normalized = 0;
	  dev_id = NULL;
	  if (j < 0)
	  {
	    // Model of PPD itself
	    if (ppd->record.device_id[0] &&
		(strstr(ppd->record.device_id, "MFG:") ||
		 strstr(ppd->record.device_id, "MANUFACTURER:")) &&
		(strstr(ppd->record.device_id, "MDL:") ||
		 strstr(ppd->record.device_id, "MODEL:")) &&
		!strstr(ppd->record.device_id, "MDL:hp_") &&
		!strstr(ppd->record.device_id, "MDL:hp-") &&
		!strstr(ppd->record.device_id, "MDL:HP_") &&
		!strstr(ppd->record.device_id, "MODEL:hp2") &&
		!strstr(ppd->record.device_id, "MODEL:hp3") &&
		!strstr(ppd->record.device_id, "MODEL:hp9") &&
		!strstr(ppd->record.device_id, "MODEL:HP2"))
	    {
	      // Cnonvert device ID to make/model string, so that we can add
	      // the language for building final index strings
	      mfg_mdl = ieee1284NormalizeMakeAndModel(ppd->record.device_id,
						      NULL,
						      IEEE1284_NORMALIZE_HUMAN,
						      buf2, sizeof(buf2),
						      NULL, NULL);
	      pre_normalized = 1;
	    }
	    else if (ppd->record.products[0][0])
	      mfg_mdl = ppd->record.products[0];
	    else
	      mfg_mdl = ppd->record.make_and_model;
	    if (ppd->record.device_id[0])
	      dev_id = ppd->record.device_id;
	  }
	  else
	    // Extra models in list of products
	    mfg_mdl = ppd->record.products[j];
	  ppd_path = (ps_ppd_path_t *)calloc(1, sizeof(ps_ppd_path_t));
	  // Base make/model/language string to generate the needed index
	  // strings
	  snprintf(buf1, sizeof(buf1) - 1, "%s (%s)",
		   mfg_mdl, ppd->record.languages[0]);
	  // IPP-compatible string as driver name
	  drivers[i].name =
	    strdup(ieee1284NormalizeMakeAndModel(buf1, ppd->record.make,
						 IEEE1284_NORMALIZE_IPP,
						 buf2, sizeof(buf2),
						 NULL, NULL));
	  ppd_path->driver_name = strdup(drivers[i].name);
	  // Path to grab PPD from repositories
	  ppd_path->ppd_path = strdup(ppd->record.name);
	  cupsArrayAdd(ppd_paths, ppd_path);
	  // Human-readable string to appear in the driver drop-down
	  if (pre_normalized)
	    drivers[i].description = strdup(buf1);
	  else
	    drivers[i].description =
	      strdup(ieee1284NormalizeMakeAndModel(buf1, ppd->record.make,
						   IEEE1284_NORMALIZE_HUMAN,
						   buf2, sizeof(buf2),
						   NULL, NULL));
	  // We only register device IDs actually found in the PPD files,
	  // PPDs without explicit device ID get matched by the
	  // ieee1284NormalizeMakeAndModel() function
	  drivers[i].device_id = (dev_id ? strdup(dev_id) : strdup(""));
	  // List sorting index with padded numbers (typos in example intended)
	  // "LaserJet 3P" < "laserjet 4P" < "Laserjet3000P" < "LaserJet 4000P"
	  drivers[i].extension =
	    strdup(ieee1284NormalizeMakeAndModel(buf1, ppd->record.make,
					IEEE1284_NORMALIZE_COMPARE |
					IEEE1284_NORMALIZE_LOWERCASE |
					IEEE1284_NORMALIZE_SEPARATOR_SPACE |
					IEEE1284_NORMALIZE_PAD_NUMBERS,
					buf2, sizeof(buf2),
					NULL, NULL));
	  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		   "File: %s; Printer (%d): %s; --> Entry %d: Driver %s; "
		   "Description: %s; Device ID: %s; Sorting index: %s",
		   ppd_path->ppd_path, j, buf1, i, drivers[i].name,
		   drivers[i].description, drivers[i].device_id,
		   (char *)(drivers[i].extension));
	  // Sort the new entry into the list via the extension
	  for (k = i;
	       k > 0 &&
	       strcmp((char *)(drivers[k - 1].extension),
		      (char *)(drivers[k].extension)) > 0;
	       k --)
	  {
	    swap = drivers[k - 1];
	    drivers[k - 1] = drivers[k];
	    drivers[k] = swap;
	  }
	  // Check for duplicates
	  if (k > 0 &&
	      (strcmp(drivers[k - 1].name, drivers[k].name) == 0 ||
	       strcasecmp(drivers[k - 1].description,
			  drivers[k].description) == 0))
	  {
	    // Remove the duplicate
	    // We do not count the freeable memory here as in the end
	    // we adjust the allocated memory anyway
	    memmove(&drivers[k], &drivers[k + 1],
		    (i - k) * sizeof(pappl_pr_driver_t));
	    i --;
	    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		     "DUPLICATE REMOVED!");
	  }
	  // Next position in the list
	  i ++;
	}
	// Add memory for PPD with multiple product entries
	num_drivers += j;
	drivers = (pappl_pr_driver_t *)reallocarray(drivers,
						    num_drivers +
						    PPD_MAX_PROD,
						    sizeof(pappl_pr_driver_t));
      }
      free(ppd);
    }
    cupsArrayDelete(ppds);

    // Final adjustment of allocated memory
    drivers = (pappl_pr_driver_t *)reallocarray(drivers, i,
						sizeof(pappl_pr_driver_t));
    num_drivers = i;
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Created %d driver entries.", num_drivers);
  }
  else
    papplLog(system, PAPPL_LOGLEVEL_FATAL, "No PPD files found.");

  papplSystemSetPrinterDrivers(system, num_drivers, drivers,
			       ps_callback, ppd_paths);

  //
  // Add filters for the different input data formats
  //

  ps_filter_data =
    (ps_filter_data_t *)calloc(1, sizeof(ps_filter_data_t));
  ps_filter_data->filter_function = pstops;
  ps_filter_data->filter_parameters = "PS";
  papplSystemAddMIMEFilter(system,
			   "application/postscript",
			   "application/vnd.printer-specific",
			   ps_filter, ps_filter_data);

  pdf_filter_data =
    (ps_filter_data_t *)calloc(1, sizeof(ps_filter_data_t));
  pdf_filter_data->filter_function = pdftops;
  pdf_filter_data->filter_parameters = "PDF";
  papplSystemAddMIMEFilter(system,
			   "application/pdf",
			   "application/vnd.printer-specific",
			   ps_filter, pdf_filter_data);
}


//
// 'ps_status()' - Get printer status.
//

static bool                   // O - `true` on success, `false` on failure
ps_status(
    pappl_printer_t *printer) // I - Printer
{
  //char	driver_name[256];     // Driver name

  (void)printer;
  
  //papplPrinterGetDriverName(printer);

  // Use commandtops CUPS filter code to check status here (ink levels, ...)
  // XXX
  
  return (true);
}


//
// 'system_cb()' - System callback.
//

pappl_system_t *			// O - New system object
system_cb(int           num_options,	// I - Number of options
	  cups_option_t *options,	// I - Options
	  void          *data)		// I - Callback data
{
  pappl_system_t	*system;	// System object
  const char		*val,		// Current option value
			*hostname,	// Hostname, if any
			*logfile,	// Log file, if any
			*system_name;	// System name, if any
  pappl_loglevel_t	loglevel;	// Log level
  int			port = 0;	// Port number, if any
  pappl_soptions_t	soptions = PAPPL_SOPTIONS_MULTI_QUEUE |
                                   PAPPL_SOPTIONS_WEB_INTERFACE |
                                   PAPPL_SOPTIONS_WEB_LOG |
                                   PAPPL_SOPTIONS_WEB_NETWORK |
                                   PAPPL_SOPTIONS_WEB_SECURITY |
                                   PAPPL_SOPTIONS_WEB_TLS;
					// System options
  static pappl_version_t versions[1] =	// Software versions
  {
    { SYSTEM_NAME, "", SYSTEM_VERSION_STR,
      { SYSTEM_VERSION_ARR_0, SYSTEM_VERSION_ARR_1,
	SYSTEM_VERSION_ARR_2, SYSTEM_VERSION_ARR_3 } }
  };

  // Parse options...
  if ((val = cupsGetOption("log-level", num_options, options)) != NULL)
  {
    if (!strcmp(val, "fatal"))
      loglevel = PAPPL_LOGLEVEL_FATAL;
    else if (!strcmp(val, "error"))
      loglevel = PAPPL_LOGLEVEL_ERROR;
    else if (!strcmp(val, "warn"))
      loglevel = PAPPL_LOGLEVEL_WARN;
    else if (!strcmp(val, "info"))
      loglevel = PAPPL_LOGLEVEL_INFO;
    else if (!strcmp(val, "debug"))
      loglevel = PAPPL_LOGLEVEL_DEBUG;
    else
    {
      fprintf(stderr, "ps_printer_app: Bad log-level value '%s'.\n", val);
      return (NULL);
    }
  }
  else
    loglevel = PAPPL_LOGLEVEL_UNSPEC;

  logfile     = cupsGetOption("log-file", num_options, options);
  hostname    = cupsGetOption("server-hostname", num_options, options);
  system_name = cupsGetOption("system-name", num_options, options);

  if ((val = cupsGetOption("server-port", num_options, options)) != NULL)
  {
    if (!isdigit(*val & 255))
    {
      fprintf(stderr, "ps_printer_app: Bad server-port value '%s'.\n", val);
      return (NULL);
    }
    else
      port = atoi(val);
  }

  // Create the system object...
  if ((system =
       papplSystemCreate(soptions,
			 system_name ? system_name : SYSTEM_NAME,
			 port,
			 "_print,_universal",
			 cupsGetOption("spool-directory", num_options, options),
			 logfile ? logfile : "-",
			 loglevel,
			 cupsGetOption("auth-service", num_options, options),
			 /* tls_only */false)) ==
      NULL)
    return (NULL);

  papplSystemAddListeners(system, NULL);
  papplSystemSetHostname(system, hostname);
  ps_setup(system);

  papplSystemSetFooterHTML(system,
                           "Copyright &copy; 2020 by Till Kamppeter. "
                           "Provided under the terms of the "
			   "<a href=\"https://www.apache.org/licenses/LICENSE-2.0\">"
			   "Apache License 2.0</a>.");
  papplSystemSetSaveCallback(system, (pappl_save_cb_t)papplSystemSaveState,
			     (void *)"/tmp/ps_printer_app.state");
  papplSystemSetVersions(system,
			 (int)(sizeof(versions) / sizeof(versions[0])),
			 versions);

  if (!papplSystemLoadState(system, "/tmp/ps_printer_app.state"))
    papplSystemSetDNSSDName(system,
			    system_name ? system_name : SYSTEM_NAME);

  return (system);
}
