/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file crashlog_unix.cpp Unix crash log handler */

#include "../../stdafx.h"
#include "../../crashlog.h"
#include "../../fileio_func.h"
#include "../../string_func.h"
#include "../../gamelog.h"
#include "../../saveload/saveload.h"

#include <setjmp.h>
#include <signal.h>
#include <sys/utsname.h>

#if defined(__GLIBC__)
/* Execinfo (and thus making stacktraces) is a GNU extension */
#	include <execinfo.h>
#endif

#if defined(__NetBSD__)
#include <unistd.h>
#endif

#ifdef WITH_UNOFFICIAL_BREAKPAD
#	include <client/linux/handler/exception_handler.h>
#endif

#if defined(__EMSCRIPTEN__)
#	include <emscripten.h>
/* We avoid abort(), as it is a SIGBART, and use _exit() instead. But emscripten doesn't know _exit(). */
#	define _exit emscripten_force_exit
#endif

#include "../../safeguards.h"

/** The signals we want our crash handler to handle. */
static constexpr int _signals_to_handle[] = { SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL, SIGQUIT };

/**
 * Unix implementation for the crash logger.
 */
class CrashLogUnix : public CrashLog {
	/** Signal that has been thrown. */
	int signum;

	void LogOSVersion(std::back_insert_iterator<std::string> &output_iterator) const override
	{
		struct utsname name;
		if (uname(&name) < 0) {
			 fmt::format_to(output_iterator, "Could not get OS version: {}\n", strerror(errno));
			 return;
		}

		fmt::format_to(output_iterator,
				"Operating system:\n"
				" Name:     {}\n"
				" Release:  {}\n"
				" Version:  {}\n"
				" Machine:  {}\n",
				name.sysname,
				name.release,
				name.version,
				name.machine
		);
	}

	void LogError(std::back_insert_iterator<std::string> &output_iterator, const std::string_view message) const override
	{
		fmt::format_to(output_iterator,
			   "Crash reason:\n"
				" Signal:  {} ({})\n"
				" Message: {}\n\n",
				strsignal(this->signum),
				this->signum,
				message
		);
	}

	void LogStacktrace(std::back_insert_iterator<std::string> &output_iterator) const override
	{
		fmt::format_to(output_iterator, "Stacktrace:\n");
#if defined(__GLIBC__)
		void *trace[64];
		int trace_size = backtrace(trace, lengthof(trace));

		char **messages = backtrace_symbols(trace, trace_size);
		for (int i = 0; i < trace_size; i++) {
			fmt::format_to(output_iterator, " [{:02}] {}\n", i, messages[i]);
		}
		free(messages);
#else
		fmt::format_to(output_iterator, " Not supported.\n");
#endif
		fmt::format_to(output_iterator, "\n");
	}

#ifdef WITH_UNOFFICIAL_BREAKPAD
	static bool MinidumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
	{
		CrashLogUnix *crashlog = reinterpret_cast<CrashLogUnix *>(context);

		crashlog->crashdump_filename = crashlog->CreateFileName(".dmp");
		std::rename(descriptor.path(), crashlog->crashdump_filename.c_str());
		return succeeded;
	}

	bool WriteCrashDump() override
	{
		return google_breakpad::ExceptionHandler::WriteMinidump(_personal_dir, MinidumpCallback, this);
	}
#endif

	/* virtual */ bool TryExecute(std::string_view section_name, std::function<bool()> &&func) override
	{
		this->try_execute_active = true;

		/* Setup a longjump in case a crash happens. */
		if (setjmp(this->internal_fault_jmp_buf) != 0) {
			fmt::print("Something went wrong when attempting to fill {} section of the crash log.\n", section_name);

			/* Reset the signals and continue on. The handler is responsible for dealing with the crash. */
			sigset_t sigs;
			sigemptyset(&sigs);
			for (int signum : _signals_to_handle) {
				sigaddset(&sigs, signum);
			}
			sigprocmask(SIG_UNBLOCK, &sigs, nullptr);

			this->try_execute_active = false;
			return false;
		}

		bool res = func();
		this->try_execute_active = false;
		return res;
	}

public:
	/**
	 * A crash log is always generated by signal.
	 * @param signum the signal that was caused by the crash.
	 */
	CrashLogUnix(int signum) :
		signum(signum)
	{
	}

	/** Buffer to track the long jump set setup. */
	jmp_buf internal_fault_jmp_buf;

	/** Whether we are in a TryExecute block. */
	bool try_execute_active = false;

	/** Points to the current crash log. */
	static CrashLogUnix *current;
};

/* static */ CrashLogUnix *CrashLogUnix::current = nullptr;

/**
 * Set a signal handler for all signals we want to capture.
 *
 * @param handler The handler to use.
 * @return sigset_t A sigset_t containing all signals we want to capture.
 */
static sigset_t SetSignals(void(*handler)(int))
{
	sigset_t sigs;
	sigemptyset(&sigs);
	for (int signum : _signals_to_handle) {
		sigaddset(&sigs, signum);
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_RESTART;

	sigemptyset(&sa.sa_mask);
	sa.sa_handler = handler;
	sa.sa_mask = sigs;

	for (int signum : _signals_to_handle) {
		sigaction(signum, &sa, nullptr);
	}

	return sigs;
}

/**
 * Entry point for a crash that happened during the handling of a crash.
 *
 * @param signum the signal that caused us to crash.
 */
static void CDECL HandleInternalCrash(int signum)
{
	if (CrashLogUnix::current == nullptr || !CrashLogUnix::current->try_execute_active) {
		fmt::print("Something went seriously wrong when creating the crash log. Aborting.\n");
		_exit(1);
	}

	longjmp(CrashLogUnix::current->internal_fault_jmp_buf, 1);
}

/**
 * Entry point for the crash handler.
 *
 * @param signum the signal that caused us to crash.
 */
static void CDECL HandleCrash(int signum)
{
	if (CrashLogUnix::current != nullptr) {
		CrashLog::AfterCrashLogCleanup();
		_exit(2);
	}

	/* Capture crashing during the handling of a crash. */
	sigset_t sigs = SetSignals(HandleInternalCrash);
	sigset_t old_sigset;
	sigprocmask(SIG_UNBLOCK, &sigs, &old_sigset);

	if (_gamelog.TestEmergency()) {
		fmt::print("A serious fault condition occurred in the game. The game will shut down.\n");
		fmt::print("As you loaded an emergency savegame no crash information will be generated.\n");
		_exit(3);
	}

	if (SaveloadCrashWithMissingNewGRFs()) {
		fmt::print("A serious fault condition occurred in the game. The game will shut down.\n");
		fmt::print("As you loaded an savegame for which you do not have the required NewGRFs\n");
		fmt::print("no crash information will be generated.\n");
		_exit(3);
	}

	CrashLogUnix *log = new CrashLogUnix(signum);
	CrashLogUnix::current = log;
	log->MakeCrashLog();

	CrashLog::AfterCrashLogCleanup();
	_exit(2);
}

/* static */ void CrashLog::InitialiseCrashLog()
{
	SetSignals(HandleCrash);
}

/* static */ void CrashLog::InitThread()
{
}
