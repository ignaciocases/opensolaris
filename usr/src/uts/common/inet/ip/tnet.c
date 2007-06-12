/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/types.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/stropts.h>
#include <sys/sunddi.h>
#include <sys/cred.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/disp.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <inet/common.h>
#include <inet/ipclassifier.h>
#include <inet/ip.h>
#include <inet/mib2.h>
#include <inet/nd.h>
#include <inet/tcp.h>
#include <inet/ip_rts.h>
#include <inet/ip_ire.h>
#include <inet/ip_if.h>
#include <sys/modhash.h>

#include <sys/tsol/label.h>
#include <sys/tsol/label_macro.h>
#include <sys/tsol/tnet.h>
#include <sys/tsol/tndb.h>
#include <sys/strsun.h>

/* tunable for strict error-reply behavior (TCP RST and ICMP Unreachable) */
int tsol_strict_error;

/*
 * Some notes on the Trusted Solaris IRE gateway security attributes:
 *
 * When running in Trusted mode, the routing subsystem determines whether or
 * not a packet can be delivered to an off-link host (not directly reachable
 * through an interface) based on the accreditation checks of the packet's
 * security attributes against those associated with the next-hop gateway.
 *
 * The next-hop gateway's security attributes can be derived from two sources
 * (in order of preference): route-related and the host database.  A Trusted
 * system must be configured with at least the host database containing an
 * entry for the next-hop gateway, or otherwise no accreditation checks can
 * be performed, which may result in the inability to send packets to any
 * off-link destination host.
 *
 * The major differences between the two sources are the number and type of
 * security attributes used for accreditation checks.  A host database entry
 * can contain at most one set of security attributes, specific only to the
 * next-hop gateway.  On contrast, route-related security attributes are made
 * up of a collection of security attributes for the distant networks, and
 * are grouped together per next-hop gateway used to reach those networks.
 * This is the preferred method, and the routing subsystem will fallback to
 * the host database entry only if there are no route-related attributes
 * associated with the next-hop gateway.
 *
 * In Trusted mode, all of the IRE entries (except LOCAL/LOOPBACK/BROADCAST/
 * INTERFACE type) are initialized to contain a placeholder to store this
 * information.  The ire_gw_secattr structure gets allocated, initialized
 * and associated with the IRE during the time of the IRE creation.  The
 * initialization process also includes resolving the host database entry
 * of the next-hop gateway for fallback purposes.  It does not include any
 * route-related attribute setup, as that process comes separately as part
 * of the route requests (add/change) made to the routing subsystem.
 *
 * The underlying logic which involves associating IREs with the gateway
 * security attributes are represented by the following data structures:
 *
 * tsol_gcdb_t, or "gcdb"
 *
 *	- This is a system-wide collection of records containing the
 *	  currently used route-related security attributes, which are fed
 *	  through the routing socket interface, e.g. "route add/change".
 *
 * tsol_gc_t, or "gc"
 *
 *	- This is the gateway credential structure, and it provides for the
 *	  only mechanism to access the contents of gcdb.  More than one gc
 *	  entries may refer to the same gcdb record.  gc's in the system are
 *	  grouped according to the next-hop gateway address.
 *
 * tsol_gcgrp_t, or "gcgrp"
 *
 *	- Group of gateway credentials, and is unique per next-hop gateway
 *	  address.  When the group is not empty, i.e. when gcgrp_count is
 *	  greater than zero, it contains one or more gc's, each pointing to
 *	  a gcdb record which indicates the gateway security attributes
 *	  associated with the next-hop gateway.
 *
 * The fields of the tsol_ire_gw_secattr_t used from within the IRE are:
 *
 * igsa_lock
 *
 *	- Lock that protects all fields within tsol_ire_gw_secattr_t.
 *
 * igsa_rhc
 *
 *	- Remote host cache database entry of next-hop gateway.  This is
 *	  used in the case when there are no route-related attributes
 *	  configured for the IRE.
 *
 * igsa_gc
 *
 *	- A set of route-related attributes that only get set for prefix
 *	  IREs.  If this is non-NULL, the prefix IRE has been associated
 *	  with a set of gateway security attributes by way of route add/
 *	  change functionality.  This field stays NULL for IRE_CACHEs.
 *
 * igsa_gcgrp
 *
 *	- Group of gc's which only gets set for IRE_CACHEs.  Each of the gc
 *	  points to a gcdb record that contains the security attributes
 *	  used to perform the credential checks of the packet which uses
 *	  the IRE.  If the group is not empty, the list of gc's can be
 *	  traversed starting at gcgrp_head.  This field stays NULL for
 *	  prefix IREs.
 */

static kmem_cache_t *ire_gw_secattr_cache;

#define	GCDB_HASH_SIZE	101
#define	GCGRP_HASH_SIZE	101

#define	GCDB_REFRELE(p) {		\
	mutex_enter(&gcdb_lock);	\
	ASSERT((p)->gcdb_refcnt > 0);	\
	if (--((p)->gcdb_refcnt) == 0)	\
		gcdb_inactive(p);	\
	ASSERT(MUTEX_HELD(&gcdb_lock));	\
	mutex_exit(&gcdb_lock);		\
}

static int gcdb_hash_size = GCDB_HASH_SIZE;
static int gcgrp_hash_size = GCGRP_HASH_SIZE;
static mod_hash_t *gcdb_hash;
static mod_hash_t *gcgrp4_hash;
static mod_hash_t *gcgrp6_hash;

static kmutex_t gcdb_lock;
kmutex_t gcgrp_lock;

static uint_t gcdb_hash_by_secattr(void *, mod_hash_key_t);
static int gcdb_hash_cmp(mod_hash_key_t, mod_hash_key_t);
static tsol_gcdb_t *gcdb_lookup(struct rtsa_s *, boolean_t);
static void gcdb_inactive(tsol_gcdb_t *);

static uint_t gcgrp_hash_by_addr(void *, mod_hash_key_t);
static int gcgrp_hash_cmp(mod_hash_key_t, mod_hash_key_t);

static int ire_gw_secattr_constructor(void *, void *, int);
static void ire_gw_secattr_destructor(void *, void *);

void
tnet_init(void)
{
	ire_gw_secattr_cache = kmem_cache_create("ire_gw_secattr_cache",
	    sizeof (tsol_ire_gw_secattr_t), 64, ire_gw_secattr_constructor,
	    ire_gw_secattr_destructor, NULL, NULL, NULL, 0);

	gcdb_hash = mod_hash_create_extended("gcdb_hash",
	    gcdb_hash_size, mod_hash_null_keydtor, mod_hash_null_valdtor,
	    gcdb_hash_by_secattr, NULL, gcdb_hash_cmp, KM_SLEEP);

	gcgrp4_hash = mod_hash_create_extended("gcgrp4_hash",
	    gcgrp_hash_size, mod_hash_null_keydtor, mod_hash_null_valdtor,
	    gcgrp_hash_by_addr, NULL, gcgrp_hash_cmp, KM_SLEEP);

	gcgrp6_hash = mod_hash_create_extended("gcgrp6_hash",
	    gcgrp_hash_size, mod_hash_null_keydtor, mod_hash_null_valdtor,
	    gcgrp_hash_by_addr, NULL, gcgrp_hash_cmp, KM_SLEEP);

	mutex_init(&gcdb_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&gcgrp_lock, NULL, MUTEX_DEFAULT, NULL);
}

void
tnet_fini(void)
{
	kmem_cache_destroy(ire_gw_secattr_cache);
	mod_hash_destroy_hash(gcdb_hash);
	mod_hash_destroy_hash(gcgrp4_hash);
	mod_hash_destroy_hash(gcgrp6_hash);
	mutex_destroy(&gcdb_lock);
	mutex_destroy(&gcgrp_lock);
}

/* ARGSUSED */
static int
ire_gw_secattr_constructor(void *buf, void *cdrarg, int kmflags)
{
	tsol_ire_gw_secattr_t *attrp = buf;

	mutex_init(&attrp->igsa_lock, NULL, MUTEX_DEFAULT, NULL);

	attrp->igsa_rhc = NULL;
	attrp->igsa_gc = NULL;
	attrp->igsa_gcgrp = NULL;

	return (0);
}

/* ARGSUSED */
static void
ire_gw_secattr_destructor(void *buf, void *cdrarg)
{
	tsol_ire_gw_secattr_t *attrp = (tsol_ire_gw_secattr_t *)buf;

	mutex_destroy(&attrp->igsa_lock);
}

tsol_ire_gw_secattr_t *
ire_gw_secattr_alloc(int kmflags)
{
	return (kmem_cache_alloc(ire_gw_secattr_cache, kmflags));
}

void
ire_gw_secattr_free(tsol_ire_gw_secattr_t *attrp)
{
	ASSERT(MUTEX_NOT_HELD(&attrp->igsa_lock));

	if (attrp->igsa_rhc != NULL) {
		TNRHC_RELE(attrp->igsa_rhc);
		attrp->igsa_rhc = NULL;
	}

	if (attrp->igsa_gc != NULL) {
		GC_REFRELE(attrp->igsa_gc);
		attrp->igsa_gc = NULL;
	}
	if (attrp->igsa_gcgrp != NULL) {
		GCGRP_REFRELE(attrp->igsa_gcgrp);
		attrp->igsa_gcgrp = NULL;
	}

	ASSERT(attrp->igsa_rhc == NULL);
	ASSERT(attrp->igsa_gc == NULL);
	ASSERT(attrp->igsa_gcgrp == NULL);

	kmem_cache_free(ire_gw_secattr_cache, attrp);
}

/* ARGSUSED */
static uint_t
gcdb_hash_by_secattr(void *hash_data, mod_hash_key_t key)
{
	const struct rtsa_s *rp = (struct rtsa_s *)key;
	const uint32_t *up, *ue;
	uint_t hash;
	int i;

	ASSERT(rp != NULL);

	/* See comments in hash_bylabel in zone.c for details */
	hash = rp->rtsa_doi + (rp->rtsa_doi << 1);
	up = (const uint32_t *)&rp->rtsa_slrange;
	ue = up + sizeof (rp->rtsa_slrange) / sizeof (*up);
	i = 1;
	while (up < ue) {
		/* using 2^n + 1, 1 <= n <= 16 as source of many primes */
		hash += *up + (*up << ((i % 16) + 1));
		up++;
		i++;
	}
	return (hash);
}

