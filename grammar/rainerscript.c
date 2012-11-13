/* rainerscript.c - routines to support RainerScript config language
 *
 * Module begun 2011-07-01 by Rainer Gerhards
 *
 * Copyright 2011 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of the rsyslog runtime library.
 *
 * The rsyslog runtime library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The rsyslog runtime library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the rsyslog runtime library.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 * A copy of the LGPL can be found in the file "COPYING.LESSER" in this distribution.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glob.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libestr.h>
#include "rsyslog.h"
#include "rainerscript.h"
#include "parserif.h"
#include "grammar.h"
#include "queue.h"
#include "srUtils.h"
#include "regexp.h"
#include "obj.h"

DEFobjCurrIf(obj)
DEFobjCurrIf(regexp)

void
readConfFile(FILE *fp, es_str_t **str)
{
	char ln[10240];
	char buf[512];
	int lenBuf;
	int bWriteLineno = 0;
	int len, i;
	int start;	/* start index of to be submitted text */
	int bContLine = 0;
	int lineno = 0;

	*str = es_newStr(4096);

	while(fgets(ln, sizeof(ln), fp) != NULL) {
		++lineno;
		if(bWriteLineno) {
			bWriteLineno = 0;
			lenBuf = sprintf(buf, "PreprocFileLineNumber(%d)\n", lineno);
			es_addBuf(str, buf, lenBuf);
		}
		len = strlen(ln);
		/* if we are continuation line, we need to drop leading WS */
		if(bContLine) {
			for(start = 0 ; start < len && isspace(ln[start]) ; ++start)
				/* JUST SCAN */;
		} else {
			start = 0;
		}
		for(i = len - 1 ; i >= start && isspace(ln[i]) ; --i)
			/* JUST SCAN */;
		if(i >= 0) {
			if(ln[i] == '\\') {
				--i;
				bContLine = 1;
			} else {
				if(bContLine) /* write line number if we had cont line */
					bWriteLineno = 1;
				bContLine = 0;
			}
			/* add relevant data to buffer */
			es_addBuf(str, ln+start, i+1 - start);
		}
		if(!bContLine)
			es_addChar(str, '\n');
	}
	/* indicate end of buffer to flex */
	es_addChar(str, '\0');
	es_addChar(str, '\0');
}

struct objlst*
objlstNew(struct cnfobj *o)
{
	struct objlst *lst;

	if((lst = malloc(sizeof(struct objlst))) != NULL) {
		lst->next = NULL;
		lst->obj = o;
	}
dbgprintf("AAAA: creating new objlst\n");
cnfobjPrint(o);

	return lst;
}

/* add object to end of object list, always returns pointer to root object */
struct objlst*
objlstAdd(struct objlst *root, struct cnfobj *o)
{
	struct objlst *l;
	struct objlst *newl;
	
	newl = objlstNew(o);
	if(root == 0) {
		root = newl;
	} else { /* find last, linear search ok, as only during config phase */
		for(l = root ; l->next != NULL ; l = l->next)
			;
		l->next = newl;
	}
	return root;
}

void
objlstDestruct(struct objlst *lst)
{
	struct objlst *toDel;

	while(lst != NULL) {
		toDel = lst;
		lst = lst->next;
		cnfobjDestruct(toDel->obj);
		free(toDel);
	}
}

void
objlstPrint(struct objlst *lst)
{
	dbgprintf("objlst %p:\n", lst);
	while(lst != NULL) {
		cnfobjPrint(lst->obj);
		lst = lst->next;
	}
}

struct nvlst*
nvlstNew(es_str_t *name, es_str_t *value)
{
	struct nvlst *lst;

	if((lst = malloc(sizeof(struct nvlst))) != NULL) {
		lst->next = NULL;
		lst->name = name;
		lst->val.datatype = 'S';
		lst->val.d.estr = value;
		lst->bUsed = 0;
	}

	return lst;
}

void
nvlstDestruct(struct nvlst *lst)
{
	struct nvlst *toDel;

	while(lst != NULL) {
		toDel = lst;
		lst = lst->next;
		es_deleteStr(toDel->name);
		if(toDel->val.datatype == 'S')
			es_deleteStr(toDel->val.d.estr);
		free(toDel);
	}
}

void
nvlstPrint(struct nvlst *lst)
{
	char *name, *value;
	dbgprintf("nvlst %p:\n", lst);
	while(lst != NULL) {
		name = es_str2cstr(lst->name, NULL);
		// TODO: support for non-string types
		value = es_str2cstr(lst->val.d.estr, NULL);
		dbgprintf("\tname: '%s', value '%s'\n", name, value);
		free(name);
		free(value);
		lst = lst->next;
	}
}

/* find a name starting at node lst. Returns node with this
 * name or NULL, if none found.
 */
struct nvlst*
nvlstFindName(struct nvlst *lst, es_str_t *name)
{
	while(lst != NULL && es_strcmp(lst->name, name))
		lst = lst->next;
	return lst;
}


/* find a name starting at node lst. Same as nvlstFindName, but
 * for classical C strings. This is useful because the config system
 * uses C string constants.
 */
static inline struct nvlst*
nvlstFindNameCStr(struct nvlst *lst, char *name)
{
	es_size_t lenName = strlen(name);
	while(lst != NULL && es_strcasebufcmp(lst->name, (uchar*)name, lenName))
		lst = lst->next;
	return lst;
}


/* check if there are duplicate names inside a nvlst and emit
 * an error message, if so.
 */
static inline void
nvlstChkDupes(struct nvlst *lst)
{
	char *cstr;

	while(lst != NULL) {
		if(nvlstFindName(lst->next, lst->name) != NULL) {
			cstr = es_str2cstr(lst->name, NULL);
			parser_errmsg("duplicate parameter '%s' -- "
			  "interpretation is ambigious, one value "
			  "will be randomly selected. Fix this problem.",
			  cstr);
			free(cstr);
		}
		lst = lst->next;
	}
}


/* check for unused params and emit error message is found. This must
 * be called after all config params have been pulled from the object
 * (otherwise the flags are not correctly set).
 */
void
nvlstChkUnused(struct nvlst *lst)
{
	char *cstr;

	while(lst != NULL) {
		if(!lst->bUsed) {
			cstr = es_str2cstr(lst->name, NULL);
			parser_errmsg("parameter '%s' not known -- "
			  "typo in config file?", 
			  cstr);
			free(cstr);
		}
		lst = lst->next;
	}
}


static inline int
doGetSize(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	unsigned char *c;
	es_size_t i;
	long long n;
	int r;
	c = es_getBufAddr(valnode->val.d.estr);
	n = 0;
	i = 0;
	while(i < es_strlen(valnode->val.d.estr) && isdigit(*c)) {
		n = 10 * n + *c - '0';
		++i;
		++c;
	}
	if(i < es_strlen(valnode->val.d.estr)) {
		++i;
		switch(*c) {
		/* traditional binary-based definitions */
		case 'k': n *= 1024; break;
		case 'm': n *= 1024 * 1024; break;
		case 'g': n *= 1024 * 1024 * 1024; break;
		case 't': n *= (int64) 1024 * 1024 * 1024 * 1024; break; /* tera */
		case 'p': n *= (int64) 1024 * 1024 * 1024 * 1024 * 1024; break; /* peta */
		case 'e': n *= (int64) 1024 * 1024 * 1024 * 1024 * 1024 * 1024; break; /* exa */
		/* and now the "new" 1000-based definitions */
		case 'K': n *= 1000; break;
	        case 'M': n *= 1000000; break;
                case 'G': n *= 1000000000; break;
			  /* we need to use the multiplication below because otherwise
			   * the compiler gets an error during constant parsing */
                case 'T': n *= (int64) 1000       * 1000000000; break; /* tera */
                case 'P': n *= (int64) 1000000    * 1000000000; break; /* peta */
                case 'E': n *= (int64) 1000000000 * 1000000000; break; /* exa */
		default: --i; break; /* indicates error */
		}
	}
	if(i == es_strlen(valnode->val.d.estr)) {
		val->val.datatype = 'N';
		val->val.d.n = n;
		r = 1;
	} else {
		parser_errmsg("parameter '%s' does not contain a valid size",
			      param->name);
		r = 0;
	}
	return r;
}


