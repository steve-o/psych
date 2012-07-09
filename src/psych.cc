/* A basic Velocity Analytics User-Plugin to exporting a new Tcl command and
 * periodically publishing out to ADH via RFA using RDM/MarketPrice.
 */

#include "psych.hh"

#define __STDC_FORMAT_MACROS
#include <cstdint>
#include <inttypes.h>

#include <windows.h>

/* Boost Posix Time */
#include <boost/date_time/gregorian/gregorian_types.hpp>

/* Boost Math */
#include <boost/math/special_functions/fpclassify.hpp>
#include <boost/math/special_functions/nonfinite_num_facets.hpp>

#include "chromium/file_util.hh"
#include "chromium/logging.hh"
#include "chromium/string_split.hh"
#include "chromium/string_tokenizer.hh"
#include "chromium/json/json_reader.hh"
#include "chromium/values.hh"
#include "snmp_agent.hh"
#include "error.hh"
#include "rfa_logging.hh"
#include "rfaostream.hh"
#include "version.hh"
#include "marketpsych.hh"

/* Default to allow up to 6 connections per host. Experiment and tuning may
 * try other values (greater than 0).  See http://crbug.com/12066.
 */
static const long kMaxSocketsPerHost = 6;

/* MarketPsych content magic number. */
#define FOURCC(a,b,c,d) ( (uint32_t) (((d)<<24) | ((c)<<16) | ((b)<<8) | (a)) )
static const uint32_t kPsychMagic (FOURCC ('#', ' ', 'M', 'a'));

/* Custom user-agent */
static const char* kHttpUserAgent = "psych/%u.%u.%u";

LONG volatile psych::psych_t::curl_ref_count_ = 0;

/* RDM Usage Guide: Section 6.5: Enterprise Platform
 * For future compatibility, the DictionaryId should be set to 1 by providers.
 * The DictionaryId for the RDMFieldDictionary is 1.
 */
static const int kDictionaryId = 1;

/* RDM: Absolutely no idea. */
static const int kFieldListId = 3;

/* RDM FIDs. */
static const int kRdmStockRicId		= 1026;
static const int kRdmSourceFeedNameId	= 1686;
static const int kRdmTimestampId	= 6378;
static const int kRdmEngineVersionId    = 8569;

/* FlexRecord Quote identifier. */
static const uint32_t kQuoteId = 40002;

/* Feed log file FlexRecord name */
static const char* kPsychFlexRecordName = "psych";

/* JSON configuration file */
static const char* kConfigJson = "config.json";

/* Tcl exported API. */
static const char* kBasicFunctionName = "psych_republish";
static const char* kResetFunctionName = "psych_hard_republish";

/* http://en.wikipedia.org/wiki/Unix_epoch */
static const boost::gregorian::date kUnixEpoch (1970, 1, 1);

LONG volatile psych::psych_t::instance_count_ = 0;

std::list<psych::psych_t*> psych::psych_t::global_list_;
boost::shared_mutex psych::psych_t::global_list_lock_;

static std::weak_ptr<rfa::common::EventQueue> g_event_queue;

using rfa::common::RFA_String;

/* Convert Posix time to Unix Epoch time.
 */
template< typename TimeT >
inline
TimeT
to_unix_epoch (
	const boost::posix_time::ptime t
	)
{
	return (t - boost::posix_time::ptime (kUnixEpoch)).total_seconds();
}

static
int
on_http_trace (
	CURL* handle,
	curl_infotype type,
	char* data,
	size_t size,
	void* userdata
	)
{
	switch (type) {
	case CURLINFO_TEXT:
		LOG(INFO) << data;
	default:
		return 0;

	case CURLINFO_HEADER_OUT:
		LOG(INFO) << "send header, " << size << " bytes";
		break;

	case CURLINFO_DATA_OUT:
		LOG(INFO) << "send data, " << size << " bytes";
		break;

	case CURLINFO_SSL_DATA_OUT:
		LOG(INFO) << "send ssl data, " << size << " bytes";
		break;

	case CURLINFO_HEADER_IN:
		LOG(INFO) << "recv header, " << size << " bytes";
		break;

	case CURLINFO_DATA_IN:
		LOG(INFO) << "recv data, " << size << " bytes";
		break;

	case CURLINFO_SSL_DATA_IN:
		LOG(INFO) << "recv ssl data, " << size << " bytes";
		break;
	}

	for (size_t i = 0; i < size; i += 0x40) {
		std::ostringstream ss;
		ss << std::setfill ('0')
		   << std::setw (4) << i;
		for (size_t c = 0; (c < 0x40) && ((i + c) < size); ++c) {
/* new line & carriage return */
			if (((i + c + 1) < size) && (data[i + c] == 0xd) && (data[i + c + 1] == 0xa)) {
				i += c + 2 - 0x40;
				break;
			}
			ss << (std::isprint (data[i + c]) ? data[i + c] : '.');
/* redundent new line */
			if (((i + c + 2) < size) && (data[i + c + 1] == 0xd) && (data[i + c + 2] == 0xa)) {
				i += c + 3 - 0x40;
				break;
			}
		}
		LOG(INFO) << ss.str();
	}
	return 0;
}

/* Incoming HTTP response header */
static
size_t
on_http_header (
	char* ptr,
	size_t size,
	size_t nmemb,
	void* userdata
	)
{
	auto connection = static_cast<psych::connection_t*> (userdata);
	assert (nullptr != connection);

/* Extract out date and current time to compare after full response. */
	if (0 == strncmp (ptr, "Date:", 5) &&
		strlen (ptr) <= strlen ("Date: ddd, dd MMM yyyy HH:mm:ss GMT\r\n"))	/* must end with <CR><LF> */
	{
		char ddd[26], MMM[26];
		tm pt_tm;

		memset (&pt_tm, 0, sizeof (pt_tm));

		int rc = sscanf (ptr, "Date: %s %hu %s %hu %hu:%hu:%hu",
			ddd, &pt_tm.tm_mday, MMM, &pt_tm.tm_year,
			&pt_tm.tm_hour, &pt_tm.tm_min, &pt_tm.tm_sec);
		if (7 == rc) {
			static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
							"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
			for (size_t i = 0; i < _countof (months); ++i) {
				if (0 == strcmp (months[i], MMM)) {
					pt_tm.tm_mon = i;
					break;
				}
			}

/* structm tm offsets */
			pt_tm.tm_year -= 1900;

/* not a valid tm: tm_wday, tm_yday, and tm_isdst not set */

			using namespace boost;
			const gregorian::date d (1900 + pt_tm.tm_year, 1 + pt_tm.tm_mon, pt_tm.tm_mday);
			const posix_time::time_duration td = posix_time::hours (pt_tm.tm_hour) + posix_time::minutes (pt_tm.tm_min) + posix_time::seconds (pt_tm.tm_sec);

			if (!d.is_not_a_date() && !td.is_not_a_date_time()) {
				const posix_time::ptime ptime (d, td);
				if (!ptime.is_not_a_date_time()) {
					connection->httpd_ptime = ptime;
				}
			}
		}
	}
	return size * nmemb;
}

