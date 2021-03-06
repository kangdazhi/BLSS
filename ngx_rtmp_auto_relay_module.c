
/*
 * Original work Copyright (C) Roman Arutyunyan
 * Modified work Copyright (C) 2017 Gnolizuh
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_relay_module.h"
#include "ngx_rtmp_live_module.h"


#define NGX_RTMP_RELAY_STREAM_OFF   0
#define NGX_RTMP_RELAY_STREAM_ALL   1
#define NGX_RTMP_RELAY_STREAM_HASH  2


static ngx_rtmp_publish_pt          next_publish;
static ngx_rtmp_play_pt             next_play;
static ngx_rtmp_delete_stream_pt    next_delete_stream;


static ngx_int_t ngx_rtmp_auto_relay_init_process(ngx_cycle_t *cycle);
static void ngx_rtmp_auto_relay_exit_process(ngx_cycle_t *cycle);
static void * ngx_rtmp_auto_relay_create_conf(ngx_cycle_t *cf);
static char * ngx_rtmp_auto_relay_init_conf(ngx_cycle_t *cycle, void *conf);
#if (NGX_HAVE_UNIX_DOMAIN)
static ngx_int_t ngx_rtmp_auto_relay_publish(ngx_rtmp_session_t *s, ngx_rtmp_publish_t *v);
static ngx_int_t ngx_rtmp_auto_relay_play(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v);
static ngx_int_t ngx_rtmp_auto_relay_delete_stream(ngx_rtmp_session_t *s, ngx_rtmp_delete_stream_t *v);
#endif


typedef struct ngx_rtmp_auto_relay_ctx_s ngx_rtmp_auto_relay_ctx_t;

struct ngx_rtmp_auto_relay_ctx_s {
    ngx_int_t                      *slots; /* NGX_MAX_PROCESSES */
    u_char                          name[NGX_RTMP_MAX_NAME];
    u_char                          args[NGX_RTMP_MAX_ARGS];
    ngx_event_t                     push_evt;
};


typedef struct {
    ngx_uint_t                      relay_stream;
    ngx_str_t                       auto_relay_socket_dir;
    ngx_msec_t                      relay_reconnect_time;
} ngx_rtmp_auto_relay_conf_t;


static ngx_conf_enum_t ngx_rtmp_auto_relay_mode_slots[] = {
    { ngx_string("off"),            NGX_RTMP_RELAY_STREAM_OFF  },
    { ngx_string("all"),            NGX_RTMP_RELAY_STREAM_ALL  },
    { ngx_string("hash"),           NGX_RTMP_RELAY_STREAM_HASH },
    { ngx_null_string,              0 }
};


