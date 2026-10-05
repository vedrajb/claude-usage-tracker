// sources.cpp - Usage data acquisition (blocking; runs on the worker thread).
//  1. OAuth: WinHTTP GET with the bearer token from ~/.claude/.credentials.json.
//  2. Fallback: run the `claude` CLI in a ConPTY, answer its prompts, send /usage
//     and parse the screen text.
// Cancellation closes OS handles so blocked calls return immediately.
#include "sources.h"

#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Token hygiene: overwrite secrets in memory as soon as they are no longer needed.
// SecureZeroMemory (unlike memset) cannot be optimised away by the compiler.
void wipe(std::string& s)
{
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
}

void wipe(std::wstring& s)
{
    if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
}

fs::path homeDir()
{
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p)) && p) {
        fs::path r(p);
        CoTaskMemFree(p);
        return r;
    }
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, ARRAYSIZE(buf));
    if (n > 0 && n < ARRAYSIZE(buf)) return fs::path(buf);
    throw std::runtime_error("Cannot determine user profile directory");
}

// RAII for kernel HANDLEs (null and INVALID_HANDLE_VALUE both mean "empty").
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE x = nullptr)
    {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = x;
    }
    HANDLE get() const { return h; }
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};

// RAII for WinHTTP handles that registers them with the CancelToken. Ownership
// rule: whoever removes the handle from the token's list closes it - either
// cancel() (closes all tracked handles) or this destructor (release() returned
// true). That prevents a double close if cancel races with normal cleanup.
struct InetHandle {
    HINTERNET h = nullptr;
    CancelToken* c = nullptr;
    explicit InetHandle(CancelToken* t) : c(t) {}
    InetHandle(const InetHandle&) = delete;
    InetHandle& operator=(const InetHandle&) = delete;
    ~InetHandle()
    {
        if (h && (!c || c->release(h))) WinHttpCloseHandle(h);
    }
    void set(HINTERNET x)
    {
        h = x;
        if (h && c && !c->track(h)) {
            WinHttpCloseHandle(h);
            h = nullptr;
        }
    }
};

// Caps protect against runaway responses / terminal output.
constexpr size_t MAX_HTTP_BODY = 1024 * 1024;
constexpr size_t MAX_PTY_BUFFER = 256 * 1024;

std::string lower(const std::string& s)
{
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool isWs(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}


} // namespace

// Closing a WinHTTP handle from another thread is the supported way to abort a
// synchronous WinHTTP call in progress: the blocked call fails immediately. Done
// in reverse order (request, connection, session) as children must close first.
void CancelToken::cancel()
{
    std::lock_guard<std::mutex> lk(mu_);
    flag_.store(true);
    for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) WinHttpCloseHandle(*it);
    handles_.clear();
}

// Refuses new handles after cancel so a worker that races with cancel() closes
// its own handle and fails fast.
bool CancelToken::track(void* h)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (flag_.load()) return false;
    handles_.push_back(h);
    return true;
}

bool CancelToken::release(void* h)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find(handles_.begin(), handles_.end(), h);
    if (it == handles_.end()) return false;
    handles_.erase(it);
    return true;
}

// Reads the token with minimal exposure: the raw file text and the parsed JSON
// copy are zeroed (wipe(text), Scrub) - only the returned copy survives and
// is the caller's to wipe. Expired tokens are rejected up front (expiresAt is
// epoch ms; 0/absent means unknown) so we fall through to the PTY path.
std::string readAccessToken(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("No OAuth credentials found");
    std::streamsize len = in.tellg();
    if (len <= 0 || len > 1024 * 1024) throw std::runtime_error("No OAuth credentials found");
    std::string text(static_cast<size_t>(len), '\0');
    in.seekg(0);
    in.read(text.data(), len);
    if (!in) {
        wipe(text);
        throw std::runtime_error("No OAuth credentials found");
    }
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    wipe(text);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error("No OAuth credentials found");
    auto it = j.find("claudeAiOauth");
    if (it == j.end() || !it->is_object()) throw std::runtime_error("No OAuth credentials found");
    auto tok = it->find("accessToken");
    if (tok == it->end() || !tok->is_string() || tok->get_ref<const std::string&>().empty())
        throw std::runtime_error("No OAuth credentials found");
    std::string* stored = tok->get_ptr<std::string*>();
    struct Scrub { std::string* p; ~Scrub() { wipe(*p); } } scrub{stored};
    auto exp = it->find("expiresAt");
    if (exp != it->end() && exp->is_number() && exp->get<double>() != 0 &&
        exp->get<double>() <= static_cast<double>(nowMs()))
        throw std::runtime_error("OAuth token expired");
    return *stored;
}

std::string readAccessToken()
{
    return readAccessToken(homeDir() / ".claude" / ".credentials.json");
}

