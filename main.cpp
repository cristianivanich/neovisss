// Punto de entrada NativeActivity: ciclo de vida, permisos (vía JNI) y bucle de render en C++.
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <chrono>
#include <memory>

#include "audio_processor.h"
#include "log.h"
#include "vulkan_engine.h"

namespace {

constexpr const char* kMicPermission = "android.permission.RECORD_AUDIO";

struct AppState {
    android_app* app = nullptr;
    JNIEnv* env = nullptr;  // hilo nativo adjuntado a la JVM
    VulkanEngine engine;
    std::shared_ptr<AudioProcessor> audio = std::make_shared<AudioProcessor>();

    bool resumed = false;
    bool micRequested = false;
    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastAudioCheck{};

    bool shouldRender() const { return resumed && engine.isReady(); }
};

bool hasMicPermission(AppState& s) {
    JNIEnv* env = s.env;
    jobject activity = s.app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    jstring perm = env->NewStringUTF(kMicPermission);
    jint res = env->CallIntMethod(activity, mid, perm);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        res = -1;
    }
    env->DeleteLocalRef(perm);
    env->DeleteLocalRef(cls);
    return res == 0;  // PackageManager.PERMISSION_GRANTED
}

void requestMicPermission(AppState& s) {
    JNIEnv* env = s.env;
    jobject activity = s.app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jclass strCls = env->FindClass("java/lang/String");
    jstring perm = env->NewStringUTF(kMicPermission);
    jobjectArray arr = env->NewObjectArray(1, strCls, nullptr);
    env->SetObjectArrayElement(arr, 0, perm);
    jmethodID mid = env->GetMethodID(cls, "requestPermissions", "([Ljava/lang/String;I)V");
    env->CallVoidMethod(activity, mid, arr, 1);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(perm);
    env->DeleteLocalRef(strCls);
    env->DeleteLocalRef(cls);
    LOGI("Solicitado permiso RECORD_AUDIO");
}

// Se llama ~2 veces por segundo: pide permiso, arranca o reinicia la captura de Oboe.
void maintainAudio(AppState& s) {
    const auto now = std::chrono::steady_clock::now();
    if (now - s.lastAudioCheck < std::chrono::milliseconds(500)) return;
    s.lastAudioCheck = now;

    if (s.audio->needsRestart()) {
        s.audio->stop();
    }
    if (s.audio->isRunning()) return;

    if (hasMicPermission(s)) {
        s.audio->start();
    } else if (!s.micRequested) {
        s.micRequested = true;
        requestMicPermission(s);
    }
}

void onCmd(android_app* app, int32_t cmd) {
    auto* s = static_cast<AppState*>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window != nullptr) {
                s->engine.init(app->window, app->activity->assetManager);
                s->lastFrame = std::chrono::steady_clock::now();
            }
            break;
        case APP_CMD_TERM_WINDOW:
            s->engine.shutdown();
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            s->engine.requestResize();
            break;
        case APP_CMD_RESUME:
            s->resumed = true;
            s->lastAudioCheck = {};  // fuerza comprobación inmediata (tras el diálogo de permisos)
            s->lastFrame = std::chrono::steady_clock::now();
            break;
        case APP_CMD_PAUSE:
            s->resumed = false;
            s->audio->stop();  // libera el micrófono en segundo plano
            break;
        default:
            break;
    }
}

}  // namespace

void android_main(android_app* app) {
    LOGI("android_main: inicio");
    AppState state;
    state.app = app;
    app->userData = &state;
    app->onAppCmd = onCmd;

    ANativeActivity_setWindowFlags(app->activity,
                                   AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);

    if (app->activity->vm->AttachCurrentThread(&state.env, nullptr) != JNI_OK) {
        LOGE("No se pudo adjuntar el hilo a la JVM");
        return;
    }

    while (!app->destroyRequested) {
        // Parado -> bloquea hasta el siguiente evento; activo -> no bloquea y renderiza (vsync por FIFO)
        const int timeout = state.shouldRender() ? 0 : -1;
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) source->process(app, source);
            if (app->destroyRequested) break;
        }
        if (app->destroyRequested) break;

        if (state.shouldRender()) {
            maintainAudio(state);

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - state.lastFrame).count();
            state.lastFrame = now;

            if (!state.engine.render(state.audio->features(), dt)) {
                LOGE("Error de render: reiniciando el motor");
                state.engine.shutdown();
                if (app->window) state.engine.init(app->window, app->activity->assetManager);
            }
        }
    }

    state.audio->stop();
    state.engine.shutdown();
    app->activity->vm->DetachCurrentThread();
    LOGI("android_main: fin");
}
