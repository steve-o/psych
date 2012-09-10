/* RFA error diagnosis helpers..
 */

#include "error.hh"

/* RFA 7.2 headers */
#include <rfa/rfa.hh>

const char*
internal::severity_string (
	const int severity_
	)
{
	const char* c;

	switch (severity_) {
	case rfa::common::Exception::Error:		c = "Error"; break;
	case rfa::common::Exception::Warning:		c = "Warning"; break;
	case rfa::common::Exception::Information:	c = "Information"; break;
	default: c = "(Unknown)"; break;
	}

	return c;
}

const char*
internal::classification_string (
	const int classification_
	)
{
	const char* c;

	switch (classification_) {
	case rfa::common::Exception::Anticipated:	c = "Anticipated"; break;
	case rfa::common::Exception::Internal:		c = "Internal"; break;
	case rfa::common::Exception::External:		c = "External"; break;
	case rfa::common::Exception::IncorrectAPIUsage:	c = "IncorrectAPIUsage"; break;
	case rfa::common::Exception::ConfigurationError:c = "ConfigurationError"; break;
	default: c = "(Unknown)"; break;
	}

	return c;
}

/* eof */