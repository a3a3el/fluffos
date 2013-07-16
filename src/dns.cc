#include "std.h"

#include "dns.h"
#include "event.h"

#include <event2/event.h>
#include <event2/dns.h>

#include "comm.h"
#include "lpc_incl.h"

static struct evdns_base *g_dns_base = NULL;

void init_dns_event_base(struct event_base *base)
{
  // Configure a DNS resolver with default nameserver
  g_dns_base = evdns_base_new(base, 1);
}

static void add_ip_entry(struct sockaddr *, socklen_t size, char *);

typedef struct addr_name_query_s {
  sockaddr_storage addr;
  socklen_t addrlen;
  struct evdns_request *req;
} addr_name_query_t;

// Reverse DNS lookup.
void on_addr_name_result(int err, char type, int count,
                         int ttl, void *addresses, void *arg)
{

  auto query = (addr_name_query_t *)arg;

  if (err) {
    debug(dns, "DNS reverse lookup fail: %s.\n", evdns_err_to_string(err));
  } else if (count == 0) {
    debug(dns, "DNS reverse lookup returns no result.\n");
  } else {
    auto result = *((char **)addresses);
    debug(dns, "DNS reverse lookup result: %d: %s\n", type, result);
    add_ip_entry((sockaddr *)&query->addr, query->addrlen, result);
  }
  delete query;
}

// Start a reverse lookup.
void query_name_by_addr(object_t *ob)
{
  auto query = new addr_name_query_t;

  const char *addr = query_ip_number(ob);
  debug(dns, "query_name_by_addr: starting lookup for %s.\n", addr);
  free_string(addr);

  // By the time resolve finish, ob may be already gone, we have to
  // copy the address.
  memcpy(&query->addr, &ob->interactive->addr, ob->interactive->addrlen);
  query->addrlen = ob->interactive->addrlen;

  // Check for mapped v4 address, if we are querying for v6 address.
  if (query->addr.ss_family == AF_INET6) {
    in6_addr *addr6 = &(((sockaddr_in6 *)(&query->addr))->sin6_addr);
    if (IN6_IS_ADDR_V4MAPPED(addr6) || IN6_IS_ADDR_V4COMPAT(addr6)) {
      in_addr *addr4 = &((in_addr *)(addr6))[3];
      debug(dns, "Found mapped v4 address, using extracted v4 address to resolve.\n")
      query->req = evdns_base_resolve_reverse(
                     g_dns_base, addr4, 0,
                     on_addr_name_result, query);
    } else {
      query->req = evdns_base_resolve_reverse_ipv6(
                     g_dns_base, addr6, 0,
                     on_addr_name_result, query);
    }
  } else {
    in_addr *addr4 = &((sockaddr_in *)&query->addr)->sin_addr;
    query->req = evdns_base_resolve_reverse(
                   g_dns_base, addr4, 0, on_addr_name_result, query);
  }
}

struct addr_number_query {
  LPC_INT key;
  char *name;
  svalue_t call_back;
  object_t *ob_to_call;
  evdns_getaddrinfo_request *req;
  int err;
  evutil_addrinfo *res;
};

// query finished, call the LPC callback.
void on_query_addr_by_name_finish(evutil_socket_t fd, short what, void *arg)
{
  auto query = (addr_number_query *)arg;

  if (query->err) {
    debug(dns, "DNS lookup fail: %" LPC_INT_FMTSTR_P ",request: %s, err: %s.\n",
          query->key, query->name, evutil_gai_strerror(query->err));
    push_undefined();
    push_undefined();
  } else {
    // push the name
    copy_and_push_string(query->name);

    // push IP address
    char host[NI_MAXHOST];
    int ret = getnameinfo(query->res->ai_addr, query->res->ai_addrlen, host,
                          sizeof(host), NULL, 0, NI_NUMERICHOST);
    if (!ret) {
      copy_and_push_string(host);
    } else {
      debug(dns, "on_query_addr_by_name_finish: getnameinfo: %s \n", gai_strerror(ret));
      push_undefined();
    }
    debug(dns, "DNS lookup success: id %" LPC_INT_FMTSTR_P ": %s -> %s \n", query->key, query->name, host);
  }


  // push the key
  push_number(query->key);

  if (query->call_back.type == T_STRING) {
    safe_apply(query->call_back.u.string, query->ob_to_call,
               3, ORIGIN_INTERNAL);
  } else {
    safe_call_function_pointer(query->call_back.u.fp, 3);
  }

  if (query->res != NULL)
    evutil_freeaddrinfo(query->res);

  free_string(query->name);
  free_svalue(&query->call_back, "on_addr_result");
  free_object(&query->ob_to_call, "on_addr_result: ");
  delete query;
}

