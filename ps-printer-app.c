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
#include <cups/dir.h>
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

typedef struct ps_driver_extension_s	// Driver data extension
{
  ppd_file_t *ppd;                      // PPD file loaded from collection
  const char *vendor_ppd_options[PAPPL_MAX_VENDOR]; // Names of the PPD options
                                        // represented as vendor options;
  // Special properties taken from the PPD file
  bool       defaults_pollable,         // Are option defaults pollable? 
             installable_options,       // Is there an "Installable Options"
                                        // group?
             installable_pollable,      // "Installable Options" pollable?
             updated;                   // Is the driver data updated for
                                        // "Installable Options" changes?
} ps_driver_extension_t;

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
#define SYSTEM_PACKAGE_NAME "ps-printer-app"
#define SYSTEM_VERSION_STR "1.0"
#define SYSTEM_VERSION_ARR_0 1
#define SYSTEM_VERSION_ARR_1 0
#define SYSTEM_VERSION_ARR_2 0
#define SYSTEM_VERSION_ARR_3 0

// System directories

#define SYSTEM_STATE_DIR "/var/lib/" SYSTEM_PACKAGE_NAME
#define SYSTEM_DATA_DIR "/usr/share/" SYSTEM_PACKAGE_NAME

// State file

#define STATE_FILE SYSTEM_STATE_DIR "/" SYSTEM_PACKAGE_NAME ".state"

// Test page

#define TESTPAGE "testpage.ps"
#define TESTPAGEDIR SYSTEM_DATA_DIR

// PPD collections used as drivers

static const char * const col_paths[] =   // PPD collection dirs
{
 "/usr/lib/cups/driver",
 "/usr/share/ppd",
 SYSTEM_STATE_DIR "/ppd"
};

static  int               num_drivers = 0; // Number of drivers (from the PPDs)
static  pappl_pr_driver_t *drivers = NULL; // Driver index (for menu and
                                           // auto-add)
cups_array_t              *ppd_paths = NULL, // List of the paths to each PPD
                          *ppd_collections;// List of all directories providing
                                           // PPD files
static  char              extra_ppd_dir[1024] = ""; // Directory where PPDs
                                           // added by the user are held
static  char              ppd_dirs_env[1024]; // Environment variable PPD_DIRS
                                           // with the PPD directories


//
// Local functions...
//

static const char *ps_autoadd(const char *device_info, const char *device_uri,
			      const char *device_id, void *data);
static void   ps_ascii85(FILE *outputfp, const unsigned char *data, int length,
			 int last_data);
static int    ps_compare_ppd_paths(void *a, void *b, void *data); 
static ps_job_data_t *ps_create_job_data(pappl_job_t *job,
					 pappl_pr_options_t *job_options);
static void   ps_driver_delete(pappl_printer_t *printer,
			       pappl_pr_driver_data_t *driver_data);
static bool   ps_driver_setup(pappl_system_t *system, const char *driver_name,
			      const char *device_uri, const char *device_id,
			      pappl_pr_driver_data_t *driver_data,
			      ipp_t **driver_attrs, void *data);
bool          ps_filter(pappl_job_t *job, pappl_device_t *device, void *data);
static void   ps_free_job_data(ps_job_data_t *job_data);
static bool   ps_have_force_gray(ppd_file_t *ppd,
				 const char **optstr, const char **choicestr);
static void   ps_identify(pappl_printer_t *printer,
			  pappl_identify_actions_t actions,
			  const char *message);
static int    ps_job_is_canceled(void *data);
static void   ps_job_log(void *data, filter_loglevel_t level,
			 const char *message, ...);
static void   ps_media_col(pwg_size_t *pwg_size, const char *def_source,
			   const char *def_type, int left_offset,
			   int top_offset, pappl_media_tracking_t tracking,
			   pappl_media_col_t *col);
static void   ps_one_bit_dither_on_draft(pappl_job_t *job,
					 pappl_pr_options_t *options);
int           ps_print_filter_function(int inputfd, int outputfd,
				       int inputseekable, filter_data_t *data,
				       void *parameters);
static int    ps_poll_device_option_defaults(pappl_printer_t *printer,
					     bool installable,
					     cups_option_t **defaults);
static void   ps_printer_web_device_config(pappl_client_t *client,
					   pappl_printer_t *printer);
static void   ps_printer_extra_setup(pappl_printer_t *printer, void *data);
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
static void   ps_system_web_add_ppd(pappl_client_t *client,
				    pappl_system_t *system);
static bool   ps_status(pappl_printer_t *printer);
static const char *ps_testpage(pappl_printer_t *printer, char *buffer,
			       size_t bufsize);
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
			ps_autoadd,     // Printer auto-addition callback
			NULL,           // Setup callback for selected driver
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
  int           score, best_score = 0,
                best = -1;


  (void)device_info;
  (void)device_uri;
  (void)data;

  if (device_id == NULL || num_drivers == 0 || drivers == NULL)
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
    // Normalize device ID to format of driver name and match
    ieee1284NormalizeMakeAndModel(device_id, NULL,
				  IEEE1284_NORMALIZE_IPP,
				  buf, sizeof(buf),
				  NULL, NULL);

    // Match make and model with device ID of driver list entry
    for (i = 1; i < num_drivers; i ++)
    {
      score = 0;

      // Match make and model with device ID of driver list entry
      if (drivers[i].device_id[0] &&
	  (num_ddid = papplDeviceParseID(drivers[i].device_id, &ddid)) > 0 &&
	  ddid != NULL)
      {
	if ((dmfg = cupsGetOption("MANUFACTURER", num_ddid, ddid)) == NULL)
	  dmfg = cupsGetOption("MFG", num_ddid, ddid);
	if ((dmdl = cupsGetOption("MODEL", num_ddid, ddid)) == NULL)
	  dmdl = cupsGetOption("MDL", num_ddid, ddid);
	if (dmfg && dmdl &&
	    strcasecmp(mfg, dmfg) == 0 &&
	    strcasecmp(mdl, dmdl) == 0)
	  // Match
	  score += 2;
	cupsFreeOptions(num_ddid, ddid);
      }

      // Match normalized device ID with driver name
      if (score == 0 && strncmp(buf, drivers[i].name, strlen(buf)) == 0)
	// Match
	score += 1;

      // PPD must at least match make and model to get considered
      if (score == 0)
	continue;

      // User-added? Prioritize, as if the user adds something, he wants
      // to use it
      if (strstr(drivers[i].name, "-user-added"))
	score += 32;

      // PPD matches user's/system's language?
      // To be added when PAPPL supports internationalization XXX
      // score + 8 for 2-char language
      // score + 16 for 5-char language/country

      // PPD is English language version?
      if (!strcmp(drivers[i].name + strlen(drivers[i].name) - 4, "--en") ||
	  !strncmp(drivers[i].name + strlen(drivers[i].name) - 7, "--en-", 5))
	score += 4;

      // Better match than the previous one?
      if (score > best_score)
      {
	best_score = score;
	best = i;
      }
    }
  }

  // Found at least one match? Take the best one
  if (best >= 0)
    ret = drivers[best].name;
  // PostScript printer but none of the PPDs match? Assign the generic PPD
  // if we have one
  else if (strcasecmp(drivers[0].name, "generic"))
    ret = "generic";
  else
    ret = NULL;

 done:

  // Clean up
  cupsFreeOptions(num_did, did);

  return (ret);
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
  ps_driver_extension_t *extension;
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
  char                  *ptr;
  pappl_printer_t       *printer = papplJobGetPrinter(job);

  //
  // Load the printer's assigned PPD file, mark the defaults, and create the
  // cache
  //

  job_data = (ps_job_data_t *)calloc(1, sizeof(ps_job_data_t));

  papplPrinterGetDriverData(printer, &driver_data);
  extension = (ps_driver_extension_t *)driver_data.extension;
  job_data->ppd = extension->ppd;
  pc = job_data->ppd->cache;

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
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: %s",
	      pc->source_option ? pc->source_option : "InputSlot");
  if ((choicestr = ppdCacheGetInputSlot(pc, NULL,
					job_options->media.source)) !=
      NULL)
    job_data->num_options = cupsAddOption(pc->source_option, choicestr,
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

  // OutputBin/output-bin
  if ((count = pc->num_bins) > 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: OutputBin");
    val = job_options->output_bin;
    for (i = 0, pwg_map = pc->bins; i < count; i ++, pwg_map ++)
      if (!strcmp(pwg_map->pwg, val))
	choicestr = pwg_map->ppd;
    if (choicestr != NULL)
      job_data->num_options = cupsAddOption("OutputBin", choicestr,
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
  // Add vendor-specific PPD options
  //

  for (i = 0;
       i < (extension->installable_options ?
	    driver_data.num_vendor - 1 :
	    driver_data.num_vendor);
       i ++)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: %s",
		extension->vendor_ppd_options[i]);
    if ((attr = papplJobGetAttribute(job, driver_data.vendor[i])) == NULL)
    {
      snprintf(buf, sizeof(buf), "%s-default", driver_data.vendor[i]);
      attr = ippFindAttribute(driver_attrs, buf, IPP_TAG_ZERO);
    }

    choicestr = NULL;
    if (attr)
    {
      ptr = strdup(ippGetString(attr, 0, NULL));
      snprintf(buf, sizeof(buf), "%s-supported", driver_data.vendor[i]);
      attr = ippFindAttribute(driver_attrs, buf, IPP_TAG_ZERO);
      if (attr == NULL)
      {
	// Should never happen
	papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
		    "  IPP Option not correctly registered (bug), "
		    "skipping ...");
	continue;
      }
      option = ppdFindOption(job_data->ppd, extension->vendor_ppd_options[i]);
      if (option == NULL)
      {
	// Should never happen
	papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
		    "  PPD Option not correctly registered (bug), "
		    "skipping ...");
	continue;
      }
      for (j = 0;
	   j < (ippGetValueTag(attr) == IPP_TAG_BOOLEAN ?
		2 : ippGetCount(attr));
	   j ++)
      {
	ppdPwgUnppdizeName(option->choices[j].text, buf, sizeof(buf), NULL);
	if (!strcasecmp(buf, ptr))
	{
	  choicestr = option->choices[j].choice;
	  break;
	}
      }
      if (choicestr != NULL &&
	  !ppdInstallableConflict(job_data->ppd,
				  extension->vendor_ppd_options[i], choicestr))
	job_data->num_options = cupsAddOption(extension->vendor_ppd_options[i],
					      choicestr, job_data->num_options,
					      &(job_data->options));
      free(ptr);
    }
  }

  // Collate (will only be used with PDF or PostScript input)
  if ((attr =
       papplJobGetAttribute(job,
			    "multiple-document-handling")) != NULL)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: Collate");
    ptr = (char *)ippGetString(attr, 0, NULL);
    if (strstr(ptr, "uncollate"))
      choicestr = "False";
    else if (strstr(ptr, "collate"))
      choicestr = "True";
    job_data->num_options = cupsAddOption("Collate", choicestr,
					  job_data->num_options,
					  &(job_data->options));
  }

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
// 'ps_driver_delete()' - Free dynamic data structures of the driver when
//                        removing a printer.
//

static void   ps_driver_delete(
    pappl_printer_t *printer,              // I - Printer to be removed
    pappl_pr_driver_data_t *driver_data)   // I - Printer's driver data
{
  int               i;
  ps_driver_extension_t *extension;


  papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		  "Freeing memory from driver data");

  extension = (ps_driver_extension_t *)driver_data->extension;

  // PPD file
  ppdClose(extension->ppd);

  // Media source
  for (i = 0; i < driver_data->num_source; i ++)
    free((char *)(driver_data->source[i]));

  // Media type
  for (i = 0; i < driver_data->num_type; i ++)
    free((char *)(driver_data->type[i]));

  // Media size
  for (i = 0; i < driver_data->num_media; i ++)
    free((char *)(driver_data->media[i]));

  // Output bins
  for (i = 0; i < driver_data->num_bin; i ++)
    free((char *)(driver_data->bin[i]));

  // Vendor options
  for (i = 0; i < driver_data->num_vendor; i ++)
  {
    free((char *)(driver_data->vendor[i]));
    if (extension->vendor_ppd_options[i])
      free((char *)(extension->vendor_ppd_options[i]));
  }

  // Extension
  free(extension);
}


//
// 'ps_driver_setup()' - PostScript driver setup callback.
//
//                       Runs in two modes: Init and Update
//
//                       It runs in Init mode when the
//                       driver_data->extension is still NULL, meaning
//                       that the extension structure is not yet
//                       defined. This is the case when the printer
//                       data structure is created on startup or on
//                       adding a printer. The we load and read the
//                       PPD and enter the properties into the driver
//                       data structure, not taking into account any
//                       user defaults or accessory settings.
//
//                       When called again with the data structure
//                       already present, it runs in Update mode,
//                       applying yser defaults and mosifying the data
//                       structure if the user changed the
//                       configuration of installable accessories.
//                       This mode is triggered when called by the
//                       ps_status() callback which in turn is called
//                       after completely loading the printer's state
//                       file entry or when doing changes on the
//                       "Device Settings" web interface page.
//

static bool				   // O - `true` on success, `false`
                                           //     on failure