static inline int
doGetBinary(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	int r = 1;
	val->val.datatype = 'N';
	if(!es_strbufcmp(valnode->val.d.estr, (unsigned char*) "on", 2)) {
		val->val.d.n = 1;
	} else if(!es_strbufcmp(valnode->val.d.estr, (unsigned char*) "off", 3)) {
		val->val.d.n = 0;
	} else {
		parser_errmsg("parameter '%s' must be \"on\" or \"off\" but "
		  "is neither. Results unpredictable.", param->name);
		val->val.d.n = 0;
		r = 0;
	}
	return r;
}

static inline int
doGetQueueType(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	char *cstr;
	int r = 1;
	if(!es_strcasebufcmp(valnode->val.d.estr, (uchar*)"fixedarray", 10)) {
		val->val.d.n = QUEUETYPE_FIXED_ARRAY;
	} else if(!es_strcasebufcmp(valnode->val.d.estr, (uchar*)"linkedlist", 10)) {
		val->val.d.n = QUEUETYPE_LINKEDLIST;
	} else if(!es_strcasebufcmp(valnode->val.d.estr, (uchar*)"disk", 4)) {
		val->val.d.n = QUEUETYPE_DISK;
	} else if(!es_strcasebufcmp(valnode->val.d.estr, (uchar*)"direct", 6)) {
		val->val.d.n = QUEUETYPE_DIRECT;
	} else {
		cstr = es_str2cstr(valnode->val.d.estr, NULL);
		parser_errmsg("param '%s': unknown queue type: '%s'",
			      param->name, cstr);
		free(cstr);
		r = 0;
	}
	val->val.datatype = 'N';
	return r;
}


/* A file create-mode must be a four-digit octal number
 * starting with '0'.
 */
static inline int
doGetFileCreateMode(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	int fmtOK = 0;
	char *cstr;
	uchar *c;

	if(es_strlen(valnode->val.d.estr) == 4) {
		c = es_getBufAddr(valnode->val.d.estr);
		if(!(   (c[0] == '0')
		     && (c[1] >= '0' && c[1] <= '7')
		     && (c[2] >= '0' && c[2] <= '7')
		     && (c[3] >= '0' && c[3] <= '7')  )  ) {
			fmtOK = 1;
		}
	}

	if(fmtOK) {
		val->val.datatype = 'N';
		val->val.d.n = (c[1]-'0') * 64 + (c[2]-'0') * 8 + (c[3]-'0');;
	} else {
		cstr = es_str2cstr(valnode->val.d.estr, NULL);
		parser_errmsg("file modes need to be specified as "
		  "4-digit octal numbers starting with '0' -"
		  "parameter '%s=\"%s\"' is not a file mode",
		param->name, cstr);
		free(cstr);
	}
	return fmtOK;
}

static inline int
doGetGID(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	char *cstr;
	int r;
	struct group *resultBuf;
	struct group wrkBuf;
	char stringBuf[2048]; /* 2048 has been proven to be large enough */

	cstr = es_str2cstr(valnode->val.d.estr, NULL);
	getgrnam_r(cstr, &wrkBuf, stringBuf, sizeof(stringBuf), &resultBuf);
	if(resultBuf == NULL) {
		parser_errmsg("parameter '%s': ID for group %s could not "
		  "be found", param->name, cstr);
		r = 0;
	} else {
		val->val.datatype = 'N';
		val->val.d.n = resultBuf->gr_gid;
		dbgprintf("param '%s': uid %d obtained for group '%s'\n",
		   param->name, (int) resultBuf->gr_gid, cstr);
		r = 1;
	}
	free(cstr);
	return r;
}

static inline int
doGetUID(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	char *cstr;
	int r;
	struct passwd *resultBuf;
	struct passwd wrkBuf;
	char stringBuf[2048]; /* 2048 has been proven to be large enough */

	cstr = es_str2cstr(valnode->val.d.estr, NULL);
	getpwnam_r(cstr, &wrkBuf, stringBuf, sizeof(stringBuf), &resultBuf);
	if(resultBuf == NULL) {
		parser_errmsg("parameter '%s': ID for user %s could not "
		  "be found", param->name, cstr);
		r = 0;
	} else {
		val->val.datatype = 'N';
		val->val.d.n = resultBuf->pw_uid;
		dbgprintf("param '%s': uid %d obtained for user '%s'\n",
		   param->name, (int) resultBuf->pw_uid, cstr);
		r = 1;
	}
	free(cstr);
	return r;
}

/* note: we support all integer formats that es_str2num support,
 * so hex and octal representations are also valid.
 */
static inline int
doGetInt(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	long long n;
	int bSuccess;

	n = es_str2num(valnode->val.d.estr, &bSuccess);
	if(!bSuccess) {
		parser_errmsg("parameter '%s' is not a proper number",
		  param->name);
	}
	val->val.datatype = 'N';
	val->val.d.n = n;
	return bSuccess;
}

static inline int
doGetNonNegInt(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	int bSuccess;

	if((bSuccess = doGetInt(valnode, param, val))) {
		if(val->val.d.n < 0) {
			parser_errmsg("parameter '%s' cannot be less than zero (was %lld)",
			  param->name, val->val.d.n);
			bSuccess = 0;
		}
	}
	return bSuccess;
}

static inline int
doGetPositiveInt(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	int bSuccess;

	if((bSuccess = doGetInt(valnode, param, val))) {
		if(val->val.d.n < 1) {
			parser_errmsg("parameter '%s' cannot be less than one (was %lld)",
			  param->name, val->val.d.n);
			bSuccess = 0;
		}
	}
	return bSuccess;
}

static inline int
doGetWord(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	es_size_t i;
	int r = 1;
	unsigned char *c;
	val->val.datatype = 'S';
	val->val.d.estr = es_newStr(32);
	c = es_getBufAddr(valnode->val.d.estr);
	for(i = 0 ; i < es_strlen(valnode->val.d.estr) && !isspace(c[i]) ; ++i) {
		es_addChar(&val->val.d.estr, c[i]);
	}
	if(i != es_strlen(valnode->val.d.estr)) {
		parser_errmsg("parameter '%s' contains whitespace, which is not "
		  "permitted - data after first whitespace ignored",
		  param->name);
		r = 0;
	}
	return r;
}

static inline int
doGetChar(struct nvlst *valnode, struct cnfparamdescr *param,
	  struct cnfparamvals *val)
{
	int r = 1;
	if(es_strlen(valnode->val.d.estr) != 1) {
		parser_errmsg("parameter '%s' must contain exactly one character "
		  "but contains %d - cannot be processed",
		  param->name, es_strlen(valnode->val.d.estr));
		r = 0;
	}
	val->val.datatype = 'S';
	val->val.d.estr = es_strdup(valnode->val.d.estr);
	return r;
}

/* get a single parameter according to its definition. Helper to
 * nvlstGetParams. returns 1 if success, 0 otherwise
 */
static inline int
nvlstGetParam(struct nvlst *valnode, struct cnfparamdescr *param,
	       struct cnfparamvals *val)
{
	uchar *cstr;
	int r;

