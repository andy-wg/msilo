/*
 * $Id$
 *
 * MSILO module
 *
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History
 * -------
 *
 * 2003-01-23: switched from t_uac to t_uac_dlg (dcm)
 * 2003-02-28: protocolization of t_uac_dlg completed (jiri)
 * 2003-03-11: updated to the new module interface (andrei)
 *             removed non-constant initializers to some strs (andrei)
 * 2003-03-16: flags parameter added (janakj)
 * 2003-04-05: default_uri #define used (jiri)
 * 2003-04-06: db_init removed from mod_init, will be called from child_init
 *             now (janakj)
 * 2003-04-07: m_dump takes a parameter which sets the way the outgoing URI
 *             is computed (dcm)
 * 2003-08-05 adapted to the new parse_content_type_hdr function (bogdan)
 * 2004-06-07 updated to the new DB api (andrei)
 * 2006-09-10 m_dump now checks if registering UA supports MESSAGE method (jh)
 * 2006-10-05 added max_messages module variable (jh)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>

#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../ut.h"
#include "../../timer.h"
#include "../../mem/shm_mem.h"
#include "../../db/db.h"
#include "../../parser/parse_from.h"
#include "../../parser/parse_content.h"
#include "../../parser/contact/parse_contact.h"
#include "../../parser/parse_allow.h"
#include "../../parser/parse_methods.h"
#include "../../resolve.h"
#include "../../usr_avp.h"
#include "../../mod_fix.h"

#include "../tm/tm_load.h"
#include "../../pt.h"

#include "ms_msg_list.h"
#include "msg_retry.h"
#include "msfuncs.h"

#define MAX_DEL_KEYS	1
#define MAX_PEEK_NUM	10
#define NR_KEYS			10
#define PH_SQL_BUF_LEN 2048
#define MSG_BODY_BUFF_LEN 2048

static str sc_mid      = str_init("id");        /* 0 */
static str sc_from     = str_init("src_addr");  /* 1 */
static str sc_to       = str_init("dst_addr");  /* 2 */
static str sc_uri_user = str_init("username");  /* 3 */
static str sc_uri_host = str_init("domain");    /* 4 */
static str sc_body     = str_init("body");      /* 5 */
static str sc_ctype    = str_init("ctype");     /* 6 */
static str sc_exp_time = str_init("exp_time");  /* 7 */
static str sc_inc_time = str_init("inc_time");  /* 8 */
static str sc_snd_time = str_init("snd_time");  /* 9 */

#define SET_STR_VAL(_str, _res, _r, _c)	\
	if (RES_ROWS(_res)[_r].values[_c].nul == 0) \
	{ \
		switch(RES_ROWS(_res)[_r].values[_c].type) \
		{ \
		case DB_STRING: \
			(_str).s=(char*)RES_ROWS(_res)[_r].values[_c].val.string_val; \
			(_str).len=strlen((_str).s); \
			break; \
		case DB_STR: \
			(_str).len=RES_ROWS(_res)[_r].values[_c].val.str_val.len; \
			(_str).s=(char*)RES_ROWS(_res)[_r].values[_c].val.str_val.s; \
			break; \
		case DB_BLOB: \
			(_str).len=RES_ROWS(_res)[_r].values[_c].val.blob_val.len; \
			(_str).s=(char*)RES_ROWS(_res)[_r].values[_c].val.blob_val.s; \
			break; \
		default: \
			(_str).len=0; \
			(_str).s=NULL; \
		} \
	}



#define S_TABLE_VERSION 6

/** database connection */
static db_con_t *db_con = NULL;
static db_func_t msilo_dbf;

/** precessed msg list - used for dumping the messages */
msg_list ml = NULL;
retry_list rl = NULL;

/** TM bind */
struct tm_binds tmb;

/** parameters */

static str ms_db_url = {NULL, 0};
static str ms_db_table = str_init("silo");
str  ms_reminder = {NULL, 0};
str  ms_outbound_proxy = {NULL, 0};

char*  ms_from = NULL; /*"sip:registrar@example.org";*/
char*  ms_contact = NULL; /*"Contact: <sip:registrar@example.org>\r\n";*/
char*  ms_content_type = NULL; /*"Content-Type: text/plain\r\n";*/
char*  ms_offline_message = NULL; /*"<em>I'm offline.</em>"*/
void**  ms_from_sp = NULL;
void**  ms_contact_sp = NULL;
void**  ms_content_type_sp = NULL;
void**  ms_offline_message_sp = NULL;

int  ms_expire_time = 259200;
int  ms_check_time = 60;
int  ms_retry_count = 0;
int  ms_send_time = 0;
int  ms_clean_period = 10;
int  ms_use_contact = 1;
int  ms_add_date = 1;
int  ms_max_messages = 0;

static str ms_snd_time_avp_param = {NULL, 0};
int ms_snd_time_avp_name = -1;
unsigned short ms_snd_time_avp_type;

str msg_type = str_init("MESSAGE");

/** module functions */
static int mod_init(void);
static int child_init(int);

static int m_store(struct sip_msg*, char*, char*);
static int m_dump(struct sip_msg*, char*, char*);

void destroy(void);

void m_clean_silo(unsigned int ticks, void *);
void m_send_ontimer(unsigned int ticks, void *);

int ms_reset_stime(int mid);

int check_message_support(struct sip_msg* msg);

/** TM callback function */
static void m_tm_callback( struct cell *t, int type, struct tmcb_params *ps);

// --------------------------------------------------------
/** Sender thread */
#define SENDER_THREAD_NUM 1
#define SENDER_THREAD_WAIT_MS 100
static volatile int senderThreadsRunning;
static int senderThreadWaiters;

pthread_mutex_t * pSenderThreadQueueCondMutex = NULL;
pthread_mutexattr_t senderThreadQueueCondMutexAttr;

pthread_cond_t * pSenderThreadQueueCond = NULL;
pthread_condattr_t senderThreadQueueCondAttr;

typedef struct t_senderThreadArg_ {
	int threadId;
	int rank;

} t_senderThreadArg, *senderThreadArg;
t_senderThreadArg senderThreadsArgs[SENDER_THREAD_NUM];
pthread_t senderThreads[SENDER_THREAD_NUM];

static int initSenderThreads(void);
static int destroySenderThreads(void);
static int initChildSenderThreads(void);
static int spawnSenderThreads(void);
static int terminateSenderThreads(void);
static void *senderThreadMain(void *varg);
static void signalNewTask(void);

// https://voipmagazine.wordpress.com/tag/extra-process/
int* sender_pid;
int pid = 0;
static int msg_process_prefork(void);
static int msg_process_postfork(void);
static void msg_process(int rank);

static proc_export_t procs[] = {
		// name, pre-fork, post-fork, function, number, flags
		{"MSG sender",  msg_process_prefork,  msg_process_postfork, msg_process, 1, PROC_FLAG_INITCHILD},
		{0,0,0,0,0,0}
};
// --------------------------------------------------------

static cmd_export_t cmds[]={
	{"m_store",  (cmd_function)m_store, 0, 0, 0,
		REQUEST_ROUTE | FAILURE_ROUTE},
	{"m_store",  (cmd_function)m_store, 1, fixup_spve_null, 0,
		REQUEST_ROUTE | FAILURE_ROUTE},
	{"m_dump",   (cmd_function)m_dump,  0, 0, 0,
		REQUEST_ROUTE},
	{"m_dump",   (cmd_function)m_dump,  1, fixup_spve_null, 0,
		REQUEST_ROUTE},
	{0,0,0,0,0,0}
};