ps_driver_setup(
    pappl_system_t       *system,	   // I - System
    const char           *driver_name,     // I - Driver name
    const char           *device_uri,	   // I - Device URI
    const char           *device_id,	   // I - Device ID
    pappl_pr_driver_data_t *driver_data,   // O - Driver data
    ipp_t                **driver_attrs,   // O - Driver attributes
    void                 *data)	           // I - Callback data
{
  int          i, j, k, l;                 // Looping variables
  bool         update;                     // Are we updating the data
                                           // structure and not freshly
                                           // creating it?
  ps_driver_extension_t *extension;
  cups_array_t *ppd_paths = (cups_array_t *)data;
  ps_ppd_path_t *ppd_path,
               search_ppd_path;
  ppd_file_t   *ppd = NULL;		   // PPD file loaded from collection
  ppd_cache_t  *pc;
  ipp_attribute_t *attr;
  int          num_options;
  cups_option_t *options,
               *opt;
  char         *keyword;
  ipp_res_t    units;			   // Resolution units
  const char   *def_source,
               *def_type;
  char         *def_bin;
  pwg_size_t   *def_media;
  int          def_res_x, def_res_y,
               def_left, def_right, def_top, def_bottom;
  ppd_group_t  *group;
  ppd_option_t *option;
  ppd_choice_t *choice, *def_choice;
  ppd_attr_t   *ppd_attr;
  pwg_map_t    *pwg_map;
  pwg_size_t   *pwg_size;
  ppd_pwg_finishings_t *finishings;
  pappl_media_col_t tmp_col;
  int          count;
  bool         pollable;
  char         buf[1024],
               ipp_opt[80],
               ipp_supported[128],
               ipp_default[128],
               ipp_choice[80];
  char         **choice_list;
  int          default_choice,
               first_choice;
  const char * const pappl_handled_options[] =
  {
   "PageSize",
   "PageRegion",
   "InputSlot",
   "MediaType",
   "Resolution",
   "ColorModel",
   "OutputBin",
   "Duplex",
   NULL
  };


  if (!driver_data || !driver_attrs)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Driver callback called without required information.");
    return (false);
  }

  if (driver_data->extension == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Initializing driver data for driver \"%s\"", driver_name);

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
      papplLog(system, PAPPL_LOGLEVEL_INFO,
	       "Automatic printer driver selection for device with URI \"%s\" "
	       "and device ID \"%s\" ...", device_uri, device_id);
      search_ppd_path.driver_name = ps_autoadd(NULL, device_uri, device_id,
					       NULL);
      if (search_ppd_path.driver_name)
	papplLog(system, PAPPL_LOGLEVEL_INFO,
		 "Automatically selected driver \"%s\".",
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
		 "For the printer driver \"%s\" got auto-selected which does "
		 "not exist in this Printer Application.",
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
      int		line;		// Line number in file

      err = ppdLastError(&line);
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "PPD %s: %s on line %d", ppd_path->ppd_path,
	       ppdErrorString(err), line);
      return (false);
    }

    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Using PPD %s: %s", ppd_path->ppd_path, ppd->nickname);

    ppdMarkDefaults(ppd);

    if ((pc = ppdCacheCreateWithPPD(ppd)) != NULL)
      ppd->cache = pc;

    //
    // Populate driver data record
    //

    // Callback functions end general properties
    driver_data->extension =
      (ps_driver_extension_t *)calloc(1, sizeof(ps_driver_extension_t));
    extension = (ps_driver_extension_t *)driver_data->extension;
    extension->ppd                  = ppd;
    extension->defaults_pollable    = false;
    extension->installable_options  = false;
    extension->installable_pollable = false;
    extension->updated              = false;
    driver_data->delete_cb          = ps_driver_delete;
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
    driver_data->testpage_cb        = ps_testpage;
    driver_data->format             = "application/vnd.printer-specific";
    driver_data->orient_default     = IPP_ORIENT_NONE;

    // Make and model
    strncpy(driver_data->make_and_model,
	    ppd->nickname,
	    sizeof(driver_data->make_and_model));

    // We are in Init mode
    update = false;
  }
  else
  {
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Updating driver data for %s", driver_data->make_and_model);
    extension = (ps_driver_extension_t *)driver_data->extension;
    ppd = extension->ppd;
    pc = ppd->cache;
    extension->updated = true;

    // We are in Update mode
    update = true;
  }

  // Note that we take into account option choice conflicts with the
  // configuration of installable accessories only in Update mode,
  // this way all options and choices are available after first
  // initialization (Init mode) so that all user defaults loaded from
  // the state file get accepted.
  //
  // Only at the end of the printer entry in the state file the
  // accessory configuration gets read. After that we re-run in Update
  // mode to correct the options and choices for the actual accessory
  // configuration.

  // Get settings of the "Installable Options" from the previous session
  if ((attr = ippFindAttribute(*driver_attrs, "installable-options-default",
			       IPP_TAG_ZERO)) != NULL &&
      ippAttributeString(attr, buf, sizeof(buf)) > 0)
  {
    options = NULL;
    num_options = cupsParseOptions(buf, 0, &options);
    ppdMarkOptions(ppd, num_options, options);
  }

  // Print speed in pages per minute (PPDs do not show different values for
  // Grayscale and Color)
  driver_data->ppm = ppd->throughput;
  if (driver_data->ppm <= 1)
    driver_data->ppm = 1;
  if (ppd->color_device)
    driver_data->ppm_color = driver_data->ppm;
  else
    driver_data->ppm_color = 0;

  // Properties not supported by the PPD
  driver_data->has_supplies = false;
  driver_data->input_face_up = false;

  // Pages face-up or face-down in output bin?
  if (pc->num_bins > 0)
    driver_data->output_face_up =
      (strstr(pc->bins->pwg, "face-up") != NULL);
  else
    driver_data->output_face_up = false;

  // No orientation requested by default
  if (!update) driver_data->orient_default = IPP_ORIENT_NONE;

  // Supported color modes
  if (ppd->color_device)
  {
    driver_data->color_supported =
      PAPPL_COLOR_MODE_AUTO | PAPPL_COLOR_MODE_COLOR |
      PAPPL_COLOR_MODE_MONOCHROME;
    if (!update) driver_data->color_default = PAPPL_COLOR_MODE_AUTO;
  }
  else
  {
    driver_data->color_supported = PAPPL_COLOR_MODE_MONOCHROME;
    driver_data->color_default = PAPPL_COLOR_MODE_MONOCHROME;
  }

  // These parameters are usually not defined in PPDs but a standard IPP
  // options settable in the web interface
  if (!update)
  {
    driver_data->content_default = PAPPL_CONTENT_AUTO;
    driver_data->quality_default = IPP_QUALITY_NORMAL;
    driver_data->scaling_default = PAPPL_SCALING_AUTO;
  }

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
  if (!update) driver_data->sides_default = PAPPL_SIDES_ONE_SIDED;
  if (pc->sides_2sided_long &&
      !(update && ppdInstallableConflict(ppd, pc->sides_option,
					 pc->sides_2sided_long)))
  {
    driver_data->sides_supported |= PAPPL_SIDES_TWO_SIDED_LONG_EDGE;
    driver_data->duplex = PAPPL_DUPLEX_NORMAL;
    if (!update &&
	(choice = ppdFindMarkedChoice(ppd, pc->sides_option)) != NULL &&
	strcmp(choice->choice, pc->sides_2sided_long) == 0)
      driver_data->sides_default = PAPPL_SIDES_TWO_SIDED_LONG_EDGE;
  }
  if (pc->sides_2sided_short &&
      !(update && ppdInstallableConflict(ppd, pc->sides_option,
					 pc->sides_2sided_short)))
  {
    driver_data->sides_supported |= PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
    driver_data->duplex = PAPPL_DUPLEX_NORMAL;
    if (!update &&
	(choice = ppdFindMarkedChoice(ppd, pc->sides_option)) != NULL &&
	strcmp(choice->choice, pc->sides_2sided_short) == 0)
      driver_data->sides_default = PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
  }
  if (driver_data->sides_default & driver_data->sides_supported == 0)
  {
    driver_data->sides_default = PAPPL_SIDES_ONE_SIDED;
    if (pc->sides_option)
      ppdMarkOption(ppd, pc->sides_option, pc->sides_1sided);
  }

  // Finishings
  driver_data->finishings = PAPPL_FINISHINGS_NONE;
  for (finishings = (ppd_pwg_finishings_t *)cupsArrayFirst(pc->finishings);
       finishings;
       finishings = (ppd_pwg_finishings_t *)cupsArrayNext(pc->finishings))
  {
    if (update)
      for (i = finishings->num_options, opt = finishings->options; i > 0;
	   i --, opt ++)
	if (ppdInstallableConflict(ppd, opt->name, opt->value))
	  break;
    if (i > 0)
      continue;
    if (finishings->value == IPP_FINISHINGS_STAPLE)
      driver_data->finishings |= PAPPL_FINISHINGS_STAPLE;
    else  if (finishings->value == IPP_FINISHINGS_PUNCH)
      driver_data->finishings |= PAPPL_FINISHINGS_PUNCH;
    else if (finishings->value == IPP_FINISHINGS_TRIM)
      driver_data->finishings |= PAPPL_FINISHINGS_TRIM;
  }

  // Resolution
  driver_data->num_resolution = 0;
  if ((option = ppdFindOption(ppd, "Resolution")) != NULL &&
      (count = option->num_choices) > 0) {
    // Valid "Resolution" option, make a sorted list of resolutions.
    if (update)
    {
      def_res_x = driver_data->x_default;
      def_res_y = driver_data->y_default;
    }
    driver_data->x_default = 0;
    driver_data->y_default = 0;
    for (i = 0, j = 0, choice = option->choices;
	 i < count && j < PAPPL_MAX_SOURCE;
	 i ++, choice ++)
      if (!(update &&
	    ppdInstallableConflict(ppd, "Resolution", choice->choice)))
      {
	if ((k = sscanf(choice->choice, "%dx%d",
			&(driver_data->x_resolution[j]),
			&(driver_data->y_resolution[j]))) == 1)
	  driver_data->y_resolution[j] = driver_data->x_resolution[j];
	else if (k <= 0)
	{
	  papplLog(system, PAPPL_LOGLEVEL_ERROR,
		   "Invalid resolution: %s", choice->choice);
	  continue;
	}
	// Default resolution
	if (j == 0 ||
	    (!update && choice->marked) ||
	    (update &&
	     def_res_x == driver_data->x_resolution[j] &&
	     def_res_y == driver_data->y_resolution[j]))
        {
	  def_choice = choice;
	  driver_data->x_default = driver_data->x_resolution[j];
	  driver_data->y_default = driver_data->y_resolution[j];
	}
	for (k = j - 1; k >= 0; k --)
	{
	  int       x1, y1,               // First X and Y resolution
	            x2, y2,               // Second X and Y resolution
	            temp;                 // Swap variable
	  x1 = driver_data->x_resolution[k];
	  y1 = driver_data->y_resolution[k];
	  x2 = driver_data->x_resolution[k + 1];
	  y2 = driver_data->y_resolution[k + 1];
	  if (x2 < x1 || (x2 == x1 && y2 < y1))
	  {
	    temp                             = driver_data->x_resolution[k];
	    driver_data->x_resolution[k]     = driver_data->x_resolution[k + 1];
	    driver_data->x_resolution[k + 1] = temp;
	    temp                             = driver_data->y_resolution[k];
	    driver_data->y_resolution[k]     = driver_data->y_resolution[k + 1];
	    driver_data->y_resolution[k + 1] = temp;
	  }
	}
	j ++;
      }
    if (j > 0)
    {
      driver_data->num_resolution = j;
      ppdMarkOption(ppd, "Resolution", def_choice->choice);
    }
    else
      papplLog(system, PAPPL_LOGLEVEL_WARN,
	       "No valid resolution choice found, using 300 dpi");
  }
  else if ((ppd_attr = ppdFindAttr(ppd, "DefaultResolution", NULL)) != NULL)
  {
    // Use the PPD-defined default resolution...
    if ((j = sscanf(ppd_attr->value, "%dx%d",
		    &(driver_data->x_resolution[0]),
		    &(driver_data->y_resolution[0]))) == 1)
      driver_data->y_resolution[0] = driver_data->x_resolution[0];
    else if (j <= 0)
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "Invalid default resolution: %s, using 300 dpi",
	       ppd_attr->value);
    driver_data->num_resolution = (j > 0 ? 1 : 0);
  }
  else
    papplLog(system, PAPPL_LOGLEVEL_WARN,
	     "No resolution information in PPD, using 300 dpi");
  if (driver_data->num_resolution == 0)
  {
    driver_data->x_resolution[0] = 300;
    driver_data->y_resolution[0] = 300;
    driver_data->num_resolution = 1;
  }
  if (driver_data->x_default == 0 || driver_data->y_default == 0)
  {
    driver_data->x_default = driver_data->x_resolution[0];
    driver_data->y_default = driver_data->y_resolution[0];
  }

  // Media source
  if ((count = pc->num_sources) > 0)
  {
    if (!update)
      choice = ppdFindMarkedChoice(ppd, pc->source_option);
    else
      for (i = 0; i < driver_data->num_source; i ++)
	free((char *)(driver_data->source[i]));
    def_source = NULL;
    for (i = 0, j = 0, pwg_map = pc->sources;
	 i < count && j < PAPPL_MAX_SOURCE;
	 i ++, pwg_map ++)
      if (!(update &&
	    ppdInstallableConflict(ppd, pc->source_option, pwg_map->ppd)))
      {
	driver_data->source[j] = strdup(pwg_map->pwg);
	if (j == 0 ||
	    (!update && choice && !strcmp(pwg_map->ppd, choice->choice)) ||
	    (update &&
	     !strcmp(pwg_map->pwg, driver_data->media_default.source)))
	{
	  def_source = driver_data->source[j];
	  ppdMarkOption(ppd, pc->source_option, pwg_map->ppd);
	}
	j ++;
      }
    driver_data->num_source = j;
  }
  if (count == 0 || driver_data->num_source == 0)
  {
    driver_data->num_source = 1;
    driver_data->source[0] = strdup("default");
    def_source = driver_data->source[0];
  }

  // Media type
  if ((count = pc->num_types) > 0)
  {
    if (!update)
      choice = ppdFindMarkedChoice(ppd, "MediaType");
    else
      for (i = 0; i < driver_data->num_type; i ++)
	free((char *)(driver_data->type[i]));
    def_type = NULL;
    for (i = 0, j = 0, pwg_map = pc->types;
	 i < count && j < PAPPL_MAX_TYPE;
	 i ++, pwg_map ++)
      if (!(update && ppdInstallableConflict(ppd, "MediaType", pwg_map->ppd)))
      {
	driver_data->type[j] = strdup(pwg_map->pwg);
	if (j == 0 ||
	    (!update && choice && !strcmp(pwg_map->ppd, choice->choice)) ||
	    (update &&
	     !strcmp(pwg_map->pwg, driver_data->media_default.type)))
	{
	  def_type = driver_data->type[j];
	  ppdMarkOption(ppd, "MediaType", pwg_map->ppd);
	}
	j ++;
      }
    driver_data->num_type = j;
  }
  if (count == 0 || driver_data->num_type == 0)
  {
    driver_data->num_type = 1;
    driver_data->type[0] = strdup("auto");
    def_type = driver_data->type[0];
  }

  // Media size, margins
  def_left = def_right = def_top = def_bottom = -1;
  driver_data->borderless = false;
  count = pc->num_sizes;
  if (!update)
    choice = ppdFindMarkedChoice(ppd, "PageSize");
  else
    for (i = 0; i < driver_data->num_media; i ++)
      free((char *)(driver_data->media[i]));
  def_media = NULL;
  j = 0;

  // Custom page size (if defined in PPD)
  if (pc->custom_min_keyword && pc->custom_max_keyword &&
      pc->custom_max_width > pc->custom_min_width &&
      pc->custom_max_length > pc->custom_min_length)
  {
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Adding custom page size:");
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "  PWG keyword min dimensions: \"%s\"", pc->custom_min_keyword);
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "  PWG keyword max dimensions: \"%s\"", pc->custom_max_keyword);
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "  Minimum dimensions (width, length): %dx%d",
	     pc->custom_min_width, pc->custom_min_length);
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "  Maximum dimensions (width, length): %dx%d",
	     pc->custom_max_width, pc->custom_max_length);
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "  Margins (left, bottom, right, top): %d, %d, %d, %d",
	     pc->custom_size.left, pc->custom_size.bottom,
	     pc->custom_size.right, pc->custom_size.top);
    driver_data->media[j] = strdup(pc->custom_max_keyword);
    j ++;
    driver_data->media[j] = strdup(pc->custom_min_keyword);
    j ++;
  }

  // Standard page sizes
  for (i = 0, pwg_size = pc->sizes;
       i < count && j < PAPPL_MAX_MEDIA;
       i ++, pwg_size ++)
    if (!(update && ppdInstallableConflict(ppd, "PageSize", pwg_size->map.ppd)))
    {
      driver_data->media[j] =
	strdup(pwg_size->map.pwg);
      if (j == 0 ||
	  (!update && choice && !strcmp(pwg_size->map.ppd, choice->choice)) ||
	  (update &&
	   !strcmp(pwg_size->map.pwg, driver_data->media_default.size_name)))
      {
	def_media = pwg_size;
	ppdMarkOption(ppd, "PageSize", pwg_size->map.ppd);
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
      j ++;
    }

  // Number of media entries (Note that custom page size uses 2 entries,
  // one holding the minimum, one the maximum dimensions)
  driver_data->num_media = j;

  // If margin info missing in the page size entries, use "HWMargins"
  // line of the PPD file, otherwise default values
  if (def_left < 0)
    def_left = (ppd->custom_margins[0] ?
		(int)(ppd->custom_margins[0] / 72.0 * 2540.0) : 635);
  if (def_bottom < 0)
    def_bottom = (ppd->custom_margins[1] ?
		  (int)(ppd->custom_margins[1] / 72.0 * 2540.0) : 1270);
  if (def_right < 0)
    def_right = (ppd->custom_margins[2] ?
		 (int)(ppd->custom_margins[2] / 72.0 * 2540.0) : 635);
  if (def_top < 0)
    def_top = (ppd->custom_margins[3] ?
	       (int)(ppd->custom_margins[3] / 72.0 * 2540.0) : 1270);

  // Set margin info
  driver_data->left_right = (def_left > def_right ? def_left : def_right);
  driver_data->bottom_top = (def_bottom > def_top ? def_bottom : def_right);

  // Set default for media
  if (def_media)
    ps_media_col(def_media, def_source, def_type, 0, 0, 0,
		 &(driver_data->media_default));

  // "media-ready" not defined in PPDs, also cannot be polled from printer
  // The user configures in the web interface what is loaded.
  //
  // The web interface shows only the input trays which are actually
  // installed on the printer, according to the configuration of
  // installable accessories on the "Device Settings" page.
  //
  // If the user accidentally removes a tray on the "Device Settings" page
  // and re-adds it while the Printer Application is still running, the
  // loaded media configuration gets restored.
  if (update)
  {
    for (i = 0, j = 0, pwg_map = pc->sources;
	 i < pc->num_sources && j < PAPPL_MAX_SOURCE;
	 i ++, pwg_map ++)
    {
      tmp_col.source[0] = '\0';
      // Go through all media sources of the PPD file, to keep the order
      if (!strcasecmp(pwg_map->pwg, driver_data->source[j]))
      {
	// Current PPD media source is available (installed)
	if (strcasecmp(pwg_map->pwg, driver_data->media_ready[j].source))
	{
	  // There is no media-col-ready item for the current media source,
	  // so first check whether we have it in the hidden "Undo" space
	  // beyond the actually used media items (it should be there when
	  // we had already set the source as installed earlier during this
	  // session of the Printer Application
	  for (k = j;
	       k < PAPPL_MAX_SOURCE && driver_data->media_ready[k].source[0] &&
		 strcasecmp(pwg_map->pwg, driver_data->media_ready[k].source);
	       k ++);
	  if (!strcasecmp(pwg_map->pwg, driver_data->media_ready[k].source))
	    // Found desired item in hidden "Undo" space beyond the actually
	    // used media-col-ready items
	    memcpy(&tmp_col, &(driver_data->media_ready[k]),
		   sizeof(pappl_media_col_t));
	  else if (k == PAPPL_MAX_SOURCE)
	    k --; // Do not push beyond the memory
	  else if (k < PAPPL_MAX_SOURCE - 1)
	    k ++; // Push up also the terminating zero item
	  // Move up the other items to make space for the new item
	  memmove(&(driver_data->media_ready[j + 1]),
		  &(driver_data->media_ready[j]),
		  (k - j) * sizeof(pappl_media_col_t));
	  if (tmp_col.source[0])
	    // Insert item from "Undo" space
	    memcpy(&(driver_data->media_ready[j]), &tmp_col,
		   sizeof(pappl_media_col_t));
	  else
	  {
	    // Create new item, as this was not in "Undo" space
	    memcpy(&(driver_data->media_ready[j]),
		   &(driver_data->media_default),
		   sizeof(pappl_media_col_t));
	    strncpy(driver_data->media_ready[j].source, driver_data->source[j],
		    sizeof(driver_data->media_ready[j].source));
	  }
	}
	// Go on with next media source
	j ++;
      }
      else
      {
	// Current PPD media source is unavailable (accessory not installed)
	if (!strcasecmp(pwg_map->pwg, driver_data->media_ready[j].source))
	{
	  // Current media-col-ready item is the unavailable media source,
	  // so move current media-col-ready away into the "Undo" space beyond
	  // the actually used media-col-ready items, so its configuration
	  // stays saved case we have removed the tray in the installable
	  // accessories by accident
	  memcpy(&tmp_col, &(driver_data->media_ready[j]),
		 sizeof(pappl_media_col_t));
	  // Pull down the rest
	  for (k = j + 1;
	       k < PAPPL_MAX_SOURCE && driver_data->media_ready[k].source[0];
	       k ++);
	  memmove(&(driver_data->media_ready[j]),
		  &(driver_data->media_ready[j + 1]),
		  (k - j - 1) * sizeof(pappl_media_col_t));
	  // Drop the saved item into the freed slot in the "Undo" space
	  memcpy(&(driver_data->media_ready[k - 1]), &tmp_col,
		 sizeof(pappl_media_col_t));
	}
      }
    }
  }
  else
  {
    // Create media-col-ready items for each media source
    for (i = 0; i < driver_data->num_source; i ++)
    {
      memcpy(&(driver_data->media_ready[i]), &(driver_data->media_default),
	     sizeof(pappl_media_col_t));
      strncpy(driver_data->media_ready[i].source, driver_data->source[i],
	      sizeof(driver_data->media_ready[i].source));
    }
    // Add a terminating zero item to manage the "Undo" space when configuring
    // available media trays on the printer
    if (i < PAPPL_MAX_SOURCE)
      driver_data->media_ready[i].source[0] = '\0';
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
    if (!update)
      choice = ppdFindMarkedChoice(ppd, "OutputBin");
    else
    {
      def_bin = strdup(driver_data->bin[driver_data->bin_default]);
      for (i = 0; i < driver_data->num_bin; i ++)
	free((char *)(driver_data->bin[i]));
    }
    driver_data->bin_default = 0;
    for (i = 0, j = 0, pwg_map = pc->bins;
	 i < count && j < PAPPL_MAX_BIN;
	 i ++, pwg_map ++)
      if (!(update && ppdInstallableConflict(ppd, "OutputBin", pwg_map->ppd)))
      {
	driver_data->bin[j] = strdup(pwg_map->pwg);
	if ((!update && choice && !strcmp(pwg_map->ppd, choice->choice)) ||
	    (update && !strcmp(pwg_map->pwg, def_bin)))
	{
	  driver_data->bin_default = j;
	  ppdMarkOption(ppd, "OutputBin", pwg_map->ppd);
	}
	j ++;
      }
    driver_data->num_bin = j;
    if (update)
      free(def_bin);
  }
  else
  {
    driver_data->num_bin = 0;
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

  // For each PPD option which is not supported by PAPPL/IPP add a
  // vendor option, so that the default for the options can get set in
  // the web interface or settings of these options can be supplied on
  // the command line.

  // Clean up old option lists on update
  if (update)
    for (i = 0; i < driver_data->num_vendor; i ++)
    {
      free((char *)(driver_data->vendor[i]));
      if (extension->vendor_ppd_options[i])
	free((char *)(extension->vendor_ppd_options[i]));
    }

  // Go through all the options of the PPD file
  driver_data->num_vendor = 0;
  for (i = ppd->num_groups, group = ppd->groups;
       i > 0;
       i --, group ++)
  {
    for (j = group->num_options, option = group->options;
         j > 0;
         j --, option ++)
    {
      // Does the option have less than 2 choices? Then it does not make
      // sense to let it show in the web interface
      if (option->num_choices < 2)
	continue;

      // Can printer's default setting of this option be polled from the
      // printer?
      snprintf(buf, sizeof(buf), "?%s", option->keyword);
      if ((ppd_attr = ppdFindAttr(ppd, buf, NULL)) != NULL &&
	  ppd_attr->value)
      {
	pollable = true;
	papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		 "Default of option \"%s\" (\"%s\") can get queried from "
		 "printer.", option->keyword, option->text);
      }
      else
	pollable = false;

      // Skip the group for installable options here, as they should not
      // show on the "Printing Defaults" page nor be listed in the response
      // to a get-printer-atrributes IPP request from a client.
      // Only note the fact that we have such options in the PPD file
      if (strncasecmp(group->name, "Installable", 11) == 0)
      {
	papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		 "Installable accessory option: \"%s\" (\"%s\")",
		 option->keyword, option->text);
	extension->installable_options = true;
	if (pollable)
	  extension->installable_pollable = true;
	continue;
      }

      // Do we have a pollable option? Mark that we have one so that
      // we can show an appropriate poll button in the web interface
      if (pollable)
	extension->defaults_pollable = true;

      // Is this option already handled by PAPPL/IPP
      for (k = 0; pappl_handled_options[k]; k ++)
	if (!strcasecmp(option->keyword, pappl_handled_options[k]))
	  break;
      if (pappl_handled_options[k] ||
	  (pc->source_option &&
	   !strcasecmp(option->keyword, pc->source_option)) ||
	  (pc->sides_option &&
	   !strcasecmp(option->keyword, pc->sides_option)))
	continue;

      // Stop and warn if we have no slot for vendor attributes any more
      // Note that we reserve one slot for saving the "Installable Options"
      // in the state file
      if (driver_data->num_vendor >= PAPPL_MAX_VENDOR - 1)
      {
	papplLog(system, PAPPL_LOGLEVEL_WARN,
		 "Too many options in PPD file, \"%s\" will not be controllable!",
		 option->keyword);
	continue;
      }

      // IPP-style names
      ppdPwgUnppdizeName(option->text, ipp_opt, sizeof(ipp_opt), NULL);
      snprintf(ipp_supported, sizeof(ipp_supported), "%s-supported", ipp_opt);
      snprintf(ipp_default, sizeof(ipp_default), "%s-default", ipp_opt);

      // Add vendor option and its choices to driver IPP attributes
      if (option->ui == PPD_UI_PICKONE || option->ui == PPD_UI_BOOLEAN)
      {
	papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		 "Adding vendor-specific option \"%s\" (\"%s\") as IPP option "
		 "\"%s\"", option->keyword, option->text, ipp_opt);
	if (*driver_attrs == NULL)
	  *driver_attrs = ippNew();
	if (option->num_choices == 2 &&
	    ((!strcasecmp(option->choices[0].text, "true") &&
	      !strcasecmp(option->choices[1].text, "false")) ||
	     (!strcasecmp(option->choices[0].text, "false") &&
	      !strcasecmp(option->choices[1].text, "true"))))
	{
	  // Create a boolean IPP option, as human-readable choices "true"
	  // and "false" are not very user-friendly

	  // On update, remove IPP attributes, keep default
	  default_choice = 0;
	  if (update)
          {
	    ippDeleteAttribute(*driver_attrs,
			       ippFindAttribute(*driver_attrs, ipp_supported,
						IPP_TAG_ZERO));
	    attr = ippFindAttribute(*driver_attrs, ipp_default, IPP_TAG_ZERO);
	    if (attr)
	    {
	      default_choice = ippGetBoolean(attr, 0);
	      ippDeleteAttribute(*driver_attrs, attr);
	    }
	    else
	      default_choice = 0;
	    if (ppdInstallableConflict(ppd, option->keyword,
				       option->choices[0].choice))
	      default_choice = -1;
	    if (ppdInstallableConflict(ppd, option->keyword,
				       option->choices[1].choice))
	    {
	      if (default_choice >= 0)
		ppdMarkOption(ppd, option->keyword, option->choices[0].choice);
	      default_choice = -1;
	    }
	    else if (default_choice < 0)
	      ppdMarkOption(ppd, option->keyword, option->choices[1].choice);
	    if (default_choice < 0)
	    {
	      papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		       "  -> Skipping - Boolean option does not make sense with current accessory configuration");
	      continue;
	    }
	  }
	  else
	  {
	    default_choice = 0;
	    for (k = 0; k < 2; k ++)
	      if (option->choices[k].marked &&
		  !strcasecmp(option->choices[k].text, "true"))
		default_choice = 1;
	  }
	  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		   "  Default: %s", (default_choice ? "true" : "false"));
	  ippAddBoolean(*driver_attrs, IPP_TAG_PRINTER, ipp_supported, 1);
	  ippAddBoolean(*driver_attrs, IPP_TAG_PRINTER, ipp_default,
			default_choice);
	}
	else
	{
	  // Create an enumerated-choice IPP option

	  // On update, remove IPP attributes, keep default
	  if (update)
          {
	    ippDeleteAttribute(*driver_attrs,
			       ippFindAttribute(*driver_attrs, ipp_supported,
						IPP_TAG_ZERO));
	    attr = ippFindAttribute(*driver_attrs, ipp_default, IPP_TAG_ZERO);
	    if (attr)
	    {
	      ippAttributeString(attr, buf, sizeof(buf));
	      ippDeleteAttribute(*driver_attrs, attr);
	    }
	    else
	      buf[0] = '\0';
	  }
	  choice_list = (char **)calloc(option->num_choices, sizeof(char *));
	  first_choice = -1;
	  default_choice = -1;
	  for (k = 0, l = 0; k < option->num_choices; k ++)
	    if (!(update && ppdInstallableConflict(ppd, option->keyword,
						   option->choices[k].choice)))
	    {
	      if (first_choice < 0)
		first_choice = k;
	      ppdPwgUnppdizeName(option->choices[k].text,
				 ipp_choice, sizeof(ipp_choice), NULL);
	      choice_list[l] = strdup(ipp_choice);
	      if ((!update && option->choices[k].marked) ||
		  (update && buf[0] && !strcasecmp(ipp_choice, buf)))
	      {
		default_choice = l;
		ppdMarkOption(ppd, option->keyword, option->choices[k].choice);
	      }
	      papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		       "  Adding choice \"%s\" (\"%s\") as \"%s\"%s",
		       option->choices[k].choice, option->choices[k].text,
		       ipp_choice,
		       default_choice == l ? " (default)" : "");
	      l ++;
	    }
	  if (l > 0 && default_choice < 0)
	  {
	    default_choice = 0;
	    ppdMarkOption(ppd, option->keyword,
			  option->choices[first_choice].choice);
	  }
	  if (l >= 2)
	  {
	    ippAddStrings(*driver_attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
			  ipp_supported, l, NULL,
			  (const char * const *)choice_list);
	    ippAddString(*driver_attrs, IPP_TAG_PRINTER, IPP_TAG_KEYWORD,
			 ipp_default, NULL, choice_list[default_choice]);
	  }
	  for (k = 0; k < l; k ++)
	    free(choice_list[k]);
	  free(choice_list);
	  if (l < 2)
	  {
	    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		     "   -> Skipping - Option does not make sense with current accessory configuration");
	    continue;
	  }
	}
      }
      else
	continue;

      // Add vendor option to lookup lists
      driver_data->vendor[driver_data->num_vendor] = strdup(ipp_opt);
      extension->vendor_ppd_options[driver_data->num_vendor] =
	strdup(option->keyword);

      // Next entry ...
      driver_data->num_vendor ++;
    }
  }

  // Add a vendor option as placeholder for saving the settings for the
  // "Installable Options" in the state file. With no "...-supported" IPP
  // attribute and IPP_TAG_TEXT format it will not appear on the "Printing
  // Defaults" web interface page.
  if (extension->installable_options)
  {
    driver_data->vendor[driver_data->num_vendor] =
      strdup("installable-options");
    extension->vendor_ppd_options[driver_data->num_vendor] = NULL;
    driver_data->num_vendor ++;
    if (!update)
      ippAddString(*driver_attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT,
		   "installable-options-default", NULL, "");
  }

  return (true);
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
  filter_data.iscanceledfunc = ps_job_is_canceled; // Function to indicate
                                                   // whether the job got
                                                   // canceled
  filter_data.iscanceleddata = job;

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

  if (filterChain(fd, nullfd, 1, &filter_data, chain) == 0)
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
// 'ps_free_job_data()' - Clean up job data with PPD options.
//