static ngx_command_t  ngx_rtmp_auto_relay_commands[] = {

    { ngx_string("relay_stream"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      0,
      offsetof(ngx_rtmp_auto_relay_conf_t, relay_stream),
      &ngx_rtmp_auto_relay_mode_slots },

    { ngx_string("relay_reconnect_time"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_rtmp_auto_relay_conf_t, relay_reconnect_time),
      NULL },

    { ngx_string("auto_relay_socket_dir"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      0,
      offsetof(ngx_rtmp_auto_relay_conf_t, auto_relay_socket_dir),
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_rtmp_auto_relay_module_ctx = {
    ngx_string("rtmp_auto_relay"),
    ngx_rtmp_auto_relay_create_conf,         /* create conf */
    ngx_rtmp_auto_relay_init_conf            /* init conf */
};


ngx_module_t  ngx_rtmp_auto_relay_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_auto_relay_module_ctx,        /* module context */
    ngx_rtmp_auto_relay_commands,           /* module directives */
    NGX_CORE_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    ngx_rtmp_auto_relay_init_process,       /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    ngx_rtmp_auto_relay_exit_process,       /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_rtmp_module_t  ngx_rtmp_auto_relay_index_module_ctx = {
    NULL,                                   /* preconfiguration */
    NULL,                                   /* postconfiguration */
    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */
    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */
    NULL,                                   /* create service configuration */
    NULL,                                   /* merge service configuration */
    NULL,                                   /* create app configuration */
    NULL                                    /* merge app configuration */
};


ngx_module_t  ngx_rtmp_auto_relay_index_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_auto_relay_index_module_ctx,  /* module context */
    NULL,                                   /* module directives */
    NGX_RTMP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


#define NGX_RTMP_AUTO_PUSH_SOCKNAME         "nginx-rtmp"


static ngx_int_t
ngx_rtmp_auto_relay_init_process(ngx_cycle_t *cycle)
{
#if (NGX_HAVE_UNIX_DOMAIN)
    ngx_rtmp_auto_relay_conf_t *apcf;
    ngx_listening_t            *ls, *lss;
    struct sockaddr_un         *saun;
    int                         reuseaddr;
    ngx_socket_t                s;
    size_t                      n;
    ngx_file_info_t             fi;

    if (ngx_process != NGX_PROCESS_WORKER) {
        return NGX_OK;
    }

    apcf = (ngx_rtmp_auto_relay_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                       ngx_rtmp_auto_relay_module);
    if (apcf->relay_stream == NGX_RTMP_RELAY_STREAM_OFF) {
        return NGX_OK;
    }

    next_publish = ngx_rtmp_publish;
    ngx_rtmp_publish = ngx_rtmp_auto_relay_publish;

    next_play = ngx_rtmp_play;
    ngx_rtmp_play = ngx_rtmp_auto_relay_play;

    next_delete_stream = ngx_rtmp_delete_stream;
    ngx_rtmp_delete_stream = ngx_rtmp_auto_relay_delete_stream;

    reuseaddr = 1;
    s = (ngx_socket_t) -1;

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, cycle->log, 0,
            "auto_relay: creating sockets");

    /* TODO: clone all RTMP listenings? */
    ls = cycle->listening.elts;
    lss = NULL;
    for (n = 0; n < cycle->listening.nelts; ++n, ++ls) {
        if (ls->handler == ngx_rtmp_init_connection) {
            lss = ls;
            break;
        }
    }

    if (lss == NULL) {
        return NGX_OK;
    }

    ls = ngx_array_push(&cycle->listening);
    if (ls == NULL) {
        return NGX_ERROR;
    }

    *ls = *lss;

    /* Disable unix socket client address extraction
     * from accept call
     * Nginx generates bad addr_text with this enabled */
    ls->addr_ntop = 0;
    ls->reuseport = 0;

    ls->socklen = sizeof(struct sockaddr_un);
    saun = ngx_pcalloc(cycle->pool, ls->socklen);
    ls->sockaddr = (struct sockaddr *) saun;
    if (ls->sockaddr == NULL) {
        return NGX_ERROR;
    }
    saun->sun_family = AF_UNIX;
    *ngx_snprintf((u_char *) saun->sun_path, sizeof(saun->sun_path),
                  "%V/" NGX_RTMP_AUTO_PUSH_SOCKNAME ".%i",
                  &apcf->auto_relay_socket_dir, ngx_process_slot)
        = 0;

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, cycle->log, 0,
                   "auto_relay: create socket '%s'",
                   saun->sun_path);

    if (ngx_file_info(saun->sun_path, &fi) != ENOENT) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, cycle->log, 0,
                       "auto_relay: delete existing socket '%s'",
                       saun->sun_path);
        ngx_delete_file(saun->sun_path);
    }

    ngx_str_set(&ls->addr_text, "worker_socket");

    s = ngx_socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                      ngx_socket_n " worker_socket failed");
        return NGX_ERROR;
    }

    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                   (const void *) &reuseaddr, sizeof(int))
        == -1)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                "setsockopt(SO_REUSEADDR) worker_socket failed");
        goto sock_error;
    }

    if (!(ngx_event_flags & NGX_USE_AIO_EVENT)) {
        if (ngx_nonblocking(s) == -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                          ngx_nonblocking_n " worker_socket failed");
            return NGX_ERROR;
        }
    }

    if (bind(s, (struct sockaddr *) saun, sizeof(*saun)) == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                      ngx_nonblocking_n " worker_socket bind failed");
        goto sock_error;
    }

    if (listen(s, NGX_LISTEN_BACKLOG) == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                      "listen() to worker_socket, backlog %d failed",
                      NGX_LISTEN_BACKLOG);
        goto sock_error;
    }

    ls->fd = s;
    ls->listen = 1;

    return NGX_OK;

