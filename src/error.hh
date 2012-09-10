/* Error diagnosis helpers.
 */

#ifndef __ERROR_HH__
#define __ERROR_HH__
#pragma once

namespace internal
{

	const char* severity_string (const int severity_);
	const char* classification_string (const int classification_);

} /* namespace internal */

#endif /* __ERROR_HH__ */

/* eof */