/*
 * opencog/util/Logger.cc
 *
 * Copyright (C) 2002-2007 Novamente LLC
 * Copyright (C) 2008, 2010 OpenCog Foundation
 * Copyright (C) 2009, 2011, 2013 Linas Vepstas
 * All Rights Reserved
 *
 * Written by Andre Senna <senna@vettalabs.com>
 *            Gustavo Gama <gama@vettalabs.com>
 *            Linas Vepstas <linasvepstas@gmail.com>
 *            Joel Pitt <joel@opencog.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef CYGWIN
#include <cxxabi.h>
#include <execinfo.h>
#endif

#include <iostream>
#include <sstream>

#include <stdarg.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifdef WIN32_NOT_UNIX
#include <winsock2.h>
#undef ERROR
#undef DEBUG
#else
#include <sys/time.h>
#endif

#ifdef HAVE_VALGRIND
#include <valgrind/drd.h>
#endif

#include <opencog/util/backtrace-symbols.h>
#include <opencog/util/platform.h>

#include "Logger.h"

#ifdef __APPLE__
#define fdatasync fsync
#endif

using namespace opencog;

// messages greater than this will be truncated
#define MAX_PRINTF_STYLE_MESSAGE_SIZE (1<<15)
const char* levelStrings[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "FINE"};

#ifndef CYGWIN /// @todo backtrace and backtrace_symbols is UNIX, we
              /// may need a WIN32 version
static void prt_backtrace(std::ostringstream& oss)
{
#define BT_BUFSZ 50
	void *bt_buf[BT_BUFSZ];

	int stack_depth = backtrace(bt_buf, BT_BUFSZ);
	char **syms = oc_backtrace_symbols(bt_buf, stack_depth);

    // Depending on how the dependencies are met, syms could be NULL
    if (syms == NULL) return;

	// Start printing at a bit into the stack, so as to avoid recording
	// the logger functions in the stack trace.
	oss << "\tStack Trace:\n";
	for (int i=2; i < stack_depth; i++)
	{
		// Most things we'll print are mangled C++ names,
		// So demangle them, get them to pretty-print.
#if defined(HAVE_BFD) && defined(HAVE_IBERTY)
		// The standard and the heck versions differ slightly in layout.
		char * begin = strstr(syms[i], "_ZN");
		char * end = strchr(syms[i], '(');
		if (!(begin && end) || end <= begin)
		{
			// Failed to pull apart the symbol names
			oss << "\t" << i << ": " << syms[i] << "\n";
		}
		else
		{
			*begin = 0x0;
			oss << "\t" << i << ": " << syms[i] << "  ";
			*begin = '_';
			size_t sz = 250;
			int status;
			char *fname = (char *) malloc(sz);
			*end = 0x0;
			char *rv = abi::__cxa_demangle(begin, fname, &sz, &status);
			if (rv) fname = rv; // might have re-alloced
			oss << fname << std::endl;
			free(fname);
		}
#else
		char * begin = strchr(syms[i], '(');
		char * end = strchr(syms[i], '+');
		if (!(begin && end) || end <= begin)
		{
			// Failed to pull apart the symbol names
			oss << "\t" << i << ": " << syms[i] << "\n";
		}
		else
		{
			*begin = 0x0;
			oss << "\t" << i << ": " << syms[i] << " ";
			*begin = '(';
			size_t sz = 250;
			int status;
			char *fname = (char *) malloc(sz);
			*end = 0x0;
			char *rv = abi::__cxa_demangle(begin+1, fname, &sz, &status);
			*end = '+';
			if (rv) fname = rv; // might have re-alloced
			oss << "(" << fname << " " << end << std::endl;
			free(fname);
		}
#endif
	}
	oss << std::endl;
	free(syms);
}
#endif

Logger::~Logger()
{
    // Wait for queue to empty
    flush();
    stop_write_loop();

    if (logfile != NULL) fclose(logfile);
}

void Logger::start_write_loop()
{
    std::unique_lock<std::mutex> lock(the_mutex);
    if (!writingLoopActive)
    {
        writingLoopActive = true;
        writer_thread = std::thread(&Logger::writing_loop, this);
    }
}

void Logger::stop_write_loop()
{
    std::unique_lock<std::mutex> lock(the_mutex);
    msg_queue.cancel();
    // rejoin thread
    writer_thread.join();
    writingLoopActive = false;
}

void Logger::writing_loop()
{
    try
    {
        while (true)
        {
            // The pending_write flag prevents Logger::flush()
            // from returning prematurely.
            std::string* msg = msg_queue.pop();
            pending_write = true;
            write_msg(*msg);
            pending_write = false;
            delete msg;
        }
    }
    catch (concurrent_queue< std::string* >::Canceled &e)
    {
        pending_write = false;
        return;
    }
}

void Logger::flush()
{
    // There is a timing window between when pending_write is set,
    // and the msg_queue being empty. We could fall through that
    // window. Yes, its stupid, but too low-importance to fix.
    // try to work around it by sleeping.
    usleep(10);

    // Perhaps we could do this with semaphores, but this is not
    // really critical code, so a busy-wait is good enough.
    while (pending_write or not msg_queue.is_empty())
    {
        usleep(100);
    }

    // Force a write to the disk. Don't need to update metadata, though.
    if (logfile) fdatasync(fileno(logfile));
}

void Logger::write_msg(const std::string &msg)
{
    std::unique_lock<std::mutex> lock(the_mutex);
    // Delay opening the file until the first logging statement is issued;
    // this allows us to set the main logger's filename without creating
    // a useless log file with the default filename.
    if (logfile == NULL)
    {
        if ((logfile = fopen(fileName.c_str(), "a")) == NULL)
        {
            fprintf(stderr, "[ERROR] Unable to open log file \"%s\"\n",
                    fileName.c_str());
            lock.unlock();
            disable();
            return;
        }

        enable();
    }

    // Write to file.
    fprintf(logfile, "%s", msg.c_str());

    // Flush, because log messages are important, especially if we
    // are about to crash. So we don't want to have these buffered up.
    fflush(logfile);

    // Stdout writing must be unlocked.
    lock.unlock();

    // Write to stdout.
    if (printToStdout)
    {
        std::cout << msg;
        std::cout.flush();
    }
}

Logger::Logger(const std::string &fname, Logger::Level level, bool tsEnabled)
    : error(*this), warn(*this), info(*this), debug(*this), fine(*this)
{
    this->fileName.assign(fname);
    this->currentLevel = level;
    this->backTraceLevel = ERROR;

    this->timestampEnabled = tsEnabled;
    this->printToStdout = false;
    this->printLevel = true;
    this->syncEnabled = false;

    this->logEnabled = true;
#ifdef HAVE_VALGRIND
    DRD_IGNORE_VAR(this->logEnabled);
    DRD_IGNORE_VAR(this->msg_queue);
#endif
    this->logfile = NULL;
    this->pending_write = false;
    this->writingLoopActive = false;

    start_write_loop();
}

Logger::Logger(const Logger& log)
    : error(*this), warn(*this), info(*this), debug(*this), fine(*this)
{
    set(log);
}

Logger& Logger::operator=(const Logger& log)
{
    this->stop_write_loop();
    msg_queue.cancel_reset();
    this->set(log);
    return *this;
}

void Logger::set(const Logger& log)
{
    std::unique_lock<std::mutex> lock(the_mutex);
    this->fileName.assign(log.fileName);
    this->component.assign(log.component);
    this->currentLevel = log.currentLevel;
    this->printLevel = log.printLevel;
    this->backTraceLevel = log.backTraceLevel;
    this->timestampEnabled = log.timestampEnabled;
    this->printToStdout = log.printToStdout;
    this->syncEnabled = log.syncEnabled;
    this->logEnabled = log.logEnabled;

    // Set NULL to force the logger to use a new FILE handle. It is
    // safer that way because closing that file may not close file of
    // the parent logger.
    this->logfile = NULL;

    this->pending_write = false;
    this->writingLoopActive = false;

    lock.unlock();

    start_write_loop();
}

// ***********************************************/
// API

