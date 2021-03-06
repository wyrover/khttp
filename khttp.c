#include "khttp.h"
#include "log.h"
#include <errno.h>
#include <time.h>

int khttp_socket_nonblock(int fd, int enable);
int khttp_socket_reuseaddr(int fd, int enable);
int http_socket_sendtimeout(int fd, int timeout);
int http_socket_recvtimeout(int fd, int timeout);

struct {
    char text[8];
}method_type[]={
    {"GET"},
    {"POST"},
    {"PUT"},
    {"DELETE"}
};

struct {
    char text[8];
}auth_type[]={
    {"None"},
    {"Digest"},
    {"Basic"}
};
static char base64_encoding_table[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
                                'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                                'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                                'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                                'w', 'x', 'y', 'z', '0', '1', '2', '3',
                                '4', '5', '6', '7', '8', '9', '+', '/'};
static char *base64_decoding_table = NULL;
static int mod_table[] = {0, 2, 1};

void build_decoding_table() {
    if(base64_decoding_table != NULL) return;
    base64_decoding_table = malloc(256);
    int i;
    for (i = 0; i < 64; i++)
        base64_decoding_table[(unsigned char) base64_encoding_table[i]] = i;
}

void base64_cleanup() {
    if(base64_decoding_table){
        free(base64_decoding_table);
        base64_decoding_table = NULL;
    }
}

char *khttp_base64_encode(const unsigned char *data,
                    size_t input_length,
                    size_t *output_length) {

    *output_length = 4 * ((input_length + 2) / 3);

    char *encoded_data = malloc(*output_length+1);
    if (encoded_data == NULL) return NULL;
    memset(encoded_data, 0, *output_length);
    int i,j;
    for (i = 0, j = 0; i < input_length;) {

        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = base64_encoding_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = base64_encoding_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = base64_encoding_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = base64_encoding_table[(triple >> 0 * 6) & 0x3F];
    }

    for (i = 0; i < mod_table[input_length % 3]; i++)
        encoded_data[*output_length - 1 - i] = '=';
    encoded_data[*output_length] = '\0';
    return encoded_data;
}
char *khttp_base64_decode(const char *data,
                    size_t input_length,
                    size_t *output_length) {
    if (base64_decoding_table == NULL) build_decoding_table();
    unsigned char *ptr = (unsigned char *)data;
    if (input_length % 4 != 0) return NULL;

    *output_length = input_length / 4 * 3;
    if (data[input_length - 1] == '=') (*output_length)--;
    if (data[input_length - 2] == '=') (*output_length)--;

    char *decoded_data = malloc(*output_length + 1);
    memset(decoded_data , 0, *output_length +1);
    if (decoded_data == NULL) return NULL;
    int i,j;
    for (i = 0, j = 0; i < input_length;) {

        uint32_t sextet_a = data[i] == '=' ? 0 & i++ : base64_decoding_table[ptr[i++]];
        uint32_t sextet_b = data[i] == '=' ? 0 & i++ : base64_decoding_table[ptr[i++]];
        uint32_t sextet_c = data[i] == '=' ? 0 & i++ : base64_decoding_table[ptr[i++]];
        uint32_t sextet_d = data[i] == '=' ? 0 & i++ : base64_decoding_table[ptr[i++]];

        uint32_t triple = (sextet_a << 3 * 6)
        + (sextet_b << 2 * 6)
        + (sextet_c << 1 * 6)
        + (sextet_d << 0 * 6);

        if (j < *output_length) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }
    return decoded_data;
}

static size_t khttp_file_size(char *file)
{
    if(!file) return -1;
    size_t len = -1;
    FILE *fp = fopen(file, "r");
    if(fp){
        fseek(fp, 0, SEEK_END);
        len = ftell(fp);
        fclose(fp);
    }
    return len;
}

static char *khttp_auth2str(int type)
{
    return auth_type[type].text;
}
static char *khttp_type2str(int type)
{
    return method_type[type].text;
}

int khttp_body_cb (http_parser *p, const char *buf, size_t len)
{
    //LOG_DEBUG("\n");
    khttp_ctx *ctx = p->data;
    char *head = ctx->body;
    if(ctx->done == 1){ //Parse done copy body
        int offset = ctx->body_len;
        //printf("-------------%zu offset %d\n%s\n", len, offset,buf);
        memcpy(head + offset, buf, len);
    }
    ctx->body_len += len;
    //LOG_DEBUG("body callbacked length:%zu\n", len);
    return 0;
}

int khttp_response_status_cb (http_parser *p, const char *buf, size_t len)
{
    //LOG_DEBUG("\n");
#ifndef KHTTP_DEBUG
    return 0;
#else
    char *tmp = malloc(len + 1);
    if(!tmp) return 0;
    tmp[len] = 0;
    memcpy(tmp, buf, len);
    LOG_DEBUG("khttp status code %s\n", tmp);
    free(tmp);
    return 0;
#endif
}

int khttp_message_complete_cb (http_parser *p)
{
    //LOG_DEBUG("\n");
    khttp_ctx *ctx = p->data;
    ctx->done = 1;
    return 0;
}

int khttp_header_field_cb (http_parser *p, const char *buf, size_t len)
{
    //LOG_DEBUG("\n");
    khttp_ctx *ctx = p->data;
    if(ctx->done == 1){
        char *tmp = malloc(len + 1);
        if(!tmp) return -KHTTP_ERR_OOM;
        tmp[len] = 0;
        ctx->header_field[ctx->header_count] = tmp;
        memcpy(tmp, buf, len);
        //printf("%d header field: %s ||||| ", ctx->header_count, tmp);
    }
    return 0;
}

int khttp_header_value_cb (http_parser *p, const char *buf, size_t len)
{
    //LOG_DEBUG("\n");
    khttp_ctx *ctx = p->data;
    if(ctx->done == 1){
        char *tmp = malloc(len + 1);
        if(!tmp) return -KHTTP_ERR_OOM;
        tmp[len] = 0;
        ctx->header_value[ctx->header_count] = tmp;
        memcpy(tmp, buf, len);
        //printf(" header value: %s\n", tmp);
        ctx->header_count ++;
    }
    return 0;
}

void khttp_dump_header(khttp_ctx *ctx)
{
    if(!ctx) return;
    int i = 0;
    for(i = 0; i < ctx->header_count ; i++){
        printf("%02d %20s     %s\n", i , ctx->header_field[i], ctx->header_value[i]);
    }
}

char *khttp_find_header(khttp_ctx *ctx, const char *header)
{
    if(!ctx) return NULL;
    int i = 0;
    for(i = 0; i < ctx->header_count ; i++){
        if(strncmp(header, ctx->header_field[i], strlen(header)) == 0) {
            //printf("match %02d %20s     %s\n", i , ctx->header_field[i], ctx->header_value[i]);
            return ctx->header_value[i];
        }
    }
    return NULL;
}

