/* -*-mode: c; indent-tabs-mode: nil; c-basic-offset: 2; -*-
 */

/**
 * mod_clientid - Per client session identifier module
 *                for the Apache web server.
 *
 * See also http://sourceforge.net/projects/mod-csrf/
 *
 * Copyright (C) 2014 Pascal Buchbinder
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/************************************************************************
 * Version
 ***********************************************************************/
static const char g_revision[] = "0.8";

/************************************************************************
 * Includes
 ***********************************************************************/
#include <unistd.h>

/* openssl */
#include <openssl/crypto.h>
#include <openssl/dh.h>
#include <openssl/bn.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/err.h>

/* apr */
#include "apr.h"
#include "apr_strings.h"
#include "apr_strmatch.h"
#include "apr_lib.h"
#include "apr_base64.h"

#define APR_WANT_STRFUNC
#include "apr_want.h"

/* apache */
//#include "ap_config.h"
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_filter.h"
#include "util_md5.h"

/************************************************************************
 * defines
 ***********************************************************************/
#define CLID_LOG_PFX(id)  "mod_clientid("#id"): "
#define CLID_LOGD_PFX  "mod_clientid(): "
#define CLID_RAND_SIZE 10
#define CLID_DELIM     "#"
#define CLID_DELIM_C   '#'
#define CLID_RND       "CLID_RND"
#define CLID_REC       "CLID_REC"
#define CLID_USR_SPE   "mod_clientid::user"
#define CLID_ETAG_N    "mod_clientid::ETag"

// status header (verify|ok) or query
#define CLID_HDR       "X-ClientId"
#define CLID_Q_PDS     "clid_post_data"

#define CLID_HANDLER   "mod_clientid:handler"
#define CLID_COOKIE_N  "chk"
#define CLID_CHECK_URL "/res/clchk.html"
#define CLID_POST_CHECK_URL "/res/clid_P_clchk"
#define CLID_MAXS      83886080

#define CLID_RNDLEN    24
#define CLID_MAX_POSTDATA     1073741824

#define CLID_FIX_FLAGS_IP     0x01
#define CLID_FIX_FLAGS_SSLSID 0x02
#define CLID_FIX_FLAGS_FP     0x04

// Apache 2.4 compat
#if (AP_SERVER_MINORVERSION_NUMBER == 4)
#define SF_CONN_REMOTEIP(r) r->connection->client_ip
#else
#define SF_CONN_REMOTEIP(r) r->connection->remote_ip
#endif

APR_DECLARE_OPTIONAL_FN(int, ssl_is_https, (conn_rec *));
static APR_OPTIONAL_FN_TYPE(ssl_is_https) *clid_is_https = NULL;
APR_DECLARE_OPTIONAL_FN(char *, ssl_var_lookup, (apr_pool_t *, server_rec *,
                                                 conn_rec *, request_rec *, char *));
static APR_OPTIONAL_FN_TYPE(ssl_var_lookup) *clid_ssl_var = NULL;

/* mod_parp, forward and optional function */
APR_DECLARE_OPTIONAL_FN(apr_table_t *, parp_hp_table, (request_rec *));
static APR_OPTIONAL_FN_TYPE(parp_hp_table) *clid_parp_hp_table_fn = NULL;

APR_DECLARE_OPTIONAL_FN(char *, parp_body_data, (request_rec *, apr_size_t *));
static APR_OPTIONAL_FN_TYPE(parp_body_data) *clid_parp_body_data_fn = NULL;

/************************************************************************
 * structures
 ***********************************************************************/
typedef enum  {
  CLID_ACTION_NEW = 0,
  CLID_ACTION_VERIFIED,
  CLID_ACTION_RESET,
  CLID_ACTION_RECHECK
} clid_acton_e;

typedef struct {
  char *ip;
  char *sslsid;
  char *fp;
  char *rnd;
  apr_uint32_t id;
  apr_int32_t generation;
} clid_rec_t;

typedef struct {
  char *indexFile;
  apr_global_mutex_t *lock;
  apr_uint32_t *clientTable;
  apr_uint32_t *clientTableIndex;
  apr_shm_t *m; 
  apr_pool_t *pool;
} clid_user_t;

typedef struct {
  // global, internal config:
  char *lockFile;
  char *pdsPath;
  // per vhost user config:
  int require;                           // policy
  const char *keyName;                   // cookie name
  const char *keyPath;                   // cookie check path
  unsigned char key[EVP_MAX_KEY_LENGTH]; // secret
  const char *check;                     // etag check path
  apr_int32_t maxgeneration;             // how often we recheck
  apr_table_t *fingerprint;
} clid_config_t;

typedef struct {
  int enabled;
} clid_dir_config_t;

/************************************************************************
 * globals
 ***********************************************************************/
module AP_MODULE_DECLARE_DATA clientid_module;
#define CLID_ICASE_MAGIC  ((void *)(&clientid_module))

/************************************************************************
 * private
 ***********************************************************************/

/**
 * Returns the UNIQUE_ID (if mod_uniqueid has been loaded
 * otherwise an empty string).
 */
static const char *clid_unique_id(request_rec *r) {
  const char *id = apr_table_get(r->subprocess_env, "UNIQUE_ID");
  if(id == NULL) {
    id = apr_pstrdup(r->pool, "-");
  }
  return id;
}