static int
gcdb_hash_cmp(mod_hash_key_t key1, mod_hash_key_t key2)
{
	struct rtsa_s *rp1 = (struct rtsa_s *)key1;
	struct rtsa_s *rp2 = (struct rtsa_s *)key2;

	ASSERT(rp1 != NULL && rp2 != NULL);

	if (blequal(&rp1->rtsa_slrange.lower_bound,
	    &rp2->rtsa_slrange.lower_bound) &&
	    blequal(&rp1->rtsa_slrange.upper_bound,
	    &rp2->rtsa_slrange.upper_bound) &&
	    rp1->rtsa_doi == rp2->rtsa_doi)
		return (0);

	/* No match; not found */
	return (-1);
}

/* ARGSUSED */
static uint_t
gcgrp_hash_by_addr(void *hash_data, mod_hash_key_t key)
{
	tsol_gcgrp_addr_t *ga = (tsol_gcgrp_addr_t *)key;
	uint_t		idx = 0;
	uint32_t	*ap;

	ASSERT(ga != NULL);
	ASSERT(ga->ga_af == AF_INET || ga->ga_af == AF_INET6);

	ap = (uint32_t *)&ga->ga_addr.s6_addr32[0];
	idx ^= *ap++;
	idx ^= *ap++;
	idx ^= *ap++;
	idx ^= *ap;

	return (idx);
}

static int
gcgrp_hash_cmp(mod_hash_key_t key1, mod_hash_key_t key2)
{
	tsol_gcgrp_addr_t *ga1 = (tsol_gcgrp_addr_t *)key1;
	tsol_gcgrp_addr_t *ga2 = (tsol_gcgrp_addr_t *)key2;

	ASSERT(ga1 != NULL && ga2 != NULL);

	/* Address family must match */
	if (ga1->ga_af != ga2->ga_af)
		return (-1);

	if (ga1->ga_addr.s6_addr32[0] == ga2->ga_addr.s6_addr32[0] &&
	    ga1->ga_addr.s6_addr32[1] == ga2->ga_addr.s6_addr32[1] &&
	    ga1->ga_addr.s6_addr32[2] == ga2->ga_addr.s6_addr32[2] &&
	    ga1->ga_addr.s6_addr32[3] == ga2->ga_addr.s6_addr32[3])
		return (0);

	/* No match; not found */
	return (-1);
}

#define	RTSAFLAGS	"\20\11cipso\3doi\2max_sl\1min_sl"

int
rtsa_validate(const struct rtsa_s *rp)
{
	uint32_t mask = rp->rtsa_mask;

	/* RTSA_CIPSO must be set, and DOI must not be zero */
	if ((mask & RTSA_CIPSO) == 0 || rp->rtsa_doi == 0) {
		DTRACE_PROBE2(tx__gcdb__log__error__rtsa__validate, char *,
		    "rtsa(1) lacks flag or has 0 doi.",
		    rtsa_s *, rp);
		return (EINVAL);
	}
	/*
	 * SL range must be specified, and it must have its
	 * upper bound dominating its lower bound.
	 */
	if ((mask & RTSA_SLRANGE) != RTSA_SLRANGE ||
	    !bldominates(&rp->rtsa_slrange.upper_bound,
	    &rp->rtsa_slrange.lower_bound)) {
		DTRACE_PROBE2(tx__gcdb__log__error__rtsa__validate, char *,
		    "rtsa(1) min_sl and max_sl not set or max_sl is "
		    "not dominating.", rtsa_s *, rp);
		return (EINVAL);
	}
	return (0);
}

/*
 * A brief explanation of the reference counting scheme:
 *
 * Prefix IREs have a non-NULL igsa_gc and a NULL igsa_gcgrp;
 * IRE_CACHEs have it vice-versa.
 *
 * Apart from dynamic references due to to reference holds done
 * actively by threads, we have the following references:
 *
 * gcdb_refcnt:
 *	- Every tsol_gc_t pointing to a tsol_gcdb_t contributes a reference
 *	  to the gcdb_refcnt.
 *
 * gc_refcnt:
 *	- A prefix IRE that points to an igsa_gc contributes a reference
 *	  to the gc_refcnt.
 *
 * gcgrp_refcnt:
 *	- An IRE_CACHE that points to an igsa_gcgrp contributes a reference
 *	  to the gcgrp_refcnt of the associated tsol_gcgrp_t.
 *	- Every tsol_gc_t in the chain headed by tsol_gcgrp_t contributes
 *	  a reference to the gcgrp_refcnt.
 */
static tsol_gcdb_t *
gcdb_lookup(struct rtsa_s *rp, boolean_t alloc)
{
	tsol_gcdb_t *gcdb = NULL;

	if (rtsa_validate(rp) != 0)
		return (NULL);

	mutex_enter(&gcdb_lock);
	/* Find a copy in the cache; otherwise, create one and cache it */
	if (mod_hash_find(gcdb_hash, (mod_hash_key_t)rp,
	    (mod_hash_val_t *)&gcdb) == 0) {
		gcdb->gcdb_refcnt++;
		ASSERT(gcdb->gcdb_refcnt != 0);

		DTRACE_PROBE2(tx__gcdb__log__info__gcdb__lookup, char *,
		    "gcdb(1) is in gcdb_hash(global)", tsol_gcdb_t *, gcdb);
	} else if (alloc) {
		gcdb = kmem_zalloc(sizeof (*gcdb), KM_NOSLEEP);
		if (gcdb != NULL) {
			gcdb->gcdb_refcnt = 1;
			gcdb->gcdb_mask = rp->rtsa_mask;
			gcdb->gcdb_doi = rp->rtsa_doi;
			gcdb->gcdb_slrange = rp->rtsa_slrange;

			if (mod_hash_insert(gcdb_hash,
			    (mod_hash_key_t)&gcdb->gcdb_attr,
			    (mod_hash_val_t)gcdb) != 0) {
				mutex_exit(&gcdb_lock);
				kmem_free(gcdb, sizeof (*gcdb));
				return (NULL);
			}

			DTRACE_PROBE2(tx__gcdb__log__info__gcdb__insert, char *,
			    "gcdb(1) inserted in gcdb_hash(global)",
			    tsol_gcdb_t *, gcdb);
		}
	}
	mutex_exit(&gcdb_lock);
	return (gcdb);
}

static void
gcdb_inactive(tsol_gcdb_t *gcdb)
{
	ASSERT(MUTEX_HELD(&gcdb_lock));
	ASSERT(gcdb != NULL && gcdb->gcdb_refcnt == 0);

	(void) mod_hash_remove(gcdb_hash, (mod_hash_key_t)&gcdb->gcdb_attr,
	    (mod_hash_val_t *)&gcdb);

	DTRACE_PROBE2(tx__gcdb__log__info__gcdb__remove, char *,
	    "gcdb(1) removed from gcdb_hash(global)",
	    tsol_gcdb_t *, gcdb);
	kmem_free(gcdb, sizeof (*gcdb));
}

tsol_gc_t *
gc_create(struct rtsa_s *rp, tsol_gcgrp_t *gcgrp, boolean_t *gcgrp_xtrarefp)
{
	tsol_gc_t *gc;
	tsol_gcdb_t *gcdb;

	*gcgrp_xtrarefp = B_TRUE;

	rw_enter(&gcgrp->gcgrp_rwlock, RW_WRITER);
	if ((gcdb = gcdb_lookup(rp, B_TRUE)) == NULL) {
		rw_exit(&gcgrp->gcgrp_rwlock);
		return (NULL);
	}

	for (gc = gcgrp->gcgrp_head; gc != NULL; gc = gc->gc_next) {
		if (gc->gc_db == gcdb) {
			ASSERT(gc->gc_grp == gcgrp);

			gc->gc_refcnt++;
			ASSERT(gc->gc_refcnt != 0);

			GCDB_REFRELE(gcdb);

			DTRACE_PROBE3(tx__gcdb__log__info__gc__create,
			    char *, "found gc(1) in gcgrp(2)",
			    tsol_gc_t *, gc, tsol_gcgrp_t *, gcgrp);
			rw_exit(&gcgrp->gcgrp_rwlock);
			return (gc);
		}
	}

	gc = kmem_zalloc(sizeof (*gc), KM_NOSLEEP);
	if (gc != NULL) {
		if (gcgrp->gcgrp_head == NULL) {
			gcgrp->gcgrp_head = gcgrp->gcgrp_tail = gc;
		} else {
			gcgrp->gcgrp_tail->gc_next = gc;
			gc->gc_prev = gcgrp->gcgrp_tail;
			gcgrp->gcgrp_tail = gc;
		}
		gcgrp->gcgrp_count++;
		ASSERT(gcgrp->gcgrp_count != 0);

		/* caller has incremented gcgrp reference for us */
		gc->gc_grp = gcgrp;

		gc->gc_db = gcdb;
		gc->gc_refcnt = 1;

		DTRACE_PROBE3(tx__gcdb__log__info__gc__create, char *,
		    "added gc(1) to gcgrp(2)", tsol_gc_t *, gc,
		    tsol_gcgrp_t *, gcgrp);

		*gcgrp_xtrarefp = B_FALSE;
	}
	rw_exit(&gcgrp->gcgrp_rwlock);

	return (gc);
}

void
gc_inactive(tsol_gc_t *gc)
{
	tsol_gcgrp_t *gcgrp = gc->gc_grp;

	ASSERT(gcgrp != NULL);
	ASSERT(RW_WRITE_HELD(&gcgrp->gcgrp_rwlock));
	ASSERT(gc->gc_refcnt == 0);

	if (gc->gc_prev != NULL)
		gc->gc_prev->gc_next = gc->gc_next;
	else
		gcgrp->gcgrp_head = gc->gc_next;
	if (gc->gc_next != NULL)
		gc->gc_next->gc_prev = gc->gc_prev;
	else
		gcgrp->gcgrp_tail = gc->gc_prev;
	ASSERT(gcgrp->gcgrp_count > 0);
	gcgrp->gcgrp_count--;

	/* drop lock before it's destroyed */
	rw_exit(&gcgrp->gcgrp_rwlock);

	DTRACE_PROBE3(tx__gcdb__log__info__gc__remove, char *,
	    "removed inactive gc(1) from gcgrp(2)",
	    tsol_gc_t *, gc, tsol_gcgrp_t *, gcgrp);

	GCGRP_REFRELE(gcgrp);

	gc->gc_grp = NULL;
	gc->gc_prev = gc->gc_next = NULL;

	if (gc->gc_db != NULL)
		GCDB_REFRELE(gc->gc_db);

	kmem_free(gc, sizeof (*gc));
}

