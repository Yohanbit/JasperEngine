// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HostServices on Android: the platform services the editor's Platform reads (the locale, the
// operating system's name and version, the device, the memory, the screen, the network).
//
// There was no Android implementation, and the editor's shared library failed to link because of
// it: editor/Platform.cpp calls all nine of these, and only Linux, Windows and Mac provided them.
// The game runtime's values come from JasperEngineAndroid, which reaches them through
// NativeEngine -- the singleton the editor does not instantiate. So this file reads what Android
// exposes to an app's own process, and reports the same shapes as the desktop files.
//
// Where a value genuinely needs Java (the locale jar, for one) it is left to the caller's
// fallback rather than faked: Platform::getPreferredLocales already falls back to
// System::getPreferredLocales when this returns nothing.

#include "HostServices.h"

#include <android/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/system_properties.h>
#include <unistd.h>

namespace jasperengine {
namespace HostServices {

    namespace {

        // __system_property_get is the only synchronous read of an Android property available to
        // native code without Java; it needs <sys/system_properties.h>.
        std::string property(const char* name){
            char value[PROP_VALUE_MAX] = {};
            const int length = __system_property_get(name, value);
            return length > 0 ? std::string(value, static_cast<size_t>(length)) : std::string();
        }

        // "13" -> "13", "33" -> "13": Android's SDK level is not its release version, so the
        // release is what a Unity "operating system" string carries and the API level goes in
        // the version. Both are only readable as properties.
        std::string releaseVersion(){
            const std::string release = property("ro.build.version.release");
            return release.empty() ? std::string("Unknown") : release;
        }

    } // namespace

    // System.openURL on Android needs an ACTION_VIEW intent on the activity, which is Java.
    // Returning false is the honest answer: Platform::platformOpenURL then logs "the system has
    // no handler for it" and records the attempt, which is what a headless run does too.
    bool openURL(const std::string&){
        return false;
    }

    // The locale is Java's (LocaleList.getDefault()); native code cannot reach it without a JNI
    // call into the activity, and Platform::getPreferredLocales falls back to the engine's own
    // System::getPreferredLocales, which does go through Java. Empty, not guessed.
    std::vector<std::string> preferredLocales(){
        return {};
    }

    std::string regionOverride(){
        return "";
    }

    std::string operatingSystem(){
        // Unity's SystemInfo.operatingSystem on Android reads "Android OS <release> / API-<level>
        // (<build id>)". The build id is not needed to tell two devices apart.
        const std::string api = property("ro.build.version.sdk");
        std::string value = "Android OS " + releaseVersion();
        if (!api.empty())
            value += " / API-" + api;
        return value;
    }

    std::string operatingSystemVersion(){
        return releaseVersion();
    }

    std::string deviceModel(){
        const std::string model = property("ro.product.model");
        return model.empty() ? std::string("Android device") : model;
    }

    int systemMemoryMiB(){
        // /proc/meminfo's MemTotal is the device's RAM in kB; the editor shows it in its device
        // information, and 0 means unknown (HostServices.h's contract).
        std::FILE* file = std::fopen("/proc/meminfo", "r");
        if (!file)
            return 0;
        char line[256] = {};
        int miB = 0;
        while (std::fgets(line, sizeof(line), file)){
            int kB = 0;
            if (std::sscanf(line, "MemTotal: %d kB", &kB) == 1){
                miB = kB / 1024;
                break;
            }
        }
        std::fclose(file);
        return miB;
    }

    NetworkReachability internetReachability(){
        // Android keeps the reachability answer in Java's ConnectivityManager. The kernel's own
        // view is what is readable here: an interface that is up, running and not the loopback.
        // /sys/class/net is readable by an app on every level this builds for.
        std::FILE* names = std::fopen("/proc/net/route", "r");
        if (!names)
            return NetworkReachability::NotReachable;
        char line[256] = {};
        bool reachable = false;
        // skip the header
        if (std::fgets(line, sizeof(line), names)){
            while (std::fgets(line, sizeof(line), names)){
                // the route table lists only routes that exist; any non-loopback entry means an
                // interface is up. A phone's data connection and Wi-Fi both appear here.
                if (!std::strstr(line, "\tlo\t") && std::strstr(line, "\t00000000\t")){
                    reachable = true;
                    break;
                }
            }
        }
        std::fclose(names);
        return reachable ? NetworkReachability::ReachableViaLocalAreaNetwork
                         : NetworkReachability::NotReachable;
    }

    float screenDpi(){
        // The density the display reports, in dots per inch, from the property Android itself
        // keeps for it. 0 lets Platform apply its own default.
        const std::string density = property("ro.sf.lcd_density");
        if (density.empty())
            return 0.0f;
        const int dpi = std::atoi(density.c_str());
        return dpi > 0 ? static_cast<float>(dpi) : 0.0f;
    }

    float refreshRate(){
        // The display's refresh rate is Java's (Display.getRefreshRate()); the engine's own
        // JasperEngineAndroid reads it through a JNI call. 60 is the value HostServices.h names
        // for "unknown", and a wrong rate here would only make the editor's frame pacing guess.
        return 60.0f;
    }

    void setKeepScreenOn(bool keepOn){
        // EditorActivity already keeps the screen on for the whole session
        // (FLAG_KEEP_SCREEN_ON in its onCreate), which is the stronger request: the window flag
        // this would set is what the game runtime uses when a game asks for it late.
        (void)keepOn;
    }

}
}