	dbgprintf("XXXX: in nvlstGetParam, name '%s', type %d, valnode->bUsed %d\n",
		  param->name, (int) param->type, valnode->bUsed);
	valnode->bUsed = 1;
	val->bUsed = 1;
	switch(param->type) {
	case eCmdHdlrQueueType:
		r = doGetQueueType(valnode, param, val);
		break;
	case eCmdHdlrUID:
		r = doGetUID(valnode, param, val);
		break;
	case eCmdHdlrGID:
		r = doGetGID(valnode, param, val);
		break;
	case eCmdHdlrBinary:
		r = doGetBinary(valnode, param, val);
		break;
	case eCmdHdlrFileCreateMode:
		r = doGetFileCreateMode(valnode, param, val);
		break;
	case eCmdHdlrInt:
		r = doGetInt(valnode, param, val);
		break;
	case eCmdHdlrNonNegInt:
		r = doGetPositiveInt(valnode, param, val);
		break;
	case eCmdHdlrPositiveInt:
		r = doGetPositiveInt(valnode, param, val);
		break;
	case eCmdHdlrSize:
		r = doGetSize(valnode, param, val);
		break;
	case eCmdHdlrGetChar:
		r = doGetChar(valnode, param, val);
		break;
	case eCmdHdlrFacility:
		cstr = (uchar*) es_str2cstr(valnode->val.d.estr, NULL);
		val->val.datatype = 'N';
		val->val.d.n = decodeSyslogName(cstr, syslogFacNames);
		free(cstr);
		r = 1;
		break;
	case eCmdHdlrSeverity:
		cstr = (uchar*) es_str2cstr(valnode->val.d.estr, NULL);
		val->val.datatype = 'N';
		val->val.d.n = decodeSyslogName(cstr, syslogPriNames);
		free(cstr);
		r = 1;
		break;
	case eCmdHdlrGetWord:
		r = doGetWord(valnode, param, val);
		break;
	case eCmdHdlrString:
		val->val.datatype = 'S';
		val->val.d.estr = es_strdup(valnode->val.d.estr);
		r = 1;
		break;
	case eCmdHdlrGoneAway:
		parser_errmsg("parameter '%s' is no longer supported",
			      param->name);
		r = 1; /* this *is* valid! */
		break;
	default:
		dbgprintf("error: invalid param type\n");
		r = 0;
		break;
	}
	return r;
}


/* obtain conf params from an nvlst and emit error messages if
 * necessary. If an already-existing param value is passed, that is
 * used. If NULL is passed instead, a new one is allocated. In that case,
 * it is the caller's duty to free it when no longer needed.
 * NULL is returned on error, otherwise a pointer to the vals array.
 */
struct cnfparamvals*
nvlstGetParams(struct nvlst *lst, struct cnfparamblk *params,
	       struct cnfparamvals *vals)
{
	int i;
	int bValsWasNULL;
	int bInError = 0;
	struct nvlst *valnode;
	struct cnfparamdescr *param;

	if(params->version != CNFPARAMBLK_VERSION) {
		dbgprintf("nvlstGetParams: invalid param block version "
			  "%d, expected %d\n",
			  params->version, CNFPARAMBLK_VERSION);
		return NULL;
	}
	
	if(vals == NULL) {
		bValsWasNULL = 1;
		if((vals = calloc(params->nParams,
				  sizeof(struct cnfparamvals))) == NULL)
			return NULL;
	} else {
		bValsWasNULL = 0;
	}

	for(i = 0 ; i < params->nParams ; ++i) {
		param = params->descr + i;
		if((valnode = nvlstFindNameCStr(lst, param->name)) == NULL)
			continue;
		if(vals[i].bUsed) {
			parser_errmsg("parameter '%s' specified more than once - "
			  "one instance is ignored. Fix config", param->name);
			continue;
		}
		if(!nvlstGetParam(valnode, param, vals + i)) {
			bInError = 1;
		}
	}


	if(bInError) {
		if(bValsWasNULL)
			cnfparamvalsDestruct(vals, params);
		vals = NULL;
	}

	return vals;
}


void
cnfparamsPrint(struct cnfparamblk *params, struct cnfparamvals *vals)
{
	int i;
	char *cstr;

	for(i = 0 ; i < params->nParams ; ++i) {
		dbgprintf("%s: ", params->descr[i].name);
		if(vals[i].bUsed) {
			// TODO: other types!
			switch(vals[i].val.datatype) {
			case 'S':
				cstr = es_str2cstr(vals[i].val.d.estr, NULL);
				dbgprintf(" '%s'", cstr);
				free(cstr);
				break;
			case 'N':
				dbgprintf("%lld", vals[i].val.d.n);
				break;
			default:
				dbgprintf("(unsupported datatype %c)",
					  vals[i].val.datatype);
			}
		} else {
			dbgprintf("(unset)");
		}
		dbgprintf("\n");
	}
}

struct cnfobj*
cnfobjNew(enum cnfobjType objType, struct nvlst *lst)
{
	struct cnfobj *o;

	if((o = malloc(sizeof(struct nvlst))) != NULL) {
		nvlstChkDupes(lst);
		o->objType = objType;
		o->nvlst = lst;
		o->subobjs = NULL;
	}

	return o;
}

void
cnfobjDestruct(struct cnfobj *o)
{
	if(o != NULL) {
		nvlstDestruct(o->nvlst);
		objlstDestruct(o->subobjs);
		free(o);
	}
}

void
cnfobjPrint(struct cnfobj *o)
{
	dbgprintf("obj: '%s'\n", cnfobjType2str(o->objType));
	nvlstPrint(o->nvlst);
}


struct cnfactlst*
cnfactlstNew(enum cnfactType actType, struct nvlst *lst, char *actLine)
{
	struct cnfactlst *actlst;

	if((actlst = malloc(sizeof(struct cnfactlst))) != NULL) {
		actlst->next = NULL;
		actlst->syslines = NULL;
		actlst->actType = actType;
		actlst->lineno = yylineno;
		actlst->cnfFile = strdup(cnfcurrfn);
		if(actType == CNFACT_V2)
			actlst->data.lst = lst;
		else
			actlst->data.legActLine = actLine;
	}
	return actlst;
}

struct cnfactlst*
cnfactlstAddSysline(struct cnfactlst* actlst, char *line)
{
	struct cnfcfsyslinelst *cflst;

	if((cflst = malloc(sizeof(struct cnfcfsyslinelst))) != NULL)   {
		cflst->line = line;
		if(actlst->syslines == NULL) {
			cflst->next = NULL;
		} else {
			cflst->next = actlst->syslines;
		}
		actlst->syslines = cflst;
	}
	return actlst;
}


void
cnfactlstDestruct(struct cnfactlst *actlst)
{
	struct cnfactlst *toDel;

	while(actlst != NULL) {
		toDel = actlst;
		actlst = actlst->next;
		free(toDel->cnfFile);
		cnfcfsyslinelstDestruct(toDel->syslines);
		if(toDel->actType == CNFACT_V2)
			nvlstDestruct(toDel->data.lst);
		else
			free(toDel->data.legActLine);
		free(toDel);
	}
	
}

static inline struct cnfcfsyslinelst*
cnfcfsyslinelstReverse(struct cnfcfsyslinelst *lst)
{
	struct cnfcfsyslinelst *curr, *prev;
	if(lst == NULL)
		return NULL;
	prev = NULL;
	while(lst != NULL) {
		curr = lst;
		lst = lst->next;
		curr->next = prev;
		prev = curr;
	}
	return prev;
}

struct cnfactlst*
cnfactlstReverse(struct cnfactlst *actlst)
{
	struct cnfactlst *curr, *prev;

	prev = NULL;
	while(actlst != NULL) {
		curr = actlst;
		actlst = actlst->next;
		curr->syslines = cnfcfsyslinelstReverse(curr->syslines);
		curr->next = prev;
		prev = curr;
	}
	return prev;
}

