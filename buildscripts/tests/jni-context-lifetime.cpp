#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <mpv/client.h>

#define UTIL_EXTERN
#include "app/src/main/jni/jni_utils.h"
#include "app/src/main/jni/globals.h"

JavaVM *g_vm;
std::atomic<mpv_handle *> g_mpv(nullptr);
std::atomic<bool> g_event_thread_started(false);
std::atomic<bool> g_shutdown_requested(false);
std::atomic<bool> g_force_shutdown(false);
std::mutex g_mpv_mutex;

static int context_storage;
static mpv_handle *const context = reinterpret_cast<mpv_handle *>(&context_storage);
static std::mutex test_mutex;
static std::condition_variable test_condition;
static bool shutdown_started, destroyed, race_shutdown;
static bool callback_unlocked;
static bool callback_ready;
static JNIEnv *event_env;
static JavaVM *attached_vm, *detached_vm, *handoff_vm;
static int invalid_calls;
static std::thread shutdown_thread;

#include "app/src/main/jni/property.cpp"
#include "app/src/main/jni/event.cpp"

extern "C" int __android_log_print(int, const char *, const char *, ...) { return 0; }
void throw_java_exception(JNIEnv *, const char *) {}
bool acquire_jni_env(JavaVM *vm, JNIEnv **env) {
    attached_vm = vm;
    *env = event_env;
    return *env != nullptr;
}
jstring utf8_to_jstring(JNIEnv *, const char *) { return nullptr; }
void handle_request_reply(JNIEnv *, mpv_event *) {}
void release_requests(JNIEnv *) {}

bool check_mpv_initialized() {
    return g_mpv && !g_shutdown_requested;
}

bool jstring_to_utf8(JNIEnv *env, jstring, std::string *value) {
    *value = "pause";
    if (race_shutdown) {
        shutdown_thread = std::thread([env] {
            g_shutdown_requested = true;
            {
                std::lock_guard<std::mutex> lock(test_mutex);
                shutdown_started = true;
            }
            test_condition.notify_all();
            finishShutdown(env, false);
        });
        std::unique_lock<std::mutex> lock(test_mutex);
        test_condition.wait(lock, [] { return shutdown_started; });
        // A correct owner keeps shutdown blocked until this property call ends.
        test_condition.wait_for(lock, std::chrono::milliseconds(100), [] { return destroyed; });
    }
    return true;
}

extern "C" int mpv_get_property(mpv_handle *handle, const char *, mpv_format, void *data) {
    std::lock_guard<std::mutex> lock(test_mutex);
    if (handle != context || destroyed) {
        invalid_calls++;
        return MPV_ERROR_UNINITIALIZED;
    }
    *static_cast<int64_t *>(data) = 1;
    return MPV_ERROR_SUCCESS;
}

extern "C" void mpv_destroy(mpv_handle *) {
    {
        std::lock_guard<std::mutex> lock(test_mutex);
        destroyed = true;
    }
    test_condition.notify_all();
}

extern "C" void mpv_terminate_destroy(mpv_handle *handle) { mpv_destroy(handle); }
extern "C" int mpv_set_property(mpv_handle *, const char *, mpv_format, void *) { return 0; }
extern "C" int mpv_set_option_string(mpv_handle *, const char *, const char *) { return 0; }
extern "C" int mpv_observe_property(mpv_handle *, uint64_t, const char *, mpv_format) { return 0; }
extern "C" const char *mpv_error_string(int) { return "test error"; }
extern "C" const char *mpv_event_name(mpv_event_id) { return "test event"; }
extern "C" mpv_event *mpv_wait_event(mpv_handle *, double) {
    static mpv_event shutdown_event = {MPV_EVENT_SHUTDOWN};
    return &shutdown_event;
}
extern "C" void mpv_free(void *) {}
extern "C" void mpv_free_node_contents(mpv_node *) {}

static jboolean JNICALL exception_check(JNIEnv *) { return JNI_FALSE; }
static void JNICALL shutdown_callback(JNIEnv *, jclass, jmethodID, va_list) {
    callback_unlocked = g_mpv_mutex.try_lock();
    if (callback_unlocked) {
        callback_ready = !g_mpv && !g_event_thread_started && !g_shutdown_requested;
        if (handoff_vm)
            g_vm = handoff_vm;
        g_mpv_mutex.unlock();
    }
}

static jint JNICALL detach_thread(JavaVM *vm) {
    detached_vm = vm;
    return JNI_OK;
}

int main() {
    JNINativeInterface_ functions = {};
    functions.ExceptionCheck = exception_check;
    functions.CallStaticVoidMethodV = shutdown_callback;
    JNIEnv env = { &functions };
    jstring property = reinterpret_cast<jstring>(&context_storage);
    int64_t value = 0;

    g_mpv = context;
    g_event_thread_started = true;
    if (common_get_property(&env, property, MPV_FORMAT_INT64, &value) < 0 || value != 1)
        return 1;

    race_shutdown = true;
    int result = common_get_property(&env, property, MPV_FORMAT_INT64, &value);
    shutdown_thread.join();
    if (result < 0 || invalid_calls || !destroyed || g_mpv || !callback_unlocked || !callback_ready) {
        std::fprintf(stderr, "JNI shutdown raced property call or session handoff: result=%d, invalid=%d, callback_unlocked=%d, callback_ready=%d\n",
                     result, invalid_calls, callback_unlocked, callback_ready);
        return 1;
    }
    race_shutdown = false;
    if (common_get_property(&env, property, MPV_FORMAT_INT64, &value) != MPV_ERROR_UNINITIALIZED)
        return 1;

    // A new session may publish its VM before the old event thread detaches.
    JNIInvokeInterface_ vm_functions = {};
    vm_functions.DetachCurrentThread = detach_thread;
    JavaVM old_vm = {&vm_functions}, next_vm = {&vm_functions};
    event_env = &env;
    g_vm = &old_vm;
    handoff_vm = &next_vm;
    g_mpv = context;
    g_event_thread_started = true;
    event_thread(nullptr);
    if (!callback_ready || attached_vm != &old_vm || detached_vm != &old_vm || g_vm != &next_vm)
        return 1;
    std::puts("JNI context lifetime: property completion, shutdown and ready session handoff passed");
    return 0;
}