sock_error:
    if (s != (ngx_socket_t) -1 && ngx_close_socket(s) == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                ngx_close_socket_n " worker_socket failed");
    }
    ngx_delete_file(saun->sun_path);

    return NGX_ERROR;

#else  /* NGX_HAVE_UNIX_DOMAIN */

    return NGX_OK;

#endif /* NGX_HAVE_UNIX_DOMAIN */
}


static void
ngx_rtmp_auto_relay_exit_process(ngx_cycle_t *cycle)
{
#if (NGX_HAVE_UNIX_DOMAIN)
    ngx_rtmp_auto_relay_conf_t  *apcf;
    u_char                      path[NGX_MAX_PATH];

    apcf = (ngx_rtmp_auto_relay_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                       ngx_rtmp_auto_relay_module);
    if (apcf->relay_stream == NGX_RTMP_RELAY_STREAM_OFF) {
        return;
    }

    *ngx_snprintf(path, sizeof(path),
                  "%V/" NGX_RTMP_AUTO_PUSH_SOCKNAME ".%i",
                  &apcf->auto_relay_socket_dir, ngx_process_slot) = 0;

    ngx_delete_file(path);
#endif
}


static void *
ngx_rtmp_auto_relay_create_conf(ngx_cycle_t *cycle)
{
    ngx_rtmp_auto_relay_conf_t       *apcf;

    apcf = ngx_pcalloc(cycle->pool, sizeof(ngx_rtmp_auto_relay_conf_t));
    if (apcf == NULL) {
        return NULL;
    }

    apcf->relay_stream = NGX_CONF_UNSET_UINT;
    apcf->relay_reconnect_time = NGX_CONF_UNSET_MSEC;

    return apcf;
}


static char *
ngx_rtmp_auto_relay_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_rtmp_auto_relay_conf_t      *apcf = conf;

    ngx_conf_init_uint_value(apcf->relay_stream, NGX_RTMP_RELAY_STREAM_OFF);
    ngx_conf_init_msec_value(apcf->relay_reconnect_time, 100);

    if (apcf->auto_relay_socket_dir.len == 0) {
        ngx_str_set(&apcf->auto_relay_socket_dir, "/tmp");
    }

    return NGX_CONF_OK;
}


