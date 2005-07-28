/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/stream.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#define	_SUN_TPI_VERSION 2
#include <sys/tihdr.h>
#include <sys/stropts.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <sys/policy.h>

#include <netinet/in.h>
#include <netinet/ip6.h>

#include <inet/common.h>
#include <inet/ip.h>
#include <inet/ip6.h>
#include <inet/ipclassifier.h>
#include "sctp_impl.h"
#include "sctp_asconf.h"
#include "sctp_addr.h"

uint_t	sctp_next_port_to_try;

/*
 * Returns 0 on success, EACCES on permission failure.
 */
static int
sctp_select_port(sctp_t *sctp, in_port_t *requested_port, int *user_specified)
{
	/*
	 * Get a valid port (within the anonymous range and should not
	 * be a privileged one) to use if the user has not given a port.
	 * If multiple threads are here, they may all start with
	 * with the same initial port. But, it should be fine as long as
	 * sctp_bindi will ensure that no two threads will be assigned
	 * the same port.
	 */
	if (*requested_port == 0) {
		*requested_port = sctp_update_next_port(sctp_next_port_to_try);
		*user_specified = 0;
	} else {
		int i;
		boolean_t priv = B_FALSE;

		/*
		 * If the requested_port is in the well-known privileged range,
		 * verify that the stream was opened by a privileged user.
		 * Note: No locks are held when inspecting sctp_g_*epriv_ports
		 * but instead the code relies on:
		 * - the fact that the address of the array and its size never
		 *   changes
		 * - the atomic assignment of the elements of the array
		 */
		if (*requested_port < sctp_smallest_nonpriv_port) {
			priv = B_TRUE;
		} else {
			for (i = 0; i < sctp_g_num_epriv_ports; i++) {
				if (*requested_port == sctp_g_epriv_ports[i]) {
					priv = B_TRUE;
					break;
				}
			}
		}
		if (priv) {
			/*
			 * sctp_bind() should take a cred_t argument so that
			 * we can use it here.
			 */
			if (secpolicy_net_privaddr(sctp->sctp_credp,
			    *requested_port) != 0) {
				dprint(1,
				    ("sctp_bind(x): no prive for port %d",
				    *requested_port));
				return (TACCES);
			}
		}
		*user_specified = 1;
	}

	return (0);
}

int
sctp_listen(sctp_t *sctp)
{
	sctp_tf_t	*tf;

	RUN_SCTP(sctp);
	/*
	 * TCP handles listen() increasing the backlog, need to check
	 * if it should be handled here too - VENU.
	 */
	if (sctp->sctp_state > SCTPS_BOUND) {
		WAKE_SCTP(sctp);
		return (EINVAL);
	}

	/* Do an anonymous bind for unbound socket doing listen(). */
	if (sctp->sctp_nsaddrs == 0) {
		struct sockaddr_storage ss;
		int ret;

		bzero(&ss, sizeof (ss));
		ss.ss_family = sctp->sctp_family;

		WAKE_SCTP(sctp);
		if ((ret = sctp_bind(sctp, (struct sockaddr *)&ss,
			sizeof (ss))) != 0)
			return (ret);
		RUN_SCTP(sctp)
	}

	sctp->sctp_state = SCTPS_LISTEN;
	(void) random_get_pseudo_bytes(sctp->sctp_secret, SCTP_SECRET_LEN);
	sctp->sctp_last_secret_update = lbolt64;
	bzero(sctp->sctp_old_secret, SCTP_SECRET_LEN);
	tf = &sctp_listen_fanout[SCTP_LISTEN_HASH(ntohs(sctp->sctp_lport))];
	sctp_listen_hash_insert(tf, sctp);

	WAKE_SCTP(sctp);
	return (0);
}

/*
 * Bind the sctp_t to a sockaddr, which includes an address and other
 * information, such as port or flowinfo.
 */
