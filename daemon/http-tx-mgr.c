/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include "net.h"

#include <pthread.h>
#include <curl/curl.h>
#include <jansson.h>
#include <event2/buffer.h>

#ifdef WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#ifndef USE_GPL_CRYPTO
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/bio.h>
#include <openssl/asn1.h>
#include <openssl/ssl.h>
#endif

#include "seafile-config.h"

#include "seafile-session.h"
#include "http-tx-mgr.h"

#include "seafile-error-impl.h"
#include "utils.h"
#include "diff-simple.h"

#define DEBUG_FLAG SEAFILE_DEBUG_TRANSFER
#include "log.h"

#include "timer.h"

#define HTTP_OK 200
#define HTTP_BAD_REQUEST 400
#define HTTP_REQUEST_TIME_OUT 408
#define HTTP_FORBIDDEN 403
#define HTTP_NOT_FOUND 404
#define HTTP_NO_QUOTA 443
#define HTTP_REPO_DELETED 444
#define HTTP_REPO_TOO_LARGE 447
#define HTTP_REPO_CORRUPTED 445
#define HTTP_BLOCK_MISSING 446
#define HTTP_INTERNAL_SERVER_ERROR 500

#define RESET_BYTES_INTERVAL_MSEC 1000

#define CLEAR_POOL_ERR_CNT 3

#ifndef SEAFILE_CLIENT_VERSION
#define SEAFILE_CLIENT_VERSION PACKAGE_VERSION
#endif

#ifdef WIN32
#define USER_AGENT_OS "Windows NT"
#endif

#ifdef __APPLE__
#define USER_AGENT_OS "Apple OS X"
#endif

#ifdef __linux__
#define USER_AGENT_OS "Linux"
#endif

#if defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__ || defined __DragonFly__
#define USER_AGENT_OS "BSD"
#endif

struct _Connection {
    CURL *curl;
    gint64 ctime;               /* Used to clean up unused connection. */
    gboolean release;           /* If TRUE, the connection will be released. */
};
typedef struct _Connection Connection;

struct _ConnectionPool {
    char *host;
    GQueue *queue;
    pthread_mutex_t lock;
    int err_cnt;
};
typedef struct _ConnectionPool ConnectionPool;

struct _HttpTxPriv {
    GHashTable *download_tasks;
    GHashTable *upload_tasks;

    GHashTable *connection_pools; /* host -> connection pool */
    pthread_mutex_t pools_lock;

    SeafTimer *reset_bytes_timer;

    char *ca_bundle_path;
    char *env_ca_bundle_path;

    /* Regex to parse error message returned by update-branch. */
    GRegex *locked_error_regex;
    GRegex *folder_perm_error_regex;
    GRegex *too_many_files_error_regex;
};
typedef struct _HttpTxPriv HttpTxPriv;

/* Http Tx Task */

HttpTxTask *
http_tx_task_new (HttpTxManager *mgr,
                  const char *repo_id,
                  int repo_version,
                  int type,
                  gboolean is_clone,
                  const char *host,
                  const char *token,
                  const char *passwd,
                  const char *worktree)
{
    HttpTxTask *task = g_new0 (HttpTxTask, 1);

    task->manager = mgr;
    memcpy (task->repo_id, repo_id, 36);
    task->repo_version = repo_version;
    task->type = type;
    task->is_clone = is_clone;

    task->host = g_strdup(host);
    task->token = g_strdup(token);

    if (passwd)
        task->passwd = g_strdup(passwd);
    if (worktree)
        task->worktree = g_strdup(worktree);

    task->error = SYNC_ERROR_ID_NO_ERROR;

    return task;
}

void
http_tx_task_free (HttpTxTask *task)
{
    g_free (task->host);
    g_free (task->token);
    g_free (task->passwd);
    g_free (task->worktree);
    g_free (task->email);
    g_free (task->username);
    g_free (task->repo_name);
    if (task->type == HTTP_TASK_TYPE_DOWNLOAD) {
        if (task->blk_ref_cnts)
            g_hash_table_destroy (task->blk_ref_cnts);
    }
    g_free (task);
}

static const char *http_task_state_str[] = {
    "normal",
    "canceled",
    "finished",
    "error",
};

static const char *http_task_rt_state_str[] = {
    "init",
    "check",
    "commit",
    "fs",
    "data",
    "update-branch",
    "finished",
};

/* Http connection and connection pool. */

static Connection *
connection_new ()
{
    Connection *conn = g_new0 (Connection, 1);

    conn->curl = curl_easy_init();
    conn->ctime = (gint64)time(NULL);

    return conn;
}

static void
connection_free (Connection *conn)
{
    curl_easy_cleanup (conn->curl);
    g_free (conn);
}

static ConnectionPool *
connection_pool_new (const char *host)
{
    ConnectionPool *pool = g_new0 (ConnectionPool, 1);
    pool->host = g_strdup(host);
    pool->queue = g_queue_new ();
    pthread_mutex_init (&pool->lock, NULL);
    return pool;
}

static ConnectionPool *
find_connection_pool (HttpTxPriv *priv, const char *host)
{
    ConnectionPool *pool;

    pthread_mutex_lock (&priv->pools_lock);
    pool = g_hash_table_lookup (priv->connection_pools, host);
    if (!pool) {
        pool = connection_pool_new (host);
        g_hash_table_insert (priv->connection_pools, g_strdup(host), pool);
    }
    pthread_mutex_unlock (&priv->pools_lock);

    return pool;
}

static Connection *
connection_pool_get_connection (ConnectionPool *pool)
{
    Connection *conn = NULL;

    pthread_mutex_lock (&pool->lock);
    conn = g_queue_pop_head (pool->queue);
    if (!conn) {
        conn = connection_new ();
    }
    pthread_mutex_unlock (&pool->lock);

    return conn;
}

static void
connection_pool_clear (ConnectionPool *pool)
{
    Connection *conn = NULL;

    while (1) {
        conn = g_queue_pop_head (pool->queue);
        if (!conn)
            break;
        connection_free (conn);
    }
}

static void
connection_pool_return_connection (ConnectionPool *pool, Connection *conn)
{
    if (!conn)
        return;

    if (conn->release) {
        connection_free (conn);

        pthread_mutex_lock (&pool->lock);
        if (++pool->err_cnt >= CLEAR_POOL_ERR_CNT) {
            connection_pool_clear (pool);
        }
        pthread_mutex_unlock (&pool->lock);

        return;
    }

    curl_easy_reset (conn->curl);

    /* Reset error count when one connection succeeded. */
    pthread_mutex_lock (&pool->lock);
    pool->err_cnt = 0;
    g_queue_push_tail (pool->queue, conn);
    pthread_mutex_unlock (&pool->lock);
}

#define LOCKED_ERROR_PATTERN "File (.+) is locked"
#define FOLDER_PERM_ERROR_PATTERN "Update to path (.+) is not allowed by folder permission settings"
#define TOO_MANY_FILES_ERROR_PATTERN "Too many files in library"

HttpTxManager *
http_tx_manager_new (struct _SeafileSession *seaf)
{
    HttpTxManager *mgr = g_new0 (HttpTxManager, 1);
    HttpTxPriv *priv = g_new0 (HttpTxPriv, 1);
    const char *env_ca_path = NULL;

    mgr->seaf = seaf;

    priv->download_tasks = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free,
                                                  (GDestroyNotify)http_tx_task_free);
    priv->upload_tasks = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free,
                                                (GDestroyNotify)http_tx_task_free);

    priv->connection_pools = g_hash_table_new (g_str_hash, g_str_equal);
    pthread_mutex_init (&priv->pools_lock, NULL);

    priv->ca_bundle_path = g_build_filename (seaf->seaf_dir, "ca-bundle.pem", NULL);

    env_ca_path = g_getenv("SEAFILE_SSL_CA_PATH");
    if (env_ca_path)
        priv->env_ca_bundle_path = g_strdup (env_ca_path);

    GError *error = NULL;
    priv->locked_error_regex = g_regex_new (LOCKED_ERROR_PATTERN, 0, 0, &error);
    if (error) {
        seaf_warning ("Failed to create regex '%s': %s\n", LOCKED_ERROR_PATTERN, error->message);
        g_clear_error (&error);
    }

    priv->folder_perm_error_regex = g_regex_new (FOLDER_PERM_ERROR_PATTERN, 0, 0, &error);
    if (error) {
        seaf_warning ("Failed to create regex '%s': %s\n", FOLDER_PERM_ERROR_PATTERN, error->message);
        g_clear_error (&error);
    }

    priv->too_many_files_error_regex = g_regex_new (TOO_MANY_FILES_ERROR_PATTERN, 0, 0, &error);
    if (error) {
        seaf_warning ("Failed to create regex '%s': %s\n", TOO_MANY_FILES_ERROR_PATTERN, error->message);
        g_clear_error (&error);
    }

    mgr->priv = priv;

    return mgr;
}

static int
reset_bytes (void *vdata)
{
    HttpTxManager *mgr = vdata;
    HttpTxPriv *priv = mgr->priv;
    GHashTableIter iter;
    gpointer key, value;
    HttpTxTask *task;

    g_hash_table_iter_init (&iter, priv->download_tasks);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        task = value;
        task->last_tx_bytes = g_atomic_int_get (&task->tx_bytes);
        g_atomic_int_set (&task->tx_bytes, 0);
    }

    g_hash_table_iter_init (&iter, priv->upload_tasks);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        task = value;
        task->last_tx_bytes = g_atomic_int_get (&task->tx_bytes);
        g_atomic_int_set (&task->tx_bytes, 0);
    }

    return 1;
}

int
http_tx_manager_start (HttpTxManager *mgr)
{
#ifdef WIN32
    /* Remove existing ca-bundle file on start. */
    g_unlink (mgr->priv->ca_bundle_path);
#endif

    /* TODO: add a timer to clean up unused Http connections. */

    mgr->priv->reset_bytes_timer = seaf_timer_new (reset_bytes,
                                                   mgr,
                                                   RESET_BYTES_INTERVAL_MSEC);

    return 0;
}

/* Common Utility Functions. */

#ifndef USE_GPL_CRYPTO

#ifdef WIN32

/* static void */
/* write_cert_name_to_pem_file (FILE *f, PCCERT_CONTEXT pc) */
/* { */
/*     char *name; */
/*     DWORD size; */

/*     fprintf (f, "\n"); */

/*     if (!CertGetCertificateContextProperty(pc, */
/*                                            CERT_FRIENDLY_NAME_PROP_ID, */
/*                                            NULL, &size)) { */
/*         return; */
/*     } */

/*     name = g_malloc ((gsize)size); */
/*     if (!name) { */
/*         seaf_warning ("Failed to alloc memory\n"); */
/*         return; */
/*     } */

/*     if (!CertGetCertificateContextProperty(pc, */
/*                                            CERT_FRIENDLY_NAME_PROP_ID, */
/*                                            name, &size)) { */
/*         g_free (name); */
/*         return; */
/*     } */

/*     if (fwrite(name, (size_t)size, 1, f) != 1) { */
/*         seaf_warning ("Failed to write pem file.\n"); */
/*         g_free (name); */
/*         return; */
/*     } */
/*     fprintf (f, "\n"); */

/*     g_free (name); */
/* } */

static void
write_cert_to_pem_file (FILE *f, PCCERT_CONTEXT pc)
{
    const unsigned char *der = pc->pbCertEncoded;
    X509 *cert;

    /* write_cert_name_to_pem_file (f, pc); */

    cert = d2i_X509 (NULL, &der, (int)pc->cbCertEncoded);
    if (!cert) {
        seaf_warning ("Failed to parse certificate from DER.\n");
        return;
    }

    /* Don't add expired certs to pem file, otherwise openssl will
     * complain certificate expired.
     */
    if (X509_cmp_current_time (X509_get_notAfter(cert)) < 0)
        return;

    if (!PEM_write_X509 (f, cert)) {
        seaf_warning ("Failed to write certificate.\n");
        X509_free (cert);
        return;
    }

    X509_free (cert);
}

static int
load_ca_from_store (FILE *f, const wchar_t *store_name)
{
    HCERTSTORE store;

    store = CertOpenSystemStoreW (0, store_name);
    if (!store) {
        seaf_warning ("Failed to open system cert store: %lu\n", GetLastError());
        return -1;
    }

    PCCERT_CONTEXT pc = NULL;
    while (1) {
        pc = CertFindCertificateInStore (store, X509_ASN_ENCODING, 0, CERT_FIND_ANY, NULL, pc);
        if (!pc)
            break;
        write_cert_to_pem_file (f, pc);
    }

    CertCloseStore(store, 0);

    return 0;
}

static int
create_ca_bundle (const char *ca_bundle_path)
{
    FILE *f;
    int ret = 0;

    f = g_fopen (ca_bundle_path, "w+b");
    if (!f) {
        seaf_warning ("Failed to open cabundle file %s: %s\n",
                      ca_bundle_path, strerror(errno));
        return -1;
    }

    if (load_ca_from_store (f, L"ROOT") < 0) {
        seaf_warning ("Failed to load ca from ROOT store.\n");
        ret = -1;
        goto out;
    }

    if (load_ca_from_store (f, L"CA") < 0) {
        seaf_warning ("Failed to load ca from CA store.\n");
        ret = -1;
        goto out;
    }

out:
    fclose (f);
    return ret;
}

#endif	/* WIN32 */

#ifndef __linux__
static void
load_ca_bundle (CURL *curl)
{
    char *ca_bundle_path = seaf->http_tx_mgr->priv->ca_bundle_path;

    /* On MacOS the certs are loaded by seafile applet instead of seaf-daemon  */
    if (!seaf_util_exists (ca_bundle_path)) {
#ifdef WIN32
        if (create_ca_bundle (ca_bundle_path) < 0)
            return;
#else
        return;
#endif
    }

    curl_easy_setopt (curl, CURLOPT_CAINFO, ca_bundle_path);
}
#else

char *ca_paths[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/ssl/certs/ca-bundle.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/usr/share/ssl/certs/ca-bundle.crt",
    "/usr/local/share/certs/ca-root-nss.crt",
    "/etc/ssl/cert.pem",
};

static void
load_ca_bundle(CURL *curl)
{
    const char *env_ca_path = seaf->http_tx_mgr->priv->env_ca_bundle_path;
    int i;
    const char *ca_path;
    gboolean found = FALSE;

    for (i = 0; i < sizeof(ca_paths) / sizeof(ca_paths[0]); i++) {
        ca_path = ca_paths[i];
        if (seaf_util_exists (ca_path)) {
            found = TRUE;
            break;
        }
    }

    if (env_ca_path) {
        if (seaf_util_exists (env_ca_path)) {
            curl_easy_setopt (curl, CURLOPT_CAINFO, env_ca_path);
            return;
        }
    }

    if (found)
        curl_easy_setopt (curl, CURLOPT_CAINFO, ca_path);
}
#endif  /* __linux__ */

static gboolean
load_certs (sqlite3_stmt *stmt, void *vdata)
{
    X509_STORE *store = vdata;
    X509 *saved = NULL;
    const char *pem_b64;
    char *pem = NULL;
    BIO *b = NULL;
    gboolean ret = TRUE;

    pem_b64 = (const char *)sqlite3_column_text (stmt, 0);

    gsize len;
    pem = (char *)g_base64_decode (pem_b64, &len);
    if (!pem) {
        seaf_warning ("Failed to decode base64.\n");
        goto out;
    }

    b = BIO_new (BIO_s_mem());
    if (!b) {
        seaf_warning ("Failed to alloc BIO\n");
        goto out;
    }

    if (BIO_write (b, pem, len) != len) {
        seaf_warning ("Failed to write pem to BIO\n");
        goto out;
    }

    saved = PEM_read_bio_X509 (b, NULL, 0, NULL);
    if (!saved) {
        seaf_warning ("Failed to read PEM from BIO\n");
        goto out;
    }

    X509_STORE_add_cert (store, saved);

out:
    g_free (pem);
    if (b)
        BIO_free (b);
    if (saved)
        X509_free (saved);

    return ret;
}

static int
load_certs_from_db (X509_STORE *store)
{
    char *cert_db_path = NULL;
    sqlite3 *db = NULL;
    char *sql;
    int ret = 0;

    cert_db_path = g_build_filename (seaf->seaf_dir, "certs.db", NULL);
    if (sqlite_open_db (cert_db_path, &db) < 0) {
        seaf_warning ("Failed to open certs.db\n");
        ret = -1;
        goto out;
    }

    sql = "SELECT cert FROM Certs;";

    if (sqlite_foreach_selected_row (db, sql, load_certs, store) < 0) {
        ret = -1;
        goto out;
    }

out:
    g_free (cert_db_path);
    if (db)
        sqlite_close_db (db);

    return ret;
}

static CURLcode
ssl_callback (CURL *curl, void *ssl_ctx, void *userptr)
{
    SSL_CTX *ctx = ssl_ctx;
    X509_STORE *store;

    store = SSL_CTX_get_cert_store(ctx);

    /* Add all certs stored in db as trusted CA certs.
     * This workaround has one limitation though. The self-signed certs cannot
     * contain chain. It must be the CA itself.
     */
    load_certs_from_db (store);

    return CURLE_OK;
}

#endif  /* USE_GPL_CRYPTO */

