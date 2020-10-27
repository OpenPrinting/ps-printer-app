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

# include <pappl/pappl.h>
# include <ppd/ppd.h>
# include <cupsfilters/log.h>
# include <cupsfilters/filter.h>
# include <cups/cups.h>
# include <string.h>
# include <limits.h>

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


//
// Local globals...
//

// PPD collections used as drivers

static const char * const col_paths[] =  // PPD collection dirs
{
 "/usr/lib/cups/driver",
 "/usr/share/ppd"
};


//
// Local functions...
//

static bool   ps_callback(pappl_system_t *system, const char *driver_name,
			  const char *device_uri,
			  pappl_driver_data_t *driver_data,
			  ipp_t **driver_attrs, void *data);
static int    ps_compare_ppd_paths(void *a, void *b, void *data); 
bool          ps_filter(pappl_job_t *job, pappl_device_t *device, void *data);
static void   ps_identify(pappl_printer_t *printer,
			  pappl_identify_actions_t actions,
			  const char *message);
static void   ps_job_log(void *data, filter_loglevel_t level,
			 const char *message, ...);
static void   ps_media_col(pwg_size_t *pwg_size, const char *def_source,
			   const char *def_type, int left_offset,
			   int top_ofsset, pappl_media_tracking_t tracking,
			   pappl_media_col_t *col);