void Logger::set_level(Logger::Level newLevel)
{
    currentLevel = newLevel;
}

Logger::Level Logger::get_level() const
{
    return currentLevel;
}

void Logger::set_backtrace_level(Logger::Level newLevel)
{
    backTraceLevel = newLevel;
}

Logger::Level Logger::get_backtrace_level() const
{
    return backTraceLevel;
}

void Logger::set_filename(const std::string& s)
{
    fileName.assign(s);

    std::unique_lock<std::mutex> lock(the_mutex);
    if (logfile != NULL) fclose(logfile);
    logfile = NULL;
    lock.unlock();

    enable();
}

const std::string& Logger::get_filename() const
{
    return fileName;
}

void Logger::set_component(const std::string& c)
{
    component = c;
}

const std::string& Logger::get_component() const
{
    return component;
}

void Logger::set_timestamp_flag(bool flag)
{
    timestampEnabled = flag;
}

void Logger::set_print_to_stdout_flag(bool flag)
{
    printToStdout = flag;
}

void Logger::set_print_level_flag(bool flag)
{
    printLevel = flag;
}

void Logger::set_sync_flag(bool flag)
{
    syncEnabled = flag;
}

void Logger::set_print_error_level_stdout()
{
    set_print_to_stdout_flag(true);
    set_level(Logger::ERROR);
}

