#include "apr.h"
#include "apr_strings.h"
#include "apr_thread_proc.h"    /* for RLIMIT stuff */
#include "apr_optional.h"
#include "apr_buckets.h"
#include "apr_lib.h"
#include "apr_poll.h"

#define APR_WANT_STRFUNC
#define APR_WANT_MEMFUNC
#include "apr_want.h"

#include "util_filter.h"
#include "ap_config.h"
#include "httpd.h"
#include "http_config.h"
#include "http_request.h"
#include "http_core.h"
#include "http_protocol.h"
#include "http_main.h"
#include "http_log.h"
#include "util_script.h"
#include "ap_mpm.h"
#include "mod_core.h"
#include "mod_cgi.h"
#include "util_md5.h"

module AP_MODULE_DECLARE_DATA tile_module;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "gen_tile.h"
#include "protocol.h"
#include "render_config.h"
#include "store.h"
#include "dir_utils.h"
#include "mod_tile.h"



#if !defined(OS2) && !defined(WIN32) && !defined(BEOS) && !defined(NETWARE)
#include "unixd.h"
#define MOD_TILE_SET_MUTEX_PERMS /* XXX Apache should define something */
#endif



apr_shm_t *stats_shm;
apr_shm_t *delaypool_shm;
char *shmfilename;
char *shmfilename_delaypool;
apr_global_mutex_t *stats_mutex;
apr_global_mutex_t *delay_mutex;
char *mutexfilename;

static int error_message(request_rec *r, const char *format, ...)
                 __attribute__ ((format (printf, 2, 3)));

static int error_message(request_rec *r, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int len;
    char *msg;

    len = vasprintf(&msg, format, ap);

    if (msg) {
        //ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "%s", msg);
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "%s", msg);
        r->content_type = "text/plain";
        if (!r->header_only)
            ap_rputs(msg, r);
        free(msg);
    }

    return OK;
}

int socket_init(request_rec *r)
{
    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);

    int fd;
    struct sockaddr_un addr;

    fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r, "failed to create unix socket");
        return FD_INVALID;
    }

    bzero(&addr, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, scfg->renderd_socket_name, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r, "socket connect failed for: %s", scfg->renderd_socket_name);
        close(fd);
        return FD_INVALID;
    }
    return fd;
}

int request_tile(request_rec *r, struct protocol *cmd, int renderImmediately)
{
    int fd;
    int ret = 0;
    int retry = 1;
    struct protocol resp;

    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);

    fd = socket_init(r);

    if (fd == FD_INVALID) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Failed to connect to renderer");
        return 0;
    }

    // cmd has already been partial filled, fill in the rest
    cmd->ver = PROTO_VER;
    switch (renderImmediately) {
    case 0: { cmd->cmd = cmdDirty; break;}
    case 1: { cmd->cmd = cmdRender; break;}
    case 2: { cmd->cmd = cmdRenderPrio; break;}
    }

    // maybe TODO: add level
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Requesting xml(%s) z(%d) x(%d) y(%d)", cmd->xmlname, cmd->z, cmd->x, cmd->y);
    do {
        ret = send(fd, cmd, sizeof(struct protocol), 0);

        if (ret == sizeof(struct protocol))
            break;

        close(fd);
        if (errno != EPIPE)
            return 0;

        fd = socket_init(r);
        if (fd == FD_INVALID)
            return 0;
    } while (retry--);

    if (renderImmediately) {
        struct timeval tv = {(renderImmediately > 1?scfg->request_timeout_priority:scfg->request_timeout), 0 };
        fd_set rx;
        int s;

        while (1) {
            FD_ZERO(&rx);
            FD_SET(fd, &rx);
            s = select(fd+1, &rx, NULL, NULL, &tv);
            if (s == 1) {
                bzero(&resp, sizeof(struct protocol));
                ret = recv(fd, &resp, sizeof(struct protocol), 0);
                if (ret != sizeof(struct protocol)) {
                    //perror("recv error");
                    break;
                }

                if (cmd->x == resp.x && cmd->y == resp.y && cmd->z == resp.z && !strcmp(cmd->xmlname, resp.xmlname) && cmd->level == resp.level) {
                    close(fd);
                    if (resp.cmd == cmdDone)
                        return 1;
                    else
                        return 0;
                } else {
                    // maybe TODO: add level
                    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                       "Response does not match request: xml(%s,%s) z(%d,%d) x(%d,%d) y(%d,%d)", cmd->xmlname,
                       resp.xmlname, cmd->z, resp.z, cmd->x, resp.x, cmd->y, resp.y);
                }
            } else {
                break;
            }
        }
    }

    close(fd);
    return 0;
}

static apr_time_t getPlanetTime(request_rec *r)
{
    static apr_time_t last_check;
    static apr_time_t planet_timestamp;
    static pthread_mutex_t planet_lock = PTHREAD_MUTEX_INITIALIZER;
    apr_time_t now = r->request_time;
    struct apr_finfo_t s;

    pthread_mutex_lock(&planet_lock);
    // Only check for updates periodically
    if (now < last_check + apr_time_from_sec(300)) {
        pthread_mutex_unlock(&planet_lock);
        return planet_timestamp;
    }

    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);
    char filename[PATH_MAX];
    snprintf(filename, PATH_MAX-1, "%s/%s", scfg->tile_dir, PLANET_TIMESTAMP);

    last_check = now;
    if (apr_stat(&s, filename, APR_FINFO_MIN, r->pool) != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Planet timestamp file (%s) is missing", filename);
        // Make something up
        planet_timestamp = now - apr_time_from_sec(3 * 24 * 60 * 60);
    } else {
        if (s.mtime != planet_timestamp) {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Planet file updated");
            planet_timestamp = s.mtime;
        }
    }
    pthread_mutex_unlock(&planet_lock);
    return planet_timestamp;
}

static enum tileState tile_state_once(request_rec *r)
{
    apr_status_t rv;
    apr_finfo_t *finfo = &r->finfo;

    if (!(finfo->valid & APR_FINFO_MTIME)) {
        rv = apr_stat(finfo, r->filename, APR_FINFO_MIN, r->pool);
        if (rv != APR_SUCCESS)
            return tileMissing;
    }

    if (finfo->mtime < getPlanetTime(r))
        return tileOld;

    return tileCurrent;
}

static enum tileState tile_state(request_rec *r, struct protocol *cmd)
{
    enum tileState state = tile_state_once(r);
#ifdef METATILEFALLBACK
    if (state == tileMissing) {
        ap_conf_vector_t *sconf = r->server->module_config;
        tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);

        // Try fallback to plain PNG
        char path[PATH_MAX];
        xyz_to_path(path, sizeof(path), scfg->tile_dir, cmd);
        r->filename = apr_pstrdup(r->pool, path);
        state = tile_state_once(r);
        // maybe TODO: add level
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "png fallback %d/%d/%d",cms->x,cmd->y,cmd->z);

        if (state == tileMissing) {
            // PNG not available either, if it gets rendered, it'll now be a .meta
            xyz_to_meta(path, sizeof(path), scfg->tile_dir, cmd);
            r->filename = apr_pstrdup(r->pool, path);
        }
    }
