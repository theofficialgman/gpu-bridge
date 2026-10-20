# Top-10 CPU processes per application scenario (raw run data in `docs/perf-raw/`)

Each block is one run (`tools/realapps/bench.sh`): per-process CPU as a percentage of one core (4 cores), the ten busiest process names plus the sum over all processes. `python3` is the sampler itself (about 7%); `htop` is a terminal on the desktop. *vsync* = presentation locked to the display; otherwise the frame rate is unlocked. The last three bridged vsync runs of OpenArena (both renderers), SuperTuxKart and mpv were repeated with `GB_STATS=1` for the call statistics, so their figures differ slightly from the ones quoted in `target-report.md`.


## OpenArena, GL3.2 renderer - unlocked

| rank | native arm64 | bridged armhf | armhf llvmpipe |
|---|---|---|---|
| 1 | ioquake3 93.0% | gbd 68.7% | ioquake3 266.4% |
| 2 | python3 6.9% | ioquake3 43.4% | python3 5.9% |
| 3 | htop 5.7% | python3 6.9% | htop 5.3% |
| 4 | nvgpu_channel_p 2.7% | htop 5.7% | Xorg 4.9% |
| 5 | plasmashell 2.6% | Xorg 2.8% | polkitd 1.3% |
| 6 | Xorg 2.6% | nvgpu_channel_p 2.7% | plasmashell 1.3% |
| 7 | irq/67-host_syn 1.7% | plasmashell 2.5% | avahi-daemon 0.6% |
| 8 | polkitd 1.5% | polkitd 1.9% | dbus-daemon 0.5% |
| 9 | avahi-daemon 0.7% | sugov:0 1.7% | irq/72-tegradc. 0.5% |
| 10 | sugov:0 0.7% | irq/67-host_syn 1.6% | nvgpu_channel_p 0.5% |
| all | ALL PROCESSES 124.5% | ALL PROCESSES 144.9% | ALL PROCESSES 291.4% |
| wall s | 23.1 | 26.1 | 45.6 |

## OpenArena, GL3.2 renderer - vsynced

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | ioquake3 38.8% | gbd 22.8% |
| 2 | python3 6.1% | ioquake3 16.9% |
| 3 | htop 5.5% | python3 6.2% |
| 4 | plasmashell 1.4% | htop 5.4% |
| 5 | Xorg 1.1% | polkitd 1.4% |
| 6 | sugov:0 0.8% | plasmashell 1.3% |
| 7 | irq/75-gk20a_st 0.8% | Xorg 1.2% |
| 8 | avahi-daemon 0.7% | irq/75-gk20a_st 0.9% |
| 9 | polkitd 0.7% | nvgpu_channel_p 0.8% |
| 10 | nvgpu_channel_p 0.7% | avahi-daemon 0.7% |
| all | ALL PROCESSES 60.9% | ALL PROCESSES 64.6% |
| wall s | 45.2 | 45.2 |

## OpenArena, fixed-function renderer - unlocked

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | ioquake3 90.3% | gbd 74.4% |
| 2 | python3 6.2% | ioquake3 48.1% |
| 3 | htop 5.6% | python3 6.7% |
| 4 | plasmashell 4.8% | htop 5.6% |
| 5 | Xorg 3.0% | Xorg 2.5% |
| 6 | irq/67-host_syn 1.5% | plasmashell 2.2% |
| 7 | nvgpu_channel_p 1.4% | nvgpu_channel_p 1.3% |
| 8 | avahi-daemon 0.6% | polkitd 1.2% |
| 9 | irq/72-tegradc. 0.5% | irq/67-host_syn 1.1% |
| 10 | sugov:0 0.5% | upowerd 1.0% |
| all | ALL PROCESSES 118.9% | ALL PROCESSES 150.3% |
| wall s | 13.1 | 27.7 |

## OpenArena, fixed-function renderer - vsynced

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | ioquake3 24.3% | gbd 35.7% |
| 2 | python3 6.2% | ioquake3 22.2% |
| 3 | htop 5.5% | python3 6.2% |
| 4 | irq/75-gk20a_st 1.3% | htop 5.5% |
| 5 | plasmashell 1.2% | sugov:0 1.8% |
| 6 | Xorg 1.2% | plasmashell 1.3% |
| 7 | polkitd 0.8% | Xorg 1.2% |
| 8 | avahi-daemon 0.7% | irq/75-gk20a_st 1.0% |
| 9 | irq/72-tegradc. 0.5% | avahi-daemon 0.7% |
| 10 | sugov:0 0.5% | irq/72-tegradc. 0.6% |
| all | ALL PROCESSES 46.7% | ALL PROCESSES 79.5% |
| wall s | 45.1 | 45.2 |

