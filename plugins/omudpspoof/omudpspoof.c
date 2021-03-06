/* omudpspoof.c
 *
 * This is a udp-based output module that support spoofing.
 *
 * This file builds on UDP spoofing code contributed by 
 * David Lang <david@lang.hm>. I then created a "real" rsyslog module
 * out of that code and omfwd. I decided to make it a separate module because
 * omfwd already mixes up too many things (TCP & UDP & a differnt modes,
 * this has historic reasons), it would not be a good idea to also add
 * spoofing to it. And, looking at the requirements, there is little in 
 * common between omfwd and this module.
 *
 * Note: I have briefly checked libnet source code and I somewhat have the feeling
 * that under some circumstances we may get into trouble with the lib. For
 * example, it registers an atexit() handler, which should not play nicely
 * with our dynamically loaded modules. Anyhow, I refrain from looking deeper
 * at libnet code, especially as testing does not show any real issues. If some
 * occur, it may be easier to modify libnet for dynamic load environments than
 * using a work-around (as a side not, libnet looks somewhat unmaintained, the CVS
 * I can see on sourceforge dates has no updates done less than 7 years ago).
 * On the other hand, it looks like libnet is thread safe (at least is appropriately
 * compiled, which I hope the standard packages are). So I do not guard calls to
 * it with my own mutex calls.
 * rgerhards, 2009-07-10
 *
 * Copyright 2009 David Lang (spoofing code)
 * Copyright 2009-2012 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fnmatch.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#ifdef USE_NETZIP
#include <zlib.h>
#endif
#include "conf.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "net.h"
#include "template.h"
#include "msg.h"
#include "cfsysline.h"
#include "module-template.h"
#include "glbl.h"
#include "errmsg.h"
#include "dirty.h"
#include "unicode-helper.h"
#include "debug.h"


#include <libnet.h>
#define _BSD_SOURCE 1
#define __BSD_SOURCE 1
#define __FAVOR_BSD 1


MODULE_TYPE_OUTPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("omudpspoof")

/* internal structures
 */
DEF_OMOD_STATIC_DATA
DEFobjCurrIf(errmsg)
DEFobjCurrIf(glbl)
DEFobjCurrIf(net)

typedef struct _instanceData {
	uchar	*host;
	uchar	*port;
	int	*pSockArray;		/* sockets to use for UDP */
	int	compressionLevel;	/* 0 - no compression, else level for zlib */
	struct addrinfo *f_addr;
	u_short sourcePort;
	u_short sourcePortStart;	/* for sorce port iteration */
	u_short sourcePortEnd;
} instanceData;

#define DFLT_SOURCE_PORT_START 32000
#define DFLT_SOURCE_PORT_END   42000

typedef struct configSettings_s {
	uchar *tplName; /* name of the default template to use */
	uchar *pszSourceNameTemplate; /* name of the template containing the spoofing address */
	uchar *pszTargetHost;
	uchar *pszTargetPort;
	int iSourcePortStart;
	int iSourcePortEnd;
} configSettings_t;
static configSettings_t cs;

/* module-global parameters */
static struct cnfparamdescr modpdescr[] = {
	{ "template", eCmdHdlrGetWord, 0 },
};
static struct cnfparamblk modpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(modpdescr)/sizeof(struct cnfparamdescr),
	  modpdescr
	};

struct modConfData_s {
	rsconf_t *pConf;	/* our overall config object */
	uchar 	*tplName;	/* default template */
};

static modConfData_t *loadModConf = NULL;/* modConf ptr to use for the current load process */
static modConfData_t *runModConf = NULL;/* modConf ptr to use for the current exec process */



BEGINinitConfVars		/* (re)set config variables to default values */
CODESTARTinitConfVars 
	cs.tplName = NULL;
	cs.pszSourceNameTemplate = NULL;
	cs.pszTargetHost = NULL;
	cs.pszTargetPort = NULL;
	cs.iSourcePortStart = DFLT_SOURCE_PORT_START;
	cs.iSourcePortEnd = DFLT_SOURCE_PORT_END;
ENDinitConfVars


/* add some variables needed for libnet */
libnet_t *libnet_handle;
char errbuf[LIBNET_ERRBUF_SIZE];
pthread_mutex_t mutLibnet;

