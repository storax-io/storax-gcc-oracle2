// storax-gcc-oracle2 — the C++26 compile oracle, written in C++23.
//
// Pure C++23: no third-party libraries, no interpreter, no shell at
// runtime. The server IS a native binary that fork/execs g++ 16.1
// (reflection + contracts) directly. It judges C++ and is C++.
//
// Design notes worth keeping:
//   * REST routing is COMPILE-TIME: a consteval FNV-1a hash turns each
//     "METHOD PATH" into a uint64 the dispatch switches on — no string
//     comparisons at request time, the route table is fixed in the binary.
//   * JSON is hand-rolled (a fixed, tiny schema in; escaped strings out) —
//     the only correctness-critical part is string un/escaping, which
//     carries the model's C++ source verbatim.
//   * jobs compile in /dev/shm; the compiler stays warm in page cache.
//
// No security hardening here by design (trusted network). Execution
// sandboxing / async job pointers land next, matching oracle1.
//
//   GET  /health   -> {ok, version, reflection, jobs_done}
//   POST /compile  {files:{name:src}, args:[...], main, run, timeout}
//                  -> {ok, rc, stdout, stderr, ms, run_rc, run_stdout, ...}

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static std::string GXX  = getenv("ORACLE_GXX") ? getenv("ORACLE_GXX")
                                               : "/opt/gcc-16.1/bin/g++";
static std::string LIB64;
static std::string WORK = getenv("ORACLE_WORKDIR") ? getenv("ORACLE_WORKDIR")
                                                   : "/dev/shm/oracle2";
static int PORT = getenv("ORACLE_PORT") ? atoi(getenv("ORACLE_PORT")) : 8951;
static constexpr std::size_t MAX_OUTPUT = 256 * 1024;
static std::atomic<long> g_jobs{0};

// --- compile-time REST routing -----------------------------------------
constexpr std::uint64_t fnv1a(std::string_view s) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}
// consteval so the route table is guaranteed fixed in the binary
consteval std::uint64_t route(std::string_view s) { return fnv1a(s); }

// --- tiny JSON (hand-rolled; no library) --------------------------------
struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false; double num = 0; std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;   // ordered (files!)
    const JVal* find(std::string_view k) const {
        for (auto& [key, v] : obj) if (key == k) return &v;
        return nullptr;
    }
};

struct JParser {
    std::string_view s; size_t i = 0;
    void ws() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++; }
    JVal parse() { ws(); return val(); }
    JVal val() {
        ws();
        if (i >= s.size()) return {};
        char c = s[i];
        if (c == '{') return obj();
        if (c == '[') return arr();
        if (c == '"') { JVal v; v.t = JVal::STR; v.str = str(); return v; }
        if (c == 't') { i += 4; JVal v; v.t = JVal::BOOL; v.b = true; return v; }
        if (c == 'f') { i += 5; JVal v; v.t = JVal::BOOL; v.b = false; return v; }
        if (c == 'n') { i += 4; return {}; }
        return num();
    }
    std::string str() {
        std::string o; i++;                     // opening quote
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { o += c; continue; }
            char e = s[i++];
            switch (e) {
                case 'n': o += '\n'; break; case 't': o += '\t'; break;
                case 'r': o += '\r'; break; case 'b': o += '\b'; break;
                case 'f': o += '\f'; break; case '/': o += '/'; break;
                case '"': o += '"'; break;  case '\\': o += '\\'; break;
                case 'u': {
                    int cp = (int)strtol(std::string(s.substr(i, 4)).c_str(),
                                         nullptr, 16); i += 4;
                    if (cp < 0x80) o += (char)cp;
                    else if (cp < 0x800) {
                        o += (char)(0xC0 | (cp >> 6));
                        o += (char)(0x80 | (cp & 0x3F));
                    } else {
                        o += (char)(0xE0 | (cp >> 12));
                        o += (char)(0x80 | ((cp >> 6) & 0x3F));
                        o += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: o += e;
            }
        }
        i++;                                     // closing quote
        return o;
    }
    JVal num() {
        size_t j = i;
        while (i < s.size() && (isdigit(s[i]) || strchr("+-.eE", s[i]))) i++;
        JVal v; v.t = JVal::NUM;
        v.num = strtod(std::string(s.substr(j, i - j)).c_str(), nullptr);
        return v;
    }
    JVal arr() {
        JVal v; v.t = JVal::ARR; i++; ws();
        while (i < s.size() && s[i] != ']') {
            v.arr.push_back(val()); ws();
            if (i < s.size() && s[i] == ',') { i++; ws(); }
        }
        i++; return v;
    }
    JVal obj() {
        JVal v; v.t = JVal::OBJ; i++; ws();
        while (i < s.size() && s[i] != '}') {
            std::string k = str(); ws();
            if (i < s.size() && s[i] == ':') i++;
            v.obj.emplace_back(k, val()); ws();
            if (i < s.size() && s[i] == ',') { i++; ws(); }
        }
        i++; return v;
    }
};