// The CLI asks to trust the working folder on first run in a new directory.
bool detectTrustPrompt(const std::string& stripped)
{
    std::string l = lower(stripped);
    return l.find("yes, i trust this folder") != std::string::npos &&
           l.find("enter to confirm") != std::string::npos;
}

// The CLI has no stable "ready" signal, so accept any of: the prompt glyph,
// the "? for shortcuts" hint, or a line ending in ">" (plain-terminal prompt).
// Hand-written scans instead of regex to mirror the JS patterns without the cost.
bool detectReadyPrompt(const std::string& stripped)
{
    if (stripped.find("\xE2\x9D\xAF") != std::string::npos) return true;

    const std::string needle = "for shortcuts";
    size_t pos = 0;
    while ((pos = stripped.find('?', pos)) != std::string::npos) {
        size_t i = pos + 1;
        while (i < stripped.size() && isWs(stripped[i])) ++i;
        if (i > pos + 1 && stripped.compare(i, needle.size(), needle) == 0) return true;
        ++pos;
    }

    size_t start = 0;
    while (start <= stripped.size()) {
        size_t end = stripped.find('\n', start);
        if (end == std::string::npos) end = stripped.size();
        size_t e = end;
        while (e > start && isWs(stripped[e - 1])) --e;
        if (e > start && stripped[e - 1] == '>') return true;
        if (end == stripped.size()) break;
        start = end + 1;
    }
    return false;
}