static const apr_uint32_t clid_crc32tab[256] = {
  0x00000000, 0x77073096, 0xee0e612c, 0x990951ba,
  0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
  0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
  0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
  0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
  0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
  0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
  0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
  0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
  0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
  0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940,
  0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
  0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116,
  0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
  0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
  0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
  0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a,
  0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
  0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
  0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
  0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
  0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
  0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c,
  0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
  0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
  0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
  0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
  0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
  0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086,
  0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
  0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4,
  0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
  0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
  0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
  0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
  0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
  0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe,
  0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
  0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
  0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
  0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252,
  0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
  0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60,
  0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
  0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
  0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
  0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04,
  0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
  0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
  0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
  0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
  0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
  0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e,
  0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
  0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
  0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
  0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
  0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
  0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
  0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
  0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6,
  0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
  0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
  0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

/**
 * Creates a CRC32 representing the provided string
 */
static apr_uint32_t clid_crc32(const char *data, const apr_size_t data_len) {
  apr_uint32_t i;
  apr_uint32_t crc;
  crc = ~0;
  
  for (i = 0; i < data_len; i++)
    crc = (crc >> 8) ^ clid_crc32tab[(crc ^ (data[i])) & 0xff];
  
  return ~crc;
}

/**
 * Returns the persistent configuration (restarts)
 *
 * @param s To fetch the process pool (s->process->pool)
 * @return
 */
static clid_user_t *clid_get_user_conf(server_rec *s) {
  clid_config_t *conf = ap_get_module_config(s->module_config, 
                                             &clientid_module);
  void *v;
  clid_user_t *u;
  apr_pool_t *ppool = s->process->pool;
  apr_pool_userdata_get(&v, CLID_USR_SPE, ppool);
  if(v) {
    return v;
  }
  u = (clid_user_t *)apr_pcalloc(ppool, sizeof(clid_user_t));
  apr_pool_userdata_set(u, CLID_USR_SPE, apr_pool_cleanup_null, ppool);
  u->lock = NULL;
  u->clientTable = NULL;
  u->clientTableIndex = NULL;
  u->m = NULL;
  apr_pool_create(&u->pool, ppool);
  u->indexFile = apr_psprintf(u->pool, "%s.index", conf->lockFile);
  return u;
}

/**
 * Set a pending etag check
 */
static void clid_table_set(clid_user_t *u, apr_uint32_t id) {
  int i = id / 32;
  int pos = id % 32;
  unsigned int flag = 1;
  flag = flag << pos;
  u->clientTable[i] |= flag;
}

/**
 * Clears a pending etag check
 */
static void clid_table_clear(clid_user_t *u, apr_uint32_t id) {
  int i = id / 32;
  int pos = id % 32;
  unsigned int flag = 1;
  flag = flag << pos;
  flag = ~flag;
  u->clientTable[i] &= flag;
}

/**
 * Tests if the session index is locked to perform an etag check
 */
static int clid_table_test(clid_user_t *u, apr_uint32_t id) {
  int i = id / 32;
  int pos = id % 32;
   unsigned int flag = 1;
   flag = flag << pos;
   if(u->clientTable[i] & flag) {
     return 1;
   }
   return 0;
}

static char *clid_getRequestid(request_rec *r) {
  const char *sec = apr_table_get(r->subprocess_env, CLID_RND);
  char *data = apr_psprintf(r->pool, "%s%s%s%s",
                            r->unparsed_uri,
                            sec,
                            strchr(r->unparsed_uri, '?') ? "&" : "?",
                            CLID_Q_PDS);
  char *md = ap_md5_binary(r->pool, (unsigned char *)data, strlen(data));
  return md;
}

static int clid_hasbody(request_rec *r) {
  const char *cl = apr_table_get(r->headers_in, "Content-Length");
  const char *te = apr_table_get(r->headers_in, "Transfer-Encoding");
  if(cl) {
    return 1;
  }
  if(te) {
    if(strcasecmp(te, "chunked") == 0) {
      return 1;
    }
  }
  if(r->read_chunked) {
    return 1;
  }
  return 0;
}

/**
 * Returns the configured server name supporting ServerAlias directive.
 *
 * @param r
 * @param server_hostname
 * @return hostname
 */
static char *clid_server_alias(request_rec *r, const char *server_hostname) {
  char *server = apr_pstrdup(r->pool, r->server->server_hostname);
  char *p;
  if(server_hostname) {
    if(strcasecmp(server_hostname, r->server->server_hostname) == 0) {
      /* match ServerName */
      server = apr_pstrdup(r->pool, r->server->server_hostname);
    } else if(r->server->names) {
      int i;
      apr_array_header_t *names = r->server->names;
      char **name = (char **)names->elts;
      for(i = 0; i < names->nelts; ++i) {
        if(!name[i]) continue;
        if(strcasecmp(server_hostname, name[i]) == 0) {
          /* match ServerAlias */
          server = apr_pstrdup(r->pool, name[i]);
        }
      }
    } else if(r->server->wild_names) {
      int i;
      apr_array_header_t *names = r->server->wild_names;
      char **name = (char **)names->elts;
      for(i = 0; i < names->nelts; ++i) {
        if(!name[i]) continue;
        if(!ap_strcasecmp_match(server_hostname, name[i]))
          /* match ServerAlias using wildcards */
          server = apr_pstrdup(r->pool, server_hostname);
      }
    }
  }
  p = strchr(server, ':');
  if(p) {
    p[0] = '\0';
  }
  return server;
}

/** 
 * Returns the url to this server, e.g. https://server1 or http://server1:8080
 * used for redirects.
 *
 * @param r
 * @return schema/hostname
 */
static char *clid_this_host(request_rec *r) {
  const char *hostport= apr_table_get(r->headers_in, "Host");
  int port = 0;
  int ssl = 0;
  int default_port;
  const char *server_hostname = r->server->server_hostname;
  if(clid_is_https) {
    ssl = clid_is_https(r->connection);
  }
  if(hostport) {
    char *p;
    hostport = apr_pstrdup(r->pool, hostport);
    if((p = strchr(hostport, ':')) != NULL) {
      server_hostname = clid_server_alias(r, hostport);
      p[0] = '\0';
      p++;
      port = atoi(p);
    } else {
      server_hostname = clid_server_alias(r, hostport);
    }
  }
  if(port == 0) {
    // pref. vhost
    port = r->server->addrs->host_port;
  }
  if(port == 0) {
    // main srv
    port = r->server->port;
  }
  default_port = ssl ? 443 : 80;
  if(port == default_port) {
    return apr_psprintf(r->pool, "%s%s",
                        ssl ? "https://" : "http://",
                        server_hostname);
  }
  return apr_psprintf(r->pool, "%s%s:%d",
                      ssl ? "https://" : "http://",
                      server_hostname,
                      port);
}

/**
 * Decryptes encrypted and base64 encoded string. See clid_enc64().
 *
 * @param r
 * @param str String to decrypt
 * @return Decrypted string or NULL on error
 */
static char *clid_dec64(request_rec *r, const char *str, unsigned char *key) {
  EVP_CIPHER_CTX cipher_ctx;
  int len = 0;
  int buf_len = 0;
  unsigned char *buf;
  char *dec = (char *)apr_palloc(r->pool, 1 + apr_base64_decode_len(str));
  int dec_len = apr_base64_decode(dec, str);
  buf = apr_pcalloc(r->pool, dec_len);

  EVP_CIPHER_CTX_init(&cipher_ctx);
  EVP_DecryptInit(&cipher_ctx, EVP_des_ede3_cbc(), key, NULL);
  if(!EVP_DecryptUpdate(&cipher_ctx, (unsigned char *)&buf[buf_len], &len,
                        (const unsigned char *)dec, dec_len)) {
    goto failed;
  }
  buf_len+=len;
  if(!EVP_DecryptFinal(&cipher_ctx, (unsigned char *)&buf[buf_len], &len)) {
    goto failed;
  }
  buf_len+=len;
  EVP_CIPHER_CTX_cleanup(&cipher_ctx);
  
  if(buf_len < CLID_RAND_SIZE) {
    goto failed;
  }
  if(buf[CLID_RAND_SIZE-1] != 'A') {
    goto failed;
  }
  buf = &buf[CLID_RAND_SIZE];
  buf_len = buf_len - CLID_RAND_SIZE;

  return apr_pstrndup(r->pool, (char *)buf, buf_len);
     
 failed:
  EVP_CIPHER_CTX_cleanup(&cipher_ctx);
  ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                CLID_LOGD_PFX"Failed to decrypt data (%s), id=%s",
                str ? str : "",
                clid_unique_id(r));
  return NULL;
}

/**
 * Encrypts and base64 encodes a string. See clid_dec64().
 *
 * @param r
 * @param str String to encrypt
 * @return Encrypted string.
 */
static char *clid_enc64(request_rec *r, const char *str, unsigned char *key) {
  char *e;
  EVP_CIPHER_CTX cipher_ctx;
  int buf_len = 0;
  int len = 0;
  unsigned char *rand = apr_pcalloc(r->pool, CLID_RAND_SIZE);
  unsigned char *buf = apr_pcalloc(r->pool,
                                   CLID_RAND_SIZE +
                                   strlen(str) +
                                   EVP_CIPHER_block_size(EVP_des_ede3_cbc()));
  RAND_bytes(rand, CLID_RAND_SIZE);
  rand[CLID_RAND_SIZE-1] = 'A';
  EVP_CIPHER_CTX_init(&cipher_ctx);
  EVP_EncryptInit(&cipher_ctx, EVP_des_ede3_cbc(), key, NULL);
  if(!EVP_EncryptUpdate(&cipher_ctx, &buf[buf_len], &len,
                        rand, CLID_RAND_SIZE)) {
    goto failed;
  }
  buf_len+=len;
  if(!EVP_EncryptUpdate(&cipher_ctx, &buf[buf_len], &len,
                        (const unsigned char *)str, strlen(str))) {
    goto failed;
  }
  buf_len+=len;
  if(!EVP_EncryptFinal(&cipher_ctx, &buf[buf_len], &len)) {
    goto failed;
  }
  buf_len+=len;
  EVP_CIPHER_CTX_cleanup(&cipher_ctx);

  e = (char *)apr_pcalloc(r->pool, 1 + apr_base64_encode_len(buf_len));
  len = apr_base64_encode(e, (const char *)buf, buf_len);
  e[len] = '\0';
  return e;

failed:
  EVP_CIPHER_CTX_cleanup(&cipher_ctx);
  ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                CLID_LOGD_PFX"Failed to encrypt data, id=%s",
                clid_unique_id(r));
  return "";
}

/**
 * Creates radom characters
 *
 * @param pool
 * @param len Number of bytes to return
 * @return Random string
 */
static char *clid_func_RND(apr_pool_t *pool, apr_int64_t len) {
  char *buf;
  unsigned char *secret = apr_pcalloc(pool, len);
  int first = len / 2;
#if APR_HAS_RANDOM
  apr_generate_random_bytes(secret, first);
#else
  RAND_bytes(secret, first);
#endif
  RAND_bytes(&secret[first], len - first);
  buf = (char *)apr_pcalloc(pool, 1 + apr_base64_encode_len(len));
  apr_base64_encode(buf, (const char *)secret, len);
  buf[len] = '\0';
  return buf;
}

/**
 * Extract the session cookie from the request.
 *
 * @param r
 * @param name Name of the cookie
 * @return Cookie or NULL if not found within the request headers
 */
static char *clid_get_remove_cookie(request_rec *r, const char *name) {
  const char *cookie_h = apr_table_get(r->headers_in, "Cookie");
  if(cookie_h) {
    char *cn = apr_pstrcat(r->pool, name, "=", NULL);
    char *pt = ap_strcasestr(cookie_h, cn);
    char *p = NULL;
    while(pt && !p) {
      // ensure we found the real cookie (and not an ending b64 str)
      if(pt == cookie_h) {
        // @beginning of the header
        p = pt;
        pt = NULL;
      } else {
        char pre = pt[-1];
        if(pre == ' ' ||
           pre == ';') {
          // @beginnin of a cookie
          p = pt;
          pt = NULL;
        } else {
          // found patter somewhere else
          pt++;
          pt = ap_strcasestr(pt, cn);
        }
      }
    }
    if(p) {
      char *value = NULL;
      char *clean = p;
      if(clean > cookie_h) {
        clean--;
        while(clean > cookie_h && clean[0] == ' ') {
          clean[0] = '\0';
          clean--;
        }
      }
      p[0] = '\0'; /* terminate the beginning of the cookie header */
      p = p + strlen(cn);
      value = ap_getword(r->pool, (const char **)&p, ';');
      while(p && (p[0] == ' ')) p++;
      /* skip a path, if there is any */
      if(p && (strncasecmp(p, "$path=", strlen("$path=")) == 0)) {
        ap_getword(r->pool, (const char **)&p, ';');
      }
      /* restore cookie header */
      if(p && p[0]) {
        if(cookie_h[0]) {
          cookie_h = apr_pstrcat(r->pool, cookie_h, " ", p, NULL);
        } else {
          cookie_h = apr_pstrcat(r->pool, p, NULL);
        }
      }
      if(strlen(cookie_h) == 0) {
        apr_table_unset(r->headers_in, "cookie");
      } else {
        if((strncasecmp(cookie_h, "$Version=", strlen("$Version=")) == 0) &&
           (strlen(cookie_h) <= strlen("$Version=X; "))) {
          /* nothing left */
          apr_table_unset(r->headers_in, "cookie");
        } else {
          apr_table_set(r->headers_in, "Cookie", cookie_h);
        }
      }
      return value;
    }
  }
  return NULL;
}

