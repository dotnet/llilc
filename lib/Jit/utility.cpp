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
    return MethodIDState::ANYARGS;

  string num = string(S, Start, I - Start);
  int NArgs = atoi(num.c_str());
  ++I; // skip )
  return NArgs;
}

unique_ptr<MethodID> MethodID::parse(const string &S, size_t &I) {
  MethodID MID;
  size_t SLen = S.length();

  string token = MID.scan(S, I);

  if (token == "") // off the end of S
    return nullptr;

  if (token == "*") {
    MID.ClassName = llvm::make_unique<string>(token);
    MID.MethodName = nullptr;
    if (I >= SLen || S[I] == ' ') {
      MID.NumArgs = MethodIDState::ANYARGS;
    }
    return llvm::make_unique<MethodID>(MID);
    return nullptr;
  }

  if (S[I] == ':') { // C:M | C:M(A)
    MID.ClassName = llvm::make_unique<string>(token);
    token = MID.scan(S, ++I); // M | M(A)
  }

  MID.MethodName = llvm::make_unique<string>(token);

  if (I >= SLen || S[I] == ' ') {
    MID.NumArgs = MethodIDState::ANYARGS;
    return llvm::make_unique<MethodID>(MID);
    return nullptr;
  }

  if (S[I] != '(') // illegal
    return nullptr;

  MID.NumArgs = MID.parseArgs(S, I);
  return llvm::make_unique<MethodID>(MID);
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
  std::list<MethodID> *MIDL = new std::list<MethodID>();
  string S = *Ups;

  auto MID = MethodID::parse(S, I);
  while (MID) {
    MIDL->push_back(*MID);
    MID = MethodID::parse(S, I);
  }

  // This write should be atomic, delete if we're not the first.
  llvm::sys::cas_flag F = llvm::sys::CompareAndSwap(
      (llvm::sys::cas_flag *)&(this->Initialized), 0x1, 0x0);
  if (F != 0x0) {
    delete MIDL;
  } else {
    this->MethodIDList = MIDL;
  }
}

bool MethodSet::contains(const char *MethodName, const char *ClassName,
                         PCCOR_SIGNATURE PCSig) {

  assert(this->isInitialized());

  int NumArgs = MethodIDState::ANYARGS; // assume no signature supplied

  if (PCSig) {
    PCSig++; // skip calling convention
    NumArgs = CorSigUncompressData(PCSig);
  }

  string StrClassName = ClassName ? ClassName : "";
  string StrMethodName = MethodName ? MethodName : "";

  auto begin = this->MethodIDList->begin();
  auto end = this->MethodIDList->end();

  for (auto p = begin; p != end; ++p) { // p => "pattern"

    // Check for "*", the common case, first
    if (p->ClassName && *p->ClassName == "*")
      return true;

    // Check for mis-match on NumArgs
    if (p->NumArgs != MethodIDState::ANYARGS && p->NumArgs != NumArgs)
      continue;

    // Check for mis-match on MethodName
    if (p->MethodName && (*p->MethodName != StrMethodName))
      continue;

    // Check for match on ClassName (we already match NumArgs and MethodName)
    if (!p->ClassName) // no ClassName
      return true;

    if (*p->ClassName == StrClassName)
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