static void   ps_free_job_data(ps_job_data_t *job_data)
{
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
  pappl_pr_driver_data_t driver_data;
  ps_driver_extension_t  *extension;
  ppd_file_t             *ppd = NULL;	// PPD file of the printer
  pappl_device_t         *device;       // PAPPL output device


  (void)actions;
  (void)message;

  // Identify the printer by sending a zero-page PostScript job to
  // make the display of the printer light up and depending on
  // hardware mechanics move and/or signal sounds play

  papplPrinterGetDriverData(printer, &driver_data);
  extension = (ps_driver_extension_t *)driver_data.extension;
  ppd = extension->ppd;

  //
  // Open access to printer device...
  //

  if ((device = papplPrinterOpenDevice(printer)) == NULL)
  {
    papplLogPrinter(printer, PAPPL_LOGLEVEL_WARN,
		    "Cannot access printer: Busy or otherwise not reachable");
    return;
  }

  // Note: We directly output to the printer device without using
  //       ps_print_filter_function() as only use printf()/puts() and
  //       not any PPD-related function of libppd for the output to
  //       the printer

  //
  // Put the printer in PostScript mode and initiate a PostScript
  // file...
  //

  if (ppd->jcl_begin)
  {
    papplDevicePuts(device, ppd->jcl_begin);
    papplDevicePuts(device, ppd->jcl_ps);
  }

  papplDevicePuts(device, "%!\n");
  papplDeviceFlush(device);

  //
  // Delay...
  //

  sleep(3);

  //
  // Finish the job...
  //

  if (ppd->jcl_end)
    papplDevicePuts(device, ppd->jcl_end);
  else
    papplDevicePuts(device, "\004");
  papplDeviceFlush(device);

  //
  // Close connection to the printer device...
  //

  papplPrinterCloseDevice(printer);
}


