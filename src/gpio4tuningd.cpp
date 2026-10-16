// Copyright (C) 2026 erixabo
// SPDX-License-Identifier: GPL-3.0-only

#include <librpitx/ngfmdmasync.h>

#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t kSampleRate = 100000;
constexpr int kDmaChannel = 14;
constexpr uint32_t kFifoSize = 10000;
constexpr size_t kBatchSamples = 5000; // 50 ms; bounds graceful stop/retune latency.
constexpr uint64_t kMinFrequencyHz = 20000;
constexpr uint64_t kMaxFrequencyHz = 1500000000ULL;
constexpr uint64_t kDefaultFrequencyHz = 433100000ULL;
constexpr const char *kDefaultTxcorrPath = "/home/.tuner/txcorr";
constexpr size_t kMaxCommandLength = 256;

std::atomic<bool> g_running{true};

void on_signal(int) { g_running.store(false); }

bool parse_u64(const std::string &text, uint64_t &out)
{
    if (text.empty() || text[0] == '-') return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno || !end || *end || value < kMinFrequencyHz || value > kMaxFrequencyHz)
        return false;
    out = static_cast<uint64_t>(value);
    return true;
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<std::string> known_gpio_programs()
{
    std::vector<std::string> found;
    DIR *proc = opendir("/proc");
    if (!proc) return {"/proc unavailable"};
    const pid_t self = getpid();
    const char *known[] = {"personal_cw", "cwook", "pisstv", "rtty8", "rpitx", "sendiq", "pi_fm_rds"};
    while (dirent *entry = readdir(proc)) {
        char *end = nullptr;
        const long raw_pid = std::strtol(entry->d_name, &end, 10);
        if (!end || *end || raw_pid <= 1 || raw_pid == self) continue;
        const std::string base = std::string("/proc/") + entry->d_name;
        std::ifstream comm_file(base + "/comm");
        std::string comm;
        std::getline(comm_file, comm);
        std::ifstream cmd_file(base + "/cmdline", std::ios::binary);
        std::string cmd((std::istreambuf_iterator<char>(cmd_file)), {});
        std::replace(cmd.begin(), cmd.end(), '\0', ' ');
        const std::string haystack = lower(comm + " " + cmd);
        for (const char *name : known) {
            if (haystack.find(name) != std::string::npos) {
                found.push_back(std::string("pid=") + std::to_string(raw_pid) + ":" + comm);
                break;
            }
        }
    }
    closedir(proc);
    return found;
}

bool gpio4_is_input(std::string &detail)
{
    FILE *pipe = popen("/usr/bin/raspi-gpio get 4 2>&1", "r");
    if (!pipe) { detail = "cannot run raspi-gpio"; return false; }
    char buffer[256];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    const int rc = pclose(pipe);
    if (rc != 0 || output.find("GPIO 4:") == std::string::npos) {
        detail = trim(output.empty() ? "raspi-gpio failed" : output);
        return false;
    }
    const auto pos = output.find("func=");
    if (pos == std::string::npos) { detail = "GPIO4 function not reported"; return false; }
    const auto end = output.find_first_of(" \r\n", pos);
    const std::string func = output.substr(pos + 5, end == std::string::npos ? end : end - pos - 5);
    detail = "func=" + func;
    return func == "INPUT";
}