static void esc(std::string& o, std::string_view s) {
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
}
static std::string jstr(std::string_view s) {
    std::string o = "\""; esc(o, s); o += '"'; return o;
}

// --- subprocess capture with timeout -----------------------------------
struct RunResult { int rc; std::string out, err; bool timed_out; };

static RunResult run_capture(const std::vector<std::string>& argv,
                             const std::string& cwd,
                             const std::vector<std::string>& env,
                             int timeout_ms) {
    int op[2], ep[2];
    if (pipe(op) || pipe(ep)) return {-1, "", "pipe failed", false};
    pid_t pid = fork();
    if (pid < 0) return {-1, "", "fork failed", false};
    if (pid == 0) {
        dup2(op[1], 1); dup2(ep[1], 2);
        close(op[0]); close(op[1]); close(ep[0]); close(ep[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        std::vector<char*> a, e;
        for (auto& x : argv) a.push_back(const_cast<char*>(x.c_str()));
        a.push_back(nullptr);
        for (auto& x : env) e.push_back(const_cast<char*>(x.c_str()));
        e.push_back(nullptr);
        execve(argv[0].c_str(), a.data(), e.data());
        _exit(127);
    }
    close(op[1]); close(ep[1]);
    fcntl(op[0], F_SETFL, O_NONBLOCK); fcntl(ep[0], F_SETFL, O_NONBLOCK);
    std::string out, err;
    pollfd pf[2] = {{op[0], POLLIN, 0}, {ep[0], POLLIN, 0}};
    int elapsed = 0; const int step = 20; bool timed_out = false;
    char buf[8192];
    while (true) {
        poll(pf, 2, step);
        for (int k = 0; k < 2; k++) {
            ssize_t n; while ((n = read(pf[k].fd, buf, sizeof buf)) > 0)
                (k == 0 ? out : err).append(buf, n);
        }
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            for (int k = 0; k < 2; k++) {
                ssize_t n; while ((n = read(pf[k].fd, buf, sizeof buf)) > 0)
                    (k == 0 ? out : err).append(buf, n);
            }
            close(op[0]); close(ep[0]);
            return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, out, err, false};
        }
        elapsed += step;
        if (timeout_ms > 0 && elapsed >= timeout_ms) {
            kill(pid, SIGKILL); waitpid(pid, nullptr, 0); timed_out = true;
            break;
        }
    }
    close(op[0]); close(ep[0]);
    return {-1, out, err, timed_out};
}

static std::vector<std::string> base_env() {
    return {"PATH=/usr/bin:/bin", "LD_LIBRARY_PATH=" + LIB64};
}
static std::string cap(std::string s) {
    if (s.size() > MAX_OUTPUT) s = s.substr(0, MAX_OUTPUT) + "\n[oracle] truncated";
    return s;
}

// --- the one job -------------------------------------------------------
static std::string compile_job(const JVal& req) {
    const JVal* files = req.find("files");
    if (!files || files->t != JVal::OBJ || files->obj.empty())
        return R"({"ok":false,"error":"need files:{name:src}"})";

    std::string main_file;
    if (const JVal* m = req.find("main")) main_file = m->str;
    else if (files->obj.size() == 1) main_file = files->obj[0].first;
    else return R"({"ok":false,"error":"multi-file needs main"})";

    bool run = false;
    if (const JVal* r = req.find("run")) run = r->b;
    int timeout_ms = 60000;
    if (const JVal* t = req.find("timeout")) timeout_ms = int(t->num * 1000);

    std::error_code ec;
    std::string d = WORK + "/job-" + std::to_string(getpid()) + "-" +
                    std::to_string(g_jobs.fetch_add(1));
    fs::create_directories(d, ec);
    for (auto& [name, src] : files->obj)
        std::ofstream(d + "/" + fs::path(name).filename().string()) << src.str;
    std::string mbase = fs::path(main_file).filename().string();

    std::vector<std::string> argv = {GXX};
    if (const JVal* a = req.find("args"); a && a->t == JVal::ARR)
        for (auto& x : a->arr) argv.push_back(x.str);
    else
        for (auto* x : {"-std=c++26", "-freflection", "-fcontracts",
                        "-fcontract-evaluation-semantic=enforce",
                        "-Wall", "-Wextra"}) argv.push_back(x);
    if (run) { argv.push_back(mbase); argv.push_back("-o"); argv.push_back("a.out"); }
    else     { argv.push_back("-fsyntax-only"); argv.push_back(mbase); }

    auto t0 = std::chrono::steady_clock::now();
    RunResult c = run_capture(argv, d, base_env(), timeout_ms);
    bool ok = (c.rc == 0 && !c.timed_out);

    std::string o = "{";
    o += "\"rc\":" + std::to_string(c.rc);
    o += ",\"stdout\":" + jstr(cap(c.out));
    o += ",\"stderr\":" + jstr(cap(c.err) + (c.timed_out ? "\n[oracle] compile timeout" : ""));
    if (ok && run) {
        RunResult r = run_capture({d + "/a.out"}, d, base_env(),
                                  std::max(5000, timeout_ms / 2));
        o += ",\"run_rc\":" + std::to_string(r.rc);
        o += ",\"run_stdout\":" + jstr(cap(r.out));
        o += ",\"run_stderr\":" + jstr(cap(r.err) + (r.timed_out ? "\n[oracle] run timeout" : ""));
        ok = (r.rc == 0 && !r.timed_out);
    }
    auto t1 = std::chrono::steady_clock::now();
    o += ",\"ms\":" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    o += ",\"ok\":" + std::string(ok ? "true" : "false") + "}";
    fs::remove_all(d, ec);
    return o;
}

