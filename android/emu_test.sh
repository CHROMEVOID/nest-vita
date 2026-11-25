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
shot 01_launch 15
key KEYCODE_X                       # Begin
shot 02_pin 3
key KEYCODE_DPAD_RIGHT; key KEYCODE_X   # PIN: Later
shot 03_stories 3
key KEYCODE_X                       # Pick a folder
shot 04_devices 3
key KEYCODE_X                       # Internal storage
shot 05_storage 3
for i in 1 2 3 4 5 6; do key KEYCODE_DPAD_DOWN; done
shot 06_movies_hl 2
key KEYCODE_V                       # use the highlighted folder
shot 07_sort 3
key KEYCODE_X                       # sort by guess
shot 08_found 12
key KEYCODE_X                       # Save
shot 09_another 4
key KEYCODE_X                       # I'm done
shot 10_music 4
key KEYCODE_X                       # Find my music
shot 11_choices 4
key KEYCODE_DPAD_DOWN; key KEYCODE_DPAD_DOWN; key KEYCODE_DPAD_DOWN
shot 12_choices_down 2
key KEYCODE_X
shot 13_home 8
key KEYCODE_DPAD_DOWN
shot 14_home_down 3
key KEYCODE_X
shot 15_detail 6
key KEYCODE_X
shot 16_play 10
shot 17_play_later 10
key KEYCODE_DPAD_RIGHT
shot 18_seek 6
key KEYCODE_C
shot 19_back 4
key KEYCODE_C
key KEYCODE_E; key KEYCODE_E
shot 20_tab 4
key KEYCODE_E
shot 21_tab2 4
adb shell input tap 960 540
shot 22_tap 3
# background and back (music service path, surface recreation)
adb shell input keyevent KEYCODE_HOME
sleep 4
adb shell dumpsys activity services com.chromevoid.nest | grep -i "ServiceRecord" > "$OUT/services.txt"
adb shell am start -n com.chromevoid.nest/.NestActivity
shot 23_resume 5

adb shell pidof com.chromevoid.nest > "$OUT/pid.txt" || echo "NOT RUNNING" > "$OUT/pid.txt"
adb logcat -d > "$OUT/logcat_full.txt"
grep -E "Nest|SDL|AndroidRuntime|DEBUG|libc|FATAL|chromevoid" "$OUT/logcat_full.txt" | tail -400 > "$OUT/logcat.txt"
ls -la "$OUT"
