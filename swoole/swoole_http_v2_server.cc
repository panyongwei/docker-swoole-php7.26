/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole.h"

#ifdef SW_USE_HTTP2
#include "swoole_http.h"
#include "swoole_coroutine.h"

#include "http2.h"
#include <main/php_variables.h>
#include <unordered_map>

namespace swoole
{
    class http2_stream
    {
        public:
        http_context* ctx;
        // uint8_t priority; // useless now
        uint32_t stream_id;
        // flow control
        uint32_t send_window;
        uint32_t recv_window;

        http2_stream(int _fd, uint32_t _stream_id)
        {
            ctx = swoole_http_context_new(_fd);
            ctx->stream = (void *) this;
            stream_id = _stream_id;
            send_window = SW_HTTP2_DEFAULT_WINDOW_SIZE;
            recv_window = SW_HTTP2_DEFAULT_WINDOW_SIZE;
        }

        ~http2_stream()
        {
            swoole_http_context_free(ctx);
        }
    };

    class http2_session
    {
        public:
        int fd;
        std::unordered_map<int, http2_stream*> streams;

        nghttp2_hd_inflater *inflater;
        nghttp2_hd_deflater *deflater;

        // flow control
        uint32_t send_window;
        uint32_t recv_window;
        uint32_t max_concurrent_streams;
        uint32_t max_frame_size;

        http2_session(int _fd)
        {
            fd = _fd;
            send_window = SW_HTTP2_DEFAULT_WINDOW_SIZE;
            recv_window = SW_HTTP2_DEFAULT_WINDOW_SIZE;
            max_concurrent_streams = SW_HTTP2_MAX_CONCURRENT_STREAMS;
            max_frame_size = SW_HTTP2_MAX_FRAME_SIZE;
            deflater = nullptr;
            inflater = nullptr;
        }

        ~http2_session()
        {
            if (inflater)
            {
                nghttp2_hd_inflate_del(inflater);
            }
            if (deflater)
            {
                nghttp2_hd_deflate_del(deflater);
            }

            for(std::unordered_map<int, http2_stream*>::iterator iter = streams.begin(); iter != streams.end(); iter++)
            {
                delete iter->second;
            }
        }
    };
}

using namespace swoole;

static std::unordered_map<int, http2_session*> http2_sessions;

static sw_inline void http2_add_header(nghttp2_nv *headers, const char *k, int kl, const char *v, int vl)
{
    headers->name = (uchar*) k;
    headers->namelen = kl;
    headers->value = (uchar*) v;
    headers->valuelen = vl;
}

static int http_build_trailer(http_context *ctx, uchar *buffer)
{
    int ret;
    nghttp2_nv nv[128];
    size_t index = 0;

    zval *trailer = sw_zend_read_property(swoole_http_response_class_entry_ptr, ctx->response.zobject, ZEND_STRL("trailer"), 1);
    if (ZVAL_IS_ARRAY(trailer))
    {
        HashTable *ht = Z_ARRVAL_P(trailer);
        zval *value = NULL;
        char *key = NULL;
        uint32_t keylen = 0;
        int type;
        SW_HASHTABLE_FOREACH_START2(ht, key, keylen, type, value)
        {
            if (!key)
            {
                break;
            }
            http2_add_header(&nv[index++], key, keylen, Z_STRVAL_P(value), Z_STRLEN_P(value));
            (void) type;
        }
        SW_HASHTABLE_FOREACH_END();
    }

    ssize_t rv;
    size_t buflen;
    size_t i;
    size_t sum = 0;
    http2_session *client = http2_sessions[ctx->fd];
    nghttp2_hd_deflater *deflater = client->deflater;

    if (!deflater)
    {
        ret = nghttp2_hd_deflate_new(&deflater, 4096);
        if (ret != 0)
        {
            swoole_php_error(E_WARNING, "nghttp2_hd_deflate_init failed with error: %s\n", nghttp2_strerror(ret));
            return SW_ERR;
        }
        client->deflater = deflater;
    }

    for (i = 0; i < index; ++i)
    {
        sum += nv[i].namelen + nv[i].valuelen;
    }

    buflen = nghttp2_hd_deflate_bound(deflater, nv, index);
    rv = nghttp2_hd_deflate_hd(deflater, (uchar *) buffer, buflen, nv, index);
    if (rv < 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_deflate_hd() failed with error: %s\n", nghttp2_strerror((int ) rv));
        return SW_ERR;
    }

    ret = nghttp2_hd_deflate_change_table_size(deflater, 4096);
    if (ret != 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_deflate_change_table_size failed with error: %s\n", nghttp2_strerror(ret));
        return SW_ERR;
    }

    return rv;
}

