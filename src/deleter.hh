/* unique_ptr deleters
 */

#ifndef __DELETER_HH__
#define __DELETER_HH__

#pragma once

#include <memory>

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

#endif /* __DELETER_HH__ */

/* eof */
