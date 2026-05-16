#include "EmulateTouchpadGesture.hpp"
#include "GestureManager.hpp"
#include "TouchVisualizer.hpp"
#include "globals.hpp"
#include "version.hpp"
#include <any>
#include <expected>
#include <stdexcept>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/CloseGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/DispatcherGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/FloatGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/FullscreenGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/MoveGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ResizeGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/SpecialWorkspaceGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/version.h>

#include <hyprlang.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/UniquePtr.hpp>
#include <hyprutils/string/ConstVarList.hpp>
#undef private

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <memory>
#include <string>

const CHyprColor s_pluginColor       = {0x61 / 255.0f, 0xAF / 255.0f, 0xEF / 255.0f, 1.0f};
const CHyprColor error_color         = {204. / 255.0, 2. / 255.0, 2. / 255.0, 1.0};
const std::string KEYWORD_HG_BIND    = "hyprgrass-bind";
const std::string KEYWORD_HG_GESTURE = "hyprgrass-gesture";

inline std::unique_ptr<Visualizer> g_pVisualizer;

static bool g_unloading = false;

void hkOnTouchDown(ITouch::SDownEvent ev, Event::SCallbackInfo& cbinfo) {
    cbinfo.cancelled = g_pGestureManager->onTouchDown(ev);
}

void hkOnTouchUp(ITouch::SUpEvent ev, Event::SCallbackInfo& cbinfo) {
    cbinfo.cancelled = g_pGestureManager->onTouchUp(ev);
}

void hkOnTouchMove(ITouch::SMotionEvent ev, Event::SCallbackInfo& cbinfo) {
    cbinfo.cancelled = g_pGestureManager->onTouchMove(ev);
}

