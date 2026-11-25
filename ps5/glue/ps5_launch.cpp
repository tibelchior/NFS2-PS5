// Launch glue for the installed title. The console mounts the title folder as /app0:
//   /app0/nfs2se/     the game files (fedata, gamedata, install.win ...)
//   /app0/data/       what the port writes: nfs2se.log is the one log, home/.nfs2se has settings and saves
// If /app0/data cannot be written, /data/nfs2se is used instead (the game stays in /app0/nfs2se).
#include <cstdio>
#include <pthread.h>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <SDL.h>
#include <csignal>
#include <ucontext.h>

#include <dirent.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdarg>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int __real_main(int argc, char **argv);
extern "C" int sceSystemServiceLoadExec(const char *path, char *const argv[]);
extern "C" void ut_ps5_log(const char *text);
extern "C" void ut_ps5_input_start(void);
extern "C" void lowmem_stats(unsigned long *base, unsigned long *used, unsigned long *size);
extern "C" int lowmem_attempt(int i, unsigned long *hint, unsigned long *size, long *result, int *err);

namespace {
std::string g_data = "/app0/data";
const std::string kGame = "/app0/nfs2se";   // the GOG files, next to data/

bool writable(const std::string &dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string probe = dir + "/.write-test";
    if (FILE *f = std::fopen(probe.c_str(), "w")) {
        std::fclose(f);
        std::remove(probe.c_str());
        return true;
    }
    return false;
}

// Log lines go straight to <data>/nfs2se.log with plain open/write (no stdio streams: reopening stderr is not safe on the
// console, and a crash handler cannot use buffered I/O anyway). The previous run is kept as nfs2se.prev.log.
char g_logpath[256] = "/app0/data/nfs2se.log";
void log_raw(const char *text) {
    const int fd = open(g_logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    (void)!write(fd, text, std::strlen(text));
    close(fd);
}
void log_raw_nl(const char *text) {
    char line[400];
    const int n = std::snprintf(line, sizeof line, "%s\n", text);
    (void)n;
    log_raw(line);
}
void open_log() {
    std::snprintf(g_logpath, sizeof g_logpath, "%s/nfs2se.log", g_data.c_str());
    // No rotation: rename() crashed on the console. Runs are separated by a marker line; delete the file by hand when it grows.
    log_raw("\n===== new run =====\n");
}
// Written before main: shows whether the title gets that far.
__attribute__((constructor(101))) void early_marker() { log_raw_nl("[ps5-launch] static constructors done"); }

extern "C" char __ehdr_start __attribute__((weak));   // start of the ELF image: the load base

void crash(int sig, siginfo_t *info, void *ctx) {
    // two threads can fault together: the second one waits so the lines of the first stay readable
    static volatile int busy;
    while (__sync_lock_test_and_set(&busy, 1)) { }
    char text[320];
    const ucontext_t *uc = static_cast<const ucontext_t *>(ctx);
    const unsigned long base = reinterpret_cast<unsigned long>(&__ehdr_start);
    const unsigned long rip = uc ? (unsigned long)uc->uc_mcontext.mc_rip : 0;
    const int n = std::snprintf(text, sizeof text, "\n[ps5-launch] CRASH: signal %d, fault address %p, code %d, rip %p, rsp %p\n", sig,
                                info ? info->si_addr : nullptr, info ? info->si_code : 0, (void *)rip, uc ? (void *)uc->uc_mcontext.mc_rsp : nullptr);
    if (n > 0) log_raw(text);
    const int t = std::snprintf(text, sizeof text, "[ps5-launch] CRASH-INFO thread %lx image-base %lx rip-offset %lx\n", (unsigned long)pthread_self(), base, rip - base);
    if (t > 0) log_raw(text);
    if (uc) {
        const auto &m = uc->uc_mcontext;
        const int r = std::snprintf(text, sizeof text,
                                    "[ps5-launch] CRASH-REGS rax %lx rbx %lx rcx %lx rdx %lx rsi %lx rdi %lx rbp %lx r8 %lx r9 %lx r10 %lx r11 %lx r12 %lx r13 %lx r14 %lx r15 %lx\n",
                                    (unsigned long)m.mc_rax, (unsigned long)m.mc_rbx, (unsigned long)m.mc_rcx, (unsigned long)m.mc_rdx, (unsigned long)m.mc_rsi,
                                    (unsigned long)m.mc_rdi, (unsigned long)m.mc_rbp, (unsigned long)m.mc_r8, (unsigned long)m.mc_r9, (unsigned long)m.mc_r10,
                                    (unsigned long)m.mc_r11, (unsigned long)m.mc_r12, (unsigned long)m.mc_r13, (unsigned long)m.mc_r14, (unsigned long)m.mc_r15);
        if (r > 0) log_raw(text);
        // code addresses found on the stack, as image offsets: the call chain, to be named with tools/symbolize.sh
        const unsigned long *sp = reinterpret_cast<const unsigned long *>(m.mc_rsp);
        int shown = 0;
        for (int i = 0; i < 256 && shown < 40; i++) {
            if (sp[i] >= base && sp[i] < base + 0x1000000ul) {
                const int k = std::snprintf(text, sizeof text, "[ps5-launch] CRASH-CALL %lx\n", sp[i] - base);
                if (k > 0) log_raw(text);
                shown++;
            }
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_crash_handlers() {
    struct sigaction sa = {};
    sa.sa_sigaction = crash;
    sa.sa_flags = SA_SIGINFO;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}
}  // namespace

// The port's data folder (no trailing slash).
extern "C" const char *ut_ps5_data_dir(void) { return g_data.c_str(); }

// The console refuses chdir() for the title ("cannot chdir to /app0/MotoRacer": no working directory, like its
// getcwd()). The game and the runtime use relative paths, so the link wraps the path calls: after a failed
// chdir the directory is remembered here and relative paths are resolved against it.
extern "C" {
int __real_chdir(const char *);
int __real_open(const char *, int, ...);
FILE *__real_fopen(const char *, const char *);
int __real_stat(const char *, struct stat *);
int __real_lstat(const char *, struct stat *);
DIR *__real_opendir(const char *);
int __real_mkdir(const char *, mode_t);
int __real_unlink(const char *);
int __real_rename(const char *, const char *);
int __real_access(const char *, int);
}
namespace {
std::string g_cwd;   // empty: the real working directory
std::string resolve(const char *path) {
    if (!path || !*path || path[0] == '/' || g_cwd.empty()) return path ? path : "";
    return g_cwd + "/" + path;
}
}  // namespace
extern "C" int __wrap_chdir(const char *path) {
    const int rc = __real_chdir(path);
    if (rc == 0) { g_cwd.clear(); return 0; }
    const int err = errno;
    struct stat st;
    const std::string target = resolve(path);
    if (__real_stat(target.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        g_cwd = target;
        std::fprintf(stderr, "[ps5-launch] chdir(%s) failed (errno %d): emulating the working directory\n", path, err);
        return 0;
    }
    errno = err;
    return -1;
}
extern "C" int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = static_cast<mode_t>(va_arg(ap, int)); va_end(ap); }
    return __real_open(resolve(path).c_str(), flags, mode);
}
extern "C" FILE *__wrap_fopen(const char *path, const char *mode) { return __real_fopen(resolve(path).c_str(), mode); }
extern "C" int __wrap_stat(const char *path, struct stat *st) { return __real_stat(resolve(path).c_str(), st); }
extern "C" int __wrap_lstat(const char *path, struct stat *st) { return __real_lstat(resolve(path).c_str(), st); }
extern "C" DIR *__wrap_opendir(const char *path) { return __real_opendir(resolve(path).c_str()); }
extern "C" int __wrap_mkdir(const char *path, mode_t mode) { return __real_mkdir(resolve(path).c_str(), mode); }
extern "C" int __wrap_unlink(const char *path) { return __real_unlink(resolve(path).c_str()); }
extern "C" int __wrap_rename(const char *a, const char *b) { return __real_rename(resolve(a).c_str(), resolve(b).c_str()); }
extern "C" int __wrap_access(const char *path, int mode) { return __real_access(resolve(path).c_str(), mode); }

// The console's getcwd() jumps to a bad address; the title has no working directory (--wrap=getcwd).
extern "C" char *__wrap_getcwd(char *buf, size_t size) {
    const std::string dir = g_cwd.empty() ? kGame : g_cwd;
    if (!buf) { buf = static_cast<char *>(std::malloc(dir.size() + 1)); if (!buf) return nullptr; }
    else if (size < dir.size() + 1) return nullptr;
    std::memcpy(buf, dir.c_str(), dir.size() + 1);
    return buf;
}

// Every exit asks the system to close the title (a plain exit makes the console show an error screen).
extern "C" void __wrap_exit(int code) {
    ut_ps5_log("[ps5-launch] exit requested, asking the system to close the title");
    std::fflush(nullptr);
    usleep(300 * 1000);
    sceSystemServiceLoadExec("exit", nullptr);
    usleep(1500 * 1000);
    _exit(code);
}


// The game prints with printf/fprintf: copy stdout and stderr into the log through a pipe.
void *pipe_reader(void *arg) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    char buf[512];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n <= 0) { usleep(10000); continue; }
        buf[n] = 0;
        log_raw(buf);
    }
    return nullptr;
}
void capture_output() {
    int fds[2];
    if (pipe(fds) != 0) { log_raw_nl("[ps5-launch] no pipe for the game output"); return; }
    const int r1 = dup2(fds[1], 1), r2 = dup2(fds[1], 2);
    char m[96]; snprintf(m, sizeof m, "[ps5-launch] output capture: pipe %d/%d dup2 %d/%d errno %d", fds[0], fds[1], r1, r2, errno);
    log_raw_nl(m);
    static pthread_t th;
    pthread_create(&th, nullptr, pipe_reader, reinterpret_cast<void *>(static_cast<intptr_t>(fds[0])));
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    printf("[game-stdout] capture test\n"); fflush(stdout);
    fprintf(stderr, "[game-stderr] capture test\n");
}

