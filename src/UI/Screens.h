#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Internal contract between the screen manager (Ui.cpp) and the individual
// screens. Each screen builds its widgets once into the page container it is
// given, at boot, then updates them in place on refresh -- so opening anything
// never has to find heap at a bad moment. The one exception is list rows: see
// the release functions below.

lv_obj_t *StatusBar_create(lv_obj_t *parent);
void StatusBar_refresh();

lv_obj_t *ScreenFocus_create(lv_obj_t *parent);
void ScreenFocus_refresh();

/// @brief Close the expanded AC panel, if open. Called when leaving the page or
/// resting, so Home always comes back to its overview.
void ScreenFocus_collapse();

lv_obj_t *ScreenDevices_create(lv_obj_t *parent);
void ScreenDevices_refresh();

lv_obj_t *ScreenSensors_create(lv_obj_t *parent);
void ScreenSensors_refresh();

/// @brief Free what a page only needs while visible: list rows, a device's
/// answer. The manager calls these when a page is left or the panel rests, and
/// the next refresh rebuilds on demand. The exception to the build-once rule
/// above, because a full list is the largest allocation the UI makes and its
/// page is off-screen almost all the time.
void ScreenDevices_release();
void ScreenSensors_release();
void ScreenDevice_release();
void ScreenResource_release();

lv_obj_t *ScreenSettings_create(lv_obj_t *parent);
void ScreenSettings_refresh();

lv_obj_t *ScreenDevice_create(lv_obj_t *parent);
void ScreenDevice_refresh();

/// @brief Point the detail page at a device, before showing it.
void ScreenDevice_open(const String &deviceName);

lv_obj_t *ScreenResource_create(lv_obj_t *parent);
void ScreenResource_refresh();

/// @brief Point the resource page at one address, before showing it.
void ScreenResource_open(const String &deviceName, const String &resourceName);

lv_obj_t *ScreenRest_create(lv_obj_t *parent);
void ScreenRest_refresh();

/// @brief Shared helper: "3m ago" / "2h ago" style age text for a now() stamp.
void Ui_formatAge(uint32_t stamp, char *out, size_t outSize);

/// @brief Whether a published value is a JSON document rather than a scalar.
///
/// This is the shape of the payload, not the protocol's `struct` value type:
/// the panel never reads resource manifests, so it has no declared type to go
/// on. A device publishing a JSON document under a `string` type -- which is
/// what an AC controller's status document is -- reads as a struct here, and
/// that is the useful answer, because the question being asked is "will this
/// fit in a list row".
bool Ui_valueIsStruct(const String &value);

/// @brief Whether there is enough heap left to build another list row.
///
/// Rows are created lazily, as devices and sensors turn up, so the panel does
/// not commit memory for a network far bigger than the one it is on. The heap
/// is shared with mbedTLS, which needs tens of KB contiguous for every MQTT
/// reconnect -- so a row is only ever worth building if the connection can
/// still be re-established afterwards. A row that is refused simply is not
/// drawn; the registry still tracks the underlying device or sensor.
bool Ui_canAllocateRow();