// Core hyprgrass-gesture handler, shared by the legacy hyprlang keyword and the
// Lua `hl.plugin.touch_gestures.gesture` function.
//
// `flags` is the trailing flag string (e.g. "p"); `RHS` is the comma-delimited
// gesture definition that the keyword/function received.
static std::expected<void, std::string> addHyprgrassGesture(const std::string& flags, const char* RHS) {
    if (g_unloading)
        return {};

    Hyprutils::String::CConstVarList data(RHS);

    auto maybePattern = parseGesturePattern(data);
    if (!maybePattern)
        return std::unexpected(std::string(maybePattern.error()));
    GestureConfig pattern = maybePattern.value();

    int startDataIdx    = 3;
    uint32_t modMask    = 0;
    float deltaScale    = 1.F;
    bool disableInhibit = false;

    for (const auto arg : flags) {
        switch (arg) {
            case 'p':
                disableInhibit = true;
                break;
            default:
                return std::unexpected("hyprgrass-gesture: invalid flag");
        }
    }

    while (true) {

        if (data[startDataIdx].starts_with("mod:")) {
            modMask = g_pKeybindManager->stringToModMask(std::string(data[startDataIdx].substr(4)));
            startDataIdx++;
            continue;
        } else if (data[startDataIdx].starts_with("scale:")) {
            try {
                deltaScale = std::clamp(std::stof(std::string(data[startDataIdx].substr(6))), 0.1F, 10.F);
                startDataIdx++;
                continue;
            } catch (...) {
                return std::unexpected(
                    std::format("Invalid delta scale: {}", std::string(data[startDataIdx].substr(6)))
                );
            }
        }

        break;
    }

    std::expected<void, std::string> resultFromGesture;

    CTrackpadGestures* handler = g_pShimTrackpadGestures->get(pattern.type);

    if (data[startDataIdx] == "dispatcher")
        resultFromGesture = handler->addGesture(
            makeUnique<CDispatcherTrackpadGesture>(
                std::string(data[startDataIdx + 1]), data.join(",", startDataIdx + 2)
            ),
            pattern.fingers, pattern.direction, modMask, deltaScale, disableInhibit
        );
    else if (data[startDataIdx] == "workspace")
        resultFromGesture = handler->addGesture(
            makeUnique<CWorkspaceSwipeGesture>(), pattern.fingers, pattern.direction, modMask, deltaScale,
            disableInhibit
        );
    else if (data[startDataIdx] == "resize")
        // this handler halves the deltaScale
        resultFromGesture = handler->addGesture(
            makeUnique<CResizeTrackpadGesture>(), pattern.fingers, pattern.direction, modMask, deltaScale * 2,
            disableInhibit
        );
    else if (data[startDataIdx] == "move")
        // this handler halves the deltaScale
        resultFromGesture = handler->addGesture(
            makeUnique<CMoveTrackpadGesture>(), pattern.fingers, pattern.direction, modMask, deltaScale * 2,
            disableInhibit
        );
    else if (data[startDataIdx] == "special")
        resultFromGesture = handler->addGesture(
            makeUnique<CSpecialWorkspaceGesture>(std::string(data[startDataIdx + 1])), pattern.fingers,
            pattern.direction, modMask, deltaScale, disableInhibit
        );
    else if (data[startDataIdx] == "close")
        resultFromGesture = handler->addGesture(
            makeUnique<CCloseTrackpadGesture>(), pattern.fingers, pattern.direction, modMask, deltaScale, disableInhibit
        );
    else if (data[startDataIdx] == "float")
        resultFromGesture = handler->addGesture(
            makeUnique<CFloatTrackpadGesture>(std::string(data[startDataIdx + 1])), pattern.fingers, pattern.direction,
            modMask, deltaScale, disableInhibit
        );
    else if (data[startDataIdx] == "fullscreen")
        resultFromGesture = handler->addGesture(
            makeUnique<CFullscreenTrackpadGesture>(std::string(data[startDataIdx + 1])), pattern.fingers,
            pattern.direction, modMask, deltaScale, disableInhibit
        );
    else if (data[startDataIdx] == "unset")
        resultFromGesture =
            handler->removeGesture(pattern.fingers, pattern.direction, modMask, deltaScale, disableInhibit);
    else if (data[startDataIdx] == "emulate_touchpad") {
        const auto fingersStr = data[startDataIdx + 1];
        uint32_t fingers      = 0;

        try {
            fingers = std::stoul(std::string(fingersStr));
        } catch (std::invalid_argument) {
            return std::unexpected(std::format("Argument for emulate_touchpad expects a number, got: {}", fingersStr));
        }

        eTrackpadGestureDirection dir = g_pTrackpadGestures->dirForString(data[startDataIdx + 2]);
        if (ShimTrackpadGestures::isPinch(pattern.direction) != ShimTrackpadGestures::isPinch(dir)) {
            if (ShimTrackpadGestures::isPinch(dir))
                return std::unexpected("emulate_touchpad: pinch gestures need to be bound to pinch touch direction");
            return std::unexpected(
                "emulate_touchpad: non-pinch gestures need to be bound to bind to a non-pinch touch direction"
            );
        }

        resultFromGesture = std::expected(handler->addGesture(
            makeUnique<EmulateTouchpadGesture>(fingers, dir), pattern.fingers, pattern.direction, modMask, deltaScale,
            disableInhibit
        ));
    } else
        return std::unexpected(std::format("Invalid gesture: {}", data[startDataIdx]));

    if (!resultFromGesture)
        return std::unexpected(resultFromGesture.error());

    return {};
}

static Hyprlang::CParseResult hyprgrassGestureKeyword(const char* LHS, const char* RHS) {
    Hyprlang::CParseResult result;

    const int prefix_size = std::size(KEYWORD_HG_GESTURE);
    const auto res        = addHyprgrassGesture(std::string(LHS).substr(prefix_size), RHS);
    if (!res)
        result.setError(res.error().c_str());

    return result;
}

static void onPreConfigReload() {
    if (g_pGestureManager)
        g_pGestureManager->internalBinds.clear();

    if (g_pShimTrackpadGestures) {
        for (auto& g : g_pShimTrackpadGestures->gestures) {
            g.clearGestures();
        }
    }
}

