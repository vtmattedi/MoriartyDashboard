# NightMare Dashboard

A handheld panel for the NightMare network, in an enclosure with a battery. It
owns no hardware of its own beyond that battery: it joins the network as an
ordinary NightMare device and drives an air conditioner, a light and an RGB
light that live on *other* devices, over the NightMare resource protocol.

Current transport: **ESP-NOW through the NightMare gateway**. The Wi-Fi IP
station remains enabled for SNTP and other direct IP services; MQTT is compiled
out (`NM_ENABLE_MQTT=0`). The gateway must share the station's Wi-Fi channel.

Hardware: **Sunton ESP32-3248S035C** — ESP32-WROOM-32, 4MB flash, no PSRAM,
ST7796 320×480 SPI panel, GT911 capacitive touch, PWM backlight. The board's RGB LED is held off and the
firmware has no way to turn it on, to spare the battery.
Held **portrait**: the UI is 320×480. Added with the enclosure: a single
lithium cell, read through a 1M / 1M divider on GPIO 35.

## Power

A panel on a battery spends most of its life not being looked at, so it steps
down through three stages as it is left alone, and one touch undoes all of it.

| Stage       | Entered                                   | What is on                                                    | A touch |
| ----------- | ----------------------------------------- | ------------------------------------------------------------- | ------- |
| **Active**  | any touch                                 | Everything. Backlight at `bl_active`.                          | is a touch |
| **Resting** | `rest_after` seconds without a touch       | The resting clock at `bl_rest`. Network still up, so the clock's corners stay live. | wakes instantly, and is spent on waking |
| **Asleep**  | `sleep_after` seconds on the resting clock | Nothing but the touch controller. Backlight off, radio stopped and deinitialised, display controller in sleep-in, processor in light sleep. | wakes in under a second; the network rejoins over the next few |

The two timeouts add: with the defaults the panel rests after one minute and
sleeps five minutes after that. Either can be `0` for never — but sleep is only
reached through rest, so with `rest_after` at 0 the panel never sleeps on its
own either. **Setup** has both sliders and a button for each stage; `SLEEP` on
the console does the same.

**Why light sleep, when the stage is the deepest one.** A true ESP32 deep sleep
can only be woken by a pin in the RTC domain, and this firmware drives no line
from the GT911's interrupt output to one. So the sleep stage keeps the processor
in *light* sleep and wakes it on a timer four times a second to ask the touch
controller, over I2C, whether anything happened — a millisecond of work per
look. The GT911's status register latches a report until the host clears it,
which is why a tap much shorter than the interval between looks is still caught.

It should cost less than it sounds, and it buys something. On this board the
floor is expected to be set by what cannot be switched off — the regulator, the
USB bridge, the touch controller itself — rather than by the difference between
the two sleep modes; that is reasoning from the parts, not a measurement, and a
meter on the battery lead is what settles it. And light sleep keeps RAM: waking is a continuation, not a boot, so the page is back
on screen before the radio has finished starting. If the GT911's INT line is
ever wired to an RTC-capable GPIO, `board_sleep_until_touch()` in
[lib/boardstuff/boardstuff.cpp](lib/boardstuff/boardstuff.cpp) is the one
function to change.

Going to sleep publishes the panel's retained `status` with `online: false`
before the radio stops, so the rest of the network sees it leave rather than
time out. Waking restarts the radio, the Wi-Fi station and ESP-NOW through the
library's own start path, and gateway discovery and the retained replay follow
exactly as after a boot.

**None of the sleep stage has run on the hardware yet.** It compiles; whether
the radio comes back cleanly on this board's heap, and whether this GT911's
firmware latches its status the way the datasheet says, are the two things to
watch on the first try. `[power]` lines on the serial console bracket every
sleep, with a heap sample on waking.

### Battery

[src/App/Battery.cpp](src/App/Battery.cpp) samples the divider every ten
seconds, smooths it, and turns the voltage into a percentage with a lookup on a
lithium cell's resting curve. It shows in the status bar, on the resting clock
and on Setup (with the voltage), turns red at 15%, and is published as two
resources of the panel's own, `battery` (percent) and `battery_voltage`, so
anything on the network can watch a panel running down. With no cell fitted the
reading is near zero, the indicator disappears and both resources go
unavailable.

Things worth knowing before trusting the number:

- **Fit a capacitor.** Two megohms is the right choice for a divider that is
  never switched off — it draws 2µA — but 500k of source impedance cannot charge
  the ADC's sampling capacitor in time, and the pin reads low and jittery on its
  own. 100nF from GPIO 35 to ground fixes it.
- **Trim it once.** `BAT_DIVIDER_MILLI` in
  [lib/boardstuff/boardstuff.h](lib/boardstuff/boardstuff.h) is the divider
  ratio in thousandths, 2000 for equal resistors. Resistor tolerance and the
  ADC's own error are both a few percent; compare `BATTERY` on the console with
  a multimeter on the cell and scale the number to match.
- **The percentage is an estimate from voltage alone.** It sags while the
  backlight is at full and recovers at rest, and it cannot tell a cell on charge
  from a full one — nothing on the board reports that a charger is attached.

## Screens

