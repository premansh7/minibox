// minibox.cpp - a tiny container runtime for Linux (C++17, no external libraries).
//
// It uses the same kernel features that Docker is built on:
//   * namespaces  (PID, UTS, mount, IPC, network)  -> isolation
//   * pivot_root                                   -> private root filesystem
//   * cgroup v2                                    -> memory / CPU / process limits
//   * mknod                                        -> a minimal private /dev
//
// Build:  g++ -std=c++17 -O2 -Wall -Wextra -o minibox minibox.cpp
// Usage:  sudo ./minibox run [--mem 100M] [--cpu 50] [--pids 64] /bin/sh
//         ./minibox ps | stats | stop     (see ./minibox help)

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <net/if.h>
#include <random>
#include <sched.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using ull = unsigned long long;

static const std::string STATE_DIR = "/run/minibox";  // where running containers are recorded
static const char *CONTAINER_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

// =====================================================================
//  Small helpers
// =====================================================================
static void warn(const std::string &m) { std::cerr << "minibox: warning: " << m << "\n"; }

[[noreturn]] static void fail(const std::string &m) {
    std::cerr << "minibox: error: " << m << "\n";
    std::exit(1);
}

static std::string errstr(const std::string &what) { return what + ": " + std::strerror(errno); }

// Write a string to an existing file (used for /sys and cgroup control files).
static bool writeFile(const std::string &path, const std::string &data) {
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    ssize_t n = write(fd, data.data(), data.size());
    close(fd);
    return n == (ssize_t)data.size();
}

static bool readU64File(const std::string &path, ull &v) {
    std::ifstream f(path);
    std::string s;
    if (!(f >> s)) return false;
    if (s == "max") { v = ~0ULL; return true; }
    try { v = std::stoull(s); } catch (...) { return false; }
    return true;
}

// Read "key value" style files such as memory.events or cpu.stat.
static bool readKey(const std::string &path, const std::string &key, ull &v) {
    std::ifstream f(path);
    std::string k;
    ull x;
    while (f >> k >> x)
        if (k == key) { v = x; return true; }
    return false;
}

static ull parseSize(const std::string &s) {
    size_t idx = 0;
    ull v = std::stoull(s, &idx);
    std::string suf = s.substr(idx);
    ull mul = 1;
    if (suf == "K" || suf == "k") mul = 1024ULL;
    else if (suf == "M" || suf == "m") mul = 1024ULL * 1024;
    else if (suf == "G" || suf == "g") mul = 1024ULL * 1024 * 1024;
    else if (!suf.empty()) throw std::invalid_argument("bad size '" + s + "' (use e.g. 512K, 100M, 1G)");
    return v * mul;
}

static std::string fmtBytes(ull b) {
    char buf[32];
    if (b >= (1ULL << 30)) snprintf(buf, sizeof buf, "%.1fG", b / 1073741824.0);
    else if (b >= (1ULL << 20)) snprintf(buf, sizeof buf, "%.1fM", b / 1048576.0);
    else if (b >= 1024) snprintf(buf, sizeof buf, "%.1fK", b / 1024.0);
    else snprintf(buf, sizeof buf, "%lluB", b);
    return buf;
}

static std::string fmtDuration(long s) {
    char buf[32];
    if (s >= 3600) snprintf(buf, sizeof buf, "%ldh %02ldm", s / 3600, (s % 3600) / 60);
    else if (s >= 60) snprintf(buf, sizeof buf, "%ldm %02lds", s / 60, s % 60);
    else snprintf(buf, sizeof buf, "%lds", s);
    return buf;
}

static std::string randomId() {
    std::random_device rd;
    char buf[8];
    snprintf(buf, sizeof buf, "%06x", rd() & 0xFFFFFF);
    return buf;
}

