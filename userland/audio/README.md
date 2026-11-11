# HakiosAudio.driver (iOS audio devices as a HAL plug-in)

`audio_plugin.c`, `audio_plugin.plist`, `hakios.audio.h`: [hakios-apollo](https://github.com/enesuzun2002/hakios-apollo)'s
AudioServerPlugIn (MIT, `LICENSE.hakios-apollo`), taken unchanged from
[ios-apollo](https://github.com/aTechGeekDev/ios-apollo) `userland/audio/` (commit 1f75f38), where it gives the Mi 10T
speakers and Bluetooth audio in iOS 16.7.16.

mediaserverd loads it from `/System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver/`. It presents four devices under
the names VirtualAudio knows an iPhone X's built-in audio by (research/audio: the configuration factory's keys):

| UID | What | Here |
|---|---|---|
| `Speaker` | stereo out 48 kHz s16 + stereo in, volume control | speaker route; samples to `/dev/apollo-audio` or `/dev/hakios-audio` if present, else dropped (silent) |
| `Hawking` | 1-ch silent input | VirtualAudio's mandatory 'vhaw' companion: without it everything stays on Null_Device |
| `Codec` | 1-ch input with data source `imic` | built-in mic port ('pmbi'); `/dev/hakios-mic` if present, else silence |
| `Actuator` | 1-ch out | Taptic Engine waveforms; `/dev/hakios-haptic` if present, else not felt; strength % in `/var/local/haptic-strength` (default 100), vibrations (>50 ms: ringtones, charger) in `/var/local/haptic-vibration-strength` (default 1, the softest) |

Replaces our kernel IOAudio2 stand-in (xnu 0226-0235/0254): boot with `sweetaudio=0` (no `sweetaudiotp/term/uid`),
or its "Codec" UID collides with the plug-in's.

Build (macOS, Xcode):

    sh userland/audio/build.sh      # -> userland/out/HakiosAudio.driver/{HakiosAudio,Info.plist}

Install on the phone (root fs is rw):

    mkdir -p /System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver
    # copy HakiosAudio (0755) and Info.plist (0644!) there, then: killall -9 mediaserverd
    # mediaserverd runs as mobile: an Info.plist left 0600 by scp makes it skip the plug-in silently

Check: `oslog -l mediaserverd` shows VirtualAudio "creating a speaker port" for UID Speaker, ports pspk / prcv? /
pmbi, and no AQMEIO "Error -1 getting output port ID".
