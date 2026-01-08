#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

// shared memory structure for health tracking
typedef struct {
    ngx_shmtx_sh_t  lock;
    time_t          last_failure;
} lazyfw_health_sh_t;

// module main configuration
typedef struct {
    ngx_str_t               engine;
    ngx_flag_t              fail_open;
    ngx_msec_t              timeout;
    ngx_uint_t              max_uri_len;
    ngx_uint_t              max_host_len;

    ngx_shm_zone_t         *health_zone;
    lazyfw_health_sh_t     *health;
    ngx_shmtx_t             health_mutex;   /* per-worker mutex handle */
} ngx_lazyfirewall_conf_t;

/* Per-worker persistent socket */
static int lazyfw_sock = -1;

#define LAZYFW_HEALTH_COOLDOWN  10   /* seconds */

// shm zone init function
static ngx_int_t
lazyfw_health_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    lazyfw_health_sh_t *sh = shm_zone->shm.addr;

    if (data) {
        lazyfw_health_sh_t *old = data;
        sh->last_failure = old->last_failure;
    } else {
        sh->last_failure = 0;
    }

    return NGX_OK;
}

// json escaping function
static u_char *
lazyfw_append_json_escaped(u_char *p, u_char *end, ngx_str_t *str)
{
    static const u_char hex[] = "0123456789abcdef";

    if (p + 2 + str->len * 6 > end) {
        return NULL;
    }

    *p++ = '"';

    u_char *src = str->data;
    u_char *src_end = src + str->len;
    while (src < src_end) {
        u_char ch = *src++;
        switch (ch) {
            case '"':  *p++ = '\\'; *p++ = '"';  break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b';  break;
            case '\f': *p++ = '\\'; *p++ = 'f';  break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:
                if (ch < 0x20) {
                    *p++ = '\\'; *p++ = 'u'; *p++ = '0'; *p++ = '0';
                    *p++ = hex[ch >> 4]; *p++ = hex[ch & 0xf];
                } else {
                    *p++ = ch;
                }
        }
    }

    *p++ = '"';
    return p;
}

// config create/merge post functions
static void *
ngx_lazyfirewall_create_conf(ngx_conf_t *cf)
{
    ngx_lazyfirewall_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(ngx_lazyfirewall_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->engine.len = 0;
    conf->fail_open = NGX_CONF_UNSET;
    conf->timeout = NGX_CONF_UNSET_MSEC;
    conf->max_uri_len = NGX_CONF_UNSET_UINT;
    conf->max_host_len = NGX_CONF_UNSET_UINT;
    conf->health_zone = NULL;
    conf->health = NULL;

    return conf;
}

static char *
ngx_lazyfirewall_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_lazyfirewall_conf_t *prev = parent;
    ngx_lazyfirewall_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->engine, prev->engine, "");
    ngx_conf_merge_value(conf->fail_open, prev->fail_open, 0);
    ngx_conf_merge_msec_value(conf->timeout, prev->timeout, 500);
    ngx_conf_merge_uint_value(conf->max_uri_len, prev->max_uri_len, 4096);
    ngx_conf_merge_uint_value(conf->max_host_len, prev->max_host_len, 255);

    if (conf->engine.len > 0) {
        if (conf->engine.len <= 5 || ngx_strncmp(conf->engine.data, (u_char *)"unix:", 5) != 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "lazyfirewall_engine must start with \"unix:\"");
            return NGX_CONF_ERROR;
        }
        size_t path_len = conf->engine.len - 5;
        if (path_len == 0 || path_len >= sizeof(((struct sockaddr_un *)0)->sun_path) - 1) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "lazyfirewall_engine path too long or empty");
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_lazyfirewall_postconfig(ngx_conf_t *cf)
{
    ngx_lazyfirewall_conf_t *conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_lazyfirewall_module);

    if (conf->engine.len == 0) {
        return NGX_OK;
    }

    ngx_str_t name = ngx_string("lazyfw_health");
    ngx_shm_zone_t *shm_zone = ngx_shared_memory_add(cf, &name, sizeof(lazyfw_health_sh_t),
                                                     &ngx_http_lazyfirewall_module);
    if (shm_zone == NULL) {
        return NGX_ERROR;
    }

    shm_zone->init = lazyfw_health_init_zone;
    shm_zone->data = NULL;

    conf->health_zone = shm_zone;

    return NGX_OK;
}