#if (NGX_HAVE_UNIX_DOMAIN)
static void
ngx_rtmp_auto_relay_all_push(ngx_event_t *ev)
{
    ngx_rtmp_session_t             *s = ev->data;

    ngx_rtmp_auto_relay_conf_t     *apcf;
    ngx_rtmp_auto_relay_ctx_t      *ctx;
    ngx_int_t                      *slot;
    ngx_int_t                       n;
    ngx_rtmp_relay_target_t         at;
    u_char                          path[sizeof("unix:") + NGX_MAX_PATH];
    u_char                          flash_ver[sizeof("APSH ,") +
                                              NGX_INT_T_LEN * 2];
    u_char                          play_path[NGX_RTMP_MAX_NAME];
    ngx_str_t                       name;
    u_char                         *p;
    ngx_str_t                      *u;
    ngx_pid_t                       pid;
    ngx_int_t                       npushed;
    ngx_core_conf_t                *ccf;
    ngx_file_info_t                 fi;

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "auto_relay_all_push: connect");

    ccf = (ngx_core_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                           ngx_core_module);

    apcf = (ngx_rtmp_auto_relay_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                                       ngx_rtmp_auto_relay_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_auto_relay_index_module);
    if (ctx == NULL) {
        return;
    }

    name.data = ctx->name;
    name.len = ngx_strlen(name.data);

    ngx_memzero(&at, sizeof(at));
    ngx_str_set(&at.page_url, "nginx-auto-all-push");
    at.tag = &ngx_rtmp_auto_relay_module;
    at.tc_url = s->tc_url;
    at.host = s->host;
    at.host_mask = s->host_mask & (NGX_RTMP_HOSTNAME_RTMP | NGX_RTMP_HOSTNAME_HTTP_FLV | NGX_RTMP_HOSTNAME_HLS);

    if (ctx->args[0]) {
        at.play_path.data = play_path;
        at.play_path.len = ngx_snprintf(play_path, sizeof(play_path),
                                        "%s?%s", ctx->name, ctx->args) -
                           play_path;
    }

    slot = ctx->slots;
    npushed = 0;

    for (n = 0; n < NGX_MAX_PROCESSES; ++n, ++slot) {
        if (n == ngx_process_slot) {
            continue;
        }

        pid = ngx_processes[n].pid;
        if (pid == 0 || pid == NGX_INVALID_PID) {
            continue;
        }

        if (*slot) {
            npushed++;
            continue;
        }

        at.data = &ngx_processes[n];

        ngx_memzero(&at.url, sizeof(at.url));
        u = &at.url.url;
        p = ngx_snprintf(path, sizeof(path) - 1,
                         "unix:%V/" NGX_RTMP_AUTO_PUSH_SOCKNAME ".%i",
                         &apcf->auto_relay_socket_dir, n);
        *p = 0;

        if (ngx_file_info(path + sizeof("unix:") - 1, &fi) != NGX_OK) {
            ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "auto_relay_all_push: " ngx_file_info_n " failed: "
                           "slot=%i pid=%P socket='%s'" "url='%V' name='%s'",
                           n, pid, path, u, ctx->name);
            continue;
        }

        u->data = path;
        u->len = p - path;
        if (ngx_parse_url(s->connection->pool, &at.url) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "auto_relay_all_push: auto-push parse_url failed "
                          "url='%V' name='%s'",
                          u, ctx->name);
            continue;
        }

        p = ngx_snprintf(flash_ver, sizeof(flash_ver) - 1, "APSH %i,%i",
                         (ngx_int_t) ngx_process_slot, (ngx_int_t) ngx_pid);
        at.flash_ver.data = flash_ver;
        at.flash_ver.len = p - flash_ver;

        ngx_log_debug4(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "auto_relay_all_push: connect slot=%i pid=%P socket='%s' name='%s'",
                       n, pid, path, ctx->name);

        if (ngx_rtmp_relay_push(s, &name, &at) == NGX_OK) {
            *slot = 1;
            npushed++;
            continue;
        }

        ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                      "auto_relay_all_push: connect failed: slot=%i pid=%P socket='%s'"
                      "url='%V' name='%s'",
                      n, pid, path, u, ctx->name);
    }

    ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "auto_relay_all_push: pushed=%i total=%i failed=%i",
                   npushed, ccf->worker_processes,
                   ccf->worker_processes - 1 - npushed);

    if (ccf->worker_processes == npushed + 1) {
        return;
    }

    /* several workers failed */

    slot = ctx->slots;

    for (n = 0; n < NGX_MAX_PROCESSES; ++n, ++slot) {
        pid = ngx_processes[n].pid;

        if (n == ngx_process_slot || *slot == 1 ||
            pid == 0 || pid == NGX_INVALID_PID)
        {
            continue;
        }

        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "auto_relay_all_push: connect failed: slot=%i pid=%P name='%s'",
                      n, pid, ctx->name);
    }

    if (!ctx->push_evt.timer_set) {
        ngx_add_timer(&ctx->push_evt, apcf->relay_reconnect_time);
    }
}


