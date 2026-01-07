#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <sys/socket.h> 
#include <sys/un.h>
#include <unistd.h>

static ngx_int_t lazyfirewall_call_engine(ngx_http_request_t *r)
{
    int sock;
    struct sockaddr_un addr;
    char buffer[512];

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        return NGX_DECLINED;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/lazyfirewall.sock");

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(sock);
        return NGX_DECLINED;
    }

    // Build JSON request
    int len = snprintf(buffer, sizeof(buffer),
        "{\"ip\":\"%.*s\",\"method\":\"%.*s\",\"uri\":\"%.*s\"}\n",
        (int)r->connection->addr_text.len, r->connection->addr_text.data,
        (int)r->method_name.len, r->method_name.data,
        (int)r->uri.len, r->uri.data
    );

    write(sock, buffer, len);

    // Read response
    int n = read(sock, buffer, sizeof(buffer) - 1);
    close(sock);

    if (n <= 0) {
        return NGX_DECLINED;
    }

    buffer[n] = 0;

    if (ngx_strstr(buffer, "BLOCK")) {
        return NGX_HTTP_FORBIDDEN;
    }

    return NGX_DECLINED;
}
static ngx_int_t ngx_lazyfirewall_handler(ngx_http_request_t *r) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "[lazyfirewall] ip=%V method=%V uri=%V",
                &r->connection->addr_text,
                &r->method_name,
                &r->uri);
    return NGX_DECLINED;
}

static ngx_int_t ngx_lazyfirewall_init(ngx_conf_t *cf) {
    ngx_http_handler_pt *h;
    ngx_http_core_main_conf_t *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_lazyfirewall_handler;
    return NGX_OK;
}

static ngx_http_module_t ngx_lazyfirewall_module_ctx = {
    NULL,                    /* preconfiguration */
    ngx_lazyfirewall_init,   /* postconfiguration */

    NULL, NULL,              /* main conf */
    NULL, NULL,              /* server conf */
    NULL, NULL               /* location conf */
};

ngx_module_t ngx_http_lazyfirewall_module = {
    NGX_MODULE_V1,
    &ngx_lazyfirewall_module_ctx,
    NULL,
    NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};
