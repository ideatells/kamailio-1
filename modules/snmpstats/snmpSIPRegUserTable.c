/*
 * $Id$
 *
 * SNMPStats Module 
 * Copyright (C) 2006 SOMA Networks, INC.
 * Written by: Jeffrey Magder (jmagder@somanetworks.com)
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * Kamailio is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 *
 * History:
 * --------
 * 2006-11-23 initial version (jmagder)
 *
 * Note: this file originally auto-generated by mib2c using
 * mib2c.array-user.conf
 *
 * This file implements the kamailioSIPRegUserTable.  For a full description of
 * the table, please see the KAMAILIO-SIP-SERVER-MIB.
 *
 * Understanding this code will be much simpler with the following information:
 *
 * 1) All rows are indexed by an integer user index.  This is different from the
 *    usrloc module, which indexes by strings.  This less natural indexing
 *    scheme was required due to SNMP String index limitations.  (for example,
 *    SNMP has maximum index lengths.)
 *
 * 2) We need a quick way of mapping usrloc indices to our integer indices.  For
 *    this reason a string indexed Hash Table was created, with each entry mapping
 *    to an integer user index. 
 *
 *    This hash table is used by the kamailioSIPContactTable (the hash table also
 *    maps a user to its contacts), as well as the kamailioSIPRegUserLookupTable.
 *    The hash table is also used for quick lookups when a user expires. (i.e, it
 *    gives us a more direct reference, instead of having to search the whole
 *    table).
 *
 * 3) We are informed about new/expired users via a callback mechanism from the
 *    usrloc module.  Because of NetSNMP inefficiencies, we had to abstract this
 *    process.  Specifically:
 *
 *    - It can take a long time for the NetSNMP code base to populate a table with
 *      a large number of records. 
 *
 *    - We rely on callbacks for updated user information. 
 *
 *    Clearly, using the SNMPStats module in this situation could lead to some
 *    big performance loses if we don't find another way to deal with this.  The
 *    solution was to use an interprocess communications buffer.  
 *
 *    Instead of adding the record directly to the table, the callback functions
 *    now adds either an add/delete command to the interprocessBuffer.  When an
 *    snmp request is recieved by the SNMPStats sub-process, it will consume
 *    this interprocess buffer, adding and deleting users.  When it is finished,
 *    it can service the SNMP request.  
 *
 *    This doesn't remove the NetSNMP inefficiency, but instead moves it to a
 *    non-critical path.  Such an approach allows SNMP support with almost no
 *    overhead to the rest of Kamailio.
 */

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <net-snmp/library/snmp_assert.h>

#include "hashTable.h"
#include "interprocess_buffer.h"
#include "utilities.h"
#include "snmpSIPRegUserTable.h"
#include "snmpstats_globals.h"

#include "../../sr_module.h"
#include "../../locking.h"
#include "../usrloc/usrloc.h"

static netsnmp_handler_registration *my_handler = NULL;
static netsnmp_table_array_callbacks cb;

oid    kamailioSIPRegUserTable_oid[]   = { kamailioSIPRegUserTable_TABLE_OID };
size_t kamailioSIPRegUserTable_oid_len = OID_LENGTH(kamailioSIPRegUserTable_oid);


/* If the usrloc module is loaded, this function will grab hooks into its
 * callback registration function, and add handleContactCallbacks() as the
 * callback for UL_CONTACT_INSERT and UL_CONTACT_EXPIRE.
 *
 * Returns 1 on success, and zero otherwise. */
int registerForUSRLOCCallbacks(void)  
{
	bind_usrloc_t bind_usrloc;
	usrloc_api_t ul;

	bind_usrloc = (bind_usrloc_t)find_export("ul_bind_usrloc", 1, 0);
	if (!bind_usrloc)
	{
		LM_ERR("Can't find ul_bind_usrloc\n");
		goto error;
	}
	if (bind_usrloc(&ul) < 0 || ul.register_ulcb == NULL)
	{
		LM_ERR("Can't bind usrloc\n");
		goto error;
	}

	ul.register_ulcb(UL_CONTACT_INSERT, handleContactCallbacks, NULL);
	
	ul.register_ulcb(UL_CONTACT_EXPIRE, handleContactCallbacks, NULL);

	return 1;

error:
	LM_INFO("failed to register for callbacks with the USRLOC module.");
	LM_INFO("kamailioSIPContactTable and kamailioSIPUserTable will be"
			" unavailable");
	return 0;
}


