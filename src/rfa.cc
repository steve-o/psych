/* RFA context.
 */

#include "rfa.hh"

#include <cassert>

#include "chromium/logging.hh"
#include "deleter.hh"

using rfa::common::RFA_String;

static const RFA_String kContextName ("RFA");
static const RFA_String kConnectionType ("RSSL_NIPROV");

/* Translate forward slashes into backward slashes for broken Rfa library.
 */
static
void
fix_rfa_string_path (
	RFA_String&	rfa_str
	)
{
#ifndef RFA_HIVE_ABBREVIATION_FIXED
/* RFA string API is hopeless, use std library. */
	std::string str (rfa_str.c_str());
	if (0 == str.compare (0, 2, "HK")) {
		if (0 == str.compare (2, 2, "LM"))
			str.replace (2, 2, "EY_LOCAL_MACHINE");
		else if (0 == strncmp (str.c_str(), "HKCC", 4))
			str.replace (2, 2, "EY_CURRENT_CONFIG");
		else if (0 == strncmp (str.c_str(), "HKCR", 4))
			str.replace (2, 2, "EY_CLASSES_ROOT");
		else if (0 == strncmp (str.c_str(), "HKCU", 4))
			str.replace (2, 2, "EY_CURRENT_USER");
		else if (0 == strncmp (str.c_str(), "HKU", 3))
			str.replace (2, 2, "EY_USERS");
		rfa_str.set (str.c_str());
	}
#endif
#ifndef RFA_FORWARD_SLASH_IN_PATH_FIXED
	size_t pos = 0;
	while (-1 != (pos = rfa_str.find ("/", (unsigned)pos)))
		rfa_str.replace ((unsigned)pos++, 1, "\\");
#endif
}

psych::rfa_t::rfa_t (const config_t& config) :
	config_ (config)
{
}

psych::rfa_t::~rfa_t()
{
	VLOG(2) << "Closing RFA.";
	rfa_config_.reset();
	rfa::common::Context::uninitialize();
}

bool
psych::rfa_t::init()
{
	VLOG(2) << "Initializing RFA.";
	rfa::common::Context::initialize();

/* 8.2.3 Populate Config Database.
 */
	VLOG(3) << "Populating RFA config database.";
	std::unique_ptr<rfa::config::StagingConfigDatabase, internal::destroy_deleter> staging (rfa::config::StagingConfigDatabase::create());
	if (!(bool)staging)
		return false;

/* Disable Windows Event Logger. */
	RFA_String name, value;

	name.set ("/Logger/AppLogger/windowsLoggerEnabled");
	fix_rfa_string_path (name);
	staging->setBool (name, false);

/* Session list */
	for (auto it = config_.sessions.begin();
		it != config_.sessions.end();
		++it)
	{
		const RFA_String sessionName (it->session_name.c_str(), 0, false),
			connectionName (it->connection_name.c_str(), 0, false);

		name = "/Sessions/" + sessionName + "/connectionList";
		fix_rfa_string_path (name);
		staging->setString (name, connectionName);
/* Connection list */
		name = "/Connections/" + connectionName + "/connectionType";
		fix_rfa_string_path (name);
		staging->setString (name, kConnectionType);
/* List of RSSL servers */
		name = "/Connections/" + connectionName + "/serverList";
		fix_rfa_string_path (name);
		std::ostringstream ss;
		for (auto jt = it->rssl_servers.begin();
			jt != it->rssl_servers.end();
			++jt)
		{
			if (jt != it->rssl_servers.begin())
				ss << ", ";
			ss << *jt;
		}		
		value.set (ss.str().c_str());
		staging->setString (name, value);
/* Default RSSL port */
		if (!it->rssl_default_port.empty()) {
			name = "/Connections/" + connectionName + "/rsslPort";
			fix_rfa_string_path (name);
			value.set (it->rssl_default_port.c_str());
			staging->setString (name, value);
		}
	}

	rfa_config_.reset (rfa::config::ConfigDatabase::acquire (kContextName));
	if (!(bool)rfa_config_)
		return false;

	VLOG(3) << "Merging RFA config database with staging database.";
	if (!rfa_config_->merge (*staging.get()))
		return false;

/* Windows Registry override. */
	if (!config_.key.empty()) {
		VLOG(3) << "Populating staging database with Windows Registry.";
		staging.reset (rfa::config::StagingConfigDatabase::create());
		if (!(bool)staging)
			return false;
		name = config_.key.c_str();
		fix_rfa_string_path (name);
		staging->load (rfa::config::windowsRegistry, name);
		VLOG(3) << "Merging RFA config database with Windows Registry staging database.";
		if (!rfa_config_->merge (*staging.get()))
			return false;
	}

	VLOG(3) << "RFA initialization complete.";
	return true;
}

/* eof */
