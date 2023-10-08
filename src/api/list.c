/* Pi-hole: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  API Implementation /api/{allow,deny}list
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "webserver/http-common.h"
#include "webserver/json_macros.h"
#include "api.h"
#include "database/gravity-db.h"
#include "events.h"
#include "shmem.h"
// getNameFromIP()
#include "database/network-table.h"

static int api_list_read(struct ftl_conn *api,
                         const int code,
                         const enum gravity_list_type listtype,
                         const char *item)
{
	const char *sql_msg = NULL;
	if(!gravityDB_readTable(listtype, item, &sql_msg, true, NULL))
	{
		return send_json_error(api, 400, // 400 Bad Request
		                       "database_error",
		                       "Could not read domains from database table",
		                       sql_msg);
	}

	tablerow table;
	cJSON *rows = JSON_NEW_ARRAY();
	while(gravityDB_readTableGetRow(listtype, &table, &sql_msg))
	{
		cJSON *row = JSON_NEW_OBJECT();

		// Special fields
		if(listtype == GRAVITY_GROUPS)
		{
			JSON_COPY_STR_TO_OBJECT(row, "name", table.name);
			JSON_COPY_STR_TO_OBJECT(row, "comment", table.comment);
		}
		else if(listtype == GRAVITY_ADLISTS)
		{
			JSON_COPY_STR_TO_OBJECT(row, "address", table.address);
			JSON_COPY_STR_TO_OBJECT(row, "comment", table.comment);
		}
		else if(listtype == GRAVITY_CLIENTS)
		{
			char *name = NULL;
			if(table.client != NULL)
			{
				// Try to obtain hostname if this is a valid IP address
				if(isValidIPv4(table.client) || isValidIPv6(table.client))
					name = getNameFromIP(NULL, table.client);
			}

			JSON_COPY_STR_TO_OBJECT(row, "client", table.client);
			JSON_COPY_STR_TO_OBJECT(row, "name", name);
			JSON_COPY_STR_TO_OBJECT(row, "comment", table.comment);

			// Free allocated memory (if applicable)
			if(name != NULL)
				free(name);
		}
		else // domainlists
		{
			JSON_COPY_STR_TO_OBJECT(row, "domain", table.domain);
			JSON_REF_STR_IN_OBJECT(row, "type", table.type);
			JSON_REF_STR_IN_OBJECT(row, "kind", table.kind);
			JSON_COPY_STR_TO_OBJECT(row, "comment", table.comment);
		}

		// Groups don't have the groups property
		if(listtype != GRAVITY_GROUPS)
		{
			if(table.group_ids != NULL)
			{
				// Black magic at work here: We build a JSON array from
				// the group_concat result delivered from the database,
				// parse it as valid array and append it as row to the
				// data
				const size_t buflen = strlen(table.group_ids)+3u;
				char *group_ids_str = calloc(buflen, sizeof(char));
				group_ids_str[0] = '[';
				strcpy(group_ids_str+1u , table.group_ids);
				group_ids_str[buflen-2u] = ']';
				group_ids_str[buflen-1u] = '\0';
				cJSON * group_ids = cJSON_Parse(group_ids_str);
				free(group_ids_str);
				JSON_ADD_ITEM_TO_OBJECT(row, "groups", group_ids);
			}
			else
			{
				// Empty group set
				cJSON *group_ids = JSON_NEW_ARRAY();
				JSON_ADD_ITEM_TO_OBJECT(row, "groups", group_ids);
			}
		}

		// Clients don't have the enabled property
		if(listtype != GRAVITY_CLIENTS)
			JSON_ADD_BOOL_TO_OBJECT(row, "enabled", table.enabled);

		// Add read-only database parameters
		JSON_ADD_NUMBER_TO_OBJECT(row, "id", table.id);
		JSON_ADD_NUMBER_TO_OBJECT(row, "date_added", table.date_added);
		JSON_ADD_NUMBER_TO_OBJECT(row, "date_modified", table.date_modified);

		// Properties added in https://github.com/pi-hole/pi-hole/pull/3951
		if(listtype == GRAVITY_ADLISTS)
		{
			JSON_REF_STR_IN_OBJECT(row, "type", table.type);
			JSON_ADD_NUMBER_TO_OBJECT(row, "date_updated", table.date_updated);
			JSON_ADD_NUMBER_TO_OBJECT(row, "number", table.number);
			JSON_ADD_NUMBER_TO_OBJECT(row, "invalid_domains", table.invalid_domains);
			JSON_ADD_NUMBER_TO_OBJECT(row, "abp_entries", table.abp_entries);
			JSON_ADD_NUMBER_TO_OBJECT(row, "status", table.status);
		}

		JSON_ADD_ITEM_TO_ARRAY(rows, row);
	}
	gravityDB_readTableFinalize();

	if(sql_msg == NULL)
	{
		// No error, send domains array
		const char *objname;
		cJSON *json = JSON_NEW_OBJECT();
		if(listtype == GRAVITY_GROUPS)
			objname = "groups";
		else if(listtype == GRAVITY_ADLISTS)
			objname = "lists";
		else if(listtype == GRAVITY_CLIENTS)
			objname = "clients";
		else // domainlists
			objname = "domains";
		JSON_ADD_ITEM_TO_OBJECT(json, objname, rows);
		JSON_SEND_OBJECT_CODE(json, code);
	}
	else
	{
		JSON_DELETE(rows);
		return send_json_error(api, 400, // 400 Bad Request
		                       "database_error",
		                       "Could not read from gravity database",
		                       sql_msg);
	}
}

static int api_list_write(struct ftl_conn *api,
                          const enum gravity_list_type listtype,
                          const char *item)
{
	tablerow row = { 0 };

	// Check if valid JSON payload is available
	if (api->payload.json == NULL)
	{
		if (api->payload.json_error == NULL)
			return send_json_error(api, 400,
			                       "bad_request",
			                       "No request body data",
			                       NULL);
		else
			return send_json_error(api, 400,
			                       "bad_request",
			                       "Invalid request body data (no valid JSON), error before hint",
			                       api->payload.json_error);
	}

	if(api->method == HTTP_POST)
	{
		// Extract domain/name/client/address from payload when using POST, all
		// others specify it as URI-component
		cJSON *json_domain, *json_name, *json_address, *json_client;
		switch(listtype)
		{
			case GRAVITY_DOMAINLIST_ALLOW_EXACT:
			case GRAVITY_DOMAINLIST_ALLOW_REGEX:
			case GRAVITY_DOMAINLIST_DENY_EXACT:
			case GRAVITY_DOMAINLIST_DENY_REGEX:
				json_domain = cJSON_GetObjectItemCaseSensitive(api->payload.json, "domain");
				if(cJSON_IsString(json_domain) && strlen(json_domain->valuestring) > 0)
				{
					row.item = json_domain->valuestring;
				}
				else
				{
					return send_json_error(api, 400,
					                       "bad_request",
					                       "Invalid request: No valid item \"domain\" in payload",
					                       NULL);
				}
				break;

			case GRAVITY_GROUPS:
				json_name = cJSON_GetObjectItemCaseSensitive(api->payload.json, "name");
				if(cJSON_IsString(json_name) && strlen(json_name->valuestring) > 0)
					row.item = json_name->valuestring;
				else
				{
					return send_json_error(api, 400,
					                       "bad_request",
					                       "Invalid request: No valid item \"name\" in payload",
					                       NULL);
				}
				break;

			case GRAVITY_CLIENTS:
				json_client = cJSON_GetObjectItemCaseSensitive(api->payload.json, "client");
				if(cJSON_IsString(json_client) && strlen(json_client->valuestring) > 0)
					row.item = json_client->valuestring;
				else
				{
					return send_json_error(api, 400,
					                       "bad_request",
					                       "Invalid request: No valid item \"client\" in payload",
					                       NULL);
				}
				break;

			case GRAVITY_ADLISTS:
				json_address = cJSON_GetObjectItemCaseSensitive(api->payload.json, "address");
				if(cJSON_IsString(json_address) && strlen(json_address->valuestring) > 0)
					row.item = json_address->valuestring;
				else
				{
					return send_json_error(api, 400,
					                       "bad_request",
					                       "Invalid request: No valid item \"address\" in payload",
					                       NULL);
				}
				break;

			// Aggregate types (and gravity) are not handled by this routine
			case GRAVITY_DOMAINLIST_ALL_ALL:
			case GRAVITY_DOMAINLIST_ALL_EXACT:
			case GRAVITY_DOMAINLIST_ALL_REGEX:
			case GRAVITY_DOMAINLIST_ALLOW_ALL:
			case GRAVITY_DOMAINLIST_DENY_ALL:
			case GRAVITY_GRAVITY:
			case GRAVITY_ANTIGRAVITY:
				return 400;
		}
	}
	else
	{
		// PUT = Use URI item
		row.item = item;
	}

	cJSON *json_comment = cJSON_GetObjectItemCaseSensitive(api->payload.json, "comment");
	if(cJSON_IsString(json_comment) && strlen(json_comment->valuestring) > 0)
		row.comment = json_comment->valuestring;
	else
		row.comment = NULL; // Default value


	// Check if there is a type field in the payload (only for lists)
	if(listtype == GRAVITY_ADLISTS)
	{
		cJSON *json_type = cJSON_GetObjectItemCaseSensitive(api->payload.json, "type");
		if(cJSON_IsString(json_type) && strlen(json_type->valuestring) > 0)
			row.type_int = strcasecmp(json_type->valuestring, "allow") == 0 ? ADLIST_ALLOW : ADLIST_BLOCK;
		else
		{
			return send_json_error(api, 400,
			                       "bad_request",
			                       "Invalid request: No valid item \"type\" in payload",
			                       NULL);
		}
	}
	else
	{
		cJSON *json_type = cJSON_GetObjectItemCaseSensitive(api->payload.json, "type");
		if(cJSON_IsString(json_type) && strlen(json_type->valuestring) > 0)
			row.type = json_type->valuestring;
		else
			row.type = NULL; // Default value
	}

	cJSON *json_kind = cJSON_GetObjectItemCaseSensitive(api->payload.json, "kind");
	if(cJSON_IsString(json_kind) && strlen(json_kind->valuestring) > 0)
		row.kind = json_kind->valuestring;
	else
		row.kind = NULL; // Default value

	cJSON *json_enabled = cJSON_GetObjectItemCaseSensitive(api->payload.json, "enabled");
	if (cJSON_IsBool(json_enabled))
		row.enabled = cJSON_IsTrue(json_enabled);
	else
		row.enabled = true; // Default value

	cJSON *json_name = cJSON_GetObjectItemCaseSensitive(api->payload.json, "name");
	if(cJSON_IsString(json_name) && strlen(json_name->valuestring) > 0)
		row.name = json_name->valuestring;
	else
		row.name = NULL; // Default value

	bool okay = true;
	char *regex_msg = NULL;
	if(listtype == GRAVITY_DOMAINLIST_ALLOW_REGEX || listtype == GRAVITY_DOMAINLIST_DENY_REGEX)
	{
		// Test validity of this regex
		regexData regex = { 0 };
		okay = compile_regex(row.item, &regex, &regex_msg);
	}

	// Try to add item to table
	const char *sql_msg = NULL;
	if(okay && (okay = gravityDB_addToTable(listtype, &row, &sql_msg, api->method)))
	{
		if(listtype != GRAVITY_GROUPS)
		{
			cJSON *groups = cJSON_GetObjectItemCaseSensitive(api->payload.json, "groups");
			if(groups != NULL)
				okay = gravityDB_edit_groups(listtype, groups, &row, &sql_msg);
			else
				// The groups array is optional, we still succeed if it
				// is omitted (groups stay as they are)
				okay = true;
		}
		else
		{
			// Groups cannot be assigned to groups
			okay = true;
		}
	}
	if(!okay)
	{
		// Error adding item, prepare error object
		const char *errortype = "database_error";
		const char *errormsg  = "Could not add to gravity database";
		const char *hint = sql_msg;
		if (regex_msg != NULL)
		{
			// Change error type and message
			errortype = "regex_error";
			errormsg = "Regex validation failed";
			hint = regex_msg;
		}

		// Send error reply
		return send_json_error(api, 400, // 400 Bad Request
		                       errortype,
		                       errormsg,
		                       hint);
	}
	// else: everything is okay

	// Inform the resolver that it needs to reload the domainlists
	set_event(RELOAD_GRAVITY);

	int response_code = 201; // 201 - Created
	if(api->method == HTTP_PUT)
		response_code = 200; // 200 - OK
	// Send GET style reply
	return api_list_read(api, response_code, listtype, row.item);
}

static int api_list_remove(struct ftl_conn *api,
                           const enum gravity_list_type listtype,
                           const char *item)
{
	const char *sql_msg = NULL;
	if(gravityDB_delFromTable(listtype, item, &sql_msg))
	{
		// Inform the resolver that it needs to reload the domainlists
		set_event(RELOAD_GRAVITY);

		// Send empty reply with code 204 No Content
		cJSON *json = JSON_NEW_OBJECT();
		JSON_SEND_OBJECT_CODE(json, 204);
	}
	else
	{
		// Send error reply
		return send_json_error(api, 400,
		                       "database_error",
		                       "Could not remove domain from database table",
		                       sql_msg);
	}
}

int api_list(struct ftl_conn *api)
{
	enum gravity_list_type listtype;
	bool can_modify = false;
	if((api->item = startsWith("/api/groups", api)) != NULL)
	{
		listtype = GRAVITY_GROUPS;
		can_modify = true;
	}
	else if((api->item = startsWith("/api/lists", api)) != NULL)
	{
		listtype = GRAVITY_ADLISTS;
		can_modify = true;
	}
	else if((api->item = startsWith("/api/clients", api)) != NULL)
	{
		listtype = GRAVITY_CLIENTS;
		can_modify = true;
	}
	else if((api->item = startsWith("/api/domains/allow/exact", api)) != NULL)
	{
		listtype = GRAVITY_DOMAINLIST_ALLOW_EXACT;
		can_modify = true;
	}
	else if((api->item = startsWith("/api/domains/allow/regex", api)) != NULL)
	{
		listtype = GRAVITY_DOMAINLIST_ALLOW_REGEX;
		can_modify = true;
	}
	else if((api->item = startsWith("/api/domains/allow", api)) != NULL)
	{
			listtype = GRAVITY_DOMAINLIST_ALLOW_ALL;
	}
	else if((api->item = startsWith("/api/domains/deny/exact", api)) != NULL)
	{
		listtype = GRAVITY_DOMAINLIST_DENY_EXACT;
		can_modify = true;
	}
	else if((api->item = startsWith("/api/domains/deny/regex", api)) != NULL)
	{
		listtype = GRAVITY_DOMAINLIST_DENY_REGEX;
		can_modify = true;
	}
	else if((api->item = startsWith("/api/domains/deny", api)) != NULL)
	{
		listtype = GRAVITY_DOMAINLIST_DENY_ALL;
	}
	else if((api->item = startsWith("/api/domains/exact", api)) != NULL)
	{
		listtype = GRAVITY_DOMAINLIST_ALL_EXACT;
	}
	else if((api->item = startsWith("/api/domains/regex", api)) != NULL)
	{
		listtype = GRAVITY_DOMAINLIST_ALL_REGEX;
	}
	else if((api->item = startsWith("/api/domains", api)) != NULL)
	{
		listtype = GRAVITY_DOMAINLIST_ALL_ALL;
	}
	else
	{
			return send_json_error(api, 400,
			                       "bad_request",
			                       "Invalid request: Specified endpoint not available",
			                       api->request->local_uri_raw);
	}

	if(api->method == HTTP_GET)
	{
		// Read list item identified by URI (or read them all)
		// We would not actually need the SHM lock here, however, we do
		// this for simplicity to ensure nobody else is editing the
		// lists while we're doing this here
		lock_shm();
		const int ret = api_list_read(api, 200, listtype, api->item);
		unlock_shm();
		return ret;
	}
	else if(can_modify && api->method == HTTP_PUT)
	{
		// Add/update item identified by URI
		if(api->item != NULL && strlen(api->item) == 0)
		{
			return send_json_error(api, 400,
			                       "uri_error",
			                       "Invalid request: Specify item in URI",
			                       NULL);
		}
		else
		{
			// We would not actually need the SHM lock here,
			// however, we do this for simplicity to ensure nobody
			// else is editing the lists while we're doing this here
			lock_shm();
			const int ret = api_list_write(api, listtype, api->item);
			unlock_shm();
			return ret;
		}
	}
	else if(can_modify && api->method == HTTP_POST)
	{
		// Add item to list identified by payload
		if(api->item != NULL && strlen(api->item) != 0)
		{
			return send_json_error(api, 400,
			                       "uri_error",
			                       "Invalid request: Specify item in payload, not as URI parameter",
			                       NULL);
		}
		else
		{
			// We would not actually need the SHM lock here,
			// however, we do this for simplicity to ensure nobody
			// else is editing the lists while we're doing this here
			lock_shm();
			const int ret = api_list_write(api, listtype, api->item);
			unlock_shm();
			return ret;
		}
	}
	else if(can_modify && api->method == HTTP_DELETE)
	{
		// Delete item from list
		// We would not actually need the SHM lock here, however, we do
		// this for simplicity to ensure nobody else is editing the
		// lists while we're doing this here
		lock_shm();
		const int ret = api_list_remove(api, listtype, api->item);
		unlock_shm();
		return ret;
	}
	else if(!can_modify)
	{
		// This list type cannot be modified (e.g., ALL_ALL)
		return send_json_error(api, 400,
		                       "uri_error",
		                       "Invalid request: Specify list to modify more precisely",
		                       api->request->local_uri_raw);
	}
	else
	{
		// This results in error 404
		return 0;
	}
}
