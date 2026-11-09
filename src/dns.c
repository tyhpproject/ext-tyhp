#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"

#include "php_tyhp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <process.h>
# include <windns.h>
# include "php_network.h"
#else
# include <arpa/inet.h>
# include <fcntl.h>
# include <netdb.h>
# include <pthread.h>
# include <signal.h>
# include <sys/socket.h>
# include <unistd.h>
# ifdef __APPLE__
#  define BIND_8_COMPAT 1
# endif
# ifdef HAVE_ARPA_NAMESER_H
#  include <arpa/nameser.h>
# endif
# ifdef HAVE_RESOLV_H
#  include <resolv.h>
# endif
# ifdef HAVE_DNS_H
#  include <dns.h>
# endif
#endif

/*
 * DNS lookups run on a side thread so the request thread can keep polling.
 * That thread allocates with malloc and free only. It does not call emalloc,
 * touch a zval, or enter the Zend executor or TSRM. It writes one byte to
 * the raw pipe or socket when the result is published. The request thread
 * owns the PHP stream on the read end.
 *
 * A lookup that outlives the request is marked cancelled in RSHUTDOWN and
 * left running. Joining there can block on the resolver, so the record stays
 * in process memory until the thread has published its result. dnsStart then
 * reaps it (the join cannot block any more). MSHUTDOWN joins what is left.
 */

#ifndef TYHP_T_AAAA
# ifdef T_AAAA
#  define TYHP_T_AAAA T_AAAA
# else
#  define TYHP_T_AAAA 28
# endif
#endif

#ifndef TYHP_C_IN
# ifdef C_IN
#  define TYHP_C_IN C_IN
# else
#  define TYHP_C_IN 1
# endif
#endif

#define TYHP_DNS_PACKET 65536
#define TYHP_DNS_MAXFQDN 255

#ifdef _WIN32
typedef LONG tyhp_dns_atomic;
#else
typedef int tyhp_dns_atomic;
#endif

typedef struct tyhp_dns_job {
	zend_long id;
	char *host;
	size_t host_len;
	char **addresses;
	size_t count;
	int unresolved;
	int cancelled;
	/* Set in RSHUTDOWN. The request that started the lookup is gone, so no
	 * later request can take or cancel it. */
	int abandoned;
	int started;
	int joined;
	tyhp_dns_atomic ready;
#ifdef _WIN32
	DWORD owner;
	HANDLE thread;
	SOCKET write_sock;
#else
	/* A forked child inherits the job list but not the threads. */
	pid_t pid;
	pthread_t owner;
	pthread_t thread;
	int write_fd;
#endif
	struct tyhp_dns_job *next;
} tyhp_dns_job;

typedef struct tyhp_dns_addrs {
	char **items;
	size_t count;
	size_t cap;
} tyhp_dns_addrs;

#ifdef _WIN32
static CRITICAL_SECTION tyhp_dns_mu;
static CRITICAL_SECTION tyhp_dns_hostent_mu;
static int tyhp_dns_locks_ready = 0;
#else
static pthread_mutex_t tyhp_dns_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tyhp_dns_hostent_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

static tyhp_dns_job *tyhp_dns_jobs = NULL;
static zend_long tyhp_dns_next_id = 1;

static void tyhp_dns_lock(void)
{
#ifdef _WIN32
	EnterCriticalSection(&tyhp_dns_mu);
#else
	pthread_mutex_lock(&tyhp_dns_mu);
#endif
}

static void tyhp_dns_unlock(void)
{
#ifdef _WIN32
	LeaveCriticalSection(&tyhp_dns_mu);
#else
	pthread_mutex_unlock(&tyhp_dns_mu);
#endif
}

static void tyhp_dns_hostent_lock(void)
{
#ifdef _WIN32
	EnterCriticalSection(&tyhp_dns_hostent_mu);
#else
	pthread_mutex_lock(&tyhp_dns_hostent_mu);
#endif
}

static void tyhp_dns_hostent_unlock(void)
{
#ifdef _WIN32
	LeaveCriticalSection(&tyhp_dns_hostent_mu);
#else
	pthread_mutex_unlock(&tyhp_dns_hostent_mu);
#endif
}

static void tyhp_dns_store_ready(tyhp_dns_job *job)
{
#ifdef _WIN32
	InterlockedExchange(&job->ready, 1);
#else
	__atomic_store_n(&job->ready, 1, __ATOMIC_RELEASE);
#endif
}

static int tyhp_dns_load_ready(tyhp_dns_job *job)
{
#ifdef _WIN32
	return InterlockedCompareExchange(&job->ready, 0, 0) != 0;
#else
	return __atomic_load_n(&job->ready, __ATOMIC_ACQUIRE) != 0;
#endif
}

