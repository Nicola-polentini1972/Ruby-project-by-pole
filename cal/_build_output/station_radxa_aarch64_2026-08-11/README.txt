Build "station" + ruby_i2c + ruby_plugins (RUBY_BUILD_ENV=radxa) - 2026-08-11

Toolchain: aarch64-linux-gnu-gcc/g++ (Debian, gcc 14.2.0), reale target ARM64
usato dal Radxa Zero3W (RUBY_BUILD_HW_PLATFORM_RADXA).

Dipendenze cross arm64 usate:
  - libpcap-dev:arm64, libcairo2-dev:arm64, libdrm-dev:arm64,
    libi2c-dev:arm64, libsdl2-dev:arm64 (pacchetti Debian arm64 gia' installati)
  - libgpiod 1.6.5 compilata da sorgente in locale (Debian offre solo v2, ma
    code/base/gpio_radxa.c usa l'API v1). Sorgente ufficiale:
    https://mirrors.edge.kernel.org/pub/software/libs/libgpiod/libgpiod-1.6.5.tar.gz
  - Rockchip MPP (Media Process Platform), sorgente ufficiale open source:
    https://github.com/rockchip-linux/mpp (clonato, buildato con
    build/linux/aarch64/make-Makefiles.bash, toolchain aarch64-linux-gnu-
    di default). Fornisce librockchip_mpp.{a,so} e gli header rk_*.h che il
    progetto include come <rockchip/rk_mpi.h> (creata una dir "rockchip/"
    con copia flat degli header da mpp/inc/ per far combaciare il path).

Comando (esempio, con LIBGPIOD_DIR = percorso build libgpiod 1.6.5,
MPP_INC = dir con la sottocartella rockchip/ degli header, MPP_LIB =
mpp/build/linux/aarch64/mpp):
  PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
  make <target> RUBY_BUILD_ENV=radxa \
    CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
    CFLAGS="-I$LIBGPIOD_DIR/include -I$MPP_INC" \
    CXXFLAGS="-I$LIBGPIOD_DIR/include -I$MPP_INC" \
    LDFLAGS="-L$LIBGPIOD_DIR/lib/.libs -L/usr/lib/aarch64-linux-gnu -L$MPP_LIB"

Fix applicato al sorgente durante la build (bug reale, non ambientale):
  code/r_central/oled/oled_icon_loader.h mancava #include <cstdint> (usava
  uint8_t senza includerlo; funzionava per caso con altri compilatori/versioni
  GCC che lo trascinavano transitivamente).

RISULTATO:
  OK  - ruby_start, ruby_controller, ruby_rt_station, ruby_tx_rc,
        ruby_rx_telemetry (+ ruby_utils: initdhcp, init_wifi, sik_config,
        alive, video_proc, update, update_worker, dbg, logger)
  OK  - ruby_i2c
  OK  - ruby_plugins: ruby_plugin_osd_ahi2, ruby_plugin_gauge_speed2,
        ruby_plugin_gauge_altitude2, ruby_plugin_gauge_ahi2,
        ruby_plugin_gauge_heading2 (.so)
  OK  - ruby_central (UI) e ruby_player_radxa: risolti dopo aver compilato
        Rockchip MPP da sorgente (vedi sopra). ruby_player_radxa e' linkato
        dinamicamente contro librockchip_mpp.so (-lrockchip_mpp): sul device
        reale questa lib e' quasi certamente gia' presente (necessaria per
        la decodifica video hardware nativa), quindi compatibilita' a
        runtime probabile, ma comunque da verificare su hardware.

STATO FINALE: TUTTI i target richiesti (station completo, ruby_central,
ruby_i2c, ruby_plugins, ruby_player_radxa) compilano e linkano senza
errori per ARM aarch64 reale.

Nota generale: a differenza della build "vehicle" (OpenIPC), qui il
toolchain e le librerie di sistema sono quelle "vere" (arm64, stesse
versioni disponibili via apt per la piattaforma target) tranne libgpiod
(v1 compilata da sorgente contro Debian trixie invece della libc del
Radxa reale) e Rockchip MPP (compilata da sorgente, stesso codice
upstream usato dal vendor). Compatibilita' a runtime piu' probabile
che nella build OpenIPC, ma non verificata su hardware reale.