static sw_inline void http2_onRequest(http_context *ctx, int from_fd)
{
    zval *retval = NULL;
    swServer *serv = SwooleG.serv;
    int fd = ctx->fd;
    zval *zrequest_object = ctx->request.zobject;
    zval *zresponse_object = ctx->response.zobject;
    SW_SEPARATE_ZVAL(zrequest_object);
    SW_SEPARATE_ZVAL(zresponse_object);

    if (SwooleG.enable_coroutine)
    {
        zval *args[2];
        args[0] = zrequest_object;
        args[1] = zresponse_object;

        zend_fcall_info_cache *cache = php_swoole_server_get_cache(serv, from_fd, SW_SERVER_CB_onRequest);
        int ret = coro_create(cache, args, 2, &retval, NULL, NULL);
        if (ret < 0)
        {
            if (ret == CORO_LIMIT)
            {
                serv->factory.end(&SwooleG.serv->factory, fd);
            }
            goto _free_object;
        }
    }
    else
    {
        zval args[2];
        args[0] = *zrequest_object;
        args[1] = *zresponse_object;

        zval *zcallback = php_swoole_server_get_callback(serv, from_fd, SW_SERVER_CB_onRequest);
        zend_fcall_info_cache *fci_cache = php_swoole_server_get_cache(serv, from_fd, SW_SERVER_CB_onRequest);
        if (sw_call_user_function_fast_ex(zcallback, fci_cache, &retval, 2, args) == FAILURE)
        {
            swoole_php_error(E_WARNING, "Http2 onRequest handler error");
        }
    }

    if (EG(exception))
    {
        zend_exception_error(EG(exception), E_ERROR);
    }

    _free_object:
    zval_ptr_dtor(zrequest_object);
    zval_ptr_dtor(zresponse_object);
    if (retval)
    {
        zval_ptr_dtor(retval);
    }
}

