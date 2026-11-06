#pragma once
#include <stdarg.h>
#include <stddef.h>
typedef int BOOL; typedef unsigned long DWORD; typedef long LONG; typedef long HRESULT;
typedef long long LONGLONG; typedef void *HANDLE; typedef HANDLE HMODULE, HINSTANCE;
typedef int (__stdcall *FARPROC)(void);
typedef union { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;
#define TRUE 1
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define PAGE_EXECUTE_READWRITE 0x40
#define GENERIC_WRITE 0x40000000UL
#define FILE_SHARE_READ 1
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0x80
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005L)
#define YieldProcessor() __builtin_ia32_pause()
#define WINAPI __stdcall
#define IMP __declspec(dllimport)
IMP DWORD WINAPI GetModuleFileNameA(HMODULE, char *, DWORD);
IMP unsigned WINAPI GetSystemDirectoryA(char *, unsigned);
IMP HMODULE WINAPI LoadLibraryA(const char *);
IMP FARPROC WINAPI GetProcAddress(HMODULE, const char *);
IMP BOOL WINAPI VirtualProtect(void *, size_t, DWORD, DWORD *);
IMP BOOL WINAPI FlushInstructionCache(HANDLE, const void *, size_t);
IMP HANDLE WINAPI GetCurrentProcess(void);
IMP BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *);
IMP BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *);
IMP void WINAPI Sleep(DWORD);
IMP unsigned WINAPI GetPrivateProfileIntA(const char *, const char *, int, const char *);
IMP DWORD WINAPI GetPrivateProfileStringA(const char *, const char *, const char *, char *, DWORD, const char *);
IMP HANDLE WINAPI CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
IMP BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMP BOOL WINAPI FlushFileBuffers(HANDLE);
IMP BOOL WINAPI CloseHandle(HANDLE);
IMP BOOL WINAPI DisableThreadLibraryCalls(HMODULE);
IMP DWORD WINAPI timeGetTime(void);
IMP unsigned WINAPI timeBeginPeriod(unsigned);
IMP unsigned WINAPI timeEndPeriod(unsigned);
IMP int WINAPI wvsprintfA(char *, const char *, va_list);
IMP HMODULE WINAPI GetModuleHandleA(const char *);
typedef struct { unsigned short wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { void *BaseAddress; void *AllocationBase; DWORD AllocationProtect; size_t RegionSize; DWORD State; DWORD Protect; DWORD Type; } MEMORY_BASIC_INFORMATION;
typedef long (__stdcall *TOPLEVEL_FILTER)(void *ep);
IMP TOPLEVEL_FILTER WINAPI SetUnhandledExceptionFilter(TOPLEVEL_FILTER);
IMP void * WINAPI AddVectoredExceptionHandler(unsigned long first, TOPLEVEL_FILTER handler);
IMP BOOL WINAPI GetModuleHandleExA(DWORD flags, const char *name, HMODULE *out);
IMP size_t WINAPI VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, size_t len);
IMP DWORD WINAPI SetFilePointer(HANDLE, LONG, LONG *, DWORD);
IMP void WINAPI GetLocalTime(SYSTEMTIME *);
IMP int __cdecl wsprintfA(char *, const char *, ...);