int
sctp_bind(sctp_t *sctp, struct sockaddr *sa, socklen_t len)
{
	int		user_specified;
	boolean_t	bind_to_req_port_only;
	in_port_t	requested_port;
	in_port_t	allocated_port;
	int		err = 0;

	ASSERT(sctp != NULL);
	ASSERT(sa);

	RUN_SCTP(sctp);

	if (sctp->sctp_state > SCTPS_BOUND) {
		err = EINVAL;
		goto done;
	}

	switch (sa->sa_family) {
	case AF_INET:
		if (len < sizeof (struct sockaddr_in) ||
		    sctp->sctp_family == AF_INET6) {
			err = EINVAL;
			goto done;
		}
		requested_port = ntohs(((struct sockaddr_in *)sa)->sin_port);
		break;
	case AF_INET6:
		if (len < sizeof (struct sockaddr_in6) ||
		    sctp->sctp_family == AF_INET) {
			err = EINVAL;
			goto done;
		}
		requested_port = ntohs(((struct sockaddr_in6 *)sa)->sin6_port);
		/* Set the flowinfo. */
		sctp->sctp_ip6h->ip6_vcf =
		    (IPV6_DEFAULT_VERS_AND_FLOW & IPV6_VERS_AND_FLOW_MASK) |
		    (((struct sockaddr_in6 *)sa)->sin6_flowinfo &
		    ~IPV6_VERS_AND_FLOW_MASK);
		break;
	default:
		err = EAFNOSUPPORT;
		goto done;
	}
	bind_to_req_port_only = requested_port == 0 ? B_FALSE : B_TRUE;

	if (sctp_select_port(sctp, &requested_port, &user_specified) != 0) {
		err = EPERM;
		goto done;
	}

	if ((err = sctp_bind_add(sctp, sa, 1, B_TRUE)) != 0)
		goto done;

	allocated_port = sctp_bindi(sctp, requested_port,
	    bind_to_req_port_only, user_specified);
	if (allocated_port == 0) {
		sctp_free_saddrs(sctp);
		if (bind_to_req_port_only) {
			err = EADDRINUSE;
			goto done;
		} else {
			err = EADDRNOTAVAIL;
			goto done;
		}
	}
	ASSERT(sctp->sctp_state == SCTPS_BOUND);
done:
	WAKE_SCTP(sctp);
	return (err);
}

/*
 * Perform bind/unbind operation of a list of addresses on a sctp_t
 */
int
sctp_bindx(sctp_t *sctp, const void *addrs, int addrcnt, int bindop)
{
	ASSERT(sctp != NULL);
	ASSERT(addrs != NULL);
	ASSERT(addrcnt > 0);

	switch (bindop) {
	case SCTP_BINDX_ADD_ADDR:
		return (sctp_bind_add(sctp, addrs, addrcnt, B_FALSE));
	case SCTP_BINDX_REM_ADDR:
		return (sctp_bind_del(sctp, addrs, addrcnt, B_FALSE));
	default:
		return (EINVAL);
	}
}

/*
 * Add a list of addresses to a sctp_t.
 */
int
sctp_bind_add(sctp_t *sctp, const void *addrs, uint32_t addrcnt,
    boolean_t caller_hold_lock)
{
	int		err = 0;
	boolean_t	do_asconf = B_FALSE;

	if (!caller_hold_lock)
		RUN_SCTP(sctp);

	if (sctp->sctp_state > SCTPS_ESTABLISHED) {
		if (!caller_hold_lock)
			WAKE_SCTP(sctp);
		return (EINVAL);
	}

	if (sctp->sctp_state > SCTPS_LISTEN) {
		/*
		 * Let's do some checking here rather than undoing the
		 * add later (for these reasons).
		 */
		if (!sctp_addip_enabled || !sctp->sctp_understands_asconf ||
		    !sctp->sctp_understands_addip) {
			if (!caller_hold_lock)
				WAKE_SCTP(sctp);
			return (EINVAL);
		}
		do_asconf = B_TRUE;
	}
	err = sctp_valid_addr_list(sctp, addrs, addrcnt);
	if (err != 0) {
		if (!caller_hold_lock)
			WAKE_SCTP(sctp);
		return (err);
	}

	/* Need to send  ASCONF messages */
	if (do_asconf) {
		err = sctp_add_ip(sctp, addrs, addrcnt);
		if (err != 0) {
			sctp_del_saddr_list(sctp, addrs, addrcnt, B_FALSE);
			if (!caller_hold_lock)
				WAKE_SCTP(sctp);
			return (err);
		}
	}
	if (!caller_hold_lock)
		WAKE_SCTP(sctp);
	if (do_asconf)
		sctp_process_sendq(sctp);
	return (0);
}