static int http2_build_header(http_context *ctx, uchar *buffer, int body_length)
{
    assert(ctx->send_header == 0);

    swServer *serv = SwooleG.serv;

    char *date_str = NULL;
    char intbuf[2][16];

    int ret;


    size_t index = 0;

    nghttp2_nv nv[128];

    /**
     * http status code
     */
    if (ctx->response.status == 0)
    {
        ctx->response.status = 200;
    }

    ret = swoole_itoa(intbuf[0], ctx->response.status);
    http2_add_header(&nv[index++], ZEND_STRL(":status"), intbuf[0], ret);

    /**
     * http header
     */
    zval *zheader = sw_zend_read_property(swoole_http_response_class_entry_ptr, ctx->response.zobject, ZEND_STRL("header"), 1);
    if (ZVAL_IS_ARRAY(zheader))
    {
        uint32_t header_flag = 0x0;

        HashTable *ht = Z_ARRVAL_P(zheader);
        zval *value = NULL;
        char *key = NULL;
        uint32_t keylen = 0;
        int type;

        SW_HASHTABLE_FOREACH_START2(ht, key, keylen, type, value)
        {
            if (!key)
            {
                break;
            }
            if (strncmp(key, "server", keylen) == 0)
            {
                header_flag |= HTTP_HEADER_SERVER;
            }
            else if (strncmp(key, "content-length", keylen) == 0)
            {
                header_flag |= HTTP_HEADER_CONTENT_LENGTH;
            }
            else if (strncmp(key, "date", keylen) == 0)
            {
                header_flag |= HTTP_HEADER_DATE;
            }
            else if (strncmp(key, "content-type", keylen) == 0)
            {
                header_flag |= HTTP_HEADER_CONTENT_TYPE;
            }
            http2_add_header(&nv[index++], key, keylen, Z_STRVAL_P(value), Z_STRLEN_P(value));
        }
        SW_HASHTABLE_FOREACH_END();
        (void)type;

        if (!(header_flag & HTTP_HEADER_SERVER))
        {
            http2_add_header(&nv[index++], ZEND_STRL("server"), ZEND_STRL(SW_HTTP_SERVER_SOFTWARE));
        }
        if (!(header_flag & HTTP_HEADER_CONTENT_LENGTH) && body_length >= 0)
        {
#ifdef SW_HAVE_ZLIB
            if (ctx->enable_compression)
            {
                body_length = swoole_zlib_buffer->length;
            }
#endif
            ret = swoole_itoa(intbuf[1], body_length);
            http2_add_header(&nv[index++], ZEND_STRL("content-length"), intbuf[1], ret);
        }
        if (!(header_flag & HTTP_HEADER_DATE))
        {
            date_str = sw_php_format_date((char *)ZEND_STRL(SW_HTTP_DATE_FORMAT), serv->gs->now, 0);
            http2_add_header(&nv[index++], ZEND_STRL("date"), date_str, strlen(date_str));
        }
        if (!(header_flag & HTTP_HEADER_CONTENT_TYPE))
        {
            http2_add_header(&nv[index++], ZEND_STRL("content-type"), ZEND_STRL("text/html"));
        }
    }
    else
    {
        http2_add_header(&nv[index++], ZEND_STRL("server"), ZEND_STRL(SW_HTTP_SERVER_SOFTWARE));
        http2_add_header(&nv[index++], ZEND_STRL("content-type"), ZEND_STRL("text/html"));

        date_str = sw_php_format_date((char *)ZEND_STRL(SW_HTTP_DATE_FORMAT), serv->gs->now, 0);
        http2_add_header(&nv[index++], ZEND_STRL("date"), date_str, strlen(date_str));

#ifdef SW_HAVE_ZLIB
        if (ctx->enable_compression)
        {
            body_length = swoole_zlib_buffer->length;
        }
#endif
        ret = swoole_itoa(intbuf[1], body_length);
        http2_add_header(&nv[index++], ZEND_STRL("content-length"), intbuf[1], ret);
    }
    //http cookies
    zval *zcookie = sw_zend_read_property(swoole_http_response_class_entry_ptr, ctx->response.zobject, ZEND_STRL("cookie"), 1);
    if (ZVAL_IS_ARRAY(zcookie))
    {
        zval *value;
        SW_HASHTABLE_FOREACH_START(Z_ARRVAL_P(zcookie), value)
        {
            if (Z_TYPE_P(value) != IS_STRING)
            {
                continue;
            }
            http2_add_header(&nv[index++], ZEND_STRL("set-cookie"), Z_STRVAL_P(value), Z_STRLEN_P(value));
        }
        SW_HASHTABLE_FOREACH_END();
    }
#ifdef SW_HAVE_ZLIB
    if (ctx->enable_compression)
    {
        const char *content_encoding = swoole_http_get_content_encoding(ctx);
        http2_add_header(&nv[index++], ZEND_STRL("content-encoding"), (char *) content_encoding, strlen(content_encoding));
    }
#endif
    ctx->send_header = 1;

    ssize_t rv;
    size_t buflen;
    size_t i;
    size_t sum = 0;

    nghttp2_hd_deflater *deflater;
    ret = nghttp2_hd_deflate_new(&deflater, 4096);
    if (ret != 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_deflate_init failed with error: %s\n", nghttp2_strerror(ret));
        return SW_ERR;
    }

    for (i = 0; i < index; ++i)
    {
        sum += nv[i].namelen + nv[i].valuelen;
    }

    buflen = nghttp2_hd_deflate_bound(deflater, nv, index);
    rv = nghttp2_hd_deflate_hd(deflater, (uchar *) buffer, buflen, nv, index);
    if (rv < 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_deflate_hd() failed with error: %s\n", nghttp2_strerror((int ) rv));
        return SW_ERR;
    }

    if (date_str)
    {
        efree(date_str);
    }

    nghttp2_hd_deflate_del(deflater);

    return rv;
}

