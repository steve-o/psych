/* RFA log system.
 *
 * The RFA Developers Guide, Chapter 9 Logger Package defines the minimum requirements
 * for performing logging.
 */

#include "rfa_logging.hh"

#include <cassert>

#include "chromium/logging.hh"
#include "rfa.hh"
#include "rfaostream.hh"

using rfa::common::RFA_String;

/* 9.2.1 Configuring the Logger Package
 * The config database is specified by the context name which must be "RFA".
 */
static const RFA_String kContextName ("RFA");

logging::rfa::LogEventProvider::LogEventProvider (
	const psych::config_t& config,
	std::shared_ptr<::rfa::common::EventQueue> event_queue
	) :
	config_ (config),
	event_queue_ (event_queue),
	handle_ (nullptr)
{
}

/* 9.3.5 RFA Application Logger Shutdown
 * Application Logger Monitors must be destroyed in the reverse order of creation.
 */
logging::rfa::LogEventProvider::~LogEventProvider()
{
	Unregister();
}

/* 9.2.2 Logging in Applications
 * We are not sending logs to RFA so we do not initialize the component logger.  We
 * setup enough to redirect to the "Starter Common" logging utility.
 *
 * Returns true on success.
 */
bool
logging::rfa::LogEventProvider::Register()
{
	VLOG(2) << "Registering RFA log event provider.";
/* 9.2.3.1 Initialize the Application logger.
 * The config database may be shared between other components, implying some form of
 * reference counting.
 */
	logger_.reset (::rfa::logger::ApplicationLogger::acquire (kContextName));
	if (!(bool)logger_)
		return false;

/* 9.2.4.2 Initialize Application Logger Monitor. */
	VLOG(3) << "Creating RFA application logger monitor.";
	const RFA_String monitorName(config_.monitor_name.c_str(), 0, false);
	monitor_.reset (logger_->createApplicationLoggerMonitor (monitorName, false /* no completion events */));
	if (!(bool)monitor_)
		return false;

/* 9.2.4.3 Registering for Log Events.
 * Setting minimum severity to "Success" is defined as everything.
 */
	VLOG(3) << "Registering RFA logger client.";
	::rfa::logger::AppLoggerInterestSpec log_spec;
	log_spec.setMinSeverity (::rfa::common::Success);
	handle_ = monitor_->registerLoggerClient (*event_queue_.get(), log_spec, *this, nullptr /* unused closure */);
	if (nullptr == handle_)
		return false;

	return true;
}

/* Unregister RFA log event consumer.
 *
 * Returns true on success.
 */
bool
logging::rfa::LogEventProvider::Unregister()
{
	VLOG(2) << "Unregistering RFA log event provider.";
/* 9.2.4.4 Closing an Event Stream for the Application Logger Monitor. */
	if (nullptr != handle_)
		monitor_->unregisterLoggerClient (handle_), handle_ = nullptr;
	monitor_.reset();
/* 9.2.3.2 Shutting down the application logger. */
	logger_.reset();
	return true;
}

/* RFA event callback from RFA logging system.
 */
void
logging::rfa::LogEventProvider::processEvent (
	const ::rfa::common::Event& event_
	)
{
	switch (event_.getType()) {
	case ::rfa::logger::LoggerNotifyEventEnum:
		processLoggerNotifyEvent (static_cast<const ::rfa::logger::LoggerNotifyEvent&> (event_));
		break;

        default: break;
        }
}

std::ostream& operator<< (std::ostream& o, const ::rfa::logger::LoggerNotifyEvent& event_) {
	static const char* severity[] = {
		"Success",
		"Information",
		"Warning",
		"Error"
	};
	o << "LoggerNotifyEvent: { "
		  "\"Severity\": \"" << severity[ event_.getSeverity() ] << "\""
		", \"Component\": \"" << event_.getComponentName() << "\""
		", \"LogId\": \"" << event_.getLogID() << "\""
		", \"Text\": \"" << event_.getMessageText() << "\""
		" }";
	return o;
}

/* RFA log event callback.
 */
void
logging::rfa::LogEventProvider::processLoggerNotifyEvent (
	const ::rfa::logger::LoggerNotifyEvent& event_
	)
{
	LOG(INFO) << event_;
}

/* eof */
