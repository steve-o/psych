/* User-configurable settings.
 *
 * NB: all strings are locale bound, RFA provides no Unicode support.
 */

#ifndef __CONFIG_HH__
#define __CONFIG_HH__
#pragma once

#include <string>
#include <vector>

/* Velocity Analytics Plugin Framework */
#include <vpf/vpf.h>

namespace chromium
{
	class DictionaryValue;
}

namespace psych
{
	struct session_config_t
	{
//  RFA session name, one session contains a horizontal scaling set of connections.
		std::string session_name;

//  RFA connection name, used for logging.
		std::string connection_name;

//  RFA publisher name, used for logging.
		std::string publisher_name;

//  TREP-RT ADH hostname or IP address.
		std::vector<std::string> rssl_servers;

//  Default TREP-RT RSSL port, e.g. 14002 (interactive), 14003 (non-interactive).
		std::string rssl_default_port;

/* DACS application Id.  If the server authenticates with DACS, the consumer
 * application may be required to pass in a valid ApplicationId.
 * Range: "" (None) or 1-511 as an Ascii string.
 */
		std::string application_id;

/* InstanceId is used to differentiate applications running on the same host.
 * If there is more than one noninteractive provider instance running on the
 * same host, they must be set as a different value by the provider
 * application. Otherwise, the infrastructure component which the providers
 * connect to will reject a login request that has the same InstanceId value
 * and cut the connection.
 * Range: "" (None) or any Ascii string, presumably to maximum RFA_String length.
 */
		std::string instance_id;

/* DACS username, frequently non-checked and set to similar: user1.
 */
		std::string user_name;

/* DACS position, the station which the user is using.
 * Range: "" (None) or "<IPv4 address>/hostname" or "<IPv4 address>/net"
 */
		std::string position;
	};

	struct resource_t
	{
		resource_t (const std::string& name_,
			const std::string& source_,
			const std::string& path_, 
			unsigned long entitlement_code_, 
			const std::map<std::string, int>& fields_, 
			const std::map<std::string, std::pair<std::string, std::string>>& items_) :
			name (name_),
			source (source_),
			path (path_),
			entitlement_code (entitlement_code_),
			fields (fields_),
			items (items_)
		{
		}

/* for logging */
		std::string name;

/* source feed name, i.e. news or social media */
		std::string source;

/* latest minute feed */
		std::string path;

/* DACS numeric entitlement code (PE) */
		unsigned long entitlement_code;

/* column name to FID mapping */
		std::map<std::string, int> fields;

/* sector to (RIC, topic) mapping */
		std::map<std::string, std::pair<std::string, std::string>> items;
	};
	
	struct resource_compare_t {
		bool operator() (const resource_t& lhs, const resource_t& rhs) const {
			return lhs.path < rhs.path;
		}
	};

	struct config_t
	{
		config_t();

#ifndef CONFIG_PSYCH_AS_APPLICATION
//  From Xml tree
		bool parseDomElement (const xercesc::DOMElement* elem);
		bool parseConfigNode (const xercesc::DOMNode* node);
		bool parseSnmpNode (const xercesc::DOMNode* node);
		bool parseAgentXNode (const xercesc::DOMNode* node);
		bool parseRfaNode (const xercesc::DOMNode* node);
		bool parseServiceNode (const xercesc::DOMNode* node);
		bool parseDacsNode (const xercesc::DOMNode* node);
		bool parseConnectionNode (const xercesc::DOMNode* node, session_config_t& session);
		bool parseServerNode (const xercesc::DOMNode* node, std::string& server);
		bool parsePublisherNode (const xercesc::DOMNode* node, std::string& publisher);
		bool parseLoginNode (const xercesc::DOMNode* node, session_config_t& session);
		bool parseSessionNode (const xercesc::DOMNode* node);
		bool parseMonitorNode (const xercesc::DOMNode* node);
		bool parseEventQueueNode (const xercesc::DOMNode* node);
		bool parseVendorNode (const xercesc::DOMNode* node);
		bool parsePsychNode (const xercesc::DOMNode* node);
		bool parseResourceNode (const xercesc::DOMNode* node);
		bool parseLinkNode (const xercesc::DOMNode* node, std::string* source, std::string* rel, unsigned long* id, std::string* href);
		bool parseFieldNode (const xercesc::DOMNode* node, std::string* name, int* id);
		bool parseItemNode (const xercesc::DOMNode* node, std::string* name, std::string* topic, std::string* src);
#endif

//   From Json tree
		bool parseConfig (const chromium::DictionaryValue* dict_val);
		bool parseSession (const chromium::DictionaryValue* dict_val);
		bool parseResource (const chromium::DictionaryValue* dict_val);