/* Removes an SNMP row indexed by userIndex, and frees the string and index it
 * pointed to. */
void deleteRegUserRow(int userIndex) 
{
	
	kamailioSIPRegUserTable_context *theRow;

	netsnmp_index indexToRemove;
	oid indexToRemoveOID;
		
	indexToRemoveOID   = userIndex;
	indexToRemove.oids = &indexToRemoveOID;
	indexToRemove.len  = 1;

	theRow = CONTAINER_FIND(cb.container, &indexToRemove);

	/* The userURI is shared memory, the index.oids was allocated from
	 * pkg_malloc(), and theRow was made with the NetSNMP API which uses
	 * malloc() */
	if (theRow != NULL) {
		CONTAINER_REMOVE(cb.container, &indexToRemove);
		pkg_free(theRow->kamailioSIPUserUri);
		pkg_free(theRow->index.oids);
		free(theRow);
	}

}

/*
 * Adds or updates a user:
 *
 *   - If a user with the name userName exists, its 'number of contacts' count
 *     will be incremented.  
 *   - If the user doesn't exist, the user will be added to the table, and its
 *     number of contacts' count set to 1. 
 */
void updateUser(char *userName) 
{
	int userIndex; 

	aorToIndexStruct_t *newRecord;

	aorToIndexStruct_t *existingRecord = 
		findHashRecord(hashTable, userName, HASH_SIZE);

	/* We found an existing record, so  we need to update its 'number of
	 * contacts' count. */
	if (existingRecord != NULL) 
	{
		existingRecord->numContacts++;
		return;
	}

	/* Make a new row, and insert a record of it into our mapping data
	 * structures */
	userIndex = createRegUserRow(userName);

	if (userIndex == 0) 
	{
		LM_ERR("kamailioSIPRegUserTable ran out of memory."
				"  Not able to add user: %s", userName);
		return;
	}

	newRecord = createHashRecord(userIndex, userName);
	
	/* If we couldn't create a record in the hash table, then we won't be
	 * able to access this row properly later.  So remove the row from the
	 * table and fail. */
	if (newRecord == NULL) {
		deleteRegUserRow(userIndex);
		LM_ERR("kamailioSIPRegUserTable was not able to push %s into the hash."
				"  User not added to this table\n", userName);
		return;
	}
	
	/* Insert the new record of the mapping data structure into the hash
	 * table */
	/*insertHashRecord(hashTable,
			createHashRecord(userIndex, userName), 
			HASH_SIZE);*/
	
	insertHashRecord(hashTable,
			newRecord, 
			HASH_SIZE);
}


/* Creates a row and inserts it.  
 *
 * Returns: The rows userIndex on success, and 0 otherwise. */
int createRegUserRow(char *stringToRegister) 
{
	int static index = 0;
	
	index++;

	kamailioSIPRegUserTable_context *theRow;

	oid  *OIDIndex;
	int  stringLength;

	theRow = SNMP_MALLOC_TYPEDEF(kamailioSIPRegUserTable_context);

	if (theRow == NULL) {
		LM_ERR("failed to create a row for kamailioSIPRegUserTable\n");
		return 0;
	}

	OIDIndex = pkg_malloc(sizeof(oid));

	if (OIDIndex == NULL) {
		free(theRow);
		LM_ERR("failed to create a row for kamailioSIPRegUserTable\n");
		return 0;
	}

	stringLength = strlen(stringToRegister);

	OIDIndex[0] = index;

	theRow->index.len  = 1;
	theRow->index.oids = OIDIndex;
	theRow->kamailioSIPUserIndex = index;

	theRow->kamailioSIPUserUri     = (unsigned char*)pkg_malloc(stringLength* sizeof(char));
    if(theRow->kamailioSIPUserUri== NULL)
    {
        pkg_free(OIDIndex);
		free(theRow);
		LM_ERR("failed to create a row for kamailioSIPRegUserTable\n");
		return 0;

    }
    memcpy(theRow->kamailioSIPUserUri, stringToRegister, stringLength);
	
    theRow->kamailioSIPUserUri_len = stringLength;

	theRow->kamailioSIPUserAuthenticationFailures = 0;

	CONTAINER_INSERT(cb.container, theRow);

	return index;
}