static zend_class_entry *tyhp_dns_ce(const char *name, size_t len)
{
	zend_class_entry *ce = zend_hash_str_find_ptr(EG(class_table), name, len);
	if (ce == NULL) {
		ce = zend_ce_exception;
	}
	return ce;
}

static void tyhp_dns_throw(const char *name, size_t len, const char *message)
{
	zend_throw_exception(tyhp_dns_ce(name, len), message, 0);
}

static int tyhp_dns_owned(const tyhp_dns_job *job)
{
#ifdef _WIN32
	return job->owner == GetCurrentThreadId();
#else
	return pthread_equal(job->owner, pthread_self()) != 0;
#endif
}

static int tyhp_dns_unlink(tyhp_dns_job *job)
{
	tyhp_dns_job **link = &tyhp_dns_jobs;

	while (*link != NULL) {
		if (*link == job) {
			*link = job->next;
			job->next = NULL;
			return 1;
		}
		link = &(*link)->next;
	}
	return 0;
}

static tyhp_dns_job *tyhp_dns_find(zend_long id)
{
	tyhp_dns_job *job;

	for (job = tyhp_dns_jobs; job != NULL; job = job->next) {
		if (job->id == id) {
			return job;
		}
	}
	return NULL;
}

/* The lookup a request may take or cancel. Abandoned lookups belong to a
 * request that already ended. */
static tyhp_dns_job *tyhp_dns_find_live(zend_long id)
{
	tyhp_dns_job *job = tyhp_dns_find(id);

	if (job != NULL && job->abandoned) {
		return NULL;
	}
	return job;
}

static zend_long tyhp_dns_alloc_id(void)
{
	zend_long id;
	int guard = 0;

	do {
		id = tyhp_dns_next_id;
		if (id <= 0) {
			id = 1;
		}
		if (id == ZEND_LONG_MAX) {
			tyhp_dns_next_id = 1;
		} else {
			tyhp_dns_next_id = id + 1;
		}
		guard++;
	} while (tyhp_dns_find(id) != NULL && guard < 1000);

	return id;
}

static void tyhp_dns_join(tyhp_dns_job *job)
{
	if (!job->started) {
		return;
	}
#ifdef _WIN32
	if (job->thread != NULL) {
		WaitForSingleObject(job->thread, INFINITE);
		CloseHandle(job->thread);
		job->thread = NULL;
	}
#else
	/* After fork() the child has this record but not its thread. */
	if (job->pid == getpid()) {
		pthread_join(job->thread, NULL);
	}
#endif
}

static void tyhp_dns_job_free(tyhp_dns_job *job)
{
	size_t i;

	if (job->host != NULL) {
		free(job->host);
	}
	if (job->addresses != NULL) {
		for (i = 0; i < job->count; i++) {
			free(job->addresses[i]);
		}
		free(job->addresses);
	}
#ifdef _WIN32
	if (job->write_sock != INVALID_SOCKET) {
		closesocket(job->write_sock);
	}
#else
	if (job->write_fd >= 0) {
		close(job->write_fd);
	}
#endif
	free(job);
}

static int tyhp_dns_addrs_add(tyhp_dns_addrs *list, const char *text)
{
	size_t i;
	size_t len;
	size_t cap;
	char *copy;
	char **items;

	for (i = 0; i < list->count; i++) {
		if (strcmp(list->items[i], text) == 0) {
			return 0;
		}
	}

	len = strlen(text);
	copy = malloc(len + 1);
	if (copy == NULL) {
		return -1;
	}
	memcpy(copy, text, len + 1);

	if (list->count == list->cap) {
		cap = list->cap == 0 ? 4 : list->cap * 2;
		items = malloc(sizeof(char *) * cap);
		if (items == NULL) {
			free(copy);
			return -1;
		}
		if (list->count > 0) {
			memcpy(items, list->items, sizeof(char *) * list->count);
		}
		free(list->items);
		list->items = items;
		list->cap = cap;
	}

	list->items[list->count++] = copy;
	return 0;
}

static void tyhp_dns_addrs_clear(tyhp_dns_addrs *list)
{
	size_t i;

	for (i = 0; i < list->count; i++) {
		free(list->items[i]);
	}
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->cap = 0;
}

/* filter_var($host, FILTER_VALIDATE_IP) with no flags. A match is the
 * original spelling, which is what DnsLookup::run returns. */
