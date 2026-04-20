#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

const size_t kProjectMaxCachedFileBytes = 1024 * 1024;
const size_t kProjectMaxPrebuiltResponseBytes = 128 * 1024;

// The real project uses 1 MB / 128 KB.
// The demo uses smaller limits so the behavior is easy to see.
const size_t kDemoMaxCachedFileBytes = 64;
const size_t kDemoMaxPrebuiltResponseBytes = 32;

struct StaticFileCacheEntry {
    std::string body;
    std::string content_type;
    std::string keep_alive_response;
    std::string close_response;
};

std::string build_response_buffer(
    const std::string& status_line,
    const std::string& content_type,
    const std::string& body,
    bool keep_alive
) {
    std::string response;
    response += status_line;
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: ";
    response += keep_alive ? "keep-alive\r\n" : "close\r\n";
    response += "\r\n";
    response += body;
    return response;
}

bool read_file(const std::string& filename, std::string& content) {
    std::ifstream ifs(filename.c_str(), std::ios::binary);
    if (!ifs) {
        std::cout << "open failed: " << filename << "\n";
        return false;
    }

    ifs.seekg(0, std::ios::end);
    std::streamoff size = ifs.tellg();
    if (size < 0) {
        std::cout << "size read failed: " << filename << "\n";
        return false;
    }

    content.resize(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    if (size > 0) {
        ifs.read(&content[0], static_cast<std::streamsize>(size));
        if (!ifs) {
            std::cout << "content read failed: " << filename << "\n";
            return false;
        }
    }

    std::cout << "disk read: " << filename << " (" << content.size() << " bytes)\n";
    return true;
}

void write_file(const std::string& filename, const std::string& content) {
    std::ofstream ofs(filename.c_str(), std::ios::binary);
    ofs << content;
}

class LocalFileCache {
public:
    std::shared_ptr<const StaticFileCacheEntry> get_static_file(const std::string& file_path) {
        {
            std::lock_guard<std::mutex> lock(cache_mtx_);
            std::unordered_map<std::string, std::shared_ptr<const StaticFileCacheEntry> >::iterator it =
                cache_.find(file_path);
            if (it != cache_.end()) {
                std::cout << "cache hit: " << file_path << "\n";
                return it->second;
            }
        }

        std::string content;
        if (!read_file(file_path, content)) {
            return std::shared_ptr<const StaticFileCacheEntry>();
        }

        std::shared_ptr<StaticFileCacheEntry> entry(new StaticFileCacheEntry());
        entry->body.swap(content);
        entry->content_type = "text/plain";

        if (entry->body.size() <= kDemoMaxPrebuiltResponseBytes) {
            entry->keep_alive_response = build_response_buffer(
                "HTTP/1.1 200 OK\r\n",
                entry->content_type,
                entry->body,
                true
            );
            entry->close_response = build_response_buffer(
                "HTTP/1.1 200 OK\r\n",
                entry->content_type,
                entry->body,
                false
            );
            std::cout << "prebuilt keep-alive/close responses\n";
        } else {
            std::cout << "body cached only, response not prebuilt\n";
        }

        if (entry->body.size() <= kDemoMaxCachedFileBytes) {
            std::lock_guard<std::mutex> lock(cache_mtx_);
            std::shared_ptr<const StaticFileCacheEntry>& slot = cache_[file_path];
            if (!slot) {
                slot = entry;
                std::cout << "stored in local cache: " << file_path << "\n";
            }
            return slot;
        }

        std::cout << "too large, return once but do not cache: " << file_path << "\n";
        return entry;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<const StaticFileCacheEntry> > cache_;
    std::mutex cache_mtx_;
};

} // namespace

int main() {
    const std::string small_file = "demo_small.txt";
    const std::string large_file = "demo_large.txt";

    write_file(small_file, "hello local cache");
    write_file(large_file, std::string(80, 'A'));

    std::cout << "Real project limits: cache <= " << kProjectMaxCachedFileBytes
              << ", prebuild <= " << kProjectMaxPrebuiltResponseBytes << "\n";
    std::cout << "Demo limits: cache <= " << kDemoMaxCachedFileBytes
              << ", prebuild <= " << kDemoMaxPrebuiltResponseBytes << "\n\n";

    LocalFileCache cache;

    std::cout << "[small file] request #1\n";
    std::shared_ptr<const StaticFileCacheEntry> small_1 = cache.get_static_file(small_file);
    std::cout << "[small file] request #2\n";
    std::shared_ptr<const StaticFileCacheEntry> small_2 = cache.get_static_file(small_file);
    std::cout << "same object after second request? "
              << (small_1.get() == small_2.get() ? "yes" : "no") << "\n";
    std::cout << "prebuilt response empty? "
              << (small_1->keep_alive_response.empty() ? "yes" : "no") << "\n\n";

    std::cout << "[large file] request #1\n";
    std::shared_ptr<const StaticFileCacheEntry> large_1 = cache.get_static_file(large_file);
    std::cout << "[large file] request #2\n";
    std::shared_ptr<const StaticFileCacheEntry> large_2 = cache.get_static_file(large_file);
    std::cout << "same object after second request? "
              << (large_1.get() == large_2.get() ? "yes" : "no") << "\n";
    std::cout << "prebuilt response empty? "
              << (large_1->keep_alive_response.empty() ? "yes" : "no") << "\n";

    std::remove(small_file.c_str());
    std::remove(large_file.c_str());
    return 0;
}