bool write_txcorr_atomic(const std::string &path, int64_t correction, std::string &error)
{
    const auto slash = path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "." : path.substr(0, slash);
    struct stat st{};
    if (stat(directory.c_str(), &st) != 0) {
        if (errno != ENOENT || mkdir(directory.c_str(), 0755) != 0) {
            error = std::string("cannot create txcorr directory: ") + std::strerror(errno);
            return false;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        error = "txcorr parent path is not a directory";
        return false;
    }
    const std::string temp = path + ".tmp." + std::to_string(getpid());
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        error = std::string("cannot create txcorr temp file: ") + std::strerror(errno);
        return false;
    }
    const std::string contents = std::to_string(correction) + "\n";
    size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t n = write(fd, contents.data() + offset, contents.size() - offset);
        if (n < 0 && errno == EINTR) {
            if (!g_running.load()) return false;
            continue;
        }
        if (n <= 0) {
            error = std::string("cannot write txcorr: ") + std::strerror(errno);
            close(fd);
            unlink(temp.c_str());
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    const int sync_result = fsync(fd);
    const int close_result = close(fd);
    if (sync_result != 0 || close_result != 0) {
        error = std::string("cannot sync txcorr: ") + std::strerror(errno);
        unlink(temp.c_str());
        return false;
    }
    if (rename(temp.c_str(), path.c_str()) != 0) {
        error = std::string("cannot replace txcorr: ") + std::strerror(errno);
        unlink(temp.c_str());
        return false;
    }
    const int dfd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return true;
}

class Radio {
public:
    explicit Radio(std::string lock_path, std::string txcorr_path)
        : lock_path_(std::move(lock_path)), txcorr_path_(std::move(txcorr_path)) {}
    ~Radio() { stop(); }

    uint64_t frequency() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return frequency_hz_;
    }

    uint64_t rx_frequency() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return target_frequency_hz_;
    }

    int64_t txcorr() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return txcorr_hz_;
    }

    bool read_txcorr_file(int64_t &value, std::string &error) const
    {
        std::ifstream file(txcorr_path_);
        if (!file) { error = "txcorr file is not set"; return false; }
        std::string text;
        std::getline(file, text);
        errno = 0;
        char *end = nullptr;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        if (errno || text.empty() || !end || *end || parsed < -1500000000LL || parsed > 1500000000LL) {
            error = "txcorr file contains an invalid integer";
            return false;
        }
        value = static_cast<int64_t>(parsed);
        return true;
    }

    bool set_txcorr(int64_t value, std::string &error)
    {
        if (value < -1500000000LL || value > 1500000000LL) {
            error = "txcorr outside -1.5 GHz..+1.5 GHz";
            return false;
        }
        if (!write_txcorr_atomic(txcorr_path_, value, error)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        txcorr_hz_ = value;
        return true;
    }

    bool start_corrected(std::string &error)
    {
        int64_t correction;
        if (!read_txcorr_file(correction, error)) return false;
        uint64_t base;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            base = base_frequency_hz_;
        }
        // txcorr is the additive command correction established by the
        // validation calibration: target RX center minus measured TX center.
        const int64_t corrected = static_cast<int64_t>(base) + correction;
        if (corrected < static_cast<int64_t>(kMinFrequencyHz) ||
            corrected > static_cast<int64_t>(kMaxFrequencyHz)) {
            error = "corrected frequency outside 20 kHz..1.5 GHz";
            return false;
        }
        if (!start(static_cast<uint64_t>(corrected), error)) {
            set_tx_frequency(base);
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        txcorr_hz_ = correction;
        return true;
    }

    bool set_rxtx(uint64_t rx_hz, std::string &error)
    {
        if (rx_hz < kMinFrequencyHz || rx_hz > kMaxFrequencyHz) {
            error = "frequency outside 20 kHz..1.5 GHz";
            return false;
        }
        uint64_t tx_hz;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tx_hz = base_frequency_hz_;
        }
        const int64_t correction = static_cast<int64_t>(tx_hz) - static_cast<int64_t>(rx_hz);
        if (!write_txcorr_atomic(txcorr_path_, correction, error)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        target_frequency_hz_ = rx_hz;
        txcorr_hz_ = correction;
        // Keep raw F unchanged; txcorr is TX minus RX target, then TC adds it.
        return true;
    }

    bool transmitting() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return requested_;
    }

    bool set_frequency(uint64_t value, std::string &error)
    {
        if (value < kMinFrequencyHz || value > kMaxFrequencyHz) {
            error = "frequency outside 20 kHz..1.5 GHz";
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        frequency_hz_ = value;
        base_frequency_hz_ = value;
        ++frequency_generation_;
        cv_.notify_all();
        return true;
    }

    void set_tx_frequency(uint64_t value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frequency_hz_ = value;
        ++frequency_generation_;
        cv_.notify_all();
    }

    bool step_frequency(int64_t delta, std::string &error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t current = static_cast<int64_t>(frequency_hz_);
        const int64_t base = static_cast<int64_t>(base_frequency_hz_);
        if ((delta > 0 && (current > static_cast<int64_t>(kMaxFrequencyHz) - delta ||
                           base > static_cast<int64_t>(kMaxFrequencyHz) - delta)) ||
            (delta < 0 && (current < static_cast<int64_t>(kMinFrequencyHz) - delta ||
                           base < static_cast<int64_t>(kMinFrequencyHz) - delta))) {
            error = "frequency step would leave 20 kHz..1.5 GHz";
            return false;
        }
        frequency_hz_ = static_cast<uint64_t>(current + delta);
        base_frequency_hz_ = static_cast<uint64_t>(base + delta);
        ++frequency_generation_;
        cv_.notify_all();
        return true;
    }

    std::string gpio_status() const
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (requested_) return "GPIO4 BUSY owner=gpio4tuningd";
        }
        const auto processes = known_gpio_programs();
        if (!processes.empty()) return "GPIO4 BUSY process=" + processes.front();
        std::string pin;
        if (!gpio4_is_input(pin)) {
            if (pin.rfind("func=", 0) == 0) return "GPIO4 BUSY " + pin;
            return "GPIO4 UNKNOWN " + pin;
        }
        int fd = open(lock_path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0660);
        if (fd < 0) return std::string("GPIO4 UNKNOWN lock-open:") + std::strerror(errno);
        const bool free = flock(fd, LOCK_EX | LOCK_NB) == 0;
        if (free) flock(fd, LOCK_UN);
        close(fd);
        return free ? "GPIO4 FREE func=INPUT" : "GPIO4 BUSY lock-held";
    }

    bool free_gpio(std::string &error)
    {
        // Stop this daemon's transmitter first, then disconnect any stale
        // peripheral function from the physical GPIO4 pad.
        return stop(error);
    }

    bool stop(std::string &error)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_ = false;
            cv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frequency_hz_ = base_frequency_hz_;
            ++frequency_generation_;
            cv_.notify_all();
        }
        const int rc = std::system("/usr/bin/raspi-gpio set 4 ip");
        if (rc != 0) {
            error = "carrier stopped, but failed to set GPIO4 to input";
            return false;
        }
        std::string detail;
        if (!gpio4_is_input(detail)) {
            error = "carrier stopped, but GPIO4 is not free: " + detail;
            return false;
        }
        return true;
    }

    void stop()
    {
        std::string ignored;
        stop(ignored);
    }

    bool start(uint64_t requested_frequency, std::string &error)
    {
        if (requested_frequency < kMinFrequencyHz || requested_frequency > kMaxFrequencyHz) {
            error = "frequency outside 20 kHz..1.5 GHz";
            return false;
        }
        set_tx_frequency(requested_frequency);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (requested_) return true;
        }
        const std::string availability = gpio_status();
        if (availability.rfind("GPIO4 FREE", 0) != 0) {
            error = availability;
            return false;
        }
        const int lock_fd = open(lock_path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0660);
        if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            if (lock_fd >= 0) close(lock_fd);
            error = "GPIO4 lock is held";
            return false;
        }
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tx_lock_fd_ = lock_fd;
            requested_ = true;
            active_ = false;
            worker_error_.clear();
        }
        worker_ = std::thread(&Radio::transmit_loop, this);
        std::unique_lock<std::mutex> lock(mutex_);
        const bool started = cv_.wait_for(lock, std::chrono::seconds(4), [this] {
            return active_ || !requested_ || !worker_error_.empty();
        });
        if (!started || !active_) {
            error = worker_error_.empty() ? "TX start timed out" : worker_error_;
            lock.unlock();
            stop();
            return false;
        }
        return true;
    }

    std::string last_error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return worker_error_;
    }