		bool validate();

//  SNMP implant.
		bool is_snmp_enabled;

//  Net-SNMP agent or sub-agent.
		bool is_agentx_subagent;

//  Net-SNMP file log target.
		std::string snmp_filelog;

//  AgentX port number to connect to master agent.
		std::string agentx_socket;

//  Windows registry key path.
		std::string key;

//  TREP-RT service name, e.g. IDN_RDF.
		std::string service_name;

//  DACS service id, e.g. 1234.
		std::string dacs_id;

//  RFA sessions comprising of session names, connection names,
//  RSSL hostname or IP address and default RSSL port, e.g. 14002, 14003.
		std::vector<session_config_t> sessions;

//  RFA application logger monitor name.
		std::string monitor_name;

//  RFA event queue name.
		std::string event_queue_name;

//  RFA vendor name.
		std::string vendor_name;

//  HTTP poll and publish interval in seconds.
		std::string interval;

//  Windows timer coalescing tolerable delay.
//  At least 32ms, corresponding to two 15.6ms platform timer interrupts.
//  Appropriate values are 10% to timer period.
//  Specify tolerable delay values and timer periods in multiples of 50 ms.
//  http://www.microsoft.com/whdc/system/pnppwr/powermgmt/TimerCoal.mspx
		std::string tolerable_delay;

//  Number of times to retry given a transient error: timeout or HTTP 5xx response.
		std::string retry_count;

//  Time period to wait before a retry attempt, in milliseconds.
		std::string retry_delay_ms;

//  Maximum time to retry transfer, in seconds.
		std::string retry_timeout_ms;

//  Maximum time for entire operation, in milliseconds.
		std::string timeout_ms;

//  Maximum time for connection phase, in milliseconds.
		std::string connect_timeout_ms;

//  HTTP pipelining disabled by default as frequently broken.
		std::string enable_http_pipelining;

//  Responses will be rejected above this size.
		std::string maximum_response_size;

//  Responses will be rejected below this size.
		std::string minimum_response_size;

//  HTTP encoding format to request:  "identity", "deflate", "gzip", etc.
		std::string request_http_encoding;

//  Time offset calibration constant to correct a systematic error or bias.
		std::string time_offset_constant;

//  HTTP provided file modification time clock offset sanity check, 0 disables.
		std::string panic_threshold;

//  Optional HTTP proxy for Internet access, beware most proxies do not correctly function with HTTP pipelining.
		std::string http_proxy;

//  DNS response cache time in seconds.
		std::string dns_cache_timeout;

//  Base href for all links.
		std::string base_url;

//  "Resources": equities, currencies, commodities, etc.
		std::vector<resource_t> resources;
	};

	inline
	std::ostream& operator<< (std::ostream& o, const session_config_t& session) {
		o << "{ "
			  "\"session_name\": \"" << session.session_name << "\""
			", \"connection_name\": \"" << session.connection_name << "\""
			", \"publisher_name\": \"" << session.publisher_name << "\""
			", \"rssl_servers\": [ ";
		for (auto it = session.rssl_servers.begin();
			it != session.rssl_servers.end();
			++it)
		{
			if (it != session.rssl_servers.begin())
				o << ", ";
			o << '"' << *it << '"';
		}
		o << " ]"
			", \"rssl_default_port\": \"" << session.rssl_default_port << "\""
			", \"application_id\": \"" << session.application_id << "\""
			", \"instance_id\": \"" << session.instance_id << "\""
			", \"user_name\": \"" << session.user_name << "\""
			", \"position\": \"" << session.position << "\""
			" }";
		return o;
	}

