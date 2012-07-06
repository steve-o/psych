/* RFA context.
 */

#ifndef __RFA_HH__
#define __RFA_HH__
#pragma once

/* Boost noncopyable base class */
#include <boost/utility.hpp>

/* RFA 7.2 */
#include <rfa.hh>

#include "config.hh"
#include "deleter.hh"

namespace psych
{
	namespace internal
	{
// The library version which works with the current version of the headers.
		#define RFA_LIBRARY_VERSION "7.2.0."

	}  // namespace internal

	class rfa_t :
		boost::noncopyable
	{
	public:
		rfa_t (const config_t& config);
		~rfa_t();

		bool init() throw (rfa::common::InvalidUsageException);
		bool VerifyVersion();

	private:

		const config_t& config_;		

/* Live config database */
		std::unique_ptr<rfa::config::ConfigDatabase, internal::release_deleter> rfa_config_;
	};

} /* namespace psych */

#endif /* __RFA_HH__ */

/* eof */
