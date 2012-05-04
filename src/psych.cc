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

/* Source data:
 * http://pipeline2m.velocity.marketpsychdata.com/feed/minutely/news/
 * http://pipeline2m.velocity.marketpsychdata.com/feed/minutely/socialmedia/
 * trmpmitr/kR39[b$vN4
 */

static const char *urls[] = {
		"http://trmpmitr:kR39[b$vN4@pipeline2m.velocity.marketpsychdata.com/feed/minutely/news/ag_commodities-latest.n.txt",
		"http://trmpmitr:kR39[b$vN4@pipeline2m.velocity.marketpsychdata.com/feed/minutely/news/currencies-latest.n.txt",
		"http://trmpmitr:kR39[b$vN4@pipeline2m.velocity.marketpsychdata.com/feed/minutely/news/em_commodities-latest.n.txt",
		"http://trmpmitr:kR39[b$vN4@pipeline2m.velocity.marketpsychdata.com/feed/minutely/news/equities-latest.n.txt"
};

struct connection_t {
	ms::unique_handle<CURL*, psych::onepass_traits> handle;
	char error[CURL_ERROR_SIZE];
	std::string data;
};

/* Apache supports HTTP pipelining, 
 * http://www.w3.org/Protocols/HTTP/Performance/Pipeline.html
 */
static const long kEnableHttpPipelining = 0;

/* Default to allow up to 6 connections per host. Experiment and tuning may
 * try other values (greater than 0).  See http://crbug.com/12066.
 */
static const long kMaxSocketsPerHost = 6;

LONG volatile psych::psych_t::curl_ref_count_ = 0;

/* RDM Usage Guide: Section 6.5: Enterprise Platform
 * For future compatibility, the DictionaryId should be set to 1 by providers.
 * The DictionaryId for the RDMFieldDictionary is 1.
 */
static const int kDictionaryId = 1;

/* RDM: Absolutely no idea. */
static const int kFieldListId = 3;

/* RDF direct limit on symbol list entries */
static const unsigned kSymbolListLimit = 150;

/* RDM FIDs. */
static const int kRdmStockRicId		= 1026;
static const int kRdmTimestampId	= 6378;

/* FlexRecord Quote identifier. */
static const uint32_t kQuoteId = 40002;

/* Feed log file FlexRecord name */
static const char* kPsychFlexRecordName = "psych";

/* Tcl exported API. */
static const char* kBasicFunctionName = "psych_query";