void
cnfactlstPrint(struct cnfactlst *actlst)
{
	struct cnfcfsyslinelst *cflst;

	while(actlst != NULL) {
		dbgprintf("aclst %p: ", actlst);
		if(actlst->actType == CNFACT_V2) {
			dbgprintf("V2 action type: ");
			nvlstPrint(actlst->data.lst);
		} else {
			dbgprintf("legacy action line: '%s'\n",
				actlst->data.legActLine);
		}
		for(  cflst = actlst->syslines
		    ; cflst != NULL ; cflst = cflst->next) {
			dbgprintf("action:cfsysline: '%s'\n", cflst->line);
		}
		actlst = actlst->next;
	}
}

struct cnfexpr*
cnfexprNew(unsigned nodetype, struct cnfexpr *l, struct cnfexpr *r)
{
	struct cnfexpr *expr;

	/* optimize some constructs during parsing */
	if(nodetype == 'M' && r->nodetype == 'N') {
		((struct cnfnumval*)r)->val *= -1;
		expr = r;
		goto done;
	}

	if((expr = malloc(sizeof(struct cnfexpr))) != NULL) {
		expr->nodetype = nodetype;
		expr->l = l;
		expr->r = r;
	}
done:
	return expr;
}


/* ensure that retval is a number; if string is no number,
 * try to convert it to one. The semantics from es_str2num()
 * are used (bSuccess tells if the conversion went well or not).
 */
static inline long long
var2Number(struct var *r, int *bSuccess)
{
	long long n;
	if(r->datatype == 'S') {
		n = es_str2num(r->d.estr, bSuccess);
	} else {
		n = r->d.n;
		if(bSuccess)
			*bSuccess = 1;
	}
	return n;
}

/* ensure that retval is a string
 */
static inline es_str_t *
var2String(struct var *r, int *bMustFree)
{
	if(r->datatype == 'N') {
		*bMustFree = 1;
		return es_newStrFromNumber(r->d.n);
	}
	*bMustFree = 0;
	return r->d.estr;
}

/* Perform a function call. This has been moved out of cnfExprEval in order
 * to keep the code small and easier to maintain.
 */
static inline void
doFuncCall(struct cnffunc *func, struct var *ret, void* usrptr)
{
	char *fname;
	char *envvar;
	int bMustFree;
	es_str_t *estr;
	char *str;
	int retval;
	struct var r[CNFFUNC_MAX_ARGS];

	dbgprintf("rainerscript: executing function id %d\n", func->fID);
	switch(func->fID) {
	case CNFFUNC_STRLEN:
		if(func->expr[0]->nodetype == 'S') {
			/* if we already have a string, we do not need to
			 * do one more recursive call.
			 */
			ret->d.n = es_strlen(((struct cnfstringval*) func->expr[0])->estr);
		} else {
			cnfexprEval(func->expr[0], &r[0], usrptr);
			estr = var2String(&r[0], &bMustFree);
			ret->d.n = es_strlen(estr);
			if(bMustFree) es_deleteStr(estr);
		}
		ret->datatype = 'N';
		break;
	case CNFFUNC_GETENV:
		/* note: the optimizer shall have replaced calls to getenv()
		 * with a constant argument to a single string (once obtained via
		 * getenv()). So we do NOT need to check if there is just a
		 * string following.
		 */
		cnfexprEval(func->expr[0], &r[0], usrptr);
		estr = var2String(&r[0], &bMustFree);
		str = (char*) es_str2cstr(estr, NULL);
		envvar = getenv(str);
		ret->datatype = 'S';
		ret->d.estr = es_newStrFromCStr(envvar, strlen(envvar));
		if(bMustFree) es_deleteStr(estr);
		if(r[0].datatype == 'S') es_deleteStr(r[0].d.estr);
		free(str);
		break;
	case CNFFUNC_TOLOWER:
		cnfexprEval(func->expr[0], &r[0], usrptr);
		estr = var2String(&r[0], &bMustFree);
		if(!bMustFree) /* let caller handle that M) */
			estr = es_strdup(estr);
		es_tolower(estr);
		ret->datatype = 'S';
		ret->d.estr = estr;
		if(r[0].datatype == 'S') es_deleteStr(r[0].d.estr);
		break;
	case CNFFUNC_CSTR:
		cnfexprEval(func->expr[0], &r[0], usrptr);
		estr = var2String(&r[0], &bMustFree);
		if(!bMustFree) /* let caller handle that M) */
			estr = es_strdup(estr);
		ret->datatype = 'S';
		ret->d.estr = estr;
		if(r[0].datatype == 'S') es_deleteStr(r[0].d.estr);
		break;
	case CNFFUNC_CNUM:
		if(func->expr[0]->nodetype == 'N') {
			ret->d.n = ((struct cnfnumval*)func->expr[0])->val;
		} else if(func->expr[0]->nodetype == 'S') {
			ret->d.n = es_str2num(((struct cnfstringval*) func->expr[0])->estr,
					      NULL);
		} else {
			cnfexprEval(func->expr[0], &r[0], usrptr);
			ret->d.n = var2Number(&r[0], NULL);
			if(r[0].datatype == 'S') es_deleteStr(r[0].d.estr);
		}
		ret->datatype = 'N';
		break;
	case CNFFUNC_RE_MATCH:
		cnfexprEval(func->expr[0], &r[0], usrptr);
		estr = var2String(&r[0], &bMustFree);
		str = es_str2cstr(estr, NULL);
		retval = regexp.regexec(func->funcdata, str, 0, NULL, 0);
		if(retval == 0)
			ret->d.n = 1;
		else {
			ret->d.n = 0;
			if(retval != REG_NOMATCH) {
				DBGPRINTF("re_match: regexec returned error %d\n", retval);
			}
		}
		ret->datatype = 'N';
		if(bMustFree) es_deleteStr(estr);
		free(str);
		if(r[0].datatype == 'S') es_deleteStr(r[0].d.estr);
		break;
	default:
		if(Debug) {
			fname = es_str2cstr(func->fname, NULL);
			dbgprintf("rainerscript: invalid function id %u (name '%s')\n",
				  (unsigned) func->fID, fname);
			free(fname);
		}
		ret->datatype = 'N';
		ret->d.n = 0;
	}
}

#define FREE_BOTH_RET \
		if(r.datatype == 'S') es_deleteStr(r.d.estr); \
		if(l.datatype == 'S') es_deleteStr(l.d.estr)

#define COMP_NUM_BINOP(x) \
	cnfexprEval(expr->l, &l, usrptr); \
	cnfexprEval(expr->r, &r, usrptr); \
	ret->datatype = 'N'; \
	ret->d.n = var2Number(&l, &convok_l) x var2Number(&r, &convok_r); \
	FREE_BOTH_RET

#define PREP_TWO_STRINGS \
		cnfexprEval(expr->l, &l, usrptr); \
		estr_l = var2String(&l, &bMustFree2); \
		cnfexprEval(expr->r, &r, usrptr); \
		estr_r = var2String(&r, &bMustFree)

#define FREE_TWO_STRINGS \
		if(bMustFree) es_deleteStr(estr_r); \
		if(bMustFree2) es_deleteStr(estr_l); \
		FREE_BOTH_RET

/* evaluate an expression.
 * Note that we try to avoid malloc whenever possible (because of
 * the large overhead it has, especially on highly threaded programs).
 * As such, the each caller level must provide buffer space for the
 * result on its stack during recursion. This permits the callee to store
 * the return value without malloc. As the value is a somewhat larger
 * struct, we could otherwise not return it without malloc.
 * Note that we implement boolean shortcut operations. For our needs, there
 * simply is no case where full evaluation would make any sense at all.
 */
