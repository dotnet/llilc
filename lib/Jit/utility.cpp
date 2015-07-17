// Utility code

#include <cstdlib>

#include "global.h"
#include "jitpch.h"

#include "cor.h" // CorSigUncompressData
#include "utility.h"
#include "llvm/Support/ConvertUTF.h"

using std::string;
using std::unique_ptr;

int MethodID::parseArgs(const string &S, size_t &I) {
  size_t Start = ++I; // skip '('

  I = S.find_first_of(")", I);
  if (I == string::npos)
    return MethodIDState::AnyArgs;

  string Num = string(S, Start, I - Start);
  int NArgs = atoi(Num.c_str());
  ++I; // skip )
  return NArgs;
}

unique_ptr<MethodID> MethodID::parse(const string &S, size_t &I) {
  MethodID MId;
  size_t SLen = S.length();

  string Token = MId.scan(S, I);

  if (Token == "") // off the end of S
    return nullptr;

  if (Token == "*") {
    MId.ClassName = llvm::make_unique<string>(Token);
    MId.MethodName = nullptr;
    if (I >= SLen || S[I] == ' ') {
      MId.NumArgs = MethodIDState::AnyArgs;
    }
    return llvm::make_unique<MethodID>(MId);
    return nullptr;
  }

  if (S[I] == ':') { // C:M | C:M(A)
    MId.ClassName = llvm::make_unique<string>(Token);
    Token = MId.scan(S, ++I); // M | M(A)
  }

  MId.MethodName = llvm::make_unique<string>(Token);

  if (I >= SLen || S[I] == ' ') {
    MId.NumArgs = MethodIDState::AnyArgs;
    return llvm::make_unique<MethodID>(MId);
    return nullptr;
  }

  if (S[I] != '(') // illegal
    return nullptr;

  MId.NumArgs = MId.parseArgs(S, I);
  return llvm::make_unique<MethodID>(MId);
  return nullptr;
}

string MethodID::scan(const string &S, size_t &I) {
  const size_t SLen = S.length();

  if (I >= SLen) // already 'off the end'
    return string{""};

  size_t Start = I = S.find_first_not_of(" \t", I); // skip whitespace

  I = S.find_first_of(" :()", I);

  if (I == string::npos) // eg: S = "*"
    I = SLen;

  return string(S, Start, I - Start);
}

void MethodSet::init(unique_ptr<string> ConfigValue) {
  if (this->isInitialized()) {
    return;
  }
  insert(std::move(ConfigValue));
}

void MethodSet::insert(unique_ptr<string> Ups) {
  size_t I = 0;
  std::list<MethodID> *MIdList = new std::list<MethodID>();
  string S = *Ups;

  auto MId = MethodID::parse(S, I);
  while (MId) {
    MIdList->push_back(*MId);
    MId = MethodID::parse(S, I);
  }

  // This write should be atomic, delete if we're not the first.
  llvm::sys::cas_flag F = llvm::sys::CompareAndSwap(
      (llvm::sys::cas_flag *)&(this->Initialized), 0x1, 0x0);
  if (F != 0x0) {
    delete MIdList;
  } else {
    this->MethodIDList = MIdList;
  }
}

bool MethodSet::contains(const char *MethodName, const char *ClassName,
                         PCCOR_SIGNATURE PCSig) {

  assert(this->isInitialized());

  int NumArgs = MethodIDState::AnyArgs; // assume no signature supplied

  if (PCSig) {
    PCSig++; // skip calling convention
    NumArgs = CorSigUncompressData(PCSig);
  }

  string StrClassName = ClassName ? ClassName : "";
  string StrMethodName = MethodName ? MethodName : "";

  auto Begin = this->MethodIDList->begin();
  auto End = this->MethodIDList->end();

  for (auto P = Begin; P != End; ++P) { // P => "pattern"

    // Check for "*", the common case, first
    if (P->ClassName && *P->ClassName == "*")
      return true;

    // Check for mis-match on NumArgs
    if (P->NumArgs != MethodIDState::AnyArgs && P->NumArgs != NumArgs)
      continue;

    // Check for mis-match on MethodName
    if (P->MethodName && (*P->MethodName != StrMethodName))
      continue;

    // Check for match on ClassName (we already match NumArgs and MethodName)
    if (!P->ClassName) // no ClassName
      return true;

    if (*P->ClassName == StrClassName)
      return true;
  }

  return false;
}

unique_ptr<std::string> Convert::utf16ToUtf8(const char16_t *WideStr) {
  // Get the length of the input
  size_t SrcLen = 0;
  for (; WideStr[SrcLen] != (char16_t)0; SrcLen++)
    ;

  llvm::ArrayRef<char> SrcBytes((const char *)WideStr, 2 * SrcLen);
  unique_ptr<std::string> OutString(new std::string);
  convertUTF16ToUTF8String(SrcBytes, *OutString);

  return OutString;
}