#endif
    return state;
}

static void add_expiry(request_rec *r, struct protocol * cmd)
{
    apr_time_t holdoff;
    apr_table_t *t = r->headers_out;
    enum tileState state = tile_state(r, cmd);
    apr_finfo_t *finfo = &r->finfo;
    char *timestr;
    long int planetTimestamp, maxAge, minCache, lastModified;

    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "expires(%s), uri(%s), filename(%s), path_info(%s)\n",
                  r->handler, r->uri, r->filename, r->path_info);

    /* If the hostname matches the "extended caching hostname" then set the cache age accordingly */
    if ((scfg->cache_extended_hostname[0] != 0) && (strstr(r->hostname,
            scfg->cache_extended_hostname) != NULL)) {
        maxAge = scfg->cache_extended_duration;
    } else {

        /* Test if the tile we are serving is out of date, then set a low maxAge*/
        if (state == tileOld) {
            holdoff = (scfg->cache_duration_dirty / 2) * (rand() / (RAND_MAX
                    + 1.0));
            maxAge = scfg->cache_duration_dirty + holdoff;
        } else {
            // cache heuristic based on zoom level
            if (cmd->z > MAX_ZOOM) {
                minCache = 0;
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                        "z (%i) is larger than MAXZOOM %i\n", cmd->z, MAX_ZOOM);
            } else {
                minCache = scfg->mincachetime[cmd->z];
            }
            // Time to the next known complete rerender
            planetTimestamp = apr_time_sec(getPlanetTime(r)
                    + apr_time_from_sec(PLANET_INTERVAL) - r->request_time);
            // Time since the last render of this tile
            lastModified = (int) (((double) apr_time_sec(r->request_time
                    - finfo->mtime))
                    * scfg->cache_duration_last_modified_factor);
            // Add a random jitter of 3 hours to space out cache expiry
            holdoff = (3 * 60 * 60) * (rand() / (RAND_MAX + 1.0));

            maxAge = MAX(minCache, planetTimestamp);
            maxAge = MAX(maxAge, lastModified);
            maxAge += holdoff;

            ap_log_rerror(
                    APLOG_MARK,
                    APLOG_DEBUG,
                    0,
                    r,
                    "caching heuristics: next planet render %ld; zoom level based %ld; last modified %ld\n",
                    planetTimestamp, minCache, lastModified);
        }

        maxAge = MIN(maxAge, scfg->cache_duration_max);
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Setting tiles maxAge to %ld\n", maxAge);

    apr_table_mergen(t, "Cache-Control",
                     apr_psprintf(r->pool, "max-age=%" APR_TIME_T_FMT,
                     maxAge));
    timestr = apr_palloc(r->pool, APR_RFC822_DATE_LEN);
    apr_rfc822_date(timestr, (apr_time_from_sec(maxAge) + r->request_time));
    apr_table_setn(t, "Expires", timestr);
}

double get_load_avg(request_rec *r)
{
    double loadavg[1];
    int n = getloadavg(loadavg, 1);

    if (n < 1)
        return 1000;
    else
        return loadavg[0];
}

static int get_global_lock(request_rec *r, apr_global_mutex_t * mutex) {
    apr_status_t rs;
    int camped;

    for (camped = 0; camped < MAXCAMP; camped++) {
        rs = apr_global_mutex_trylock(mutex);
        if (APR_STATUS_IS_EBUSY(rs)) {
            apr_sleep(CAMPOUT);
        } else if (rs == APR_SUCCESS) {
            return 1;
        } else if (APR_STATUS_IS_ENOTIMPL(rs)) {
            /* If it's not implemented, just hang in the mutex. */
            rs = apr_global_mutex_lock(mutex);
            if (rs == APR_SUCCESS) {
                return 1;
            } else {
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Could not get hardlock");
                return 0;
            }
        } else {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Unknown return status from trylock");
            return 0;
        }
    }
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Timedout trylock");
    return 0;
}

static int incRespCounter(int resp, request_rec *r, struct protocol * cmd) {
    stats_data *stats;

    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);

    if (!scfg->enableGlobalStats) {
        /* If tile stats reporting is not enable
         * pretend we correctly updated the counter to
         * not fill the logs with warnings about failed
         * stats
         */
        return 1;
    }

    if (get_global_lock(r, stats_mutex) != 0) {
        stats = (stats_data *)apr_shm_baseaddr_get(stats_shm);
        switch (resp) {
        case OK: {
            stats->noResp200++;
			if (cmd != NULL) {
				stats->noRespZoom[cmd->z]++;
			}
            break;
        }
        case HTTP_NOT_MODIFIED: {
            stats->noResp304++;
			if (cmd != NULL) {
				stats->noRespZoom[cmd->z]++;
			}
            break;
        }
        case HTTP_NOT_FOUND: {
            stats->noResp404++;
            break;
        }
		case HTTP_SERVICE_UNAVAILABLE: {
            stats->noResp503++;
            break;
        }
        case HTTP_INTERNAL_SERVER_ERROR: {
            stats->noResp5XX++;
            break;
        }
        default: {
            stats->noRespOther++;
        }

        }
        apr_global_mutex_unlock(stats_mutex);
        /* Swallowing the result because what are we going to do with it at
         * this stage?
         */
        return 1;
    } else {
        return 0;
    }
}

static int incFreshCounter(int status, request_rec *r) {
    stats_data *stats;

    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);

    if (!scfg->enableGlobalStats) {
        /* If tile stats reporting is not enable
         * pretend we correctly updated the counter to
         * not fill the logs with warnings about failed
         * stats
         */
        return 1;
    }

    if (get_global_lock(r, stats_mutex) != 0) {
        stats = (stats_data *)apr_shm_baseaddr_get(stats_shm);
        switch (status) {
        case FRESH: {
            stats->noFreshCache++;
            break;
        }
        case FRESH_RENDER: {
            stats->noFreshRender++;
            break;
        }
        case OLD: {
            stats->noOldCache++;
            break;
        }
        case OLD_RENDER: {
            stats->noOldRender++;
            break;
        }
        }
        apr_global_mutex_unlock(stats_mutex);
        /* Swallowing the result because what are we going to do with it at
         * this stage?
         */
        return 1;
    } else {
        return 0;
    }
}