//
// 'ps_job_is_canceled()' - Return 1 if the job is canceled, which is
//                          the case when papplJobIsCanceled() returns
//                          true.
//

static int
ps_job_is_canceled(void *data)
{
  pappl_job_t *job = (pappl_job_t *)data;


  return (papplJobIsCanceled(job) ? 1 : 0);
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
  while ((bytes = read(inputfd, buffer, sizeof(buffer))) > 0)
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
// 'ps_poll_device_option_defaults()' - This function uses query PostScript
//                                      code from the PPD file to poll
//                                      default option settings from the
//                                      printer
//

static int                      // O - Number of polled default settings
                                //     0: Error
ps_poll_device_option_defaults(
    pappl_printer_t *printer,   // I - Printer to be polled
    bool installable,           // I - Poll installable accessory configuration?
    cups_option_t **defaults)   // O - Option list of polled default settings
{
  int                    i, j, k;       // Looping variables
  pappl_pr_driver_data_t driver_data;
  ps_driver_extension_t  *extension;
  ppd_file_t             *ppd = NULL;	// PPD file of the printer
  int                    num_defaults;  // Number of polled default settings
  pappl_device_t         *device;       // PAPPL output device
  int		         status = 0;	// Exit status
  ppd_group_t            *group;
  ppd_option_t	         *option;	// Current option in PPD
  ppd_attr_t	         *attr;		// Query command attribute
  const char	         *valptr;	// Pointer into attribute value
  char		         buf[1024],	// String buffer
                         *bufptr;	// Pointer into buffer
  ssize_t	         bytes;		// Number of bytes read


  papplPrinterGetDriverData(printer, &driver_data);
  extension = (ps_driver_extension_t *)driver_data.extension;
  ppd = extension->ppd;

  *defaults = NULL;
  num_defaults = 0;

  //
  // Open access to printer device...
  //

  if ((device = papplPrinterOpenDevice(printer)) == NULL)
  {
    papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		    "Cannot access printer: Busy or otherwise not reachable");
    return (0);
  }


  // Note: We directly output to the printer device without using
  //       ps_print_filter_function() as the original code
  //       (commandtops filter of CUPS) only uses printf()/puts() and
  //       not any PPD-related function of libcups (eq. libppd)
  //       for the output to the printer

  //
  // Put the printer in PostScript mode...
  //

  if (ppd->jcl_begin)
  {
    papplDevicePuts(device, ppd->jcl_begin);
    papplDevicePuts(device, ppd->jcl_ps);
  }

  papplDevicePuts(device, "%!\n");
  papplDevicePuts(device, "userdict dup(\\004)cvn{}put (\\004\\004)cvn{}put\n");

  papplDeviceFlush(device);

  //
  // https://github.com/apple/cups/issues/4028
  //
  // As a lot of PPDs contain bad PostScript query code, we need to prevent one
  // bad query sequence from affecting all auto-configuration.  The following
  // error handler allows us to log PostScript errors to cupsd.
  //

  papplDevicePuts(device,
    "/cups_handleerror {\n"
    "  $error /newerror false put\n"
    "  (:PostScript error in \") print cups_query_keyword print (\": ) "
    "print\n"
    "  $error /errorname get 128 string cvs print\n"
    "  (; offending command:) print $error /command get 128 string cvs "
    "print (\n) print flush\n"
    "} bind def\n"
    "errordict /timeout {} put\n"
    "/cups_query_keyword (?Unknown) def\n");
  papplDeviceFlush(device);

  //
  // Loop through every option in the PPD file and ask for the current
  // value...
  //

  papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		  "Reading printer-internal default settings...");

  for (i = ppd->num_groups, group = ppd->groups;
       i > 0;
       i --, group ++)
  {

    // When "installable" is true, We are treating only the
    // "Installable Options" group of options in the PPD file here
    // otherwise only the other options

    if (strncasecmp(group->name, "Installable", 11) == 0)
    {
      if (!installable)
	continue;
    }
    else if (installable)
      continue;

    for (j = group->num_options, option = group->options;
	 j > 0;
	 j --, option ++)
    {
      // Does the option have less than 2 choices? Then it does not make
      // sense to query its default value
      if (option->num_choices < 2)
	continue;

      //
      // See if we have a query command for this option...
      //

      snprintf(buf, sizeof(buf), "?%s", option->keyword);

      if ((attr = ppdFindAttr(ppd, buf, NULL)) == NULL || !attr->value)
      {
	papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
			"Skipping %s option...", option->keyword);
	continue;
      }

      //
      // Send the query code to the printer...
      //

      papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		      "Querying %s...", option->keyword);

      for (bufptr = buf, valptr = attr->value; *valptr; valptr ++)
      {
	//
	// Log the query code, breaking at newlines...
	//

	if (*valptr == '\n')
	{
	  *bufptr = '\0';
	  papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
			  "%s\\n", buf);
	  bufptr = buf;
	}
	else if (*valptr < ' ')
        {
	  if (bufptr >= (buf + sizeof(buf) - 4))
          {
	    *bufptr = '\0';
	    papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
			    "%s", buf);
	    bufptr = buf;
	  }

	  if (*valptr == '\r')
          {
	    *bufptr++ = '\\';
	    *bufptr++ = 'r';
	  }
	  else if (*valptr == '\t')
          {
	    *bufptr++ = '\\';
	    *bufptr++ = 't';
          }
	  else
          {
	    *bufptr++ = '\\';
	    *bufptr++ = '0' + ((*valptr / 64) & 7);
	    *bufptr++ = '0' + ((*valptr / 8) & 7);
	    *bufptr++ = '0' + (*valptr & 7);
	  }
	}
	else
        {
	  if (bufptr >= (buf + sizeof(buf) - 1))
          {
	    *bufptr = '\0';
	    papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
			    "%s", buf);
	    bufptr = buf;
	  }

	  *bufptr++ = *valptr;
	}
      }

      if (bufptr > buf)
      {
	*bufptr = '\0';
	papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
			"%s", buf);
      }

      papplDevicePrintf(device, "/cups_query_keyword (?%s) def\n",
			option->keyword); // Set keyword for error reporting
      papplDevicePuts(device, "{ (");
      for (valptr = attr->value; *valptr; valptr ++)
      {
	if (*valptr == '(' || *valptr == ')' || *valptr == '\\')
	  papplDevicePuts(device, "\\");
	papplDeviceWrite(device, valptr, 1);
      }
      papplDevicePuts(device,
		      ") cvx exec } stopped { cups_handleerror } if clear\n");
                                          // Send query code
      papplDeviceFlush(device);

      //
      // Read the response data...
      //

      bufptr    = buf;
      buf[0] = '\0';
      // If no bytes get read (bytes <= 0), repeat up to 100 times in
      // 100 msec intervals (10 sec timeout)
      for (k = 0; k < 100; k ++)
      {
	//
	// Read answer from device ...
	//

	bytes = papplDeviceRead(device, bufptr,
				sizeof(buf) - (size_t)(bufptr - buf) - 1);

	//
	// No bytes of the answer arrived yet? Retry ...
	//

	if (bytes <= 0)
        {
	  papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
			  "Answer not ready yet, retrying in 100 ms.");
	  usleep(100000);
	  continue;
        }

	//
	// No newline at the end? Go on reading ...
	//

	bufptr += bytes;
	*bufptr = '\0';

	if (bytes == 0 ||
	    (bufptr > buf && bufptr[-1] != '\r' && bufptr[-1] != '\n'))
	  continue;

	//
	// Trim whitespace and control characters from both ends...
	//

	bytes = bufptr - buf;

	for (bufptr --; bufptr >= buf; bufptr --)
	  if (isspace(*bufptr & 255) || iscntrl(*bufptr & 255))
	    *bufptr = '\0';
	  else
	    break;

	for (bufptr = buf; isspace(*bufptr & 255) || iscntrl(*bufptr & 255);
	     bufptr ++);

	if (bufptr > buf)
        {
	  memmove(buf, bufptr, strlen(bufptr) + 1);
	  bufptr = buf;
	}

	papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
			"Got %d bytes.", (int)bytes);

	//
	// Skip blank lines...
	//

	if (!buf[0])
	  continue;

	//
	// Check the response...
	//

	if ((bufptr = strchr(buf, ':')) != NULL)
        {
	  //
	  // PostScript code for this option in the PPD is broken; show the
	  // interpreter's error message that came back...
	  //

	  papplLogPrinter(printer, PAPPL_LOGLEVEL_WARN,
			  "%s", bufptr + 1);
	  status = 1;
	  break;
	}

	//
	// Verify the result is a valid option choice...
	//

	if (!ppdFindChoice(option, buf))
        {
	  if (!strcasecmp(buf, "Unknown"))
	  {
	    papplLogPrinter(printer, PAPPL_LOGLEVEL_WARN,
			    "Unknown default setting for option \"%s\"",
			    option->keyword);
	    status = 1;
	    break;
	  }

	  bufptr    = buf;
	  buf[0] = '\0';
	  continue;
	}

        //
        // Write out the result and move on to the next option...
	//

	papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
			"Read default setting for \"%s\": \"%s\"",
			option->keyword, buf);
	num_defaults = cupsAddOption(option->keyword, buf, num_defaults,
				     defaults);
	break;
      }

      //
      // Printer did not answer this option's query
      //

      if (bytes <= 0)
      {
	papplLogPrinter(printer, PAPPL_LOGLEVEL_WARN,
			"No answer to query for option %s within 10 sec "
			"timeout.", option->keyword);
	status = 1;
      }
    }
  }

  //
  // Finish the job...
  //

  papplDeviceFlush(device);
  if (ppd->jcl_end)
    papplDevicePuts(device, ppd->jcl_end);
  else
    papplDevicePuts(device, "\004");
  papplDeviceFlush(device);

  //
  // Close connection to the printer device...
  //

  papplPrinterCloseDevice(printer);

  //
  // Return...
  //

  if (status)
    papplLogPrinter(printer, PAPPL_LOGLEVEL_WARN,
		    "Unable to configure some printer options.");

  return (num_defaults);
}