static void
set_proxy (CURL *curl, gboolean is_https)
{
    /* Disable proxy if proxy options are not set properly. */
    if (!seaf->use_http_proxy || !seaf->http_proxy_type || !seaf->http_proxy_addr) {
        curl_easy_setopt (curl, CURLOPT_PROXY, NULL);
        return;
    }

    if (g_strcmp0(seaf->http_proxy_type, PROXY_TYPE_HTTP) == 0) {
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
        /* Use CONNECT method create a SSL tunnel if https is used. */
        if (is_https)
            curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
        curl_easy_setopt(curl, CURLOPT_PROXY, seaf->http_proxy_addr);
        curl_easy_setopt(curl, CURLOPT_PROXYPORT,
                         seaf->http_proxy_port > 0 ? seaf->http_proxy_port : 80);
        if (seaf->http_proxy_username && seaf->http_proxy_password) {
            curl_easy_setopt(curl, CURLOPT_PROXYAUTH,
                             CURLAUTH_BASIC |
                             CURLAUTH_DIGEST |
                             CURLAUTH_DIGEST_IE |
                             CURLAUTH_GSSNEGOTIATE |
                             CURLAUTH_NTLM);
            curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, seaf->http_proxy_username);
            curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, seaf->http_proxy_password);
        }
    } else if (g_strcmp0(seaf->http_proxy_type, PROXY_TYPE_SOCKS) == 0) {
        if (seaf->http_proxy_port < 0)
            return;
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
        curl_easy_setopt(curl, CURLOPT_PROXY, seaf->http_proxy_addr);
        curl_easy_setopt(curl, CURLOPT_PROXYPORT, seaf->http_proxy_port);
        if (seaf->http_proxy_username && g_strcmp0 (seaf->http_proxy_username, "") != 0)
            curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, seaf->http_proxy_username);
        if (seaf->http_proxy_password && g_strcmp0 (seaf->http_proxy_password, "") != 0)
            curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, seaf->http_proxy_password);
    }
}

#ifdef WIN32
static int
sockopt_callback (void *clientp, curl_socket_t curlfd, curlsocktype purpose)
{
    /* Set large enough TCP buffer size.
     * This greatly enhances sync speed for high latency network.
     * Windows by default use 8KB buffers, which is too small for such case.
     * Linux has auto-tuning for TCP buffers, so don't need to set manually.
     * OSX is TBD.
     */

#define DEFAULT_SNDBUF_SIZE (1 << 17) /* 128KB */

    /* Set send buffer size. */
    int sndbuf_size;
    socklen_t optlen;

    optlen = sizeof(int);
    getsockopt (curlfd, SOL_SOCKET, SO_SNDBUF, (char *)&sndbuf_size, &optlen);

    if (sndbuf_size < DEFAULT_SNDBUF_SIZE) {
        sndbuf_size = DEFAULT_SNDBUF_SIZE;
        optlen = sizeof(int);
        setsockopt (curlfd, SOL_SOCKET, SO_SNDBUF, (char *)&sndbuf_size, optlen);
    }

    /* Disable Nagle's algorithm. */
    int val = 1;
    optlen = sizeof(int);
    setsockopt (curlfd, IPPROTO_TCP, TCP_NODELAY, (char *)&val, optlen);

    return CURL_SOCKOPT_OK;
}
#endif  /* WIN32 */

typedef struct _HttpResponse {
    char *content;
    size_t size;
} HttpResponse;

static size_t
recv_response (void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    HttpResponse *rsp = userp;

    rsp->content = g_realloc (rsp->content, rsp->size + realsize);
    if (!rsp->content) {
        seaf_warning ("Not enough memory.\n");
        /* return a value other than realsize to signify an error. */
        return 0;
    }

    memcpy (rsp->content + rsp->size, contents, realsize);
    rsp->size += realsize;

    return realsize;
}

extern FILE *seafile_get_log_fp ();

#define FS_ID_LIST_TIMEOUT_SEC 1800
#define HTTP_TIMEOUT_SEC 300

/*
 * The @timeout parameter is for detecting network connection problems. 
 * The @timeout parameter should be set to TRUE for data-transfer-only operations,
 * such as getting objects, blocks. For operations that requires calculations
 * on the server side, the timeout should be set to FALSE. Otherwise when
 * the server sometimes takes more than 45 seconds to calculate the result,
 * the client will time out.
 */
static int
http_get (CURL *curl, const char *url, const char *token,
          int *rsp_status, char **rsp_content, gint64 *rsp_size,
          HttpRecvCallback callback, void *cb_data,
          gboolean timeout, int timeout_sec, int *pcurl_error)
{
    char *token_header;
    struct curl_slist *headers = NULL;
    int ret = 0;

    if (seafile_debug_flag_is_set (SEAFILE_DEBUG_CURL)) {
        curl_easy_setopt (curl, CURLOPT_VERBOSE, 1);
        curl_easy_setopt (curl, CURLOPT_STDERR, seafile_get_log_fp());
    }

    headers = curl_slist_append (headers, "User-Agent: Seafile/"SEAFILE_CLIENT_VERSION" ("USER_AGENT_OS")");

    if (token) {
        token_header = g_strdup_printf ("Seafile-Repo-Token: %s", token);
        headers = curl_slist_append (headers, token_header);
        g_free (token_header);
        token_header = g_strdup_printf ("Authorization: Token %s", token);
        headers = curl_slist_append (headers, token_header);
        g_free (token_header);
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (timeout) {
        /* Set low speed limit to 1 bytes. This effectively means no data. */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, timeout_sec);
    }

    if (seaf->disable_verify_certificate) {
        curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt (curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    HttpResponse rsp;
    memset (&rsp, 0, sizeof(rsp));
    if (rsp_content) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, recv_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rsp);
    } else if (callback) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, cb_data);
    }

    gboolean is_https = (strncasecmp(url, "https", strlen("https")) == 0);
    set_proxy (curl, is_https);

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

#ifndef USE_GPL_CRYPTO
    load_ca_bundle (curl);
#endif

#ifndef USE_GPL_CRYPTO
    if (!seaf->disable_verify_certificate) {
        curl_easy_setopt (curl, CURLOPT_SSL_CTX_FUNCTION, ssl_callback);
        curl_easy_setopt (curl, CURLOPT_SSL_CTX_DATA, url);
    }
#endif

#ifdef WIN32
    curl_easy_setopt (curl, CURLOPT_SOCKOPTFUNCTION, sockopt_callback);
#endif

    int rc = curl_easy_perform (curl);
    if (rc != 0) {
        seaf_warning ("libcurl failed to GET %s: %s.\n",
                      url, curl_easy_strerror(rc));
        if (pcurl_error)
            *pcurl_error = rc;
        ret = -1;
        goto out;
    }

    long status;
    rc = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK) {
        seaf_warning ("Failed to get status code for GET %s.\n", url);
        ret = -1;
        goto out;
    }

    *rsp_status = status;

    if (rsp_content) {
        *rsp_content = rsp.content;
        *rsp_size = rsp.size;
    }

out:
    if (ret < 0)
        g_free (rsp.content);
    curl_slist_free_all (headers);
    return ret;
}

typedef struct _HttpRequest {
    const char *content;
    const char *origin_content;
    size_t size;
    size_t origin_size;
} HttpRequest;

static size_t
send_request (void *ptr, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size *nmemb;
    size_t copy_size;
    HttpRequest *req = userp;

    if (req->size == 0)
        return 0;

    copy_size = MIN(req->size, realsize);
    memcpy (ptr, req->content, copy_size);
    req->size -= copy_size;
    req->content = req->content + copy_size;

    return copy_size;
}

// Only handle rewinding caused by redirects. In this case, offset and origin should both be 0.
// Do not handle rewinds triggered by other causes.
static size_t
rewind_http_request (void *clientp, curl_off_t offset, int origin)
{
    HttpRequest *req = clientp;
    if (offset == 0 && origin == 0) {
        req->content = req->origin_content;
        req->size = req->origin_size;
        return CURL_SEEKFUNC_OK;
    }

    return CURL_SEEKFUNC_FAIL;
}

typedef size_t (*HttpRewindCallback) (void *, curl_off_t, int);

typedef size_t (*HttpSendCallback) (void *, size_t, size_t, void *);

static int
http_put (CURL *curl, const char *url, const char *token,
          const char *req_content, gint64 req_size,
          HttpSendCallback callback, void *cb_data, HttpRewindCallback rewind_cb,
          int *rsp_status, char **rsp_content, gint64 *rsp_size,
          gboolean timeout, int *pcurl_error)
{
    char *token_header;
    struct curl_slist *headers = NULL;
    int ret = 0;

    if (seafile_debug_flag_is_set (SEAFILE_DEBUG_CURL)) {
        curl_easy_setopt (curl, CURLOPT_VERBOSE, 1);
        curl_easy_setopt (curl, CURLOPT_STDERR, seafile_get_log_fp());
    }

    headers = curl_slist_append (headers, "User-Agent: Seafile/"SEAFILE_CLIENT_VERSION" ("USER_AGENT_OS")");
    /* Disable the default "Expect: 100-continue" header */
    headers = curl_slist_append (headers, "Expect:");

    if (token) {
        token_header = g_strdup_printf ("Seafile-Repo-Token: %s", token);
        headers = curl_slist_append (headers, token_header);
        g_free (token_header);
        token_header = g_strdup_printf ("Authorization: Token %s", token);
        headers = curl_slist_append (headers, token_header);
        g_free (token_header);
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

    if (timeout) {
        /* Set low speed limit to 1 bytes. This effectively means no data. */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, HTTP_TIMEOUT_SEC);
    }

    if (seaf->disable_verify_certificate) {
        curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt (curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    HttpRequest req;
    if (req_content) {
        memset (&req, 0, sizeof(req));
        req.content = req_content;
        req.origin_content = req_content;
        req.size = req_size;
        req.origin_size = req_size;
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, send_request);
        curl_easy_setopt(curl, CURLOPT_READDATA, &req);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)req_size);

        curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, rewind_http_request);
        curl_easy_setopt(curl, CURLOPT_SEEKDATA, &req);
    } else if (callback != NULL) {
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, callback);
        curl_easy_setopt(curl, CURLOPT_READDATA, cb_data);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)req_size);

        if (rewind_cb != NULL) {
            curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, rewind_cb);
            curl_easy_setopt(curl, CURLOPT_SEEKDATA, cb_data);
        }
    } else {
        curl_easy_setopt (curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)0);
    }

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    HttpResponse rsp;
    memset (&rsp, 0, sizeof(rsp));
    if (rsp_content) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, recv_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rsp);
    }

    gboolean is_https = (strncasecmp(url, "https", strlen("https")) == 0);
    set_proxy (curl, is_https);

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

#ifndef USE_GPL_CRYPTO
    load_ca_bundle (curl);
#endif

#ifndef USE_GPL_CRYPTO
    if (!seaf->disable_verify_certificate) {
        curl_easy_setopt (curl, CURLOPT_SSL_CTX_FUNCTION, ssl_callback);
        curl_easy_setopt (curl, CURLOPT_SSL_CTX_DATA, url);
    }
#endif

#ifdef WIN32
    curl_easy_setopt (curl, CURLOPT_SOCKOPTFUNCTION, sockopt_callback);
#endif

    int rc = curl_easy_perform (curl);
    if (rc != 0) {
        seaf_warning ("libcurl failed to PUT %s: %s.\n",
                      url, curl_easy_strerror(rc));
        if (pcurl_error)
            *pcurl_error = rc;
        ret = -1;
        goto out;
    }

    long status;
    rc = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK) {
        seaf_warning ("Failed to get status code for PUT %s.\n", url);
        ret = -1;
        goto out;
    }

    *rsp_status = status;

    if (rsp_content) {
        *rsp_content = rsp.content;
        *rsp_size = rsp.size;
    }

out:
    if (ret < 0)
        g_free (rsp.content);
    curl_slist_free_all (headers);
    return ret;
}

static int
http_post (CURL *curl, const char *url, const char *token,
           const char *req_content, gint64 req_size,
           int *rsp_status, char **rsp_content, gint64 *rsp_size,
           gboolean timeout, int *pcurl_error)
{
    char *token_header;
    struct curl_slist *headers = NULL;
    int ret = 0;

    g_return_val_if_fail (req_content != NULL, -1);

    if (seafile_debug_flag_is_set (SEAFILE_DEBUG_CURL)) {
        curl_easy_setopt (curl, CURLOPT_VERBOSE, 1);
        curl_easy_setopt (curl, CURLOPT_STDERR, seafile_get_log_fp());
    }

    headers = curl_slist_append (headers, "User-Agent: Seafile/"SEAFILE_CLIENT_VERSION" ("USER_AGENT_OS")");
    /* Disable the default "Expect: 100-continue" header */
    headers = curl_slist_append (headers, "Expect:");

    if (token) {
        token_header = g_strdup_printf ("Seafile-Repo-Token: %s", token);
        headers = curl_slist_append (headers, token_header);
        g_free (token_header);
        token_header = g_strdup_printf ("Authorization: Token %s", token);
        headers = curl_slist_append (headers, token_header);
        g_free (token_header);
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    if (timeout) {
        /* Set low speed limit to 1 bytes. This effectively means no data. */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, HTTP_TIMEOUT_SEC);
    }

    if (seaf->disable_verify_certificate) {
        curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt (curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    HttpRequest req;
    memset (&req, 0, sizeof(req));
    req.content = req_content;
    req.origin_content = req_content;
    req.size = req_size;
    req.origin_size = req_size;
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, send_request);
    curl_easy_setopt(curl, CURLOPT_READDATA, &req);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req_size);

    curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, rewind_http_request);
    curl_easy_setopt(curl, CURLOPT_SEEKDATA, &req);

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    HttpResponse rsp;
    memset (&rsp, 0, sizeof(rsp));
    if (rsp_content) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, recv_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rsp);
    }

#ifndef USE_GPL_CRYPTO
    load_ca_bundle (curl);
#endif

#ifndef USE_GPL_CRYPTO
    if (!seaf->disable_verify_certificate) {
        curl_easy_setopt (curl, CURLOPT_SSL_CTX_FUNCTION, ssl_callback);
        curl_easy_setopt (curl, CURLOPT_SSL_CTX_DATA, url);
    }
#endif

    gboolean is_https = (strncasecmp(url, "https", strlen("https")) == 0);
    set_proxy (curl, is_https);

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* All POST requests should remain POST after redirect. */
    curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);

#ifdef WIN32
    curl_easy_setopt (curl, CURLOPT_SOCKOPTFUNCTION, sockopt_callback);
#endif

    int rc = curl_easy_perform (curl);
    if (rc != 0) {
        seaf_warning ("libcurl failed to POST %s: %s.\n",
                      url, curl_easy_strerror(rc));
        if (pcurl_error)
            *pcurl_error = rc;
        ret = -1;
        goto out;
    }

    long status;
    rc = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK) {
        seaf_warning ("Failed to get status code for POST %s.\n", url);
        ret = -1;
        goto out;
    }

    *rsp_status = status;

    if (rsp_content) {
        *rsp_content = rsp.content;
        *rsp_size = rsp.size;
    }

out:
    if (ret < 0)
        g_free (rsp.content);
    curl_slist_free_all (headers);
    return ret;
}

static int
http_error_to_http_task_error (int status)
{
    if (status == HTTP_BAD_REQUEST)
        /* This is usually a bug in the client. Set to general error. */
        return SYNC_ERROR_ID_GENERAL_ERROR;
    else if (status == HTTP_FORBIDDEN)
        return SYNC_ERROR_ID_ACCESS_DENIED;
    else if (status >= HTTP_INTERNAL_SERVER_ERROR)
        return SYNC_ERROR_ID_SERVER;
    else if (status == HTTP_NOT_FOUND)
        return SYNC_ERROR_ID_SERVER;
    else if (status == HTTP_NO_QUOTA)
        return SYNC_ERROR_ID_QUOTA_FULL;
    else if (status == HTTP_REPO_DELETED)
        return SYNC_ERROR_ID_SERVER_REPO_DELETED;
    else if (status == HTTP_REPO_CORRUPTED)
        return SYNC_ERROR_ID_SERVER_REPO_CORRUPT;
    else if (status == HTTP_REPO_TOO_LARGE || status == HTTP_REQUEST_TIME_OUT)
        return SYNC_ERROR_ID_LIBRARY_TOO_LARGE;
    else
        return SYNC_ERROR_ID_GENERAL_ERROR;
}

static void
handle_http_errors (HttpTxTask *task, int status)
{
    task->error = http_error_to_http_task_error (status);
}

static int
curl_error_to_http_task_error (int curl_error)
{
    if (curl_error == CURLE_SSL_CACERT ||
        curl_error == CURLE_PEER_FAILED_VERIFICATION)
        return SYNC_ERROR_ID_SSL;

    switch (curl_error) {
    case CURLE_COULDNT_RESOLVE_PROXY:
        return SYNC_ERROR_ID_RESOLVE_PROXY;
    case CURLE_COULDNT_RESOLVE_HOST:
        return SYNC_ERROR_ID_RESOLVE_HOST;
    case CURLE_COULDNT_CONNECT:
        return SYNC_ERROR_ID_CONNECT;
    case CURLE_OPERATION_TIMEDOUT:
        return SYNC_ERROR_ID_TX_TIMEOUT;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
        return SYNC_ERROR_ID_SSL;
    case CURLE_GOT_NOTHING:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
        return SYNC_ERROR_ID_TX;
    case CURLE_SEND_FAIL_REWIND:
        return SYNC_ERROR_ID_UNHANDLED_REDIRECT;
    default:
        return SYNC_ERROR_ID_NETWORK;
    }
}

static void
handle_curl_errors (HttpTxTask *task, int curl_error)
{
    task->error = curl_error_to_http_task_error (curl_error);
}

