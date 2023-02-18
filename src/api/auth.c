/* Pi-hole: A black hole for Internet advertisements
*  (c) 2019 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  API Implementation /api/auth
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "../FTL.h"
#include "../webserver/http-common.h"
#include "../webserver/json_macros.h"
#include "api.h"
#include "../log.h"
#include "../config/config.h"
// get_password_hash()
#include "../setupVars.h"
// (un)lock_shm()
#include "../shmem.h"

// crypto library
#include <nettle/sha2.h>
#include <nettle/base64.h>
#include <nettle/version.h>

// On 2017-08-27 (after v3.3, before v3.4), nettle changed the type of
// destination from uint_8t* to char* in all base64 and base16 functions
// (armor-signedness branch). This is a breaking change as this is a change in
// signedness causing issues when compiling FTL against older versions of
// nettle. We create this constant here to have a conversion if necessary.
// See https://github.com/gnutls/nettle/commit/f2da403135e2b2f641cf0f8219ad5b72083b7dfd
#if NETTLE_VERSION_MAJOR == 3 && NETTLE_VERSION_MINOR < 4
#define NETTLE_SIGN (uint8_t*)
#else
#define NETTLE_SIGN
#endif

// How many bits should the SID use?
#define SID_BITSIZE 128
#define SID_SIZE BASE64_ENCODE_RAW_LENGTH(SID_BITSIZE/8) + 1

// SameSite=Strict: Defense against some classes of cross-site request forgery
// (CSRF) attacks. This ensures the session cookie will only be sent in a
// first-party (i.e., Pi-hole) context and NOT be sent along with requests
// initiated by third party websites.
//
// HttpOnly: the cookie cannot be accessed through client side script (if the
// browser supports this flag). As a result, even if a cross-site scripting
// (XSS) flaw exists, and a user accidentally accesses a link that exploits this
// flaw, the browser (primarily Internet Explorer) will not reveal the cookie to
// a third party.
#define FTL_SET_COOKIE "Set-Cookie: sid=%s; SameSite=Strict; Path=/; Max-Age=%u; HttpOnly\r\n"
#define FTL_DELETE_COOKIE "Set-Cookie: sid=deleted; SameSite=Strict; Path=/; Max-Age=-1\r\n"

static struct {
	bool used;
	time_t login_at;
	time_t valid_until;
	char remote_addr[48]; // Large enough for IPv4 and IPv6 addresses, hard-coded in civetweb.h as mg_request_info.remote_addr
	char user_agent[128];
	char sid[SID_SIZE];
} auth_data[API_MAX_CLIENTS] = {{false, 0, 0, {0}, {0}, {0}}};

#define CHALLENGE_SIZE (2*SHA256_DIGEST_SIZE)
static struct {
	char challenge[CHALLENGE_SIZE + 1];
	char response[CHALLENGE_SIZE + 1];
	time_t valid_until;
} challenges[API_MAX_CHALLENGES] = {{{0}, {0}, 0}};

// Convert RAW data into hex representation
// Two hexadecimal digits are generated for each input byte.
static void sha256_hex(uint8_t *data, char *buffer)
{
	for (unsigned int i = 0; i < SHA256_DIGEST_SIZE; i++)
	{
		sprintf(buffer, "%02x", data[i]);
		buffer += 2;
	}
}

// Can we validate this client?
// Returns -1 if not authenticated or expired
// Returns >= 0 for any valid authentication
#define LOCALHOSTv4 "127.0.0.1"
#define LOCALHOSTv6 "::1"
int check_client_auth(struct ftl_conn *api)
{
	// Is the user requesting from localhost?
	// This may be allowed without authentication depending on the configuration
	if(!config.webserver.api.localAPIauth.v.b && (strcmp(api->request->remote_addr, LOCALHOSTv4) == 0 ||
	                                              strcmp(api->request->remote_addr, LOCALHOSTv6) == 0))
		return API_AUTH_LOCALHOST;

	// Check if there is a password hash
	if(strlen(config.webserver.api.pwhash.v.s) == 0u)
		return API_AUTH_EMPTYPASS;

	// Does the client provide a session cookie?
	char sid[SID_SIZE];
	const char *sid_source = "cookie";
	// Try to extract SID from cookie
	bool sid_avail = http_get_cookie_str(api, "sid", sid, SID_SIZE);

	// If not, does the client provide a session ID via GET/POST?
	if(!sid_avail && api->payload.avail)
	{
		// Try to extract SID from form-encoded payload
		if(GET_VAR("sid", sid, api->payload.raw) > 0)
		{
			// "+" may have been replaced by " ", undo this here
			for(unsigned int i = 0; i < SID_SIZE; i++)
				if(sid[i] == ' ')
					sid[i] = '+';

			// Zero terminate SID string
			sid[SID_SIZE-1] = '\0';
			// Mention source of SID
			sid_source = "payload (form-data)";
			// Mark SID as available
			sid_avail = true;
		}
		// Try to extract SID from root of a possibly included JSON payload
		else if(api->payload.json != NULL)
		{
			cJSON *sid_obj = cJSON_GetObjectItem(api->payload.json, "sid");
			if(cJSON_IsString(sid_obj))
			{
				// Copy SID string
				strncpy(sid, sid_obj->valuestring, SID_SIZE - 1u);
				// Zero terminate SID string
				sid[SID_SIZE-1] = '\0';
				// Mention source of SID
				sid_source = "payload (JSON)";
				// Mark SID as available
				sid_avail = true;
			}
		}
	}

	// If not, does the client provide a session ID via HEADER?
	if(!sid_avail)
	{
		const char *sid_header = NULL;
		// Try to extract SID from header
		if((sid_header = mg_get_header(api->conn, "sid")) != NULL ||
		   (sid_header = mg_get_header(api->conn, "X-FTL-SID")) != NULL)
		{
			// Copy SID string
			strncpy(sid, sid_header, SID_SIZE - 1u);
			// Zero terminate SID string
			sid[SID_SIZE-1] = '\0';
			// Mention source of SID
			sid_source = "header";
			// Mark SID as available
			sid_avail = true;
		}
	}

	if(!sid_avail)
	{
		log_debug(DEBUG_API, "API Authentification: FAIL (no SID provided)");
		return API_AUTH_UNAUTHORIZED;
	}

	// else: Analyze SID
	int user_id = API_AUTH_UNAUTHORIZED;
	const time_t now = time(NULL);
	log_debug(DEBUG_API, "Read sid=\"%s\" from %s", sid, sid_source);

	for(unsigned int i = 0; i < API_MAX_CLIENTS; i++)
	{
		if(auth_data[i].used &&
		   auth_data[i].valid_until >= now &&
		   strcmp(auth_data[i].remote_addr, api->request->remote_addr) == 0 &&
		   strcmp(auth_data[i].sid, sid) == 0)
		{
			user_id = i;
			break;
		}
	}
	if(user_id > API_AUTH_UNAUTHORIZED)
	{
		// Authentication succesful:
		// - We know this client
		// - The session is (still) valid
		// - The IP matches the one we know for this SID

		// Update timestamp of this client to extend
		// the validity of their API authentication
		auth_data[user_id].valid_until = now + config.webserver.sessionTimeout.v.ui;

		// Update user cookie
		if(snprintf(pi_hole_extra_headers, sizeof(pi_hole_extra_headers),
		            FTL_SET_COOKIE,
		            auth_data[user_id].sid, config.webserver.sessionTimeout.v.ui) < 0)
		{
			return send_json_error(api, 500, "internal_error", "Internal server error", NULL);
		}

		if(config.debug.api.v.b)
		{
			char timestr[128];
			get_timestr(timestr, auth_data[user_id].valid_until, false, false);
			log_debug(DEBUG_API, "Recognized known user: user_id %i valid_until: %s remote_addr %s",
				user_id, timestr, auth_data[user_id].remote_addr);
		}
	}
	else
		log_debug(DEBUG_API, "API Authentification: FAIL (SID invalid/expired)");

	api->user_id = user_id;

	return user_id;
}

// Check received response
static bool check_response(const char *response, const time_t now)
{
	// Loop over all responses and try to validate response
	for(unsigned int i = 0; i < API_MAX_CHALLENGES; i++)
	{
		// Skip expired entries
		if(challenges[i].valid_until < now)
			continue;

		if(strcasecmp(challenges[i].response, response) == 0)
		{
			// This challange-response has been used
			// Invalidate to prevent replay attacks
			challenges[i].valid_until = 0;
			return true;
		}
	}

	// If transmitted challenge wasn't found -> this is an invalid auth request
	return false;
}

static int get_all_sessions(struct ftl_conn *api, cJSON *json)
{
	const time_t now = time(NULL);
	cJSON *sessions = JSON_NEW_ARRAY();
	for(int i = 0; i < API_MAX_CLIENTS; i++)
	{
		if(!auth_data[i].used)
			continue;
		cJSON *session = JSON_NEW_OBJECT();
		JSON_ADD_NUMBER_TO_OBJECT(session, "id", i);
		JSON_ADD_BOOL_TO_OBJECT(session, "current_session", i == api->user_id);
		JSON_ADD_BOOL_TO_OBJECT(session, "valid", auth_data[i].valid_until >= now);
		JSON_ADD_BOOL_TO_OBJECT(session, "login_at", auth_data[i].login_at);
		JSON_ADD_NUMBER_TO_OBJECT(session, "last_active", auth_data[i].valid_until - config.webserver.sessionTimeout.v.ui);
		JSON_ADD_NUMBER_TO_OBJECT(session, "valid_until", auth_data[i].valid_until);
		JSON_REF_STR_IN_OBJECT(session, "remote_addr", auth_data[i].remote_addr);
		JSON_REF_STR_IN_OBJECT(session, "user_agent", auth_data[i].user_agent);
		JSON_ADD_ITEM_TO_ARRAY(sessions, session);
	}
	JSON_ADD_ITEM_TO_OBJECT(json, "sessions", sessions);
	return 0;
}

static int get_session_object(struct ftl_conn *api, cJSON *json, const int user_id, const time_t now)
{
	// Authentication not needed
	if(user_id == API_AUTH_LOCALHOST || user_id == API_AUTH_EMPTYPASS)
	{
		cJSON *session = JSON_NEW_OBJECT();
		JSON_ADD_BOOL_TO_OBJECT(session, "valid", true);
		JSON_ADD_NULL_TO_OBJECT(session, "sid");
		JSON_ADD_NUMBER_TO_OBJECT(session, "validity", -1);
		JSON_ADD_ITEM_TO_OBJECT(json, "session", session);
		return 0;
	}

	// Valid session
	if(user_id > API_AUTH_UNAUTHORIZED && auth_data[user_id].used)
	{
		cJSON *session = JSON_NEW_OBJECT();
		JSON_ADD_BOOL_TO_OBJECT(session, "valid", true);
		JSON_REF_STR_IN_OBJECT(session, "sid", auth_data[user_id].sid);
		JSON_ADD_NUMBER_TO_OBJECT(session, "validity", auth_data[user_id].valid_until - now);
		JSON_ADD_ITEM_TO_OBJECT(json, "session", session);
		return 0;
	}

	// No valid session
	cJSON *session = JSON_NEW_OBJECT();
	JSON_ADD_BOOL_TO_OBJECT(session, "valid", false);
	JSON_ADD_NULL_TO_OBJECT(session, "sid");
	JSON_ADD_NUMBER_TO_OBJECT(session, "validity", -1);
	JSON_ADD_ITEM_TO_OBJECT(json, "session", session);
	return 0;
}

static void delete_session(const int user_id)
{
	// Skip if nothing to be done here
	if(user_id < 0)
		return;

	auth_data[user_id].used = false;
	auth_data[user_id].valid_until = 0;
	memset(auth_data[user_id].sid, 0, sizeof(auth_data[user_id].sid));
	memset(auth_data[user_id].remote_addr, 0, sizeof(auth_data[user_id].remote_addr));
	memset(auth_data[user_id].user_agent, 0, sizeof(auth_data[user_id].user_agent));
}

void delete_all_sessions(void)
{
	for(unsigned int i = 0; i < API_MAX_CLIENTS; i++)
		delete_session(i);
}

static int send_api_auth_status(struct ftl_conn *api, const int user_id, const time_t now)
{
	if(user_id == API_AUTH_LOCALHOST)
	{
		log_debug(DEBUG_API, "API Auth status: OK (localhost does not need auth)");

		cJSON *json = JSON_NEW_OBJECT();
		JSON_ADD_NULL_TO_OBJECT(json, "challenge");
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT(json);
	}

	if(user_id == API_AUTH_EMPTYPASS)
	{
		log_debug(DEBUG_API, "API Auth status: OK (empty password)");

		cJSON *json = JSON_NEW_OBJECT();
		JSON_ADD_NULL_TO_OBJECT(json, "challenge");
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT(json);
	}

	if(user_id > API_AUTH_UNAUTHORIZED && (api->method == HTTP_GET || api->method == HTTP_POST))
	{
		log_debug(DEBUG_API, "API Auth status: OK");

		// Ten minutes validity
		if(snprintf(pi_hole_extra_headers, sizeof(pi_hole_extra_headers),
		            FTL_SET_COOKIE,
		            auth_data[user_id].sid, config.webserver.sessionTimeout.d.ui) < 0)
		{
			return send_json_error(api, 500, "internal_error", "Internal server error", NULL);
		}

		cJSON *json = JSON_NEW_OBJECT();
		JSON_ADD_NULL_TO_OBJECT(json, "challenge");
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT(json);
	}
	else if(user_id > API_AUTH_UNAUTHORIZED && api->method == HTTP_DELETE)
	{
		log_debug(DEBUG_API, "API Auth status: Logout, asking to delete cookie");

		// Revoke client authentication. This slot can be used by a new client afterwards.
		delete_session(user_id);

		strncpy(pi_hole_extra_headers, FTL_DELETE_COOKIE, sizeof(pi_hole_extra_headers));
		cJSON *json = JSON_NEW_OBJECT();
		JSON_ADD_NULL_TO_OBJECT(json, "challenge");
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT_CODE(json, 410); // 410 Gone
	}
	else
	{
		log_debug(DEBUG_API, "API Auth status: Invalid, asking to delete cookie");

		strncpy(pi_hole_extra_headers, FTL_DELETE_COOKIE, sizeof(pi_hole_extra_headers));
		cJSON *json = JSON_NEW_OBJECT();
		JSON_ADD_NULL_TO_OBJECT(json, "challenge");
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT_CODE(json, 401); // 401 Unauthorized
	}
}

static void generateChallenge(const unsigned int idx, const time_t now)
{
	uint8_t raw_challenge[SHA256_DIGEST_SIZE];
	for(unsigned i = 0; i < SHA256_DIGEST_SIZE; i+= 4)
	{
		const long rval = random();
		raw_challenge[i] = rval & 0xFF;
		raw_challenge[i+1] = (rval >> 8) & 0xFF;
		raw_challenge[i+2] = (rval >> 16) & 0xFF;
		raw_challenge[i+3] = (rval >> 24) & 0xFF;
	}
	sha256_hex(raw_challenge, challenges[idx].challenge);
	challenges[idx].valid_until = now + API_CHALLENGE_TIMEOUT;
}

static void generateResponse(const unsigned int idx)
{
	uint8_t raw_response[SHA256_DIGEST_SIZE];
	struct sha256_ctx ctx;
	sha256_init(&ctx);

	// Add challenge in hex representation
	sha256_update(&ctx,
	              sizeof(challenges[idx].challenge)-1,
	              (uint8_t*)challenges[idx].challenge);

	// Add separator
	sha256_update(&ctx, 1, (uint8_t*)":");

	// Get and add password hash from setupVars.conf
	sha256_update(&ctx,
	              strlen(config.webserver.api.pwhash.v.s),
	              (uint8_t*)config.webserver.api.pwhash.v.s);

	sha256_digest(&ctx, SHA256_DIGEST_SIZE, raw_response);
	sha256_hex(raw_response, challenges[idx].response);
}

static void generateSID(char *sid)
{
	uint8_t raw_sid[SID_SIZE];
	for(unsigned i = 0; i < (SID_BITSIZE/8); i+= 4)
	{
		const long rval = random();
		raw_sid[i] = rval & 0xFF;
		raw_sid[i+1] = (rval >> 8) & 0xFF;
		raw_sid[i+2] = (rval >> 16) & 0xFF;
		raw_sid[i+3] = (rval >> 24) & 0xFF;
	}
	base64_encode_raw(NETTLE_SIGN sid, SID_BITSIZE/8, raw_sid);
	sid[SID_SIZE-1] = '\0';
}

// api/auth
//  GET: Check authentication and obtain a challenge
//  POST: Login
//  DELETE: Logout
int api_auth(struct ftl_conn *api)
{
	// Check HTTP method
	const time_t now = time(NULL);

	const bool empty_password = strlen(config.webserver.api.pwhash.v.s) == 0u;

	int user_id = API_AUTH_UNAUTHORIZED;

	bool reponse_set = false;
	char response[CHALLENGE_SIZE+1u] = { 0 };

	// Did the client authenticate before and we can validate this?
	user_id = check_client_auth(api);

	// If this is a valid session, we can exit early at this point
	if(user_id != API_AUTH_UNAUTHORIZED)
		return send_api_auth_status(api, user_id, now);

	// Login attempt, extract response
	if(api->method == HTTP_POST)
	{
		// Try to extract response from payload
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

		// Check if response is available
		cJSON *json_response;
		if((json_response = cJSON_GetObjectItemCaseSensitive(api->payload.json, "response")) == NULL)
		{
			const char *message = "No response found in JSON payload";
			log_debug(DEBUG_API, "API auth error: %s", message);
			return send_json_error(api, 400,
			                       "bad_request",
			                       message,
			                       NULL);
		}

		// Check response length
		if(strlen(json_response->valuestring) != CHALLENGE_SIZE)
		{
			const char *message = "Invalid response length";
			log_debug(DEBUG_API, "API auth error: %s", message);
			return send_json_error(api, 400,
			                       "bad_request",
			                       message,
			                       NULL);
		}

		// Accept challenge
		strncpy(response, json_response->valuestring, CHALLENGE_SIZE);
		// response is already null-terminated
		reponse_set = true;
	}

	// Logout attempt
	if(api->method == HTTP_DELETE)
	{
		log_debug(DEBUG_API, "API Auth: User with ID %i wants to log out", user_id);
		return send_api_auth_status(api, user_id, now);
	}

	// Login attempt and/or auth check
	if(reponse_set || empty_password)
	{
		// - Client tries to authenticate using a challenge response, or
		// - There no password on this machine
		const bool response_correct = check_response(response, now);
		if(response_correct || empty_password)
		{
			// Accepted
			for(unsigned int i = 0; i < API_MAX_CLIENTS; i++)
			{
				// Expired slow, mark as unused
				if(auth_data[i].used &&
				   auth_data[i].valid_until < now)
				{
					log_debug(DEBUG_API, "API: Session of client %u (%s) expired, freeing...",
					          i, auth_data[i].remote_addr);
					delete_session(i);
				}

				// Found unused authentication slot (might have been freed before)
				if(!auth_data[i].used)
				{
					// Mark as used
					auth_data[i].used = true;
					// Set validitiy to now + timeout
					auth_data[i].login_at = now;
					auth_data[i].valid_until = now + config.webserver.sessionTimeout.v.ui;
					// Set remote address
					strncpy(auth_data[i].remote_addr, api->request->remote_addr, sizeof(auth_data[i].remote_addr));
					auth_data[i].remote_addr[sizeof(auth_data[i].remote_addr)-1] = '\0';
					// Store user-agent (if available)
					const char *user_agent = mg_get_header(api->conn, "user-agent");
					if(user_agent != NULL)
					{
						strncpy(auth_data[i].user_agent, user_agent, sizeof(auth_data[i].user_agent));
						auth_data[i].user_agent[sizeof(auth_data[i].user_agent)-1] = '\0';
					}
					else
					{
						auth_data[i].user_agent[0] = '\0';
					}

					// Generate new SID
					generateSID(auth_data[i].sid);

					user_id = i;
					break;
				}
			}

			// Debug logging
			if(config.debug.api.v.b && user_id > API_AUTH_UNAUTHORIZED)
			{
				char timestr[128];
				get_timestr(timestr, auth_data[user_id].valid_until, false, false);
				log_debug(DEBUG_API, "API: Registered new user: user_id %i valid_until: %s remote_addr %s (accepted due to %s)",
				          user_id, timestr, auth_data[user_id].remote_addr,
				          response_correct ? "correct response" : "empty password");
			}
			if(user_id == API_AUTH_UNAUTHORIZED)
			{
				log_warn("No free API seats available, not authenticating client");
			}
		}
		else
		{
			log_debug(DEBUG_API, "API: Response incorrect. Response=%s, FTL=%s", response, config.webserver.api.pwhash.v.s);
		}

		// Free allocated memory
		return send_api_auth_status(api, user_id, now);
	}
	else
	{
		// Client wants to get a challenge
		// Generate a challenge
		unsigned int i;

		// Get an empty/expired slot
		for(i = 0; i < API_MAX_CHALLENGES; i++)
			if(challenges[i].valid_until < now)
				break;

		// If there are no empty/expired slots, then find the oldest challenge
		// and replace it
		if(i == API_MAX_CHALLENGES)
		{
			unsigned int minidx = 0;
			time_t minval = now;
			for(i = 0; i < API_MAX_CHALLENGES; i++)
			{
				if(challenges[i].valid_until < minval)
				{
					minval = challenges[i].valid_until;
					minidx = i;
				}
			}
			i = minidx;
		}

		// Generate and store new challenge
		generateChallenge(i, now);

		// Compute and store expected response for this challenge (SHA-256)
		generateResponse(i);

		log_debug(DEBUG_API, "API: Sending challenge=%s", challenges[i].challenge);

		// Return to user
		cJSON *json = JSON_NEW_OBJECT();
		JSON_REF_STR_IN_OBJECT(json, "challenge", challenges[i].challenge);
		get_session_object(api, json, -1, now);
		JSON_SEND_OBJECT(json);
	}
}

char * __attribute__((malloc)) hash_password(const char *password)
{
	char response[2 * SHA256_DIGEST_SIZE + 1] = { 0 };
	uint8_t raw_response[SHA256_DIGEST_SIZE];
	struct sha256_ctx ctx;

	// Hash password a first time
	sha256_init(&ctx);
	sha256_update(&ctx,
	              strlen(password),
	              (uint8_t*)password);

	sha256_digest(&ctx, SHA256_DIGEST_SIZE, raw_response);
	sha256_hex(raw_response, response);

	// Hash password a second time
	sha256_init(&ctx);
	sha256_update(&ctx,
	              strlen(response),
	              (uint8_t*)response);

	sha256_digest(&ctx, SHA256_DIGEST_SIZE, raw_response);
	sha256_hex(raw_response, response);

	return strdup(response);
}

int api_auth_session(struct ftl_conn *api)
{
	// Get session object
	cJSON *json = JSON_NEW_OBJECT();
	get_all_sessions(api, json);
	JSON_SEND_OBJECT(json);
}