int khttp_field_copy(char *in, char *out, int len)
{
    if(in == NULL || out == NULL) return -1;
    int i = 0;
    for(i = 0; i < strlen(in); i ++ ){
        if(in[i] != '"') {
            out[i] = in[i];
        }else{
            out[i] = 0;
            break;
        }
    }
    return 0;
}

int khttp_parse_auth(khttp_ctx *ctx, char *value)
{
    char *realm;
    char *nonce;
    char *qop;
    char *opaque;
    char *ptr = value;
    if(strncmp(ptr, "Digest", 6) == 0){
        ctx->auth_type = KHTTP_AUTH_DIGEST;
        if((realm = strstr(ptr, "realm")) != NULL){
            realm = realm + strlen("realm:\"");
            khttp_field_copy(realm, ctx->realm, KHTTP_REALM_LEN);
        }
        if((nonce = strstr(ptr, "nonce")) != NULL){
            nonce = nonce + strlen("nonce:\"");
            khttp_field_copy(nonce, ctx->nonce, KHTTP_NONCE_LEN);
        }
        if((opaque = strstr(ptr, "opaque")) != NULL){
            opaque = opaque + strlen("opaque:\"");
            khttp_field_copy(opaque, ctx->opaque, KHTTP_OPAQUE_LEN);
        }
        if((qop = strstr(ptr, "qop")) != NULL){
            qop = qop + strlen("qop:\"");
            khttp_field_copy(qop, ctx->qop, KHTTP_QOP_LEN);
        }
    }else if(strncmp(ptr, "Basic", 5) == 0){
        ctx->auth_type = KHTTP_AUTH_BASIC;
    }
    //Digest realm="Users", nonce="KYRxkHxBfiylcOAMM3YiUPWqzUkdgv8y", qop="auth"
    return 0;
}

void khttp_free_header(khttp_ctx *ctx)
{
    if(!ctx) return;
    int i = 0;
    for(i = 0; i < ctx->header_count ; i++){
        if(ctx->header_field[i]) {
            free(ctx->header_field[i]);
            ctx->header_field[i] = NULL;
        }
        if(ctx->header_value[i]) {
            free(ctx->header_value[i]);
            ctx->header_value[i] = NULL;
        }
    }
    ctx->header_count = 0;
}

void khttp_free_body(khttp_ctx *ctx)
{
    if(ctx->body){
        free(ctx->body);
        ctx->body = NULL;
    }
    ctx->done = 0;
}

static http_parser_settings http_parser_cb =
{
    .on_message_begin       = 0
    ,.on_header_field       = khttp_header_field_cb
    ,.on_header_value       = khttp_header_value_cb
    ,.on_url                = 0
    ,.on_status             = khttp_response_status_cb
    ,.on_body               = khttp_body_cb
    ,.on_headers_complete   = 0
    ,.on_message_complete   = khttp_message_complete_cb
};

khttp_ctx *khttp_new()
{
    unsigned char rands[8];
    khttp_ctx *ctx = malloc(sizeof(khttp_ctx));
    if(!ctx){
        LOG_ERROR("khttp context create failure out of memory\n");
        return NULL;
    }
    memset(ctx, 0, sizeof(khttp_ctx));
#ifdef KHTTP_USE_URANDOM
    FILE *fp = fopen("/dev/urandom", "r");
    if(fp){
        size_t len = fread(rands, 1, 8, fp);
        sprintf(ctx->boundary, "%02x%02x%02x%02x%02x%02x%02x%02x",
                rands[0], rands[1], rands[2], rands[3],
                rands[4], rands[5], rands[6], rands[7]
                );
        fclose(fp);
    }else{
#else
    if(1){
#endif
        srand (time(NULL));
        int r = rand();
        rands[0] = r >> 24;
        rands[1] = r >> 16;
        rands[2] = r >> 8;
        rands[3] = r;
        srand(r);
        r = rand();
        rands[4] = r >> 24;
        rands[5] = r >> 16;
        rands[6] = r >> 8;
        rands[7] = r;
        sprintf(ctx->boundary, "%02x%02x%02x%02x%02x%02x%02x%02x",
                rands[0], rands[1], rands[2], rands[3],
                rands[4], rands[5], rands[6], rands[7]
                );
        //TODO random generate boundary
    }
    return ctx;
}

void khttp_destroy(khttp_ctx *ctx)
{
    if(!ctx) return;
    khttp_free_header(ctx);
    khttp_free_body(ctx);
#ifdef OPENSSL
    if(ctx->ssl){
        SSL_set_shutdown(ctx->ssl, 2);
        SSL_shutdown(ctx->ssl);
        SSL_free(ctx->ssl);
        ctx->ssl = NULL;
        if(ctx->ssl_ctx) SSL_CTX_free(ctx->ssl_ctx);
    }
#endif
    if(ctx->fd > 0) close(ctx->fd);
    if(ctx->body) {
        free(ctx->body);
        ctx->body = NULL;
    }
    if(ctx->data) {
        free(ctx->data);
        ctx->data = NULL;
    }
    if(ctx->form) {
        free(ctx->form);
        ctx->form = NULL;
    }
    if(ctx){
        free(ctx);
    }
}

int khttp_socket_create()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        LOG_ERROR("khttp socket create failure %d(%s)\n", errno, strerror(errno));
        return fd;
    }
    //Default enable nonblock / reuseaddr and set send / recv timeout
    //khttp_socket_nonblock(fd, 1);
    khttp_socket_reuseaddr(fd, 1);
    //http_socket_sendtimeout(fd, KHTTP_SEND_TIMEO);
    //http_socket_recvtimeout(fd, KHTTP_RECV_TIMEO);
    return fd;
}

int khttp_socket_nonblock(int fd, int enable)
{
    unsigned long on = enable;
    int ret = ioctl(fd, FIONBIO, &on);
    if(ret != 0){
        LOG_WARN("khttp set socket nonblock failure %d(%s)\n", errno, strerror(errno));
    }
    return ret;
}

int khttp_socket_reuseaddr(int fd, int enable)
{
    int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&enable, sizeof(enable));
    if(ret != 0){
        LOG_WARN("khttp set socket reuseaddr failure %d(%s)\n", errno, strerror(errno));
    }
    return ret;
}

int http_socket_sendtimeout(int fd, int timeout)
{
    struct timeval tv;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    int ret = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    if(ret != 0){
        LOG_WARN("khttp set socket send timeout failure %d(%s)\n", errno, strerror(errno));
    }
    return ret;
}

int http_socket_recvtimeout(int fd, int timeout)
{
    struct timeval tv;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    int ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    if(ret != 0){
        LOG_WARN("khttp set socket recv timeout failure %d(%s)\n", errno, strerror(errno));
    }
    return ret;
}