static void
emit_transfer_done_signal (HttpTxTask *task)
{
    if (task->type == HTTP_TASK_TYPE_DOWNLOAD)
        g_signal_emit_by_name (seaf, "repo-http-fetched", task);
    else
        g_signal_emit_by_name (seaf, "repo-http-uploaded", task);
}

static void
transition_state (HttpTxTask *task, int state, int rt_state)
{
    seaf_message ("Transfer repo '%.8s': ('%s', '%s') --> ('%s', '%s')\n",
                  task->repo_id,
                  http_task_state_to_str(task->state),
                  http_task_rt_state_to_str(task->runtime_state),
                  http_task_state_to_str(state),
                  http_task_rt_state_to_str(rt_state));

    if (state != task->state)
        task->state = state;
    task->runtime_state = rt_state;

    if (rt_state == HTTP_TASK_RT_STATE_FINISHED) {
        /* Clear download head info. */
        if (task->type == HTTP_TASK_TYPE_DOWNLOAD &&
            state == HTTP_TASK_STATE_FINISHED)
            seaf_repo_manager_set_repo_property (seaf->repo_mgr,
                                                 task->repo_id,
                                                 REPO_PROP_DOWNLOAD_HEAD,
                                                 EMPTY_SHA1);

        emit_transfer_done_signal (task);
    }
}

typedef struct {
    char *host;
    gboolean use_fileserver_port;
    HttpProtocolVersionCallback callback;
    void *user_data;

    gboolean success;
    gboolean not_supported;
    int version;
    int error_code;
} CheckProtocolData;

static int
parse_protocol_version (const char *rsp_content, int rsp_size, CheckProtocolData *data)
{
    json_t *object = NULL;
    json_error_t jerror;
    int version;

    object = json_loadb (rsp_content, rsp_size, 0, &jerror);
    if (!object) {
        seaf_warning ("Parse response failed: %s.\n", jerror.text);
        return -1;
    }

    if (json_object_has_member (object, "version")) {
        version = json_object_get_int_member (object, "version");
        data->version = version;
    } else {
        seaf_warning ("Response doesn't contain protocol version.\n");
        json_decref (object);
        return -1;
    }

    json_decref (object);
    return 0;
}