static int delay_allowed(request_rec *r, enum tileState state) {
	delaypool * delayp;
	int delay = 0;
	int i,j;

    ap_conf_vector_t *sconf = r->server->module_config;
	tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);
	delayp = (delaypool *)apr_shm_baseaddr_get(delaypool_shm);

	/* TODO: fix IPv6 compatibility */
	in_addr_t ip = inet_addr(r->connection->remote_ip);
	
	int hashkey = ip % DELAY_HASHTABLE_WHITELIST_SIZE;
	if (delayp->whitelist[hashkey] == ip) {
		return 1;
	}

	/* If a delaypool fillup is ongoing, just skip accounting to not block on a lock */
	if (delayp->locked) {
		ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "skipping delay pool accounting, during fillup procedure\n");
		return 1;
	}

	
	hashkey = ip % DELAY_HASHTABLE_SIZE;
	
	if (get_global_lock(r,delay_mutex) == 0) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Could not acquire lock, skipping delay pool accounting\n");
		return 1;
	};
	if (delayp->users[hashkey].ip_addr == ip) {
		/* Repeat the process to determin if we have tockens in the bucket, as the fillup only runs once a client hits an empty bucket,
		   so in the mean time, the bucket might have been filled */
		for (j = 0; j < 3; j++) {
			//ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Checking delays: Current poolsize: %i tiles and %i renders\n", delayp->users[hashkey].available_tiles, delayp->users[hashkey].available_render_req);
			delay = 0;
			if (delayp->users[hashkey].available_tiles > 0) {
				delayp->users[hashkey].available_tiles--;
			} else {
				delay = 1;
			}
			if (state == tileMissing) {
				if (delayp->users[hashkey].available_render_req > 0) {
					delayp->users[hashkey].available_render_req--;
				} else {
					delay = 2;
				}
			}

			if (delay > 0) {
				/* If we are on the second round, we really  hit an empty delaypool, timeout for a while to slow down clients */
				if (j > 0) {
					apr_global_mutex_unlock(delay_mutex);
					ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Delaypool: Client %s has hit its limits, throtteling (%i)\n", r->connection->remote_ip, delay);
					sleep(CLIENT_PENALTY);
					if (get_global_lock(r,delay_mutex) == 0) {
						ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Could not acquire lock, but had to delay\n");
						return 0;
					};
				}
				/* We hit an empty bucket, so run the bucket fillup procedure to check if new tokens should have arrived in the mean time. */
				apr_time_t now = apr_time_now();
				int tiles_topup = (now - delayp->last_tile_fillup) / scfg->delaypoolTileRate;
				int render_topup = (now - delayp->last_render_fillup) / scfg->delaypoolRenderRate;
				//ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Filling up pools with %i tiles and %i renders\n", tiles_topup, render_topup);
				if ((tiles_topup > 0) || (render_topup > 0)) {
					delayp->locked = 1;
					for (i = 0; i < DELAY_HASHTABLE_SIZE; i++) {
						delayp->users[i].available_tiles += tiles_topup;
						delayp->users[i].available_render_req += render_topup;
						if (delayp->users[i].available_tiles > scfg->delaypoolTileSize) {
							delayp->users[i].available_tiles = scfg->delaypoolTileSize;
						}
						if (delayp->users[i].available_render_req > scfg->delaypoolRenderSize) {
							delayp->users[i].available_render_req = scfg->delaypoolRenderSize;
						}
					}
					delayp->locked = 0;
				}
				delayp->last_tile_fillup += scfg->delaypoolTileRate*tiles_topup;
				delayp->last_render_fillup += scfg->delaypoolRenderRate*render_topup;				
				
			} else {
				break;
			}
		}
	} else {
		ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Creating a new delaypool for ip %s, overwriting %s\n", r->connection->remote_ip, inet_ntoa(inet_makeaddr(delayp->users[hashkey].ip_addr,delayp->users[hashkey].ip_addr)));
		delayp->users[hashkey].ip_addr = ip;
		delayp->users[hashkey].available_tiles = scfg->delaypoolTileSize;
	    delayp->users[hashkey].available_render_req = scfg->delaypoolRenderSize;
		delay = 0;
	}
	apr_global_mutex_unlock(delay_mutex);

	if (delay > 0) {
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Delaypool: Client %s has hit its limits, rejecting (%i)\n", r->connection->remote_ip, delay);
		return 0;
	} else {
		return 1;
	}
}

static int tile_handler_dirty(request_rec *r)
{
    if(strcmp(r->handler, "tile_dirty"))
        return DECLINED;

    struct protocol * cmd = (struct protocol *)ap_get_module_config(r->request_config, &tile_module);
    if (cmd == NULL)
        return DECLINED;

    request_tile(r, cmd, 0);
    return error_message(r, "Tile submitted for rendering\n");
}

static int tile_storage_hook(request_rec *r)
{
//    char abs_path[PATH_MAX];
    int avg;
    int renderPrio = 0;
    enum tileState state;

    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "tile_storage_hook: handler(%s), uri(%s), filename(%s), path_info(%s)",
                  r->handler, r->uri, r->filename, r->path_info);

    if (!r->handler)
        return DECLINED;

    // Any status request is OK. tile_dirty also doesn't need to be handled, as tile_handler_dirty will take care of it
    if (!strcmp(r->handler, "tile_status") || !strcmp(r->handler, "tile_dirty") || !strcmp(r->handler, "tile_mod_stats"))
        return OK;

    if (strcmp(r->handler, "tile_serve"))
        return DECLINED;

    struct protocol * cmd = (struct protocol *)ap_get_module_config(r->request_config, &tile_module);
    if (cmd == NULL)
        return DECLINED;

/*
should already be done
    // Generate the tile filename
#ifdef METATILE
    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);
    xyz_to_meta(abs_path, sizeof(abs_path), scfg->tile_dir, cmd->xmlname, cmd->x, cmd->y, cmd->z);
#else
    xyz_to_path(abs_path, sizeof(abs_path), scfg->tile_dir, cmd->xmlname, cmd->x, cmd->y, cmd->z);
#endif
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "abs_path(%s)", abs_path);
    r->filename = apr_pstrdup(r->pool, abs_path);
*/
    avg = get_load_avg(r);
    state = tile_state(r, cmd);

    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);

	if (scfg->enableTileThrotteling && !delay_allowed(r, state)) {
		if (!incRespCounter(HTTP_SERVICE_UNAVAILABLE, r, cmd)) {
                   ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                        "Failed to increase response stats counter");
        }
        return HTTP_SERVICE_UNAVAILABLE;		
	}

    switch (state) {
        case tileCurrent:
            if (!incFreshCounter(FRESH, r)) {
                ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                    "Failed to increase fresh stats counter");
            }
            return OK;
            break;
        case tileOld:
            if (avg > scfg->max_load_old) {
               // Too much load to render it now, mark dirty but return old tile
               request_tile(r, cmd, 0);
               ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Load larger max_load_old (%d). Mark dirty and deliver from cache.", scfg->max_load_old);
               if (!incFreshCounter(OLD, r)) {
                   ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                        "Failed to increase fresh stats counter");
               }
               return OK;
            }
            renderPrio = 1;
            break;
        case tileMissing:
            if (avg > scfg->max_load_missing) {
               request_tile(r, cmd, 0);
               ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Load larger max_load_missing (%d). Return HTTP_NOT_FOUND.", scfg->max_load_missing);
               if (!incRespCounter(HTTP_NOT_FOUND, r, cmd)) {
                   ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                        "Failed to increase response stats counter");
               }
               return HTTP_NOT_FOUND;
            }
            renderPrio = 2;
            break;
    }

    if (request_tile(r, cmd, renderPrio)) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "Update file info abs_path(%s)", r->filename);
        // Need to update fileinfo for new rendered tile
        apr_stat(&r->finfo, r->filename, APR_FINFO_MIN, r->pool);
        if (!incFreshCounter(FRESH_RENDER, r)) {
            ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                    "Failed to increase fresh stats counter");
        }
        return OK;
    }

    if (state == tileOld) {
        if (!incFreshCounter(OLD_RENDER, r)) {
            ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                    "Failed to increase fresh stats counter");
        }
        return OK;
    }
    if (!incRespCounter(HTTP_NOT_FOUND, r, cmd)) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                "Failed to increase response stats counter");
    }

    return HTTP_NOT_FOUND;
}