// The Authorization header is built directly into a wstring that is wiped on
// every exit path (Wiper) and right after the request is sent, so the token
// lives in as few places as possible; the narrow copy is wiped immediately.
UsageData fetchViaOAuth(CancelToken* cancel)
{
    static const wchar_t prefix[] = L"Authorization: Bearer ";
    static const wchar_t suffix[] = L"\r\nanthropic-beta: oauth-2025-04-20\r\n";
    std::wstring headers;
    struct Wiper { std::wstring& s; ~Wiper() { wipe(s); } } wiper{headers};
    {
        std::string token = readAccessToken();
        size_t plen = ARRAYSIZE(prefix) - 1, slen = ARRAYSIZE(suffix) - 1;
        headers.resize(plen + token.size() + slen);
        wmemcpy(headers.data(), prefix, plen);
        int n = token.empty() ? 0
                              : MultiByteToWideChar(CP_UTF8, 0, token.data(), static_cast<int>(token.size()),
                                                    headers.data() + plen, static_cast<int>(token.size()));
        wipe(token);
        if (n <= 0) throw std::runtime_error("Invalid OAuth token");
        wmemcpy(headers.data() + plen + n, suffix, slen);
        headers.resize(plen + static_cast<size_t>(n) + slen);
    }

    // Handles are tracked so cancel() can abort any blocking WinHTTP call below.
    InetHandle session(cancel), conn(cancel), req(cancel);
    session.set(WinHttpOpen(L"claude-usage-tracker", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    // Automatic proxy (WPAD/PAC) can fail on locked-down machines; retry with the
    // default system proxy unless we were cancelled.
    if (!session.h && !(cancel && cancel->cancelled()))
        session.set(WinHttpOpen(L"claude-usage-tracker", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) throw std::runtime_error("WinHttpOpen failed");
    // resolve, connect, send, receive (ms): bounds each stage so the worker cannot hang.
    WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 15000);

    conn.set(WinHttpConnect(session.h, L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!conn.h) throw std::runtime_error("WinHttpConnect failed");
    req.set(WinHttpOpenRequest(conn.h, L"GET", L"/api/oauth/usage", nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) throw std::runtime_error("WinHttpOpenRequest failed");

    if (!WinHttpSendRequest(req.h, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        throw std::runtime_error("HTTP request failed (error " + std::to_string(GetLastError()) + ")");
    wipe(headers);

    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("Failed to read HTTP status");
    if (status != 200) throw std::runtime_error("Usage endpoint returned HTTP " + std::to_string(status));

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) throw std::runtime_error("Failed to read response");
        if (avail == 0) break;
        if (body.size() + avail > MAX_HTTP_BODY) throw std::runtime_error("Response too large");
        size_t old = body.size();
        body.resize(old + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, body.data() + old, avail, &got))
            throw std::runtime_error("Failed to read response");
        body.resize(old + got);
    }
    return parseOAuthUsage(nlohmann::json::parse(body));
}

namespace {

// Everything for one pseudo-console run. Members are declared so that the
// destructor can tear down in a safe order (see ~PtySession).
struct PtySession {
    Handle inRead, inWrite, outRead, outWrite, process, thread, job;
    HPCON pc = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    std::string attrStorage;
    std::mutex mu;
    std::string buffer;
    size_t total = 0;
    std::thread reader;
    std::atomic<bool> readerDone{false};

    // Cleanup order matters:
    //  1. kill the process tree (job) so nothing writes to the console any more;
    //  2. close our input end, then ClosePseudoConsole, which flushes and ends the
    //     output pipe - the reader normally sees EOF by itself;
    //  3. close the write end of the output pipe we hold, else ReadFile never EOFs;
    //  4. if the reader is still blocked in ReadFile, CancelSynchronousIo breaks
    //     it (retried, since it only works while the call is pending) before
    //     closing the read handle and joining. Closing a handle under a blocked
    //     ReadFile would be unsafe, hence cancel first.
    // The process/job handles go last; closing the job also kills stragglers
    // (KILL_ON_JOB_CLOSE).
    ~PtySession()
    {
        if (job) TerminateJobObject(job.get(), 1);
        if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) TerminateProcess(process.get(), 1);
        inWrite.reset();
        if (pc) ClosePseudoConsole(pc);
        outWrite.reset();
        if (reader.joinable()) {
            ULONGLONG t0 = GetTickCount64();
            while (!readerDone.load() && GetTickCount64() - t0 < 1000) Sleep(10);
            for (int i = 0; i < 40 && !readerDone.load(); ++i) {
                CancelSynchronousIo(reader.native_handle());
                Sleep(50);
            }
            outRead.reset();
            reader.join();
        }
        outRead.reset();
        inRead.reset();
        if (attrs) DeleteProcThreadAttributeList(attrs);
        thread.reset();
        process.reset();
        job.reset();
    }

    void write(const char* s)
    {
        DWORD n = 0;
        WriteFile(inWrite.get(), s, static_cast<DWORD>(strlen(s)), &n, nullptr);
    }

    std::string snapshot(size_t& seen)
    {
        std::lock_guard<std::mutex> lk(mu);
        seen = total;
        return buffer;
    }

    size_t received()
    {
        std::lock_guard<std::mutex> lk(mu);
        return total;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mu);
        buffer.clear();
    }
};

// Sleeps in 50 ms slices so cancellation is noticed quickly. Returns false if cancelled.
bool sleepCancelable(int ms, CancelToken* cancel)
{
    for (int waited = 0; waited < ms; waited += 50) {
        if (cancel && cancel->cancelled()) return false;
        Sleep(50);
    }
    return !(cancel && cancel->cancelled());
}

} // namespace

// Drives the CLI through a ConPTY. Any failure throws; PtySession's destructor
// always cleans up the process and threads.
UsageData fetchViaPty(const fs::path& cwd, CancelToken* cancel)
{
    std::error_code ec;
    fs::create_directories(cwd, ec);
    if (ec) throw std::runtime_error("Cannot create working directory: " + ec.message());

    PtySession s;
    // Two anonymous pipes: we write keystrokes to the console input and read the
    // rendered output. Non-inheritable; the pseudo console duplicates what it needs.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    HANDLE a = nullptr, b = nullptr;
    if (!CreatePipe(&a, &b, &sa, 0)) throw std::runtime_error("CreatePipe failed");
    s.inRead.reset(a);
    s.inWrite.reset(b);
    if (!CreatePipe(&a, &b, &sa, 0)) throw std::runtime_error("CreatePipe failed");
    s.outRead.reset(a);
    s.outWrite.reset(b);

    if (FAILED(CreatePseudoConsole({140, 50}, s.inRead.get(), s.outWrite.get(), 0, &s.pc))) {
        s.pc = nullptr;
        throw std::runtime_error("CreatePseudoConsole failed");
    }

    // The pseudo console is attached to the child through a process-thread
    // attribute list (sized by a first, intentionally failing, call). 140x50 is
    // wide enough that the /usage screen does not wrap.
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    s.attrStorage.resize(attrSize);
    s.attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(s.attrStorage.data());
    if (!InitializeProcThreadAttributeList(s.attrs, 1, 0, &attrSize)) {
        s.attrs = nullptr;
        throw std::runtime_error("InitializeProcThreadAttributeList failed");
    }
    if (!UpdateProcThreadAttribute(s.attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, s.pc, sizeof(HPCON),
                                   nullptr, nullptr))
        throw std::runtime_error("UpdateProcThreadAttribute failed");

    // Job object with KILL_ON_JOB_CLOSE: `cmd /c claude` spawns a tree (node etc.);
    // terminating only cmd would leak it. Closing the job (even if we crash)
    // kills every process in it.
    s.job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!s.job) throw std::runtime_error("CreateJobObject failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(s.job.get(), JobObjectExtendedLimitInformation, &jl, sizeof(jl)))
        throw std::runtime_error("SetInformationJobObject failed");

    // Run via cmd /c so the `claude` shim (.cmd) on PATH resolves.
    wchar_t comspec[MAX_PATH * 2];
    DWORD cn = GetEnvironmentVariableW(L"ComSpec", comspec, ARRAYSIZE(comspec));
    std::wstring shell = (cn > 0 && cn < ARRAYSIZE(comspec)) ? std::wstring(comspec) : L"cmd.exe";
    std::wstring cmd = L"\"" + shell + L"\" /c claude";

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = s.attrs;
    // STARTF_USESTDHANDLES with null std handles: stops the child inheriting our
    // (GUI process, usually invalid) stdio so it uses the pseudo console instead.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    // CREATE_SUSPENDED: the child must be in the job *before* it runs, otherwise it
    // could spawn children that escape the job.
    PROCESS_INFORMATION pi{};
    std::wstring cwdW = cwd.wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, cwdW.c_str(),
                        &si.StartupInfo, &pi))
        throw std::runtime_error("CreateProcess failed (error " + std::to_string(GetLastError()) + ")");
    s.process.reset(pi.hProcess);
    s.thread.reset(pi.hThread);
    if (!AssignProcessToJobObject(s.job.get(), pi.hProcess))
        throw std::runtime_error("AssignProcessToJobObject failed");
    ResumeThread(pi.hThread);

    // The pseudo console now owns its ends; drop ours so EOF is observable once
    // the console closes.
    s.inRead.reset();
    s.outWrite.reset();

    // Reader thread: blocking ReadFile into a bounded buffer (keeps the newest
    // output). `total` only grows, so the poll loop can detect new output even
    // after the buffer has been trimmed.
    s.reader = std::thread([&s] {
        char tmp[4096];
        DWORD n = 0;
        while (ReadFile(s.outRead.get(), tmp, sizeof(tmp), &n, nullptr) && n > 0) {
            std::lock_guard<std::mutex> lk(s.mu);
            s.buffer.append(tmp, n);
            s.total += n;
            if (s.buffer.size() > MAX_PTY_BUFFER) s.buffer.erase(0, s.buffer.size() - MAX_PTY_BUFFER);
        }
        s.readerDone.store(true);
    });

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(30000);
    // Poll state machine: [trust prompt -> confirm] -> wait for ready prompt ->
    // type /usage, pause, press Enter -> parse until output is parseable.
    // Overall 30 s timeout.
    bool sentUsage = false, sentSubmit = false;
    size_t lastSeen = static_cast<size_t>(-1);

    for (;;) {
        if (!sleepCancelable(500, cancel)) throw std::runtime_error("Cancelled");
        if (WaitForSingleObject(s.process.get(), 0) == WAIT_OBJECT_0)
            throw std::runtime_error("claude exited before /usage was parsed");
        if (clock::now() >= deadline) throw std::runtime_error("Timed out waiting for /usage output");

        if (s.received() == lastSeen) continue;
        std::string buf = s.snapshot(lastSeen);
        std::string text = stripAnsi(buf);
        if (!sentUsage && detectTrustPrompt(text)) {
            s.write("\x1b[B");
            if (!sleepCancelable(300, cancel)) throw std::runtime_error("Cancelled");
            s.write("\r");
            s.clear();
            continue;
        }
        if (!sentUsage && text.size() > 200 && detectReadyPrompt(text)) {
            sentUsage = true;
            // Text and Enter are sent separately with a pause so the CLI treats the
            // Enter as a keypress (submit) rather than part of pasted text.
            s.write("/usage");
            if (!sleepCancelable(800, cancel)) throw std::runtime_error("Cancelled");
            s.write("\r");
            sentSubmit = true;
            continue;
        }
        if (sentSubmit) {
            // Output arrives incrementally; parse failures just mean "not there yet".
            try {
                return parseUsageText(buf, nowMs());
            } catch (...) {
            }
        }
    }
}

// OAuth is fast and silent; the PTY launches a process, so it is only the
// fallback. Both error messages are kept for diagnosis.
UsageData fetchUsage(const fs::path& cwd, CancelToken* cancel)
{
    std::string oauthErr;
    try {
        UsageData d = fetchViaOAuth(cancel);
        d.source = "oauth";
        return d;
    } catch (const std::exception& e) {
        oauthErr = e.what();
    }
    if (cancel && cancel->cancelled()) throw std::runtime_error("Cancelled");
    // No credentials at all means the CLI is logged out: report it right away
    // instead of spawning a PTY that can only time out. (An expired token still
    // falls through, since launching the CLI can refresh it.)
    if (oauthErr.find("No OAuth credentials") != std::string::npos) throw std::runtime_error("oauth: " + oauthErr);
    try {
        UsageData d = fetchViaPty(cwd, cancel);
        d.source = "pty";
        return d;
    } catch (const std::exception& e) {
        throw std::runtime_error("oauth: " + oauthErr + "; pty: " + e.what());
    }
}