	inline
	std::ostream& operator<< (std::ostream& o, const resource_t& resource) {
		o << "{ "
			  "\"name\": \"" << resource.name << "\""
			", \"source\": \"" << resource.source << "\""
			", \"path\": \"" << resource.path << "\""
			", \"entitlement_code\": " << resource.entitlement_code << ""
			", \"fields\": { ";
		for (auto it = resource.fields.begin();
			it != resource.fields.end();
			++it)
		{
			if (it != resource.fields.begin())
				o << ", ";
			o << '"' << it->first << "\": " << it->second;
		}
		o << " }"
			", \"items\": { ";
		for (auto it = resource.items.begin();
			it != resource.items.end();
			++it)
		{
			if (it != resource.items.begin())
				o << ", ";
			o << '"' << it->first << "\": { "
				  "\"RIC\": \"" << it->second.first << "\""
				", \"topic\": \"" << it->second.second << "\""
				" }";
		}
		o << " }"
			" }";
		return o;
	}

	inline
	std::ostream& operator<< (std::ostream& o, const config_t& config) {
		o << "\"config_t\": { "
			  "\"is_snmp_enabled\": " << (0 == config.is_snmp_enabled ? "false" : "true") << ""
			", \"is_agentx_subagent\": " << (0 == config.is_agentx_subagent ? "false" : "true") << ""
			", \"agentx_socket\": \"" << config.agentx_socket << "\""
			", \"key\": \"" << config.key << "\""
			", \"service_name\": \"" << config.service_name << "\""
			", \"dacs_id\": \"" << config.dacs_id << "\""
			", \"sessions\": [";
		for (auto it = config.sessions.begin();
			it != config.sessions.end();
			++it)
		{
			if (it != config.sessions.begin())
				o << ", ";
			o << *it;
		}
		o << " ]"
			", \"monitor_name\": \"" << config.monitor_name << "\""
			", \"event_queue_name\": \"" << config.event_queue_name << "\""
			", \"vendor_name\": \"" << config.vendor_name << "\""
			", \"interval\": \"" << config.interval << "\""
			", \"tolerable_delay\": \"" << config.tolerable_delay << "\""
			", \"retry_count\": \"" << config.retry_count << "\""
			", \"retry_delay_ms\": \"" << config.retry_delay_ms << "\""
			", \"retry_timeout_ms\": \"" << config.retry_timeout_ms << "\""
			", \"timeout_ms\": \"" << config.timeout_ms << "\""
			", \"connect_timeout_ms\": \"" << config.connect_timeout_ms << "\""
			", \"enable_http_pipelining\": \"" << config.enable_http_pipelining << "\""
			", \"maximum_response_size\": \"" << config.maximum_response_size << "\""
			", \"minimum_response_size\": \"" << config.minimum_response_size << "\""
			", \"request_http_encoding\": \"" << config.request_http_encoding << "\""
			", \"time_offset_constant\": \"" << config.time_offset_constant << "\""
			", \"panic_threshold\": \"" << config.panic_threshold << "\""
			", \"http_proxy\": \"" << config.http_proxy << "\""
			", \"dns_cache_timeout\": \"" << config.dns_cache_timeout << "\""
			", \"base_url\": \"" << config.base_url << "\""
			", \"resources\": [";
		for (auto it = config.resources.begin();
			it != config.resources.end();
			++it)
		{
			if (it != config.resources.begin())
				o << ", ";
			o << *it;
		}
		o << " ]"
			" }";
		return o;
	}

} /* namespace psych */

#endif /* __CONFIG_HH__ */

/* eof */