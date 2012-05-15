/* A basic Velocity Analytics User-Plugin to exporting a new Tcl command and
 * periodically publishing out to ADH via RFA using RDM/MarketPrice.
 */

#include "psych.hh"

#define __STDC_FORMAT_MACROS
#include <cstdint>
#include <inttypes.h>
#include <array>

/* Boost Posix Time */
#include "boost/date_time/gregorian/gregorian_types.hpp"

#include "chromium/logging.hh"
#include "chromium/string_split.hh"
#include "chromium/string_tokenizer.hh"
#include "snmp_agent.hh"
#include "error.hh"
#include "rfa_logging.hh"
#include "rfaostream.hh"
#include "version.hh"

/* Default to allow up to 6 connections per host. Experiment and tuning may
 * try other values (greater than 0).  See http://crbug.com/12066.
 */
static const long kMaxSocketsPerHost = 6;

/* MarketPsych content magic number. */
#define FOURCC(a,b,c,d) ( (uint32_t) (((d)<<24) | ((c)<<16) | ((b)<<8) | (a)) )
static const uint32_t kPsychMagic (FOURCC ('#', ' ', '2', '0'));

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
static const int kRdmTimestampId	= 6378;

/* FlexRecord Quote identifier. */
static const uint32_t kQuoteId = 40002;

/* Feed log file FlexRecord name */
static const char* kPsychFlexRecordName = "psych";

/* Tcl exported API. */
static const char* kBasicFunctionName = "psych_republish";
static const char* kResetFunctionName = "psych_hard_republish";

/* http://en.wikipedia.org/wiki/Unix_epoch */
static const boost::gregorian::date kUnixEpoch (1970, 1, 1);

LONG volatile psych::psych_t::instance_count_ = 0;

std::list<psych::psych_t*> psych::psych_t::global_list_;
boost::shared_mutex psych::psych_t::global_list_lock_;

using rfa::common::RFA_String;

/* Boney M. defined: round half up the river of Babylon.
 */
static inline
double
round_half_up (double x)
{
	return floor (x + 0.5);
}

/* mantissa of 10E6
 */
static inline
int64_t
psych_mantissa (double x)
{
	return (int64_t) round_half_up (x * 1000000.0);
}

/* round a double value to 6 decimal places using round half up
 */
static inline
double
psych_round (double x)
{
	return (double) psych_mantissa (x) / 1000000.0;
}