static std::string health() {
    RunResult v = run_capture({GXX, "--version"}, "", base_env(), 10000);
    std::string ver = v.out.substr(0, v.out.find('\n'));
    JParser jp{R"({"files":{"p.cpp":"#include <meta>\nstatic_assert(^^int==^^int); using T=[:^^int:]; static_assert(__is_same(T,int));\nint main(){}\n"}})"};
    JVal probe = jp.parse();
    std::string pr = compile_job(probe);
    bool refl = pr.find("\"ok\":true") != std::string::npos;
    std::string o = "{\"ok\":";
    o += refl ? "true" : "false";
    o += ",\"version\":" + jstr(ver);
    o += ",\"reflection\":" + std::string(refl ? "true" : "false");
    o += ",\"jobs_done\":" + std::to_string(g_jobs.load()) + "}";
    return o;
}

// --- minimal HTTP/1.1 (thread per connection, keep-alive) --------------
static void reply(int fd, int code, const std::string& body) {
    std::string h = "HTTP/1.1 " + std::to_string(code) +
        " OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n";
    (void)!write(fd, h.data(), h.size());
    (void)!write(fd, body.data(), body.size());
}

static void handle(int fd) {
    std::string buf; char tmp[8192];
    while (true) {
        size_t he;
        while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
            ssize_t n = read(fd, tmp, sizeof tmp);
            if (n <= 0) { close(fd); return; }
            buf.append(tmp, n);
        }
        std::string head = buf.substr(0, he);
        size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
        std::string method = head.substr(0, sp1);
        std::string path = head.substr(sp1 + 1, sp2 - sp1 - 1);
        size_t cl = 0;
        if (size_t p = head.find("Content-Length:"); p != std::string::npos)
            cl = atol(head.c_str() + p + 15);
        std::string body = buf.substr(he + 4);
        while (body.size() < cl) {
            ssize_t n = read(fd, tmp, sizeof tmp);
            if (n <= 0) { close(fd); return; }
            body.append(tmp, n);
        }
        buf = body.substr(cl);

        switch (fnv1a(method + " " + path)) {       // compile-time route ids
            case route("GET /health"):
                reply(fd, 200, health()); break;
            case route("POST /compile"): {
                JParser jp{body}; JVal req = jp.parse();
                std::string r = compile_job(req);
                reply(fd, r.find("\"error\"") != std::string::npos ? 400 : 200, r);
                break;
            }
            default:
                reply(fd, 404, R"({"error":"GET /health or POST /compile"})");
        }
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    LIB64 = fs::path(GXX).parent_path().parent_path().string() + "/lib64";
    fs::create_directories(WORK);
    std::string h = health();
    fprintf(stderr, "oracle2 :%d  %s\n", PORT, h.c_str());
    if (h.find("\"reflection\":true") == std::string::npos) {
        fprintf(stderr, "REFUSING: reflection probe failed\n"); return 1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(PORT);
    if (bind(s, (sockaddr*)&a, sizeof a) || listen(s, 256)) {
        perror("bind/listen"); return 1;
    }
    while (true) {
        int c = accept(s, nullptr, nullptr);
        if (c >= 0) std::thread(handle, c).detach();
    }
}