int swoole_http2_do_response(http_context *ctx, swString *body)
{
    http2_session *client = http2_sessions[ctx->fd];
    http2_stream *stream = (http2_stream *) ctx->stream;
    char header_buffer[SW_BUFFER_SIZE_STD];
    int ret;

#ifdef SW_HAVE_ZLIB
    if (ctx->enable_compression)
    {
        if (body->length > 0)
        {
            swoole_http_response_compress(body, ctx->compression_method, ctx->compression_level);
        }
        else
        {
            ctx->enable_compression = 0;
        }
    }
#endif

    ret = http2_build_header(ctx, (uchar *) header_buffer, body->length);
    swString_clear(swoole_http_buffer);

    /**
     +---------------+
     |Pad Length? (8)|
     +-+-------------+-----------------------------------------------+
     |E|                 Stream Dependency? (31)                     |
     +-+-------------+-----------------------------------------------+
     |  Weight? (8)  |
     +-+-------------+-----------------------------------------------+
     |                   Header Block Fragment (*)                 ...
     +---------------------------------------------------------------+
     |                           Padding (*)                       ...
     +---------------------------------------------------------------+
     */
    char frame_header[SW_HTTP2_FRAME_HEADER_SIZE];
    zval *trailer = sw_zend_read_property(swoole_http_response_class_entry_ptr, ctx->response.zobject, ZEND_STRL("trailer"), 1);

    if (!ZVAL_IS_ARRAY(trailer) && body->length == 0)
    {
        swHttp2_set_frame_header(frame_header, SW_HTTP2_TYPE_HEADERS, ret, SW_HTTP2_FLAG_END_HEADERS | SW_HTTP2_FLAG_END_STREAM, stream->stream_id);
    }
    else
    {
        swHttp2_set_frame_header(frame_header, SW_HTTP2_TYPE_HEADERS, ret, SW_HTTP2_FLAG_END_HEADERS, stream->stream_id);
    }

    swString_append_ptr(swoole_http_buffer, frame_header, SW_HTTP2_FRAME_HEADER_SIZE);
    swString_append_ptr(swoole_http_buffer, header_buffer, ret);

    int flag = SW_HTTP2_FLAG_END_STREAM;
    if (ZVAL_IS_ARRAY(trailer))
    {
        flag = SW_HTTP2_FLAG_NONE;
    }

    ret = swServer_tcp_send(SwooleG.serv, ctx->fd, swoole_http_buffer->str, swoole_http_buffer->length);
    if (ret < 0)
    {
        ctx->send_header = 0;
        return SW_ERR;
    }

    ctx->send_header = 1;
    if (!ZVAL_IS_ARRAY(trailer) && body->length == 0)
    {
        goto _end;
    }

    char *p;
    size_t l;
    size_t send_n;

#ifdef SW_HAVE_ZLIB
    if (ctx->enable_compression)
    {
        p = swoole_zlib_buffer->str;
        l = swoole_zlib_buffer->length;
    }
    else
#endif
    {
        p = body->str;
        l = body->length;
    }

    while (l > 0)
    {
        int _send_flag;
        swString_clear(swoole_http_buffer);
        if (l > client->max_frame_size)
        {
            send_n = client->max_frame_size;
            _send_flag = 0;
        }
        else
        {
            send_n = l;
            _send_flag = flag;
        }
        swHttp2_set_frame_header(frame_header, SW_HTTP2_TYPE_DATA, send_n, _send_flag, stream->stream_id);
        swString_append_ptr(swoole_http_buffer, frame_header, SW_HTTP2_FRAME_HEADER_SIZE);
        swString_append_ptr(swoole_http_buffer, p, send_n);

        if (swServer_tcp_send(SwooleG.serv, ctx->fd, swoole_http_buffer->str, swoole_http_buffer->length) < 0)
        {
            return SW_ERR;
        }
        else
        {
            l -= send_n;
            p += send_n;
        }
    }

    if (trailer)
    {
        swString_clear(swoole_http_buffer);
        memset(header_buffer, 0, sizeof(header_buffer));
        ret = http_build_trailer(ctx, (uchar *) header_buffer);
        swHttp2_set_frame_header(frame_header, SW_HTTP2_TYPE_HEADERS, ret, SW_HTTP2_FLAG_END_HEADERS | SW_HTTP2_FLAG_END_STREAM, stream->stream_id);
        swString_append_ptr(swoole_http_buffer, frame_header, SW_HTTP2_FRAME_HEADER_SIZE);
        swString_append_ptr(swoole_http_buffer, header_buffer, ret);

        if (swServer_tcp_send(SwooleG.serv, ctx->fd, swoole_http_buffer->str, swoole_http_buffer->length) < 0)
        {
            return SW_ERR;
        }
    }

    _end:
    if (body->length > 0)
    {
        client->send_window -= body->length;    // TODO:flow control?
    }

    client->streams.erase(stream->stream_id);
    delete stream;

    return SW_OK;
}

