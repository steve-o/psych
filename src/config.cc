/* User-configurable settings.
 */

#include "config.hh"

#include "chromium/logging.hh"

psych::config_t::config_t() :
/* default values */
	is_snmp_enabled (false),
	is_agentx_subagent (true)
{
/* C++11 initializer lists not supported in MSVC2010 */
}

/* Minimal error handling parsing of an Xml node pulled from the
 * Analytics Engine.
 *
 * Returns true if configuration is valid, returns false on invalid content.
 */

using namespace xercesc;

/** L"" prefix is used in preference to u"" because of MSVC2010 **/

bool
psych::config_t::validate()
{
	if (service_name.empty()) {
		LOG(ERROR) << "Undefined service name.";
		return false;
	}
	if (sessions.empty()) {
		LOG(ERROR) << "Undefined session, expecting one or more session node.";
		return false;
	}
	for (auto it = sessions.begin();
		it != sessions.end();
		++it)
	{
		if (it->session_name.empty()) {
			LOG(ERROR) << "Undefined session name.";
			return false;
		}
		if (it->connection_name.empty()) {
			LOG(ERROR) << "Undefined connection name for <session name=\"" << it->session_name << "\">.";
			return false;
		}
		if (it->publisher_name.empty()) {
			LOG(ERROR) << "Undefined publisher name for <session name=\"" << it->session_name << "\">.";
			return false;
		}
		if (it->rssl_servers.empty()) {
			LOG(ERROR) << "Undefined server list for <connection name=\"" << it->connection_name << "\">.";
			return false;
		}
		if (it->application_id.empty()) {
			LOG(ERROR) << "Undefined application ID for <session name=\"" << it->session_name << "\">.";
			return false;
		}
		if (it->instance_id.empty()) {
			LOG(ERROR) << "Undefined instance ID for <session name=\"" << it->session_name << "\">.";
			return false;
		}
		if (it->user_name.empty()) {
			LOG(ERROR) << "Undefined user name for <session name=\"" << it->session_name << "\">.";
			return false;
		}
	}
	if (monitor_name.empty()) {
		LOG(ERROR) << "Undefined monitor name.";
		return false;
	}
	if (event_queue_name.empty()) {
		LOG(ERROR) << "Undefined event queue name.";
		return false;
	}
	if (vendor_name.empty()) {
		LOG(ERROR) << "Undefined vendor name.";
		return false;
	}

/* Maximum response size must be provided for buffer allocation. */
	if (maximum_response_size.empty()) {
		LOG(ERROR) << "Undefined maximum response size.";
		return false;
	}
	long value = std::atol (maximum_response_size.c_str());
	if (value <= 0) {
		LOG(ERROR) << "Invalid maximum response size \"" << maximum_response_size << "\".";
		return false;
	}

/* "resources" */
	for (auto it = resources.begin(); it != resources.end(); ++it) {
		if (it->name.empty()) {
			LOG(ERROR) << "Undefined resource name.";
			return false;
		}
		if (it->path.empty()) {
			LOG(ERROR) << "Undefined " << it->name << " feed path.";
			return false;
		}
		if (it->fields.empty()) {
			LOG(ERROR) << "Undefined " << it->name << " column FID mapping.";
			return false;
		}
		if (it->items.empty()) {
			LOG(ERROR) << "Undefined " << it->name << " sector: RIC and topic mapping.";
			return false;
		}
	}
	return true;
}

bool
psych::config_t::parseDomElement (
	const DOMElement*	root
	)
{
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

	LOG(INFO) << "Parsing configuration ...";
/* Plugin configuration wrapped within a <config> node. */
	nodeList = root->getElementsByTagName (L"config");

	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseConfigNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <config> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <config> nodes found in configuration.";

	if (!validate()) {
		LOG(ERROR) << "Failed validation, malformed configuration file requires correction.";
		return false;
	}

	LOG(INFO) << "Parsing complete.";
	return true;
}

bool
psych::config_t::parseConfigNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* <Snmp> */
	nodeList = elem->getElementsByTagName (L"Snmp");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseSnmpNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <Snmp> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
/* <Rfa> */
	nodeList = elem->getElementsByTagName (L"Rfa");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseRfaNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <Rfa> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <Rfa> nodes found in configuration.";
/* <psych> */
	nodeList = elem->getElementsByTagName (L"psych");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parsePsychNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <psych> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <crosses> nodes found in configuration.";
	return true;
}

/* <Snmp> */
bool
psych::config_t::parseSnmpNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	const DOMNodeList* nodeList;
	vpf::XMLStringPool xml;
	std::string attr;

/* logfile="file path" */
	attr = xml.transcode (elem->getAttribute (L"filelog"));
	if (!attr.empty())
		snmp_filelog = attr;

