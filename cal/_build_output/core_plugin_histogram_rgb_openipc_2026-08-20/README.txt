Build "ruby_core_plugin_histogram_rgb" (RUBY_BUILD_ENV=openipc) - 2026-08-20

Toolchain usato: toolchain.sigmastar-infinity6e (ufficiale, scaricato da
https://github.com/OpenIPC/firmware/releases/download/toolchain/toolchain.sigmastar-infinity6e.tgz)
  - ABI: hard-float (gnueabihf) - VERIFICATA con readelf -A: Tag_ABI_VFP_args = VFP registers
  - Prefisso target: arm-openipc-linux-gnueabihf
  - gcc/g++ 13.3.0 (Buildroot)

Comando:
  TC=/home/pole/toolchains/arm-openipc-linux-gnueabihf_sdk-buildroot
  export PATH="$TC/bin:$PATH"
  export LDFLAGS=-Wl,--allow-shlib-undefined
  cd code/r_plugins_core
  make ruby_core_plugin_histogram_rgb \
    CC="arm-linux-gcc -B$TC/arm-openipc-linux-gnueabihf/bin/" \
    CXX="arm-linux-g++ -B$TC/arm-openipc-linux-gnueabihf/bin/"

Risultato: compilazione e link completati senza errori/warning. Binario ARM
EABI5 hard-float, corretto per il firmware OpenIPC reale (a differenza del
tentativo precedente con arm-linux-gnueabi-gcc soft-float, solo per verifica
codice).

File incluso: ruby_core_plugin_histogram_rgb.so.1.0.1

Da installare sul drone in: /root/ruby/plugins/core/
(core plugin "RGB Histogram (live video)", cattura frame via V4L2 e invia i
bin dell'istogramma al controller; vedi code/r_plugins_core/ruby_core_plugin_histogram_rgb.cpp
per i dettagli e i limiti noti, in particolare la dipendenza da un device
V4L2 raw non garantito su tutti i SoC OpenIPC).
