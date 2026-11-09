// SPDX-FileCopyrightText:  2021-2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shell/shell.h"
#include "shell/network_compat.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <regex>

#include <errno.h>

#if defined(WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include <lz4.h>

#include <vector>
#include <deque>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "config/config.h"
#include "cpu/callback.h"
#include "cpu/registers.h"
#include "dos/programs/more_output.h"
#include "dos/programs/setver.h"
#include "gui/common.h"
#include "gui/mapper.h"
#include "hardware/timer.h"
#include "misc/support.h"
#include "shell/autoexec.h"
#include "utils/fs_utils.h"
#include "utils/string_utils.h"

callback_number_t call_shellstop = 0;

// Larger scope so shell_del autoexec can use it to
// remove things from the environment
static DosShell* first_shell = nullptr;

DosShell* DOS_GetFirstShell()
{
	return first_shell;
}

constexpr uint16_t InvalidFileHandle = DOS_FILES;

static Bitu shellstop_handler()
{
	return CBRET_STOP;
}

std::unique_ptr<Program> SHELL_ProgramCreate()
{
	return ProgramCreate<DosShell>();
}

DosShell::DosShell()
{
	AddShellCmdsToHelpList();
	help_detail = {HELP_Filter::All,
	               HELP_Category::Misc,
	               HELP_CmdType::Program,
	               "COMMAND"};

	static std::weak_ptr<ShellHistory> global_shell_history = {};
	history = global_shell_history.lock();
	if (!history) {
		history              = std::make_shared<ShellHistory>();
		global_shell_history = history;
	}
}

// This function gets redirection targets from the given shell command line and
// returns the results as a struct. The results include the redirection targets
// as well as the processed command line with the targets stripped off.
//
// Note that real MS-DOS is quite nuanced in its whitespace handling:
// - 'echo 1>out.txt' produces a 3-byte file: '1CRLF'
// - 'echo 1 > out.txt' produces a 4-byte file: '1 CRLF'
// - 'echo 1 >out.txt ' produces a 5-byte file: '1  CRLF'
// This behavior is replicated here; see the shell_redirection_tests for more
// examples.
//
std::optional<DosShell::RedirectionResults> DosShell::GetRedirection(const std::string_view line)
{
	// Returned results and quick-access references to members.
	RedirectionResults results = {};
	auto& [processed_line, in_file, out_file, pipe_target, is_appending] = results;

	// The regex splits the line into quoted and redirection groups
	static const std::regex re(R"(("[^"]*"\s*)|([<>|]|>>|<<)\s*([^<>| ]+)(\s*))",
	                           std::regex::optimize);
	//
	//    ("[^"]*"\s*)  # Group 1: double-quoted text
	//    |             # -or-
	//    (             # Group 2: the redirection tokens, either
	//        [<>|]     # Single character tokens
	//        |         # -or-
	//        >>|<<     # Double character tokens
	//    )
	//    \s*           # Whitespace after the redirection token
	//    ([^<>| ]+)    # Group 3: target file or program name
	//    (\s*)        # Group 4: the target's trailing whitespace
	//
	// Named match indexes:
	enum { Quote = 1, Token = 2, Target = 3, Tail = 4 };

	// Match iterator (shorthand to avoid visual overload below)
	auto m = std::cregex_iterator(line.data(), line.data() + line.size(), re);
	auto m_end = std::cregex_iterator();

	// Tracks the end of the pre-matched portion in the line. This is used to
	// step forward as we iterate through the matches.
	size_t pre_match_end = 0;

	while (m != m_end) {
		// Pre-match
		// [ ... ]
		// This is the text that's neither quoted nor a redirection.
		const auto pre_match_len = m->position() - pre_match_end;
		const auto pre_match_line = line.substr(pre_match_end, pre_match_len);

		// The pre-matched content shouldn't contain any redirection tokens.
		// If it does, it would generate a syntax error under real MS-DOS,
		// so we do the same and bail-out with an empty string.
		if (pre_match_line.find_first_of("<>|") != std::string_view::npos) {
			return std::nullopt;
		}

		processed_line += pre_match_line;

		// Match content is the actual redirection token(s)
		const auto& match = *m;
		if (match[Quote].matched) {
			processed_line += match[Quote].str();
		} else if (match[Token].matched) {
			const std::string token = match[Token].str();
			if (token == ">" || token == ">>" || token == "<" || token == "|") {
				// Keep the token in the processed output unless it's a redirect.
				processed_line += token;
			}
		}
		if (match[Target].matched) {
			const std::string target = match[Target].str();
			if (match[Token].matched && match[Token].str() == ">") {
				out_file = target;
			} else if (match[Token].matched && match[Token].str() == ">>") {
				out_file = target;
				is_appending = true;
			} else if (match[Token].matched && match[Token].str() == "<") {
				in_file = target;
			} else if (match[Token].matched && match[Token].str() == "|") {
				pipe_target = target;
			}
		}
		pre_match_end = m->position() + m->length();
		++m;
	}

	processed_line += line.substr(pre_match_end);
	return results;
}