/* Incoming HTTP response content */
static
size_t
on_http_data (
	char* ptr,
	size_t size,
	size_t nmemb,
	void* userdata
	)
{
	auto connection = static_cast<psych::connection_t*> (userdata);
	assert (nullptr != connection);
	char *effective_url = nullptr;
	curl_easy_getinfo (connection->handle.get(), CURLINFO_EFFECTIVE_URL, &effective_url);
	VLOG(3) << size << 'x' << nmemb << " for: " << effective_url;
	if ((connection->data.size() + (size * nmemb)) > connection->data.capacity()) {
		LOG(WARNING) << "Aborting long transfer for " << connection->url;
		return 0;
	}
	connection->data.append (ptr, size * nmemb);
	return size * nmemb;
}

psych::psych_t::psych_t() :
	is_shutdown_ (false),
	last_activity_ (boost::posix_time::microsec_clock::universal_time()),
	min_tcl_time_ (boost::posix_time::pos_infin),
	max_tcl_time_ (boost::posix_time::neg_infin),
	total_tcl_time_ (boost::posix_time::seconds(0)),
	min_refresh_time_ (boost::posix_time::pos_infin),
	max_refresh_time_ (boost::posix_time::neg_infin),
	total_refresh_time_ (boost::posix_time::seconds(0))
{
	ZeroMemory (cumulative_stats_, sizeof (cumulative_stats_));
	ZeroMemory (snap_stats_, sizeof (snap_stats_));

/* Unique instance number, never decremented. */
	instance_ = InterlockedExchangeAdd (&instance_count_, 1L);

	boost::unique_lock<boost::shared_mutex> (global_list_lock_);
	global_list_.push_back (this);
}

psych::psych_t::~psych_t()
{
/* Remove from list before clearing. */
	boost::unique_lock<boost::shared_mutex> (global_list_lock_);
	global_list_.remove (this);

	clear();
}

#ifndef CONFIG_PSYCH_AS_APPLICATION
/* Plugin entry point from the Velocity Analytics Engine.
 */

void
psych::psych_t::init (
	const vpf::UserPluginConfig& vpf_config
	)
{
/* Thunk to VA user-plugin base class. */
	vpf::AbstractUserPlugin::init (vpf_config);

/* Save copies of provided identifiers. */
	plugin_id_.assign (vpf_config.getPluginId());
	plugin_type_.assign (vpf_config.getPluginType());
	LOG(INFO) << "{ "
		  "\"pluginType\": \"" << plugin_type_ << "\""
		", \"pluginId\": \"" << plugin_id_ << "\""
		", \"instance\": " << instance_ <<
		", \"version\": \"" << version_major << '.' << version_minor << '.' << version_build << "\""
		", \"build\": { "
			  "\"date\": \"" << build_date << "\""
			", \"time\": \"" << build_time << "\""
			", \"system\": \"" << build_system << "\""
			", \"machine\": \"" << build_machine << "\""
			" }"
		" }";
	
	if (!config_.parseDomElement (vpf_config.getXmlConfigData())) {
		is_shutdown_ = true;
		throw vpf::UserPluginException ("Invalid configuration, aborting.");
	}
	if (!init()) {
		clear();
		is_shutdown_ = true;
		throw vpf::UserPluginException ("Initialization failed, aborting.");
	}
}
#endif /* CONFIG_PSYCH_AS_APPLICATION */

bool
psych::psych_t::init()
{
/* TODO: split large configuration display into multiple parts */
	LOG(INFO) << config_;

/** libcurl initialisation. **/
	CURLcode curl_errno;
/* initialise everything, not thread-safe */
	if (InterlockedExchangeAdd (&curl_ref_count_, 1L) == 0) {
		curl_errno = curl_global_init (CURL_GLOBAL_ALL);
		if (CURLE_OK != curl_errno) {
			LOG(ERROR) << "curl_global_init failed: { "
				"\"code\": " << (int)curl_errno <<
				" }";
			return false;
		}
	}

/* multi-interface context */
	multipass_.reset (curl_multi_init());
	if (!(bool)multipass_)
		return false;

	CURLMcode curl_merrno;
/* libcurl 7.16.0: HTTP Pipelining as far as possible. */
	if (!config_.enable_http_pipelining.empty()) {
		const long value = std::atol (config_.enable_http_pipelining.c_str());
		curl_merrno = curl_multi_setopt (multipass_.get(), CURLMOPT_PIPELINING, value);
		LOG_IF(WARNING, CURLM_OK != curl_merrno) << "CURLMOPT_PIPELINING failed: { "
			  "\"code\": " << (int)curl_merrno <<
			", \"text\": \"" << curl_multi_strerror (curl_merrno) << "\""
			" }";
	}

/* libcurl 7.16.3: maximum amount of simultaneously open connections that libcurl may cache. */
	curl_merrno = curl_multi_setopt (multipass_.get(), CURLMOPT_MAXCONNECTS, kMaxSocketsPerHost);
	LOG_IF(WARNING, CURLM_OK != curl_merrno) << "CURLMOPT_MAXCONNECTS failed: { "
		  "\"code\": " << (int)curl_merrno <<
		", \"text\": \"" << curl_multi_strerror (curl_merrno) << "\""
		" }";

/** RFA initialisation. **/
	try {
/* RFA context. */
		rfa_.reset (new rfa_t (config_));
		if (!(bool)rfa_ || !rfa_->init())
			return false;

/* RFA asynchronous event queue. */
		const RFA_String eventQueueName (config_.event_queue_name.c_str(), 0, false);
		event_queue_.reset (rfa::common::EventQueue::create (eventQueueName), std::mem_fun (&rfa::common::EventQueue::destroy));
		if (!(bool)event_queue_)
			return false;
/* Create weak pointer to handle application shutdown. */
		g_event_queue = event_queue_;

/* RFA logging. */
		log_.reset (new logging::rfa::LogEventProvider (config_, event_queue_));
		if (!(bool)log_ || !log_->Register())
			return false;

/* RFA provider. */
		provider_.reset (new provider_t (config_, rfa_, event_queue_));
		if (!(bool)provider_ || !provider_->init())
			return false;

/* Create state for published instruments.
 */
		for (auto it = config_.resources.begin(); it != config_.resources.end(); ++it)
		{
/* create connection */
			std::string url (config_.base_url);
			url.append (it->path);
			auto connection = std::make_shared<connection_t> (*it, url);
			connections_.emplace (std::make_pair (*it, connection));

/* create stream per "name" */
			std::map<std::string, std::pair<std::string, std::shared_ptr<broadcast_stream_t>>> name_map;
			for (auto jt = it->items.begin();
				it->items.end() != jt;
				++jt)
			{
				std::shared_ptr<broadcast_stream_t> stream;
/* RIC may not be unique */
				auto kt = stream_vector_.find (jt->second.first);
				if (stream_vector_.end() == kt) {
					VLOG(1) << "create stream <" << jt->second.first << ">";
					stream = std::make_shared<broadcast_stream_t> (*it);
					assert ((bool)stream);
					if (!provider_->createItemStream (jt->second.first.c_str(), stream))
						return false;
					stream_vector_.emplace (std::make_pair (jt->second.first, stream));
				} else {
					VLOG(1) << "re-use stream <" << jt->second.first << ">";
					stream = kt->second;
				}

				name_map.emplace (std::make_pair (jt->first, std::make_pair (jt->second.second, stream)));
			}
			query_vector_.emplace (std::make_pair (*it, name_map));
		}

	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
			", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
			", \"StatusText\": \"" << e.getStatus().getStatusText() << "\" }";
		return false;
	} catch (rfa::common::InvalidConfigurationException& e) {
		LOG(ERROR) << "InvalidConfigurationException: { "
			  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
			", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
			", \"StatusText\": \"" << e.getStatus().getStatusText() << "\""
			", \"ParameterName\": \"" << e.getParameterName() << "\""
			", \"ParameterValue\": \"" << e.getParameterValue() << "\" }";
		return false;
	}

#ifndef CONFIG_PSYCH_AS_APPLICATION
/* No main loop inside this thread, must spawn new thread for message pump. */
	event_pump_.reset (new event_pump_t (event_queue_));
	if (!(bool)event_pump_) {
		LOG(ERROR) << "Cannot create event pump.";
		return false;
	}

	event_thread_.reset (new boost::thread (*event_pump_.get()));
	if (!(bool)event_thread_) {
		LOG(ERROR) << "Cannot spawn event thread.";
		return false;
	}
#endif /* CONFIG_PSYCH_AS_APPLICATION */

/* Spawn SNMP implant. */
	if (config_.is_snmp_enabled) {
		snmp_agent_.reset (new snmp_agent_t (*this));
		if (!(bool)snmp_agent_)
			return false;
	}

#ifndef CONFIG_PSYCH_AS_APPLICATION
/* Register Tcl API. */
	if (!register_tcl_api (getId()))
		return false;
#endif

/* Timer for periodic publishing.
 */
	using namespace boost;
	posix_time::ptime due_time;
	if (!get_next_interval (&due_time)) {
		LOG(ERROR) << "Cannot calculate next interval.";
		return false;
	}
/* convert Boost Posix Time into a Chrono time point */
	const auto time = to_unix_epoch<std::time_t> (due_time);
	const auto tp = chrono::system_clock::from_time_t (time);

	const chrono::seconds td (std::stoul (config_.interval));
	timer_.reset (new time_pump_t<chrono::system_clock> (tp, td, this));
	if (!(bool)timer_) {
		LOG(ERROR) << "Cannot create time pump.";
		return false;
	}
	timer_thread_.reset (new thread (*timer_.get()));
	if (!(bool)timer_thread_) {
		LOG(ERROR) << "Cannot spawn timer thread.";
		return false;
	}
	LOG(INFO) << "Added periodic timer, interval " << td.count() << " seconds"
		", offset " << config_.time_offset_constant <<
		", due time " << posix_time::to_simple_string (due_time);
	return true;
}