/* <agentX> */
	nodeList = elem->getElementsByTagName (L"agentX");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseAgentXNode (nodeList->item (i))) {
			vpf::XMLStringPool xml;
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <agentX> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	this->is_snmp_enabled = true;
	return true;
}

bool
psych::config_t::parseAgentXNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* subagent="bool" */
	attr = xml.transcode (elem->getAttribute (L"subagent"));
	if (!attr.empty())
		is_agentx_subagent = (0 == attr.compare ("true"));

/* socket="..." */
	attr = xml.transcode (elem->getAttribute (L"socket"));
	if (!attr.empty())
		agentx_socket = attr;
	return true;
}

/* </Snmp> */

/* <Rfa> */
bool
psych::config_t::parseRfaNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;
	std::string attr;

/* key="name" */
	attr = xml.transcode (elem->getAttribute (L"key"));
	if (!attr.empty())
		key = attr;

/* <service> */
	nodeList = elem->getElementsByTagName (L"service");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseServiceNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <service> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <service> nodes found in configuration.";
/* <DACS> */
	nodeList = elem->getElementsByTagName (L"DACS");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseDacsNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <DACS> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <DACS> nodes found in configuration.";
/* <session> */
	nodeList = elem->getElementsByTagName (L"session");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseSessionNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <session> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <session> nodes found, RFA behaviour is undefined without a server list.";
/* <monitor> */
	nodeList = elem->getElementsByTagName (L"monitor");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseMonitorNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <monitor> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
/* <eventQueue> */
	nodeList = elem->getElementsByTagName (L"eventQueue");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseEventQueueNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <eventQueue> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
/* <vendor> */
	nodeList = elem->getElementsByTagName (L"vendor");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseVendorNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <vendor> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	return true;
}

bool
psych::config_t::parseServiceNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* name="name" */
	attr = xml.transcode (elem->getAttribute (L"name"));
	if (attr.empty()) {
/* service name cannot be empty */
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}
	service_name = attr;
	return true;
}

bool
psych::config_t::parseDacsNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* id="numeric value" */
	attr = xml.transcode (elem->getAttribute (L"id"));
	if (!attr.empty())
		dacs_id = attr;
	LOG_IF(WARNING, dacs_id.empty()) << "Undefined DACS service ID.";
	return true;
}
bool
psych::config_t::parseSessionNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;
	session_config_t session;

/* name="name" */
	session.session_name = xml.transcode (elem->getAttribute (L"name"));
	if (session.session_name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}

/* <publisher> */
	nodeList = elem->getElementsByTagName (L"publisher");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parsePublisherNode (nodeList->item (i), session.publisher_name)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <publisher> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
/* <connection> */
	nodeList = elem->getElementsByTagName (L"connection");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseConnectionNode (nodeList->item (i), session)) {
			LOG(ERROR) << "Failed parsing <connection> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <connection> nodes found, RFA behaviour is undefined without a server list.";
/* <login> */
	nodeList = elem->getElementsByTagName (L"login");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseLoginNode (nodeList->item (i), session)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <login> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <login> nodes found in configuration.";	
		
	sessions.push_back (session);
	return true;
}

bool
psych::config_t::parseConnectionNode (
	const DOMNode*		node,
	session_config_t&	session
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* name="name" */
	session.connection_name = xml.transcode (elem->getAttribute (L"name"));
	if (session.connection_name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}
/* defaultPort="port" */
	session.rssl_default_port = xml.transcode (elem->getAttribute (L"defaultPort"));

/* <server> */
	nodeList = elem->getElementsByTagName (L"server");
	for (int i = 0; i < nodeList->getLength(); i++) {
		std::string server;
		if (!parseServerNode (nodeList->item (i), server)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <server> nth-node #" << (1 + i) << ": \"" << text_content << "\".";			
			return false;
		}
		session.rssl_servers.push_back (server);
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <server> nodes found, RFA behaviour is undefined without a server list.";

	return true;
}

bool
psych::config_t::parseServerNode (
	const DOMNode*		node,
	std::string&		server
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	server = xml.transcode (elem->getTextContent());
	if (server.size() == 0) {
		LOG(ERROR) << "Undefined hostname or IPv4 address.";
		return false;
	}
	return true;
}

bool
psych::config_t::parseLoginNode (
	const DOMNode*		node,
	session_config_t&	session
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

/* applicationId="id" */
	session.application_id = xml.transcode (elem->getAttribute (L"applicationId"));
/* instanceId="id" */
	session.instance_id = xml.transcode (elem->getAttribute (L"instanceId"));
/* userName="name" */
	session.user_name = xml.transcode (elem->getAttribute (L"userName"));
/* position="..." */
	session.position = xml.transcode (elem->getAttribute (L"position"));
	return true;
}

bool
psych::config_t::parseMonitorNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* name="name" */
	attr = xml.transcode (elem->getAttribute (L"name"));
	if (!attr.empty())
		monitor_name = attr;
	return true;
}

