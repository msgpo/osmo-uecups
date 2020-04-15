/* SPDX-License-Identifier: GPL-2.0 */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <pthread.h>

#include <osmocom/core/linuxlist.h>
#include <osmocom/core/socket.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/logging.h>

#include "gtp.h"
#include "internal.h"

#define LOGEP(ep, lvl, fmt, args ...) \
	LOGP(DEP, lvl, "%s: " fmt, (ep)->name, ## args)

/***********************************************************************
 * GTP Endpoint (UDP socket)
 ***********************************************************************/

/* one thread for reading from each GTP/UDP socket (GTP decapsulation -> tun) */
static void *gtp_endpoint_thread(void *arg)
{
	struct gtp_endpoint *ep = (struct gtp_endpoint *)arg;
	struct gtp_daemon *d = ep->d;

	uint8_t buffer[MAX_UDP_PACKET+sizeof(struct gtp1_header)];

	while (1) {
		struct gtp_tunnel *t;
		const struct gtp1_header *gtph;
		int rc, nread, outfd;
		uint32_t teid;

		/* 1) read GTP packet from UDP socket */
		rc = recvfrom(ep->fd, buffer, sizeof(buffer), 0, (struct sockaddr *)NULL, 0);
		if (rc < 0) {
			LOGEP(ep, LOGL_FATAL, "Error reading from UDP socket: %s\n", strerror(errno));
			exit(1);
		}
		nread = rc;
		if (nread < sizeof(*gtph)) {
			LOGEP(ep, LOGL_NOTICE, "Short read: %d < %lu\n", nread, sizeof(*gtph));
			continue;
		}
		gtph = (struct gtp1_header *)buffer;

		/* check GTP heaader contents */
		if (gtph->flags != 0x30) {
			LOGEP(ep, LOGL_NOTICE, "Unexpected GTP Flags: 0x%02x\n", gtph->flags);
			continue;
		}
		if (gtph->type != GTP_TPDU) {
			LOGEP(ep, LOGL_NOTICE, "Unexpected GTP Message Type: 0x%02x\n", gtph->type);
			continue;
		}
		if (sizeof(*gtph)+ntohs(gtph->length) > nread) {
			LOGEP(ep, LOGL_NOTICE, "Shotr GTP Message: %lu < len=%d\n",
				sizeof(*gtph)+ntohs(gtph->length), nread);
			continue;
		}
		teid = ntohl(gtph->tid);

		/* 2) look-up tunnel based on TEID */
		pthread_rwlock_rdlock(&d->rwlock);
		t = _gtp_tunnel_find_r(d, teid, ep);
		if (!t) {
			pthread_rwlock_unlock(&d->rwlock);
			LOGEP(ep, LOGL_NOTICE, "Unable to find tunnel for TEID=0x%08x\n", teid);
			continue;
		}
		outfd = t->tun_dev->fd;
		pthread_rwlock_unlock(&d->rwlock);

		/* 3) write to TUN device */
		rc = write(outfd, buffer+sizeof(*gtph), ntohs(gtph->length));
		if (rc < nread-sizeof(struct gtp1_header)) {
			LOGEP(ep, LOGL_FATAL, "Error writing to tun device %s\n", strerror(errno));
			exit(1);
		}
	}
}

static struct gtp_endpoint *
_gtp_endpoint_create(struct gtp_daemon *d, const struct sockaddr_storage *bind_addr)
{
	struct gtp_endpoint *ep = talloc_zero(d, struct gtp_endpoint);
	char ipstr[INET6_ADDRSTRLEN];
	char portstr[8];
	int rc;

	if (!ep)
		return NULL;

	rc = getnameinfo((struct sockaddr *)bind_addr, sizeof(*bind_addr),
			 ipstr, sizeof(ipstr), portstr, sizeof(portstr), NI_NUMERICHOST|NI_NUMERICSERV);
	if (rc != 0)
		goto out_free;
	ep->name = talloc_asprintf(ep, "%s:%s", ipstr, portstr);

	ep->d = d;
	ep->use_count = 1;
	ep->bind_addr = *bind_addr;
	ep->fd = socket(ep->bind_addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
	if (ep->fd < 0) {
		LOGEP(ep, LOGL_ERROR, "Cannot create UDP socket: %s\n", strerror(errno));
		goto out_free;
	}
	rc = bind(ep->fd, (struct sockaddr *) &ep->bind_addr, sizeof(ep->bind_addr));
	if (rc < 0) {
		LOGEP(ep, LOGL_ERROR, "Cannot bind UDP socket: %s\n", strerror(errno));
		goto out_close;
	}

	if (pthread_create(&ep->thread, NULL, gtp_endpoint_thread, ep)) {
		LOGEP(ep, LOGL_ERROR, "Cannot start GTP thread: %s\n", strerror(errno));
		goto out_close;
	}

	llist_add_tail(&ep->list, &d->gtp_endpoints);
	LOGEP(ep, LOGL_INFO, "Created\n");

	return ep;

out_close:
	close(ep->fd);
out_free:
	talloc_free(ep);
	return NULL;
}

struct gtp_endpoint *
_gtp_endpoint_find(struct gtp_daemon *d, const struct sockaddr_storage *bind_addr)
{
	struct gtp_endpoint *ep;

	llist_for_each_entry(ep, &d->gtp_endpoints, list) {
		if (sockaddr_equals((const struct sockaddr *) &ep->bind_addr,
				    (const struct sockaddr *) bind_addr)) {
			return ep;
		}
	}
	return NULL;
}

struct gtp_endpoint *
gtp_endpoint_find_or_create(struct gtp_daemon *d, const struct sockaddr_storage *bind_addr)
{
	struct gtp_endpoint *ep;

	/* talloc is not thread safe, all alloc/free must come from main thread */
	ASSERT_MAIN_THREAD(d);

	pthread_rwlock_wrlock(&d->rwlock);
	ep = _gtp_endpoint_find(d, bind_addr);
	if (ep)
		ep->use_count++;
	else
		ep = _gtp_endpoint_create(d, bind_addr);
	pthread_rwlock_unlock(&d->rwlock);

	return ep;
}

/* UNLOCKED hard/forced destroy; caller must make sure references are cleaned up */
static void _gtp_endpoint_destroy(struct gtp_endpoint *ep)
{
	/* talloc is not thread safe, all alloc/free must come from main thread */
	ASSERT_MAIN_THREAD(ep->d);

	if (ep->use_count)
		LOGEP(ep, LOGL_ERROR, "Destroying despite use_count %lu != 0\n", ep->use_count);
	else
		LOGEP(ep, LOGL_INFO, "Destroying\n");

	pthread_cancel(ep->thread);
	llist_del(&ep->list);
	close(ep->fd);
	talloc_free(ep);
}

/* UNLOCKED remove all objects referencing this ep and then destroy */
void _gtp_endpoint_deref_destroy(struct gtp_endpoint *ep)
{
	struct gtp_daemon *d = ep->d;
	struct sockaddr_storage ss = ep->bind_addr;
	struct gtp_tunnel *t, *t2;
	struct gtp_endpoint *ep2;

	/* talloc is not thread safe, all alloc/free must come from main thread */
	ASSERT_MAIN_THREAD(ep->d);

	/* iterate over all tunnels; delete all references to ep */
	llist_for_each_entry_safe(t, t2, &d->gtp_tunnels, list) {
		if (t->gtp_ep == ep)
			_gtp_tunnel_destroy(t);
	}

	/* _gtp_endpoint_destroy may already have been called via
	 * _gtp_tunnel_destroy -> gtp_endpoint_release, so we have to
	 * check if the ep can still be found in the list */
	ep2 = _gtp_endpoint_find(d, &ss);
	if (ep2 && ep2 == ep)
		_gtp_endpoint_destroy(ep2);
}

/* UNLOCKED release a reference; destroy if refcount drops to 0 */
bool _gtp_endpoint_release(struct gtp_endpoint *ep)
{
	bool released = false;

	/* talloc is not thread safe, all alloc/free must come from main thread */
	ASSERT_MAIN_THREAD(ep->d);

	ep->use_count--;
	if (ep->use_count == 0) {
		_gtp_endpoint_destroy(ep);
		released = true;
	} else
		LOGEP(ep, LOGL_DEBUG, "Release; new use_count=%lu\n", ep->use_count);

	return released;
}


/* release a reference; destroy if refcount drops to 0 */
bool gtp_endpoint_release(struct gtp_endpoint *ep)
{
	struct gtp_daemon *d = ep->d;
	bool released;

	/* talloc is not thread safe, all alloc/free must come from main thread */
	ASSERT_MAIN_THREAD(ep->d);

	pthread_rwlock_wrlock(&d->rwlock);
	released = _gtp_endpoint_release(ep);
	pthread_rwlock_unlock(&d->rwlock);

	return released;
}