/* Application entry point.
 */
int
psych::psych_t::run()
{
	LOG(INFO) << "{ "
		  "\"version\": \"" << version_major << '.' << version_minor << '.' << version_build << "\""
		", \"build\": { "
			  "\"date\": \"" << build_date << "\""
			", \"time\": \"" << build_time << "\""
			", \"system\": \"" << build_system << "\""
			", \"machine\": \"" << build_machine << "\""
			" }"
		" }";

	std::unique_ptr<chromium::Value> root;
	std::string json;

	if (!file_util::ReadFileToString (kConfigJson, &json)) {
		LOG(ERROR) << "Cannot read configuration file \"" << kConfigJson << "\".";
		return EXIT_FAILURE;
	}
/* parse JSON configuration */
	{
		int error_code; std::string error_msg;
		chromium::Value* v = chromium::JSONReader::ReadAndReturnError (json, false, &error_code, &error_msg);
		if (nullptr == v) {
			LOG(ERROR) << "Cannot read JSON configuration, error code: " <<
				error_code << " text: \"" << error_msg << "\".";
		}
	}
	root.reset (chromium::JSONReader::Read (json, false));
	CHECK (root.get());
	CHECK (root->IsType (chromium::Value::TYPE_DICTIONARY));
	if (!config_.parseConfig (static_cast<chromium::DictionaryValue*>(root.get())))
		return EXIT_FAILURE;

	if (!init())
		return EXIT_FAILURE;

	LOG(INFO) << "Init complete, Entering main loop.";
	mainLoop();
	LOG(INFO) << "Main loop terminated.";
	destroy();
	return EXIT_SUCCESS;
}

/* On a shutdown event set a global flag and force the event queue
 * to catch the event by submitting a log event.
 */
static
BOOL
CtrlHandler (
	DWORD	fdwCtrlType
	)
{
	const char* message;
	switch (fdwCtrlType) {
	case CTRL_C_EVENT:
		message = "Caught ctrl-c event, shutting down.";
		break;
	case CTRL_CLOSE_EVENT:
		message = "Caught close event, shutting down.";
		break;
	case CTRL_BREAK_EVENT:
		message = "Caught ctrl-break event, shutting down.";
		break;
	case CTRL_LOGOFF_EVENT:
		message = "Caught logoff event, shutting down.";
		break;
	case CTRL_SHUTDOWN_EVENT:
	default:
		message = "Caught shutdown event, shutting down.";
		break;
	}
/* if available, deactivate global event queue pointer to break running loop. */
	if (!g_event_queue.expired()) {
		auto sp = g_event_queue.lock();
		sp->deactivate();
	}
	LOG(INFO) << message;
	return TRUE;
}

void
psych::psych_t::mainLoop()
{
/* Add shutdown handler. */
	::SetConsoleCtrlHandler ((PHANDLER_ROUTINE)::CtrlHandler, TRUE);
	while (event_queue_->isActive()) {
		event_queue_->dispatch (rfa::common::Dispatchable::InfiniteWait);
	}
/* Remove shutdown handler. */
	::SetConsoleCtrlHandler ((PHANDLER_ROUTINE)::CtrlHandler, FALSE);
}

void
psych::psych_t::clear()
{
/* Stop generating new events. */
	if (timer_thread_) {
		timer_thread_->interrupt();
		timer_thread_->join();
	}	
	timer_thread_.reset();
	timer_.reset();

/* Close SNMP agent. */
	snmp_agent_.reset();

/* Signal message pump thread to exit. */
	if ((bool)event_queue_)
		event_queue_->deactivate();
/* Drain and close event queue. */
	if ((bool)event_thread_)
		event_thread_->join();

/* Release everything with an RFA dependency. */
	event_thread_.reset();
	event_pump_.reset();
	stream_vector_.clear();
	query_vector_.clear();
	assert (provider_.use_count() <= 1);
	provider_.reset();
	assert (log_.use_count() <= 1);
	log_.reset();
	assert (event_queue_.use_count() <= 1);
	event_queue_.reset();
	assert (rfa_.use_count() <= 1);
	rfa_.reset();

/* cleanup libcurl */
	multipass_.reset();
/* not thread-safe */
	if (1 == InterlockedExchangeAdd (&curl_ref_count_, -1L))
		curl_global_cleanup();
}

/* Plugin exit point.
 */

void
psych::psych_t::destroy()
{
	LOG(INFO) << "Closing instance.";
#ifndef CONFIG_PSYCH_AS_APPLICATION
/* Unregister Tcl API. */
	unregister_tcl_api (getId());
#endif
	clear();
	LOG(INFO) << "Runtime summary: {"
		    " \"tclQueryReceived\": " << cumulative_stats_[PSYCH_PC_TCL_QUERY_RECEIVED] <<
		   ", \"timerQueryReceived\": " << cumulative_stats_[PSYCH_PC_TIMER_QUERY_RECEIVED] <<
		" }";
	LOG(INFO) << "Instance closed.";
#ifndef CONFIG_PSYCH_AS_APPLICATION
	vpf::AbstractUserPlugin::destroy();
#endif
}

/* callback from periodic timer.
 */