void
cnfexprEval(struct cnfexpr *expr, struct var *ret, void* usrptr)
{
	struct var r, l; /* memory for subexpression results */
	es_str_t *estr_r, *estr_l;
	int convok_r, convok_l;
	int bMustFree, bMustFree2;
	long long n_r, n_l;

	dbgprintf("eval expr %p, type '%c'(%u)\n", expr, expr->nodetype, expr->nodetype);
	switch(expr->nodetype) {
	/* note: comparison operations are extremely similar. The code can be copyied, only
	 * places flagged with "CMP" need to be changed.
	 */
	case CMP_EQ:
		cnfexprEval(expr->l, &l, usrptr);
		cnfexprEval(expr->r, &r, usrptr);
		ret->datatype = 'N';
		if(l.datatype == 'S') {
			if(r.datatype == 'S') {
				ret->d.n = !es_strcmp(l.d.estr, r.d.estr); /*CMP*/
			} else {
				n_l = var2Number(&l, &convok_l);
				if(convok_l) {
					ret->d.n = (n_l == r.d.n); /*CMP*/
				} else {
					estr_r = var2String(&r, &bMustFree);
					ret->d.n = !es_strcmp(l.d.estr, estr_r); /*CMP*/
					if(bMustFree) es_deleteStr(estr_r);
				}
			}
		} else {
			if(r.datatype == 'S') {
				n_r = var2Number(&r, &convok_r);
				if(convok_r) {
					ret->d.n = (l.d.n == n_r); /*CMP*/
				} else {
					estr_l = var2String(&l, &bMustFree);
					ret->d.n = !es_strcmp(r.d.estr, estr_l); /*CMP*/
					if(bMustFree) es_deleteStr(estr_l);
				}
			} else {
				ret->d.n = (l.d.n == r.d.n); /*CMP*/
			}
		}
		FREE_BOTH_RET;
		break;
	case CMP_NE:
		cnfexprEval(expr->l, &l, usrptr);
		cnfexprEval(expr->r, &r, usrptr);
		ret->datatype = 'N';
		if(l.datatype == 'S') {
			if(r.datatype == 'S') {
				ret->d.n = es_strcmp(l.d.estr, r.d.estr); /*CMP*/
			} else {
				n_l = var2Number(&l, &convok_l);
				if(convok_l) {
					ret->d.n = (n_l != r.d.n); /*CMP*/
				} else {
					estr_r = var2String(&r, &bMustFree);
					ret->d.n = es_strcmp(l.d.estr, estr_r); /*CMP*/
					if(bMustFree) es_deleteStr(estr_r);
				}
			}
		} else {
			if(r.datatype == 'S') {
				n_r = var2Number(&r, &convok_r);
				if(convok_r) {
					ret->d.n = (l.d.n != n_r); /*CMP*/
				} else {
					estr_l = var2String(&l, &bMustFree);
					ret->d.n = es_strcmp(r.d.estr, estr_l); /*CMP*/
					if(bMustFree) es_deleteStr(estr_l);
				}
			} else {
				ret->d.n = (l.d.n != r.d.n); /*CMP*/
			}
		}
		FREE_BOTH_RET;
		break;
	case CMP_LE:
		cnfexprEval(expr->l, &l, usrptr);
		cnfexprEval(expr->r, &r, usrptr);
		ret->datatype = 'N';
		if(l.datatype == 'S') {
			if(r.datatype == 'S') {
				ret->d.n = es_strcmp(l.d.estr, r.d.estr) <= 0; /*CMP*/
			} else {
				n_l = var2Number(&l, &convok_l);
				if(convok_l) {
					ret->d.n = (n_l <= r.d.n); /*CMP*/
				} else {
					estr_r = var2String(&r, &bMustFree);
					ret->d.n = es_strcmp(l.d.estr, estr_r) <= 0; /*CMP*/
					if(bMustFree) es_deleteStr(estr_r);
				}
			}
		} else {
			if(r.datatype == 'S') {
				n_r = var2Number(&r, &convok_r);
				if(convok_r) {
					ret->d.n = (l.d.n <= n_r); /*CMP*/
				} else {
					estr_l = var2String(&l, &bMustFree);
					ret->d.n = es_strcmp(r.d.estr, estr_l) <= 0; /*CMP*/
					if(bMustFree) es_deleteStr(estr_l);
				}
			} else {
				ret->d.n = (l.d.n <= r.d.n); /*CMP*/
			}
		}
		FREE_BOTH_RET;
		break;
	case CMP_GE:
		cnfexprEval(expr->l, &l, usrptr);
		cnfexprEval(expr->r, &r, usrptr);
		ret->datatype = 'N';
		if(l.datatype == 'S') {
			if(r.datatype == 'S') {
				ret->d.n = es_strcmp(l.d.estr, r.d.estr) >= 0; /*CMP*/
			} else {
				n_l = var2Number(&l, &convok_l);
				if(convok_l) {
					ret->d.n = (n_l >= r.d.n); /*CMP*/
				} else {
					estr_r = var2String(&r, &bMustFree);
					ret->d.n = es_strcmp(l.d.estr, estr_r) >= 0; /*CMP*/
					if(bMustFree) es_deleteStr(estr_r);
				}
			}
		} else {
			if(r.datatype == 'S') {
				n_r = var2Number(&r, &convok_r);
				if(convok_r) {
					ret->d.n = (l.d.n >= n_r); /*CMP*/
				} else {
					estr_l = var2String(&l, &bMustFree);
					ret->d.n = es_strcmp(r.d.estr, estr_l) >= 0; /*CMP*/
					if(bMustFree) es_deleteStr(estr_l);
				}
			} else {
				ret->d.n = (l.d.n >= r.d.n); /*CMP*/
			}
		}
		FREE_BOTH_RET;
		break;
	case CMP_LT:
		cnfexprEval(expr->l, &l, usrptr);
		cnfexprEval(expr->r, &r, usrptr);
		ret->datatype = 'N';
		if(l.datatype == 'S') {
			if(r.datatype == 'S') {
				ret->d.n = es_strcmp(l.d.estr, r.d.estr) < 0; /*CMP*/
			} else {
				n_l = var2Number(&l, &convok_l);
				if(convok_l) {
					ret->d.n = (n_l < r.d.n); /*CMP*/
				} else {
					estr_r = var2String(&r, &bMustFree);
					ret->d.n = es_strcmp(l.d.estr, estr_r) < 0; /*CMP*/
					if(bMustFree) es_deleteStr(estr_r);
				}
			}
		} else {
			if(r.datatype == 'S') {
				n_r = var2Number(&r, &convok_r);
				if(convok_r) {
					ret->d.n = (l.d.n < n_r); /*CMP*/
				} else {
					estr_l = var2String(&l, &bMustFree);
					ret->d.n = es_strcmp(r.d.estr, estr_l) < 0; /*CMP*/
					if(bMustFree) es_deleteStr(estr_l);
				}
			} else {
				ret->d.n = (l.d.n < r.d.n); /*CMP*/
			}
		}
		FREE_BOTH_RET;
		break;
	case CMP_GT:
		cnfexprEval(expr->l, &l, usrptr);
		cnfexprEval(expr->r, &r, usrptr);
		ret->datatype = 'N';
		if(l.datatype == 'S') {
			if(r.datatype == 'S') {
				ret->d.n = es_strcmp(l.d.estr, r.d.estr) > 0; /*CMP*/
			} else {
				n_l = var2Number(&l, &convok_l);
				if(convok_l) {
					ret->d.n = (n_l > r.d.n); /*CMP*/
				} else {
					estr_r = var2String(&r, &bMustFree);
					ret->d.n = es_strcmp(l.d.estr, estr_r) > 0; /*CMP*/
					if(bMustFree) es_deleteStr(estr_r);
				}
			}
		} else {
			if(r.datatype == 'S') {
				n_r = var2Number(&r, &convok_r);
				if(convok_r) {
					ret->d.n = (l.d.n > n_r); /*CMP*/
				} else {
					estr_l = var2String(&l, &bMustFree);
					ret->d.n = es_strcmp(r.d.estr, estr_l) > 0; /*CMP*/
					if(bMustFree) es_deleteStr(estr_l);
				}
			} else {
				ret->d.n = (l.d.n > r.d.n); /*CMP*/
			}
		}
		FREE_BOTH_RET;
		break;
	case CMP_STARTSWITH:
		PREP_TWO_STRINGS;
		ret->datatype = 'N';
		ret->d.n = es_strncmp(estr_l, estr_r, estr_r->lenStr) == 0;
		FREE_TWO_STRINGS;
		break;
	case CMP_STARTSWITHI:
		PREP_TWO_STRINGS;
		ret->datatype = 'N';
		ret->d.n = es_strncasecmp(estr_l, estr_r, estr_r->lenStr) == 0;
		FREE_TWO_STRINGS;
		break;
	case CMP_CONTAINS:
		PREP_TWO_STRINGS;
		ret->datatype = 'N';
		ret->d.n = es_strContains(estr_l, estr_r) != -1;
		FREE_TWO_STRINGS;
		break;
	case CMP_CONTAINSI:
		PREP_TWO_STRINGS;
		ret->datatype = 'N';
		ret->d.n = es_strCaseContains(estr_l, estr_r) != -1;
		FREE_TWO_STRINGS;
		break;
	case OR:
		cnfexprEval(expr->l, &l, usrptr);
		ret->datatype = 'N';
		if(var2Number(&l, &convok_l)) {
			ret->d.n = 1ll;
		} else {
			cnfexprEval(expr->r, &r, usrptr);
			if(var2Number(&r, &convok_r))
				ret->d.n = 1ll;
			else 
				ret->d.n = 0ll;
			if(r.datatype == 'S') es_deleteStr(r.d.estr); 
		}
		if(l.datatype == 'S') es_deleteStr(l.d.estr);
		break;
	case AND:
		cnfexprEval(expr->l, &l, usrptr);
		ret->datatype = 'N';
		if(var2Number(&l, &convok_l)) {
			cnfexprEval(expr->r, &r, usrptr);
			if(var2Number(&r, &convok_r))
				ret->d.n = 1ll;
			else 
				ret->d.n = 0ll;
			if(r.datatype == 'S') es_deleteStr(r.d.estr); 
		} else {
			ret->d.n = 0ll;
		}
		if(l.datatype == 'S') es_deleteStr(l.d.estr);
		break;
	case NOT:
		cnfexprEval(expr->r, &r, usrptr);
		ret->datatype = 'N';
		ret->d.n = !var2Number(&r, &convok_r);
		if(r.datatype == 'S') es_deleteStr(r.d.estr);
		break;
	case 'N':
		ret->datatype = 'N';
		ret->d.n = ((struct cnfnumval*)expr)->val;
		break;
	case 'S':
		ret->datatype = 'S';
		ret->d.estr = es_strdup(((struct cnfstringval*)expr)->estr);
		break;
	case 'V':
		ret->datatype = 'S';
		ret->d.estr = cnfGetVar(((struct cnfvar*)expr)->name, usrptr);
		break;
	case '+':
		COMP_NUM_BINOP(+);
		break;
	case '-':
		COMP_NUM_BINOP(-);
		break;
	case '*':
		COMP_NUM_BINOP(*);
		break;
	case '/':
		COMP_NUM_BINOP(/);
		break;
	case '%':
		COMP_NUM_BINOP(%);
		break;
	case 'M':
		cnfexprEval(expr->r, &r, usrptr);
		ret->datatype = 'N';
		ret->d.n = -var2Number(&r, &convok_r);
		if(r.datatype == 'S') es_deleteStr(r.d.estr);
		break;
	case 'F':
		doFuncCall((struct cnffunc*) expr, ret, usrptr);
		break;
	default:
		ret->datatype = 'N';
		ret->d.n = 0ll;
		dbgprintf("eval error: unknown nodetype %u['%c']\n",
			(unsigned) expr->nodetype, (char) expr->nodetype);
		break;
	}
}