static
void
on_timer (
	PTP_CALLBACK_INSTANCE Instance,
	PVOID Context,
	PTP_TIMER Timer
	)
{
	psych::psych_t* psych = static_cast<psych::psych_t*>(Context);
	psych->processTimer (nullptr);
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
		LOG(WARNING) << "Aborting long transfer.";
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
	LOG(INFO) << "{ pluginType: \"" << plugin_type_ << "\""
		", pluginId: \"" << plugin_id_ << "\""
		", instance: " << instance_ <<
		", version: \"" << version_major << '.' << version_minor << '.' << version_build << "\""
		", build: { date: \"" << build_date << "\""
			", time: \"" << build_time << "\""
			", system: \"" << build_system << "\""
			", machine: \"" << build_machine << "\""
			" }"
		" }";

	if (!config_.parseDomElement (vpf_config.getXmlConfigData()))
		goto cleanup;

	LOG(INFO) << config_;

/** libcurl initialisation. **/
	CURLcode curl_errno;
/* initialise everything, not thread-safe */
	if (InterlockedExchangeAdd (&curl_ref_count_, 1L) == 0) {
		curl_errno = curl_global_init (CURL_GLOBAL_ALL);
		if (CURLE_OK != curl_errno) {
			LOG(ERROR) << "curl_global_init failed: { code: " << (int)curl_errno << " }";
			goto cleanup;
		}
	}

/* multi-interface context */
	multipass_.reset (curl_multi_init());
	if (!(bool)multipass_)
		goto cleanup;

	CURLMcode curl_merrno;
/* libcurl 7.16.0: HTTP Pipelining as far as possible. */
	if (!config_.enable_http_pipelining.empty()) {
		const long value = std::atol (config_.enable_http_pipelining.c_str());
		curl_merrno = curl_multi_setopt (multipass_.get(), CURLMOPT_PIPELINING, value);
		LOG_IF(WARNING, CURLM_OK != curl_merrno) << "CURLMOPT_PIPELINING failed: { "
			"code: " << (int)curl_merrno << ", "
			"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";
	}

/* libcurl 7.16.3: maximum amount of simultaneously open connections that libcurl may cache. */
	curl_merrno = curl_multi_setopt (multipass_.get(), CURLMOPT_MAXCONNECTS, kMaxSocketsPerHost);
	LOG_IF(WARNING, CURLM_OK != curl_merrno) << "CURLMOPT_MAXCONNECTS failed: { "
		"code: " << (int)curl_merrno << ", "
		"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";

/** RFA initialisation. **/
	try {
/* RFA context. */
		rfa_.reset (new rfa_t (config_));
		if (!(bool)rfa_ || !rfa_->init())
			goto cleanup;

/* RFA asynchronous event queue. */
		const RFA_String eventQueueName (config_.event_queue_name.c_str(), 0, false);
		event_queue_.reset (rfa::common::EventQueue::create (eventQueueName), std::mem_fun (&rfa::common::EventQueue::destroy));
		if (!(bool)event_queue_)
			goto cleanup;

/* RFA logging. */
		log_.reset (new logging::LogEventProvider (config_, event_queue_));
		if (!(bool)log_ || !log_->Register())
			goto cleanup;

/* RFA provider. */
		provider_.reset (new provider_t (config_, rfa_, event_queue_));
		if (!(bool)provider_ || !provider_->init())
			goto cleanup;

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
			std::map<std::string, std::shared_ptr<broadcast_stream_t>> name_map;
			for (auto jt = it->items.begin();
				it->items.end() != jt;
				++jt)
			{
				auto stream = std::make_shared<broadcast_stream_t> (*it);
				assert ((bool)stream);
				if (!provider_->createItemStream (jt->second.first.c_str(), stream))
					goto cleanup;
				name_map.emplace (std::make_pair (jt->first, stream));
			}
			stream_vector_.emplace (std::make_pair (*it, name_map));
		}

/* Microsoft threadpool timer. */
		timer_.reset (CreateThreadpoolTimer (static_cast<PTP_TIMER_CALLBACK>(on_timer), this /* closure */, nullptr /* env */));
		if (!(bool)timer_)
			goto cleanup;

	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			"Severity: \"" << severity_string (e.getSeverity()) << "\""
			", Classification: \"" << classification_string (e.getClassification()) << "\""
			", StatusText: \"" << e.getStatus().getStatusText() << "\" }";
		goto cleanup;
	} catch (rfa::common::InvalidConfigurationException& e) {
		LOG(ERROR) << "InvalidConfigurationException: { "
			"Severity: \"" << severity_string (e.getSeverity()) << "\""
			", Classification: \"" << classification_string (e.getClassification()) << "\""
			", StatusText: \"" << e.getStatus().getStatusText() << "\""
			", ParameterName: \"" << e.getParameterName() << "\""
			", ParameterValue: \"" << e.getParameterValue() << "\" }";
		goto cleanup;
	}

/* No main loop inside this thread, must spawn new thread for message pump. */
	event_pump_.reset (new event_pump_t (event_queue_));
	if (!(bool)event_pump_)
		goto cleanup;

	thread_.reset (new boost::thread (*event_pump_.get()));
	if (!(bool)thread_)
		goto cleanup;

/* Spawn SNMP implant. */
	if (config_.is_snmp_enabled) {
		snmp_agent_.reset (new snmp_agent_t (*this));
		if (!(bool)snmp_agent_)
			goto cleanup;
	}

/* Register Tcl API. */
	registerCommand (getId(), kBasicFunctionName);
	LOG(INFO) << "Registered Tcl API \"" << kBasicFunctionName << "\"";
	registerCommand (getId(), kResetFunctionName);
	LOG(INFO) << "Registered Tcl API \"" << kResetFunctionName << "\"";

/* Timer for periodic publishing.
 */
	FILETIME due_time;
	if (!get_next_interval (&due_time)) {
		LOG(ERROR) << "Cannot calculate next interval.";
		goto cleanup;
	}
	const DWORD timer_period = std::stoul (config_.interval) * 1000;
#if 1
	SetThreadpoolTimer (timer_.get(), &due_time, timer_period, 0);
	LOG(INFO) << "Added periodic timer, interval " << timer_period << "ms";
