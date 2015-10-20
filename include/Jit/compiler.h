//===--------------- include/Jit/compiler.h ---------------------*- C++ -*-===//
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
/// \brief Declaration of the compiler functor used in the ORC infrastructure
///        to compile a module.
///
//===----------------------------------------------------------------------===//

#ifndef COMPILER_H
#define COMPILER_H

#include "GcInfo.h"
#include "LLILCJit.h"
#include "llvm/ExecutionEngine/ObjectMemoryBuffer.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {
namespace orc {

/// \brief Default compile functor: Takes a single IR module and returns an
///        ObjectFile.
class LLILCCompiler {
public:
  /// \brief Construct a simple compile functor with the given target.
  LLILCCompiler(TargetMachine &TM) : TM(TM) {}

  /// \brief Compile a Module to an ObjectFile.
  object::OwningBinary<object::ObjectFile> operator()(Module &M) const {
    SmallVector<char, 0> ObjBufferSV;
    raw_svector_ostream ObjStream(ObjBufferSV);

    legacy::PassManager PM;
    MCContext *Ctx;
    if (TM.addPassesToEmitMC(PM, Ctx, ObjStream))
      llvm_unreachable("Target does not support MC emission.");
    PM.add(new GcInfoRecorder());
    PM.run(M);
    std::unique_ptr<MemoryBuffer> ObjBuffer(
        new ObjectMemoryBuffer(std::move(ObjBufferSV)));
    ErrorOr<std::unique_ptr<object::ObjectFile>> Obj =
        object::ObjectFile::createObjectFile(ObjBuffer->getMemBufferRef());
    // TODO: Actually report errors helpfully.
    typedef object::OwningBinary<object::ObjectFile> OwningObj;
    if (Obj)
      return OwningObj(std::move(*Obj), std::move(ObjBuffer));
    return OwningObj(nullptr, nullptr);
  }

private:
  TargetMachine &TM;
};
} // namespace orc
} // namespace llvm
#endif // COMPILER_H