void Logger::enable()
{
    logEnabled = true;
}

void Logger::disable()
{
    logEnabled = false;
}

void Logger::log(Logger::Level level, const std::string &txt)
{
    static const unsigned int max_queue_size_allowed = 1024;
    // Don't log if not enabled, or level is too low.
    if (!logEnabled) return;
    if (level > currentLevel) return;

    std::ostringstream oss;
    if (timestampEnabled)
    {
        struct timeval stv;
        struct tm stm;
        char timestamp[64];
        char timestampStr[256];


        ::gettimeofday(&stv, NULL);
        time_t t = stv.tv_sec;
        gmtime_r(&t, &stm);
        strftime(timestamp, sizeof(timestamp), "%F %T", &stm);
        snprintf(timestampStr, sizeof(timestampStr),
                "[%s:%03d] ",timestamp, stv.tv_usec / 1000);
        oss << timestampStr;
    }

    if (printLevel)
        oss << "[" << get_level_string(level) << "] ";

    if (!component.empty())
        oss << "[" << component << "] ";

    oss << txt << std::endl;

    if (level <= backTraceLevel)
    {
#ifndef CYGWIN
        prt_backtrace(oss);
#endif
    }

    msg_queue.push(new std::string(oss.str()));

    // If the queue gets too full, block until it's flushed to file or
    // stdout. This can sometimes happen, if some component is spewing
    // lots of debugging messages in a tight loop.
    if (msg_queue.size() > max_queue_size_allowed) flush();

    // Errors are associated with immenent crashes. Make sure that the
    // stack trace is written to disk *before* the crash happens! Yes,
    // this introduces latency and lag. Tough. Don't generate errors.
    if (level <= backTraceLevel) flush();

    if (syncEnabled) flush();
}

void Logger::backtrace()
{
    static const unsigned int max_queue_size_allowed = 1024;
    std::ostringstream oss;

    #ifndef CYGWIN
    prt_backtrace(oss);
    #endif

    msg_queue.push(new std::string(oss.str()));

    // If the queue gets too full, block until it's flushed to file or
    // stdout. This can sometimes happen, if some component is spewing
    // lots of debugging messages in a tight loop.
    if (msg_queue.size() > max_queue_size_allowed) {
        flush();
    }
}

void Logger::logva(Logger::Level level, const char *fmt, va_list args)
{
    if (level <= currentLevel) {
        char buffer[MAX_PRINTF_STYLE_MESSAGE_SIZE];
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        std::string msg = buffer;
        log(level, msg);
    }
}

void Logger::log(Logger::Level level, const char *fmt, ...)
{
    va_list args; va_start(args, fmt); logva(level, fmt, args); va_end(args);
}

void Logger::Error::operator()(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); logger.logva(ERROR, fmt, args); va_end(args);
}

void Logger::Warn::operator()(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); logger.logva(WARN,  fmt, args); va_end(args);
}

void Logger::Info::operator()(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); logger.logva(INFO,  fmt, args); va_end(args);
}

void Logger::Debug::operator()(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); logger.logva(DEBUGGER, fmt, args); va_end(args);
}

void Logger::Fine::operator()(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); logger.logva(FINE,  fmt, args); va_end(args);
}

const char* Logger::get_level_string(const Logger::Level level)
{
    if (level == BAD_LEVEL)
        return "Bad level";
    else
        return levelStrings[level];
}

Logger::Level Logger::get_level_from_string(const std::string& levelStr)
{
    unsigned int nLevels = sizeof(levelStrings) / sizeof(levelStrings[0]);
    const char* lstr = levelStr.c_str();
    for (unsigned int i = 0; i < nLevels; ++i) {
        if (0 == strcasecmp(lstr, levelStrings[i]))
            return (Logger::Level) i;
    }
    return BAD_LEVEL;
}

// Create and return the single instance
Logger& opencog::logger()
{
    static Logger instance;
    return instance;
}