// Game settings: the template with the buttons of the virtual pad (SDL button numbers: A 0, B 1, X 2, Y 3, back 4, start 6, dpad 11-14).
void write_conf(const std::string &home) {
    std::ifstream in(kGame + "/nfs2se.conf.template");
    if (!in) { log_raw_nl("[ps5-launch] conf template not readable"); return; }
    std::string out, line;
    while (std::getline(in, line)) {
        if (line.rfind("Joystick0DPadButtons=", 0) == 0) line = "Joystick0DPadButtons=-1,-1,-1,-1";
        else if (line.rfind("Joystick0EscButton=", 0) == 0) line = "Joystick0EscButton=-1";
        else if (line.rfind("Joystick0ResetButton=", 0) == 0) line = "Joystick0ResetButton=-1";
        else if (line.rfind("JoystickDisableAxesInMenu=", 0) == 0) line = "JoystickDisableAxesInMenu=1";   // menus use the arrow keys only (no double input)
        out += line + "\n";
    }
    std::filesystem::create_directories(home + "/.nfs2se");
    std::ofstream(home + "/.nfs2se/nfs2se.conf") << out << "\n";
}

// The title's entry point (app_crt.cpp calls "main", the link wraps it).
extern "C" int __wrap_main(int, char **) {
    log_raw_nl("[ps5-launch] main reached");
    if (!writable(g_data)) g_data = "/data/nfs2se";
    std::error_code ec;
    std::filesystem::create_directories(g_data, ec);
    open_log();
    log_raw_nl("[ps5-launch] log opened, installing crash handlers");
    install_crash_handlers();
    setenv("NFS_NOSIG", "1", 1);   // keep this crash handler instead of the game wrapper's
    {
        unsigned long base, used, size;
        lowmem_stats(&base, &used, &size);   // first use creates the arena
        char line[160];
        for (int i = 0;; i++) {
            unsigned long h, sz; long r; int e;
            if (!lowmem_attempt(i, &h, &sz, &r, &e)) break;
            snprintf(line, sizeof line, "[ps5-launch] mmap hint %#lx size %#lx -> %#lx errno %d", h, sz, r, e);
            log_raw_nl(line);
        }
        snprintf(line, sizeof line, "[ps5-launch] low-memory arena at %#lx, %lu of %lu bytes used", base, used, size);
        log_raw_nl(line);
        if (!base) log_raw_nl("[ps5-launch] NO low memory: the game cannot run");
    }
    // SDL only uses its dummy drivers when asked: frames go to the display through ps5_present.cpp, sound
    // through the audio driver patch. Hints, not environment variables (SDL keeps its own environment copy).
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
    // The game opens its files relative to the working directory (the console has none): resolve against the game folder.
    g_cwd = kGame;
    // Settings and saves go to $HOME/.nfs2se
    static std::string home = g_data + "/home";
    std::filesystem::create_directories(home, ec);
    setenv("HOME", home.c_str(), 1);
    ut_ps5_log("[ps5-launch] NFS II SE (Vulkan Glide) PS5 starting");
    for (const char *f : {"nfs2se.conf.template", "fedata", "gamedata", "install.win"})
        ut_ps5_log((std::string("[ps5-launch] ") + f + ((::access((kGame + "/" + f).c_str(), F_OK) == 0) ? " present" : " MISSING")).c_str());
    write_conf(home);
    capture_output();
    ut_ps5_log("[ps5-launch] starting the controller");
    ut_ps5_input_start();
    ut_ps5_log("[ps5-launch] entering the game");
    static char a0[] = "nfs2se";
    char *argv2[] = {a0, nullptr};
    const int code = __real_main(1, argv2);
    // Ending the process from inside makes the console report an error; asking the system to close the title
    // is how a game quits.
    ut_ps5_log("[ps5-launch] game finished, asking the system to close the title");
    std::fflush(nullptr);
    usleep(500 * 1000);
    const int rc = sceSystemServiceLoadExec("exit", nullptr);
    std::fprintf(stderr, "[ps5] the system did not close the title: %d\n", rc);
    usleep(1500 * 1000);
    _exit(code);
}