//
// 'ps_printer_web_device_config()' - Web interface page for entering/polling
//                                    the configuration of printer add-ons
//                                    ("Installable Options" in PPD and polling
//                                    default option settings
//

static void
ps_printer_web_device_config(
    pappl_client_t  *client,		// I - Client
    pappl_printer_t *printer)		// I - Printer
{
  int          i, j, k;                 // Looping variables
  const char   *status = NULL;		// Status text, if any
  const char   *uri = NULL;             // Client URI
  pappl_pr_driver_data_t driver_data;
  ipp_t        *driver_attrs;
  ps_driver_extension_t *extension;
  ppd_file_t   *ppd = NULL;		// PPD file of the printer
  ppd_cache_t  *pc;
  char         *keyword;
  ppd_group_t  *group;
  ppd_option_t *option;
  ppd_choice_t *choice;
  ppd_attr_t   *ppd_attr;
  int          default_choice;
  int          num_options = 0;         // Number of polled options
  cups_option_t	*options = NULL;        // Polled options
  cups_option_t *opt;
  bool         polled_installables = false,
               polled_defaults = false;


  papplPrinterGetDriverData(printer, &driver_data);
  driver_attrs = papplPrinterGetDriverAttributes(printer);
  extension = (ps_driver_extension_t *)driver_data.extension;
  ppd = extension->ppd;
  pc = ppd->cache;

  if (!papplClientHTMLAuthorize(client))
    return;

  // Handle POSTs to set "Installable Options" and poll default settings...
  if (papplClientGetMethod(client) == HTTP_STATE_POST)
  {
    int			num_form = 0;	// Number of form variables
    cups_option_t	*form = NULL;	// Form variables
    int                 num_installables = 0; // Number of installable options 
    cups_option_t	*installables = NULL; // Set installable options
    int                 num_vendor = 0; // Number of vendor-specific options 
    cups_option_t	*vendor = NULL; // vendor-specific options
    ipp_attribute_t     *attr;
    const char		*action;	// Form action
    char                buf[1024];
    const char          *value;
    char                *ptr1, *ptr2;
    pwg_map_t           *pwg_map;
    pwg_size_t          *pwg_size;
    int                 polled_def_source = -1;
    const char          *polled_def_size = NULL,
                        *polled_def_type = NULL;
    int                 best = 0;
    char                ipp_supported[128],
                        ipp_choice[80];

    if ((num_form = papplClientGetForm(client, &form)) == 0)
    {
      status = "Invalid form data.";
    }
    else if (!papplClientIsValidForm(client, num_form, form))
    {
      status = "Invalid form submission.";
    }
    else if ((action = cupsGetOption("action", num_form, form)) == NULL)
    {
      status = "Missing action.";
    }
    else if (!strcmp(action, "set-installable"))
    {
      status = "Installable accessory configuration saved.";
      buf[0] = '\0';
      for (i = num_form, opt = form; i > 0;
	   i --, opt ++)
	if (opt->name[0] == '\t')
	{
	  if (opt->name[1] == '\t')
	  {
	    ptr1 = strdup(opt->name + 2);
	    ptr2 = strchr(ptr1, '\t');
	    *ptr2 = '\0';
	    ptr2 ++;
	    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1,
		     " %s=%s", ptr1, ptr2);
	    num_installables = cupsAddOption(ptr1, ptr2,
					     num_installables, &installables);
	    free(ptr1);
	  }
	  else
	  {
	    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1,
		     " %s=%s", opt->name + 1, opt->value);
	    num_installables = cupsAddOption(opt->name + 1, opt->value,
					     num_installables, &installables);
	  }
	}
      papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		      "\"Installable Options\" from web form:%s", buf);

      buf[0] = '\0';
      for (i = ppd->num_groups, group = ppd->groups;
	   i > 0;
	   i --, group ++)
      {

	// We are treating only the "Installable Options" group of options
	// in the PPD file here

	if (strncasecmp(group->name, "Installable", 11) != 0)
	  continue;

	for (j = group->num_options, option = group->options;
	     j > 0;
	     j --, option ++)
        {
	  // Does the option have less than 2 choices? Then it does not make
	  // sense to let it show in the web interface
	  if (option->num_choices < 2)
	    continue;

	  if ((value = cupsGetOption(option->keyword,
				     num_installables, installables)) == NULL)
	  {
	    // Unchecked check box option
	    if (!strcasecmp(option->choices[0].text, "false"))
	      value = option->choices[0].choice;
	    else if (!strcasecmp(option->choices[1].text, "false"))
	      value = option->choices[1].choice;
	  }
	  ppdMarkOption(ppd, option->keyword, value);
	  snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1,
		   "%s=%s ", option->keyword, value);
	}
	if (buf[0])
	  buf[strlen(buf) - 1] = '\0';
      }
      cupsFreeOptions(num_installables, installables);

      // Put the settings into an IPP attribute to save in the state file
      papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		      "\"Installable Options\" marked in PPD: %s", buf);
      ippDeleteAttribute(driver_attrs,
			 ippFindAttribute(driver_attrs,
					  "installable-options-default",
					  IPP_TAG_ZERO));
      ippAddString(driver_attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT,
		   "installable-options-default", NULL, buf);

      // Update the driver data to only show options and choices which make
      // sense with the current installable accessory configuration
      extension->updated = false;
      ps_status(printer);
    }
    else if (!strcmp(action, "poll-installable"))
    {
      // Poll installed options info
      num_options = ps_poll_device_option_defaults(printer, true, &options);
      if (num_options)
      {
	status = "Installable accessory configuration polled from printer.";
	polled_installables = true;

	// Get current settings of the "Installable Options"
	num_installables = 0;
	installables = NULL;
	if ((attr = ippFindAttribute(driver_attrs,
				     "installable-options-default",
				     IPP_TAG_ZERO)) != NULL)
	{
	  if (ippAttributeString(attr, buf, sizeof(buf)) > 0)
	  {
	    num_installables = cupsParseOptions(buf, 0, &installables);
	    ppdMarkOptions(ppd, num_installables, installables);
	  }
	  ippDeleteAttribute(driver_attrs, attr);
	}

	// Join polled settings and mark them in the PPD
	for (i = num_options, opt = options; i > 0; i --, opt ++)
        {
	  ppdMarkOption(ppd, opt->name, opt->value);
	  num_installables = cupsAddOption(opt->name, opt->value,
					   num_installables, &installables);
        }

	// Create new option string for saving in the state file
	buf[0] = '\0';
	for (i = num_installables, opt = installables; i > 0; i --, opt ++)
	  snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1,
		   "%s=%s ", opt->name, opt->value);
        if (buf[0])
	  buf[strlen(buf) - 1] = '\0';

	// Clean up
	cupsFreeOptions(num_installables, installables);

        // Put the settings into an IPP attribute to save in the state file
	papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
			"\"Installable Options\" marked in PPD: %s", buf);
	ippAddString(driver_attrs, IPP_TAG_PRINTER, IPP_TAG_TEXT,
		     "installable-options-default", NULL, buf);

	// Update the driver data to only show options and choices which make
	// sense with the current installable accessory configuration
	extension->updated = false;
	ps_status(printer);
      }
      else
	status = "Could not poll installable accessory configuration from "
	         "printer.";
    }
    else if (!strcmp(action, "poll-defaults"))
    {
      // Poll default option values
      num_options = ps_poll_device_option_defaults(printer, false,
						   &options);
      if (num_options)
      {
	// Read the polled option settings, mark them in the PPD, update them
	// in the printer data and create a summary for logging
	status = "Option defaults polled from printer.";
	polled_defaults = true;

	snprintf(buf, sizeof(buf) - 1, "Option defaults polled from printer:");
	for (i = num_options, opt = options; i > 0; i --, opt ++)
	{
	  ppdMarkOption(ppd, opt->name, opt->value);
	  snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1,
		   " %s=%s", opt->name, opt->value);
	  if (!strcasecmp(opt->name, "PageSize"))
	  {
	    for (j = 0, pwg_size = pc->sizes; j < pc->num_sizes;
		 j ++, pwg_size ++)
	      if (!strcasecmp(opt->value, pwg_size->map.ppd))
		break;
	    if (j < pc->num_sizes)
	    {
	      for (k = 0; k < driver_data.num_media; k ++)
		if (!strcasecmp(pwg_size->map.pwg, driver_data.media[k]))
		  break;
	      if (k < driver_data.num_media)
		polled_def_size = driver_data.media[k];
	    }
	  }
	  else if (!strcasecmp(opt->name, pc->source_option)) // InputSlot
	  {
	    for (j = 0, pwg_map = pc->sources; j < pc->num_sources;
		 j ++, pwg_map ++)
	      if (!strcasecmp(opt->value, pwg_map->ppd))
		break;
	    if (j < pc->num_sources)
	    {
	      for (k = 0; k < driver_data.num_source; k ++)
		if (!strcasecmp(pwg_map->pwg, driver_data.source[k]))
		  break;
	      if (k < driver_data.num_source)
		polled_def_source = k;
	    }
	  }
	  else if (!strcasecmp(opt->name, "MediaType"))
	  {
	    for (j = 0, pwg_map = pc->types; j < pc->num_types;
		 j ++, pwg_map ++)
	      if (!strcasecmp(opt->value, pwg_map->ppd))
		break;
	    if (j < pc->num_types)
	    {
	      for (k = 0; k < driver_data.num_type; k ++)
		if (!strcasecmp(pwg_map->pwg, driver_data.type[k]))
		  break;
	      if (k < driver_data.num_type)
		polled_def_type = driver_data.type[k];
	    }
	  }
	  else if (!strcasecmp(opt->name, "Resolution"))
	  {
	    if (sscanf(opt->value, "%dx%ddpi",
		       &driver_data.x_default, &driver_data.y_default) == 1)
	      driver_data.y_default = driver_data.x_default;
	  }
	  else if (!strcasecmp(opt->name, "ColorModel"))
	  {
	    if (ppd->color_device)
	    {
	      if (strcasestr(opt->value, "Gray") ||
		  strcasestr(opt->value, "Mono") ||
		  strcasestr(opt->value, "Black"))
		driver_data.color_default = PAPPL_COLOR_MODE_MONOCHROME;
	      else if (strcasestr(opt->value, "Color") ||
		       strcasestr(opt->value, "RGB") ||
		       strcasestr(opt->value, "CMY"))
		driver_data.color_default = PAPPL_COLOR_MODE_COLOR;
	      else
		driver_data.color_default = PAPPL_COLOR_MODE_AUTO;
	    }
	  }
	  else if (!strcasecmp(opt->name, "OutputBin"))
	  {
	    for (j = 0, pwg_map = pc->bins; j < pc->num_bins; j ++, pwg_map ++)
	      if (!strcasecmp(opt->value, pwg_map->ppd))
		break;
	    if (j < pc->num_bins)
	    {
	      for (k = 0; k < driver_data.num_bin; k ++)
		if (!strcasecmp(pwg_map->pwg, driver_data.bin[k]))
		  break;
	      if (k < driver_data.num_bin)
		driver_data.bin_default = k;
	    }
	  }
	  else if (!strcasecmp(opt->name, pc->sides_option)) // Duplex
	  {
	    if (!strcasecmp(opt->value, pc->sides_1sided))
	      driver_data.sides_default = PAPPL_SIDES_ONE_SIDED;
	    else if (!strcasecmp(opt->value, pc->sides_2sided_long))
	      driver_data.sides_default = PAPPL_SIDES_TWO_SIDED_LONG_EDGE;
	    else if (!strcasecmp(opt->value, pc->sides_2sided_short))
	      driver_data.sides_default = PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
	  }
	  else if (strcasecmp(opt->name, "PageRegion"))
	  {
	    // Vendor options
	    for (j = 0; j < driver_data.num_vendor; j ++)
	    {
	      if (extension->vendor_ppd_options[j] &&
		  !(strcasecmp(opt->name, extension->vendor_ppd_options[j])))
	      {
		if ((option = ppdFindOption(ppd, opt->name)) != NULL &&
		    (choice = ppdFindChoice(option, opt->value)) != NULL)
		{
		  snprintf(ipp_supported, sizeof(ipp_supported),
			   "%s-supported", driver_data.vendor[j]);
		  if ((attr = ippFindAttribute(driver_attrs, ipp_supported,
					       IPP_TAG_ZERO)) != NULL)
		  {
		    if (ippGetValueTag(attr) == IPP_TAG_BOOLEAN)
		    {
		      if (!strcasecmp(choice->text, "True"))
			num_vendor = cupsAddOption(driver_data.vendor[j],
						   "true", num_vendor, &vendor);
		      else if (!strcasecmp(choice->text, "False"))
			num_vendor = cupsAddOption(driver_data.vendor[j],
						   "false",
						   num_vendor, &vendor);
		    }
		    else
		    {
		      ppdPwgUnppdizeName(choice->text, ipp_choice,
					 sizeof(ipp_choice), NULL);
		      num_vendor = cupsAddOption(driver_data.vendor[j],
						 ipp_choice,
						 num_vendor, &vendor);
		    }
		  }
		}
	      }
	    }
	  }
	}

	// Media Source
	if (polled_def_source < 0)
	{
	  if (polled_def_size || polled_def_type)
	    for (i = 0; i < driver_data.num_source; i ++)
	    {
	      j = 0;
	      if (polled_def_size &&
		  !strcasecmp(polled_def_size,
			      driver_data.media_ready[i].size_name))
		j += 2;
	      if (polled_def_type &&
		  !strcasecmp(polled_def_type,
			      driver_data.media_ready[i].type))
		j += 1;
	      if (j > best)
	      {
		best = j;
		driver_data.media_default = driver_data.media_ready[i];
	      }
	    }
	}
	else
	  driver_data.media_default =
	    driver_data.media_ready[polled_def_source];

	papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG, "%s", buf);

	// Submit the changed default values
	papplPrinterSetDriverDefaults(printer, &driver_data,
				      num_vendor, vendor);

	// Clean up
	if (num_vendor)
	  cupsFreeOptions(num_vendor, vendor);
      }
      else
	status = "Could not poll option defaults from printer.";
    }
    else
      status = "Unknown action.";

    cupsFreeOptions(num_form, form);
  }

  papplClientHTMLPrinterHeader(client, printer, "Printer Device Settings", 0, NULL, NULL);

  if (status)
    papplClientHTMLPrintf(client, "          <div class=\"banner\">%s</div>\n", status);

  uri = papplClientGetURI(client);

  if (extension->installable_options)
  {
    papplClientHTMLPuts(client,
			"          <h3>Installable printer accessories</h3>\n");
    if (polled_installables)
      papplClientHTMLPuts(client,
			  "          <br>Settings obtained from polling the printer are marked with an asterisk (\"*\")</br>\n");

    papplClientHTMLStartForm(client, uri, false);

    papplClientHTMLPuts(client,
			"          <table class=\"form\">\n"
			"            <tbody>\n");

    for (i = ppd->num_groups, group = ppd->groups;
	 i > 0;
	 i --, group ++)
    {

      // We are treating only the "Installable Options" group of options
      // in the PPD file here

      if (strncasecmp(group->name, "Installable", 11) != 0)
	continue;

      for (j = group->num_options, option = group->options;
	   j > 0;
	   j --, option ++)
      {
	// Does the option have less than 2 choices? Then it does not make
	// sense to let it show in the web interface
	if (option->num_choices < 2)
	  continue;

	papplClientHTMLPrintf(client, "              <tr><th>%s:</th><td>",
			      option->text);

	if (option->num_choices == 2 &&
	    ((!strcasecmp(option->choices[0].text, "true") &&
	      !strcasecmp(option->choices[1].text, "false")) ||
	     (!strcasecmp(option->choices[0].text, "false") &&
	      !strcasecmp(option->choices[1].text, "true"))))
	{
	  // Create a check box widget, as human-readable choices "true"
	  // and "false" are not very user-friendly
	  default_choice = 0;
	  for (k = 0; k < 2; k ++)
	    if (!strcasecmp(option->choices[k].text, "true"))
	    {
	      if (option->choices[k].marked)
		default_choice = 1;
	      // Stop here to make k be the index of the "True" value of this
	      // option so that we can extract its machine-readable value
	      break;
	    }
	  // We precede the option name with a two tabs to mark it as an
	  // option represented by a check box, we also add the machine-
	  // readable choice name for "True" (checked). This way we can treat
	  // the result correctly, taking into account that nothing for this
	  // option gets submitted when the box is unchecked.
	  papplClientHTMLPrintf(client, "<input type=\"checkbox\" name=\"\t\t%s\t%s\"%s>", option->keyword, option->choices[k].choice, default_choice == 1 ? " checked" : "");
	}
	else
	{
	  // Create a drop-down widget
	  // We precede the option name with a single tab to tell that this
	  // option comes from a drop-down. The drop-down choice always gets
	  // submitted, so the option name in the name field is enough for
	  // parsing the submitted result.
	  // The tab in the beginning also assures that the PPD option names
	  // never conflict with fixed option names of this function, like
	  // "action" or "session".
	  papplClientHTMLPrintf(client, "<select name=\"\t%s\">", option->keyword);
	  default_choice = 0;
	  for (k = 0; k < option->num_choices; k ++)
	    papplClientHTMLPrintf(client, "<option value=\"%s\"%s>%s</option>", option->choices[k].choice, option->choices[k].marked ? " selected" : "", option->choices[k].text);
	  papplClientHTMLPuts(client, "</select>");
	}

	// Mark options which got updated by polling from the printer with
	// an asterisk
	papplClientHTMLPrintf(client, "%s",
			      (polled_installables &&
			       cupsGetOption(option->keyword, num_options,
					     options) != NULL ? " *" : ""));

	papplClientHTMLPuts(client, "</td></tr>\n");
      }
    }
    papplClientHTMLPuts(client,
			"              <tr><th></th><td><button type=\"submit\" name=\"action\" value=\"set-installable\">Set</button>");
    if (extension->installable_pollable)
    {
      papplClientHTMLStartForm(client, uri, false);
      papplClientHTMLPrintf(client, "\n          &nbsp;<button type=\"submit\" name=\"action\" value=\"poll-installable\">Poll from printer</button>\n");
    }
    papplClientHTMLPuts(client,
			"</td></tr>\n"
			"            </tbody>\n"
			"          </table>\n"
			"        </form>\n");

  }

  if (extension->installable_options &&
      extension->defaults_pollable)
    papplClientHTMLPrintf(client, "          <hr>\n");

  if (extension->defaults_pollable)
  {
    papplClientHTMLPuts(client,
			"          <h3>Poll printing defaults from the printer</h3>\n");

    papplClientHTMLPuts(client,
			"          <br>Note that settings polled from the printer overwrite your original settings.</br>\n");
    if (polled_defaults)
      papplClientHTMLPuts(client,
			  "          <br>Polling results:</br>\n");

    papplClientHTMLStartForm(client, uri, false);
    papplClientHTMLPuts(client,
			"          <table class=\"form\">\n"
			"            <tbody>\n");

    // If we have already polled and display the page again, show poll
    // results as options which got updated by polling with their
    // values (they are in list "options").
    if (polled_defaults && num_options)
      for (i = num_options, opt = options; i > 0; i --, opt ++)
	if ((option = ppdFindOption(ppd, opt->name)) != NULL &&
	    (choice = ppdFindChoice(option, opt->value)) != NULL)
	  papplClientHTMLPrintf(client,
			    "              <tr><th>%s:</th><td>%s</td></tr>\n",
			    option->text, choice->text);

    papplClientHTMLPrintf(client, "          <tr><th></th><td><input type=\"hidden\" name=\"action\" value=\"poll-defaults\"><input type=\"submit\" value=\"%s\"></td>\n",
			  (polled_defaults ? "Poll again" : "Poll"));

    papplClientHTMLPuts(client,
			"            </tbody>\n"
			"          </table>"
			"        </form>\n");
  }

  papplClientHTMLPrinterFooter(client);

  if (num_options)
    cupsFreeOptions(num_options, options);
}


