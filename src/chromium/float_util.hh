// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUM_FLOAT_UTIL_HH_
#define CHROMIUM_FLOAT_UTIL_HH_
#pragma once

#include <float.h>
#include <math.h>

namespace chromium {

inline bool IsFinite(const double& number) {
#ifdef _WIN32
  return _finite(number) != 0;
#else
  return finite(number) != 0;
#endif
}

}  // namespace chromium

#endif  // CHROMIUM_FLOAT_UTIL_HH_
