// Minimal IL2CPP runtime access through GameAssembly.dll exports. Engine-level: nothing here is specific to a game.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace uxr::il2cpp {

struct Domain; struct Assembly; struct Image; struct Class; struct Type; struct MethodInfo;
struct Object; struct String; struct Array; struct Exception; struct Thread;

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };
struct Rect { float x, y, w, h; };
struct Color { float r, g, b, a; };
struct Matrix4x4 { float m[16]; float& at(int r, int c) { return m[c * 4 + r]; } }; // Unity: column-major

struct Api {
    Domain* (*domain_get)();
    const Assembly** (*domain_get_assemblies)(const Domain*, size_t*);
    const Image* (*assembly_get_image)(const Assembly*);
    const char* (*image_get_name)(const Image*);
    Class* (*class_from_name)(const Image*, const char*, const char*);
    const MethodInfo* (*class_get_method_from_name)(Class*, const char*, int);
    const Type* (*class_get_type)(Class*);
    Object* (*type_get_object)(const Type*);
    Object* (*runtime_invoke)(const MethodInfo*, void*, void**, Exception**);
    Thread* (*thread_attach)(Domain*);
    void* (*object_unbox)(Object*);
    Class* (*object_get_class)(Object*);
    const char* (*class_get_name)(Class*);
    const char* (*class_get_namespace)(Class*);
    const uint16_t* (*string_chars)(String*);
    int32_t (*string_length)(String*);
    String* (*string_new)(const char*);
    uint32_t (*array_length)(Array*);
    const Image* (*get_corlib)();
    void* (*resolve_icall)(const char*);
    void (*format_exception)(const Exception*, char*, int);
    Object* (*object_new)(Class*);
    uint32_t (*gchandle_new)(Object*, bool pinned);
    Object* (*gchandle_get_target)(uint32_t);
    void (*gchandle_free)(uint32_t);
};

extern Api api;
extern HMODULE gameAssembly;

bool LoadApi(HMODULE ga);                 // resolve exports
bool WaitForRuntime(DWORD timeoutMs);     // waits until il2cpp_init has run (corlib available)
void AttachThisThread();

const Image* FindImage(const char* name); // e.g. "UnityEngine.CoreModule.dll"
Class* FindClass(const char* ns, const char* name, const char* imageHint = nullptr); // searches all images if no hint
const MethodInfo* FindMethod(Class* k, const char* name, int argc);
inline void* MethodPointer(const MethodInfo* m) { return m ? *(void* const*)m : nullptr; } // MethodInfo::methodPointer is first

// Invoke wrapper: logs and returns nullptr on managed exception.
Object* Invoke(const MethodInfo* m, void* self, void** args = nullptr);
template <class T> bool InvokeValue(const MethodInfo* m, void* self, T& out, void** args = nullptr) {
    Object* r = Invoke(m, self, args);
    if (!r) return false;
    out = *(T*)api.object_unbox(r);
    return true;
}

std::string ToUtf8(String* s);

// Strong GC handle so a Unity object we keep between frames is never collected under us.
struct Handle {
    uint32_t h = 0;
    Object* get() const { return h ? api.gchandle_get_target(h) : nullptr; }
    void set(Object* o) { reset(); if (o) h = api.gchandle_new(o, false); }
    void reset() { if (h) api.gchandle_free(h); h = 0; }
};
Object* TypeObject(Class* k); // System.Type for a class
inline Object** ArrayData(Array* a) { return (Object**)((char*)a + 32); } // x64 Il2CppArray header

} // namespace uxr::il2cpp