static int tyhp_dns_valid_ipv4(const char *str, size_t str_len, int *ip)
{
	const char *end = str + str_len;
	int num;
	int m;
	int n = 0;

	while (str < end) {
		int leading_zero;

		if (*str < '0' || *str > '9') {
			return 0;
		}
		leading_zero = (*str == '0');
		m = 1;
		num = (*(str++) - '0');
		while (str < end && *str >= '0' && *str <= '9') {
			num = num * 10 + (*(str++) - '0');
			if (num > 255 || ++m > 3) {
				return 0;
			}
		}
		if (leading_zero && (num != 0 || m > 1)) {
			return 0;
		}
		ip[n++] = num;
		if (n == 4) {
			return str == end;
		}
		if (str >= end || *(str++) != '.') {
			return 0;
		}
	}
	return 0;
}

static int tyhp_dns_valid_ipv6(const char *str, size_t str_len, int *ip)
{
	int compressed_pos = -1;
	int blocks = 0;
	unsigned int num;
	unsigned int n;
	int i;
	const char *ipv4 = NULL;
	const char *end;
	int ip4elm[4];
	const char *start = str;

	if (memchr(str, ':', str_len) == NULL) {
		return 0;
	}

	ipv4 = memchr(str, '.', str_len);
	if (ipv4 != NULL) {
		while (ipv4 > str && *(ipv4 - 1) != ':') {
			ipv4--;
		}
		if (!tyhp_dns_valid_ipv4(ipv4, (size_t)(str_len - (size_t)(ipv4 - str)), ip4elm)) {
			return 0;
		}
		str_len = (size_t)(ipv4 - str);
		if (str_len < 2) {
			return 0;
		}
		if (ipv4[-2] != ':') {
			str_len--;
		}
		blocks = 2;
	}

	end = str + str_len;
	while (str < end) {
		if (*str == ':') {
			if (++str >= end) {
				return 0;
			}
			if (*str == ':') {
				if (compressed_pos >= 0) {
					return 0;
				}
				if (blocks < 8) {
					ip[blocks] = -1;
				}
				compressed_pos = blocks++;
				if (++str == end) {
					if (blocks > 8) {
						return 0;
					}
					goto fixup_ip;
				}
			} else if ((str - 1) == start) {
				return 0;
			}
		}

		num = 0;
		n = 0;
		while (str < end) {
			if (*str >= '0' && *str <= '9') {
				num = 16 * num + (unsigned int)(*str - '0');
			} else if (*str >= 'a' && *str <= 'f') {
				num = 16 * num + (unsigned int)(*str - 'a') + 10;
			} else if (*str >= 'A' && *str <= 'F') {
				num = 16 * num + (unsigned int)(*str - 'A') + 10;
			} else {
				break;
			}
			n++;
			str++;
		}
		if (blocks < 8) {
			ip[blocks] = (int)num;
		}
		if (n < 1 || n > 4) {
			return 0;
		}
		if (++blocks > 8) {
			return 0;
		}
	}

fixup_ip:
	if (ipv4 != NULL) {
		for (i = 0; i < 5; i++) {
			ip[i] = 0;
		}
		ip[i++] = 0xffff;
		ip[i++] = 256 * ip4elm[0] + ip4elm[1];
		ip[i++] = 256 * ip4elm[2] + ip4elm[3];
	} else if (compressed_pos >= 0 && blocks <= 8) {
		int offset = 8 - blocks;
		for (i = 7; i > compressed_pos + offset; i--) {
			ip[i] = ip[i - offset];
		}
		for (i = compressed_pos + offset; i >= compressed_pos; i--) {
			ip[i] = 0;
		}
	}

	return (compressed_pos >= 0 && blocks <= 8) || blocks == 8;
}

static int tyhp_dns_is_ip(const char *str, size_t len)
{
	int ip[8];

	memset(ip, 0, sizeof(ip));
	if (memchr(str, ':', len) != NULL) {
		return tyhp_dns_valid_ipv6(str, len, ip) != 0;
	}
	if (memchr(str, '.', len) != NULL) {
		return tyhp_dns_valid_ipv4(str, len, ip) != 0;
	}
	return 0;
}

/* Same text dns_get_record() builds for an AAAA rdata: the first run of
 * zero groups is compressed, and a later zero group is written as 0. */