static void *
check_protocol_version_thread (void *vdata)
{
    CheckProtocolData *data = vdata;
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;

    pool = find_connection_pool (priv, data->host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", data->host);
        return vdata;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", data->host);
        return vdata;
    }

    curl = conn->curl;

    if (!data->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/protocol-version", data->host);
    else
        url = g_strdup_printf ("%s/protocol-version", data->host);

    int curl_error;
    if (http_get (curl, url, NULL, &status, &rsp_content, &rsp_size, NULL, NULL, TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        conn->release = TRUE;
        data->error_code = curl_error_to_http_task_error (curl_error);
        goto out;
    }

    data->success = TRUE;

    if (status == HTTP_OK) {
        if (rsp_size == 0)
            data->not_supported = TRUE;
        else if (parse_protocol_version (rsp_content, rsp_size, data) < 0)
            data->not_supported = TRUE;
    } else {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
        data->not_supported = TRUE;
        data->error_code = http_error_to_http_task_error (status);
    }

out:
    g_free (url);
    g_free (rsp_content);
    connection_pool_return_connection (pool, conn);

    return vdata;
}

static void
check_protocol_version_done (void *vdata)
{
    CheckProtocolData *data = vdata;
    HttpProtocolVersion result;

    memset (&result, 0, sizeof(result));
    result.check_success = data->success;
    result.not_supported = data->not_supported;
    result.version = data->version;
    result.error_code = data->error_code;

    data->callback (&result, data->user_data);

    g_free (data->host);
    g_free (data);
}

int
http_tx_manager_check_protocol_version (HttpTxManager *manager,
                                        const char *host,
                                        gboolean use_fileserver_port,
                                        HttpProtocolVersionCallback callback,
                                        void *user_data)
{
    CheckProtocolData *data = g_new0 (CheckProtocolData, 1);

    data->host = g_strdup(host);
    data->use_fileserver_port = use_fileserver_port;
    data->callback = callback;
    data->user_data = user_data;

    int ret = seaf_job_manager_schedule_job (seaf->job_mgr,
                                             check_protocol_version_thread,
                                             check_protocol_version_done,
                                             data);
    if (ret < 0) {
        g_free (data->host);
        g_free (data);
    }

    return ret;
}

typedef struct {
    char *host;
    gboolean use_notif_server_port;
    HttpNotifServerCallback callback;
    void *user_data;

    gboolean success;
    gboolean not_supported;
} CheckNotifServerData;

static void *
check_notif_server_thread (void *vdata)
{
    CheckNotifServerData *data = vdata;
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;

    pool = find_connection_pool (priv, data->host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", data->host);
        return vdata;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", data->host);
        return vdata;
    }

    curl = conn->curl;

    if (!data->use_notif_server_port)
        url = g_strdup_printf ("%s/notification/ping", data->host);
    else
        url = g_strdup_printf ("%s/ping", data->host);

    int curl_error;
    if (http_get (curl, url, NULL, &status, &rsp_content, &rsp_size, NULL, NULL, TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        conn->release = TRUE;
        goto out;
    }

    data->success = TRUE;

    if (status != HTTP_OK) {
        data->not_supported = TRUE;
    }

out:
    g_free (url);
    g_free (rsp_content);
    connection_pool_return_connection (pool, conn);

    return vdata;
}

static void
check_notif_server_done (void *vdata)
{
    CheckNotifServerData *data = vdata;

    data->callback ((data->success && !data->not_supported), data->user_data);

    g_free (data->host);
    g_free (data);
}

int
http_tx_manager_check_notif_server (HttpTxManager *manager,
                                    const char *host,
                                    gboolean use_notif_server_port,
                                    HttpNotifServerCallback callback,
                                    void *user_data)
{
    CheckNotifServerData *data = g_new0 (CheckNotifServerData, 1);

    data->host = g_strdup(host);
    data->use_notif_server_port = use_notif_server_port;
    data->callback = callback;
    data->user_data = user_data;

    int ret = seaf_job_manager_schedule_job (seaf->job_mgr,
                                             check_notif_server_thread,
                                             check_notif_server_done,
                                             data);
    if (ret < 0) {
        g_free (data->host);
        g_free (data);
    }

    return ret;
}

/* Check Head Commit. */

typedef struct {
    char repo_id[41];
    int repo_version;
    char *host;
    char *token;
    gboolean use_fileserver_port;
    HttpHeadCommitCallback callback;
    void *user_data;

    gboolean success;
    gboolean is_corrupt;
    gboolean is_deleted;
    char head_commit[41];
    int error_code;
} CheckHeadData;

static int
parse_head_commit_info (const char *rsp_content, int rsp_size, CheckHeadData *data)
{
    json_t *object = NULL;
    json_error_t jerror;
    const char *head_commit;

    object = json_loadb (rsp_content, rsp_size, 0, &jerror);
    if (!object) {
        seaf_warning ("Parse response failed: %s.\n", jerror.text);
        return -1;
    }

    if (json_object_has_member (object, "is_corrupted") &&
        json_object_get_int_member (object, "is_corrupted"))
        data->is_corrupt = TRUE;

    if (!data->is_corrupt) {
        head_commit = json_object_get_string_member (object, "head_commit_id");
        if (!head_commit) {
            seaf_warning ("Check head commit for repo %s failed. "
                          "Response doesn't contain head commit id.\n",
                          data->repo_id);
            json_decref (object);
            return -1;
        }
        memcpy (data->head_commit, head_commit, 40);
    }

    json_decref (object);
    return 0;
}

static void *
check_head_commit_thread (void *vdata)
{
    CheckHeadData *data = vdata;
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;

    pool = find_connection_pool (priv, data->host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", data->host);
        return vdata;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", data->host);
        return vdata;
    }

    curl = conn->curl;

    if (!data->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/commit/HEAD",
                               data->host, data->repo_id);
    else
        url = g_strdup_printf ("%s/repo/%s/commit/HEAD",
                               data->host, data->repo_id);

    int curl_error;
    if (http_get (curl, url, data->token, &status, &rsp_content, &rsp_size,
                  NULL, NULL, TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        conn->release = TRUE;
        data->error_code = curl_error_to_http_task_error (curl_error);
        goto out;
    }

    if (status == HTTP_OK) {
        if (parse_head_commit_info (rsp_content, rsp_size, data) < 0) {
            data->error_code = SYNC_ERROR_ID_NETWORK;
            goto out;
        }
        data->success = TRUE;
    } else if (status == HTTP_REPO_DELETED) {
        data->is_deleted = TRUE;
        data->success = TRUE;
    } else {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
        data->error_code = http_error_to_http_task_error (status);
    }

out:
    g_free (url);
    g_free (rsp_content);
    connection_pool_return_connection (pool, conn);
    return vdata;
}

static void
check_head_commit_done (void *vdata)
{
    CheckHeadData *data = vdata;
    HttpHeadCommit result;

    memset (&result, 0, sizeof(result));
    result.check_success = data->success;
    result.is_corrupt = data->is_corrupt;
    result.is_deleted = data->is_deleted;
    memcpy (result.head_commit, data->head_commit, 40);
    result.error_code = data->error_code;

    data->callback (&result, data->user_data);

    g_free (data->host);
    g_free (data->token);
    g_free (data);
}

int
http_tx_manager_check_head_commit (HttpTxManager *manager,
                                   const char *repo_id,
                                   int repo_version,
                                   const char *host,
                                   const char *token,
                                   gboolean use_fileserver_port,
                                   HttpHeadCommitCallback callback,
                                   void *user_data)
{
    CheckHeadData *data = g_new0 (CheckHeadData, 1);

    memcpy (data->repo_id, repo_id, 36);
    data->repo_version = repo_version;
    data->host = g_strdup(host);
    data->token = g_strdup(token);
    data->callback = callback;
    data->user_data = user_data;
    data->use_fileserver_port = use_fileserver_port;

    if (seaf_job_manager_schedule_job (seaf->job_mgr,
                                       check_head_commit_thread,
                                       check_head_commit_done,
                                       data) < 0) {
        g_free (data->host);
        g_free (data->token);
        g_free (data);
        return -1;
    }

    return 0;
}

/* Get folder permissions. */

void
http_folder_perm_req_free (HttpFolderPermReq *req)
{
    if (!req)
        return;
    g_free (req->token);
    g_free (req);
}

void
http_folder_perm_res_free (HttpFolderPermRes *res)
{
    GList *ptr;

    if (!res)
        return;
    for (ptr = res->user_perms; ptr; ptr = ptr->next)
        folder_perm_free ((FolderPerm *)ptr->data);
    for (ptr = res->group_perms; ptr; ptr = ptr->next)
        folder_perm_free ((FolderPerm *)ptr->data);
    g_free (res);
}

typedef struct {
    char *host;
    gboolean use_fileserver_port;
    GList *requests;
    HttpGetFolderPermsCallback callback;
    void *user_data;

    gboolean success;
    GList *results;
} GetFolderPermsData;

/* Make sure the path starts with '/' but doesn't end with '/'. */
static char *
canonical_perm_path (const char *path)
{
    int len = strlen(path);
    char *copy, *ret;

    if (strcmp (path, "/") == 0)
        return g_strdup(path);

    if (path[0] == '/' && path[len-1] != '/')
        return g_strdup(path);

    copy = g_strdup(path);

    if (copy[len-1] == '/')
        copy[len-1] = 0;

    if (copy[0] != '/')
        ret = g_strconcat ("/", copy, NULL);
    else
        ret = copy;

    return ret;
}

static GList *
parse_permission_list (json_t *array, gboolean *error)
{
    GList *ret = NULL, *ptr;
    json_t *object, *member;
    size_t n;
    int i;
    FolderPerm *perm;
    const char *str;

    *error = FALSE;

    n = json_array_size (array);
    for (i = 0; i < n; ++i) {
        object = json_array_get (array, i);

        perm = g_new0 (FolderPerm, 1);

        member = json_object_get (object, "path");
        if (!member) {
            seaf_warning ("Invalid folder perm response format: no path.\n");
            *error = TRUE;
            goto out;
        }
        str = json_string_value(member);
        if (!str) {
            seaf_warning ("Invalid folder perm response format: invalid path.\n");
            *error = TRUE;
            goto out;
        }
        perm->path = canonical_perm_path (str);

        member = json_object_get (object, "permission");
        if (!member) {
            seaf_warning ("Invalid folder perm response format: no permission.\n");
            *error = TRUE;
            goto out;
        }
        str = json_string_value(member);
        if (!str) {
            seaf_warning ("Invalid folder perm response format: invalid permission.\n");
            *error = TRUE;
            goto out;
        }
        perm->permission = g_strdup(str);

        ret = g_list_append (ret, perm);
    }

out:
    if (*error) {
        for (ptr = ret; ptr; ptr = ptr->next)
            folder_perm_free ((FolderPerm *)ptr->data);
        g_list_free (ret);
        ret = NULL;
    }

    return ret;
}

static int
parse_folder_perms (const char *rsp_content, int rsp_size, GetFolderPermsData *data)
{
    json_t *array = NULL, *object, *member;
    json_error_t jerror;
    size_t n;
    int i;
    GList *results = NULL, *ptr;
    HttpFolderPermRes *res;
    const char *repo_id;
    int ret = 0;
    gboolean error;

    array = json_loadb (rsp_content, rsp_size, 0, &jerror);
    if (!array) {
        seaf_warning ("Parse response failed: %s.\n", jerror.text);
        return -1;
    }

    n = json_array_size (array);
    for (i = 0; i < n; ++i) {
        object = json_array_get (array, i);

        res = g_new0 (HttpFolderPermRes, 1);

        member = json_object_get (object, "repo_id");
        if (!member) {
            seaf_warning ("Invalid folder perm response format: no repo_id.\n");
            ret = -1;
            goto out;
        }
        repo_id = json_string_value(member);
        if (strlen(repo_id) != 36) {
            seaf_warning ("Invalid folder perm response format: invalid repo_id.\n");
            ret = -1;
            goto out;
        }
        memcpy (res->repo_id, repo_id, 36);
 
        member = json_object_get (object, "ts");
        if (!member) {
            seaf_warning ("Invalid folder perm response format: no timestamp.\n");
            ret = -1;
            goto out;
        }
        res->timestamp = json_integer_value (member);

        member = json_object_get (object, "user_perms");
        if (!member) {
            seaf_warning ("Invalid folder perm response format: no user_perms.\n");
            ret = -1;
            goto out;
        }
        res->user_perms = parse_permission_list (member, &error);
        if (error) {
            ret = -1;
            goto out;
        }

        member = json_object_get (object, "group_perms");
        if (!member) {
            seaf_warning ("Invalid folder perm response format: no group_perms.\n");
            ret = -1;
            goto out;
        }
        res->group_perms = parse_permission_list (member, &error);
        if (error) {
            ret = -1;
            goto out;
        }

        results = g_list_append (results, res);
    }

out:
    json_decref (array);

    if (ret < 0) {
        for (ptr = results; ptr; ptr = ptr->next)
            http_folder_perm_res_free ((HttpFolderPermRes *)ptr->data);
        g_list_free (results);
    } else {
        data->results = results;
    }

    return ret;
}

static char *
compose_get_folder_perms_request (GList *requests)
{
    GList *ptr;
    HttpFolderPermReq *req;
    json_t *object, *array;
    char *req_str = NULL;

    array = json_array ();

    for (ptr = requests; ptr; ptr = ptr->next) {
        req = ptr->data;

        object = json_object ();
        json_object_set_new (object, "repo_id", json_string(req->repo_id));
        json_object_set_new (object, "token", json_string(req->token));
        json_object_set_new (object, "ts", json_integer(req->timestamp));

        json_array_append_new (array, object);
    }

    req_str = json_dumps (array, 0);
    if (!req_str) {
        seaf_warning ("Faile to json_dumps.\n");
    }

    json_decref (array);
    return req_str;
}

static void *
get_folder_perms_thread (void *vdata)
{
    GetFolderPermsData *data = vdata;
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    char *req_content = NULL;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;
    GList *ptr;

    pool = find_connection_pool (priv, data->host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", data->host);
        return vdata;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", data->host);
        return vdata;
    }

    curl = conn->curl;

    if (!data->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/folder-perm", data->host);
    else
        url = g_strdup_printf ("%s/repo/folder-perm", data->host);

    req_content = compose_get_folder_perms_request (data->requests);
    if (!req_content)
        goto out;

    if (http_post (curl, url, NULL, req_content, strlen(req_content),
                   &status, &rsp_content, &rsp_size, TRUE, NULL) < 0) {
        conn->release = TRUE;
        goto out;
    }

    if (status == HTTP_OK) {
        if (parse_folder_perms (rsp_content, rsp_size, data) < 0)
            goto out;
        data->success = TRUE;
    } else {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
    }

out:
    for (ptr = data->requests; ptr; ptr = ptr->next)
        http_folder_perm_req_free ((HttpFolderPermReq *)ptr->data);
    g_list_free (data->requests);

    g_free (url);
    g_free (req_content);
    g_free (rsp_content);
    connection_pool_return_connection (pool, conn);
    return vdata;
}

static void
get_folder_perms_done (void *vdata)
{
    GetFolderPermsData *data = vdata;
    HttpFolderPerms cb_data;

    memset (&cb_data, 0, sizeof(cb_data));
    cb_data.success = data->success;
    cb_data.results = data->results;

    data->callback (&cb_data, data->user_data);

    GList *ptr;
    for (ptr = data->results; ptr; ptr = ptr->next)
        http_folder_perm_res_free ((HttpFolderPermRes *)ptr->data);
    g_list_free (data->results);

    g_free (data->host);
    g_free (data);
}

int
http_tx_manager_get_folder_perms (HttpTxManager *manager,
                                  const char *host,
                                  gboolean use_fileserver_port,
                                  GList *folder_perm_requests,
                                  HttpGetFolderPermsCallback callback,
                                  void *user_data)
{
    GetFolderPermsData *data = g_new0 (GetFolderPermsData, 1);

    data->host = g_strdup(host);
    data->requests = folder_perm_requests;
    data->callback = callback;
    data->user_data = user_data;
    data->use_fileserver_port = use_fileserver_port;

    if (seaf_job_manager_schedule_job (seaf->job_mgr,
                                       get_folder_perms_thread,
                                       get_folder_perms_done,
                                       data) < 0) {
        g_free (data->host);
        g_free (data);
        return -1;
    }

    return 0;
}

/* Get Locked Files. */

void
http_locked_files_req_free (HttpLockedFilesReq *req)
{
    if (!req)
        return;
    g_free (req->token);
    g_free (req);
}

void
http_locked_files_res_free (HttpLockedFilesRes *res)
{
    if (!res)
        return;

    g_hash_table_destroy (res->locked_files);
    g_free (res);
}

typedef struct {
    char *host;
    gboolean use_fileserver_port;
    GList *requests;
    HttpGetLockedFilesCallback callback;
    void *user_data;

    gboolean success;
    GList *results;
} GetLockedFilesData;

static GHashTable *
parse_locked_file_list (json_t *array)
{
    GHashTable *ret = NULL;
    size_t n, i;
    json_t *obj, *string, *integer;

    ret = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    if (!ret) {
        return NULL;
    }

    n = json_array_size (array);
    for (i = 0; i < n; ++i) {
        obj = json_array_get (array, i);
        string = json_object_get (obj, "path");
        if (!string) {
            g_hash_table_destroy (ret);
            return NULL;
        }
        integer = json_object_get (obj, "by_me");
        if (!integer) {
            g_hash_table_destroy (ret);
            return NULL;
        }
        g_hash_table_insert (ret,
                             g_strdup(json_string_value(string)),
                             (void*)(long)json_integer_value(integer));
    }

    return ret;
}

static int
parse_locked_files (const char *rsp_content, int rsp_size, GetLockedFilesData *data)
{
    json_t *array = NULL, *object, *member;
    json_error_t jerror;
    size_t n;
    int i;
    GList *results = NULL;
    HttpLockedFilesRes *res;
    const char *repo_id;
    int ret = 0;

    array = json_loadb (rsp_content, rsp_size, 0, &jerror);
    if (!array) {
        seaf_warning ("Parse response failed: %s.\n", jerror.text);
        return -1;
    }

    n = json_array_size (array);
    for (i = 0; i < n; ++i) {
        object = json_array_get (array, i);

        res = g_new0 (HttpLockedFilesRes, 1);

        member = json_object_get (object, "repo_id");
        if (!member) {
            seaf_warning ("Invalid locked files response format: no repo_id.\n");
            ret = -1;
            goto out;
        }
        repo_id = json_string_value(member);
        if (strlen(repo_id) != 36) {
            seaf_warning ("Invalid locked files response format: invalid repo_id.\n");
            ret = -1;
            goto out;
        }
        memcpy (res->repo_id, repo_id, 36);
 
        member = json_object_get (object, "ts");
        if (!member) {
            seaf_warning ("Invalid locked files response format: no timestamp.\n");
            ret = -1;
            goto out;
        }
        res->timestamp = json_integer_value (member);

        member = json_object_get (object, "locked_files");
        if (!member) {
            seaf_warning ("Invalid locked files response format: no locked_files.\n");
            ret = -1;
            goto out;
        }

        res->locked_files = parse_locked_file_list (member);
        if (res->locked_files == NULL) {
            ret = -1;
            goto out;
        }

        results = g_list_append (results, res);
    }

out:
    json_decref (array);

    if (ret < 0) {
        g_list_free_full (results, (GDestroyNotify)http_locked_files_res_free);
    } else {
        data->results = results;
    }

    return ret;
}

static char *
compose_get_locked_files_request (GList *requests)
{
    GList *ptr;
    HttpLockedFilesReq *req;
    json_t *object, *array;
    char *req_str = NULL;

    array = json_array ();

    for (ptr = requests; ptr; ptr = ptr->next) {
        req = ptr->data;

        object = json_object ();
        json_object_set_new (object, "repo_id", json_string(req->repo_id));
        json_object_set_new (object, "token", json_string(req->token));
        json_object_set_new (object, "ts", json_integer(req->timestamp));

        json_array_append_new (array, object);
    }

    req_str = json_dumps (array, 0);
    if (!req_str) {
        seaf_warning ("Faile to json_dumps.\n");
    }

    json_decref (array);
    return req_str;
}

static void *
get_locked_files_thread (void *vdata)
{
    GetLockedFilesData *data = vdata;
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    char *req_content = NULL;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;

    pool = find_connection_pool (priv, data->host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", data->host);
        return vdata;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", data->host);
        return vdata;
    }

    curl = conn->curl;

    if (!data->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/locked-files", data->host);
    else
        url = g_strdup_printf ("%s/repo/locked-files", data->host);

    req_content = compose_get_locked_files_request (data->requests);
    if (!req_content)
        goto out;

    if (http_post (curl, url, NULL, req_content, strlen(req_content),
                   &status, &rsp_content, &rsp_size, TRUE, NULL) < 0) {
        conn->release = TRUE;
        goto out;
    }

    if (status == HTTP_OK) {
        if (parse_locked_files (rsp_content, rsp_size, data) < 0)
            goto out;
        data->success = TRUE;
    } else {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
    }

out:
    g_list_free_full (data->requests, (GDestroyNotify)http_locked_files_req_free);

    g_free (url);
    g_free (req_content);
    g_free (rsp_content);
    connection_pool_return_connection (pool, conn);
    return vdata;
}

static void
get_locked_files_done (void *vdata)
{
    GetLockedFilesData *data = vdata;
    HttpLockedFiles cb_data;

    memset (&cb_data, 0, sizeof(cb_data));
    cb_data.success = data->success;
    cb_data.results = data->results;

    data->callback (&cb_data, data->user_data);

    g_list_free_full (data->results, (GDestroyNotify)http_locked_files_res_free);

    g_free (data->host);
    g_free (data);
}

int
http_tx_manager_get_locked_files (HttpTxManager *manager,
                                  const char *host,
                                  gboolean use_fileserver_port,
                                  GList *locked_files_requests,
                                  HttpGetLockedFilesCallback callback,
                                  void *user_data)
{
    GetLockedFilesData *data = g_new0 (GetLockedFilesData, 1);

    data->host = g_strdup(host);
    data->requests = locked_files_requests;
    data->callback = callback;
    data->user_data = user_data;
    data->use_fileserver_port = use_fileserver_port;

    if (seaf_job_manager_schedule_job (seaf->job_mgr,
                                       get_locked_files_thread,
                                       get_locked_files_done,
                                       data) < 0) {
        g_free (data->host);
        g_free (data);
        return -1;
    }

    return 0;
}

/* Synchronous interfaces for locking/unlocking a file on the server. */

int
http_tx_manager_lock_file (HttpTxManager *manager,
                           const char *host,
                           gboolean use_fileserver_port,
                           const char *token,
                           const char *repo_id,
                           const char *path)
{
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    int status;
    int ret = 0;

    pool = find_connection_pool (priv, host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", host);
        return -1;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", host);
        return -1;
    }

    curl = conn->curl;

    char *esc_path = g_uri_escape_string(path, NULL, FALSE);
    if (!use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/lock-file?p=%s", host, repo_id, esc_path);
    else
        url = g_strdup_printf ("%s/repo/%s/lock-file?p=%s", host, repo_id, esc_path);
    g_free (esc_path);

    if (http_put (curl, url, token, NULL, 0, NULL, NULL, NULL,
                  &status, NULL, NULL, TRUE, NULL) < 0) {
        conn->release = TRUE;
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for PUT %s: %d.\n", url, status);
        ret = -1;
    }

out:
    g_free (url);
    connection_pool_return_connection (pool, conn);
    return ret;
}

int
http_tx_manager_unlock_file (HttpTxManager *manager,
                             const char *host,
                             gboolean use_fileserver_port,
                             const char *token,
                             const char *repo_id,
                             const char *path)
{
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    int status;
    int ret = 0;

    pool = find_connection_pool (priv, host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", host);
        return -1;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", host);
        return -1;
    }

    curl = conn->curl;

    char *esc_path = g_uri_escape_string(path, NULL, FALSE);
    if (!use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/unlock-file?p=%s", host, repo_id, esc_path);
    else
        url = g_strdup_printf ("%s/repo/%s/unlock-file?p=%s", host, repo_id, esc_path);
    g_free (esc_path);

    if (http_put (curl, url, token, NULL, 0, NULL, NULL, NULL,
                  &status, NULL, NULL, TRUE, NULL) < 0) {
        conn->release = TRUE;
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for PUT %s: %d.\n", url, status);
        ret = -1;
    }

out:
    g_free (url);
    connection_pool_return_connection (pool, conn);
    return ret;
}

typedef struct {
    char *host;
    char *url;
    char *api_token;
    HttpAPIGetCallback callback;
    void *user_data;

    gboolean success;
    char *rsp_content;
    int rsp_size;
    int error_code;
    int http_status;
} APIGetData;

static void *
fileserver_api_get_request (void *vdata)
{
    APIGetData *data = vdata;
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;


    pool = find_connection_pool (priv, data->host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", data->host);
        return vdata;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", data->host);
        return vdata;
    }

    curl = conn->curl;

    int curl_error;
    if (http_get (curl, data->url, data->api_token, &status, &rsp_content, &rsp_size, NULL, NULL,
                  TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        conn->release = TRUE;
        data->error_code = curl_error_to_http_task_error (curl_error);
        goto out;
    }

    data->rsp_content = rsp_content;
    data->rsp_size = rsp_size;

    if (status == HTTP_OK) {
        data->success = TRUE;
    } else {
        seaf_warning ("Bad response code for GET %s: %d.\n", data->url, status);
        data->error_code = http_error_to_http_task_error (status);
        data->http_status = status;
    }

out:
    connection_pool_return_connection (pool, conn);
    return vdata;

}

static void
fileserver_api_get_request_done (void *vdata)
{
    APIGetData *data = vdata;
    HttpAPIGetResult cb_data;

    memset (&cb_data, 0, sizeof(cb_data));
    cb_data.success = data->success;
    cb_data.rsp_content = data->rsp_content;
    cb_data.rsp_size = data->rsp_size;
    cb_data.error_code = data->error_code;
    cb_data.http_status = data->http_status;

    data->callback (&cb_data, data->user_data);

    g_free (data->rsp_content);
    g_free (data->host);
    g_free (data->url);
    g_free (data->api_token);
    g_free (data);
}

int
http_tx_manager_fileserver_api_get  (HttpTxManager *manager,
                                     const char *host,
                                     const char *url,
                                     const char *api_token,
                                     HttpAPIGetCallback callback,
                                     void *user_data)
{
    APIGetData *data = g_new0 (APIGetData, 1);

    data->host = g_strdup(host);
    data->url = g_strdup(url);
    data->api_token = g_strdup(api_token);
    data->callback = callback;
    data->user_data = user_data;

    if (seaf_job_manager_schedule_job (seaf->job_mgr,
                                       fileserver_api_get_request,
                                       fileserver_api_get_request_done,
                                       data) < 0) {
        g_free (data->rsp_content);
        g_free (data->host);
        g_free (data->url);
        g_free (data->api_token);
        g_free (data);
        return -1;
    }

    return 0;
}

static char *
repo_id_list_to_json (GList *repo_id_list)
{
    json_t *array = json_array();
    GList *ptr;
    char *repo_id;

    for (ptr = repo_id_list; ptr; ptr = ptr->next) {
        repo_id = ptr->data;
        json_array_append_new (array, json_string(repo_id));
    }

    char *data = json_dumps (array, JSON_COMPACT);
    if (!data) {
        seaf_warning ("Failed to dump json.\n");
        json_decref (array);
        return NULL;
    }

    json_decref (array);
    return data;
}

static GHashTable *
repo_head_commit_map_from_json (const char *json_str, gint64 len)
{
    json_t *object;
    json_error_t jerror;
    GHashTable *ret;

    object = json_loadb (json_str, (size_t)len, 0, &jerror);
    if (!object) {
        seaf_warning ("Failed to load json: %s\n", jerror.text);
        return NULL;
    }

    ret = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    void *iter = json_object_iter (object);
    const char *key;
    json_t *value;
    while (iter) {
        key = json_object_iter_key (iter);
        value = json_object_iter_value (iter);
        if (json_typeof(value) != JSON_STRING) {
            seaf_warning ("Bad json object format when parsing head commit id map.\n");
            g_hash_table_destroy (ret);
            goto out;
        }
        g_hash_table_replace (ret, g_strdup (key), g_strdup(json_string_value(value)));
        iter = json_object_iter_next (object, iter);
    }

out:
    json_decref (object);
    return ret;
}

GHashTable *
http_tx_manager_get_head_commit_ids (HttpTxManager *manager,
                                     const char *host,
                                     gboolean use_fileserver_port,
                                     GList *repo_id_list,
                                     int *ret_status)
{
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    char *req_content = NULL;
    gint64 req_size;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;
    GHashTable *map = NULL;

    pool = find_connection_pool (priv, host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", host);
        return NULL;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", host);
        return NULL;
    }

    curl = conn->curl;

    if (!use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/head-commits-multi/", host);
    else
        url = g_strdup_printf ("%s/repo/head-commits-multi/", host);

    req_content = repo_id_list_to_json (repo_id_list);
    req_size = strlen(req_content);

    if (http_post (curl, url, NULL, req_content, req_size,
                   &status, &rsp_content, &rsp_size, TRUE, NULL) < 0) {
        conn->release = TRUE;
        goto out;
    }

    *ret_status = status;

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for POST %s: %d\n", url, status);
        goto out;
    }

    map = repo_head_commit_map_from_json (rsp_content, rsp_size);

out:
    g_free (url);
    connection_pool_return_connection (pool, conn);
    /* returned by json_dumps(). */
    free (req_content);
    g_free (rsp_content);
    return map;
}

static gboolean
remove_task_help (gpointer key, gpointer value, gpointer user_data)
{
    HttpTxTask *task = value;
    const char *repo_id = user_data;

    if (strcmp(task->repo_id, repo_id) != 0)
        return FALSE;

    return TRUE;
}

static void
clean_tasks_for_repo (HttpTxManager *manager, const char *repo_id)
{
    g_hash_table_foreach_remove (manager->priv->download_tasks,
                                 remove_task_help, (gpointer)repo_id);

    g_hash_table_foreach_remove (manager->priv->upload_tasks,
                                 remove_task_help, (gpointer)repo_id);
}

static int
check_permission (HttpTxTask *task, Connection *conn)
{
    CURL *curl;
    char *url;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;
    int ret = 0;
    json_t *rsp_obj = NULL, *reason = NULL, *unsyncable_path = NULL;
    const char *reason_str = NULL, *unsyncable_path_str = NULL;
    json_error_t jerror;

    curl = conn->curl;

    const char *type = (task->type == HTTP_TASK_TYPE_DOWNLOAD) ? "download" : "upload";
    const char *url_prefix = (task->use_fileserver_port) ? "" : "seafhttp/";
    if (seaf->client_name) {
        char *client_name = g_uri_escape_string (seaf->client_name,
                                                 NULL, FALSE);
        url = g_strdup_printf ("%s/%srepo/%s/permission-check/?op=%s"
                               "&client_id=%s&client_name=%s",
                               task->host, url_prefix, task->repo_id, type,
                               seaf->client_id, client_name);
        g_free (client_name);
    } else {
        url = g_strdup_printf ("%s/%srepo/%s/permission-check/?op=%s",
                               task->host, url_prefix, task->repo_id, type);
    }

    int curl_error;
    if (http_get (curl, url, task->token, &status, &rsp_content, &rsp_size, NULL, NULL, TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        conn->release = TRUE;
        handle_curl_errors (task, curl_error);
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);

        if (status != HTTP_FORBIDDEN || !rsp_content) {
            handle_http_errors (task, status);
            ret = -1;
            goto out;
        }

        rsp_obj = json_loadb (rsp_content, rsp_size, 0 ,&jerror);
        if (!rsp_obj) {
            seaf_warning ("Parse check permission response failed: %s.\n", jerror.text);
            handle_http_errors (task, status);
            json_decref (rsp_obj);
            ret = -1;
            goto out;
        }

        reason = json_object_get (rsp_obj, "reason");
        if (!reason) {
            handle_http_errors (task, status);
            json_decref (rsp_obj);
            ret = -1;
            goto out;
        }

        reason_str = json_string_value (reason);
        if (g_strcmp0 (reason_str, "no write permission") == 0) {
            task->error = SYNC_ERROR_ID_NO_WRITE_PERMISSION;
        } else if (g_strcmp0 (reason_str, "unsyncable share permission") == 0) {
            task->error = SYNC_ERROR_ID_PERM_NOT_SYNCABLE;

            unsyncable_path = json_object_get (rsp_obj, "unsyncable_path");
            if (!unsyncable_path) {
                json_decref (rsp_obj);
                ret = -1;
                goto out;
            }

            unsyncable_path_str = json_string_value (unsyncable_path);
            if (unsyncable_path_str)
                send_file_sync_error_notification (task->repo_id, task->repo_name,
                                                   unsyncable_path_str,
                                                   SYNC_ERROR_ID_PERM_NOT_SYNCABLE);
        } else {
            task->error = SYNC_ERROR_ID_ACCESS_DENIED;
        }

        ret = -1;
    }

out:
    g_free (url);
    g_free (rsp_content);
    curl_easy_reset (curl);

    return ret;
}

/* Upload. */

static void *http_upload_thread (void *vdata);
static void http_upload_done (void *vdata);

int
http_tx_manager_add_upload (HttpTxManager *manager,
                            const char *repo_id,
                            int repo_version,
                            const char *host,
                            const char *token,
                            int protocol_version,
                            gboolean use_fileserver_port,
                            GError **error)
{
    HttpTxTask *task;
    SeafRepo *repo;

    if (!repo_id) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Empty argument(repo_id)");
        return -1;
    }

    if (!token) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Empty argument(token)");
        return -1;
    }

    if (!host) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Empty argument(host)");
        return -1;
    }

    repo = seaf_repo_manager_get_repo (seaf->repo_mgr, repo_id);
    if (!repo) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Repo not found");
        return -1;
    }

    clean_tasks_for_repo (manager, repo_id);

    task = http_tx_task_new (manager, repo_id, repo_version,
                             HTTP_TASK_TYPE_UPLOAD, FALSE,
                             host, token, NULL, NULL);

    task->protocol_version = protocol_version;

    task->state = HTTP_TASK_STATE_NORMAL;

    task->use_fileserver_port = use_fileserver_port;

    task->repo_name = g_strdup(repo->name);

    g_hash_table_insert (manager->priv->upload_tasks,
                         g_strdup(repo_id),
                         task);

    if (seaf_job_manager_schedule_job (seaf->job_mgr,
                                       http_upload_thread,
                                       http_upload_done,
                                       task) < 0) {
        g_hash_table_remove (manager->priv->upload_tasks, repo_id);
        return -1;
    }

    return 0;
}

static gboolean
dirent_same (SeafDirent *dent1, SeafDirent *dent2)
{
    return (strcmp(dent1->id, dent2->id) == 0 &&
            dent1->mode == dent2->mode &&
            dent1->mtime == dent2->mtime);
}

typedef struct {
    HttpTxTask *task;
    gint64 delta;
    GHashTable *active_paths;
} CalcQuotaDeltaData;

static int
check_quota_and_active_paths_diff_files (int n, const char *basedir,
                                         SeafDirent *files[], void *vdata)
{
    CalcQuotaDeltaData *data = vdata;
    SeafDirent *file1 = files[0];
    SeafDirent *file2 = files[1];
    gint64 size1, size2;
    char *path;

    if (file1 && file2) {
        size1 = file1->size;
        size2 = file2->size;
        data->delta += (size1 - size2);

        if (!dirent_same (file1, file2)) {
            path = g_strconcat(basedir, file1->name, NULL);
            g_hash_table_replace (data->active_paths, path, (void*)(long)S_IFREG);
        }
    } else if (file1 && !file2) {
        data->delta += file1->size;

        path = g_strconcat (basedir, file1->name, NULL);
        g_hash_table_replace (data->active_paths, path, (void*)(long)S_IFREG);
    } else if (!file1 && file2) {
        data->delta -= file2->size;
    }

    return 0;
}

static int
check_quota_and_active_paths_diff_dirs (int n, const char *basedir,
                                        SeafDirent *dirs[], void *vdata,
                                        gboolean *recurse)
{
    CalcQuotaDeltaData *data = vdata;
    SeafDirent *dir1 = dirs[0];
    SeafDirent *dir2 = dirs[1];
    char *path;

    /* When a new empty dir is created, or a dir became empty. */
    if ((!dir2 && dir1 && strcmp(dir1->id, EMPTY_SHA1) == 0) ||
	(dir2 && dir1 && strcmp(dir1->id, EMPTY_SHA1) == 0 && strcmp(dir2->id, EMPTY_SHA1) != 0)) {
        path = g_strconcat (basedir, dir1->name, NULL);
        g_hash_table_replace (data->active_paths, path, (void*)(long)S_IFDIR);
    }

    return 0;
}

static int
calculate_upload_size_delta_and_active_paths (HttpTxTask *task,
                                              gint64 *delta,
                                              GHashTable *active_paths)
{
    int ret = 0;
    SeafBranch *local = NULL, *master = NULL;
    SeafCommit *local_head = NULL, *master_head = NULL;

    local = seaf_branch_manager_get_branch (seaf->branch_mgr, task->repo_id, "local");
    if (!local) {
        seaf_warning ("Branch local not found for repo %.8s.\n", task->repo_id);
        ret = -1;
        goto out;
    }
    master = seaf_branch_manager_get_branch (seaf->branch_mgr, task->repo_id, "master");
    if (!master) {
        seaf_warning ("Branch master not found for repo %.8s.\n", task->repo_id);
        ret = -1;
        goto out;
    }

    local_head = seaf_commit_manager_get_commit (seaf->commit_mgr,
                                                 task->repo_id, task->repo_version,
                                                 local->commit_id);
    if (!local_head) {
        seaf_warning ("Local head commit not found for repo %.8s.\n",
                      task->repo_id);
        ret = -1;
        goto out;
    }
    master_head = seaf_commit_manager_get_commit (seaf->commit_mgr,
                                                 task->repo_id, task->repo_version,
                                                 master->commit_id);
    if (!master_head) {
        seaf_warning ("Master head commit not found for repo %.8s.\n",
                      task->repo_id);
        ret = -1;
        goto out;
    }

    CalcQuotaDeltaData data;
    memset (&data, 0, sizeof(data));
    data.task = task;
    data.active_paths = active_paths;

    DiffOptions opts;
    memset (&opts, 0, sizeof(opts));
    memcpy (opts.store_id, task->repo_id, 36);
    opts.version = task->repo_version;
    opts.file_cb = check_quota_and_active_paths_diff_files;
    opts.dir_cb = check_quota_and_active_paths_diff_dirs;
    opts.data = &data;

    const char *trees[2];
    trees[0] = local_head->root_id;
    trees[1] = master_head->root_id;
    if (diff_trees (2, trees, &opts) < 0) {
        seaf_warning ("Failed to diff local and master head for repo %.8s.\n",
                      task->repo_id);
        ret = -1;
        goto out;
    }

    *delta = data.delta;

out:
    seaf_branch_unref (local);
    seaf_branch_unref (master);
    seaf_commit_unref (local_head);
    seaf_commit_unref (master_head);

    return ret;
}

static int
check_quota (HttpTxTask *task, Connection *conn, gint64 delta)
{
    CURL *curl;
    char *url;
    int status;
    int ret = 0;

    curl = conn->curl;

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/quota-check/?delta=%"G_GINT64_FORMAT"",
                               task->host, task->repo_id, delta);
    else
        url = g_strdup_printf ("%s/repo/%s/quota-check/?delta=%"G_GINT64_FORMAT"",
                               task->host, task->repo_id, delta);

    int curl_error;
    if (http_get (curl, url, task->token, &status, NULL, NULL, NULL, NULL, TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        conn->release = TRUE;
        handle_curl_errors (task, curl_error);
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
        handle_http_errors (task, status);
        ret = -1;
    }

out:
    g_free (url);
    curl_easy_reset (curl);

    return ret;
}

static int
send_commit_object (HttpTxTask *task, Connection *conn)
{
    CURL *curl;
    char *url;
    int status;
    char *data;
    int len;
    int ret = 0;

    if (seaf_obj_store_read_obj (seaf->commit_mgr->obj_store,
                                 task->repo_id, task->repo_version,
                                 task->head, (void**)&data, &len) < 0) {
        seaf_warning ("Failed to read commit %s.\n", task->head);
        task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
        return -1;
    }

    curl = conn->curl;

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/commit/%s",
                               task->host, task->repo_id, task->head);
    else
        url = g_strdup_printf ("%s/repo/%s/commit/%s",
                               task->host, task->repo_id, task->head);

    int curl_error;
    if (http_put (curl, url, task->token,
                  data, len,
                  NULL, NULL, NULL,
                  &status, NULL, NULL, TRUE, &curl_error) < 0) {
        conn->release = TRUE;
        handle_curl_errors (task, curl_error);
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for PUT %s: %d.\n", url, status);
        handle_http_errors (task, status);
        ret = -1;
    }

out:
    g_free (url);
    curl_easy_reset (curl);
    g_free (data);

    return ret;
}

typedef struct {
    GList **pret;
    GHashTable *checked_objs;
} CalcFsListData;

static int
collect_file_ids (int n, const char *basedir, SeafDirent *files[], void *vdata)
{
    SeafDirent *file1 = files[0];
    SeafDirent *file2 = files[1];
    CalcFsListData *data = vdata;
    GList **pret = data->pret;
    int dummy;

    if (!file1 || strcmp (file1->id, EMPTY_SHA1) == 0)
        return 0;

    if (g_hash_table_lookup (data->checked_objs, file1->id))
        return 0;

    if (!file2 || strcmp (file1->id, file2->id) != 0) {
        *pret = g_list_prepend (*pret, g_strdup(file1->id));
        g_hash_table_insert (data->checked_objs, g_strdup(file1->id), &dummy);
    }

    return 0;
}

static int
collect_dir_ids (int n, const char *basedir, SeafDirent *dirs[], void *vdata,
                 gboolean *recurse)
{
    SeafDirent *dir1 = dirs[0];
    SeafDirent *dir2 = dirs[1];
    CalcFsListData *data = vdata;
    GList **pret = data->pret;
    int dummy;

    if (!dir1 || strcmp (dir1->id, EMPTY_SHA1) == 0)
        return 0;

    if (g_hash_table_lookup (data->checked_objs, dir1->id))
        return 0;

    if (!dir2 || strcmp (dir1->id, dir2->id) != 0) {
        *pret = g_list_prepend (*pret, g_strdup(dir1->id));
        g_hash_table_insert (data->checked_objs, g_strdup(dir1->id), &dummy);
    }

    return 0;
}

static GList *
calculate_send_fs_object_list (HttpTxTask *task)
{
    GList *ret = NULL;
    SeafBranch *local = NULL, *master = NULL;
    SeafCommit *local_head = NULL, *master_head = NULL;
    GList *ptr;

    local = seaf_branch_manager_get_branch (seaf->branch_mgr, task->repo_id, "local");
    if (!local) {
        seaf_warning ("Branch local not found for repo %.8s.\n", task->repo_id);
        goto out;
    }
    master = seaf_branch_manager_get_branch (seaf->branch_mgr, task->repo_id, "master");
    if (!master) {
        seaf_warning ("Branch master not found for repo %.8s.\n", task->repo_id);
        goto out;
    }

    local_head = seaf_commit_manager_get_commit (seaf->commit_mgr,
                                                 task->repo_id, task->repo_version,
                                                 local->commit_id);
    if (!local_head) {
        seaf_warning ("Local head commit not found for repo %.8s.\n",
                      task->repo_id);
        goto out;
    }
    master_head = seaf_commit_manager_get_commit (seaf->commit_mgr,
                                                  task->repo_id, task->repo_version,
                                                  master->commit_id);
    if (!master_head) {
        seaf_warning ("Master head commit not found for repo %.8s.\n",
                      task->repo_id);
        goto out;
    }

    /* Diff won't traverse the root object itself. */
    if (strcmp (local_head->root_id, master_head->root_id) != 0)
        ret = g_list_prepend (ret, g_strdup(local_head->root_id));

    CalcFsListData *data = g_new0(CalcFsListData, 1);
    data->pret = &ret;
    data->checked_objs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, NULL);

    DiffOptions opts;
    memset (&opts, 0, sizeof(opts));
    memcpy (opts.store_id, task->repo_id, 36);
    opts.version = task->repo_version;
    opts.file_cb = collect_file_ids;
    opts.dir_cb = collect_dir_ids;
    opts.data = data;

    const char *trees[2];
    trees[0] = local_head->root_id;
    trees[1] = master_head->root_id;
    if (diff_trees (2, trees, &opts) < 0) {
        seaf_warning ("Failed to diff local and master head for repo %.8s.\n",
                      task->repo_id);
        for (ptr = ret; ptr; ptr = ptr->next)
            g_free (ptr->data);
        ret = NULL;
    }

    g_hash_table_destroy (data->checked_objs);
    g_free (data);

out:
    seaf_branch_unref (local);
    seaf_branch_unref (master);
    seaf_commit_unref (local_head);
    seaf_commit_unref (master_head);
    return ret;
}

#define ID_LIST_SEGMENT_N 1000

static int
upload_check_id_list_segment (HttpTxTask *task, Connection *conn, const char *url,
                              GList **send_id_list, GList **recv_id_list)
{
    json_t *array;
    json_error_t jerror;
    char *obj_id;
    int n_sent = 0;
    char *data = NULL;
    int len;
    CURL *curl;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;
    int ret = 0;

    /* Convert object id list to JSON format. */

    array = json_array ();

    while (*send_id_list != NULL) {
        obj_id = (*send_id_list)->data;
        json_array_append_new (array, json_string(obj_id));

        *send_id_list = g_list_delete_link (*send_id_list, *send_id_list);
        g_free (obj_id);

        if (++n_sent >= ID_LIST_SEGMENT_N)
            break;
    }

    seaf_debug ("Check %d ids for %s:%s.\n",
                n_sent, task->host, task->repo_id);

    data = json_dumps (array, 0);
    len = strlen(data);
    json_decref (array);

    /* Send fs object id list. */

    curl = conn->curl;

    int curl_error;
    if (http_post (curl, url, task->token,
                   data, len,
                   &status, &rsp_content, &rsp_size, TRUE, &curl_error) < 0) {
        conn->release = TRUE;
        handle_curl_errors (task, curl_error);
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for POST %s: %d.\n", url, status);
        handle_http_errors (task, status);
        ret = -1;
        goto out;
    }

    /* Process needed object id list. */

    array = json_loadb (rsp_content, rsp_size, 0, &jerror);
    if (!array) {
        seaf_warning ("Invalid JSON response from the server: %s.\n", jerror.text);
        task->error = SYNC_ERROR_ID_SERVER;
        ret = -1;
        goto out;
    }

    int i;
    size_t n = json_array_size (array);
    json_t *str;

    seaf_debug ("%lu objects or blocks are needed for %s:%s.\n",
                n, task->host, task->repo_id);

    for (i = 0; i < n; ++i) {
        str = json_array_get (array, i);
        if (!str) {
            seaf_warning ("Invalid JSON response from the server.\n");
            json_decref (array);
            ret = -1;
            goto out;
        }

        *recv_id_list = g_list_prepend (*recv_id_list, g_strdup(json_string_value(str)));
    }

    json_decref (array);

out:
    curl_easy_reset (curl);
    g_free (data);
    g_free (rsp_content);

    return ret;
}

#define MAX_OBJECT_PACK_SIZE (1 << 20) /* 1MB */

#ifdef WIN32
__pragma(pack(push, 1))
typedef struct {
    char obj_id[40];
    guint32 obj_size;
    guint8 object[0];
} ObjectHeader;
__pragma(pack(pop))
#else
typedef struct {
    char obj_id[40];
    guint32 obj_size;
    guint8 object[0];
} __attribute__((__packed__)) ObjectHeader;
#endif

static int
send_fs_objects (HttpTxTask *task, Connection *conn, GList **send_fs_list)
{
    struct evbuffer *buf;
    ObjectHeader hdr;
    char *obj_id;
    char *data;
    int len;
    int total_size;
    unsigned char *package;
    CURL *curl;
    char *url = NULL;
    int status;
    int ret = 0;
    int n_sent = 0;

    buf = evbuffer_new ();
    curl = conn->curl;

    while (*send_fs_list != NULL) {
        obj_id = (*send_fs_list)->data;

        if (seaf_obj_store_read_obj (seaf->fs_mgr->obj_store,
                                     task->repo_id, task->repo_version,
                                     obj_id, (void **)&data, &len) < 0) {
            seaf_warning ("Failed to read fs object %s in repo %s.\n",
                          obj_id, task->repo_id);
            task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
            ret = -1;
            goto out;
        }

        ++n_sent;

        memcpy (hdr.obj_id, obj_id, 40);
        hdr.obj_size = htonl (len);

        evbuffer_add (buf, &hdr, sizeof(hdr));
        evbuffer_add (buf, data, len);

        g_free (data);
        *send_fs_list = g_list_delete_link (*send_fs_list, *send_fs_list);
        g_free (obj_id);

        total_size = evbuffer_get_length (buf);
        if (total_size >= MAX_OBJECT_PACK_SIZE)
            break;
    }

    seaf_debug ("Sending %d fs objects for %s:%s.\n",
                n_sent, task->host, task->repo_id);

    package = evbuffer_pullup (buf, -1);

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/recv-fs/",
                               task->host, task->repo_id);
    else
        url = g_strdup_printf ("%s/repo/%s/recv-fs/",
                               task->host, task->repo_id);

    int curl_error;
    if (http_post (curl, url, task->token,
                   (char *)package, evbuffer_get_length(buf),
                   &status, NULL, NULL, TRUE, &curl_error) < 0) {
        conn->release = TRUE;
        handle_curl_errors (task, curl_error);
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for POST %s: %d.\n", url, status);
        handle_http_errors (task, status);
        ret = -1;
    }

out:
    g_free (url);
    evbuffer_free (buf);
    curl_easy_reset (curl);

    return ret;
}

typedef struct {
    GList *block_list;
    GHashTable *added_blocks;
    HttpTxTask *task;
} CalcBlockListData;

static void
add_to_block_list (GList **block_list, GHashTable *added_blocks, const char *block_id)
{
    int dummy;

    if (g_hash_table_lookup (added_blocks, block_id))
        return;

    *block_list = g_list_prepend (*block_list, g_strdup(block_id));
    g_hash_table_insert (added_blocks, g_strdup(block_id), &dummy);
}

static int
block_list_diff_files (int n, const char *basedir, SeafDirent *files[], void *vdata)
{
    SeafDirent *file1 = files[0];
    SeafDirent *file2 = files[1];
    CalcBlockListData *data = vdata;
    HttpTxTask *task = data->task;
    Seafile *f1 = NULL, *f2 = NULL;
    int i;

    if (file1 && strcmp (file1->id, EMPTY_SHA1) != 0) {
        if (!file2) {
            f1 = seaf_fs_manager_get_seafile (seaf->fs_mgr,
                                              task->repo_id, task->repo_version,
                                              file1->id);
            if (!f1) {
                seaf_warning ("Failed to get seafile object %s:%s.\n",
                              task->repo_id, file1->id);
                return -1;
            }
            for (i = 0; i < f1->n_blocks; ++i)
                add_to_block_list (&data->block_list, data->added_blocks,
                                   f1->blk_sha1s[i]);
            seafile_unref (f1);
        } else if (strcmp (file1->id, file2->id) != 0) {
            f1 = seaf_fs_manager_get_seafile (seaf->fs_mgr,
                                              task->repo_id, task->repo_version,
                                              file1->id);
            if (!f1) {
                seaf_warning ("Failed to get seafile object %s:%s.\n",
                              task->repo_id, file1->id);
                return -1;
            }
            f2 = seaf_fs_manager_get_seafile (seaf->fs_mgr,
                                              task->repo_id, task->repo_version,
                                              file2->id);
            if (!f2) {
                seafile_unref (f1);
                seaf_warning ("Failed to get seafile object %s:%s.\n",
                              task->repo_id, file2->id);
                return -1;
            }

            GHashTable *h = g_hash_table_new (g_str_hash, g_str_equal);
            int dummy;
            for (i = 0; i < f2->n_blocks; ++i)
                g_hash_table_insert (h, f2->blk_sha1s[i], &dummy);

            for (i = 0; i < f1->n_blocks; ++i)
                if (!g_hash_table_lookup (h, f1->blk_sha1s[i]))
                    add_to_block_list (&data->block_list, data->added_blocks,
                                       f1->blk_sha1s[i]);

            seafile_unref (f1);
            seafile_unref (f2);
            g_hash_table_destroy (h);
        }
    }

    return 0;
}

static int
block_list_diff_dirs (int n, const char *basedir, SeafDirent *dirs[], void *data,
                      gboolean *recurse)
{
    /* Do nothing */
    return 0;
}

static int
calculate_block_list (HttpTxTask *task, GList **plist)
{
    int ret = 0;
    SeafBranch *local = NULL, *master = NULL;
    SeafCommit *local_head = NULL, *master_head = NULL;

    local = seaf_branch_manager_get_branch (seaf->branch_mgr, task->repo_id, "local");
    if (!local) {
        seaf_warning ("Branch local not found for repo %.8s.\n", task->repo_id);
        ret = -1;
        goto out;
    }
    master = seaf_branch_manager_get_branch (seaf->branch_mgr, task->repo_id, "master");
    if (!master) {
        seaf_warning ("Branch master not found for repo %.8s.\n", task->repo_id);
        ret = -1;
        goto out;
    }

    local_head = seaf_commit_manager_get_commit (seaf->commit_mgr,
                                                 task->repo_id, task->repo_version,
                                                 local->commit_id);
    if (!local_head) {
        seaf_warning ("Local head commit not found for repo %.8s.\n",
                      task->repo_id);
        ret = -1;
        goto out;
    }
    master_head = seaf_commit_manager_get_commit (seaf->commit_mgr,
                                                 task->repo_id, task->repo_version,
                                                 master->commit_id);
    if (!master_head) {
        seaf_warning ("Master head commit not found for repo %.8s.\n",
                      task->repo_id);
        ret = -1;
        goto out;
    }

    CalcBlockListData data;
    memset (&data, 0, sizeof(data));
    data.added_blocks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    data.task = task;

    DiffOptions opts;
    memset (&opts, 0, sizeof(opts));
    memcpy (opts.store_id, task->repo_id, 36);
    opts.version = task->repo_version;
    opts.file_cb = block_list_diff_files;
    opts.dir_cb = block_list_diff_dirs;
    opts.data = &data;

    const char *trees[2];
    trees[0] = local_head->root_id;
    trees[1] = master_head->root_id;
    if (diff_trees (2, trees, &opts) < 0) {
        seaf_warning ("Failed to diff local and master head for repo %.8s.\n",
                      task->repo_id);
        g_hash_table_destroy (data.added_blocks);

        GList *ptr;
        for (ptr = data.block_list; ptr; ptr = ptr->next)
            g_free (ptr->data);

        ret = -1;
        goto out;
    }

    g_hash_table_destroy (data.added_blocks);
    *plist = data.block_list;

out:
    seaf_branch_unref (local);
    seaf_branch_unref (master);
    seaf_commit_unref (local_head);
    seaf_commit_unref (master_head);
    return ret;
}

typedef struct {
    char block_id[41];
    BlockHandle *block;
    HttpTxTask *task;
} SendBlockData;

static size_t
send_block_callback (void *ptr, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size *nmemb;
    SendBlockData *data = userp;
    HttpTxTask *task = data->task;
    int n;

    if (task->state == HTTP_TASK_STATE_CANCELED || task->all_stop)
        return CURL_READFUNC_ABORT;

    n = seaf_block_manager_read_block (seaf->block_mgr,
                                       data->block,
                                       ptr, realsize);
    if (n < 0) {
        seaf_warning ("Failed to read block %s in repo %.8s.\n",
                      data->block_id, task->repo_id);
        task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
        return CURL_READFUNC_ABORT;
    }

    /* Update global transferred bytes. */
    g_atomic_int_add (&(seaf->sync_mgr->sent_bytes), n);

    /* Update transferred bytes for this task */
    g_atomic_int_add (&task->tx_bytes, n);

    /* If uploaded bytes exceeds the limit, wait until the counter
     * is reset. We check the counter every 100 milliseconds, so we
     * can waste up to 100 milliseconds without sending data after
     * the counter is reset.
     */
    while (1) {
        gint sent = g_atomic_int_get(&(seaf->sync_mgr->sent_bytes));
        if (seaf->sync_mgr->upload_limit > 0 &&
            sent > seaf->sync_mgr->upload_limit)
            /* 100 milliseconds */
            g_usleep (100000);
        else
            break;
    }

    return n;
}

static size_t
rewind_block_callback (void *clientp, curl_off_t offset, int origin)
{
    if (offset != 0 || origin != 0) {
        return CURL_SEEKFUNC_FAIL;
    }

    SendBlockData *data = clientp;
    HttpTxTask *task = data->task;

    int rc = seaf_block_manager_rewind_block (seaf->block_mgr,
                                              data->block);
    if (rc < 0) {
        seaf_warning ("Failed to rewind block %s in repo %s.\n",
                      data->block_id, task->repo_id);
        return CURL_SEEKFUNC_FAIL;
    }

    return CURL_SEEKFUNC_OK;
}

static int
send_block (HttpTxTask *task, Connection *conn, const char *block_id, guint32 *psize)
{
    CURL *curl;
    char *url;
    int status;
    BlockMetadata *bmd;
    BlockHandle *block;
    int ret = 0;

    bmd = seaf_block_manager_stat_block (seaf->block_mgr,
                                         task->repo_id, task->repo_version,
                                         block_id);
    if (!bmd) {
        seaf_warning ("Failed to stat block %s in repo %s.\n",
                      block_id, task->repo_id);
        task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
        return -1;
    }

    block = seaf_block_manager_open_block (seaf->block_mgr,
                                           task->repo_id, task->repo_version,
                                           block_id, BLOCK_READ);
    if (!block) {
        seaf_warning ("Failed to open block %s in repo %s.\n",
                      block_id, task->repo_id);
        task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
        g_free (bmd);
        return -1;
    }

    SendBlockData data;
    memset (&data, 0, sizeof(data));
    memcpy (data.block_id, block_id, 40);
    data.block = block;
    data.task = task;

    curl = conn->curl;

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/block/%s",
                               task->host, task->repo_id, block_id);
    else
        url = g_strdup_printf ("%s/repo/%s/block/%s",
                               task->host, task->repo_id, block_id);

    int curl_error;
    if (http_put (curl, url, task->token,
                  NULL, bmd->size,
                  send_block_callback, &data, rewind_block_callback,
                  &status, NULL, NULL, TRUE, &curl_error) < 0) {
        if (task->state == HTTP_TASK_STATE_CANCELED)
            goto out;

        if (task->error == SYNC_ERROR_ID_NO_ERROR) {
            /* Only release connection when it's a network error */
            conn->release = TRUE;
            handle_curl_errors (task, curl_error);
        }
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for PUT %s: %d.\n", url, status);
        handle_http_errors (task, status);
        ret = -1;
        goto out;
    }

    if (psize)
        *psize = bmd->size;

out:
    g_free (url);
    curl_easy_reset (curl);
    g_free (bmd);
    seaf_block_manager_close_block (seaf->block_mgr, block);
    seaf_block_manager_block_handle_free (seaf->block_mgr, block);

    return ret;
}

typedef struct BlockUploadData {
    HttpTxTask *http_task;
    ConnectionPool *cpool;
    GAsyncQueue *finished_tasks;
} BlockUploadData;

typedef struct BlockUploadTask {
    char block_id[41];
    int result;
    guint32 block_size;
} BlockUploadTask;

static void
block_upload_task_free (BlockUploadTask *task)
{
    g_free (task);
}

static void
upload_block_thread_func (gpointer data, gpointer user_data)
{
    BlockUploadTask *task = data;
    BlockUploadData *tx_data = user_data;
    HttpTxTask *http_task = tx_data->http_task;
    Connection *conn;
    int ret = 0;

    conn = connection_pool_get_connection (tx_data->cpool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", http_task->host);
        http_task->error = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        ret = -1;
        goto out;
    }

    ret = send_block (http_task, conn, task->block_id, &task->block_size);

    connection_pool_return_connection (tx_data->cpool, conn);

out:
    task->result = ret;
    g_async_queue_push (tx_data->finished_tasks, task);
}

#define DEFAULT_UPLOAD_BLOCK_THREADS 3

static int
multi_threaded_send_blocks (HttpTxTask *http_task, GList *block_list)
{
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    GThreadPool *tpool;
    GAsyncQueue *finished_tasks;
    GHashTable *pending_tasks;
    ConnectionPool *cpool;
    GList *ptr;
    char *block_id;
    BlockUploadTask *task;
    SyncInfo *info;
    int ret = 0;

    if (block_list == NULL)
        return 0;

    cpool = find_connection_pool (priv, http_task->host);
    if (!cpool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", http_task->host);
        http_task->error = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        return -1;
    }

    finished_tasks = g_async_queue_new ();

    BlockUploadData data;
    data.http_task = http_task;
    data.finished_tasks = finished_tasks;
    data.cpool = cpool;

    tpool = g_thread_pool_new (upload_block_thread_func, &data,
                               DEFAULT_UPLOAD_BLOCK_THREADS, FALSE, NULL);

    pending_tasks = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free,
                                           (GDestroyNotify)block_upload_task_free);

    info = seaf_sync_manager_get_sync_info (seaf->sync_mgr, http_task->repo_id);

    for (ptr = block_list; ptr; ptr = ptr->next) {
        block_id = ptr->data;

        task = g_new0 (BlockUploadTask, 1);
        memcpy (task->block_id, block_id, 40);

        if (!g_hash_table_lookup (pending_tasks, block_id)) {
            g_hash_table_insert (pending_tasks, g_strdup(block_id), task);
            g_thread_pool_push (tpool, task, NULL);
        } else {
            g_free (task);
        }
    }

    while ((task = g_async_queue_pop (finished_tasks)) != NULL) {
        if (task->result < 0 || http_task->state == HTTP_TASK_STATE_CANCELED) {
            ret = task->result;
            http_task->all_stop = TRUE;
            break;
        }

        ++(http_task->done_blocks);

        if (info && info->multipart_upload) {
            info->uploaded_bytes += (gint64)task->block_size;
        }

        g_hash_table_remove (pending_tasks, task->block_id);
        if (g_hash_table_size(pending_tasks) == 0)
            break;
    }

    g_thread_pool_free (tpool, TRUE, TRUE);

    g_hash_table_destroy (pending_tasks);

    g_async_queue_unref (finished_tasks);

    return ret;
}

