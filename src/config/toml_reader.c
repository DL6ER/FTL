/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Config routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "toml_reader.h"
#include "config.h"
#include "setupVars.h"
#include "log.h"
// getprio(), setprio()
#include <sys/resource.h>
// argv_dnsmasq
#include "args.h"
// INT_MAX
#include <limits.h>

#include "tomlc99/toml.h"
#include "../datastructure.h"
// openFTLtoml()
#include "toml_helper.h"

// Private prototypes
static toml_table_t *parseTOML(void);
static void reportDebugConfig(void);

bool readFTLtoml(void)
{
	// Initialize config with default values
	setDefaults();

	// We read the debug setting first so DEBUG_CONFIG can already
	readDebugSettings();

	log_debug(DEBUG_CONFIG, "Reading TOML config file: full config");

	// Parse lines in the config file
	toml_table_t *conf = parseTOML();
	if(!conf)
		return false;

	// Read [dns] section
	toml_table_t *dns = toml_table_in(conf, "dns");
	if(dns)
	{
		getBlockingMode();

		toml_datum_t cname_deep_inspect = toml_bool_in(dns, "CNAMEdeepInspect");
		if(cname_deep_inspect.ok)
			config.cname_deep_inspection = cname_deep_inspect.u.b;
		else
			log_debug(DEBUG_CONFIG, "dns.CNAMEdeepInspect DOES NOT EXIST");

		toml_datum_t block_esni = toml_bool_in(dns, "blockESNI");
		if(block_esni.ok)
			config.blockESNI = block_esni.u.b;
		else
			log_debug(DEBUG_CONFIG, "dns.blockESNI DOES NOT EXIST");

		toml_datum_t edns0_ecs = toml_bool_in(dns, "EDNS0ECS");
		if(edns0_ecs.ok)
			config.edns0_ecs = edns0_ecs.u.b;
		else
			log_debug(DEBUG_CONFIG, "dns.EDNS0ECS DOES NOT EXIST");

		toml_datum_t ignore_localhost = toml_bool_in(dns, "ignoreLocalhost");
		if(ignore_localhost.ok)
			config.ignore_localhost = ignore_localhost.u.b;
		else
			log_debug(DEBUG_CONFIG, "dns.ignoreLocalhost DOES NOT EXIST");

		toml_datum_t showDNSSEC = toml_bool_in(dns, "showDNSSEC");
		if(showDNSSEC.ok)
			config.show_dnssec = showDNSSEC.u.b;
		else
			log_debug(DEBUG_CONFIG, "dns.showDNSSEC DOES NOT EXIST");

		toml_datum_t piholePTR = toml_string_in(dns, "piholePTR");
		if(piholePTR.ok)
		{
			if(strcasecmp(piholePTR.u.s, "none") == 0 ||
			   strcasecmp(piholePTR.u.s, "false") == 0)
				config.pihole_ptr = PTR_NONE;
			else if(strcasecmp(piholePTR.u.s, "hostname") == 0)
				config.pihole_ptr = PTR_HOSTNAME;
			else if(strcasecmp(piholePTR.u.s, "hostnamefqdn") == 0)
				config.pihole_ptr = PTR_HOSTNAMEFQDN;
		}
		else
			log_debug(DEBUG_CONFIG, "dns.piholePTR DOES NOT EXIST");

		toml_datum_t replyWhenBusy = toml_string_in(dns, "replyWhenBusy");
		if(replyWhenBusy.ok)
		{
			if(strcasecmp(replyWhenBusy.u.s, "DROP") == 0)
				config.reply_when_busy = BUSY_DROP;
			else if(strcasecmp(replyWhenBusy.u.s, "REFUSE") == 0)
				config.reply_when_busy = BUSY_REFUSE;
			else if(strcasecmp(replyWhenBusy.u.s, "BLOCK") == 0)
				config.reply_when_busy = BUSY_BLOCK;
		}
		else
			log_debug(DEBUG_CONFIG, "dns.replyWhenBusy DOES NOT EXIST");

		toml_datum_t blockTTL = toml_int_in(dns, "blockTTL");
		if(blockTTL.ok)
			config.block_ttl = blockTTL.u.i;
		else
			log_debug(DEBUG_CONFIG, "dns.blockTTL DOES NOT EXIST");

		// Read [dns.specialDomains] section
		toml_table_t *specialDomains = toml_table_in(dns, "specialDomains");
		if(specialDomains)
		{
			toml_datum_t mozillaCanary = toml_bool_in(specialDomains, "mozillaCanary");
			if(mozillaCanary.ok)
				config.special_domains.mozilla_canary = mozillaCanary.u.b;
			else
				log_debug(DEBUG_CONFIG, "dns.specialDomains.mozillaCanary DOES NOT EXIST");

			toml_datum_t blockICloudPR = toml_bool_in(specialDomains, "blockICloudPR");
			if(blockICloudPR.ok)
				config.special_domains.mozilla_canary = blockICloudPR.u.b;
			else
				log_debug(DEBUG_CONFIG, "dns.specialDomains.blockICloudPR DOES NOT EXIST");
		}
		else
			log_debug(DEBUG_CONFIG, "dns.specialDomains DOES NOT EXIST");

		// Read [dns.reply] section
		toml_table_t *reply = toml_table_in(dns, "reply");
		if(reply)
		{
			// Read [dns.reply.own_host] section
			toml_table_t *own_host = toml_table_in(reply, "own_host");
			if(own_host)
			{
				toml_datum_t ipv4 = toml_string_in(own_host, "IPv4");
				if(ipv4.ok)
				{
					if(inet_pton(AF_INET, ipv4.u.s, &config.reply_addr.own_host.v4))
						config.reply_addr.own_host.overwrite_v4 = true;
					free(ipv4.u.s);
				}
				else
					log_debug(DEBUG_CONFIG, "dns.reply.own_host.IPv4 DOES NOT EXIST");

				toml_datum_t ipv6 = toml_string_in(own_host, "IPv6");
				if(ipv6.ok)
				{
					if(inet_pton(AF_INET, ipv6.u.s, &config.reply_addr.own_host.v6))
						config.reply_addr.own_host.overwrite_v6 = true;
					free(ipv6.u.s);
				}
				else
					log_debug(DEBUG_CONFIG, "dns.reply.own_host.IPv6 DOES NOT EXIST");
			}
			else
			{
				log_debug(DEBUG_CONFIG, "dns.reply.own_host DOES NOT EXIST");
			}
			// Read [dns.reply.ip_blocking] section
			toml_table_t *ip_blocking = toml_table_in(reply, "ip_blocking");
			if(ip_blocking)
			{
				toml_datum_t ipv4 = toml_string_in(ip_blocking, "IPv4");
				if(ipv4.ok)
				{
					if(inet_pton(AF_INET, ipv4.u.s, &config.reply_addr.ip_blocking.v4))
						config.reply_addr.ip_blocking.overwrite_v4 = true;
					free(ipv4.u.s);
				}
				else
					log_debug(DEBUG_CONFIG, "dns.reply.ip_blocking.IPv4 DOES NOT EXIST");

				toml_datum_t ipv6 = toml_string_in(ip_blocking, "IPv6");
				if(ipv6.ok)
				{
					if(inet_pton(AF_INET, ipv6.u.s, &config.reply_addr.ip_blocking.v6))
						config.reply_addr.ip_blocking.overwrite_v6 = true;
					free(ipv6.u.s);
				}
				else
					log_debug(DEBUG_CONFIG, "dns.reply.ip_blocking.IPv6 DOES NOT EXIST");
			}
			else
			{
				log_debug(DEBUG_CONFIG, "dns.reply.ip_blocking DOES NOT EXIST");
			}
		}
		else
			log_debug(DEBUG_CONFIG, "dns.reply DOES NOT EXIST");

		// Read [dns.rate_limit] section
		toml_table_t *rate_limit = toml_table_in(dns, "rateLimit");
		if(rate_limit)
		{
			toml_datum_t count = toml_int_in(rate_limit, "count");
			if(count.ok)
				config.rate_limit.count = count.u.i;
			else
				log_debug(DEBUG_CONFIG, "dns.rateLimit.count DOES NOT EXIST");

			toml_datum_t interval = toml_int_in(rate_limit, "interval");
			if(interval.ok)
				config.rate_limit.interval = interval.u.i;
			else
				log_debug(DEBUG_CONFIG, "dns.rateLimit.interval DOES NOT EXIST");
		}
		else
			log_debug(DEBUG_CONFIG, "dns.rateLimit DOES NOT EXIST");
	}
	else
		log_debug(DEBUG_CONFIG, "dns DOES NOT EXIST");

	// Read [resolver] section
	toml_table_t *resolver = toml_table_in(conf, "resolver");
	if(resolver)
	{
		toml_datum_t resolve_ipv4 = toml_bool_in(resolver, "resolveIPv4");
		if(resolve_ipv4.ok)
			config.resolveIPv4 = resolve_ipv4.u.b;
		else
			log_debug(DEBUG_CONFIG, "resolver.resolveIPv4 DOES NOT EXIST");

		toml_datum_t resolve_ipv6 = toml_bool_in(resolver, "resolveIPv6");
		if(resolve_ipv6.ok)
			config.resolveIPv6 = resolve_ipv6.u.b;
		else
			log_debug(DEBUG_CONFIG, "resolver.resolveIPv6 DOES NOT EXIST");

		toml_datum_t network_names = toml_bool_in(resolver, "networkNames");
		if(network_names.ok)
			config.networkNames = network_names.u.b;
		else
			log_debug(DEBUG_CONFIG, "resolver.networkNames DOES NOT EXIST");

		toml_datum_t refresh = toml_string_in(resolver, "refresh");
		if(refresh.ok)
		{
			// Iterate over possible blocking modes and check if it applies
			bool found = false;
			for(enum refresh_hostnames rh = REFRESH_ALL; rh <= REFRESH_NONE; rh++)
			{
				const char *rhstr = get_refresh_hostnames_str(rh);
				if(strcasecmp(rhstr, refresh.u.s) == 0)
				{
					config.refresh_hostnames = rh;
					found = true;
					break;
				}
			}
			if(!found)
				log_warn("Unknown hostname refresh mode, using default");
			free(refresh.u.s);
		}
		else
			log_debug(DEBUG_CONFIG, "resolver.refresh DOES NOT EXIST");
	}
	else
		log_debug(DEBUG_CONFIG, "resolver DOES NOT EXIST");

	// Read [database] section
	toml_table_t *database = toml_table_in(conf, "database");
	if(database)
	{
		toml_datum_t dbimport = toml_bool_in(database, "DBimport");
		if(dbimport.ok)
			config.DBimport = dbimport.u.b;
		else
			log_debug(DEBUG_CONFIG, "database.DBimport DOES NOT EXIST");

		toml_datum_t max_history = toml_int_in(database, "maxHistory");
		if(max_history.ok)
		{
			// Sanity check
			if(max_history.u.i >= 0.0 && max_history.u.i <= MAXLOGAGE * 3600)
				config.maxHistory = max_history.u.i;
			else
				log_warn("Invalid setting for database.maxHistory, using default");
		}
		else
			log_debug(DEBUG_CONFIG, "database.maxHistory DOES NOT EXIST");

		toml_datum_t maxdbdays = toml_int_in(database, "maxDBdays");
		if(maxdbdays.ok)
		{
			const int maxdbdays_max = INT_MAX / 24 / 60 / 60;
			// Prevent possible overflow
			if(maxdbdays.u.i > maxdbdays_max)
				config.maxDBdays = maxdbdays_max;

			// Only use valid values
			else if(maxdbdays.u.i == -1 || maxdbdays.u.i >= 0)
				config.maxDBdays = maxdbdays.u.i;
			else
				log_warn("Invalid setting for database.maxDBdays, using default");
		}
		else
			log_debug(DEBUG_CONFIG, "database.maxDBdays DOES NOT EXIST");

		toml_datum_t dbinterval = toml_int_in(database, "DBinterval");
		if(dbinterval.ok)
		{
			// check if the read value is
			// - larger than 10sec, and
			// - smaller than 24*60*60sec (once a day)
			if(dbinterval.u.i >= 10 && dbinterval.u.i <= 24*60*60)
				config.DBinterval = dbinterval.u.i;
			else
				log_warn("Invalid setting for database.DBinterval, using default");
		}
		else
			log_debug(DEBUG_CONFIG, "database.DBinterval DOES NOT EXIST");

		// Read [database.network] section
		toml_table_t *network = toml_table_in(database, "network");
		if(network)
		{
			toml_datum_t parse_arp = toml_bool_in(network, "parseARP");
			if(parse_arp.ok)
				config.parse_arp_cache = parse_arp.u.b;
			else
				log_debug(DEBUG_CONFIG, "database.network.parseARP DOES NOT EXIST");

			toml_datum_t expire = toml_int_in(network, "expire");
			if(expire.ok)
			{
				// Only use valid values, max is one year
				if(expire.u.i > 0 && expire.u.i <= 365)
					config.maxDBdays = expire.u.i;
				else
					log_warn("Invalid setting for database.network.expire, using default");
			}
			else
				log_debug(DEBUG_CONFIG, "database.network.expire DOES NOT EXIST");
		}
		else
			log_debug(DEBUG_CONFIG, "database.network DOES NOT EXIST");
	}
	else
		log_debug(DEBUG_CONFIG, "database DOES NOT EXIST");

	// Read [http] section
	toml_table_t *http = toml_table_in(conf, "http");
	if(http)
	{
		toml_datum_t localAPIauth = toml_bool_in(http, "localAPIauth");
		if(localAPIauth.ok)
			config.http.localAPIauth = localAPIauth.u.b;
		else
			log_debug(DEBUG_CONFIG, "http.localAPIauth DOES NOT EXIST");

		toml_datum_t prettyJSON = toml_bool_in(http, "prettyJSON");
		if(prettyJSON.ok)
			config.http.prettyJSON = prettyJSON.u.b;
		else
			log_debug(DEBUG_CONFIG, "http.prettyJSON DOES NOT EXIST");

		toml_datum_t sessionTimeout = toml_int_in(http, "sessionTimeout");
		if(sessionTimeout.ok)
		{
			if(sessionTimeout.u.i >= 0)
				config.http.sessionTimeout = sessionTimeout.u.i;
			else
				log_warn("Invalid setting for http.sessionTimeout, using default");
		}
		else
			log_debug(DEBUG_CONFIG, "http.sessionTimeout DOES NOT EXIST");

		toml_datum_t domain = toml_string_in(http, "domain");
		if(domain.ok && strlen(domain.u.s) > 0)
			config.http.domain = domain.u.s;
		else
			log_debug(DEBUG_CONFIG, "http.domain DOES NOT EXIST or EMPTY");

		toml_datum_t acl = toml_string_in(http, "acl");
		if(acl.ok && strlen(acl.u.s) > 0)
			config.http.acl = acl.u.s;
		else
			log_debug(DEBUG_CONFIG, "http.acl DOES NOT EXIST or EMPTY");

		toml_datum_t port = toml_string_in(http, "port");
		if(port.ok && strlen(port.u.s) > 0)
		{
			config.http.port = port.u.s;
		}
		else
			log_debug(DEBUG_CONFIG, "http.port DOES NOT EXIST or EMPTY");

		// Read [http.paths] section
		toml_table_t *paths = toml_table_in(http, "paths");
		if(paths)
		{
			toml_datum_t webroot = toml_string_in(paths, "webroot");
			if(webroot.ok && strlen(webroot.u.s) > 0)
				config.http.paths.webroot = webroot.u.s;
			else
				log_debug(DEBUG_CONFIG, "http.paths.webroot DOES NOT EXIST or EMPTY");

			toml_datum_t webhome = toml_string_in(paths, "webhome");
			if(webhome.ok && strlen(webhome.u.s) > 0)
				config.http.paths.webhome = webhome.u.s;
			else
				log_debug(DEBUG_CONFIG, "http.paths.webhome DOES NOT EXIST or EMPTY");
		}
		else
			log_debug(DEBUG_CONFIG, "http.paths DOES NOT EXIST");
	}
	else
		log_debug(DEBUG_CONFIG, "http DOES NOT EXIST");

	// Read [files] section
	toml_table_t *files = toml_table_in(conf, "files");
	if(files)
	{
		// log file path is read earlier

		toml_datum_t pid = toml_string_in(files, "pid");
		if(pid.ok && strlen(pid.u.s) > 0)
			config.files.pid = pid.u.s;
		else
			log_debug(DEBUG_CONFIG, "files.pid DOES NOT EXIST or EMPTY");

		toml_datum_t fdatabase = toml_string_in(files, "database");
		if(fdatabase.ok && strlen(fdatabase.u.s) > 0)
			config.files.database = fdatabase.u.s;
		else
			log_debug(DEBUG_CONFIG, "files.database DOES NOT EXIST or EMPTY");

		toml_datum_t gravity = toml_string_in(files, "gravity");
		if(gravity.ok && strlen(gravity.u.s) > 0)
			config.files.gravity = gravity.u.s;
		else
			log_debug(DEBUG_CONFIG, "files.gravity DOES NOT EXIST or EMPTY");

		toml_datum_t macvendor = toml_string_in(files, "macvendor");
		if(macvendor.ok && strlen(macvendor.u.s) > 0)
			config.files.macvendor = macvendor.u.s;
		else
			log_debug(DEBUG_CONFIG, "files.macvendor DOES NOT EXIST or EMPTY");

		toml_datum_t setupVars = toml_string_in(files, "setupVars");
		if(setupVars.ok && strlen(setupVars.u.s) > 0)
			config.files.setupVars = setupVars.u.s;
		else
			log_debug(DEBUG_CONFIG, "files.setupVars DOES NOT EXIST or EMPTY");

		toml_datum_t http_info = toml_string_in(files, "HTTPinfo");
		if(http_info.ok && strlen(http_info.u.s) > 0)
			config.files.http_info = http_info.u.s;
		else
			log_debug(DEBUG_CONFIG, "files.HTTPinfo DOES NOT EXIST or EMPTY");

		toml_datum_t ph7_error = toml_string_in(files, "PH7error");
		if(ph7_error.ok && strlen(ph7_error.u.s) > 0)
			config.files.ph7_error = ph7_error.u.s;
		else
			log_debug(DEBUG_CONFIG, "files.PH7error DOES NOT EXIST or EMPTY");
	}
	else
		log_debug(DEBUG_CONFIG, "files DOES NOT EXIST");

	// Read [misc] section
	toml_table_t *misc = toml_table_in(conf, "misc");
	if(misc)
	{
		// Load privacy level
		getPrivacyLevel();

		toml_datum_t nicey = toml_int_in(misc, "nice");
		if(nicey.ok)
		{
			// -999 = disabled
			const int priority = nicey.u.i;
			const int which = PRIO_PROCESS;
			const id_t pid = getpid();
			config.nice = getpriority(which, pid);

			if(priority == -999 || config.nice == priority)
			{
				// Do not set nice value
				log_debug(DEBUG_CONFIG, "Not changing process priority.");
				log_debug(DEBUG_CONFIG, "  Asked for %d, is %d", priority, config.nice);
			}
			else
			{
				const int ret = setpriority(which, pid, priority);
				if(ret == -1)
					// ERROR EPERM: The calling process attempted to increase its priority
					// by supplying a negative value but has insufficient privileges.
					// On Linux, the RLIMIT_NICE resource limit can be used to define a limit to
					// which an unprivileged process's nice value can be raised. We are not
					// affected by this limit when pihole-FTL is running with CAP_SYS_NICE
					log_warn("Cannot set process priority to %d: %s",
					         priority, strerror(errno));

				config.nice = getpriority(which, pid);
			}

			if(config.nice != priority)
				log_info("Set process niceness to %d (instead of %d)",
				         config.nice, priority);
		}
		else
			log_debug(DEBUG_CONFIG, "misc.nice DOES NOT EXIST");

		toml_datum_t delay_startup = toml_int_in(misc, "delayStartup");
		if(delay_startup.ok)
		{
			// Maximum is 300 seconds
			if(delay_startup.u.i >= 0 && delay_startup.u.i <= 300)
				config.delay_startup = delay_startup.u.i;
			else
				log_warn("Invalid setting for misc.delayStartup, using default");
		}
		else
			log_debug(DEBUG_CONFIG, "misc.delayStartup DOES NOT EXIST");

		toml_datum_t addr2line = toml_bool_in(misc, "addr2line");
		if(addr2line.ok)
			config.addr2line = addr2line.u.b;
		else
			log_debug(DEBUG_CONFIG, "misc.addr2line DOES NOT EXIST");

		// Read [misc.check] section
		toml_table_t *check = toml_table_in(misc, "check");
		if(check)
		{
			toml_datum_t load = toml_bool_in(check, "load");
			if(load.ok)
				config.check.load = load.u.b;
			else
				log_debug(DEBUG_CONFIG, "misc.check.load DOES NOT EXIST");

			toml_datum_t disk = toml_int_in(check, "disk");
			if(disk.ok && disk.u.i >= 0 && disk.u.i <= 100)
				config.check.disk = disk.u.i;
			else
				log_debug(DEBUG_CONFIG, "misc.check.disk DOES NOT EXIST or is INVALID");

			toml_datum_t shmem = toml_int_in(check, "shmem");
			if(shmem.ok && shmem.u.i >= 0 && shmem.u.i <= 100)
				config.check.shmem = shmem.u.i;
			else
				log_debug(DEBUG_CONFIG, "misc.check.shmem DOES NOT EXIST or is INVALID");
		}
		else
			log_debug(DEBUG_CONFIG, "misc.check DOES NOT EXIST");
	}
	else
		log_debug(DEBUG_CONFIG, "misc DOES NOT EXIST");

	if(config.debug)
	{
		// Enable debug logging in dnsmasq (only effective before starting the resolver)
		argv_dnsmasq[2] = "--log-debug";
	}

	toml_free(conf);
	return true;
}