/*
 * Remove one or more addresses bound to the sctp_t.
 */
int
sctp_bind_del(sctp_t *sctp, const void *addrs, uint32_t addrcnt,
    boolean_t caller_hold_lock)
{
	int		error = 0;
	boolean_t	do_asconf = B_FALSE;

	if (!caller_hold_lock)
		RUN_SCTP(sctp);

	if (sctp->sctp_state > SCTPS_ESTABLISHED) {
		if (!caller_hold_lock)
			WAKE_SCTP(sctp);
		return (EINVAL);
	}
	/*
	 * Fail the remove if we are beyond listen, but can't send this
	 * to the peer.
	 */
	if (sctp->sctp_state > SCTPS_LISTEN) {
		if (!sctp_addip_enabled || !sctp->sctp_understands_asconf ||
		    !sctp->sctp_understands_addip) {
			if (!caller_hold_lock)
				WAKE_SCTP(sctp);
			return (EINVAL);
		}
		do_asconf = B_TRUE;
	}

	/* Can't delete the last address nor all of the addresses */
	if (sctp->sctp_nsaddrs == 1 || addrcnt >= sctp->sctp_nsaddrs) {
		if (!caller_hold_lock)
			WAKE_SCTP(sctp);
		return (EINVAL);
	}

	error = sctp_del_ip(sctp, addrs, addrcnt);
	if (!caller_hold_lock)
		WAKE_SCTP(sctp);
	if (error == 0 && do_asconf)
		sctp_process_sendq(sctp);
	return (error);
}

/*
 * If the "bind_to_req_port_only" parameter is set, if the requested port
 * number is available, return it, If not return 0
 *
 * If "bind_to_req_port_only" parameter is not set and
 * If the requested port number is available, return it.  If not, return
 * the first anonymous port we happen across.  If no anonymous ports are
 * available, return 0. addr is the requested local address, if any.
 *
 * In either case, when succeeding update the sctp_t to record the port number
 * and insert it in the bind hash table.
 */
