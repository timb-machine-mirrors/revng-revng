#ifndef REVNGC_MANGLING_H
#define REVNGC_MANGLING_H

//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

// Standard includes
#include <cctype>

// LLVM includes
#include <llvm/ADT/STLExtras.h>

// This is very simple for now.
// In the future we might consider making it more robust using something like
// Punycode https://tools.ietf.org/html/rfc3492 , which also has the nice
// property of being deterministically reversible.
inline std::string makeCIdentifier(std::string S) {
  llvm::for_each(S, [](char &C) {
    if (not std::isalnum(C))
      C = '_';
  });
  return S;
}

#endif // REVNGC_MANGLING_H