bool
psych::config_t::parseEventQueueNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* name="name" */
	attr = xml.transcode (elem->getAttribute (L"name"));
	if (!attr.empty())
		event_queue_name = attr;
	return true;
}

bool
psych::config_t::parsePublisherNode (
	const DOMNode*		node,
	std::string&		name
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

/* name="name" */
	name = xml.transcode (elem->getAttribute (L"name"));
	return true;
}

bool
psych::config_t::parseVendorNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* name="name" */
	attr = xml.transcode (elem->getAttribute (L"name"));
	if (!attr.empty())
		vendor_name = attr;
	return true;
}

/* </Rfa> */

/* <psych> */
bool
psych::config_t::parsePsychNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;
	std::string attr;

/* interval="seconds" */
	attr = xml.transcode (elem->getAttribute (L"interval"));
	if (!attr.empty())
		interval = attr;
/* tolerableDelay="milliseconds" */
	attr = xml.transcode (elem->getAttribute (L"tolerableDelay"));
	if (!attr.empty())
		tolerable_delay = attr;
/* retryCount="count" */
	attr = xml.transcode (elem->getAttribute (L"retryCount"));
	if (!attr.empty())
		retry_count = attr;
/* retryDelayMs="milliseconds" */
	attr = xml.transcode (elem->getAttribute (L"retryDelayMs"));
	if (!attr.empty())
		retry_delay_ms = attr;
/* retryTimeoutMs="milliseconds" */
	attr = xml.transcode (elem->getAttribute (L"retryTimeoutMs"));
	if (!attr.empty())
		retry_timeout_ms = attr;
/* timeoutMs="milliseconds" */
	attr = xml.transcode (elem->getAttribute (L"timeoutMs"));
	if (!attr.empty())
		timeout_ms = attr;
/* connectTimeoutMs="milliseconds" */
	attr = xml.transcode (elem->getAttribute (L"connectTimeoutMs"));
	if (!attr.empty())
		connect_timeout_ms = attr;
/* enableHttpPipelining="boolean" */
	attr = xml.transcode (elem->getAttribute (L"enableHttpPipelining"));
	if (!attr.empty())
		enable_http_pipelining = attr;
/* maximumResponseSize="bytes" */
	attr = xml.transcode (elem->getAttribute (L"maximumResponseSize"));
	if (!attr.empty())
		maximum_response_size = attr;
/* minimumResponseSize="bytes" */
	attr = xml.transcode (elem->getAttribute (L"minimumResponseSize"));
	if (!attr.empty())
		minimum_response_size = attr;
/* requestHttpEncoding="encoding" */
	attr = xml.transcode (elem->getAttribute (L"requestHttpEncoding"));
	if (!attr.empty())
		request_http_encoding = attr;
/* timeOffsetConstant="time duration" */
	attr = xml.transcode (elem->getAttribute (L"timeOffsetConstant"));
	if (!attr.empty())
		time_offset_constant = attr;
/* panicThreshold="seconds" */
	attr = xml.transcode (elem->getAttribute (L"panicThreshold"));
	if (!attr.empty())
		panic_threshold = attr;
/* httpProxy="proxy" */
	attr = xml.transcode (elem->getAttribute (L"httpProxy"));
	if (!attr.empty())
		http_proxy = attr;
/* dnsCacheTimeout="seconds" */
	attr = xml.transcode (elem->getAttribute (L"dnsCacheTimeout"));
	if (!attr.empty())
		dns_cache_timeout = attr;
/* href="url" */
	attr = xml.transcode (elem->getAttribute (L"href"));
	if (!attr.empty())
		base_url = attr;

/* reset all lists */
	for (auto it = resources.begin(); it != resources.end(); ++it)
		it->fields.clear(), it->items.clear();
/* <resource> */
	nodeList = elem->getElementsByTagName (L"resource");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseResourceNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <resource> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <resource> nodes found.";
	return true;
}

bool
psych::config_t::parseResourceNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

	if (!elem->hasAttributes()) {
		LOG(ERROR) << "No attributes found, a \"name\" attribute is required.";
		return false;
	}
/* name="name" */
	std::string name (xml.transcode (elem->getAttribute (L"name")));
	if (name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute.";
		return false;
	}