int khttp_md5sum(char *input, int len, char *out)
{
    int ret = 0, i = 0;
#ifdef OPENSSL
    MD5_CTX ctx;
    char buf[3] = {'\0'};
    unsigned char md5[MD5_DIGEST_LENGTH];
    if(input == NULL || len < 1 || out == NULL)
        return -1;
    MD5_Init(&ctx);
    MD5_Update(&ctx, input, len);
    MD5_Final(md5, &ctx);
    out[0] = '\0';
    for(i=0;i<MD5_DIGEST_LENGTH;i++)
    {
        sprintf(buf, "%02x", md5[i]);
        strcat(out, buf);
    }
#else
//#error "FIXME NO OPENSSL"
#endif
    //LOG_DEBUG("MD5:[%s]\n", out);
    return ret;
}

int khttp_set_method(khttp_ctx *ctx, int method)
{
    if(method < KHTTP_GET || method > KHTTP_DELETE){
        LOG_ERROR("khttp set method parameter out of range\n");
        return -KHTTP_ERR_PARAM;
    }
    ctx->method = method;
    return KHTTP_ERR_OK;
}
void khttp_copy_host(char *in, char *out)
{
    int i = 0;
    for(i=0 ; i<strlen(in) ; i++) {
        if(in[i] == ':' || in[i] == '/' || in[i] == '\0') break;
        out[i] = in[i];
    }
}

void khttp_dump_uri(khttp_ctx *ctx)
{
    printf("======================\n");
    printf("host: %s\n", ctx->host);
    printf("port: %d\n", ctx->port);
    printf("path: %s\n", ctx->path);
}

void khttp_dump_message_flow(char *data, int len, int way)
{
#ifdef KHTTP_DEBUG_SESS
    //data[len]  = 0;
    printf("----------------------------------------------\n");
    if(way == 0){
        printf("          Client   >>>    Server\n");
    }else{
        printf("          Server   >>>    Client\n");
    }
    printf("----------------------------------------------\n");
    printf("%s\n", data);
    printf("----------------------------------------------\n");
#endif
}

int http_send(khttp_ctx *ctx, void *buf, int len, int timeout)
{
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000 )  * 1000;
    if(ctx->fd < 0) return -KHTTP_ERR_NO_FD;
    int sent = 0;
    char *head = buf;
    do {
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(ctx->fd, &fs);
        int ret = select(ctx->fd +1, NULL, &fs, NULL, &tv);
        if(ret >= 0){
            // ret == 0 handle?
            //LOG_DEBUG("send:\n%s\nfd:%d\n", head, ctx->fd);
            ret = send(ctx->fd, head + sent, len - sent, 0);
            if(ret > 0) {
                sent += ret;
            } else {
                LOG_ERROR("khttp send error %d (%s)\n", errno, strerror(errno));
                return -KHTTP_ERR_SEND;
            }
        }else{
            return -KHTTP_ERR_DISCONN;
        }
    }while(sent < len);
    return KHTTP_ERR_OK;
}
#ifdef OPENSSL
int https_send(khttp_ctx *ctx, void *buf, int len, int timeout)
{
    int sent = 0;
    char *head = buf;
    struct timeval tv;
    int ret = KHTTP_ERR_OK;
    int retry = 3;//FIXME define in header
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    if(ctx->fd < 0) return -KHTTP_ERR_NO_FD;
    do {
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(ctx->fd, &fs);
        int res = select(ctx->fd + 1, NULL, &fs, NULL, &tv);
        if(res >= 0){
            //LOG_DEBUG("send data...\n");
            res = SSL_write(ctx->ssl, head + sent, len - sent);
            if(res > 0){
                sent += res;
            }else if(errno == -EAGAIN && retry != 0){
                retry--;
            }else{
                ret = -KHTTP_ERR_SEND;
                break;
            }
        }else{
            ret = -KHTTP_ERR_DISCONN;
            break;
        }
    }while(sent < len);
    //LOG_DEBUG("send https success\n%s\n", (char *)buf);
    return ret;
}
#endif
int http_recv(khttp_ctx *ctx, void *buf, int len, int timeout)
{
    int ret = KHTTP_ERR_OK;
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000)  * 1000;
    if(ctx->fd < 0) return -KHTTP_ERR_NO_FD;
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(ctx->fd, &fs);
    ret = select(ctx->fd + 1, &fs, NULL, NULL, &tv);
    if(ret >= 0) {
        if(ret == 0) {
            LOG_ERROR("khttp recv timeout\n");
        }
        ret = recv(ctx->fd, buf, len, 0);
        if(ret < 0) {
            LOG_ERROR("khttp recv error %d (%s)\n", errno, strerror(errno));
            return -KHTTP_ERR_RECV;
        }
    }else{
        LOG_ERROR("khttp recv select error %d (%s)\n", errno, strerror(errno));
    }
    return ret;
}
#ifdef OPENSSL
int https_recv(khttp_ctx *ctx, void *buf, int len, int timeout)
{
    if(ctx == NULL || buf == NULL || len <= 0) return -KHTTP_ERR_PARAM;
    int ret = KHTTP_ERR_OK;
    int res = 0;
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    if(SSL_pending(ctx->ssl) > 0){
        //data available
        res = SSL_read(ctx->ssl, buf, len);
        if(res <= 0){
            LOG_ERROR("SSL_read  error %d(%s)\n", errno, strerror(errno));
            ret = -KHTTP_ERR_RECV;
            goto end;
        }
        ret = res;
    }else{
        //data not available select socket
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(ctx->fd, &fs);
        res = select(ctx->fd + 1, &fs, NULL, NULL, &tv);
        if(res < 0){
            LOG_ERROR("https select error %d(%s)\n", errno, strerror(errno));
            ret = -KHTTP_ERR_RECV;
            goto end;
        }else if(res == 0){
            LOG_ERROR("https select timeout\n");
            ret = -KHTTP_ERR_TIMEOUT;
            goto end;
        }
        res = SSL_read(ctx->ssl, buf, len);
        if(res <= 0){
            LOG_ERROR("SSL_read  error %d(%s)\n", errno, strerror(errno));
            ret = -KHTTP_ERR_RECV;
            goto end;
        }
        ret = res;
    }
end:
    return ret;
}
#endif
int khttp_set_uri(khttp_ctx *ctx, char *uri)
{
    char *head = uri;
    char *host = NULL;
    char *path = NULL;
    char *port = NULL;

    if(!ctx || !uri){
        return KHTTP_ERR_PARAM;
    }
    if(strncasecmp(uri, "https://", 8) == 0) {
        ctx->proto = KHTTP_HTTPS;
        host = head + 8;
#ifdef OPENSSL
        ctx->send = https_send;
        ctx->recv = https_recv;
#else
//#error "FIXME NO OPENSSL"
#endif
    } else if(strncasecmp(uri, "http://", 7) == 0) {
        ctx->proto = KHTTP_HTTP;
        host = head + 7;
        ctx->send = http_send;
        ctx->recv = http_recv;
    } else {
        ctx->proto = KHTTP_HTTP;
        host = head;
        ctx->send = http_send;
        ctx->recv = http_recv;
    }
    if((path = strchr(host, '/'))!= NULL) {
        strncpy(ctx->path, path, KHTTP_PATH_LEN);
    } else {
        strcpy(ctx->path, "/");
    }
    if((port = strchr(host, ':'))!= NULL) {
        ctx->port = atoi(port + 1);
        if(ctx->port < 1 || ctx->port > 65535){
            LOG_ERROR("khttp set port out of range: %d! use default port\n", ctx->port);
            if(ctx->proto == KHTTP_HTTPS) ctx->port = 443;
            else ctx->port = 80;
        }
    } else {
        // No port. Set default port number
        if(ctx->proto == KHTTP_HTTPS) ctx->port = 443;
        else ctx->port = 80;
    }
    khttp_copy_host(host, ctx->host);
    return KHTTP_ERR_OK;
}
#ifdef OPENSSL
static int ssl_ca_verify_cb(int ok, X509_STORE_CTX *store)
{
    int depth, err;
    X509 *cert = NULL;
    char data[KHTTP_SSL_DATA_LEN];
    if(!ok) {
        cert = X509_STORE_CTX_get_current_cert(store);
        depth = X509_STORE_CTX_get_error_depth(store);
        err = X509_STORE_CTX_get_error(store);
        LOG_DEBUG("Error with certificate at depth: %i", depth);
        X509_NAME_oneline(X509_get_issuer_name(cert), data, KHTTP_SSL_DATA_LEN);
        LOG_DEBUG(" issuer = %s", data);
        X509_NAME_oneline(X509_get_subject_name(cert), data, KHTTP_SSL_DATA_LEN);
        LOG_DEBUG(" subject = %s", data);
        LOG_DEBUG(" err %i:%s", err, X509_verify_cert_error_string(err));
        return 0;
    }
    return ok;
}