// process init/exit functions
static ngx_int_t
ngx_lazyfirewall_init_process(ngx_cycle_t *cycle)
{
    ngx_lazyfirewall_conf_t *conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_lazyfirewall_module);

    if (conf && conf->health_zone) {
        conf->health = conf->health_zone->shm.addr;

        if (ngx_shmtx_create(&conf->health_mutex, &conf->health->lock,
                             conf->health_zone->shm.name.data) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    lazyfw_sock = -1;

    return NGX_OK;
}

static void
ngx_lazyfirewall_exit_process(ngx_cycle_t *cycle)
{
    if (lazyfw_sock != -1) {
        close(lazyfw_sock);
        lazyfw_sock = -1;
    }
}

// directive definitions
static ngx_command_t ngx_lazyfirewall_commands[] = {
    { ngx_string("lazyfirewall_engine"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1, ngx_conf_set_str_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ngx_lazyfirewall_conf_t, engine), NULL },
    { ngx_string("lazyfirewall_fail_open"), NGX_HTTP_MAIN_CONF|NGX_CONF_FLAG, ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ngx_lazyfirewall_conf_t, fail_open), NULL },
    { ngx_string("lazyfirewall_timeout"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1, ngx_conf_set_msec_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ngx_lazyfirewall_conf_t, timeout), NULL },
    { ngx_string("lazyfirewall_max_uri_len"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1, ngx_conf_set_num_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ngx_lazyfirewall_conf_t, max_uri_len), NULL },
    { ngx_string("lazyfirewall_max_host_len"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1, ngx_conf_set_num_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ngx_lazyfirewall_conf_t, max_host_len), NULL },
    ngx_null_command
};

// mark failure in shared memory
static void
lazyfw_mark_failure(ngx_lazyfirewall_conf_t *conf, ngx_log_t *log)
{
    if (conf->health == NULL) return;

    if (ngx_shmtx_lock(&conf->health_mutex)) {
        conf->health->last_failure = ngx_time();
        ngx_shmtx_unlock(&conf->health_mutex);
    } else {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "lazyfirewall: failed to acquire health lock (cannot mark failure)");
    }
}


