> **A note from esp32-matter (this repo) before Espressif's own README below:**
>
> This is Espressif's own reference BLE Mesh Bridge example from the
> esp-matter SDK (`$ESP_MATTER_PATH/examples/bridge_apps/blemesh_bridge`),
> reproduced here verbatim (Public Domain/CC0, per its own file headers)
> rather than rewritten — this is the same "port a real, working reference
> rather than guess the integration shape" principle already used
> elsewhere in this repo (SM2335EGH, APA102, OpenTherm in
> `firmware/thermostat/`, and firmware/camera/ itself at a much larger
> scale), applied here to a genuinely new category for this repo: a
> Matter **bridge** (Aggregator device type) — one node dynamically
> exposing OTHER, non-Matter devices as real Matter endpoints, created
> and removed at runtime rather than fixed at build time the way every
> other device type in this repo is.
>
> **What this actually bridges, and what it doesn't:**
>
> This bridges real, provisioned **BLE Mesh** nodes (the standardized,
> provisioned mesh protocol) — specifically, in Espressif's own reference
> flow, a BLE Mesh Generic OnOff Server node — onto the Matter fabric as
> a dynamically-created On/Off Light endpoint. This is NOT the same thing
> as bridging plain BLE *advertising* peripherals (e.g. Xiaomi Mijia-
> style sensors that just broadcast data with no mesh provisioning at
> all) — that would be a genuinely different integration, not attempted
> here. `create_bridge_devices()` (the callback esp-matter's own bridge
> infrastructure calls to add Matter clusters onto each newly-created
> bridged endpoint) already maps five real device types (On/Off Light,
> Dimmable Light, Color Temperature Light, Extended Color Light, On/Off
> Light Switch) — reused exactly as Espressif shipped it, not narrowed or
> extended.
>
> **Why this needs its own build treatment, unlike this repo's own
> simplified CMakeLists pattern every other device type uses:**
>
> BLE Mesh and Matter's own BLE-based commissioning share the same BLE
> radio/stack — bridging BLE Mesh genuinely needs a custom Matter
> platform integration layer (`examples/common/blemesh_platform/platform/
> ESP32_custom/`, a real, complete alternate implementation of
> `PlatformManagerImpl`/`ConnectivityManagerImpl`/`BLEManagerImpl`/etc.,
> selected via `CONFIG_CHIP_ENABLE_EXTERNAL_PLATFORM`), on top of the
> generic dynamic-endpoint bridge infrastructure every bridge in this
> repo shares (`components/esp_matter_bridge` + `examples/common/
> app_bridge`). This repo's own CMakeLists.txt (which deliberately
> excludes `$ESP_MATTER_PATH/examples/common` from
> `EXTRA_COMPONENT_DIRS` — see the root `CLAUDE.md`'s own "Build" section
> for why) was never designed to support either of those, so this
> firmware keeps Espressif's own unmodified `CMakeLists.txt`/`main/
> CMakeLists.txt`/`partitions.csv`/`sdkconfig.defaults` — the exact same
> "keep the reference's own working build infrastructure, not this
> repo's simplified pattern" choice firmware/camera/'s own repository-
> layout entry already documents in full.
>
> **Build-verified for real**, not assumed: built successfully in the
> pinned `espressif/esp-matter:release-v1.6_idf_v5.5.4` Docker image for
> `esp32` (classic ESP32/WROOM-32, this repo's own default target) —
> notable since Espressif's own README below documents testing only on
> `esp32c3`; confirmed here that the reference genuinely builds clean on
> classic ESP32 too, no target-specific changes needed. **Not hardware-
> tested** — no BLE Mesh node/provisioner hardware was physically
> available when this was added, and unlike every other device type in
> this repo, verifying this one for real also needs a second, real BLE
> Mesh peripheral device (e.g. Espressif's own `onoff_server` example on
> a separate board, per this file's own "Post Commissioning Setup"
> section below) on the mesh network being bridged.
>
> **Not in the product wizard** — same reasoning as firmware/camera/:
> `tools/product-wizard/` assumes one device type = one simplified
> project = one set of GPIO `#define`s to customize; a device built from
> Espressif's own unmodified reference project, with no GPIO fields of
> its own at all (every bridged device's own hardware lives on a
> completely separate board), doesn't fit that model.