static void
ngx_rtmp_auto_relay_hash_push(ngx_event_t *ev)
{
    ngx_rtmp_session_t             *s = ev->data;

    ngx_rtmp_auto_relay_conf_t     *apcf;
    ngx_rtmp_auto_relay_ctx_t      *ctx;
    ngx_rtmp_relay_target_t         at;
    u_char                          path[sizeof("unix:") + NGX_MAX_PATH];
    u_char                          flash_ver[sizeof("APSH ,") +
                                              NGX_INT_T_LEN * 2];
    u_char                          play_path[NGX_RTMP_MAX_NAME];
    ngx_str_t                       name;
    u_char                         *p;
    ngx_str_t                      *u;
    ngx_int_t                       h;
    ngx_pid_t                       pid;
    ngx_core_conf_t                *ccf;
    ngx_file_info_t                 fi;

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "auto_relay_hash_push: connect");

    ccf = (ngx_core_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                           ngx_core_module);

    apcf = (ngx_rtmp_auto_relay_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                                       ngx_rtmp_auto_relay_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_auto_relay_index_module);
    if (ctx == NULL) {
        return;
    }

    name.data = ctx->name;
    name.len = ngx_strlen(name.data);

    h = ngx_hash_key(ctx->name, ngx_strlen(ctx->name)) % ccf->worker_processes;
    if (h == ngx_process_slot) {
        return;
    }

    pid = ngx_processes[h].pid;
    if (pid == 0 || pid == NGX_INVALID_PID) {
        return;
    }

    ngx_memzero(&at, sizeof(at));
    ngx_str_set(&at.page_url, "nginx-auto-hash-push");
    at.tag = &ngx_rtmp_auto_relay_module;
    at.tc_url = s->tc_url;
    at.host = s->host;
    at.host_mask = s->host_mask & (NGX_RTMP_HOSTNAME_RTMP | NGX_RTMP_HOSTNAME_HTTP_FLV | NGX_RTMP_HOSTNAME_HLS);

    if (ctx->args[0]) {
        at.play_path.data = play_path;
        at.play_path.len = ngx_snprintf(play_path, sizeof(play_path),
                                        "%s?%s", ctx->name, ctx->args) -
                           play_path;
    }

    at.data = &ngx_processes[h];

    ngx_memzero(&at.url, sizeof(at.url));
    u = &at.url.url;
    p = ngx_snprintf(path, sizeof(path) - 1,
                     "unix:%V/" NGX_RTMP_AUTO_PUSH_SOCKNAME ".%i",
                     &apcf->auto_relay_socket_dir, h);
    *p = 0;

    if (ngx_file_info(path + sizeof("unix:") - 1, &fi) != NGX_OK) {
        ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "auto_relay_hash_push: " ngx_file_info_n " failed: "
                       "slot=%i pid=%P socket='%s'" "url='%V' name='%s'",
                       h, pid, path, u, ctx->name);
        return;
    }

    u->data = path;
    u->len = p - path;
    if (ngx_parse_url(s->connection->pool, &at.url) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "auto_relay_hash_push: auto-push parse_url failed "
                      "url='%V' name='%s'",
                      u, ctx->name);
        return;
    }

    p = ngx_snprintf(flash_ver, sizeof(flash_ver) - 1, "APSH %i,%i",
                     (ngx_int_t) ngx_process_slot, (ngx_int_t) ngx_pid);
    at.flash_ver.data = flash_ver;
    at.flash_ver.len = p - flash_ver;

    ngx_log_debug4(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "auto_relay_hash_push: connect slot=%i pid=%P socket='%s' name='%s'",
                   h, pid, path, ctx->name);

    if (ngx_rtmp_relay_push(s, &name, &at) == NGX_OK) {
        return;
    }

    ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                  "auto_relay_hash_push: connect failed: slot=%i pid=%P socket='%s'"
                  "url='%V' name='%s'",
                  h, pid, path, u, ctx->name);

    if (!ctx->push_evt.timer_set) {
        ngx_add_timer(&ctx->push_evt, apcf->relay_reconnect_time);
    }
}