private:
    void transmit_loop()
    {
        try {
            uint64_t seen_generation = 0;
            while (true) {
                uint64_t freq;
                uint64_t generation;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!requested_) break;
                    freq = frequency_hz_;
                    generation = frequency_generation_;
                }
                std::unique_ptr<ngfmdmasync> modulator(
                    new ngfmdmasync(freq, kSampleRate, kDmaChannel, kFifoSize));
                std::vector<float> carrier(kBatchSamples, 0.0f);
                while (true) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!requested_ || frequency_generation_ != generation) break;
                    }
                    modulator->SetFrequencySamples(carrier.data(), carrier.size());
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!active_) {
                            active_ = true;
                            cv_.notify_all();
                        }
                    }
                    seen_generation = generation;
                }
                modulator.reset(); // librpitx teardown disables the GPIO4 carrier.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    active_ = false;
                    cv_.notify_all();
                    if (!requested_) break;
                    if (frequency_generation_ == seen_generation) break;
                }
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_error_ = e.what();
            requested_ = false;
            active_ = false;
            cv_.notify_all();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_error_ = "unknown librpitx exception";
            requested_ = false;
            active_ = false;
            cv_.notify_all();
        }
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_ = false;
            if (!requested_) fd = tx_lock_fd_;
            if (fd >= 0) tx_lock_fd_ = -1;
            cv_.notify_all();
        }
        if (fd >= 0) {
            flock(fd, LOCK_UN);
            close(fd);
        }
    }

    const std::string lock_path_;
    const std::string txcorr_path_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t frequency_hz_ = kDefaultFrequencyHz;
    uint64_t base_frequency_hz_ = kDefaultFrequencyHz;
    uint64_t target_frequency_hz_ = kDefaultFrequencyHz;
    int64_t txcorr_hz_ = 0;
    uint64_t frequency_generation_ = 1;
    bool requested_ = false;
    bool active_ = false;
    int tx_lock_fd_ = -1;
    std::string worker_error_;
    std::thread worker_;
};