int khttp_ssl_setup(khttp_ctx *ctx)
{
    int ret = 0;
    SSL_load_error_strings();
    if(SSL_library_init() != 1) {
        LOG_ERROR("SSL library init failure\n");
        return -KHTTP_ERR_SSL;
    }
    if(ctx->ssl_method == KHTTP_METHOD_SSLV2_3){
        if( (ctx->ssl_ctx = SSL_CTX_new(SSLv23_client_method())) == NULL) {
            LOG_ERROR("SSL setup request method SSLv23 failure\n");
            return -KHTTP_ERR_SSL;
        }
    }else if(ctx->ssl_method == KHTTP_METHOD_SSLV3){
        if( (ctx->ssl_ctx = SSL_CTX_new(SSLv3_client_method())) == NULL) {
            LOG_ERROR("SSL setup request method SSLv3 failure\n");
            return -KHTTP_ERR_SSL;
        }
    }else if(ctx->ssl_method == KHTTP_METHOD_TLSV1){
        if( (ctx->ssl_ctx = SSL_CTX_new(TLSv1_client_method())) == NULL) {
            LOG_ERROR("SSL setup request method TLSv1 failure\n");
            return -KHTTP_ERR_SSL;
        }
#ifndef __MAC__
    }else if(ctx->ssl_method == KHTTP_METHOD_TLSV1_1){
        if( (ctx->ssl_ctx = SSL_CTX_new(TLSv1_1_client_method())) == NULL) {
            LOG_ERROR("SSL setup request method TLSv1_1 failure\n");
            return -KHTTP_ERR_SSL;
        }
    }else if(ctx->ssl_method == KHTTP_METHOD_TLSV1_2){
        if( (ctx->ssl_ctx = SSL_CTX_new(TLSv1_2_client_method())) == NULL) {
            LOG_ERROR("SSL setup request method TLSv1_2 failure\n");
            return -KHTTP_ERR_SSL;
        }
#endif
    }else{
        //Not going happen
    }
    // Pass server auth
    if(ctx->pass_serv_auth){
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);
    }else{
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, ssl_ca_verify_cb);
        SSL_CTX_set_verify_depth(ctx->ssl_ctx, KHTTP_SSL_DEPTH);
        if(SSL_CTX_load_verify_locations(ctx->ssl_ctx, ctx->cert_path, NULL) != 1){
            LOG_ERROR("khttp not able to load certificate on path: %s\n", ctx->cert_path);
        }
    }
    SSL_CTX_set_default_passwd_cb_userdata(ctx->ssl_ctx, ctx->key_pass);
    if(SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, ctx->cert_path) == 1) {
        LOG_DEBUG("khttp load certificate success\n");
    }
    if(SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, ctx->key_path, SSL_FILETYPE_PEM) == 1) {
        LOG_DEBUG("khttp load private key success\n");
    }
    if(SSL_CTX_check_private_key(ctx->ssl_ctx) == 1) {
        LOG_DEBUG("khttp check private key success\n");
    }
    if((ctx->ssl = SSL_new(ctx->ssl_ctx)) == NULL) {
        LOG_ERROR("create SSL failure\n");
        return -KHTTP_ERR_SSL;
    }
    if((ret = SSL_set_fd(ctx->ssl, ctx->fd)) != 1) {
        ret = SSL_get_error(ctx->ssl, ret);
        LOG_ERROR("set SSL fd failure %d\n", ret);
        return -KHTTP_ERR_SSL;
    }
    if((ret = SSL_connect(ctx->ssl)) != 1) {
        char error_buffer[256];
        LOG_ERROR("SSL_connect failure %d\n", ret);
        ret = SSL_get_error(ctx->ssl, ret);
        if(SSL_ERROR_WANT_READ == ret){
            return KHTTP_ERR_OK;
        }else if(SSL_ERROR_WANT_WRITE == ret) {
            return KHTTP_ERR_OK;
        }
        switch(ret){
            case 0x1470E086:
            case 0x14090086:
                ret = SSL_get_verify_result(ctx->ssl);
                if(ret != X509_V_OK){
                    snprintf(error_buffer, sizeof(error_buffer),
                            "SSL certificate problem: %s",
                            X509_verify_cert_error_string(ret));
                }
            default:
                ERR_error_string_n(ret, error_buffer, sizeof(error_buffer));
                break;
        }
        LOG_ERROR("SSL_get_error failure %d %s\n", ret, error_buffer);
        return -KHTTP_ERR_SSL;//TODO
    }
    //LOG_DEBUG("Connect to SSL server success\n");
    return KHTTP_ERR_OK;
}

int khttp_ssl_set_method(khttp_ctx *ctx, int method)
{
    if(method >= KHTTP_METHOD_SSLV2_3 && method <= KHTTP_METHOD_TLSV1_2){
        ctx->ssl_method = method;
    }
    return KHTTP_ERR_OK;
}

int khttp_ssl_skip_auth(khttp_ctx *ctx)
{
    ctx->pass_serv_auth = 1;
    return KHTTP_ERR_OK;
}

