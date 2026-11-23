// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's Android backend: GameActivity's window, an EGL OpenGL ES 3 context, GameActivity's
// input buffers and the soft keyboard. The frame loop is EditorFrame, shared with the Linux and
// Windows backends, and the ImGui renderer is RendererGL: it asks this file for a context and
// nothing else, so no GL loader is involved (an ES 3 app links libGLESv3.so directly).
//
// This file never instantiates NativeEngine. NativeEngine is the game runtime's host (swappy frame
// pacing, its own surface, its own input routing); the editor hosts the engine through
// Engine::systemInit with the editor's own System, as the desktop backends do. Two hosts over one
// window would fight for it.

#include "Backend.h"
#include "EditorFrame.h"
#include "EditorHost.h"
#include "diag/HangWatchdog.h"
#include "renderer/Renderer.h"
#include "Theme.h"

#include "AndroidTextInput.h"
#include "AndroidTouchGestures.h"
#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "TextureRender.h"

#include "imgui.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <game-activity/GameActivity.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::editor {

    namespace {

        // ImGui's style is authored for a ~96 dpi desktop at 1:1. A phone is ~420 dpi: at 1:1 a
        // menu row is about 20 px, well under a fingertip. Scaling the style (not the atlas)
        // puts a frame near Android's 48 dp touch target.
        constexpr float ANDROID_UI_SCALE = 2.6f;

        android_app* gAndroidApp = nullptr;

        EGLDisplay gDisplay = EGL_NO_DISPLAY;
        EGLSurface gSurface = EGL_NO_SURFACE;
        EGLContext gContext = EGL_NO_CONTEXT;
        EGLConfig gConfig = nullptr;

        int gWidth = 0;
        int gHeight = 0;
        bool gShouldClose = false;
        bool gContextUp = false;
        bool gRedrawRequested = false;

        double gLastFrameTime = 0.0;

        // One ImGui pointer: the first finger of a gesture owns it until it lifts, so a second
        // finger can't drag a slider the first one grabbed.
        int32_t gActivePointerId = -1;
        bool gPointerDown = false;
        // the IME's own state, so show/hide is called on a change and not every frame
        bool gSoftKeyboardShown = false;

        // The soft keyboard's text arrives on GameActivity's thread; ImGui's input queue is not
        // thread safe, so it is queued here and drained once per frame.
        std::mutex gTextMutex;
        std::vector<unsigned int> gPendingCharacters;


        // EditorActivity's clipboard methods, looked up once: the loop calls them on every
        // copy/paste, and GetMethodID is a class lookup, not a cheap call.
        jmethodID gSetClipboardMethod = nullptr;
        jmethodID gGetClipboardMethod = nullptr;

        jmethodID clipboardMethod(JNIEnv* env, const char* name, const char* signature){
            jmethodID* slot = std::strcmp(name, "setEditorClipboardText") == 0
                ? &gSetClipboardMethod : &gGetClipboardMethod;
            if (*slot)
                return *slot;
            if (!gAndroidApp || !gAndroidApp->activity || !gAndroidApp->activity->clazz)
                return nullptr;
            jclass activityClass = env->GetObjectClass(gAndroidApp->activity->clazz);
            if (!activityClass)
                return nullptr;
            *slot = env->GetMethodID(activityClass, name, signature);
            env->DeleteLocalRef(activityClass);
            if (env->ExceptionCheck()){
                // an activity without the method: no clipboard, and no exception left pending
                env->ExceptionClear();
                return nullptr;
            }
            return *slot;
        }

        double monotonicSeconds(){
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

    } // namespace

    // -------------------------------------------------------------------------------------------
    // EGL

    namespace {

        bool createEglContext(android_app* app){
            if (!app || !app->window){
                Log::error("[editor] no native window to draw into");
                return false;
            }

            gDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (gDisplay == EGL_NO_DISPLAY || eglInitialize(gDisplay, nullptr, nullptr) != EGL_TRUE){
                Log::error("[editor] eglInitialize failed: %d", eglGetError());
                return false;
            }

            // An alpha channel and a depth buffer: ImGui draws translucent panels over the
            // window, and the Scene view renders 3D through the same context.
            const EGLint configAttribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 24,
                EGL_STENCIL_SIZE, 8,
                EGL_NONE
            };
            EGLint numConfigs = 0;
            if (eglChooseConfig(gDisplay, configAttribs, &gConfig, 1, &numConfigs) != EGL_TRUE ||
                numConfigs == 0){
                Log::error("[editor] no ES3 EGL config: %d", eglGetError());
                return false;
            }

            gSurface = eglCreateWindowSurface(gDisplay, gConfig, app->window, nullptr);
            if (gSurface == EGL_NO_SURFACE){
                Log::error("[editor] eglCreateWindowSurface failed: %d", eglGetError());
                return false;
            }

            const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
            gContext = eglCreateContext(gDisplay, gConfig, EGL_NO_CONTEXT, contextAttribs);
            if (gContext == EGL_NO_CONTEXT){
                Log::error("[editor] eglCreateContext failed: %d", eglGetError());
                return false;
            }
            if (eglMakeCurrent(gDisplay, gSurface, gSurface, gContext) != EGL_TRUE){
                Log::error("[editor] eglMakeCurrent failed: %d", eglGetError());
                return false;
            }

            eglQuerySurface(gDisplay, gSurface, EGL_WIDTH, &gWidth);
            eglQuerySurface(gDisplay, gSurface, EGL_HEIGHT, &gHeight);

            Log::print("[editor] ES3 context up, %dx%d, %s / %s", gWidth, gHeight,
                      reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                      reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
            gContextUp = true;
            return true;
        }

        void makeCurrent(ImGuiViewport*){
            if (gContextUp && gSurface != EGL_NO_SURFACE)
                eglMakeCurrent(gDisplay, gSurface, gSurface, gContext);
        }

        // ImGui draws into the window's default framebuffer; the engine's Scene view draws into
        // its own framebuffer, which ImGui then samples. So presenting is one eglSwapBuffers.
        void swapBuffers(ImGuiViewport*){
            if (gContextUp && gSurface != EGL_NO_SURFACE)
                eglSwapBuffers(gDisplay, gSurface);
        }

        void setSwapInterval(int interval){
            if (gContextUp)
                eglSwapInterval(gDisplay, interval);
        }

        RendererPlatform::GLProc getProcAddress(const char*){
            return nullptr;
        }

        bool createContext(){
            return gContextUp || createEglContext(gAndroidApp);
        }

        void destroyContext(){
            if (gDisplay == EGL_NO_DISPLAY)
                return;
            eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (gContext != EGL_NO_CONTEXT){
                eglDestroyContext(gDisplay, gContext);
                gContext = EGL_NO_CONTEXT;
            }
            if (gSurface != EGL_NO_SURFACE){
                eglDestroySurface(gDisplay, gSurface);
                gSurface = EGL_NO_SURFACE;
            }
            eglTerminate(gDisplay);
            gDisplay = EGL_NO_DISPLAY;
            gContextUp = false;
        }

    } // namespace

    // -------------------------------------------------------------------------------------------
    // ImGui platform

    namespace {

        void platformNewFrame(){
            ImGuiIO& io = ImGui::GetIO();

            if (gSurface != EGL_NO_SURFACE){
                int w = 0;
                int h = 0;
                eglQuerySurface(gDisplay, gSurface, EGL_WIDTH, &w);
                eglQuerySurface(gDisplay, gSurface, EGL_HEIGHT, &h);
                if (w > 0 && h > 0){
                    gWidth = w;
                    gHeight = h;
                }
            }
            io.DisplaySize = ImVec2(static_cast<float>(gWidth), static_cast<float>(gHeight));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

            const double now = monotonicSeconds();
            io.DeltaTime = gLastFrameTime > 0.0
                ? static_cast<float>(now - gLastFrameTime) : 1.0f / 60.0f;
            gLastFrameTime = now;

            {
                std::lock_guard<std::mutex> lock(gTextMutex);
                for (unsigned int c : gPendingCharacters)
                    io.AddInputCharacter(c);
                gPendingCharacters.clear();
            }

            // The soft keyboard follows ImGui's focus: shown while a text field wants input,
            // hidden once it doesn't (a dialog closed, another widget focused). Cached because a
            // GameActivity call per frame would fight the IME's own animation.
            const bool wantText = io.WantTextInput;
            if (wantText != gSoftKeyboardShown){
                gSoftKeyboardShown = wantText;
                Backend::showSoftKeyboard(wantText);
            }
        }

        bool initImGuiPlatform(){
            ImGuiIO& io = ImGui::GetIO();

            // the clipboard: ImGui's text fields (and every name field) need these two
            ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
            platformIo.Platform_SetClipboardTextFn = &setClipboardText;
            platformIo.Platform_GetClipboardTextFn = &getClipboardText;
            platformIo.Platform_ClipboardUserData = nullptr;

            io.BackendPlatformUserData = gAndroidApp;
            io.BackendPlatformName = "imgui_impl_jasperengine_android";
            // No OS mouse cursors (a finger has no shape), no OS-set pointer position, and no
            // multi-viewport: Android has one window, and ImGui's viewports would ask this
            // backend for OS windows it cannot make.
            io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

            ImGuiViewport* viewport = ImGui::GetMainViewport();
            viewport->PlatformUserData = gAndroidApp;
            viewport->PlatformHandle = gAndroidApp ? gAndroidApp->window : nullptr;
            viewport->PlatformHandleRaw = viewport->PlatformHandle;
            return true;
        }

        void shutdownImGuiPlatform(){
            ImGui::DestroyPlatformWindows();
        }

    } // namespace

    // -------------------------------------------------------------------------------------------
    // Input

    void Backend::showSoftKeyboard(bool show){
        if (!gAndroidApp || !gAndroidApp->activity)
            return;
        if (show){
            // https://developer.android.com/reference/android/text/InputType: the engine's own
            // choice (NativeEngine.cpp: TYPE_CLASS_TEXT = 1 with IME_FLAG_NO_FULLSCREEN), which is
            // the value its tested soft keyboard is known to work with. Named here rather than
            // copied as an opaque constant so a reader can check them against the reference.
            constexpr int TYPE_CLASS_TEXT = 0x00000001;
            constexpr int IME_ACTION_NONE = 0x00000001;          // EditorInfo.IME_ACTION_NONE
            constexpr int IME_FLAG_NO_FULLSCREEN = 0x02000000;   // EditorInfo.IME_FLAG_NO_FULLSCREEN
            GameActivity_setImeEditorInfo(gAndroidApp->activity, TYPE_CLASS_TEXT,
                                          IME_ACTION_NONE, IME_FLAG_NO_FULLSCREEN);
            GameActivity_showSoftInput(gAndroidApp->activity, 0);
        }else{
            GameActivity_hideSoftInput(gAndroidApp->activity, 0);
        }
    }

    void Backend::queueText(const std::u16string& utf16){
        std::lock_guard<std::mutex> lock(gTextMutex);
        for (size_t i = 0; i < utf16.size(); ++i){
            char32_t codepoint = utf16[i];
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF && i + 1 < utf16.size()){
                const char32_t low = utf16[i + 1];
                if (low >= 0xDC00 && low <= 0xDFFF){
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                }
            }
            gPendingCharacters.push_back(codepoint);
        }
    }

    namespace {

        // one table of keys, used by both the event filter below and the key handler
        ImGuiKey imGuiKeyFromAndroid(int32_t keycode){
            switch (keycode){
                case AKEYCODE_DEL: return ImGuiKey_Backspace;
                case AKEYCODE_FORWARD_DEL: return ImGuiKey_Delete;
                case AKEYCODE_ENTER: return ImGuiKey_Enter;
                case AKEYCODE_ESCAPE: return ImGuiKey_Escape;
                case AKEYCODE_TAB: return ImGuiKey_Tab;
                case AKEYCODE_SPACE: return ImGuiKey_Space;
                case AKEYCODE_DPAD_LEFT: return ImGuiKey_LeftArrow;
                case AKEYCODE_DPAD_RIGHT: return ImGuiKey_RightArrow;
                case AKEYCODE_DPAD_UP: return ImGuiKey_UpArrow;
                case AKEYCODE_DPAD_DOWN: return ImGuiKey_DownArrow;
                case AKEYCODE_MOVE_HOME: return ImGuiKey_Home;
                case AKEYCODE_MOVE_END: return ImGuiKey_End;
                case AKEYCODE_PAGE_UP: return ImGuiKey_PageUp;
                case AKEYCODE_PAGE_DOWN: return ImGuiKey_PageDown;
                // a hardware keyboard's letters, digits and punctuation: sent as keys too, so
                // shortcuts (Ctrl+S and friends) work, and as characters by addImGuiCharacterFor
                case AKEYCODE_A: return ImGuiKey_A;
                case AKEYCODE_B: return ImGuiKey_B;
                case AKEYCODE_C: return ImGuiKey_C;
                case AKEYCODE_D: return ImGuiKey_D;
                case AKEYCODE_E: return ImGuiKey_E;
                case AKEYCODE_F: return ImGuiKey_F;
                case AKEYCODE_G: return ImGuiKey_G;
                case AKEYCODE_H: return ImGuiKey_H;
                case AKEYCODE_I: return ImGuiKey_I;
                case AKEYCODE_J: return ImGuiKey_J;
                case AKEYCODE_K: return ImGuiKey_K;
                case AKEYCODE_L: return ImGuiKey_L;
                case AKEYCODE_M: return ImGuiKey_M;
                case AKEYCODE_N: return ImGuiKey_N;
                case AKEYCODE_O: return ImGuiKey_O;
                case AKEYCODE_P: return ImGuiKey_P;
                case AKEYCODE_Q: return ImGuiKey_Q;
                case AKEYCODE_R: return ImGuiKey_R;
                case AKEYCODE_S: return ImGuiKey_S;
                case AKEYCODE_T: return ImGuiKey_T;
                case AKEYCODE_U: return ImGuiKey_U;
                case AKEYCODE_V: return ImGuiKey_V;
                case AKEYCODE_W: return ImGuiKey_W;
                case AKEYCODE_X: return ImGuiKey_X;
                case AKEYCODE_Y: return ImGuiKey_Y;
                case AKEYCODE_Z: return ImGuiKey_Z;
                case AKEYCODE_0: return ImGuiKey_0;
                case AKEYCODE_1: return ImGuiKey_1;
                case AKEYCODE_2: return ImGuiKey_2;
                case AKEYCODE_3: return ImGuiKey_3;
                case AKEYCODE_4: return ImGuiKey_4;
                case AKEYCODE_5: return ImGuiKey_5;
                case AKEYCODE_6: return ImGuiKey_6;
                case AKEYCODE_7: return ImGuiKey_7;
                case AKEYCODE_8: return ImGuiKey_8;
                case AKEYCODE_9: return ImGuiKey_9;
                case AKEYCODE_COMMA: return ImGuiKey_Comma;
                case AKEYCODE_PERIOD: return ImGuiKey_Period;
                case AKEYCODE_MINUS: return ImGuiKey_Minus;
                case AKEYCODE_EQUALS: return ImGuiKey_Equal;
                case AKEYCODE_LEFT_BRACKET: return ImGuiKey_LeftBracket;
                case AKEYCODE_RIGHT_BRACKET: return ImGuiKey_RightBracket;
                case AKEYCODE_BACKSLASH: return ImGuiKey_Backslash;
                case AKEYCODE_SEMICOLON: return ImGuiKey_Semicolon;
                case AKEYCODE_APOSTROPHE: return ImGuiKey_Apostrophe;
                case AKEYCODE_SLASH: return ImGuiKey_Slash;
                case AKEYCODE_GRAVE: return ImGuiKey_GraveAccent;
                case AKEYCODE_NUMPAD_0: return ImGuiKey_Keypad0;
                case AKEYCODE_NUMPAD_1: return ImGuiKey_Keypad1;
                case AKEYCODE_NUMPAD_2: return ImGuiKey_Keypad2;
                case AKEYCODE_NUMPAD_3: return ImGuiKey_Keypad3;
                case AKEYCODE_NUMPAD_4: return ImGuiKey_Keypad4;
                case AKEYCODE_NUMPAD_5: return ImGuiKey_Keypad5;
                case AKEYCODE_NUMPAD_6: return ImGuiKey_Keypad6;
                case AKEYCODE_NUMPAD_7: return ImGuiKey_Keypad7;
                case AKEYCODE_NUMPAD_8: return ImGuiKey_Keypad8;
                case AKEYCODE_NUMPAD_9: return ImGuiKey_Keypad9;
                case AKEYCODE_NUMPAD_DOT: return ImGuiKey_KeypadDecimal;
                case AKEYCODE_NUMPAD_COMMA: return ImGuiKey_KeypadDecimal;
                case AKEYCODE_NUMPAD_DIVIDE: return ImGuiKey_KeypadDivide;
                case AKEYCODE_NUMPAD_MULTIPLY: return ImGuiKey_KeypadMultiply;
                case AKEYCODE_NUMPAD_SUBTRACT: return ImGuiKey_KeypadSubtract;
                case AKEYCODE_NUMPAD_ADD: return ImGuiKey_KeypadAdd;
                case AKEYCODE_NUMPAD_EQUALS: return ImGuiKey_KeypadEqual;
                // modifiers: ImGui tracks them so Ctrl/Cmd shortcuts work
                case AKEYCODE_SHIFT_LEFT: return ImGuiKey_LeftShift;
                case AKEYCODE_SHIFT_RIGHT: return ImGuiKey_RightShift;
                case AKEYCODE_CTRL_LEFT: return ImGuiKey_LeftCtrl;
                case AKEYCODE_CTRL_RIGHT: return ImGuiKey_RightCtrl;
                case AKEYCODE_ALT_LEFT: return ImGuiKey_LeftAlt;
                case AKEYCODE_ALT_RIGHT: return ImGuiKey_RightAlt;
                case AKEYCODE_META_LEFT: return ImGuiKey_LeftSuper;
                case AKEYCODE_META_RIGHT: return ImGuiKey_RightSuper;
                case AKEYCODE_INSERT: return ImGuiKey_Insert;
                default: return ImGuiKey_None;
            }
        }

        // The character a hardware key stands for. A physical keyboard reports letters, digits and
        // punctuation as KEYS, and ImGui's text fields take their characters from
        // AddInputCharacter: without this, typing "abc" on a Bluetooth keyboard into a name field
        // did nothing at all, and Shift and Caps Lock were not consulted either.
        //
        // With no deltas of its own, Android's key event carries no unicodeChar, so the character
        // is derived here: Shift maps a Latin letter to upper case and a digit/punctuation key to
        // its unshifted US-layout symbol. Caps Lock applies to letters only, as on a desktop.
        void addImGuiCharacterFor(const GameActivityKeyEvent* event){
            ImGuiIO& io = ImGui::GetIO();
            const int32_t keycode = event->keyCode;
            const bool shift = (event->metaState & AMETA_SHIFT_ON) != 0 ||
                               (event->metaState & AMETA_SHIFT_LEFT_ON) != 0;
            const bool caps = (event->metaState & AMETA_CAPS_LOCK_ON) != 0;

            switch (keycode){
                case AKEYCODE_DEL: io.AddInputCharacter('\b'); return;
                case AKEYCODE_ENTER: io.AddInputCharacter('\r'); return;
                case AKEYCODE_TAB: io.AddInputCharacter('\t'); return;
                case AKEYCODE_SPACE: io.AddInputCharacter(' '); return;
                default: break;
            }

            if (keycode >= AKEYCODE_A && keycode <= AKEYCODE_Z){
                const char letter = static_cast<char>('a' + (keycode - AKEYCODE_A));
                const bool upper = shift != caps;   // XOR, as a desktop's Shift+Caps
                io.AddInputCharacter(static_cast<unsigned int>(upper ? std::toupper(letter) : letter));
                return;
            }
            if (keycode >= AKEYCODE_0 && keycode <= AKEYCODE_9){
                static const char* const shifted = ")!@#$%^&*(";
                const int digit = keycode - AKEYCODE_0;
                io.AddInputCharacter(static_cast<unsigned int>(shift ? shifted[digit] : ('0' + digit)));
                return;
            }

            // punctuation: the unshifted and shifted US-layout characters of each key
            struct Pair { int32_t key; char plain; char withShift; };
            static const Pair punctuation[] = {
                {AKEYCODE_COMMA, ',', '<'}, {AKEYCODE_PERIOD, '.', '>'},
                {AKEYCODE_MINUS, '-', '_'}, {AKEYCODE_EQUALS, '=', '+'},
                {AKEYCODE_LEFT_BRACKET, '[', '{'}, {AKEYCODE_RIGHT_BRACKET, ']', '}'},
                {AKEYCODE_BACKSLASH, '\\', '|'}, {AKEYCODE_SEMICOLON, ';', ':'},
                {AKEYCODE_APOSTROPHE, '\'', '"'}, {AKEYCODE_SLASH, '/', '?'},
                {AKEYCODE_GRAVE, '`', '~'}, {AKEYCODE_STAR, '*', '*'},
                {AKEYCODE_POUND, '#', '#'},
                {AKEYCODE_NUMPAD_0, '0', '0'}, {AKEYCODE_NUMPAD_1, '1', '1'},
                {AKEYCODE_NUMPAD_2, '2', '2'}, {AKEYCODE_NUMPAD_3, '3', '3'},
                {AKEYCODE_NUMPAD_4, '4', '4'}, {AKEYCODE_NUMPAD_5, '5', '5'},
                {AKEYCODE_NUMPAD_6, '6', '6'}, {AKEYCODE_NUMPAD_7, '7', '7'},
                {AKEYCODE_NUMPAD_8, '8', '8'}, {AKEYCODE_NUMPAD_9, '9', '9'},
                {AKEYCODE_NUMPAD_DOT, '.', '.'}, {AKEYCODE_NUMPAD_COMMA, ',', ','},
                {AKEYCODE_NUMPAD_DIVIDE, '/', '/'}, {AKEYCODE_NUMPAD_MULTIPLY, '*', '*'},
                {AKEYCODE_NUMPAD_SUBTRACT, '-', '-'}, {AKEYCODE_NUMPAD_ADD, '+', '+'},
                {AKEYCODE_NUMPAD_EQUALS, '=', '='},
            };
            for (const Pair& p : punctuation){
                if (p.key != keycode)
                    continue;
                io.AddInputCharacter(static_cast<unsigned int>(shift ? p.withShift : p.plain));
                return;
            }
        }

        // -- the clipboard ---------------------------------------------------------------------
        //
        // ImGui's text fields (every name field, the editor search, the console) read and write the
        // clipboard through two platform callbacks. Linux answers them from the X11 selection and
        // Windows from the Win32 clipboard; Android has neither reachable from C++, so the call goes
        // to EditorActivity's Java methods (EditorClipboard), the same route the engine's own
        // runtime takes for its HTML UI clipboard.
        //
        // The activity is reached through the JNIEnv of the frame thread. GameActivity attaches its
        // android_main thread to the VM before calling it, so GetEnv succeeds there; the fallback
        // AttachCurrentThread covers a frame loop that was moved to another thread.
        JNIEnv* editorJniEnv(JavaVM** outVm){
            JavaVM* vm = nullptr;
            if (!gAndroidApp || !gAndroidApp->activity)
                return nullptr;
            if (gAndroidApp->activity->vm == nullptr)
                return nullptr;
            vm = gAndroidApp->activity->vm;
            JNIEnv* env = nullptr;
            if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env){
                JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("jasperengine-editor-frame"), nullptr};
                if (vm->AttachCurrentThread(&env, &args) != JNI_OK)
                    return nullptr;
            }
            if (outVm) *outVm = vm;
            return env;
        }

        void setClipboardText(ImGuiContext*, const char* text){
            JavaVM* vm = nullptr;
            JNIEnv* env = editorJniEnv(&vm);
            if (!env || !gAndroidApp->activity->clazz)
                return;
            jstring value = env->NewStringUTF(text ? text : "");
            if (!value)
                return;
            env->CallVoidMethod(gAndroidApp->activity->clazz,
                                clipboardMethod(env, "setEditorClipboardText", "(Ljava/lang/String;)V"),
                                value);
            env->DeleteLocalRef(value);
            if (env->ExceptionCheck())
                env->ExceptionClear();
        }

        const char* getClipboardText(ImGuiContext*) {
            static std::string cached;
            cached.clear();
            JavaVM* vm = nullptr;
            JNIEnv* env = editorJniEnv(&vm);
            if (!env || !gAndroidApp->activity->clazz)
                return cached.c_str();
            jstring value = static_cast<jstring>(
                env->CallObjectMethod(gAndroidApp->activity->clazz,
                                      clipboardMethod(env, "getEditorClipboardText", "()Ljava/lang/String;")));
            if (!value || env->ExceptionCheck()){
                if (env->ExceptionCheck()) env->ExceptionClear();
                return cached.c_str();
            }
            const char* utf8 = env->GetStringUTFChars(value, nullptr);
            if (utf8){
                cached = utf8;
                env->ReleaseStringUTFChars(value, utf8);
            }
            env->DeleteLocalRef(value);
            return cached.c_str();
        }

        // GameActivity's glue filters input before any app code sees it, and its two defaults are
        // not what an editor wants. The key default drops only volume/camera; this one states the
        // rule instead of inheriting it: every key an ImGui window can use, plus BACK when it comes
        // from the soft keyboard (some IMEs send their backspace that way, as the engine's own
        // key_filter notes), and nothing else -- a device key keeps working.
        bool editorKeyFilter(const GameActivityKeyEvent* event){
            if (event->keyCode == AKEYCODE_BACK)
                return (event->flags & AKEY_EVENT_FLAG_SOFT_KEYBOARD) != 0;
            return imGuiKeyFromAndroid(event->keyCode) != ImGuiKey_None;
        }

        // The default motion filter admits source == SOURCE_TOUCHSCREEN only, so a tablet's
        // trackpad or a plugged-in mouse would reach the editor as nothing at all. Everything the
        // editor can use gets through: the touchscreen, a mouse and a touchpad; a gamepad's
        // joystick motion is not ImGui's to consume and stays out (Android's own key routing still
        // handles its buttons).
        bool editorMotionFilter(const GameActivityMotionEvent* event){
            const int32_t source = event->source & AINPUT_SOURCE_CLASS_MASK;
            return source == AINPUT_SOURCE_CLASS_POINTER ||   // touchscreen, trackpad
                   source == AINPUT_SOURCE_CLASS_TRACKBALL;
        }

        void processKeyEvents(android_input_buffer* buffer){
            ImGuiIO& io = ImGui::GetIO();
            for (uint64_t i = 0; i < buffer->keyEventsCount; ++i){
                const GameActivityKeyEvent* event = &buffer->keyEvents[i];
                const bool down = event->action == AKEY_EVENT_ACTION_DOWN;
                // some IMEs send the soft keyboard's backspace as BACK; only take those
                if (event->keyCode == AKEYCODE_BACK){
                    if ((event->flags & AKEY_EVENT_FLAG_SOFT_KEYBOARD) != 0){
                        io.AddKeyEvent(ImGuiKey_Backspace, down);
                        if (down) io.AddInputCharacter('\b');
                    }else if (down && !io.WantTextInput){
                        // Back with no text field focused leaves the editor, as on desktop
                        gShouldClose = true;
                    }
                    continue;
                }
                const ImGuiKey key = imGuiKeyFromAndroid(event->keyCode);
                if (key == ImGuiKey_None)
                    continue;
                if (event->repeatCount > 0)
                    continue;
                io.AddKeyEvent(key, down);
                // a held key repeats the character while the IME does not; ImGui asks for the key
                // event only once, so the repeat is the character
                if (down)
                    addImGuiCharacterFor(event);
            }
        }

        // Input's touches, filled from the same motion events the ImGui pointer is built from.
        //
        // Why it matters: AndroidTouchGestures.h recognises long press, tap/double tap, pinch and
        // drag from `Input::getTouches()`, and nothing on Android ever added one -- the editor sent
        // every finger straight to ImGui and the engine's own touch state stayed empty, so no
        // gesture could fire and a game played in this build saw no touches either. The events are
        // already here, so the same loop fills both.
        //
        // `seconds` is Input's own clock (Input::getInputTime), not a fresh one: a gesture must not
        // advance while the engine's input time is paused (the editor's Play stop).
        void feedInputTouches(android_input_buffer* buffer){
            const int32_t action = (buffer->motionEventsCount > 0)
                ? (buffer->motionEvents[0].action & AMOTION_EVENT_ACTION_MASK)
                : AMOTION_EVENT_ACTION_CANCEL;
            (void)action;

            // every finger this batch knows about, so one that lifted is removed once
            bool seen[32] = {};
            for (uint64_t i = 0; i < buffer->motionEventsCount; ++i){
                const GameActivityMotionEvent* event = &buffer->motionEvents[i];
                const int32_t eventAction = event->action & AMOTION_EVENT_ACTION_MASK;

                if (eventAction == AMOTION_EVENT_ACTION_CANCEL){
                    Input::clearTouches();
                    return;
                }

                for (int32_t p = 0; p < static_cast<int32_t>(event->pointerCount) && p < 32; ++p){
                    const GameActivityPointerAxes* pointer = &event->pointers[p];
                    const int id = pointer->id;
                    if (id < 0 || id >= 32)
                        continue;
                    const float x = GameActivityPointerAxes_getX(pointer);
                    const float y = GameActivityPointerAxes_getY(pointer);

                    // DOWN and POINTER_DOWN add; MOVE repositions; UP and POINTER_UP remove.
                    // GameActivity reports the whole pointer set on MOVE, so only the finger that
                    // moved needs repositioning and the rest are already where they were.
                    const bool lifts = eventAction == AMOTION_EVENT_ACTION_UP ||
                                       (eventAction == AMOTION_EVENT_ACTION_POINTER_UP &&
                                        p == ((event->action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                              AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT));

                    if (lifts){
                        Input::removeTouch(id);
                        continue;
                    }

                    if (!seen[id]){
                        // addTouch adds a new finger and repositions one already down, so a MOVE of
                        // an existing finger is the same call
                        Input::addTouch(id, x, y);
                        seen[id] = true;
                    } else {
                        Input::setTouchPosition(id, x, y);
                    }
                }
            }
        }

        void processMotionEvents(android_input_buffer* buffer){
            feedInputTouches(buffer);
            ImGuiIO& io = ImGui::GetIO();
            for (uint64_t i = 0; i < buffer->motionEventsCount; ++i){
                GameActivityMotionEvent* event = &buffer->motionEvents[i];

                // A wheel or a real mouse still works on a tablet with a trackpad
                const int32_t source = event->source;
                if ((source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE){
                    float vscroll = GameActivityPointerAxes_getAxisValue(
                        &event->pointers[0], AMOTION_EVENT_AXIS_VSCROLL);
                    if (vscroll != 0.0f)
                        io.AddMouseWheelEvent(0.0f, vscroll);
                }

                const int32_t action = event->action & AMOTION_EVENT_ACTION_MASK;
                if (action != AMOTION_EVENT_ACTION_DOWN && action != AMOTION_EVENT_ACTION_MOVE &&
                    action != AMOTION_EVENT_ACTION_UP && action != AMOTION_EVENT_ACTION_CANCEL &&
                    action != AMOTION_EVENT_ACTION_POINTER_DOWN &&
                    action != AMOTION_EVENT_ACTION_POINTER_UP){
                    continue;
                }

                // MOVE carries every finger and reports no pointer index, so its position is
                // the one of the finger that owns the pointer.
                if (action == AMOTION_EVENT_ACTION_MOVE){
                    for (int32_t p = 0; p < static_cast<int32_t>(event->pointerCount); ++p){
                        if (event->pointers[p].id != gActivePointerId)
                            continue;
                        io.AddMousePosEvent(GameActivityPointerAxes_getX(&event->pointers[p]),
                                            GameActivityPointerAxes_getY(&event->pointers[p]));
                        break;
                    }
                    continue;
                }

                const int32_t actionIndex = (event->action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
                if (actionIndex >= static_cast<int32_t>(event->pointerCount))
                    continue;
                const GameActivityPointerAxes* pointer = &event->pointers[actionIndex];

                if (action == AMOTION_EVENT_ACTION_DOWN){
                    // a new gesture: its first finger owns the pointer
                    gActivePointerId = pointer->id;
                    io.AddMousePosEvent(GameActivityPointerAxes_getX(pointer),
                                        GameActivityPointerAxes_getY(pointer));
                    io.AddMouseButtonEvent(0, true);
                    gPointerDown = true;
                    continue;
                }

                // a second finger is not a drag of the one the editor is following
                if (pointer->id != gActivePointerId)
                    continue;

                if (action == AMOTION_EVENT_ACTION_UP ||
                    action == AMOTION_EVENT_ACTION_POINTER_UP){
                    io.AddMouseButtonEvent(0, false);
                    gPointerDown = false;
                    if (action == AMOTION_EVENT_ACTION_UP)
                        gActivePointerId = -1;
                }else if (action == AMOTION_EVENT_ACTION_CANCEL){
                    io.AddMouseButtonEvent(0, false);
                    gPointerDown = false;
                    gActivePointerId = -1;
                }
            }

            (void)gPointerDown;
        }

        // GameActivity's soft keyboard keeps the field's state on the Java side; the app reads it
        // with GameActivity_getTextInputState, which calls back synchronously on this thread. The
        // text is Java's modified UTF-8, decoded by the engine's own tested converter (the one its
        // InputField uses) rather than a second copy of the format.
        void drainTextInputState(android_app* app){
            if (!app || !app->activity || !app->textInputState)
                return;
            GameActivity_getTextInputState(app->activity, [](void* context, const GameTextInputState* state){
                if (!context || !state)
                    return;
                Backend::queueText(jasperengine::android::decodeModifiedUtf8(state->text_UTF8,
                                                                            state->text_length));
                // the state was taken: GameActivity does not hand it over twice
                static_cast<android_app*>(context)->textInputState = false;
            }, app);
        }

        // The gesture recognizer's state, kept between frames (AndroidTouchGestures.h's Recognizer
        // is a plain value: it holds the last positions and the tap window).
        jasperengine::android::RecognizerState gGestures;
        volatile bool gLongPressPending = false;

        // Advances the recognizer with this frame's touches and turns a long press into the editor's
        // context click. This is what the old code here said did not exist: "A long press on Android
        // reports nothing special by default; the editor's context click (button 1) comes from the
        // toolbars instead". Holding a finger on an object is the phone's way of asking for its menu,
        // and there is no reason to make the user find a toolbar button for it.
        //
        // The press is cancelled by moving further than the slop, so a drag never opens a menu, and
        // one that becomes a long press is consumed so it does not also fire as a tap.
        void pumpGestures(){
            const std::vector<Touch> touches = Input::getTouches();
            const float seconds = static_cast<float>(Input::getInputTime());

            auto& state = gGestures;
            jasperengine::android::updateLongPress(state, touches, seconds);
            if (state.longPress.triggered){
                // held still long enough: the context click, at the finger
                ImGuiIO& io = ImGui::GetIO();
                io.AddMousePosEvent(state.longPress.position.x, state.longPress.position.y);
                io.AddMouseButtonEvent(1, true);
                gLongPressPending = true;
            }
            if (gLongPressPending && state.longPress.pointer < 0){
                // the finger that held lifted: release the button the press opened
                ImGui::GetIO().AddMouseButtonEvent(1, false);
                gLongPressPending = false;
            }

            // the tap and the double tap are kept current so a future consumer (a double-tap zoom, a
            // tap-to-select) does not have to start its own timing; nothing reads them yet
            jasperengine::android::updateTap(state, touches);
            jasperengine::android::updateDrag(state.drag, touches);
        }

        void onAppCmd(android_app* app, int32_t cmd){
            switch (cmd){
                case APP_CMD_INIT_WINDOW:
                    if (app->window && gContextUp){
                        if (gSurface != EGL_NO_SURFACE){
                            eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                            eglDestroySurface(gDisplay, gSurface);
                        }
                        gSurface = eglCreateWindowSurface(gDisplay, gConfig, app->window, nullptr);
                        if (gSurface == EGL_NO_SURFACE)
                            Log::error("[editor] re-creating the surface failed: %d", eglGetError());
                        else
                            eglMakeCurrent(gDisplay, gSurface, gSurface, gContext);
                    }
                    break;
                case APP_CMD_TERM_WINDOW:
                    // backgrounded: the window surface goes, the context and everything ImGui
                    // and the engine hold stay. APP_CMD_INIT_WINDOW rebuilds the surface.
                    if (gContextUp && gSurface != EGL_NO_SURFACE){
                        eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                        eglDestroySurface(gDisplay, gSurface);
                        gSurface = EGL_NO_SURFACE;
                    }
                    break;
                case APP_CMD_WINDOW_RESIZED:
                case APP_CMD_CONFIG_CHANGED:
                    gRedrawRequested = true;
                    break;
                case APP_CMD_DESTROY:
                    gShouldClose = true;
                    break;
                default:
                    break;
            }
        }

    } // namespace

    // -------------------------------------------------------------------------------------------
    // Backend contract

    App Backend::app;

    App& Backend::getApp(){
        return app;
    }

    // The entry point GameActivity calls (its NativeActivity-like android_main), declared by
    // android_native_app_glue. It runs on GameActivity's main thread, which is also the GL thread.
    int Backend::runAndroid(android_app* app, int argc, char** argv){
        gAndroidApp = app;
        app->userData = nullptr;
        app->onAppCmd = &onAppCmd;
        // GameActivity's own defaults are replaced by the editor's (editorKeyFilter and
        // editorMotionFilter above): the default motion filter admits SOURCE_TOUCHSCREEN and
        // nothing else, so a tablet's trackpad or a USB mouse reached the editor as no events.
        android_app_set_key_event_filter(app, &editorKeyFilter);
        android_app_set_motion_event_filter(app, &editorMotionFilter);

        // The window exists by the time android_main runs, but the first APP_CMD_INIT_WINDOW
        // may still be ahead of us: wait for it rather than drawing into no surface.
        while (!app->window && !app->destroyRequested){
            android_poll_source* source = nullptr;
            int events = 0;
            if (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0){
                if (source)
                    source->process(app, source);
            }
        }
        if (app->destroyRequested)
            return 0;

        const int result = init(argc, argv);
        if (result != 0)
            Log::error("[editor] Backend::init returned %d", result);
        return result;
    }

    int Backend::init(int argc, char** argv){
        setEditorHost(&app);
        app.initializeSettings();

        if (!createContext())
            return -1;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        initImGuiPlatform();

        // Touch UI: scale the style before App::setup(), which lays the dock out for it
        ImGui::GetStyle().ScaleAllSizes(ANDROID_UI_SCALE);
        Theme::setUiScale(ANDROID_UI_SCALE);

        app.setup();
        gLastFrameTime = monotonicSeconds();

        RendererPlatform platform;
        platform.createContext = &createContext;
        platform.destroyContext = &destroyContext;
        platform.makeCurrent = &makeCurrent;
        platform.swapBuffers = &swapBuffers;
        platform.setSwapInterval = &setSwapInterval;
        platform.getProcAddress = &getProcAddress;
        platform.hasResetNotification = nullptr;

        static std::unique_ptr<Renderer> renderer;
        renderer = std::make_unique<Renderer>();
        // "" lets ImGui's ES3 branch pick "#version 300 es" by itself
        if (!renderer->init(platform, gWidth, gHeight, true)){
            Log::error("[editor] the ImGui renderer failed to initialize");
            shutdownImGuiPlatform();
            ImGui::DestroyContext();
            destroyContext();
            return -1;
        }

        static EditorFrame editorFrame;
        editorFrame.init(*renderer, app, &platformNewFrame);

        auto renderFrame = [&](bool forceRedraw){
            if (gShouldClose) return;
            EditorFrameState state{gRedrawRequested};
            state.framePeriod = 1.0 / 60.0;
            state.forceRedraw = forceRedraw;
            state.minimized = gSurface == EGL_NO_SURFACE;
            state.focused = true;
            state.width = gWidth;
            state.height = gHeight;
            if (!editorFrame.run(state))
                gShouldClose = true;
        };

        app.setLoadingPump([&](bool render){
            if (render) renderFrame(true);
        });

        bool engineStarted = false;
        if (!gShouldClose){
            app.engineInit(argc, argv);
            engineStarted = true;
        }
        bool engineViewReady = false;
        if (engineStarted && !gShouldClose){
            app.engineViewLoaded();
            engineViewReady = true;
        }
        if (engineStarted && !gShouldClose)
            app.loadStartupProject();

        Log::print("[editor] entering the frame loop");

        while (!gShouldClose){
            drainTextInputState(gAndroidApp);

            android_input_buffer* inputBuffer = android_app_swap_input_buffers(gAndroidApp);
            if (inputBuffer){
                if (inputBuffer->keyEventsCount)
                    processKeyEvents(inputBuffer);
                if (inputBuffer->motionEventsCount)
                    processMotionEvents(inputBuffer);
                android_app_clear_input_buffers(gAndroidApp, inputBuffer);
            }

            // After the events, before the frame draws: a finger that has been down long enough
            // becomes this frame's context click (see pumpGestures).
            pumpGestures();

            android_poll_source* source = nullptr;
            int events = 0;
            const int timeoutMillis = editorFrame.isIdle() ? 100 : 0;
            while (ALooper_pollOnce(timeoutMillis, nullptr, &events,
                                    reinterpret_cast<void**>(&source)) >= 0){
                if (source)
                    source->process(gAndroidApp, source);
                if (gAndroidApp->destroyRequested){
                    gShouldClose = true;
                    break;
                }
            }
            if (gShouldClose) break;

            diag::heartbeat();
            app.beforeFrame();
            if (gShouldClose) break;
            renderFrame(false);
            if (app.consumeBenchmarkExit())
                gShouldClose = true;
        }

        Log::print("[editor] leaving the frame loop");
        app.shutdownBackgroundWork();
        app.saveWindowSettings(gWidth, gHeight, false, ANDROID_UI_SCALE);

        renderer->shutdownImGui();
        shutdownImGuiPlatform();
        ImGui::DestroyContext();
        if (engineViewReady)
            app.engineViewDestroyed();
        renderer.reset();
        destroyContext();
        if (engineStarted)
            app.engineShutdown();
        return app.getExitCode();
    }

    // -- the rest of the contract: a phone has no OS cursors, no OS windows and no menu bar, so
    // these are no-ops rather than missing behaviour

    void Backend::disableMouseCursor(){}
    void Backend::enableMouseCursor(){}
    void Backend::setMouseControlSuspended(bool){}
    void Backend::setMouseMode(MouseMode){}
    void Backend::setGameCursorInSceneRect(bool){}
    void Backend::closeWindow(){ gShouldClose = true; }

    float Backend::penPressure(){
        // The engine's Android input already reads the stylus's pressure axis
        // (NativeEngine.cpp stylusPressureOf -> Input::setPenPressure) and returns -1 for a finger,
        // a mouse or a hover, which is exactly this contract: a value below zero means "no pen".
        // The terrain brush (window/TerrainEditWindow.cpp) paints at the pressure it gets here, so
        // returning -1 unconditionally made a stylus paint at one fixed strength on a tablet while
        // pressing harder did nothing.
        return jasperengine::Input::getPenPressure();
    }

    bool Backend::isRunningOnWayland(){ return false; }

    ImVec2 Backend::sceneRenderScale(ImVec2 framebufferScale, float dpiScale){
        return ImVec2(framebufferScale.x * dpiScale, framebufferScale.y * dpiScale);
    }

    float Backend::setMainMenu(const PlatformMenuModel&, PlatformMenuCallback){
        // no menu bar on Android: zero asks App to draw its ImGui fallback
        return 0.0f;
    }

    bool Backend::nativeMenuHandlesKeyEquivalents(){ return false; }

    ImTextureID Backend::getImGuiTexture(TextureRender* texture){
        return static_cast<ImTextureID>(texture->getGLHandler());
    }

    void Backend::updateWindowTitle(const std::string&){}

    void* Backend::getNFDWindowHandle(){ return nullptr; }

} // namespace jasperengine::editor

// GameActivity's entry point. android_native_app_glue calls it on the GL thread.
extern "C" void android_main(struct android_app* app){
    jasperengine::editor::Backend::runAndroid(app, 0, nullptr);
}
