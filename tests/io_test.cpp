// Fault injection and concurrent writes exercise preservation of config.json.
#include "json.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

enum class Fault { None, Rename, MissingTemp, ShortWrite, Sync };
Fault gFault = Fault::None;
int gFailures = 0;
int gChecks = 0;

void check(bool condition, const char* message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        printf("FAIL: %s\n", message);
    }
}

std::string read(const std::filesystem::path& path) {
    std::string content;
    std::string error;
    check(coui::readFile(path.string(), content, error), "read test file");
    return content;
}

bool noTemps(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().filename().string().find(".tmp.") != std::string::npos) return false;
    }
    return true;
}

} // namespace

extern "C" int __real_rename(const char*, const char*);
extern "C" size_t __real_fwrite(const void*, size_t, size_t, FILE*);
extern "C" int __real_fsync(int);

extern "C" int __wrap_rename(const char* from, const char* to) {
    if (gFault == Fault::MissingTemp) {
        unlink(from);
        errno = ENOENT;
        return -1;
    }
    if (gFault == Fault::Rename) {
        errno = EACCES;
        return -1;
    }
    return __real_rename(from, to);
}

extern "C" size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* file) {
    if (gFault == Fault::ShortWrite) {
        size_t written = __real_fwrite(data, size, count / 2, file);
        errno = ENOSPC;
        return written;
    }
    return __real_fwrite(data, size, count, file);
}

extern "C" int __wrap_fsync(int fd) {
    if (gFault == Fault::Sync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int main() {
    const char* base = getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/closeoreui-io-XXXXXX";
    char* directory = mkdtemp(pattern.data());
    if (directory == nullptr) return 1;
    const std::filesystem::path root = directory;
    const auto path = root / "config.json";
    const std::string seed = "{\"enabled\":false}\n";
    std::string error;
    const auto collision = path.string() + ".tmp." +
            std::to_string(static_cast<uint64_t>(getpid())) + ".0";
    FILE* existing = fopen(collision.c_str(), "wb");
    check(existing != nullptr, "create preexisting exclusive-name collision");
    if (existing == nullptr) return 1;
    const std::string marker = "do not overwrite";
    check(fwrite(marker.data(), 1, marker.size(), existing) == marker.size(), "write collision marker");
    check(fclose(existing) == 0, "close collision marker");
    check(coui::writeFileAtomically(path.string(), seed, error), "create original config");
    check(read(collision) == marker, "exclusive creation retries without overwriting colliding files");
    std::filesystem::remove(collision);
    check(coui::writeFileAtomically(path.string() + ".tmp", "legacy temporary file", error),
          "create preexisting shared temporary file");
    for (Fault fault : {Fault::Rename, Fault::MissingTemp, Fault::ShortWrite, Fault::Sync}) {
        gFault = fault;
        error.clear();
        check(!coui::writeFileAtomically(path.string(), "{\"enabled\":true}\n", error),
              "injected I/O failure reported");
        gFault = Fault::None;
        check(!error.empty(), "I/O error includes a message");
        check(read(path) == seed, "failed save preserves every original byte");
        check(noTemps(root), "failed save cleans up only its private temporary file");
    }
    check(coui::writeFileAtomically(path.string(), "", error), "empty file can be atomically saved");
    check(read(path).empty(), "successful empty write replaces the original");
    check(read(path.string() + ".tmp") == "legacy temporary file", "shared .tmp was not touched");
    check(coui::writeFileAtomically(path.string(), "{\"writer\":\"seed\"}", error), "seed concurrent test");

    std::atomic<int> done{0};
    std::atomic<int> writeErrors{0};
    auto writer = [&](char marker) {
        std::string content = "{\"writer\":\"" + std::string(16384, marker) + "\"}";
        for (int i = 0; i < 20; ++i) {
            std::string writeError;
            if (!coui::writeFileAtomically(path.string(), content, writeError)) ++writeErrors;
        }
        ++done;
    };
    std::thread first(writer, 'a');
    std::thread second(writer, 'b');
    bool wholeFiles = true;
    do {
        std::string content;
        std::string readError;
        coui::Json json;
        if (!coui::readFile(path.string(), content, readError) ||
            !coui::Json::parse(content, json, readError) || json.find("writer") == nullptr) {
            wholeFiles = false;
        } else {
            const std::string value = json.find("writer")->asString();
            if (value != "seed" && (value.size() != 16384 ||
                (value[0] != 'a' && value[0] != 'b') ||
                value.find_first_not_of(value[0]) != std::string::npos)) wholeFiles = false;
        }
    } while (done.load() != 2);
    first.join();
    second.join();
    check(writeErrors == 0, "concurrent writers do not steal each other's temporary files");
    check(wholeFiles, "concurrent readers see only complete JSON snapshots");
    check(noTemps(root), "successful writes leave no private temporary files");
    std::filesystem::remove_all(root); // only this test's mkdtemp directory
    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures != 0;
}