//
// 'ps_printer_extra_setup()' - Extra code for setting up a printer, for
//                              example to get extra buttons/pages on the web
//                              interface for this printer.
//

static void   ps_printer_extra_setup(pappl_printer_t *printer,
				     void *data)
{
  char                   path[256];     // Path to resource
  pappl_system_t         *system;	// System
  pappl_pr_driver_data_t driver_data;
  ps_driver_extension_t  *extension;


  system = papplPrinterGetSystem(printer);

  papplPrinterGetDriverData(printer, &driver_data);
  extension = (ps_driver_extension_t *)driver_data.extension;
  if (extension->defaults_pollable ||
      extension->installable_options)
  {
    papplPrinterGetPath(printer, "device", path, sizeof(path));
    papplSystemAddResourceCallback(system, path, "text/html",
			     (pappl_resource_cb_t)ps_printer_web_device_config,
			     printer);
    papplPrinterAddLink(printer, "Device Settings", path,
			PAPPL_LOPTIONS_NAVIGATION | PAPPL_LOPTIONS_STATUS);
  }
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
  filter_data.iscanceledfunc = ps_job_is_canceled;
  filter_data.iscanceleddata = job;
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
  filter_data_t      filter_data;
  FILE               *devout;

  // Log function for output to device
  filter_data.logfunc = ps_job_log; // Job log function catching page counts
                                    // ("PAGE: XX YY" messages)
  filter_data.logdata = job;
  // Function to indicate that the job got canceled
  filter_data.iscanceledfunc = ps_job_is_canceled;
  filter_data.iscanceleddata = job;
  // Load PPD file and determine the PPD options equivalent to the job options
  job_data = ps_create_job_data(job, options);
  // The filter has no output, data is going directly to the device
  nullfd = open("/dev/null", O_RDWR);
  // Create file descriptor/pipe to which the functions of libppd can send
  // the data so that it gets passed on to the device
  job_data->device_fd = filterPOpen(ps_print_filter_function, -1, nullfd,
				    0, &filter_data, device,
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
// 'ps_setup_driver_list()' - Create a driver list of the available PPD files.
//

static void
ps_setup_driver_list(pappl_system_t *system)      // I - System
{
  int              i, j, k;
  char             *ptr1, *ptr2;
  char             *generic_ppd, *mfg_mdl, *mdl, *dev_id;
  ps_ppd_path_t    *ppd_path;
  int              num_options = 0;
  cups_option_t    *options = NULL;
  cups_array_t     *ppds;
  ppd_info_t       *ppd;
  char             buf1[1024], buf2[1024];
  int              pre_normalized;
  pappl_pr_driver_t swap;


  //
  // Create the list of all available PPD files
  //

  ppds = ppdCollectionListPPDs(ppd_collections, 0,
			       num_options, options,
			       (filter_logfunc_t)papplLog, system);

  //
  // Create driver list from the PPD list and submit it
  //
  
  if (ppds)
  {
    i = 0;
    num_drivers = cupsArrayCount(ppds);
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Found %d PPD files.", num_drivers);
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
    if (drivers)
      free(drivers);
    drivers = (pappl_pr_driver_t *)calloc(num_drivers + PPD_MAX_PROD,
					  sizeof(pappl_pr_driver_t));
    // Create list of PPD file paths
    if (ppd_paths)
      cupsArrayDelete(ppd_paths);
    ppd_paths = cupsArrayNew(ps_compare_ppd_paths, NULL);
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
	      // Convert device ID to make/model string, so that we can add
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
	  snprintf(buf1, sizeof(buf1) - 1, "%s%s (%s)",
		   mfg_mdl,
		   (!strncmp(ppd->record.name, extra_ppd_dir,
			     strlen(extra_ppd_dir)) ? " - USER-ADDED" : ""),
		   ppd->record.languages[0]);
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
			       ps_autoadd, ps_printer_extra_setup,
			       ps_driver_setup, ppd_paths);
}


//
// 'ps_setup()' - Setup PostScript driver.
//

static void
ps_setup(pappl_system_t *system)      // I - System
{
  int              i;
  char             *ptr1, *ptr2;
  ppd_collection_t *col = NULL;
  ps_filter_data_t *ps_filter_data,
                   *pdf_filter_data;

  //
  // Create PPD collection index data structure
  //

  ppd_paths = cupsArrayNew(ps_compare_ppd_paths, NULL);
  ppd_collections = cupsArrayNew(NULL, NULL);

  //
  // Build PPD list from all repositories
  //

  if ((ptr1 = getenv("PPD_PATHS")) != NULL)
  {
    strncpy(ppd_dirs_env, ptr1, sizeof(ppd_dirs_env));
    ptr1 = ppd_dirs_env;
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

  //
  // Last entry in the list is the directory for the user to drop
  // extra PPD files in via the web interface
  //

  if (col && !extra_ppd_dir[0])
    strncpy(extra_ppd_dir, col->path, sizeof(extra_ppd_dir));

  // XXX Check whether extra_ppd_dir exists and is writable, create if possible

  //
  // Create the list of all available PPD files
  //

  ps_setup_driver_list(system);

  //
  // Add web admin interface page for adding PPD files
  //

  papplSystemAddResourceCallback(system, "/addppd", "text/html",
				 (pappl_resource_cb_t)ps_system_web_add_ppd,
				 system);
  papplSystemAddLink(system, "Add PPD Files", "/addppd",
		     PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);

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
// 'ps_system_web_add_ppd()' - Web interface page for adding/deleting
//                             PPD files by the user, to add support for
//                             printers not supported by the built-in PPD
//                             files
//

static void
ps_system_web_add_ppd(
    pappl_client_t *client,		// I - Client
    pappl_system_t *system)		// I - System
{
  int                 i;                // Looping variable
  const char          *status = NULL;	// Status text, if any
  const char          *uri = NULL;      // Client URI
  pappl_version_t     version;
  cups_array_t        *uploaded,        // List of uploaded PPDs, to delete
                                        // them in certain error conditions
                      *accepted_report, // Report of accepted PPDs with warnings
                      *rejected_report; // Report of rejected PPDs with errors
  cups_dir_t          *dir;             // User PPD file directory
  cups_dentry_t       *dent;            // Entry in user PPD file directory
  cups_array_t        *user_ppd_files;  // List of user-uploaded PPD files


  if (!papplClientHTMLAuthorize(client))
    return;

  // Create arrays to log PPD file upload
  uploaded = cupsArrayNew3(NULL, NULL, NULL, 0, NULL,
			   (cups_afree_func_t)free);
  accepted_report = cupsArrayNew3(NULL, NULL, NULL, 0, NULL,
				  (cups_afree_func_t)free);
  rejected_report = cupsArrayNew3(NULL, NULL, NULL, 0, NULL,
				  (cups_afree_func_t)free);

  // Handle POSTs to add and delete PPD files...
  if (papplClientGetMethod(client) == HTTP_STATE_POST)
  {
    int			num_form = 0;	// Number of form variables
    cups_option_t	*form = NULL;	// Form variables
    cups_option_t	*opt;
    const char		*action;	// Form action
    char                strbuf[2048],
                        destpath[2048];	// File destination path
    const char	        *content_type;	// Content-Type header
    const char	        *boundary;	// boundary value for multi-part
    http_t              *http;
    bool                error = false;
    bool                ppd_repo_changed = false; // PPD(s) added or removed?
    char		*ptr;		// Pointer into string


    http = papplClientGetHTTP(client);
    content_type = httpGetField(http, HTTP_FIELD_CONTENT_TYPE);
    if (!strcmp(content_type, "application/x-www-form-urlencoded"))
    {
      // URL-encoded form data, PPD file uploads not possible in this format 
      // Use papplClientGetForm() to do the needed decoding
      error = true;
      if ((num_form = papplClientGetForm(client, &form)) == 0)
      {
	status = "Invalid form data.";
      }
      else if (!papplClientIsValidForm(client, num_form, form))
      {
	status = "Invalid form submission.";
      }
      else
	error = false;
    }
    else if (!strncmp(content_type, "multipart/form-data; ", 21) &&
	     (boundary = strstr(content_type, "boundary=")) != NULL)
    {
      // Multi-part form data, probably we have a PPD file upload
      // Use our own reading method to allow for submitting more than
      // one PPD file through the single input widget and for a larger
      // total amount of data
      char	buf[32768],		// Message buffer
		*bufinptr,		// Pointer end of incoming data
		*bufreadptr,		// Pointer for reading buffer
		*bufend;		// End of message buffer
      size_t	body_size = 0;		// Size of message body
      ssize_t	bytes;			// Bytes read
      http_state_t initial_state;	// Initial HTTP state
      char	name[1024],		// Form variable name
		filename[1024],		// Form filename
		bstring[256],		// Boundary string to look for
		*bend,			// End of value (boundary)
		*line;			// Start of line
      size_t	blen;			// Length of boundary string
      FILE      *fp = NULL;
      ppd_file_t *ppd = NULL;		// PPD file data for verification


      // Read one buffer full of data, then search for \r only up to a
      // position in the buffer so that the boundary string still fits
      // after the \r, check for line end \r\n (in header) or boundary
      // string (after header, file/value), then save value into
      // "form" option list or file data into destination file, move
      // rest of buffer content (after line break/boundary) to
      // beginning of buffer, read rest of buffer space full,
      // continue. If no line end found, error (too long line), if no
      // boundary found, error in case of option value, write to
      // destination file in case of file. Move rest of buffer content
      // to the beginning of buffer, read buffer full, continue.
      initial_state = httpGetState(http);

      // Format the boundary string we are looking for...
      snprintf(bstring, sizeof(bstring), "\r\n--%s", boundary + 9);
      blen = strlen(bstring);
      papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
		     "Boundary string: \"%s\", %ld bytes",
		     bstring, (long)blen);

      // Parse lines in the message body...
      name[0] = '\0';
      filename[0] = '\0';

      for (bufinptr = buf, bufend = buf + sizeof(buf);
	   (bytes = httpRead2(http, bufinptr,
			      (size_t)(bufend - bufinptr))) > 0 ||
	     bufinptr > buf;)
      {
	body_size += (size_t)bytes;
	papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
		       "Bytes left over: %ld; Bytes read: %ld; Total bytes read: %ld",
		       (long)(bufinptr - buf), (long)bytes, (long)body_size);
	bufinptr += bytes;
	*bufinptr = '\0';

	for (bufreadptr = buf; bufreadptr < bufinptr;)
        {
	  if (fp == NULL)
	  {
	    // Split out a line...
	    for (line = bufreadptr; bufreadptr < bufinptr - 1; bufreadptr ++)
	    {
	      if (!memcmp(bufreadptr, "\r\n", 2))
	      {
		*bufreadptr = '\0';
		bufreadptr += 2;
		break;
	      }
	    }

	    if (bufreadptr >= bufinptr)
	      break;
	  }

	  if (!*line || fp)
          {
	    papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
			   "Data (value or file).");

	    // End of headers, grab value...
	    if (!name[0])
	    {
	      // No name value...
	      papplLogClient(client, PAPPL_LOGLEVEL_ERROR,
			     "Invalid multipart form data: Form field name missing.");
	      // Stop here
	      status = "Invalid form data.";
	      error = true;
	      break;
	    }

	    for (bend = bufinptr - blen - 2,
		   ptr = memchr(bufreadptr, '\r', (size_t)(bend - bufreadptr));
		 ptr;
		 ptr = memchr(ptr + 1, '\r', (size_t)(bend - ptr - 1)))
	    {
	      // Check for boundary string...
	      if (!memcmp(ptr, bstring, blen))
		break;
	    }

	    if (!ptr && !filename[0])
	    {
	      // When reading a file, write out curremt data into destination
	      // file, when not reading a file, error out
	      // No boundary string, invalid data...
	      papplLogClient(client, PAPPL_LOGLEVEL_ERROR,
			     "Invalid multipart form data: Form field %s: File without filename or excessively long value.",
			     name);
	      // Stop here
	      status = "Invalid form data.";
	      error = true;
	      break;
	    }

	    // Value/file data is in buffer range from ptr to bend,
	    // Reading continues at bufreadptr
	    if (ptr)
	    {
	      bend       = ptr;
	      ptr        = bufreadptr;
	      bufreadptr = bend + blen;
	    }
	    else
	    {
	      ptr        = bufreadptr;
	      bufreadptr = bend;
	    }

	    if (filename[0])
	    {
	      // Save data of an embedded file...

	      // New file
	      if (fp == NULL) // First data portion
	      {
		snprintf(destpath, sizeof(destpath), "%s/%s", extra_ppd_dir,
			 filename);
		papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
			       "Creating file: %s", destpath);
		if ((fp = fopen(destpath, "w+")) == NULL)
		{
		  papplLogClient(client, PAPPL_LOGLEVEL_ERROR,
				 "Unable to create file: %s",
				 strerror(errno));
		  // Report error
		  snprintf(strbuf, sizeof(strbuf),
			   "%s: Cannot create file - %s",
			   filename, strerror(errno));
		  cupsArrayAdd(rejected_report, strdup(strbuf));
		  // Stop here
		  status = "Error uploading PPD file(s), uploading stopped.";
		  error = true;
		  break;
		}
	      }

	      if (fp)
	      {
		// Write the data
		while (bend > ptr) // We have data to write
		{
		  bytes = fwrite(ptr, 1, (size_t)(bend - ptr), fp);
		  papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
				 "Bytes to write: %ld; %ld bytes written",
				 (long)(bend - ptr), (long)bytes);
		  if (errno)
		  {
		    papplLogClient(client, PAPPL_LOGLEVEL_ERROR,
				   "Error writing into file %s: %s",
				   destpath, strerror(errno));
		    // Report error
		    snprintf(strbuf, sizeof(strbuf),
			     "%s: Cannot write file - %s",
			     filename, strerror(errno));
		    cupsArrayAdd(rejected_report, strdup(strbuf));
		    // PPD incomplete, close and delete it.
		    fclose(fp);
		    fp = NULL;
		    unlink(destpath);
		    // Stop here
		    status = "Error uploading PPD file(s), uploading stopped.";
		    error = true;
		    break;
		  }
		  ptr += bytes;
		}

		// Close the file and verify whether it is a usable PPD file
		if (bufreadptr > bend) // We have read the boundary string
		{
		  fclose(fp);
		  fp = NULL;
		  // Default is PPD_CONFORM_RELAXED, needs testing XXX
		  ppdSetConformance(PPD_CONFORM_STRICT);
		  if ((ppd = ppdOpenFile(destpath)) == NULL)
		  {
		    ppd_status_t err;		// Last error in file
		    int		 linenum;	// Line number in file

		    // Log error
		    err = ppdLastError(&linenum);
		    papplLogClient(client, PAPPL_LOGLEVEL_ERROR,
				   "PPD %s: %s on line %d", destpath,
				   ppdErrorString(err), linenum);
		    // PPD broken (or not a PPD), delete it.
		    unlink(destpath);
		    // Report error
		    snprintf(strbuf, sizeof(strbuf),
			     "%s: Not a PPD or file corrupted", filename);
		    cupsArrayAdd(rejected_report, strdup(strbuf));
		  }
		  else
		  {
		    // Check for cupsFilter(2) entries and issue warning
		    if (ppd->num_filters)
		    {
		      // We have at least one "*cupsFilter(2):..." line
		      // in the PPD file
		      // Log success with warning
		      snprintf(strbuf, sizeof(strbuf),
			       "%s: WARNING: CUPS driver PPD, possibly non-PostScript",
			       filename);
		      cupsArrayAdd(accepted_report, strdup(strbuf));
		    }
		    else
		    {
		      // Log success without warning
		      snprintf(strbuf, sizeof(strbuf), "%s: OK", filename);
		      cupsArrayAdd(accepted_report, strdup(strbuf));
		    }
		    ppdClose(ppd);
		    // New PPD added, so driver list needs update
		    ppd_repo_changed = true;
		    // Log the addtion of the PPD file
		    cupsArrayAdd(uploaded, strdup(destpath));
		  }
		  ppdSetConformance(PPD_CONFORM_RELAXED);
		}
	      }
	    }
	    else
	    {
	      // Save the form variable...
	      *bend = '\0';
	      num_form = cupsAddOption(name, ptr, num_form, &form);
	      papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
			     "Form variable: %s=%s", name, ptr);

	      // If we have found the session ID (comes before the first PPD
	      // file), verify it
	      if (!strcasecmp(name, "session") &&
		  !papplClientIsValidForm(client, num_form, form))
	      {
		papplLogClient(client, PAPPL_LOGLEVEL_ERROR,
			       "Invalid session ID: %s",
			       ptr);
		// Remove already uploaded PPD files ...
		while (ptr = cupsArrayFirst(uploaded))
		{
		  unlink(ptr);
		  cupsArrayRemove(uploaded, ptr);
		}
		// ... and the reports about them
		while (cupsArrayRemove(accepted_report,
				       cupsArrayFirst(accepted_report)));
		while (cupsArrayRemove(rejected_report,
				       cupsArrayFirst(rejected_report)));
		// Stop here
		status = "Invalid form submission.";
		error = true;
		break;
	      }
	    }

	    if (fp == NULL)
	    {
	      name[0]     = '\0';
	      filename[0] = '\0';

	      if (bufreadptr < (bufinptr - 1) &&
		  bufreadptr[0] == '\r' && bufreadptr[1] == '\n')
		bufreadptr += 2;
	    }

	    break;
	  }
	  else
	  {
	    papplLogClient(client, PAPPL_LOGLEVEL_DEBUG, "Line '%s'.", line);

	    if (!strncasecmp(line, "Content-Disposition:", 20))
            {
	      if ((ptr = strstr(line + 20, " name=\"")) != NULL)
	      {
		strncpy(name, ptr + 7, sizeof(name));

		if ((ptr = strchr(name, '\"')) != NULL)
		  *ptr = '\0';
	      }

	      if ((ptr = strstr(line + 20, " filename=\"")) != NULL)
	      {
		strncpy(filename, ptr + 11, sizeof(filename));

		if ((ptr = strchr(filename, '\"')) != NULL)
		  *ptr = '\0';
	      }
	      if (filename[0])
		papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
			       "Found file from form field \"%s\" with file "
			       "name \"%s\"",
			       name, filename);
	      else
		papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
			       "Found value for field \"%s\"", name);
	    }

	    break;
	  }
	}

	if (error)
	  break;

	if (bufinptr > bufreadptr)
	{
	  memmove(buf, bufreadptr, (size_t)(bufinptr - bufreadptr));
	  bufinptr = buf + (bufinptr - bufreadptr);
	}
	else
	  bufinptr = buf;
      }
      papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
		     "Read %ld bytes of form data (%s).",
		     (long)body_size, content_type);

      // Flush remaining data...
      if (httpGetState(http) == initial_state)
	httpFlush(http);
    }

    strbuf[0] = '\0';
    for (i = num_form, opt = form; i > 0; i --, opt ++)
      snprintf(strbuf + strlen(strbuf), sizeof(strbuf) - strlen(strbuf) - 1,
	       "%s=%s ", opt->name, opt->value);
    if (strbuf[0])
      strbuf[strlen(strbuf) - 1] = '\0';

    papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
		   "Form variables: %s", strbuf);

    // Check non-file form input values
    if (!error)
    {
      if ((action = cupsGetOption("action", num_form, form)) == NULL)
      {
	status = "Missing action.";
	error = true;
      }
      else if (!strcmp(action, "add-ppdfiles"))
	status = "PPD file(s) uploaded.";
      else if (!strcmp(action, "delete-ppdfiles"))
      {
	for (i = num_form, opt = form; i > 0;
	     i --, opt ++)
	  if (opt->name[0] == '\t')
	  {
	    snprintf(destpath, sizeof(destpath), "%s/%s", extra_ppd_dir,
		     opt->name + 1);
	    papplLogClient(client, PAPPL_LOGLEVEL_DEBUG,
			   "Deleting file: %s", destpath);
	    unlink(destpath);
	    ppd_repo_changed = true;
	  }
	if (ppd_repo_changed)
	  status = "PPD file(s) deleted.";
	else
	  status = "No PPD file selected for deletion.";
      }
      else
      {
	status = "Unknown action.";
	error = true;
      }
      if (error)
      {
	// Remove already uploaded PPD files ...
	while (ptr = cupsArrayFirst(uploaded))
	{
	  unlink(ptr);
	  cupsArrayRemove(uploaded, ptr);
	}
	// ... and the reports about them
	while (cupsArrayRemove(accepted_report,
			       cupsArrayFirst(accepted_report)));
	while (cupsArrayRemove(rejected_report,
			       cupsArrayFirst(rejected_report)));
      }
    }

    // Refresh driver list (if at least 1 PPD got added or removed)
    if (ppd_repo_changed)
      ps_setup_driver_list(system);

    cupsFreeOptions(num_form, form);
  }

  if (!papplClientRespond(client, HTTP_STATUS_OK, NULL, "text/html", 0, 0))
    goto clean_up;
  papplClientHTMLHeader(client, "Add support for extra printers", 0);
  if (papplSystemGetVersions(system, 1, &version) > 0)
    papplClientHTMLPrintf(client,
                          "    <div class=\"header2\">\n"
                          "      <div class=\"row\">\n"
                          "        <div class=\"col-12 nav\">\n"
                          "          Version %s\n"
                          "        </div>\n"
                          "      </div>\n"
                          "    </div>\n", version.sversion);
  papplClientHTMLPuts(client, "    <div class=\"content\">\n");

  papplClientHTMLPrintf(client,
			"      <div class=\"row\">\n"
			"        <div class=\"col-12\">\n"
			"          <h1 class=\"title\">Add support for extra printer models</h1>\n");

  if (status)
    papplClientHTMLPrintf(client, "          <div class=\"banner\">%s</div>\n", status);

  papplClientHTMLPuts(client,
		      "        <h3>Add the PPD file(s) of your printer(s)</h3>\n");
  papplClientHTMLPuts(client,
		      "        <p>If your printer is not already supported by this Printer Application, you can add support for it by uploading your printer's PPD file here. Only add PPD files for PostScript printers, PPD files of CUPS drivers do not work with this Printer Application!</p>\n");

  uri = papplClientGetURI(client);

  // Multi-part, as we want to upload a PPD file here
  papplClientHTMLStartForm(client, uri, true);

  papplClientHTMLPuts(client,
		      "          <table class=\"form\">\n"
		      "            <tbody>\n");

  if (cupsArrayCount(rejected_report))
  {
    for (i = 0; i < cupsArrayCount(rejected_report); i ++)
      papplClientHTMLPrintf(client,
			    (i == 0 ?
			     "              <tr><th>Upload&nbsp;failed:</th><td>%s</td></tr>\n" :
			     "              <tr><th></th><td>%s</td></tr>\n"),
			    (char *)cupsArrayIndex(rejected_report, i));
    papplClientHTMLPuts(client,
			"              <tr><th></th><td></td></tr>\n");
  }
  if (cupsArrayCount(accepted_report))
  {
    for (i = 0; i < cupsArrayCount(accepted_report); i ++)
      papplClientHTMLPrintf(client,
			    (i == 0 ?
			     "              <tr><th>Uploaded:</th><td>%s</td></tr>\n" :
			     "              <tr><th></th><td>%s</td></tr>\n"),
			    (char *)cupsArrayIndex(accepted_report, i));
    papplClientHTMLPuts(client,
			"              <tr><th></th><td></td></tr>\n");
  }
  papplClientHTMLPuts(client,
		      "              <tr><th><label for=\"ppdfiles\">PPD&nbsp;file(s):</label></th><td><input type=\"file\" name=\"ppdfiles\" accept=\".ppd,.PPD,.ppd.gz,.PPD.gz\" required multiple></td><td>(Only individual PPD files, no PPD-generating executables)</td></tr>\n");
  papplClientHTMLPuts(client,
		      "              <tr><th></th><td><button type=\"submit\" name=\"action\" value=\"add-ppdfiles\">Add PPDs</button></td><td></td></tr>\n");
  papplClientHTMLPuts(client,
		      "            </tbody>\n"
		      "          </table>\n"
		      "        </form>\n");

  if ((dir = cupsDirOpen(extra_ppd_dir)) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_WARN,
	     "Unable to read user PPD directory '%s': %s",
	     extra_ppd_dir, strerror(errno));
  }
  else
  {
    user_ppd_files = cupsArrayNew3((cups_array_func_t)strcasecmp,
				   NULL, NULL, 0, NULL,
				   (cups_afree_func_t)free);
    while ((dent = cupsDirRead(dir)) != NULL)
      if (dent->filename[0] && dent->filename[0] != '.')
	cupsArrayAdd(user_ppd_files, strdup(dent->filename));

    cupsDirClose(dir);

    if (cupsArrayCount(user_ppd_files))
    {
      papplClientHTMLPrintf(client, "          <hr>\n");

      papplClientHTMLPuts(client,
			  "          <h3>User-uploaded PPD files</h3>\n");

      papplClientHTMLPuts(client,
			  "          <p>To remove files, mark them and click the \"Delete\" button</p>\n");

      papplClientHTMLStartForm(client, uri, false);
      papplClientHTMLPuts(client,
			  "          <table class=\"form\">\n"
			  "            <tbody>\n");

      for (i = 0; i < cupsArrayCount(user_ppd_files); i ++)
	papplClientHTMLPrintf(client,
			      "              <tr><th><input type=\"checkbox\" name=\"\t%s\"></th><td>%s</td></tr>\n",
			      (char *)cupsArrayIndex(user_ppd_files, i),
			      (char *)cupsArrayIndex(user_ppd_files, i));

      papplClientHTMLPuts(client, "          <tr><th></th><td><input type=\"hidden\" name=\"action\" value=\"delete-ppdfiles\"><input type=\"submit\" value=\"Delete\"></td>\n");

      papplClientHTMLPuts(client,
			  "            </tbody>\n"
			  "          </table>\n"
			  "        </form>\n");
    }

    cupsArrayDelete(user_ppd_files);
  }

  papplClientHTMLPuts(client,
                      "      </div>\n"
                      "    </div>\n");
  papplClientHTMLFooter(client);

 clean_up:
  // Clean up
  cupsArrayDelete(uploaded);
  cupsArrayDelete(accepted_report);
  cupsArrayDelete(rejected_report);
}


