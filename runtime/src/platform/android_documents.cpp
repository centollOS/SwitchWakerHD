#include "android_documents.h"
#include "../exception_report.h"
#include <SDL3/SDL.h>
#include <jni.h>

namespace host::android {
std::string copy_picked_document(const std::string& uri, uint64_t limit) {
    const char* unavailable = "This file can't be opened here";
    auto env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env || env->PushLocalFrame(8) != 0) { if (env) env->ExceptionClear(); exception_report::raise(unavailable); }
    std::string result;
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    auto cls = activity ? env->GetObjectClass(activity) : nullptr;
    auto method = cls ? env->GetStaticMethodID(cls, "copyPickedDocument", "(Ljava/lang/String;J)Ljava/lang/String;") : nullptr;
    auto text = env->ExceptionCheck() ? nullptr : env->NewStringUTF(uri.c_str());
    if (method && text && !env->ExceptionCheck()) {
        auto answer = static_cast<jstring>(env->CallStaticObjectMethod(cls, method, text, jlong(limit)));
        if (answer && !env->ExceptionCheck())
            if (auto chars = env->GetStringUTFChars(answer, nullptr)) { result = chars; env->ReleaseStringUTFChars(answer, chars); }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
    // "ok:" + the copy's path, or "error:" + a message for the player
    if (result.rfind("ok:", 0) == 0 && result.size() > 3) return result.substr(3);
    exception_report::raise(result.rfind("error:", 0) == 0 ? result.substr(6) : std::string(unavailable));
}
}
