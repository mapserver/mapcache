/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: high level service dispatching
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2011 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include "mapcache.h"
#include <apr_strings.h>
#include <math.h>

/** \addtogroup services */
/** @{ */


void mapcache_service_dispatch_request(mapcache_context *ctx, mapcache_request **request, char *pathinfo, apr_table_t *params, mapcache_cfg *config)
{
  int i;
  mapcache_service *service = NULL;

  /* skip leading /'s */
  while(pathinfo && (*pathinfo) == '/')
    ++pathinfo;

  /* set default url prefix or skip empty pathinfo */
  if((!pathinfo || strlen(pathinfo) == 0) && config->default_service) {
      pathinfo = apr_pstrdup(ctx->pool,config->default_service->url_prefix);
  } else if(!pathinfo) {
    ctx->set_error(ctx,404,"missing a service");
    return;
  }

  /*skip leading /'s */
  while((*pathinfo) == '/')
    ++pathinfo;

  for(i=0; i<MAPCACHE_SERVICES_COUNT; i++) {
    /* loop through the services that have been configured */
    int prefixlen;
    service = config->services[i];
    if(!service) continue; /* skip an unconfigured service */
    prefixlen = strlen(service->url_prefix);
    if(strncmp(service->url_prefix,pathinfo, prefixlen)) continue; /*skip a service who's prefix does not correspond */
    ctx->service = service;
    //if(*(pathinfo+prefixlen)!='/' && *(pathinfo+prefixlen)!='\0') continue; /*we matched the prefix but there are trailing characters*/
    pathinfo += prefixlen; /* advance pathinfo to after the service prefix */
    service->parse_request(ctx,service,request,pathinfo,params,config);
    if(*request)
      (*request)->service = service;

    /* stop looping on services */
    return;
  }

  if (config->default_service) {
    /* no matching url prefix of any service:
       assume request should go to the default service */
    service = config->default_service;
    ctx->service = service;
    ctx->service->parse_request(ctx,service,request,pathinfo,params,config);

    if(*request)
      (*request)->service = service;

    return;
  }

  ctx->set_error(ctx,404,"unknown service %s",pathinfo);
}

int mapcache_config_services_enabled(mapcache_context *ctx, mapcache_cfg *config) {
  int count=0,i;
  for(i=0; i<MAPCACHE_SERVICES_COUNT; i++) {
    if(config->services[i])
      count++;
  }
  return count;
}

/** @} */





/* vim: ts=2 sts=2 et sw=2
*/