static int tile_handler_status(request_rec *r)
{
    enum tileState state;
    char time_str[APR_CTIME_LEN];

    if(strcmp(r->handler, "tile_status"))
        return DECLINED;

    struct protocol * cmd = (struct protocol *)ap_get_module_config(r->request_config, &tile_module);
    if (cmd == NULL){
        sleep(CLIENT_PENALTY);
        return HTTP_NOT_FOUND;
    }

    state = tile_state(r, cmd);
    if (state == tileMissing)
        return error_message(r, "Unable to find a tile at %s\n", r->filename);
    apr_ctime(time_str, r->finfo.mtime);

    return error_message(r, "Tile is %s. Last rendered at %s\n", (state == tileOld) ? "due to be rendered" : "clean", time_str);
}

static int tile_handler_mod_stats(request_rec *r)
{
    stats_data * stats;
    stats_data local_stats;
	int i;

    if (strcmp(r->handler, "tile_mod_stats"))
        return DECLINED;

    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);

    if (!scfg->enableGlobalStats) {
        return error_message(r, "Stats are not enabled for this server");
    }

    if (get_global_lock(r,stats_mutex) != 0) {
        //Copy over the global counter variable into
        //local variables, that we can immediately
        //release the lock again
        stats = (stats_data *) apr_shm_baseaddr_get(stats_shm);
        memcpy(&local_stats, stats, sizeof(stats_data));
        apr_global_mutex_unlock(stats_mutex);
    } else {
        return error_message(r, "Failed to acquire lock, can't display stats");
    }

    ap_rprintf(r, "NoResp200: %li\n", local_stats.noResp200);
    ap_rprintf(r, "NoResp304: %li\n", local_stats.noResp304);
    ap_rprintf(r, "NoResp404: %li\n", local_stats.noResp404);
	ap_rprintf(r, "NoResp503: %li\n", local_stats.noResp503);
    ap_rprintf(r, "NoResp5XX: %li\n", local_stats.noResp5XX);
    ap_rprintf(r, "NoRespOther: %li\n", local_stats.noRespOther);
    ap_rprintf(r, "NoFreshCache: %li\n", local_stats.noFreshCache);
    ap_rprintf(r, "NoOldCache: %li\n", local_stats.noOldCache);
    ap_rprintf(r, "NoFreshRender: %li\n", local_stats.noFreshRender);
    ap_rprintf(r, "NoOldRender: %li\n", local_stats.noOldRender);
	for (i = 0; i <= MAX_ZOOM; i++) {
		ap_rprintf(r, "NoRespZoom%02i: %li\n", i, local_stats.noRespZoom[i]);
	}



    return OK;
}

static int tile_handler_serve(request_rec *r)
{
    const int tile_max = MAX_SIZE;
    unsigned char *buf;
    int len;
    apr_status_t errstatus;

    if(strcmp(r->handler, "tile_serve"))
        return DECLINED;

    struct protocol * cmd = (struct protocol *)ap_get_module_config(r->request_config, &tile_module);
    if (cmd == NULL){
        sleep(CLIENT_PENALTY);
        if (!incRespCounter(HTTP_NOT_FOUND, r, cmd)) {
            ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                    "Failed to increase response stats counter");
        }
        return HTTP_NOT_FOUND;
    }

    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "tile_handler_serve: xml(%s) z(%d) x(%d) y(%d)", cmd->xmlname, cmd->z, cmd->x, cmd->y);

    // FIXME: It is a waste to do the malloc + read if we are fulfilling a HEAD or returning a 304.
    buf = malloc(tile_max);
    if (!buf) {
        if (!incRespCounter(HTTP_INTERNAL_SERVER_ERROR, r, cmd)) {
            ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                    "Failed to increase response stats counter");
        }
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    len = tile_read(cmd, buf, tile_max);
    if (len > 0) {
#if 0
        // Set default Last-Modified and Etag headers
        ap_update_mtime(r, r->finfo.mtime);
        ap_set_last_modified(r);
        ap_set_etag(r);
#else
        // Use MD5 hash as only cache attribute.
        // If a tile is re-rendered and produces the same output
        // then we can continue to use the previous cached copy
        char *md5 = ap_md5_binary(r->pool, buf, len);
        apr_table_setn(r->headers_out, "ETag",
                        apr_psprintf(r->pool, "\"%s\"", md5));
#endif
        ap_set_content_type(r, "image/png");
        ap_set_content_length(r, len);
        add_expiry(r, cmd);
        if ((errstatus = ap_meets_conditions(r)) != OK) {
            free(buf);
            if (!incRespCounter(errstatus, r, cmd)) {
                ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                        "Failed to increase response stats counter");
            }
            return errstatus;
        } else {
            ap_rwrite(buf, len, r);
            free(buf);
            if (!incRespCounter(errstatus, r, cmd)) {
                ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                        "Failed to increase response stats counter");
            }
            return OK;
        }
    }
    free(buf);
    //ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "len = %d", len);
    if (!incRespCounter(HTTP_NOT_FOUND, r, cmd)) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                "Failed to increase response stats counter");
    }
    return DECLINED;
}