static toml_table_t *parseTOML(void)
{
	// Try to open default config file. Use fallback if not found
	FILE *fp;
	if((fp = openFTLtoml("r")) == NULL)
	{
		log_debug(DEBUG_CONFIG, "No config file available (%s), using defaults",
		          strerror(errno));
		return NULL;
	}

	// Parse lines in the config file
	char errbuf[200];
	toml_table_t *conf = toml_parse_file(fp, errbuf, sizeof(errbuf));
	fclose(fp);

	if(conf == NULL)
	{
		log_err("Cannot parse config file: %s", errbuf);
		return NULL;
	}

	log_debug(DEBUG_CONFIG, "TOML file parsing: OK");

	return conf;
}

bool getPrivacyLevel(void)
{
	log_debug(DEBUG_CONFIG, "Reading TOML config file: privacy level");

	toml_table_t *conf = parseTOML();
	if(!conf)
		return false;

	toml_table_t *misc = toml_table_in(conf, "misc");
	if(!misc)
	{
		log_debug(DEBUG_CONFIG, "misc does not exist");
		toml_free(conf);
		return false;
	}

	toml_datum_t privacylevel = toml_int_in(misc, "privacyLevel");
	if(!privacylevel.ok)
	{
		log_debug(DEBUG_CONFIG, "misc.privacyLevel does not exist");
		toml_free(conf);
		return false;
	}

	if(privacylevel.u.i >= PRIVACY_SHOW_ALL && privacylevel.u.i <= PRIVACY_MAXIMUM)
		config.privacylevel = privacylevel.u.i;
	else
		log_warn("Invalid setting for misc.privacyLevel");

	toml_free(conf);
	return true;
}