in_port_t
sctp_bindi(sctp_t *sctp, in_port_t port, int bind_to_req_port_only,
    int user_specified)
{
	/* number of times we have run around the loop */
	int count = 0;
	/* maximum number of times to run around the loop */
	int loopmax;
	zoneid_t zoneid = sctp->sctp_zoneid;

	/*
	 * Lookup for free addresses is done in a loop and "loopmax"
	 * influences how long we spin in the loop
	 */
	if (bind_to_req_port_only) {
		/*
		 * If the requested port is busy, don't bother to look
		 * for a new one. Setting loop maximum count to 1 has
		 * that effect.
		 */
		loopmax = 1;
	} else {
		/*
		 * If the requested port is busy, look for a free one
		 * in the anonymous port range.
		 * Set loopmax appropriately so that one does not look
		 * forever in the case all of the anonymous ports are in use.
		 */
		loopmax = (sctp_largest_anon_port -
		    sctp_smallest_anon_port + 1);
	}
	do {
		uint16_t	lport;
		sctp_tf_t	*tbf;
		sctp_t		*lsctp;
		int		addrcmp;

		lport = htons(port);

		/*
		 * Ensure that the sctp_t is not currently in the bind hash.
		 * Hold the lock on the hash bucket to ensure that
		 * the duplicate check plus the insertion is an atomic
		 * operation.
		 *
		 * This function does an inline lookup on the bind hash list
		 * Make sure that we access only members of sctp_t
		 * and that we don't look at sctp_sctp, since we are not
		 * doing a SCTPB_REFHOLD. For more details please see the notes
		 * in sctp_compress()
		 */
		sctp_bind_hash_remove(sctp);
		tbf = &sctp_bind_fanout[SCTP_BIND_HASH(port)];
		mutex_enter(&tbf->tf_lock);
		for (lsctp = tbf->tf_sctp; lsctp != NULL;
		    lsctp = lsctp->sctp_bind_hash) {

			if (lport != lsctp->sctp_lport ||
			    lsctp->sctp_zoneid != zoneid ||
			    lsctp->sctp_state < SCTPS_BOUND)
				continue;

			addrcmp = sctp_compare_saddrs(sctp, lsctp);
			if (addrcmp != SCTP_ADDR_DISJOINT) {
				if (!sctp->sctp_reuseaddr) {
					/* in use */
					break;
				} else if (lsctp->sctp_state == SCTPS_BOUND ||
				    lsctp->sctp_state == SCTPS_LISTEN) {
					/*
					 * socket option SO_REUSEADDR is set
					 * on the binding sctp_t.
					 *
					 * We have found a match of IP source
					 * address and source port, which is
					 * refused regardless of the
					 * SO_REUSEADDR setting, so we break.
					 */
					break;
				}
			}
		}
		if (lsctp != NULL) {
			/* The port number is busy */
			mutex_exit(&tbf->tf_lock);
		} else {
			/*
			 * This port is ours. Insert in fanout and mark as
			 * bound to prevent others from getting the port
			 * number.
			 */
			sctp->sctp_state = SCTPS_BOUND;
			sctp->sctp_lport = lport;
			sctp->sctp_sctph->sh_sport = sctp->sctp_lport;

			ASSERT(&sctp_bind_fanout[SCTP_BIND_HASH(port)] == tbf);
			sctp_bind_hash_insert(tbf, sctp, 1);

			mutex_exit(&tbf->tf_lock);

			/*
			 * We don't want sctp_next_port_to_try to "inherit"
			 * a port number supplied by the user in a bind.
			 */
			if (user_specified != 0)
				return (port);

			/*
			 * This is the only place where sctp_next_port_to_try
			 * is updated. After the update, it may or may not
			 * be in the valid range.
			 */
			sctp_next_port_to_try = port + 1;
			return (port);
		}

		if ((count == 0) && (user_specified)) {
			/*
			 * We may have to return an anonymous port. So
			 * get one to start with.
			 */
			port = sctp_update_next_port(sctp_next_port_to_try);
			user_specified = 0;
		} else {
			port = sctp_update_next_port(port + 1);
		}

		/*
		 * Don't let this loop run forever in the case where
		 * all of the anonymous ports are in use.
		 */
	} while (++count < loopmax);
	return (0);
}

/*
 * Don't let port fall into the privileged range.
 * Since the extra privileged ports can be arbitrary we also
 * ensure that we exclude those from consideration.
 * sctp_g_epriv_ports is not sorted thus we loop over it until
 * there are no changes.
 *
 * Note: No locks are held when inspecting sctp_g_*epriv_ports
 * but instead the code relies on:
 * - the fact that the address of the array and its size never changes
 * - the atomic assignment of the elements of the array
 */
in_port_t
sctp_update_next_port(in_port_t port)
{
	int i;

retry:
	if (port < sctp_smallest_anon_port || port > sctp_largest_anon_port)
		port = sctp_smallest_anon_port;

	if (port < sctp_smallest_nonpriv_port)
		port = sctp_smallest_nonpriv_port;

	for (i = 0; i < sctp_g_num_epriv_ports; i++) {
		if (port == sctp_g_epriv_ports[i]) {
			port++;
			/*
			 * Make sure whether the port is in the
			 * valid range.
			 *
			 * XXX Note that if sctp_g_epriv_ports contains
			 * all the anonymous ports this will be an
			 * infinite loop.
			 */
			goto retry;
		}
	}
	return (port);
}