static void tyhp_dns_format_groups(const unsigned int groups[8], char *dst, size_t dst_size)
{
	char *tp = dst;
	char *end = dst + dst_size;
	int have_break = 0;
	int in_break = 0;
	int i;

	if (dst_size == 0) {
		return;
	}
	dst[0] = '\0';
	for (i = 0; i < 8; i++) {
		unsigned int group = groups[i];

		if (group != 0) {
			int wrote;

			if (tp > dst) {
				in_break = 0;
				if (tp + 1 >= end) {
					dst[0] = '\0';
					return;
				}
				*tp++ = ':';
			}
			wrote = snprintf(tp, (size_t)(end - tp), "%x", group);
			if (wrote < 0 || wrote >= (int)(end - tp)) {
				dst[0] = '\0';
				return;
			}
			tp += wrote;
		} else if (!have_break) {
			have_break = 1;
			in_break = 1;
			if (tp + 1 >= end) {
				dst[0] = '\0';
				return;
			}
			*tp++ = ':';
		} else if (!in_break) {
			if (tp + 2 >= end) {
				dst[0] = '\0';
				return;
			}
			*tp++ = ':';
			*tp++ = '0';
		}
	}
	if (have_break && in_break) {
		if (tp + 1 >= end) {
			dst[0] = '\0';
			return;
		}
		*tp++ = ':';
	}
	if (tp < end) {
		*tp = '\0';
	} else {
		dst[dst_size - 1] = '\0';
	}
}

static int tyhp_dns_collect_ipv4(const char *host, tyhp_dns_addrs *out)
{
	struct hostent *hp;
	int failed = 0;

	tyhp_dns_hostent_lock();
	hp = gethostbyname(host);
	if (hp != NULL && hp->h_addrtype == AF_INET && hp->h_addr_list != NULL) {
		int i;

		for (i = 0; !failed; i++) {
			struct in_addr *entry = NULL;
			struct in_addr in;
			char text[INET_ADDRSTRLEN];

			memcpy(&entry, &hp->h_addr_list[i], sizeof(entry));
			if (entry == NULL) {
				break;
			}
			memcpy(&in, entry, sizeof(in));
			if (inet_ntop(AF_INET, &in, text, sizeof(text)) == NULL) {
				continue;
			}
			if (tyhp_dns_addrs_add(out, text) != 0) {
				failed = 1;
			}
		}
	}
	tyhp_dns_hostent_unlock();
	return failed ? -1 : 0;
}

/* Same cleanup php_dns_free_handle() does after res_nclose() / res_ndestroy():
 * glibc can leave the extended name server addresses allocated. */
#if !defined(_WIN32) && !defined(HAVE_DNS_SEARCH) && defined(HAVE_RES_NSEARCH) && defined(__GLIBC__) && !defined(HAVE_DEPRECATED_DNS_FUNCS)
static void tyhp_dns_free_res(struct __res_state *res)
{
	int ns;

	for (ns = 0; ns < MAXNS; ns++) {
		if (res->_u._ext.nsaddrs[ns] != NULL) {
			free(res->_u._ext.nsaddrs[ns]);
			res->_u._ext.nsaddrs[ns] = NULL;
		}
	}
}
#else
# define tyhp_dns_free_res(res) ((void)(res))
#endif

#if !defined(_WIN32) && defined(HAVE_DN_EXPAND) && defined(HAVE_DN_SKIPNAME)
static int tyhp_dns_parse_aaaa(const unsigned char *packet, size_t length, tyhp_dns_addrs *out)
{
	const unsigned char *cp;
	const unsigned char *end;
	unsigned qd;
	unsigned an;

	if (length < 12) {
		return 0;
	}

	qd = ((unsigned)packet[4] << 8) | packet[5];
	an = ((unsigned)packet[6] << 8) | packet[7];
	cp = packet + 12;
	end = packet + length;

	while (qd > 0) {
		int skipped = dn_skipname(cp, end);

		qd--;
		if (skipped < 0 || cp + skipped + 4 > end) {
			return 0;
		}
		cp += skipped + 4;
	}

	while (an > 0 && cp < end) {
		char name[1024];
		int expanded;
		unsigned type;
		unsigned dlen;
		unsigned int groups[8];
		char text[64];
		int g;

		an--;
		expanded = dn_expand(packet, end, cp, name, (int)sizeof(name) - 2);
		if (expanded < 0) {
			return 0;
		}
		cp += expanded;
		if (cp + 10 > end) {
			return 0;
		}
		type = ((unsigned)cp[0] << 8) | cp[1];
		dlen = ((unsigned)cp[8] << 8) | cp[9];
		cp += 10;
		if (dlen == 0 || cp + dlen > end) {
			return 0;
		}
		if (type != TYHP_T_AAAA) {
			cp += dlen;
			continue;
		}
		/* dns_get_record() requires 16 bytes in the packet, not in this record. */
		if (cp + 16 > end) {
			return 0;
		}
		for (g = 0; g < 8; g++) {
			groups[g] = ((unsigned)cp[g * 2] << 8) | cp[g * 2 + 1];
		}
		tyhp_dns_format_groups(groups, text, sizeof(text));
		if (text[0] != '\0' && tyhp_dns_addrs_add(out, text) != 0) {
			return -1;
		}
		/* dns_get_record() consumes the 16 address bytes, not a longer rdata. */
		cp += 16;
	}
	return 0;
}
#endif