bool
psych::psych_t::processTimer (
	const boost::chrono::time_point<boost::chrono::system_clock>& t
	)
{
/* calculate timer accuracy, typically 15-1ms with default timer resolution.
 */
	if (DLOG_IS_ON(INFO)) {
		using namespace boost::chrono;
		auto now = system_clock::now();
		auto ms = duration_cast<milliseconds> (now - t);
		if (0 == ms.count()) {
			LOG(INFO) << "delta " << duration_cast<microseconds> (now - t).count() << "us";
		} else {
			LOG(INFO) << "delta " << ms.count() << "ms";
		}
	}

	cumulative_stats_[PSYCH_PC_TIMER_QUERY_RECEIVED]++;

/* Prevent overlapped queries. */
	boost::unique_lock<boost::shared_mutex> lock (query_mutex_, boost::try_to_lock_t());
	if (!lock.owns_lock()) {
		LOG(WARNING) << "Periodic refresh aborted due to running query.";
		return true;
	}

	try {
		httpPsychQuery (connections_, QUERY_HTTP_KEEPALIVE | QUERY_IF_MODIFIED_SINCE);
	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
			", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
			", \"StatusText\": \"" << e.getStatus().getStatusText() << "\" }";
	}
	return true;
}

/* Calculate the next bin close timestamp for the requested timezone.
 */
bool
psych::psych_t::get_next_interval (
	boost::posix_time::ptime* t
	)
{
	using namespace boost::posix_time;

	const time_duration reference_tod = duration_from_string (config_.time_offset_constant);
	const int interval_seconds = std::stoi (config_.interval);
	const ptime now_ptime (second_clock::universal_time());
	const time_duration now_tod = now_ptime.time_of_day();

	ptime reference_ptime (now_ptime.date(), reference_tod);

/* yesterday */
	while ((reference_tod + seconds (interval_seconds)) > now_tod)
		reference_ptime -= boost::gregorian::days (1);

	const time_duration offset = now_ptime - reference_ptime;

/* round down to multiple of interval */
	const ptime end_ptime = reference_ptime + seconds ((offset.total_seconds() / interval_seconds) * interval_seconds);

/* increment to next period */
	const ptime next_ptime = end_ptime + seconds (interval_seconds);

	*t = next_ptime;
	return true;
}

/* execute MarketPsych HTTP query.
 */

#define RETRY_SLEEP_DEFAULT 1000L   /* ms */
#define RETRY_SLEEP_MAX     600000L /* ms == 10 minutes */

bool
psych::psych_t::httpPsychQuery (
	std::map<resource_t, std::shared_ptr<connection_t>, resource_compare_t>& connections,
	int flags
	)
{
	using namespace boost::posix_time;
	const ptime t0 (microsec_clock::universal_time());
	last_activity_ = t0;

	VLOG(1) << "curl start:";
/* retries are handled on a carousel basis, one round tries every connection queued. */
	std::vector<std::shared_ptr<connection_t>> pending;
	pending.reserve (connections.size());
	std::for_each (connections.begin(), connections.end(),
		[&pending](std::pair<resource_t, std::shared_ptr<connection_t>> pair)
	{
		pending.emplace_back (pair.second);
	});

	ptime retrystart (t0);
	long retry_numretries = std::atol (config_.retry_count.c_str());
	const long config_retry_delay = std::atol (config_.retry_delay_ms.c_str());
	const long retry_sleep_default = (0 != config_retry_delay) ? config_retry_delay : RETRY_SLEEP_DEFAULT;
	long retry_sleep = retry_sleep_default;

/* big phat loop */
	for(;;)
	{
		std::for_each (pending.begin(), pending.end(),
			[&](std::shared_ptr<connection_t> connection)
		{
			VLOG(2) << "preparing URL " << connection->url;
			connection->handle.reset (curl_easy_init());
			if (!(bool)connection->handle)
				return;
			connection->data.clear();
			CURL* eh = connection->handle.get();
/* Maximum filesize, use original < 2GB libcurl option for convenience. */
			assert (!config_.maximum_response_size.empty());
			const long maxfilesize = std::atol (config_.maximum_response_size.c_str());
			connection->data.reserve (maxfilesize);
			CURLcode curl_errno = curl_easy_setopt (eh, CURLOPT_MAXFILESIZE, maxfilesize);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_MAXFILESIZE failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* target resource */
			curl_errno = curl_easy_setopt (eh, CURLOPT_URL, connection->url.c_str());
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_URL failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* incoming response header */
			curl_errno = curl_easy_setopt (eh, CURLOPT_HEADERFUNCTION, on_http_header);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_HEADERFUNCTION failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* incoming response data */
			curl_errno = curl_easy_setopt (eh, CURLOPT_WRITEFUNCTION, on_http_data);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_WRITEFUNCTION failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* closure for callbacks */
			curl_errno = curl_easy_setopt (eh, CURLOPT_WRITEHEADER, connection.get());
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_WRITEHEADER failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
			curl_errno = curl_easy_setopt (eh, CURLOPT_WRITEDATA, connection.get());
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_WRITEDATA failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* closure for information queue */
			curl_errno = curl_easy_setopt (eh, CURLOPT_PRIVATE, connection.get());
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_PRIVATE failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* do not include header in output */
			curl_errno = curl_easy_setopt (eh, CURLOPT_HEADER, 0L);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_HEADER failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* Fresh connection for hard-refresh.  Socket is left open for re-use. */
			if (!(flags & QUERY_HTTP_KEEPALIVE)) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_FRESH_CONNECT, 1L);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_FRESH_CONNECT failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
			}
/* Connection timeout: minimum 1s when using system name resolver */
			if (!config_.connect_timeout_ms.empty()) {
				const long connect_timeout_ms = std::atol (config_.connect_timeout_ms.c_str());
				curl_errno = curl_easy_setopt (eh, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_CONNECTTIMEOUT_MS failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
			}
/* Force IPv4 */
			curl_errno = curl_easy_setopt (eh, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_IPRESOLVE failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* Transfer timeout */
			if (!config_.timeout_ms.empty()) {
				const long timeout_ms = std::atol (config_.timeout_ms.c_str());
				curl_errno = curl_easy_setopt (eh, CURLOPT_TIMEOUT_MS, timeout_ms);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_TIMEOUT_MS failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
			}
/* DNS response cache, in seconds. */
			if (!config_.dns_cache_timeout.empty()) {
				const long timeout = std::atol (config_.dns_cache_timeout.c_str());
				curl_errno = curl_easy_setopt (eh, CURLOPT_DNS_CACHE_TIMEOUT, timeout);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_DNS_CACHE_TIMEOUT failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
			}
/* Custom user-agent */
			char user_agent[1024];
			sprintf_s (user_agent, sizeof (user_agent), kHttpUserAgent, version_major, version_minor, version_build);
			curl_errno = curl_easy_setopt (eh, CURLOPT_USERAGENT, user_agent);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_USERAGENT failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* Extract file modification time */
			curl_errno = curl_easy_setopt (eh, CURLOPT_FILETIME, (long)1L);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_FILETIME failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* The If-Modified-Since header */
			if (flags & QUERY_IF_MODIFIED_SINCE) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_TIMECONDITION, (long)CURL_TIMECOND_IFMODSINCE);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_TIMECONDITION failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
/* This should be the time in seconds since 1 Jan 1970 GMT as per RFC2616 */
				const long timevalue = connection->last_filetime;
				curl_errno = curl_easy_setopt (eh, CURLOPT_TIMEVALUE, timevalue);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_TIMEVALUE failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
			}
/* Record approximate request timestamp. */
			connection->request_ptime = t0;
			connection->httpd_ptime = boost::posix_time::not_a_date_time;
/* Request encoding: identity, deflate or gzip. */
			if (!config_.request_http_encoding.empty()) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_ACCEPT_ENCODING, config_.request_http_encoding.c_str());
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_ACCEPT_ENCODING failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
			}