/**
 * Returns the SSL session id of the current connection
 *
 * @param r
 * @return ID as a string (or a string containing "null" for plain txt conn)
 */
static char *clid_getsslsid(request_rec *r) {
  char *sslsid = NULL;
  if(clid_ssl_var) {
    char *s = clid_ssl_var(r->pool, r->server, r->connection, r, "SSL_SESSION_ID");
    if(s) {
      sslsid = apr_pstrdup(r->pool, s);
    }
  }
  if(!sslsid) {
    sslsid = apr_pstrdup(r->pool, "null");
  }
  return sslsid;
}

static char *clid_create_etag(apr_pool_t *pool, clid_rec_t *rec) {
  char *md = ap_md5_binary(pool, (unsigned char *)rec->rnd, CLID_RNDLEN-4);
  /* TODO: it's obvious, that this ETag is not the usual "inode-size-mtime" thing
   *       md[6] = '-';
   *       md[10] = '-';
   *       md[24] = '\0'; */
  return md;
}

/**
 * Retruns a HTTP fingerprint of the client
 *
 * @param r Request record to extract the data from (request header,
 *          SSL attributes etc)
 * @param data Additional data to add to the fingerprint
 * @return String representing the client fingerprint
 */
static char *clid_clientfp(request_rec *r, const char *data) {
  clid_config_t *conf = ap_get_module_config(r->server->module_config, 
                                             &clientid_module);
  apr_table_entry_t *entry = (apr_table_entry_t *)apr_table_elts(conf->fingerprint)->elts;
  char *fp = "";
  apr_uint32_t crc;
  char *id;
  int idlen;
  int i;
  for(i = 0; i < apr_table_elts(conf->fingerprint)->nelts; i++) {
    const char *v = NULL;
    if(entry[i].val[0] == 'S') {
      // prefer mod_ssl variables
      if(clid_ssl_var) {
        v = clid_ssl_var(r->pool, r->server, r->connection, r, entry[i].key);
      }
    }
    if(v == NULL) {
      // "fallback" to request header
      v = apr_table_get(r->headers_in, entry[i].key);
    }
    if(v) {
      fp = apr_pstrcat(r->pool, fp, v, NULL);
    }
  }
  crc = clid_crc32(fp, strlen(fp));
  idlen = apr_base64_encode_len(sizeof(apr_uint32_t));
  id = (char *)apr_pcalloc(r->pool, idlen + 1);
  apr_base64_encode(id, (const char *)&crc, sizeof(apr_uint32_t));
  id[idlen] = '\0';
  //return apr_psprintf(r->pool, "%ud", crc);
  // TODO fingerprint is nothing secure/unique so a short crc32 might be enough
  //md = ap_md5_binary(r->pool, (unsigned char *)fp, strlen(fp));
  return id;
}

/**
 * Create a new client record containing the IP, SSL session id,
 * fingerprint and a random client id.
 *
 * @param r
 * @param rnd Existing random which shall be re-used (optional)
 * @param id Used together with an existing radnom
 * @return New client record
 */
static clid_rec_t *clid_create_rec(request_rec *r, const char *rnd, 
                                   apr_uint32_t id) {
  clid_rec_t *rec = apr_pcalloc(r->pool, sizeof(clid_rec_t));
  rec->ip = apr_pstrdup(r->pool, SF_CONN_REMOTEIP(r));
  rec->sslsid = clid_getsslsid(r);
  rec->fp = clid_clientfp(r, NULL);
  if(rnd) {
    rec->rnd = apr_pstrdup(r->pool, rnd);
    rec->id = id;
  } else {
    clid_user_t *u = clid_get_user_conf(r->server);
    rec->rnd = clid_func_RND(r->pool, CLID_RNDLEN);
    rec->generation = 0;
    apr_global_mutex_lock(u->lock);          /* @CRT01 */
    rec->id = *u->clientTableIndex;
    *u->clientTableIndex = rec->id + 1;
    if(*u->clientTableIndex == CLID_MAXS) {
      /* reuse existing entries
         -> we might have clients still using them */
      *u->clientTableIndex = 0;
    }
    clid_table_clear(u, rec->id);
    apr_global_mutex_unlock(u->lock);        /* @CRT01 */
  }
  return rec;
}

static clid_rec_t *clid_rec_d2i(apr_pool_t *pool, const char *str) {
  clid_rec_t *rec = apr_pcalloc(pool, sizeof(clid_rec_t));
  char *id;
  char *gen;
  rec->ip = apr_pstrdup(pool, str);
  rec->sslsid = strchr(rec->ip, CLID_DELIM_C);
  if(rec->sslsid == NULL) {
    return NULL;
  }
  rec->sslsid[0] = '\0';
  rec->sslsid++;
  rec->fp = strchr(rec->sslsid, CLID_DELIM_C);
  if(rec->fp == NULL) {
    return NULL;
  }
  rec->fp[0] = '\0';
  rec->fp++;
  rec->rnd = strchr(rec->fp, CLID_DELIM_C);
  if(rec->rnd == NULL) {
    return NULL;
  }
  rec->rnd[0] = '\0';
  rec->rnd++;
  id = strchr(rec->rnd, CLID_DELIM_C);
  if(id == NULL) {
    return NULL;
  }
  id[0] = '\0';
  id++;
  gen = strchr(id, CLID_DELIM_C);
  if(gen == NULL) {
    return NULL;
  }
  gen[0] = '\0';
  gen++;
  rec->id = atoi(id);
  rec->generation = atoi(gen);
  return rec;
}

static char *clid_rec_i2d(apr_pool_t *pool, clid_rec_t *rec) {
  char *id = apr_psprintf(pool,
                          "%s"CLID_DELIM"%s"CLID_DELIM"%s"CLID_DELIM"%s"CLID_DELIM"%u"CLID_DELIM"%d",
                          rec->ip,
                          rec->sslsid,
                          rec->fp,
                          rec->rnd,
                          rec->id,
                          rec->generation);
  return id;
}

static char *clid_create_cookie(request_rec *r, clid_config_t *conf, 
                                clid_rec_t *rec) {
  char *payload = clid_rec_i2d(r->pool, rec);
  char *sc = apr_pstrcat(r->pool,
                         conf->keyName, "=",
                         clid_enc64(r, payload, conf->key),
                         "; httpOnly; Path=/",
                         NULL);
  /* TODO we don't set the "secure" flag at the moment (sometimes,
     the cookie may be used non https requests too and there is 
     still the option of using the Strict-Transport-Security
     response header) */
  apr_table_set(r->subprocess_env, CLID_REC, payload);
  ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                    CLID_LOGD_PFX"Create new cookie [%s], id=%s",
                    payload, clid_unique_id(r));
  return sc;
}

/**
 * Client needs to access the check pae and proves it is the orignal
 * client by sending the correct ETag
 */ 