// intermediate result from evdns_getaddrinfo
void on_getaddr_result(int err, evutil_addrinfo *res, void *arg)
{
  auto query = (addr_number_query *)arg;
  query->err = err;
  query->res = res;

  // Schedule an immediate event to call LPC callback.
  event_base_once(g_event_base, -1, EV_TIMEOUT,
                  on_query_addr_by_name_finish, query, NULL);
}

/*
 * Try to resolve "name" and call the callback when finish.
 */
LPC_INT query_addr_by_name(const char *name, svalue_t *call_back)
{
  static LPC_INT key = 0;

  struct evutil_addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = 0; // EVUTIL_AI_V4MAPPED | EVUTIL_AI_ADDRCONFIG;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = 0;

  auto query = new addr_number_query;
  memset(query, 0, sizeof(addr_number_query));

  query->key = key++;
  query->name = make_shared_string(name);
  query->ob_to_call = current_object;
  assign_svalue_no_free(&query->call_back, call_back);

  add_ref(current_object, "query_addr_number: ");

  query->req = evdns_getaddrinfo(
                 g_dns_base, name, NULL, &hints, on_getaddr_result, query);

  debug(dns, "DNS lookup scheduled: %" LPC_INT_FMTSTR_P ", %s\n", query->key, name);

  return query->key;
}                               /* query_addr_number() */

#define IPSIZE 200
typedef struct {
  struct sockaddr_storage addr;
  socklen_t addrlen;
  char *name;
} ipentry_t;

static ipentry_t iptable[IPSIZE];
static int ipcur;

#ifdef DEBUGMALLOC_EXTENSIONS
void mark_iptable()
{
  int i;

  for (i = 0; i < IPSIZE; i++)
    if (iptable[i].name) {
      EXTRA_REF(BLOCK(iptable[i].name))++;
    }
}
#endif

const char *query_ip_name(object_t *ob)
{
  int i;

  if (ob == 0) {
    ob = command_giver;
  }
  if (!ob || ob->interactive == 0) {
    return NULL;
  }
  for (i = 0; i < IPSIZE; i++) {
    if (iptable[i].addrlen == ob->interactive->addrlen &&
        !memcmp(&iptable[i].addr, &ob->interactive->addr,
                ob->interactive->addrlen) &&
        iptable[i].name) {
      return (iptable[i].name);
    }
  }
  return query_ip_number(ob);
}

static void add_ip_entry(struct sockaddr *addr, socklen_t size, char *name)
{
  int i;

  for (i = 0; i < IPSIZE; i++) {
    if (iptable[i].addrlen == size &&
        !memcmp(&iptable[i].addr, addr, size)) {
      return;
    }
  }
  memcpy(&iptable[ipcur].addr, addr, size);
  iptable[ipcur].addrlen = size;

  if (iptable[ipcur].name) {
    free_string(iptable[ipcur].name);
  }
  iptable[ipcur].name = make_shared_string(name);
  ipcur = (ipcur + 1) % IPSIZE;
}

const char *query_ip_number(object_t *ob)
{
  if (ob == 0) {
    ob = command_giver;
  }
  if (!ob || ob->interactive == 0) {
    return 0;
  }
  char host[NI_MAXHOST];
  getnameinfo((sockaddr *)&ob->interactive->addr, sizeof(ob->interactive->addr),
              host, sizeof(host), NULL, 0, NI_NUMERICHOST);
  return make_shared_string(host);
}