## SuperTuxKart - unlocked

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | supertuxkart 95.6% | supertuxkart 70.3% |
| 2 | python3 6.9% | gbd 51.7% |
| 3 | htop 6.0% | python3 6.9% |
| 4 | Xorg 2.0% | htop 5.7% |
| 5 | nvgpu_channel_p 1.7% | Xorg 1.8% |
| 6 | plasmashell 1.3% | nvgpu_channel_p 1.6% |
| 7 | pipewire 1.1% | plasmashell 1.3% |
| 8 | avahi-daemon 0.7% | pipewire-pulse 1.3% |
| 9 | irq/67-host_syn 0.7% | pipewire 1.1% |
| 10 | sugov:0 0.6% | irq/67-host_syn 0.7% |
| all | ALL PROCESSES 121.3% | ALL PROCESSES 147.5% |
| wall s | 52.3 | 52.8 |

## SuperTuxKart - vsynced

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | supertuxkart 48.4% | supertuxkart 47.0% |
| 2 | python3 6.4% | gbd 33.7% |
| 3 | htop 5.5% | python3 6.4% |
| 4 | sugov:0 1.8% | htop 5.6% |
| 5 | polkitd 1.5% | sugov:0 1.8% |
| 6 | plasmashell 1.3% | Xorg 1.4% |
| 7 | Xorg 1.2% | plasmashell 1.3% |
| 8 | pipewire 1.0% | pipewire-pulse 1.2% |
| 9 | avahi-daemon 0.7% | pipewire 1.0% |
| 10 | nvgpu_channel_p 0.7% | nvgpu_channel_p 0.8% |
| all | ALL PROCESSES 74.6% | ALL PROCESSES 105.8% |
| wall s | 52.2 | 53.3 |

## mpv (high-quality profile) - unlocked

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | mpv 147.7% | mpv 97.4% |
| 2 | kwin_x11 82.5% | kwin_x11 80.1% |
| 3 | python3 9.5% | gbd 61.0% |
| 4 | Xorg 7.0% | python3 9.5% |
| 5 | htop 6.1% | Xorg 7.9% |
| 6 | nvgpu_channel_p 2.6% | htop 6.5% |
| 7 | irq/67-host_syn 1.7% | nvgpu_channel_p 3.3% |
| 8 | tegradc.1/a 1.2% | irq/67-host_syn 2.3% |
| 9 | plasmashell 1.2% | tegradc.1/a 1.3% |
| 10 | avahi-daemon 0.8% | kworker/u8:4 1.0% |
| all | ALL PROCESSES 267.0% | ALL PROCESSES 278.6% |
| wall s | 9.5 | 12.3 |

## mpv (high-quality profile) - vsynced

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | mpv 50.8% | mpv 48.9% |
| 2 | kwin_x11 15.2% | gbd 24.6% |
| 3 | Xorg 7.6% | kwin_x11 14.2% |
| 4 | python3 6.9% | python3 7.3% |
| 5 | htop 6.0% | Xorg 6.8% |
| 6 | nvgpu_channel_p 1.8% | htop 6.0% |
| 7 | irq/75-gk20a_st 1.6% | sugov:0 2.7% |
| 8 | sugov:0 1.5% | nvgpu_channel_p 1.4% |
| 9 | irq/67-host_syn 1.3% | irq/67-host_syn 1.3% |
| 10 | tegradc.1/a 1.3% | irq/75-gk20a_st 1.2% |
| all | ALL PROCESSES 99.9% | ALL PROCESSES 121.3% |
| wall s | 31.2 | 31.8 |

## glxgears - unlocked

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | glxgears 90.0% | gbd 93.5% |
| 2 | kwin_x11 54.2% | glxgears.arm-li 17.3% |
| 3 | Xorg 8.5% | Xorg 8.6% |
| 4 | irq/67-host_syn 7.6% | kwin_x11 7.1% |
| 5 | python3 6.3% | python3 6.5% |
| 6 | htop 5.7% | irq/67-host_syn 5.8% |
| 7 | nvgpu_channel_p 5.5% | htop 5.7% |
| 8 | polkitd 2.0% | nvgpu_channel_p 5.3% |
| 9 | kworker/u8:4 1.6% | kworker/u8:1 1.7% |
| 10 | kworker/u8:0 1.5% | kworker/u8:0 1.6% |
| all | ALL PROCESSES 190.8% | ALL PROCESSES 161.8% |
| wall s | 30.0 | 30.0 |

## glxgears - vsynced

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | kwin_x11 10.0% | kwin_x11 13.1% |
| 2 | glxgears 8.1% | gbd 7.9% |
| 3 | python3 6.4% | python3 6.4% |
| 4 | htop 5.5% | htop 5.6% |
| 5 | Xorg 3.9% | Xorg 3.8% |
| 6 | irq/75-gk20a_st 1.9% | irq/75-gk20a_st 1.8% |
| 7 | polkitd 1.4% | tegradc.1/a 1.2% |
| 8 | tegradc.1/a 1.2% | nvgpu_channel_p 1.1% |
| 9 | nvgpu_channel_p 1.0% | irq/67-host_syn 0.9% |
| 10 | irq/67-host_syn 0.8% | avahi-daemon 0.8% |
| all | ALL PROCESSES 45.9% | ALL PROCESSES 48.6% |
| wall s | 30.0 | 30.0 |

