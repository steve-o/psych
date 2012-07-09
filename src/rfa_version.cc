/* RFA version build helper.
 */

#include <cassert>
#include <cstdlib>
#include <memory>
#include <rfa/rfa.hh>

namespace internal {

	struct release_deleter {
		template <class T> void operator()(T* ptr) {
			ptr->release();
		};
	};

	struct destroy_deleter {
		template <class T> void operator()(T* ptr) {
			ptr->destroy();
		};
	};

} /* namespace internal */

using rfa::common::RFA_String;

static const RFA_String kEventLoggerName ("\\Logger\\AppLogger\\windowsLoggerEnabled");
static const RFA_String kFileLoggerName ("\\Logger\\AppLogger\\fileLoggerEnabled");
static const RFA_String kSessionName ("Session1");
static const RFA_String kContextName ("RFA");

class context_t {
public:
	context_t() {
		rfa::common::Context::initialize();
		std::unique_ptr<rfa::config::StagingConfigDatabase, internal::destroy_deleter> staging (rfa::config::StagingConfigDatabase::create());
		assert ((bool)staging);
		staging->setBool (kEventLoggerName, false);
		staging->setBool (kFileLoggerName, false);
		config_.reset (rfa::config::ConfigDatabase::acquire (kContextName));
		assert ((bool)config_);
		config_->merge (*staging.get());
	}
	~context_t() {
		config_.reset();
		rfa::common::Context::uninitialize();
	}
protected:
	std::unique_ptr<rfa::config::ConfigDatabase, internal::release_deleter> config_;
};

int
main (
	int argc,
	char* argv[]
	)
{
	std::shared_ptr<context_t> context;
	std::unique_ptr<rfa::sessionLayer::Session, internal::release_deleter> session;

	try {
		context.reset (new context_t ());
		assert ((bool)context);
		session.reset (rfa::sessionLayer::Session::acquire (kSessionName));
		assert ((bool)session);
	} catch (rfa::common::InvalidUsageException&) {
		printf ("rfa::common::InvalidUsageException\n");
	} catch (rfa::common::InvalidConfigurationException&) {
/* deliberately ignore */
	}

	printf ("%s\n", rfa::common::Context::getRFAVersionInfo()->getProductVersion().c_str());
	return EXIT_SUCCESS;
}

/* eof */