static int tyhp_dns_collect_aaaa(const char *host, tyhp_dns_addrs *out)
{
#ifdef _WIN32
	PDNS_RECORD result = NULL;
	PDNS_RECORD rec;
	DNS_STATUS status;
	int failed = 0;

	status = DnsQuery_A(host, DNS_TYPE_AAAA, DNS_QUERY_STANDARD, NULL, &result, NULL);
	if (status != 0) {
		return 0;
	}
	for (rec = result; rec != NULL && !failed; rec = rec->pNext) {
		unsigned int groups[8];
		char text[64];
		int i;

		if (rec->Flags.S.Section != DnsSectionAnswer || rec->wType != DNS_TYPE_AAAA) {
			continue;
		}
		for (i = 0; i < 4; i++) {
			DWORD chunk = rec->Data.AAAA.Ip6Address.IP6Dword[i];
			groups[i * 2] = htons(LOWORD(chunk));
			groups[i * 2 + 1] = htons(HIWORD(chunk));
		}
		tyhp_dns_format_groups(groups, text, sizeof(text));
		if (text[0] != '\0' && tyhp_dns_addrs_add(out, text) != 0) {
			failed = 1;
		}
	}
	DnsRecordListFree(result, DnsFreeRecordListDeep);
	return failed ? -1 : 0;
#elif defined(HAVE_DNS_SEARCH)
	dns_handle_t handle;
	struct sockaddr_storage from;
	uint32_t fromlen = sizeof(from);
	unsigned char *packet;
	int32_t n;
	int failed = 0;

	handle = dns_open(NULL);
	if (handle == NULL) {
		return 0;
	}
	packet = malloc(TYHP_DNS_PACKET);
	if (packet == NULL) {
		dns_free(handle);
		return -1;
	}
	memset(&from, 0, sizeof(from));
	n = dns_search(handle, host, TYHP_C_IN, TYHP_T_AAAA, (char *)packet, TYHP_DNS_PACKET, (struct sockaddr *)&from, &fromlen);
# if defined(HAVE_DN_EXPAND) && defined(HAVE_DN_SKIPNAME)
	if (n > 12 && (size_t)n <= TYHP_DNS_PACKET) {
		failed = tyhp_dns_parse_aaaa(packet, (size_t)n, out);
	}
# else
	(void)n;
# endif
	free(packet);
	dns_free(handle);
	return failed;
#elif defined(HAVE_RES_NSEARCH)
	struct __res_state state;
	unsigned char *packet;
	int n;
	int failed = 0;

	memset(&state, 0, sizeof(state));
	if (res_ninit(&state) != 0) {
		return 0;
	}
	packet = malloc(TYHP_DNS_PACKET);
	if (packet == NULL) {
		res_nclose(&state);
		tyhp_dns_free_res(&state);
		return -1;
	}
	n = res_nsearch(&state, host, TYHP_C_IN, TYHP_T_AAAA, packet, TYHP_DNS_PACKET);
# if defined(HAVE_DN_EXPAND) && defined(HAVE_DN_SKIPNAME)
	if (n > 12 && (size_t)n <= TYHP_DNS_PACKET) {
		failed = tyhp_dns_parse_aaaa(packet, (size_t)n, out);
	}
# else
	(void)n;
# endif
	free(packet);
# ifdef HAVE_RES_NDESTROY
	res_ndestroy(&state);
# else
	res_nclose(&state);
# endif
	tyhp_dns_free_res(&state);
	return failed;
#elif defined(HAVE_RES_SEARCH)
	unsigned char *packet;
	int n;
	int failed = 0;

	packet = malloc(TYHP_DNS_PACKET);
	if (packet == NULL) {
		return -1;
	}
	tyhp_dns_hostent_lock();
	res_init();
	n = res_search(host, TYHP_C_IN, TYHP_T_AAAA, packet, TYHP_DNS_PACKET);
	tyhp_dns_hostent_unlock();
# if defined(HAVE_DN_EXPAND) && defined(HAVE_DN_SKIPNAME)
	if (n > 12 && (size_t)n <= TYHP_DNS_PACKET) {
		failed = tyhp_dns_parse_aaaa(packet, (size_t)n, out);
	}
# else
	(void)n;
# endif
	free(packet);
	return failed;
#else
	(void)host;
	(void)out;
	return 0;
#endif
}