int khttp_ssl_set_cert_key(khttp_ctx *ctx, char *cert, char *key, char *pw)
{
    if(ctx == NULL || cert == NULL || key == NULL) return -KHTTP_ERR_PARAM;
    if(khttp_file_size(cert) <= 0) return -KHTTP_ERR_NO_FILE;
    if(khttp_file_size(key) <= 0) return -KHTTP_ERR_NO_FILE;
    strncpy(ctx->cert_path, cert, KHTTP_PATH_LEN);
    strncpy(ctx->key_path, key, KHTTP_PATH_LEN);
    if(pw) strncpy(ctx->key_pass, pw, KHTTP_PASS_LEN);
    return KHTTP_ERR_OK;
}
#endif
int khttp_set_username_password(khttp_ctx *ctx, char *username, char *password, int auth_type)
{
    if(ctx == NULL || username == NULL || password == NULL) return -KHTTP_ERR_PARAM;
    strncpy(ctx->username, username, KHTTP_USER_LEN);
    strncpy(ctx->password, password, KHTTP_PASS_LEN);
    if(auth_type == KHTTP_AUTH_DIGEST){
        ctx->auth_type = KHTTP_AUTH_DIGEST;
    }else{
        //Default auth type is basic if auth_type not define
        ctx->auth_type = KHTTP_AUTH_BASIC;
    }
    return KHTTP_ERR_OK;
}

int khttp_set_post_data(khttp_ctx *ctx, char *data)
{
    if(ctx == NULL || data == NULL) return -KHTTP_ERR_PARAM;
    if(ctx->data) free(ctx->data);
    //Malloc memory from data string length. Should be protect?
    ctx->data = malloc(strlen(data) + 1);
    if(!ctx->data) return -KHTTP_ERR_OOM;
    //Copy from data
    strcpy(ctx->data, data);
    return KHTTP_ERR_OK;
}

int khttp_set_post_form(khttp_ctx *ctx, char *key, char *value, int type)
{
    if(ctx == NULL || key == NULL || value == NULL || type < KHTTP_FORM_STRING || type > KHTTP_FORM_FILE) return -KHTTP_ERR_PARAM;
    if(type == KHTTP_FORM_STRING){
        size_t offset = ctx->form_len;
        ctx->form_len = ctx->form_len + 44 + strlen("Content-Disposition: form-data; name=\"\"\r\n\r\n") + 2;
        ctx->form_len = ctx->form_len + strlen(key) + strlen(value);
        ctx->form = realloc(ctx->form, ctx->form_len + 1);
        if(ctx->form == NULL) return -KHTTP_ERR_OOM;
        char *head = ctx->form + offset;
        sprintf(head, "--------------------------%s\r\n"
                "Content-Disposition: form-data; name=\"%s\"\r\n\r\n"
                "%s\r\n"
                ,ctx->boundary
                ,key
                ,value
                );
    }else{
        //TODO add file type checking
        //text/plain or application/octet-stream
        size_t offset = ctx->form_len;
        ctx->form_len = ctx->form_len + 44 + strlen("Content-Disposition: form-data; name=\"\"; filename=\"\"\r\nContent-Type: application/octet-stream\r\n\r\n") + 2;
        //origin size + end boundary + header + file end(\r\n)
        size_t file_size = khttp_file_size(value);
        if(file_size <= 0){
            LOG_ERROR("File %s not exist\n",value);
            return -KHTTP_ERR_NO_FILE;
        }
        //Calculate the latest form length
        ctx->form_len = ctx->form_len + strlen(key) + file_size + strlen(value);
        ctx->form = realloc(ctx->form, ctx->form_len + 1);
        if(ctx->form == NULL) return -KHTTP_ERR_OOM;
        //Write the next header
        char *head = ctx->form + offset;
        int head_len = sprintf(head, "--------------------------%s\r\n"
                "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\n"
                ,ctx->boundary
                ,key
                ,value
                );
        head = head + head_len;//Offset header
        FILE *fp = fopen(value, "r");
        if(fp){
            if(fread(head , 1, file_size, fp) != file_size){
                LOG_ERROR("read file failure\n");
                fclose(fp);
                return -KHTTP_ERR_FILE_READ;
            }
            fclose(fp);
        }
        head = head + file_size;
        head[0] = '\r';
        head[1] = '\n';
        head[2] = 0;
        //LOG_DEBUG("\n%s\n", ctx->form);
    }
    return KHTTP_ERR_OK;
}