SDispatchResult listInternalBinds(std::string) {
    static const DragGestureType dragGestureTypes[3] = {
        DragGestureType::SWIPE,
        DragGestureType::LONG_PRESS,
        DragGestureType::EDGE_SWIPE,
    };
    Log::logger->log(Log::DEBUG, "[hyprgrass] Listing internal binds:");
    for (const auto& bind : g_pGestureManager->internalBinds) {
        Log::logger->log(Log::DEBUG, "[hyprgrass] | gesture: {}", bind->key);
        Log::logger->log(Log::DEBUG, "[hyprgrass] |     dispatcher: {}", bind->handler);
        Log::logger->log(Log::DEBUG, "[hyprgrass] |     arg: {}", bind->arg);
        Log::logger->log(Log::DEBUG, "[hyprgrass] |     mouse: {}", bind->mouse);
        Log::logger->log(Log::DEBUG, "[hyprgrass] |     locked: {}", bind->locked);
        Log::logger->log(Log::DEBUG, "[hyprgrass] |");
    }

    for (const auto& type : dragGestureTypes) {
        auto handler = g_pShimTrackpadGestures->get(type);
        for (const auto& g : handler->m_gestures) {
            DragGestureEvent gev = {
                .time         = 0,
                .type         = type,
                .direction    = toHyprgrassDirection(g->direction),
                .finger_count = static_cast<uint32_t>(g->fingerCount),
                .edge_origin  = static_cast<uint32_t>(g->fingerCount),
            };
            Log::logger->log(Log::DEBUG, "[hyprgrass] | gesture: {}", gev.to_string());
            Log::logger->log(Log::DEBUG, "[hyprgrass] |     modifiers: {}", g->modMask);
            Log::logger->log(Log::DEBUG, "[hyprgrass] |     scaling: {}", g->deltaScale);
        }
    }
    return SDispatchResult{.success = true};
}

// Core hyprgrass-bind handler, shared by the legacy hyprlang keyword and the
// Lua `hl.plugin.touch_gestures.bind*` functions.
//
// `flagStr` is the trailing flag string (e.g. "m", "l", "ml"); `V` is the
// comma-delimited bind definition `<mods>, <gesture_event>, <dispatcher>, [args]`.
static std::expected<void, std::string> addHyprgrassBind(const std::string& flagStr, const std::string& V) {
    std::string v = V;
    auto vars     = Hyprutils::String::CVarList(v, 4);
    struct {
        bool mouse;
        bool locked;
    } flags = {};

    if (vars.size() < 3)
        return std::unexpected("must have at least 3 fields: <empty>, <gesture_event>, <dispatcher>, [args]");

    uint32_t modMask = g_pKeybindManager->stringToModMask(vars[0]);

    for (char c : flagStr) {
        switch (c) {
            case 'm':
                flags.mouse = true;
                break;
            case 'l':
                flags.locked = true;
                break;
            default:
                HyprlandAPI::addNotification(
                    PHANDLE, std::string("ignoring invalid hyprgrass-bind flag: ") + c, error_color, 5000
                );
        }
    }

    const auto key            = vars[1];
    const auto dispatcher     = flags.mouse ? "mouse" : vars[2];
    const auto dispatcherArgs = flags.mouse ? vars[2] : vars[3];

    g_pGestureManager->internalBinds.emplace_back(makeShared<SKeybind>(SKeybind{
        .key     = key,
        .modmask = modMask,
        .handler = dispatcher,
        .arg     = dispatcherArgs,
        .locked  = flags.locked,
        .mouse   = flags.mouse,
    }));

    return {};
}

Hyprlang::CParseResult hyrgrassBindKeyword(const char* K, const char* V) {
    Hyprlang::CParseResult result;

    const int prefix_size = std::size(KEYWORD_HG_BIND);
    const auto res        = addHyprgrassBind(std::string(K).substr(prefix_size), V);
    if (!res)
        result.setError(res.error().c_str());

    return result;
}

