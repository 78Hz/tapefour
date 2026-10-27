#include "Launch.h"

namespace tf::pocket
{
#if JUCE_ANDROID
juce::URL documentOpenedWith()
{
    // activity.getIntent().getData().toString()
    auto* env = juce::getEnv();
    auto activity = juce::getCurrentActivity();
    if (env == nullptr || activity.get() == nullptr) return {};
    auto call = [env] (jobject object, const char* method, const char* signature) -> jobject
    {
        if (object == nullptr) return nullptr;
        juce::LocalRef<jclass> cls (env->GetObjectClass (object));
        auto id = env->GetMethodID (cls.get(), method, signature);
        if (id == nullptr) { env->ExceptionClear(); return nullptr; }
        auto result = env->CallObjectMethod (object, id);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        return result;
    };
    juce::LocalRef<jobject> intent (call (activity.get(), "getIntent", "()Landroid/content/Intent;"));
    juce::LocalRef<jobject> data (call (intent.get(), "getData", "()Landroid/net/Uri;"));
    juce::LocalRef<jobject> text (call (data.get(), "toString", "()Ljava/lang/String;"));
    if (text.get() == nullptr) return {};
    return juce::URL (juce::juceString ((jstring) text.get()));
}
#else
juce::URL documentOpenedWith() { return {}; }
#endif
} // namespace tf::pocket