/* HTTP proxy for internal development */
			if (!config_.http_proxy.empty()) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_PROXY, config_.http_proxy.c_str());
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_PROXY failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
			}
/* buffer for text form of error codes */
			curl_errno = curl_easy_setopt (eh, CURLOPT_ERRORBUFFER, connection->error);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_ERRORBUFFER failed: { "
				  "\"code\": " << (int)curl_errno <<
				", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
				" }";
/* debug mode */
			if (VLOG_IS_ON (10)) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_VERBOSE, 1L);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_VERBOSE failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
				curl_errno = curl_easy_setopt (eh, CURLOPT_DEBUGFUNCTION, on_http_trace);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_DEBUGFUNCTION failed: { "
					  "\"code\": " << (int)curl_errno <<
					", \"text\": \"" << curl_easy_strerror (curl_errno) << "\""
					" }";
			}

			CURLMcode curl_merrno = curl_multi_add_handle (multipass_.get(), eh);
			LOG_IF(ERROR, CURLM_OK != curl_merrno) << "curl_multi_add_handle failed: { "
				  "\"url\": \"" << connection->url << "\""
				", \"code\": " << (int)curl_merrno <<
				", \"text\": \"" << curl_multi_strerror (curl_merrno) << "\""
				" }";
		});

		int running_handles = 0;
		
		VLOG(3) << "perform";
		CURLMcode curl_merrno = curl_multi_perform (multipass_.get(), &running_handles);
		LOG_IF(ERROR, CURLM_OK != curl_merrno && CURLM_CALL_MULTI_PERFORM != curl_merrno) << "curl_multi_perform failed: { "
			  "\"code\": " << (int)curl_merrno <<
			", \"text\": \"" << curl_multi_strerror (curl_merrno) << "\""
			" }";

		cumulative_stats_[PSYCH_PC_HTTP_REQUEST_SENT] += pending.size();

		while (running_handles > 0)
		{
			VLOG(3) << "perform";
			while (CURLM_CALL_MULTI_PERFORM == curl_merrno)
				curl_merrno = curl_multi_perform (multipass_.get(), &running_handles);
			LOG_IF(ERROR, CURLM_OK != curl_merrno) << "curl_multi_perform failed: { "
				  "\"code\": " << (int)curl_merrno <<
				", \"text\": \"" << curl_multi_strerror (curl_merrno) << "\""
				" }";
			if (0 == running_handles)
				break;

			fd_set readfds, writefds, exceptfds;
			int max_fd = -1;
			long timeout = -1;
			FD_ZERO (&readfds);
			FD_ZERO (&writefds);
			FD_ZERO (&exceptfds);

			curl_merrno = curl_multi_fdset (multipass_.get(), &readfds, &writefds, &exceptfds, &max_fd);
			LOG_IF(ERROR, CURLM_OK != curl_merrno && CURLM_CALL_MULTI_PERFORM != curl_merrno) << "curl_multi_fdset failed: { "
				  "\"code\": " << (int)curl_merrno <<
				", \"text\": \"" << curl_multi_strerror (curl_merrno) << "\""
				" }";
			curl_merrno = curl_multi_timeout (multipass_.get(), &timeout);
			LOG_IF(ERROR, CURLM_OK != curl_merrno && CURLM_CALL_MULTI_PERFORM != curl_merrno) << "curl_multi_timeout failed: { "
				  "\"code\": " << (int)curl_merrno <<
				", \"text\": \"" << curl_multi_strerror (curl_merrno) << "\""
				" }";

			if (-1 == max_fd || -1 == timeout) { /* not monitorable state */
				Sleep (100);	/* recommended 100ms wait */
			} else {
				struct timeval tv;
				tv.tv_sec = timeout / 1000;
				tv.tv_usec = (timeout % 1000) * 1000;
/* ignore error state */
				select (1 + max_fd, &readfds, &writefds, &exceptfds, &tv);
			}
			curl_merrno = CURLM_CALL_MULTI_PERFORM;
		}

		VLOG(2) << "curl result processing.";
		std::string engine_version;
		ptime open_time, close_time;
		std::vector<std::string> columns;
		std::vector<std::pair<std::string, std::vector<double>>> rows;

		int msgs_in_queue = 0;
		CURLMsg* msg = curl_multi_info_read (multipass_.get(), &msgs_in_queue);
		while (nullptr != msg) {
			VLOG(3) << "result: { "
				  "\"msg\": " << msg->msg <<
				", \"code\": " << msg->data.result <<
				", \"text\": \"" << curl_easy_strerror (msg->data.result) << "\""
				" }";
			if (CURLMSG_DONE == msg->msg) {
				void* ptr;
				curl_easy_getinfo (msg->easy_handle, CURLINFO_PRIVATE, &ptr);
				auto connection = static_cast<connection_t*> (ptr);
				open_time = close_time = not_a_date_time;
				columns.clear(); rows.clear();
				if (processHttpResponse (connection, &engine_version, &open_time, &close_time, &columns, &rows))
				{
					try {
						sendRefresh (connection->resource, engine_version, open_time, close_time, columns, rows);
					} catch (rfa::common::InvalidUsageException& e) {
						LOG(ERROR) << "InvalidUsageException: { "
							  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
							", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
							", \"StatusText\": \"" << e.getStatus().getStatusText() << "\""
							" }";
					}
/* ignoring RFA, request is now considered successful */
					const CURLMcode curl_merrno = curl_multi_remove_handle (multipass_.get(), connection->handle.get());
					LOG_IF(ERROR, CURLM_OK != curl_merrno) << "curl_multi_remove_handle failed: { "
						  "\"code\": " << (int)curl_merrno <<
						", \"text\": \"" << curl_multi_strerror (curl_merrno) << "\""
						" }";
/* remove from pending queue */
					pending.erase (std::remove_if (pending.begin(),
						pending.end(),
						[ptr](std::shared_ptr<connection_t>& connection) -> bool {
							return connection.get() == ptr;
					}));
				}
				else
				{
/* header or payload */
					cumulative_stats_[PSYCH_PC_HTTP_MALFORMED]++;
				}
			}
			msg = curl_multi_info_read (multipass_.get(), &msgs_in_queue);
		}

/* complete */
		if (pending.empty())
			break;
/* retry */
		if (retry_numretries > 0 &&
			(config_.retry_timeout_ms.empty() ||
			((microsec_clock::universal_time() - retrystart).total_milliseconds() < std::atol (config_.retry_timeout_ms.c_str()))) )
		{
			LOG(WARNING) << "Transient problem, will retry in "
				<< retry_sleep << " milliseconds.  "
				<< retry_numretries << " retries left.";

			Sleep (retry_sleep);
			--retry_numretries;
			if (0 == config_retry_delay) {
				retry_sleep *= 2;
				if (retry_sleep > RETRY_SLEEP_MAX)
					retry_sleep = RETRY_SLEEP_MAX;
			}
			continue;
		}

		LOG(WARNING) << "Aborted transfer.";
		cumulative_stats_[PSYCH_PC_HTTP_RETRIES_EXCEEDED]++;
		break;
	}

	VLOG(2) << "curl cleanup.";
	std::for_each (connections.begin(), connections.end(),
		[](std::pair<resource_t, std::shared_ptr<connection_t>> pair)
	{
		pair.second->handle.reset();
	});

	VLOG(1) << "curl fin.";