static param_export_t params[]={
	{ "db_url",           STR_PARAM, &ms_db_url.s             },
	{ "db_table",         STR_PARAM, &ms_db_table.s           },
	{ "from_address",     STR_PARAM, &ms_from                 },
	{ "contact_hdr",      STR_PARAM, &ms_contact              },
	{ "content_type_hdr", STR_PARAM, &ms_content_type         },
	{ "offline_message",  STR_PARAM, &ms_offline_message      },
	{ "reminder",         STR_PARAM, &ms_reminder.s           },
	{ "outbound_proxy",   STR_PARAM, &ms_outbound_proxy.s     },
	{ "expire_time",      INT_PARAM, &ms_expire_time          },
	{ "check_time",       INT_PARAM, &ms_check_time           },
	{ "retry_count",      INT_PARAM, &ms_retry_count          },
	{ "send_time",        INT_PARAM, &ms_send_time            },
	{ "clean_period",     INT_PARAM, &ms_clean_period         },
	{ "use_contact",      INT_PARAM, &ms_use_contact          },
	{ "sc_mid",           STR_PARAM, &sc_mid.s                },
	{ "sc_from",          STR_PARAM, &sc_from.s               },
	{ "sc_to",            STR_PARAM, &sc_to.s                 },
	{ "sc_uri_user",      STR_PARAM, &sc_uri_user.s           },
	{ "sc_uri_host",      STR_PARAM, &sc_uri_host.s           },
	{ "sc_body",          STR_PARAM, &sc_body.s               },
	{ "sc_ctype",         STR_PARAM, &sc_ctype.s              },
	{ "sc_exp_time",      STR_PARAM, &sc_exp_time.s           },
	{ "sc_inc_time",      STR_PARAM, &sc_inc_time.s           },
	{ "sc_snd_time",      STR_PARAM, &sc_snd_time.s           },
	{ "snd_time_avp",     STR_PARAM, &ms_snd_time_avp_param.s },
	{ "add_date",         INT_PARAM, &ms_add_date             },
	{ "max_messages",     INT_PARAM, &ms_max_messages         },
	{ 0,0,0 }
};

#ifdef STATISTICS
#include "../../statistics.h"

stat_var* ms_stored_msgs;
stat_var* ms_dumped_msgs;
stat_var* ms_failed_msgs;
stat_var* ms_dumped_rmds;
stat_var* ms_failed_rmds;

static stat_export_t msilo_stats[] = {
	{"stored_messages" ,  0,  &ms_stored_msgs  },
	{"dumped_messages" ,  0,  &ms_dumped_msgs  },
	{"failed_messages" ,  0,  &ms_failed_msgs  },
	{"dumped_reminders" , 0,  &ms_dumped_rmds  },
	{"failed_reminders" , 0,  &ms_failed_rmds  },
	{0,0,0}
};

#endif
/** module exports */
struct module_exports exports= {
	"msilo",    /* module id */
	MODULE_VERSION,
	DEFAULT_DLFLAGS, /* dlopen flags */
	cmds,       /* module's exported functions */
	params,     /* module's exported parameters */
#ifdef STATISTICS
	msilo_stats,
#else
	0,          /* exported statistics */
#endif
	0,          /* exported MI functions */
	0,          /* exported pseudo-variables */
	procs,          /* extra processes */
	mod_init,   /* module initialization function */
	(response_function) 0,       /* response handler */
	(destroy_function) destroy,  /* module destroy function */
	child_init  /* per-child init function */
};

/**
 * init module function
 */
static int mod_init(void)
{
	pv_spec_t avp_spec;

	init_db_url( ms_db_url , 0 /*cannot be null*/);
	ms_db_table.len = strlen (ms_db_table.s);
	sc_mid.len = strlen(sc_mid.s);
	sc_from.len = strlen(sc_from.s);
	sc_to.len = strlen(sc_to.s);
	sc_uri_user.len = strlen(sc_uri_user.s);
	sc_uri_host.len = strlen(sc_uri_host.s);
	sc_body.len = strlen(sc_body.s);
	sc_ctype.len = strlen(sc_ctype.s);
	sc_exp_time.len = strlen(sc_exp_time.s);
	sc_inc_time.len = strlen(sc_inc_time.s);
	sc_snd_time.len = strlen(sc_snd_time.s);
	if (ms_snd_time_avp_param.s)
		ms_snd_time_avp_param.len = strlen(ms_snd_time_avp_param.s);

	LM_DBG("initializing ...\n");

	/* binding to mysql module  */
	if (db_bind_mod(&ms_db_url, &msilo_dbf))
	{
		LM_INFO("database module not found\n");
		return -1;
	}

	if (!DB_CAPABILITY(msilo_dbf, DB_CAP_ALL)) {
		LM_ERR("database module does not implement "
		    "all functions needed by the module\n");
		return -1;
	}

	if (ms_snd_time_avp_param.s && ms_snd_time_avp_param.len > 0) {
		if (pv_parse_spec(&ms_snd_time_avp_param, &avp_spec)==0
				|| avp_spec.type!=PVT_AVP) {
			LM_ERR("malformed or non AVP %.*s AVP definition\n",
					ms_snd_time_avp_param.len, ms_snd_time_avp_param.s);
			return -1;
		}

		if(pv_get_avp_name(0, &(avp_spec.pvp), &ms_snd_time_avp_name,
					&ms_snd_time_avp_type)!=0)
		{
			LM_ERR("[%.*s]- invalid AVP definition\n",
					ms_snd_time_avp_param.len, ms_snd_time_avp_param.s);
			return -1;
		}
	}

	db_con = msilo_dbf.init(&ms_db_url);
	if (!db_con)
	{
		LM_ERR("failed to connect to the database\n");
		return -1;
	}

	if(db_check_table_version(&msilo_dbf, db_con, &ms_db_table, S_TABLE_VERSION) < 0) {
		LM_ERR("error during table version check.\n");
		return -1;
	}
	if(db_con)
		msilo_dbf.close(db_con);
	db_con = NULL;

	/* load the TM API */
	if (load_tm_api(&tmb)!=0) {
		LM_ERR("can't load TM API\n");
		return -1;
	}

	if(ms_from!=NULL)
	{
		ms_from_sp = (void**)pkg_malloc(sizeof(void*));
		if(ms_from_sp==NULL)
		{
			LM_ERR("no more pkg\n");
			return -1;
		}
		*ms_from_sp = (void*)ms_from;
		if(fixup_spve_null(ms_from_sp, 1)!=0)
		{
			LM_ERR("bad contact parameter\n");
			return -1;
		}
	}
	if(ms_contact!=NULL)
	{
		ms_contact_sp = (void**)pkg_malloc(sizeof(void*));
		if(ms_contact_sp==NULL)
		{
			LM_ERR("no more pkg\n");
			return -1;
		}
		*ms_contact_sp = (void*)ms_contact;
		if(fixup_spve_null(ms_contact_sp, 1)!=0)
		{
			LM_ERR("bad contact parameter\n");
			return -1;
		}
	}
	if(ms_content_type!=NULL)
	{
		ms_content_type_sp = (void**)pkg_malloc(sizeof(void*));
		if(ms_content_type_sp==NULL)
		{
			LM_ERR("no more pkg\n");
			return -1;
		}
		*ms_content_type_sp = (void*)ms_content_type;
		if(fixup_spve_null(ms_content_type_sp, 1)!=0)
		{
			LM_ERR("bad content_type parameter\n");
			return -1;
		}
	}
	if(ms_offline_message!=NULL)
	{
		ms_offline_message_sp = (void**)pkg_malloc(sizeof(void*));
		if(ms_offline_message_sp==NULL)
		{
			LM_ERR("no more pkg\n");
			return -1;
		}
		*ms_offline_message_sp = (void*)ms_offline_message;
		if(fixup_spve_null(ms_offline_message_sp, 1)!=0)
		{
			LM_ERR("bad offline_message parameter\n");
			return -1;
		}
	}
	if(ms_offline_message!=NULL && ms_content_type==NULL)
	{
		LM_ERR("content_type parameter must be set\n");
		return -1;
	}

	ml = msg_list_init();
	if(ml==NULL)
	{
		LM_ERR("can't initialize msg list\n");
		return -1;
	}

	rl = retry_list_init();
	if(rl==NULL)
	{
		LM_ERR("can't initialize retry list\n");
		return -1;
	}

	if(ms_check_time<0)
	{
		LM_ERR("bad check time value\n");
		return -1;
	}
	register_timer( "msilo-clean", m_clean_silo, 0, ms_check_time);
	if(ms_send_time>0 && ms_reminder.s!=NULL)
		register_timer( "msilo-reminder", m_send_ontimer, 0, ms_send_time);

	if(ms_reminder.s!=NULL)
		ms_reminder.len = strlen(ms_reminder.s);
	if(ms_outbound_proxy.s!=NULL)
		ms_outbound_proxy.len = strlen(ms_outbound_proxy.s);

	// Sender thread startup.
	return initSenderThreads() == 0 ? 0 : -1;
}