#else
/* requires Platform SDK 7.1 */
	typedef BOOL (WINAPI *SetWaitableTimerExProc)(
		__in  HANDLE hTimer,
		__in  const LARGE_INTEGER *lpDueTime,
		__in  LONG lPeriod,
		__in  PTIMERAPCROUTINE pfnCompletionRoutine,
		__in  LPVOID lpArgToCompletionRoutine,
		__in  PREASON_CONTEXT WakeContext,
		__in  ULONG TolerableDelay
	);
	SetWaitableTimerExProc pFnSetWaitableTimerEx = nullptr;
	ULONG tolerance = std::stoul (config_.tolerable_delay);
	REASON_CONTEXT reasonContext = {0};
	reasonContext.Version = 0;
	reasonContext.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
	reasonContext.Reason.SimpleReasonString = L"psychTimer";
	HMODULE hKernel32Module = GetModuleHandle (_T("kernel32.dll"));
	BOOL timer_status = false;
	if (nullptr != hKernel32Module)
		pFnSetWaitableTimerEx = (SetWaitableTimerExProc) ::GetProcAddress (hKernel32Module, "SetWaitableTimerEx");
	if (nullptr != pFnSetWaitableTimerEx)
		timer_status = pFnSetWaitableTimerEx (timer_.get(), &due_time, timer_period, nullptr, nullptr, &reasonContext, tolerance);
	if (timer_status) {
		LOG(INFO) << "Added periodic timer, interval " << timer_period << "ms, tolerance " << tolerance << "ms";
	} else {
		SetThreadpoolTimer (timer_.get(), &due_time, timer_period, 0);
		LOG(INFO) << "Added periodic timer, interval " << timer_period << "ms";
	}
#endif
	LOG(INFO) << "Init complete, awaiting queries.";

	return;
cleanup:
	LOG(INFO) << "Init failed, cleaning up.";
	clear();
	is_shutdown_ = true;
	throw vpf::UserPluginException ("Init failed.");
}

void
psych::psych_t::clear()
{
/* Stop generating new events. */
	if (timer_)
		SetThreadpoolTimer (timer_.get(), nullptr, 0, 0);
	timer_.reset();

/* Close SNMP agent. */
	snmp_agent_.reset();

/* Signal message pump thread to exit. */
	if ((bool)event_queue_)
		event_queue_->deactivate();
/* Drain and close event queue. */
	if ((bool)thread_)
		thread_->join();

/* Release everything with an RFA dependency. */
	thread_.reset();
	event_pump_.reset();
	stream_vector_.clear();
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
/* Unregister Tcl API. */
	deregisterCommand (getId(), kBasicFunctionName);
	LOG(INFO) << "Unregistered Tcl API \"" << kBasicFunctionName << "\"";
	deregisterCommand (getId(), kResetFunctionName);
	LOG(INFO) << "Unregistered Tcl API \"" << kResetFunctionName << "\"";
	clear();
	LOG(INFO) << "Runtime summary: {"
		    " tclQueryReceived: " << cumulative_stats_[PSYCH_PC_TCL_QUERY_RECEIVED] <<
		   ", timerQueryReceived: " << cumulative_stats_[PSYCH_PC_TIMER_QUERY_RECEIVED] <<
		" }";
	LOG(INFO) << "Instance closed.";
	vpf::AbstractUserPlugin::destroy();
}

/* Tcl boilerplate.
 */

#define Tcl_GetLongFromObj \
	(tclStubsPtr->PTcl_GetLongFromObj)	/* 39 */
#define Tcl_GetStringFromObj \
	(tclStubsPtr->PTcl_GetStringFromObj)	/* 41 */
#define Tcl_ListObjAppendElement \
	(tclStubsPtr->PTcl_ListObjAppendElement)/* 44 */
#define Tcl_ListObjIndex \
	(tclStubsPtr->PTcl_ListObjIndex)	/* 46 */
#define Tcl_ListObjLength \
	(tclStubsPtr->PTcl_ListObjLength)	/* 47 */
#define Tcl_NewDoubleObj \
	(tclStubsPtr->PTcl_NewDoubleObj)	/* 51 */
#define Tcl_NewListObj \
	(tclStubsPtr->PTcl_NewListObj)		/* 53 */
#define Tcl_NewStringObj \
	(tclStubsPtr->PTcl_NewStringObj)	/* 56 */
#define Tcl_SetResult \
	(tclStubsPtr->PTcl_SetResult)		/* 232 */
#define Tcl_SetObjResult \
	(tclStubsPtr->PTcl_SetObjResult)	/* 235 */
#define Tcl_WrongNumArgs \
	(tclStubsPtr->PTcl_WrongNumArgs)	/* 264 */