static ngx_int_t
ngx_rtmp_auto_relay_publish(ngx_rtmp_session_t *s, ngx_rtmp_publish_t *v)
{
    ngx_rtmp_auto_relay_conf_t     *apcf;
    ngx_rtmp_auto_relay_ctx_t      *ctx;
    ngx_rtmp_live_stream_t        **stream;

    if (s->auto_relayed || (s->relay && !s->static_relay)) {
        goto next;
    }

    apcf = (ngx_rtmp_auto_relay_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                                       ngx_rtmp_auto_relay_module);
    if (apcf->relay_stream == NGX_RTMP_RELAY_STREAM_OFF) {
        goto next;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_auto_relay_index_module);
    if (ctx == NULL) {
        ctx = ngx_palloc(s->connection->pool,
                         sizeof(ngx_rtmp_auto_relay_ctx_t));
        if (ctx == NULL) {
            goto next;
        }
        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_auto_relay_index_module);

    }
    ngx_memzero(ctx, sizeof(*ctx));

    ngx_memcpy(ctx->name, v->name, sizeof(ctx->name));
    ngx_memcpy(ctx->args, v->args, sizeof(ctx->args));

    ctx->push_evt.data = s;
    ctx->push_evt.log = s->connection->log;
    ctx->push_evt.handler = ngx_rtmp_auto_relay_all_push;

    stream = ngx_rtmp_live_get_stream(s, ctx->name, 0);
    if (stream != NULL) {
        goto next;
    }

    ctx->slots = ngx_pcalloc(s->connection->pool,
                             sizeof(ngx_int_t) * NGX_MAX_PROCESSES);
    if (ctx->slots == NULL) {
        goto next;
    }

    if (apcf->relay_stream == NGX_RTMP_RELAY_STREAM_ALL) {
        ngx_rtmp_auto_relay_all_push(&ctx->push_evt);
    } else if (apcf->relay_stream == NGX_RTMP_RELAY_STREAM_HASH) {
        ngx_rtmp_auto_relay_hash_push(&ctx->push_evt);
    }

next:
    return next_publish(s, v);
}


static void
ngx_rtmp_auto_relay_hash_pull(ngx_rtmp_session_t *s)
{
    ngx_rtmp_auto_relay_conf_t     *apcf;
    ngx_rtmp_auto_relay_ctx_t      *ctx;
    ngx_rtmp_relay_target_t         at;
    u_char                          path[sizeof("unix:") + NGX_MAX_PATH];
    u_char                          flash_ver[sizeof("APSH ,") +
                                              NGX_INT_T_LEN * 2];
    u_char                          play_path[NGX_RTMP_MAX_NAME];
    ngx_str_t                       name;
    u_char                         *p;
    ngx_str_t                      *u;
    ngx_int_t                       h;
    ngx_pid_t                       pid;
    ngx_core_conf_t                *ccf;
    ngx_file_info_t                 fi;

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "auto_relay_hash_pull: connect");

    ccf = (ngx_core_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                           ngx_core_module);

    apcf = (ngx_rtmp_auto_relay_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                                       ngx_rtmp_auto_relay_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_auto_relay_index_module);
    if (ctx == NULL) {
        return;
    }

    name.data = ctx->name;
    name.len = ngx_strlen(name.data);

    h = ngx_hash_key(ctx->name, ngx_strlen(ctx->name)) % ccf->worker_processes;
    if (h == ngx_process_slot) {
        return;
    }

    pid = ngx_processes[h].pid;
    if (pid == 0 || pid == NGX_INVALID_PID) {
        return;
    }

    ngx_memzero(&at, sizeof(at));
    ngx_str_set(&at.page_url, "nginx-auto-hash-pull");
    at.tag = &ngx_rtmp_auto_relay_module;
    at.tc_url = s->tc_url;
    at.host = s->host;
    at.host_mask = s->host_mask & (NGX_RTMP_HOSTNAME_RTMP | NGX_RTMP_HOSTNAME_HTTP_FLV | NGX_RTMP_HOSTNAME_HLS);

    if (ctx->args[0]) {
        at.play_path.data = play_path;
        at.play_path.len = ngx_snprintf(play_path, sizeof(play_path),
                                        "%s?%s", ctx->name, ctx->args) -
                           play_path;
    }

    at.data = &ngx_processes[h];

    ngx_memzero(&at.url, sizeof(at.url));
    u = &at.url.url;
    p = ngx_snprintf(path, sizeof(path) - 1,
                     "unix:%V/" NGX_RTMP_AUTO_PUSH_SOCKNAME ".%i",
                     &apcf->auto_relay_socket_dir, h);
    *p = 0;

    if (ngx_file_info(path + sizeof("unix:") - 1, &fi) != NGX_OK) {
        ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "auto_relay_hash_pull: " ngx_file_info_n " failed: "
                       "slot=%i pid=%P socket='%s'" "url='%V' name='%s'",
                       h, pid, path, u, ctx->name);
        return;
    }

    u->data = path;
    u->len = p - path;
    if (ngx_parse_url(s->connection->pool, &at.url) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "auto_relay_hash_pull: auto-push parse_url failed "
                      "url='%V' name='%s'",
                      u, ctx->name);
        return;
    }

    p = ngx_snprintf(flash_ver, sizeof(flash_ver) - 1, "APSH %i,%i",
                     (ngx_int_t) ngx_process_slot, (ngx_int_t) ngx_pid);
    at.flash_ver.data = flash_ver;
    at.flash_ver.len = p - flash_ver;

    ngx_log_debug4(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "auto_relay_hash_pull: connect slot=%i pid=%P socket='%s' name='%s'",
                   h, pid, path, ctx->name);

    if (ngx_rtmp_relay_pull(s, &name, &at) == NGX_OK) {
        return;
    }

    ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                  "auto_relay_hash_pull: connect failed: slot=%i pid=%P socket='%s'"
                  "url='%V' name='%s'",
                  h, pid, path, u, ctx->name);
}