/* Initializes the kamailioSIPRegUserTable module.  */
void init_kamailioSIPRegUserTable(void)
{
	/* Register this table with the master agent */
	initialize_table_kamailioSIPRegUserTable();

	/* We need to create a default row, so create DefaultUser */
	static char *defaultUser = "DefaultUser";
	
	createRegUserRow(defaultUser);
}



/*
 * Initialize the kamailioSIPRegUserTable table by defining its contents and how
 * it's structured
 */
void initialize_table_kamailioSIPRegUserTable(void)
{
	netsnmp_table_registration_info *table_info;

	if(my_handler) {
		snmp_log(LOG_ERR, "initialize_table_kamailioSIPRegUserTable_hand"
				"ler called again\n");
		return;
	}

	memset(&cb, 0x00, sizeof(cb));

	/* create the table structure itself */
	table_info = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info);

	my_handler = 
		netsnmp_create_handler_registration(
			"kamailioSIPRegUserTable",
			netsnmp_table_array_helper_handler, 
			kamailioSIPRegUserTable_oid,
			kamailioSIPRegUserTable_oid_len,
			HANDLER_CAN_RONLY);

	if (!my_handler || !table_info) {
		snmp_log(LOG_ERR, "malloc failed in initialize_table_kamailio"
				"SIPRegUserTable_handler\n");
		return; /** mallocs failed */
	}

	netsnmp_table_helper_add_index(table_info, ASN_UNSIGNED);

	table_info->min_column = kamailioSIPRegUserTable_COL_MIN;
	table_info->max_column = kamailioSIPRegUserTable_COL_MAX;

	cb.get_value = kamailioSIPRegUserTable_get_value;
	cb.container = netsnmp_container_find("kamailioSIPRegUserTable_primary:"
			"kamailioSIPRegUserTable:" "table_container");

	DEBUGMSGTL(("initialize_table_kamailioSIPRegUserTable",
				"Registering table kamailioSIPRegUserTable "
				"as a table array\n"));

	netsnmp_table_container_register(my_handler, table_info, &cb, 
			cb.container, 1);
}


/* Handles SNMP GET requests. */
int kamailioSIPRegUserTable_get_value(
		netsnmp_request_info *request,
		netsnmp_index *item,
		netsnmp_table_request_info *table_info )
{
	/* First things first, we need to consume the interprocess buffer, in
	 * case something has changed. We want to return the freshest data. */
	consumeInterprocessBuffer();

	netsnmp_variable_list *var = request->requestvb;

	kamailioSIPRegUserTable_context *context = 
		(kamailioSIPRegUserTable_context *)item;

	switch(table_info->colnum) 
	{

		case COLUMN_KAMAILIOSIPUSERURI:
			/** SnmpAdminString = ASN_OCTET_STR */
			snmp_set_var_typed_value(var, ASN_OCTET_STR,
				(unsigned char*)context->kamailioSIPUserUri,
				context->kamailioSIPUserUri_len );
			break;
	
		case COLUMN_KAMAILIOSIPUSERAUTHENTICATIONFAILURES:
			/** COUNTER = ASN_COUNTER */
			snmp_set_var_typed_value(var, ASN_COUNTER,
				(unsigned char*)
				&context->kamailioSIPUserAuthenticationFailures,
				sizeof(
				context->kamailioSIPUserAuthenticationFailures));
		break;
	
		default: /** We shouldn't get here */
			snmp_log(LOG_ERR, "unknown column in "
				"kamailioSIPRegUserTable_get_value\n");

			return SNMP_ERR_GENERR;
	}

	return SNMP_ERR_NOERROR;
}


const kamailioSIPRegUserTable_context *
kamailioSIPRegUserTable_get_by_idx(netsnmp_index * hdr)
{
	return (const kamailioSIPRegUserTable_context *)
		CONTAINER_FIND(cb.container, hdr );
}


