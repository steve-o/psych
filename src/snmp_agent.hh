/* SNMP agent, single session.
 */

#ifndef __SNMP_AGENT_HH__
#define __SNMP_AGENT_HH__
#pragma once

/* Boost noncopyable base class. */
#include <boost/utility.hpp>

/* Boost threading. */
#include <boost/thread.hpp>

#include <winsock2.h>

namespace psych
{
	class psych_t;

	namespace snmp
	{
		class event_pump_t;
	}

	class snmp_agent_t
	{
	public:
		snmp_agent_t (psych_t& psych);
		~snmp_agent_t();

		bool Run();
		void Clear();

	private:
		psych_t& psych_;

/* SNMP event pump and thread. */
		std::unique_ptr<snmp::event_pump_t> event_pump_;
		std::unique_ptr<boost::thread> thread_;

/* Shutdown notification socket. */
		SOCKET s_[2];

/* Only one session. */
		static LONG volatile ref_count_;
	};

} /* namespace psych */

#endif /* __SNMP_AGENT_HH__ */

/* eof */