int khttp_send_http_req(khttp_ctx *ctx)
{
    char resp_str[KHTTP_RESP_LEN];
    //FIXME change to dynamic size
    char *req = malloc(KHTTP_REQ_SIZE);
    if(!req) return -KHTTP_ERR_OOM;

    memset(req, 0, KHTTP_REQ_SIZE);
    int len = 0;
    if(ctx->method == KHTTP_GET) {
        if(ctx->auth_type == KHTTP_AUTH_BASIC){
            len = snprintf(resp_str, KHTTP_RESP_LEN, "%s:%s", ctx->username, ctx->password);
            size_t base64_len;
            char *base64 = khttp_base64_encode((unsigned char *) resp_str, len, &base64_len);
            if(!base64) return -KHTTP_ERR_OOM;
            len = snprintf(req, KHTTP_REQ_SIZE, "GET %s HTTP/1.1\r\n"
                "Authorization: Basic %s\r\n"
                "User-Agent: %s\r\n"
                "Host: %s\r\n"
                "Accept: */*\r\n"
                "\r\n", ctx->path, base64 ,KHTTP_USER_AGENT, ctx->host);
            //TODO add len > KHTTP_REQ_SIZE handle
            free(base64);
            base64 = NULL;
        }else{
            len = snprintf(req, KHTTP_REQ_SIZE, "GET %s HTTP/1.1\r\n"
                "User-Agent: %s\r\n"
                "Host: %s\r\n"
                "Accept: */*\r\n"
                "\r\n", ctx->path, KHTTP_USER_AGENT, ctx->host);
        }
    }else if(ctx->method == KHTTP_POST){
        if(ctx->auth_type == KHTTP_AUTH_BASIC){
            len = snprintf(resp_str, KHTTP_RESP_LEN, "%s:%s", ctx->username, ctx->password);
            size_t base64_len;
            char *base64 = khttp_base64_encode((unsigned char *) resp_str, len, &base64_len);
            if(!base64) return -KHTTP_ERR_OOM;
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE, "POST %s HTTP/1.1\r\n"
                    "Authorization: Basic %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n", ctx->path, base64 ,KHTTP_USER_AGENT, ctx->host, strlen(ctx->data));
            }else if(ctx->form){
                len = snprintf(req, KHTTP_REQ_SIZE, "POST %s HTTP/1.1\r\n"
                    "Authorization: Basic %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Expect: 100-continue\r\n"
                    "Content-Type: multipart/form-data; boundary=------------------------%s\r\n"
                    "\r\n", ctx->path, base64 ,KHTTP_USER_AGENT, ctx->host, ctx->form_len + 44, ctx->boundary);
                //FIXME change the Content-Type to dynamic like application/x-www-form-urlencoded or application/json...
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE, "POST %s HTTP/1.1\r\n"
                    "Authorization: Basic %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "\r\n", ctx->path, base64 ,KHTTP_USER_AGENT, ctx->host);
                //TODO add len > KHTTP_REQ_SIZE handle
            }
            free(base64);
            base64 = NULL;
        }else{
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE, "POST %s HTTP/1.1\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n", ctx->path, KHTTP_USER_AGENT, ctx->host, strlen(ctx->data));
            }else if(ctx->form){
                len = snprintf(req, KHTTP_REQ_SIZE, "POST %s HTTP/1.1\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Expect: 100-continue\r\n"
                    "Content-Type: multipart/form-data; boundary=------------------------%s\r\n"
                    "\r\n", ctx->path, KHTTP_USER_AGENT, ctx->host, ctx->form_len + 46, ctx->boundary);
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE, "POST %s HTTP/1.1\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "\r\n", ctx->path, KHTTP_USER_AGENT, ctx->host);
            }
        }
    }else if(ctx->method == KHTTP_PUT){
        if(ctx->auth_type == KHTTP_AUTH_BASIC){
            len = snprintf(resp_str, KHTTP_RESP_LEN, "%s:%s", ctx->username, ctx->password);
            size_t base64_len;
            char *base64 = khttp_base64_encode((unsigned char *) resp_str, len, &base64_len);
            if(!base64) return -KHTTP_ERR_OOM;
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE, "PUT %s HTTP/1.1\r\n"
                    "Authorization: Basic %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n", ctx->path, base64 ,KHTTP_USER_AGENT, ctx->host, strlen(ctx->data));
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE, "PUT %s HTTP/1.1\r\n"
                    "Authorization: Basic %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "\r\n", ctx->path, base64 ,KHTTP_USER_AGENT, ctx->host);
                //TODO add len > KHTTP_REQ_SIZE handle
            }
            free(base64);
            base64 = NULL;
        }else{
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE, "PUT %s HTTP/1.1\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n", ctx->path, KHTTP_USER_AGENT, ctx->host, strlen(ctx->data));
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE, "PUT %s HTTP/1.1\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "\r\n", ctx->path, KHTTP_USER_AGENT, ctx->host);
            }
        }
    }else if(ctx->method == KHTTP_DELETE){
        if(ctx->auth_type == KHTTP_AUTH_BASIC){
            len = snprintf(resp_str, KHTTP_RESP_LEN, "%s:%s", ctx->username, ctx->password);
            size_t base64_len;
            char *base64 = khttp_base64_encode((unsigned char *) resp_str, len, &base64_len);
            if(!base64) return -KHTTP_ERR_OOM;
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE, "DELETE %s HTTP/1.1\r\n"
                    "Authorization: Basic %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n", ctx->path, base64 ,KHTTP_USER_AGENT, ctx->host, strlen(ctx->data));
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE, "DELETE %s HTTP/1.1\r\n"
                    "Authorization: Basic %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "\r\n", ctx->path, base64 ,KHTTP_USER_AGENT, ctx->host);
                //TODO add len > KHTTP_REQ_SIZE handle
            }
            free(base64);
            base64 = NULL;
        }else{
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE, "DELETE %s HTTP/1.1\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n", ctx->path, KHTTP_USER_AGENT, ctx->host, strlen(ctx->data));
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE, "DELETE %s HTTP/1.1\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s\r\n"
                    "Accept: */*\r\n"
                    "\r\n", ctx->path, KHTTP_USER_AGENT, ctx->host);
            }
        }
    }else{
        //TODO add DELETE and UPDATE?
    }
    if(req){
        khttp_dump_message_flow(req, len, 0);
        if(ctx->send(ctx, req, len, KHTTP_SEND_TIMEO) != KHTTP_ERR_OK){
            LOG_ERROR("khttp request send failure\n");
        }
    }
    if(ctx->data){
        khttp_dump_message_flow(ctx->data, len, 0);
        if(ctx->send(ctx, ctx->data, strlen(ctx->data), KHTTP_SEND_TIMEO) != KHTTP_ERR_OK){
            LOG_ERROR("khttp request send failure\n");
        }
    }
    free(req);
    return 0;
}
int khttp_send_form(khttp_ctx *ctx)
{
    if(ctx->form){
        //LOG_DEBUG("length: %lu\n%s",ctx->form_len, ctx->form);
        if(ctx->send(ctx, ctx->form, ctx->form_len, KHTTP_SEND_TIMEO) != KHTTP_ERR_OK){
            LOG_ERROR("khttp request send failure\n");
        }
        char buf[47];
        memset(buf, 0, 47);
        snprintf(buf, 47,"--------------------------%s--\r\n", ctx->boundary);
        if(ctx->send(ctx, buf, 46, KHTTP_SEND_TIMEO) != KHTTP_ERR_OK){
            LOG_ERROR("khttp request send failure\n");
        }
    }
    return -KHTTP_ERR_OK;
}
int khttp_send_http_auth(khttp_ctx *ctx)
{
    char ha1[KHTTP_NONCE_LEN];
    char ha2[KHTTP_NONCE_LEN];
    char resp_str[KHTTP_RESP_LEN];
    char response[KHTTP_NONCE_LEN];
    char cnonce[KHTTP_CNONCE_LEN];
    char *req = malloc(KHTTP_REQ_SIZE);
    if(!req) return -KHTTP_ERR_OOM;
    char *cnonce_b64 = NULL;
    char path[KHTTP_PATH_LEN + 8];
    int len = 0;
    if (ctx->auth_type == KHTTP_AUTH_DIGEST){
        //HA1
        len = snprintf(resp_str, KHTTP_CNONCE_LEN, "%s:%s:%s", ctx->username, ctx->realm, ctx->password);
        memset(ha1, 0, KHTTP_NONCE_LEN);
        khttp_md5sum(resp_str, len, ha1);
        //HA2
        len = snprintf(path, KHTTP_PATH_LEN + 8, "%s:%s", khttp_type2str(ctx->method), ctx->path);
        memset(ha2, 0, KHTTP_NONCE_LEN);
        khttp_md5sum(path, len, ha2);
        //cnonce
        //TODO add random rule generate cnonce
        khttp_md5sum(cnonce, strlen(cnonce), cnonce);
        size_t cnonce_b64_len;
        cnonce_b64 = khttp_base64_encode((unsigned char *) cnonce, 32, &cnonce_b64_len);
        //response
        if(strcmp(ctx->qop, "auth") == 0){
            //FIXME dynamic generate nonceCount "00000001"
            len = snprintf(resp_str, KHTTP_RESP_LEN, "%s:%s:%s:%s:%s:%s", ha1, ctx->nonce, "00000001", cnonce_b64, ctx->qop, ha2);
            khttp_md5sum(resp_str, len, response);
        }else{
            len = snprintf(resp_str, KHTTP_RESP_LEN, "%s:%s:%s", ha1, ctx->nonce, ha2);
            khttp_md5sum(resp_str, len, response);
        }
    }else if(ctx->auth_type == KHTTP_AUTH_BASIC){
        len = snprintf(resp_str, KHTTP_RESP_LEN, "%s:%s", ctx->username, ctx->password);
        size_t cnonce_b64_len;
        cnonce_b64 = khttp_base64_encode((unsigned char *) resp_str, len, &cnonce_b64_len);
    }
    if(ctx->method == KHTTP_GET) {
        if(ctx->auth_type == KHTTP_AUTH_DIGEST){//Digest auth
            len = snprintf(req, KHTTP_REQ_SIZE,
                "GET %s HTTP/1.1\r\n"
                "Authorization: %s username=\"%s\", realm=\"%s\", "
                "nonce=\"%s\", uri=\"%s\", "
                "cnonce=\"%s\", nc=00000001, qop=%s, "
                "response=\"%s\"\r\n"
                "User-Agent: %s\r\n"
                "Host: %s:%d\r\n"
                "Accept: */*\r\n\r\n",
                ctx->path,
                khttp_auth2str(ctx->auth_type), ctx->username, ctx->realm,
                ctx->nonce, ctx->path,
                cnonce_b64, ctx->qop,
                response,
                KHTTP_USER_AGENT,
                ctx->host, ctx->port
                );
        }else{//Basic auth
            len = snprintf(req, KHTTP_REQ_SIZE,
                "GET %s HTTP/1.1\r\n"
                "Authorization: %s %s\r\n"
                "User-Agent: %s\r\n"
                "Host: %s:%d\r\n"
                "Accept: */*\r\n\r\n",
                ctx->path,
                khttp_auth2str(ctx->auth_type), cnonce_b64,
                KHTTP_USER_AGENT,
                ctx->host, ctx->port
                );
        }
    }else if(ctx->method == KHTTP_POST){
        if(ctx->auth_type == KHTTP_AUTH_DIGEST){//Digest auth
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "POST %s HTTP/1.1\r\n"
                    "Authorization: %s username=\"%s\", realm=\"%s\", "
                    "nonce=\"%s\", uri=\"%s\", "
                    "cnonce=\"%s\", nc=00000001, qop=%s, "
                    "response=\"%s\"\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), ctx->username, ctx->realm,
                    ctx->nonce, ctx->path,
                    cnonce_b64, ctx->qop,
                    response,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port,
                    strlen(ctx->data)
                    );
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "POST %s HTTP/1.1\r\n"
                    "Authorization: %s username=\"%s\", realm=\"%s\", "
                    "nonce=\"%s\", uri=\"%s\", "
                    "cnonce=\"%s\", nc=00000001, qop=%s, "
                    "response=\"%s\"\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), ctx->username, ctx->realm,
                    ctx->nonce, ctx->path,
                    cnonce_b64, ctx->qop,
                    response,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port
                    );
            }
        }else{//Basic auth
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "POST %s HTTP/1.1\r\n"
                    "Authorization: %s %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), cnonce_b64,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port,
                    strlen(ctx->data)
                    );
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "POST %s HTTP/1.1\r\n"
                    "Authorization: %s %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n"
                    "\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), cnonce_b64,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port
                    );
            }
        }
    }else if(ctx->method == KHTTP_PUT){
        if(ctx->auth_type == KHTTP_AUTH_DIGEST){//Digest auth
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "PUT %s HTTP/1.1\r\n"
                    "Authorization: %s username=\"%s\", realm=\"%s\", "
                    "nonce=\"%s\", uri=\"%s\", "
                    "cnonce=\"%s\", nc=00000001, qop=%s, "
                    "response=\"%s\"\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), ctx->username, ctx->realm,
                    ctx->nonce, ctx->path,
                    cnonce_b64, ctx->qop,
                    response,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port,
                    strlen(ctx->data)
                    );
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "PUT %s HTTP/1.1\r\n"
                    "Authorization: %s username=\"%s\", realm=\"%s\", "
                    "nonce=\"%s\", uri=\"%s\", "
                    "cnonce=\"%s\", nc=00000001, qop=%s, "
                    "response=\"%s\"\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), ctx->username, ctx->realm,
                    ctx->nonce, ctx->path,
                    cnonce_b64, ctx->qop,
                    response,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port
                    );
            }
        }else{//Basic auth
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "PUT %s HTTP/1.1\r\n"
                    "Authorization: %s %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), cnonce_b64,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port,
                    strlen(ctx->data)
                    );
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "PUT %s HTTP/1.1\r\n"
                    "Authorization: %s %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n"
                    "\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), cnonce_b64,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port
                    );
            }
        }
    }else if(ctx->method == KHTTP_DELETE){
        if(ctx->auth_type == KHTTP_AUTH_DIGEST){//Digest auth
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "DELETE %s HTTP/1.1\r\n"
                    "Authorization: %s username=\"%s\", realm=\"%s\", "
                    "nonce=\"%s\", uri=\"%s\", "
                    "cnonce=\"%s\", nc=00000001, qop=%s, "
                    "response=\"%s\"\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), ctx->username, ctx->realm,
                    ctx->nonce, ctx->path,
                    cnonce_b64, ctx->qop,
                    response,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port,
                    strlen(ctx->data)
                    );
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "DELETE %s HTTP/1.1\r\n"
                    "Authorization: %s username=\"%s\", realm=\"%s\", "
                    "nonce=\"%s\", uri=\"%s\", "
                    "cnonce=\"%s\", nc=00000001, qop=%s, "
                    "response=\"%s\"\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), ctx->username, ctx->realm,
                    ctx->nonce, ctx->path,
                    cnonce_b64, ctx->qop,
                    response,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port
                    );
            }
        }else{//Basic auth
            if(ctx->data){
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "DELETE %s HTTP/1.1\r\n"
                    "Authorization: %s %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n"
                    "Content-Length: %zu\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\n"
                    "\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), cnonce_b64,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port,
                    strlen(ctx->data)
                    );
            }else{
                len = snprintf(req, KHTTP_REQ_SIZE,
                    "DELETE %s HTTP/1.1\r\n"
                    "Authorization: %s %s\r\n"
                    "User-Agent: %s\r\n"
                    "Host: %s:%d\r\n"
                    "Accept: */*\r\n"
                    "\r\n",
                    ctx->path,
                    khttp_auth2str(ctx->auth_type), cnonce_b64,
                    KHTTP_USER_AGENT,
                    ctx->host, ctx->port
                    );
            }
        }
    }else{
    }
    khttp_dump_message_flow(req, len, 0);
    if(ctx->send(ctx, req, len, KHTTP_SEND_TIMEO) != KHTTP_ERR_OK){
        LOG_ERROR("khttp request send failure\n");
    }
    if(ctx->data){
        khttp_dump_message_flow(ctx->data, len, 0);
        if(ctx->send(ctx, ctx->data, strlen(ctx->data), KHTTP_SEND_TIMEO) != KHTTP_ERR_OK){
            LOG_ERROR("khttp request send failure\n");
        }
    }
    if(cnonce_b64) free(cnonce_b64);
    if(req) free(req);
    return 0;
}