int
psych::psych_t::execute (
	const vpf::CommandInfo& cmdInfo,
	vpf::TCLCommandData& cmdData
	)
{
	int retval = TCL_ERROR;
	TCLLibPtrs* tclStubsPtr = (TCLLibPtrs*)cmdData.mClientData;
	Tcl_Interp* interp = cmdData.mInterp;		/* Current interpreter. */
	const boost::posix_time::ptime t0 (boost::posix_time::microsec_clock::universal_time());
	last_activity_ = t0;

	cumulative_stats_[PSYCH_PC_TCL_QUERY_RECEIVED]++;

	try {
		const char* command = cmdInfo.getCommandName();
		if (0 == strcmp (command, kBasicFunctionName))
			retval = tclPsychRepublish (cmdInfo, cmdData);
		else if  (0 == strcmp (command, kResetFunctionName))
			retval = tclPsychHardRepublish (cmdInfo, cmdData);
		else
			Tcl_SetResult (interp, "unknown function", TCL_STATIC);
	}
/* FlexRecord exceptions */
	catch (const vpf::PluginFrameworkException& e) {
		/* yay broken Tcl API */
		Tcl_SetResult (interp, (char*)e.what(), TCL_VOLATILE);
	}
	catch (...) {
		Tcl_SetResult (interp, "Unhandled exception", TCL_STATIC);
	}

/* Timing */
	const boost::posix_time::ptime t1 (boost::posix_time::microsec_clock::universal_time());
	const boost::posix_time::time_duration td = t1 - t0;
	DLOG(INFO) << "execute complete" << td.total_milliseconds() << "ms";
	if (td < min_tcl_time_) min_tcl_time_ = td;
	if (td > max_tcl_time_) max_tcl_time_ = td;
	total_tcl_time_ += td;

	return retval;
}

/* psych_republish
 */
int
psych::psych_t::tclPsychRepublish (
	const vpf::CommandInfo& cmdInfo,
	vpf::TCLCommandData& cmdData
	)
{
	TCLLibPtrs* tclStubsPtr = (TCLLibPtrs*)cmdData.mClientData;
	Tcl_Interp* interp = cmdData.mInterp;		/* Current interpreter. */
/* Refresh already running.  Note locking is handled outside query to enable
 * feedback to Tcl interface.
 */
	boost::unique_lock<boost::shared_mutex> lock (query_mutex_, boost::try_to_lock_t());
	if (!lock.owns_lock()) {
		Tcl_SetResult (interp, "query already running", TCL_STATIC);
		return TCL_ERROR;
	}

/* dup connections map from configuration */
	std::map<resource_t, std::shared_ptr<connection_t>, resource_compare_t> connections;
	std::for_each (connections_.begin(), connections_.end(),
		[&connections](std::pair<resource_t, std::shared_ptr<connection_t>> pair)
	{
		auto connection = std::make_shared<connection_t> (pair.first, pair.second->url);
		connections.emplace (std::make_pair (pair.first, connection));
	});

	httpPsychQuery (connections, QUERY_HTTP_KEEPALIVE);
	DVLOG(3) << "query complete.";

	return TCL_OK;
}

/* psych_hard_republish
 */
int
psych::psych_t::tclPsychHardRepublish (
	const vpf::CommandInfo& cmdInfo,
	vpf::TCLCommandData& cmdData
	)
{
	TCLLibPtrs* tclStubsPtr = (TCLLibPtrs*)cmdData.mClientData;
	Tcl_Interp* interp = cmdData.mInterp;		/* Current interpreter. */
	boost::unique_lock<boost::shared_mutex> lock (query_mutex_, boost::try_to_lock_t());
	if (!lock.owns_lock()) {
		Tcl_SetResult (interp, "query already running", TCL_STATIC);
		return TCL_ERROR;
	}

/* dup connections map from configuration */
	std::map<resource_t, std::shared_ptr<connection_t>, resource_compare_t> connections;
	std::for_each (connections_.begin(), connections_.end(),
		[&connections](std::pair<resource_t, std::shared_ptr<connection_t>> pair)
	{
		auto connection = std::make_shared<connection_t> (pair.first, pair.second->url);
		connections.emplace (std::make_pair (pair.first, connection));
	});

	httpPsychQuery (connections, 0);
	DVLOG(3) << "query complete.";

	return TCL_OK;
}

class flexrecord_t {
public:
	flexrecord_t (const __time32_t& timestamp, const char* symbol, const char* record)
	{
		VHTime vhtime;
		struct tm tm_time = { 0 };
		
		VHTimeProcessor::TTTimeToVH ((__time32_t*)&timestamp, &vhtime);
		_gmtime32_s (&tm_time, &timestamp);

		stream_ << std::setfill ('0')
/* 1: timeStamp : t_string : server receipt time, fixed format: YYYYMMDDhhmmss.ttt, e.g. 20120114060928.227 */
			<< std::setw (4) << 1900 + tm_time.tm_year
			<< std::setw (2) << 1 + tm_time.tm_mon
			<< std::setw (2) << tm_time.tm_mday
			<< std::setw (2) << tm_time.tm_hour
			<< std::setw (2) << tm_time.tm_min
			<< std::setw (2) << tm_time.tm_sec
			<< '.'
			<< std::setw (3) << 0
/* 2: eyeCatcher : t_string : @@a */
			<< ",@@a"
/* 3: recordType : t_string : FR */
			   ",FR"
/* 4: symbol : t_string : e.g. MSFT */
			   ","
			<< symbol
/* 5: defName : t_string : FlexRecord name, e.g. Quote */
			<< ',' << record
/* 6: sourceName : t_string : FlexRecord name of base derived record. */
			<< ","
/* 7: sequenceID : t_u64 : Sequence number. */
			   "," << sequence_++
/* 8: exchTimeStamp : t_VHTime : exchange timestamp */
			<< ",V" << vhtime
/* 9: subType : t_s32 : record subtype */
			<< ","
/* 10..497: user-defined data fields */
			   ",";
	}