/* Timing */
	const ptime t1 (microsec_clock::universal_time());
	const time_duration td = t1 - t0;
	LOG(INFO) << "Refresh complete " << td.total_milliseconds() << "ms.";
	if (td < min_refresh_time_) min_refresh_time_ = td;
	if (td > max_refresh_time_) max_refresh_time_ = td;
	total_refresh_time_ += td;
	return true;
}

bool
psych::psych_t::processHttpResponse (
	connection_t* connection,
	std::string* engine_version,
	boost::posix_time::ptime* open_time,
	boost::posix_time::ptime* close_time,
	std::vector<std::string>* columns,
	std::vector<std::pair<std::string, std::vector<double>>>* rows
	)
{
	assert (nullptr != connection);
	CURL* eh = connection->handle.get();
	assert (nullptr != eh);

	char *effective_url = nullptr;
	curl_easy_getinfo (eh, CURLINFO_EFFECTIVE_URL, &effective_url);
	long response_code;
	curl_easy_getinfo (eh, CURLINFO_RESPONSE_CODE, &response_code);
	char *content_type = nullptr;
	curl_easy_getinfo (eh, CURLINFO_CONTENT_TYPE, &content_type);
	double size_download;
	curl_easy_getinfo (eh, CURLINFO_SIZE_DOWNLOAD, &size_download);
	double total_time;
	curl_easy_getinfo (eh, CURLINFO_TOTAL_TIME, &total_time);
	double starttransfer_time;
	curl_easy_getinfo (eh, CURLINFO_STARTTRANSFER_TIME, &starttransfer_time);

/* dump HTTP-decoded content */
	VLOG(4) << "HTTP: { "
			  "\"url\": \"" << effective_url << "\""
			", \"status\": " << response_code <<
			", \"type\": \"" << content_type << "\""
			", \"size\": " << size_download <<
			", \"content\": " << connection->data.size() <<
			", \"time\": " << total_time <<
			", \"latency\": " << starttransfer_time <<
			" }";
	VLOG(5) << "payload: " << connection->data;

/* breakdown and count each range of response code */
	if (200 != response_code)
	{
		if (response_code >= 100 && response_code <= 199)
			cumulative_stats_[PSYCH_PC_HTTP_1XX_RECEIVED]++;
		else if (response_code >= 200 && response_code <= 299)
			cumulative_stats_[PSYCH_PC_HTTP_2XX_RECEIVED]++;
		else if (response_code >= 300 && response_code <= 399)
			cumulative_stats_[PSYCH_PC_HTTP_3XX_RECEIVED]++;
		else if (response_code >= 400 && response_code <= 499)
			cumulative_stats_[PSYCH_PC_HTTP_4XX_RECEIVED]++;
		else if (response_code >= 500 && response_code <= 599)
			cumulative_stats_[PSYCH_PC_HTTP_5XX_RECEIVED]++;
		if (304 == response_code)
			cumulative_stats_[PSYCH_PC_HTTP_304_RECEIVED]++;
		LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on status code: " << response_code << ".";
		return false;
	}

	cumulative_stats_[PSYCH_PC_HTTP_200_RECEIVED]++;
	cumulative_stats_[PSYCH_PC_HTTP_2XX_RECEIVED]++;

	if (0 != strncmp (content_type, "text/plain", strlen ("text/plain"))) {
		LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on content-type: \"" << content_type << "\".";
		return false;
	}

	const long minimum_response_size = !config_.minimum_response_size.empty() ? std::atol (config_.minimum_response_size.c_str()) : sizeof (uint32_t);
	assert (minimum_response_size > sizeof (uint32_t));
	if (connection->data.size() < minimum_response_size) {
		LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on content size: " << connection->data.size() << " less than configured minimum response size of " << minimum_response_size << " bytes.";
		return false;
	}

/* inspect payload */
	const char* cdata = connection->data.c_str();
	const uint32_t magic (FOURCC (cdata[0], cdata[1], cdata[2], cdata[3]));
	if (kPsychMagic != magic) {
		LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on payload magic number: " << std::hex << std::showbase << magic << ".";
		return false;
	}

/* time difference to HTTPD server: for monitoring only. */
	long httpd_offset = 0L;
	if (!connection->httpd_ptime.is_not_a_date_time()) {
		cumulative_stats_[PSYCH_PC_HTTPD_CLOCK_DRIFT] = httpd_offset = (connection->httpd_ptime - connection->request_ptime).total_seconds();
	}

/* ex: 2012-May-03 21:19:00 */
	long filetime = -1L;
	long http_offset = 0L;
	curl_easy_getinfo (eh, CURLINFO_FILETIME, &filetime);
	if (-1L != filetime) {
		assert (!connection->request_ptime.is_not_a_date_time());

/* perform sanity check on timestamp. */
		const long request_filetime = (connection->request_ptime - boost::posix_time::ptime (kUnixEpoch)).total_seconds();
		cumulative_stats_[PSYCH_PC_HTTP_CLOCK_DRIFT] = http_offset = filetime - request_filetime;
		if (!config_.panic_threshold.empty()) {
			if (labs (http_offset) >= std::atol (config_.panic_threshold.c_str())) {
				LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on filetime clock offset " << http_offset << " seconds breaching panic threshold " << config_.panic_threshold << ".";
				return false;
			}
		}
		connection->last_filetime = filetime;
	}

/* extract data table from payload */
	enum { STATE_TIMESTAMP, STATE_HEADER, STATE_ROW, STATE_FIN } state = STATE_TIMESTAMP;
	StringTokenizer t (connection->data, "\n");

	while (t.GetNext()) {
		if (STATE_TIMESTAMP == state) {
/* # MarketPsych Engine Version x.y | 2012-05-02 21:19:00 UTC - 2012-05-03 21:19:00 UTC */
			const auto& token = t.token();
			if (token.size() < strlen ("# MarketPsych Engine Version 0 | 2012-05-02 21:19:00 UTC - 2012-05-03 21:19:00 UTC")) {
				LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on malformed header \"" << token << "\".";
				return false;
			}

			static const size_t prefix_len = strlen ("# MarketPsych Engine Version ");
			static const size_t date_len   = strlen ("2012-05-02 21:19:00");

			const auto space_after_version = token.find (" ", prefix_len);
			if (std::string::npos == space_after_version) {
				LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on malformed header \"" << token << "\".";
				return false;
			}
			*engine_version = token.substr (prefix_len, space_after_version - prefix_len);

			const auto pipe_delimiter = token.find ("| ", space_after_version);
			if (std::string::npos == pipe_delimiter) {
				LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on malformed header \"" << token << "\".";
				return false;
			}
			const auto open_time_string = token.substr (pipe_delimiter + strlen ("| "), date_len);
			try {
				*open_time = boost::posix_time::time_from_string (open_time_string);
			} catch (const std::exception& e) {
				LOG(WARNING) << "Caught exception parsing open time \"" << open_time_string << "\": " << e.what();
			}

			const auto hyphen_delimiter = token.find ("- ", pipe_delimiter);
			if (std::string::npos == hyphen_delimiter) {
				LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on malformed header \"" << token << "\".";
				return false;
			}
			const auto close_time_string = token.substr (hyphen_delimiter + strlen ("- "), date_len);
			try {
				*close_time = boost::posix_time::time_from_string (close_time_string);
			} catch (const std::exception& e) {
				LOG(WARNING) << "Caught exception parsing close time \"" << close_time_string << "\": " << e.what();
			}
			if (open_time->is_not_a_date_time() || close_time->is_not_a_date_time()) {
				LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on malformed header \"" << token << "\".";
				return false;
			}
			state = STATE_HEADER;
		} else if (STATE_HEADER == state) {
/* Sector  Buzz    Sentiment...                        */
			chromium::SplitString (t.token(), '\t', columns);
			if (columns->empty()) {
				LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on malformed table header \"" << t.token() << "\".";
				return false;
			}
			if (columns->size() < 2) {
				LOG(WARNING) << "Aborted HTTP transfer " << connection->url << " on malformed table header \"" << t.token() << "\".";
				return false;
			}
			state = STATE_ROW;
		} else if (STATE_ROW == state) {
/* 1679    0.00131 0.00131...                          */
			if (t.token().size() > 0 && '#' == t.token()[0]) {
				state = STATE_FIN;
				continue;
			}
			std::vector<std::string> row_text;
			chromium::SplitString (t.token(), '\t', &row_text);
			if (row_text.size() != columns->size()) {
				LOG(WARNING) << "Partial HTTP transfer " << connection->url << " on malformed table data \"" << t.token() << "\".";
				continue;
			}
/* C++98 does not define infinity or NaN for text streams:
 * http://www.boost.org/doc/libs/1_47_0/libs/math/doc/sf_and_dist/html/math_toolkit/utils/fp_facets/intro.html
 */
			std::locale new_locale(std::locale(std::locale(std::locale(), new boost::math::nonfinite_num_put<char>), new boost::math::nonfinite_num_get<char>));
			std::vector<double> row_double;
			for (size_t i = 1; i < row_text.size(); ++i) {
				std::stringstream ss;
				ss.imbue (new_locale);
				ss << row_text[i];
				double f;
				ss >> f;
#if 0
/* std::strtod()
 * If the converted value falls out of range of corresponding return type, range error occurs and HUGE_VAL is returned.
 */
				double f = std::strtod (row_text[i].c_str(), nullptr);
				if (HUGE_VAL == f || -HUGE_VAL == f) {
					LOG(WARNING) << "Partial HTTP transfer " << connection->url << " on overflow: { "
						  "\"overflow\": \"" << ((HUGE_VAL == f) ? "HUGE_VAL" : "-HUGE_VAL") << "\""
						", \"row\": " << (1 + row_double.size()) <<
						", \"column\": \"" << (*columns)[i] << "\""
						", \"text\": \"" << row_text[i] << "\""
						" }";
/* normalize to limits */
#ifdef max
#	undef max
#endif
					if (HUGE_VAL == f)
						f = std::numeric_limits<double>::max();
					else if (-HUGE_VAL == f)
						f = std::numeric_limits<double>::lowest();
				}
#endif
				row_double.emplace_back (f);
			}
			rows->emplace_back (std::make_pair (row_text[0], row_double));
		} else if (STATE_FIN == state) {
			break;
		}
	}

/* MarketPsych timestamp */
	long psych_offset = 0L;
	if (!close_time->is_not_a_date_time()) {
		cumulative_stats_[PSYCH_PC_PSYCH_CLOCK_DRIFT] = psych_offset = (*close_time - connection->request_ptime).total_seconds();
	}

	VLOG(3) << "Parsing complete.";

	if (LOG_IS_ON(INFO)) {
		using namespace boost::posix_time;
		const ptime file_ptime = from_time_t (filetime);
		LOG(INFO) << "Timing: { "
			  "\"httpd_offset\": " << httpd_offset <<
			", \"http_offset\": " << http_offset <<
			", \"psych_offset\": " << psych_offset <<
			", \"request_time\": \"" << to_simple_string (connection->request_ptime) << "\""
			", \"httpd_time\": \"" << to_simple_string (connection->httpd_ptime) << "\""
			", \"filetime\": \"" << to_simple_string (file_ptime) << "\""
			", \"open\": \"" << to_simple_string (*open_time) << "\""
			", \"close\": \"" << to_simple_string (*close_time) << "\""
			" }";
	}

/* dump decoded time details */
	if (VLOG_IS_ON(4)) {
		using namespace boost::posix_time;
		const ptime file_ptime = from_time_t (filetime);
		VLOG(4) << "Timing: { "
			  "\"request_time\": \"" << to_simple_string (connection->request_ptime) << "\""
			", \"httpd_time\": \"" << to_simple_string (connection->httpd_ptime) << "\""
			", \"filetime\": \"" << to_simple_string (file_ptime) << "\""
			", \"open\": \"" << to_simple_string (*open_time) << "\""
			", \"close\": \"" << to_simple_string (*close_time) << "\""
			" }";
	}

	return true;
}

