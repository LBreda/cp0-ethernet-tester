/// @file linux_input.h
/// @brief Linux keyboard input for LVGL: key routing, nav-button shortcuts and key listeners.

#pragma once

#include "lvgl.h"

#include <cstddef>
#include <cstdint>

namespace platform {

/// Number of navigation buttons that can be bound to shortcut keys.
constexpr size_t kNavKeyCount = 5;
/// Base for app-specific key codes, outside the LVGL/Unicode range.
constexpr uint32_t kSpecialKeyBase = 0x200000U;
constexpr uint32_t kKeyPrintScreen = kSpecialKeyBase | 18U;
constexpr uint32_t kKeyHelp = kSpecialKeyBase | 19U;

/// Called when a key is released.
using KeyReleaseListener = void (*)(uint32_t key, const char* key_name, void* user_data);
/// Called before normal routing; return true to consume the key.
using GlobalKeyListener = bool (*)(uint32_t key,
                                   const char* key_name,
                                   bool long_pressed,
                                   void* user_data);

/// Creates the keyboard input device for the display.
void init_key_input(lv_display_t* display);
/// Installs the key router on an existing input device.
void attach_key_router(lv_indev_t* indev);
/// Enables or disables mapping shortcut keys to registered nav buttons.
void set_nav_shortcut_mode(bool enabled);
/// Binds a button to nav slot `index` (< kNavKeyCount).
void register_nav_button(size_t index, lv_obj_t* button);
/// Unbinds the button if it is still registered at `index`.
void unregister_nav_button(size_t index, lv_obj_t* button);
/// Sets the single key-release listener.
void set_key_release_listener(KeyReleaseListener listener, void* user_data);
/// Clears the key-release listener if it matches listener/user_data.
void clear_key_release_listener(KeyReleaseListener listener, void* user_data);
/// Sets the single global key listener.
void set_global_key_listener(GlobalKeyListener listener, void* user_data);
/// Clears the global key listener if it matches listener/user_data.
void clear_global_key_listener(GlobalKeyListener listener, void* user_data);
/// Returns a human-readable name for a key code.
const char* describe_key(uint32_t key);

} // namespace platform