tsol_gcgrp_t *
gcgrp_lookup(tsol_gcgrp_addr_t *ga, boolean_t alloc)
{
	tsol_gcgrp_t *gcgrp = NULL;
	mod_hash_t *hashp;

	ASSERT(ga->ga_af == AF_INET || ga->ga_af == AF_INET6);

	hashp = (ga->ga_af == AF_INET) ? gcgrp4_hash : gcgrp6_hash;

	mutex_enter(&gcgrp_lock);
	if (mod_hash_find(hashp, (mod_hash_key_t)ga,
	    (mod_hash_val_t *)&gcgrp) == 0) {
		gcgrp->gcgrp_refcnt++;
		ASSERT(gcgrp->gcgrp_refcnt != 0);

		DTRACE_PROBE3(tx__gcdb__log__info__gcgrp__lookup, char *,
		    "found gcgrp(1) in hash(2)", tsol_gcgrp_t *, gcgrp,
		    mod_hash_t *, hashp);

	} else if (alloc) {
		gcgrp = kmem_zalloc(sizeof (*gcgrp), KM_NOSLEEP);
		if (gcgrp != NULL) {
			gcgrp->gcgrp_refcnt = 1;
			rw_init(&gcgrp->gcgrp_rwlock, NULL, RW_DEFAULT, NULL);
			bcopy(ga, &gcgrp->gcgrp_addr, sizeof (*ga));

			if (mod_hash_insert(hashp,
			    (mod_hash_key_t)&gcgrp->gcgrp_addr,
			    (mod_hash_val_t)gcgrp) != 0) {
				mutex_exit(&gcgrp_lock);
				kmem_free(gcgrp, sizeof (*gcgrp));
				return (NULL);
			}

			DTRACE_PROBE3(tx__gcdb__log__info__gcgrp__insert,
			    char *, "inserted gcgrp(1) in hash(2)",
			    tsol_gcgrp_t *, gcgrp, mod_hash_t *, hashp);
		}
	}
	mutex_exit(&gcgrp_lock);
	return (gcgrp);
}

void
gcgrp_inactive(tsol_gcgrp_t *gcgrp)
{
	tsol_gcgrp_addr_t *ga;
	mod_hash_t *hashp;

	ASSERT(MUTEX_HELD(&gcgrp_lock));
	ASSERT(!RW_LOCK_HELD(&gcgrp->gcgrp_rwlock));
	ASSERT(gcgrp != NULL && gcgrp->gcgrp_refcnt == 0);
	ASSERT(gcgrp->gcgrp_head == NULL && gcgrp->gcgrp_count == 0);

	ga = &gcgrp->gcgrp_addr;
	ASSERT(ga->ga_af == AF_INET || ga->ga_af == AF_INET6);

	hashp = (ga->ga_af == AF_INET) ? gcgrp4_hash : gcgrp6_hash;
	(void) mod_hash_remove(hashp, (mod_hash_key_t)ga,
	    (mod_hash_val_t *)&gcgrp);
	rw_destroy(&gcgrp->gcgrp_rwlock);

	DTRACE_PROBE3(tx__gcdb__log__info__gcgrp__remove, char *,
	    "removed inactive gcgrp(1) from hash(2)",
	    tsol_gcgrp_t *, gcgrp, mod_hash_t *, hashp);

	kmem_free(gcgrp, sizeof (*gcgrp));
}

/*
 * Converts CIPSO option to sensitivity label.
 * Validity checks based on restrictions defined in
 * COMMERCIAL IP SECURITY OPTION (CIPSO 2.2) (draft-ietf-cipso-ipsecurity)
 */
static boolean_t
cipso_to_sl(const uchar_t *option, bslabel_t *sl)
{
	const struct cipso_option *co = (const struct cipso_option *)option;
	const struct cipso_tag_type_1 *tt1;

	tt1 = (struct cipso_tag_type_1 *)&co->cipso_tag_type[0];
	if (tt1->tag_type != 1 ||
	    tt1->tag_length < TSOL_TT1_MIN_LENGTH ||
	    tt1->tag_length > TSOL_TT1_MAX_LENGTH ||
	    tt1->tag_length + TSOL_CIPSO_TAG_OFFSET > co->cipso_length)
		return (B_FALSE);

	bsllow(sl);	/* assumed: sets compartments to all zeroes */
	LCLASS_SET((_bslabel_impl_t *)sl, tt1->tag_sl);
	bcopy(tt1->tag_cat, &((_bslabel_impl_t *)sl)->compartments,
	    tt1->tag_length - TSOL_TT1_MIN_LENGTH);
	return (B_TRUE);
}

/*
 * Parse the CIPSO label in the incoming packet and construct a ts_label_t
 * that reflects the CIPSO label and attach it to the dblk cred. Later as
 * the mblk flows up through the stack any code that needs to examine the
 * packet label can inspect the label from the dblk cred. This function is
 * called right in ip_rput for all packets, i.e. locally destined and
 * to be forwarded packets. The forwarding path needs to examine the label
 * to determine how to forward the packet.
 *
 * For IPv4, IP header options have been pulled up, but other headers might not
 * have been.  For IPv6, any hop-by-hop options have been pulled up, but any
 * other headers might not be present.
 */
boolean_t
tsol_get_pkt_label(mblk_t *mp, int version)
{
	tsol_tpc_t	*src_rhtp;
	uchar_t		*opt_ptr = NULL;
	const ipha_t	*ipha;
	bslabel_t	sl;
	uint32_t	doi;
	tsol_ip_label_t	label_type;
	const cipso_option_t *co;
	const void	*src;
	const ip6_t	*ip6h;

	ASSERT(DB_TYPE(mp) == M_DATA);

	if (version == IPV4_VERSION) {
		ipha = (const ipha_t *)mp->b_rptr;
		src = &ipha->ipha_src;
		label_type = tsol_get_option(mp, &opt_ptr);
	} else {
		uchar_t		*after_secopt;
		boolean_t	hbh_needed;
		const uchar_t	*ip6hbh;
		size_t		optlen;

		label_type = OPT_NONE;
		ip6h = (const ip6_t *)mp->b_rptr;
		src = &ip6h->ip6_src;
		if (ip6h->ip6_nxt == IPPROTO_HOPOPTS) {
			ip6hbh = (const uchar_t *)&ip6h[1];
			optlen = (ip6hbh[1] + 1) << 3;
			ASSERT(ip6hbh + optlen <= mp->b_wptr);
			opt_ptr = tsol_find_secopt_v6(ip6hbh, optlen,
			    &after_secopt, &hbh_needed);
			/* tsol_find_secopt_v6 guarantees some sanity */
			if (opt_ptr != NULL &&
			    (optlen = opt_ptr[1]) >= 8) {
				opt_ptr += 2;
				bcopy(opt_ptr, &doi, sizeof (doi));
				doi = ntohl(doi);
				if (doi == IP6LS_DOI_V4 &&
				    opt_ptr[4] == IP6LS_TT_V4 &&
				    opt_ptr[5] <= optlen - 4 &&
				    opt_ptr[7] <= optlen - 6) {
					opt_ptr += sizeof (doi) + 2;
					label_type = OPT_CIPSO;
				}
			}
		}
	}

	switch (label_type) {
	case OPT_CIPSO:
		/*
		 * Convert the CIPSO label to the internal format
		 * and attach it to the dblk cred.
		 * Validity checks based on restrictions defined in
		 * COMMERCIAL IP SECURITY OPTION (CIPSO 2.2)
		 * (draft-ietf-cipso-ipsecurity)
		 */
		if (version == IPV6_VERSION && ip6opt_ls == 0)
			return (B_FALSE);
		co = (const struct cipso_option *)opt_ptr;
		if ((co->cipso_length <
		    TSOL_CIPSO_TAG_OFFSET + TSOL_TT1_MIN_LENGTH) ||
		    (co->cipso_length > IP_MAX_OPT_LENGTH))
			return (B_FALSE);
		bcopy(co->cipso_doi, &doi, sizeof (doi));
		doi = ntohl(doi);
		if (!cipso_to_sl(opt_ptr, &sl))
			return (B_FALSE);
		setbltype(&sl, SUN_SL_ID);
		break;

	case OPT_NONE:
		/*
		 * Handle special cases that are not currently labeled, even
		 * though the sending system may otherwise be configured as
		 * labeled.
		 *	- IGMP
		 *	- IPv4 ICMP Router Discovery
		 *	- IPv6 Neighbor Discovery
		 */
		if (version == IPV4_VERSION) {
			if (ipha->ipha_protocol == IPPROTO_IGMP)
				return (B_TRUE);
			if (ipha->ipha_protocol == IPPROTO_ICMP) {
				const struct icmp *icmp = (const struct icmp *)
				    (mp->b_rptr + IPH_HDR_LENGTH(ipha));

				if ((uchar_t *)icmp > mp->b_wptr) {
					if (!pullupmsg(mp,
					    (uchar_t *)icmp - mp->b_rptr + 1))
						return (B_FALSE);
					icmp = (const struct icmp *)
					    (mp->b_rptr +
					    IPH_HDR_LENGTH(ipha));
				}
				if (icmp->icmp_type == ICMP_ROUTERADVERT ||
				    icmp->icmp_type == ICMP_ROUTERSOLICIT)
					return (B_TRUE);
			}
			src = &ipha->ipha_src;
		} else {
			if (ip6h->ip6_nxt == IPPROTO_ICMPV6) {
				const icmp6_t *icmp6 = (const icmp6_t *)
				    (mp->b_rptr + IPV6_HDR_LEN);

				if ((uchar_t *)icmp6 + ICMP6_MINLEN >
				    mp->b_wptr) {
					if (!pullupmsg(mp,
					    (uchar_t *)icmp6 - mp->b_rptr +
					    ICMP6_MINLEN))
						return (B_FALSE);
					icmp6 = (const icmp6_t *)
					    (mp->b_rptr + IPV6_HDR_LEN);
				}
				if (icmp6->icmp6_type >= MLD_LISTENER_QUERY &&
				    icmp6->icmp6_type <= ICMP6_MAX_INFO_TYPE)
					return (B_TRUE);
			}
			src = &ip6h->ip6_src;
		}

		/*
		 * Look up the tnrhtp database and get the implicit label
		 * that is associated with this unlabeled host and attach
		 * it to the packet.
		 */
		if ((src_rhtp = find_tpc(src, version, B_FALSE)) == NULL)
			return (B_FALSE);

		/* If the sender is labeled, drop the unlabeled packet. */
		if (src_rhtp->tpc_tp.host_type != UNLABELED) {
			TPC_RELE(src_rhtp);
			pr_addr_dbg("unlabeled packet forged from %s\n",
			    version == IPV4_VERSION ? AF_INET : AF_INET6, src);
			return (B_FALSE);
		}

		sl = src_rhtp->tpc_tp.tp_def_label;
		setbltype(&sl, SUN_SL_ID);
		doi = src_rhtp->tpc_tp.tp_doi;
		TPC_RELE(src_rhtp);
		break;

	default:
		return (B_FALSE);
	}

	/* Make sure no other thread is messing with this mblk */
	ASSERT(DB_REF(mp) == 1);
	if (DB_CRED(mp) == NULL) {
		DB_CRED(mp) = newcred_from_bslabel(&sl, doi, KM_NOSLEEP);
		if (DB_CRED(mp) == NULL)
			return (B_FALSE);
	} else {
		cred_t	*newcr;

		newcr = copycred_from_bslabel(DB_CRED(mp), &sl, doi,
		    KM_NOSLEEP);
		if (newcr == NULL)
			return (B_FALSE);
		crfree(DB_CRED(mp));
		DB_CRED(mp) = newcr;
	}

	/*
	 * If the source was unlabeled, then flag as such,
	 * while remembering that CIPSO routers add headers.
	 */
	if (label_type == OPT_NONE)
		crgetlabel(DB_CRED(mp))->tsl_flags |= TSLF_UNLABELED;
	else if (label_type == OPT_CIPSO) {
		if ((src_rhtp = find_tpc(src, version, B_FALSE)) == NULL)
			return (B_FALSE);
		if (src_rhtp->tpc_tp.host_type == UNLABELED)
		    crgetlabel(DB_CRED(mp))->tsl_flags |=
		    TSLF_UNLABELED;
		TPC_RELE(src_rhtp);
	}

	return (B_TRUE);
}