/* IPv4 list from gethostbyname (what gethostbynamel() returns), then the
 * AAAA list from the same DNS search dns_get_record($host, DNS_AAAA) uses.
 * Duplicates keep the first spelling. A literal IP is that spelling alone. */
static void tyhp_dns_resolve(tyhp_dns_job *job)
{
	tyhp_dns_addrs list;
	int failed = 0;

	memset(&list, 0, sizeof(list));
	if (memchr(job->host, '\0', job->host_len) != NULL) {
		/* gethostbynamel() and dns_get_record() reject a name with a NUL byte.
		 * The C resolvers would stop at it and look up the prefix instead. */
		failed = 0;
	} else if (tyhp_dns_is_ip(job->host, job->host_len)) {
		failed = tyhp_dns_addrs_add(&list, job->host);
	} else {
		if (job->host_len <= TYHP_DNS_MAXFQDN) {
			failed = tyhp_dns_collect_ipv4(job->host, &list);
		}
		if (!failed) {
			failed = tyhp_dns_collect_aaaa(job->host, &list);
		}
	}

	if (failed) {
		tyhp_dns_addrs_clear(&list);
	}
	if (list.count == 0) {
		free(list.items);
		list.items = NULL;
		job->unresolved = 1;
	} else {
		job->unresolved = 0;
	}
	job->addresses = list.items;
	job->count = list.count;
}

static void tyhp_dns_wake(tyhp_dns_job *job)
{
	char byte = 1;

	tyhp_dns_store_ready(job);
#ifdef _WIN32
	if (job->write_sock != INVALID_SOCKET) {
		send(job->write_sock, &byte, 1, 0);
	}
#else
	if (job->write_fd >= 0) {
		ssize_t written;

		do {
			written = write(job->write_fd, &byte, 1);
		} while (written < 0 && errno == EINTR);
	}
#endif
}

#ifdef _WIN32
static unsigned __stdcall tyhp_dns_thread_main(void *arg)
{
	tyhp_dns_job *job = (tyhp_dns_job *)arg;

	tyhp_dns_resolve(job);
	tyhp_dns_wake(job);
	return 0;
}
#else
static void *tyhp_dns_thread_main(void *arg)
{
	tyhp_dns_job *job = (tyhp_dns_job *)arg;
	sigset_t blocked;

	/* A closed read end must not kill the process. This mask is the side
	 * thread's only; the request thread is left alone. */
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &blocked, NULL);

	tyhp_dns_resolve(job);
	tyhp_dns_wake(job);
	return NULL;
}
#endif

static int tyhp_dns_start_thread(tyhp_dns_job *job)
{
#ifdef _WIN32
	uintptr_t handle = _beginthreadex(NULL, 0, tyhp_dns_thread_main, job, 0, NULL);
	if (handle == 0) {
		return -1;
	}
	job->thread = (HANDLE)handle;
#else
	if (pthread_create(&job->thread, NULL, tyhp_dns_thread_main, job) != 0) {
		return -1;
	}
#endif
	job->started = 1;
	return 0;
}