/* forward definitions */
static rsRetVal doTryResume(instanceData *pData);


/* this function gets the default template. It coordinates action between
 * old-style and new-style configuration parts.
 */
static inline uchar*
getDfltTpl(void)
{
	if(loadModConf != NULL && loadModConf->tplName != NULL)
		return loadModConf->tplName;
	else if(cs.tplName == NULL)
		return (uchar*)"RSYSLOG_FileFormat";
	else
		return cs.tplName;
}


/* set the default template to be used
 * This is a module-global parameter, and as such needs special handling. It needs to
 * be coordinated with values set via the v2 config system (rsyslog v6+). What we do
 * is we do not permit this directive after the v2 config system has been used to set
 * the parameter.
 */
rsRetVal
setLegacyDfltTpl(void __attribute__((unused)) *pVal, uchar* newVal)
{
	DEFiRet;

	if(loadModConf != NULL && loadModConf->tplName != NULL) {
		free(newVal);
		errmsg.LogError(0, RS_RET_ERR, "omudpspoof default template already set via module "
			"global parameter - can no longer be changed");
		ABORT_FINALIZE(RS_RET_ERR);
	}
	free(cs.tplName);
	cs.tplName = newVal;
finalize_it:
	RETiRet;
}

/* Close the UDP sockets.
 * rgerhards, 2009-05-29
 */
static rsRetVal
closeUDPSockets(instanceData *pData)
{
	DEFiRet;
	assert(pData != NULL);
	if(pData->pSockArray != NULL) {
		net.closeUDPListenSockets(pData->pSockArray);
		pData->pSockArray = NULL;
		freeaddrinfo(pData->f_addr);
		pData->f_addr = NULL;
	}
	RETiRet;
}


/* get the syslog forward port
 * We may change the implementation to try to lookup the port
 * if it is unspecified. So far, we use the IANA default auf 514.
 * rgerhards, 2007-06-28
 */
static inline uchar *getFwdPt(instanceData *pData)
{
	return (pData->port == NULL) ? UCHAR_CONSTANT("514") : pData->port;
}


BEGINbeginCnfLoad
CODESTARTbeginCnfLoad
	loadModConf = pModConf;
	pModConf->pConf = pConf;
	pModConf->tplName = NULL;
ENDbeginCnfLoad

BEGINsetModCnf
	struct cnfparamvals *pvals = NULL;
	int i;
CODESTARTsetModCnf
	pvals = nvlstGetParams(lst, &modpblk, NULL);
	if(pvals == NULL) {
		errmsg.LogError(0, RS_RET_MISSING_CNFPARAMS, "error processing module "
				"config parameters [module(...)]");
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	if(Debug) {
		dbgprintf("module (global) param blk for omudpspoof:\n");
		cnfparamsPrint(&modpblk, pvals);
	}

	for(i = 0 ; i < modpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(modpblk.descr[i].name, "template")) {
			loadModConf->tplName = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
			if(cs.tplName != NULL) {
				errmsg.LogError(0, RS_RET_DUP_PARAM, "omudpspoof: warning: default template "
						"was already set via legacy directive - may lead to inconsistent "
						"results.");
			}
		} else {
			dbgprintf("omudpspoof: program error, non-handled "
			  "param '%s' in beginCnfLoad\n", modpblk.descr[i].name);
		}
	}
finalize_it:
	if(pvals != NULL)
		cnfparamvalsDestruct(pvals, &modpblk);
ENDsetModCnf

BEGINendCnfLoad
CODESTARTendCnfLoad
	loadModConf = NULL; /* done loading */
	/* free legacy config vars */
	free(cs.tplName);
	cs.tplName = NULL;
ENDendCnfLoad

BEGINcheckCnf
CODESTARTcheckCnf
ENDcheckCnf

BEGINactivateCnf
CODESTARTactivateCnf
	runModConf = pModConf;
ENDactivateCnf

BEGINfreeCnf
CODESTARTfreeCnf
	free(pModConf->tplName);
ENDfreeCnf