// Collects every argument passed to a Lua function into a single
// comma-delimited string, equivalent to the value the hyprlang keyword received.
//
// Each argument may itself be a comma-delimited fragment, so both calling
// styles work:
//   hl.plugin.touch_gestures.bind(", tap:3, exec, firefox")
//   hl.plugin.touch_gestures.bind("", "tap:3", "exec", "firefox")
static std::string luaArgsToValueString(lua_State* L) {
    std::string out;
    const int   n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1)
            out += ", ";
        size_t      len = 0;
        const char* s   = luaL_tolstring(L, i, &len); // pushes a string copy
        out.append(s, len);
        lua_pop(L, 1);
    }
    return out;
}

// hl.plugin.touch_gestures.bind / bindm / bindl
// Each invocation registers exactly one gesture bind, sidestepping the Lua
// table duplicate-key limitation of trying to express binds via hl.config.
static int luaHyprgrassBindFlags(lua_State* L, const std::string& flags) {
    if (g_unloading || !g_pGestureManager)
        return 0;

    const auto res = addHyprgrassBind(flags, luaArgsToValueString(L));
    if (!res) {
        lua_pushstring(L, std::format("hyprgrass-bind: {}", res.error()).c_str());
        return lua_error(L);
    }
    return 0;
}

static int luaHyprgrassBind(lua_State* L) {
    return luaHyprgrassBindFlags(L, "");
}

static int luaHyprgrassBindm(lua_State* L) {
    return luaHyprgrassBindFlags(L, "m");
}

static int luaHyprgrassBindl(lua_State* L) {
    return luaHyprgrassBindFlags(L, "l");
}

// hl.plugin.touch_gestures.gesture
static int luaHyprgrassGesture(lua_State* L) {
    if (g_unloading)
        return 0;

    const auto value = luaArgsToValueString(L);
    const auto res   = addHyprgrassGesture("", value.c_str());
    if (!res) {
        lua_pushstring(L, std::format("hyprgrass-gesture: {}", res.error()).c_str());
        return lua_error(L);
    }
    return 0;
}