#ifdef _WIN32
static int tyhp_dns_open_notifier(tyhp_dns_job *job, php_stream **stream)
{
	SOCKET listener = INVALID_SOCKET;
	SOCKET reader = INVALID_SOCKET;
	SOCKET writer = INVALID_SOCKET;
	struct sockaddr_in addr;
	int addrlen;
	BOOL nodelay = TRUE;
	php_stream *opened;

	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET) {
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		closesocket(listener);
		return -1;
	}
	if (listen(listener, 1) != 0) {
		closesocket(listener);
		return -1;
	}
	addrlen = sizeof(addr);
	if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) {
		closesocket(listener);
		return -1;
	}
	writer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (writer == INVALID_SOCKET) {
		closesocket(listener);
		return -1;
	}
	if (connect(writer, (struct sockaddr *)&addr, addrlen) != 0) {
		closesocket(writer);
		closesocket(listener);
		return -1;
	}
	reader = accept(listener, NULL, NULL);
	closesocket(listener);
	if (reader == INVALID_SOCKET) {
		closesocket(writer);
		return -1;
	}
	/* Another local process could have connected to the listener first. Keep
	 * the accepted socket only when its peer is our writer. */
	{
		struct sockaddr_in writer_addr;
		struct sockaddr_in peer_addr;
		int writer_len = sizeof(writer_addr);
		int peer_len = sizeof(peer_addr);

		if (getsockname(writer, (struct sockaddr *)&writer_addr, &writer_len) != 0
			|| getpeername(reader, (struct sockaddr *)&peer_addr, &peer_len) != 0
			|| writer_addr.sin_port != peer_addr.sin_port
			|| writer_addr.sin_addr.s_addr != peer_addr.sin_addr.s_addr) {
			closesocket(reader);
			closesocket(writer);
			return -1;
		}
	}
	setsockopt(reader, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
	setsockopt(writer, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

	opened = php_stream_sock_open_from_socket((php_socket_t)reader, NULL);
	if (opened == NULL) {
		closesocket(reader);
		closesocket(writer);
		return -1;
	}
	job->write_sock = writer;
	*stream = opened;
	return 0;
}
#else
static int tyhp_dns_open_notifier(tyhp_dns_job *job, php_stream **stream)
{
	int fds[2];
	php_stream *opened;

	if (pipe(fds) != 0) {
		return -1;
	}
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
# ifdef F_SETNOSIGPIPE
	/* macOS raises SIGPIPE for the process, not for the writing thread, so the
	 * side thread's signal mask does not protect the request thread. Turn the
	 * signal off for this descriptor; the write fails with EPIPE instead. */
	fcntl(fds[1], F_SETNOSIGPIPE, 1);
# endif
	opened = php_stream_fopen_from_fd(fds[0], "rb", NULL);
	if (opened == NULL) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	job->write_fd = fds[1];
	*stream = opened;
	return 0;
}
#endif

static tyhp_dns_job *tyhp_dns_job_create(const char *host, size_t host_len)
{
	tyhp_dns_job *job = calloc(1, sizeof(*job));

	if (job == NULL) {
		return NULL;
	}
	job->host = malloc(host_len + 1);
	if (job->host == NULL) {
		free(job);
		return NULL;
	}
	memcpy(job->host, host, host_len);
	job->host[host_len] = '\0';
	job->host_len = host_len;
#ifdef _WIN32
	job->owner = GetCurrentThreadId();
	job->write_sock = INVALID_SOCKET;
	job->thread = NULL;
#else
	job->pid = getpid();
	job->owner = pthread_self();
	job->write_fd = -1;
#endif
	return job;
}

static void tyhp_dns_return_cancelled(zval *return_value)
{
	array_init(return_value);
	add_assoc_bool(return_value, "ok", 0);
	add_assoc_string(return_value, "reason", "cancelled");
	add_assoc_string(return_value, "message", "DNS lookup was cancelled.");
}

static void tyhp_dns_return_unresolved(zval *return_value, const char *host, size_t host_len)
{
	const char prefix[] = "Could not resolve host ";
	size_t prefix_len = sizeof(prefix) - 1;
	zend_string *message = zend_string_alloc(prefix_len + host_len + 1, 0);

	memcpy(ZSTR_VAL(message), prefix, prefix_len);
	if (host_len > 0) {
		memcpy(ZSTR_VAL(message) + prefix_len, host, host_len);
	}
	ZSTR_VAL(message)[prefix_len + host_len] = '.';

	array_init(return_value);
	add_assoc_bool(return_value, "ok", 0);
	add_assoc_string(return_value, "reason", "unresolved");
	add_assoc_str(return_value, "message", message);
}

static void tyhp_dns_return_addresses(zval *return_value, char **addresses, size_t count)
{
	zval list;
	size_t i;

	array_init(&list);
	for (i = 0; i < count; i++) {
		add_next_index_string(&list, addresses[i]);
	}
	array_init(return_value);
	add_assoc_bool(return_value, "ok", 1);
	add_assoc_zval(return_value, "addresses", &list);
}

void tyhp_dns_module_init(void)
{
#ifdef _WIN32
	if (!tyhp_dns_locks_ready) {
		InitializeCriticalSection(&tyhp_dns_mu);
		InitializeCriticalSection(&tyhp_dns_hostent_mu);
		tyhp_dns_locks_ready = 1;
	}
#endif
}

void tyhp_dns_request_shutdown(void)
{
	tyhp_dns_job *job;

	tyhp_dns_lock();
	for (job = tyhp_dns_jobs; job != NULL; job = job->next) {
		if (tyhp_dns_owned(job)) {
			job->cancelled = 1;
			job->abandoned = 1;
		}
	}
	tyhp_dns_unlock();
}

/* Frees abandoned lookups whose thread has already published its result.
 * Joining one cannot block on the resolver, so this is safe on the request
 * thread. Lookups still resolving stay until a later pass or MSHUTDOWN. */
static void tyhp_dns_reap_abandoned(void)
{
	tyhp_dns_job *reaped = NULL;
	tyhp_dns_job **link;
	tyhp_dns_job *next;

	tyhp_dns_lock();
	link = &tyhp_dns_jobs;
	while (*link != NULL) {
		tyhp_dns_job *job = *link;

		if (job->abandoned && tyhp_dns_load_ready(job)) {
			*link = job->next;
			job->next = reaped;
			reaped = job;
		} else {
			link = &job->next;
		}
	}
	tyhp_dns_unlock();

	while (reaped != NULL) {
		next = reaped->next;
		tyhp_dns_join(reaped);
		tyhp_dns_job_free(reaped);
		reaped = next;
	}
}

void tyhp_dns_module_shutdown(void)
{
	tyhp_dns_job *job;
	tyhp_dns_job *next;

	tyhp_dns_lock();
	job = tyhp_dns_jobs;
	tyhp_dns_jobs = NULL;
	tyhp_dns_unlock();

	while (job != NULL) {
		next = job->next;
		if (job->started && !job->joined) {
			tyhp_dns_join(job);
			job->joined = 1;
		}
		tyhp_dns_job_free(job);
		job = next;
	}

#ifdef _WIN32
	if (tyhp_dns_locks_ready) {
		DeleteCriticalSection(&tyhp_dns_mu);
		DeleteCriticalSection(&tyhp_dns_hostent_mu);
		tyhp_dns_locks_ready = 0;
	}
#endif
}

PHP_METHOD(Ext, dnsStart)
{
	char *host;
	size_t host_len;
	tyhp_dns_job *job;
	php_stream *stream = NULL;
	zend_long id;
	zval pipe;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STRING(host, host_len)
	ZEND_PARSE_PARAMETERS_END();

	tyhp_dns_reap_abandoned();

	job = tyhp_dns_job_create(host, host_len);
	if (job == NULL || tyhp_dns_open_notifier(job, &stream) != 0) {
		if (job != NULL) {
			tyhp_dns_job_free(job);
		}
		tyhp_dns_throw("runtimeexception", sizeof("runtimeexception") - 1, "Could not start the DNS lookup.");
		return;
	}

	tyhp_dns_lock();
	id = tyhp_dns_alloc_id();
	job->id = id;
	job->next = tyhp_dns_jobs;
	tyhp_dns_jobs = job;
	tyhp_dns_unlock();

	if (tyhp_dns_start_thread(job) != 0) {
		int owned;

		tyhp_dns_lock();
		owned = tyhp_dns_unlink(job);
		tyhp_dns_unlock();
		if (owned) {
			php_stream_free(stream, PHP_STREAM_FREE_CLOSE);
			tyhp_dns_job_free(job);
		}
		tyhp_dns_throw("runtimeexception", sizeof("runtimeexception") - 1, "Could not start the DNS lookup.");
		return;
	}

	array_init(return_value);
	add_assoc_long(return_value, "id", id);
	php_stream_to_zval(stream, &pipe);
	add_assoc_zval(return_value, "pipe", &pipe);
}

PHP_METHOD(Ext, dnsTake)
{
	zend_long id;
	tyhp_dns_job *job;
	int cancelled;
	int unresolved;
	char **addresses;
	size_t count;
	char *host;
	size_t host_len;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	tyhp_dns_lock();
	job = tyhp_dns_find_live(id);
	if (job == NULL) {
		tyhp_dns_unlock();
		tyhp_dns_throw("invalidargumentexception", sizeof("invalidargumentexception") - 1, "Unknown DNS lookup id.");
		return;
	}
	if (!tyhp_dns_load_ready(job)) {
		tyhp_dns_unlock();
		tyhp_dns_throw("logicexception", sizeof("logicexception") - 1, "DNS lookup is not ready.");
		return;
	}

	cancelled = job->cancelled;
	unresolved = job->unresolved;
	addresses = job->addresses;
	count = job->count;
	host = job->host;
	host_len = job->host_len;
	job->joined = 1;
	tyhp_dns_unlink(job);
	tyhp_dns_unlock();

	if (job->started) {
		tyhp_dns_join(job);
	}

	if (cancelled) {
		tyhp_dns_return_cancelled(return_value);
	} else if (unresolved || count == 0) {
		tyhp_dns_return_unresolved(return_value, host, host_len);
	} else {
		tyhp_dns_return_addresses(return_value, addresses, count);
	}
	tyhp_dns_job_free(job);
}

PHP_METHOD(Ext, dnsCancel)
{
	zend_long id;
	tyhp_dns_job *job;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	tyhp_dns_lock();
	job = tyhp_dns_find_live(id);
	if (job != NULL) {
		job->cancelled = 1;
	}
	tyhp_dns_unlock();
}
