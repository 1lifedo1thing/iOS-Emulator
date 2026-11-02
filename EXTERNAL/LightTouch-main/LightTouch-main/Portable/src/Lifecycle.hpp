#pragma once
namespace lt {
enum class TerminalIssue { none, engine_exit, unconfirmed_shutdown, dirty_state };
inline TerminalIssue terminal_issue(bool done, bool publishing, bool confirmed, bool dirty, int exit_code) {
    if (exit_code != 0)
        return TerminalIssue::engine_exit;
    if (!done)
        return TerminalIssue::none;
    if (!confirmed)
        return TerminalIssue::unconfirmed_shutdown;
    if (!publishing && dirty)
        return TerminalIssue::dirty_state;
    return TerminalIssue::none;
}
} // namespace lt
