# Android / DJI HIL quickstart

## What runs where

```text
DJI aircraft-side Simulator + physical remote controller
    -> OpenFly Go on Android -> pose/session messages -> UE/AirSim
    <- Android virtual camera <- rendered JPEG frames <- Gaussian scene
```

DJI Simulator is the motion authority. UE follows its state and renders the
virtual camera; it does not run a second flight dynamics model for the HIL
vehicle. Android handles control and safety actions. `run_expo_east.sh` opens
HIL mode by default. Its optional `--preview` mode uses AirSim SimpleFlight and
is not a hardware-in-the-loop session.

## 1. Prepare the devices

- Linux PC with the extracted OpenFlySplat UE runtime and an active scene.
  The Expo East package already contains one; the scene-free package requires
  `./convert_nanogs_ply.sh /path/to/model.ply --crop-mode none` first.
- Android phone running the matching OpenFly Go client: MSDK V4 for the Mini 2
  setup, or MSDK V5 for the Mini 4 Pro setup. The Android app is a separate
  companion, not included in the UE executable. Source builds require your own
  DJI application key.
- Compatible DJI aircraft and its paired remote controller. Connect the phone
  to the remote controller by USB and confirm that the app sees the aircraft.

Use a stationary bench setup with the propellers removed. Confirm that the app
reports **DJI Simulator active** before issuing any simulated takeoff/control
command. A healthy network connection alone does not mean simulation is active.

## 2. Start the UE host

If `run_expo_east.sh` is already open, continue to phone connection below; the
HIL host is already running. Otherwise, close any software-only preview and run
from `Linux/OpenFlySplatUE`:

```bash
./run_openfly_hil.sh --settings NanoGSConverter/Config/OpenFlyHil.json
```

The scene opens with the HIL monitor. Before connecting the phone, a waiting
state is expected. To show both HIL and renderer controls, add `--ui All`.

## 3. Connect the phone

Open the app's **UE HIL** panel and choose either:

| Mode | Steps |
| --- | --- |
| Android hotspot / Android 热点直连 | Enable the phone hotspot, connect the PC to it, then start HIL in the app. Discovery uses UDP; do not hard-code a hotspot gateway address. |
| Existing LAN / 已有局域网 | Join the same LAN on phone and PC, enter the PC's LAN IP as the UE address, then start HIL. |

Keep the default UDP port **30020** and image TCP port **30022** unless both
ends are configured differently. Starting HIL attempts to start/adopt the DJI
Simulator session. If this does not complete, use the app's DJI Simulator
start/status control and inspect its message. Never force a simulator restart
while its virtual aircraft is airborne.

Select **UE** in the HIL camera-source selector to display the rendered image
instead of the physical DJI camera. Check all three indicators before moving:

1. UE peer/heartbeat connected.
2. DJI Simulator active with fresh state and nonzero POSE updates.
3. TCP image connected with an advancing frame counter.

The hardware controller can then operate the simulated aircraft. Finish by
landing the virtual aircraft, stopping the simulator/HIL session in the phone,
and closing the UE application.

## 4. Network directions

| Receiver | Port | Traffic |
| --- | --- | --- |
| UE PC | UDP 30020 | Android HELLO, POSE, heartbeat and PING |
| Android phone | UDP 30021 | UE heartbeat, PONG and safety EVENT notifications |
| Android phone | TCP 30022 | UE initiates the connection and sends framed JPEG images |

Allow these directions on the trusted local network. AirSim RPC port 41451 is
separate and is not needed for phone discovery. No ADB connection is required
for the runtime data path. This HIL transport has no authentication/encryption;
do not expose it to the public Internet.

## 5. Troubleshooting

| Symptom | Check |
| --- | --- |
| Waiting for phone / 等待手机连接 | Same network, correct UE LAN IP, UDP 30020/30021, and hotspot/client isolation. |
| Image online, waiting for DJI Simulator | Network works. Check aircraft/remote USB connection and simulator state in the app. |
| Pose online, no image | Phone TCP 30022 listener, firewall and UE camera-source selection. UE connects to the phone, not the reverse. |
| Two UE windows or port-in-use error | Close the software preview or another HIL process before launching again. |
| Sky only / missing GS scene | Confirm that the package has `Content/NanoGSData/RuntimeActive/active_scene.runtime.json` and its referenced tree directory. Restart UE after conversion. |
| Spawn is inside geometry | Close UE, adjust the scene's ground/start settings and reconvert; do not resolve a bad spawn by starting a hardware control session. |

After a network timeout, the UE monitor enters a waiting state. Restore the
network and let the phone reconnect; check fresh simulator state and image
frames again before resuming.

## Scene origin and rendering

The scene converter generates a PlayerStart and a hidden floor. The runtime
uses that PlayerStart as the common AirSim/HIL origin. The floor is not a
building collision mesh: visible GS surfaces do not automatically provide
physical collision geometry. Start height is measured above estimated local
ground; do not treat the PLY origin as ground level.

The profile requests 1440x1080 JPEG, quality 92, up to 30 FPS. V4 and V5 expose
different DJI simulator state APIs; a phone POSE transmit setting is not proof
of an equally fast fresh hardware state stream. For the recorded configuration
and measured rates, see `HIL_PERFORMANCE_CN.md` in the source documentation.