bool
psych::psych_t::sendRefresh (
	const psych::resource_t& resource,
	const std::string& engine_version,
	const boost::posix_time::ptime& open_time,
	const boost::posix_time::ptime& close_time,
	const std::vector<std::string>& columns,
	const std::vector<std::pair<std::string, std::vector<double>>>& rows
	)
{
/* 7.5.9.1 Create a response message (4.2.2) */
	rfa::message::RespMsg response (false);	/* reference */

/* 7.5.9.2 Set the message model type of the response. */
	response.setMsgModelType (rfa::rdm::MMT_MARKET_PRICE);
/* 7.5.9.3 Set response type. */
	response.setRespType (rfa::message::RespMsg::RefreshEnum);
	response.setIndicationMask (rfa::message::RespMsg::RefreshCompleteFlag);
/* 7.5.9.4 Set the response type enumation. */
	response.setRespTypeNum (rfa::rdm::REFRESH_UNSOLICITED);

/* 7.5.9.5 Create or re-use a request attribute object (4.2.4) */
	rfa::message::AttribInfo attribInfo (false);	/* reference */
	attribInfo.setNameType (rfa::rdm::INSTRUMENT_NAME_RIC);
	RFA_String service_name (config_.service_name.c_str(), 0, false);	/* reference */
	attribInfo.setServiceName (service_name);
	response.setAttribInfo (attribInfo);

/* 6.2.8 Quality of Service. */
	rfa::common::QualityOfService QoS;
/* Timeliness: age of data, either real-time, unspecified delayed timeliness,
 * unspecified timeliness, or any positive number representing the actual
 * delay in seconds.
 */
	QoS.setTimeliness (rfa::common::QualityOfService::realTime);
/* Rate: minimum period of change in data, either tick-by-tick, just-in-time
 * filtered rate, unspecified rate, or any positive number representing the
 * actual rate in milliseconds.
 */
	QoS.setRate (rfa::common::QualityOfService::tickByTick);
	response.setQualityOfService (QoS);

/* 4.3.1 RespMsg.Payload */
// not std::map :(  derived from rfa::common::Data
	fields_.setAssociatedMetaInfo (provider_->getRwfMajorVersion(), provider_->getRwfMinorVersion());
	fields_.setInfo (kDictionaryId, kFieldListId);

/* DataBuffer based fields must be pre-encoded and post-bound. */
	rfa::data::FieldListWriteIterator it;
	rfa::data::FieldEntry stock_ric_field (false), sf_name_field (false), timestamp_field (false), price_field (false), engine_field (false);
	rfa::data::DataBuffer stock_ric_data (false), sf_name_data (false), timestamp_data (false), price_data (false), engine_data (false);
	rfa::data::Real64 real64;
	struct tm _tm;

/* STOCK_RIC
 */
	stock_ric_field.setFieldID (kRdmStockRicId);

/* SF_NAME
 */
	sf_name_field.setFieldID (kRdmSourceFeedNameId);
	const RFA_String sf_name (resource.source.c_str(), 0, false);
	sf_name_data.setFromString (sf_name, rfa::data::DataBuffer::StringRMTESEnum);
	sf_name_field.setData (sf_name_data);
	VLOG(3) << "source feed name: " << resource.source;

/* ENGINE_VER
 */
	engine_field.setFieldID (kRdmEngineVersionId);
	const RFA_String engine (engine_version.c_str(), 0, false);
	engine_data.setFromString (engine, rfa::data::DataBuffer::StringRMTESEnum);
	engine_field.setData (engine_data);
	VLOG(3) << "engine version: " << engine_version;

/* TIMESTAMP: ISO 8601 format, UTC: YYYY-MM-DD hh:mm:ss.sss
 */
	using namespace boost::posix_time;
	using namespace boost::gregorian;
	timestamp_field.setFieldID (kRdmTimestampId);
	std::ostringstream ss;
	ss << std::setfill ('0')
	   << std::setw (4) << (int)close_time.date().year()
	   << '-'
	   << std::setw (2) << (int)close_time.date().month()
	   << '-'
	   << std::setw (2) << (int)close_time.date().day()
	   << ' '
	   << std::setw (2) << (int)close_time.time_of_day().hours()
	   << ':'
	   << std::setw (2) << (int)close_time.time_of_day().minutes()
	   << ':'
	   << std::setw (2) << (int)close_time.time_of_day().seconds()
	   << ".000";
/* stringstream to RFA_String requires a copy */
	const RFA_String timestamp (ss.str().c_str(), 0, true);
	timestamp_data.setFromString (timestamp, rfa::data::DataBuffer::StringRMTESEnum);
	timestamp_field.setData (timestamp_data);
	VLOG(3) << "timestamp: " << ss.str();

/* HIGH_1, LOW_1 as PRICE field type */
	real64.setMagnitudeType (rfa::data::ExponentNeg6);
	price_data.setReal64 (real64);
	price_field.setData (price_data);

	rfa::common::RespStatus status;
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
	status.setStreamState (rfa::common::RespStatus::OpenEnum);
/* Data quality state: Ok, Suspect, or Unspecified. */
	status.setDataState (rfa::common::RespStatus::OkEnum);
/* Error code, e.g. NotFound, InvalidArgument, ... */
	status.setStatusCode (rfa::common::RespStatus::NoneEnum);
	response.setRespStatus (status);

	auto& name_map = query_vector_[resource];
	std::for_each (rows.begin(), rows.end(), [&](std::pair<std::string, std::vector<double>> row)
	{
/* row may not exist in map */
		auto jt = name_map.find (row.first);
		if (name_map.end() == jt) {
			VLOG(3) << "Unmapped row \"" << row.first << "\".";
			return;
		}
		auto& stream = jt->second.second;

		VLOG(2) << "Publishing to stream " << stream->rfa_name;
		attribInfo.setName (stream->rfa_name);
		it.start (fields_);
/* STOCK_RIC */
		const RFA_String stock_ric (jt->second.first.c_str(), 0, false);
		stock_ric_data.setFromString (stock_ric, rfa::data::DataBuffer::StringAsciiEnum);
		stock_ric_field.setData (stock_ric_data);
		it.bind (stock_ric_field);
/* SF_NAME */
		it.bind (sf_name_field);
/* ENGINE_VER */
		it.bind (engine_field);
/* TIMESTAMP */
		it.bind (timestamp_field);

/* map each column data to a TREP-RT FID */
		size_t column_idx = 0;
		std::for_each (columns.begin(), columns.end(), [&](std::string column)
		{
			const auto kt = resource.fields.find (column);
			if (resource.fields.end() == kt) {
				VLOG(3) << "Unmapped column \"" << column << "\".";
				return;
			}
			price_field.setFieldID (kt->second);
			if (boost::math::isnan (row.second[column_idx])) {
				price_data.setBlankData (rfa::data::DataBuffer::Real64Enum);
				price_field.setData (price_data);
				VLOG(4) << column << "(" << kt->second << "): <blank>";
			} else {
				real64.setValue (marketpsych::mantissa (row.second[column_idx]));
				price_data.setReal64 (real64);
				price_field.setData (price_data);
				VLOG(4) << column << "(" << kt->second << "): " << row.second[column_idx];
			}
			it.bind (price_field);
			++column_idx;
		});

		it.complete();
		response.setPayload (fields_);

/* Add "DACS lock", i.e. permissioning data to item stream.
 * Message manifest & buffer are not copied and must survive scope till delivery.
 */
		rfa::common::Buffer buf;
		auto manifest (response.getManifest());
		if (!config_.dacs_id.empty()) {
			rfa::common::RFA_Vector<unsigned long> peList (1);
			peList.push_back (resource.entitlement_code);
			if (generatePELock (&buf, peList)) {
				manifest.setPermissionData (buf);
				response.setManifest (manifest);
			}
		}

#ifdef DEBUG
/* 4.2.8 Message Validation.  RFA provides an interface to verify that
 * constructed messages of these types conform to the Reuters Domain
 * Models as specified in RFA API 7 RDM Usage Guide.
 */
		RFA_String warningText;
		const uint8_t validation_status = response.validateMsg (&warningText);
		if (rfa::message::MsgValidationWarning == validation_status) {
			LOG(ERROR) << "respMsg::validateMsg: { "
				"\"warningText\": \"" << warningText << "\""
				"}";
		} else {
			assert (rfa::message::MsgValidationOk == validation_status);
		}
#endif
		provider_->send (*stream.get(), static_cast<rfa::common::Msg&> (response));
	});

	return true;
}

