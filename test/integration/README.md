# Integration Test Plan

This document contains test cases for integration testing. In order to verify this project
correct behavior, one should perform and verify all tests described here. Please note, that
passing all this test cases does not mean that there are no bugs within the project, but it
assures that most common bluez-alsa use cases will not fail or produce undefined behavior.

## Automatic PCM test

Run `test-alsa-pcm` test with real hardware. One might do it as follows:

```sh
cd build
make check TESTS=
./test/test-alsa-pcm --pcm=<real-BT-headset> capture
./test/test-alsa-pcm --pcm=<real-BT-headset> playback
```

## Transport acquire/release

1. Start and stop playback for few times using A2DP and SCO profiles.
2. Start A2DP/SCO playback and in the middle of it disconnect the headset using e.g.
   `bluetoothctl` tool.
3. Start A2DP/SCO playback and in the middle of it power off the headset using on-device button
   or switch (if such is available).
4. Start A2DP/SCO playback and walk away with the headset until the link is lost (putting the
   Bluetooth device into a Faraday cage will also work, e.g. use microwave oven).
5. Start A2DP/SCO playback, walk away with the headset and then stop playback (before transport
   link timeout).
