#include "ReadEngine.hpp"
#include "Options.hpp"
#include "ServiceLookup.hpp"
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

using namespace pcc_probe;
using cppc_lab::Code;
namespace {
size_t checks = 0, failures = 0, groups = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
void group(const char *name) { ++groups; std::printf("%s\n", name); }
std::vector<uint8_t> load(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "Missing saved metadata fixture: %s\n", path.c_str()); std::exit(2); }
    struct stat st{};
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 65536) {
        std::fclose(f); std::exit(2);
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(st.st_size));
    const bool ok = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size() && std::fgetc(f) == EOF;
    std::fclose(f);
    if (!ok) std::exit(2);
    return bytes;
}
Bytes view(const std::vector<uint8_t> &v) { return {v.data(), v.size()}; }
void store(std::vector<uint8_t> &bytes, size_t at, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) bytes.at(at+i) = static_cast<uint8_t>(value >> (i*8));
}
void checksum(std::vector<uint8_t> &b) {
    b.at(9) = 0;
    uint8_t sum = 0;
    for (uint8_t byte : b) sum = static_cast<uint8_t>(sum + byte);
    b.at(9) = static_cast<uint8_t>(0u - sum);
}
struct Fixture {
    std::vector<uint8_t> pcct, madt, processors, cpc;
    TableSet tables{};
    std::unique_ptr<Workspace> w{new Workspace{}};
    explicit Fixture(const char *path) : pcct(load(std::string(path)+"/PCCT.bin")),
        madt(load(std::string(path)+"/APIC.bin")), processors(load(std::string(path)+"/SSDT-2.bin")),
        cpc(load(std::string(path)+"/SSDT-3.bin")) {
        for (const auto *bytes : {&pcct, &madt, &processors, &cpc}) considerTable(view(*bytes), tables);
        auto &i = w->info;
        i.abiVersion = AMDZEN_ABI_VERSION; i.structSize = sizeof(i);
        std::strcpy(i.brand, "AMD Ryzen 7 5700X 8-Core Processor");
        i.family = 0x19; i.model = 0x21; i.logicalCpus = 16; i.physicalCores = 8; i.threadsPerCore = 2;
        cppc_lab::Madt m{};
        CHECK(cppc_lab::parseMadt(view(madt), m) == Code::Ok);
        // EXPLICIT TEST-ONLY runtime topology, not measured host numbering. Pick
        // a permutation unrelated to MADT ordering and join by numeric identity.
        for (const auto &cpu : m.cpus) {
            if (!cpu.enabled || cpu.acpiId >= 16) continue;
            const uint32_t xnu = (cpu.acpiId * 5u + 3u) % 16u;
            i.apicId[xnu] = static_cast<uint16_t>(cpu.apicId);
            i.coreOfCpu[xnu] = static_cast<uint16_t>(cpu.acpiId / 2u);
        }
        CHECK(validateProfile(tables, *w) == Result::Ok);
        if (w->discovery.cpuCount != 16) std::exit(2);
    }
};
struct Op { std::string name; uint64_t offset, value; uint8_t bytes; };
class Fake final : public ReadTransport {
public:
    Header header{cppc_lab::kPccSignature, 0, cppc_lab::kComplete}; // Synthetic TEST firmware only.
    uint64_t now = 100000, bell = UINT64_C(0xaabbccddffffffff), step = 50;
    bool lease = true, mapped = false, sent = false, complete = true, commit = false;
    uint32_t clockCalls = 0, headerCalls = 0, illegal = 0, released = 0;
    std::string fail;
    std::vector<Op> ops;
    std::function<void(Fake &, const char *)> hook;
    void event(const char *name, uint64_t offset = 0, uint8_t width = 0, uint64_t value = 0) {
        ops.push_back({name, offset, value, width});
        if (hook) hook(*this, name);
    }
    bool access(const char *name, uint64_t off = 0, uint8_t width = 0, uint64_t value = 0) {
        if (!mapped) ++illegal;
        event(name, off, width, value);
        return fail != name;
    }
    bool claim(const cppc_lab::ChannelDescriptor &) override {
        event("claim"); mapped = lease; return lease;
    }
    void releaseMapping() override {
        if (!mapped) ++illegal;
        event("unmap"); mapped = false; ++released;
    }
    uint64_t nowUs() override { ++clockCalls; if (hook) hook(*this, "clock"); return now; }
    void pause() override { event("pause"); now += step; }
    bool readHeader(Header &out) override {
        ++headerCalls;
        if (sent && complete) header.status = cppc_lab::kComplete;
        const bool ok = access("header"); out = header; return ok;
    }
    bool readDoorbell(uint64_t &out) override {
        const bool ok = access("doorbell-read", 0, 8); out = bell; return ok;
    }
    bool beginCommit(const Control &control, const ClientState &client) override {
        event("begin-commit");
        commit = control.cancellation(client) == Result::Ok && fail != "begin-commit";
        return commit;
    }
    void endCommit() override { event("end-commit"); commit = false; }
    bool writeReadCommand() override {
        if (!commit) ++illegal;
        header.command = 0;
        return access("command-write", 4, 2, 0);
    }
    bool clearStatus() override {
        if (!commit) ++illegal;
        header.status = 0;
        return access("status-write", 6, 2, 0);
    }
    void deviceBarrier() override { (void)access("barrier"); }
    bool writeDoorbell(uint64_t value) override {
        if (!commit) ++illegal;
        bell = value; sent = true;
        return access("doorbell-write", 0, 8, value);
    }
    bool readField(uint64_t offset, uint8_t bytes, uint64_t &out) override {
        const bool ok = access("field-read", offset, bytes);
        out = 0x100u + offset; return ok;
    }
    size_t count(const char *name) const {
        size_t result = 0;
        for (const auto &op : ops) if (op.name == name) ++result;
        return result;
    }
    size_t writes() const { return count("command-write") + count("status-write") + count("doorbell-write"); }
};
struct Run {
    Fixture &fixture;
    std::unique_ptr<Workspace> w;
    Request request{kAbi, sizeof(Request), 0, kAllFields, 100000, kReadAcknowledgement, {0,0}};
    Reply reply{};
    Control control;
    ClientState client;
    std::mutex gate; // Mirrors the service's admission/cancel/reply-commit mutex.
    Fake fake;
    explicit Run(Fixture &f) : fixture(f), w(new Workspace(*f.w)) { prepare(); }
    void prepare() {
        initializeReply(request, reply);
        describe(request, fixture.tables, *w, reply);
    }
    Result call(bool admin = true, bool boot = true, bool config = true, bool nullTransport = false) {
        prepare();
        {
            std::lock_guard<std::mutex> lock(gate);
            const Result entered = control.tryEnter(client);
            if (entered != Result::Ok) return entered;
        }
        const Result result = readOnce(request, admin, boot, config, *w, control, client,
                                       nullTransport ? nullptr : &fake, reply);
        std::lock_guard<std::mutex> lock(gate);
        return completeCall(request, result, control, client, reply);
    }
    bool unknownPcc() const {
        for (uint32_t i = 0; i < kFields; ++i)
            if (reply.fields[i].descriptorKind == static_cast<uint32_t>(DescriptorKind::Pcc) &&
                reply.fields[i].state != static_cast<uint32_t>(ValueState::Unknown)) return false;
        return true;
    }
};

