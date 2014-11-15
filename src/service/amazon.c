/*
 * cloudfs: amazon source
 *   By Benjamin Kittridge. Copyright (C) 2013, All rights reserved.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <netdb.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/poll.h>
#include <openssl/md5.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <curl/curl.h>
#include "config.h"
#include "log.h"
#include "misc.h"
#include "service/amazon.h"

////////////////////////////////////////////////////////////////////////////////
// Class:       amazon
// Description: Amazon web services storage

////////////////////////////////////////////////////////////////////////////////
// Section:     Service table

const struct store_intr amazon_intr = {
  .load           = amazon_load,

  .list_bucket    = amazon_list_bucket,
  .create_bucket  = amazon_create_bucket,
  .exists_bucket  = amazon_exists_bucket,
  .delete_bucket  = amazon_delete_bucket,

  .list_object    = amazon_list_object,
  .put_object     = amazon_put_object,
  .get_object     = amazon_get_object,
  .exists_object  = amazon_exists_object,
  .delete_object  = amazon_delete_object,
};

////////////////////////////////////////////////////////////////////////////////
// Section:     Global variables

const char *amazon_key = NULL,
           *amazon_secret = NULL,
           *amazon_location = NULL;

static CURLSH *amazon_curl_share = NULL;

static sem_t amazon_curl_share_sem;

static sem_t *amazon_openssl_sem = NULL;

static bool amazon_use_https = true;

////////////////////////////////////////////////////////////////////////////////
// Section:     Load

void amazon_load() {
  if (!(amazon_key = config_get("amazon-key")))
    error("Must specify --amazon-key");
  if (!(amazon_secret = config_get("amazon-secret")))
    error("Must specify --amazon-secret");
  if (config_get("dont-use-https"))
    amazon_use_https = false;
  else
    amazon_load_openssl();
  amazon_location = config_get("amazon-location");
  amazon_load_curl();
}

static void amazon_curl_share_lock(CURL *handle, curl_lock_data data,
                                   curl_lock_access access, void *useptr) {
  sem_wait(&amazon_curl_share_sem);
}

static void amazon_curl_share_unlock(CURL *handle, curl_lock_data data,
                                     void *useptr) {
  sem_post(&amazon_curl_share_sem);
}

void amazon_load_curl() {
  curl_global_init(CURL_GLOBAL_ALL);

  sem_init(&amazon_curl_share_sem, 0, 1);
  amazon_curl_share = curl_share_init();
  if (curl_share_setopt(amazon_curl_share, CURLSHOPT_LOCKFUNC,
                        amazon_curl_share_lock))
    error("curl_share_setopt failed");
  if (curl_share_setopt(amazon_curl_share, CURLSHOPT_UNLOCKFUNC,
                        amazon_curl_share_unlock))
    error("curl_share_setopt failed");
  if (curl_share_setopt(amazon_curl_share, CURLSHOPT_SHARE,
                        CURL_LOCK_DATA_DNS))
    error("curl_share_setopt failed");
}

static void amazon_openssl_locking_function(int mode, int n,
                                            const char *file, int line) {
  if (mode & CRYPTO_LOCK)
    sem_wait(&amazon_openssl_sem[n]);
  else
    sem_post(&amazon_openssl_sem[n]);
}

static unsigned long amazon_openssl_id_function() {
  return pthread_self();
}

void amazon_load_openssl() {
  uint32_t i, num_locks;

  num_locks = CRYPTO_num_locks();
  if (!(amazon_openssl_sem = calloc(num_locks,
                                    sizeof(*amazon_openssl_sem))))
    stderror("calloc");
  for (i = 0; i < num_locks; i++)
    sem_init(&amazon_openssl_sem[i], 0, 1);
  CRYPTO_set_id_callback(amazon_openssl_id_function);
  CRYPTO_set_locking_callback(amazon_openssl_locking_function);
}

////////////////////////////////////////////////////////////////////////////////
// Section:     Helper functions

static char *url_encode(const char *str, uint32_t len) {
  static const char *escape = "/?=&";
  const char *ptr;
  char *nptr, *nstr;
  uint32_t rlen;

  for (rlen = 0, ptr = str; ptr - str < len; ptr++) {
    if (!isprint(*ptr) || strchr(escape, *ptr))
      rlen += 4;
    else
      rlen++;
  }

  if (!(nstr = malloc(rlen + 1)))
    stderror("malloc");
  for (ptr = str, nptr = nstr; ptr - str < len; ptr++) {
    if (!isprint(*ptr) || strchr(escape, *ptr)) {
      sprintf(nptr, "%%%02X", (uint8_t)*ptr);
      nptr += 4;
    } else {
      *nptr++ = *ptr;
    }
  }
  *nptr = 0;
  return nstr;
}

static char *xml_decode(const char *str, uint32_t len) {
  static struct {
    char *ent, ctr;
  } set[] = {
    {   "&lt;",  '<' },
    {   "&gt;",  '>' },
    {  "&amp;",  '&' },
    { "&apos;", '\'' },
    { "&quot;",  '"' },
  };
  uint32_t i, rlen, num;
  bool f;
  const char *ptr;
  char *nptr, *nstr;

  for (rlen = 0, ptr = str; ptr - str < len; ptr++) {
    if (*ptr == '&') {
      rlen += 2;
      for (; *ptr && *ptr != ';'; ptr++)
        continue;
    } else {
      rlen++;
    }
  }

  if (!(nstr = malloc(rlen + 1)))
    stderror("malloc");
  for (ptr = str, nptr = nstr; ptr - str < len; ptr++) {
    if (*ptr == '&') {
      for (f = false, i = 0; i < sizearr(set); i++) {
        if (!strncmp(ptr, set[i].ent, strlen(set[i].ent))) {
          *nptr++ = set[i].ctr;
          f = true;
          break;
        }
      }
      if (!f && ptr[1] == '#') {
        if (ptr[2] == 'x')
          num = strtoul(ptr + 3, NULL, 16);
        else
          num = strtoul(ptr + 2, NULL, 16);
        if (num > 0xff) {
          *nptr++ = num & 0xff;
          *nptr++ = (num >> 8) & 0xff;
        } else {
          *nptr++ = num;
        }
      }
      for (; *ptr && *ptr != ';'; ptr++)
        continue;
    } else {
      *nptr++ = *ptr;
    }
  }
  *nptr = 0;
  return nstr;
}

static void xml_push_tags(struct store_list *list, const char *tag,
                          char *sbuf) {
  char *ptr, *name, *xml_name;
  uint32_t tag_len;

  tag_len = strlen(tag);
  while ((sbuf = strstr(sbuf, tag))) {
    for (name = sbuf + tag_len, ptr = name;
         *ptr && *ptr != '<'; ptr++)
      continue;
    *ptr++ = 0;

    xml_name = xml_decode(name, strlen(name));
    store_list_push(list, xml_name);
    free(xml_name);

    sbuf = ptr;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Section:     Buckets

int amazon_list_bucket(const char *prefix, uint32_t max_count,
                       struct store_list *list) {
  char *buf;
  int ret;

  ret = amazon_request_call(AMAZON_REQUEST_GET,
                            "", "/",
                            NULL, 0,
                            &buf, NULL);

  if (ret == SUCCESS)
    xml_push_tags(list, "<Name>", buf);

  free(buf);
  return ret;
}

int amazon_create_bucket(const char *bucket) {
  char *xml;
  int ret;

  asprintf(&xml,
           "<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/"
               "doc/2006-03-01/\">\n"
           "  <LocationConstraint>%s</LocationConstraint>\n"
           "</CreateBucketConfiguration>\n",
           amazon_location ?: "");

  ret = amazon_request_call(AMAZON_REQUEST_PUT,
                            bucket, "/",
                            xml, strlen(xml),
                            NULL, NULL);

  free(xml);
  return ret;
}

int amazon_exists_bucket(const char *bucket) {
  return amazon_request_call(AMAZON_REQUEST_HEAD,
                             bucket, "/",
                             NULL, 0,
                             NULL, NULL);
}

int amazon_delete_bucket(const char *bucket) {
  return amazon_request_call(AMAZON_REQUEST_DELETE,
                             bucket, "/",
                             NULL, 0,
                             NULL, NULL);
}

////////////////////////////////////////////////////////////////////////////////
// Section:     Objects

int amazon_list_object(const char *bucket, const char *prefix,
                       uint32_t max_count, struct store_list *list) {
  char *buf, *url, *esc_prefix;
  int ret;

  esc_prefix = url_encode(prefix, strlen(prefix));
  if (asprintf(&url, "/?prefix=&marker=%s&max-keys=%u",
               esc_prefix, max_count) < 0)
    stderror("asprintf");

  ret = amazon_request_call(AMAZON_REQUEST_GET,
                            bucket, url,
                            NULL, 0,
                            &buf, NULL);

  if (ret == SUCCESS)
    xml_push_tags(list, "<Key>", buf);

  free(esc_prefix);
  free(url);
  free(buf);
  return ret;
}

int amazon_put_object(const char *bucket, const char *object,
                      const char *buf, uint32_t len) {
  return amazon_request_call(AMAZON_REQUEST_PUT,
                             bucket, object,
                             buf, len,
                             NULL, NULL);
}

int amazon_get_object(const char *bucket, const char *object,
                      char **buf, uint32_t *len) {
  return amazon_request_call(AMAZON_REQUEST_GET,
                             bucket, object,
                             NULL, 0,
                             buf, len);
}

int amazon_exists_object(const char *bucket, const char *object) {
  return amazon_request_call(AMAZON_REQUEST_HEAD,
                             bucket, object,
                             NULL, 0,
                             NULL, NULL);
}

int amazon_delete_object(const char *bucket, const char *object) {
  return amazon_request_call(AMAZON_REQUEST_DELETE,
                             bucket, object,
                             NULL, 0,
                             NULL, NULL);
}

////////////////////////////////////////////////////////////////////////////////
// Section:     Amazon request

int amazon_request_call(enum amazon_request_method method,
                        const char *bucket, const char *object,
                        const char *data, uint32_t data_len,
                        char **out_buf, uint32_t *out_len) {
  struct amazon_request *c;
  int ret, retry;

  ret = SYS_ERROR;

  for (retry = 0; retry < AMAZON_REQUEST_RETRY; retry++) {
    c = amazon_request_new(method, bucket, object);
    amazon_request_set_req(c, data, data_len);
    amazon_request_perform(c);
    if (!c->resp_code || c->resp_code == 500) {
      amazon_request_free(c);
      if (retry >= 2)
        warning("Failure while contacting Amazon S3, retrying...");
      sleep(retry * 5);
      continue;
    }
    switch (c->resp_code) {
      case 200:
      case 204:
        ret = SUCCESS;
        if (out_buf) {
          *out_buf = c->resp_data;
          c->resp_data = NULL;
        }
        if (out_len)
          *out_len = c->resp_len;
        break;

      case 404:
        ret = NOT_FOUND;
        break;
    }
    amazon_request_free(c);
    break;
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// Section:     Request initialization

struct amazon_request *amazon_request_new(enum amazon_request_method method,
                                          const char *bucket,
                                          const char *object) {
  struct amazon_request *c;

  if (!(c = calloc(sizeof(*c), 1)))
    stderror("calloc");

  c->method = method;
  if (asprintf(&c->location, "s3%s%s.amazonaws.com",
      amazon_location ? "-" : "", amazon_location ?: "") < 0)
    stderror("asprintf");
  if (asprintf(&c->object, "%s%s",
      object && *object != '/' ? "/" : "",
      object) < 0)
    stderror("asprintf");
  c->bucket = bucket;
  return c;
}

void amazon_request_set_req(struct amazon_request *c, const char *data,
                            uint32_t data_len) {
  c->req_data = data;
  c->req_ptr = data;
  c->req_len = data_len;
  c->req_left = data_len;
}

////////////////////////////////////////////////////////////////////////////////
// Section:     Request helper functions

static const char *method(struct amazon_request *c) {
  switch (c->method) {
    case AMAZON_REQUEST_GET:    return "GET";
    case AMAZON_REQUEST_PUT:    return "PUT";
    case AMAZON_REQUEST_DELETE: return "DELETE";
    case AMAZON_REQUEST_HEAD:   return "HEAD";
  }
  error("Invalid method passed");
}

static void astrcat(char **str, const char *format, ...) {
  va_list args;
  char *buf, *ptr;
  uint32_t len;

  va_start(args, format);
  if (vasprintf(&buf, format, args) < 0)
    stderror("vasprintf");
  va_end(args);

  len = (*str ? strlen(*str) : 0);
  if (!(ptr = realloc(*str, len + strlen(buf) + 1)))
    stderror("realloc");
  ptr[len] = 0;
  strcat(ptr, buf);
  free(buf);

  *str = ptr;
}

static void curdate(char *str, int32_t size) {
  static const char *lookup_week[7]   = { "Sun", "Mon", "Tue", "Wed",
                                          "Thu", "Fri", "Sat" },
                    *lookup_month[12] = { "Jan", "Feb", "Mar", "Apr",
                                          "May", "Jun", "Jul", "Aug",
                                          "Sep", "Oct", "Nov", "Dec" };
  struct tm tm;
  time_t ct;

  ct = time(NULL);
  gmtime_r(&ct, &tm);
  snprintf(str, size, "%s, %02d %s %04d %02d:%02d:%02d +%.4d",
           lookup_week[tm.tm_wday], tm.tm_mday,
           lookup_month[tm.tm_mon], tm.tm_year + 1900,
           tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tm.tm_gmtoff / 36));
}

static void base64_encode(const char *in_str, uint32_t in_len, char **out_str) {
  static const char *base64_lookup =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789+/";
  const char *ptr;
  char *out;
  uint8_t *in;
  int32_t len;
  uint32_t c;

  len = (((in_len / 3) + 1) * 4) + 1;
  if (!(out = malloc(len)))
    stderror("malloc");

  for (c = 0, ptr = in_str; ptr < in_str + in_len; ptr += 3) {
    in = (uint8_t*)ptr;
    len = (in_str + in_len) - ptr;

    out[c++] = base64_lookup[in[0] >> 2];
    out[c++] = base64_lookup[((in[0] & 0x03) << 4) |
                             (len > 1 ? ((in[1] & 0xf0) >> 4) : 0)];
    out[c++] = len > 1 ? base64_lookup[((in[1] & 0x0f) << 2) |
                               (len > 2 ? ((in[2] & 0xc0) >> 6) : 0)] : '=';
    out[c++] = len > 2 ? base64_lookup[in[2] & 0x3f] : '=';
  }
  out[c] = 0;

  *out_str = out;
}

////////////////////////////////////////////////////////////////////////////////
// Section:     Generate and write request

void amazon_request_perform(struct amazon_request *c) {
  CURL *curl;
  CURLcode ret;
  struct curl_slist *header;
  char dig[MD5_DIGEST_LENGTH], date[1 << 9],
    *md5, *host, *auth;

  if (!(curl = curl_easy_init()))
    error("Unable to init curl");

  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
  curl_easy_setopt(curl, CURLOPT_SHARE, amazon_curl_share);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method(c));
  if (c->method == AMAZON_REQUEST_HEAD)
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

  if (asprintf(&host, "%s://%s%s%s%s",
               amazon_use_https ? "https" : "http",
               c->bucket ? c->bucket : "",
               c->bucket && *c->bucket ? "." : "",
               c->location,
               c->object) < 0)
    stderror("asprintf");

  curl_easy_setopt(curl, CURLOPT_URL, host);
  free(host);

  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) c->req_len);

  MD5((uint8_t*) c->req_data, c->req_len, (uint8_t*) dig);
  base64_encode(dig, sizeof(dig), &md5);
  curdate(date, sizeof(date));

  auth = amazon_request_access(c, date, md5);

#define set_header(name, value) ({ \
      char *hdr; \
      if (asprintf(&hdr, "%s: %s", name, value) < 0) \
        stderror("asprintf"); \
      header = curl_slist_append(header, hdr); \
      free(hdr); })

  header = NULL;

  set_header("Date", date);
  set_header("Content-MD5", md5);
  set_header("Authorization", auth);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header);

  free(md5);
  free(auth);

  if (c->req_len) {
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, amazon_request_read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, c);
  }

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, amazon_request_write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, c);

  if ((ret = curl_easy_perform(curl)) != CURLE_OK)
    warning("Curl failed: %s", curl_easy_strerror(ret));
  else
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &c->resp_code);

  curl_slist_free_all(header);
  curl_easy_cleanup(curl);
}

char *amazon_request_access(struct amazon_request *c, const char *date,
                            const char *md5) {
  char *data, secret[SHA_CBLOCK], mac[SHA_DIGEST_LENGTH],
       *bmac, *goodurl, *ptr, *auth;
  uint32_t mdlen;

  data = NULL;
  astrcat(&data, "%s\n", method(c));
  astrcat(&data, "%s\n\n", md5);
  astrcat(&data, "%s\n", date);

  if (!(goodurl = strdup(c->object)))
    stderror("strdup");
  for (ptr = goodurl; *ptr && *ptr != '?'; ptr++)
    continue;
  if (*ptr)
    *ptr++ = 0;

  astrcat(&data, "%s%s%s",
      c->bucket && *c->bucket ? "/" : "",
      c->bucket ? c->bucket : "",
      goodurl);
  free(goodurl);

  memset(secret, 0, sizeof(secret));
  strncpy(secret, amazon_secret, sizeof(secret));

  mdlen = sizeof(mac);
  HMAC(EVP_sha1(),
       (uint8_t*) secret, sizeof(secret),
       (uint8_t*) data, strlen(data),
       (uint8_t*) mac, &mdlen);

  base64_encode(mac, mdlen, &bmac);
  free(data);

  if (asprintf(&auth, "AWS %s:%s", amazon_key, bmac) < 0)
    stderror("asprintf");
  free(bmac);
  return auth;
}

////////////////////////////////////////////////////////////////////////////////
// Section:     Curl callbacks

size_t amazon_request_read_callback(void *ptr, size_t size, size_t nmemb,
                                    void *stream) {
  struct amazon_request *c;
  size_t len;

  c = (struct amazon_request*) stream;

  size *= nmemb;
  len = min(size, c->req_left);

  memcpy(ptr, c->req_ptr, len);
  c->req_ptr  += len;
  c->req_left -= len;
  return len;
}

size_t amazon_request_write_callback(void *ptr, size_t size, size_t nmemb,
                                     void *stream) {
  struct amazon_request *c;

  c = (struct amazon_request*) stream;

  size *= nmemb;

  if (!(c->resp_data = realloc(c->resp_data, c->resp_len + size)))
    stderror("realloc");
  memcpy(c->resp_data + c->resp_len, ptr, size);
  c->resp_len += size;
  return size;
}

////////////////////////////////////////////////////////////////////////////////
// Section:     Free request

void amazon_request_free(struct amazon_request *c) {
  if (c->location)
    free(c->location);
  if (c->object)
    free(c->object);
  if (c->resp_data)
    free(c->resp_data);
  free(c);
}