/**
 * Initialize children
 */
static int child_init(int rank)
{
	LM_DBG("rank #%d / pid <%d>\n", rank, getpid());
	if (msilo_dbf.init==0)
	{
		LM_CRIT("database not bound\n");
		return -1;
	}
	db_con = msilo_dbf.init(&ms_db_url);
	if (!db_con)
	{
		LM_ERR("child %d: failed to connect database\n", rank);
		return -1;
	}
	else
	{
		if (msilo_dbf.use_table(db_con, &ms_db_table) < 0) {
			LM_ERR("child %d: failed in use_table\n", rank);
			return -1;
		}

		LM_DBG("#%d database connection opened successfully\n", rank);
	}

	return 0;

	// Sender threads.
	//return initChildSenderThreads() == 0 ? 0 : -1;
}

/**
 * store message
 * mode = "0" -- look for outgoing URI starting with new_uri
 * 		= "1" -- look for outgoing URI starting with r-uri
 * 		= "2" -- look for outgoing URI only at to header
 */

static int m_store(struct sip_msg* msg, char* owner, char* s2)
{
	str body, str_hdr, ctaddr;
	struct to_body *pto, *pfrom;
	struct sip_uri puri;
	str duri, owner_s;
	db_key_t db_keys[NR_KEYS-1];
	db_val_t db_vals[NR_KEYS-1];
	db_key_t db_cols[1];
	db_res_t* res = NULL;

	int nr_keys = 0, val, lexpire;
	content_type_t ctype;
#define MS_BUF1_SIZE	MSG_BODY_BUFF_LEN
	static char ms_buf1[MS_BUF1_SIZE];
	int mime;
	str notify_from;
	str notify_body;
	str notify_ctype;
	str notify_contact;

	int_str        avp_value;
	struct usr_avp *avp;

	LM_DBG("------------ start ------------\n");

	/* get message body - after that whole SIP MESSAGE is parsed */
	if ( get_body( msg, &body)!=0 || body.len==0)
	{
		LM_ERR("cannot extract body from msg\n");
		goto error;
	}

	/* get TO URI */
	if(!msg->to || !msg->to->body.s)
	{
	    LM_ERR("cannot find 'to' header!\n");
	    goto error;
	}

	pto = get_to(msg);
	if (pto == NULL || pto->error != PARSE_OK) {
		LM_ERR("failed to parse TO header\n");
		goto error;
	}

	/* get the owner */
	memset(&puri, 0, sizeof(struct sip_uri));
	if(owner)
	{
		if(fixup_get_svalue(msg, (gparam_p)owner, &owner_s)!=0)
		{
			LM_ERR("invalid owner uri parameter");
			return -1;
		}
		if(parse_uri(owner_s.s, owner_s.len, &puri)!=0)
		{
			LM_ERR("bad owner SIP address!\n");
			goto error;
		} else {
			LM_DBG("using user id [%.*s]\n", owner_s.len, owner_s.s);
		}
	} else { /* get it from R-URI */
		if(msg->new_uri.len <= 0)
		{
			if(msg->first_line.u.request.uri.len <= 0)
			{
				LM_ERR("bad dst URI!\n");
				goto error;
			}
			duri = msg->first_line.u.request.uri;
		} else {
			duri = msg->new_uri;
		}
		LM_DBG("NEW R-URI found - check if is AoR!\n");
		if(parse_uri(duri.s, duri.len, &puri)!=0)
		{
			LM_ERR("bad dst R-URI!!\n");
			goto error;
		}
	}
	if(puri.user.len<=0)
	{
		LM_ERR("no username for owner\n");
		goto error;
	}

	db_keys[nr_keys] = &sc_uri_user;

	db_vals[nr_keys].type = DB_STR;
	db_vals[nr_keys].nul = 0;
	db_vals[nr_keys].val.str_val.s = puri.user.s;
	db_vals[nr_keys].val.str_val.len = puri.user.len;

	nr_keys++;

	db_keys[nr_keys] = &sc_uri_host;

	db_vals[nr_keys].type = DB_STR;
	db_vals[nr_keys].nul = 0;
	db_vals[nr_keys].val.str_val.s = puri.host.s;
	db_vals[nr_keys].val.str_val.len = puri.host.len;

	nr_keys++;

	if (msilo_dbf.use_table(db_con, &ms_db_table) < 0)
	{
		LM_ERR("failed to use_table\n");
		goto error;
	}

	if (ms_max_messages > 0) {
	    db_cols[0] = &sc_inc_time;
	    if (msilo_dbf.query(db_con, db_keys, 0, db_vals, db_cols,
				2, 1, 0, &res) < 0 ) {
			LM_ERR("failed to query the database\n");
			return -1;
	    }
	    if (RES_ROW_N(res) >= ms_max_messages) {
			LM_ERR("too many messages for AoR '%.*s@%.*s'\n",
			    puri.user.len, puri.user.s, puri.host.len, puri.host.s);
 	        msilo_dbf.free_result(db_con, res);
		return -1;
	    }
	    msilo_dbf.free_result(db_con, res);
	}

	/* Set To key */
	db_keys[nr_keys] = &sc_to;

	db_vals[nr_keys].type = DB_STR;
	db_vals[nr_keys].nul = 0;
	db_vals[nr_keys].val.str_val.s = pto->uri.s;
	db_vals[nr_keys].val.str_val.len = pto->uri.len;

	nr_keys++;

	/* check FROM URI */
	if(!msg->from || !msg->from->body.s)
	{
		LM_ERR("cannot find 'from' header!\n");
		goto error;
	}

	if(msg->from->parsed == NULL)
	{
		LM_DBG("'From' header not parsed\n");
		/* parsing from header */
		if ( parse_from_header( msg )<0 )
		{
			LM_ERR("cannot parse From header\n");
			goto error;
		}
	}
	pfrom = (struct to_body*)msg->from->parsed;
	LM_DBG("'From' header: <%.*s>\n", pfrom->uri.len, pfrom->uri.s);

	db_keys[nr_keys] = &sc_from;

	db_vals[nr_keys].type = DB_STR;
	db_vals[nr_keys].nul = 0;
	db_vals[nr_keys].val.str_val.s = pfrom->uri.s;
	db_vals[nr_keys].val.str_val.len = pfrom->uri.len;

	nr_keys++;

	/* add the message's body in SQL query */

	db_keys[nr_keys] = &sc_body;

	db_vals[nr_keys].type = DB_BLOB;
	db_vals[nr_keys].nul = 0;
	db_vals[nr_keys].val.blob_val.s = body.s;
	db_vals[nr_keys].val.blob_val.len = body.len;

	nr_keys++;

	lexpire = ms_expire_time;
	/* add 'content-type' -- parse the content-type header */
	if ((mime=parse_content_type_hdr(msg))<1 )
	{
		LM_ERR("cannot parse Content-Type header\n");
		goto error;
	}

	db_keys[nr_keys]      = &sc_ctype;
	db_vals[nr_keys].type = DB_STR;
	db_vals[nr_keys].nul  = 0;
	db_vals[nr_keys].val.str_val.s   = "text/plain";
	db_vals[nr_keys].val.str_val.len = 10;

	/** check the content-type value */
	if( mime!=(TYPE_TEXT<<16)+SUBTYPE_PLAIN
		&& mime!=(TYPE_MESSAGE<<16)+SUBTYPE_CPIM )
	{
		if(m_extract_content_type(msg->content_type->body.s,
				msg->content_type->body.len, &ctype, CT_TYPE) != -1)
		{
			LM_DBG("'content-type' found\n");
			db_vals[nr_keys].val.str_val.s   = ctype.type.s;
			db_vals[nr_keys].val.str_val.len = ctype.type.len;
		}
	}
	nr_keys++;

	/* check 'expires' -- no more parsing - already done by get_body() */
	if(msg->expires && msg->expires->body.len > 0)
	{
		LM_DBG("'expires' found\n");
		val = atoi(msg->expires->body.s);
		if(val > 0)
			lexpire = (ms_expire_time<=val)?ms_expire_time:val;
	}

	/* current time */
	val = (int)time(NULL);

	/* add expiration time */
	db_keys[nr_keys] = &sc_exp_time;
	db_vals[nr_keys].type = DB_INT;
	db_vals[nr_keys].nul = 0;
	db_vals[nr_keys].val.int_val = val+lexpire;
	nr_keys++;

	/* add incoming time */
	db_keys[nr_keys] = &sc_inc_time;
	db_vals[nr_keys].type = DB_INT;
	db_vals[nr_keys].nul = 0;
	db_vals[nr_keys].val.int_val = val;
	nr_keys++;

	/* add sending time */
	db_keys[nr_keys] = &sc_snd_time;
	db_vals[nr_keys].type = DB_INT;
	db_vals[nr_keys].nul = 0;
	db_vals[nr_keys].val.int_val = 0;
	if(ms_snd_time_avp_name >= 0)
	{
		avp = NULL;
		avp=search_first_avp(ms_snd_time_avp_type, ms_snd_time_avp_name,
				&avp_value, 0);
		if(avp!=NULL && is_avp_str_val(avp))
		{
			if(ms_extract_time(&avp_value.s, &db_vals[nr_keys].val.int_val)!=0)
				db_vals[nr_keys].val.int_val = 0;
		}
	}
	nr_keys++;

	if(msilo_dbf.insert(db_con, db_keys, db_vals, nr_keys) < 0)
	{
		LM_ERR("failed to store message\n");
		goto error;
	}
	LM_INFO("message stored. T:<%.*s> F:<%.*s>\n",
		pto->uri.len, pto->uri.s, pfrom->uri.len, pfrom->uri.s);

#ifdef STATISTICS
	update_stat(ms_stored_msgs, 1);
#endif

	if(ms_from==NULL || ms_offline_message == NULL)
		goto done;

	LM_DBG("sending info message.\n");
	if(fixup_get_svalue(msg, (gparam_p)*ms_from_sp, &notify_from)!=0
			|| notify_from.len<=0)
	{
		LM_WARN("cannot get notification From address\n");
		goto done;
	}
	if(fixup_get_svalue(msg, (gparam_p)*ms_offline_message_sp, &notify_body)!=0
			|| notify_body.len<=0)
	{
		LM_WARN("cannot get notification body\n");
		goto done;
	}
	if(fixup_get_svalue(msg, (gparam_p)*ms_content_type_sp, &notify_ctype)!=0
			|| notify_ctype.len<=0)
	{
		LM_WARN("cannot get notification content type\n");
		goto done;
	}

	if(ms_contact!=NULL && fixup_get_svalue(msg, (gparam_p)*ms_contact_sp,
				&notify_contact)==0 && notify_contact.len>0)
	{
		if(notify_contact.len+notify_ctype.len>=MS_BUF1_SIZE)
		{
			LM_WARN("insufficient buffer to build notification headers\n");
			goto done;
		}
		memcpy(ms_buf1, notify_contact.s, notify_contact.len);
		memcpy(ms_buf1+notify_contact.len, notify_ctype.s, notify_ctype.len);
		str_hdr.s = ms_buf1;
		str_hdr.len = notify_contact.len + notify_ctype.len;
	} else {
		str_hdr = notify_ctype;
	}

	/* look for Contact header -- must be parsed by now*/
	ctaddr.s = NULL;
	if(ms_use_contact && msg->contact!=NULL && msg->contact->body.s!=NULL
			&& msg->contact->body.len > 0)
	{
		LM_DBG("contact header found\n");
		if((msg->contact->parsed!=NULL
			&& ((contact_body_t*)(msg->contact->parsed))->contacts!=NULL)
			|| (parse_contact(msg->contact)==0
			&& msg->contact->parsed!=NULL
			&& ((contact_body_t*)(msg->contact->parsed))->contacts!=NULL))
		{
			LM_DBG("using contact header for info msg\n");
			ctaddr.s =
			((contact_body_t*)(msg->contact->parsed))->contacts->uri.s;
			ctaddr.len =
			((contact_body_t*)(msg->contact->parsed))->contacts->uri.len;

			if(!ctaddr.s || ctaddr.len < 6 || strncasecmp(ctaddr.s, "sip:", 4)
				|| ctaddr.s[4]==' ')
				ctaddr.s = NULL;
			else
				LM_DBG("feedback contact [%.*s]\n",	ctaddr.len,ctaddr.s);
		}
	}

	tmb.t_request(&msg_type,  /* Type of the message */
			(ctaddr.s)?&ctaddr:&pfrom->uri,    /* Request-URI */
			&pfrom->uri,      /* To */
			&notify_from,     /* From */
			&str_hdr,         /* Optional headers including CRLF */
			&notify_body,     /* Message body */
			(ms_outbound_proxy.s)?&ms_outbound_proxy:0, /* outbound uri */
			NULL,             /* Callback function */
			NULL,             /* Callback parameter */
			NULL
		);

done:
	return 1;
error:
	return -1;
}