bool getBlockingMode(void)
{
	log_debug(DEBUG_CONFIG, "Reading TOML config file: DNS blocking mode");

	toml_table_t *conf = parseTOML();
	if(!conf)
		return false;

	toml_table_t *dns = toml_table_in(conf, "dns");
	if(!dns)
	{
		log_debug(DEBUG_CONFIG, "dns does not exist");
		toml_free(conf);
		return false;
	}

	toml_datum_t blockingmode = toml_string_in(dns, "blockingmode");
	if(!blockingmode.ok)
	{
		log_debug(DEBUG_CONFIG, "dns.blockingmode DOES NOT EXIST");
		toml_free(conf);
		return false;
	}

	// Iterate over possible blocking modes and check if it applies
	bool found = false;
	for(enum blocking_mode bm = MODE_IP; bm < MODE_MAX; bm++)
	{
		const char *bmstr = get_blocking_mode_str(bm);
		if(strcasecmp(bmstr, blockingmode.u.s) == 0)
		{
			config.blockingmode = bm;
			found = true;
			break;
		}
	}
	if(!found)
		log_warn("Unknown blocking mode \"%s\"", blockingmode.u.s);
	free(blockingmode.u.s);

	toml_free(conf);
	return true;
}

bool readDebugSettings(void)
{
	log_debug(DEBUG_CONFIG, "Reading TOML config file: debug settings");

	toml_table_t *conf = parseTOML();
	if(!conf)
		return false;

	// Read [debug] section
	toml_table_t *debug = toml_table_in(conf, "debug");
	if(!debug)
	{
		log_debug(DEBUG_CONFIG, "debug DOES NOT EXIST");
		toml_free(conf);
		return false;
	}

	toml_datum_t all = toml_bool_in(debug, "all");
	if(all.ok && all.u.b)
		config.debug = ~(enum debug_flag)0;
	else if(!all.ok)
		log_debug(DEBUG_CONFIG, "debug.all DOES NOT EXIST");
	else
	{
		// debug.all is false
		char buffer[64];
		for(enum debug_flag flag = DEBUG_DATABASE; flag < DEBUG_EXTRA; flag <<= 1)
		{
			const char *name, *desc;
			debugstr(flag, &name, &desc);
			memset(buffer, 0, sizeof(buffer));
			strcpy(buffer, name+6); // offset "debug_"
			strtolower(buffer);

			toml_datum_t flagstr = toml_bool_in(debug, buffer);

			// Only set debug flags that are specified
			if(!flagstr.ok)
			{
				log_debug(DEBUG_CONFIG, "debug.%s DOES NOT EXIST", buffer);
				continue;
			}

			if(flagstr.u.b)
				config.debug |= flag;  // SET bit
			else
				config.debug &= ~flag; // CLR bit
		}
	}

	reportDebugConfig();

	toml_free(conf);
	return true;
}