/*
 * This routine determines whether the given packet should be accepted locally.
 * It does a range/set check on the packet's label by looking up the given
 * address in the remote host database.
 */
boolean_t
tsol_receive_local(const mblk_t *mp, const void *addr, uchar_t version,
    boolean_t shared_addr, const conn_t *connp)
{
	const cred_t *credp;
	ts_label_t *plabel, *conn_plabel;
	tsol_tpc_t *tp;
	boolean_t retv;
	const bslabel_t *label, *conn_label;

	/*
	 * The cases in which this can happen are:
	 *	- IPv6 Router Alert, where ip_rput_data_v6 deliberately skips
	 *	  over the label attachment process.
	 *	- MLD output looped-back to ourselves.
	 *	- IPv4 Router Discovery, where tsol_get_pkt_label intentionally
	 *	  avoids the labeling process.
	 * We trust that all valid paths in the code set the cred pointer when
	 * needed.
	 */
	if ((credp = DB_CRED(mp)) == NULL)
		return (B_TRUE);

	/*
	 * If this packet is from the inside (not a remote host) and has the
	 * same zoneid as the selected destination, then no checks are
	 * necessary.  Membership in the zone is enough proof.  This is
	 * intended to be a hot path through this function.
	 */
	if (!crisremote(credp) &&
	    crgetzone(credp) == crgetzone(connp->conn_cred))
		return (B_TRUE);

	plabel = crgetlabel(credp);
	conn_plabel = crgetlabel(connp->conn_cred);
	ASSERT(plabel != NULL && conn_plabel != NULL);

	label = label2bslabel(plabel);
	conn_label = label2bslabel(crgetlabel(connp->conn_cred));

	/*
	 * MLPs are always validated using the range and set of the local
	 * address, even when the remote host is unlabeled.
	 */
	if (connp->conn_mlp_type == mlptBoth ||
	/* LINTED: no consequent */
	    connp->conn_mlp_type == (shared_addr ? mlptShared : mlptPrivate)) {
		;

	/*
	 * If this is a packet from an unlabeled sender, then we must apply
	 * different rules.  If the label is equal to the zone's label, then
	 * it's allowed.  If it's not equal, but the zone is either the global
	 * zone or the label is dominated by the zone's label, then allow it
	 * as long as it's in the range configured for the destination.
	 */
	} else if (plabel->tsl_flags & TSLF_UNLABELED) {
		if (plabel->tsl_doi == conn_plabel->tsl_doi &&
		    blequal(label, conn_label))
			return (B_TRUE);

		/*
		 * conn_zoneid is global for an exclusive stack, thus we use
		 * conn_cred to get the zoneid
		 */
		if (!connp->conn_mac_exempt ||
		    (crgetzoneid(connp->conn_cred) != GLOBAL_ZONEID &&
		    (plabel->tsl_doi != conn_plabel->tsl_doi ||
		    !bldominates(conn_label, label)))) {
			DTRACE_PROBE3(
			    tx__ip__log__drop__receivelocal__mac_unl,
			    char *,
			    "unlabeled packet mp(1) fails mac for conn(2)",
			    mblk_t *, mp, conn_t *, connp);
			return (B_FALSE);
		}

	/*
	 * If this is a packet from a labeled sender, verify the
	 * label on the packet matches the connection label.
	 */
	} else {
		if (plabel->tsl_doi != conn_plabel->tsl_doi ||
		    !blequal(label, conn_label)) {
			DTRACE_PROBE3(tx__ip__log__drop__receivelocal__mac__slp,
			    char *,
			    "packet mp(1) failed label match to SLP conn(2)",
			    mblk_t *, mp, conn_t *, connp);
			return (B_FALSE);
		}
		/*
		 * No further checks will be needed if this is a zone-
		 * specific address because (1) The process for bringing up
		 * the interface ensures the zone's label is within the zone-
		 * specific address's valid label range; (2) For cases where
		 * the conn is bound to the unspecified addresses, ip fanout
		 * logic ensures conn's zoneid equals the dest addr's zoneid;
		 * (3) Mac-exempt and mlp logic above already handle all
		 * cases where the zone label may not be the same as the
		 * conn label.
		 */
		if (!shared_addr)
			return (B_TRUE);
	}

	tp = find_tpc(addr, version, B_FALSE);
	if (tp == NULL) {
		DTRACE_PROBE3(tx__ip__log__drop__receivelocal__no__tnr,
		    char *, "dropping mp(1), host(2) lacks entry",
		    mblk_t *, mp, void *, addr);
		return (B_FALSE);
	}

	/*
	 * The local host address should not be unlabeled at this point.  The
	 * only way this can happen is that the destination isn't unicast.  We
	 * assume that the packet should not have had a label, and thus should
	 * have been handled by the TSLF_UNLABELED logic above.
	 */
	if (tp->tpc_tp.host_type == UNLABELED) {
		retv = B_FALSE;
		DTRACE_PROBE3(tx__ip__log__drop__receivelocal__flag, char *,
		    "mp(1) unlabeled source, but tp is not unlabeled.",
		    mblk_t *, mp, tsol_tpc_t *, tp);

	} else if (tp->tpc_tp.host_type != SUN_CIPSO) {
		retv = B_FALSE;
		DTRACE_PROBE3(tx__ip__log__drop__receivelocal__tptype, char *,
		    "delivering mp(1), found unrecognized tpc(2) type.",
		    mblk_t *, mp, tsol_tpc_t *, tp);

	} else if (plabel->tsl_doi != tp->tpc_tp.tp_doi) {
		retv = B_FALSE;
		DTRACE_PROBE3(tx__ip__log__drop__receivelocal__mac, char *,
		    "mp(1) could not be delievered to tp(2), doi mismatch",
		    mblk_t *, mp, tsol_tpc_t *, tp);

	} else if (!_blinrange(label, &tp->tpc_tp.tp_sl_range_cipso) &&
	    !blinlset(label, tp->tpc_tp.tp_sl_set_cipso)) {
		retv = B_FALSE;
		DTRACE_PROBE3(tx__ip__log__drop__receivelocal__mac, char *,
		    "mp(1) could not be delievered to tp(2), bad mac",
		    mblk_t *, mp, tsol_tpc_t *, tp);
	} else {
		retv = B_TRUE;
	}

	TPC_RELE(tp);

	return (retv);
}

