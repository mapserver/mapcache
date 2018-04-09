#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <apr_general.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <sqlite3.h>
#include "mapcache.h"
#include "ezxml.h"


// Command line options
const apr_getopt_option_t optlist[] = {
///  { "verbose",   'v', FALSE, "Display full report as a YAML document" },
  { "config",    'c', TRUE,  "configuration file (/path/to/mapcache.xml)" },
  { "dimension", 'D', TRUE,  "set the value of a dimension: format"
                               " DIMENSIONNAME=VALUE. Can be used multiple"
                               " times for multiple dimensions" },
  { "tileset",   't', TRUE,  "tileset to analyze" },
  { "grid",      'g', TRUE,  "grid to analyze" },
  { "extent",    'e', TRUE,  "extent to analyze: format minx,miny,maxx,maxy" },
  { "zoom",      'z', TRUE,  "Set min and max zoom levels to analyze,"
                               " separated by a comma, eg: 12,15" },
  { "query",     'q', TRUE,  "Set query for counting tiles in a rectangle" },
  { "endofopt",   0,  FALSE, "End of options" }
};


// Mapcache log function
void mapcache_log(mapcache_context *ctx, mapcache_log_level lvl, char *msg, ...)
{
  va_list args;
  va_start(args, msg);
  vfprintf(stderr, msg, args);
  va_end(args);
  fprintf(stderr, "\n");
}


// Replace all occurrences of substr in string
char * str_replace_all(apr_pool_t *pool, const char *string,
                       const char *substr, const char *replacement)
{
  char * replaced = apr_pstrdup(pool, string);
  while (strstr(replaced, substr)) {
    replaced = mapcache_util_str_replace(pool, string, substr, replacement);
  }
  return replaced;
}


// Build up actual SQLite filename from dbfile template
char * dbfilename(apr_pool_t * pool, char * template,
                  mapcache_tileset * tileset, mapcache_grid * grid,
                  apr_array_header_t * dimensions, apr_hash_t * fmt, int z,
                  int tilx, int tily, int xcount, int ycount)
{
  char * path = apr_pstrdup(pool, template);
  if (!strstr(path, "{")) {
    return path;
  }

  // Tileset and grid
  path = str_replace_all(pool, path, "{tileset}", tileset->name);
  path = str_replace_all(pool, path, "{grid}", grid->name);

  // Dimensions, both {dim} and {dim:foo}
  if (strstr(path, "{dim") && dimensions) {
    char * dimstr = "";
    int i = dimensions->nelts;
    while(i--) {
      mapcache_requested_dimension *entry;
      const char * val;
      char *solodim;
      entry = APR_ARRAY_IDX(dimensions, i, mapcache_requested_dimension*);
      val = mapcache_util_str_sanitize(pool, entry->cached_value, "/.", '#');
      solodim = apr_pstrcat(pool, "{dim:", entry->dimension->name, "}", NULL);
      dimstr = apr_pstrcat(pool, dimstr, "#", val, NULL);
      if(strstr(path, solodim)) {
        path = str_replace_all(pool, path, solodim, val);
      }
    }
    path = str_replace_all(pool, path, "{dim}", dimstr);
  }


  // Zoom level
  path = str_replace_all(pool, path, "{z}",
      apr_psprintf(pool, apr_hash_get(fmt, "z", APR_HASH_KEY_STRING), z));

  // X coordinate
  if (xcount > 0) {
    int maxx = grid->levels[z]->maxx;
    char * curfmt;
    curfmt = apr_hash_get(fmt, "x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{x}",
        apr_psprintf(pool, curfmt, tilx/xcount*xcount));
    curfmt = apr_hash_get(fmt, "div_x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{div_x}",
        apr_psprintf(pool, curfmt, tilx/xcount));
    curfmt = apr_hash_get(fmt, "inv_x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_x}",
        apr_psprintf(pool, curfmt, (maxx-1-tilx)/xcount*xcount));
    curfmt = apr_hash_get(fmt, "inv_div_x", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_div_x}",
        apr_psprintf(pool, curfmt, (maxx-1-tilx)/xcount));
  }

  // Y coordinate
  if (ycount > 0) {
    int maxy = grid->levels[z]->maxy;
    char * curfmt;
    curfmt = apr_hash_get(fmt, "y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{y}",
        apr_psprintf(pool, curfmt, tily/ycount*ycount));
    curfmt = apr_hash_get(fmt, "div_y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{div_y}",
        apr_psprintf(pool, curfmt, tily/ycount));
    curfmt = apr_hash_get(fmt, "inv_y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_y}",
        apr_psprintf(pool, curfmt, (maxy-1-tily)/ycount*ycount));
    curfmt = apr_hash_get(fmt, "inv_div_y", APR_HASH_KEY_STRING);
    path = str_replace_all(pool, path, "{inv_div_y}",
        apr_psprintf(pool, curfmt, (maxy-1-tily)/ycount));
  }

  return path;
}