	std::string str() { return stream_.str(); }
	std::ostream& stream() { return stream_; }
private:
	std::ostringstream stream_;
	static uint64_t sequence_;
};

uint64_t flexrecord_t::sequence_ = 0;

/* http://msdn.microsoft.com/en-us/library/4ey61ayt.aspx */
#define CTIME_LENGTH	26

/* callback from periodic timer.
 */
void
psych::psych_t::processTimer (
	void*	pClosure
	)
{
	cumulative_stats_[PSYCH_PC_TIMER_QUERY_RECEIVED]++;

/* Prevent overlapped queries. */
	boost::unique_lock<boost::shared_mutex> lock (query_mutex_, boost::try_to_lock_t());
	if (!lock.owns_lock()) {
		LOG(WARNING) << "Periodic refresh aborted due to running query.";
		return;
	}

	try {
		httpPsychQuery (connections_, QUERY_HTTP_KEEPALIVE | QUERY_IF_MODIFIED_SINCE);
	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			"Severity: \"" << severity_string (e.getSeverity()) << "\""
			", Classification: \"" << classification_string (e.getClassification()) << "\""
			", StatusText: \"" << e.getStatus().getStatusText() << "\" }";
	}
}

/* Calculate the next bin close timestamp for the requested timezone.
 */
bool
psych::psych_t::get_next_interval (
	FILETIME* ft
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

/* shift is difference between 1970-Jan-01 & 1601-Jan-01 in 100-nanosecond intervals.
 */
	const uint64_t shift = 116444736000000000ULL; // (27111902 << 32) + 3577643008

	union {
		FILETIME as_file_time;
		uint64_t as_integer; // 100-nanos since 1601-Jan-01
	} caster;
	caster.as_integer = (next_ptime - ptime (kUnixEpoch)).total_microseconds() * 10; // upconvert to 100-nanos
	caster.as_integer += shift; // now 100-nanos since 1601-Jan-01

	*ft = caster.as_file_time;
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
	pending.reserve (connections_.size());
	std::for_each (connections_.begin(), connections_.end(),
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
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* target resource */
			curl_errno = curl_easy_setopt (eh, CURLOPT_URL, connection->url.c_str());
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_URL failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* incoming response header */
			curl_errno = curl_easy_setopt (eh, CURLOPT_HEADERFUNCTION, on_http_header);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_HEADERFUNCTION failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* incoming response data */
			curl_errno = curl_easy_setopt (eh, CURLOPT_WRITEFUNCTION, on_http_data);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_WRITEFUNCTION failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* closure for callbacks */
			curl_errno = curl_easy_setopt (eh, CURLOPT_WRITEHEADER, connection.get());
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_WRITEHEADER failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			curl_errno = curl_easy_setopt (eh, CURLOPT_WRITEDATA, connection.get());
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_WRITEDATA failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* closure for information queue */
			curl_errno = curl_easy_setopt (eh, CURLOPT_PRIVATE, connection.get());
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_PRIVATE failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* do not include header in output */
			curl_errno = curl_easy_setopt (eh, CURLOPT_HEADER, 0L);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_HEADER failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* Fresh connection for hard-refresh.  Socket is left open for re-use. */
			if (!(flags & QUERY_HTTP_KEEPALIVE)) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_FRESH_CONNECT, 1L);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_FRESH_CONNECT failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			}