// access handler
static ngx_int_t
ngx_lazyfirewall_handler(ngx_http_request_t *r)
{
    ngx_lazyfirewall_conf_t *conf = ngx_http_get_module_main_conf(r, ngx_http_lazyfirewall_module);

    if (r->connection->error) {
        if (lazyfw_sock != -1) {
            close(lazyfw_sock);
            lazyfw_sock = -1;
        }
        return NGX_DECLINED;
    }

    if (conf->engine.len == 0) {
        return NGX_DECLINED;
    }

    /* Early oversized reject */
    if (r->unparsed_uri.len > conf->max_uri_len ||
        r->connection->addr_text.len > 64) {
        return NGX_HTTP_REQUEST_URI_TOO_LONG;
    }

    ngx_str_t host = ngx_null_string;
    if (r->headers_in.host) {
        host = r->headers_in.host->value;
        if (host.len > conf->max_host_len) {
            return NGX_HTTP_BAD_REQUEST;
        }
    }

    /* Cooldown check - conservative on lock failure */
    ngx_uint_t cooldown_active = 0;
    if (conf->health) {
        if (ngx_shmtx_lock(&conf->health_mutex)) {
            time_t now = ngx_time();
            if (conf->health->last_failure > 0 && (now - conf->health->last_failure) < LAZYFW_HEALTH_COOLDOWN) {
                cooldown_active = 1;
            }
            ngx_shmtx_unlock(&conf->health_mutex);
        } else {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                          "lazyfirewall: failed to acquire health lock");
            cooldown_active = 1;  /* conservative: assume unhealthy */
        }
    }

    if (cooldown_active) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0, "lazyfirewall: backend unhealthy, skipping");
        return conf->fail_open ? NGX_DECLINED : NGX_HTTP_FORBIDDEN;
    }

    /* Persistent per-worker socket */
    if (lazyfw_sock == -1) {
        lazyfw_sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (lazyfw_sock == -1) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_socket_errno, "lazyfirewall: socket() failed");
            lazyfw_mark_failure(conf, r->connection->log);
            goto failed;
        }

        struct timeval tv;
        tv.tv_sec = conf->timeout / 1000;
        tv.tv_usec = (conf->timeout % 1000) * 1000;
        setsockopt(lazyfw_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(lazyfw_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        size_t path_len = conf->engine.len - 5;
        ngx_memcpy(addr.sun_path, conf->engine.data + 5, path_len);
        addr.sun_path[path_len] = '\0';

        if (connect(lazyfw_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_socket_errno, "lazyfirewall: connect failed");
            close(lazyfw_sock);
            lazyfw_sock = -1;
            lazyfw_mark_failure(conf, r->connection->log);
            goto failed;
        }
    }

    /* Build JSON */
    u_char json_buf[8192];
    u_char *p = json_buf;
    u_char *end_p = json_buf + sizeof(json_buf) - 1;

    p = ngx_cpymem(p, "{\"ip\":", 6);
    p = lazyfw_append_json_escaped(p, end_p, &r->connection->addr_text);
    if (p == NULL) goto json_overflow;

    p = ngx_cpymem(p, ",\"host\":", 9);
    p = lazyfw_append_json_escaped(p, end_p, &host);
    if (p == NULL) goto json_overflow;

    p = ngx_cpymem(p, ",\"method\":", 10);
    p = lazyfw_append_json_escaped(p, end_p, &r->method_name);
    if (p == NULL) goto json_overflow;

    p = ngx_cpymem(p, ",\"uri\":", 8);
    p = lazyfw_append_json_escaped(p, end_p, &r->unparsed_uri);
    if (p == NULL) goto json_overflow;

    p = ngx_cpymem(p, "}\n", 2);

    size_t json_len = p - json_buf;

    if (write(lazyfw_sock, json_buf, json_len) != (ssize_t)json_len) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_socket_errno, "lazyfirewall: write failed");
        close(lazyfw_sock);
        lazyfw_sock = -1;
        lazyfw_mark_failure(conf, r->connection->log);
        goto failed;
    }

    char resp_buf[512];
    ssize_t n = read(lazyfw_sock, resp_buf, sizeof(resp_buf) - 1);
    if (n <= 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, n == 0 ? 0 : ngx_socket_errno, "lazyfirewall: read failed");
        close(lazyfw_sock);
        lazyfw_sock = -1;
        lazyfw_mark_failure(conf, r->connection->log);
        goto failed;
    }

    resp_buf[n] = '\0';

    /* Trim and strict validate */
    while (n > 0 && ngx_isspace(resp_buf[n-1])) n--;
    resp_buf[n] = '\0';

    if (n == 5 && ngx_strncasecmp((u_char *)resp_buf, (u_char *)"block", 5) == 0) {
        return NGX_HTTP_FORBIDDEN;
    }

    return NGX_DECLINED;

json_overflow:
    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "lazyfirewall: JSON buffer overflow (oversized input)");
failed:
    return conf->fail_open ? NGX_DECLINED : NGX_HTTP_FORBIDDEN;
}

// phase init function
static ngx_int_t
ngx_lazyfirewall_init(ngx_conf_t *cf)
{
    ngx_http_core_main_conf_t *cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    ngx_http_handler_pt *h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_lazyfirewall_handler;
    return NGX_OK;
}

// module context and definition
static ngx_http_module_t ngx_lazyfirewall_module_ctx = {
    NULL,                                /* preconfiguration */
    ngx_lazyfirewall_init,               /* postconfiguration */
    ngx_lazyfirewall_create_conf,        /* create main conf */
    ngx_lazyfirewall_merge_conf,         /* merge main conf */
    NULL, NULL, NULL, NULL
};

ngx_module_t ngx_http_lazyfirewall_module = {
    NGX_MODULE_V1,
    &ngx_lazyfirewall_module_ctx,
    ngx_lazyfirewall_commands,
    NGX_HTTP_MODULE,
    NULL,                                /* init master */
    NULL,                                /* init module */
    ngx_lazyfirewall_init_process,       /* init process */
    NULL,                                /* init thread */
    NULL,                                /* exit thread */
    ngx_lazyfirewall_exit_process,       /* exit process */
    NULL,                                /* exit master */
    NGX_MODULE_V1_PADDING
};