BEGINcreateInstance
CODESTARTcreateInstance
ENDcreateInstance


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINfreeInstance
CODESTARTfreeInstance
	/* final cleanup */
	closeUDPSockets(pData);
	free(pData->port);
	free(pData->host);
ENDfreeInstance


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	DBGPRINTF("%s", pData->host);
ENDdbgPrintInstInfo


/* Send a message via UDP
 * Note: libnet is not thread-safe, so we need to ensure that only one
 * instance ever is calling libnet code.
 * rgehards, 2007-12-20
 */
static inline rsRetVal
UDPSend(instanceData *pData, uchar *pszSourcename, char *msg, size_t len)
{
	struct addrinfo *r;
	int lsent = 0;
	int bSendSuccess;
	struct sockaddr_in *tempaddr,source_ip;
	libnet_ptag_t ip, ipo;
	libnet_ptag_t udp;
	sbool bNeedUnlock = 0;
	DEFiRet;

	if(pData->pSockArray == NULL) {
		CHKiRet(doTryResume(pData));
	}

	ip = ipo = udp = 0;
	if(pData->sourcePort++ >= pData->sourcePortEnd){
		pData->sourcePort = pData->sourcePortStart;
	}

	inet_pton(AF_INET, (char*)pszSourcename, &(source_ip.sin_addr));

	bSendSuccess = RSFALSE;
	d_pthread_mutex_lock(&mutLibnet);
	bNeedUnlock = 1;
	for (r = pData->f_addr; r; r = r->ai_next) {
		tempaddr = (struct sockaddr_in *)r->ai_addr;
		libnet_clear_packet(libnet_handle);
		/* note: libnet does need ports in host order NOT in network byte order! -- rgerhards, 2009-11-12 */
		udp = libnet_build_udp(
			ntohs(pData->sourcePort),/* source port */
			ntohs(tempaddr->sin_port),/* destination port */
			LIBNET_UDP_H + len,	/* packet length */
			0,			/* checksum */
			(u_char*)msg,		/* payload */
			len,			/* payload size */
			libnet_handle,		/* libnet handle */
			udp);			/* libnet id */
		if (udp == -1) {
			DBGPRINTF("Can't build UDP header: %s\n", libnet_geterror(libnet_handle));
		}

		ip = libnet_build_ipv4(
			LIBNET_IPV4_H +  len + LIBNET_UDP_H, /* length */
			0,				/* TOS */
			242,				/* IP ID */
			0,				/* IP Frag */
			64,				/* TTL */
			IPPROTO_UDP,			/* protocol */
			0,				/* checksum */
			source_ip.sin_addr.s_addr,
			tempaddr->sin_addr.s_addr,
			NULL,				/* payload */
			0,				/* payload size */
			libnet_handle,			/* libnet handle */
			ip);				/* libnet id */
		if (ip == -1) {
			DBGPRINTF("Can't build IP header: %s\n", libnet_geterror(libnet_handle));
		}

		/* Write it to the wire. */
		lsent = libnet_write(libnet_handle);
		if (lsent == -1) {
			DBGPRINTF("Write error: %s\n", libnet_geterror(libnet_handle));
		} else {
			bSendSuccess = RSTRUE;
			break;
		}
	}
	/* finished looping */
	if (bSendSuccess == RSFALSE) {
		DBGPRINTF("error forwarding via udp, suspending\n");
		iRet = RS_RET_SUSPENDED;
	}

finalize_it:
	if(bNeedUnlock) {
		d_pthread_mutex_unlock(&mutLibnet);
	}
	RETiRet;
}


/* try to resume connection if it is not ready
 * rgerhards, 2007-08-02
 */
