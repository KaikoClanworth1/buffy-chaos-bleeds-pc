/* dbghelp.h on Android: no symbol lookup (names come from logcat's own
 * backtraces instead). */
#pragma once
typedef unsigned long long DWORD64;
typedef struct { unsigned long SizeOfStruct, TypeIndex; unsigned long long Reserved[2]; unsigned long Index, Size;
                 unsigned long long ModBase; unsigned long Flags; unsigned long long Value, Address;
                 unsigned long Register, Scope, Tag, NameLen, MaxNameLen; char Name[1]; } SYMBOL_INFO;
#define SYMOPT_DEFERRED_LOADS 0x4
#define SYMOPT_UNDNAME        0x2
#define SymSetOptions(o)                 ((void)(o), 0)
#define SymInitialize(p, s, i)           ((void)(p), 0)
#define SymFromAddr(p, a, d, s)          ((void)(p), (void)(a), (void)(d), (void)(s), 0)
