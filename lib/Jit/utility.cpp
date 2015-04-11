// Utility code

#include "global.h"
#include "jitpch.h"

#include "utility.h"
#include "llvm/Support/ConvertUTF.h"

// Checks given parsed representation to see if method is in the set.

bool MethodSet::contains(const char *Name, const char *ClassName,
                         PCCOR_SIGNATURE sig) {
  assert(this->isInitialized());

  std::list<MethodName>::const_iterator iterator;

  for (iterator = this->MethodList->begin();
       iterator != this->MethodList->end(); ++iterator) {
    if (iterator->Name->compare("*") == 0) {
      return true;
    }

    // Just matching method name.  This is way too loose but have not yet
    // plumbed through the class name and wildcard logic.

    if (iterator->Name->compare(Name) == 0) {
      return true;
    }
  }

  return false;
}

std::unique_ptr<std::string> Convert::wideToUtf8(const wchar_t *WideStr) {
  ArrayRef<char> SrcBytes((const char *)WideStr, (2 * wcslen(WideStr)));
  std::unique_ptr<std::string> OutString(new std::string);
  llvm::convertUTF16ToUTF8String(SrcBytes, *OutString);

  return OutString;
}
