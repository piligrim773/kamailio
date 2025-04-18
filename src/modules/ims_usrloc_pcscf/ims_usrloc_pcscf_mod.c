/*
 * Copyright (C) 2012 Smile Communications, jason.penton@smilecoms.com
 * Copyright (C) 2012 Smile Communications, richard.good@smilecoms.com
 *
 * The initial version of this code was written by Dragos Vingarzan
 * (dragos(dot)vingarzan(at)fokus(dot)fraunhofer(dot)de and the
 * Fraunhofer FOKUS Institute. It was and still is maintained in a separate
 * branch of the original SER. We are therefore migrating it to
 * Kamailio/SR and look forward to maintaining it from here on out.
 * 2011/2012 Smile Communications, Pty. Ltd.
 * ported/maintained/improved by
 * Jason Penton (jason(dot)penton(at)smilecoms.com and
 * Richard Good (richard(dot)good(at)smilecoms.com) as part of an
 * effort to add full IMS support to Kamailio/SR using a new and
 * improved architecture
 *
 * NB: A lot of this code was originally part of OpenIMSCore,
 * FhG Fokus.
 * Copyright (C) 2004-2006 FhG Fokus
 * Thanks for great work! This is an effort to
 * break apart the various CSCF functions into logically separate
 * components. We hope this will drive wider use. We also feel
 * that in this way the architecture is more complete and thereby easier
 * to manage in the Kamailio/SR environment
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include <stdio.h>
#include "ims_usrloc_pcscf_mod.h"
#include "../../core/sr_module.h"
#include "../../core/dprint.h"
#include "../../core/rpc_lookup.h"
#include "../../core/timer.h" /* register_timer */
#include "../../core/globals.h"
#include "../../core/ut.h" /* str_init */
#include "dlist.h"		   /* register_udomain */
#include "udomain.h"	   /* {insert,delete,get,release}_urecord */
#include "pcontact.h"	   /* {insert,delete,get}_ucontact */
#include "ul_rpc.h"
#include "ul_callback.h"
#include "usrloc.h"
#include "usrloc_db.h"

MODULE_VERSION

#define DEFAULT_DBG_FILE "/var/log/usrloc_debug"
static FILE *debug_file;

int audit_expired_pcontacts_timeout = 40;
int audit_expired_pcontacts_interval = 60;

static void audit_usrloc_expired_pcontacts_timer(
		unsigned int ticks, void *param);

static int mod_init(void); /*!< Module initialization function */
static void destroy(void); /*!< Module destroy function */
static void timer(unsigned int ticks, void *param); /*!< Timer handler */
static int child_init(int rank); /*!< Per-child init function */

extern int bind_usrloc(usrloc_api_t *api);
extern int ul_locks_no;


int expires_grace =
		3600; //default is a grace period of 1 hour - after this contact is removed from P

/*
 * Module parameters and their default values
 */
str usrloc_debug_file = str_init(DEFAULT_DBG_FILE);
int usrloc_debug = 0;
int ul_hash_size = 9;
int ims_ulp_init_flag = 0;
str db_url = str_init(DEFAULT_DB_URL); /*!< Database URL */
int timer_interval = 60;			   /*!< Timer interval in seconds */
int db_mode =
		0; /*!< Database sync scheme: 0-no db, 1-write through, 2-write back, 3-only db */
int ul_fetch_rows = 2000;
int match_contact_host_port =
		1; /*!< Should we match contact just based on rui host and port*/

db1_con_t *ul_dbh = 0;
db_func_t ul_dbf;

/*! \brief
 * Exported functions
 */
static cmd_export_t cmds[] = {
		{"ul_bind_ims_usrloc_pcscf", (cmd_function)bind_usrloc, 1, 0, 0, 0},
		{0, 0, 0, 0, 0, 0}};

/*! \brief
 * Exported parameters
 */
static param_export_t params[] = {{"hash_size", PARAM_INT, &ul_hash_size},
		{"timer_interval", PARAM_INT, &timer_interval},
		{"usrloc_debug_file", PARAM_STR, &usrloc_debug_file},
		{"enable_debug_file", PARAM_INT, &usrloc_debug},

		{"db_url", PARAM_STR, &db_url},
		{"timer_interval", PARAM_INT, &timer_interval},
		{"db_mode", PARAM_INT, &db_mode},

		{"match_contact_host_port", PARAM_INT, &match_contact_host_port},
		{"audit_expired_pcontacts_timeout", PARAM_INT,
				&audit_expired_pcontacts_timeout},
		{"audit_expired_pcontacts_interval", PARAM_INT,
				&audit_expired_pcontacts_interval},
		{"expires_grace", PARAM_INT, &expires_grace},

		{0, 0, 0}};

stat_export_t mod_stats[] = {{"registered_contacts", STAT_IS_FUNC,
									 (stat_var **)get_number_of_contacts},
		{"registered_impus", STAT_IS_FUNC, (stat_var **)get_number_of_impu},
		{"expired_contacts", STAT_IS_FUNC, (stat_var **)get_number_of_expired},
		{0, 0, 0}};