static bool isAlive(pid_t pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

// =====================================================================
//  Cgroup v2 wrapper (RAII: the cgroup directory is removed automatically)
// =====================================================================
static std::string cgroupRoot() {
    if (fs::exists("/sys/fs/cgroup/cgroup.controllers")) return "/sys/fs/cgroup";
    // The system is not using cgroup v2 at the usual place: try our own mount.
    std::string alt = STATE_DIR + "/cgroup";
    std::error_code ec;
    if (!fs::exists(alt + "/cgroup.controllers", ec)) {
        fs::create_directories(alt, ec);
        mount("none", alt.c_str(), "cgroup2", 0, nullptr);
    }
    return fs::exists(alt + "/cgroup.controllers", ec) ? alt : "";
}

static void enableControllers(const std::string &cgroupDir) {
    for (const char *c : {"+cpu", "+memory", "+pids"})  // one by one: a missing one must not block the rest
        writeFile(cgroupDir + "/cgroup.subtree_control", c);
}

class Cgroup {
public:
    explicit Cgroup(const std::string &id) {
        std::string root = cgroupRoot();
        if (root.empty()) {
            warn("cgroup v2 is not available - resource limits are disabled");
            return;
        }
        enableControllers(root);
        std::string parent = root + "/minibox";
        std::error_code ec;
        fs::create_directories(parent, ec);
        enableControllers(parent);
        std::string p = parent + "/" + id;
        if (!fs::create_directory(p, ec) && ec) {
            warn("cannot create cgroup " + p + ": " + ec.message());
            return;
        }
        path_ = p;
    }
    Cgroup(const Cgroup &) = delete;
    Cgroup &operator=(const Cgroup &) = delete;
    ~Cgroup() { remove(); }

    bool active() const { return !path_.empty(); }
    const std::string &path() const { return path_; }

    void limit(const std::string &file, const std::string &value, const char *what) {
        if (!active()) return;
        if (!writeFile(path_ + "/" + file, value))
            warn(std::string("could not apply the ") + what + " limit (is that cgroup controller enabled?)");
    }
    bool addPid(pid_t pid) { return active() && writeFile(path_ + "/cgroup.procs", std::to_string(pid)); }

    void remove() {
        if (path_.empty()) return;
        for (int i = 0; i < 50; i++) {  // the kernel may need a moment after the last process exits
            if (rmdir(path_.c_str()) == 0 || errno == ENOENT) break;
            usleep(100000);
        }
        path_.clear();
    }

private:
    std::string path_;
};

// =====================================================================
//  Container registry: one small file per running container in /run/minibox
// =====================================================================
struct Info {
    std::string id, name, cmd, cgroup;
    pid_t pid = 0;
    time_t started = 0;
};

static std::string infoPath(const std::string &id) { return STATE_DIR + "/" + id + ".info"; }

static void saveInfo(const Info &i) {
    std::ofstream f(infoPath(i.id));
    f << "id=" << i.id << "\nname=" << i.name << "\npid=" << i.pid << "\nstarted=" << i.started << "\ncgroup="
      << i.cgroup << "\ncmd=" << i.cmd << "\n";
}

static bool loadInfo(const std::string &path, Info &i) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "id") i.id = v;
        else if (k == "name") i.name = v;
        else if (k == "pid") i.pid = (pid_t)atoi(v.c_str());
        else if (k == "started") i.started = (time_t)atol(v.c_str());
        else if (k == "cgroup") i.cgroup = v;
        else if (k == "cmd") i.cmd = v;
    }
    return !i.id.empty() && i.pid > 0;
}

// Returns running containers; records of dead containers are cleaned up.
static std::vector<Info> listContainers() {
    std::vector<Info> out;
    std::error_code ec;
    if (!fs::exists(STATE_DIR, ec)) return out;
    for (auto &e : fs::directory_iterator(STATE_DIR, ec)) {
        if (e.path().extension() != ".info") continue;
        Info i;
        if (!loadInfo(e.path().string(), i)) continue;
        if (!isAlive(i.pid)) {
            fs::remove(e.path(), ec);
            if (!i.cgroup.empty()) rmdir(i.cgroup.c_str());
            continue;
        }
        out.push_back(i);
    }
    std::sort(out.begin(), out.end(), [](const Info &a, const Info &b) { return a.started < b.started; });
    return out;
}

static bool findContainer(const std::string &ref, Info &out) {
    for (auto &c : listContainers())
        if (c.id == ref || c.name == ref || (ref.size() >= 3 && c.id.compare(0, ref.size(), ref) == 0)) {
            out = c;
            return true;
        }
    return false;
}

// =====================================================================
//  The container side: runs as PID 1 inside the new namespaces
// =====================================================================
struct Options {
    ull memBytes = 0;
    int cpuPercent = 0;
    int pids = 0;
    bool hostNet = false;
    std::string hostname, name, rootfs = "./rootfs";
    std::vector<std::string> cmd;
};