bool getLogFilePathTOML(void)
{
	log_debug(DEBUG_CONFIG, "Reading TOML config file: log file path");

	toml_table_t *conf = parseTOML();
	if(!conf)
		return false;

	toml_table_t *files = toml_table_in(conf, "files");
	if(!files)
	{
		log_debug(DEBUG_CONFIG, "files does not exist");
		toml_free(conf);
		return false;
	}

	toml_datum_t log = toml_string_in(files, "log");
	if(!log.ok)
	{
		log_debug(DEBUG_CONFIG, "files.log DOES NOT EXIST");
		toml_free(conf);
		return false;
	}

	// Only replace string when it is different
	if(strcmp(config.files.log,log.u.s) != 0)
		config.files.log = log.u.s; // Allocated string
	else
		free(log.u.s);

	toml_free(conf);
	return true;
}

static void reportDebugConfig(void)
{
	if(!config.debug)
		return;

	log_debug(DEBUG_ANY, "***********************");
	log_debug(DEBUG_ANY, "*    DEBUG SETTINGS   *");
	for(enum debug_flag flag = DEBUG_DATABASE; flag < DEBUG_EXTRA; flag <<= 1)
	{
		const char *name, *desc;
		debugstr(flag, &name, &desc);
		unsigned int spaces = 20 - strlen(name);
		log_debug(DEBUG_ANY, "* %s:%*s %s", name+6, spaces, "", config.debug & flag ? "YES *" : "NO  *");
	}
	log_debug(DEBUG_ANY, "***********************");
}
