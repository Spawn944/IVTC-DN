#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <DbgHelp.h>
#include <stdio.h>
#include <exception>
#include <signal.h>

// Auto-link the debug library for minidumps
#pragma comment(lib, "dbghelp.lib")

static void WriteCrashLog(const char* message) {
	FILE* f = fopen("crash_log.txt", "a");
	if (f) {
		fprintf(f, "[CRASH] %s\n", message);
		fclose(f);
	}
	OutputDebugStringA("[CRASH] ");
	OutputDebugStringA(message);
	OutputDebugStringA("\n");
}

static void CreateMiniDump(EXCEPTION_POINTERS* pep) {
	HANDLE hFile = CreateFileA("crash_dump.dmp", GENERIC_READ | GENERIC_WRITE,
		0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if ((hFile != NULL) && (hFile != INVALID_HANDLE_VALUE)) {
		MINIDUMP_EXCEPTION_INFORMATION mdei;
		mdei.ThreadId = GetCurrentThreadId();
		mdei.ExceptionPointers = pep;
		mdei.ClientPointers = FALSE;

		MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
			hFile, MiniDumpNormal, (pep != 0) ? &mdei : 0, 0, 0);
		CloseHandle(hFile);
		WriteCrashLog("Minidump successfully generated: crash_dump.dmp (Open this in Visual Studio!)");
	}
	else {
		WriteCrashLog("Failed to create crash_dump.dmp file.");
	}
}

static LONG WINAPI GlobalCrashHandler(EXCEPTION_POINTERS* exceptionInfo) {
	WriteCrashLog("FATAL EXCEPTION CAUGHT (Access Violation / Segfault)!");
	char buffer[256];
	snprintf(buffer, sizeof(buffer), "Exception Code: 0x%08X", exceptionInfo->ExceptionRecord->ExceptionCode);
	WriteCrashLog(buffer);
	CreateMiniDump(exceptionInfo);
	return EXCEPTION_EXECUTE_HANDLER;
}

static void InvalidParameterHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t pReserved) {
	WriteCrashLog("Invalid Parameter Handler triggered (CRT Failure)!");
}

static void PureCallHandler() {
	WriteCrashLog("Pure Virtual Function Call triggered!");
}

static void SignalHandler(int signal) {
	char buf[128];
	snprintf(buf, sizeof(buf), "Signal caught: %d", signal);
	WriteCrashLog(buf);
}

static void TerminateHandler() {
	WriteCrashLog("std::terminate called!");
	abort();
}

extern Walnut::Application* Walnut::CreateApplication(int argc, char** argv);
bool g_ApplicationRunning = true;

namespace Walnut {
	int Main(int argc, char** argv)
	{
		// Register all defensive crash handlers
		SetUnhandledExceptionFilter(GlobalCrashHandler);
		_set_invalid_parameter_handler(InvalidParameterHandler);
		_set_purecall_handler(PureCallHandler);
		std::set_terminate(TerminateHandler);
		signal(SIGSEGV, SignalHandler);
		signal(SIGABRT, SignalHandler);
		signal(SIGILL, SignalHandler);

		try {
			while (g_ApplicationRunning)
			{
				Walnut::Application* app = Walnut::CreateApplication(argc, argv);
				app->Run();
				delete app;
			}
		}
		catch (const std::exception& e) {
			WriteCrashLog("Standard C++ Exception Caught:");
			WriteCrashLog(e.what());
		}
		catch (...) {
			WriteCrashLog("Unknown C++ Exception caught!");
		}

		return 0;
	}
}

#if defined WL_DIST && defined WL_PLATFORM_WINDOWS

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hInstPrev, PSTR cmdline, int cmdshow)
{
	return Walnut::Main(__argc, __argv);
}

#else

int main(int argc, char** argv)
{
	return Walnut::Main(argc, argv);
}

#endif // WL_DIST