int khttp_recv_http_resp(khttp_ctx *ctx)
{
    char buf[KHTTP_NETWORK_BUF];
    memset(buf, 0, KHTTP_NETWORK_BUF);
    int len = 0;
    char *data = NULL;
    // Pass context to http parser data pointer
    ctx->hp.data = ctx;
    int total = 0;
    for(;;) {
        len = ctx->recv(ctx, buf, KHTTP_NETWORK_BUF, KHTTP_RECV_TIMEO);
        if(len < 0) {
            return -KHTTP_ERR_RECV;
        }
        if(len == 0) return -KHTTP_ERR_DISCONN;
        data = realloc(data, total + len + 1);
        memcpy(data + total, buf, len);
        total += len;
        http_parser_init(&ctx->hp, HTTP_RESPONSE);
        ctx->body_len = 0;//Reset body length until parse finish.
        if(strncmp(data, "HTTP/1.1 100 Continue", 21) == 0){
            ctx->cont = 1;
            // char *end = strstr(data, "\r\n\r\n");
            //LOG_DEBUG("len: %d\n%s\n", len, data);
            if(len == 25){//Only get 100 Continue
                ctx->hp.status_code = 100;
                //LOG_DEBUG("Only get 100 continue\n");
                goto end;
            }else if(len > 25){
                ctx->cont = 1;//Get 100 Continue and others
                total = total - 25;
                memmove(data, data + 25, len - 25);
            }
        }
        //LOG_INFO("Parse:\n%s\n", data);
        http_parser_execute(&ctx->hp, &http_parser_cb, data, total);
        if(ctx->done == 1){
            break;
        }
    }
    http_parser_init(&ctx->hp, HTTP_RESPONSE);
    // Malloc memory for body
    ctx->body = malloc(ctx->body_len + 1);
    if(!ctx->body){
        return -KHTTP_ERR_OOM;
    }
    //LOG_DEBUG("malloc %zu byte for body\n", ctx->body_len);
    memset(ctx->body, 0, ctx->body_len + 1);
    //Set body length to 0 before parse
    ctx->body_len = 0;
    if(ctx->body == NULL) return -KHTTP_ERR_OOM;
    http_parser_execute(&ctx->hp, &http_parser_cb, data, total);
    //LOG_DEBUG("status_code %d\n", ctx->hp.status_code);
    //LOG_DEBUG("body:\n%s\n", ctx->body);
    //FIXME why mark end of data will crash. WTF
    data[total] = 0;
    khttp_dump_message_flow(data, total, 0);
    // Free receive buffer
end:
    free(data);
    return KHTTP_ERR_OK;
}