struct module_exports exports = {
		"ims_usrloc_pcscf", DEFAULT_DLFLAGS, /* dlopen flags */
		cmds,								 /* exported functions */
		params,								 /* export parameters */
		0,									 /* exported RPC functions */
		0,									 /* exported pseudo-variables */
		0,									 /* response·function */
		mod_init,	/* module initialization function */
		child_init, /* per-child·init·function*/
		destroy		/* destroy function */
};


/*! \brief
 * Module initialization function
 */
static int mod_init(void)
{

	if(usrloc_debug) {
		LM_INFO("Logging usrloc records to %.*s\n", usrloc_debug_file.len,
				usrloc_debug_file.s);
		debug_file = fopen(usrloc_debug_file.s, "a");
		fprintf(debug_file, "starting\n");
		fflush(debug_file);
	}

#ifdef STATISTICS
	/* register statistics */
	if(register_module_stats("ims_usrloc_pcscf", mod_stats) != 0) {
		LM_ERR("failed to register core statistics\n");
		return -1;
	}
#endif

	if(ul_hash_size <= 1)
		ul_hash_size = 512;
	else
		ul_hash_size = 1 << ul_hash_size;
	ul_locks_no = ul_hash_size;

	if(ul_init_locks() != 0) {
		LM_ERR("locks array initialization failed\n");
		return -1;
	}

	/* Regsiter RPC */
	if(rpc_register_array(ul_rpc) != 0) {
		LM_ERR("failed to register RPC commands\n");
		return -1;
	}

	/* Register cache timer */
	LM_DBG("Registering cache timer");
	register_timer(timer, 0, timer_interval);

	/* Register audit timer */
	if(db_mode == DB_ONLY)
		register_timer(audit_usrloc_expired_pcontacts_timer, 0,
				audit_expired_pcontacts_interval);


	/* init the callbacks list */
	if(init_ulcb_list() < 0) {
		LM_ERR("usrloc/callbacks initialization failed\n");
		return -1;
	}

	/* Shall we use database ? */
	if(db_mode != NO_DB) { /* Yes */
		if(ul_fetch_rows <= 0) {
			LM_ERR("invalid fetch_rows number '%d'\n", ul_fetch_rows);
			return -1;
		}

		if(db_bind_mod(&db_url, &ul_dbf) < 0) { /* Find database module */
			LM_ERR("failed to bind database module\n");
			return -1;
		}
		if(!DB_CAPABILITY(ul_dbf, DB_CAP_ALL)) {
			LM_ERR("database module does not implement all functions"
				   " needed by the module\n");
			return -1;
		}
		LM_DBG("Running in DB mode %i\n", db_mode);
	}

	ims_ulp_init_flag = 1;

	return 0;
}

static int child_init(int _rank)
{
	dlist_t *ptr;

	/* connecting to DB ? */
	switch(db_mode) {
		case NO_DB:
			return 0;
		case WRITE_THROUGH:
			/* connect to db only from SIP workers, TIMER and MAIN processes */
			if(_rank <= 0 && _rank != PROC_TIMER && _rank != PROC_MAIN)
				return 0;
			break;
		case WRITE_BACK:
			/* connect to db only from TIMER (for flush), from MAIN (for
			 * final flush() and from child 1 for preload */
			if(_rank != PROC_TIMER && _rank != PROC_MAIN
					&& _rank != PROC_SIPINIT)
				return 0;
			break;
	}

	LM_DBG("Connecting to usrloc_pcscf DB for rank %d\n", _rank);
	ul_dbh = ul_dbf.init(&db_url); /* Get a database connection per child */

	if(!ul_dbh) {
		LM_ERR("child(%d): failed to connect to database\n", _rank);
		return -1;
	}
	/* _rank==PROC_SIPINIT is used even when fork is disabled */
	if(_rank == PROC_SIPINIT && db_mode != DB_ONLY) {
		// if cache is used, populate domains from DB
		for(ptr = root; ptr; ptr = ptr->next) {
			LM_DBG("Preloading domain %.*s\n", ptr->name.len, ptr->name.s);
			if(preload_udomain(ul_dbh, ptr->d) < 0) {
				LM_ERR("child(%d): failed to preload domain '%.*s'\n", _rank,
						ptr->name.len, ZSW(ptr->name.s));
				return -1;
			}
		}
	}

	return 0;
}

/*! \brief
 * Module destroy function
 */
static void destroy(void)
{
	free_all_udomains();
	ul_destroy_locks();

	/* free callbacks list */
	destroy_ulcb_list();

	free_service_route_buf();
	free_impu_buf();

	if(db_mode)
		destroy_db();

	if(cbp_qos)
		shm_free(cbp_qos);

	if(cbp_registrar)
		shm_free(cbp_registrar);
}

static void audit_usrloc_expired_pcontacts_timer(
		unsigned int ticks, void *param)
{

	audit_usrloc_expired_pcontacts(root->d);
}


/*! \brief
 * Timer handler
 */
static void timer(unsigned int ticks, void *param)
{
	LM_DBG("Syncing cache\n");
	if(usrloc_debug) {
		print_all_udomains(debug_file);
		fflush(debug_file);
	}

	if(synchronize_all_udomains() != 0) {
		LM_ERR("synchronizing cache failed\n");
	}
}
