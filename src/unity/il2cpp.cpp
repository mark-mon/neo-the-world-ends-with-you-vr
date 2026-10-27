#include "il2cpp.h"
#include "../common/log.h"

namespace uxr::il2cpp {

Api api{};
HMODULE gameAssembly = nullptr;

#define RESOLVE(n)                                                               \
    api.n = (decltype(api.n))GetProcAddress(ga, "il2cpp_" #n);                  \
    if (!api.n) { LOGE("il2cpp: export il2cpp_%s missing", #n); ok = false; }

bool LoadApi(HMODULE ga) {
    gameAssembly = ga;
    bool ok = true;
    RESOLVE(domain_get) RESOLVE(domain_get_assemblies) RESOLVE(assembly_get_image) RESOLVE(image_get_name)
    RESOLVE(class_from_name) RESOLVE(class_get_method_from_name) RESOLVE(class_get_type) RESOLVE(type_get_object)
    RESOLVE(runtime_invoke) RESOLVE(thread_attach) RESOLVE(object_unbox) RESOLVE(object_get_class)
    RESOLVE(class_get_name) RESOLVE(class_get_namespace) RESOLVE(string_chars) RESOLVE(string_length)
    RESOLVE(string_new) RESOLVE(array_length) RESOLVE(get_corlib) RESOLVE(resolve_icall) RESOLVE(format_exception)
    RESOLVE(object_new) RESOLVE(gchandle_new) RESOLVE(gchandle_get_target) RESOLVE(gchandle_free)
    return ok;
}

bool WaitForRuntime(DWORD timeoutMs) {
    DWORD t0 = GetTickCount();
    while (!api.get_corlib()) {
        if (GetTickCount() - t0 > timeoutMs) return false;
        Sleep(5);
    }
    return true;
}

void AttachThisThread() { api.thread_attach(api.domain_get()); }

const Image* FindImage(const char* name) {
    size_t n = 0;
    const Assembly** as = api.domain_get_assemblies(api.domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        const Image* img = api.assembly_get_image(as[i]);
        if (img && _stricmp(api.image_get_name(img), name) == 0) return img;
    }
    return nullptr;
}

Class* FindClass(const char* ns, const char* name, const char* imageHint) {
    if (imageHint) {
        if (const Image* img = FindImage(imageHint))
            if (Class* k = api.class_from_name(img, ns, name)) return k;
    }
    size_t n = 0;
    const Assembly** as = api.domain_get_assemblies(api.domain_get(), &n);
    for (size_t i = 0; i < n; i++)
        if (Class* k = api.class_from_name(api.assembly_get_image(as[i]), ns, name)) return k;
    LOGW("il2cpp: class %s.%s not found", ns, name);
    return nullptr;
}

const MethodInfo* FindMethod(Class* k, const char* name, int argc) {
    if (!k) return nullptr;
    const MethodInfo* m = api.class_get_method_from_name(k, name, argc);
    if (!m) LOGW("il2cpp: method %s.%s(%d args) not found", api.class_get_name(k), name, argc);
    return m;
}

Object* Invoke(const MethodInfo* m, void* self, void** args) {
    if (!m) return nullptr;
    Exception* ex = nullptr;
    Object* r = api.runtime_invoke(m, self, args, &ex);
    if (ex) {
        char buf[1024] = {};
        api.format_exception(ex, buf, sizeof(buf));
        LOGW("il2cpp: exception: %s", buf);
        return nullptr;
    }
    return r;
}

std::string ToUtf8(String* s) {
    if (!s) return "<null>";
    int len = api.string_length(s);
    const wchar_t* w = (const wchar_t*)api.string_chars(s);
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), n, nullptr, nullptr);
    return out;
}

Object* TypeObject(Class* k) { return k ? api.type_get_object(api.class_get_type(k)) : nullptr; }

} // namespace uxr::il2cpp