//---------------------------------------------------------

static inline void
cnffuncDestruct(struct cnffunc *func)
{
	unsigned short i;

	for(i = 0 ; i < func->nParams ; ++i) {
		cnfexprDestruct(func->expr[i]);
	}
	/* some functions require special destruction */
	switch(func->fID) {
		case CNFFUNC_RE_MATCH:
			regexp.regfree(func->funcdata);
			free(func->funcdata);
			free(func->fname);
			break;
		default:break;
	}
}

/* Destruct an expression and all sub-expressions contained in it.
 */
void
cnfexprDestruct(struct cnfexpr *expr)
{

	dbgprintf("cnfexprDestruct expr %p, type '%c'(%u)\n", expr, expr->nodetype, expr->nodetype);
	switch(expr->nodetype) {
	case CMP_NE:
	case CMP_EQ:
	case CMP_LE:
	case CMP_GE:
	case CMP_LT:
	case CMP_GT:
	case CMP_STARTSWITH:
	case CMP_STARTSWITHI:
	case CMP_CONTAINS:
	case CMP_CONTAINSI:
	case OR:
	case AND:
	case '+':
	case '-':
	case '*':
	case '/':
	case '%': /* binary */
		cnfexprDestruct(expr->l);
		cnfexprDestruct(expr->r);
		break;
	case NOT: 
	case 'M': /* unary */
		cnfexprDestruct(expr->r);
		break;
	case 'N':
		break;
	case 'S':
		es_deleteStr(((struct cnfstringval*)expr)->estr);
		break;
	case 'V':
		free(((struct cnfvar*)expr)->name);
		break;
	case 'F':
		cnffuncDestruct((struct cnffunc*)expr);
		break;
	default:break;
	}
	free(expr);
}

//---- END


/* Evaluate an expression as a bool. This is added because expressions are
 * mostly used inside filters, and so this function is quite common and
 * important.
 */
int
cnfexprEvalBool(struct cnfexpr *expr, void *usrptr)
{
	int convok;
	struct var ret;
	cnfexprEval(expr, &ret, usrptr);
	return var2Number(&ret, &convok);
}

