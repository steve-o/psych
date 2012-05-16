// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUM_LOGGING_WIN_HH_
#define CHROMIUM_LOGGING_WIN_HH_
#pragma once

#include <string>

/* Boost noncopyable base class */
#include <boost/utility.hpp>

#include "win/event_trace_provider.hh"
#include "logging.hh"

template <typename Type>
struct StaticMemorySingletonTraits;

namespace logging {

// Event ID for the log messages we generate.
extern const GUID kLogEventId;

// Feature enable mask for LogEventProvider.
enum LogEnableMask {
  // If this bit is set in our provider enable mask, we will include
  // a stack trace with every log message.
  ENABLE_STACK_TRACE_CAPTURE = 0x0001,
  // If this bit is set in our provider enable mask, the provider will log
  // a LOG message with only the textual content of the message, and no
  // stack trace.
  ENABLE_LOG_MESSAGE_ONLY = 0x0002,
};

// The message types our log event provider generates.
// ETW likes user message types to start at 10.
enum LogMessageTypes {
  // A textual only log message, contains a zero-terminated string.
  LOG_MESSAGE = 10,
  // A message with a stack trace, followed by the zero-terminated
  // message text.
  LOG_MESSAGE_WITH_STACKTRACE = 11,
  // A message with:
  //  a stack trace,
  //  the line number as a four byte integer,
  //  the file as a zero terminated UTF8 string,
  //  the zero-terminated UTF8 message text.
  LOG_MESSAGE_FULL = 12,
};

// Trace provider class to drive log control and transport
// with Event Tracing for Windows.
class LogEventProvider : public chromium::win::EtwTraceProvider {
 public:
  static LogEventProvider* GetInstance();

  static bool LogMessage(logging::LogSeverity severity, const char* file,
      int line, size_t message_start, const std::string& str);

  static void Initialize(const GUID& provider_name);
  static void Uninitialize();

 protected:
  // Overridden to manipulate the log level on ETW control callbacks.
  virtual void OnEventsEnabled();
  virtual void OnEventsDisabled();

 private:
  LogEventProvider();

  // The log severity prior to OnEventsEnabled,
  // restored in OnEventsDisabled.
  logging::LogSeverity old_log_level_;

  friend struct StaticMemorySingletonTraits<LogEventProvider>;
};

}  // namespace logging

#endif  // CHROMIUM_LOGGING_WIN_HH_