/**
 * dump message
 */
static int m_dump(struct sip_msg* msg, char* owner, char* str2)
{
	struct to_body *pto = NULL;
	db_key_t db_keys[3];
	db_key_t ob_key;
	db_op_t  db_ops[3];
	db_val_t db_vals[3];
	db_key_t db_cols[6];
	db_res_t* db_res = NULL;
	int i, db_no_cols = 6, db_no_keys = 3, mid; //n;
//	static char hdr_buf[1024];
//	static char body_buf[1024];
	struct sip_uri puri;
	str owner_s;

//	str str_vals[4], hdr_str , body_str;
//	time_t rtime;
	time_t dumpId;

	/* init */
	ob_key = &sc_mid;

	db_keys[0]=&sc_uri_user;
	db_keys[1]=&sc_uri_host;
	db_keys[2]=&sc_snd_time;
	db_ops[0]=OP_EQ;
	db_ops[1]=OP_EQ;
	db_ops[2]=OP_EQ;

	db_cols[0]=&sc_mid;
	db_cols[1]=&sc_from;
	db_cols[2]=&sc_to;
	db_cols[3]=&sc_body;
	db_cols[4]=&sc_ctype;
	db_cols[5]=&sc_inc_time;


	LM_DBG("------------ start ------------\n");
//	hdr_str.s=hdr_buf;
//	hdr_str.len=1024;
//	body_str.s=body_buf;
//	body_str.len=1024;

	/* check for TO header */
	if(msg->to==NULL && (parse_headers(msg, HDR_TO_F, 0)==-1
				|| msg->to==NULL || msg->to->body.s==NULL))
	{
		LM_ERR("cannot find TO HEADER!\n");
		goto error;
	}

	pto = get_to(msg);
	if (pto == NULL || pto->error != PARSE_OK) {
		LM_ERR("failed to parse TO header\n");
		goto error;
	}

	/**
	 * check if has expires=0 (REGISTER)
	 */
	if(parse_headers(msg, HDR_EXPIRES_F, 0) >= 0)
	{
		/* check 'expires' > 0 */
		if(msg->expires && msg->expires->body.len > 0)
		{
			i = atoi(msg->expires->body.s);
			if(i <= 0)
			{ /* user goes offline */
				LM_DBG("user <%.*s> goes offline - expires=%d\n",
						pto->uri.len, pto->uri.s, i);
				goto error;
			}
			else
				LM_DBG("user <%.*s> online - expires=%d\n",
						pto->uri.len, pto->uri.s, i);
		}
	}
	else
	{
		LM_ERR("failed to parse 'expires'\n");
		goto error;
	}

	if (check_message_support(msg)!=0) {
	    LM_DBG("MESSAGE method not supported\n");
	    return -1;
	}

	/* get the owner */
	memset(&puri, 0, sizeof(struct sip_uri));
	if(owner)
	{
		if(fixup_get_svalue(msg, (gparam_p)owner, &owner_s)!=0)
		{
			LM_ERR("invalid owner uri parameter");
			return -1;
		}
		if(parse_uri(owner_s.s, owner_s.len, &puri)!=0)
		{
			LM_ERR("bad owner SIP address!\n");
			goto error;
		} else {
			LM_DBG("using user id [%.*s]\n", owner_s.len, owner_s.s);
		}
	} else { /* get it from  To URI */
		if(parse_uri(pto->uri.s, pto->uri.len, &puri)!=0)
		{
			LM_ERR("bad owner To URI!\n");
			goto error;
		}
	}
	if(puri.user.len<=0 || puri.user.s==NULL
			|| puri.host.len<=0 || puri.host.s==NULL)
	{
		LM_ERR("bad owner URI!\n");
		goto error;
	}

	time(&dumpId);

	db_vals[0].type = DB_STR;
	db_vals[0].nul = 0;
	db_vals[0].val.str_val.s = puri.user.s;
	db_vals[0].val.str_val.len = puri.user.len;

	db_vals[1].type = DB_STR;
	db_vals[1].nul = 0;
	db_vals[1].val.str_val.s = puri.host.s;
	db_vals[1].val.str_val.len = puri.host.len;

	db_vals[2].type = DB_INT;
	db_vals[2].nul = 0;
	db_vals[2].val.int_val = 0;

	if (msilo_dbf.use_table(db_con, &ms_db_table) < 0)
	{
		LM_ERR("failed to use_table\n");
		goto error;
	}

	if((msilo_dbf.query(db_con,db_keys,db_ops,db_vals,db_cols,db_no_keys,
				db_no_cols, ob_key, &db_res)!=0) || (RES_ROW_N(db_res) <= 0))
	{
		LM_DBG("no stored message for <%.*s>!\n", pto->uri.len,	pto->uri.s);
		goto done;
	}

	LM_INFO("dumping [%d] messages for <%.*s>!!!\n",
			RES_ROW_N(db_res), pto->uri.len, pto->uri.s);

	for(i = 0; i < RES_ROW_N(db_res); i++)
	{
		int cur_flags = 0;
		int cur_retry = 0;
		mid =  RES_ROWS(db_res)[i].values[0].val.int_val;
		if(msg_list_check_msg(ml, mid, &cur_retry, &cur_flags))
		{
			LM_INFO("message[%d] mid=%d already sent. Flags: %d, retry: %d\n", i, mid, cur_flags, cur_retry);
			continue;
		}

		// Add to the retry queue, signal to the executor.
		retry_add_element(rl, mid, 0, dumpId + 3);

//		memset(str_vals, 0, 4*sizeof(str));
//		SET_STR_VAL(str_vals[0], db_res, i, 1); /* from */
//		SET_STR_VAL(str_vals[1], db_res, i, 2); /* to */
//		SET_STR_VAL(str_vals[2], db_res, i, 3); /* body */
//		SET_STR_VAL(str_vals[3], db_res, i, 4); /* ctype */
//		rtime =
//			(time_t)RES_ROWS(db_res)[i].values[5/*inc time*/].val.int_val;
//
//		hdr_str.len = 1024;
//		if(m_build_headers(&hdr_str, str_vals[3] /*ctype*/,
//				str_vals[0]/*from*/, rtime /*Date*/, (long) (dumpId * 1000l)) < 0)
//		{
//			LM_ERR("headers building failed [%d]\n", mid);
//			if (msilo_dbf.free_result(db_con, db_res) < 0)
//				LM_ERR("failed to free the query result\n");
//			msg_list_set_flag(ml, mid, MS_MSG_ERRO);
//			goto error;
//		}
//
//		LM_DBG("msg [%d-%d] for: %.*s\n", i+1, mid,	pto->uri.len, pto->uri.s);
//
//		/** sending using TM function: t_uac */
//		body_str.len = 1024;
//		n = m_build_body(&body_str, rtime, str_vals[2/*body*/], 0);
//		if(n<0)
//			LM_DBG("sending simple body\n");
//		else
//			LM_DBG("sending composed body\n");
//
//			int res = tmb.t_request(&msg_type,  /* Type of the message */
//					&str_vals[1],     /* Request-URI (To) */
//					&str_vals[1],     /* To */
//					&str_vals[0],     /* From */
//					&hdr_str,         /* Optional headers including CRLF */
//					(n<0)?&str_vals[2]:&body_str, /* Message body */
//					(ms_outbound_proxy.s)?&ms_outbound_proxy:0,
//									/* outbound uri */
//					m_tm_callback,    /* Callback function */
//					(void*)(long)mid, /* Callback parameter */
//					NULL
//				);
//
//			if (res < 0){
//				LM_WARN("message sending failed [%d], res=%d messages for <%.*s>!\n",
//						mid, res, pto->uri.len, pto->uri.s);
//			}
	}

	signalNewTask();

done:
	/**
	 * Free the result because we don't need it
	 * anymore
	 */
	if (db_res!=NULL && msilo_dbf.free_result(db_con, db_res) < 0)
		LM_ERR("failed to free result of query\n");

	return 1;
error:
	return -1;
}