boolean_t
tsol_can_accept_raw(mblk_t *mp, boolean_t check_host)
{
	ts_label_t	*plabel = NULL;
	tsol_tpc_t	*src_rhtp, *dst_rhtp;
	boolean_t	retv;

	if (DB_CRED(mp) != NULL)
		plabel = crgetlabel(DB_CRED(mp));

	/* We are bootstrapping or the internal template was never deleted */
	if (plabel == NULL)
		return (B_TRUE);

	if (IPH_HDR_VERSION(mp->b_rptr) == IPV4_VERSION) {
		ipha_t *ipha = (ipha_t *)mp->b_rptr;

		src_rhtp = find_tpc(&ipha->ipha_src, IPV4_VERSION,
		    B_FALSE);
		if (src_rhtp == NULL)
			return (B_FALSE);
		dst_rhtp = find_tpc(&ipha->ipha_dst, IPV4_VERSION,
		    B_FALSE);
	} else {
		ip6_t *ip6h = (ip6_t *)mp->b_rptr;

		src_rhtp = find_tpc(&ip6h->ip6_src, IPV6_VERSION,
		    B_FALSE);
		if (src_rhtp == NULL)
			return (B_FALSE);
		dst_rhtp = find_tpc(&ip6h->ip6_dst, IPV6_VERSION,
		    B_FALSE);
	}
	if (dst_rhtp == NULL) {
		TPC_RELE(src_rhtp);
		return (B_FALSE);
	}

	if (label2doi(plabel) != src_rhtp->tpc_tp.tp_doi) {
		retv = B_FALSE;

	/*
	 * Check that the packet's label is in the correct range for labeled
	 * sender, or is equal to the default label for unlabeled sender.
	 */
	} else if ((src_rhtp->tpc_tp.host_type != UNLABELED &&
	    !_blinrange(label2bslabel(plabel),
	    &src_rhtp->tpc_tp.tp_sl_range_cipso) &&
	    !blinlset(label2bslabel(plabel),
	    src_rhtp->tpc_tp.tp_sl_set_cipso)) ||
	    (src_rhtp->tpc_tp.host_type == UNLABELED &&
	    !blequal(&plabel->tsl_label, &src_rhtp->tpc_tp.tp_def_label))) {
		retv = B_FALSE;

	} else if (check_host) {
		retv = B_TRUE;

	/*
	 * Until we have SL range in the Zone structure, pass it
	 * when our own address lookup returned an internal entry.
	 */
	} else switch (dst_rhtp->tpc_tp.host_type) {
	case UNLABELED:
		retv = B_TRUE;
		break;

	case SUN_CIPSO:
		retv = _blinrange(label2bslabel(plabel),
		    &dst_rhtp->tpc_tp.tp_sl_range_cipso) ||
		    blinlset(label2bslabel(plabel),
		    dst_rhtp->tpc_tp.tp_sl_set_cipso);
		break;

	default:
		retv = B_FALSE;
	}
	TPC_RELE(src_rhtp);
	TPC_RELE(dst_rhtp);
	return (retv);
}

/*
 * This routine determines whether a response to a failed packet delivery or
 * connection should be sent back.  By default, the policy is to allow such
 * messages to be sent at all times, as these messages reveal little useful
 * information and are healthy parts of TCP/IP networking.
 *
 * If tsol_strict_error is set, then we do strict tests: if the packet label is
 * within the label range/set of this host/zone, return B_TRUE; otherwise
 * return B_FALSE, which causes the packet to be dropped silently.
 *
 * Note that tsol_get_pkt_label will cause the packet to drop if the sender is
 * marked as labeled in the remote host database, but the packet lacks a label.
 * This means that we don't need to do a lookup on the source; the
 * TSLF_UNLABELED flag is sufficient.
 */
boolean_t
tsol_can_reply_error(const mblk_t *mp)
{
	ts_label_t	*plabel = NULL;
	tsol_tpc_t	*rhtp;
	const ipha_t	*ipha;
	const ip6_t	*ip6h;
	boolean_t	retv;
	bslabel_t	*pktbs;

	/* Caller must pull up at least the IP header */
	ASSERT(MBLKL(mp) >= (IPH_HDR_VERSION(mp->b_rptr) == IPV4_VERSION ?
	    sizeof (*ipha) : sizeof (*ip6h)));

	if (!tsol_strict_error)
		return (B_TRUE);

	if (DB_CRED(mp) != NULL)
		plabel = crgetlabel(DB_CRED(mp));

	/* We are bootstrapping or the internal template was never deleted */
	if (plabel == NULL)
		return (B_TRUE);

	if (IPH_HDR_VERSION(mp->b_rptr) == IPV4_VERSION) {
		ipha = (const ipha_t *)mp->b_rptr;
		rhtp = find_tpc(&ipha->ipha_dst, IPV4_VERSION, B_FALSE);
	} else {
		ip6h = (const ip6_t *)mp->b_rptr;
		rhtp = find_tpc(&ip6h->ip6_dst, IPV6_VERSION, B_FALSE);
	}

	if (rhtp == NULL || label2doi(plabel) != rhtp->tpc_tp.tp_doi) {
		retv = B_FALSE;
	} else {
		/*
		 * If we're in the midst of forwarding, then the destination
		 * address might not be labeled.  In that case, allow unlabeled
		 * packets through only if the default label is the same, and
		 * labeled ones if they dominate.
		 */
		pktbs = label2bslabel(plabel);
		switch (rhtp->tpc_tp.host_type) {
		case UNLABELED:
			if (plabel->tsl_flags & TSLF_UNLABELED) {
				retv = blequal(pktbs,
				    &rhtp->tpc_tp.tp_def_label);
			} else {
				retv = bldominates(pktbs,
				    &rhtp->tpc_tp.tp_def_label);
			}
			break;

		case SUN_CIPSO:
			retv = _blinrange(pktbs,
			    &rhtp->tpc_tp.tp_sl_range_cipso) ||
			    blinlset(pktbs, rhtp->tpc_tp.tp_sl_set_cipso);
			break;

		default:
			retv = B_FALSE;
			break;
		}
	}

	if (rhtp != NULL)
		TPC_RELE(rhtp);

	return (retv);
}

/*
 * Finds the zone associated with the given packet.  Returns GLOBAL_ZONEID if
 * the zone cannot be located.
 *
 * This is used by the classifier when the packet matches an ALL_ZONES IRE, and
 * there's no MLP defined.
 *
 * Note that we assume that this is only invoked in the ALL_ZONES case.
 * Handling other cases would require handle exclusive stack zones where either
 * this routine or the callers would have to map from
 * the zoneid (zone->zone_id) to what IP uses in conn_zoneid etc.
 */
zoneid_t
tsol_packet_to_zoneid(const mblk_t *mp)
{
	cred_t *cr = DB_CRED(mp);
	zone_t *zone;
	ts_label_t *label;

	if (cr != NULL) {
		if ((label = crgetlabel(cr)) != NULL) {
			zone = zone_find_by_label(label);
			if (zone != NULL) {
				zoneid_t zoneid = zone->zone_id;

				zone_rele(zone);
				return (zoneid);
			}
		}
	}
	return (GLOBAL_ZONEID);
}

