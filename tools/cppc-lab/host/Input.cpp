#include "Input.hpp"
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cppc_lab_host {
using namespace cppc_lab;

bool readFile(const char *path, uint8_t *out, size_t capacity, size_t &size) {
    size = 0;
    // Only explicit regular files. Nonblocking/no-follow avoids FIFOs and
    // symlink surprises; no mmap, device reads, or executable input loading.
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) { fprintf(stderr, "Cannot open regular input: %s\n", path); return false; }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > capacity) {
        close(fd);
        fprintf(stderr, "Input is not a bounded regular file: %s\n", path);
        return false;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) { close(fd); return false; }
    const size_t got = fread(out, 1, capacity, file);
    const bool ok = !ferror(file) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!ok) { fprintf(stderr, "Cannot read bounded input: %s\n", path); return false; }
    size = got;
    return true;
}

bool loadInputs(bool hexTables, const char *pcctPath, const char *madtPath,
                const char *cpcPath, const char *mapPath, Inputs &out) {
    uint8_t text[kMaxTextBytes];
    size_t size = 0;
    Code code = Code::Ok;
    const char *const paths[] = {pcctPath, madtPath};
    uint8_t *const buffers[] = {out.pcctBytes, out.madtBytes};
    size_t *const lengths[] = {&out.pcctSize, &out.madtSize};
    for (size_t i = 0; i < 2; ++i) {
        if (hexTables) {
            if (!readFile(paths[i], text, sizeof(text), size)) return false;
            code = decodeHex({text, size}, buffers[i], kMaxTableBytes, *lengths[i]);
            if (code != Code::Ok) { fprintf(stderr, "%s: %s\n", paths[i], codeName(code)); return false; }
        } else if (!readFile(paths[i], buffers[i], kMaxTableBytes, *lengths[i])) return false;
    }
    code = parsePcct({out.pcctBytes, out.pcctSize}, out.pcct);
    if (code != Code::Ok) { fprintf(stderr, "PCCT: %s\n", codeName(code)); return false; }
    code = parseMadt({out.madtBytes, out.madtSize}, out.madt);
    if (code != Code::Ok) { fprintf(stderr, "MADT: %s\n", codeName(code)); return false; }
    if (!readFile(cpcPath, text, sizeof(text), size)) return false;
    code = parseStaticCpc({text, size}, out.cpc);
    if (code != Code::Ok) { fprintf(stderr, "Static CPC: %s\n", codeName(code)); return false; }
    if (!readFile(mapPath, text, sizeof(text), size)) return false;
    code = parseCpuMap({text, size}, out.map);
    if (code != Code::Ok) { fprintf(stderr, "Caller CPU map: %s\n", codeName(code)); return false; }
    return true;
}
} // namespace cppc_lab_host