static int http2_parse_header(http2_session *client, http_context *ctx, int flags, char *in, size_t inlen)
{
    nghttp2_hd_inflater *inflater = client->inflater;

    if (!inflater)
    {
        int ret = nghttp2_hd_inflate_new(&inflater);
        if (ret != 0)
        {
            swoole_php_error(E_WARNING, "nghttp2_hd_inflate_init() failed, Error: %s[%d].", nghttp2_strerror(ret), ret);
            return SW_ERR;
        }
        client->inflater = inflater;
    }

    if (flags & SW_HTTP2_FLAG_PRIORITY)
    {
        //int stream_deps = ntohl(*(int *) (in));
        //uint8_t weight = in[4];
        in += 5;
        inlen -= 5;
    }

    zval *zheader = ctx->request.zheader;
    zval *zserver = ctx->request.zserver;

    ssize_t rv;
    for (;;)
    {
        nghttp2_nv nv;
        int inflate_flags = 0;
        size_t proclen;

        rv = nghttp2_hd_inflate_hd(inflater, &nv, &inflate_flags, (uchar *) in, inlen, 1);
        if (rv < 0)
        {
            swoole_php_error(E_WARNING, "inflate failed, Error: %s[%zd].", nghttp2_strerror(rv), rv);
            return -1;
        }

        proclen = (size_t) rv;

        in += proclen;
        inlen -= proclen;

        if (inflate_flags & NGHTTP2_HD_INFLATE_EMIT)
        {
            swTraceLog(SW_TRACE_HTTP2, "Header: " SW_ECHO_BLUE "[%zu]: %s[%zu]", nv.name, nv.namelen, nv.value, nv.valuelen);

            if (nv.name[0] == ':')
            {
                if (strncasecmp((char *) nv.name + 1, "method", nv.namelen -1) == 0)
                {
                    add_assoc_stringl_ex(zserver, ZEND_STRL("request_method"), (char *) nv.value, nv.valuelen);
                }
                else if (strncasecmp((char *) nv.name + 1, "path", nv.namelen -1) == 0)
                {
                    char *pathbuf = SwooleTG.buffer_stack->str;
                    char *v_str = strchr((char *) nv.value, '?');
                    if (v_str)
                    {
                        v_str++;
                        int k_len = v_str - (char *) nv.value - 1;
                        int v_len = nv.valuelen - k_len - 1;
                        memcpy(pathbuf, nv.value, k_len);
                        pathbuf[k_len] = 0;
                        add_assoc_stringl_ex(zserver, ZEND_STRL("query_string"), v_str, v_len);
                        add_assoc_stringl_ex(zserver, ZEND_STRL("request_uri"), pathbuf, k_len);

                        zval *zget;
                        zval *zrequest_object = ctx->request.zobject;
                        swoole_http_server_array_init(get, request);

                        //no need free, will free by treat_data
                        char *query = estrndup(v_str, v_len);
                        //parse url params
                        sapi_module.treat_data(PARSE_STRING, query, zget);
                    }
                    else
                    {
                        add_assoc_stringl_ex(zserver, ZEND_STRL("request_uri"), (char *) nv.value, nv.valuelen);
                    }
                }
                else if (strncasecmp((char *) nv.name + 1, "authority", nv.namelen -1) == 0)
                {
                    add_assoc_stringl_ex(zheader, ZEND_STRL("host"), (char * ) nv.value, nv.valuelen);
                }
            }
            else
            {
                if (strncasecmp((char *) nv.name, "content-type", nv.namelen) == 0)
                {
                    if (http_strncasecmp("application/x-www-form-urlencoded", (char *) nv.value, nv.valuelen))
                    {
                        ctx->request.post_form_urlencoded = 1;
                    }
                    else if (http_strncasecmp("multipart/form-data", (char *) nv.value, nv.valuelen))
                    {
                        int boundary_len = nv.valuelen - strlen("multipart/form-data; boundary=");
                        if (boundary_len <= 0)
                        {
                            swWarn("invalid multipart/form-data body fd:%d.", ctx->fd);
                            return 0;
                        }
                        swoole_http_parse_form_data(ctx, (char*) nv.value + nv.valuelen - boundary_len, boundary_len);
                        ctx->parser.data = ctx;
                    }
                }
                else if (strncasecmp((char *) nv.name, "cookie", nv.namelen) == 0)
                {
                    zval *zcookie = ctx->request.zcookie;
                    zval *zrequest_object = ctx->request.zobject;
                    if (!zcookie)
                    {
                        swoole_http_server_array_init(cookie, request);
                    }

                    char keybuf[SW_HTTP_COOKIE_KEYLEN];
                    char *v_str = strchr((char *) nv.value, '=') + 1;
                    int k_len = v_str - (char *) nv.value - 1;
                    int v_len = nv.valuelen - k_len - 1;
                    memcpy(keybuf, nv.value, k_len);
                    keybuf[k_len] = 0;
                    add_assoc_stringl_ex(zcookie, keybuf, k_len, v_str, v_len);
                    continue;
                }
#ifdef SW_HAVE_ZLIB
                else if (SwooleG.serv->http_compression && strncasecmp((char *) nv.name, "accept-encoding", nv.namelen) == 0)
                {
                    swoole_http_get_compression_method(ctx, (char *) nv.value, nv.valuelen);
                }
#endif
                add_assoc_stringl_ex(zheader, (char *) nv.name, nv.namelen, (char *) nv.value, nv.valuelen);
            }
        }

        if (inflate_flags & NGHTTP2_HD_INFLATE_FINAL)
        {
            nghttp2_hd_inflate_end_headers(inflater);
            break;
        }

        if ((inflate_flags & NGHTTP2_HD_INFLATE_EMIT) == 0 && inlen == 0)
        {
            break;
        }
    }

    rv = nghttp2_hd_inflate_change_table_size(inflater, 4096);
    if (rv != 0)
    {
        return rv;
    }
    return SW_OK;
}

