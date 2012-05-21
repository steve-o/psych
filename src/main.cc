/* MarketPsych feed handler as an application.
 */

#include "psych.hh"

#include <cstdlib>

#include "chromium/command_line.hh"
#include "chromium/logging.hh"
#include "chromium/logging_win.hh"

// {A86E8172-4520-4043-B509-AF75C35326D3}
DEFINE_GUID(kLogProvider, 
0xa86e8172, 0x4520, 0x4043, 0xb5, 0x9, 0xaf, 0x75, 0xc3, 0x53, 0x26, 0xd3);

static
bool
log_handler (
	int			severity,
	const char*		file,
	int			line,
	size_t			message_start,
	const std::string&	str
	)
{
	fprintf (stdout, "%s", str.c_str());
	fflush (stdout);
	return true;
}

class env_t
{
public:
	env_t (int argc, const char* argv[])
	{
/* startup from clean string */
		CommandLine::Init (argc, argv);
/* forward onto logging */
		logging::InitLogging(
			"/psych.log",
#	if 0
			logging::LOG_ONLY_TO_FILE,
#	else
			logging::LOG_NONE,
#	endif
			logging::DONT_LOCK_LOG_FILE,
			logging::APPEND_TO_OLD_LOG_FILE,
			logging::ENABLE_DCHECK_FOR_NON_OFFICIAL_RELEASE_BUILDS
			);
		logging::LogEventProvider::Initialize(kLogProvider);
	}
};

class winsock_t
{
	bool initialized;
public:
	winsock_t (unsigned majorVersion, unsigned minorVersion) :
		initialized (false)
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
		initialized = true;
	}

	~winsock_t ()
	{
		if (initialized)
			WSACleanup();
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

	psych::psych_t psych;
	return psych.run();
}

/* eof */