// Query SQLite for getting tile count in specified area
int count_tiles(mapcache_context * ctx, const char * dbfile,
                const char * count_query, mapcache_extent til, int z,
                mapcache_grid * grid, mapcache_tileset * tileset,
                apr_array_header_t * dimensions)
{
  sqlite3 * db;
  sqlite3_stmt * res;
  int rc, idx;
  int count;

  rc = sqlite3_open_v2(dbfile, &db, SQLITE_OPEN_READONLY, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    return 0;
  }
  sqlite3_busy_timeout(db, 5000);

  rc = sqlite3_prepare_v2(db, count_query, -1, &res, 0);
  if (rc != SQLITE_OK) {
    ctx->set_error(ctx, 500, "SQLite failed: '%s'", sqlite3_errmsg(db));
    sqlite3_close(db);
    return 0;
  }

  idx = sqlite3_bind_parameter_index(res, ":minx");
  if (idx) sqlite3_bind_int(res, idx, til.minx);
  idx = sqlite3_bind_parameter_index(res, ":miny");
  if (idx) sqlite3_bind_int(res, idx, til.miny);
  idx = sqlite3_bind_parameter_index(res, ":maxx");
  if (idx) sqlite3_bind_int(res, idx, til.maxx);
  idx = sqlite3_bind_parameter_index(res, ":maxy");
  if (idx) sqlite3_bind_int(res, idx, til.maxy);
  idx = sqlite3_bind_parameter_index(res, ":z");
  if (idx) sqlite3_bind_int(res, idx, z);
  idx = sqlite3_bind_parameter_index(res, ":grid");
  if (idx) sqlite3_bind_text(res, idx, grid->name, -1, SQLITE_STATIC);
  idx = sqlite3_bind_parameter_index(res, ":tileset");
  if (idx) sqlite3_bind_text(res, idx, tileset->name, -1, SQLITE_STATIC);
  idx = sqlite3_bind_parameter_index(res, ":dim");
  if (idx) {
    char * dim = "";
    if (dimensions) {
      mapcache_tile tile;
      tile.dimensions = dimensions;
      dim = mapcache_util_get_tile_dimkey(ctx, &tile, NULL, NULL);
    }
    sqlite3_bind_text(res, idx, dim, -1, SQLITE_STATIC);
  }

  rc = sqlite3_step(res);
  switch (rc) {
    case SQLITE_ROW:
      count = strtol((const char*)sqlite3_column_text(res, 0), NULL, 10);
      break;
    case SQLITE_DONE:
      ctx->set_error(ctx, 500, "SQLite returned no tile count");
      count = 0;
      break;
    default:
      ctx->set_error(ctx, 500, "SQLite failed: '%s'", sqlite3_errmsg(db));
      count = 0;
  }

  sqlite3_finalize(res);
  sqlite3_close(db);

  return count;
}


