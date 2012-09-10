/* SNMP agent, single session.
 */

#include "snmp_agent.hh"

/* redirect namespace pollution */
#define U64 __netsnmp_U64

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

/* revert Net-snmp namespace pollution */
#undef U64
#undef LOG_INFO
#undef LOG_WARNING

#include "chromium/logging.hh"
#include "psych.hh"
#include "psychMIB.hh"

/* Net-SNMP requires application name for logging and optional configuration. */
static const char* kSnmpApplicationName = "psych";

/* Atomic reference count for single threaded initialisation. */
LONG volatile psych::snmp_agent_t::ref_count_ = 0;

class psych::snmp::event_pump_t
{
public:
/* A socket to break the SNMP message pump. */
	event_pump_t (SOCKET s_[2])
	{
		s[0] = s_[0];
		s[1] = s_[1];
	}

	void Run()
	{
		assert (s[0] != INVALID_SOCKET);

		LOG(INFO) << "Entering SNMP message pump.";
		for (;;)
		{
			int fds = 0, block = 1;
			fd_set fdset;
			struct timeval timeout;

			FD_ZERO(&fdset);
			::snmp_select_info (&fds, &fdset, &timeout, &block);
			FD_SET(s[0], &fdset);
/* WinSock interpretation is count of descriptors, not the highest. */
			DVLOG(3) << "snmp select";
			fds = ::select (0, &fdset, NULL, NULL, block ? NULL : &timeout);
			if (fds) {
				if (FD_ISSET(s[0], &fdset)) {
					LOG(INFO) << "SNMP exit signaled";
					break;
				}
				DVLOG(3) << "snmp_read";
				::snmp_read (&fdset);
			} else {
				DVLOG(3) << "snmp_timeout";
				::snmp_timeout();
			}
		}
		LOG(INFO) << "Leaving SNMP message pump.";
	}

private:
	SOCKET s[2];
};

psych::snmp_agent_t::snmp_agent_t (psych::psych_t& psych) :
	psych_ (psych)
/* Awaiting C++11 full support */
//	s_ ({ INVALID_SOCKET, INVALID_SOCKET }),
{
	s_[0] = s_[1] = INVALID_SOCKET;
	Run();
}

psych::snmp_agent_t::~snmp_agent_t (void)
{
	Clear();
}

bool
psych::snmp_agent_t::Run (void)
{
/* Instance already running. */
	if (InterlockedExchangeAdd (&ref_count_, 1L) > 0)
		return true;

/* Sub-agent connects to a master agent, otherwise become oneself a master agent. */
	if (psych_.config_.is_agentx_subagent)
	{
		LOG(INFO) << "Configuring as SNMP AgentX sub-agent.";
		if (!psych_.config_.agentx_socket.empty())
		{
			LOG(INFO) << "Using AgentX socket " << psych_.config_.agentx_socket << '.';
			::netsnmp_ds_set_string (NETSNMP_DS_APPLICATION_ID,
						 NETSNMP_DS_AGENT_X_SOCKET,
						 psych_.config_.agentx_socket.c_str());
		}
		::netsnmp_ds_set_boolean (NETSNMP_DS_APPLICATION_ID,
					  NETSNMP_DS_AGENT_ROLE,
					  TRUE);
	}

/* SNMP file logging offers additional error detail, especially with >= Net-SNMP 5.7. */
	if (!psych_.config_.snmp_filelog.empty()) {
		LOG(INFO) << "Setting Net-SNMP filelog to \"" << psych_.config_.snmp_filelog << "\"";
		::snmp_enable_filelog (psych_.config_.snmp_filelog.c_str(), 0);
	}

	LOG(INFO) << "Initialising SNMP agent.";
	if (0 != ::init_agent (kSnmpApplicationName)) {
		LOG(ERROR) << "Initialise SNMP agent: see SNMP log for further details.";
		return false;
	}

/* MIB tables and respective handlers. */
	if (!init_psychMIB ())
		return false;

/* read config and parse mib */
	LOG(INFO) << "Initialising SNMP.";
	::init_snmp (kSnmpApplicationName);

	if (!psych_.config_.is_agentx_subagent)
	{
		LOG(INFO) << "Connecting to SNMP master agent.";
		if (0 != ::init_master_agent ()) {
			LOG(ERROR) << "Initialise SNMP master agent: see SNMP log for further details.";
			return false;
		}
	}

/* create notification channel */
/* use loopback sockets to simulate a pipe suitable for win32/select() */
        struct sockaddr_in addr;
        SOCKET listener;
        int sockerr;
        int addrlen = sizeof (addr);
        unsigned long one = 1;

        s_[0] = s_[1] = INVALID_SOCKET;

        listener = socket (AF_INET, SOCK_STREAM, 0);
        assert (listener != INVALID_SOCKET);

	ZeroMemory (&addr, sizeof (addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr ("127.0.0.1");
        assert (addr.sin_addr.s_addr != INADDR_NONE);

        sockerr = ::bind (listener, (const struct sockaddr*)&addr, sizeof (addr));
        assert (sockerr != SOCKET_ERROR);

        sockerr = getsockname (listener, (struct sockaddr*)&addr, &addrlen);
        assert (sockerr != SOCKET_ERROR);

// Listen for incoming connections.
        sockerr = listen (listener, 1);
        assert (sockerr != SOCKET_ERROR);

// Create the socket.
        s_[1] = WSASocket (AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
        assert (s_[1] != INVALID_SOCKET);

// Connect to the remote peer.
        sockerr = connect (s_[1], (struct sockaddr*)&addr, addrlen);
/* Failure may be delayed from bind and may be due to socket exhaustion as explained
 * in MSDN(bind Function).
 */
        assert (sockerr != SOCKET_ERROR);

// Accept connection.
        s_[0] = accept (listener, NULL, NULL);
        assert (s_[0] != INVALID_SOCKET);

// Set read-end to non-blocking mode
        sockerr = ioctlsocket (s_[0], FIONBIO, &one);
        assert (sockerr != SOCKET_ERROR);

// We don't need the listening socket anymore. Close it.
        sockerr = closesocket (listener);
        assert (sockerr != SOCKET_ERROR);

/* spawn thread to handle SNMP requests */
	LOG(INFO) << "Spawning SNMP thread.";
	event_pump_.reset (new snmp::event_pump_t (s_));
	if (!(bool)event_pump_)
		return false;
	thread_.reset (new boost::thread ([this](){ event_pump_->Run(); }));
	if (!(bool)thread_)
		return false;
	LOG(INFO) << "SNMP init complete.";
	return true;
}

/* Terminate thread and free resources.
 */

void
psych::snmp_agent_t::Clear (void)
{
	LOG(INFO) << "clear()";
	if (0 == ref_count_)
		return;
	if (1 != InterlockedExchangeAdd (&ref_count_, -1L))
		return;
	if (s_[1] != INVALID_SOCKET) {
		const char one = '1';
		LOG(INFO) << "Signalling SNMP exit";
		send (s_[1], &one, sizeof (one), 0);
	}
	if ((bool)thread_ && thread_->joinable())
		thread_->join();
	if (s_[0] != INVALID_SOCKET)
		closesocket (s_[0]);
	if (s_[1] != INVALID_SOCKET)
		closesocket (s_[1]);
	s_[0] = s_[1] = INVALID_SOCKET;
	::snmp_shutdown (kSnmpApplicationName);
	LOG(INFO) << "SNMP shutdown.";
}

/* eof */