static int tile_translate(request_rec *r)
{
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "tile_translate: uri(%s)", r->uri);

    int i,n,limit,oob;
    char option[11];

    ap_conf_vector_t *sconf = r->server->module_config;
    tile_server_conf *scfg = ap_get_module_config(sconf, &tile_module);

    tile_config_rec *tile_configs = (tile_config_rec *) scfg->configs->elts;

    /*
     * The page /mod_tile returns global stats about the number of tiles
     * handled and in what state those tiles were.
     * This should probably not be hard coded
     */
    if (!strncmp("/mod_tile", r->uri, strlen("/mod_tile"))) {
        r->handler = "tile_mod_stats";
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                "tile_translate: retrieving global mod_tile stats");
        return OK;
    }

    for (i = 0; i < scfg->configs->nelts; ++i) {
        tile_config_rec *tile_config = &tile_configs[i];

        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "tile_translate: baseuri(%s) name(%s)", tile_config->baseuri, tile_config->xmlname);

        if (!strncmp(tile_config->baseuri, r->uri, strlen(tile_config->baseuri))) {

            struct protocol * cmd = (struct protocol *) apr_pcalloc(r->pool, sizeof(struct protocol));
            bzero(cmd, sizeof(struct protocol));

            if ( !tile_config->levels ) {
                n = sscanf(r->uri+strlen(tile_config->baseuri), "%d/%d/%d.png/%10s", &(cmd->z), &(cmd->x), &(cmd->y), option);
                if (n < 3) return DECLINED;
                cmd->level = NO_LEVELS;
            }
            else {
                n = sscanf(r->uri+strlen(tile_config->baseuri), "%d/%d/%d.%d.png/%10s", &(cmd->z), &(cmd->x), &(cmd->y), &(cmd->level), option);
                if (n < 4) return DECLINED;
            }


            oob = (cmd->z < 0 || cmd->z > MAX_ZOOM);
            if (!oob) {
                 // valid x/y for tiles are 0 ... 2^zoom-1
                 limit = (1 << cmd->z) - 1;
                 oob =  (cmd->x < 0 || cmd->x > limit || cmd->y < 0 || cmd->y > limit);
            }

            if (oob) {
                sleep(CLIENT_PENALTY);
                //Don't increase stats counter here,
                //As we are interested in valid tiles only
                return HTTP_NOT_FOUND;
            }

            strcpy(cmd->xmlname, tile_config->xmlname);

            // Store a copy for later
            ap_set_module_config(r->request_config, &tile_module, cmd);

            // Generate the tile filename?
            char abs_path[PATH_MAX];
#ifdef METATILE
            xyz_to_meta(abs_path, sizeof(abs_path), scfg->tile_dir, cmd);
#else
            xyz_to_path(abs_path, sizeof(abs_path), scfg->tile_dir, cmd);
#endif
            r->filename = apr_pstrdup(r->pool, abs_path);

            if (n == 4) {
                if (!strcmp(option, "status")) r->handler = "tile_status";
                else if (!strcmp(option, "dirty")) r->handler = "tile_dirty";
                else return DECLINED;
            } else {
                r->handler = "tile_serve";
            }

            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "tile_translate: op(%s) xml(%s) z(%d) x(%d) y(%d)", r->handler , cmd->xmlname, cmd->z, cmd->x, cmd->y);

            return OK;
        }
    }
    return DECLINED;
}

/*
 * This routine is called in the parent, so we'll set up the shared
 * memory segment and mutex here.
 */

static int mod_tile_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                             apr_pool_t *ptemp, server_rec *s)
{
    void *data; /* These two help ensure that we only init once. */
    const char *userdata_key = "mod_tile_init_module";
    apr_status_t rs;
    stats_data *stats;
	delaypool *delayp;
	int i;


    /*
     * The following checks if this routine has been called before.
     * This is necessary because the parent process gets initialized
     * a couple of times as the server starts up, and we don't want
     * to create any more mutexes and shared memory segments than
     * we're actually going to use.
     */
    apr_pool_userdata_get(&data, userdata_key, s->process->pool);
    if (!data) {
        apr_pool_userdata_set((const void *) 1, userdata_key,
                              apr_pool_cleanup_null, s->process->pool);
        return OK;
    } /* Kilroy was here */

    /* Create the shared memory segment */

    /*
     * Create a unique filename using our pid. This information is
     * stashed in the global variable so the children inherit it.
     * TODO get the location from the environment $TMPDIR or somesuch.
     */
    shmfilename = apr_psprintf(pconf, "/tmp/httpd_shm.%ld", (long int)getpid());
	shmfilename_delaypool = apr_psprintf(pconf, "/tmp/httpd_shm_delay.%ld", (long int)getpid());

    /* Now create that segment */
    rs = apr_shm_create(&stats_shm, sizeof(stats_data),
                        (const char *) shmfilename, pconf);
    if (rs != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rs, s,
                     "Failed to create shared memory segment on file %s",
                     shmfilename);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

	rs = apr_shm_create(&delaypool_shm, sizeof(delaypool),
                        (const char *) shmfilename_delaypool, pconf);
    if (rs != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rs, s,
                     "Failed to create shared memory segment on file %s",
                     shmfilename_delaypool);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Created it, now let's zero it out */
    stats = (stats_data *)apr_shm_baseaddr_get(stats_shm);
    stats->noResp200 = 0;
    stats->noResp304 = 0;
    stats->noResp404 = 0;
	stats->noResp503 = 0;
    stats->noResp5XX = 0;
	for (i = 0; i <= MAX_ZOOM; i++) {
		stats->noRespZoom[i] = 0;
	}
    stats->noRespOther = 0;
    stats->noFreshCache = 0;
    stats->noFreshRender = 0;
    stats->noOldCache = 0;
    stats->noOldRender = 0;

	delayp = (delaypool *)apr_shm_baseaddr_get(delaypool_shm);
	
	delayp->last_tile_fillup = apr_time_now();
	delayp->last_render_fillup = apr_time_now();

	for (i = 0; i < DELAY_HASHTABLE_SIZE; i++) {
		delayp->users[i].ip_addr = (in_addr_t)0;
		delayp->users[i].available_tiles = 0;
		delayp->users[i].available_render_req = 0;
	}
	for (i = 0; i < DELAY_HASHTABLE_WHITELIST_SIZE; i++) {
		delayp->whitelist[i] = (in_addr_t)0;
	}
	/* TODO: need a way to initialise the delaypool whitelist */


    /* Create global mutex */

    /*
     * Create another unique filename to lock upon. Note that
     * depending on OS and locking mechanism of choice, the file
     * may or may not be actually created.
     */
    mutexfilename = apr_psprintf(pconf, "/tmp/httpd_mutex.%ld",
                                 (long int) getpid());

    rs = apr_global_mutex_create(&stats_mutex, (const char *) mutexfilename,
                                 APR_LOCK_DEFAULT, pconf);
    if (rs != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rs, s,
                     "Failed to create mutex on file %s",
                     mutexfilename);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

#ifdef MOD_TILE_SET_MUTEX_PERMS
    rs = unixd_set_global_mutex_perms(stats_mutex);
    if (rs != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rs, s,
                     "Parent could not set permissions on mod_tile "
                     "mutex: check User and Group directives");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
#endif /* MOD_TILE_SET_MUTEX_PERMS */

    /*
     * Create another unique filename to lock upon. Note that
     * depending on OS and locking mechanism of choice, the file
     * may or may not be actually created.
     */
    mutexfilename = apr_psprintf(pconf, "/tmp/httpd_mutex_delay.%ld",
                                 (long int) getpid());

    rs = apr_global_mutex_create(&delay_mutex, (const char *) mutexfilename,
                                 APR_LOCK_DEFAULT, pconf);
    if (rs != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rs, s,
                     "Failed to create mutex on file %s",
                     mutexfilename);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

#ifdef MOD_TILE_SET_MUTEX_PERMS
    rs = unixd_set_global_mutex_perms(delay_mutex);
    if (rs != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rs, s,
                     "Parent could not set permissions on mod_tile "
                     "mutex: check User and Group directives");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
#endif /* MOD_TILE_SET_MUTEX_PERMS */

    return OK;
}


/*
 * This routine gets called when a child inits. We use it to attach
 * to the shared memory segment, and reinitialize the mutex.
 */