static int clid_redirect2check(request_rec *r, clid_config_t *conf) {
  char *cookieName = apr_pstrcat(r->pool, conf->keyName, CLID_COOKIE_N, NULL); 
  char *redirectCookie = apr_pstrcat(r->pool, cookieName, "=",
                                 clid_enc64(r, r->unparsed_uri, conf->key),
                                 "; Max-Age=8; Path=", conf->check, NULL);
  char *redirect_page = apr_psprintf(r->pool, "%s%s",
                                     clid_this_host(r),
                                     conf->check);
  int rc = HTTP_MOVED_TEMPORARILY;

  /* Notes about POST requests (body data):
   * - We have to store the request body because the browser
   *   won't send us the If-None-Match header if we answer
   *   by a 307.
   *   While we could accept this limitation for the initial
   *   id creation, we really need a solution when dealing
   *   with rule violations.
   *   We use either an internal post data store (pds) of
   *   store the data at the client within a HTML page and
   *   use JavaScript to do the ETag check and re-post the
   *   data.
   * - Browser (FF) never sends the If-None-Match header
   *   to 302/307 responses. But it does of redirected
   *   twice. This is true if post is submitted by a
   *   regular HTML form but ajax requests may react
   *   different...
   */
  if(conf->pdsPath == NULL) {
    // not post data store => use client side data store
    if(r->method_number == M_POST) {
      const char *contentType = apr_table_get(r->headers_in, "Content-Type");
      if(contentType != NULL) {
        apr_table_add(r->notes, "parp", "mod_clientid"); 
      }
      r->handler = apr_pstrdup(r->pool, CLID_HANDLER);
      apr_table_set(r->notes, CLID_HANDLER, CLID_HANDLER);
      return DECLINED;
    } else {
      if(clid_hasbody(r)) {
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                      CLID_LOG_PFX(30)"Request has body"
                      " but method '%s' is not supported, id=%s",
                      r->method,
                      clid_unique_id(r));
      }
      ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                    CLID_LOGD_PFX"Redirect to ETag check page, id=%s",
                    clid_unique_id(r));
      apr_table_add(r->err_headers_out, "Set-Cookie", redirectCookie);
      apr_table_set(r->headers_out, "Location", redirect_page);
      return HTTP_MOVED_TEMPORARILY;
    }
  } else {
    // using the post data store...
    if(clid_hasbody(r)) {
      char *origUri = apr_psprintf(r->pool, "%s%s%s",
                                   r->unparsed_uri,
                                   strchr(r->unparsed_uri, '?') ? "&" : "?",
                                   CLID_Q_PDS);
      char *reqid = clid_getRequestid(r); 
      // POST (body data) requies two redirects
      redirect_page = apr_psprintf(r->pool, "%s%s",
                                   clid_this_host(r),
                                   CLID_POST_CHECK_URL);
      /* and we need to store the request data in order to
       * inject it when returing from the check page */
      redirectCookie = apr_pstrcat(r->pool, cookieName, "=",
                                   clid_enc64(r, origUri, conf->key),
                                   "; Max-Age=8; Path=", conf->check, NULL);
      apr_table_set(r->notes, CLID_Q_PDS, reqid);
      rc = DECLINED;
    }
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                  CLID_LOGD_PFX"Redirect to ETag check page, id=%s",
                  clid_unique_id(r));
    apr_table_add(r->err_headers_out, "Set-Cookie", redirectCookie);
    apr_table_set(r->headers_out, "Location", redirect_page);
    return rc;
  }
}

/**
 * Verifies or creates the client id
 *
 * @param r
 * @param conf
 * @return DECLINED or 30x (cookie check/renew/recheck)
 */
static int clid_setid(request_rec *r, clid_config_t *conf) {
  clid_user_t *u = clid_get_user_conf(r->server);
  const char *verified = apr_table_get(r->subprocess_env, CLID_REC);
  clid_rec_t *rec = NULL;
  clid_rec_t *newrec = NULL;
  clid_acton_e action = CLID_ACTION_NEW;
  if(verified) {
    rec = clid_rec_d2i(r->pool, verified);
  }
  if(rec) {
    int require = 0;
    int changed = 0;
    action = CLID_ACTION_VERIFIED;
    newrec = clid_create_rec(r, rec->rnd, rec->id);
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                  CLID_LOGD_PFX"Found valid cookie [%s], id=%s",
                  verified, clid_unique_id(r));
    if(conf->require & CLID_FIX_FLAGS_IP) {
      require++;
      if(strcmp(rec->ip, newrec->ip)) {
        // ip has changed
        changed++;
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                      CLID_LOGD_PFX"IP has changed, id=%s",
                      clid_unique_id(r));
      }
    }
    if(conf->require & CLID_FIX_FLAGS_SSLSID) {
      require++;
      if(strcmp(rec->sslsid, newrec->sslsid)) {
        // ssl session id has changed
        changed++;
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                      CLID_LOGD_PFX"SSL session id has changed, id=%s",
                      clid_unique_id(r));
      }
    }
    if(conf->require & CLID_FIX_FLAGS_FP) {
      require++;
      if(strcmp(rec->fp, newrec->fp)) {
        // fingerprint has changed
        changed++;
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                      CLID_LOGD_PFX"Fingerprint has changed, id=%s",
                      clid_unique_id(r));
      }
    }
    if(changed > 0) {
      if(changed >= require) {
        // rule violation
        action = CLID_ACTION_RECHECK;
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                      CLID_LOGD_PFX"Action: check ETag, id=%s",
                      clid_unique_id(r));
        if(conf->maxgeneration > 0) {
          if(rec->generation >= conf->maxgeneration) {
            char *sc = clid_create_cookie(r, conf, newrec);
            action = CLID_ACTION_RESET;
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                          CLID_LOGD_PFX"Max generations - skip check ETag, id=%s",
                          clid_unique_id(r));
            apr_table_add(r->err_headers_out, "Set-Cookie", sc);
          }
        }
      } else {
        // renew cookie
        char *sc = clid_create_cookie(r, conf, newrec);
        action = CLID_ACTION_RESET;
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                      CLID_LOGD_PFX"Action: set new cookie, id=%s",
                      clid_unique_id(r));
        apr_table_add(r->err_headers_out, "Set-Cookie", sc);
      }
    }
  }

  /*
   * temp (POST) check page
   */
  if(r->parsed_uri.path && 
     (strcmp(CLID_POST_CHECK_URL, r->parsed_uri.path) == 0)) {
    char *redirect_page = apr_psprintf(r->pool, "%s%s",
                                       clid_this_host(r),
                                       conf->check);
    apr_table_set(r->headers_out, "Location", redirect_page);
    apr_table_add(r->err_headers_out, "Cache-Control", "no-cache, no-store");
    return HTTP_MOVED_TEMPORARILY;
  }

  /* 
   * cookie check page
   */
  if(r->parsed_uri.path && 
     (strcmp(conf->keyPath, r->parsed_uri.path) == 0)) {
    apr_table_add(r->headers_out, "Cache-Control", "no-cache, no-store");
    if(verified) {
      if(r->parsed_uri.query &&  
         (strncmp(r->parsed_uri.query, "r=", 2) == 0)) {
        /* client has send a cookie, redirect to original url */
        char *q = r->parsed_uri.query;
        char *origUrl = clid_dec64(r, &q[2], conf->key);
        if(origUrl) {
          char *cookieName = apr_pstrcat(r->pool, conf->keyName, 
                                         CLID_COOKIE_N, NULL); 
          char *redirectCookie = apr_pstrcat(r->pool, cookieName, "=",
                                             clid_enc64(r, origUrl, conf->key),
                                             "; Max-Age=8; Path=", conf->check, NULL);
          char *redirect_page = apr_psprintf(r->pool, "%s%s",
                                             clid_this_host(r),
                                             conf->check);
          ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                        CLID_LOGD_PFX"Cookie check page from [%s] "
                        "=> redirect to ETag check, id=%s",
                        origUrl, clid_unique_id(r));
          apr_table_set(r->headers_out, "Location", redirect_page);
          apr_table_add(r->err_headers_out, "Set-Cookie", redirectCookie);
          apr_table_add(r->err_headers_out, "Cache-Control", "no-cache, no-store");
          return HTTP_MOVED_TEMPORARILY;
        } else {
          // return the cookie check page
          ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                        CLID_LOG_PFX(021)"Cookie check failed "
                        "(no valid request), id=%s",
                        clid_unique_id(r));
          return DECLINED;
        }
      } else {
        // return the cookie check page
        if(r->args && strcmp(r->args, CLID_HDR) == 0) {
          // ajax unlock call
          apr_table_add(r->headers_out, CLID_HDR, "ok");
          ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                        CLID_LOGD_PFX"Ajax call to Cookie check, id=%s",
                        clid_unique_id(r));
        } else {
          ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                        CLID_LOGD_PFX"Cookie check failed "
                        "(no valid request), id=%s",
                        clid_unique_id(r));
        }
        return DECLINED;
      }
    } else {
      // return the cookie check page (no cookie available)
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                    CLID_LOG_PFX(022)"Cookie check failed (no valid cookie), id=%s",
                    clid_unique_id(r));
      return DECLINED;
    }
  }

  /*
   * new client, no cookie
   */
  if(action == CLID_ACTION_NEW) {
    /* new request, create a cookie */
    clid_rec_t *rec = clid_create_rec(r, NULL, 0);
    char *sc = clid_create_cookie(r, conf, rec);
    char *redirect_page = apr_pstrcat(r->pool, clid_this_host(r),
                                      conf->keyPath,
                                      "?r=",
                                      clid_enc64(r, r->unparsed_uri, 
                                                 conf->key),
                                      NULL);
    apr_table_set(r->subprocess_env, CLID_RND, rec->rnd);
    apr_table_set(r->subprocess_env, CLID_REC, clid_rec_i2d(r->pool, rec));
    apr_table_set(r->headers_out, "Location", redirect_page);
    apr_table_add(r->err_headers_out, "Set-Cookie", sc);
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                  CLID_LOGD_PFX"Redirect to cookie check page, id=%s",
                  clid_unique_id(r));
    return HTTP_MOVED_TEMPORARILY;
  }
  
  /*
   * ETag/If-None-Match check path
   */
  if(r->parsed_uri.path && (strcmp(conf->check, r->parsed_uri.path) == 0)) {
    const char *origPath = conf->keyPath;
    char *cookieName = apr_pstrcat(r->pool, conf->keyName, 
                                   CLID_COOKIE_N, NULL); 
    char *redirect_page;
    char *etagStr = clid_create_etag(r->pool, rec);
    char *redirectCookie = clid_get_remove_cookie(r, cookieName);
    if(redirectCookie) {
      char *verifiedRedirectCookie = clid_dec64(r, redirectCookie, 
                                                conf->key);
      if(verifiedRedirectCookie) {
        origPath = verifiedRedirectCookie;
      }
    }
    redirect_page = apr_pstrcat(r->pool, clid_this_host(r), origPath, NULL);
    
    /*
     * existing cookie is (at least partially) valid
     */
    if(action == CLID_ACTION_VERIFIED || action == CLID_ACTION_RESET) {
      /* first request with a matching cookie:
       * - set tag
       * - redirect to orignal page */
      apr_table_add(r->err_headers_out, "ETag", etagStr);
      apr_table_set(r->headers_out, "Location", redirect_page);
      ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                    CLID_LOGD_PFX"Set ETag (%s) and redirect "
                    "to original url [%s], id=%s",
                    etagStr,
                    origPath,
                    clid_unique_id(r));
      return HTTP_MOVED_TEMPORARILY;
    }
    /*
     * we need to check the etag
     */
    if(action == CLID_ACTION_RECHECK) {
      const char *reqETag = apr_table_get(r->headers_in, "If-None-Match");
      if((reqETag != NULL) && (reqETag[0] == '"')) {
        // strip leading/tailing double quotes
        char *tag = apr_pstrdup(r->pool, &reqETag[1]);
        char *tagstart = tag;
        while(tag && tag[0]) {
          if(tag[0] == '"') {
            tag[0] = '\0';
          }
          tag++;
        }
        reqETag = tagstart;
      }
      /* followup request
       * - check tag
       * - redirect to orignal page if tag was correct and set new cookie
       */
      if((reqETag != NULL) && 
         strcmp(reqETag, etagStr) == 0) {
        // ok, redirect to orignal page and set new cookie
        char *sc;
        if(apr_table_get(r->headers_in, CLID_HDR)) {
          // ajax unlock call, don't call the application
          redirect_page = apr_pstrcat(r->pool, clid_this_host(r),
                                      conf->keyPath, "?"CLID_HDR ,NULL);
        }
        newrec->generation = rec->generation + 1; // increment the generation counter
        sc = clid_create_cookie(r, conf, newrec);
        apr_table_add(r->err_headers_out, "ETag", etagStr);
        apr_table_set(r->headers_out, "Location", redirect_page);
        apr_table_add(r->err_headers_out, "Set-Cookie", sc);
        apr_global_mutex_lock(u->lock);          /* @CRT03 */
        clid_table_clear(u, rec->id);
        apr_global_mutex_unlock(u->lock);        /* @CRT03 */
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                      CLID_LOGD_PFX"Received valid ETag, redirect to '%s', id=%s",
                      redirect_page,
                      clid_unique_id(r));
        return HTTP_MOVED_TEMPORARILY;
      }
      // failed: show page and clear cookies
      {
        char *clearCookie = apr_pstrcat(r->pool, conf->keyName,
                                        "=; Max-Age=0; Path=/", NULL);
        char *clearETAGCookie = apr_pstrcat(r->pool, conf->keyName, CLID_COOKIE_N,
                                            "=; Max-Age=0; Path=", conf->check, NULL); 
        apr_table_add(r->err_headers_out, CLID_HDR, "failed");
        apr_table_add(r->err_headers_out, "Set-Cookie", clearCookie);
        apr_table_add(r->err_headers_out, "Set-Cookie", clearETAGCookie);
        /* page should not be cached but we won't set 
           "Cache-Control: no-cache, no-store" neither. better: unset ETag */
        apr_table_set(r->notes, CLID_ETAG_N, "");
      }
      ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                    CLID_LOG_PFX(023)"ETag check failed, id=%s",
                    clid_unique_id(r));
      return DECLINED;
    }
  }

  /*
   * lock request or recheck etag
   */
  if(action == CLID_ACTION_RECHECK) {
    char *redirect_page;
    int loop = 0;
    int checkRunning = 0;
    int startRecheck = 0;
    apr_global_mutex_lock(u->lock);          /* @CRT02 */
    checkRunning = clid_table_test(u, rec->id);
    if(!checkRunning) {
      startRecheck = 1;
      clid_table_set(u, rec->id);
    }
    apr_global_mutex_unlock(u->lock);        /* @CRT02 */
    if(startRecheck) {
      return clid_redirect2check(r, conf);
    }
    // wait to get access
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                  CLID_LOGD_PFX"Start waiting for ETag unlock, id=%s",
                  clid_unique_id(r));
    while(checkRunning) {
      loop++;
      sleep(1);
      apr_global_mutex_lock(u->lock);          /* @CRT04 */
      checkRunning = clid_table_test(u, rec->id);
      apr_global_mutex_unlock(u->lock);        /* @CRT04 */
      if(!checkRunning) {
        /* another request has unlocked the session and received
           a new client id cookie */
        break;
      }
      if(loop > 10) {
        /* no other request has unlocked the session yet
           => start another ETag check (try unlocking the session) */
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      CLID_LOG_PFX(020)"Timeout while "
                      "waiting for unlock by ETag check, id=%s",
                      clid_unique_id(r));
        return clid_redirect2check(r, conf);
      }
    }
    // lets the client re-try with the new cookie he should have received
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                  CLID_LOGD_PFX"Unlocked, redirect to inital page, id=%s",
                  clid_unique_id(r));
    redirect_page = apr_psprintf(r->pool, "%s%s",
                                 clid_this_host(r),
                                 r->unparsed_uri);
    apr_table_set(r->headers_out, "Location", redirect_page);
    return HTTP_TEMPORARY_REDIRECT; // 307 should always work here
  }
  return DECLINED;
}

