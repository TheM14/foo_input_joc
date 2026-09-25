// Force-included into every translation unit of this project (ForcedIncludeFiles).
//
// The SDK is written against the canonical component PCH, which pulls in large
// parts of the standard library before any SDK header is seen.  Without a PCH the
// SDK headers can be the first thing a translation unit includes, and several of
// them use std:: types without including the header that declares them (initquit.h
// needs <functional>, for one).  Listing the standard headers here keeps that
// dependency explicit instead of relying on what the compiler happens to include
// transitively.
#pragma once

// The kernel's C++ sources are compiled into this component (kernel\joc_kernel.vcxproj),
// so the shared headers must not mark their entry points as dllimport.
#define JOC_STATIC 1

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