int main(int argc, char * argv[])
{
  mapcache_context ctx;
  apr_getopt_t * opt;
  int status;
  int optk;
  const char * optv;
///  int verbose = FALSE;
  int json_output = TRUE;
  const char * config_file = NULL;
  const char * tileset_name = NULL;
  const char * grid_name = NULL;
  const char * dim_spec = NULL;
  const char * count_query = NULL;
  mapcache_tileset * tileset;
  mapcache_grid * grid;
  mapcache_cache * cache;
  apr_array_header_t * dimensions = NULL;
  char * cache_dbfile = NULL;
  apr_hash_t * xyz_fmt;
  int i, ix, iy, iz;
  ezxml_t doc, node;
  ezxml_t cache_node = NULL;
  ezxml_t dbfile_node = NULL;
  char * text;
  apr_hash_index_t * hi;
  int cache_xcount, cache_ycount;
  mapcache_extent bbox = { 0, 0, 0, 0 };
  double * list = NULL;
  int nelts;
  int minzoom = 0, maxzoom = 0;
  mapcache_extent pix, til, db;
  double resolution;
  double coverage;
  double cache_max = 0, cache_cached = 0;
  apr_off_t cache_size = 0;


  // Initialize Apache runtime and Mapcache context
  apr_initialize();
  apr_pool_create(&ctx.pool, NULL);
  mapcache_context_init(&ctx);
  ctx.config = mapcache_configuration_create(ctx.pool);
  ctx.log = mapcache_log;


  // Parse command-line options
  apr_getopt_init(&opt, ctx.pool, argc, (const char*const*)argv);
  while ((status = apr_getopt_long(opt, optlist, &optk, &optv)) == APR_SUCCESS)
  {
    switch (optk) {
///      case 'v': // --verbose
///        verbose = TRUE;
///        break;
      case 'c': // --config <config_file>
        config_file = optv;
        break;
      case 't': // --tileset <tileset_name>
        tileset_name = optv;
        break;
      case 'g': // --grid <grid_name>
        grid_name = optv;
        break;
      case 'D': // --dimension <dim_spec>
        if (!dim_spec) {
          dim_spec = optv;
        } else {
          dim_spec = apr_pstrcat(ctx.pool, dim_spec, ":", optv, NULL);
        }
        break;
      case 'q': // --query <count_query>
        count_query = optv;
        break;
      case 'e': // --extent <minx>,<miny>,<maxx>,<maxy>
        if (mapcache_util_extract_double_list(&ctx, optv, ",", &list, &nelts)
            != MAPCACHE_SUCCESS || nelts != 4)
        {
          ctx.set_error(&ctx, 500, "failed to parse extent, expected four"
                        " real numbers separated by comma, got: \"%s\"", optv);
        }
        bbox.minx = list[0];
        bbox.miny = list[1];
        bbox.maxx = list[2];
        bbox.maxy = list[3];
        break;
      case 'z': // --zoom <z>
        minzoom = strtol(optv, &text, 10);
        maxzoom = minzoom;
        if (*text == ',') {
          maxzoom = strtol(text+1, &text, 10);
        }
        if (*text != '\0') {
          ctx.set_error(&ctx, 500, "bad int format for -z option: %s", optv);
          goto failure;
        }
        break;

    }
  }
  if (status != APR_EOF) {
    ctx.set_error(&ctx, 500, "Bad options");
    goto failure;
  }


  // Load Mapcache configuration file in Mapcache internal data structure
  if (!config_file) {
    ctx.set_error(&ctx, 500, "Configuration file has not been specified"
                             " (need: --config <file>)");
    goto failure;
  }
  mapcache_configuration_parse(&ctx, config_file, ctx.config, 0);
  if (ctx.get_error(&ctx)) goto failure;


  // Load MapCache configuration again, this time as an XML document, in order
  // to gain access to settings that are unreacheable from Mapcache API
  doc = ezxml_parse_file(config_file);


  // Retrieve tileset information
  if (!tileset_name) {
    ctx.set_error(&ctx, 500, "tileset has not been specified"
                             " (need: --tileset <name>)");
    goto failure;
  }
  tileset = mapcache_configuration_get_tileset(ctx.config, tileset_name);
  if (!tileset) {
    ctx.set_error(&ctx, 500, "tileset \"%s\" has not been found"
                             " in configuration", tileset_name);
    goto failure;
  }


  // Retrieve grid information
  if (!grid_name) {
    ctx.set_error(&ctx, 500, "grid has not been specified"
                             " (need: --grid <name>)");
    goto failure;
  }
  for (i=0 ; i<tileset->grid_links->nelts ; i++) {
    mapcache_grid_link * grid_link;
    grid_link = APR_ARRAY_IDX(tileset->grid_links, i, mapcache_grid_link*);
    if (strcmp(grid_link->grid->name, grid_name)==0) {
      grid = grid_link->grid;
      break;
    }
  }
  if (!grid) {
    ctx.set_error(&ctx, 500, "grid \"%s\" has not been found in \"%s\""
                             " tileset config.", grid_name, tileset->name);
    goto failure;
  }


  // Retrieve cache information
  cache = tileset->_cache;
  if (cache->type != MAPCACHE_CACHE_SQLITE) {
    ctx.set_error(&ctx, 500, "cache \"%s\" of tileset \"%s\" is not of"
                             " type SQLite", cache->name, tileset->name);
    goto failure;
  }
  for (node = ezxml_child(doc, "cache") ; node ; node = node->next) {
    if (strcmp(ezxml_attr(node, "name"), cache->name) == 0) {
      cache_node = node;
      break;
    }
  }
  if (!cache_node) {
    ctx.set_error(&ctx, 500, "cache \"%s\" has not been not found",
                             cache->name);
    goto failure;
  }
  dbfile_node = ezxml_child(cache_node, "dbfile");
  cache_dbfile = dbfile_node->txt;
  if (!cache_dbfile) {
    ctx.set_error(&ctx, 500, "Failed to parse <dbfile> tag of cache \"%s\"",
                             cache->name);
    goto failure;
  }
  xyz_fmt = apr_hash_make(ctx.pool);
  apr_hash_set(xyz_fmt, "x",         APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "y",         APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "z",         APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "inv_x",     APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "inv_y",     APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "div_x",     APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "div_y",     APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "inv_div_x", APR_HASH_KEY_STRING, "(not set)");
  apr_hash_set(xyz_fmt, "inv_div_y", APR_HASH_KEY_STRING, "(not set)");
  for (hi = apr_hash_first(ctx.pool, xyz_fmt) ; hi ; hi = apr_hash_next(hi)) {
    const char *key, *val, *attr;
    apr_hash_this(hi, (const void**)&key, NULL, (void**)&val);
    attr = apr_pstrcat(ctx.pool, key, "_fmt", NULL);
    val = ezxml_attr(dbfile_node, attr);
    if (!val) val = "%d";
    apr_hash_set(xyz_fmt, key, APR_HASH_KEY_STRING, val);
  }
  cache_xcount = cache_ycount = -1;
  text = ezxml_child(node, "xcount")->txt;
  if (text) cache_xcount = (int)strtol(text, NULL, 10);
  text = ezxml_child(node, "ycount")->txt;
  cache_ycount = (int)strtol(text, NULL, 10);


  // Retrieve dimensions information
  if (tileset->dimensions) {
    // Set up dimensions with default values
    dimensions = apr_array_make(ctx.pool, tileset->dimensions->nelts,
                                sizeof(mapcache_requested_dimension*));
    for ( i=0 ; i < tileset->dimensions->nelts ; i++ ) {
      mapcache_dimension * dim;
      mapcache_requested_dimension * reqdim;
      dim = APR_ARRAY_IDX(tileset->dimensions, i, mapcache_dimension*);
      reqdim = apr_pcalloc(ctx.pool, sizeof(mapcache_requested_dimension));
      reqdim->dimension = dim;
      reqdim->requested_value = dim->default_value;
      reqdim->cached_value = dim->default_value;
      APR_ARRAY_PUSH(dimensions, mapcache_requested_dimension*) = reqdim;
    }
    // Update dimensions with values specified with --dim command line option
    // syntax is: "dim1=value1:dim2=value2:..."
    if (dim_spec) {
      char * ds = apr_pstrdup(ctx.pool, dim_spec);
      char *kvp, *last;
      for (kvp=apr_strtok(ds, ":", &last)
           ; kvp ; kvp = apr_strtok(NULL, ":", &last))
      {
        char *key, *val, *sav;
        key = apr_strtok(kvp, "=", &sav);
        val = apr_strtok(NULL, "=", &sav);
        if (!key || !val) {
          ctx.set_error(&ctx, 500, "Can't parse dimension settings: %s\n",
                                   dim_spec);
          goto failure;
        }
        mapcache_set_requested_dimension(&ctx, dimensions, key, val);
        mapcache_set_cached_dimension(&ctx, dimensions, key, val);
        if (GC_HAS_ERROR(&ctx)) goto failure;
      }
    }
    // Check that dimension values are valid
    for ( i=0 ; i < dimensions->nelts ; i++) {
      mapcache_requested_dimension *entry;
      apr_array_header_t * vals;
      entry = APR_ARRAY_IDX(dimensions, i, mapcache_requested_dimension*);
      vals = entry->dimension->get_entries_for_value(&ctx,
                entry->dimension, entry->requested_value, tileset, NULL, grid);
      if (GC_HAS_ERROR(&ctx)) goto failure;
      if (!vals || vals->nelts == 0) {
        ctx.set_error(&ctx, 500, "invalid value \"%s\" for dimension \"%s\"\n",
                      entry->requested_value, entry->dimension->name);
        goto failure;
      }
    }
  }


  // Set default query for counting tiles in a SQLite cache file
  if (!count_query) {
    count_query = "SELECT count(rowid)"
                  "  FROM tiles"
                  " WHERE (x between :minx and :maxx)"
                  "   AND (y between :miny and :maxy)"
                  "   AND tileset=:tileset AND grid=:grid AND dim=:dim";
  }


  // Check requested bounding box and zoom level with respect to grid extent
  if (bbox.minx < grid->extent.minx || bbox.minx > grid->extent.maxx) {
    ctx.set_error(&ctx, 500, "Lower left X coordinate %.18g not in valid"
                             "interval [ %.18g, %.18g ]", bbox.minx,
                             grid->extent.minx, grid->extent.maxx);
    goto failure;
  }
  if (bbox.miny < grid->extent.miny || bbox.miny > grid->extent.maxy) {
    ctx.set_error(&ctx, 500, "Lower left Y coordinate %.18g not in valid"
                             "interval [ %.18g, %.18g ]", bbox.miny,
                             grid->extent.miny, grid->extent.maxy);
    goto failure;
  }
  if (bbox.maxx < grid->extent.minx || bbox.maxx > grid->extent.maxx) {
    ctx.set_error(&ctx, 500, "Upper right X coordinate %.18g not in valid"
                             "interval [ %.18g, %.18g ]", bbox.maxx,
                             grid->extent.minx, grid->extent.maxx);
    goto failure;
  }
  if (bbox.maxy < grid->extent.miny || bbox.maxy > grid->extent.maxy) {
    ctx.set_error(&ctx, 500, "Upper right Y coordinate %.18g not in valid"
                             "interval [ %.18g, %.18g ]", bbox.maxy,
                             grid->extent.miny, grid->extent.maxy);
    goto failure;
  }
  if (minzoom < 0 || minzoom >= grid->nlevels) {
    ctx.set_error(&ctx, 500, "Zoom level %d not in valid interval [ %d, %d ]",
                  minzoom, 0, grid->nlevels-1);
    goto failure;
  }
  if (maxzoom < 0 || maxzoom >= grid->nlevels) {
    ctx.set_error(&ctx, 500, "Zoom level %d not in valid interval [ %d, %d ]",
                  maxzoom, 0, grid->nlevels-1);
    goto failure;
  }
  // Swap bounds and zoom levels if inverted
  if (bbox.minx > bbox.maxx) {
    double swap = bbox.minx;
    bbox.minx = bbox.maxx;
    bbox.maxx = swap;
  }
  if (bbox.miny > bbox.maxy) {
    double swap = bbox.miny;
    bbox.miny = bbox.maxy;
    bbox.maxy = swap;
  }
  if (minzoom > maxzoom) {
    int swap = minzoom;
    minzoom = maxzoom;
    maxzoom = swap;
  }

  if (json_output) {
    printf("{\n");
    printf("  \"layer\": \"%s\",\n", tileset->name);
    printf("  \"grid\": \"%s\",\n", grid->name);
    printf("  \"extent\": [ %.18g, %.18g, %.18g, %.18g ],\n",
        bbox.minx, bbox.miny, bbox.maxx, bbox.maxy);
    printf("  \"unit\": \"%s\",\n", grid->unit==MAPCACHE_UNIT_METERS? "m":
                                    grid->unit==MAPCACHE_UNIT_DEGREES?"dd":
                                                                      "ft");
    printf("  \"zoom_levels\": [\n");
  }

  // Loop on all requested zoom levels
  for (iz = minzoom ; iz <=maxzoom ; iz ++) {
    double zoom_max = 0, zoom_cached = 0;

    if (json_output) {
      if (iz > minzoom) printf(",\n");
      printf("    {\n");
      printf("      \"level\": %d,\n", iz);
      printf("      \"files\": [\n");
    }

    // Convert bounding box coordinates in pixels, tiles and DB files
    resolution = grid->levels[iz]->resolution;
    pix.minx = floor((bbox.minx-grid->extent.minx)/resolution);
    pix.miny = floor((bbox.miny-grid->extent.miny)/resolution);
    pix.maxx = floor((bbox.maxx-grid->extent.minx)/resolution);
    pix.maxy = floor((bbox.maxy-grid->extent.miny)/resolution);
    til.minx = floor(pix.minx/grid->tile_sx);
    til.miny = floor(pix.miny/grid->tile_sy);
    til.maxx = floor(pix.maxx/grid->tile_sx);
    til.maxy = floor(pix.maxy/grid->tile_sy);
    db.minx  = floor(til.minx/cache_xcount);
    db.miny  = floor(til.miny/cache_ycount);
    db.maxx  = floor(til.maxx/cache_xcount);
    db.maxy  = floor(til.maxy/cache_ycount);

    // List DB files containing portions of bounding box
    // and count cached tiles belonging to bounding box
    for (ix = db.minx ; ix <= db.maxx ; ix++) {
      for (iy = db.miny ; iy <= db.maxy ; iy++) {
        int x;
        int y;
        char * dbfile;
        apr_file_t * filehandle;
        apr_finfo_t fileinfo;
        int nbtiles_file_max, nbtiles_file_cached;
        int nbtiles_extent_max, nbtiles_extent_cached;
        mapcache_extent area;
        const mapcache_extent full_extent = {
              0, 0, (double)INT_MAX, (double)INT_MAX };

        // Build up BD file name
        x = ix * cache_xcount;
        y = iy * cache_ycount;
        dbfile = dbfilename(ctx.pool, cache_dbfile, tileset, grid, dimensions,
            xyz_fmt, iz, x, y, cache_xcount, cache_ycount);

        fileinfo.size = 0;
        nbtiles_file_max = cache_xcount * cache_ycount;
        nbtiles_file_cached = 0;
        area.minx = fmaxl(x, til.minx);
        area.miny = fmaxl(y, til.miny);
        area.maxx = fminl(x+cache_xcount-1, til.maxx);
        area.maxy = fminl(y+cache_ycount-1, til.maxy);
        nbtiles_extent_max = (area.maxx-area.minx+1)*(area.maxy-area.miny+1);
        nbtiles_extent_cached = 0;

        // If file exists, get its size and its cached tile counts both in
        // total and in specified extent
        if (APR_SUCCESS == apr_file_open(&filehandle, dbfile,
                                         APR_FOPEN_READ|APR_FOPEN_BINARY,
                                         APR_FPROT_OS_DEFAULT, ctx.pool))
        {
          // Get file size
          apr_file_info_get(&fileinfo, APR_FINFO_SIZE, filehandle);
          apr_file_close(filehandle);

          // Get number of cached tiles within extent present in file
          nbtiles_extent_cached = count_tiles(&ctx, dbfile, count_query, til,
                                              iz, grid, tileset, dimensions);

          // Get total number of cached tiles present in file
          nbtiles_file_cached = count_tiles(&ctx, dbfile, count_query,
                                            full_extent, iz, grid, tileset,
                                            dimensions);
        }
        zoom_max += nbtiles_extent_max;
        zoom_cached += nbtiles_extent_cached;
        cache_max += nbtiles_extent_max;
        cache_cached += nbtiles_extent_cached;
        cache_size += fileinfo.size;

        if (json_output) {
          coverage = (double)nbtiles_extent_cached/(double)nbtiles_extent_max;
          if (ix > db.minx || iy > db.miny) printf(",\n");
          printf("        {\n");
          printf("          \"name\": \"%s\",\n", dbfile);
          printf("          \"size\": %jd,\n", (intmax_t)fileinfo.size);
          printf("          \"nb_tiles\": {\n");
          printf("            \"file_maximum\": %d,\n", nbtiles_file_max);
          printf("            \"file_cached\": %d,\n", nbtiles_file_cached);
          printf("            \"extent_maximum\": %d,\n", nbtiles_extent_max);
          printf("            \"extent_cached\": %d\n", nbtiles_extent_cached);
          printf("          },\n");
          printf("          \"coverage\": %3.5g\n", coverage);
          printf("        }");
        }
      }
    }

    if (json_output) {
      coverage = zoom_cached / zoom_max;
      printf("\n      ],\n");
      printf("      \"nb_tiles\": {\n");
      printf("        \"extent_maximum\": %.18g,\n", zoom_max);
      printf("        \"extent_cached\": %.18g\n", zoom_cached);
      printf("      },\n");
      printf("      \"coverage\": %3.5g\n", coverage);
      printf("    }");
    }
  }


  if (json_output) {
    apr_off_t tile_size = (apr_off_t)(cache_size/cache_cached);
    apr_off_t missing_size = (apr_off_t)(cache_max-cache_cached)*tile_size;
    coverage = cache_cached / cache_max;
    printf("\n  ],\n");
    printf("  \"nb_tiles\": {\n");
    printf("    \"extent_maximum\": %.18g,\n", cache_max);
    printf("    \"extent_cached\": %.18g\n", cache_cached);
    printf("  },\n");
    printf("  \"extent_cached_size\": %jd,\n", (intmax_t)cache_size);
    printf("  \"avg_tile_size\": %jd,\n", tile_size);
    printf("  \"estimated_missing_tile_size\": %jd,\n", missing_size);
    printf("  \"coverage\": %3.5g\n", coverage);
    printf("}\n");
  }


// success:
  apr_terminate();
  return 0;


failure:
  fprintf(stderr, "%s: %s\n", argv[0], ctx.get_error_message(&ctx));
  apr_terminate();
  return 1;
}
