// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains utility functions for dealing with the local
// filesystem.

#ifndef CHROMIUM_FILE_UTIL_HH_
#define CHROMIUM_FILE_UTIL_HH_
#pragma once

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <stdio.h>

#include <string>

//#include "base/base_export.h"
//#include "base/basictypes.h"
//#include "base/file_path.h"
//#include "base/memory/scoped_ptr.h"
//#include "base/platform_file.h"
//#include "base/string16.h"

namespace file_util {

// Read the file at |path| into |contents|, returning true on success.
// |contents| may be NULL, in which case this function is useful for its
// side effect of priming the disk cache.
// Useful for unit tests.
bool ReadFileToString(const std::string& path, std::string* contents);

// Wrapper for fopen-like calls. Returns non-NULL FILE* on success.
FILE* OpenFile(const std::string& filename, const char* mode);

// Closes file opened by OpenFile. Returns true on success.
bool CloseFile(FILE* file);

}  // namespace file_util

#endif  // CHROMIUM_FILE_UTIL_HH_