int
tsol_ire_match_gwattr(ire_t *ire, const ts_label_t *tsl)
{
	int		error = 0;
	tsol_ire_gw_secattr_t *attrp = NULL;
	tsol_tnrhc_t	*gw_rhc = NULL;
	tsol_gcgrp_t	*gcgrp = NULL;
	tsol_gc_t	*gc = NULL;
	in_addr_t	ga_addr4;
	void		*paddr = NULL;

	/* Not in Trusted mode or IRE is local/loopback/broadcast/interface */
	if (!is_system_labeled() ||
	    (ire->ire_type & (IRE_LOCAL | IRE_LOOPBACK | IRE_BROADCAST |
	    IRE_INTERFACE)))
		goto done;

	/*
	 * If we don't have a label to compare with, or the IRE does not
	 * contain any gateway security attributes, there's not much that
	 * we can do.  We let the former case pass, and the latter fail,
	 * since the IRE doesn't qualify for a match due to the lack of
	 * security attributes.
	 */
	if (tsl == NULL || ire->ire_gw_secattr == NULL) {
		if (tsl != NULL) {
			DTRACE_PROBE3(
			tx__ip__log__drop__irematch__nogwsec, char *,
			"ire(1) lacks ire_gw_secattr when matching label(2)",
			ire_t *, ire, ts_label_t *, tsl);
			error = EACCES;
		}
		goto done;
	}

	attrp = ire->ire_gw_secattr;

	/*
	 * The possible lock order scenarios related to the tsol gateway
	 * attribute locks are documented at the beginning of ip.c in the
	 * lock order scenario section.
	 */
	mutex_enter(&attrp->igsa_lock);

	/*
	 * Depending on the IRE type (prefix vs. cache), we seek the group
	 * structure which contains all security credentials of the gateway.
	 * A prefix IRE is associated with at most one gateway credential,
	 * while a cache IRE is associated with every credentials that the
	 * gateway has.
	 */
	if ((gc = attrp->igsa_gc) != NULL) {			/* prefix */
		gcgrp = gc->gc_grp;
		ASSERT(gcgrp != NULL);
		rw_enter(&gcgrp->gcgrp_rwlock, RW_READER);
	} else if ((gcgrp = attrp->igsa_gcgrp) != NULL) {	/* cache */
		rw_enter(&gcgrp->gcgrp_rwlock, RW_READER);
		gc = gcgrp->gcgrp_head;
		if (gc == NULL) {
			/* gc group is empty, so the drop lock now */
			ASSERT(gcgrp->gcgrp_count == 0);
			rw_exit(&gcgrp->gcgrp_rwlock);
			gcgrp = NULL;
		}
	}

	if (gcgrp != NULL)
		GCGRP_REFHOLD(gcgrp);

	if ((gw_rhc = attrp->igsa_rhc) != NULL) {
		/*
		 * If our cached entry has grown stale, then discard it so we
		 * can get a new one.
		 */
		if (gw_rhc->rhc_invalid || gw_rhc->rhc_tpc->tpc_invalid) {
			TNRHC_RELE(gw_rhc);
			attrp->igsa_rhc = gw_rhc = NULL;
		} else {
			TNRHC_HOLD(gw_rhc)
		}
	}

	/* Last attempt at loading the template had failed; try again */
	if (gw_rhc == NULL) {
		if (gcgrp != NULL) {
			tsol_gcgrp_addr_t *ga = &gcgrp->gcgrp_addr;

			if (ire->ire_ipversion == IPV4_VERSION) {
				ASSERT(ga->ga_af == AF_INET);
				IN6_V4MAPPED_TO_IPADDR(&ga->ga_addr, ga_addr4);
				paddr = &ga_addr4;
			} else {
				ASSERT(ga->ga_af == AF_INET6);
				paddr = &ga->ga_addr;
			}
		} else if (ire->ire_ipversion == IPV6_VERSION &&
		    !IN6_IS_ADDR_UNSPECIFIED(&ire->ire_gateway_addr_v6)) {
			paddr = &ire->ire_gateway_addr_v6;
		} else if (ire->ire_ipversion == IPV4_VERSION &&
		    ire->ire_gateway_addr != INADDR_ANY) {
			paddr = &ire->ire_gateway_addr;
		}

		/* We've found a gateway address to do the template lookup */
		if (paddr != NULL) {
			ASSERT(gw_rhc == NULL);
			gw_rhc = find_rhc(paddr, ire->ire_ipversion, B_FALSE);
			if (gw_rhc != NULL) {
				/*
				 * Note that if the lookup above returned an
				 * internal template, we'll use it for the
				 * time being, and do another lookup next
				 * time around.
				 */
				/* Another thread has loaded the template? */
				if (attrp->igsa_rhc != NULL) {
					TNRHC_RELE(gw_rhc)
					/* reload, it could be different */
					gw_rhc = attrp->igsa_rhc;
				} else {
					attrp->igsa_rhc = gw_rhc;
				}
				/*
				 * Hold an extra reference just like we did
				 * above prior to dropping the igsa_lock.
				 */
				TNRHC_HOLD(gw_rhc)
			}
		}
	}

	mutex_exit(&attrp->igsa_lock);
	/* Gateway template not found */
	if (gw_rhc == NULL) {
		/*
		 * If destination address is directly reachable through an
		 * interface rather than through a learned route, pass it.
		 */
		if (paddr != NULL) {
			DTRACE_PROBE3(
			    tx__ip__log__drop__irematch__nogwtmpl, char *,
			    "ire(1), label(2) off-link with no gw_rhc",
			    ire_t *, ire, ts_label_t *, tsl);
			error = EINVAL;
		}
		goto done;
	}

	if (gc != NULL) {
		tsol_gcdb_t *gcdb;
		/*
		 * In the case of IRE_CACHE we've got one or more gateway
		 * security credentials to compare against the passed in label.
		 * Perform label range comparison against each security
		 * credential of the gateway. In the case of a prefix ire
		 * we need to match against the security attributes of
		 * just the route itself, so the loop is executed only once.
		 */
		ASSERT(gcgrp != NULL);
		do {
			gcdb = gc->gc_db;
			if (tsl->tsl_doi == gcdb->gcdb_doi &&
			    _blinrange(&tsl->tsl_label, &gcdb->gcdb_slrange))
				break;
			if (ire->ire_type == IRE_CACHE)
				gc = gc->gc_next;
			else
				gc = NULL;
		} while (gc != NULL);

		if (gc == NULL) {
			DTRACE_PROBE3(
			    tx__ip__log__drop__irematch__nogcmatched,
			    char *, "ire(1), tsl(2): all gc failed match",
			    ire_t *, ire, ts_label_t *, tsl);
			error = EACCES;
		}
	} else {
		/*
		 * We didn't find any gateway credentials in the IRE
		 * attributes; fall back to the gateway's template for
		 * label range checks, if we are required to do so.
		 */
		ASSERT(gw_rhc != NULL);
		switch (gw_rhc->rhc_tpc->tpc_tp.host_type) {
		case SUN_CIPSO:
			if (tsl->tsl_doi !=
			    gw_rhc->rhc_tpc->tpc_tp.tp_doi ||
			    (!_blinrange(&tsl->tsl_label,
			    &gw_rhc->rhc_tpc->tpc_tp.
			    tp_sl_range_cipso) &&
			    !blinlset(&tsl->tsl_label,
			    gw_rhc->rhc_tpc->tpc_tp.tp_sl_set_cipso))) {
				error = EACCES;
				DTRACE_PROBE4(
				    tx__ip__log__drop__irematch__deftmpl,
				    char *, "ire(1), tsl(2), gw_rhc(3) "
				    "failed match (cipso gw)",
				    ire_t *, ire, ts_label_t *, tsl,
				    tsol_tnrhc_t *, gw_rhc);
			}
			break;

		case UNLABELED:
			if (tsl->tsl_doi !=
				gw_rhc->rhc_tpc->tpc_tp.tp_doi ||
			    (!_blinrange(&tsl->tsl_label,
			    &gw_rhc->rhc_tpc->tpc_tp.tp_gw_sl_range) &&
			    !blinlset(&tsl->tsl_label,
			    gw_rhc->rhc_tpc->tpc_tp.tp_gw_sl_set))) {
				error = EACCES;
				DTRACE_PROBE4(
				    tx__ip__log__drop__irematch__deftmpl,
				    char *, "ire(1), tsl(2), gw_rhc(3) "
				    "failed match (unlabeled gw)",
				    ire_t *, ire, ts_label_t *, tsl,
				    tsol_tnrhc_t *, gw_rhc);
			}
			break;
		}
	}

done:

	if (gcgrp != NULL) {
		rw_exit(&gcgrp->gcgrp_rwlock);
		GCGRP_REFRELE(gcgrp);
	}

	if (gw_rhc != NULL)
		TNRHC_RELE(gw_rhc)

	return (error);
}

/*
 * Performs label accreditation checks for packet forwarding.
 *
 * Returns a pointer to the modified mblk if allowed for forwarding,
 * or NULL if the packet must be dropped.
 */
mblk_t *
tsol_ip_forward(ire_t *ire, mblk_t *mp)
{
	tsol_ire_gw_secattr_t *attrp = NULL;
	ipha_t		*ipha;
	ip6_t		*ip6h;
	const void	*pdst;
	const void	*psrc;
	boolean_t	off_link;
	tsol_tpc_t	*dst_rhtp, *gw_rhtp;
	tsol_ip_label_t label_type;
	uchar_t		*opt_ptr = NULL;
	ts_label_t	*tsl;
	uint8_t		proto;
	int		af, adjust;
	uint16_t	iplen;
	boolean_t	need_tpc_rele = B_FALSE;
	ipaddr_t	*gw;
	ip_stack_t	*ipst = ire->ire_ipst;

	ASSERT(ire != NULL && mp != NULL);
	ASSERT(ire->ire_stq != NULL);

	af = (ire->ire_ipversion == IPV4_VERSION) ? AF_INET : AF_INET6;

	if (IPH_HDR_VERSION(mp->b_rptr) == IPV4_VERSION) {
		ASSERT(ire->ire_ipversion == IPV4_VERSION);
		ipha = (ipha_t *)mp->b_rptr;
		psrc = &ipha->ipha_src;
		pdst = &ipha->ipha_dst;
		proto = ipha->ipha_protocol;

		/*
		 * off_link is TRUE if destination not directly reachable.
		 * Surya note: we avoid creation of per-dst IRE_CACHE entries
		 * for forwarded packets, so we set off_link to be TRUE
		 * if the packet dst is different from the ire_addr of
		 * the ire for the nexthop.
		 */
		off_link = ((ipha->ipha_dst != ire->ire_addr) ||
		    (ire->ire_gateway_addr != INADDR_ANY));
	} else {
		ASSERT(ire->ire_ipversion == IPV6_VERSION);
		ip6h = (ip6_t *)mp->b_rptr;
		psrc = &ip6h->ip6_src;
		pdst = &ip6h->ip6_dst;
		proto = ip6h->ip6_nxt;

		if (proto != IPPROTO_TCP && proto != IPPROTO_UDP &&
		    proto != IPPROTO_ICMPV6) {
			uint8_t *nexthdrp;
			uint16_t hdr_len;

			if (!ip_hdr_length_nexthdr_v6(mp, ip6h, &hdr_len,
			    &nexthdrp)) {
				/* malformed packet; drop it */
				return (NULL);
			}
			proto = *nexthdrp;
		}

		/* destination not directly reachable? */
		off_link = !IN6_IS_ADDR_UNSPECIFIED(&ire->ire_gateway_addr_v6);
	}

	if ((tsl = MBLK_GETLABEL(mp)) == NULL)
		return (mp);

	label_type = tsol_get_option(mp, &opt_ptr);

	ASSERT(psrc != NULL && pdst != NULL);
	dst_rhtp = find_tpc(pdst, ire->ire_ipversion, B_FALSE);

	if (dst_rhtp == NULL) {
		/*
		 * Without a template we do not know if forwarding
		 * violates MAC
		 */
		DTRACE_PROBE3(tx__ip__log__drop__forward__nodst, char *,
		    "mp(1) dropped, no template for destination ip4|6(2)",
		    mblk_t *, mp, void *, pdst);
		return (NULL);
	}

	/*
	 * Gateway template must have existed for off-link destinations,
	 * since tsol_ire_match_gwattr has ensured such condition.
	 */
	if (ire->ire_ipversion == IPV4_VERSION && off_link) {
		/*
		 * Surya note: first check if we can get the gw_rhtp from
		 * the ire_gw_secattr->igsa_rhc; if this is null, then
		 * do a lookup based on the ire_addr (address of gw)
		 */
		if (ire->ire_gw_secattr != NULL &&
		    ire->ire_gw_secattr->igsa_rhc != NULL) {
			attrp = ire->ire_gw_secattr;
			gw_rhtp = attrp->igsa_rhc->rhc_tpc;
		} else  {
			/*
			 * use the ire_addr if this is the IRE_CACHE of nexthop
			 */
			gw = (ire->ire_gateway_addr == NULL? &ire->ire_addr :
			    &ire->ire_gateway_addr);
			gw_rhtp = find_tpc(gw, ire->ire_ipversion, B_FALSE);
			need_tpc_rele = B_TRUE;
		}
		if (gw_rhtp == NULL) {
			DTRACE_PROBE3(tx__ip__log__drop__forward__nogw, char *,
			    "mp(1) dropped, no gateway in ire attributes(2)",
			    mblk_t *, mp, tsol_ire_gw_secattr_t *, attrp);
			mp = NULL;
			goto keep_label;
		}
	}
	if (ire->ire_ipversion == IPV6_VERSION &&
	    ((attrp = ire->ire_gw_secattr) == NULL || attrp->igsa_rhc == NULL ||
	    (gw_rhtp = attrp->igsa_rhc->rhc_tpc) == NULL) && off_link) {
		DTRACE_PROBE3(tx__ip__log__drop__forward__nogw, char *,
		    "mp(1) dropped, no gateway in ire attributes(2)",
		    mblk_t *, mp, tsol_ire_gw_secattr_t *, attrp);
		mp = NULL;
		goto keep_label;
	}

	/*
	 * Check that the label for the packet is acceptable
	 * by destination host; otherwise, drop it.
	 */
	switch (dst_rhtp->tpc_tp.host_type) {
	case SUN_CIPSO:
		if (tsl->tsl_doi != dst_rhtp->tpc_tp.tp_doi ||
		    (!_blinrange(&tsl->tsl_label,
		    &dst_rhtp->tpc_tp.tp_sl_range_cipso) &&
		    !blinlset(&tsl->tsl_label,
		    dst_rhtp->tpc_tp.tp_sl_set_cipso))) {
			DTRACE_PROBE4(tx__ip__log__drop__forward__mac, char *,
			    "labeled packet mp(1) dropped, label(2) fails "
			    "destination(3) accredation check",
			    mblk_t *, mp, ts_label_t *, tsl,
			    tsol_tpc_t *, dst_rhtp);
			mp = NULL;
			goto keep_label;
		}
		break;


	case UNLABELED:
		if (tsl->tsl_doi != dst_rhtp->tpc_tp.tp_doi ||
		    !blequal(&dst_rhtp->tpc_tp.tp_def_label,
		    &tsl->tsl_label)) {
			DTRACE_PROBE4(tx__ip__log__drop__forward__mac, char *,
			    "unlabeled packet mp(1) dropped, label(2) fails "
			    "destination(3) accredation check",
			    mblk_t *, mp, ts_label_t *, tsl,
			    tsol_tpc_t *, dst_rhtp);
			mp = NULL;
			goto keep_label;
		}
		break;
	}
	if (label_type == OPT_CIPSO) {
		/*
		 * We keep the label on any of the following cases:
		 *
		 *   1. The destination is labeled (on/off-link).
		 *   2. The unlabeled destination is off-link,
		 *	and the next hop gateway is labeled.
		 */
		if (dst_rhtp->tpc_tp.host_type != UNLABELED ||
		    (off_link &&
		    gw_rhtp->tpc_tp.host_type != UNLABELED))
			goto keep_label;

		/*
		 * Strip off the CIPSO option from the packet because: the
		 * unlabeled destination host is directly reachable through
		 * an interface (on-link); or, the unlabeled destination host
		 * is not directly reachable (off-link), and the next hop
		 * gateway is unlabeled.
		 */
		adjust = (af == AF_INET) ? tsol_remove_secopt(ipha, MBLKL(mp)) :
		    tsol_remove_secopt_v6(ip6h, MBLKL(mp));

		ASSERT(adjust <= 0);
		if (adjust != 0) {

			/* adjust is negative */
			ASSERT((mp->b_wptr + adjust) >= mp->b_rptr);
			mp->b_wptr += adjust;

			if (af == AF_INET) {
				ipha = (ipha_t *)mp->b_rptr;
				iplen = ntohs(ipha->ipha_length) + adjust;
				ipha->ipha_length = htons(iplen);
				ipha->ipha_hdr_checksum = 0;
				ipha->ipha_hdr_checksum = ip_csum_hdr(ipha);
			}
			DTRACE_PROBE3(tx__ip__log__info__forward__adjust,
			    char *,
			    "mp(1) adjusted(2) for CIPSO option removal",
			    mblk_t *, mp, int, adjust);
		}
		goto keep_label;
	}

	ASSERT(label_type == OPT_NONE);
	ASSERT(dst_rhtp != NULL);

	/*
	 * We need to add CIPSO option if the destination or the next hop
	 * gateway is labeled.  Otherwise, pass the packet as is.
	 */
	if (dst_rhtp->tpc_tp.host_type == UNLABELED &&
	    (!off_link || gw_rhtp->tpc_tp.host_type == UNLABELED))
		goto keep_label;

	if ((af == AF_INET &&
	    tsol_check_label(DB_CRED(mp), &mp, &adjust, B_FALSE, ipst) != 0) ||
	    (af == AF_INET6 &&
	    tsol_check_label_v6(DB_CRED(mp), &mp, &adjust, B_FALSE,
	    ipst) != 0)) {
		mp = NULL;
		goto keep_label;
	}

	ASSERT(adjust != -1);
	if (adjust != 0) {
		if (af == AF_INET) {
			ipha = (ipha_t *)mp->b_rptr;
			iplen = ntohs(ipha->ipha_length) + adjust;
			ipha->ipha_length = htons(iplen);
			ipha->ipha_hdr_checksum = 0;
			ipha->ipha_hdr_checksum = ip_csum_hdr(ipha);
		}

		DTRACE_PROBE3(tx__ip__log__info__forward__adjust, char *,
		    "mp(1) adjusted(2) for CIPSO option removal",
		    mblk_t *, mp, int, adjust);
	}

keep_label:
	TPC_RELE(dst_rhtp);
	if (need_tpc_rele && gw_rhtp != NULL)
		TPC_RELE(gw_rhtp);
	return (mp);
}

