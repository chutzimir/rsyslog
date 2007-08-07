/* action.h
 * Header file for the action object
 *
 * File begun on 2007-08-06 by RGerhards (extracted from syslogd.c)
 *
 * Copyright 2007 Rainer Gerhards and Adiscon GmbH.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#ifndef	ACTION_H_INCLUDED
#define	ACTION_H_INCLUDED 1

#include "syslogd-types.h"

/* the following struct defines the action object data structure
 */
struct action_s {
	time_t	f_time;		/* time this was last written */
	int	bExecWhenPrevSusp;/* execute only when previous action is suspended? */
	short	bEnabled;	/* is the related action enabled (1) or disabled (0)? */
	short	bSuspended;	/* is the related action temporarily suspended? */
	time_t	ttResumeRtry;	/* when is it time to retry the resume? */
	int	iNbrResRtry;	/* number of retries since last suspend */
	struct moduleInfo *pMod;/* pointer to output module handling this selector */
	void	*pModData;	/* pointer to module data - contents is module-specific */
	int	f_ReduceRepeated;/* reduce repeated lines 0 - no, 1 - yes */
	int	f_prevcount;	/* repetition cnt of prevline */
	int	f_repeatcount;	/* number of "repeated" msgs */
	int	iNumTpls;	/* number of array entries for template element below */
	struct template **ppTpl;/* array of template to use - strings must be passed to doAction
				 * in this order. */

	uchar **ppMsgs;		/* array of message pointers for doAction */
	struct msg* f_pMsg;	/* pointer to the message (this will replace the other vars with msg
				 * content later). This is preserved after the message has been
				 * processed - it is also used to detect duplicates.
				 */
};
typedef struct action_s action_t;


/* function prototypes
 */
rsRetVal actionConstruct(action_t **ppThis);
rsRetVal actionDestruct(action_t *pThis);
rsRetVal actionTryResume(action_t *pThis);
rsRetVal actionSuspend(action_t *pThis);
rsRetVal actionDbgPrint(action_t *pThis);

#if 1
#define actionIsSuspended(pThis) ((pThis)->bSuspended == 1)
#else
/* The function is a debugging aid */
inline int actionIsSuspended(action_t *pThis)
{
	int i;
	i =  pThis->bSuspended == 1;
	dprintf("in IsSuspend(), returns %d\n", i);
	return i;
}
#endif

#endif /* #ifndef ACTION_H_INCLUDED */
/*
 * vi:set ai:
 */