inline static void
doIndent(int indent)
{
	int i;
	for(i = 0 ; i < indent ; ++i)
		dbgprintf("  ");
}
void
cnfexprPrint(struct cnfexpr *expr, int indent)
{
	struct cnffunc *func;
	int i;

	//dbgprintf("expr %p, indent %d, type '%c'\n", expr, indent, expr->nodetype);
	switch(expr->nodetype) {
	case CMP_EQ:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("==\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case CMP_NE:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("!=\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case CMP_LE:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("<=\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case CMP_GE:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf(">=\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case CMP_LT:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("<\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case CMP_GT:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf(">\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case CMP_CONTAINS:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("CONTAINS\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case CMP_CONTAINSI:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("CONTAINS_I\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case CMP_STARTSWITH:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("STARTSWITH\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case CMP_STARTSWITHI:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("STARTSWITH_I\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case OR:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("OR\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case AND:
		cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("AND\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case NOT:
		doIndent(indent);
		dbgprintf("NOT\n");
		cnfexprPrint(expr->r, indent+1);
		break;
	case 'S':
		doIndent(indent);
		cstrPrint("string '", ((struct cnfstringval*)expr)->estr);
		dbgprintf("'\n");
		break;
	case 'N':
		doIndent(indent);
		dbgprintf("%lld\n", ((struct cnfnumval*)expr)->val);
		break;
	case 'V':
		doIndent(indent);
		dbgprintf("var '%s'\n", ((struct cnfvar*)expr)->name);
		break;
	case 'F':
		doIndent(indent);
		func = (struct cnffunc*) expr;
		cstrPrint("function '", func->fname);
		dbgprintf("' (id:%d, params:%hu)\n", func->fID, func->nParams);
		for(i = 0 ; i < func->nParams ; ++i) {
			cnfexprPrint(func->expr[i], indent+1);
		}
		break;
	case '+':
	case '-':
	case '*':
	case '/':
	case '%':
	case 'M':
		if(expr->l != NULL)
			cnfexprPrint(expr->l, indent+1);
		doIndent(indent);
		dbgprintf("%c\n", (char) expr->nodetype);
		cnfexprPrint(expr->r, indent+1);
		break;
	default:
		dbgprintf("error: unknown nodetype %u\n",
			(unsigned) expr->nodetype);
		break;
	}
}

struct cnfnumval*
cnfnumvalNew(long long val)
{
	struct cnfnumval *numval;
	if((numval = malloc(sizeof(struct cnfnumval))) != NULL) {
		numval->nodetype = 'N';
		numval->val = val;
	}
	return numval;
}

struct cnfstringval*
cnfstringvalNew(es_str_t *estr)
{
	struct cnfstringval *strval;
	if((strval = malloc(sizeof(struct cnfstringval))) != NULL) {
		strval->nodetype = 'S';
		strval->estr = estr;
	}
	return strval;
}

struct cnfvar*
cnfvarNew(char *name)
{
	struct cnfvar *var;
	if((var = malloc(sizeof(struct cnfvar))) != NULL) {
		var->nodetype = 'V';
		var->name = name;
	}
	return var;
}

struct cnfrule *
cnfruleNew(enum cnfFiltType filttype, struct cnfactlst *actlst)
{
	struct cnfrule* cnfrule;
	if((cnfrule = malloc(sizeof(struct cnfrule))) != NULL) {
		cnfrule->nodetype = 'R';
		cnfrule->filttype = filttype;
		cnfrule->actlst = cnfactlstReverse(actlst);
	}
	return cnfrule;
}

void
cnfrulePrint(struct cnfrule *rule)
{
	dbgprintf("------ start rule %p:\n", rule);
	dbgprintf("%s: ", cnfFiltType2str(rule->filttype));
	switch(rule->filttype) {
	case CNFFILT_NONE:
		break;
	case CNFFILT_PRI:
	case CNFFILT_PROP:
		dbgprintf("%s\n", rule->filt.s);
		break;
	case CNFFILT_SCRIPT:
		dbgprintf("\n");
		cnfexprPrint(rule->filt.expr, 0);
		break;
	}
	cnfactlstPrint(rule->actlst);
	dbgprintf("------ end rule %p\n", rule);
}

/* note: the sysline itself was already freed during processing
 * and as such MUST NOT be freed again!
 */
void
cnfcfsyslinelstDestruct(struct cnfcfsyslinelst *cfslst)
{
	struct cnfcfsyslinelst *toDel;
	while(cfslst != NULL) {
		toDel = cfslst;
		cfslst = cfslst->next;
		free(toDel);
	}
}

void
cnfruleDestruct(struct cnfrule *rule)
{
	if(   rule->filttype == CNFFILT_PRI
	   || rule->filttype == CNFFILT_PROP)
		free(rule->filt.s);
	cnfactlstDestruct(rule->actlst);
	free(rule);
}

struct cnffparamlst *
cnffparamlstNew(struct cnfexpr *expr, struct cnffparamlst *next)
{
	struct cnffparamlst* lst;
	if((lst = malloc(sizeof(struct cnffparamlst))) != NULL) {
		lst->nodetype = 'P';
		lst->expr = expr;
		lst->next = next;
	}
	return lst;
}

/* Obtain function id from name AND number of params. Issues the
 * relevant error messages if errors are detected.
 */
static inline enum cnffuncid
funcName2ID(es_str_t *fname, unsigned short nParams)
{
	if(!es_strbufcmp(fname, (unsigned char*)"strlen", sizeof("strlen") - 1)) {
		if(nParams != 1) {
			parser_errmsg("number of parameters for strlen() must be one "
				      "but is %d.", nParams);
			return CNFFUNC_INVALID;
		}
		return CNFFUNC_STRLEN;
	} else if(!es_strbufcmp(fname, (unsigned char*)"getenv", sizeof("getenv") - 1)) {
		if(nParams != 1) {
			parser_errmsg("number of parameters for getenv() must be one "
				      "but is %d.", nParams);
			return CNFFUNC_INVALID;
		}
		return CNFFUNC_GETENV;
	} else if(!es_strbufcmp(fname, (unsigned char*)"tolower", sizeof("tolower") - 1)) {
		if(nParams != 1) {
			parser_errmsg("number of parameters for tolower() must be one "
				      "but is %d.", nParams);
			return CNFFUNC_INVALID;
		}
		return CNFFUNC_TOLOWER;
	} else if(!es_strbufcmp(fname, (unsigned char*)"cstr", sizeof("cstr") - 1)) {
		if(nParams != 1) {
			parser_errmsg("number of parameters for cstr() must be one "
				      "but is %d.", nParams);
			return CNFFUNC_INVALID;
		}
		return CNFFUNC_CSTR;
	} else if(!es_strbufcmp(fname, (unsigned char*)"cnum", sizeof("cnum") - 1)) {
		if(nParams != 1) {
			parser_errmsg("number of parameters for cnum() must be one "
				      "but is %d.", nParams);
			return CNFFUNC_INVALID;
		}
		return CNFFUNC_CNUM;
	} else if(!es_strbufcmp(fname, (unsigned char*)"re_match", sizeof("re_match") - 1)) {
		if(nParams != 2) {
			parser_errmsg("number of parameters for re_match() must be two "
				      "but is %d.", nParams);
			return CNFFUNC_INVALID;
		}
		return CNFFUNC_RE_MATCH;
	} else {
		return CNFFUNC_INVALID;
	}
}


static inline rsRetVal
initFunc_re_match(struct cnffunc *func)
{
	rsRetVal localRet;
	char *regex = NULL;
	regex_t *re;
	DEFiRet;

	func->funcdata = NULL;
	if(func->expr[1]->nodetype != 'S') {
		parser_errmsg("param 2 of re_match() must be a constant string");
		FINALIZE;
	}

	CHKmalloc(re = malloc(sizeof(regex_t)));
	func->funcdata = re;

	regex = es_str2cstr(((struct cnfstringval*) func->expr[1])->estr, NULL);
	
	if((localRet = objUse(regexp, LM_REGEXP_FILENAME)) == RS_RET_OK) {
		if(regexp.regcomp(re, (char*) regex, REG_EXTENDED) != 0) {
			parser_errmsg("cannot compile regex '%s'", regex);
			ABORT_FINALIZE(RS_RET_ERR);
		}
	} else { /* regexp object could not be loaded */
		parser_errmsg("could not load regex support - regex ignored");
		ABORT_FINALIZE(RS_RET_ERR);
	}

finalize_it:
	free(regex);
	RETiRet;
}

struct cnffunc *
cnffuncNew(es_str_t *fname, struct cnffparamlst* paramlst)
{
	struct cnffunc* func;
	struct cnffparamlst *param, *toDel;
	unsigned short i;
	unsigned short nParams;

	/* we first need to find out how many params we have */
	nParams = 0;
	for(param = paramlst ; param != NULL ; param = param->next)
		++nParams;
	if((func = malloc(sizeof(struct cnffunc) + (nParams * sizeof(struct cnfexp*))))
	   != NULL) {
		func->nodetype = 'F';
		func->fname = fname;
		func->nParams = nParams;
		func->fID = funcName2ID(fname, nParams);
		/* shuffle params over to array (access speed!) */
		param = paramlst;
		for(i = 0 ; i < nParams ; ++i) {
			func->expr[i] = param->expr;
			toDel = param;
			param = param->next;
			free(toDel);
		}
		/* some functions require special initialization */
		switch(func->fID) {
			case CNFFUNC_RE_MATCH:
				/* need to compile the regexp in param 2, so this MUST be a constant */
				initFunc_re_match(func);
				break;
			default:break;
		}
	}
	return func;
}

int
cnfDoInclude(char *name)
{
	char *cfgFile;
	char *finalName;
	unsigned i;
	int result;
	glob_t cfgFiles;
	struct stat fileInfo;
	char nameBuf[MAXFNAME+1];

	finalName = name;
	if(stat(name, &fileInfo) == 0) {
		/* stat usually fails if we have a wildcard - so this does NOT indicate error! */
		if(S_ISDIR(fileInfo.st_mode)) {
			/* if we have a directory, we need to add "*" to get its files */
			snprintf(nameBuf, sizeof(nameBuf), "%s*", name);
			finalName = nameBuf;
		}
	}
	/* Use GLOB_MARK to append a trailing slash for directories. */
	/* Use GLOB_NOMAGIC to detect wildcards that match nothing. */
	result = glob(finalName, GLOB_MARK | GLOB_NOMAGIC, NULL, &cfgFiles);

	/* Silently ignore wildcards that match nothing */
	if(result == GLOB_NOMATCH) {
		return 1;
	}

	if(result == GLOB_NOSPACE || result == GLOB_ABORTED) {
		char errStr[1024];
		rs_strerror_r(errno, errStr, sizeof(errStr));
		parser_errmsg("error accessing config file or directory '%s': %s",
				finalName, errStr);
		return 1;
	}

	for(i = 0; i < cfgFiles.gl_pathc; i++) {
		cfgFile = cfgFiles.gl_pathv[i];

		if(stat(cfgFile, &fileInfo) != 0)
		{
			char errStr[1024];
			rs_strerror_r(errno, errStr, sizeof(errStr));
			parser_errmsg("error accessing config file or directory '%s': %s",
					cfgFile, errStr);
		}

		if(S_ISREG(fileInfo.st_mode)) { /* config file */
			dbgprintf("requested to include config file '%s'\n", cfgFile);
			cnfSetLexFile(cfgFile);
		} else if(S_ISDIR(fileInfo.st_mode)) { /* config directory */
			dbgprintf("requested to include directory '%s'\n", cfgFile);
			cnfDoInclude(cfgFile);
		} else {
			dbgprintf("warning: unable to process IncludeConfig directive '%s'\n", cfgFile);
		}
	}

	globfree(&cfgFiles);
	return 0;
}

void
varDelete(struct var *v)
{
	if(v->datatype == 'S')
		es_deleteStr(v->d.estr);
}

void
cnfparamvalsDestruct(struct cnfparamvals *paramvals, struct cnfparamblk *blk)
{
	int i;
	for(i = 0 ; i < blk->nParams ; ++i) {
		varDelete(&paramvals[i].val);
	}
	free(paramvals);
}

/* find the index (or -1!) for a config param by name. This is used to 
 * address the parameter array. Of course, we could use with static
 * indices, but that would create some extra bug potential. So we
 * resort to names. As we do this only during the initial config parsing
 * stage the (considerable!) extra overhead is OK. -- rgerhards, 2011-07-19
 */
int
cnfparamGetIdx(struct cnfparamblk *params, char *name)
{
	int i;
	for(i = 0 ; i < params->nParams ; ++i)
		if(!strcmp(params->descr[i].name, name))
			break;
	if(i == params->nParams)
		i = -1; /* not found */
	return i;
}


void
cstrPrint(char *text, es_str_t *estr)
{
	char *str;
	str = es_str2cstr(estr, NULL);
	dbgprintf("%s%s", text, str);
	free(str);
}

/* init must be called once before any parsing of the script files start */
rsRetVal
initRainerscript(void)
{
	DEFiRet;
	CHKiRet(objGetObjInterface(&obj));
finalize_it:
	RETiRet;
}

/* we need a function to check for octal digits */
static inline int
isodigit(uchar c)
{
	return(c >= '0' && c <= '7');
}

/**
 * Get numerical value of a hex digit. This is a helper function.
 * @param[in] c a character containing 0..9, A..Z, a..z anything else
 * is an (undetected) error.
 */
static inline int
hexDigitVal(char c)
{
	int r;
	if(c < 'A')
		r = c - '0';
	else if(c < 'a')
		r = c - 'A' + 10;
	else
		r = c - 'a' + 10;
	return r;
}

/* Handle the actual unescaping.
 * a helper to unescapeStr(), to help make the function easier to read.
 */
static inline void
doUnescape(unsigned char *c, int len, int *iSrc, int iDst)
{
	if(c[*iSrc] == '\\') {
		if(++(*iSrc) == len) {
			/* error, incomplete escape, treat as single char */
			c[iDst] = '\\';
		}
		/* regular case, unescape */
		switch(c[*iSrc]) {
		case 'a':
			c[iDst] = '\007';
			break;
		case 'b':
			c[iDst] = '\b';
			break;
		case 'f':
			c[iDst] = '\014';
			break;
		case 'n':
			c[iDst] = '\n';
			break;
		case 'r':
			c[iDst] = '\r';
			break;
		case 't':
			c[iDst] = '\t';
			break;
		case '\'':
			c[iDst] = '\'';
			break;
		case '"':
			c[iDst] = '"';
			break;
		case '?':
			c[iDst] = '?';
			break;
		case '$':
			c[iDst] = '$';
			break;
		case '\\':
			c[iDst] = '\\';
			break;
		case 'x':
			if(    (*iSrc)+2 >= len
			   || !isxdigit(c[(*iSrc)+1])
			   || !isxdigit(c[(*iSrc)+2])) {
				/* error, incomplete escape, use as is */
				c[iDst] = '\\';
				--(*iSrc);
			}
			c[iDst] = (hexDigitVal(c[(*iSrc)+1]) << 4) +
				  hexDigitVal(c[(*iSrc)+2]);
			*iSrc += 2;
			break;
		case '0': /* octal escape */
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
			if(    (*iSrc)+2 >= len
			   || !isodigit(c[(*iSrc)+1])
			   || !isodigit(c[(*iSrc)+2])) {
				/* error, incomplete escape, use as is */
				c[iDst] = '\\';
				--(*iSrc);
			}
			c[iDst] = ((c[(*iSrc)  ] - '0') << 6) +
			          ((c[(*iSrc)+1] - '0') << 3) +
			          ( c[(*iSrc)+2] - '0');
			*iSrc += 2;
			break;
		default:
			/* error, incomplete escape, indicate by '?' */
			c[iDst] = '?';
			break;
		}
	} else {
		/* regular character */
		c[iDst] = c[*iSrc];
	}
}

void
unescapeStr(uchar *s, int len)
{
	int iSrc, iDst;
	assert(s != NULL);

	/* scan for first escape sequence (if we are luky, there is none!) */
	iSrc = 0;
	while(iSrc < len && s[iSrc] != '\\')
		++iSrc;
	/* now we have a sequence or end of string. In any case, we process
	 * all remaining characters (maybe 0!) and unescape.
	 */
	if(iSrc != len) {
		iDst = iSrc;
		while(iSrc < len) {
			doUnescape(s, len, &iSrc, iDst);
			++iSrc;
			++iDst;
		}
		s[iDst] = '\0';
	}
}