int           ps_print_filter_function(int inputfd, int outputfd,
				       int inputseekable, int *jobcanceled,
				       filter_data_t *data, void *parameters);
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
  papplMainloop(argc, argv, "1.0", NULL, 0, NULL, NULL, NULL, NULL, NULL, system_cb, NULL, NULL);
  return (0);
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
    pappl_driver_data_t  *driver_data,     // O - Driver data
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

  search_ppd_path.driver_name = driver_name;
  ppd_path = (ps_ppd_path_t *)cupsArrayFind(ppd_paths,
					    &search_ppd_path);
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
  driver_data->identify           = ps_identify;
  driver_data->identify_default   = PAPPL_IDENTIFY_ACTIONS_SOUND;
  driver_data->identify_supported = PAPPL_IDENTIFY_ACTIONS_DISPLAY |
    PAPPL_IDENTIFY_ACTIONS_SOUND;
  driver_data->print              = NULL;
  driver_data->rendjob            = NULL;
  driver_data->rendpage           = NULL;
  driver_data->rstartjob          = NULL;
  driver_data->rstartpage         = NULL;
  driver_data->rwriteline         = NULL;
  driver_data->status             = ps_status;
  driver_data->testpage           = NULL; // XXX Add ps_testpage() function
  driver_data->format             = "application/vnd.printer-specific";
  driver_data->orient_default     = IPP_ORIENT_NONE;
  driver_data->quality_default    = IPP_QUALITY_NORMAL;

  // Make and model
  strncpy(driver_data->make_and_model,
	  ppd->nickname,
	  sizeof(driver_data->make_and_model));

  // Print speed in pages per minute
  driver_data->ppm = ppd->throughput;
  if (ppd->color_device)
    driver_data->ppm_color = ppd->throughput;
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
  // XXX Check ColorModel option for defaults
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
      PAPPL_PWG_RASTER_TYPE_SRGB_8 | PAPPL_PWG_RASTER_TYPE_SRGB_16;
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
    choice = ppdFindMarkedChoice(ppd, pc->sides_option);
    if (strcmp(choice->choice, pc->sides_2sided_long) == 0)
      driver_data->sides_default = PAPPL_SIDES_TWO_SIDED_LONG_EDGE;
  }
  if (pc->sides_2sided_short)
  {
    driver_data->sides_supported |= PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
    driver_data->duplex = PAPPL_DUPLEX_NORMAL;
    choice = ppdFindMarkedChoice(ppd, pc->sides_option);
    if (strcmp(choice->choice, pc->sides_2sided_short) == 0)
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
// 'ps_filter()' - PAPPL generic filter function wrapper
//

bool
ps_filter(
    pappl_job_t    *job,		// I - Job
    pappl_device_t *device,		// I - Device
    void           *data)		// I - Filter data
{
  int                   i, j, count;
  ppd_file_t            *ppd = NULL;	// PPD file loaded from collection
  ppd_cache_t           *pc;
  ps_filter_data_t	*psfd = (ps_filter_data_t *)data;
  pappl_driver_data_t   driver_data;    // Printer driver data
  const char            *ppd_path;
  const char		*filename;	// Input filename
  int			fd;		// Input file descriptor
  int                   nullfd;         // File descriptor for /dev/null
  pappl_joptions_t	pappl_options;	// Job options
  bool			ret = false;	// Return value
  int		        num_options = 0;// Number of PPD print options
  cups_option_t	        *options = NULL;// PPD print options
  cups_option_t         *opt;
  filter_data_t         filter_data;    // Data for supplying to filter
                                        // function
  ipp_t                 *driver_attrs;  // Printer (driver) IPP attributes
  char                  buf[1024];      // Buffer for building strings
  const char            *choicestr,     // Chice name from PPD option
                        *val;           // Value string from IPP option
  ipp_t                 *attrs;         // IPP Attributes structure
  ipp_t		        *media_col,	// media-col IPP structure
                        *media_size;	// media-size IPP structure
  ipp_attribute_t       *attr;
  int                   pcm;            // Print color mode: 0: mono,
                                        // 1: color (for presets)
  int                   pq;             // IPP Print quality value (for presets)
  int		        num_presets;	// Number of presets
  cups_option_t	        *presets;       // Presets of PPD options
  ppd_option_t          *option;        // PPD option
  ppd_choice_t          *choice;        // Choice in PPD option
  ppd_attr_t            *ppd_attr;
  cups_array_t          *chain;         // Filter function chain
  filter_filter_in_chain_t *filter,     // Filter function call for filtering
                           *print;      // Filter function call for printing
  int                   jobcanceled = 0;// Is job canceled?
  pappl_printer_t       *printer = papplJobGetPrinter(job);
  int                   xres, yres;

  //
  // Load the printer's assigned PPD file, mark the defaults, and create the
  // cache
  //

  papplPrinterGetDriverData(printer, &driver_data);
  ppd_path = (const char *)driver_data.extension;
  if ((ppd =
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
  }

  ppdMarkDefaults(ppd);
  if ((pc = ppdCacheCreateWithPPD(ppd)) != NULL)
    ppd->cache = pc;

  driver_attrs = papplPrinterGetDriverAttributes(printer);

  //
  // Open the input file...
  //

  filename = papplJobGetFilename(job);
  if ((fd = open(filename, O_RDONLY)) < 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to open JPEG file '%s': %s", filename, strerror(errno));
    return (false);
  }

  //
  // Get job options and request the image data in the format we need...
  //

  papplJobGetOptions(job, &pappl_options, INT_MAX, 1);

  //
  // Find the PPD (or filter) options corresponding to the job options
  //

  // Job options without PPD equivalent
  //  - print-content-optimize
  //  - print-darkness
  //  - darkness-configured
  //  - print-speed

  // page-ranges (filter option)
  if (pappl_options.first_page > 1 || pappl_options.last_page < INT_MAX)
  {
    snprintf(buf, sizeof(buf), "%d-%d",
	     pappl_options.first_page, pappl_options.last_page);
    num_options = cupsAddOption("page-ranges", buf, num_options, &options);
  }

  // Finishings
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding options for finishings");
  if (pappl_options.finishings & PAPPL_FINISHINGS_PUNCH)
    num_options = ppdCacheGetFinishingOptions(pc, NULL,
					      IPP_FINISHINGS_PUNCH,
					      num_options, &options);
  if (pappl_options.finishings & PAPPL_FINISHINGS_STAPLE)
    num_options = ppdCacheGetFinishingOptions(pc, NULL,
					      IPP_FINISHINGS_STAPLE,
					      num_options, &options);
  if (pappl_options.finishings & PAPPL_FINISHINGS_TRIM)
    num_options = ppdCacheGetFinishingOptions(pc, NULL,
					      IPP_FINISHINGS_TRIM,
					      num_options, &options);

  // PageSize/media/media-size/media-size-name
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: PageSize");
  attrs = ippNew();
  media_col = ippNew();
  media_size = ippNew();
  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"x-dimension", pappl_options.media.size_width);
  ippAddInteger(media_size, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"y-dimension", pappl_options.media.size_length);
  ippAddCollection(media_col, IPP_TAG_PRINTER, "media-size", media_size);
  ippDelete(media_size);
  ippAddString(media_col, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-size-name",
	       NULL, pappl_options.media.size_name);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"media-left-margin", pappl_options.media.left_margin);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"media-right-margin", pappl_options.media.right_margin);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"media-top-margin", pappl_options.media.top_margin);
  ippAddInteger(media_col, IPP_TAG_PRINTER, IPP_TAG_INTEGER,
		"media-bottom-margin", pappl_options.media.bottom_margin);
  ippAddCollection(attrs, IPP_TAG_PRINTER, "media-col", media_col);
  ippDelete(media_col);
  if ((choicestr = ppdCacheGetPageSize(pc, attrs, NULL, NULL)) != NULL)
    num_options = cupsAddOption("PageSize", choicestr, num_options, &options);
  ippDelete(attrs);

  // InputSlot/media-source
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: InputSlot");
  if ((choicestr = ppdCacheGetInputSlot(pc, NULL,
					pappl_options.media.source)) !=
      NULL)
    num_options = cupsAddOption("InputSlot", choicestr, num_options,
				&options);

  // MediaType/media-type
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: MediaType");
  if ((choicestr = ppdCacheGetMediaType(pc, NULL,
					pappl_options.media.type)) != NULL)
    num_options = cupsAddOption("MediaType", choicestr, num_options,
				&options);

  // orientation-requested (filter option)
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
	      "Adding option: orientation-requested");
  if (pappl_options.orientation_requested >= IPP_ORIENT_PORTRAIT &&
      pappl_options.orientation_requested <  IPP_ORIENT_NONE)
  {
    snprintf(buf, sizeof(buf), "%d", pappl_options.orientation_requested);
    num_options = cupsAddOption("orientation-requested", buf, num_options,
				&options);
  }

  // Presets, selected by color/bw and print quality
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG,
	      "Adding option presets depending on requested print quality");
  if (ppd->color_device &&
      (pappl_options.print_color_mode &
       (PAPPL_COLOR_MODE_AUTO | PAPPL_COLOR_MODE_COLOR)) != 0)
    pcm = 1;
  else
    pcm = 0;
  if (pappl_options.print_quality == IPP_QUALITY_DRAFT)
    pq = 0;
  else if (pappl_options.print_quality == IPP_QUALITY_HIGH)
    pq = 2;
  else
    pq = 1;
  num_presets = pc->num_presets[pcm][pq];
  presets     = pc->presets[pcm][pq];
  for (i = 0; i < num_presets; i ++)
    num_options = cupsAddOption(presets[i].name, presets[i].value,
				num_options, &options);

  // print-scaling (filter option)
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: print-scaling");
  if (pappl_options.print_scaling)
  {
    if (pappl_options.print_scaling & PAPPL_SCALING_AUTO)
      num_options = cupsAddOption("print-scaling", "auto", num_options,
				  &options);
    if (pappl_options.print_scaling & PAPPL_SCALING_AUTO_FIT)
      num_options = cupsAddOption("print-scaling", "auto-fit", num_options,
				  &options);
    if (pappl_options.print_scaling & PAPPL_SCALING_FILL)
      num_options = cupsAddOption("print-scaling", "fill", num_options,
				  &options);
    if (pappl_options.print_scaling & PAPPL_SCALING_FIT)
      num_options = cupsAddOption("print-scaling", "fit", num_options,
				  &options);
    if (pappl_options.print_scaling & PAPPL_SCALING_NONE)
      num_options = cupsAddOption("print-scaling", "none", num_options,
				  &options);
  }

  // Resolution/printer-resolution
  // Only add a "Resolution" option if there is none yet (from presets)
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: Resolution");
  if (cupsGetOption("Resolution", num_options, options) == NULL)
  {
    if ((pappl_options.printer_resolution[0] &&
	 ((attr = papplJobGetAttribute(job, "printer-resolution")) != NULL ||
	  (attr = papplJobGetAttribute(job, "Resolution")) != NULL) &&
	 (option = ppdFindOption(ppd, "Resolution")) != NULL &&
	 (count = option->num_choices) > 0))
    {
      for (i = 0, choice = option->choices; i < count; i ++, choice ++)
      {
	if ((j = sscanf(choice->choice, "%dx%d", &xres, &yres)) == 1)
	  yres = xres;
	else if (j <= 0)
	  continue;
	if (pappl_options.printer_resolution[0] == xres &&
	    (pappl_options.printer_resolution[1] == yres ||
	     (pappl_options.printer_resolution[1] == 0 && xres == yres)))
	  break;
      }
      if (i < count)
	num_options = cupsAddOption("Resolution", choice->choice, num_options,
				    &options);
    }
    else if (pappl_options.printer_resolution[0])
    {
      if (pappl_options.printer_resolution[1] &&
	  pappl_options.printer_resolution[0] !=
	  pappl_options.printer_resolution[1])
	snprintf(buf, sizeof(buf) - 1, "%dx%ddpi",
		 pappl_options.printer_resolution[0],
		 pappl_options.printer_resolution[1]);
      else
	snprintf(buf, sizeof(buf) - 1, "%ddpi",
		 pappl_options.printer_resolution[0]);
      num_options = cupsAddOption("Resolution", buf, num_options,
				  &options);
    }
    else if ((ppd_attr = ppdFindAttr(ppd, "DefaultResolution", NULL)) != NULL)
      num_options = cupsAddOption("Resolution", ppd_attr->value, num_options,
				  &options);
  }

  // Duplex/sides
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: Duplex");
  if (pappl_options.sides && pc->sides_option)
  {
    if (pappl_options.sides & PAPPL_SIDES_ONE_SIDED &&
	pc->sides_1sided)
      num_options = cupsAddOption(pc->sides_option,
				  pc->sides_1sided,
				  num_options, &options);
    else if (pappl_options.sides & PAPPL_SIDES_TWO_SIDED_LONG_EDGE &&
	     pc->sides_2sided_long)
      num_options = cupsAddOption(pc->sides_option,
				  pc->sides_2sided_long,
				  num_options, &options);
    else if (pappl_options.sides & PAPPL_SIDES_TWO_SIDED_SHORT_EDGE &&
	     pc->sides_2sided_short)
      num_options = cupsAddOption(pc->sides_option,
				  pc->sides_2sided_short,
				  num_options, &options);
  }

  //
  // Find further options directly from the job IPP attributes
  //

  // OutputBin/output-bin
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Adding option: OutputBin");
  if ((attr = papplJobGetAttribute(job, "output-bin")) == NULL)
    attr = ippFindAttribute(driver_attrs, "output-bin-default",
			    IPP_TAG_ZERO);
  if (attr)
  {
    val = ippGetString(attr, 0, NULL);
    if ((choicestr = ppdCacheGetOutputBin(pc, val)) != NULL)
      num_options = cupsAddOption("OutputBin", choicestr, num_options,
				  &options);
  }

  // XXX User driver_data.bin_default (int) for default

  // XXX "Collate" option

  // XXX Control Gray/Color printing?

  //
  // Create data record to call filter functions
  //

  filter_data.job_id = papplJobGetID(job);
  filter_data.job_user = strdup(papplJobGetUsername(job));
  filter_data.job_title = strdup(papplJobGetName(job));
  filter_data.copies = pappl_options.copies;
  filter_data.job_attrs = NULL; // We use PPD/filter options
  filter_data.printer_attrs = NULL;  // We use the printer's PPD file
  filter_data.num_options = num_options;
  filter_data.options = options; // PPD/filter options
  filter_data.ppdfile = NULL;
  filter_data.ppd = ppd;
  filter_data.logfunc = ps_job_log; // Job log function catching page counts
                                    // ("PAGE: XX YY" messages)
  filter_data.logdata = job;

  snprintf(buf, sizeof(buf) - 1, "Calling filter with PPD options:");
  for (i = num_options, opt = options; i > 0; i --, opt ++)
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1,
	     " %s=%s", opt->name, opt->value);
  papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "%s", buf);  

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

  free(filter_data.job_user);
  free(filter_data.job_title);
  free(filter);
  free(print);
  cupsArrayDelete(chain);
  ppdClose(ppd);
  close(fd);
  close(nullfd);

  return (ret);
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
  //close(fd);
  close(inputfd);
  close(outputfd);
  return (0);
}