static void
notify_permission_error (HttpTxTask *task, const char *error_str)
{
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    GMatchInfo *match_info;
    char *path;

    if (g_regex_match (priv->locked_error_regex, error_str, 0, &match_info)) {
        path = g_match_info_fetch (match_info, 1);
        send_file_sync_error_notification (task->repo_id, task->repo_name, path,
                                           SYNC_ERROR_ID_FILE_LOCKED);
        g_free (path);

        /* Set more accurate error. */
        task->error = SYNC_ERROR_ID_FILE_LOCKED;
    } else if (g_regex_match (priv->folder_perm_error_regex, error_str, 0, &match_info)) {
        path = g_match_info_fetch (match_info, 1);
        /* The path returned by server begins with '/'. */
        send_file_sync_error_notification (task->repo_id, task->repo_name,
                                           (path[0] == '/') ? (path + 1) : path,
                                           SYNC_ERROR_ID_FOLDER_PERM_DENIED);
        g_free (path);

        task->error = SYNC_ERROR_ID_FOLDER_PERM_DENIED;
    } else if (g_regex_match (priv->too_many_files_error_regex, error_str, 0, &match_info)) {
        task->error = SYNC_ERROR_ID_TOO_MANY_FILES;
    }

    g_match_info_free (match_info);
}