//
// 'ps_status()' - Get printer status.
//

static bool                   // O - `true` on success, `false` on failure
ps_status(
    pappl_printer_t *printer) // I - Printer
{
  int                    i;
  pappl_system_t         *system;              // System
  pappl_pr_driver_data_t driver_data;
  ps_driver_extension_t  *extension;
  ipp_t                  *driver_attrs,
                         *vendor_attrs;
  ipp_attribute_t        *attr;
  char                   buf[1024];


  // Get system...
  system = papplPrinterGetSystem(printer);

  papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		  "Status callback called.");

  // Load the driver data
  papplPrinterGetDriverData(printer, &driver_data);
  driver_attrs = papplPrinterGetDriverAttributes(printer);
  extension = (ps_driver_extension_t *)driver_data.extension;
  if (!extension->updated)
  {
    if ((attr = ippFindAttribute(driver_attrs, "installable-options-default",
				 IPP_TAG_ZERO)) != NULL &&
	ippAttributeString(attr, buf, sizeof(buf)) > 0)
      papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		      "Applying installable accessories settings: %s", buf);
    else
      papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		      "Installable Options settings not found");

    // Update the driver data to correspond with the printer hardware
    // accessory configuration ("Installable Options" in the PPD)
    ps_driver_setup(system, NULL, NULL, NULL, &driver_data, &driver_attrs,
		    NULL);

    // Copy the vendor option IPP attributes
    vendor_attrs = ippNew();
    for (i = 0; i < driver_data.num_vendor; i ++)
    {
      snprintf(buf, sizeof(buf), "%s-default", driver_data.vendor[i]);
      attr = ippFindAttribute(driver_attrs, buf, IPP_TAG_ZERO);
      if (attr)
	ippCopyAttribute(vendor_attrs, attr, 0);
      snprintf(buf, sizeof(buf), "%s-supported", driver_data.vendor[i]);
      attr = ippFindAttribute(driver_attrs, buf, IPP_TAG_ZERO);
      if (attr)
	ippCopyAttribute(vendor_attrs, attr, 0);
    }

    // Save the updated driver data back to the printer
    papplPrinterSetDriverData(printer, &driver_data, NULL);

    // Save the vendor options IPP attributes back into the driver attributes
    driver_attrs = papplPrinterGetDriverAttributes(printer);
    ippCopyAttributes(driver_attrs, vendor_attrs, 0, NULL, NULL);
    ippDelete(vendor_attrs);

    // Save new default settings
    papplSystemSaveState(system, STATE_FILE);
  }

  // Use commandtops CUPS filter code to check status here (ink levels, ...)
  // XXX

  // Do PostScript jobs for polling only once a minute or once every five
  // minutes, therefore save time of last call in a static variable. and
  // only poll again if last poll is older than given time.

  // Needs ink level support in PAPPL
  // (https://github.com/michaelrsweet/pappl/issues/83)

  return (true);
}