int khttp_perform(khttp_ctx *ctx)
{
    char *str = NULL;
    struct addrinfo hints;
    struct addrinfo *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    int res = 0;
    int ret = KHTTP_ERR_OK;
    char port[16];
    sprintf(port, "%d", ctx->port);
    if((res = getaddrinfo(ctx->host, port, &hints, &result)) != 0){
        LOG_ERROR("khttp DNS lookup failure. getaddrinfo: %s\n", gai_strerror(res));
        ret = -KHTTP_ERR_DNS;
        goto err;
    }
    ctx->serv_addr.sin_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
    ctx->serv_addr.sin_port = htons(ctx->port);
    //char addrstr[100];
    //inet_ntop (result->ai_family, &ctx->serv_addr.sin_addr, addrstr, 100);
    //LOG_DEBUG("IP:%s\n", addrstr);
    ctx->fd = khttp_socket_create();
    if(ctx->fd < 1){
        LOG_ERROR("khttp socket create error\n");
        ret = -KHTTP_ERR_SOCK;
        goto err1;
    }
    if(connect(ctx->fd, result->ai_addr, result->ai_addrlen)!= 0) {
        if(errno == -EINPROGRESS){
            //sleep(1);
        }else{
           LOG_ERROR("khttp connect to server error %d(%s)\n", errno, strerror(errno));
           ret = -KHTTP_ERR_CONNECT;
           goto err1;
        }
    }
    //LOG_DEBUG("khttp connect to server successfully\n");
    freeaddrinfo(result);
    if(ctx->proto == KHTTP_HTTPS){
#ifdef OPENSSL
        if(khttp_ssl_setup(ctx) != KHTTP_ERR_OK){
            LOG_ERROR("khttp ssl setup failure\n");
            return -KHTTP_ERR_SSL;
        }
        //LOG_DEBUG("khttp setup ssl connection successfully\n");
#else
        return -KHTTP_ERR_NOT_SUPP;
#endif
    }
    int count = 0;
    for(;;)
    {
        if(ctx->hp.status_code == 401){
            //LOG_DEBUG("Send HTTP authentication response\n");
            //FIXME change to khttp_send_http_auth
            if((res = khttp_send_http_auth(ctx)) != 0){
                LOG_ERROR("khttp send HTTP authentication response failure %d\n", res);
                break;
            }
            //FIXME
            ctx->hp.status_code = 0;
        }else if(ctx->hp.status_code == 200){
            if(ctx->cont == 1 && ctx->form != NULL){
                khttp_send_form(ctx);
                ctx->cont = 0;//Clean continue flag for next read
                goto end;//Send data then end
            }
        }else if(ctx->hp.status_code == 100){
            if(ctx->cont == 1 && ctx->form != NULL){
                khttp_send_form(ctx);
                ctx->cont = 0;//Clean continue flag for next read
            }
            //TODO What's next if no form or data to send
        }else{
            //LOG_DEBUG("Send HTTP request\n");
            if((res = khttp_send_http_req(ctx)) != 0){
                LOG_ERROR("khttp send HTTP request failure %d\n", res);
                break;
            }
        }
        //Free all header before recv data
        khttp_free_header(ctx);
        khttp_free_body(ctx);
        if((res = khttp_recv_http_resp(ctx)) != 0){
            LOG_ERROR("khttp recv HTTP response failure %d\n", res);
            ret = res;
            goto err;
        }
        //LOG_DEBUG("receive HTTP response success\n");
        switch(ctx->hp.status_code)
        {
            case 401:
                str = khttp_find_header(ctx, "WWW-Authenticate");
                if(khttp_parse_auth(ctx, str) != 0) {
                    LOG_ERROR("khttp parse auth string failure\n");
                    goto err;
                }
                if(count == 1 || (count == 0 && ctx->auth_type == KHTTP_AUTH_BASIC)){
                    goto end;
                }
                break;
            case 200:
                //LOG_INFO("GOT 200 OK count:%d\n", count);
                if(ctx->cont == 1 && count == 0){
                    //LOG_INFO("Got 200 OK before send post data/form\n");
                    break;
                }
                goto end;
                break;
            case 100:
                //LOG_INFO("GOT 100 Continue\n");
                if(ctx->cont == 1 && count == 0){
                    break;
                }
                //khttp_send_form(ctx);
                //Send form data...
                break;
            default:
                goto end;
                break;
        }
        // Session count
        count ++;
        //LOG_DEBUG("recv http data\n");
        //LOG_DEBUG("end\n%s\n", ctx->body);
        //printf("end\n%s\n", (char *)ctx->body);
    }
end:
    return ret;
err1:
    freeaddrinfo(result);
err:
    //khttp_dump_header(ctx);
    return ret;
}
