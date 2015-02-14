/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: memcache cache backend.
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

#include "mapcache-config.h"
#ifdef USE_REDIS

#include "mapcache.h"
#include <apr_strings.h>
#include <limits.h>
#include <errno.h>

#define REDIS_GET_CACHE(t) ((mapcache_cache_redis*)t->tileset->cache)
#define REDIS_GET_TILE_KEY(c, t) (mapcache_util_get_tile_key(c, t, NULL, " \r\n\t\f\e\a\b", "#"))
#define IS_REDIS_ERROR_STATUS(r) (r->type != REDIS_REPLY_STATUS || strncmp(r->str, "OK", 2) != 0)

static struct redisContext* _redis_get_connection(mapcache_context *ctx,
                                                  mapcache_tile* tile)
{
  mapcache_cache_redis *cache = REDIS_GET_CACHE(tile);
  redisContext* conn = redisConnect(cache->host, cache->port);
  if (!conn || conn->err) {
    ctx->set_error(ctx,500, "redis: failed to connect to server %s:%d", cache->host, cache->port);
    return NULL;
  }
  return conn;
}

static int _mapcache_cache_redis_has_tile(mapcache_context *ctx,
                                          mapcache_tile *tile)
{
  char *key = REDIS_GET_TILE_KEY(ctx, tile);
  if(GC_HAS_ERROR(ctx)) {
    return MAPCACHE_FALSE;
  }
  int returnValue = MAPCACHE_TRUE;
  redisContext *conn = _redis_get_connection(ctx, tile);
  if(!conn) {
    return MAPCACHE_FALSE;
  }
  redisReply *reply = redisCommand(conn, "EXISTS %s", key);
  if(reply->type != REDIS_REPLY_INTEGER) {
    returnValue = MAPCACHE_FALSE;
  }
  else if(reply->integer == 0) {
    returnValue = MAPCACHE_FALSE;
  }
  freeReplyObject(reply);
  redisFree(conn);
  return returnValue;
}

static void _mapcache_cache_redis_delete(mapcache_context *ctx,
                                         mapcache_tile *tile) 
{
  char* key = REDIS_GET_TILE_KEY(ctx, tile);
  GC_CHECK_ERROR(ctx);
  redisContext *conn = _redis_get_connection(ctx, tile);
  if(!conn) {
    return;
  }
  redisReply *reply = redisCommand(conn, "DEL %s", key);
  if(reply->type == REDIS_REPLY_ERROR) {
    ctx->set_error(ctx, 500, "redis: failed to delete key %s: %s", key, reply->str);
  }
  freeReplyObject(reply);
  redisFree(conn);
}

static int _mapcache_cache_redis_get(mapcache_context *ctx,
                                     mapcache_tile *tile)
{
  char* key = REDIS_GET_TILE_KEY(ctx, tile);
  if(GC_HAS_ERROR(ctx)) {
    return MAPCACHE_FAILURE;
  }
  tile->encoded_data = mapcache_buffer_create(0, ctx->pool);
  redisContext *conn = _redis_get_connection(ctx, tile);
  if(!conn) {
    return MAPCACHE_FAILURE;
  }
  redisReply *reply = redisCommand(conn, "GET %s", key);
  if(reply->type != REDIS_REPLY_STRING) {
    freeReplyObject(reply);
    return MAPCACHE_CACHE_MISS;
  }
  tile->encoded_data->buf = apr_pcalloc(ctx->pool, reply->len);
  memcpy(tile->encoded_data->buf, reply->str, reply->len);
  tile->encoded_data->size = reply->len;
  if(tile->encoded_data->size == 0) {
    freeReplyObject(reply);
    ctx->set_error(ctx, 500, "redis: cache returned 0-length data for tile %d %d %d\n",tile->x,tile->y,tile->z);
    return MAPCACHE_FAILURE;
  }
  memcpy(
    &tile->mtime,
    &(((char*)tile->encoded_data->buf)[tile->encoded_data->size-sizeof(apr_time_t)]),
    sizeof(apr_time_t));
  ((char*)tile->encoded_data->buf)[tile->encoded_data->size+sizeof(apr_time_t)]='\0';
  tile->encoded_data->avail = tile->encoded_data->size;
  tile->encoded_data->size -= sizeof(apr_time_t);
  freeReplyObject(reply);
  redisFree(conn);
  return MAPCACHE_SUCCESS;
}

/**
 * \brief push tile data to redis
 *
 * writes the content of mapcache_tile::data to the configured redis instance.
 * \private \memberof mapcache_cache_redis
 * \sa mapcache_cache::tile_set()
 */