std::shared_ptr<HOOK_CALLBACK_FN> g_pTouchDownHook;
std::shared_ptr<HOOK_CALLBACK_FN> g_pTouchUpHook;
std::shared_ptr<HOOK_CALLBACK_FN> g_pTouchMoveHook;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    HyprlandAPI::addConfigValue(
        PHANDLE, "plugin:touch_gestures:workspace_swipe_fingers", Hyprlang::CConfigValue((Hyprlang::INT)3)
    );
    HyprlandAPI::addConfigValue(
        PHANDLE, "plugin:touch_gestures:workspace_swipe_edge", Hyprlang::CConfigValue((Hyprlang::STRING) "d")
    );
    HyprlandAPI::addConfigValue(
        PHANDLE, "plugin:touch_gestures:sensitivity", Hyprlang::CConfigValue((Hyprlang::FLOAT)1.0)
    );
    HyprlandAPI::addConfigValue(
        PHANDLE, "plugin:touch_gestures:long_press_delay", Hyprlang::CConfigValue((Hyprlang::INT)400)
    );
    HyprlandAPI::addConfigValue(
        PHANDLE, "plugin:touch_gestures:edge_margin", Hyprlang::CConfigValue((Hyprlang::INT)10)
    );
    HyprlandAPI::addConfigValue(
        PHANDLE, "plugin:touch_gestures:experimental:send_cancel", Hyprlang::CConfigValue((Hyprlang::INT)1)
    );
    HyprlandAPI::addConfigValue(
        PHANDLE, "plugin:touch_gestures:resize_on_border_long_press", Hyprlang::CConfigValue((Hyprlang::INT)1)
    );
    HyprlandAPI::addConfigValue(
        PHANDLE, "plugin:touch_gestures:emulate_touchpad_swipe", Hyprlang::CConfigValue((Hyprlang::INT)0)
    );
    HyprlandAPI::addConfigValue(
        PHANDLE, "plugin:touch_gestures:debug:visualize_touch", Hyprlang::CConfigValue((Hyprlang::INT)0)
    );

    // Legacy hyprlang config (hyprland.conf). addConfigKeyword is a no-op when
    // the active config is Lua-based, so this stays harmless for Lua users.
    HyprlandAPI::addConfigKeyword(
        PHANDLE, KEYWORD_HG_BIND, hyrgrassBindKeyword, Hyprlang::SHandlerOptions{.allowFlags = true}
    );
    HyprlandAPI::addConfigKeyword(
        PHANDLE, KEYWORD_HG_GESTURE, hyprgrassGestureKeyword, Hyprlang::SHandlerOptions{true}
    );

    // Lua config (hyprland.lua). Each function maps 1:1 to a hyprlang keyword
    // and may be called any number of times, unlike a Lua table key. These are
    // exposed under the global `hl` table as hl.plugin.touch_gestures.<name>.
    // addLuaFunction is a no-op when the active config is legacy hyprlang.
    HyprlandAPI::addLuaFunction(PHANDLE, "touch_gestures", "bind", luaHyprgrassBind);
    HyprlandAPI::addLuaFunction(PHANDLE, "touch_gestures", "bindm", luaHyprgrassBindm);
    HyprlandAPI::addLuaFunction(PHANDLE, "touch_gestures", "bindl", luaHyprgrassBindl);
    HyprlandAPI::addLuaFunction(PHANDLE, "touch_gestures", "gesture", luaHyprgrassGesture);

    static auto P0 = Event::bus()->m_events.config.preReload.listen([&] { onPreConfigReload(); });

    HyprlandAPI::addDispatcherV2(PHANDLE, "touchBind", [&](std::string args) {
        HyprlandAPI::addNotification(
            PHANDLE, "[hyprgrass] touchBind dispatcher deprecated, use the hyprgrass-bind keyword instead",
            CHyprColor(0.8, 0.2, 0.2, 1.0), 5000
        );
        g_pGestureManager->touchBindDispatcher(args);
        return SDispatchResult{
            .success = true,
        };
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprgrass:debug:binds", listInternalBinds);

    const std::string hlTargetVersion = __hyprland_api_get_hash();
    const std::string hlVersion       = __hyprland_api_get_client_hash();

    if (hlVersion != hlTargetVersion) {
        HyprlandAPI::addNotification(
            PHANDLE, "Mismatched Hyprland version! check logs for details", CHyprColor(0.8, 0.7, 0.26, 1.0), 5000
        );
        Log::logger->log(Log::ERR, "[hyprgrass] version mismatch!");
        Log::logger->log(Log::ERR, "[hyprgrass] | hyprgrass was built against: {}", hlTargetVersion);
        Log::logger->log(Log::ERR, "[hyprgrass] | actual hyprland version: {}", hlVersion);
    }

    static auto P1 = Event::bus()->m_events.input.touch.down.listen(hkOnTouchDown);
    static auto P2 = Event::bus()->m_events.input.touch.up.listen(hkOnTouchUp);
    static auto P3 = Event::bus()->m_events.input.touch.motion.listen(hkOnTouchMove);

    HyprlandAPI::reloadConfig();

    const auto EMULATE_TOUCHPAD =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:touch_gestures:emulate_touchpad_swipe")
            ->getDataStaticPtr();

    if (**EMULATE_TOUCHPAD) {
        HyprlandAPI::addNotification(
            PHANDLE,
            "[hyprgrass] plugin:touch_gestures:emulate_touchpad_swipe,\n"
            "use `hyprgrass-gesture = swipe, <fingers>, <direction>, emulate_touchpad, <fingers>, <direction>`",
            CHyprColor(0.8, 0.2, 0.2, 1.0), 5000
        );
    }

    g_pGestureManager       = std::make_unique<GestureManager>();
    g_pShimTrackpadGestures = std::make_unique<ShimTrackpadGestures>();

    return {"hyprgrass", "Touchscreen gestures", "horriblename", HYPRGRASS_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // idk if I should do this, but just in case
    g_pGestureManager.reset();
    g_unloading = true;
}