/**
 * - cleaning up the messages that got reply
 * - delete expired messages from database
 */
void m_clean_silo(unsigned int ticks, void *param)
{
	msg_list_el mle = NULL, p;
	db_key_t db_keys[MAX_DEL_KEYS];
	db_val_t db_vals[MAX_DEL_KEYS];
	db_op_t  db_ops[1] = { OP_LEQ };
	int n;
	long deletedTotal = 0;
	long iters = 0;

	LM_DBG("cleaning stored messages - %d\n", ticks);

	msg_list_check(ml); // Separates message with flag (DONE | ERROR) in sent_list to the done_list.
	mle = p = msg_list_reset(ml); // Extracts done_list and returns it here.
	n = 0;
	while(p)
	{
		if(p->flag & MS_MSG_DONE)
		{
			iters += 1;
#ifdef STATISTICS
			if(p->flag & MS_MSG_TSND)
				update_stat(ms_dumped_msgs, 1);
			else
				update_stat(ms_dumped_rmds, 1);
#endif

			db_keys[n] = &sc_mid;
			db_vals[n].type = DB_INT;
			db_vals[n].nul = 0;
			db_vals[n].val.int_val = p->msgid;
			LM_DBG("cleaning sent message [%d]\n", p->msgid);
			n++;
			if(n==MAX_DEL_KEYS)
			{
				if (msilo_dbf.delete(db_con, db_keys, NULL, db_vals, n) < 0)
					LM_ERR("failed to clean %d messages.\n",n);
				else
					deletedTotal += n;
				n = 0;
			}
		}
		if((p->flag & MS_MSG_ERRO) && (p->flag & MS_MSG_TSND))
		{ /* set snd time to 0 */
			ms_reset_stime(p->msgid);
#ifdef STATISTICS
			update_stat(ms_failed_rmds, 1);
#endif

		}
#ifdef STATISTICS
		if((p->flag & MS_MSG_ERRO) && !(p->flag & MS_MSG_TSND))
			update_stat(ms_failed_msgs, 1);
#endif
		p = p->next;
	}
	if(n>0)
	{
		if (msilo_dbf.delete(db_con, db_keys, NULL, db_vals, n) < 0)
			LM_ERR("failed to clean %d messages\n", n);
		else
			deletedTotal += n;
		n = 0;
	}

	msg_list_el_free_all(mle);
	if (deletedTotal > 0 || iters > 0){
		LM_INFO("Totaly cleaned messages: %ld, ticks: %d, iters: %ld\n", deletedTotal, ticks, iters);
	}

	/* cleaning expired messages */
	if(ticks%(ms_check_time*ms_clean_period)<ms_check_time)
	{
		LM_DBG("cleaning expired messages\n");
		db_keys[0] = &sc_exp_time;
		db_vals[0].type = DB_INT;
		db_vals[0].nul = 0;
		db_vals[0].val.int_val = (int)time(NULL);
		if (msilo_dbf.delete(db_con, db_keys, db_ops, db_vals, 1) < 0)
			LM_DBG("ERROR cleaning expired messages\n");
	}
}

/**
 * destroy function
 */
void destroy(void)
{
	LM_DBG("msilo destroy module ...\n");
	destroySenderThreads();

	msg_list_free(ml);
	retry_list_free(rl);

	if(db_con && msilo_dbf.close)
		msilo_dbf.close(db_con);
}

