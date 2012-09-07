/* Tcl command exports
 */

#include "psych.hh"

#define __STDC_FORMAT_MACROS
#include <cstdint>
#include <inttypes.h>

/* Feed log file FlexRecord name */
static const char* kPsychFlexRecordName = "psych";

/* Tcl exported API. */
static const char* kBasicApi = "psych_republish";
static const char* kResetApi = "psych_hard_republish";

static const char* kTclApi[] = {
	kBasicApi,
	kResetApi
};

#ifndef CONFIG_PSYCH_AS_APPLICATION
/* Register Tcl API.
 */
bool
psych::psych_t::RegisterTclApi (const char* id)
{
	for (size_t i = 0; i < _countof (kTclApi); ++i) {
		registerCommand (id, kTclApi[i]);
		LOG(INFO) << "Registered Tcl API \"" << kTclApi[i] << "\"";
	}
	return true;
}

/* Unregister Tcl API.
 */
bool
psych::psych_t::UnregisterTclApi (const char* id)
{
	for (size_t i = 0; i < _countof (kTclApi); ++i) {
		deregisterCommand (id, kTclApi[i]);
		LOG(INFO) << "Unregistered Tcl API \"" << kTclApi[i] << "\"";
	}
	return true;
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
	TCLLibPtrs* tclStubsPtr = static_cast<TCLLibPtrs*> (cmdData.mClientData);
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
		Tcl_SetResult (interp, const_cast<char*> (e.what()), TCL_VOLATILE);
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
	TCLLibPtrs* tclStubsPtr = static_cast<TCLLibPtrs*> (cmdData.mClientData);
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
		auto dup_connection = std::make_shared<connection_t> (pair.first, pair.second->url);
		connections.emplace (std::make_pair (pair.first, dup_connection));
	});

	httpPsychQuery (connections, QUERY_HTTP_KEEPALIVE);
	DVLOG(3) << "query complete.";

	connections.clear();

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
	TCLLibPtrs* tclStubsPtr = static_cast<TCLLibPtrs*> (cmdData.mClientData);
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
		auto dup_connection = std::make_shared<connection_t> (pair.first, pair.second->url);
		connections.emplace (std::make_pair (pair.first, dup_connection));
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

#endif /* CONFIG_PSYCH_AS_APPLICATION */

/* eof */