static void _mapcache_cache_redis_set(mapcache_context *ctx,
                                      mapcache_tile *tile)
{
  /* set expiration to one day if not configured */
  int expires = 86400;
  if(tile->tileset->auto_expire)
    expires = tile->tileset->auto_expire;
  mapcache_cache_redis *cache = REDIS_GET_CACHE(tile);
  char *key = mapcache_util_get_tile_key(ctx, tile,NULL," \r\n\t\f\e\a\b","#");
  GC_CHECK_ERROR(ctx);

  if(!tile->encoded_data) {
    tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
    GC_CHECK_ERROR(ctx);
  }

  /* concatenate the current time to the end of the memcache data so we can extract it out
   * when we re-get the tile */
  char *data = calloc(1,tile->encoded_data->size+sizeof(apr_time_t));
  apr_time_t now = apr_time_now();
  apr_pool_cleanup_register(ctx->pool, data, (void*)free, apr_pool_cleanup_null);
  memcpy(data,tile->encoded_data->buf,tile->encoded_data->size);
  memcpy(&(data[tile->encoded_data->size]),&now,sizeof(apr_time_t));

  redisContext *conn = _redis_get_connection(ctx, tile);
  if(!conn) {
    return;
  }
  redisReply *reply = redisCommand(conn,
                                   "SETEX %s %d %b",
                                   key,
                                   expires,
                                   data,
                                   tile->encoded_data->size + sizeof(apr_time_t));
  
  if(IS_REDIS_ERROR_STATUS(reply)) {
    ctx->set_error(ctx, 500, "failed to store tile %d %d %d to redis cache %s",
                   tile->x, tile->y, tile->z, cache->cache.name);
  }

  freeReplyObject(reply);
  
  redisFree(conn);
}

/**
 * \private \memberof mapcache_cache_redis
 */
static void _mapcache_cache_redis_configuration_parse_xml(mapcache_context *ctx,
                                                          ezxml_t node,
                                                          mapcache_cache *cache,
                                                          mapcache_cfg *config)
{
  mapcache_cache_redis *dcache = (mapcache_cache_redis*)cache;
  dcache->host = NULL;
  dcache->port = 0;

  ezxml_t xhost = ezxml_child(node, "host");
  ezxml_t xport = ezxml_child(node, "port");

  if (!xhost || !xhost->txt || !*xhost->txt) {
    ctx->set_error(ctx, 400, "cache %s: redis cache with no <host>", cache->name);
    return;
  } else {
    dcache->host = apr_pstrdup(ctx->pool, xhost->txt);
  }

  if (!xport || !xport->txt || !*xport->txt) {
    ctx->set_error(ctx, 400, "cache %s: redis cache with no <port>", cache->name);
    return;
  } else {
    unsigned long int iport = strtoul(xport->txt, NULL, 10);
    if(iport == ULONG_MAX && errno == ERANGE) {
      ctx->set_error(ctx, 400, "port value %s too large to be parsed for redis cache %s", xport->txt, cache->name);
      return;
    } else if(iport == 0) {
      ctx->set_error(ctx, 400, "failed to parse port value %s for redis cache %s", xport->txt, cache->name);
      return;
    }
    dcache->port = iport;
  }
}

/**
 * \private \memberof mapcache_cache_redis
 */
static void _mapcache_cache_redis_configuration_post_config(mapcache_context *ctx,
                                                            mapcache_cache *cache,
                                                            mapcache_cfg *cfg)
{
}

/**
 * \brief creates and initializes a mapcache_redis_cache
 */
mapcache_cache* mapcache_cache_redis_create(mapcache_context *ctx)
{
  mapcache_cache_redis *cache = apr_pcalloc(ctx->pool,
                                            sizeof(mapcache_cache_redis));
  if(!cache) {
    ctx->set_error(ctx, 500, "failed to allocate redis cache");
    return NULL;
  }
  cache->cache.metadata = apr_table_make(ctx->pool,3);
  cache->cache.type = MAPCACHE_CACHE_REDIS;
  cache->cache.tile_get = _mapcache_cache_redis_get;
  cache->cache.tile_exists = _mapcache_cache_redis_has_tile;
  cache->cache.tile_set = _mapcache_cache_redis_set;
  cache->cache.tile_delete = _mapcache_cache_redis_delete;
  cache->cache.configuration_post_config = _mapcache_cache_redis_configuration_post_config;
  cache->cache.configuration_parse_xml = _mapcache_cache_redis_configuration_parse_xml;
  return (mapcache_cache*)cache;
}

#endif

/* vim: ts=2 sts=2 et sw=2
*/