/* Connection timeout: minimum 1s when using system name resolver */
			if (!config_.connect_timeout_ms.empty()) {
				const long connect_timeout_ms = std::atol (config_.connect_timeout_ms.c_str());
				curl_errno = curl_easy_setopt (eh, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_CONNECTTIMEOUT_MS failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			}
/* Force IPv4 */
			curl_errno = curl_easy_setopt (eh, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_IPRESOLVE failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* Transfer timeout */
			if (!config_.timeout_ms.empty()) {
				const long timeout_ms = std::atol (config_.timeout_ms.c_str());
				curl_errno = curl_easy_setopt (eh, CURLOPT_TIMEOUT_MS, timeout_ms);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_TIMEOUT_MS failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			}
/* DNS response cache, in seconds. */
			if (!config_.dns_cache_timeout.empty()) {
				const long timeout = std::atol (config_.dns_cache_timeout.c_str());
				curl_errno = curl_easy_setopt (eh, CURLOPT_DNS_CACHE_TIMEOUT, timeout);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_DNS_CACHE_TIMEOUT failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			}
/* Custom user-agent */
			char user_agent[1024];
			sprintf_s (user_agent, sizeof (user_agent), kHttpUserAgent, version_major, version_minor, version_build);
			curl_errno = curl_easy_setopt (eh, CURLOPT_USERAGENT, user_agent);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_USERAGENT failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* Extract file modification time */
			curl_errno = curl_easy_setopt (eh, CURLOPT_FILETIME, (long)1L);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_FILETIME failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* The If-Modified-Since header */
			if (flags & QUERY_IF_MODIFIED_SINCE) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_TIMECONDITION, (long)CURL_TIMECOND_IFMODSINCE);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_TIMECONDITION failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* This should be the time in seconds since 1 Jan 1970 GMT as per RFC2616 */
				const long timevalue = connection->last_filetime;
				curl_errno = curl_easy_setopt (eh, CURLOPT_TIMEVALUE, timevalue);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_TIMEVALUE failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			}
/* Record approximate request timestamp. */
			connection->request_ptime = t0;
			connection->httpd_ptime = boost::posix_time::not_a_date_time;