void m_send_ontimer(unsigned int ticks, void *param)
{
	db_key_t db_keys[2];
	db_op_t  db_ops[2];
	db_val_t db_vals[2];
	db_key_t db_cols[6];
	db_res_t* db_res = NULL;
	int i, db_no_cols = 6, db_no_keys = 2, mid, n;
	static char hdr_buf[1024];
	static char uri_buf[1024];
	static char body_buf[MSG_BODY_BUFF_LEN];
	str puri;
	time_t ttime;

	str str_vals[4], hdr_str , body_str;
	time_t stime;
	time_t dumpId;

	if(ms_reminder.s==NULL)
	{
		LM_WARN("reminder address null\n");
		return;
	}

	/* init */
	db_keys[0]=&sc_snd_time;
	db_keys[1]=&sc_snd_time;
	db_ops[0]=OP_NEQ;
	db_ops[1]=OP_LEQ;

	db_cols[0]=&sc_mid;
	db_cols[1]=&sc_uri_user;
	db_cols[2]=&sc_uri_host;
	db_cols[3]=&sc_body;
	db_cols[4]=&sc_ctype;
	db_cols[5]=&sc_snd_time;


	LM_DBG("------------ start ------------\n");
	hdr_str.s=hdr_buf;
	hdr_str.len=1024;
	body_str.s=body_buf;
	body_str.len=MSG_BODY_BUFF_LEN;

	db_vals[0].type = DB_INT;
	db_vals[0].nul = 0;
	db_vals[0].val.int_val = 0;

	db_vals[1].type = DB_INT;
	db_vals[1].nul = 0;
	ttime = time(NULL);
	db_vals[1].val.int_val = (int)ttime;

	if (msilo_dbf.use_table(db_con, &ms_db_table) < 0)
	{
		LM_ERR("failed to use_table\n");
		return;
	}

	if((msilo_dbf.query(db_con,db_keys,db_ops,db_vals,db_cols,db_no_keys,
				db_no_cols, NULL,&db_res)!=0) || (RES_ROW_N(db_res) <= 0))
	{
		LM_DBG("no message for <%.*s>!\n", 24, ctime((const time_t*)&ttime));
		goto done;
	}

	time(&dumpId);
	LM_DBG("dumping [%d] messages for <%.*s>!!!\n", RES_ROW_N(db_res), 24,
			ctime((const time_t*)&ttime));

	for(i = 0; i < RES_ROW_N(db_res); i++)
	{
		mid =  RES_ROWS(db_res)[i].values[0].val.int_val;
		if(msg_list_check_msg(ml, mid, NULL, NULL))
		{
			LM_DBG("message[%d] mid=%d already sent.\n", i, mid);
			continue;
		}

		memset(str_vals, 0, 4*sizeof(str));
		SET_STR_VAL(str_vals[0], db_res, i, 1); /* user */
		SET_STR_VAL(str_vals[1], db_res, i, 2); /* host */
		SET_STR_VAL(str_vals[2], db_res, i, 3); /* body */
		SET_STR_VAL(str_vals[3], db_res, i, 4); /* ctype */

		hdr_str.len = 1024;
		if(m_build_headers(&hdr_str, str_vals[3] /*ctype*/,
				ms_reminder/*from*/,0/*Date*/, (long) (dumpId * 1000l)) < 0)
		{
			LM_ERR("headers building failed [%d]\n", mid);
			if (msilo_dbf.free_result(db_con, db_res) < 0)
				LM_DBG("failed to free result of query\n");
			msg_list_set_flag(ml, mid, MS_MSG_ERRO);
			return;
		}

		puri.s = uri_buf;
		puri.len = 4 + str_vals[0].len + 1 + str_vals[1].len;
		memcpy(puri.s, "sip:", 4);
		memcpy(puri.s+4, str_vals[0].s, str_vals[0].len);
		puri.s[4+str_vals[0].len] = '@';
		memcpy(puri.s+4+str_vals[0].len+1, str_vals[1].s, str_vals[1].len);

		LM_DBG("msg [%d-%d] for: %.*s\n", i+1, mid,	puri.len, puri.s);

		/** sending using TM function: t_uac */
		body_str.len = MSG_BODY_BUFF_LEN;
		stime =
			(time_t)RES_ROWS(db_res)[i].values[5/*snd time*/].val.int_val;
		n = m_build_body(&body_str, 0, str_vals[2/*body*/], stime);
		if(n<0)
			LM_DBG("sending simple body\n");
		else
			LM_DBG("sending composed body\n");

		msg_list_set_flag(ml, mid, MS_MSG_TSND);

		tmb.t_request(&msg_type,  /* Type of the message */
					&puri,            /* Request-URI */
					&puri,            /* To */
					&ms_reminder,     /* From */
					&hdr_str,         /* Optional headers including CRLF */
					(n<0)?&str_vals[2]:&body_str, /* Message body */
					(ms_outbound_proxy.s)?&ms_outbound_proxy:0,
							/* outbound uri */
					m_tm_callback,    /* Callback function */
					(void*)(long)mid,  /* Callback parameter */
					NULL
				);
	}

done:
	/**
	 * Free the result because we don't need it anymore
	 */
	if (db_res!=NULL && msilo_dbf.free_result(db_con, db_res) < 0)
		LM_DBG("failed to free result of query\n");

	return;
}

int ms_reset_stime(int mid)
{
	db_key_t db_keys[1];
	db_op_t  db_ops[1];
	db_val_t db_vals[1];
	db_key_t db_cols[1];
	db_val_t db_cvals[1];

	db_keys[0]=&sc_mid;
	db_ops[0]=OP_EQ;

	db_vals[0].type = DB_INT;
	db_vals[0].nul = 0;
	db_vals[0].val.int_val = mid;


	db_cols[0]=&sc_snd_time;
	db_cvals[0].type = DB_INT;
	db_cvals[0].nul = 0;
	db_cvals[0].val.int_val = 0;

	LM_DBG("updating send time for [%d]!\n", mid);

	if (msilo_dbf.use_table(db_con, &ms_db_table) < 0)
	{
		LM_ERR("failed to use_table\n");
		return -1;
	}

	if(msilo_dbf.update(db_con,db_keys,db_ops,db_vals,db_cols,db_cvals,1,1)!=0)
	{
		LM_ERR("failed to make update for [%d]!\n",	mid);
		return -1;
	}
	return 0;
}

/*
 * Check if REGISTER request has contacts that support MESSAGE method or
 * if MESSAGE method is listed in Allow header and contact does not have
 * methods parameter.
 */
int check_message_support(struct sip_msg* msg)
{
	contact_t* c;
	unsigned int allow_message = 0;
	unsigned int allow_hdr = 0;
	str *methods_body;
	unsigned int methods;

	/* Parse all headers in order to see all Allow headers */
	if (parse_headers(msg, HDR_EOH_F, 0) == -1)
	{
		LM_ERR("failed to parse headers\n");
		return -1;
	}

	if (parse_allow(msg) == 0)
	{
		allow_hdr = 1;
		allow_message = get_allow_methods(msg) & METHOD_MESSAGE;
	}
	LM_DBG("Allow message: %u\n", allow_message);

	if (!msg->contact)
	{
		LM_DBG("no Contact found\n");
		return -1;
	}
	if (parse_contact(msg->contact) < 0)
	{
		LM_ERR("failed to parse Contact HF\n");
		return -1;
	}
	if (((contact_body_t*)msg->contact->parsed)->star)
	{
		LM_DBG("* Contact found\n");
		return -1;
	}

	if (contact_iterator(&c, msg, 0) < 0)
		return -1;

	/*
	 * Check contacts for MESSAGE method in methods parameter list
	 * If contact does not have methods parameter, use Allow header methods,
	 * if any.  Stop if MESSAGE method is found.
	 */
	while(c)
	{
		if (c->methods)
		{
			methods_body = &(c->methods->body);
			if (parse_methods(methods_body, &methods) < 0)
			{
				LM_ERR("failed to parse contact methods\n");
				return -1;
			}
			if (methods & METHOD_MESSAGE)
			{
				LM_DBG("MESSAGE contact found\n");
				return 0;
			}
		} else {
			if (allow_message)
			{
				LM_DBG("MESSAGE found in Allow Header\n");
				return 0;
			}
		}
		if (contact_iterator(&c, msg, c) < 0)
		{
			LM_DBG("MESSAGE contact not found\n");
			return -1;
		}
	}
	/* no Allow header and no methods in Contact => dump MESSAGEs */
	if(allow_hdr==0)
		return 0;
	return -1;
}

static int msg_process_prefork(void){
	LM_INFO("MSG process prefork");
	return 0;
}

static int msg_process_postfork(void){
	LM_INFO("MSG process postfork");
	return 0;
}

