//===----------------- lib/Jit/options.cpp ----------------------*- C++ -*-===//
//
// LLILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Definition of the Options class that encapsulates JIT options
///        extracted from CoreCLR config values.
///
//===----------------------------------------------------------------------===//

#include "global.h"
#include "jitpch.h"
#include "LLILCJit.h"
#include "options.h"

// For now we're always running as the altjit
#define ALT_JIT 1

// These are the instantiations of the static method sets in Options.h.
// This MethodSets are initialized from CLRConfig values passed through
// the corinfo.h interface.

MethodSet Options::AltJitMethodSet;
MethodSet Options::AltJitNgenMethodSet;

Options::Options(LLILCJitContext *Context) : Context(Context) {}

void Options::initialize() {
  // Set 'IsAltJit' based on environment information.
  setIsAltJit();

  // Set dump level for this JIT invocation.
  setDumpLevel();

  // Set optimization level for this JIT invocation.
  setOptLevel();
}

void Options::setIsAltJit() {
  // Initial state is that we are not an alternative jit until proven otherwise;

  IsAltJit = false;

// NDEBUG is !Debug

#if !defined(NDEBUG)

  // DEBUG case

  // Get/reuse method set that contains the altjit method value.
  MethodSet *AltJit = nullptr;

  if (Context->Flags & CORJIT_FLG_PREJIT) {
    wchar_t *NgenStr = Context->JitInfo->getStringConfigValue(L"AltJitNgen");
    std::unique_ptr<std::string> NgenUtf8 = Convert::wideToUtf8(NgenStr);
    AltJitNgenMethodSet.init(std::move(NgenUtf8));
    Context->JitInfo->freeStringConfigValue(NgenStr);
    AltJit = &AltJitNgenMethodSet;
  } else {
    wchar_t *JitStr = Context->JitInfo->getStringConfigValue(L"AltJit");
    // Move this to the UTIL code and ifdef it for platform
    std::unique_ptr<std::string> JitUtf8 = Convert::wideToUtf8(JitStr);
    AltJitMethodSet.init(std::move(JitUtf8));
    Context->JitInfo->freeStringConfigValue(JitStr);
    AltJit = &AltJitMethodSet;
  }

#ifdef ALT_JIT
  if (AltJit->contains(Context->MethodName.data(), nullptr,
                       Context->MethodInfo->args.pSig)) {
    IsAltJit = true;
  }
#endif // ALT_JIT

#else
  if (Context->Flags & CORJIT_FLG_PREJIT) {
    wchar_t *NgenStr = Context->JitInfo->getStringConfigValue(L"AltJitNgen");
    std::unique_ptr<std::string> NgenUtf8 = Convert::wideToUtf8(NgenStr);
    if (NgenUtf8->compare("*") == 0) {
      IsAltJit = true;
    }
  } else {
    wchar_t *JitStr = Context->JitInfo->getStringConfigValue(L"AltJit");
    std::unique_ptr<std::string> JitUtf8 = Convert::wideToUtf8(JitStr);
    if (JitUtf8->compare("*") == 0) {
      IsAltJit = true;
    }
  }
#endif
}

void Options::setDumpLevel() {
  wchar_t *LevelWStr = Context->JitInfo->getStringConfigValue(L"DUMPLLVMIR");
  if (LevelWStr) {
    std::unique_ptr<std::string> Level = Convert::wideToUtf8(LevelWStr);
    std::transform(Level->begin(), Level->end(), Level->begin(), ::toupper);
    if (Level->compare("VERBOSE") == 0) {
      DumpLevel = VERBOSE;
      return;
    }
    if (Level->compare("SUMMARY") == 0) {
      DumpLevel = SUMMARY;
      return;
    }
  }

  DumpLevel = NODUMP;
}

void Options::setOptLevel() {
  // Currently we only check for the debug flag but this will be extended
  // to account for further opt levels as we move forward.
  if ((Context->Flags & CORJIT_FLG_DEBUG_CODE) != 0) {
    OptLevel = ::OptLevel::DEBUG_CODE;
  }
}

Options::~Options() {}
