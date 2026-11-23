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
#include "Engine.h"
#include "Log.h"
#include "TextureRender.h"

#include "imgui.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <game-activity/GameActivity.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

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
            // the engine's own call, same flags: a text field's keyboard edits in place
            constexpr int IME_ACTION_NONE = 1;
            constexpr int IME_FLAG_NO_FULLSCREEN = 33554432;
            GameActivity_setImeEditorInfo(gAndroidApp->activity, InputType_dot_TYPE_CLASS_TEXT,
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
                default: return ImGuiKey_None;
            }
        }

        // ImGui wants a character for Enter, Tab and Backspace: a hardware keyboard reports them
        // as keys only, and a text field would never see the newline or the deletion.
        void addImGuiCharacterFor(int32_t keycode){
            ImGuiIO& io = ImGui::GetIO();
            switch (keycode){
                case AKEYCODE_DEL: io.AddInputCharacter('\b'); break;
                case AKEYCODE_ENTER: io.AddInputCharacter('\r'); break;
                case AKEYCODE_TAB: io.AddInputCharacter('\t'); break;
                case AKEYCODE_SPACE: io.AddInputCharacter(' '); break;
                default: break;
            }
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
                if (down)
                    addImGuiCharacterFor(event->keyCode);
            }
        }

        void processMotionEvents(android_input_buffer* buffer){
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

            // A long press on Android reports nothing special by default; the editor's context
            // click (button 1) comes from the toolbars instead, which suits a touch screen better.
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
        // a finger has no pressure axis ImGui consumes; a stylus would report it here
        return -1.0f;
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