static void msg_process(int rank)
{
	/* if this blasted server had a decent I/O loop, we'd
	 * just add our socket to it and connect().
	 */
	pid = my_pid();
	*sender_pid = pid;

	LM_INFO("started child message sender process, rank: %d\n", rank);
	senderThreadsRunning = 1;

	t_senderThreadArg arg;
	arg.rank = rank;
	arg.threadId = 0;
	senderThreadMain(&arg);
}

/**
 * Called when server starts so shared variables for all processes are allocated and initialized.
 */
static int initSenderThreads(void){
	senderThreadsRunning = 1;
	senderThreadWaiters = 0;

	sender_pid = (int*)shm_malloc(sizeof(int));
	if(sender_pid == NULL) {
		LM_ERR("No more shared memory\n");
		return -1;
	}

	/* Initialise attribute to mutex. */
	pthread_mutexattr_init(&senderThreadQueueCondMutexAttr);
	pthread_mutexattr_setpshared(&senderThreadQueueCondMutexAttr, PTHREAD_PROCESS_SHARED);

	/* Allocate memory to pmutex here. */
	pSenderThreadQueueCondMutex = (pthread_mutex_t *)shm_malloc(sizeof(pthread_mutex_t));
	if (pSenderThreadQueueCondMutex == NULL){
		LM_CRIT("Could not allocate memory for mutex");
		return -1;
	}

	/* Initialise mutex. */
	pthread_mutex_init(pSenderThreadQueueCondMutex, &senderThreadQueueCondMutexAttr);

	/* Initialise attribute to condition. */
	pthread_condattr_init(&senderThreadQueueCondAttr);
	pthread_condattr_setpshared(&senderThreadQueueCondAttr, PTHREAD_PROCESS_SHARED);

	/* Allocate memory to pcond here. */
	pSenderThreadQueueCond = (pthread_cond_t *)shm_malloc(sizeof(pthread_cond_t));
	if (pSenderThreadQueueCond == NULL){
		LM_CRIT("Could not allocate memory for cond variable");
		return -1;
	}

	/* Initialise condition. */
	pthread_cond_init(pSenderThreadQueueCond, &senderThreadQueueCondAttr);

	return 0;
}

/**
 * Terminates running sender threads.
 */
static int destroySenderThreads(void){
	terminateSenderThreads();
	// TODO: wait for termination so mutex & conditions are not destroyed before memory release.
	sleep(1);

	// Clean up.
	if (pSenderThreadQueueCondMutex != NULL) {
		pthread_mutex_destroy(pSenderThreadQueueCondMutex);
		shm_free(pSenderThreadQueueCondMutex);
		pSenderThreadQueueCondMutex = NULL;
	}

	pthread_mutexattr_destroy(&senderThreadQueueCondMutexAttr);

	if (pSenderThreadQueueCond != NULL) {
		pthread_cond_destroy(pSenderThreadQueueCond);
		shm_free(pSenderThreadQueueCond);
		pSenderThreadQueueCond = NULL;
	}

	pthread_condattr_destroy(&senderThreadQueueCondAttr);

	if (sender_pid != NULL){
		shm_free(sender_pid);
		sender_pid = NULL;
	}

	return 0;
}

/**
 * Called when worker process is initialized. Spawns sender threads.
 */
static int initChildSenderThreads(void){
	return spawnSenderThreads();
}

/**
 * Routine starts given amount of sender threads.
 */
static int spawnSenderThreads(void){
	int t = 0;
	int rc = 0;
	senderThreadsRunning = 1;

	for(t = 0; t < SENDER_THREAD_NUM; t++){
		rc = pthread_create(&senderThreads[t], NULL, senderThreadMain, (void *)&senderThreadsArgs[t]);
		if (rc){
			LM_ERR("ERROR; return code from pthread_create() is %d\n", rc);
			break;
		}
	}

	// Check for fails.
	if (rc){
		terminateSenderThreads();
		return -1;
	}

	return 0;
}

/**
 * Sets all sender threads to terminate.
 */
static int terminateSenderThreads(void){
	// Running flag set to false.
	senderThreadsRunning = 0;
	if (pSenderThreadQueueCondMutex == NULL || pSenderThreadQueueCond == NULL){
		return -1;
	}

	// Broadcast signal to check the queue so all sender threads are woken up in order to terminate.
	LM_INFO("Terminating senders");
	pthread_mutex_lock(pSenderThreadQueueCondMutex);
	if (pSenderThreadQueueCond != NULL) {
		pthread_cond_broadcast(pSenderThreadQueueCond);
	}
	pthread_mutex_unlock(pSenderThreadQueueCondMutex);

	return 0;
}

static void signalNewTask(void){
	if (pSenderThreadQueueCondMutex == NULL){
		LM_CRIT("Mutex is null");
		return;
	}

	if (pSenderThreadQueueCond == NULL){
		LM_CRIT("Condition variable is null");
		return;
	}

	// TODO: refactor to semaphore. Signal increments semaphore. Sender process resets to zero.
	// Signal should be always signalled only if some thread is waiting.

	//int rc = pthread_mutex_lock(pSenderThreadQueueCondMutex);
	//rc = pthread_cond_signal(pSenderThreadQueueCond);
	//rc = pthread_mutex_unlock(pSenderThreadQueueCondMutex);
}

/**
 * Main sender thread.
 */
static int sendMessages(const retry_list_el list);
static void *senderThreadMain(void *varg)
{
	senderThreadArg arg = (senderThreadArg) varg;
	LM_INFO("Sender thread %d in rank %d started\n", arg->threadId, arg->rank);
	size_t toPeek = MAX_PEEK_NUM;
	size_t peekNum = 0;

	// Work loop.
	while(senderThreadsRunning){
		retry_list_el elems = NULL;
		int signaled = 0;

		// Maximum wait time in condition wait is x seconds so we don't deadlock (soft deadlock).
		struct timespec timeToWait;
		struct timeval now;
		gettimeofday(&now, NULL);
		timeToWait.tv_sec = now.tv_sec + ((SENDER_THREAD_WAIT_MS) / 1000);
		timeToWait.tv_nsec = (now.tv_usec+1000UL*((SENDER_THREAD_WAIT_MS) % 1000))*1000UL;

		// <critical_section> monitor queue, poll one job from queue.
		pthread_mutex_lock(pSenderThreadQueueCondMutex);
		senderThreadWaiters += 1;
		{
			// If queue is empty, wait for insertion signal.
			if (retry_is_empty(rl)) {
				// Wait signaling, note mutex is atomically unlocked while waiting.
				// CPU cycles are saved here since thread blocks while waiting for new jobs.
				signaled = pthread_cond_timedwait(pSenderThreadQueueCond, pSenderThreadQueueCondMutex, &timeToWait);
				senderThreadWaiters -= 1;
			}

			// Check job queue again.
			elems = retry_peek_n(rl, toPeek, &peekNum);
		}
		pthread_mutex_unlock(pSenderThreadQueueCondMutex);
		// </critical_section>

		// If signaling ended with command to quit.
		if (!senderThreadsRunning){
			break;
		}

		// Job may be nil. If is, continue with waiting.
		if (elems == NULL){
			continue;
		}

		// Send messages.
		LM_INFO("Going to dump %d messages", (int)peekNum);
		sendMessages(elems);
	}

	LM_INFO("Sender thread %d in rank %d finished\n", arg->threadId, arg->rank);
	return NULL;
}