/************************************************************************
 * handlers
 ***********************************************************************/

static int clid_fixup(request_rec *r) {
  if(ap_is_initial_req(r)) {
    if(apr_table_get(r->notes, CLID_HANDLER)) {
      // ensure the mod_clientid handler is used
      r->handler = apr_pstrdup(r->pool, CLID_HANDLER);
    }
  }
  return DECLINED;
}

/* CLID_USE_JS enables POST request handling using a HTML page which
 * performs the ETag check call and sends the POST request again. */

/**
 * Returns the HTML page containing the JS to re-check POST requests
 */
static int clid_handler(request_rec * r) {
  clid_config_t *conf = ap_get_module_config(r->server->module_config, 
                                             &clientid_module);
  if(conf->pdsPath != NULL) {
    // uses post data store
    return DECLINED;
  }
  if(strcmp(r->handler, CLID_HANDLER) == 0) {
    apr_table_t *params = NULL;
    const char *contentType = apr_table_get(r->headers_in, "Content-Type");
    if(contentType == NULL) {
      contentType = apr_pstrdup(r->pool, "");
    }
    if(clid_parp_hp_table_fn) {
      params = clid_parp_hp_table_fn(r);
    }
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, r,
                  CLID_LOGD_PFX"Return verifcation page, id=%s",
                  clid_unique_id(r));
    
    // this is not the application's page, don't allow caching:
    apr_table_add(r->headers_out, "Cache-Control", "no-cache, no-store");

    /* TODO the JS won't be processed by clients not expecting a html page (ajax)
     * => detect those clients and provide an alternative check */
    // allows custom js (ajax call) to detect and handle this response:
    apr_table_add(r->headers_out, CLID_HDR, "verify");

    ap_set_content_type(r, "text/html");

    ap_rprintf(r, "<html><head><title>Session Validation</title>\n");
    ap_rprintf(r, "<script type=\"text/javascript\">\n");
    ap_rprintf(r, "var checkLink = \"%s\";\n", ap_escape_html(r->pool, conf->check));
    ap_rprintf(r, "var checkReq;\n");
    ap_rprintf(r, "if(window.XMLHttpRequest) {\n");
    ap_rprintf(r, " checkReq = new XMLHttpRequest();\n");
    ap_rprintf(r, "} else {\n");
    ap_rprintf(r, " checkReq = new ActiveXObject(\"Microsoft.XMLHTTP\");\n");
    ap_rprintf(r, "}\n");
    ap_rprintf(r, "checkReq.open(\"GET\", checkLink, true);\n");
    ap_rprintf(r, "checkReq.setRequestHeader(\""CLID_HDR"\", \"ping\");\n");
    ap_rprintf(r, "checkReq.onreadystatechange = function() {\n");
    ap_rprintf(r, "  if(checkReq.readyState == 4){\n");
    ap_rprintf(r, "     if(checkReq.status == 200) {\n");
    /* we could check the response but it does not really matter: if the ETag check
     * has failed, the next request starts a new session/client id cookie */
    //ap_rprintf(r, "       var resH = this.getResponseHeader(\"%s\");\n", CLID_HDR);
    //ap_rprintf(r, "       if(resH != null) {\n");
    //ap_rprintf(r, "         if(resH != \"OK\") {\n");
    //ap_rprintf(r, "           window.location = \"%s\";\n",
    //           ap_escape_html(r->pool, r->unparsed_uri));
    //ap_rprintf(r, "         }\n");
    //ap_rprintf(r, "       }\n");
    if(params && strcasestr(contentType, "application/x-www-form-urlencoded")) {
      // we are using a self-submiting form if possible
      ap_rprintf(r, "         document.forms[0].submit();\n");
    } else {
      // fallback to ajax call sending raw post
      if(contentType && clid_parp_body_data_fn) {
        apr_size_t len;
        const char *data = clid_parp_body_data_fn(r, &len);
        if(data) {
          int elen = apr_base64_encode_len(len);
          char *enc = (char *)apr_pcalloc(r->pool, 1 + elen);
          elen = apr_base64_encode(enc, (const char *)data, len);
          enc[elen] = '\0';
          ap_rprintf(r, "          function clidDec64(s) {\n");
          ap_rprintf(r, "              var e={},i,b=0,c,x,l=0,a,r='',w=String.fromCharCode,L=s.length;\n");
          ap_rprintf(r, "              var A=\"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\";\n");
          ap_rprintf(r, "              for(i=0;i<64;i++){e[A.charAt(i)]=i;}\n");
          ap_rprintf(r, "              for(x=0;x<L;x++){\n");
          ap_rprintf(r, "                  c=e[s.charAt(x)];b=(b<<6)+c;l+=6;\n");
          ap_rprintf(r, "                  while(l>=8){((a=(b>>>(l-=8))&0xff)||(x<(L-2)))&&(r+=w(a));}\n");
          ap_rprintf(r, "              }\n");
          ap_rprintf(r, "              return r;\n");
          ap_rprintf(r, "          }\n");
          ap_rprintf(r, "          var link = \"%s\";\n",
               ap_escape_html(r->pool, r->unparsed_uri));
          ap_rprintf(r, "          var req;\n");
          ap_rprintf(r, "          var rawparam = \"%s\";\n", enc);
          ap_rprintf(r, "          var param = clidDec64(rawparam);\n");
          ap_rprintf(r, "          if(window.XMLHttpRequest) {\n");
          ap_rprintf(r, "           req = new XMLHttpRequest();\n");
          ap_rprintf(r, "          } else {\n");
          ap_rprintf(r, "           req = new ActiveXObject(\"Microsoft.XMLHTTP\");\n");
          ap_rprintf(r, "          }\n");
          ap_rprintf(r, "          req.open(\"POST\", link, true);\n");
          ap_rprintf(r, "          req.setRequestHeader(\"Content-type\", \"%s\");\n",
                     ap_escape_html(r->pool, contentType));
          ap_rprintf(r, "          req.setRequestHeader(\"Content-length\", param.length);\n");
          ap_rprintf(r, "          req.setRequestHeader(\"Connection\", \"close\");\n");
          ap_rprintf(r, "          req.onreadystatechange = function() {\n");
          ap_rprintf(r, "            if(req.readyState == 4){\n");
          ap_rprintf(r, "              if(req.status == 200) {\n");
          ap_rprintf(r, "                var res = req.responseText;\n");
          ap_rprintf(r, "                var ct = req.getResponseHeader(\"Content-Type\");\n");
          ap_rprintf(r, "                if(ct == null) {\n");
          ap_rprintf(r, "                   ct = \"text/plain\";\n");
          ap_rprintf(r, "                }\n");
          ap_rprintf(r, "                var doc = document.open(ct);\n");
          ap_rprintf(r, "                doc.write(res);\n");
          ap_rprintf(r, "                doc.close();\n");
          ap_rprintf(r, "               } else {\n");
          ap_rprintf(r, "                window.location = \"%s\";\n",
                     ap_escape_html(r->pool, r->unparsed_uri));
          ap_rprintf(r, "               }\n");
          ap_rprintf(r, "            }\n");
          ap_rprintf(r, "          }\n");
          ap_rprintf(r, "          req.send(param);\n");
        }
      }
    }
    ap_rprintf(r, "     } else {\n");
    ap_rprintf(r, "       window.location = \"%s\";\n",
               ap_escape_html(r->pool, r->unparsed_uri));
    ap_rprintf(r, "     }\n");
    ap_rprintf(r, "  }\n");
    ap_rprintf(r, "}\n");
    ap_rprintf(r, "checkReq.send();\n");
    ap_rprintf(r, "\n");
    ap_rprintf(r, "</script>\n");
    ap_rprintf(r, "</head><body>Please continue here: \n");
    ap_rprintf(r, "<form method=\"POST\" action=\"%s\">\n",
               ap_escape_html(r->pool, r->unparsed_uri));
    ap_rprintf(r, "<input type=\"submit\" />\n");

    if(params) {
      int i;
      apr_table_entry_t *entry = (apr_table_entry_t *)apr_table_elts(params)->elts;
      for(i = 0; i < apr_table_elts(params)->nelts; ++i) {
        const char *key = entry[i].key;
        const char *val = entry[i].val;
        if(key) {
          char *keyu = apr_pstrdup(r->pool, key);
          char *valu = NULL;
          ap_unescape_url(keyu);
          if(val) {
            valu = apr_pstrdup(r->pool, val);
            ap_unescape_url(valu);
          }
          if(strcasecmp(key, "submit") != 0) {
            ap_rprintf(r, "<input type=\"hidden\" name=\"%s\" value=\"%s\"/>\n",
                       ap_escape_html(r->pool, key),
                       valu ? ap_escape_html(r->pool, valu) : "");
          }
        }
      }
    }
    ap_rprintf(r, "</form>\n");
    ap_rprintf(r, "</body></html>\n");
    return OK;
  }
  return DECLINED;
}