static rsRetVal doTryResume(instanceData *pData)
{
	int iErr;
	struct addrinfo *res;
	struct addrinfo hints;
	DEFiRet;

	if(pData->pSockArray != NULL)
		FINALIZE;

	/* The remote address is not yet known and needs to be obtained */
	DBGPRINTF(" %s\n", pData->host);
	memset(&hints, 0, sizeof(hints));
	/* port must be numeric, because config file syntax requires this */
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_family = glbl.GetDefPFFamily();
	hints.ai_socktype = SOCK_DGRAM;
	if((iErr = (getaddrinfo((char*)pData->host, (char*)getFwdPt(pData), &hints, &res))) != 0) {
		DBGPRINTF("could not get addrinfo for hostname '%s':'%s': %d%s\n",
			  pData->host, getFwdPt(pData), iErr, gai_strerror(iErr));
		ABORT_FINALIZE(RS_RET_SUSPENDED);
	}
	DBGPRINTF("%s found, resuming.\n", pData->host);
	pData->f_addr = res;
	pData->pSockArray = net.create_udp_socket((uchar*)pData->host, NULL, 0);

finalize_it:
	if(iRet != RS_RET_OK) {
		if(pData->f_addr != NULL) {
			freeaddrinfo(pData->f_addr);
			pData->f_addr = NULL;
		}
		iRet = RS_RET_SUSPENDED;
	}

	RETiRet;
}


BEGINtryResume
CODESTARTtryResume
	iRet = doTryResume(pData);
ENDtryResume

BEGINdoAction
	char *psz; /* temporary buffering */
	register unsigned l;
	int iMaxLine;
CODESTARTdoAction
	CHKiRet(doTryResume(pData));

	iMaxLine = glbl.GetMaxLine();

	DBGPRINTF(" %s:%s/udpspoofs\n", pData->host, getFwdPt(pData));

	psz = (char*) ppString[0];
	l = strlen((char*) psz);
	if((int) l > iMaxLine)
		l = iMaxLine;

#	ifdef	USE_NETZIP
	/* Check if we should compress and, if so, do it. We also
	 * check if the message is large enough to justify compression.
	 * The smaller the message, the less likely is a gain in compression.
	 * To save CPU cycles, we do not try to compress very small messages.
	 * What "very small" means needs to be configured. Currently, it is
	 * hard-coded but this may be changed to a config parameter.
	 * rgerhards, 2006-11-30
	 */
	if(pData->compressionLevel && (l > CONF_MIN_SIZE_FOR_COMPRESS)) {
		Bytef *out;
		uLongf destLen = iMaxLine + iMaxLine/100 +12; /* recommended value from zlib doc */
		uLong srcLen = l;
		int ret;
		/* TODO: optimize malloc sequence? -- rgerhards, 2008-09-02 */
		CHKmalloc(out = (Bytef*) MALLOC(destLen));
		out[0] = 'z';
		out[1] = '\0';
		ret = compress2((Bytef*) out+1, &destLen, (Bytef*) psz,
				srcLen, pData->compressionLevel);
		DBGPRINTF("Compressing message, length was %d now %d, return state  %d.\n",
			l, (int) destLen, ret);
		if(ret != Z_OK) {
			/* if we fail, we complain, but only in debug mode
			 * Otherwise, we are silent. In any case, we ignore the
			 * failed compression and just sent the uncompressed
			 * data, which is still valid. So this is probably the
			 * best course of action.
			 * rgerhards, 2006-11-30
			 */
			DBGPRINTF("Compression failed, sending uncompressed message\n");
		} else if(destLen+1 < l) {
			/* only use compression if there is a gain in using it! */
			DBGPRINTF("there is gain in compression, so we do it\n");
			psz = (char*) out;
			l = destLen + 1; /* take care for the "z" at message start! */
		}
		++destLen;
	}
#	endif

	CHKiRet(UDPSend(pData, ppString[1], psz, l));

finalize_it:
ENDdoAction


BEGINparseSelectorAct
	uchar *sourceTpl;
