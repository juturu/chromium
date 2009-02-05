// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/command_line.h"
#include "base/message_loop.h"
#include "base/path_service.h"
#include "base/platform_thread.h"
#include "base/string_util.h"
#include "base/system_monitor.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_counters.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/l10n_util.h"
#include "chrome/common/logging_chrome.h"
#include "chrome/common/main_function_params.h"
#include "chrome/common/resource_bundle.h"
#if defined(OS_WIN)
#include "chrome/common/win_util.h"
#endif
#include "chrome/renderer/renderer_main_platform_delegate.h"
#include "chrome/renderer/render_process.h"

#include "chromium_strings.h"
#include "generated_resources.h"

// This function provides some ways to test crash and assertion handling
// behavior of the renderer.
static void HandleRendererErrorTestParameters(const CommandLine& command_line) {
  // This parameter causes an assertion.
  if (command_line.HasSwitch(switches::kRendererAssertTest)) {
    DCHECK(false);
  }

  // This parameter causes a null pointer crash (crash reporter trigger).
  if (command_line.HasSwitch(switches::kRendererCrashTest)) {
    int* bad_pointer = NULL;
    *bad_pointer = 0;
  }

  if (command_line.HasSwitch(switches::kRendererStartupDialog)) {
#if defined(OS_WIN)
    std::wstring title = l10n_util::GetString(IDS_PRODUCT_NAME);
    title += L" renderer";  // makes attaching to process easier
    MessageBox(NULL, L"renderer starting...", title.c_str(),
               MB_OK | MB_SETFOREGROUND);
#else if defined(OS_MACOSX)
  // TODO(playmobil): In the long term, overriding this flag doesn't seem
  // right, either use our own flag or open a dialog we can use.
  // This is just to ease debugging in the interim.
  LOG(WARNING) << "Renderer ("
               << getpid()
               << ") paused waiting for debugger to attach @ pid" ;
  pause();
#endif  // !OS_WIN
  }
}

// mainline routine for running as the Rendererer process
int RendererMain(const MainFunctionParams& parameters) {
  const CommandLine& parsed_command_line = parameters.command_line_;
  RendererMainPlatformDelegate platform(parameters);

  StatsScope<StatsCounterTimer>
      startup_timer(chrome::Counters::renderer_main());

  // The main thread of the renderer services IO.
  MessageLoopForIO main_message_loop;
  std::wstring app_name = chrome::kBrowserAppName;
  PlatformThread::SetName(WideToASCII(app_name + L"_RendererMain").c_str());

  // Initialize the SystemMonitor
  base::SystemMonitor::Start();

  platform.PlatformInitialize();

  bool no_sandbox = parsed_command_line.HasSwitch(switches::kNoSandbox);
  platform.InitSandboxTests(no_sandbox);

  HandleRendererErrorTestParameters(parsed_command_line);

  std::wstring channel_name =
    parsed_command_line.GetSwitchValue(switches::kProcessChannelID);
  if (RenderProcess::GlobalInit(channel_name)) {
    bool run_loop = true;
    if (!no_sandbox) {
      run_loop = platform.EnableSandbox();
    }

    platform.RunSandboxTests();

    startup_timer.Stop();  // End of Startup Time Measurement.

    if (run_loop) {
      // Load the accelerator table from the browser executable and tell the
      // message loop to use it when translating messages.
      MessageLoop::current()->Run();
    }

    RenderProcess::GlobalCleanup();
  }
  platform.PlatformUninitialize();
  return 0;
}

