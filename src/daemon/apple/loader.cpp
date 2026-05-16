#include "apple/loader.hpp"

#include <cstdio>
#include <dlfcn.h>

namespace wrapper::apple {

namespace {

// Helper: dlopen with friendlier error reporting.
void* open_lib(const std::string& path, std::string* err_out) {
    // RTLD_NOW: resolve every symbol up front so we fail fast at
    //   dlopen() time rather than at first call.
    // RTLD_GLOBAL: make symbols available for subsequent dlopens
    //   (the storeservicescore.so chain expects mediaplatform's
    //   exports to be visible during its own load).
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
        const char* msg = dlerror();
        if (err_out != nullptr) {
            *err_out = "dlopen(" + path + "): " + (msg ? msg : "unknown error");
        }
        std::fprintf(stderr, "loader: %s\n", err_out ? err_out->c_str() : "");
    }
    return h;
}

// Helper: dlsym with friendlier error reporting. Handle may be a real
// dlopen result OR RTLD_DEFAULT (which on bionic/x86_64 is the literal
// value 0 - so a null-check on the handle here would be wrong).
template <typename T>
bool resolve(void* h, const char* name, T* out, std::string* err_out) {
    dlerror();  // clear
    void* sym = dlsym(h, name);
    const char* msg = dlerror();
    if (msg != nullptr || sym == nullptr) {
        if (err_out) {
            *err_out = std::string("dlsym(") + name + "): " + (msg ? msg : "not found");
        }
        std::fprintf(stderr, "loader: %s\n", err_out ? err_out->c_str() : "");
        return false;
    }
    *out = reinterpret_cast<T>(sym);
    return true;
}

}  // namespace

bool Loader::open(const std::string& libs_dir) {
    if (ok_) return true;

    // Load order matters: libc++_shared.so first (Apple's C++ runtime),
    // then mediaplatform, then storeservicescore. The androidappmusic
    // lib is dlopen'd last but is not strictly required to resolve
    // symbols for Phase 1.0 (kept for parity with the upstream link
    // line).
    //
    // We use absolute paths so dlopen doesn't have to consult
    // LD_LIBRARY_PATH or rpath; the Android linker still uses /system/lib64
    // search rules for the DT_NEEDED entries of these libs themselves.
    auto load = [&](const std::string& path, void** dest) {
        *dest = open_lib(path, &last_error_);
        return *dest != nullptr;
    };

    // libc++_shared.so is already loaded as a DT_NEEDED entry of the
    // daemon ELF itself (we link against the same SONAME at build
    // time), so we do not dlopen it here.
    if (!load(libs_dir + "/libmediaplatform.so",     &h_libmediaplatform_))     return false;
    if (!load(libs_dir + "/libstoreservicescore.so", &h_libstoreservicescore_)) return false;
    if (!load(libs_dir + "/libandroidappmusic.so",   &h_libandroidappmusic_))   return false;

    // Apple's symbols are distributed across libstoreservicescore.so,
    // libmediaplatform.so, and libandroidappmusic.so. Rather than
    // hand-mapping each symbol to a handle, we resolve via RTLD_DEFAULT
    // which searches every RTLD_GLOBAL-loaded lib plus the executable.
    // The bionic resolver (_resolv_*) lives in libc.so and is reachable
    // the same way.
    using namespace abi;

    if (!resolve(RTLD_DEFAULT,
                 mangled::resolv_set_nameservers_for_net,
                 &symbols_.resolv_set_nameservers_for_net, &last_error_)) return false;

    // Vtable symbol (data object, not a function pointer).
    {
        dlerror();
        void* v = dlsym(RTLD_DEFAULT, mangled::vtable_RequestContextConfig);
        const char* msg = dlerror();
        if (v == nullptr || msg != nullptr) {
            last_error_ = std::string("dlsym(") + mangled::vtable_RequestContextConfig
                          + "): " + (msg ? msg : "not found");
            std::fprintf(stderr, "loader: %s\n", last_error_.c_str());
            return false;
        }
        symbols_.vtable_RequestContextConfig = reinterpret_cast<void**>(v);
    }

#define RESOLVE(field, name) \
    if (!resolve(RTLD_DEFAULT, mangled::name, &symbols_.field, &last_error_)) return false

    RESOLVE(FootHillConfig_config,          FootHillConfig_config);
    RESOLVE(DeviceGUID_instance,            DeviceGUID_instance);
    RESOLVE(DeviceGUID_configure,           DeviceGUID_configure);
    RESOLVE(make_shared_RequestContext,     make_shared_RequestContext);
    RESOLVE(RequestContextConfig_ctor,      RequestContextConfig_ctor);

    RESOLVE(RCC_setBaseDirectoryPath,  RCC_setBaseDirectoryPath);
    RESOLVE(RCC_setClientIdentifier,   RCC_setClientIdentifier);
    RESOLVE(RCC_setVersionIdentifier,  RCC_setVersionIdentifier);
    RESOLVE(RCC_setPlatformIdentifier, RCC_setPlatformIdentifier);
    RESOLVE(RCC_setProductVersion,     RCC_setProductVersion);
    RESOLVE(RCC_setDeviceModel,        RCC_setDeviceModel);
    RESOLVE(RCC_setBuildVersion,       RCC_setBuildVersion);
    RESOLVE(RCC_setLocaleIdentifier,   RCC_setLocaleIdentifier);
    RESOLVE(RCC_setLanguageIdentifier, RCC_setLanguageIdentifier);

    RESOLVE(RequestContextManager_configure,         RequestContextManager_configure);
    RESOLVE(RequestContext_init,                     RequestContext_init);
    RESOLVE(RequestContext_setFairPlayDirectoryPath, RequestContext_setFairPlayDirectoryPath);

#undef RESOLVE

    ok_ = true;
    last_error_.clear();
    return true;
}

void Loader::close() {
    // We deliberately do NOT dlclose() the Apple libs. Apple's libs
    // set up process-global state (DeviceGUID singleton, FootHill
    // config) that does not cope with being torn down. The handles
    // get released when the process exits.
    ok_ = false;
    symbols_ = Symbols{};
    h_libstoreservicescore_ = nullptr;
    h_libmediaplatform_     = nullptr;
    h_libandroidappmusic_   = nullptr;
}

}  // namespace wrapper::apple