CODESTARTparseSelectorAct
CODE_STD_STRING_REQUESTparseSelectorAct(2)
	/* first check if this config line is actually for us */
	if(strncmp((char*) p, ":omudpspoof:", sizeof(":omudpspoof:") - 1)) {
		ABORT_FINALIZE(RS_RET_CONFLINE_UNPROCESSED);
	}

	/* ok, if we reach this point, we have something for us */
	p += sizeof(":omudpspoof:") - 1; /* eat indicator sequence  (-1 because of '\0'!) */
	CHKiRet(createInstance(&pData));

	sourceTpl = (cs.pszSourceNameTemplate == NULL) ? UCHAR_CONSTANT("RSYSLOG_omudpspoofDfltSourceTpl")
						    : cs.pszSourceNameTemplate;

	if(cs.pszTargetHost == NULL) {
		errmsg.LogError(0, NO_ERRCODE, "No $ActionOMUDPSpoofTargetHost given, can not continue with this action.");
		ABORT_FINALIZE(RS_RET_HOST_NOT_SPECIFIED);
	}

	/* fill instance properties */
	CHKmalloc(pData->host = ustrdup(cs.pszTargetHost));
	if(cs.pszTargetPort == NULL)
		pData->port = NULL;
	else 
		CHKmalloc(pData->port = ustrdup(cs.pszTargetPort));
	CHKiRet(OMSRsetEntry(*ppOMSR, 1, ustrdup(sourceTpl), OMSR_NO_RQD_TPL_OPTS));
	pData->sourcePort = pData->sourcePortStart = cs.iSourcePortStart;
	pData->sourcePortEnd = cs.iSourcePortEnd;

	/* process template */
	CHKiRet(cflineParseTemplateName(&p, *ppOMSR, 0, OMSR_NO_RQD_TPL_OPTS,
		(cs.tplName == NULL) ? (uchar*)"RSYSLOG_TraditionalForwardFormat" : cs.tplName));

CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


/* a common function to free our configuration variables - used both on exit
 * and on $ResetConfig processing. -- rgerhards, 2008-05-16
 */
static void
freeConfigVars(void)
{
	free(cs.tplName);
	cs.tplName = NULL;
	free(cs.pszTargetHost);
	cs.pszTargetHost = NULL;
	free(cs.pszTargetPort);
	cs.pszTargetPort = NULL;
}


BEGINmodExit
CODESTARTmodExit
	/* destroy the libnet state needed for forged UDP sources */
	libnet_destroy(libnet_handle);
	pthread_mutex_destroy(&mutLibnet);
	/* release what we no longer need */
	objRelease(errmsg, CORE_COMPONENT);
	objRelease(glbl, CORE_COMPONENT);
	objRelease(net, LM_NET_FILENAME);
	freeConfigVars();
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
CODEqueryEtryPt_STD_CONF2_QUERIES
CODEqueryEtryPt_STD_CONF2_setModCnf_QUERIES
ENDqueryEtryPt


/* Reset config variables for this module to default values.
 * rgerhards, 2008-03-28
 */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	freeConfigVars();
	/* we now must reset all non-string values */
	cs.iSourcePortStart = DFLT_SOURCE_PORT_START;
	cs.iSourcePortEnd = DFLT_SOURCE_PORT_END;
	return RS_RET_OK;
}


BEGINmodInit()
CODESTARTmodInit
INITLegCnfVars
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(net,LM_NET_FILENAME));

	/* Initialize the libnet library.  Root priviledges are required.
	* this initializes a IPv4 socket to use for forging UDP packets.
	*/
	libnet_handle = libnet_init(
	    LIBNET_RAW4,                            /* injection type */
	    NULL,                                   /* network interface */
	    errbuf);                                /* errbuf */

	if(libnet_handle == NULL) {
		errmsg.LogError(0, NO_ERRCODE, "Error initializing libnet, can not continue ");
		ABORT_FINALIZE(RS_RET_ERR_LIBNET_INIT);
	}
	pthread_mutex_init(&mutLibnet, NULL);

	CHKiRet(regCfSysLineHdlr((uchar *)"actionomudpspoofdefaulttemplate", 0, eCmdHdlrGetWord, setLegacyDfltTpl, NULL, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"actionomudpspoofsourcenametemplate", 0, eCmdHdlrGetWord, NULL, &cs.pszSourceNameTemplate, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"actionomudpspooftargethost", 0, eCmdHdlrGetWord, NULL, &cs.pszTargetHost, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"actionomudpspooftargetport", 0, eCmdHdlrGetWord, NULL, &cs.pszTargetPort, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"actionomudpspoofsourceportstart", 0, eCmdHdlrInt, NULL, &cs.iSourcePortStart, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"actionomudpspoofsourceportend", 0, eCmdHdlrInt, NULL, &cs.iSourcePortEnd, NULL));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler, resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
ENDmodInit

/* vim:set ai:
 */