## es2gears_x11 - unlocked

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | es2gears_x11 90.7% | kwin_x11 92.3% |
| 2 | Xorg 16.2% | gbd 89.1% |
| 3 | irq/67-host_syn 15.7% | es2gears_x11.ar 42.1% |
| 4 | nvgpu_channel_p 14.1% | Xorg 15.2% |
| 5 | kwin_x11 11.8% | irq/67-host_syn 11.8% |
| 6 | python3 6.5% | nvgpu_channel_p 9.9% |
| 7 | htop 6.0% | python3 7.0% |
| 8 | kworker/u8:2 2.9% | htop 5.9% |
| 9 | kworker/u8:1 2.7% | kworker/u8:1 2.5% |
| 10 | kworker/u8:4 2.0% | kworker/u8:0 2.4% |
| all | ALL PROCESSES 179.3% | ALL PROCESSES 288.2% |
| wall s | 30.0 | 30.0 |

## es2gears_x11 - vsynced

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | kwin_x11 9.5% | kwin_x11 10.5% |
| 2 | es2gears_x11 8.4% | gbd 7.7% |
| 3 | python3 6.2% | python3 6.2% |
| 4 | htop 5.6% | htop 5.8% |
| 5 | Xorg 3.7% | Xorg 4.0% |
| 6 | irq/75-gk20a_st 1.9% | irq/75-gk20a_st 1.8% |
| 7 | tegradc.1/a 1.2% | polkitd 1.4% |
| 8 | nvgpu_channel_p 1.0% | tegradc.1/a 1.2% |
| 9 | polkitd 1.0% | nvgpu_channel_p 1.0% |
| 10 | irq/67-host_syn 0.8% | irq/67-host_syn 0.9% |
| all | ALL PROCESSES 44.7% | ALL PROCESSES 47.0% |
| wall s | 30.0 | 30.0 |

## neverball - unlocked

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | neverball 44.5% | gbd 80.0% |
| 2 | python3 6.1% | neverball 24.0% |
| 3 | htop 5.5% | plasmashell 7.2% |
| 4 | pipewire-pulse 1.6% | python3 6.3% |
| 5 | sugov:0 1.5% | htop 5.7% |
| 6 | plasmashell 1.2% | Xorg 3.0% |
| 7 | Xorg 1.0% | pipewire-pulse 2.0% |
| 8 | irq/75-gk20a_st 1.0% | deskflow-core 1.6% |
| 9 | pipewire 0.8% | pipewire 1.1% |
| 10 | avahi-daemon 0.6% | kwin_x11 1.0% |
| all | ALL PROCESSES 67.5% | ALL PROCESSES 139.3% |
| wall s | 45.2 | 46.7 |

## neverball - vsynced

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | neverball 47.6% | gbd 83.1% |
| 2 | plasmashell 7.4% | neverball 24.2% |
| 3 | python3 6.1% | python3 6.1% |
| 4 | htop 5.4% | htop 5.5% |
| 5 | Xorg 3.4% | pipewire-pulse 1.7% |
| 6 | pipewire-pulse 1.7% | Xorg 1.2% |
| 7 | deskflow-core 1.4% | plasmashell 1.1% |
| 8 | sugov:0 1.3% | sugov:0 1.0% |
| 9 | kwin_x11 1.0% | pipewire 0.9% |
| 10 | irq/75-gk20a_st 1.0% | irq/75-gk20a_st 0.8% |
| all | ALL PROCESSES 81.7% | ALL PROCESSES 130.4% |
| wall s | 45.2 | 46.7 |

## Extreme Tux Racer - unlocked

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | etr 40.5% | gbd 82.9% |
| 2 | python3 6.2% | etr 65.9% |
| 3 | htop 5.6% | python3 7.9% |
| 4 | Xorg 2.3% | htop 6.3% |
| 5 | sugov:0 1.5% | Xorg 2.0% |
| 6 | tegradc.1/a 1.0% | pipewire-pulse 1.5% |
| 7 | pipewire 0.9% | pipewire 1.3% |
| 8 | plasmashell 0.9% | plasmashell 1.0% |
| 9 | avahi-daemon 0.7% | avahi-daemon 0.8% |
| 10 | nvgpu_channel_p 0.5% | nvgpu_channel_p 0.7% |
| all | ALL PROCESSES 63.9% | ALL PROCESSES 175.7% |
| wall s | 60.1 | 60.1 |

## Extreme Tux Racer - vsynced

| rank | native arm64 | bridged armhf |
|---|---|---|
| 1 | etr 39.8% | gbd 83.1% |
| 2 | python3 6.3% | etr 65.7% |
| 3 | htop 5.6% | python3 7.9% |
| 4 | Xorg 2.4% | htop 6.1% |
| 5 | plasmashell 1.5% | Xorg 2.1% |
| 6 | sugov:0 1.5% | pipewire-pulse 1.4% |
| 7 | tegradc.1/a 1.0% | pipewire 1.3% |
| 8 | pipewire 1.0% | plasmashell 1.0% |
| 9 | avahi-daemon 0.7% | avahi-daemon 0.7% |
| 10 | nvgpu_channel_p 0.5% | nvgpu_channel_p 0.7% |
| all | ALL PROCESSES 64.7% | ALL PROCESSES 175.4% |
| wall s | 60.1 | 60.1 |