struct LookupFake {
    static constexpr int32_t noMemory = -1, badIterator = -2;
    bool allocate = true, iteratorValid = true;
    int32_t enumerationResult = 0;
    uint32_t iterator = 9;
    int marker = 0;
    size_t matchingCalls = 0, enumerationCalls = 0, nextCalls = 0, validityCalls = 0;
    std::vector<uint32_t> objects, released;
    int *matching(const char *) { ++matchingCalls; return allocate ? &marker : nullptr; }
    int32_t enumerate(int *, uint32_t &out) { ++enumerationCalls; out = iterator; return enumerationResult; }
    uint32_t next(uint32_t) {
        const size_t index = nextCalls++;
        return index < objects.size() ? objects[index] : 0;
    }
    bool valid(uint32_t) { ++validityCalls; return iteratorValid; }
    void release(uint32_t object) { released.push_back(object); }
};
void lookups() {
    group("lookup allocation/API/iterator errors never become confirmed absence");
    const size_t releases[]{0, 1, 0, 1, 2, 3};
    for (unsigned fault = 0; fault < 6; ++fault) {
        LookupFake api;
        switch (fault) {
            case 0: api.allocate = false; break;
            case 1: api.enumerationResult = -3; break;
            case 2: api.enumerationResult = -3; api.iterator = 0; break;
            case 3: api.iteratorValid = false; break;
            case 4: api.iteratorValid = false; api.objects = {101}; break;
            case 5: api.iteratorValid = false; api.objects = {101, 102}; break;
        }
        const ServiceLookup result = lookupUniqueService(api, "probe");
        CHECK(result.state == LookupState::Error && result.service == 0);
        CHECK(result.state != LookupState::Absent); // The CLI fallback condition.
        CHECK(result.error == (fault == 0 ? -1 : (fault == 1 || fault == 2) ? -3 : -2));
        CHECK(api.matchingCalls == 1);
        CHECK(api.enumerationCalls == (fault == 0 ? 0u : 1u));
        CHECK(api.nextCalls == (fault < 3 ? 0u : 2u));
        CHECK(api.validityCalls == (fault < 3 ? 0u : 1u));
        CHECK(api.released.size() == releases[fault]);
        for (uint32_t object : api.released) CHECK(object != 0);
    }
    group("confirmed missing/unique/ambiguous services, successful NULL and handle ownership");
    for (unsigned count = 0; count < 5; ++count) {
        LookupFake api;
        if (count == 1) api.objects = {101};
        else if (count == 2) api.objects = {101, 102};
        else if (count == 3) api.objects = {0, 101}; // Inconsistent enumeration cannot mean absence.
        else if (count == 4) api.iterator = 0; // Documented IOKit success + NULL: no matching services.
        const ServiceLookup result = lookupUniqueService(api, "probe");
        const LookupState expected = count == 0 || count == 4 ? LookupState::Absent : count == 1 ? LookupState::Found : LookupState::Ambiguous;
        CHECK(result.state == expected && result.error == 0);
        CHECK(result.service == (count == 1 ? 101u : 0u));
        CHECK(api.matchingCalls == 1 && api.enumerationCalls == 1 &&
              api.nextCalls == (count == 4 ? 0u : 2u) && api.validityCalls == (count == 4 ? 0u : 1u));
        CHECK(api.released.size() == (count == 2 ? 3u : count == 3 ? 2u : count == 4 ? 0u : 1u));
        if (count != 4) CHECK(api.released[0] == 9);
        if (count == 2) CHECK(api.released[1] == 102 && api.released[2] == 101);
        if (count == 3) CHECK(api.released[1] == 101);
    }
}
void commitment(Fixture &f) {
    group("priority CPU capability loss and restoration invalidate; unrelated changes do not");
    constexpr uint32_t cpu = 1, graphics = 2; // Fake masks; the kernel passes the SDK CPU constant.
    Control capabilities; ClientState capabilityClient;
    capabilities.capabilitiesChanged(cpu, cpu | graphics, cpu);
    CHECK(capabilities.flags() == 0);
    capabilities.capabilitiesChanged(cpu | graphics, cpu, cpu);
    CHECK(capabilities.flags() == 0);
    capabilities.capabilitiesChanged(cpu, 0, cpu);
    CHECK((capabilities.flags() & PowerInvalidated) != 0);
    CHECK(capabilities.tryEnter(capabilityClient) == Result::PowerChanged);
    capabilities.capabilitiesChanged(0, cpu, cpu);
    CHECK(capabilities.tryEnter(capabilityClient) == Result::PowerChanged);
    Control restoration;
    restoration.capabilitiesChanged(0, cpu, cpu);
    CHECK(restoration.tryEnter(capabilityClient) == Result::PowerChanged);

    group("stop/capability-loss/close before mutex-protected reply commitment clear all metadata");
    const Request request{kAbi, sizeof(Request), 0, kAllFields, 0, 0, {0, 0}};
    for (unsigned kind = 0; kind < 3; ++kind) {
        Control control; ClientState client;
        Reply reply{}; initializeReply(request, reply);
        describe(request, f.tables, *f.w, reply);
        reply.flags |= BootEnabled | ConfigEnabled;
        std::mutex gate, handoff;
        std::condition_variable cv;
        bool ready = false, resume = false;
        Result result = Result::Cancelled;
        CHECK(control.tryEnter(client) == Result::Ok);
        std::thread worker([&] {
            {
                std::unique_lock<std::mutex> lock(handoff);
                ready = true; cv.notify_all();
                cv.wait(lock, [&] { return resume; });
            }
            std::lock_guard<std::mutex> lock(gate);
            result = completeCall(request, Result::Ok, control, client, reply);
        });
        {
            std::unique_lock<std::mutex> lock(handoff);
            cv.wait(lock, [&] { return ready; });
        }
        {
            std::lock_guard<std::mutex> lock(gate);
            if (kind == 0) control.stop();
            else if (kind == 1) control.capabilitiesChanged(cpu, 0, cpu);
            else client.close(); // Same mutex as the kernel's closeClient().
        }
        {
            std::lock_guard<std::mutex> lock(handoff);
            resume = true; cv.notify_all();
        }
        worker.join();
        const Result expected = kind == 0 ? Result::Stopped : kind == 1 ? Result::PowerChanged : Result::ClientClosed;
        CHECK(result == expected && reply.result == static_cast<uint32_t>(expected));
        CHECK((reply.flags & (MetadataValid | ActiveCall)) == 0);
        CHECK((reply.flags & (BootEnabled | ConfigEnabled)) == (BootEnabled | ConfigEnabled));
        CHECK(reply.acpiPath[0] == '\0' && reply.sharedBase == 0 && reply.sharedBytes == 0);
        CHECK(reply.startedUs == 0 && reply.completedUs == 0 && reply.doorbellAddress == 0);
        for (const FieldView &field : reply.fields) {
            CHECK(field.state == static_cast<uint32_t>(ValueState::Unknown));
            CHECK(field.descriptorKind == static_cast<uint32_t>(DescriptorKind::Unsupported));
            CHECK(field.value == 0 && field.descriptorAddress == 0 && field.byteOffset == 0);
        }
        CHECK(validateReply(request, Selector::Info, reply, sizeof(reply)));
        CHECK(control.tryEnter(client) == expected);
    }
    group("reply commitment leaves ActiveCall and records a snapshot before later cancellation");
    Control control; ClientState client;
    Reply reply{}; initializeReply(request, reply); describe(request, f.tables, *f.w, reply);
    std::mutex gate;
    CHECK(control.tryEnter(client) == Result::Ok);
    {
        std::lock_guard<std::mutex> lock(gate);
        CHECK(completeCall(request, Result::Ok, control, client, reply) == Result::Ok);
    }
    CHECK((reply.flags & MetadataValid) != 0 && (reply.flags & ActiveCall) == 0);
    {
        std::lock_guard<std::mutex> lock(gate);
        client.close(); control.stop();
    }
    CHECK((reply.flags & (ServiceStopped | PowerInvalidated)) == 0); // Snapshot was committed first.
    CHECK(validateReply(request, Selector::Info, reply, sizeof(reply)));
    CHECK(control.tryEnter(client) == Result::Stopped);
}