static int buildSqlQuery(char * sql_query, str * sql_str, long * midsToLoad, size_t midsToLoadSize);
static int sendMessages(const retry_list_el list){
	db_res_t* db_res = NULL;
	int i, n;
	char hdr_buf[1024];
	char body_buf[MSG_BODY_BUFF_LEN];

	char sql_query[PH_SQL_BUF_LEN];
	str sql_str;

	str str_vals[4], hdr_str , body_str;
	time_t rtime;
	time_t dumpId;

	long midsInDb[MAX_PEEK_NUM];
	size_t midsInDbSize = 0;

	long midsToLoad[MAX_PEEK_NUM];
	size_t midsToLoadSize = 0;

	// Logic.
	if (list == NULL){
		LM_INFO("Message list is empty");
		return -1;
	}

	// Check not before.
	while(senderThreadsRunning) {
		time_t send_time;
		time(&send_time);
		if (list->not_before > send_time) {
			usleep(25l*1000l);
		} else {
			break;
		}
	}

	// Load message with given MID from database.
	// Raw query, unfortunately.
	retry_list_el p0 = list;
	while(p0 && midsToLoadSize < MAX_PEEK_NUM){
		midsToLoad[midsToLoadSize++] = p0->msgid;
		p0 = p0->prev;

		if (midsToLoadSize >= MAX_PEEK_NUM && p0 != NULL){
			LM_CRIT("List is not ended with prev=NULL");
			break;
		}
	}

	hdr_str.s=hdr_buf;
	hdr_str.len=1024;
	body_str.s=body_buf;
	body_str.len=MSG_BODY_BUFF_LEN;

	if (buildSqlQuery(sql_query, &sql_str, midsToLoad, midsToLoadSize) < 0){
		LM_CRIT("Could not build sql string");
		return -1;
	}

	LM_INFO("MySQL query string: %.*s", sql_str.len, sql_str.s);

	time(&dumpId);
	if (msilo_dbf.use_table(db_con, &ms_db_table) < 0)
	{
		LM_ERR("failed to use_table\n");
		goto error;
	}

	if((msilo_dbf.raw_query(db_con, &sql_str, &db_res)!=0) || (RES_ROW_N(db_res) <= 0))
	{
		LM_DBG("no stored messages for size=%d!\n", (int)midsToLoadSize);
		goto done;
	}

	LM_INFO("resend: dumping [%d] messages for size: %d\n",  RES_ROW_N(db_res), (int)midsToLoadSize);
	for(i = 0; i < RES_ROW_N(db_res); i++)
	{
		retry_list_el p1 = list;

		int findIter = 0;
		int mid = RES_ROWS(db_res)[i].values[0].val.int_val;
		midsInDb[midsInDbSize++] = mid;

		// Find this mid in the list.
		while(p1){
			if (p1->msgid == mid){
				break;
			}
			findIter += 1;
			p1 = p1->prev;
		}

		if (p1 == NULL){
			LM_CRIT("Message loaded from DB not found in list: <%d>, findIter: [%d]", mid, findIter);
			continue;
		}

		memset(str_vals, 0, 4*sizeof(str));
		SET_STR_VAL(str_vals[0], db_res, i, 1); /* from */
		SET_STR_VAL(str_vals[1], db_res, i, 2); /* to */
		SET_STR_VAL(str_vals[2], db_res, i, 3); /* body */
		SET_STR_VAL(str_vals[3], db_res, i, 4); /* ctype */
		rtime = (time_t)RES_ROWS(db_res)[i].values[5/*inc time*/].val.int_val;

		hdr_str.len = 1024;
		if(m_build_headers(&hdr_str, str_vals[3] /*ctype*/,
						   str_vals[0]/*from*/, rtime /*Date*/, (long) (dumpId * 1000l)) < 0)
		{
			LM_ERR("resend: headers building failed [%d]\n", mid);
			if (msilo_dbf.free_result(db_con, db_res) < 0)
				LM_ERR("resend: failed to free the query result\n");
			msg_list_set_flag(ml, mid, MS_MSG_ERRO);
			goto error;
		}

		LM_DBG("resend: msg [%d-%d] for: %.*s\n", i+1, mid,	str_vals[1].len, str_vals[1].s);

		/** sending using TM function: t_uac */
		body_str.len = MSG_BODY_BUFF_LEN;
		n = m_build_body(&body_str, rtime, str_vals[2/*body*/], 0);
		if(n<0)
			LM_DBG("resend: sending simple body\n");
		else
			LM_DBG("resend: sending composed body\n");

		int res = tmb.t_request(&msg_type,  /* Type of the message */
								&str_vals[1],     /* Request-URI (To) */
								&str_vals[1],     /* To */
								&str_vals[0],     /* From */
								&hdr_str,         /* Optional headers including CRLF */
								(n<0)?&str_vals[2]:&body_str, /* Message body */
								(ms_outbound_proxy.s)?&ms_outbound_proxy:0,
				/* outbound uri */
								m_tm_callback,    /* Callback function */
								(void*)p1, /* Callback parameter */
								NULL
		);

		if (res < 0){
			LM_WARN("resend: message sending failed [%d], res=%d messages for <%.*s>!\n",
					mid, res, str_vals[1].len, str_vals[1].s);
		}
	}

	// Messages not found in the database are removed from retry queue
	// since its record gets lost.

	// Remove bounds.
	p0 = list;
	while(p0){
		retry_list_el prev = p0->prev;
		p0->next = NULL;
		p0->prev = NULL;
		p0 = prev;
	}

done:
	/**
	 * Free the result because we don't need it
	 * anymore
	 */
	if (db_res!=NULL && msilo_dbf.free_result(db_con, db_res) < 0)
		LM_ERR("resend: failed to free result of query\n");

	return 1;
error:
	return -1;

}

/**
 * TM callback function - delete message from database if was sent OK
 */
void m_tm_callback( struct cell *t, int type, struct tmcb_params *ps)
{
	retry_list_el cur_elem = NULL;
	if(ps->param==NULL)
	{
		LM_INFO("message id not received\n");
		goto done;
	}

	cur_elem = *((retry_list_el*)ps->param);

	LM_INFO("completed with status %d [mid: %d]\n", ps->code, cur_elem->msgid);
	if(!db_con)
	{
		LM_ERR("db_con is NULL\n");
		goto done;
	}
	if(ps->code >= 300)
	{
		int shouldResend = cur_elem->retry_ctr < ms_retry_count;
		LM_INFO("message <%d> was not sent successfully, resendCtr: %d, shouldResend: %d\n",
				cur_elem->msgid, cur_elem->retry_ctr, shouldResend);

		if (shouldResend){
			retry_add_element(rl, cur_elem->msgid, cur_elem->retry_ctr + 1, 0);
			signalNewTask();
		}
		goto done;
	}

	// By seting DONE cleaning thread will remove it from the list and from the database.
	LM_INFO("message <%d> was sent successfully\n", cur_elem->msgid);
	msg_list_set_flag(ml, cur_elem->msgid, MS_MSG_DONE);

	// Free SHM memory.
	retry_list_el_free(cur_elem);

	done:
	return;
}

static int buildSqlQuery(char * sql_query, str * sql_str, long * midsToLoad, size_t midsToLoadSize) {
	int off = 0, ret = 0, i = 0;
	ret = snprintf(sql_query, PH_SQL_BUF_LEN, "SELECT `%.*s`, `%.*s`, `%.*s`, `%.*s`, `%.*s`, `%.*s` FROM `%.*s` WHERE ",
				   sc_mid.len, sc_mid.s,
				   sc_from.len, sc_from.s,
				   sc_to.len, sc_to.s,
				   sc_body.len, sc_body.s,
				   sc_ctype.len, sc_ctype.s,
				   sc_inc_time.len, sc_inc_time.s,
				   ms_db_table.len, ms_db_table.s);
	if (ret < 0 || ret >= PH_SQL_BUF_LEN) goto error;
	off = ret;

	// WHERE conditions.
	for(i = 0; i < midsToLoadSize; i++){
		ret = snprintf(sql_query + off, PH_SQL_BUF_LEN - off, " `%.*s`=%d ", sc_mid.len, sc_mid.s, (int)midsToLoad[i]);
		if (ret < 0 || ret >= (PH_SQL_BUF_LEN - off)) goto error;
		off += ret;

		if (i+1 < midsToLoadSize){
			ret = snprintf(sql_query + off, PH_SQL_BUF_LEN - off, " OR ");
			if (ret < 0 || ret >= (PH_SQL_BUF_LEN - off)) goto error;
			off += ret;
		}
	}

	ret = snprintf(sql_query + off, PH_SQL_BUF_LEN - off, " ORDER BY `%.*s`", sc_mid.len, sc_mid.s);
	if (ret < 0 || ret >= (PH_SQL_BUF_LEN - off)) goto error;
	off += ret;

	// Null terminate.
	if (off + 1 >= PH_SQL_BUF_LEN) goto error;
	sql_query[off + 1] = '\0';
	sql_str->s = sql_query;
	sql_str->len = off;

	return 0;
error:
	return -1;
}