/*
 * Name:	tsol_rtsa_init()
 *
 * Normal:	Sanity checks on the route security attributes provided by
 *		user.  Convert it into a route security parameter list to
 *		be returned to caller.
 *
 * Output:	EINVAL if bad security attributes in the routing message
 *		ENOMEM if unable to allocate data structures
 *		0 otherwise.
 *
 * Note:	On input, cp must point to the end of any addresses in
 *		the rt_msghdr_t structure.
 */
int
tsol_rtsa_init(rt_msghdr_t *rtm, tsol_rtsecattr_t *sp, caddr_t cp)
{
	uint_t	sacnt;
	int	err;
	caddr_t	lim;
	tsol_rtsecattr_t *tp;

	ASSERT((cp >= (caddr_t)&rtm[1]) && sp != NULL);

	/*
	 * In theory, we could accept as many security attributes configured
	 * per route destination.  However, the current design is limited
	 * such that at most only one set security attributes is allowed to
	 * be associated with a prefix IRE.  We therefore assert for now.
	 */
	/* LINTED */
	ASSERT(TSOL_RTSA_REQUEST_MAX == 1);

	sp->rtsa_cnt = 0;
	lim = (caddr_t)rtm + rtm->rtm_msglen;
	ASSERT(cp <= lim);

	if ((lim - cp) < sizeof (rtm_ext_t) ||
	    ((rtm_ext_t *)cp)->rtmex_type != RTMEX_GATEWAY_SECATTR)
		return (0);

	if (((rtm_ext_t *)cp)->rtmex_len < sizeof (tsol_rtsecattr_t))
		return (EINVAL);

	cp += sizeof (rtm_ext_t);

	if ((lim - cp) < sizeof (*tp) ||
	    (tp = (tsol_rtsecattr_t *)cp, (sacnt = tp->rtsa_cnt) == 0) ||
	    (lim - cp) < TSOL_RTSECATTR_SIZE(sacnt))
		return (EINVAL);

	/*
	 * Trying to add route security attributes when system
	 * labeling service is not available, or when user supllies
	 * more than the maximum number of security attributes
	 * allowed per request.
	 */
	if ((sacnt > 0 && !is_system_labeled()) ||
	    sacnt > TSOL_RTSA_REQUEST_MAX)
		return (EINVAL);

	/* Ensure valid credentials */
	if ((err = rtsa_validate(&((tsol_rtsecattr_t *)cp)->
	    rtsa_attr[0])) != 0) {
		cp += sizeof (*sp);
		return (err);
	}

	bcopy(cp, sp, sizeof (*sp));
	cp += sizeof (*sp);
	return (0);
}

int
tsol_ire_init_gwattr(ire_t *ire, uchar_t ipversion, tsol_gc_t *gc,
    tsol_gcgrp_t *gcgrp)
{
	tsol_ire_gw_secattr_t *attrp;
	boolean_t exists = B_FALSE;
	in_addr_t ga_addr4;
	void *paddr = NULL;

	ASSERT(ire != NULL);

	/*
	 * The only time that attrp can be NULL is when this routine is
	 * called for the first time during the creation/initialization
	 * of the corresponding IRE.  It will only get cleared when the
	 * IRE is deleted.
	 */
	if ((attrp = ire->ire_gw_secattr) == NULL) {
		attrp = ire_gw_secattr_alloc(KM_NOSLEEP);
		if (attrp == NULL)
			return (ENOMEM);
		ire->ire_gw_secattr = attrp;
	} else {
		exists = B_TRUE;
		mutex_enter(&attrp->igsa_lock);

		if (attrp->igsa_rhc != NULL) {
			TNRHC_RELE(attrp->igsa_rhc);
			attrp->igsa_rhc = NULL;
		}

		if (attrp->igsa_gc != NULL)
			GC_REFRELE(attrp->igsa_gc);
		if (attrp->igsa_gcgrp != NULL)
			GCGRP_REFRELE(attrp->igsa_gcgrp);
	}
	ASSERT(!exists || MUTEX_HELD(&attrp->igsa_lock));

	/*
	 * References already held by caller and we keep them;
	 * note that both gc and gcgrp may be set to NULL to
	 * clear out igsa_gc and igsa_gcgrp, respectively.
	 */
	attrp->igsa_gc = gc;
	attrp->igsa_gcgrp = gcgrp;

	if (gcgrp == NULL && gc != NULL) {
		gcgrp = gc->gc_grp;
		ASSERT(gcgrp != NULL);
	}

	/*
	 * Intialize the template for gateway; we use the gateway's
	 * address found in either the passed in gateway credential
	 * or group pointer, or the ire_gateway_addr{_v6} field.
	 */
	if (gcgrp != NULL) {
		tsol_gcgrp_addr_t *ga = &gcgrp->gcgrp_addr;

		/*
		 * Caller is holding a reference, and that we don't
		 * need to hold any lock to access the address.
		 */
		if (ipversion == IPV4_VERSION) {
			ASSERT(ga->ga_af == AF_INET);
			IN6_V4MAPPED_TO_IPADDR(&ga->ga_addr, ga_addr4);
			paddr = &ga_addr4;
		} else {
			ASSERT(ga->ga_af == AF_INET6);
			paddr = &ga->ga_addr;
		}
	} else if (ipversion == IPV6_VERSION &&
	    !IN6_IS_ADDR_UNSPECIFIED(&ire->ire_gateway_addr_v6)) {
		paddr = &ire->ire_gateway_addr_v6;
	} else if (ipversion == IPV4_VERSION &&
	    ire->ire_gateway_addr != INADDR_ANY) {
		paddr = &ire->ire_gateway_addr;
	}

	/*
	 * Lookup the gateway template; note that we could get an internal
	 * template here, which we cache anyway.  During IRE matching, we'll
	 * try to update this gateway template cache and hopefully get a
	 * real one.
	 */
	if (paddr != NULL) {
		attrp->igsa_rhc = find_rhc(paddr, ipversion, B_FALSE);
	}

	if (exists)
		mutex_exit(&attrp->igsa_lock);

	return (0);
}