void hashes(Fixture &f) {
    group("SHA-256 vectors and locked source provenance");
    const std::array<std::pair<const char *, const char *>, 3> vectors{{
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}
    }};
    constexpr char digits[] = "0123456789abcdef";
    for (const auto &v : vectors) {
        uint8_t digest[32];
        sha256({reinterpret_cast<const uint8_t *>(v.first), std::strlen(v.first)}, digest);
        for (size_t i = 0; i < 32; ++i) {
            CHECK(digits[digest[i] >> 4] == v.second[i*2]);
            CHECK(digits[digest[i] & 15] == v.second[i*2+1]);
        }
    }
    CHECK(hashMatches(view(f.processors), kProcessorHash));
    CHECK(hashMatches(view(f.cpc), kCpcHash));
    CHECK(hashMatches(normalizedProfile(), kNormalizedHash));
    CHECK(hashMatches(view(f.pcct), "5a31529d042b238e4a4dc3d8f7ccbe9cd1e2dbb3845f78caea31b244afcf50a0"));
    CHECK(hashMatches(view(f.madt), "d4879c22b99e84feaa67cd54ca2ea1bfaf16e951e42ecb5654936fb973ef4c23"));
}
void abi(Fixture &f) {
    group("request/selector/inline buffer ABI and non-actuating CLI parser");
    Request info{kAbi, sizeof(Request), 0, kAllFields, 0, 0, {0,0}};
    CHECK(validateRequest(0, info) == Result::Ok);
    CHECK(validateRequest(1, info) == Result::BadRequest);
    for (uint32_t selector : {2u, 3u, UINT32_MAX}) CHECK(validateRequest(selector, info) == Result::BadRequest);
    for (size_t in = 0; in < sizeof(Request)+2; ++in)
        for (size_t out : {size_t(0), sizeof(Reply)-1, sizeof(Reply), sizeof(Reply)+1})
            CHECK(validateWireSizes(0, in, out, 0, 0, false, false) == (in == sizeof(Request) && out == sizeof(Reply)));
    CHECK(!validateWireSizes(2, sizeof(Request), sizeof(Reply), 0, 0, false, false));
    CHECK(!validateWireSizes(0, sizeof(Request), sizeof(Reply), 1, 0, false, false));
    CHECK(!validateWireSizes(0, sizeof(Request), sizeof(Reply), 0, 1, false, false));
    CHECK(!validateWireSizes(0, sizeof(Request), sizeof(Reply), 0, 0, true, false));
    CHECK(!validateWireSizes(0, sizeof(Request), sizeof(Reply), 0, 0, false, true));
    const std::vector<std::vector<const char *>> bad{{"p", "read"}, {"p", "write"}, {"p", "inspect"},
        {"p", "info", kReadAckOption}, {"p", "info", "--timeout-us", "1"},
        {"p", "info", "--cpu", "16"}, {"p", "info", "--cpu", "-1"}, {"p", "--cpu", "0x1"},
        {"p", "--cpu", "18446744073709551616"}, {"p", "--fields", "highest,highest"},
        {"p", "--fields", "highest,"}, {"p", "--fields", ""}, {"p", "--fields", "raw-address"},
        {"p", "--cpu", "0", "--cpu", "1"}, {"p", "read", kReadAckOption, "--timeout-us", "0"},
        {"p", "read", kReadAckOption, "--timeout-us", "1000001"},
        {"p", "read", kReadAckOption, kReadAckOption, "--timeout-us", "1000"},
        {"p", "info", "--fields", "highest", "--fields", "all"}};
    Options options{};
    for (const auto &args : bad) CHECK(!parseOptions(static_cast<int>(args.size()), args.data(), options));
    const char *defaultArgs[]{"p"};
    CHECK(parseOptions(1, defaultArgs, options) && options.selector == Selector::Info && !options.help);
    const char *helpArgs[]{"p", "read", "--help"};
    CHECK(parseOptions(3, helpArgs, options) && options.help);
    const char *readArgs[]{"p", "read", "--cpu", "15", "--fields", "highest,min,max,epp",
                           "--timeout-us", "1000000", kReadAckOption};
    CHECK(parseOptions(static_cast<int>(sizeof(readArgs)/sizeof(readArgs[0])), readArgs, options) &&
          options.selector == Selector::ReadOnce);
    CHECK(options.request.cpu == 15 && options.request.acknowledgement == kReadAcknowledgement);
    Run r(f);
    r.request = info; r.prepare();
    CHECK(validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)-1));
    r.reply.fields[0].state = static_cast<uint32_t>(ValueState::FirmwareRead);
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    r.prepare(); r.reply.fields[0].value = 42;
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    r.prepare(); r.reply.flags |= 0x80000000;
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    r.prepare(); r.reply.result = UINT32_MAX;
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    CHECK(r.fake.ops.empty()); // Info/descriptor formatting has no transport access.
    group("successful INFO requires nonterminal metadata and valid unknown PCC geometry");
    Reply empty{}; initializeReply(info, empty);
    CHECK(!validateReply(info, Selector::Info, empty, sizeof(empty)));
    for (uint32_t terminal : {uint32_t(ServiceStopped), uint32_t(PowerInvalidated)}) {
        r.prepare(); r.reply.flags |= terminal;
        CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        r.prepare();
        auto &field = r.reply.fields[cppc_lab::Highest];
        CHECK(field.state == static_cast<uint32_t>(ValueState::Unknown));
        switch (fault) {
            case 0: field.bitWidth = 0; break;
            case 1: field.bitWidth = 7; break;
            case 2: field.addressSpace = 0; break;
            case 3: field.descriptorAddress = UINT64_MAX; break;
            case 4: ++field.byteOffset; break;
            case 5: field.byteOffset = r.reply.sharedBytes; break;
            case 6: field.descriptorAddress = r.reply.sharedBytes - r.reply.headerBytes;
                    field.byteOffset = r.reply.sharedBytes; break;
            case 7: field.byteOffset = 0; break;
        }
        CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    }
    group("ownership observations and result/value consistency across the wire");
    for (uint32_t ownership : {static_cast<uint32_t>(Ownership::Count), UINT32_MAX}) {
        r.prepare(); r.reply.ownership = ownership;
        CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    }
    r.prepare(); r.reply.ownership = static_cast<uint32_t>(Ownership::PlatformMayOwn);
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    r.prepare(); r.reply.flags |= Attempted | PlatformMayOwn;
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    r.prepare(); r.reply.flags |= Submitted;
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    r.prepare(); r.reply.ownership = static_cast<uint32_t>(Ownership::ObservedOs);
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    r.prepare(); r.reply.flags &= ~uint32_t(MetadataValid);
    CHECK(!validateReply(info, Selector::Info, r.reply, sizeof(r.reply)));
    CHECK(std::strcmp(ownershipName(Ownership::Unknown), "unknown") == 0);
    CHECK(std::strstr(ownershipName(Ownership::ObservedOs), "not an exclusive OS lease") != nullptr);
    CHECK(std::strstr(ownershipName(Ownership::PlatformMayOwn), "timeout does not release") != nullptr);
    CHECK(std::strcmp(ownershipName(Ownership::Count), "invalid-ownership") == 0);
    Run success(f);
    CHECK(success.call() == Result::Ok);
    CHECK(success.reply.ownership == static_cast<uint32_t>(Ownership::ObservedOs));
    CHECK(validateReply(success.request, Selector::ReadOnce, success.reply, sizeof(success.reply)));
    const Reply valid = success.reply;
    for (unsigned fault = 0; fault < 16; ++fault) {
        success.reply = valid;
        auto &field = success.reply.fields[cppc_lab::Highest];
        switch (fault) {
            case 0: success.reply.physicalTransportAvailable = 0; break;
            case 1: success.reply.ownership = static_cast<uint32_t>(Ownership::Unknown); break;
            case 2: success.reply.flags &= ~uint32_t(Submitted); break;
            case 3: success.reply.flags |= PowerInvalidated; break;
            case 4: success.reply.completedUs = success.reply.startedUs - 1; break;
            case 5: field.bitWidth = 0; break;
            case 6: field.bitWidth = 7; break;
            case 7: field.value = UINT64_C(1) << 32; break;
            case 8: field.byteOffset = success.reply.sharedBytes; break;
            case 9: field.addressSpace = 0; break;
            case 10: field.descriptorAddress = UINT64_MAX; break;
            case 11: success.reply.flags &= ~uint32_t(BootEnabled); break;
            case 12: success.reply.flags &= ~uint32_t(ConfigEnabled); break;
            case 13: field.state = static_cast<uint32_t>(ValueState::Unknown); field.value = 0; break;
            case 14: success.reply.completedUs = success.reply.startedUs + success.request.timeoutUs; break;
            case 15: success.reply.completedUs = success.reply.startedUs + success.request.timeoutUs + 1; break;
        }
        CHECK(!validateReply(success.request, Selector::ReadOnce, success.reply, sizeof(success.reply)));
    }
}
void profiles(Fixture &f) {
    group("both current SSDTs required; missing, duplicated, mutated and truncated tables");
    auto w = std::unique_ptr<Workspace>(new Workspace(*f.w));
    TableSet tables = f.tables;
    for (unsigned which = 0; which < 4; ++which) {
        tables = f.tables;
        uint32_t *counts[]{&tables.pcctCount, &tables.madtCount, &tables.processorCount, &tables.cpcCount};
        *counts[which] = 0;
        CHECK(validateProfile(tables, *w) == Result::ProfileMismatch);
        CHECK(w->discovery.cpuCount == 0);
        *counts[which] = 2;
        CHECK(validateProfile(tables, *w) == Result::ProfileMismatch);
    }
    for (const auto *source : {&f.processors, &f.cpc}) {
        std::vector<uint8_t> modified = *source;
        modified.back() ^= 1; checksum(modified);
        tables = f.tables;
        if (source == &f.processors) tables.processors = view(modified); else tables.cpc = view(modified);
        CHECK(validateProfile(tables, *w) == Result::ProfileMismatch);
        TableSet scanned{};
        considerTable(view(modified), scanned);
        CHECK(scanned.cpcCount == 0 && scanned.processorCount == 0);
    }
    for (size_t length = 0; length < f.pcct.size(); ++length) {
        tables = f.tables; tables.pcct.size = length;
        CHECK(validateProfile(tables, *w) == Result::DescriptorInvalid);
        CHECK(w->discovery.cpuCount == 0);
    }
    std::vector<uint8_t> bad = f.pcct;
    bad[9] ^= 1; tables = f.tables; tables.pcct = view(bad);
    CHECK(validateProfile(tables, *w) == Result::DescriptorInvalid);
    const std::array<std::pair<size_t, uint8_t>, 7> changes{{{8,3},{36,1},{48,1},{49,61},{72,1},{73,32},{75,3}}};
    for (const auto &change : changes) {
        bad = f.pcct; bad[change.first] = change.second; checksum(bad); tables.pcct = view(bad);
        CHECK(validateProfile(tables, *w) != Result::Ok);
        CHECK(w->discovery.cpuCount == 0);
    }
    group("PCCT addresses derived per supplied current table; bounds, aliases and profile shape");
    bad = f.pcct;
    // Test-only addresses, never supplied to any kernel ABI or mapped by tests.
    store(bad, 56, 0x300000, 8); store(bad, 76, 0x500000, 8); checksum(bad);
    tables = f.tables; tables.pcct = view(bad);
    CHECK(validateProfile(tables, *w) == Result::Ok);
    CHECK(w->discovery.pcct.channels[0].baseAddress == 0x300000);
    CHECK(w->discovery.pcct.channels[0].doorbell.address == 0x500000);
    for (uint64_t address : {UINT64_C(0), UINT64_C(3), UINT64_MAX, UINT64_C(1)<<52}) {
        store(bad, 56, address, 8); checksum(bad);
        CHECK(validateProfile(tables, *w) != Result::Ok);
    }
    bad = f.pcct; store(bad, 56, 0x300000, 8); store(bad, 76, 0x300008, 8); checksum(bad); tables.pcct = view(bad);
    CHECK(validateProfile(tables, *w) == Result::DescriptorInvalid);
    bad = f.pcct; store(bad, 100, 4001, 4); checksum(bad); tables.pcct = view(bad);
    CHECK(validateProfile(tables, *w) == Result::ProfileMismatch);
    bad = f.pcct; store(bad, 84, UINT64_MAX, 8); checksum(bad); tables.pcct = view(bad);
    CHECK(validateProfile(tables, *w) == Result::ProfileMismatch);
    CHECK(validateProfile(f.tables, *w) == Result::Ok);
    group("independent runtime APIC/XNU/core join, not name or MADT-order numbering");
    for (uint32_t acpi = 0; acpi < 16; ++acpi) {
        const auto &cpu = w->discovery.cpus[acpi];
        CHECK(cpu.acpiId == acpi);
        CHECK(cpu.xnuCpu == (acpi * 5 + 3) % 16);
    }
    const AMDZenInfo original = w->info;
    for (unsigned fault = 0; fault < 11; ++fault) {
        w->info = original;
        switch (fault) {
            case 0: ++w->info.abiVersion; break;
            case 1: --w->info.structSize; break;
            case 2: w->info.logicalCpus = 15; break;
            case 3: w->info.family = 0x17; break;
            case 4: w->info.model = 0x61; break;
            case 5: w->info.apicId[0] = w->info.apicId[1]; break;
            case 6: w->info.apicId[0] = 0xffff; break;
            case 7: w->info.coreOfCpu[0] = 8; break;
            case 8: w->info.coreOfCpu[0] = static_cast<uint16_t>((w->info.coreOfCpu[0]+1)%8); break;
            case 9: std::memset(w->info.brand, 'X', sizeof(w->info.brand)); break;
            case 10: w->info.controlBlockReason = kAMDZenBlockHypervisor; break;
        }
        CHECK(validateProfile(f.tables, *w) == Result::IdentityMismatch);
        CHECK(w->discovery.cpuCount == 0);
    }
    w->info = original;
    std::swap(w->info.apicId[0], w->info.apicId[15]);
    std::swap(w->info.coreOfCpu[0], w->info.coreOfCpu[15]);
    CHECK(validateProfile(f.tables, *w) == Result::Ok);
    w->info = original;
    std::vector<uint8_t> reordered = f.madt;
    std::vector<size_t> records;
    for (size_t at = 44; at < reordered.size(); at += reordered.at(at+1))
        if (reordered.at(at) == 0) records.push_back(at);
    for (size_t i = 0; i < records.size()/2; ++i)
        for (size_t b = 0; b < 8; ++b) std::swap(reordered[records[i]+b], reordered[records[records.size()-1-i]+b]);
    tables = f.tables; tables.madt = view(reordered);
    CHECK(validateProfile(tables, *w) == Result::Ok);
}
void gating(Fixture &f) {
    group("administrator, boot/config, malformed request and metadata gates: zero access");
    for (unsigned fault = 0; fault < 13; ++fault) {
        Run r(f);
        bool admin = true, boot = true, config = true;
        Result expected = Result::BadRequest;
        switch (fault) {
            case 0: admin = false; expected = Result::NotAdministrator; break;
            case 1: boot = false; expected = Result::GateClosed; break;
            case 2: config = false; expected = Result::GateClosed; break;
            case 3: r.request.acknowledgement = 0; break;
            case 4: r.request.reserved[1] = 1; break;
            case 5: r.request.structSize = 0; break;
            case 6: r.request.abiVersion = 2; break;
            case 7: r.request.cpu = 16; break;
            case 8: r.request.fieldMask = 0; break;
            case 9: r.request.fieldMask |= 1u<<31; break;
            case 10: r.request.timeoutUs = kMaxTimeoutUs+1; break;
            case 11: r.request.timeoutUs = 0; break;
            case 12: r.w->discovery.cpuCount = 0; expected = Result::MetadataUnavailable; break;
        }
        CHECK(r.call(admin, boot, config) == expected);
        CHECK(r.fake.ops.empty() && r.fake.clockCalls == 0);
        CHECK((r.control.flags() & Attempted) == 0);
    }
    group("production null transport refuses without MMIO even with all gates satisfied");
    Run blocked(f);
    CHECK(blocked.call(true, true, true, true) == Result::TransportUnavailable);
    CHECK(blocked.fake.ops.empty() && blocked.fake.clockCalls == 0);
    CHECK((blocked.control.flags() & Attempted) != 0);
    CHECK((blocked.control.flags() & (Submitted | PlatformMayOwn)) == 0);
    CHECK(blocked.call(true, true, true, true) == Result::AlreadyAttempted);
    CHECK(blocked.unknownPcc());
    Run denied(f); denied.fake.lease = false;
    CHECK(denied.call() == Result::TransportUnavailable);
    CHECK(denied.fake.count("claim") == 1 && denied.fake.writes() == 0 && denied.fake.released == 0);
}
void readProtocol(Fixture &f) {
    group("actual observed header required: no synthetic acquisition writes");
    for (unsigned fault = 0; fault < 9; ++fault) {
        Run r(f);
        switch (fault) {
            case 0: r.fake.header.signature = 0; break;
            case 1: r.fake.header.signature |= 1; break;
            case 2: r.fake.header.command = 1; break;
            case 3: r.fake.header.command = 0x8000; break;
            case 4: r.fake.header.status = 0; break;
            case 5: r.fake.header.status = cppc_lab::kError; break;
            case 6: r.fake.header.status = cppc_lab::kComplete | cppc_lab::kError; break;
            case 7: r.fake.header.status = cppc_lab::kComplete | cppc_lab::kSciDoorbell; break;
            case 8: r.fake.header.status = cppc_lab::kComplete | cppc_lab::kPlatformNotify; break;
        }
        CHECK(r.call() == Result::UnknownOwnership);
        CHECK(r.fake.writes() == 0 && r.fake.count("doorbell-read") == 0);
        CHECK(r.fake.released == 1 && r.fake.illegal == 0 && r.unknownPcc());
    }
    Run changed(f);
    changed.fake.hook = [](Fake &t, const char *op) {
        if (std::strcmp(op, "header") == 0 && t.headerCalls == 2) t.header.status = 0;
    };
    CHECK(changed.call() == Result::UnknownOwnership && changed.fake.writes() == 0);
    group("exact command/status/barrier/masked doorbell order and selected fields only");
    Run r(f);
    r.request.fieldMask = (1u << cppc_lab::Highest) | (1u << cppc_lab::Desired) |
                          (1u << cppc_lab::ReferenceCounter) | (1u << cppc_lab::LowestFrequency);
    CHECK(r.call() == Result::Ok);
    CHECK(r.fake.writes() == 3 && r.fake.illegal == 0 && r.fake.released == 1);
    CHECK(r.fake.bell == UINT64_C(0xaabbccdd00000001));
    std::vector<Op> writes;
    size_t commandAt = 0;
    for (size_t i = 0; i < r.fake.ops.size(); ++i) {
        const auto &op = r.fake.ops[i];
        if (op.name.find("-write") != std::string::npos) writes.push_back(op);
        if (op.name == "command-write") commandAt = i;
    }
    CHECK(writes.size() == 3);
    if (writes.size() == 3) {
        CHECK(writes[0].name == "command-write" && writes[0].offset == 4 && writes[0].bytes == 2 && writes[0].value == 0);
        CHECK(writes[1].name == "status-write" && writes[1].offset == 6 && writes[1].bytes == 2 && writes[1].value == 0);
        CHECK(writes[2].name == "doorbell-write" && writes[2].bytes == 8);
    }
    CHECK(r.fake.ops.at(commandAt+1).name == "status-write");
    CHECK(r.fake.ops.at(commandAt+2).name == "barrier");
    CHECK(r.fake.ops.at(commandAt+3).name == "doorbell-write");
    CHECK(r.fake.ops.at(commandAt+4).name == "barrier");
    CHECK(r.fake.count("field-read") == 2);
    CHECK(r.reply.fields[cppc_lab::Highest].state == static_cast<uint32_t>(ValueState::FirmwareRead));
    CHECK(r.reply.fields[cppc_lab::ReferenceCounter].state == static_cast<uint32_t>(ValueState::Unknown));
    CHECK(r.reply.fields[cppc_lab::LowestFrequency].state == static_cast<uint32_t>(ValueState::StaticInteger));
    CHECK(r.reply.fields[cppc_lab::Minimum].state == static_cast<uint32_t>(ValueState::NotRequested));
    CHECK(r.reply.completedUs >= r.reply.startedUs + 1000); // MPAR guard, not nominal-derived timeout.
    CHECK(validateReply(r.request, Selector::ReadOnce, r.reply, sizeof(r.reply)));
    const size_t ops = r.fake.ops.size();
    CHECK(r.call() == Result::AlreadyAttempted && r.fake.ops.size() == ops);
    group("mask-preserving doorbell calculation over varied old values");
    uint64_t old = 0x1234;
    for (unsigned n = 0; n < 256; ++n) {
        old = old * UINT64_C(6364136223846793005) + 1;
        Run varied(f); varied.fake.bell = old; varied.request.fieldMask = 1;
        CHECK(varied.call() == Result::Ok);
        CHECK(varied.fake.bell == ((old & UINT64_C(0xffffffff00000000)) | 1));
        CHECK(varied.fake.writes() == 3 && varied.fake.illegal == 0);
    }
    group("firmware errors, protocol faults, and partially failed submission quarantine");
    for (unsigned fault = 0; fault < 6; ++fault) {
        Run bad(f); bad.fake.complete = false;
        bad.fake.hook = [fault](Fake &t, const char *op) {
            if (!t.sent || std::strcmp(op, "header") != 0) return;
            switch (fault) {
                case 0: t.header.status = cppc_lab::kComplete | cppc_lab::kError; break;
                case 1: t.header.status = cppc_lab::kError; break;
                case 2: t.header.status = cppc_lab::kPlatformNotify; break;
                case 3: t.header.status = 0x8000; break;
                case 4: t.header.signature ^= 1; break;
                case 5: t.header.command = 1; break;
            }
        };
        CHECK(bad.call() == (fault < 2 ? Result::FirmwareError : Result::Protocol));
        CHECK(bad.unknownPcc() && bad.fake.released == 1);
        CHECK(((bad.control.flags() & PlatformMayOwn) != 0) == (fault != 0));
        CHECK(((bad.control.flags() & Quarantined) != 0) == (fault != 0));
    }
    for (const char *fail : {"command-write", "status-write", "doorbell-write"}) {
        Run bad(f); bad.fake.fail = fail;
        CHECK(bad.call() == Result::AccessFailed);
        CHECK((bad.control.flags() & (PlatformMayOwn | Quarantined)) == (PlatformMayOwn | Quarantined));
        CHECK(bad.fake.count("doorbell-write") <= 1 && bad.fake.released == 1);
        const size_t count = bad.fake.ops.size();
        CHECK(bad.call() == Result::AlreadyAttempted && bad.fake.ops.size() == count);
    }
}
void clocks(Fixture &f) {
    group("bounded deadlines, timeout, late completion, backwards and frozen clocks");
    Run overflow(f); overflow.fake.now = UINT64_MAX - 1;
    CHECK(overflow.call() == Result::Deadline && overflow.fake.ops.empty());
    Run shortDeadline(f); shortDeadline.request.timeoutUs = 1;
    CHECK(shortDeadline.call() == Result::Timeout && shortDeadline.fake.writes() == 0);
    for (bool sent : {false, true}) {
        Run backward(f); backward.fake.complete = false;
        backward.fake.hook = [sent](Fake &t, const char *op) {
            if (std::strcmp(op, "clock") == 0 && (sent ? t.sent : t.clockCalls == 2)) t.now = 0;
        };
        CHECK(backward.call() == Result::TimeBackwards && backward.unknownPcc());
        CHECK(((backward.control.flags() & Quarantined) != 0) == sent);
        CHECK(backward.fake.writes() == (sent ? 3u : 0u));
    }
    Run frozen(f); frozen.fake.step = 0;
    CHECK(frozen.call() == Result::Timeout && frozen.fake.writes() == 0);
    CHECK(frozen.fake.count("pause") < 20000);
    Run timeout(f); timeout.fake.complete = false; timeout.request.timeoutUs = 2000;
    CHECK(timeout.call() == Result::Timeout);
    CHECK((timeout.control.flags() & (Quarantined | PlatformMayOwn)) == (Quarantined | PlatformMayOwn));
    const size_t count = timeout.fake.ops.size();
    timeout.fake.header.status = cppc_lab::kComplete; // Late firmware completion, not observed by host.
    CHECK(timeout.call() == Result::AlreadyAttempted && timeout.fake.ops.size() == count);
    CHECK((timeout.control.flags() & PlatformMayOwn) != 0);
    for (uint64_t delta : {UINT64_C(0), UINT64_C(1)}) {
        Run late(f);
        const uint64_t deadline = late.fake.now + late.request.timeoutUs;
        late.fake.hook = [deadline, delta](Fake &t, const char *op) {
            if (t.sent && std::strcmp(op, "header") == 0) t.now = deadline + delta;
        };
        CHECK(late.call() == Result::LateCompletion && late.unknownPcc());
        CHECK((late.control.flags() & Quarantined) != 0);
        CHECK((late.control.flags() & PlatformMayOwn) == 0);
        CHECK(late.fake.count("field-read") == 0);
    }
    Run stale(f);
    stale.fake.hook = [](Fake &t, const char *op) { if (std::strcmp(op, "field-read") == 0) t.now += 100000; };
    CHECK(stale.call() == Result::LateCompletion && stale.unknownPcc());
    CHECK(stale.reply.completedUs == 0);
}
void cancellation(Fixture &f) {
    group("one channel across clients: concurrent calls rejected, not queued/retried");
    Control control; ClientState a, b;
    std::mutex mutex; std::condition_variable cv;
    bool entered = false, release = false;
    Result workerResult = Result::Cancelled;
    std::thread worker([&] {
        workerResult = control.tryEnter(a);
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return release; });
        control.leave();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return entered; });
        CHECK(control.tryEnter(b) == Result::Busy);
        release = true; cv.notify_all();
    }
    worker.join();
    CHECK(workerResult == Result::Ok && (control.flags() & ActiveCall) == 0);
    CHECK(control.tryEnter(b) == Result::Ok); control.leave();
    group("stop/sleep/client close before acquisition and during in-flight requests");
    for (unsigned kind = 0; kind < 3; ++kind) {
        Run early(f);
        auto cancel = [kind](Run &r) {
            if (kind == 0) r.control.stop(); else if (kind == 1) r.control.powerChanged(); else r.client.close();
        };
        const Result expected = kind == 0 ? Result::Stopped : kind == 1 ? Result::PowerChanged : Result::ClientClosed;
        cancel(early);
        CHECK(early.call() == expected && early.fake.ops.empty());
        Run active(f); active.fake.complete = false;
        std::mutex m; std::condition_variable condition;
        bool inFlight = false, resume = false;
        active.fake.hook = [&](Fake &t, const char *op) {
            if (!t.sent || std::strcmp(op, "pause") != 0) return;
            std::unique_lock<std::mutex> lock(m);
            inFlight = true; condition.notify_all();
            condition.wait(lock, [&] { return resume; });
        };
        Result result = Result::Cancelled;
        std::thread caller([&] { result = active.call(); });
        {
            std::unique_lock<std::mutex> lock(m);
            condition.wait(lock, [&] { return inFlight; });
            CHECK((active.control.flags() & ActiveCall) != 0);
            cancel(active);
            resume = true; condition.notify_all();
        }
        caller.join();
        CHECK(result == expected && active.unknownPcc());
        CHECK(active.fake.released == 1 && !active.fake.mapped && active.fake.illegal == 0);
        CHECK((active.control.flags() & (PlatformMayOwn | Quarantined)) == (PlatformMayOwn | Quarantined));
        CHECK((active.control.flags() & ActiveCall) == 0);
        const size_t count = active.fake.ops.size();
        CHECK(active.call() == expected && active.fake.ops.size() == count);
        if (kind == 1) {
            // A wake notification does not clear invalidation or re-arm anything.
            active.control.powerChanged();
            CHECK(active.call() == Result::PowerChanged && active.fake.ops.size() == count);
        }
    }
    group("cancellation before commit, cancellation during collection, no post-unmap access");
    Run commit(f);
    commit.fake.hook = [&](Fake &, const char *op) {
        if (std::strcmp(op, "begin-commit") == 0) commit.control.stop();
    };
    CHECK(commit.call() == Result::Stopped && commit.fake.writes() == 0);
    Run collection(f);
    collection.fake.hook = [&](Fake &, const char *op) {
        if (std::strcmp(op, "field-read") == 0) collection.client.close();
    };
    CHECK(collection.call() == Result::ClientClosed && collection.unknownPcc());
    CHECK(collection.fake.count("field-read") == 1 && collection.fake.illegal == 0);
    CHECK(collection.reply.completedUs == 0);
}
}
int main(int argc, const char *argv[]) {
    if (argc != 2) { std::fputs("usage: tests saved-metadata-directory\n", stderr); return 2; }
    Fixture fixture(argv[1]);
    lookups(); commitment(fixture);
    hashes(fixture); abi(fixture); profiles(fixture); gating(fixture);
    readProtocol(fixture); clocks(fixture); cancellation(fixture);
    std::printf("%zu groups, %zu checks, %zu failures (fake transport / saved metadata only)\n", groups, checks, failures);
    return failures == 0 ? 0 : 1;
}
