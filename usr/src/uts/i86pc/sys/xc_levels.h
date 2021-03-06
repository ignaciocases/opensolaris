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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_XC_LEVELS_H
#define	_SYS_XC_LEVELS_H

#ifdef	__cplusplus
extern "C" {
#endif

/* PILs associated with cross calls */
#define	XC_CPUPOKE_PIL	11	/* poke to cause wakeup, no service function */
#define	XC_SYS_PIL	13	/* should be defined elsewhere */
#define	XC_HI_PIL	15	/* cross call with service function */

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_XC_LEVELS_H */
