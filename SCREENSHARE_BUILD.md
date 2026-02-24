# Mumble - Screen Sharing (Build Instructions)

## Prerequisites

### Ubuntu/Debian

```bash
sudo apt install build-essential cmake git pkg-config \
  qt6-base-dev qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools \
  libqt6svg6-dev qt6-5compat-dev \
  libboost-dev libssl-dev libprotobuf-dev protobuf-compiler \
  libcap-dev libxi-dev \
  libpipewire-0.3-dev libspa-0.2-dev \
  libavcodec-dev libavutil-dev libswscale-dev
```

### Fedora

```bash
sudo dnf install cmake gcc-c++ git pkg-config \
  qt6-qtbase-devel qt6-qttools-devel qt6-qt5compat-devel qt6-qtsvg-devel \
  boost-devel openssl-devel protobuf-devel protobuf-compiler \
  libcap-devel libXi-devel \
  pipewire-devel \
  libavcodec-free-devel libavutil-free-devel libswscale-free-devel
```

## Clone and build

```bash
git clone https://github.com/jgrubio/mumble.git
cd mumble
git checkout feature/screenshare
git submodule update --init --recursive

cmake -Dscreenshare=ON -Doverlay=OFF -Dplugins=OFF -Dalsa=OFF -Djackaudio=OFF -Dportaudio=OFF -Dpulseaudio=OFF -Drnnoise=OFF -Dspeechd=OFF -Dqtspeech=OFF -Dtranslations=OFF -Dupdate=OFF -Dice=OFF -Dzeroconf=OFF -Dtests=OFF -Dwarnings-as-errors=OFF -B build -S .

cmake --build build -j$(nproc)
```

## Run

### Server (murmurd)

```bash
./build/src/murmur/mumble-server
```

The server listens on port **64738** (TCP+UDP) by default.

### Client (mumble)

```bash
./build/src/mumble/mumble
```

Connect to the server using its IP address and port 64738.

## Screen sharing usage

1. Connect to a server with the modified client
2. Join a channel
3. Use **Share Screen** from the menu to start sharing
4. Other users in the channel will see the shared screen automatically