A top bar on every screen: a back arrow (not on the menu), the page's title, then
clock, battery, Wi-Fi and link state. There is no bottom bar; 320x452 is left for
the page.

**Nothing scrolls if it can help it.** A scroll redraws the whole 320x480 screen
every frame, and sending that over SPI takes about 60 ms at 40 MHz, so scrolling
can never be smooth on this panel. The menu and settings are *pages*: swipe
left or right, with dots at the bottom showing where you are. A page flip draws
once. (LVGL's render buffer is also double, so drawing overlaps the transfer.)
Devices and a device's resources are paged lists (Prev/Next or swipe).

**Menu** (home, and where waking from rest lands). Big tiles, six to a page. A
tile is a control that shows its own state, or a door to another page.

| Tile | Tap | Hold |
| ---- | --- | ---- |
| Light, AC, Bed | toggle on/off; lit when on, with state and temperature (Bed is the RGB light, Sherlock, and shows its temperature top-right) | the full controls |
| PC | monitors off | sleep the PC |
| Rest / Sleep (page 2) | go to that power stage | |
| Controls | the full controls: AC setpoint and sync, weather, light, PC, RGB colour | |
| Devices | the device list | |
| Battery, Settings (page 1), Update (page 2) | the matching settings page | |

An unbound or silent tile is dimmed and says so; tapping an unbound one opens
Devices, where it is bound.

**Settings** pages, each one subject: Display (brightness, the two timeouts, Rest
now / Sleep now), Power (battery and the three stages), Network, Update (firmware
from the PC) and System (uptime, memory, reboot).

Two implementation notes for anyone extending this. Greying is done with
explicit colours, not `LV_STYLE_OPA`: any opacity below 255 makes LVGL render
that object into an intermediate layer, asking for up to `LV_LAYER_SIMPLE_BUF_SIZE`
(24KB) on *every redraw*, which this board cannot spare. And LVGL still delivers
clicks to a button in `LV_STATE_DISABLED`, so handlers re-check liveness
themselves. A widget that is dragged sideways (a slider) must call
`Pager_noSwipe`, or the swipe at the end of the drag also turns the page.

| Screen       | What it does                                                                 |
| ------------ | ---------------------------------------------------------------------------- |
| **Menu**     | Tiles, above. |
| **Controls** | The old home page, reached by holding a light, AC or RGB tile or the Controls tile: the AC with Auto, sleep and adjust (setpoint, **Sync with unit**), weather, light, PC and RGB colour. It still scrolls. |
| **Devices**  | Everything heard announcing itself on `<device>/status`, seven to a page, with online state and age. Tap a row to open the device. |
| **Device**   | Header, then: bind the device as the **AC**, the **PC** link or the **Bed** light (the RGB role) (each a lit toggle; a device role is the contract's own resource names, so the RGB one uses `rgb`); **Resources**; reboot or delete. Under that, Boot / Hardware / System, with the device's answer filling the rest of the page. Opening a device starts receiving its resources. |
| **Resources**| One device's resources, six to a page, with a badge on anything already in use. Tap a row for that resource's page. |
| **Resource** | Two swipeable pages. **Value**: the reading, large, with its device, topic and age (a document's fields listed below). **Use this as**: big buttons for the light switch, the resting screen's resource and the door sensor (with which of 1/0 means open), plus Stop using this. The AC, PC link and RGB are device roles, set on the device page. |
| **Settings** | Pages, above. |
| **Resting**  | After the idle timeout or Rest now: a dimmed logo and a 120px clock, hours above minutes, with the corners carrying what matters. Any touch wakes it, onto the first menu page. |

### What the panel keeps of the network

Only **devices** are tracked all the time: the panel subscribes to `+/status` and
nothing else network-wide, so the registry holds up to 32 devices and nothing
else a device publishes. Everything else is fetched when it is wanted:

- **The open device's resources**: opening a device page subscribes to
  `<device>/resource/+/state` (the gateway replays the retained values), and
  leaving it unsubscribes and drops them. One device at a time.
- **What a role uses**: the AC, light, RGB and PC are subscribed to by their own
  bindings, and the door and rest sensors by `Net_loop`; those stay.

This replaced a standing `+/resource/+/state` subscription. Receiving every
resource of every device filled the inbox in a burst, dropped frames without any
priority (a dropped retained status is a device that never appears), and cost
radio time on a battery. The inbox is 8KB, down from 12KB.

Still scrolling, and the next candidate for pages: Controls.

## Build and flash

Native ESP-IDF, built through PlatformIO. There is no Arduino core in this
firmware.

```sh
pio run                 # build
pio run -t upload       # flash over USB
pio device monitor      # serial console, 115200
```

Kconfig choices are in [sdkconfig.defaults](sdkconfig.defaults), which is
source and says why each one is there. PlatformIO generates
`sdkconfig.esp32-3248S035C` from it; that file is not source and is gitignored.
Delete it if a change to the defaults does not seem to take.

**Coming from the Arduino build.** Flash over USB once; the two are not
OTA-compatible in any way that has been tried. Expect the stored settings to be
gone on the first boot: the settings partition is now mounted as SPIFFS, which
formats a partition it cannot read. That means the device name, the stored
Wi-Fi profile, every binding and the display settings go back to their
defaults, and the panel joins with the `creds.h` fallback profile. The Arduino
tree as it last built is kept at the git ref `refs/snapshots/pre-idf-port`.

First build only:

```sh
cp include/creds.example.h include/creds.h   # then fill it in
```

`include/creds.h` holds the Wi-Fi fallback profile and is gitignored. ESP-NOW
uses gateway discovery and needs no broker credentials or certificate.
`include/NightMareConfig.h` selects the library's features and is not secret —
it is where the timezone, the log level and the threading choices live.

### Updating over Wi-Fi

After one USB flash with this build (rollback needs its bootloader), later
builds go on from the PC. The PC pushes the image straight to the panel; Dash-P4
is not involved and nothing is fetched from a web server.

1. Put a long random `DASHLINK_TOKEN` in `include/creds.h` (see
   `creds.example.h`) and flash. Without it update mode refuses to open.
2. On the panel: Settings > Firmware update > Update from PC (or `OTA START` on
   the console). It shows `ip:7340`. Update mode closes by itself after two
   minutes without a PC.
3. On the PC, from `devices/Dash-P4/pc`:

```sh
set DASHLINK_TOKEN=<the same token>
python -m dashlink push-fw --target dashboard --host <panel ip>
```

It sends `.pio/build/esp32-3248S035C/firmware.bin` (or `--file`). The panel
checks the sha256, the image and the slot, restarts, and the command reads the
build identity from the new session to say whether it is running or was rolled
back. The protocol is DashLink's, see `Dash-P4/pc/PROTOCOL.md`.

What it will not do: update below 30% battery (a panel with no cell fitted is
allowed), rest into sleep while update mode is open, or open without a token.
A new image boots on probation and is marked good after 20 s of a running main
loop; one that resets before that is replaced by the previous image by the
bootloader. The panel also listens for the first minute after such a boot,
only so the PC can make that check. The link is not encrypted (DashLink v0):
the token keeps strangers on the LAN out, it does not hide the image.

## What the panel says about itself

The panel is an ordinary NightMare device, so it publishes the same retained
documents it reads from everything else: `<panel>/status`, `<panel>/info`, and
`<panel>/telemetry/{system,network}`. Its hardware topology is retained as
`<panel>/hardware` and `<panel>/hardware/msgpack`; `HW` returns the JSON form
over the console and can republish either encoding.

[include/NightMareHardware.h](include/NightMareHardware.h) is what makes
`hardware.board` read `sunton-esp32-3248s035c:v1` and the topology lists all
nineteen pins — the board's eighteen and the battery divider on GPIO 35 —
instead of `unspecified` / `none`. Connections reference compact
device and bus indexes and carry signal type, direction, pull, and active-low
state. Pin-specific operating details remain in the source comments rather
than being sent as rendering metadata.

Those numbers are **copied** from `lib/boardstuff/boardstuff.h` rather than
included from it, and the two have to be changed together. The header is read
by the *library's* build, and `boardstuff.h` pulls in LVGL and Wire — including
it would drag the whole display stack into NightMareNetwork's translation units
to fetch a dozen integers. The board is fixed hardware and these numbers change
about never; that is the trade, and it is written at the top of the file.

This list used to be expensive: `INFO` sized its JSON pool as
`2048 + connections * 256`, so eighteen pins meant a single contiguous ~6.4KB
allocation on every MQTT connect. Under ArduinoJson 7 the document grows
through a chain of small pools instead, so the length of this list no longer
decides the size of any one allocation.

## How it talks to the network

Every capability on a NightMare network has an address of the form

```
<device>/resource/<resource>
```

with retained manifests at `<device>/manifest` and
`<device>/manifest/msgpack`, plus resource topics under
`<device>/resource/<resource>`:
retained value at `…/state`, and the transient `…/set` and `…/invoke`. The
governing rule is **a manifest describes, `/state` tells the truth** — a missing
or disagreeing manifest never invalidates a value.

The panel uses that in two different ways, and the split is the shape of the
whole application.

### What it drives

Everything the panel controls is a **bound Remote resource**. The library owns
the subscription, the decoding, the freshness and the optimistic write; the
panel only says what each one is pointed at.

- [src/App/AcClient.cpp](src/App/AcClient.cpp) — the air conditioner's nine
  resources.
- [src/App/LightControl.cpp](src/App/LightControl.cpp) — the light and the
  colour.

Each is declared with no source and pointed at whatever is bound by
`…_applyTargets()`, called from `Net_loop()` when a binding changes. Retargeting
is a `setSource()`, not a rebuild: the library unsubscribes the old address,
drops everything it learned from it, subscribes the new one, and the broker
replays that device's retained state on its own.

Writes are **optimistic**. Tapping power flips the card immediately; the owner's
retained state wins again when it arrives, or when the ~5s window closes. So a
control that appears to work and then reverts means the command did not take,
not that the panel dropped it.

### What it watches

The library no longer subscribes to `#`, so the Devices and Resources screens
are fed by three explicit subscriptions installed in
[src/App/Net.cpp](src/App/Net.cpp). They are remembered in RAM and reinstalled
on every reconnect.

| Subscription            | Goes to                                                       |
| ----------------------- | ------------------------------------------------------------- |
| `+/status`              | Device registry. Retained JSON: `{"name":…,"hardware":…,"online":true}`. An empty payload is a tombstone — it takes a device offline but never creates one. |
| `+/resource/+/state`    | Resource registry, one entry per `(device, resource)`, holding the codec representation exactly as published. |
| `Control/forecast`      | The weather model.                                             |

Anything the resource manager recognises as belonging to a *bound* resource is
consumed before it reaches the panel's own callback — which is why
`LightControl_mirrorToRegistry()` copies the bound light and colour back into
the registry once a second. Without it a light would vanish from the Resources
screen the moment it was given a job, taking the only way to unbind it with it.
The AC's resources are deliberately *not* mirrored: they are all on the Home
card already, and eighteen of them would crowd every other device out of a
40-entry table.

`Control/time` never reaches the panel's code at all — the library consumes it
inside the MQTT client and sets the clock — and time sync needs no request job
of its own, because the library runs SNTP and asks on `Control/request` whenever
its clock is invalid.

## Binding

The panel starts bound to nothing and says so on each card. The two registers
are bound in different places, because they are different shapes of thing.

**The AC is bound to a device**, on the **Devices → Device** page. Its nine
resources are a contract the controller publishes under names it chose, so
naming the device names all of them:

| Resource            | Type          | What the panel does with it |
| ------------------- | ------------- | --------------------------- |
| `ac_state`          | int8 sensor   | the state chip, and the card's freshness |
| `ac_known`          | bool sensor   | shows "unknown" when the controller does not believe itself |
| `ac_sleep_deadline` | uint32 sensor | the sleep countdown, as an epoch second |
| `temperature`       | float sensor  | the room reading it controls against |
| `ac_power`          | bool state    | the card, and tapping it |
| `ac_temperature`    | uint8 state   | the unit's own setpoint, 18–30 |
| `ac_target`         | float state   | the room target; negative disables the thermostat |
| `ac_manual_sync`    | action        | `{"power":bool,"temperature":int}` |
| `ac_sleep`          | action        | `{"minutes":int}`, 0 cancels |

**The PC is bound to a device too**, on the same page, and it is not the PC:
it is the panel that is linked to the PC. This panel has no link of its own.
Dash-P4 does, and offers it to the network as three resources, which
[src/App/PcClient.cpp](src/App/PcClient.cpp) is the other end of:

| Resource          | Type        | What the panel does with it |
| ----------------- | ----------- | --------------------------- |
| `pc_linked`       | bool sensor | whether the PC card's buttons are live |
| `pc_monitors_off` | action      | no arguments; **Monitors off** |
| `pc_sleep`        | action      | no arguments; **Sleep**, after a confirming tap |

An invoke has no reply, so the button shows "Sent" for a moment and nothing
more. After a sleep, `pc_linked` going false a few seconds later is the
confirmation. Nothing on either panel can wake the PC again.

**The light and the colour are bound to one resource each**, from a resource's
own page — **Resources → tap a row**. Nothing in the protocol says a light must
be called `light`, so rather than assuming, the panel shows what is actually
being published and asks what it is for. The door sensor and the resting
screen's free slot are chosen the same way. That page also offers the AC, as a
shortcut for someone standing on `ac_power`; it binds the device, and the button
says so.

**A value that is a JSON document reads as `struct` in the list**, and only
there. A status document clipped to a 90px column shows something like
`{"state":1,"tar`, which is worse than useless — the type is the honest summary,
and the resource page flattens the whole thing to `key  value` lines one tap
away. That test is the *shape* of the payload, not the protocol's `struct` value
type: the panel never reads manifests, so it has no declared type to go on. A
device publishing JSON under a `string` type — which is what an AC controller's
`ac_status` is — reads as a struct here, and for the question being asked ("will
this fit in a row") that is the right answer.

Roles are **not exclusive by device**. One board can be the air conditioner and
own the bound colour as well — that is a real device, not a mistake. The only
thing that cannot be shared is a single `(device, resource)` address, which the
resource manager refuses on its own.

Bindings persist in the NightMare settings store (`ac_host`, `pc_host`,
`light_host` / `light_res`, `rgb_host` / `rgb_res`), so they survive a reboot but not a
filesystem wipe. They can also be set from the console — over serial, or over
MQTT on `<panel>/console/in`:

```
TARGET                          # show current bindings as JSON
TARGET AC Adler                 # bind the AC role to the device "Adler"
TARGET LIGHT Mycroft light      # bind the light role to Mycroft's "light"
TARGET RGB Sherlock             # bind the colour role, default resource name
TARGET PC Dash-P4               # bind the PC role to the panel linked to the PC
TARGET LIGHT ""                 # unbind
FORECAST                        # request a forecast now
HEAP                            # free / min-ever-free / largest free block
BATTERY                         # cell present, millivolts, percent
SLEEP                           # sleep now; a touch wakes it
```

Names are **case-sensitive** — `Micro` and `micro` are different devices.

A `TARGET` that changes something is *staged*, not applied: a command arriving
over MQTT runs on the MQTT client's task, and the bindings belong to `loop()`.
`Net_loop()` picks it up within a few milliseconds, so the effect is immediate,
but the reply reports what was asked for rather than what is bound.

The framework's own built-ins (`PING`, `INFO`, `TIME`, `JOB`, `MQTT`, `WIFI`,
`CONFIG`, `REBOOT`) and the `>` resource-command grammar work on this panel too:
`>list` names every resource it has bound, and `> ac_power set true` drives one
directly.

### Brightness, out of one number

A colour resource is a single 24-bit value, so "off", "which colour" and "how
bright" all have to live in it. The panel reads them back out as: **off** is a
published 0, **brightness** is the largest of the three channels, and **colour**
is the value scaled back up so that channel reads 255. The hue and level last
chosen are kept on the panel as well, so switching off and on again restores
what it was, and the colour wheel opens on the colour that was picked rather
than on a dimmed version of it.

### Exploring a device

The detail page can ask a device about itself over MQTTP — the same
request/response-over-MQTT protocol the backend uses:

```
request  ->  <Device>/console/controlled/<id>/in     the command text
reply    <-  <Device>/console/controlled/<id>/out
```

Three commands are offered — `INFO BOOT`, `INFO HARDWARE`, `INFO SYSTEM` —
because those are sections of the framework's own telemetry document and every
NightMare device answers them without a project resolver. Nothing
device-specific is offered: a command implemented only in one firmware's local
resolver would simply time out everywhere else.

**The panel is a viewer, not a store.** That is the whole design. The backend
persists every answer from every device forever in Postgres; the panel holds one
answer, for one device, for as long as the page is open. Leaving frees it and
reopening asks again, which costs nothing because the data is live anyway.
Concretely, in [src/App/Mqttp.cpp](src/App/Mqttp.cpp):

- **One request in flight** — a single slot, not a map. One screen, one user,
  one question at a time, so memory is bounded by construction.
- **A fixed `.bss` buffer**, never grown, never freed. Reassembling chunks into
  a growing `String` is exactly the heap fragmentation that breaks the next
  mbedTLS handshake on this board — see the heap note below.
- **Truncate, don't fail** at `MQTTP_RESPONSE_MAX`. A partial answer with a
  marker beats an error.
- **8s timeout**, not the backend's 20s: someone is standing in front of it.

**Listening is per device and per page, not per request.** The library no longer
takes `#`, so replies have to be subscribed for — but subscribing *per request*
does not work, and this is worth knowing before anyone tries it again:
`esp_mqtt_client_subscribe()` only queues a SUBSCRIBE and returns, and at QoS 0
a device answering `INFO BOOT` in about a millisecond beats the broker's SUBACK
over a TLS link every time. The reply is dropped before anyone is listening for
it, and every query times out.

So `Mqttp_listenTo()` takes `<device>/console/controlled/+/out` when the device
page opens and gives it back when the page is left. The wildcard is one device
wide, the SUBACK has the time between opening a page and tapping a button, and
the request id still does the correlating. Only the synchronous variant is
implemented, not `asynccontrolled`.

Replies of 512 bytes or less arrive whole; larger ones are chunked by the sender
as `;;<n>/<total>;;<data>` and reassembled. Note that `MQTT_Send` drops empty
payloads, so a command that returns nothing is indistinguishable from a timeout.

### Rebooting and deleting a device

The same dialog offers one action on the device itself, chosen by whether it is
answering — the two are never both useful, so only one is shown:

- **Online → Reboot.** Publishes `REBOOT` to `<device>/console/in`.
- **Offline → Delete.** Forgets it locally, unbinds every role it held, and
  clears the retained announcements with zero-length retained publishes: its
  `status`, its `resources` manifest, and the `state` of every resource the
  panel has seen it publish. Any one of those left retained re-announces the
  device on the next resubscribe.

Both need a second tap to confirm. Delete does not decommission anything: a
device that is still running reappears on its next message. It clears the
leftovers of something already gone. A resource the panel never heard cannot be
tombstoned — the same limitation the library's own identity cleanup has, for the
same reason.

### The AC's door and unit sync

**Door.** Any boolean resource can be the door sensor: tap its row on Resources
and choose whether a reading of 1/true means *open* or *closed* — reed switches
report either, depending only on wiring. Only `true`/`false`/`1`/`0` are
understood; anything else shows as "no reading" rather than guessing. This is
independent of the AC device's own door logic, which still drives the
"paused - door" state. The binding is stored under `door_device` / `door_key`,
names kept from before the resource protocol so an already-deployed panel does
not lose them on the upgrade. The AC controller uses the same two names for its
own door reference, but in its own settings store on its own device — setting
one here does not set the other.

**Sync with unit.** Not a command to the AC. It tells the controller what the
physical unit is *already* doing, after it was changed behind the controller's
back — usually with the unit's own IR remote. Set the temperature and on/off the
unit's display shows, then Sync; the panel invokes `ac_manual_sync`. No
confirmation, because it corrects a belief rather than changing the room.

**Sleep is real now.** The `🔔` button offers Off / 30 min / 1 / 2 / 4 hours and
invokes `ac_sleep`; the controller owns the timer, counts it down itself and
publishes the deadline on `ac_sleep_deadline`, which is what the card displays.
The panel need not be awake for the unit to switch off. (This replaces a mock
that was kept and counted down on the panel only.)

## Other config keys

| Key          | Default | Meaning                                  |
| ------------ | ------- | ---------------------------------------- |
| `rest_after` | `60`    | Idle seconds before the resting screen. `0` disables it. |
| `sleep_after`| `300`   | Seconds on the resting screen before the panel sleeps. `0` disables it. |
| `bl_active`  | `200`   | Backlight level while in use, 8–255.     |
| `bl_rest`    | `24`    | Backlight level while resting, 0–255.    |
| `door_device` / `door_key` | — | Device and resource publishing the door state. Set from the Resources page. |
| `door_invert`| `0`     | `1` when a reading of 1/true means *closed* rather than open. |
| `rest_device` / `rest_key` | — | The resource shown in the resting screen's free corner. |
| `ac_timezone`| —       | POSIX TZ string. Overrides `NM_TIMEZONE` at runtime. |

The display keys are editable on the **Setup** page, which applies them
immediately. They can also be set over the console — `CONFIG SET rest_after 120`
— but the screen manager caches them, so a console change takes effect on the
next reboot unless something calls `Ui_reloadSettings()`.

The connection is fixed to ESP-NOW in this build. Setup reports gateway state;
there is no broker selector.

## Backend: the forecast reply is not implemented yet

Time sync works today, and no longer needs the backend at all: the library runs
SNTP, and `Control/request` = `time` is only a fallback.

**Weather does not**: the panel publishes `Control/request` = `forecast` and
nothing replies, so the weather card stays on "waiting for forecast".

To finish it, add a `forecast` branch alongside the existing `time` one in
`backend/src/app/Services/ControlService.ts`, publishing to `Control/forecast`:

```json
{ "icon": "partly-cloudy-day", "temp": 24.1, "feels": 25.0,
  "min": 19.2, "max": 29.8, "rain": 20, "condition": "Partly cloudy" }
```

`icon` is a [Visual Crossing](https://www.visualcrossing.com/resources/documentation/weather-api/defining-icon-set-in-the-weather-api/)
icon name, mapped to the bundled amCharts art by `visualCrossingToId()` in
`include/Weather icons/icons/convert.h`. Every field is optional, and the longer
spellings `feelslike`, `tempmin`, `tempmax`, `precipprob`, `conditions` are
accepted too — so a Visual Crossing `currentConditions` object can be forwarded
more or less unchanged.

## Layout

```
src/
  main.cpp              app_main, startup, the loop, and the TARGET/FORECAST/HEAP/BATTERY/SLEEP commands
  Sys.h                 The clock, the sleep and the string helpers every file uses
  App/
    Net.cpp             The wildcard subscriptions, the inbox, message routing, radio suspend/resume
    Battery.cpp         The cell: voltage, percentage, and the two resources that publish them
    AcClient.cpp        The air conditioner's resource contract
    LightControl.cpp    The light and the colour, and the one-number colour model
    PcClient.cpp        The PC's monitors-off and sleep, through the panel linked to it
    Targets.cpp         Which (device, resource) fills which role, persisted
    Registry.cpp        Bounded snapshot of devices and resource values seen
    Mqttp.cpp           Correlated console request/response, one at a time
    SensorBindings.cpp  The door and resting-screen resources
    Forecast.cpp        Weather model; the only place the icon set is included
  UI/
    Ui.cpp              Screen manager, nav bar, the active/resting/asleep stages
    Theme.cpp           Colours, shared styles, widget builders
    Screen*.cpp         One file per screen; ScreenResource.cpp is the binder
    Assets/logo_mw.c    Generated by tools/png2lvgl.py
    Assets/font_clock_120.c  Montserrat 120px digits (SIL OFL), lv_font_conv; command in its header
include/
  NightMareConfig.h     Library feature selection; read this before changing behaviour
  NightMareHardware.h   The board and its pins, as published in <device>/info
  creds.h               Wi-Fi fallback profile; gitignored
lib/                    Vendored libraries -- see lib/README.md
tools/png2lvgl.py       PNG to LVGL C array converter
```

Screens build their widgets once and update them in place; nothing is created or
destroyed while the panel runs, so the heap does not fragment over long uptimes.

## Things to know

The transport and memory notes below describe the current ESP-NOW build unless
explicitly marked historical. MQTT/TLS tuning is no longer active.

- **The panel is single-tasked, and two settings keep it that way.**
  ESP-NOW delivers on the Wi-Fi event task. The bridge in
  [src/App/Net.cpp](src/App/Net.cpp) only copies frames into a fixed-size ring
  buffer; `Net_loop()` runs framework routing and dashboard routing. The queue
  drops rather than blocks the event task; `Net_droppedMessages()` (and the
  `HEAP` command) should stay at 0.

  `NM_SCHEDULER_OWN_TASK 0` in `include/NightMareConfig.h` is the other half:
  with the default of 1 the Scheduler gets its own FreeRTOS task and every job
  callback — the forecast refresh, the heap log — would run in parallel with the
  screen it is updating. At 0, `tickNightMareESP()` runs them from `loop()`.

  Two things deliberately escape the rule, and both are safe for a stated
  reason. The library decodes bound-resource state on the MQTT task and stores
  it in the resource object — which is why every value the panel binds is a
  **scalar**: a bool, an int, a float, a 32-bit colour, so a read from `loop()`
  is a single aligned load and cannot tear. (`AcClient` deliberately does not
  consume the controller's `ac_status` String document for exactly this reason.)
  And a console command arriving over MQTT runs on the MQTT task, so a `TARGET`
  that changes a binding is staged through `Targets_request()` and applied by
  `Net_loop()` instead of being written there.

- **An LVGL out-of-memory restarts the panel; it does not freeze it.** LVGL's
  default assert handler is `while(1);`, which hung the `loop()` task — touch
  and Serial dead while the MQTT tasks kept logging. `LV_ASSERT_HANDLER` in
  `lib/lv_conf.h` now calls `lvgl_assert_failed()` (in `lib/boardstuff`), which
  prints `[lvgl] assert failed: free=… largest=… min=…` and restarts. The malloc
  assert stays enabled on purpose: about a quarter of LVGL's call sites do not
  check for NULL afterwards, so without it a failed allocation crashes somewhere
  arbitrary instead.

- **Heap is the tight resource, not flash.** No PSRAM, so LVGL widgets, the
  render buffer, the WiFi driver and mbedTLS all share ~290KB. mbedTLS needs
  tens of KB *contiguous* for each MQTT handshake, and when it cannot get it the
  IDF client does not fail cleanly — `esp_mqtt_client_init()` returns a
  half-built handle and the first publish panics the core into a boot loop.

  Three things keep that from happening, and all three are load-bearing:
  device rows are built **lazily**, while Resources uses six reusable rows and
  pages through the registry (neither allocates below the `Ui_canAllocateRow()`
  floor); the LVGL render buffer is small and deliberately **not**
  DMA-capable, since the DMA pool is what the WiFi driver draws from; and
  `startNightMareESP()` runs from `loop()` behind a heap check rather than from
  `setup()`.

  That last one moved. The library now brings MQTT up from inside the WiFi task
  on the first connection, so the panel can no longer guard the handshake
  directly — the last point it still controls is whether WiFi starts at all, and
  that is what `tryStartNetwork()` in [src/main.cpp](src/main.cpp) gates. It is
  a weaker guarantee, because association itself costs heap in between, but it
  is still the difference between a red MQTT icon and a reboot loop.

  Dialogs are built at boot, never on first tap: built on demand they ask for
  memory at whatever moment the tap lands, including mid-TLS-reconnect, and a
  failed allocation inside LVGL 8 crashes instead of failing. List rows are the
  opposite — built on demand and freed when their page is left or the panel
  rests. The Resources list never holds more than one six-row page, regardless
  of how many entries the registry contains.

  `HEAP` on the console reports free, minimum-ever-free, largest free block and
  dropped messages. Largest block matters as much as the total: a fragmented
  heap fails the handshake while looking comfortable — 91KB free with a 37KB
  largest block is a heap that cannot reconnect.

  The serial log samples it at `boot`, `after lvgl` and `after ui`, then at
  `mqtt up` and `after burst`, then `periodic` every 30 s. The middle pair
  brackets the worst moment on purpose, because the 30 s sample is far too
  coarse to catch it: between those two readings the broker replays every
  retained status and resource state on the network at once, and the library
  publishes this device's status, manifest, `/info` and both telemetry
  documents back to back, all while mbedTLS still holds the two 16KB record
  buffers from the handshake. An `esp_tls_conn_write error, errno=11` (EAGAIN,
  which the ESP-IDF strerror renders as the misleading "No more processes")
  followed by `mqtt_client: Error to resend data` is what running out of room
  there looks like from the outside: lwIP could not get a send buffer.

  **The numbers here are `MALLOC_CAP_8BIT`, not `MALLOC_CAP_INTERNAL`** — see
  [src/App/Heap.h](src/App/Heap.h). The Arduino accessors (`ESP.getFreeHeap()`)
  reported
  `MALLOC_CAP_INTERNAL`, which counts IRAM that no byte-buffer `malloc` can
  ever hand out; on this board that reads about 37KB high and its
  largest-free-block never moves off 36852. Every gate that used to read them
  was measuring the wrong pool, and `Ui_canAllocateRow()` was returning true
  unconditionally as a result. The `(internal=…)` figure is still printed for
  comparison, because it is what every other ESP32 tool shows and the gap
  between the two is the point.

- **No OTA.** The firmware is ~2.1MB and a 4MB flash cannot hold two OTA slots
  that size — the partition table ([src/OneAppNoOta.csv](src/OneAppNoOta.csv))
  is a single ~3.75MB app. Flashing is over USB. Fitting OTA would mean cutting
  roughly 250KB, most easily from the 21 bundled weather icons (~12KB each) or
  the 180px logo (~97KB).

  `NM_ENABLE_OTA` is therefore 0 in `include/NightMareConfig.h`. Left on, it
  still starts ArduinoOTA on first connect — a task, an mDNS responder and a UDP
  listener, held forever — and that heap is what the HiveMQ TLS handshake runs
  short of. Turn it back on only together with an OTA-capable partition table.

- **The HiveMQ TLS connection is the single largest memory cost.** The prebuilt
  Arduino libraries fix mbedTLS at `SSL_MAX_CONTENT_LEN 16384` with dynamic
  buffers off and `KEEP_PEER_CERTIFICATE` on: every connection holds a 16KB input
  and a 16KB output record buffer (each one contiguous block) plus the broker's
  certificate chain, and a handshake needs roughly 50–60KB. A handshake that
  cannot get it fails with `esp-tls 0x801A` / `tls stack 0x2880`
  (`MBEDTLS_ERR_X509_ALLOC_FAILED`). None of it can be changed from
  `platformio.ini`. That was the Arduino build, and it is history twice over:
  MQTT is compiled out, and on native ESP-IDF the mbedTLS settings are ordinary
  Kconfig options that `sdkconfig.defaults` could set if TLS ever comes back.

- **What moved in the ESP-IDF port.** Worth knowing when reading older notes in
  this file, most of which were written against the Arduino build:
  - There is no `setup()`/`loop()` pair supplied by a framework. `app_main()`
    in [src/main.cpp](src/main.cpp) calls the project's own `setup()` once and
    `loop()` forever, on the main task, pinned to the second core with a 12KB
    stack. Everything said here about "the `loop()` task" still holds; it is
    that task.
  - Strings are `std::string`. The helpers the Arduino `String` had as
    methods (`trim`, `toLower`, `slice`, `replaceAll`, `toLong` ...) are the
    library's free functions, reached as `Text::` through
    [src/Sys.h](src/Sys.h), which also has the millisecond clock and the sleep.
    One real difference: a `std::string` cannot be built from a null pointer,
    so a JSON value that might not be a string is checked before it is used.
  - The display is driven through esp_lcd's SPI panel-IO with DMA, and the
    transfer is asynchronous: LVGL gets its buffer back from the SPI interrupt.
    TFT_eSPI is gone. The ST7796 init sequence and rotation table are the ones
    it used, kept verbatim in
    [lib/boardstuff/boardstuff.cpp](lib/boardstuff/boardstuff.cpp).
  - Only the IDF components `src/` depends on are built (`set(COMPONENTS ...)`
    in [CMakeLists.txt](CMakeLists.txt)). Besides the build time, that is what
    keeps the linker-script step inside the Windows command-line limit, so a
    new `REQUIRES` in `src/CMakeLists.txt` is how a component is added.
  - None of this has run on the board. It compiles and links; the display
    bring-up, touch, ADC and sleep are all new code against new drivers.

- **If the display or touch is wrong on real hardware**, these are the knobs —
  none of it could be verified without the board in hand:
  - Colours inverted → send the inversion-on command (`0x21`) at the end of
    `displayBegin()` in `lib/boardstuff/boardstuff.cpp`.
  - Red and blue swapped → drop `MadBgr` from the `MemoryAccess` constants in
    the same file.
  - Tearing or garbage at speed → lower `ST7796_SPI_FREQ` in
    `lib/boardstuff/boardstuff.h` from 40MHz.
  - Image upside down → in `platformio.ini`, swap `-D TFT_ORIENTATION_PORTRAIT`
    for `-D TFT_ORIENTATION_PORTRAIT_INV` (rotation 0 ↔ 2, same axis, 180°
    apart). Which one is right depends only on which way up the enclosure
    holds the board. Touch is derived from the same rotation, so it follows
    automatically — there is no second place to edit.
  - Image right but touch wrong → the two have genuinely drifted, and the
    rotation-0/2 branches of `gt911_read_touches()` in
    [lib/boardstuff/boardstuff.cpp](lib/boardstuff/boardstuff.cpp) are where to
    look. Portrait rotation 2 was verified on hardware; rotation 0 is the
    controller's native frame, passed through untouched.
  - The layout is portrait-only. The landscape macros still rotate the panel
    and its touch correctly, but the screens are no longer laid out for 480×320.
  - Anything laying out pixels must use `SCREEN_WIDTH`/`SCREEN_HEIGHT`, never
    `TFT_WIDTH`/`TFT_HEIGHT`: those stay the panel's native portrait size and
    are what the touch transform is expressed in.

## License

This project is MIT licensed — see [LICENSE](LICENSE).

It also carries vendored third-party work, each under its own terms. Both are
redistributed here with their licence files intact, and one of them has a
condition you must keep:

| What | Where | Licence |
| --- | --- | --- |
| LVGL 8.3.6 | [lib/lvgl](lib/lvgl) | MIT, © LVGL Kft — [LICENCE.txt](lib/lvgl/LICENCE.txt) |
| Weather icons | [include/Weather icons](include/Weather%20icons) | **CC BY 4.0**, © [amCharts](https://www.amcharts.com/) — [LICENSE](include/Weather%20icons/LICENSE) |
| Font Awesome 5 webfont | `include/Weather icons/animated/c/` | Font: SIL OFL 1.1; icons: CC BY 4.0, © Fonticons, Inc. |

**The icons require attribution.** CC BY 4.0 is not public domain: anything that
ships these icons — this firmware, a fork, a photo of the panel used
commercially — has to credit amCharts. That obligation travels with the
artwork and is not waived by this project's MIT licence, which covers the code
only.

LVGL is vendored rather than pulled as a dependency because
[lib/lv_conf.h](lib/lv_conf.h) is written against that exact version; see
[lib/README.md](lib/README.md).