static int clid_header_parser_pds(request_rec *r) {
  if(ap_is_initial_req(r)) {
    const char *reqid = apr_table_get(r->notes, CLID_Q_PDS);
    clid_config_t *conf = ap_get_module_config(r->server->module_config, 
                                               &clientid_module);
    if(conf->pdsPath == NULL) {
      // no post data store
      return DECLINED;
    }
    if(reqid) {
      if(clid_parp_body_data_fn) {
        apr_size_t len;
        const char *data = clid_parp_body_data_fn(r, &len);
        if(data) {
          // TODO store the data
          // TODO store the data
          // TODO store the data
          // TODO store the data
          // TODO store the data
        }
      }
      return HTTP_MOVED_TEMPORARILY;
    }
  }
  return DECLINED;
}

static int clid_header_parser(request_rec *r) {
  if(ap_is_initial_req(r)) {
    clid_config_t *conf = ap_get_module_config(r->server->module_config, 
                                               &clientid_module);
    clid_dir_config_t *dconf = ap_get_module_config(r->per_dir_config,
                                                    &clientid_module);
    if(dconf->enabled == 0) {
      return DECLINED;
    }
    if(conf->keyName) {
      int rc = clid_setid(r, conf);
      if(rc != DECLINED) {
        return rc;
      }
    }
  }
  return DECLINED;
}

static int clid_post_read_request(request_rec *r) {
  if(ap_is_initial_req(r)) {
    clid_config_t *conf = ap_get_module_config(r->server->module_config, 
                                               &clientid_module);
    if(conf->keyName) {
      char *cookie = clid_get_remove_cookie(r, conf->keyName);
      apr_table_unset(r->headers_in, CLID_RND);
      apr_table_unset(r->headers_in, CLID_REC);
      if(cookie) {
        const char *verified = clid_dec64(r, cookie, conf->key);
        if(verified) {
          clid_rec_t *rec = clid_rec_d2i(r->pool, verified);
          if(rec) {
            apr_table_set(r->subprocess_env, CLID_RND, rec->rnd);
            apr_table_set(r->subprocess_env, CLID_REC, verified);
          }
        }
      }
    }
  }
  return DECLINED;
}

static int clid_verify_config(apr_pool_t *pconf, server_rec *bs) {
  server_rec *s = bs;
  while(s) {
    clid_config_t *conf = ap_get_module_config(s->module_config, 
                                               &clientid_module);
    if(conf->keyName != NULL && conf->lockFile == NULL) {
      ap_log_error(APLOG_MARK, APLOG_EMERG, 0, bs, 
                   CLID_LOG_PFX(003)"CLID_SemFile directive "
                   "has not been defined.");
      return 0;
    }
    s = s->next;
  }
  // OK
  return 1;
}

/** finalize configuration */
static void clid_config_test(apr_pool_t *pconf, server_rec *bs) {
  clid_verify_config(pconf, bs);
}