//
// 'ps_testpage()' - Return a test page file to print
//

static const char *			// O - Filename or `NULL`
ps_testpage(
    pappl_printer_t *printer,		// I - Printer
    char            *buffer,		// I - File Buffer
    size_t          bufsize)		// I - Buffer Size
{
  const char	    *str;		// String pointer


  // Find the right test file...
  if ((str = getenv("TESTPAGE_DIR")) != NULL)
    snprintf(buffer, bufsize, "%s/%s", str, TESTPAGE);
  else if ((str = getenv("TESTPAGE")) != NULL)
    snprintf(buffer, bufsize, "%s", str);
  else
    snprintf(buffer, bufsize, "%s/%s", TESTPAGEDIR, TESTPAGE);

  // Does it actually exist?
  if (access(buffer, R_OK))
  {
    papplLogPrinter(printer, PAPPL_LOGLEVEL_ERROR,
		    "Test page %s not found or not readable.", buffer);
    *buffer = '\0';
    return (NULL);
  }
  else
  {
    papplLogPrinter(printer, PAPPL_LOGLEVEL_DEBUG,
		    "Using test page: %s", buffer);
    return (buffer);
  }
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
			     (void *)STATE_FILE);
  papplSystemSetVersions(system,
			 (int)(sizeof(versions) / sizeof(versions[0])),
			 versions);

  if (!papplSystemLoadState(system, STATE_FILE))
    papplSystemSetDNSSDName(system,
			    system_name ? system_name : SYSTEM_NAME);

  return (system);
}