/* <field> */
	std::map<std::string, int> fields;
	nodeList = elem->getElementsByTagName (L"field");
	for (int i = 0; i < nodeList->getLength(); i++) {
		std::string field_name;
		int field_id;
		if (!parseFieldNode (nodeList->item (i), &field_name, &field_id)) {
			LOG(ERROR) << "Failed parsing <field> nth-node #" << (1 + i) << ".";
			return false;
		}
		fields.emplace (std::make_pair (field_name, field_id));
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <field> nodes found.";
/* <item> */
	std::map<std::string, std::pair<std::string, std::string>> items;
	nodeList = elem->getElementsByTagName (L"item");
	for (int i = 0; i < nodeList->getLength(); i++) {
		std::string item_name, item_topic, item_src;
		if (!parseItemNode (nodeList->item (i), &item_name, &item_topic, &item_src)) {
			LOG(ERROR) << "Failed parsing <item> nth-node #" << (1 + i) << ".";
			return false;
		}
		items.emplace (std::make_pair (item_src, std::make_pair (item_name, item_topic)));
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <item> nodes found.";

/* <link> */
	nodeList = elem->getElementsByTagName (L"link");
	for (int i = 0; i < nodeList->getLength(); i++) {
		std::string link_rel, link_name, link_href;
		unsigned long link_id;
		if (!parseLinkNode (nodeList->item (i), &link_name, &link_rel, &link_id, &link_href)) {
			LOG(ERROR) << "Failed parsing <link> nth-node #" << (1 + i) << ".";
			return false;
		}
		resources.emplace_back (std::vector<resource_t>::value_type (name, link_name, link_href, link_id, fields, items));
	}
	if (0 == nodeList->getLength()) {
		LOG(WARNING) << "No <link> nodes found.";
		return true;
	}
	return true;
}

/* parse <link rel="resource" id="entitlement code" href="URL"/>
 */

bool
psych::config_t::parseLinkNode (
	const DOMNode*		node,
	std::string*		source,
	std::string*		rel,
	unsigned long*		id,
	std::string*		href
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

	if (!elem->hasAttributes()) {
		LOG(ERROR) << "No attributes found, \"rel\", \"name\", and \"href\" attributes are required.";
		return false;
	}
/* rel="resource" */
	*rel = xml.transcode (elem->getAttribute (L"rel"));
	if (rel->empty()) {
		LOG(ERROR) << "Undefined \"rel\" attribute, value cannot be empty.";
		return false;
	}
/* name="source feed name" */
	*source = xml.transcode (elem->getAttribute (L"name"));
	if (source->empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}
/* href="URL" */
	*href = xml.transcode (elem->getAttribute (L"href"));
	if (href->empty()) {
		LOG(ERROR) << "Undefined \"href\" attribute, value cannot be empty.";
		return false;
	}
/* id="integer" */
	const std::string id_text = xml.transcode (elem->getAttribute (L"id"));
	if (!id_text.empty())
		*id = std::strtoul (id_text.c_str(), nullptr, 10);
	else
		*id = 0;
	return true;
}

/* parse <field name="name" id="id"/>
 */

bool
psych::config_t::parseFieldNode (
	const DOMNode*		node,
	std::string*		name,
	int*			id
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

	if (!elem->hasAttributes()) {
		LOG(ERROR) << "No attributes found, \"name\" and \"id\" attributes are required.";
		return false;
	}
/* name="text" */
	*name = xml.transcode (elem->getAttribute (L"name"));
	if (name->empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}
/* id="integer" */
	const std::string id_text = xml.transcode (elem->getAttribute (L"id"));
	if (id_text.empty()) {
		LOG(ERROR) << "Undefined \"id\" attribute, value cannot be empty.";
		return false;
	}

	*id = std::stoi (id_text);
	return true;
}

/* parse <item name="name" topic="topic" src="text"/>
 */

bool
psych::config_t::parseItemNode (
	const DOMNode*		node,
	std::string*		name,
	std::string*		topic,
	std::string*		src
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

	if (!elem->hasAttributes()) {
		LOG(ERROR) << "No attributes found, \"name\", \"topic\", and \"src\" attributes are required.";
		return false;
	}
/* name="text" */
	*name = xml.transcode (elem->getAttribute (L"name"));
	if (name->empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}
/* topic="text" */
	*topic = xml.transcode (elem->getAttribute (L"topic"));
	if (topic->empty()) {
		LOG(ERROR) << "Undefined \"topic\" attribute, value cannot be empty.";
		return false;
	}
/* src="text" */
	*src = xml.transcode (elem->getAttribute (L"src"));
	if (src->empty()) {
		LOG(ERROR) << "Undefined \"src\" attribute, value cannot be empty.";
		return false;
	}
	return true;
}

/* </psych> */
/* </config> */

/* eof */
