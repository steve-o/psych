// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "file_util.hh"

#include <windows.h>
#include <propvarutil.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <time.h>

#include <limits>
#include <string>

//#include "base/file_path.h"
//#include "base/logging.h"
//#include "base/metrics/histogram.h"
//#include "base/process_util.h"
//#include "base/string_number_conversions.h"
//#include "base/string_util.h"
//#include "base/threading/thread_restrictions.h"
//#include "base/time.h"
//#include "base/utf_string_conversions.h"
//#include "base/win/pe_image.h"
//#include "base/win/scoped_comptr.h"
//#include "base/win/scoped_handle.h"
//#include "base/win/win_util.h"
//#include "base/win/windows_version.h"

namespace file_util {

FILE* OpenFile(const std::string& filename, const char* mode) {
//  base::ThreadRestrictions::AssertIOAllowed();
  return _fsopen(filename.c_str(), mode, _SH_DENYNO);
}

}  // namespace file_util
