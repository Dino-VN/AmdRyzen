#ifndef AMDZEN_PCC_SERVICE_LOOKUP_HPP
#define AMDZEN_PCC_SERVICE_LOOKUP_HPP
#include <stdint.h>

namespace pcc_probe {
enum class LookupState { Found, Absent, Ambiguous, Error };
struct ServiceLookup {
    LookupState state;
    uint32_t service;
    int32_t error;
};

// Api mirrors the small IOKit lookup surface. enumerate() consumes the matching
// dictionary on both success and failure. A returned service belongs to caller.
// Host tests supply fake handles; only cli/main.cpp binds this to IOKit.
template <class Api>
ServiceLookup lookupUniqueService(Api &api, const char *name) {
    auto matching = api.matching(name);
    if (!matching) return {LookupState::Error, 0, Api::noMemory};
    uint32_t iterator = 0;
    const int32_t error = api.enumerate(matching, iterator);
    if (error != 0) {
        if (iterator != 0) api.release(iterator);
        return {LookupState::Error, 0, error};
    }
    // IOKitLib explicitly returns success + NULL for no matching services.
    // This is confirmed absence, not allocation failure or an invalid iterator.
    if (iterator == 0) return {LookupState::Absent, 0, 0};
    const uint32_t service = api.next(iterator);
    const uint32_t extra = api.next(iterator);
    const bool valid = api.valid(iterator);
    api.release(iterator);
    if (!valid) {
        if (extra != 0) api.release(extra);
        if (service != 0) api.release(service);
        return {LookupState::Error, 0, Api::badIterator};
    }
    if (extra != 0) {
        api.release(extra);
        if (service != 0) api.release(service);
        return {LookupState::Ambiguous, 0, 0};
    }
    return {service != 0 ? LookupState::Found : LookupState::Absent, service, 0};
}
} // namespace pcc_probe
#endif