/* 1.1 Product Description
 * Requirements are transported on the Enterprise Platform in protocol
 * messages called locks. The DACS LOCK API provides functions to manipulate
 * locks in a manner such that the source application need not know any of the
 * details of the encoding scheme or message structure. For a source server to
 * be DACS compliant, based on content, it must publish locks for the items it
 * publishes; i.e., the source server application must produce lock events. Any
 * item published without a lock or with a null lock is available to everybody
 * that is permissioned for that service, even those without any subservice
 * permissions.
 */
bool
psych::psych_t::generatePELock (
	rfa::common::Buffer* buf,
	const rfa::common::RFA_Vector<unsigned long>& peList
	)
{
	assert (!config_.dacs_id.empty());
/* A unique numeric ID assigned to each network service, cannot use service name. */
	const int serviceID = std::atoi (config_.dacs_id.c_str());

	using namespace rfa::dacs;
	AuthorizationLock authLock (serviceID, AuthorizationLock::OR, peList);
	AuthorizationLockData lockData;
	AuthorizationLockStatus retStatus;
	AuthorizationLockData::LockResult result = authLock.getLock (lockData, retStatus);

	if (AuthorizationLockData::LOCK_SUCCESS != result) {
		LOG(ERROR) << "authLock.getLock: { "
			"\"statusText\": \"" << retStatus.getStatusText() << "\""
			" }";
		return false;
	}
	buf->setFrom (lockData.c_lockData(), lockData.size(), lockData.size());
	return true;
}


/* eof */