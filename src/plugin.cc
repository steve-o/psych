/* Boilerplate for exporting a data type to the Analytics Engine.
 */

/* VA leaks a dependency upon _MAX_PATH */
#include <cstdlib>

/* Velocity Analytics Plugin Framework */
#include <vpf/vpf.h>

#include "chromium/command_line.hh"
#include "chromium/logging.hh"
#include "psych.hh"

static const char* kPluginType = "psychPlugin";

namespace
{
	class env_t
	{
	public:
		env_t (const char* varname)
		{
/* startup from clean string */
			CommandLine::Init (0, nullptr);
/* the program name */
			std::string command_line (kPluginType);
/* parameters from environment */
			char* buffer;
			size_t numberOfElements;
			const errno_t err = _dupenv_s (&buffer, &numberOfElements, varname);
			if (0 == err && numberOfElements > 0) {
				command_line.append (" ");
				command_line.append (buffer);
				free (buffer);
			}
/* update command line */
			CommandLine::ForCurrentProcess()->ParseFromString (command_line);
/* forward onto logging */
			logging::InitLogging(
				"/psych.log",
#if 0
				logging::LOG_ONLY_TO_FILE,
#else
				logging::LOG_ONLY_TO_VHAYU_LOG,
#endif
				logging::DONT_LOCK_LOG_FILE,
				logging::APPEND_TO_OLD_LOG_FILE,
				logging::ENABLE_DCHECK_FOR_NON_OFFICIAL_RELEASE_BUILDS
				);
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

	class factory_t : public vpf::ObjectFactory
	{
		env_t env;
		winsock_t winsock;
	public:
		factory_t() :
			env ("TR_DEBUG"),
			winsock (2, 2)
		{
			registerType (kPluginType);
		}

/* no API to unregister type. */

		virtual void* newInstance (const char* type)
		{
			assert (0 == strcmp (kPluginType, type));
			return new psych::psych_t();
		}
	};

	static factory_t g_factory_instance;

} /* anonymous namespace */

/* eof */
