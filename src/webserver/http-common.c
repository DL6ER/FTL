/* Pi-hole: A black hole for Internet advertisements
*  (c) 2019 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Common HTTP server routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "../FTL.h"
#include "http-common.h"
#include "../config/config.h"
#include "../log.h"
#include "json_macros.h"
// UINT_MAX
#include <limits.h>
// HUGE_VAL
#include <math.h>

char pi_hole_extra_headers[PIHOLE_HEADERS_MAXLEN] = { 0 };

// Provides a compile-time flag for JSON formatting
// This should never be needed as all modern browsers
// typically contain a JSON explorer
// This string needs to be freed after using it
char *json_formatter(const cJSON *object)
{
	if(config.webserver.api.prettyJSON.v.b)
	{
		/* Examplary output:
		{
			"queries in database":	70,
			"database filesize":	49152,
			"SQLite version":	"3.30.1"
		}
		*/
		return cJSON_Print(object);
	}
	else
	{
		/* Exemplary output
		{"queries in database":70,"database filesize":49152,"SQLite version":"3.30.1"}
		*/
		return cJSON_PrintUnformatted(object);
	}
}

int send_http(struct ftl_conn *api, const char *mime_type,
              const char *msg)
{
	mg_send_http_ok(api->conn, mime_type, strlen(msg));
	return mg_write(api->conn, msg, strlen(msg));
}

int send_http_code(struct ftl_conn *api, const char *mime_type,
                   int code, const char *msg)
{
	// Payload will be sent with text/plain encoding due to
	// the first line being "Error <code>" by definition
	//return mg_send_http_error(conn, code, "%s", msg);
	my_send_http_error_headers(api->conn, code,
	                           mime_type,
	                           strlen(msg));

	return mg_write(api->conn, msg, strlen(msg));
}

int send_json_unauthorized(struct ftl_conn *api)
{
	return send_json_error(api, 401,
                               "unauthorized",
                               "Unauthorized",
                               NULL);
}

int send_json_error(struct ftl_conn *api, const int code,
                    const char *key, const char* message,
                    const char *hint)
{
	if(hint)
		log_warn("API: %s (%s)", message, hint);
	else
		log_warn("API: %s", message);

	cJSON *error = JSON_NEW_OBJECT();
	JSON_REF_STR_IN_OBJECT(error, "key", key);
	JSON_REF_STR_IN_OBJECT(error, "message", message);
	JSON_REF_STR_IN_OBJECT(error, "hint", hint);

	cJSON *json = JSON_NEW_OBJECT();
	JSON_ADD_ITEM_TO_OBJECT(json, "error", error);
	JSON_SEND_OBJECT_CODE(json, code);
}

int send_json_success(struct ftl_conn *api)
{
	cJSON *json = JSON_NEW_OBJECT();
	JSON_REF_STR_IN_OBJECT(json, "status", "success");
	JSON_SEND_OBJECT(json);
}

int send_http_internal_error(struct ftl_conn *api)
{
	return mg_send_http_error(api->conn, 500, "Internal server error");
}

bool get_bool_var(const char *source, const char *var, bool *boolean)
{
	char buffer[16] = { 0 };
	if(!source)
		return false;
	if(GET_VAR(var, buffer, source) > 0)
	{
		*boolean = (strcasecmp(buffer, "true") == 0);
		return true;
	}
	return false;
}

static bool get_long_var_msg(const char *source, const char *var, long *num, const char **msg)
{
	char buffer[128] = { 0 };
	if(GET_VAR(var, buffer, source) < 1)
	{
		// Parameter not found
		*msg = NULL;
		return false;
	}

	// Try to get the value
	char *endptr = NULL;
	errno = 0;
	const long val = strtol(buffer, &endptr, 10);

	// Error checking
	if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN)) ||
	    (errno != 0 && val == 0))
	{
		*msg = strerror(errno);
		return false;
	}

	if (endptr == buffer)
	{
		*msg = "No digits were found";
		return false;
	}

	// Otherwise: success
	*num = val;
	return true;
}

bool get_ulong_var_msg(const char *source, const char *var, unsigned long *num, const char **msg)
{
	char buffer[128] = { 0 };
	if(GET_VAR(var, buffer, source) < 1)
	{
		// Parameter not found
		*msg = NULL;
		return false;
	}

	// Try to get the value
	char *endptr = NULL;
	errno = 0;
	const unsigned long val = strtoul(buffer, &endptr, 10);

	// Error checking
	if ((errno == ERANGE && val == ULONG_MAX) ||
	    (errno != 0 && val == 0))
	{
		*msg = strerror(errno);
		return false;
	}

	if (endptr == buffer)
	{
		*msg = "No digits were found";
		return false;
	}

	// Otherwise: success
	*num = val;
	return true;
}

bool get_int_var_msg(const char *source, const char *var, int *num, const char **msg)
{
	long val = 0;
	if(!get_long_var_msg(source, var, &val, msg))
		return false;

	if(val > (long)INT_MAX)
	{
		*msg = "Specified integer too large, maximum allowed number is "  xstr(INT_MAX);
		return false;
	}

	if(val < (long)INT_MIN)
	{
		*msg = "Specified integer too negative, minimum allowed number is "  xstr(INT_MIN);
		return false;
	}

	*num = (int)val;
	return true;
}

bool get_int_var(const char *source, const char *var, int *num)
{
	const char *msg = NULL;
	if(!source)
		return false;
	return get_int_var_msg(source, var, num, &msg);
}

