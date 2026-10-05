#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "parser.h"

// sources.h - Usage data acquisition: OAuth HTTPS API (preferred) with a ConPTY
// fallback that drives the `claude` CLI's /usage screen. Both are blocking and
// meant for the worker thread; CancelToken lets the UI abort them promptly.

/// Cooperative cancellation shared between the UI and a worker. Besides a flag it
/// tracks in-flight OS handles (HTTP/process/pipe) so cancel() can close/interrupt
/// them and unblock waits that a plain flag could not.
class CancelToken {
public:
    /// True once cancel() was called.
    bool cancelled() const { return flag_.load(); }
    /// Sets the flag and aborts all tracked handles; thread-safe, idempotent.
    void cancel();
    /// Registers a handle to abort on cancel; false if already cancelled.
    bool track(void* h);
    /// Unregisters; true if the caller still owns (and must close) the handle.
    bool release(void* h);

private:
    std::atomic<bool> flag_{false};
    std::mutex mu_;
    std::vector<void*> handles_;
};

/// Reads the Claude OAuth access token from the default credentials file ("" if absent).
std::string readAccessToken();
/// Same, from an explicit file (used by tests).
std::string readAccessToken(const std::filesystem::path& file);

/// Screen-text detectors for the CLI's folder-trust dialog and the ready prompt (input is ANSI-stripped).
bool detectTrustPrompt(const std::string& stripped);
bool detectReadyPrompt(const std::string& stripped);

/// Queries the OAuth usage endpoint. Throws std::runtime_error on failure/cancel.
UsageData fetchViaOAuth(CancelToken* cancel = nullptr);
/// Runs `claude` in a pseudo console in `cwd` and parses /usage. Throws on failure/timeout/cancel.
UsageData fetchViaPty(const std::filesystem::path& cwd, CancelToken* cancel = nullptr);
/// Tries OAuth first, then the PTY fallback. Throws if both fail.
UsageData fetchUsage(const std::filesystem::path& cwd, CancelToken* cancel = nullptr);