static int
update_branch (HttpTxTask *task, Connection *conn)
{
    CURL *curl;
    char *url;
    int status;
    char *rsp_content = NULL;
    char *rsp_content_str = NULL;
    gint64 rsp_size;
    int ret = 0;

    curl = conn->curl;

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/commit/HEAD/?head=%s",
                               task->host, task->repo_id, task->head);
    else
        url = g_strdup_printf ("%s/repo/%s/commit/HEAD/?head=%s",
                               task->host, task->repo_id, task->head);

    int curl_error;
    if (http_put (curl, url, task->token,
                  NULL, 0,
                  NULL, NULL, NULL,
                  &status, &rsp_content, &rsp_size, TRUE, &curl_error) < 0) {
        conn->release = TRUE;
        handle_curl_errors (task, curl_error);
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for PUT %s: %d.\n", url, status);
        handle_http_errors (task, status);

        if (status == HTTP_FORBIDDEN || status == HTTP_BLOCK_MISSING) {
            rsp_content_str = g_new0 (gchar, rsp_size + 1);
            memcpy (rsp_content_str, rsp_content, rsp_size);
            if (status == HTTP_FORBIDDEN) {
                seaf_warning ("%s\n", rsp_content_str);
                notify_permission_error (task, rsp_content_str);
            } else if (status == HTTP_BLOCK_MISSING) {
                seaf_warning ("Failed to upload files: %s\n", rsp_content_str);
                task->error = SYNC_ERROR_ID_BLOCK_MISSING;
            }
            g_free (rsp_content_str);
        }

        ret = -1;
    }

out:
    g_free (url);
    g_free (rsp_content);
    curl_easy_reset (curl);

    return ret;
}

static void
update_master_branch (HttpTxTask *task)
{
    SeafBranch *branch;
    branch = seaf_branch_manager_get_branch (seaf->branch_mgr,
                                             task->repo_id,
                                             "master");
    if (!branch) {
        branch = seaf_branch_new ("master", task->repo_id, task->head);
        seaf_branch_manager_add_branch (seaf->branch_mgr, branch);
        seaf_branch_unref (branch);
    } else {
        seaf_branch_set_commit (branch, task->head);
        seaf_branch_manager_update_branch (seaf->branch_mgr, branch);
        seaf_branch_unref (branch);
    }
}

static void
set_path_status_syncing (gpointer key, gpointer value, gpointer user_data)
{
    HttpTxTask *task = user_data;
    char *path = key;
    int mode = (int)(long)value;
    seaf_sync_manager_update_active_path (seaf->sync_mgr,
                                          task->repo_id,
                                          path,
                                          mode,
                                          SYNC_STATUS_SYNCING,
                                          TRUE);
}

static void
set_path_status_synced (gpointer key, gpointer value, gpointer user_data)
{
    HttpTxTask *task = user_data;
    char *path = key;
    int mode = (int)(long)value;
    seaf_sync_manager_update_active_path (seaf->sync_mgr,
                                          task->repo_id,
                                          path,
                                          mode,
                                          SYNC_STATUS_SYNCED,
                                          TRUE);
}