struct ChildArgs {
    const Options *opt;
    int pipeR, pipeW;
};

static bool childErr(const std::string &what) {
    std::fprintf(stderr, "minibox(container): %s: %s\n", what.c_str(), std::strerror(errno));
    return false;
}

// Create /dev/null, /dev/zero, ... as real device files (mknod). If the kernel refuses
// (for example in a restricted sandbox), bind-mount the host's device instead.
static void makeDevices(const std::string &devDir) {
    struct Dev { const char *name; unsigned maj, min; };
    const Dev devs[] = {{"null", 1, 3}, {"zero", 1, 5}, {"full", 1, 7}, {"random", 1, 8}, {"urandom", 1, 9}, {"tty", 5, 0}};
    for (const Dev &d : devs) {
        std::string path = devDir + "/" + d.name;
        if (mknod(path.c_str(), S_IFCHR | 0666, makedev(d.maj, d.min)) == 0) {
            chmod(path.c_str(), 0666);  // ignore the umask
        } else {
            int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0666);
            if (fd >= 0) close(fd);
            std::string host = std::string("/dev/") + d.name;
            if (mount(host.c_str(), path.c_str(), nullptr, MS_BIND, nullptr) != 0)
                warn(std::string("could not create /dev/") + d.name);
        }
    }
    (void)!symlink("/proc/self/fd", (devDir + "/fd").c_str());
    (void)!symlink("/proc/self/fd/0", (devDir + "/stdin").c_str());
    (void)!symlink("/proc/self/fd/1", (devDir + "/stdout").c_str());
    (void)!symlink("/proc/self/fd/2", (devDir + "/stderr").c_str());
}