//
// 'ps_setup()' - Setup PostScript driver.
//

static void
ps_setup(pappl_system_t *system)      // I - System
{
  int              i;
  const char       *col_paths_env;
  char             *ptr1, *ptr2;
  cups_array_t     *ppd_paths,
                   *ppd_collections;
  ppd_collection_t *col;
  ps_ppd_path_t    *ppd_path;
  int              num_options = 0;
  cups_option_t    *options = NULL;
  int              num_ppds;
  cups_array_t     *ppds;
  ppd_info_t       *ppd;
  char             buf[1024];
  pappl_driver_t   *drivers = NULL;
  ps_filter_data_t *ps_filter_data,
                   *image_filter_data,
                   *raster_filter_data,
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
    strncpy(buf, col_paths_env, sizeof(buf));
    ptr1 = buf;
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

  // XXX Add driver name "auto" as very first entry, with this ps_callback
  //     will call ppdCollectionListPPDs() with the device ID as filter and
  //     a limit to just 1 PPD file (this should also happen when the Printer
  //     Application starts and the PPD for a configured printer cannot be
  //     found (removed or renamed in update).

  //
  // Create driver list from the PPD list and submit it
  //
  
  if (ppds)
  {
    num_ppds = cupsArrayCount(ppds);
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Found %d PPD files.\n", num_ppds);
    drivers = (pappl_driver_t *)calloc(num_ppds, sizeof(pappl_driver_t));
    for (ppd = (ppd_info_t *)cupsArrayFirst(ppds), i = 0;
	 ppd;
	 ppd = (ppd_info_t *)cupsArrayNext(ppds), i ++)
    {
      ppd_path = (ps_ppd_path_t *)calloc(1, sizeof(ps_ppd_path_t));
      // XXX Return to more human-readable driver names
      snprintf(buf, sizeof(buf) - 1, "D%010d", i);
      drivers[i].name = strdup(buf);
      ppd_path->driver_name = strdup(buf);
      ppd_path->ppd_path = strdup(ppd->record.name);
      cupsArrayAdd(ppd_paths, ppd_path);
      // XXX Better human-readable names, Nickname can be duplicate, use
      //     product + driver part of NickName + (language)
      snprintf(buf, sizeof(buf) - 1, "%s (%s)",
	       ppd->record.make_and_model,
	       ppd->record.languages[0]);
      drivers[i].description = strdup(buf);
      drivers[i].device_id = strdup(ppd->record.device_id);
      free(ppd);
      papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	       "PPD %s: %s - File: %s Device ID: %s", drivers[i].name,
	       drivers[i].description, ppd_path->ppd_path,
	       drivers[i].device_id);
    }
    cupsArrayDelete(ppds);
  }
  else
    papplLog(system, PAPPL_LOGLEVEL_FATAL, "No PPD files found.");

  papplSystemSetDrivers(system, num_ppds, drivers,
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

  image_filter_data =
    (ps_filter_data_t *)calloc(1, sizeof(ps_filter_data_t));
  image_filter_data->filter_function = imagetops;
  image_filter_data->filter_parameters = "IMG";
  papplSystemAddMIMEFilter(system,
			   "image/jpeg", "application/vnd.printer-specific",
			   ps_filter, image_filter_data);

  // XXX Chain rastertops with pstops? (for multi-page and page logging)
  raster_filter_data =
    (ps_filter_data_t *)calloc(1, sizeof(ps_filter_data_t));
  raster_filter_data->filter_function = rastertops;
  raster_filter_data->filter_parameters = "RASTER";
  papplSystemAddMIMEFilter(system,
			   "image/pwg-raster",
			   "application/vnd.printer-specific",
			   ps_filter, raster_filter_data);
  papplSystemAddMIMEFilter(system,
			   "image/urf", "application/vnd.printer-specific",
			   ps_filter, raster_filter_data);

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
                                   PAPPL_SOPTIONS_STANDARD |
                                   PAPPL_SOPTIONS_LOG |
                                   PAPPL_SOPTIONS_NETWORK |
                                   PAPPL_SOPTIONS_SECURITY |
                                   PAPPL_SOPTIONS_TLS;
					// System options
  static pappl_version_t versions[1] =	// Software versions
  {
    { "PostScript Printer Application", "", "1.0", { 1, 0, 0, 0 } }
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
			 system_name ? system_name : "PS Printer App",
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
			    system_name ? system_name : "PS Printer App");

  return (system);
}
