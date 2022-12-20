////////////////////////////////////////////////////////////////////////////////
// Copyright 2019-2020 Lawrence Livermore National Security, LLC and other
// DiHydrogen Project Developers. See the top-level LICENSE file for details.
//
// SPDX-License-Identifier: Apache-2.0
////////////////////////////////////////////////////////////////////////////////

#pragma once
#ifndef H2_UTILS_LOGGER_HPP_INCLUDED
#define H2_UTILS_LOGGER_HPP_INCLUDED

#include "spdlog/pattern_formatter.h"
#include "spdlog/spdlog.h"

namespace h2
{

class Logger
{
public:

  enum LogLevelType : unsigned char {
        TRACE = 0x1,
        DEBUG = 0x2,
        INFO = 0x4,
        WARN = 0x8,
        ERROR = 0x10,
        CRITICAL = 0x20,
        OFF = 0x40,
        //FIXME: Or set as default?
        ALL = 0x7F
    };// enum class LogLevelType

    /** @brief Logger constructor.
     *  @param std::string name Name of logger.
     *  Default = [<Date> <Time> <Timezone>] [<Hostname> <Rank>] [<Log Level>]
     *  @param std::string sink Logging sink.
     **/
     Logger(std::string name)
       : Logger(std::move(name), "stdout")
    {}
    Logger(std::string name, std::string sink,
         std::string pattern = "[%D %H:%M %z] [%h (Rank %w/%W)] [%^%L%$] %v");
    ~Logger() {}
    ::spdlog::logger& get() { return *m_logger; }

    void load_levels(const char* input);
    void set_log_level(std::vector<LogLevelType> levels);
    void set_mask(unsigned char mask);

    bool should_log(LogLevelType level) const noexcept;

private:

    std::shared_ptr<::spdlog::logger> m_logger;
    unsigned char m_mask;

}; // class Logger
} // namespace h2

#endif // H2_UTILS_LOGGER_HPP_INCLUDED