static void mod_tile_child_init(apr_pool_t *p, server_rec *s)
{
    apr_status_t rs;

     /*
      * Re-open the mutex for the child. Note we're reusing
      * the mutex pointer global here.
      */
     rs = apr_global_mutex_child_init(&stats_mutex,
                                      (const char *) mutexfilename,
                                      p);
     if (rs != APR_SUCCESS) {
         ap_log_error(APLOG_MARK, APLOG_CRIT, rs, s,
                     "Failed to reopen mutex on file %s",
                     shmfilename);
         /* There's really nothing else we can do here, since
          * This routine doesn't return a status. */
         exit(1); /* Ugly, but what else? */
     }
}

static void register_hooks(__attribute__((unused)) apr_pool_t *p)
{
    ap_hook_post_config(mod_tile_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(mod_tile_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(tile_handler_serve, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(tile_handler_dirty, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(tile_handler_status, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(tile_handler_mod_stats, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_translate_name(tile_translate, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_map_to_storage(tile_storage_hook, NULL, NULL, APR_HOOK_FIRST);
}

static const char *_add_tile_config(cmd_parms *cmd, void *mconfig, const char *baseuri, const char *name, int minzoom, int maxzoom)
{
    if (strlen(name) == 0) {
        return "ConfigName value must not be null";
    }

    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    tile_config_rec *tilecfg = apr_array_push(scfg->configs);

    // Ensure URI string ends with a trailing slash
    int urilen = strlen(baseuri);

    if (urilen==0)
      snprintf(tilecfg->baseuri, PATH_MAX, "/");
    else if (baseuri[urilen-1] != '/')
      snprintf(tilecfg->baseuri, PATH_MAX, "%s/", baseuri);
    else
      snprintf(tilecfg->baseuri, PATH_MAX, "%s", baseuri);

    strncpy(tilecfg->xmlname, name, XMLCONFIG_MAX-1);
    tilecfg->xmlname[XMLCONFIG_MAX-1] = 0;
    tilecfg->minzoom = minzoom;
    tilecfg->maxzoom = maxzoom;


    return NULL;
}

static const char *add_tile_config(cmd_parms *cmd, void *mconfig, const char *baseuri, const char *name)
{
    return _add_tile_config(cmd, mconfig, baseuri, name, 0, MAX_ZOOM);
}

static const char *load_tile_config(cmd_parms *cmd, void *mconfig, const char *conffile)
{
    FILE * hini ;
    char filename[PATH_MAX];
    char xmlname[XMLCONFIG_MAX];
    char line[INILINE_MAX];
    char key[INILINE_MAX];
    char value[INILINE_MAX];
    const char * result;

    if (strlen(conffile) == 0) {
        strcpy(filename, RENDERD_CONFIG);
    } else {
        strcpy(filename, conffile);
    }

    // Load the config
    if ((hini=fopen(filename, "r"))==NULL) {
        return "Unable to open config file";
    }

    while (fgets(line, INILINE_MAX, hini)!=NULL) {
        if (line[0] == '#') continue;
        if (line[strlen(line)-1] == '\n') line[strlen(line)-1] = 0;
        if (line[0] == '[') {
            if (strlen(line) >= XMLCONFIG_MAX){
                return "XML name too long";
            }
            sscanf(line, "[%[^]]", xmlname);
        } else if (sscanf(line, "%[^=]=%[^;#]", key, value) == 2
               ||  sscanf(line, "%[^=]=\"%[^\"]\"", key, value) == 2) {

            if (!strcmp(key, "URI")){
                if (strlen(value) >= PATH_MAX){
                    return "URI too long";
                }
                result = add_tile_config(cmd, mconfig, value, xmlname);
                if (result != NULL) return result;
            }
        }
    }
    fclose(hini);
    return NULL;
}

static const char *mod_tile_request_timeout_config(cmd_parms *cmd, void *mconfig, const char *request_timeout_string)
{
    int request_timeout;

    if (sscanf(request_timeout_string, "%d", &request_timeout) != 1) {
        return "ModTileRequestTimeout needs integer argument";
    }

    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    scfg->request_timeout = request_timeout;
    return NULL;
}

static const char *mod_tile_request_timeout_missing_config(cmd_parms *cmd, void *mconfig, const char *request_timeout_string)
{
    int request_timeout;

    if (sscanf(request_timeout_string, "%d", &request_timeout) != 1) {
        return "ModTileMissingRequestTimeout needs integer argument";
    }

    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    scfg->request_timeout_priority = request_timeout;
    return NULL;
}

static const char *mod_tile_max_load_old_config(cmd_parms *cmd, void *mconfig, const char *max_load_old_string)
{
    int max_load_old;

    if (sscanf(max_load_old_string, "%d", &max_load_old) != 1) {
        return "ModTileMaxLoadOld needs integer argument";
    }

    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    scfg->max_load_old = max_load_old;
    return NULL;
}

static const char *mod_tile_max_load_missing_config(cmd_parms *cmd, void *mconfig, const char *max_load_missing_string)
{
    int max_load_missing;

    if (sscanf(max_load_missing_string, "%d", &max_load_missing) != 1) {
        return "ModTileMaxLoadMissing needs integer argument";
    }

    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    scfg->max_load_missing = max_load_missing;
    return NULL;
}

static const char *mod_tile_renderd_socket_name_config(cmd_parms *cmd, void *mconfig, const char *renderd_socket_name_string)
{
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    strncpy(scfg->renderd_socket_name, renderd_socket_name_string, PATH_MAX-1);
    scfg->renderd_socket_name[PATH_MAX-1] = 0;
    return NULL;
}

static const char *mod_tile_tile_dir_config(cmd_parms *cmd, void *mconfig, const char *tile_dir_string)
{
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    strncpy(scfg->tile_dir, tile_dir_string, PATH_MAX-1);
    scfg->tile_dir[PATH_MAX-1] = 0;
    return NULL;
}

static const char *mod_tile_cache_extended_host_name_config(cmd_parms *cmd, void *mconfig, const char *cache_extended_hostname)
{
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    strncpy(scfg->cache_extended_hostname, cache_extended_hostname, PATH_MAX-1);
    scfg->cache_extended_hostname[PATH_MAX-1] = 0;

    return NULL;
}

static const char *mod_tile_cache_extended_duration_config(cmd_parms *cmd, void *mconfig, const char *cache_duration_string)
{
    int cache_duration;
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    if (sscanf(cache_duration_string, "%d", &cache_duration) != 1) {
            return "ModTileCacheExtendedDuration needs integer argument";
    }
    scfg->cache_extended_duration = cache_duration;

    return NULL;
}

static const char *mod_tile_cache_lastmod_factor_config(cmd_parms *cmd, void *mconfig, const char *modified_factor_string)
{
    float modified_factor;
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config,
            &tile_module);
    if (sscanf(modified_factor_string, "%f", &modified_factor) != 1) {
        return "ModTileCacheLastModifiedFactor needs float argument";
    }
    scfg->cache_duration_last_modified_factor = modified_factor;
    return NULL;
}

static const char *mod_tile_cache_duration_max_config(cmd_parms *cmd, void *mconfig, const char *cache_duration_string)
{
    int cache_duration;
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config,
            &tile_module);
    if (sscanf(cache_duration_string, "%d", &cache_duration) != 1) {
        return "ModTileCacheDurationMax needs integer argument";
    }
    scfg->cache_duration_max = cache_duration;
    return NULL;
}

static const char *mod_tile_cache_duration_dirty_config(cmd_parms *cmd, void *mconfig, const char *cache_duration_string)
{
    int cache_duration;
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config,
            &tile_module);
    if (sscanf(cache_duration_string, "%d", &cache_duration) != 1) {
        return "ModTileCacheDurationDirty needs integer argument";
    }
    scfg->cache_duration_dirty = cache_duration;
    return NULL;
}

static const char *mod_tile_cache_duration_minimum_config(cmd_parms *cmd, void *mconfig, const char *cache_duration_string)
{
    int cache_duration;
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config,
            &tile_module);
    if (sscanf(cache_duration_string, "%d", &cache_duration) != 1) {
        return "ModTileCacheDurationMinimum needs integer argument";
    }
    scfg->cache_duration_minimum = cache_duration;
    return NULL;
}

static const char *mod_tile_cache_duration_low_config(cmd_parms *cmd, void *mconfig, const char *zoom_level_string, const char *cache_duration_string)
{
    int zoom_level;
    int cache_duration;
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    if (sscanf(zoom_level_string, "%d", &zoom_level) != 1) {
            return "ModTileCacheDurationLowZoom needs integer argument";
    }
    if (sscanf(cache_duration_string, "%d", &cache_duration) != 1) {
            return "ModTileCacheDurationLowZoom needs integer argument";
    }
    scfg->cache_level_low_zoom = zoom_level;
    scfg->cache_duration_low_zoom = cache_duration;

    return NULL;
}
static const char *mod_tile_cache_duration_medium_config(cmd_parms *cmd, void *mconfig, const char *zoom_level_string, const char *cache_duration_string)
{
    int zoom_level;
    int cache_duration;
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    if (sscanf(zoom_level_string, "%d", &zoom_level) != 1) {
            return "ModTileCacheDurationMediumZoom needs integer argument";
    }
    if (sscanf(cache_duration_string, "%d", &cache_duration) != 1) {
            return "ModTileCacheDurationMediumZoom needs integer argument";
    }
    scfg->cache_level_medium_zoom = zoom_level;
    scfg->cache_duration_medium_zoom = cache_duration;

    return NULL;
}

static const char *mod_tile_enable_stats(cmd_parms *cmd, void *mconfig, int enableStats)
{
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    scfg->enableGlobalStats = enableStats;
    return NULL;
}

static const char *mod_tile_enable_throtteling(cmd_parms *cmd, void *mconfig, int enableThrotteling)
{
    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    scfg->enableTileThrotteling = enableThrotteling;
    return NULL;
}

static const char *mod_tile_delaypool_tiles_config(cmd_parms *cmd, void *mconfig, const char *bucketsize_string, const char *topuprate_string)
{
    int bucketsize;
    float topuprate;

    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    if (sscanf(bucketsize_string, "%d", &bucketsize) != 1) {
            return "ModTileThrottelingTileSize needs integer argument";
    }
    if (sscanf(topuprate_string, "%f", &topuprate) != 1) {
            return "ModTileThrottelingTileRate needs number argument";
    }
    scfg->delaypoolTileSize = bucketsize;

	/*Convert topup rate into microseconds per tile */
    scfg->delaypoolTileRate = (long)(1000000.0/topuprate);

    return NULL;
}

static const char *mod_tile_delaypool_render_config(cmd_parms *cmd, void *mconfig, const char *bucketsize_string, const char *topuprate_string)
{
    int bucketsize;
    float topuprate;

    tile_server_conf *scfg = ap_get_module_config(cmd->server->module_config, &tile_module);
    if (sscanf(bucketsize_string, "%d", &bucketsize) != 1) {
            return "ModTileThrottelingTileSize needs integer argument";
    }
    if (sscanf(topuprate_string, "%f", &topuprate) != 1) {
            return "ModTileThrottelingTileRate needs number argument";
    }
    scfg->delaypoolRenderSize = bucketsize;

	/*Convert topup rate into microseconds per tile */
    scfg->delaypoolRenderRate = (long)(1000000.0/topuprate);

    return NULL;
}

static void *create_tile_config(apr_pool_t *p, server_rec *s)
{
    tile_server_conf * scfg = (tile_server_conf *) apr_pcalloc(p, sizeof(tile_server_conf));

    scfg->configs = apr_array_make(p, 4, sizeof(tile_config_rec));
    scfg->request_timeout = REQUEST_TIMEOUT;
    scfg->request_timeout_priority = REQUEST_TIMEOUT;
    scfg->max_load_old = MAX_LOAD_OLD;
    scfg->max_load_missing = MAX_LOAD_MISSING;
    strncpy(scfg->renderd_socket_name, RENDER_SOCKET, PATH_MAX-1);
    scfg->renderd_socket_name[PATH_MAX-1] = 0;
    strncpy(scfg->tile_dir, HASH_PATH, PATH_MAX-1);
    scfg->tile_dir[PATH_MAX-1] = 0;
	memset(&(scfg->cache_extended_hostname),0,PATH_MAX);
	scfg->cache_extended_duration = 0;
    scfg->cache_duration_dirty = 15*60;
    scfg->cache_duration_last_modified_factor = 0.0;
    scfg->cache_duration_max = 7*24*60*60;
    scfg->cache_duration_minimum = 3*60*60;
    scfg->cache_duration_low_zoom = 6*24*60*60;
    scfg->cache_duration_medium_zoom = 1*24*60*60;
    scfg->cache_level_low_zoom = 0;
    scfg->cache_level_medium_zoom = 0;
    scfg->enableGlobalStats = 1;
	scfg->enableTileThrotteling = 0;
	scfg->delaypoolTileSize = AVAILABLE_TILE_BUCKET_SIZE;
	scfg->delaypoolTileRate = RENDER_TOPUP_RATE;
	scfg->delaypoolRenderSize = AVAILABLE_RENDER_BUCKET_SIZE;
	scfg->delaypoolRenderRate = RENDER_TOPUP_RATE;


    return scfg;
}

static void *merge_tile_config(apr_pool_t *p, void *basev, void *overridesv)
{
    int i;
    tile_server_conf * scfg = (tile_server_conf *) apr_pcalloc(p, sizeof(tile_server_conf));
    tile_server_conf * scfg_base = (tile_server_conf *) basev;
    tile_server_conf * scfg_over = (tile_server_conf *) overridesv;

    scfg->configs = apr_array_append(p, scfg_base->configs, scfg_over->configs);
    scfg->request_timeout = scfg_over->request_timeout;
	scfg->request_timeout_priority = scfg_over->request_timeout_priority;
    scfg->max_load_old = scfg_over->max_load_old;
    scfg->max_load_missing = scfg_over->max_load_missing;
    strncpy(scfg->renderd_socket_name, scfg_over->renderd_socket_name, PATH_MAX-1);
    scfg->renderd_socket_name[PATH_MAX-1] = 0;
    strncpy(scfg->tile_dir, scfg_over->tile_dir, PATH_MAX-1);
    scfg->tile_dir[PATH_MAX-1] = 0;
    strncpy(scfg->cache_extended_hostname, scfg_over->cache_extended_hostname, PATH_MAX-1);
    scfg->cache_extended_hostname[PATH_MAX-1] = 0;
    scfg->cache_extended_duration = scfg_over->cache_extended_duration;
    scfg->cache_duration_dirty = scfg_over->cache_duration_dirty;
    scfg->cache_duration_last_modified_factor = scfg_over->cache_duration_last_modified_factor;
    scfg->cache_duration_max = scfg_over->cache_duration_max;
    scfg->cache_duration_minimum = scfg_over->cache_duration_minimum;
    scfg->cache_duration_low_zoom = scfg_over->cache_duration_low_zoom;
    scfg->cache_duration_medium_zoom = scfg_over->cache_duration_medium_zoom;
    scfg->cache_level_low_zoom = scfg_over->cache_level_low_zoom;
    scfg->cache_level_medium_zoom = scfg_over->cache_level_medium_zoom;
    scfg->enableGlobalStats = scfg_over->enableGlobalStats;
	scfg->enableTileThrotteling = scfg_over->enableTileThrotteling;
	scfg->delaypoolTileSize = scfg_over->delaypoolTileSize;
	scfg->delaypoolTileRate = scfg_over->delaypoolTileRate;
	scfg->delaypoolRenderSize = scfg_over->delaypoolRenderSize;
	scfg->delaypoolRenderRate = scfg_over->delaypoolRenderRate;

    //Construct a table of minimum cache times per zoom level
    for (i = 0; i <= MAX_ZOOM; i++) {
        if (i <= scfg->cache_level_low_zoom) {
            scfg->mincachetime[i] = scfg->cache_duration_low_zoom;
        } else if (i <= scfg->cache_level_medium_zoom) {
            scfg->mincachetime[i] = scfg->cache_duration_medium_zoom;
        } else {
            scfg->mincachetime[i] = scfg->cache_duration_minimum;
        }
    }

    return scfg;
}

static const command_rec tile_cmds[] =
{
    AP_INIT_TAKE1(
        "LoadTileConfigFile",            /* directive name */
        load_tile_config,                /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "load an entire renderd config file"  /* directive description */
    ),
    AP_INIT_TAKE2(
        "AddTileConfig",                 /* directive name */
        add_tile_config,                 /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "path and name of renderd config to use"  /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileRequestTimeout",         /* directive name */
        mod_tile_request_timeout_config, /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set timeout in seconds on mod_tile requests"  /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileMissingRequestTimeout",         /* directive name */
        mod_tile_request_timeout_missing_config, /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set timeout in seconds on missing mod_tile requests"  /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileMaxLoadOld",             /* directive name */
        mod_tile_max_load_old_config,    /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set max load for rendering old tiles"  /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileMaxLoadMissing",         /* directive name */
        mod_tile_max_load_missing_config, /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set max load for rendering missing tiles"  /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileRenderdSocketName",      /* directive name */
        mod_tile_renderd_socket_name_config, /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set name of unix domain socket for connecting to rendering daemon"  /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileTileDir",                /* directive name */
        mod_tile_tile_dir_config,        /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set name of tile cache directory"  /* directive description */
    ),
	AP_INIT_TAKE1(
        "ModTileCacheExtendedHostName",                /* directive name */
        mod_tile_cache_extended_host_name_config,        /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "set hostname for extended period caching"  /* directive description */
    ),
	AP_INIT_TAKE1(
        "ModTileCacheExtendedDuration",                /* directive name */
        mod_tile_cache_extended_duration_config,        /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "set length of extended period caching"  /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileCacheDurationMax",                /* directive name */
        mod_tile_cache_duration_max_config,        /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set the maximum cache expiry in seconds"  /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileCacheDurationDirty",                    /* directive name */
        mod_tile_cache_duration_dirty_config,           /* config action routine */
        NULL,                                           /* argument to include in call */
        OR_OPTIONS,                                     /* where available */
        "Set the cache expiry for serving dirty tiles"  /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileCacheDurationMinimum",          /* directive name */
        mod_tile_cache_duration_minimum_config, /* config action routine */
        NULL,                                   /* argument to include in call */
        OR_OPTIONS,                             /* where available */
        "Set the minimum cache expiry"          /* directive description */
    ),
    AP_INIT_TAKE1(
        "ModTileCacheLastModifiedFactor",       /* directive name */
        mod_tile_cache_lastmod_factor_config,   /* config action routine */
        NULL,                                   /* argument to include in call */
        OR_OPTIONS,                             /* where available */
        "Set the factor by which the last modified determins cache expiry" /* directive description */
    ),
    AP_INIT_TAKE2(
        "ModTileCacheDurationLowZoom",       /* directive name */
        mod_tile_cache_duration_low_config,                 /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set the minimum cache duration and zoom level for low zoom tiles"  /* directive description */
    ),
    AP_INIT_TAKE2(
        "ModTileCacheDurationMediumZoom",       /* directive name */
        mod_tile_cache_duration_medium_config,                 /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set the minimum cache duration and zoom level for medium zoom tiles"  /* directive description */
    ),
    AP_INIT_FLAG(
        "ModTileEnableStats",       /* directive name */
        mod_tile_enable_stats,                 /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "On Off - enable of keeping stats about what mod_tile is serving"  /* directive description */
    ),
	AP_INIT_FLAG(
        "ModTileEnableTileThrotteling",       /* directive name */
        mod_tile_enable_throtteling,                 /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "On Off - enable of throtteling of IPs who excessively download tiles such as scrapers"  /* directive description */
    ),
	AP_INIT_TAKE2(
        "ModTileThrottelingTiles",       /* directive name */
        mod_tile_delaypool_tiles_config,                 /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set the initial bucket size (number of tiles) and top up rate (tiles per second) for throtteling tile request per IP"  /* directive description */
    ),
	AP_INIT_TAKE2(
        "ModTileThrottelingRenders",       /* directive name */
        mod_tile_delaypool_render_config,                 /* config action routine */
        NULL,                            /* argument to include in call */
        OR_OPTIONS,                      /* where available */
        "Set the initial bucket size (number of tiles) and top up rate (tiles per second) for throtteling tile request per IP"  /* directive description */
    ),
    {NULL}
};

module AP_MODULE_DECLARE_DATA tile_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,                                /* dir config creater */
    NULL,                                /* dir merger --- default is to override */
    create_tile_config,                  /* server config */
    merge_tile_config,                   /* merge server config */
    tile_cmds,                           /* command apr_table_t */
    register_hooks                       /* register hooks */
};