static apr_status_t clid_cleanup_index(void *p) {
  clid_user_t *u = p;
  if(u->m) {
    // write current index to file (for the next server start)
    apr_file_t *f = NULL;
    apr_pool_t *fpool;
    int rc;
    apr_pool_create(&fpool, NULL);
    rc = apr_file_open(&f, u->indexFile, APR_WRITE|APR_TRUNCATE|APR_CREATE, 
                       APR_OS_DEFAULT, fpool);
    if(rc == APR_SUCCESS) {
      char *number = apr_psprintf(fpool, "%u", *u->clientTableIndex);
      apr_file_puts(number, f);
    } else {
      ap_log_error(APLOG_MARK, APLOG_CRIT, 0, NULL, 
                   CLID_LOG_PFX(006)"Failed to store index to %s. "
                   "Could not open file.",
                   u->indexFile);
    }
    apr_pool_destroy(fpool);
  }
  return APR_SUCCESS;
}  

/**
 * init childs / mutexes
 */
static void clid_child_init(apr_pool_t *p, server_rec *bs) {
  clid_config_t *conf = ap_get_module_config(bs->module_config, 
                                             &clientid_module);
  clid_user_t *u = clid_get_user_conf(bs);
  if(conf && u->lock) {
    apr_global_mutex_child_init(&u->lock, conf->lockFile, p);
  }
  if(conf->pdsPath) {
    apr_dir_make_recursive(conf->pdsPath, APR_FPROT_UREAD|APR_FPROT_UWRITE|APR_FPROT_UEXECUTE|
                           APR_FPROT_GREAD|APR_FPROT_GWRITE|APR_FPROT_GEXECUTE|
                           APR_FINFO_WPROT, p);
  }
}

/**
 * post config:
 * - check config
 * - create process pool config
 * - init defaults
 */
static int clid_post_config(apr_pool_t *pconf, apr_pool_t *plog, 
                            apr_pool_t *ptemp, server_rec *bs) {
  server_rec *s = bs;
  clid_config_t *conf = ap_get_module_config(bs->module_config, 
                                             &clientid_module);
  ap_add_version_component(pconf, apr_psprintf(pconf, "mod_clientid/%s", 
                                               g_revision));
  clid_is_https = APR_RETRIEVE_OPTIONAL_FN(ssl_is_https);
  clid_ssl_var = APR_RETRIEVE_OPTIONAL_FN(ssl_var_lookup);
  if(!clid_is_https) {
    ap_log_error(APLOG_MARK, APLOG_CRIT, 0, bs, 
                 CLID_LOG_PFX(001)"mod_ssl not available!");
  }
  clid_config_test(pconf, bs);
  if(!clid_verify_config(pconf, bs)) {
    exit(1);
  }

  if(ap_find_linked_module("mod_parp.c") == NULL) {
    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, bs, 
                 CLID_LOG_PFX(007)"mod_parp not available");
    clid_parp_hp_table_fn = NULL;
    clid_parp_body_data_fn = NULL;
  } else {
    clid_parp_hp_table_fn = APR_RETRIEVE_OPTIONAL_FN(parp_hp_table);
    clid_parp_body_data_fn = APR_RETRIEVE_OPTIONAL_FN(parp_body_data);
  }

  if(conf->lockFile) {
    clid_user_t *u = clid_get_user_conf(bs);
    apr_uint32_t i;
    if(u->lock == NULL) {
      apr_file_t *f = NULL;
      apr_pool_t *fpool;
      apr_pool_t *cpool;
      char *semFile = apr_psprintf(u->pool, "%s.m", conf->lockFile);
      int bits = sizeof(apr_uint32_t) * 8;
      int msize = APR_ALIGN_DEFAULT(sizeof(apr_uint32_t) * CLID_MAXS / bits + sizeof(apr_uint32_t));
      int rv;
      ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, bs,
                   CLID_LOGD_PFX"Allocates shared memory, %d bytes.", msize);

      rv = apr_global_mutex_create(&u->lock, conf->lockFile,
                                   APR_LOCK_DEFAULT, u->pool);
      if(rv != APR_SUCCESS) {
        char buf[MAX_STRING_LEN];
        apr_strerror(rv, buf, sizeof(buf));
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, bs, 
                     CLID_LOG_PFX(002)"Failed to create mutex (%s): %s.",
                     conf->lockFile, buf);
        exit(1);
      }
      //rv = apr_shm_create(&u->m, msize + 1024, NULL, u->pool);
      apr_shm_remove(semFile, u->pool);
      rv = apr_shm_create(&u->m, msize + 1024, semFile, u->pool);
      if(rv != APR_SUCCESS) {
        char buf[MAX_STRING_LEN];
        apr_strerror(rv, buf, sizeof(buf));
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, bs, 
                     CLID_LOG_PFX(004)"Failed to create shared memory "
                     "(%s %d): %s.",
                     conf->lockFile, msize, buf);
        exit(1);
      }
      u->clientTableIndex = apr_shm_baseaddr_get(u->m);
      u->clientTable = u->clientTableIndex;
      u->clientTable++;
      for(i = 0; i < CLID_MAXS/bits; i++) {
        u->clientTable[i] = 0;
      }

      // read id from file
      apr_pool_create(&fpool, NULL);
      rv = apr_file_open(&f, u->indexFile, APR_READ|APR_CREATE, 
                         APR_OS_DEFAULT, fpool);
      if(rv == APR_SUCCESS) {
        int numberlen = 48;
        char *number = apr_pcalloc(fpool, numberlen);
        number[0] = '0';
        number[1] = '\0';
        apr_file_gets(number, numberlen, f);
        *u->clientTableIndex = atoi(number);
        if(*u->clientTableIndex >= CLID_MAXS) {
          *u->clientTableIndex = 0;
        }
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, bs,
                     CLID_LOGD_PFX"Index restored from %s: %u.",
                     u->indexFile, *u->clientTableIndex);
      } else {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, bs, 
                     CLID_LOG_PFX(005)"Failed to read index file %s.",
                     u->indexFile);
      }
      apr_pool_destroy(fpool);

      apr_pool_create(&cpool, u->pool);
      apr_pool_cleanup_register(cpool, u, clid_cleanup_index, apr_pool_cleanup_null);
    }
  }
  // define default client attributes to calculate the fingerprint
  while(s) {
    conf = ap_get_module_config(s->module_config, &clientid_module);
    if(conf->fingerprint == NULL) {
      conf->fingerprint = apr_table_make(pconf, 20);
      apr_table_add(conf->fingerprint, "Accept-Language", "H");
      apr_table_add(conf->fingerprint, "Accept-Encoding", "H");
      apr_table_add(conf->fingerprint, "User-Agent", "H");
      apr_table_add(conf->fingerprint, "SSL_CIPHER", "S");
      apr_table_add(conf->fingerprint, "SSL_PROTOCOL", "S");
      apr_table_add(conf->fingerprint, "SSL_CIPHER_USEKEYSIZE", "S");
      apr_table_add(conf->fingerprint, "SSL_CIPHER_ALGKEYSIZE", "S");
    }
    s = s->next;
  }
  return DECLINED;
}

static apr_status_t clid_out_filter(ap_filter_t *f, apr_bucket_brigade *bb) {
  request_rec *r = f->r;
  if(r->status == HTTP_OK && apr_table_get(r->notes, CLID_ETAG_N)) {
    // page access, no redirect => client must not cache THIS page
    apr_table_unset(r->headers_out, "ETag");
    apr_table_unset(r->headers_out, "Last-Modified");
  }
  ap_remove_output_filter(f);
  return ap_pass_brigade(f->next, bb);
}

static void clid_insert_filter(request_rec *r) {
  ap_add_output_filter("clid_out_filter", NULL, r, r->connection);
}

/************************************************************************
 * directiv handlers 
 ***********************************************************************/
static void *clid_dir_config_create(apr_pool_t *p, char *d) {
  clid_dir_config_t *dconf = apr_pcalloc(p, sizeof(clid_dir_config_t));
  dconf->enabled = -1;
  return dconf;
}

static void *clid_dir_config_merge(apr_pool_t *p, void *basev, void *addv) {
  clid_dir_config_t *b = (clid_dir_config_t *)basev;
  clid_dir_config_t *o = (clid_dir_config_t *)addv;
  clid_dir_config_t *m = apr_pcalloc(p, sizeof(clid_dir_config_t));
  if(o->enabled != -1) {
    m->enabled = o->enabled;
  } else {
    m->enabled = b->enabled;
  }
  return m;
}

static void *clid_srv_config_create(apr_pool_t *p, server_rec *s) {
  clid_config_t *sconf = apr_pcalloc(p, sizeof(clid_config_t));
  sconf->lockFile = NULL;
  sconf->pdsPath = NULL;
  sconf->require = 0;
  sconf->keyName = NULL;
  sconf->keyPath = NULL;
  sconf->check = apr_pstrdup(p, CLID_CHECK_URL);
  sconf->maxgeneration = -1;
  sconf->fingerprint = NULL;
  return sconf;
}