bool get_uint_var_msg(const char *source, const char *var, unsigned int *num, const char **msg)
{
	long val = 0;
	if(!get_long_var_msg(source, var, &val, msg))
		return false;

	if(val > (long)UINT_MAX)
	{
		*msg = "Specified integer too large, maximum allowed number is "  xstr(UINT_MAX);
		return false;
	}

	if(val < 0)
	{
		*msg = "Specified integer negative, this is not allowed";
		return false;
	}

	*num = (unsigned int)val;
	return true;
}

bool get_uint_var(const char *source, const char *var, unsigned int *num)
{
	const char *msg = NULL;
	if(!source)
		return false;
	return get_uint_var_msg(source, var, num, &msg);
}

bool get_double_var_msg(const char *source, const char *var, double *num, const char **msg)
{
	char buffer[128] = { 0 };
	if(!source)
		return false;
	if(GET_VAR(var, buffer, source) < 1)
	{
		// Parameter not found
		*msg = NULL;
		return false;
	}

	// Try to get the value
	char *endptr = NULL;
	errno = 0;
	const double val = strtod(buffer, &endptr);

	// Error checking
	if (errno != 0)
	{
		*msg = strerror(errno);
		return false;
	}

	if (endptr == buffer)
	{
		*msg = "No digits were found";
		return false;
	}

	// Otherwise: success
	*num = val;
	return true;
}

bool get_double_var(const char *source, const char *var, double *num)
{
	const char *msg = NULL;
	if(!source)
		return false;
	return get_double_var_msg(source, var, num, &msg);
}

const char* __attribute__((pure)) startsWith(const char *path, struct ftl_conn *api)
{
	// We use local_uri_raw here to get the unescaped URI, see
	// https://github.com/civetweb/civetweb/pull/975
	if(strncmp(path, api->request->local_uri_raw, strlen(path)) == 0)
		if(api->request->local_uri_raw[strlen(path)] == '/')
		{
			// Path match with argument after ".../"
			if(api->action_path != NULL)
				free(api->action_path);
			api->action_path = strdup(api->request->local_uri_raw);
			api->action_path[strlen(path)] = '\0';
			return api->request->local_uri_raw + strlen(path) + 1u;
		}
		else if(strlen(path) == strlen(api->request->local_uri_raw))
		{
			// Path match directly, no argument
			if(api->action_path != NULL)
				free(api->action_path);
			api->action_path = strdup(api->request->local_uri_raw);
			return "";
		}
		else
		{
			// Further components in URL, assume this did't match, e.g.
			// /api/domains/regex[123].com
			return NULL;
		}
	else
		// Path does not match
		return NULL;
}

bool http_get_cookie_int(struct ftl_conn *api, const char *cookieName, int *i)
{
	// Maximum cookie length is 4KB
	char cookieValue[4096];
	const char *cookie = mg_get_header(api->conn, "Cookie");
	if(mg_get_cookie(cookie, cookieName, cookieValue, sizeof(cookieValue)) > 0)
	{
		*i = atoi(cookieValue);
		return true;
	}
	return false;
}

bool http_get_cookie_str(struct ftl_conn *api, const char *cookieName, char *str, size_t str_size)
{
	const char *cookie = mg_get_header(api->conn, "Cookie");
	if(mg_get_cookie(cookie, cookieName, str, str_size) > 0)
	{
		return true;
	}
	return false;
}

enum http_method __attribute__((pure)) http_method(struct mg_connection *conn)
{
	const struct mg_request_info *request = mg_get_request_info(conn);
	if(strcmp(request->request_method, "GET") == 0)
		return HTTP_GET;
	else if(strcmp(request->request_method, "DELETE") == 0)
		return HTTP_DELETE;
	else if(strcmp(request->request_method, "PUT") == 0)
		return HTTP_PUT;
	else if(strcmp(request->request_method, "POST") == 0)
		return HTTP_POST;
	else if(strcmp(request->request_method, "PATCH") == 0)
		return HTTP_PATCH;
	else if(strcmp(request->request_method, "OPTIONS") == 0)
		return HTTP_OPTIONS;
	else
		return HTTP_UNKNOWN;
}

const char * __attribute__((const)) get_http_method_str(const enum http_method method)
{
	switch(method)
	{
		case HTTP_GET:
			return "GET";
		case HTTP_DELETE:
			return "DELETE";
		case HTTP_PUT:
			return "PUT";
		case HTTP_POST:
			return "POST";
		case HTTP_PATCH:
			return "PATCH";
		case HTTP_OPTIONS:
			return "OPTIONS";
		case HTTP_UNKNOWN: // fall through
		default:
			return "UNKNOWN";
	}
}

void read_and_parse_payload(struct ftl_conn *api)
{
	// Read payload
	api->payload.size = mg_read(api->conn, api->payload.raw, MAX_PAYLOAD_BYTES - 1);
	if (api->payload.size < 1)
	{
		log_debug(DEBUG_API, "Received no payload");
		return;
	}
	else if (api->payload.size >= MAX_PAYLOAD_BYTES-1)
	{
		// If we reached the upper limit of payload size, we have likely
		// truncated the payload. The only reasonable thing to do here is to
		// discard the payload altogether
		log_warn("API: Received too large payload - DISCARDING");
		return;
	}

	// Debug output of received payload (if enabled)
	log_debug(DEBUG_API, "Received payload with size: %lu", api->payload.size);

	// Terminate string
	api->payload.raw[api->payload.size] = '\0';

	// Set flag to indicate that we have a payload
	api->payload.avail = true;

	// Try to parse possibly existing JSON payload
	api->payload.json = cJSON_Parse(api->payload.raw);
	api->payload.json_error = cJSON_GetErrorPtr();
}