static sw_inline void http2_server_send_window_update(int fd, int stream_id, uint32_t size)
{
    char frame[SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_WINDOW_UPDATE_SIZE];
    swTraceLog(SW_TRACE_HTTP2, "send [" SW_ECHO_YELLOW "] stream_id=%d, size=%d", "WINDOW_UPDATE", stream_id, size);
    *(uint32_t*) ((char *)frame + SW_HTTP2_FRAME_HEADER_SIZE) = htonl(size);
    swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_WINDOW_UPDATE, SW_HTTP2_WINDOW_UPDATE_SIZE, 0, stream_id);
    swServer_tcp_send(SwooleG.serv, fd, frame, SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_WINDOW_UPDATE_SIZE);
}

/**
 * Http2
 */
int swoole_http2_onFrame(swConnection *conn, swEventData *req)
{
    swServer *serv = SwooleG.serv;
    int fd = req->info.fd;
    int from_fd = req->info.from_fd;

    http2_session *client = http2_sessions[conn->session_id];
    if (client == nullptr)
    {
        client = new http2_session(fd);
        http2_sessions[conn->session_id] = client;
    }

    http2_stream *stream = nullptr;
    http_context *ctx = nullptr;
    zval *zrequest_object = nullptr;
    zval *zdata;
    SW_MAKE_STD_ZVAL(zdata);
    php_swoole_get_recv_data(zdata, req, NULL, 0);

    char *buf = Z_STRVAL_P(zdata);
    int type = buf[3];
    int flags = buf[4];
    uint32_t stream_id = ntohl((*(int *) (buf + 5))) & 0x7fffffff;
    ssize_t length = swHttp2_get_length(buf);
    buf += SW_HTTP2_FRAME_HEADER_SIZE;

    uint16_t id = 0;
    uint32_t value = 0;

    switch (type)
    {
    case SW_HTTP2_TYPE_SETTINGS:
    {
        if (flags & SW_HTTP2_FLAG_ACK)
        {
            swHttp2FrameTraceLog(recv, "ACK");
            break;
        }

        while(length > 0)
        {
            id = ntohs(*(uint16_t *) (buf));
            value = ntohl(*(uint32_t *) (buf + sizeof(uint16_t)));
            swHttp2FrameTraceLog(recv, "id=%d, value=%d", id, value);
            switch (id)
            {
            case SW_HTTP2_SETTING_HEADER_TABLE_SIZE:
                swTraceLog(SW_TRACE_HTTP2, "setting: header_compression_table_max=%u.", value);
                break;
            case SW_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS:
                client->max_concurrent_streams = value;
                swTraceLog(SW_TRACE_HTTP2, "setting: max_concurrent_streams=%u.", value);
                break;
            case SW_HTTP2_SETTINGS_INIT_WINDOW_SIZE:
                client->send_window = value;
                swTraceLog(SW_TRACE_HTTP2, "setting: init_send_window=%u.", value);
                break;
            case SW_HTTP2_SETTINGS_MAX_FRAME_SIZE:
                client->max_frame_size = value;
                swTraceLog(SW_TRACE_HTTP2, "setting: max_frame_size=%u.", value);
                break;
            case SW_HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE:
                // client->max_header_list_size = value; // useless now
                swTraceLog(SW_TRACE_HTTP2, "setting: max_header_list_size=%u.", value);
                break;
            default:
                // disable warning and ignore it because some websites are not following http2 protocol totally
                // swWarn("unknown option[%d]: %d.", id, value);
                break;
            }
            buf += sizeof(id) + sizeof(value);
            length -= sizeof(id) + sizeof(value);
        }
        break;
    }
    case SW_HTTP2_TYPE_HEADERS:
    {
        stream = client->streams[stream_id];
        swHttp2FrameTraceLog(recv, "%s", (stream ? "exist stream" : "new stream"));
        if (!stream)
        {
            stream = new http2_stream(fd, stream_id);
            if (unlikely(!stream->ctx))
            {
                zval_ptr_dtor(zdata);
                swoole_error_log(SW_LOG_WARNING, SW_ERROR_HTTP2_STREAM_NO_HEADER, "http2 create stream#%d context error.", stream_id);
                return SW_ERR;
            }
            client->streams[stream_id] = stream;
            ctx = stream->ctx;

            zrequest_object = ctx->request.zobject;
            zend_update_property_long(Z_OBJCE_P(zrequest_object), zrequest_object, ZEND_STRL("streamId"), stream_id);

            zval *zserver = ctx->request.zserver;
            add_assoc_long(zserver, "request_time", serv->gs->now);
            // Add REQUEST_TIME_FLOAT
            double now_float = swoole_microtime();
            add_assoc_double(zserver, "request_time_float", now_float);
            add_assoc_long(zserver, "server_port", swConnection_get_port(&SwooleG.serv->connection_list[conn->from_fd]));
            add_assoc_long(zserver, "remote_port", swConnection_get_port(conn));
            add_assoc_string(zserver, "remote_addr", swConnection_get_ip(conn));
            add_assoc_string(zserver, "server_protocol", (char *) "HTTP/2");
            add_assoc_string(zserver, "server_software", (char *) SW_HTTP_SERVER_SOFTWARE);
        }
        else
        {
            ctx = stream->ctx;
        }

        http2_parse_header(client, ctx, flags, buf, length);

        if (flags & SW_HTTP2_FLAG_END_STREAM)
        {
            http2_onRequest(ctx, from_fd);
        }
        else
        {
            // need continue frame
        }
        break;
    }
    case SW_HTTP2_TYPE_DATA:
    {
        swHttp2FrameTraceLog(recv, "data");
        if (client->streams.find(stream_id) == client->streams.end())
        {
            zval_ptr_dtor(zdata);
            swoole_error_log(SW_LOG_WARNING, SW_ERROR_HTTP2_STREAM_NOT_FOUND, "http2 stream#%d not found.", stream_id);
            return SW_ERR;
        }
        stream = client->streams[stream_id];
        ctx = stream->ctx;

        zrequest_object = ctx->request.zobject;
        zend_update_property_long(Z_OBJCE_P(zrequest_object), zrequest_object, ZEND_STRL("streamId"), stream_id);

        swString *buffer = ctx->request.post_buffer;
        if (!buffer)
        {
            buffer = swString_new(SW_HTTP2_DATA_BUFFER_SIZE);
            ctx->request.post_buffer = buffer;
        }
        swString_append_ptr(buffer, buf, length);

        // flow control
        client->recv_window -= length;
        stream->recv_window -= length;
        if (length > 0)
        {
            if (client->recv_window < (SW_HTTP2_MAX_WINDOW_SIZE / 4))
            {
                http2_server_send_window_update(fd, 0, SW_HTTP2_MAX_WINDOW_SIZE - client->recv_window);
                client->recv_window = SW_HTTP2_MAX_WINDOW_SIZE;
            }
            if (stream->recv_window < (SW_HTTP2_MAX_WINDOW_SIZE / 4))
            {
                http2_server_send_window_update(fd, stream_id, SW_HTTP2_MAX_WINDOW_SIZE - stream->recv_window);
                stream->recv_window = SW_HTTP2_MAX_WINDOW_SIZE;
            }
        }

        if (flags & SW_HTTP2_FLAG_END_STREAM)
        {
            if (SwooleG.serv->http_parse_post && ctx->request.post_form_urlencoded)
            {
                zval *zpost;
                swoole_http_server_array_init(post, request);
                char *post_content = estrndup(buffer->str, buffer->length);
                sapi_module.treat_data(PARSE_STRING, post_content, zpost);
            }
            else if (ctx->mt_parser != NULL)
            {
                multipart_parser *multipart_parser = ctx->mt_parser;
                size_t n = multipart_parser_execute(multipart_parser, buffer->str, buffer->length);
                if (n != (size_t) length)
                {
                    swoole_php_fatal_error(E_WARNING, "parse multipart body failed.");
                }
            }
            http2_onRequest(ctx, from_fd);
        }
        break;
    }
    case SW_HTTP2_TYPE_PING:
    {
        swHttp2FrameTraceLog(recv, "ping");
        char ping_frame[SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE];
        swHttp2_set_frame_header(ping_frame, SW_HTTP2_TYPE_PING, SW_HTTP2_FRAME_PING_PAYLOAD_SIZE, SW_HTTP2_FLAG_ACK, stream_id);
        memcpy(ping_frame + SW_HTTP2_FRAME_HEADER_SIZE, buf, SW_HTTP2_FRAME_PING_PAYLOAD_SIZE);
        swServer_tcp_send(SwooleG.serv, fd, ping_frame, SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE);
        break;
    }
    case SW_HTTP2_TYPE_WINDOW_UPDATE:
    {
        value = ntohl(*(uint32_t *) buf);
        if (stream_id == 0)
        {
            client->send_window += value;
        }
        else if (client->streams.find(stream_id) != client->streams.end())
        {
            stream = client->streams[stream_id];
            stream->send_window += value;
        }
        swHttp2FrameTraceLog(recv, "window_size_increment=%d", value);
        break;
    }
    case SW_HTTP2_TYPE_RST_STREAM:
    {
        value = ntohl(*(int *) (buf));
        swHttp2FrameTraceLog(recv, "error_code=%d", value);
        if (client->streams.find(stream_id) != client->streams.end())
        {
            // TODO: i onRequest and use request->recv
            // stream exist
            stream = client->streams[stream_id];
            client->streams.erase(stream_id);
            delete stream;
        }
        break;
    }
    case SW_HTTP2_TYPE_GOAWAY:
    {
        uint32_t server_last_stream_id = ntohl(*(uint32_t *) (buf));
        buf += 4;
        value = ntohl(*(uint32_t *) (buf));
        buf += 4;
        swHttp2FrameTraceLog(recv, "last_stream_id=%d, error_code=%d, opaque_data=[%.*s]", server_last_stream_id, value, (int) (length - SW_HTTP2_GOAWAY_SIZE), buf);
        //TODO: onRequest
        (void) server_last_stream_id;

        break;
    }
    default:
    {
        swHttp2FrameTraceLog(recv, "");
    }
    }

    zval_ptr_dtor(zdata);

    return SW_OK;
}

void swoole_http2_free(swConnection *conn)
{
    if (http2_sessions.find(conn->session_id) == http2_sessions.end())
    {
        return;
    }
    http2_session *client = http2_sessions[conn->session_id];
    http2_sessions.erase(conn->session_id);
    delete client;
}
#endif
