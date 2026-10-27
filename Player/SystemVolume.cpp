// JUCE's JNI helpers (getEnv, LocalRef, getAppContext...) are only declared when this is set first.
#define JUCE_CORE_INCLUDE_JNI_HELPERS 1

#include "SystemVolume.h"

// iOS: SystemVolume_ios.mm. Android: the media stream's volume, through AudioManager.
// Computers: none (the player keeps its own volume).

#if JUCE_ANDROID
namespace tf::pocket::SystemVolume
{
namespace
{
    constexpr jint streamMusic = 3;   // AudioManager.STREAM_MUSIC

    // The AudioManager, from the app's context (Context.getSystemService ("audio")).
    juce::LocalRef<jobject> audioManager()
    {
        auto* env = juce::getEnv();
        auto context = juce::getAppContext();
        if (env == nullptr || context.get() == nullptr) return juce::LocalRef<jobject> (nullptr);
        juce::LocalRef<jclass> contextClass (env->GetObjectClass (context.get()));
        auto getSystemService = env->GetMethodID (contextClass.get(), "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        return juce::LocalRef<jobject> (env->CallObjectMethod (context.get(), getSystemService, juce::javaString ("audio").get()));
    }

    jint call (const char* method, const char* signature, jint a = 0, jint b = 0, jint c = 0, int args = 0)
    {
        auto* env = juce::getEnv();
        auto am = audioManager();
        if (env == nullptr || am.get() == nullptr) return 0;
        juce::LocalRef<jclass> amClass (env->GetObjectClass (am.get()));
        auto id = env->GetMethodID (amClass.get(), method, signature);
        if (id == nullptr) { env->ExceptionClear(); return 0; }
        if (args == 3) { env->CallVoidMethod (am.get(), id, a, b, c); return 0; }
        return env->CallIntMethod (am.get(), id, a);
    }
}

bool available() { return true; }

void attach()
{
    // The side buttons set the media volume even while nothing plays (Activity.setVolumeControlStream).
    auto* env = juce::getEnv();
    auto activity = juce::getCurrentActivity();
    if (env == nullptr || activity.get() == nullptr) return;
    juce::LocalRef<jclass> activityClass (env->GetObjectClass (activity.get()));
    if (auto id = env->GetMethodID (activityClass.get(), "setVolumeControlStream", "(I)V"))
        env->CallVoidMethod (activity.get(), id, streamMusic);
    else
        env->ExceptionClear();
}

float get()
{
    const auto max = call ("getStreamMaxVolume", "(I)I", streamMusic);
    return max > 0 ? (float) call ("getStreamVolume", "(I)I", streamMusic) / (float) max : 0.0f;
}

void set (float level)
{
    const auto max = call ("getStreamMaxVolume", "(I)I", streamMusic);
    if (max <= 0) return;
    call ("setStreamVolume", "(III)V", streamMusic, (jint) juce::roundToInt (juce::jlimit (0.0f, 1.0f, level) * (float) max), 0, 3);
}

int steps() { return juce::jmax (1, (int) call ("getStreamMaxVolume", "(I)I", streamMusic)); }
} // namespace tf::pocket::SystemVolume

#elif ! JUCE_IOS
namespace tf::pocket::SystemVolume
{
bool available() { return false; }
void attach() {}
float get() { return 0.0f; }
void set (float) {}
int steps() { return 10; }
} // namespace tf::pocket::SystemVolume
#endif