static void *
http_upload_thread (void *vdata)
{
    HttpTxTask *task = vdata;
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn = NULL;
    char *url = NULL;
    GList *send_fs_list = NULL, *needed_fs_list = NULL;
    GList *block_list = NULL, *needed_block_list = NULL;
    GHashTable *active_paths = NULL;

    SeafBranch *local = seaf_branch_manager_get_branch (seaf->branch_mgr,
                                                        task->repo_id, "local");
    if (!local) {
        seaf_warning ("Failed to get branch local of repo %.8s.\n", task->repo_id);
        task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
        return NULL;
    }
    memcpy (task->head, local->commit_id, 40);
    seaf_branch_unref (local);

    pool = find_connection_pool (priv, task->host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", task->host);
        task->error = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        goto out;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", task->host);
        task->error = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        goto out;
    }

    /* seaf_message ("Upload with HTTP sync protocol version %d.\n", */
    /*               task->protocol_version); */

    transition_state (task, task->state, HTTP_TASK_RT_STATE_CHECK);

    gint64 delta = 0;
    active_paths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    if (calculate_upload_size_delta_and_active_paths (task, &delta, active_paths) < 0) {
        seaf_warning ("Failed to calculate upload size delta for repo %s.\n",
                      task->repo_id);
        task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
        goto out;
    }

    g_hash_table_foreach (active_paths, set_path_status_syncing, task);

    if (check_permission (task, conn) < 0) {
        seaf_warning ("Upload permission denied for repo %.8s on server %s.\n",
                      task->repo_id, task->host);
        goto out;
    }

    if (check_quota (task, conn, delta) < 0) {
        seaf_warning ("Not enough quota for repo %.8s on server %s.\n",
                      task->repo_id, task->host);
        goto out;
    }

    if (task->state == HTTP_TASK_STATE_CANCELED)
        goto out;

    transition_state (task, task->state, HTTP_TASK_RT_STATE_COMMIT);

    if (send_commit_object (task, conn) < 0) {
        seaf_warning ("Failed to send head commit for repo %.8s.\n", task->repo_id);
        goto out;
    }

    if (task->state == HTTP_TASK_STATE_CANCELED)
        goto out;

    transition_state (task, task->state, HTTP_TASK_RT_STATE_FS);

    send_fs_list = calculate_send_fs_object_list (task);
    if (!send_fs_list) {
        seaf_warning ("Failed to calculate fs object list for repo %.8s.\n",
                      task->repo_id);
        task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
        goto out;
    }

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/check-fs/",
                               task->host, task->repo_id);
    else
        url = g_strdup_printf ("%s/repo/%s/check-fs/",
                               task->host, task->repo_id);

    while (send_fs_list != NULL) {
        if (upload_check_id_list_segment (task, conn, url,
                                          &send_fs_list, &needed_fs_list) < 0) {
            seaf_warning ("Failed to check fs list for repo %.8s.\n", task->repo_id);
            goto out;
        }

        if (task->state == HTTP_TASK_STATE_CANCELED)
            goto out;
    }
    g_free (url);
    url = NULL;

    while (needed_fs_list != NULL) {
        if (send_fs_objects (task, conn, &needed_fs_list) < 0) {
            seaf_warning ("Failed to send fs objects for repo %.8s.\n", task->repo_id);
            goto out;
        }

        if (task->state == HTTP_TASK_STATE_CANCELED)
            goto out;
    }

    transition_state (task, task->state, HTTP_TASK_RT_STATE_BLOCK);

    if (calculate_block_list (task, &block_list) < 0) {
        seaf_warning ("Failed to calculate block list for repo %.8s.\n",
                      task->repo_id);
        task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
        goto out;
    }

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/check-blocks/",
                               task->host, task->repo_id);
    else
        url = g_strdup_printf ("%s/repo/%s/check-blocks/",
                               task->host, task->repo_id);

    while (block_list != NULL) {
        if (upload_check_id_list_segment (task, conn, url,
                                          &block_list, &needed_block_list) < 0) {
            seaf_warning ("Failed to check block list for repo %.8s.\n",
                          task->repo_id);
            goto out;
        }

        if (task->state == HTTP_TASK_STATE_CANCELED)
            goto out;
    }
    g_free (url);
    url = NULL;

    task->n_blocks = g_list_length (needed_block_list);

    seaf_debug ("%d blocks to send for %s:%s.\n",
                task->n_blocks, task->host, task->repo_id);

    if (multi_threaded_send_blocks(task, needed_block_list) < 0 ||
        task->state == HTTP_TASK_STATE_CANCELED)
        goto out;

    transition_state (task, task->state, HTTP_TASK_RT_STATE_UPDATE_BRANCH);

    if (update_branch (task, conn) < 0) {
        seaf_warning ("Failed to update branch of repo %.8s.\n", task->repo_id);
        goto out;
    }

    /* After successful upload, the cached 'master' branch should be updated to
     * the head commit of 'local' branch.
     */
    update_master_branch (task);

    if (active_paths != NULL)
        g_hash_table_foreach (active_paths, set_path_status_synced, task);

out:
    string_list_free (send_fs_list);
    string_list_free (needed_fs_list);
    string_list_free (block_list);
    string_list_free (needed_block_list);

    if (active_paths)
        g_hash_table_destroy (active_paths);

    g_free (url);

    connection_pool_return_connection (pool, conn);

    return vdata;
}

static void
http_upload_done (void *vdata)
{
    HttpTxTask *task = vdata;

    if (task->error != SYNC_ERROR_ID_NO_ERROR)
        transition_state (task, HTTP_TASK_STATE_ERROR, HTTP_TASK_RT_STATE_FINISHED);
    else if (task->state == HTTP_TASK_STATE_CANCELED)
        transition_state (task, task->state, HTTP_TASK_RT_STATE_FINISHED);
    else
        transition_state (task, HTTP_TASK_STATE_FINISHED, HTTP_TASK_RT_STATE_FINISHED);
}

/* Download */

static void *http_download_thread (void *vdata);
static void http_download_done (void *vdata);

int
http_tx_manager_add_download (HttpTxManager *manager,
                              const char *repo_id,
                              int repo_version,
                              const char *host,
                              const char *token,
                              const char *server_head_id,
                              gboolean is_clone,
                              const char *passwd,
                              const char *worktree,
                              int protocol_version,
                              const char *email,
                              const char *username,
                              gboolean use_fileserver_port,
                              const char *repo_name,
                              GError **error)
{
    HttpTxTask *task;
    SeafRepo *repo;

    if (!repo_id) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Empty argument(repo_id)");
        return -1;
    }

    if (!token) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Empty argument(token)");
        return -1;
    }

    if (!host) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Empty argument(host)");
        return -1;
    }

    if (!server_head_id) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Empty argument(server_head_id)");
        return -1;
    }

    if (!email) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Empty argument(email)");
        return -1;
    }

    if (!is_clone) {
        repo = seaf_repo_manager_get_repo (seaf->repo_mgr, repo_id);
        if (!repo) {
            g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "Repo not found");
            return -1;
        }
    }

    clean_tasks_for_repo (manager, repo_id);

    task = http_tx_task_new (manager, repo_id, repo_version,
                             HTTP_TASK_TYPE_DOWNLOAD, is_clone,
                             host, token, passwd, worktree);

    memcpy (task->head, server_head_id, 40);
    task->protocol_version = protocol_version;
    task->email = g_strdup(email);
    if (username)
        task->username = g_strdup(username);

    task->state = HTTP_TASK_STATE_NORMAL;

    task->use_fileserver_port = use_fileserver_port;

    task->blk_ref_cnts = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);
    pthread_mutex_init (&task->ref_cnt_lock, NULL);

    g_hash_table_insert (manager->priv->download_tasks,
                         g_strdup(repo_id),
                         task);

    task->repo_name = g_strdup(repo_name);

    if (seaf_job_manager_schedule_job (seaf->job_mgr,
                                       http_download_thread,
                                       http_download_done,
                                       task) < 0) {
        g_hash_table_remove (manager->priv->download_tasks, repo_id);
        return -1;
    }

    return 0;
}

static int
get_commit_object (HttpTxTask *task, Connection *conn)
{
    CURL *curl;
    char *url;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;
    int ret = 0;

    curl = conn->curl;

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/commit/%s",
                               task->host, task->repo_id, task->head);
    else
        url = g_strdup_printf ("%s/repo/%s/commit/%s",
                               task->host, task->repo_id, task->head);

    int curl_error;
    if (http_get (curl, url, task->token, &status,
                  &rsp_content, &rsp_size,
                  NULL, NULL, TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        conn->release = TRUE;
        handle_curl_errors (task, curl_error);
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
        handle_http_errors (task, status);
        ret = -1;
        goto out;
    }

    int rc = seaf_obj_store_write_obj (seaf->commit_mgr->obj_store,
                                       task->repo_id, task->repo_version,
                                       task->head,
                                       rsp_content,
                                       rsp_size,
                                       FALSE);
    if (rc < 0) {
        seaf_warning ("Failed to save commit %s in repo %.8s.\n",
                      task->head, task->repo_id);
        task->error = SYNC_ERROR_ID_WRITE_LOCAL_DATA;
        ret = -1;
    }

out:
    g_free (url);
    g_free (rsp_content);
    curl_easy_reset (curl);

    return ret;
}

static int
get_needed_fs_id_list (HttpTxTask *task, Connection *conn, GList **fs_id_list)
{
    SeafBranch *master;
    CURL *curl;
    char *url = NULL;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;
    int ret = 0;
    json_t *array;
    json_error_t jerror;
    const char *obj_id;

    const char *url_prefix = (task->use_fileserver_port) ? "" : "seafhttp/";

    if (!task->is_clone) {
        master = seaf_branch_manager_get_branch (seaf->branch_mgr,
                                                 task->repo_id,
                                                 "master");
        if (!master) {
            seaf_warning ("Failed to get branch master for repo %.8s.\n",
                          task->repo_id);
            return -1;
        }

        url = g_strdup_printf ("%s/%srepo/%s/fs-id-list/"
                               "?server-head=%s&client-head=%s",
                               task->host, url_prefix, task->repo_id,
                               task->head, master->commit_id);

        seaf_branch_unref (master);
    } else {
        url = g_strdup_printf ("%s/%srepo/%s/fs-id-list/?server-head=%s",
                               task->host, url_prefix, task->repo_id, task->head);
    }

    curl = conn->curl;

    int curl_error;
    if (http_get (curl, url, task->token, &status,
                  &rsp_content, &rsp_size,
                  NULL, NULL, (!task->is_clone), FS_ID_LIST_TIMEOUT_SEC, &curl_error) < 0) {
        conn->release = TRUE;
        handle_curl_errors (task, curl_error);
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
        handle_http_errors (task, status);
        ret = -1;
        goto out;
    }

    array = json_loadb (rsp_content, rsp_size, 0, &jerror);
    if (!array) {
        seaf_warning ("Invalid JSON response from the server: %s.\n", jerror.text);
        task->error = SYNC_ERROR_ID_SERVER;
        ret = -1;
        goto out;
    }

    int i;
    size_t n = json_array_size (array);
    json_t *str;

    seaf_debug ("Received fs object list size %lu from %s:%s.\n",
                n, task->host, task->repo_id);

    task->n_fs_objs = (int)n;

    GHashTable *checked_objs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      g_free, NULL);

    for (i = 0; i < n; ++i) {
        str = json_array_get (array, i);
        if (!str) {
            seaf_warning ("Invalid JSON response from the server.\n");
            json_decref (array);
            string_list_free (*fs_id_list);
            ret = -1;
            goto out;
        }

        obj_id = json_string_value(str);

        if (g_hash_table_lookup (checked_objs, obj_id)) {
            ++(task->done_fs_objs);
            continue;
        }
        char *key = g_strdup(obj_id);
        g_hash_table_replace (checked_objs, key, key);

        if (!seaf_obj_store_obj_exists (seaf->fs_mgr->obj_store,
                                        task->repo_id, task->repo_version,
                                        obj_id)) {
            *fs_id_list = g_list_prepend (*fs_id_list, g_strdup(obj_id));
        } else if (task->is_clone) {
            gboolean io_error = FALSE;
            gboolean sound;
            sound = seaf_fs_manager_verify_object (seaf->fs_mgr,
                                                   task->repo_id, task->repo_version,
                                                   obj_id, FALSE, &io_error);
            if (!sound && !io_error) {
                *fs_id_list = g_list_prepend (*fs_id_list, g_strdup(obj_id));
            } else {
                ++(task->done_fs_objs);
            }
        } else {
            ++(task->done_fs_objs);
        }
    }

    json_decref (array);
    g_hash_table_destroy (checked_objs);

out:
    g_free (url);
    g_free (rsp_content);
    curl_easy_reset (curl);

    return ret;
}

#define GET_FS_OBJECT_N 100

static int
get_fs_objects (HttpTxTask *task, Connection *conn, GList **fs_list)
{
    json_t *array;
    char *obj_id;
    int n_sent = 0;
    char *data = NULL;
    int len;
    CURL *curl;
    char *url = NULL;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;
    int ret = 0;
    GHashTable *requested;

    /* Convert object id list to JSON format. */

    requested = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    array = json_array ();

    while (*fs_list != NULL) {
        obj_id = (*fs_list)->data;
        json_array_append_new (array, json_string(obj_id));

        *fs_list = g_list_delete_link (*fs_list, *fs_list);

        g_hash_table_replace (requested, obj_id, obj_id);

        if (++n_sent >= GET_FS_OBJECT_N)
            break;
    }

    seaf_debug ("Requesting %d fs objects from %s:%s.\n",
                n_sent, task->host, task->repo_id);

    data = json_dumps (array, 0);
    len = strlen(data);
    json_decref (array);

    /* Send fs object id list. */

    curl = conn->curl;

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/pack-fs/", task->host, task->repo_id);
    else
        url = g_strdup_printf ("%s/repo/%s/pack-fs/", task->host, task->repo_id);

    int curl_error;
    if (http_post (curl, url, task->token,
                   data, len,
                   &status, &rsp_content, &rsp_size, TRUE, &curl_error) < 0) {
        conn->release = TRUE;
        handle_curl_errors (task, curl_error);
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for POST %s: %d.\n", url, status);
        handle_http_errors (task, status);
        ret = -1;
        goto out;
    }

    /* Save received fs objects. */

    int n_recv = 0;
    char *p = rsp_content;
    ObjectHeader *hdr = (ObjectHeader *)p;
    char recv_obj_id[41];
    int n = 0;
    int size;
    int rc;
    while (n < rsp_size) {
        memcpy (recv_obj_id, hdr->obj_id, 40);
        recv_obj_id[40] = 0;
        size = ntohl (hdr->obj_size);
        if (n + sizeof(ObjectHeader) + size > rsp_size) {
            seaf_warning ("Incomplete object package received for repo %.8s.\n",
                          task->repo_id);
            task->error = SYNC_ERROR_ID_SERVER;
            ret = -1;
            goto out;
        }

        ++n_recv;

        rc = seaf_obj_store_write_obj (seaf->fs_mgr->obj_store,
                                       task->repo_id, task->repo_version,
                                       recv_obj_id,
                                       hdr->object,
                                       size, FALSE);
        if (rc < 0) {
            seaf_warning ("Failed to write fs object %s in repo %.8s.\n",
                          recv_obj_id, task->repo_id);
            task->error = SYNC_ERROR_ID_WRITE_LOCAL_DATA;
            ret = -1;
            goto out;
        }

        g_hash_table_remove (requested, recv_obj_id);

        ++(task->done_fs_objs);

        p += (sizeof(ObjectHeader) + size);
        n += (sizeof(ObjectHeader) + size);
        hdr = (ObjectHeader *)p;
    }

    seaf_debug ("Received %d fs objects from %s:%s.\n",
                n_recv, task->host, task->repo_id);

    /* The server may not return all the objects we requested.
     * So we need to add back the remaining object ids into fs_list.
     */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, requested);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        obj_id = key;
        *fs_list = g_list_prepend (*fs_list, g_strdup(obj_id));
    }
    g_hash_table_destroy (requested);

out:
    g_free (url);
    g_free (data);
    g_free (rsp_content);
    curl_easy_reset (curl);

    return ret;
}

typedef struct {
    char block_id[41];
    BlockHandle *block;
    HttpTxTask *task;
} GetBlockData;

static size_t
get_block_callback (void *ptr, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size *nmemb;
    SendBlockData *data = userp;
    HttpTxTask *task = data->task;
    size_t n;

    if (task->state == HTTP_TASK_STATE_CANCELED || task->all_stop)
        return 0;

    n = seaf_block_manager_write_block (seaf->block_mgr,
                                        data->block,
                                        ptr, realsize);
    if (n < realsize) {
        seaf_warning ("Failed to write block %s in repo %.8s.\n",
                      data->block_id, task->repo_id);
        task->error = SYNC_ERROR_ID_WRITE_LOCAL_DATA;
        return n;
    }

    /* Update global transferred bytes. */
    g_atomic_int_add (&(seaf->sync_mgr->recv_bytes), n);

    /* Update transferred bytes for this task */
    g_atomic_int_add (&task->tx_bytes, n);

    /* If uploaded bytes exceeds the limit, wait until the counter
     * is reset. We check the counter every 100 milliseconds, so we
     * can waste up to 100 milliseconds without sending data after
     * the counter is reset.
     */
    while (1) {
        gint sent = g_atomic_int_get(&(seaf->sync_mgr->recv_bytes));
        if (seaf->sync_mgr->download_limit > 0 &&
            sent > seaf->sync_mgr->download_limit)
            /* 100 milliseconds */
            g_usleep (100000);
        else
            break;
    }

    return n;
}

