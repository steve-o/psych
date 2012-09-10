/* MarketPsych feed handler as an application.
 */

#include "psych.hh"

#include <cstdlib>

#include <windows.h>
#include <mmsystem.h>

#pragma comment (lib, "winmm")

#include "chromium/chromium_switches.hh"
#include "chromium/command_line.hh"
#include "chromium/logging.hh"
#include "chromium/logging_win.hh"

// {A86E8172-4520-4043-B509-AF75C35326D3}
DEFINE_GUID(kLogProvider, 
0xa86e8172, 0x4520, 0x4043, 0xb5, 0x9, 0xaf, 0x75, 0xc3, 0x53, 0x26, 0xd3);

class env_t
{
public:
	env_t (int argc, const char* argv[])
	{
/* startup from clean string */
		CommandLine::Init (argc, argv);
		std::string log_path = GetLogFileName();
/* forward onto logging */
		logging::InitLogging(
			log_path.c_str(),
			DetermineLogMode (*CommandLine::ForCurrentProcess()),
			logging::DONT_LOCK_LOG_FILE,
			logging::APPEND_TO_OLD_LOG_FILE,
			logging::ENABLE_DCHECK_FOR_NON_OFFICIAL_RELEASE_BUILDS
			);
		logging::LogEventProvider::Initialize(kLogProvider);
	}

protected:
	std::string GetLogFileName() {
		const std::string log_filename ("/psych.log");
		return log_filename;
	}

	logging::LoggingDestination DetermineLogMode (const CommandLine& command_line) {
#ifdef NDEBUG
		const logging::LoggingDestination kDefaultLoggingMode = logging::LOG_NONE;
#else
		const logging::LoggingDestination kDefaultLoggingMode = logging::LOG_TO_BOTH_FILE_AND_SYSTEM_DEBUG_LOG;
#endif

		logging::LoggingDestination log_mode;
// Let --enable-logging=file force Vhayu and file logging, particularly useful for
// non-debug builds where otherwise you can't get logs on fault at all.
		if (command_line.GetSwitchValueASCII (switches::kEnableLogging) == "file")
			log_mode = logging::LOG_ONLY_TO_FILE;
		else
			log_mode = kDefaultLoggingMode;
		return log_mode;
	}

	static bool log_handler (int severity, const char* file, int line, size_t message_start, const std::string& str)
	{
		fprintf (stdout, "%s", str.c_str());
		fflush (stdout);
/* allow additional log targets */
		return false;
	}
};

class winsock_t
{
	bool initialized_;
public:
	winsock_t (unsigned majorVersion, unsigned minorVersion) :
		initialized_ (false)
	{
		WORD wVersionRequested = MAKEWORD (majorVersion, minorVersion);
		WSADATA wsaData;
		if (WSAStartup (wVersionRequested, &wsaData) != 0) {
			LOG(ERROR) << "WSAStartup returned " << WSAGetLastError();
			return;
		}
		if (LOBYTE (wsaData.wVersion) != majorVersion || HIBYTE (wsaData.wVersion) != minorVersion) {
			WSACleanup();
			LOG(ERROR) << "WSAStartup failed to provide requested version " << majorVersion << '.' << minorVersion;
			return;
		}
		initialized_ = true;
	}

	~winsock_t ()
	{
		if (initialized_)
			WSACleanup();
	}
};

class timecaps_t
{
	UINT wTimerRes;
public:
	timecaps_t (unsigned resolution_ms) :
		wTimerRes (0)
	{
		TIMECAPS tc;
		if (MMSYSERR_NOERROR == timeGetDevCaps (&tc, sizeof (TIMECAPS))) {
			wTimerRes = min (max (tc.wPeriodMin, resolution_ms), tc.wPeriodMax);
			if (TIMERR_NOCANDO == timeBeginPeriod (wTimerRes)) {
				LOG(WARNING) << "Minimum timer resolution " << wTimerRes << "ms is out of range.";
				wTimerRes = 0;
			}
		} else {
			LOG(WARNING) << "Failed to query timer device resolution.";
		}
	}

	~timecaps_t()
	{
		if (wTimerRes > 0)
			timeEndPeriod (wTimerRes);
	}
};

int
main (
	int		argc,
	const char*	argv[]
	)
{
#ifdef _MSC_VER
/* Suppress abort message. */
	_set_abort_behavior (0, ~0);
#endif

	env_t env (argc, argv);
	winsock_t winsock (2, 2);
	timecaps_t timecaps (1 /* ms */);

	psych::psych_t psych;
	return psych.Run();
}

/* eof */