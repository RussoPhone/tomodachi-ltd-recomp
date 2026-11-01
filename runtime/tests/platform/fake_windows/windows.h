#pragma once

#include <cstddef>
#include <cstdint>

using BOOL = int;
using DWORD = unsigned long;
using ULONG = unsigned long;
using ULONG64 = std::uint64_t;
using ULONG_PTR = std::uintptr_t;
using SIZE_T = std::size_t;
using LONG = std::int32_t;
using PVOID = void*;
using HANDLE = void*;
using HMODULE = void*;
using LPHANDLE = HANDLE*;

#define WINAPI
#define CALLBACK

#define PAGE_NOACCESS 0x01
#define PAGE_READWRITE 0x04
#define MEM_RESERVE 0x2000
#define MEM_COMMIT 0x1000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_RELEASE 0x8000
#define DUPLICATE_SAME_ACCESS 0x00000002
#define FALSE 0
#define TRUE 1
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION -1

struct EXCEPTION_RECORD {
    DWORD ExceptionCode;
    ULONG_PTR ExceptionInformation[2];
};
struct EXCEPTION_POINTERS {
    EXCEPTION_RECORD* ExceptionRecord;
};

extern "C" {
HMODULE GetModuleHandleW(const wchar_t* name);
void* GetProcAddress(HMODULE module, const char* name);
HANDLE GetCurrentProcess();
BOOL DuplicateHandle(HANDLE source_process, HANDLE source, HANDLE target_process, LPHANDLE target,
                     DWORD access, BOOL inherit, DWORD options);
BOOL CloseHandle(HANDLE handle);
BOOL VirtualFree(void* address, SIZE_T size, DWORD type);
void* VirtualAlloc(void* address, SIZE_T size, DWORD type, DWORD protection);
void* AddVectoredExceptionHandler(ULONG first, LONG(CALLBACK* handler)(EXCEPTION_POINTERS*));
DWORD GetLastError();
}