static ngx_int_t
ngx_rtmp_auto_relay_play(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v)
{
    ngx_rtmp_auto_relay_conf_t     *apcf;
    ngx_rtmp_auto_relay_ctx_t      *ctx;
    ngx_rtmp_live_stream_t        **stream;

    if (s->auto_relayed || (s->relay && !s->static_relay)) {
        goto next;
    }

    apcf = (ngx_rtmp_auto_relay_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                                       ngx_rtmp_auto_relay_module);
    if (apcf->relay_stream != NGX_RTMP_RELAY_STREAM_HASH) {
        goto next;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_auto_relay_index_module);
    if (ctx == NULL) {
        ctx = ngx_palloc(s->connection->pool,
                         sizeof(ngx_rtmp_auto_relay_ctx_t));
        if (ctx == NULL) {
            goto next;
        }
        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_auto_relay_index_module);

    }
    ngx_memzero(ctx, sizeof(*ctx));

    ngx_memcpy(ctx->name, v->name, sizeof(ctx->name));
    ngx_memcpy(ctx->args, v->args, sizeof(ctx->args));

    stream = ngx_rtmp_live_get_stream(s, ctx->name, 0);
    if (stream != NULL) {
        goto next;
    }

    ngx_rtmp_auto_relay_hash_pull(s);

next:
    return next_play(s, v);
}


static ngx_int_t
ngx_rtmp_auto_relay_delete_stream(ngx_rtmp_session_t *s,
    ngx_rtmp_delete_stream_t *v)
{
    ngx_rtmp_auto_relay_conf_t     *apcf;
    ngx_rtmp_auto_relay_ctx_t      *ctx, *pctx;
    ngx_rtmp_relay_ctx_t           *rctx;
    ngx_int_t                       slot;

    apcf = (ngx_rtmp_auto_relay_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                                       ngx_rtmp_auto_relay_module);
    if (apcf->relay_stream == NGX_RTMP_RELAY_STREAM_OFF) {
        goto next;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_auto_relay_index_module);
    if (ctx) {
        if (ctx->push_evt.timer_set) {
            ngx_del_timer(&ctx->push_evt);
        }
        goto next;
    }

    /* skip non-relays & publishers */
    rctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_relay_module);
    if (rctx == NULL ||
        rctx->tag != &ngx_rtmp_auto_relay_module ||
        rctx->publish == NULL)
    {
        goto next;
    }

    slot = (ngx_process_t *) rctx->data - &ngx_processes[0];

    ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "auto_relay: disconnect slot=%i app='%V' name='%V'",
                   slot, &rctx->app, &rctx->name);

    pctx = ngx_rtmp_get_module_ctx(rctx->publish->session,
                                   ngx_rtmp_auto_relay_index_module);
    if (pctx == NULL) {
        goto next;
    }

    pctx->slots[slot] = 0;

    /* push reconnect */
    if (!pctx->push_evt.timer_set) {
        ngx_add_timer(&pctx->push_evt, apcf->relay_reconnect_time);
    }

next:
    return next_delete_stream(s, v);
}
#endif /* NGX_HAVE_UNIX_DOMAIN */
