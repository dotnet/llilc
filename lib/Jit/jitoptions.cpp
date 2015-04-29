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
#include "jitoptions.h"

// Define a macro for cross-platform UTF-16 string literals.
#if defined(_MSC_VER)
#define UTF16(lit) L##lit
#else
#define UTF16(lit) u##lit
#endif

// For now we're always running as the altjit
#define ALT_JIT 1

// These are the instantiations of the static method sets in Options.h.
// This MethodSets are initialized from CLRConfig values passed through
// the corinfo.h interface.

MethodSet JitOptions::AltJitMethodSet;
MethodSet JitOptions::AltJitNgenMethodSet;

template <typename UTF16CharT>
char16_t *getStringConfigValue(ICorJitInfo *CorInfo, const UTF16CharT *Name) {
  static_assert(sizeof(UTF16CharT) == 2, "UTF16CharT is the wrong size!");
  return (char16_t *)CorInfo->getStringConfigValue((const wchar_t *)Name);
}

template <typename UTF16CharT>
void freeStringConfigValue(ICorJitInfo *CorInfo, UTF16CharT *Value) {
  static_assert(sizeof(UTF16CharT) == 2, "UTF16CharT is the wrong size!");
  return CorInfo->freeStringConfigValue((wchar_t *)Value);
}

JitOptions::JitOptions(LLILCJitContext &Context) {
  // Set 'IsAltJit' based on environment information.
  IsAltJit = queryIsAltJit(Context);

  // Set dump level for this JIT invocation.
  DumpLevel = queryDumpLevel(Context);

  // Set optimization level for this JIT invocation.
  OptLevel = queryOptLevel(Context);

  // Set whether to use conservative GC.
  UseConservativeGC = queryUseConservativeGC(Context);

  // Set whether to insert statepoints.
  DoInsertStatepoints = queryDoInsertStatepoints(Context);

  // Set whether to do tail call opt.
  DoTailCallOpt = queryDoTailCallOpt(Context);

  // Validate Statepoint and Conservative GC state.
  assert(DoInsertStatepoints ||
         UseConservativeGC && "Statepoints required for precise-GC");
}

bool JitOptions::queryDoTailCallOpt(LLILCJitContext &Context) {
  return (bool)DEFAULT_TAIL_CALL_OPT;
}

bool JitOptions::queryIsAltJit(LLILCJitContext &Context) {
  // Initial state is that we are not an alternative jit until proven otherwise;
  bool IsAlternateJit = false;

// NDEBUG is !Debug

#if !defined(NDEBUG)

  // DEBUG case

  // Get/reuse method set that contains the altjit method value.
  MethodSet *AltJit = nullptr;

  if (Context.Flags & CORJIT_FLG_PREJIT) {
    if (!AltJitNgenMethodSet.isInitialized()) {
      char16_t *NgenStr =
          getStringConfigValue(Context.JitInfo, UTF16("AltJitNgen"));
      std::unique_ptr<std::string> NgenUtf8 = Convert::utf16ToUtf8(NgenStr);
      AltJitNgenMethodSet.init(std::move(NgenUtf8));
      freeStringConfigValue(Context.JitInfo, NgenStr);
    }
    // Set up AltJitNgen set to be used for AltJit test.
    AltJit = &AltJitNgenMethodSet;
  } else {
    if (!AltJitMethodSet.isInitialized()) {
      char16_t *JitStr = getStringConfigValue(Context.JitInfo, UTF16("AltJit"));
      // Move this to the UTIL code and ifdef it for platform
      std::unique_ptr<std::string> JitUtf8 = Convert::utf16ToUtf8(JitStr);
      AltJitMethodSet.init(std::move(JitUtf8));
      freeStringConfigValue(Context.JitInfo, JitStr);
    }
    // Set up AltJit set to be use for AltJit test.
    AltJit = &AltJitMethodSet;
  }

#ifdef ALT_JIT
  if (AltJit->contains(Context.MethodName.data(), nullptr,
                       Context.MethodInfo->args.pSig)) {
    IsAlternateJit = true;
  }
#endif // ALT_JIT

#else
  if (Context.Flags & CORJIT_FLG_PREJIT) {
    char16_t *NgenStr =
        getStringConfigValue(Context.JitInfo, UTF16("AltJitNgen"));
    std::unique_ptr<std::string> NgenUtf8 = Convert::utf16ToUtf8(NgenStr);
    if (NgenUtf8->compare("*") == 0) {
      IsAlternateJit = true;
    }
  } else {
    char16_t *JitStr = getStringConfigValue(Context.JitInfo, UTF16("AltJit"));
    std::unique_ptr<std::string> JitUtf8 = Convert::utf16ToUtf8(JitStr);
    if (JitUtf8->compare("*") == 0) {
      IsAlternateJit = true;
    }
  }
#endif

  return IsAlternateJit;
}

::DumpLevel JitOptions::queryDumpLevel(LLILCJitContext &Context) {
  ::DumpLevel JitDumpLevel = ::DumpLevel::NODUMP;

  char16_t *LevelWStr =
      getStringConfigValue(Context.JitInfo, UTF16("DUMPLLVMIR"));
  if (LevelWStr) {
    std::unique_ptr<std::string> Level = Convert::utf16ToUtf8(LevelWStr);
    std::transform(Level->begin(), Level->end(), Level->begin(), ::toupper);
    if (Level->compare("VERBOSE") == 0) {
      JitDumpLevel = ::DumpLevel::VERBOSE;
    } else if (Level->compare("SUMMARY") == 0) {
      JitDumpLevel = ::DumpLevel::SUMMARY;
    }
  }

  return JitDumpLevel;
}

// Get the GC-Scheme used by the runtime -- conservative/precise
bool JitOptions::queryUseConservativeGC(LLILCJitContext &Context) {
  char16_t *GCStr =
      getStringConfigValue(Context.JitInfo, UTF16("GCCONSERVATIVE"));
  return (GCStr != nullptr);
}

// Determine if GC statepoints should be inserted.
bool JitOptions::queryDoInsertStatepoints(LLILCJitContext &Context) {
  char16_t *StatePointStr =
      getStringConfigValue(Context.JitInfo, UTF16("INSERTSTATEPOINTS"));
  return (StatePointStr != nullptr);
}

OptLevel JitOptions::queryOptLevel(LLILCJitContext &Context) {
  ::OptLevel JitOptLevel = ::OptLevel::INVALID;
  // Currently we only check for the debug flag but this will be extended
  // to account for further opt levels as we move forward.
  if ((Context.Flags & CORJIT_FLG_DEBUG_CODE) != 0) {
    JitOptLevel = ::OptLevel::DEBUG_CODE;
  }

  return JitOptLevel;
}

JitOptions::~JitOptions() {}
