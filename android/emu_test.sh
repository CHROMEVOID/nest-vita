#!/bin/bash
# CI smoke test: install Nest in an Android emulator (1920x1080, like the Retroid
# Pocket 6), give it a test show and a song, walk through it with key presses and
# taps, and keep screenshots plus the log. Usage: emu_test.sh Nest.apk outdir
APK=$1
OUT=$2
mkdir -p "$OUT"
shot() { sleep "${2:-3}"; adb exec-out screencap -p > "$OUT/$1.png"; echo "shot $1"; }
key() { adb shell input keyevent "$@"; sleep 1.2; }

adb shell wm size 1920x1080
adb shell wm density 320
adb shell settings put system screen_off_timeout 1800000
adb install -r -g "$APK" || exit 1
adb shell appops set --uid com.chromevoid.nest MANAGE_EXTERNAL_STORAGE allow
adb shell appops set com.chromevoid.nest MANAGE_EXTERNAL_STORAGE allow

# a small library: one show with two episodes, one song
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=24 -f lavfi -i "sine=frequency=330:sample_rate=48000" \
  -t 40 -c:v libx264 -profile:v high -pix_fmt yuv420p -c:a aac -ac 2 -shortest -movflags +faststart ep1.mp4
cp ep1.mp4 ep2.mp4
ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=440:sample_rate=44100" -t 30 -c:a libmp3lame -b:a 192k \
  -metadata title="Test Song" -metadata artist="Nest Tester" -metadata album="Smoke Test" song.mp3
adb shell mkdir -p "/sdcard/Movies/Test\ Show\ \(2020\)/Season\ 1" /sdcard/Music/Tester
adb push ep1.mp4 "/sdcard/Movies/Test Show (2020)/Season 1/Test Show S01E01.mp4"
adb push ep2.mp4 "/sdcard/Movies/Test Show (2020)/Season 1/Test Show S01E02.mp4"
adb push song.mp3 /sdcard/Music/Tester/song.mp3

adb logcat -c
adb shell am start -W -n com.chromevoid.nest/.NestActivity
shot 01_launch 12
shot 02_after20 8
# through the first-run screens with ✕ (X key) and confirm
for i in 1 2 3 4 5 6; do key KEYCODE_X; done
shot 03_onboard 4
for i in 1 2 3 4 5 6; do key KEYCODE_X; done
shot 04_more 6
shot 05_scan 15
key KEYCODE_DPAD_DOWN
key KEYCODE_X
shot 06_open 5
key KEYCODE_X
shot 07_play 8
shot 08_play_later 8
key KEYCODE_C
key KEYCODE_C
shot 09_back 4
adb shell input tap 960 540
shot 10_tap 3
# background and back (music service path, surface recreation)
adb shell input keyevent KEYCODE_HOME
sleep 3
adb shell am start -n com.chromevoid.nest/.NestActivity
shot 11_resume 5

adb shell pidof com.chromevoid.nest > "$OUT/pid.txt" || echo "NOT RUNNING" > "$OUT/pid.txt"
adb logcat -d > "$OUT/logcat_full.txt"
grep -E "Nest|SDL|AndroidRuntime|DEBUG|libc|FATAL|chromevoid" "$OUT/logcat_full.txt" | tail -400 > "$OUT/logcat.txt"
ls -la "$OUT"