bool send_text(int fd, const std::string &text)
{
    size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t n = send(fd, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool read_line(int fd, std::string &line)
{
    line.clear();
    while (line.size() <= kMaxCommandLength) {
        char c;
        const ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        // Accept LF, CR, and CRLF clients. CRLF's trailing LF becomes an
        // empty line, which process_command deliberately ignores.
        if (c == '\n' || c == '\r') return true;
        line.push_back(c);
    }
    line.clear();
    return false;
}

std::string help_text()
{
    return "f                         get frequency [Hz]\n"
           "F <Hz>                    set absolute frequency [Hz]\n"
           "F+ <+Hz|-Hz>              shift current frequency by signed Hz\n"
           "r                         get RX target frequency [Hz]\n"
           "c                         get txcorr [Hz]\n"
           "c=<signed Hz>             overwrite txcorr file directly\n"
           "R <Hz>                    set RX target; save raw-TX minus RX target as txcorr\n"
           "t                         get carrier status\n"
           "free                      stop TX and force GPIO4 to input\n"
           "TC                        start carrier at base F plus txcorr\n"
           "T 1 [Hz]                  start raw carrier (default current Hz)\n"
           "T 0                       stop carrier and restore uncorrected base F\n"
           "g                         get GPIO4 availability\n"
           "?                         this help\n"
           "q                         close connection\n";
}

bool process_command(Radio &radio, int client, const std::string &raw, bool &close_client)
{
    std::istringstream input(trim(raw));
    std::string command, arg, extra;
    if (!(input >> command)) return true;
    input >> arg;
    input >> extra;
    if (command == "?" || lower(command) == "help" || lower(command) == "h") {
        if (!arg.empty() || !extra.empty())
            return send_text(client, "RPRT 1 help takes no arguments\n");
        return send_text(client, help_text() + "RPRT 0\n");
    }
    if (command == "q" || command == "Q") {
        close_client = true;
        return send_text(client, "RPRT 0\n");
    }
    if (command == "f" && arg.empty())
        return send_text(client, std::to_string(radio.frequency()) + "\nRPRT 0\n");
    if (command == "r" && arg.empty())
        return send_text(client, std::to_string(radio.rx_frequency()) + "\nRPRT 0\n");
    if (command == "TC" && arg.empty()) {
        std::string error;
        if (!radio.start_corrected(error)) return send_text(client, "RPRT 1 " + error + "\n");
        return send_text(client, std::to_string(radio.frequency()) + "\nRPRT 0\n");
    }
    if (command == "c" && arg.empty())
    {
        int64_t value;
        std::string error;
        if (!radio.read_txcorr_file(value, error)) return send_text(client, "RPRT 1 " + error + "\n");
        return send_text(client, std::to_string(value) + "\nRPRT 0\n");
    }
    if (command.rfind("c=", 0) == 0 && arg.empty()) {
        const std::string value_text = command.substr(2);
        errno = 0;
        char *end = nullptr;
        const long long value = std::strtoll(value_text.c_str(), &end, 10);
        if (errno || value_text.empty() || !end || *end)
            return send_text(client, "RPRT 1 txcorr must be a signed integer in Hz\n");
        std::string error;
        if (!radio.set_txcorr(value, error)) return send_text(client, "RPRT 1 " + error + "\n");
        return send_text(client, std::to_string(radio.txcorr()) + "\nRPRT 0\n");
    }
    if (command == "R" && !arg.empty() && extra.empty()) {
        uint64_t rx_hz;
        std::string error;
        if (!parse_u64(arg, rx_hz)) return send_text(client, "RPRT 1 invalid RX frequency\n");
        if (!radio.set_rxtx(rx_hz, error)) return send_text(client, "RPRT 1 " + error + "\n");
        return send_text(client, "TXCORR " + std::to_string(radio.txcorr()) + "\n" +
                         "TXFREQ " + std::to_string(radio.frequency()) + "\nRPRT 0\n");
    }
    if (command == "t" && arg.empty())
        return send_text(client, std::string(radio.transmitting() ? "1\n" : "0\n") + "RPRT 0\n");
    if (command == "free" && arg.empty()) {
        std::string error;
        if (!radio.free_gpio(error)) return send_text(client, "RPRT 1 " + error + "\n");
        return send_text(client, "GPIO4 FREE func=INPUT\nRPRT 0\n");
    }
    if (command == "g" && arg.empty())
        return send_text(client, radio.gpio_status() + "\nRPRT 0\n");
    if (command == "F" && !arg.empty() && extra.empty()) {
        uint64_t value;
        std::string error;
        if (!parse_u64(arg, value))
            return send_text(client, "RPRT 1 frequency must be an integer in 20 kHz..1.5 GHz\n");
        if (!radio.set_frequency(value, error)) return send_text(client, "RPRT 1 " + error + "\n");
        return send_text(client, "RPRT 0\n" + std::to_string(radio.frequency()) + "\n");
    }
    if (command == "F+" && !arg.empty() && extra.empty()) {
        if (arg[0] != '+' && arg[0] != '-')
            return send_text(client, "RPRT 1 F+ requires a signed Hz value\n");
        errno = 0;
        char *end = nullptr;
        const long long delta = std::strtoll(arg.c_str(), &end, 10);
        if (errno || !end || *end || delta < -static_cast<long long>(kMaxFrequencyHz) ||
            delta > static_cast<long long>(kMaxFrequencyHz))
            return send_text(client, "RPRT 1 invalid signed frequency step\n");
        std::string error;
        if (!radio.step_frequency(static_cast<int64_t>(delta), error))
            return send_text(client, "RPRT 1 " + error + "\n");
        return send_text(client, "RPRT 0\n" + std::to_string(radio.frequency()) + "\n");
    }
    if (command == "T" && (arg == "0" || arg == "1")) {
        std::string error;
        if (arg == "0") {
            if (!extra.empty()) return send_text(client, "RPRT 1 unexpected argument\n");
            if (!radio.stop(error)) return send_text(client, "RPRT 1 " + error + "\n");
            return send_text(client, "RPRT 0\n");
        }
        uint64_t frequency = radio.frequency();
        if (!extra.empty()) {
            if (input >> command) return send_text(client, "RPRT 1 unexpected argument\n");
            if (!parse_u64(extra, frequency)) return send_text(client, "RPRT 1 invalid frequency\n");
            if (!radio.set_frequency(frequency, error)) return send_text(client, "RPRT 1 " + error + "\n");
        }
        if (!radio.start(frequency, error)) return send_text(client, "RPRT 1 " + error + "\n");
        return send_text(client, "RPRT 0\n");
    }
    return send_text(client, "RPRT 1 unknown or malformed command\n");
}

void usage(const char *name)
{
    std::cerr << "Usage: " << name << " [--bind IPv4] [--port N] [--lock PATH] [--txcorr PATH]\n";
}
} // namespace

int main(int argc, char **argv)
{
    std::string bind_address = "127.0.0.1";
    std::string lock_path = "/run/lock/gpio4tuning-gpio4.lock";
    std::string txcorr_path = kDefaultTxcorrPath;
    uint16_t port = 37357;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bind" && i + 1 < argc) bind_address = argv[++i];
        else if (arg == "--port" && i + 1 < argc) {
            const long value = std::strtol(argv[++i], nullptr, 10);
            if (value < 1 || value > 65535) { usage(argv[0]); return 2; }
            port = static_cast<uint16_t>(value);
        } else if (arg == "--lock" && i + 1 < argc) lock_path = argv[++i];
        else if (arg == "--txcorr" && i + 1 < argc) txcorr_path = argv[++i];
        else { usage(argv[0]); return 2; }
    }

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    const int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) { std::perror("socket"); return 1; }
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        std::cerr << "Invalid IPv4 bind address: " << bind_address << "\n";
        close(server);
        return 2;
    }
    if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        std::perror("bind"); close(server); return 1;
    }
    if (listen(server, 8) != 0) { std::perror("listen"); close(server); return 1; }
    std::cerr << "gpio4tuningd listening on " << bind_address << ':' << port
              << "; carrier OFF; default frequency " << kDefaultFrequencyHz << " Hz\n";

    Radio radio(lock_path, txcorr_path);
    while (g_running.load()) {
        const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            std::perror("accept"); break;
        }
        std::string line;
        bool close_client = false;
        while (!close_client && g_running.load() && read_line(client, line))
            if (!process_command(radio, client, line, close_client)) break;
        close(client);
    }
    radio.stop();
    close(server);
    return 0;
}