/* Request encoding: identity, deflate or gzip. */
			if (!config_.request_http_encoding.empty()) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_ACCEPT_ENCODING, config_.request_http_encoding.c_str());
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_ACCEPT_ENCODING failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			}
/* HTTP proxy for internal development */
			if (!config_.http_proxy.empty()) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_PROXY, config_.http_proxy.c_str());
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_PROXY failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			}
/* buffer for text form of error codes */
			curl_errno = curl_easy_setopt (eh, CURLOPT_ERRORBUFFER, connection->error);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_ERRORBUFFER failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* debug mode */
			if (VLOG_IS_ON (10)) {
				curl_errno = curl_easy_setopt (eh, CURLOPT_VERBOSE, 1L);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_VERBOSE failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
				curl_errno = curl_easy_setopt (eh, CURLOPT_DEBUGFUNCTION, on_http_trace);
				LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_DEBUGFUNCTION failed: { "
					"code: " << (int)curl_errno << ", "
					"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			}

			CURLMcode curl_merrno = curl_multi_add_handle (multipass_.get(), eh);
			LOG_IF(ERROR, CURLM_OK != curl_merrno) << "curl_multi_add_handle failed: { "
				"code: " << (int)curl_merrno << ", "
				"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";
		});

		int running_handles = 0;
		
		VLOG(3) << "perform";
		CURLMcode curl_merrno = curl_multi_perform (multipass_.get(), &running_handles);
		LOG_IF(ERROR, CURLM_OK != curl_merrno && CURLM_CALL_MULTI_PERFORM != curl_merrno) << "curl_multi_perform failed: { "
			"code: " << (int)curl_merrno << ", "
			"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";

		cumulative_stats_[PSYCH_PC_HTTP_REQUEST_SENT] += pending.size();

		while (running_handles > 0)
		{
			VLOG(3) << "perform";
			while (CURLM_CALL_MULTI_PERFORM == curl_merrno)
				curl_merrno = curl_multi_perform (multipass_.get(), &running_handles);
			LOG_IF(ERROR, CURLM_OK != curl_merrno) << "curl_multi_perform failed: { "
				"code: " << (int)curl_merrno << ", "
				"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";
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
				"code: " << (int)curl_merrno << ", "
				"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";
			curl_merrno = curl_multi_timeout (multipass_.get(), &timeout);
			LOG_IF(ERROR, CURLM_OK != curl_merrno && CURLM_CALL_MULTI_PERFORM != curl_merrno) << "curl_multi_timeout failed: { "
				"code: " << (int)curl_merrno << ", "
				"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";

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
		ptime open_time, close_time;
		std::vector<std::string> columns;
		std::vector<std::pair<std::string, std::vector<double>>> rows;

		int msgs_in_queue = 0;
		CURLMsg* msg = curl_multi_info_read (multipass_.get(), &msgs_in_queue);
		while (nullptr != msg) {
			VLOG(3) << "result: { "
				"msg: " << msg->msg << ", "
				"code: " << msg->data.result << ", "
				"text: \"" << curl_easy_strerror (msg->data.result) << "\" }";
			if (CURLMSG_DONE == msg->msg) {
				void* ptr;
				curl_easy_getinfo (msg->easy_handle, CURLINFO_PRIVATE, &ptr);
				auto connection = static_cast<connection_t*> (ptr);
				open_time = close_time = not_a_date_time;
				columns.clear(); rows.clear();
				if (processHttpResponse (connection, &open_time, &close_time, &columns, &rows))
				{
					try {
						sendRefresh (connection->resource, open_time, close_time, columns, rows);
					} catch (rfa::common::InvalidUsageException& e) {
						LOG(ERROR) << "InvalidUsageException: { "
							  "Severity: \"" << severity_string (e.getSeverity()) << "\""
							", Classification: \"" << classification_string (e.getClassification()) << "\""
							", StatusText: \"" << e.getStatus().getStatusText() << "\" }";
					}
/* ignoring RFA, request is now considered successful */
					const CURLMcode curl_merrno = curl_multi_remove_handle (multipass_.get(), connection->handle.get());
					LOG_IF(ERROR, CURLM_OK != curl_merrno) << "curl_multi_remove_handle failed: { "
						"code: " << (int)curl_merrno << ", "
						"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";
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
	std::for_each (connections_.begin(), connections_.end(),
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
			  "url: \"" << effective_url << "\""
			", status: " << response_code <<
			", type: \"" << content_type << "\""
			", size: " << size_download <<
			", content: " << connection->data.size() <<
			", time: " << total_time <<
			", latency: " << starttransfer_time <<
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
		LOG(WARNING) << "Aborted HTTP transfer on status code: " << response_code << ".";
		return false;
	}

	cumulative_stats_[PSYCH_PC_HTTP_200_RECEIVED]++;
	cumulative_stats_[PSYCH_PC_HTTP_2XX_RECEIVED]++;

	if (0 != strncmp (content_type, "text/plain", strlen ("text/plain"))) {
		LOG(WARNING) << "Aborted HTTP transfer on content-type: \"" << content_type << "\".";
		return false;
	}

	const long minimum_response_size = !config_.minimum_response_size.empty() ? std::atol (config_.minimum_response_size.c_str()) : sizeof (uint32_t);
	assert (minimum_response_size > sizeof (uint32_t));
	if (connection->data.size() < minimum_response_size) {
		LOG(WARNING) << "Aborted HTTP transfer on content size: " << connection->data.size() << ".";
		return false;
	}

/* inspect payload */
	const char* cdata = connection->data.c_str();
	const uint32_t magic (FOURCC (cdata[0], cdata[1], cdata[2], cdata[3]));
	if (kPsychMagic != magic) {
		LOG(WARNING) << "Aborted HTTP transfer on payload magic number: " << std::hex << std::showbase << magic << ".";
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
				LOG(WARNING) << "Aborted HTTP transfer on filetime clock offset " << http_offset << " seconds breaching panic threshold " << config_.panic_threshold << ".";
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
/* # 2012-05-02 21:19:00 UTC - 2012-05-03 21:19:00 UTC */
			const auto& token = t.token();
			if (token.size() != strlen ("# 2012-05-02 21:19:00 UTC - 2012-05-03 21:19:00 UTC")) {
				LOG(WARNING) << "Aborted HTTP transfer on malformed data.";
				return false;
			}
			*open_time = boost::posix_time::time_from_string (token.substr (
				strlen ("# "),
				strlen ("2012-05-02 21:19:00")));
			*close_time = boost::posix_time::time_from_string (token.substr (
				strlen ("# 2012-05-02 21:19:00 UTC - "),
				strlen ("2012-05-03 21:19:00")));
			if (open_time->is_not_a_date_time() || close_time->is_not_a_date_time()) {
				LOG(WARNING) << "Aborted HTTP transfer on malformed data.";
				return false;
			}
			state = STATE_HEADER;
		} else if (STATE_HEADER == state) {
/* Sector  Buzz    Sentiment...                        */
			chromium::SplitString (t.token(), '\t', columns);
			if (columns->empty()) {
				LOG(WARNING) << "Aborted HTTP transfer on malformed data.";
				return false;
			}
			if (columns->size() < 2) {
				LOG(WARNING) << "Aborted HTTP transfer on malformed data.";
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
				LOG(WARNING) << "Aborted HTTP transfer on malformed data.";
				return false;
			}
/* std:atof()
 * If the converted value falls out of range of the return type, the return value is undefined.
 */
			std::vector<double> row_double;
			for (size_t i = 1; i < row_text.size(); ++i) {
/* std::strtod()
 * If the converted value falls out of range of corresponding return type, range error occurs and HUGE_VAL, HUGE_VALF or HUGE_VALL is returned.
 */
				double f = std::strtod (row_text[i].c_str(), nullptr);
				if (HUGE_VAL == f || -HUGE_VAL == f) {
					LOG(WARNING) << "Aborted HTTP transfer on overflow: { "
						"overflow: \"" << ((HUGE_VAL == f) ? "HUGE_VAL" : "-HUGE_VAL") << "\", "
						"row: " << (1 + row_double.size()) << ", "
						"column: \"" << (*columns)[i] << "\", "
						"text: \"" << row_text[i] << "\" "
						"}";
					return false;
				}
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

{
		using namespace boost::posix_time;
		const ptime file_ptime = from_time_t (filetime);
		LOG(INFO) << "Timing: { "
			  "httpd_offset: " << httpd_offset <<
			", http_offset: " << http_offset <<
			", psych_offset: " << psych_offset <<
			", request_time: \"" << to_simple_string (connection->request_ptime) << "\""
			", httpd_time: \"" << to_simple_string (connection->httpd_ptime) << "\""
			", filetime: \"" << to_simple_string (file_ptime) << "\""
			", open: " << to_simple_string (*open_time) << "\""
			", close: \"" << to_simple_string (*close_time) << "\""
			" }";
}

/* dump decoded time details */
	if (VLOG_IS_ON(4)) {
		using namespace boost::posix_time;
		const ptime file_ptime = from_time_t (filetime);
		VLOG(4) << "Timing: { "
			  "request_time: \"" << to_simple_string (connection->request_ptime) << "\""
			", httpd_time: \"" << to_simple_string (connection->httpd_ptime) << "\""
			", filetime: \"" << to_simple_string (file_ptime) << "\""
			", open: " << to_simple_string (*open_time) << "\""
			", close: \"" << to_simple_string (*close_time) << "\""
			" }";
	}

	return sendRefresh (connection->resource, *open_time, *close_time, *columns, *rows);
}

bool
psych::psych_t::sendRefresh (
	const psych::resource_t& resource,
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
	rfa::data::FieldEntry timestamp_field (false), price_field (false);
	rfa::data::DataBuffer timestamp_data (false), price_data (false);
	rfa::data::Real64 real64;
	struct tm _tm;

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
	const RFA_String rfa_string (ss.str().c_str(), 0, true);
	timestamp_data.setFromString (rfa_string, rfa::data::DataBuffer::StringRMTESEnum);
	timestamp_field.setData (timestamp_data);

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

	auto& name_map = stream_vector_[resource];
	std::for_each (rows.begin(), rows.end(),
		[&](std::pair<std::string, std::vector<double>> row)
	{
/* row may not exist in map */
		auto jt = name_map.find (row.first);
		if (name_map.end() == jt) {
			VLOG(3) << "Unmapped row \"" << row.first << "\".";
			return;
		}
		auto& stream = jt->second;

		VLOG(2) << "Publishing to stream " << stream->rfa_name;

		attribInfo.setName (stream->rfa_name);
		it.start (fields_);
/* TIMESTAMP */
		it.bind (timestamp_field);

/* map each column data to a TREP-RT FID */
		size_t column_idx = 0;
		std::for_each (columns.begin(), columns.end(),
			[&](std::string column)
		{
			const auto kt = resource.fields.find (column);
			if (resource.fields.end() == kt) {
				VLOG(3) << "Unmapped column \"" << column << "\".";
				return;
			}
			price_field.setFieldID (kt->second);
			const int64_t mantissa = psych_mantissa (row.second[column_idx]);
			real64.setValue (mantissa);		
			it.bind (price_field);
			++column_idx;
		});

		it.complete();
		response.setPayload (fields_);

/* Add "DACS lock", i.e. permissioning data to item stream. */
		if (!config_.dacs_id.empty()) {
			rfa::common::RFA_Vector<unsigned long> peList (1);
			peList.push_back (resource.entitlement_code);
			rfa::common::Buffer buf;
			if (generatePELock (&buf, peList)) {
				auto manifest (response.getManifest());
				manifest.setPermissionData (buf);
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
			LOG(ERROR) << "respMsg::validateMsg: { warningText: \"" << warningText << "\" }";
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
		LOG(ERROR) << "authLock.getLock: { statusText: \"" << retStatus.getStatusText() << "\" }";
		return false;
	}
	buf->setFrom (lockData.c_lockData(), lockData.size(), lockData.size());
	return true;
}


/* eof */