# BLE Mesh Bridge

This example demonstrates a Matter-BLE Mesh Bridge that bridges BLE Mesh devices to Matter fabric.

See the [docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html) for more information about building and flashing the firmware.

💡 Important:  `create_bridge_devices` callback can be used to add data model elements (e.g., attributes, commands, etc.) to the bridge endpoint.

## 1. Additional Environment Setup

No additional setup is required.

## 2. Post Commissioning Setup

### 2.1 Discovering BLE Mesh Devices

You can read the parts list from the Bridge to get the number of the
bridged devices.

```
descriptor read parts-list 0x7283 0x0
```

If there is no other BLE Mesh device on the BLE Mesh Network, you will get
a result with an aggregator endpoint. Example:

```
Data = [
    1,  <---------------------------- Aggregator Endpoint
],
```

There is no child endpoint for the aggregator endpoint. Read the parts list
on the aggregator endpoint, and you will get an empty result.

```
descriptor read parts-list 0x7283 1
...
Data = [

],
...
```

### 2.2 Setup BLE Mesh Node on ESP32-C3

Build and run BLE Mesh onoff_server app on another ESP32-C3 board.

```
cd ${IDF_PATH}/examples/bluetooth/esp_ble_mesh/onoff_models/onoff_server
idf.py set-target esp32c3
idf.py -p <port> build flash monitor
```

The BLE Mesh device will be provisioned by provisioner and a dynamic
endpoint will be added on the Bridge device. You can read the parts list
on Endpoint 1 again to get the dynamic endpoint ID.

```
descriptor read parts-list 0x7283 1
```

The data will now contain the information of the connected BLE Mesh devices.
Example:

```
Data = [
    2,  <---------------------------- Bridged Endpoint
],
```

It means that the BLE Mesh Node is added as Endpoint 2 on the Bridge
device. You can read the device type list on the Bridged Endpoint.

```
descriptor read device-type-list 0x7283 2
```

You will get the device types of the endpoint:

```
DeviceTypeList: 2 entries
  [1]: {
    Type: 19 <-------------------- Bridged Node device type
    Revision: 1
   }
  [2]: {
    Type: 256 <-------------------- OnOff Light device type
    Revision: 2
   }
```

You can also read the cluster servers list on the dynamic endpoint.

```
descriptor read server-list 0x7283 2
```

This will give the list of supported server clusters. Example:

```
OnDescriptorServerListListAttributeResponse: 5 entries
  [1]: 3
  [2]: 4
  [3]: 5
  [4]: 6    <------------------------ OnOff Cluster
  [5]: 57   <------------------------ Bridged Device Basic Information Cluster
```

### 2.3 Control the BLE Mesh Node with chip-tool

Now you can control the BLE Mesh Node on chip tool.

```
onoff toggle 0x7283 2
```

## 3. Device Performance

### 3.1 Memory usage

The following is the Memory and Flash Usage.

-   `Bootup` == Device just finished booting up. Device is not
    commissioned or connected to wifi yet.
-   `After Commissioning` == Device is connected to wifi and is also
    commissioned and is rebooted.
-   `After Adding a Bridged Device` == A BLE-Mesh OnOff Light is added
    on the Bridge.
-   device used: esp32c3_devkit_m
-   tested on:
    [b40bf8e3](https://github.com/espressif/esp-matter/commit/b40bf8e398161bcac515fd57eb13d14e031e3a91)
    (2022-04-17)
-   IDF: release/v5.1 [420ebd20](https://github.com/espressif/esp-idf/commit/420ebd208ae9eb71cb71ebd22742d1175f11addc)

|                         | Bootup | After Commissioning | After Adding a Bridged Device |
|:-                       |:-:     |:-:                  |:=:                            |
|**Free Internal Memory** |61KB    |58KB                 |54KB                           |

**Flash Usage**: Firmware binary size: 1.6MB

This should give you a good idea about the amount of free memory that is
available for you to run your application's code.