int
get_block (HttpTxTask *task, Connection *conn, const char *block_id)
{
    CURL *curl;
    char *url;
    int status;
    BlockHandle *block;
    int ret = 0;
    int *pcnt;

    block = seaf_block_manager_open_block (seaf->block_mgr,
                                           task->repo_id, task->repo_version,
                                           block_id, BLOCK_WRITE);
    if (!block) {
        seaf_warning ("Failed to open block %s in repo %.8s.\n",
                      block_id, task->repo_id);
        return -1;
    }

    GetBlockData data;
    memcpy (data.block_id, block_id, 40);
    data.block = block;
    data.task = task;

    curl = conn->curl;

    if (!task->use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/block/%s",
                               task->host, task->repo_id, block_id);
    else
        url = g_strdup_printf ("%s/repo/%s/block/%s",
                               task->host, task->repo_id, block_id);

    int curl_error;
    if (http_get (curl, url, task->token, &status, NULL, NULL,
                  get_block_callback, &data, TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        if (task->state == HTTP_TASK_STATE_CANCELED)
            goto error;

        if (task->error == SYNC_ERROR_ID_NO_ERROR) {
            /* Only release the connection when it's a network error. */
            conn->release = TRUE;
            handle_curl_errors (task, curl_error);
        }
        ret = -1;
        goto error;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
        handle_http_errors (task, status);
        ret = -1;
        goto error;
    }

    BlockMetadata *bmd = seaf_block_manager_stat_block_by_handle(seaf->block_mgr, block);
    if (bmd == NULL) {
        seaf_warning ("Failed to get block %s meta data in repo %.8s.\n", block_id, task->repo_id);
        ret = -1;
        goto error;
    }
    
    seaf_block_manager_close_block (seaf->block_mgr, block);

    pthread_mutex_lock (&task->ref_cnt_lock);

    task->done_download += bmd->size;
    g_free (bmd);    

    /* Don't overwrite the block if other thread already downloaded it.
     * Since we've locked ref_cnt_lock, we can be sure the block won't be removed.
     */
    if (!seaf_block_manager_block_exists (seaf->block_mgr,
                                          task->repo_id, task->repo_version,
                                          block_id) &&
        seaf_block_manager_commit_block (seaf->block_mgr, block) < 0)
    {
        seaf_warning ("Failed to commit block %s in repo %.8s.\n",
                      block_id, task->repo_id);
        task->error = SYNC_ERROR_ID_WRITE_LOCAL_DATA;
        ret = -1;
    }

    if (ret == 0) {
        pcnt = g_hash_table_lookup (task->blk_ref_cnts, block_id);
        if (!pcnt) {
            pcnt = g_new0(int, 1);
            g_hash_table_insert (task->blk_ref_cnts, g_strdup(block_id), pcnt);
        }
        *pcnt += 1;
    }

    pthread_mutex_unlock (&task->ref_cnt_lock);

    seaf_block_manager_block_handle_free (seaf->block_mgr, block);

    g_free (url);

    return ret;

error:
    g_free (url);

    seaf_block_manager_close_block (seaf->block_mgr, block);
    seaf_block_manager_block_handle_free (seaf->block_mgr, block);

    return ret;
}

/*
int
http_tx_task_download_file_blocks (HttpTxTask *task, const char *file_id)
{
    Seafile *file;
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    int ret = 0;

    file = seaf_fs_manager_get_seafile (seaf->fs_mgr,
                                        task->repo_id,
                                        task->repo_version,
                                        file_id);
    if (!file) {
        seaf_warning ("Failed to find seafile object %s in repo %.8s.\n",
                      file_id, task->repo_id);
        return -1;
    }

    pool = find_connection_pool (priv, task->host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", task->host);
        task->error = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        seafile_unref (file);
        return -1;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", task->host);
        task->error = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        seafile_unref (file);
        return -1;
    }

    int i;
    char *block_id;
    int *pcnt;
    for (i = 0; i < file->n_blocks; ++i) {
        block_id = file->blk_sha1s[i];
        pthread_mutex_lock (&task->ref_cnt_lock);
        if (seaf_block_manager_block_exists (seaf->block_mgr,
                                             task->repo_id, task->repo_version,
                                             block_id)) {
            BlockMetadata *bmd;
            bmd = seaf_block_manager_stat_block (seaf->block_mgr,
                                                 task->repo_id, task->repo_version,
                                                 block_id);
            if (bmd) {
                pcnt = g_hash_table_lookup (task->blk_ref_cnts, block_id);
                if (!pcnt) {
                    pcnt = g_new0(int, 1);
                    g_hash_table_insert (task->blk_ref_cnts, g_strdup(block_id), pcnt);
                }
                *pcnt += 1;
                pthread_mutex_unlock (&task->ref_cnt_lock);
                task->done_download += bmd->size;
                g_free (bmd);
                continue;
            }
        }
        pthread_mutex_unlock (&task->ref_cnt_lock);

        ret = get_block (task, conn, block_id);
        if (ret < 0 || task->state == HTTP_TASK_STATE_CANCELED)
            break;
    }

    connection_pool_return_connection (pool, conn);

    seafile_unref (file);

    return ret;
}
*/

int
http_tx_manager_get_block (HttpTxManager *manager,
                           const char *repo_id,
                           const char *block_id,
                           const char *host,
                           const char *token,
                           gboolean use_fileserver_port,
                           int *error_id,
                           HttpRecvCallback get_blk_cb,
                           void *user_data)
{
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    int status;
    int ret = 0;

    pool = find_connection_pool (priv, host);
    if (!pool) {
        *error_id = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        seaf_warning ("Failed to create connection pool for host %s.\n", host);
        return -1;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        *error_id = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        seaf_warning ("Failed to get connection to host %s.\n", host);
        return -1;
    }

    curl = conn->curl;

    if (!use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/block/%s",
                               host, repo_id, block_id);
    else
        url = g_strdup_printf ("%s/repo/%s/block/%s",
                               host, repo_id, block_id);

    int curl_error;
    if (http_get (curl, url, token, &status, NULL, NULL,
                  get_blk_cb, user_data, TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        *error_id = curl_error_to_http_task_error (curl_error);
        conn->release = TRUE;
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        *error_id = curl_error_to_http_task_error (curl_error);
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
        ret = -1;
        goto out;
    }

out:
    g_free (url);
    connection_pool_return_connection (pool, conn);
    return ret;
}

static int
parse_block_map (const char *rsp_content, gint64 rsp_size,
                 gint64 **pblock_map, int *n_blocks)
{
    json_t *array, *element;
    json_error_t jerror;
    size_t n, i;
    gint64 *block_map = NULL;
    int ret = 0;

    array = json_loadb (rsp_content, rsp_size, 0, &jerror);
    if (!array) {
        seaf_warning ("Failed to load json: %s\n", jerror.text);
        return -1;
    }

    if (json_typeof (array) != JSON_ARRAY) {
        seaf_warning ("Response is not a json array.\n");
        ret = -1;
        goto out;
    }

    n = json_array_size (array);
    block_map = g_new0 (gint64, n);

    for (i = 0; i < n; ++i) {
        element = json_array_get (array, i);
        if (json_typeof (element) != JSON_INTEGER) {
            seaf_warning ("Block map element not an integer.\n");
            ret = -1;
            goto out;
        }
        block_map[i] = (gint64)json_integer_value (element);
    }

out:
    json_decref (array);
    if (ret < 0) {
        g_free (block_map);
        *pblock_map = NULL;
    } else {
        *pblock_map = block_map;
        *n_blocks = (int)n;
    }
    return ret;
}

int
http_tx_manager_get_file_block_map (HttpTxManager *manager,
                                    const char *repo_id,
                                    const char *file_id,
                                    const char *host,
                                    const char *token,
                                    gboolean use_fileserver_port,
                                    gint64 **pblock_map,
                                    int *n_blocks)
{
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn;
    CURL *curl;
    char *url;
    int status;
    char *rsp_content = NULL;
    gint64 rsp_size;
    int ret = 0;

    pool = find_connection_pool (priv, host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", host);
        return -1;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", host);
        return -1;
    }

    curl = conn->curl;

    if (!use_fileserver_port)
        url = g_strdup_printf ("%s/seafhttp/repo/%s/block-map/%s",
                               host, repo_id, file_id);
    else
        url = g_strdup_printf ("%s/repo/%s/block-map/%s",
                               host, repo_id, file_id);

    int curl_error;
    if (http_get (curl, url, token, &status, &rsp_content, &rsp_size,
                  NULL, NULL, TRUE, HTTP_TIMEOUT_SEC, &curl_error) < 0) {
        conn->release = TRUE;
        ret = -1;
        goto out;
    }

    if (status != HTTP_OK) {
        seaf_warning ("Bad response code for GET %s: %d.\n", url, status);
        ret = -1;
        goto out;
    }

    if (parse_block_map (rsp_content, rsp_size, pblock_map, n_blocks) < 0) {
        ret = -1;
        goto out;
    }

out:
    g_free (url);
    g_free (rsp_content);
    connection_pool_return_connection (pool, conn);
    return ret;
}

static int
update_local_repo (HttpTxTask *task)
{
    SeafRepo *repo;
    SeafCommit *new_head;
    SeafBranch *branch;
    int ret = 0;

    new_head = seaf_commit_manager_get_commit (seaf->commit_mgr,
                                               task->repo_id,
                                               task->repo_version,
                                               task->head);
    if (!new_head) {
        seaf_warning ("Failed to get commit %s:%s.\n", task->repo_id, task->head);
        task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
        return -1;
    }

    /* If repo doesn't exist, create it.
     * Note that branch doesn't exist either in this case.
     */
    repo = seaf_repo_manager_get_repo (seaf->repo_mgr, new_head->repo_id);
    if (task->is_clone) {
        gboolean empty_enc_key = FALSE;
        char *value = seaf_repo_manager_get_repo_property (seaf->repo_mgr, new_head->repo_id, REPO_PROP_EMPTY_ENC_KEY);
        if (g_strcmp0(value, "true") == 0)
            empty_enc_key = TRUE;
        g_free (value);

        if (repo != NULL) {
            repo->empty_enc_key = empty_enc_key;
            goto out;
        }

        repo = seaf_repo_new (new_head->repo_id, NULL, NULL);
        if (repo == NULL) {
            /* create repo failed */
            task->error = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
            ret = -1;
            goto out;
        }
        repo->empty_enc_key = empty_enc_key;

        seaf_repo_from_commit (repo, new_head);

        seaf_repo_manager_add_repo (seaf->repo_mgr, repo);

        /* If it's a new repo, create 'local' and 'master' branch */
        branch = seaf_branch_new ("local", task->repo_id, task->head);
        seaf_branch_manager_add_branch (seaf->branch_mgr, branch);
        seaf_branch_unref (branch);

        branch = seaf_branch_new ("master", task->repo_id, task->head);
        seaf_branch_manager_add_branch (seaf->branch_mgr, branch);
        seaf_branch_unref (branch);
    } else {
        if (!repo) {
            task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
            ret = -1;
            goto out;
        }

        branch = seaf_branch_manager_get_branch (seaf->branch_mgr, 
                                                 task->repo_id,
                                                 "master");
        if (!branch) {
            seaf_warning ("Branch master not found for repo %.8s.\n", task->repo_id);
            task->error = SYNC_ERROR_ID_LOCAL_DATA_CORRUPT;
            ret = -1;
            goto out;
        }
        seaf_branch_set_commit (branch, new_head->commit_id);
        seaf_branch_manager_update_branch (seaf->branch_mgr, branch);
        seaf_branch_unref (branch);

        /* Update repo head branch. */
        seaf_branch_set_commit (repo->head, new_head->commit_id);
        seaf_branch_manager_update_branch (seaf->branch_mgr, repo->head);

        if (g_strcmp0 (repo->name, new_head->repo_name) != 0)
            seaf_repo_set_name (repo, new_head->repo_name);
    }

out:
    seaf_commit_unref (new_head);
    return ret;
}

static void *
http_download_thread (void *vdata)
{
    HttpTxTask *task = vdata;
    HttpTxPriv *priv = seaf->http_tx_mgr->priv;
    ConnectionPool *pool;
    Connection *conn = NULL;
    GList *fs_id_list = NULL;

    pool = find_connection_pool (priv, task->host);
    if (!pool) {
        seaf_warning ("Failed to create connection pool for host %s.\n", task->host);
        task->error = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        goto out;
    }

    conn = connection_pool_get_connection (pool);
    if (!conn) {
        seaf_warning ("Failed to get connection to host %s.\n", task->host);
        task->error = SYNC_ERROR_ID_NOT_ENOUGH_MEMORY;
        goto out;
    }

    /* seaf_message ("Download with HTTP sync protocol version %d.\n", */
    /*               task->protocol_version); */

    transition_state (task, task->state, HTTP_TASK_RT_STATE_CHECK);

    if (check_permission (task, conn) < 0) {
        seaf_warning ("Download permission denied for repo %.8s on server %s.\n",
                      task->repo_id, task->host);
        goto out;
    }

    if (task->state == HTTP_TASK_STATE_CANCELED)
        goto out;

    transition_state (task, task->state, HTTP_TASK_RT_STATE_COMMIT);

    if (get_commit_object (task, conn) < 0) {
        seaf_warning ("Failed to get server head commit for repo %.8s on server %s.\n",
                      task->repo_id, task->host);
        goto out;
    }

    if (task->state == HTTP_TASK_STATE_CANCELED)
        goto out;

    transition_state (task, task->state, HTTP_TASK_RT_STATE_FS);

    if (get_needed_fs_id_list (task, conn, &fs_id_list) < 0) {
        seaf_warning ("Failed to get fs id list for repo %.8s on server %s.\n",
                      task->repo_id, task->host);
        goto out;
    }

    if (task->state == HTTP_TASK_STATE_CANCELED)
        goto out;

    while (fs_id_list != NULL) {
        if (get_fs_objects (task, conn, &fs_id_list) < 0) {
            seaf_warning ("Failed to get fs objects for repo %.8s on server %s.\n",
                          task->repo_id, task->host);
            goto out;
        }

        if (task->state == HTTP_TASK_STATE_CANCELED)
            goto out;
    }

    transition_state (task, task->state, HTTP_TASK_RT_STATE_BLOCK);

    /* Record download head commit id, so that we can resume download
     * if this download is interrupted.
     */
    seaf_repo_manager_set_repo_property (seaf->repo_mgr,
                                         task->repo_id,
                                         REPO_PROP_DOWNLOAD_HEAD,
                                         task->head);

    int rc = seaf_repo_fetch_and_checkout (task, task->head);
    switch (rc) {
    case FETCH_CHECKOUT_SUCCESS:
        break;
    case FETCH_CHECKOUT_CANCELED:
        goto out;
    case FETCH_CHECKOUT_FAILED:
        task->error = SYNC_ERROR_ID_WRITE_LOCAL_DATA;
        goto out;
    case FETCH_CHECKOUT_TRANSFER_ERROR:
        goto out;
    case FETCH_CHECKOUT_LOCKED:
        task->error = SYNC_ERROR_ID_FILE_LOCKED_BY_APP;
        goto out;
    }

    update_local_repo (task);

out:
    connection_pool_return_connection (pool, conn);
    string_list_free (fs_id_list);
    return vdata;
}

static void
http_download_done (void *vdata)
{
    HttpTxTask *task = vdata;

    if (task->error != SYNC_ERROR_ID_NO_ERROR)
        transition_state (task, HTTP_TASK_STATE_ERROR, HTTP_TASK_RT_STATE_FINISHED);
    else if (task->state == HTTP_TASK_STATE_CANCELED)
        transition_state (task, task->state, HTTP_TASK_RT_STATE_FINISHED);
    else
        transition_state (task, HTTP_TASK_STATE_FINISHED, HTTP_TASK_RT_STATE_FINISHED);
}

GList*
http_tx_manager_get_upload_tasks (HttpTxManager *manager)
{
    return g_hash_table_get_values (manager->priv->upload_tasks);
}

GList*
http_tx_manager_get_download_tasks (HttpTxManager *manager)
{
    return g_hash_table_get_values (manager->priv->download_tasks);
}

HttpTxTask *
http_tx_manager_find_task (HttpTxManager *manager, const char *repo_id)
{
    HttpTxTask *task = NULL;

    task = g_hash_table_lookup (manager->priv->upload_tasks, repo_id);
    if (task)
        return task;

    task = g_hash_table_lookup (manager->priv->download_tasks, repo_id);
    return task;
}

void
http_tx_manager_cancel_task (HttpTxManager *manager,
                             const char *repo_id,
                             int task_type)
{
    HttpTxTask *task = NULL;

    if (task_type == HTTP_TASK_TYPE_DOWNLOAD)
        task = g_hash_table_lookup (manager->priv->download_tasks, repo_id);
    else
        task = g_hash_table_lookup (manager->priv->upload_tasks, repo_id);

    if (!task)
        return;

    if (task->state != HTTP_TASK_STATE_NORMAL) {
        seaf_warning ("Cannot cancel task not in NORMAL state.\n");
        return;
    }

    if (task->runtime_state == HTTP_TASK_RT_STATE_INIT) {
        transition_state (task, HTTP_TASK_STATE_CANCELED, HTTP_TASK_RT_STATE_FINISHED);
        return;
    }

    /* Only change state. runtime_state will be changed in worker thread. */
    transition_state (task, HTTP_TASK_STATE_CANCELED, task->runtime_state);
}

int
http_tx_task_get_rate (HttpTxTask *task)
{
    return task->last_tx_bytes;
}

const char *
http_task_state_to_str (int state)
{
    if (state < 0 || state >= N_HTTP_TASK_STATE)
        return "unknown";

    return http_task_state_str[state];
}

const char *
http_task_rt_state_to_str (int rt_state)
{
    if (rt_state < 0 || rt_state >= N_HTTP_TASK_RT_STATE)
        return "unknown";

    return http_task_rt_state_str[rt_state];
}