/*
 * This function figures the type of MLP that we'll be using based on the
 * address that the user is binding and the zone.  If the address is
 * unspecified, then we're looking at both private and shared.  If it's one
 * of the zone's private addresses, then it's private only.  If it's one
 * of the global addresses, then it's shared only.
 *
 * If we can't figure out what it is, then return mlptSingle.  That's actually
 * an error case.
 *
 * The callers are assume to pass in zone->zone_id and not the zoneid that
 * is stored in a conn_t (since the latter will be GLOBAL_ZONEID in an
 * exclusive stack zone).
 */
mlp_type_t
tsol_mlp_addr_type(zoneid_t zoneid, uchar_t version, const void *addr,
    ip_stack_t *ipst)
{
	in_addr_t in4;
	ire_t *ire;
	ipif_t *ipif;
	zoneid_t addrzone;
	zoneid_t ip_zoneid;

	ASSERT(addr != NULL);

	/*
	 * For exclusive stacks we set the zoneid to zero
	 * to operate as if in the global zone for IRE and conn_t comparisons.
	 */
	if (ipst->ips_netstack->netstack_stackid != GLOBAL_NETSTACKID)
		ip_zoneid = GLOBAL_ZONEID;
	else
		ip_zoneid = zoneid;

	if (version == IPV6_VERSION &&
	    IN6_IS_ADDR_V4MAPPED((const in6_addr_t *)addr)) {
		IN6_V4MAPPED_TO_IPADDR((const in6_addr_t *)addr, in4);
		addr = &in4;
		version = IPV4_VERSION;
	}

	if (version == IPV4_VERSION) {
		in4 = *(const in_addr_t *)addr;
		if (in4 == INADDR_ANY) {
			return (mlptBoth);
		}
		ire = ire_cache_lookup(in4, ip_zoneid, NULL, ipst);
	} else {
		if (IN6_IS_ADDR_UNSPECIFIED((const in6_addr_t *)addr)) {
			return (mlptBoth);
		}
		ire = ire_cache_lookup_v6(addr, ip_zoneid, NULL, ipst);
	}
	/*
	 * If we can't find the IRE, then we have to behave exactly like
	 * ip_bind_laddr{,_v6}.  That means looking up the IPIF so that users
	 * can bind to addresses on "down" interfaces.
	 *
	 * If we can't find that either, then the bind is going to fail, so
	 * just give up.  Note that there's a miniscule chance that the address
	 * is in transition, but we don't bother handling that.
	 */
	if (ire == NULL) {
		if (version == IPV4_VERSION)
			ipif = ipif_lookup_addr(*(const in_addr_t *)addr, NULL,
			    ip_zoneid, NULL, NULL, NULL, NULL, ipst);
		else
			ipif = ipif_lookup_addr_v6((const in6_addr_t *)addr,
			    NULL, ip_zoneid, NULL, NULL, NULL, NULL, ipst);
		if (ipif == NULL) {
			return (mlptSingle);
		}
		addrzone = ipif->ipif_zoneid;
		ipif_refrele(ipif);
	} else {
		addrzone = ire->ire_zoneid;
		ire_refrele(ire);
	}
	return (addrzone == ALL_ZONES ? mlptShared : mlptPrivate);
}

/*
 * Since we are configuring local interfaces, and we know trusted
 * extension CDE requires local interfaces to be cipso host type in
 * order to function correctly, we'll associate a cipso template
 * to each local interface and let the interface come up.  Configuring
 * a local interface to be "unlabeled" host type is a configuration error.
 * We'll override that error and make the interface host type to be cipso
 * here.
 *
 * The code is optimized for the usual "success" case and unwinds things on
 * error.  We don't want to go to the trouble and expense of formatting the
 * interface name for the usual case where everything is configured correctly.
 */
boolean_t
tsol_check_interface_address(const ipif_t *ipif)
{
	tsol_tpc_t *tp;
	char addrbuf[INET6_ADDRSTRLEN];
	int af;
	const void *addr;
	zone_t *zone;
	ts_label_t *plabel;
	const bslabel_t *label;
	char ifbuf[LIFNAMSIZ + 10];
	const char *ifname;
	boolean_t retval;
	tsol_rhent_t rhent;
	netstack_t *ns = ipif->ipif_ill->ill_ipst->ips_netstack;

	if (IN6_IS_ADDR_V4MAPPED(&ipif->ipif_v6lcl_addr)) {
		af = AF_INET;
		addr = &V4_PART_OF_V6(ipif->ipif_v6lcl_addr);
	} else {
		af = AF_INET6;
		addr = &ipif->ipif_v6lcl_addr;
	}

	tp = find_tpc(&ipif->ipif_v6lcl_addr, IPV6_VERSION, B_FALSE);

	/* assumes that ALL_ZONES implies that there is no exclusive stack */
	if (ipif->ipif_zoneid == ALL_ZONES) {
		zone = NULL;
	} else if (ns->netstack_stackid == GLOBAL_NETSTACKID) {
		/* Shared stack case */
		zone = zone_find_by_id(ipif->ipif_zoneid);
	} else {
		/* Exclusive stack case */
		zone = zone_find_by_id(crgetzoneid(ipif->ipif_ill->ill_credp));
	}
	if (zone != NULL) {
		plabel = zone->zone_slabel;
		ASSERT(plabel != NULL);
		label = label2bslabel(plabel);
	}

	/*
	 * If it's CIPSO and an all-zones address, then we're done.
	 * If it's a CIPSO zone specific address, the zone's label
	 * must be in the range or set specified in the template.
	 * When the remote host entry is missing or the template
	 * type is incorrect for this interface, we create a
	 * CIPSO host entry in kernel and allow the interface to be
	 * brought up as CIPSO type.
	 */
	if (tp != NULL && (
	    /* The all-zones case */
	    (tp->tpc_tp.host_type == SUN_CIPSO &&
	    tp->tpc_tp.tp_doi == default_doi &&
	    ipif->ipif_zoneid == ALL_ZONES) ||
	    /* The local-zone case */
	    (zone != NULL && plabel->tsl_doi == tp->tpc_tp.tp_doi &&
	    ((tp->tpc_tp.host_type == SUN_CIPSO &&
	    (_blinrange(label, &tp->tpc_tp.tp_sl_range_cipso) ||
	    blinlset(label, tp->tpc_tp.tp_sl_set_cipso))))))) {
		if (zone != NULL)
			zone_rele(zone);
		TPC_RELE(tp);
		return (B_TRUE);
	}

	ifname = ipif->ipif_ill->ill_name;
	if (ipif->ipif_id != 0) {
		(void) snprintf(ifbuf, sizeof (ifbuf), "%s:%u", ifname,
		    ipif->ipif_id);
		ifname = ifbuf;
	}
	(void) inet_ntop(af, addr, addrbuf, sizeof (addrbuf));

	if (tp == NULL) {
		cmn_err(CE_NOTE, "template entry for %s missing. Default to "
		    "CIPSO type for %s", ifname, addrbuf);
		retval = B_TRUE;
	} else if (tp->tpc_tp.host_type == UNLABELED) {
		cmn_err(CE_NOTE, "template type for %s incorrectly configured. "
		    "Change to CIPSO type for %s", ifname, addrbuf);
		retval = B_TRUE;
	} else if (ipif->ipif_zoneid == ALL_ZONES) {
		if (tp->tpc_tp.host_type != SUN_CIPSO) {
			cmn_err(CE_NOTE, "%s failed: %s isn't set to CIPSO for "
			    "all-zones. Converted to CIPSO.", ifname, addrbuf);
			retval = B_TRUE;
		} else {
			cmn_err(CE_NOTE, "%s failed: %s has wrong DOI %d "
			    "instead of %d", ifname, addrbuf,
			    tp->tpc_tp.tp_doi, default_doi);
			retval = B_FALSE;
		}
	} else if (zone == NULL) {
		cmn_err(CE_NOTE, "%s failed: zoneid %d unknown",
		    ifname, ipif->ipif_zoneid);
		retval = B_FALSE;
	} else if (plabel->tsl_doi != tp->tpc_tp.tp_doi) {
		cmn_err(CE_NOTE, "%s failed: zone %s has DOI %d but %s has "
		    "DOI %d", ifname, zone->zone_name, plabel->tsl_doi,
		    addrbuf, tp->tpc_tp.tp_doi);
		retval = B_FALSE;
	} else {
		cmn_err(CE_NOTE, "%s failed: zone %s label incompatible with "
		    "%s", ifname, zone->zone_name, addrbuf);
		tsol_print_label(label, "zone label");
		retval = B_FALSE;
	}

	if (zone != NULL)
		zone_rele(zone);
	if (tp != NULL)
		TPC_RELE(tp);
	if (retval) {
		/*
		 * we've corrected a config error and let the interface
		 * come up as cipso. Need to insert an rhent.
		 */
		if ((rhent.rh_address.ta_family = af) == AF_INET) {
			rhent.rh_prefix = 32;
			rhent.rh_address.ta_addr_v4 = *(struct in_addr *)addr;
		} else {
			rhent.rh_prefix = 128;
			rhent.rh_address.ta_addr_v6 = *(in6_addr_t *)addr;
		}
		(void) strcpy(rhent.rh_template, "cipso");
		if (tnrh_load(&rhent) != 0) {
			cmn_err(CE_NOTE, "%s failed: Cannot insert CIPSO "
			    "template for local addr %s", ifname, addrbuf);
			retval = B_FALSE;
		}
	}
	return (retval);
}