/* Default FlexRecord fields. */
static const char* kDefaultBidField = "BidPrice";
static const char* kDefaultAskField = "AskPrice";

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
int
on_trace (
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

static
size_t
on_data (
	char* ptr,
	size_t size,
	size_t nmemb,
	void* userdata
	)
{
	auto connection = static_cast<connection_t*> (userdata);
	assert (nullptr != connection);
	char *effective_url = nullptr;
	curl_easy_getinfo (connection->handle.get(), CURLINFO_EFFECTIVE_URL, &effective_url);
	LOG(INFO) << size << 'x' << nmemb << " for: " << effective_url;
	if ((connection->data.size() + (size * nmemb)) > connection->data.capacity()) {
		LOG(WARNING) << "Aborting long transfer.";
		return 0;
	}
	connection->data.append (ptr, size * nmemb);
	return size * nmemb;
}

static
void
processData (
	connection_t* connection
	)
{
	assert (nullptr != connection);
	CURL* eh = connection->handle.get();
	assert (nullptr != eh);

	long value;
	char *effective_url = nullptr;
	char *content_type = nullptr;

	curl_easy_getinfo (eh, CURLINFO_EFFECTIVE_URL, &effective_url);
	LOG(INFO) << "processing: " << effective_url;
/* ex: 200 */
	curl_easy_getinfo (eh, CURLINFO_RESPONSE_CODE, &value);
	LOG(INFO) << "response code: " << value;

	if (200 != value) {
		LOG(INFO) << "Aborting on response code: " << value;
		return;
	}

/* ex: 2012-May-03 21:19:00 */
	curl_easy_getinfo (eh, CURLINFO_FILETIME, &value);
	boost::posix_time::ptime filetime = boost::posix_time::from_time_t (value);
	LOG(INFO) << "filetime: " << boost::posix_time::to_simple_string (filetime);
/* 7.25.0 Win64: returns -652835029 */
//	curl_easy_getinfo (eh, CURLINFO_TOTAL_TIME, &value);
//	LOG(INFO) << "total time: " << value;
/* 7.25.0 Win64: returns 0 */
//	curl_easy_getinfo (eh, CURLINFO_NAMELOOKUP_TIME, &value);
//	LOG(INFO) << "name resolution time: " << value;
/* 7.25.0 Win64: returns 0 */
//	curl_easy_getinfo (eh, CURLINFO_CONNECT_TIME, &value);
//	LOG(INFO) << "connection time: " << value;
/* 7.25.0 Win64: returns 0 */
//	curl_easy_getinfo (eh, CURLINFO_PRETRANSFER_TIME, &value);
//	LOG(INFO) << "pre-transfer time: " << value;
/* 7.25.0 Win64: returns -927712936 */
//	curl_easy_getinfo (eh, CURLINFO_STARTTRANSFER_TIME, &value);
//	LOG(INFO) << "start-transfer time: " << value;
/* ex: 372 */
	curl_easy_getinfo (eh, CURLINFO_REQUEST_SIZE, &value);
	LOG(INFO) << "request size: " << value;
/* ex: 367 */
	curl_easy_getinfo (eh, CURLINFO_HEADER_SIZE, &value);
	LOG(INFO) << "header size: " << value;
/* ex: text/plain */
	curl_easy_getinfo (eh, CURLINFO_CONTENT_TYPE, &content_type);
	LOG(INFO) << "content type: " << content_type;

	if (0 != strncmp (content_type, "text/plain", strlen ("text/plain"))) {
		LOG(INFO) << "Aborting on content-type: \"" << content_type << "\"";
		return;
	}

/* ex: 0 */
	curl_easy_getinfo (eh, CURLINFO_CONDITION_UNMET, &value);
	LOG(INFO) << "time condition unmet: " << value;

	LOG(INFO) << "content: " << connection->data;

static const int kMinimumSize = 128;

	if (connection->data.size() < kMinimumSize) {
		LOG(INFO) << "Aborting on underflow, " << connection->data.size() << " bytes.";
		return;
	}

static const uint32_t kPsychMagic = 0x30322023;

	const uint32_t magic = *(uint32_t*)connection->data.c_str();
	if (kPsychMagic != magic) {
		LOG(INFO) << "Aborting on magic number mismatch: " << magic;
		return;
	}

	enum { STATE_TIMESTAMP, STATE_HEADER, STATE_ROW, STATE_FIN } state = STATE_TIMESTAMP;
	StringTokenizer t (connection->data, "\n");

/* # 2012-05-02 21:19:00 UTC - 2012-05-03 21:19:00 UTC */
	boost::posix_time::ptime open_time, close_time;
/* Sector  Buzz    Sentiment...                        */
	std::vector<std::string> columns;
/* 1679    0.00131 0.00131...                          */
	std::vector<std::vector<double>> values;
	while (t.GetNext()) {
		if (STATE_TIMESTAMP == state) {
			const auto& token = t.token();
			if (token.size() != strlen ("# 2012-05-02 21:19:00 UTC - 2012-05-03 21:19:00 UTC")) {
				LOG(INFO) << "Aborting on invalid timestamp header.";
				return;
			}
			open_time = boost::posix_time::time_from_string (token.substr (
				strlen ("# "),
				strlen ("2012-05-02 21:19:00")));
			close_time = boost::posix_time::time_from_string (token.substr (
				strlen ("# 2012-05-02 21:19:00 UTC - "),
				strlen ("2012-05-03 21:19:00")));
			if (open_time.is_not_a_date_time() || close_time.is_not_a_date_time()) {
				LOG(INFO) << "Aborting on invalid timestamp header.";
				return;
			}
			LOG(INFO) << "open: " << boost::posix_time::to_simple_string (open_time) << " "
				"close: " << boost::posix_time::to_simple_string (close_time);
			state = STATE_HEADER;
		} else if (STATE_HEADER == state) {
			chromium::SplitString (t.token(), '\t', &columns);
			if (columns.empty()) {
				LOG(INFO) << "Aborting on empty table header.";
				return;
			}
			state = STATE_ROW;
		} else if (STATE_ROW == state) {
			if (t.token().size() > 0 && '#' == t.token()[0]) {
				state = STATE_FIN;
				continue;
			}
			std::vector<std::string> row_text;
			chromium::SplitString (t.token(), '\t', &row_text);
			if (row_text.size() != columns.size()) {
				LOG(INFO) << "Aborting on row column count mismatch.";
				return;
			}
/* std:atof()
 * If the converted value falls out of range of the return type, the return value is undefined.
 */
			std::vector<double> row_double;
			for (size_t i = 0; i < row_text.size(); ++i) {
/* std::strtod()
 * If the converted value falls out of range of corresponding return type, range error occurs and HUGE_VAL, HUGE_VALF or HUGE_VALL is returned.
 */
				double f = std::strtod (row_text[i].c_str(), nullptr);
				if (HUGE_VAL == f || -HUGE_VAL == f) {
					LOG(INFO) << "Aborting on overflow { "
						"overflow: \"" << ((HUGE_VAL == f) ? "HUGE_VAL" : "-HUGE_VAL") << "\", "
						"row: " << (1 + row_double.size()) << ", "
						"column: \"" << columns[i] << "\", "
						"text: \"" << row_text[i] << "\" "
						"}";
					return;
				}
				row_double.push_back (f);
			}
			values.push_back (row_double);
		} else if (STATE_FIN == state) {
			break;
		}
	}

	LOG(INFO) << "Parsing complete.";
}

psych::psych_t::psych_t() :
	is_shutdown_ (false),
	last_activity_ (boost::posix_time::microsec_clock::universal_time()),
	min_tcl_time_ (boost::posix_time::pos_infin),
	max_tcl_time_ (boost::posix_time::neg_infin),
	total_tcl_time_ (boost::posix_time::seconds(0))
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
	curl_merrno = curl_multi_setopt (multipass_.get(), CURLMOPT_PIPELINING, kEnableHttpPipelining);
	LOG_IF(WARNING, CURLM_OK != curl_merrno) << "CURLMOPT_PIPELINING failed: { "
		"code: " << (int)curl_merrno << ", "
		"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";

/* libcurl 7.16.3: maximum amount of simultaneously open connections that libcurl may cache. */
	curl_merrno = curl_multi_setopt (multipass_.get(), CURLMOPT_MAXCONNECTS, kMaxSocketsPerHost);
	LOG_IF(WARNING, CURLM_OK != curl_merrno) << "CURLMOPT_MAXCONNECTS failed: { "
		"code: " << (int)curl_merrno << ", "
		"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";

/* test here */
	{
		std::array<connection_t, _countof (urls)> connections;

static const size_t kReserveCapacity = 16 * 1024;

		LOG(INFO) << "curl start:";
		for (unsigned i = 0; i < connections.size(); ++i)
		{
			LOG(INFO) << "preparing URL " << urls[i];
			connections[i].handle.reset (curl_easy_init());
			if (!(bool)connections[i].handle)
				goto cleanup;
			connections[i].data.reserve (kReserveCapacity);
			CURL* eh = connections[i].handle.get();
			curl_errno = curl_easy_setopt (eh, CURLOPT_WRITEFUNCTION, on_data);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_WRITEFUNCTION failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* closure for callbacks */
			curl_errno = curl_easy_setopt (eh, CURLOPT_WRITEDATA, &connections[i]);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_WRITEDATA failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* closure for information queue */
			curl_errno = curl_easy_setopt (eh, CURLOPT_PRIVATE, &connections[i]);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_PRIVATE failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* do not include header in output */
			curl_errno = curl_easy_setopt (eh, CURLOPT_HEADER, 0L);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_HEADER failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
			curl_errno = curl_easy_setopt (eh, CURLOPT_URL, urls[i]);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_URL failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";

/* Connection timeout: minimum 1s when using system name resolver */
			long timeout_ms = 1 * 1000;
			curl_errno = curl_easy_setopt (eh, CURLOPT_TIMEOUT_MS, timeout_ms);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_TIMEOUT_MS failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";

/* Custom user-agent */
static const char* kHttpUserAgent = "psych-agent/0.0.1";
			curl_errno = curl_easy_setopt (eh, CURLOPT_USERAGENT, kHttpUserAgent);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_USERAGENT failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";

/* Extract file modification time */
			curl_errno = curl_easy_setopt (eh, CURLOPT_FILETIME, (long)1L);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_FILETIME failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";

/* The If-Modified-Since header */
			curl_errno = curl_easy_setopt (eh, CURLOPT_TIMECONDITION, (long)CURL_TIMECOND_IFMODSINCE);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_TIMECONDITION failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";
/* This should be the time in seconds since 1 Jan 1970 GMT as per RFC2616 */
			long timevalue = 0L;
			curl_errno = curl_easy_setopt (eh, CURLOPT_TIMEVALUE, timevalue);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_TIMEVALUE failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";

/* Request encoding: identity, deflate or gzip.
 * "deflate" and "gzip" are the same deflate compression scheme with either adler32 or crc32 checksum.
 * adler32 is a faster checksum.
 */
static const char* kHttpEncoding = "deflate";
			curl_errno = curl_easy_setopt (eh, CURLOPT_ACCEPT_ENCODING, kHttpEncoding);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_ACCEPT_ENCODING failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";

static const char* proxy = "10.65.98.107:8080";
			curl_errno = curl_easy_setopt (eh, CURLOPT_PROXY, proxy);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_PROXY failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";

			curl_errno = curl_easy_setopt (eh, CURLOPT_ERRORBUFFER, connections[i].error);
			LOG_IF(WARNING, CURLE_OK != curl_errno) << "CURLOPT_ERRORBUFFER failed: { "
				"code: " << (int)curl_errno << ", "
				"text: \"" << curl_easy_strerror (curl_errno) << "\" }";

/* debug mode */
//			curl_easy_setopt (eh, CURLOPT_VERBOSE, 1L);
//			curl_easy_setopt (eh, CURLOPT_DEBUGFUNCTION, on_trace);

			curl_merrno = curl_multi_add_handle (multipass_.get(), eh);
			LOG_IF(ERROR, CURLM_OK != curl_merrno) << "curl_multi_add_handle failed: { "
				"code: " << (int)curl_merrno << ", "
				"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";
		}

		int running_handles = 0;
		
		LOG(INFO) << "perform";
		curl_merrno = curl_multi_perform (multipass_.get(), &running_handles);
		LOG_IF(ERROR, CURLM_OK != curl_merrno && CURLM_CALL_MULTI_PERFORM != curl_merrno) << "curl_multi_perform failed: { "
			"code: " << (int)curl_merrno << ", "
			"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";

		while (running_handles > 0)
		{
			LOG(INFO) << "perform";
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

		LOG(INFO) << "curl information.";
		int msgs_in_queue = 0;
		CURLMsg* msg = curl_multi_info_read (multipass_.get(), &msgs_in_queue);
		while (nullptr != msg) {
			LOG(INFO) << "result: { "
				"msg: " << msg->msg << ", "
				"code: " << msg->data.result << ", "
				"text: \"" << curl_easy_strerror (msg->data.result) << "\" }";
			if (CURLMSG_DONE == msg->msg) {
				void* ptr;
				curl_easy_getinfo (msg->easy_handle, CURLINFO_PRIVATE, &ptr);
				processData (static_cast<connection_t*> (ptr));
			}
			msg = curl_multi_info_read (multipass_.get(), &msgs_in_queue);
		}

		LOG(INFO) << "curl cleanup.";
		for (unsigned i = 0; i < _countof (urls); ++i) {
			curl_merrno = curl_multi_remove_handle (multipass_.get(), connections[i].handle.get());
			LOG_IF(ERROR, CURLM_OK != curl_merrno) << "curl_multi_remove_handle failed: { "
				"code: " << (int)curl_merrno << ", "
				"text: \"" << curl_multi_strerror (curl_merrno) << "\" }";

			connections[i].handle.reset();
		}

		LOG(INFO) << "curl fin.";
	}

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
	clear();
	LOG(INFO) << "Runtime summary: {"
		    " tclQueryReceived: " << cumulative_stats_[PSYCH_PC_TCL_QUERY_RECEIVED] <<
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
			retval = tclPsychQuery (cmdInfo, cmdData);
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

/* psych_query <poop>
 */
int
psych::psych_t::tclPsychQuery (
	const vpf::CommandInfo& cmdInfo,
	vpf::TCLCommandData& cmdData
	)
{
	TCLLibPtrs* tclStubsPtr = (TCLLibPtrs*)cmdData.mClientData;
	Tcl_Interp* interp = cmdData.mInterp;		/* Current interpreter. */
	int objc = cmdData.mObjc;			/* Number of arguments. */
	Tcl_Obj** CONST objv = cmdData.mObjv;		/* Argument strings. */

	if (objc < 2 || objc > 5) {
		Tcl_WrongNumArgs (interp, 1, objv, "symbolList ?startTime? ?endTime?");
		return TCL_ERROR;
	}

/* startTime if not provided is market open. */
	__time32_t startTime;
	if (objc >= 3)
		Tcl_GetLongFromObj (interp, objv[2], &startTime);
	else
		startTime = TBPrimitives::GetOpeningTime();

/* endTime if not provided is now. */
	__time32_t endTime;
	if (objc >= 4)
		Tcl_GetLongFromObj (interp, objv[3], &endTime);
	else
		endTime = TBPrimitives::GetCurrentTime();

/* Time must be ascending. */
	if (endTime <= startTime) {
		Tcl_SetResult (interp, "endTime must be after startTime", TCL_STATIC);
		return TCL_ERROR;
	}

	DLOG(INFO) << "startTime=" << startTime << ", endTime=" << endTime;

/* symbolList must be a list object.
 * NB: VA 7.0 does not export Tcl_ListObjGetElements()
 */
	int listLen, result = Tcl_ListObjLength (interp, objv[1], &listLen);
	if (TCL_OK != result)
		return result;
	if (0 == listLen) {
		Tcl_SetResult (interp, "bad symbol list", TCL_STATIC);
		return TCL_ERROR;
	}

	DLOG(INFO) << "symbol list with #" << listLen << " entries";

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

bool
psych::psych_t::sendRefresh()
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
	rfa::data::FieldEntry timeact_field (false), activ_date_field (false), price_field (false);
	rfa::data::DataBuffer timeact_data (false), activ_date_data (false), price_data (false);
	rfa::data::Real64 real64;
	rfa::data::Time rfaTime;
	rfa::data::Date rfaDate;
	struct tm _tm;

/* TIMEACT */
	timeact_field.setFieldID (kRdmTimestampId);
//	_gmtime32_s (&_tm, &end_time32);
	rfaTime.setHour   (_tm.tm_hour);
	rfaTime.setMinute (_tm.tm_min);
	rfaTime.setSecond (_tm.tm_sec);
	rfaTime.setMillisecond (0);
	timeact_data.setTime (rfaTime);
	timeact_field.setData (timeact_data);

/* HIGH_1, LOW_1 as PRICE field type */
	real64.setMagnitudeType (rfa::data::ExponentNeg6);
	price_data.setReal64 (real64);
	price_field.setData (price_data);

/* ACTIV_DATE */
//	activ_date_field.setFieldID (kRdmActiveDateId);
	rfaDate.setDay   (/* rfa(1-31) */ _tm.tm_mday        /* tm(1-31) */);
	rfaDate.setMonth (/* rfa(1-12) */ 1 + _tm.tm_mon     /* tm(0-11) */);
	rfaDate.setYear  (/* rfa(yyyy) */ 1900 + _tm.tm_year /* tm(yyyy-1900 */);
	activ_date_data.setDate (rfaDate);
	activ_date_field.setData (activ_date_data);

	rfa::common::RespStatus status;
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
	status.setStreamState (rfa::common::RespStatus::OpenEnum);
/* Data quality state: Ok, Suspect, or Unspecified. */
	status.setDataState (rfa::common::RespStatus::OkEnum);
/* Error code, e.g. NotFound, InvalidArgument, ... */
	status.setStatusCode (rfa::common::RespStatus::NoneEnum);
	response.setRespStatus (status);

	std::for_each (stream_vector_.begin(), stream_vector_.end(),
		[&](std::shared_ptr<broadcast_stream_t>& stream)
	{
		attribInfo.setName (stream->rfa_name);
		it.start (fields_);
/* TIMACT */
		it.bind (timeact_field);

/* ACTIV_DATE */
		it.bind (activ_date_field);
		it.complete();
		response.setPayload (fields_);

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

/* eof */