static bool setupFilesystem(const std::string &root) {
    // 1. Mount changes made in here must never leak back to the host.
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) return childErr("make / private");
    // 2. pivot_root needs the new root to be a mount point: bind-mount it onto itself.
    if (mount(root.c_str(), root.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) return childErr("bind-mount rootfs");

    std::error_code ec;
    for (const char *d : {"/proc", "/dev", "/tmp", "/sys", "/root", "/.oldroot"}) fs::create_directories(root + d, ec);

    // 3. /proc shows only this container's processes thanks to the PID namespace.
    if (mount("proc", (root + "/proc").c_str(), "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0)
        return childErr("mount /proc");
    // 4. A private /dev containing only a handful of safe devices.
    if (mount("tmpfs", (root + "/dev").c_str(), "tmpfs", MS_NOSUID, "mode=755,size=64k") != 0) return childErr("mount /dev");
    makeDevices(root + "/dev");
    // 5. A writable scratch space.
    if (mount("tmpfs", (root + "/tmp").c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=64m") != 0)
        return childErr("mount /tmp");

    // 6. Switch to the new root and drop every reference to the host filesystem.
    std::string old = root + "/.oldroot";
    if (syscall(SYS_pivot_root, root.c_str(), old.c_str()) == 0) {
        if (chdir("/") != 0) return childErr("chdir /");
        umount2("/.oldroot", MNT_DETACH);
        rmdir("/.oldroot");
    } else {
        // Some systems (e.g. root on a ramfs) refuse pivot_root; chroot is a weaker fallback.
        warn(errstr("pivot_root failed, falling back to chroot") + " (less secure)");
        if (chroot(root.c_str()) != 0 || chdir("/") != 0) return childErr("chroot");
    }
    return true;
}

static void bringUpLoopback() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

static volatile pid_t g_cmdPid = 0;
static void forwardToCommand(int sig) {
    if (g_cmdPid > 0) kill(g_cmdPid, sig);
}

// Mini "init": as PID 1 we must forward signals to the real command and reap zombies,
// otherwise `minibox stop` could not terminate the container politely.
static int childMain(void *arg) {
    auto *a = static_cast<ChildArgs *>(arg);
    const Options &o = *a->opt;

    close(a->pipeW);
    char go = 0;
    ssize_t n = read(a->pipeR, &go, 1);  // wait until the parent has put us into the cgroup
    close(a->pipeR);
    if (n != 1) return 1;

    if (sethostname(o.hostname.c_str(), o.hostname.size()) != 0) childErr("sethostname");
    if (!o.hostNet) bringUpLoopback();
    if (!setupFilesystem(o.rootfs)) return 1;

    struct sigaction sa{};
    sa.sa_handler = forwardToCommand;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    signal(SIGINT, SIG_IGN);  // Ctrl+C reaches the command directly via the terminal

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    sigprocmask(SIG_BLOCK, &block, &old);  // no signal may slip in before g_cmdPid is set

    pid_t cmd = fork();
    if (cmd < 0) return childErr("fork") ? 0 : 1;
    if (cmd == 0) {
        signal(SIGINT, SIG_DFL);
        sigprocmask(SIG_SETMASK, &old, nullptr);
        if (chdir("/root") != 0 && chdir("/") != 0) _exit(126);
        clearenv();
        setenv("PATH", CONTAINER_PATH, 1);
        setenv("HOME", "/root", 1);
        setenv("HOSTNAME", o.hostname.c_str(), 1);
        setenv("TERM", getenv("TERM") ? getenv("TERM") : "xterm", 1);
        setenv("PS1", "[minibox \\h] \\w # ", 1);
        std::vector<char *> argv;
        for (const auto &s : o.cmd) argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::fprintf(stderr, "minibox(container): cannot execute '%s': %s\n", argv[0], std::strerror(errno));
        _exit(127);
    }
    g_cmdPid = cmd;
    sigprocmask(SIG_SETMASK, &old, nullptr);

    int code = 0;
    for (;;) {
        int st = 0;
        pid_t w = waitpid(-1, &st, 0);  // reaps the command AND any orphaned grandchildren
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (w == cmd) {
            code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            break;
        }
    }
    return code;  // when PID 1 exits, the kernel kills everything left in the namespace
}

// =====================================================================
//  Command: run
// =====================================================================
static volatile pid_t g_child = 0;
static void forwardToChild(int sig) {
    if (g_child > 0) kill(g_child, sig);
}

static int cmdRun(const std::vector<std::string> &args) {
    Options o;
    size_t i = 0;
    auto need = [&](const char *flag) -> std::string {
        if (i + 1 >= args.size()) fail(std::string(flag) + " needs a value");
        return args[++i];
    };
    for (; i < args.size(); i++) {
        const std::string &a = args[i];
        if (a == "--mem") o.memBytes = parseSize(need("--mem"));
        else if (a == "--cpu") o.cpuPercent = std::stoi(need("--cpu"));
        else if (a == "--pids") o.pids = std::stoi(need("--pids"));
        else if (a == "--hostname") o.hostname = need("--hostname");
        else if (a == "--name") o.name = need("--name");
        else if (a == "--rootfs") o.rootfs = need("--rootfs");
        else if (a == "--host-net") o.hostNet = true;
        else if (a == "--") { i++; break; }
        else if (a.size() > 1 && a[0] == '-') fail("unknown option " + a);
        else break;
    }
    for (; i < args.size(); i++) o.cmd.push_back(args[i]);
    if (o.cmd.empty()) o.cmd.push_back("/bin/sh");

    if (geteuid() != 0) fail("minibox run must be run as root (try: sudo ./minibox run ...)");

    std::error_code ec;
    fs::path rootAbs = fs::canonical(o.rootfs, ec);
    if (ec || !fs::is_directory(rootAbs))
        fail("root filesystem '" + o.rootfs + "' not found. Create one with: sudo ./make_rootfs.sh " + o.rootfs);
    o.rootfs = rootAbs.string();

    fs::create_directories(STATE_DIR, ec);
    std::string id = randomId();
    if (o.name.empty()) o.name = id;
    if (o.hostname.empty()) o.hostname = o.name;
    for (auto &c : listContainers())
        if (c.name == o.name) fail("a container named '" + o.name + "' is already running");

    // --- cgroup with the requested limits ---
    Cgroup cg(id);
    if (o.memBytes) {
        cg.limit("memory.max", std::to_string(o.memBytes), "memory");
        if (cg.active()) writeFile(cg.path() + "/memory.swap.max", "0");  // make the limit hard (best effort)
    }
    if (o.cpuPercent > 0) cg.limit("cpu.max", std::to_string(o.cpuPercent * 1000) + " 100000", "CPU");
    if (o.pids > 0) cg.limit("pids.max", std::to_string(o.pids), "process-count");

    int fds[2];
    if (pipe(fds) != 0) fail(errstr("pipe"));

    const size_t STACK = 1024 * 1024;
    void *stack = mmap(nullptr, STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) fail(errstr("mmap"));

    signal(SIGINT, SIG_IGN);  // Ctrl+C is for the command inside the container
    signal(SIGTERM, forwardToChild);
    signal(SIGHUP, forwardToChild);

    ChildArgs ca{&o, fds[0], fds[1]};
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWIPC | SIGCHLD;
    if (!o.hostNet) flags |= CLONE_NEWNET;
    pid_t child = clone(childMain, static_cast<char *>(stack) + STACK, flags, &ca);
    if (child < 0) fail(errstr("clone (are namespaces enabled in this kernel?)"));
    g_child = child;
    close(fds[0]);

    if (!cg.addPid(child) && cg.active()) warn("could not move the container into its cgroup");

    std::string cmdline;
    for (auto &s : o.cmd) cmdline += (cmdline.empty() ? "" : " ") + s;
    Info info;
    info.id = id;
    info.name = o.name;
    info.pid = child;
    info.started = time(nullptr);
    info.cgroup = cg.path();
    info.cmd = cmdline;
    saveInfo(info);

    std::cerr << "[minibox] container " << id << " (" << o.name << ") started, host pid " << child << ": " << cmdline
              << "\n";
    if (write(fds[1], "x", 1) != 1) warn("could not release the container");  // let the child continue
    close(fds[1]);

    int st = 0;
    while (waitpid(child, &st, 0) < 0 && errno == EINTR) {}

    ull oom = 0;
    if (cg.active()) readKey(cg.path() + "/memory.events", "oom_kill", oom);
    fs::remove(infoPath(id), ec);
    munmap(stack, STACK);

    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    std::cerr << "[minibox] container " << id << " exited with code " << code;
    if (oom > 0) std::cerr << "  (killed by the out-of-memory limit)";
    std::cerr << "\n";
    return code;  // the Cgroup destructor now removes the cgroup directory
}

// =====================================================================
//  Command: ps
// =====================================================================
static int cmdPs() {
    auto list = listContainers();
    printf("%-8s %-14s %-8s %-9s %s\n", "ID", "NAME", "PID", "UPTIME", "COMMAND");
    for (auto &c : list)
        printf("%-8s %-14.14s %-8d %-9s %s\n", c.id.c_str(), c.name.c_str(), (int)c.pid,
               fmtDuration((long)difftime(time(nullptr), c.started)).c_str(), c.cmd.c_str());
    if (list.empty()) printf("(no running containers)\n");
    return 0;
}

// =====================================================================
//  Command: stop
// =====================================================================
static bool waitGone(pid_t pid, int millis) {
    for (int t = 0; t < millis; t += 100) {
        if (!isAlive(pid)) return true;
        usleep(100000);
    }
    return !isAlive(pid);
}

static int cmdStop(const std::vector<std::string> &args) {
    int timeout = 3;
    std::string ref;
    for (size_t i = 0; i < args.size(); i++) {
        if ((args[i] == "-t" || args[i] == "--time") && i + 1 < args.size()) timeout = std::stoi(args[++i]);
        else ref = args[i];
    }
    if (ref.empty()) fail("usage: minibox stop [-t SECONDS] NAME|ID");
    if (geteuid() != 0) fail("minibox stop must be run as root");
    Info c;
    if (!findContainer(ref, c)) fail("no running container '" + ref + "'");

    std::cerr << "[minibox] stopping " << c.id << " (" << c.name << ") ...\n";
    kill(c.pid, SIGTERM);  // PID 1 of the container forwards this to the command
    if (!waitGone(c.pid, timeout * 1000)) {
        std::cerr << "[minibox] did not stop in " << timeout << "s, sending SIGKILL\n";
        kill(c.pid, SIGKILL);
        waitGone(c.pid, 2000);
    }
    std::cerr << "[minibox] stopped\n";
    return 0;
}

// =====================================================================
//  Command: stats
// =====================================================================
struct Sample {
    ull mem = 0, memMax = 0, cpuUsec = 0;
    size_t pids = 0;
};

static std::vector<pid_t> cgroupPids(const std::string &cg) {
    std::vector<pid_t> v;
    std::ifstream f(cg + "/cgroup.procs");
    int p;
    while (f >> p) v.push_back(p);
    return v;
}

static ull procRssBytes(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmRSS:") == 0) return strtoull(line.c_str() + 6, nullptr, 10) * 1024;
    return 0;
}

static ull procCpuUsec(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(f, line);
    size_t close = line.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream rest(line.substr(close + 2));
    std::vector<std::string> fld;
    std::string t;
    while (rest >> t) fld.push_back(t);
    if (fld.size() < 13) return 0;
    return (stoull(fld[11]) + stoull(fld[12])) * 1000000ULL / (ull)sysconf(_SC_CLK_TCK);
}

static Sample takeSample(const Info &c) {
    Sample s;
    auto pids = cgroupPids(c.cgroup);
    s.pids = pids.size();
    ull v;
    // Prefer the kernel's own accounting; fall back to /proc if a controller is not enabled.
    if (readU64File(c.cgroup + "/memory.current", v)) s.mem = v;
    else for (pid_t p : pids) s.mem += procRssBytes(p);
    if (readU64File(c.cgroup + "/memory.max", v)) s.memMax = v;
    if (readKey(c.cgroup + "/cpu.stat", "usage_usec", v)) s.cpuUsec = v;
    else for (pid_t p : pids) s.cpuUsec += procCpuUsec(p);
    return s;
}

static int cmdStats(const std::vector<std::string> &args) {
    bool watch = false;
    std::string ref;
    for (auto &a : args) {
        if (a == "-w" || a == "--watch") watch = true;
        else ref = a;
    }
    if (ref.empty()) fail("usage: minibox stats [-w] NAME|ID");
    Info c;
    if (!findContainer(ref, c)) fail("no running container '" + ref + "'");
    if (c.cgroup.empty()) fail("this container has no cgroup, so no statistics are available");

    printf("%-8s %-14s %7s %-24s %5s\n", "ID", "NAME", "CPU%", "MEMORY", "PIDS");
    do {
        Sample a = takeSample(c);
        usleep(1000000);
        Sample b = takeSample(c);
        if (!isAlive(c.pid)) { printf("(container has exited)\n"); break; }
        double cpu = b.cpuUsec >= a.cpuUsec ? (b.cpuUsec - a.cpuUsec) / 1e6 * 100.0 : 0.0;
        std::string mem = fmtBytes(b.mem);
        if (b.memMax == ~0ULL) mem += " / unlimited";
        else if (b.memMax > 0) mem += " / " + fmtBytes(b.memMax);
        printf("%-8s %-14.14s %6.1f%% %-24s %5zu\n", c.id.c_str(), c.name.c_str(), cpu, mem.c_str(), b.pids);
        fflush(stdout);
    } while (watch);
    return 0;
}

// =====================================================================
//  main
// =====================================================================
static void usage() {
    std::cout <<
        "minibox - a tiny container runtime\n\n"
        "  minibox run [options] [--] [COMMAND [ARGS...]]   (default command: /bin/sh)\n"
        "      --rootfs DIR     root filesystem to use         (default ./rootfs)\n"
        "      --name NAME      container name\n"
        "      --hostname NAME  hostname inside the container\n"
        "      --mem SIZE       memory limit, e.g. 64M, 1G\n"
        "      --cpu PERCENT    CPU limit, 100 = one core\n"
        "      --pids N         maximum number of processes\n"
        "      --host-net       share the host network instead of an isolated one\n"
        "  minibox ps                       list running containers\n"
        "  minibox stats [-w] NAME|ID       CPU / memory / process usage (-w = keep updating)\n"
        "  minibox stop [-t SEC] NAME|ID    stop a container (SIGTERM, then SIGKILL)\n";
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    std::vector<std::string> rest(argv + 2, argv + argc);
    try {
        if (cmd == "run") return cmdRun(rest);
        if (cmd == "ps") return cmdPs();
        if (cmd == "stop") return cmdStop(rest);
        if (cmd == "stats") return cmdStats(rest);
        if (cmd == "help" || cmd == "-h" || cmd == "--help") { usage(); return 0; }
    } catch (const std::exception &e) {
        fail(std::string("invalid argument: ") + e.what());
    }
    std::cerr << "minibox: unknown command '" << cmd << "'\n\n";
    usage();
    return 1;
}