static void *clid_srv_config_merge(apr_pool_t *p, void *basev, void *addv) {
  clid_config_t *sconf = apr_pcalloc(p, sizeof(clid_config_t));
  clid_config_t *base = basev;
  clid_config_t *add = addv;

  sconf->lockFile = base->lockFile;
  sconf->pdsPath = base->pdsPath;

  if(add->keyName) {
    sconf->require = add->require;
    sconf->keyName = add->keyName;
    sconf->keyPath = add->keyPath;
    memcpy(sconf->key, add->key, EVP_MAX_KEY_LENGTH);
  } else {
    sconf->require = base->require;
    sconf->keyName = base->keyName;
    sconf->keyPath = base->keyPath;
    memcpy(sconf->key, base->key, EVP_MAX_KEY_LENGTH);
  }
  if(add->maxgeneration >= 0) {
    sconf->maxgeneration = add->maxgeneration;
  } else {
    sconf->maxgeneration = base->maxgeneration;
  }
  if(strcmp(add->check, CLID_CHECK_URL) != 0) {
    sconf->check = add->check;
  } else {
    sconf->check = base->check;
  }
  if(add->fingerprint) {
    sconf->fingerprint = add->fingerprint;
  } else {
    sconf->fingerprint = base->fingerprint;
  }
  return sconf;
}

const char *clid_semfile_cmd(cmd_parms *cmd, void *dcfg, const char *path) {
  clid_config_t *conf = ap_get_module_config(cmd->server->module_config, &clientid_module);
  const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
  if (err != NULL) {
    return err;
  }
  conf->lockFile = ap_server_root_relative(cmd->pool, path);
  return NULL;
}

const char *clid_pdspath_cmd(cmd_parms *cmd, void *dcfg, const char *path) {
  clid_config_t *conf = ap_get_module_config(cmd->server->module_config, &clientid_module);
  const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
  if (err != NULL) {
    return err;
  }
  conf->pdsPath = ap_server_root_relative(cmd->pool, path);
  return "post data store has not yet been implemented";
}

const char *clid_enable_cmd(cmd_parms *cmd, void *dcfg, int flag) {
  clid_dir_config_t *dconf = (clid_dir_config_t *)dcfg;
  dconf->enabled = flag;
  return NULL;
}

const char *clid_generation_cmd(cmd_parms *cmd, void *dcfg, const char *num) {
  clid_config_t *conf = ap_get_module_config(cmd->server->module_config, &clientid_module);
  conf->maxgeneration = atoi(num);
  if(conf->maxgeneration <= 0 && num[0] != '0') {
    return apr_psprintf(cmd->pool, "%s: requires numeric value",
                        cmd->directive->directive);
  }
  return NULL;
}

const char *clid_check_cmd(cmd_parms *cmd, void *dcfg, const char *path) {
  clid_config_t *conf = ap_get_module_config(cmd->server->module_config, &clientid_module);
  conf->check = apr_pstrdup(cmd->pool, path);
  return NULL;
}

const char *clid_cookie_attr_cmd(cmd_parms *cmd, void *dcfg, const char *secret,
                             const char *name, const char *path) {
  clid_config_t *conf = ap_get_module_config(cmd->server->module_config, &clientid_module);
  conf->keyName = apr_pstrdup(cmd->pool, name);
  conf->keyPath = apr_pstrdup(cmd->pool, path);
  EVP_BytesToKey(EVP_des_ede3_cbc(), EVP_sha1(), NULL, (unsigned char *)secret,
                 strlen(secret), 1, conf->key, NULL);
  return NULL;
}

const char *clid_require_cmd(cmd_parms *cmd, void *dcfg, const char *attr) {
  clid_config_t *conf = ap_get_module_config(cmd->server->module_config, &clientid_module);
  if(strcasestr(attr, "ip")) {
    conf->require |= CLID_FIX_FLAGS_IP;
  }
  if(strcasestr(attr, "ssl")) {
    conf->require |= CLID_FIX_FLAGS_SSLSID;
  }
  if(strcasestr(attr, "fingerprint")) {
    conf->require |= CLID_FIX_FLAGS_FP;
  }
  if(strcasestr(attr, "fp")) {
    conf->require |= CLID_FIX_FLAGS_FP;
  }
  return NULL;
}

const char *clid_fingerprint_cmd(cmd_parms *cmd, void *dcfg, const char *attr) {
  clid_config_t *conf = ap_get_module_config(cmd->server->module_config, &clientid_module);
  if(conf->fingerprint == NULL) {
    conf->fingerprint = apr_table_make(cmd->pool, 20);
  }
  if(strncasecmp(attr, "SSL_", 4) == 0) {
    apr_table_set(conf->fingerprint, attr, "S");
  } else {
    apr_table_set(conf->fingerprint, attr, "H");
  }
  return NULL;
}

static const command_rec clid_config_cmds[] = {
  AP_INIT_TAKE3("CLID_Cookie", clid_cookie_attr_cmd, NULL,
                RSRC_CONF,
                "CLID_Cookie <secret> <cookie name> <check path>, enables"
                " the module by defining a secret for data encryption,"
                " a name for to cookie used to store the client attributes"
                " as well as its secret, and an URL path which is used"
                " to verify the client accepts cookies."),
  AP_INIT_TAKE1("CLID_Check", clid_check_cmd, NULL,
                RSRC_CONF,
                "CLID_Check <path>, defines the path of the URL where"
                " the ETag is set/verified."),
  AP_INIT_ITERATE("CLID_Require", clid_require_cmd, NULL,
                  RSRC_CONF,
                  "CLID_Require <attribute>, specifies the client"
                  " attributes which must not change at the very same"
                  " time/request. Client sessions whose attributes change"
                  " within the same request are validated using the"
                  " ETag. Available attributes are 'ip', 'ssl', and 'fp'."),
  AP_INIT_ITERATE("CLID_Fingerprint", clid_fingerprint_cmd, NULL,
                  RSRC_CONF,
                  "CLID_Fingerprint <attribute>, specifies the attributes"
                  " used to calculate the fingerprint, e.g. Accept-Language,"
                  " Accept-Encoding, User-Agent, SSL_CIPHER, ..."),
  AP_INIT_TAKE1("CLID_SemFile", clid_semfile_cmd, NULL,
                RSRC_CONF,
                "CLID_SemFile <path>, file path within the server's file"
                " system to create the lock file for semaphore/mutex."),
  AP_INIT_TAKE1("CLID_StorePath", clid_pdspath_cmd, NULL,
                RSRC_CONF,
                "CLID_StorePath <path>, path to the directory to store"
                " request data temporary."),
  AP_INIT_TAKE1("CLID_MaxCheck", clid_generation_cmd, NULL,
                RSRC_CONF,
                "CLID_MaxCheck <number>, defines how many times"
                " the module performs an ETag check to re-validate"
                " a session. Cookies are automatically renewed without"
                " further ETag checks if the counter is reached."
                " Default is '0' (infinite)."),
  AP_INIT_FLAG("CLID_Enable", clid_enable_cmd, NULL,
               ACCESS_CONF,
               "CLID_Enable 'on'|'off', enables session enforcement."
               " Default is 'on'."),
  { NULL }
};

/************************************************************************
 * apache register 
 ***********************************************************************/
static void clid_register_hooks(apr_pool_t * p) {
  // TODO: check compatibility to mod_csrf
  static const char *pre[] = { "mod_ssl.c", NULL };
  static const char *post[] = { "mod_setenvifplus.c", "mod_parp.c", NULL };
  static const char *prepds[] = { "mod_parp.c", NULL };
  ap_hook_post_read_request(clid_post_read_request, pre, post, APR_HOOK_MIDDLE);
  ap_hook_header_parser(clid_header_parser, pre, post, APR_HOOK_FIRST);
  ap_hook_header_parser(clid_header_parser_pds, prepds, NULL, APR_HOOK_FIRST);
  ap_hook_post_config(clid_post_config, pre, NULL, APR_HOOK_MIDDLE);
  ap_hook_child_init(clid_child_init, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_test_config(clid_config_test, NULL,NULL, APR_HOOK_MIDDLE);
  ap_hook_fixups(clid_fixup, pre, NULL, APR_HOOK_MIDDLE);
  ap_register_output_filter("clid_out_filter", clid_out_filter, NULL, AP_FTYPE_RESOURCE+1);
  ap_hook_insert_filter(clid_insert_filter, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_handler(clid_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

/************************************************************************
 * apache module definition 
 ***********************************************************************/
module AP_MODULE_DECLARE_DATA clientid_module ={ 
  STANDARD20_MODULE_STUFF,
  clid_dir_config_create,                    /**< dir config */
  clid_dir_config_merge,                     /**< dir merger */
  clid_srv_config_create,                    /**< server config */
  clid_srv_config_merge,                     /**< server merger */
  clid_config_cmds,                          /**< command table */
  clid_register_hooks,                       /**< hook registery */
};
