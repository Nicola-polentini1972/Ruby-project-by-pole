Build "vehicle" (RUBY_BUILD_ENV=openipc) - 2026-08-11

Toolchain usato: arm-linux-gnueabi-gcc/g++ (Debian, gcc 14.2.0)
  - ABI: soft-float (gnueabi), NON gnueabihf
  - libc: glibc 2.41 (Debian trixie)
Libreria: libpcap 1.10.5 compilata staticamente per ARM con questo stesso
toolchain (sorgente: github.com/the-tcpdump-group/libpcap, tag libpcap-1.10.5).

Comando:
  make vehicle RUBY_BUILD_ENV=openipc \
    CC=arm-linux-gnueabi-gcc CXX=arm-linux-gnueabi-g++ \
    CFLAGS="-I<libpcap_src>" CXXFLAGS="-I<libpcap_src>" \
    LDFLAGS="-L<libpcap_src>/build-arm"

Risultato: compilazione e link completati senza errori (solo warning
"packed member" nei file mavlink generati, innocui e presenti anche nelle
build ufficiali).

LIMITAZIONE NOTA: questo NON e' il toolchain ufficiale del progetto
(CLAUDE.md richiede "toolchain.sigmastar-infinity6e" con ABI
arm-openipc-linux-gnueabihf, hard-float). Questi binari sono ARM validi ma
quasi certamente NON compatibili a runtime con il firmware OpenIPC reale sul
drone (ABI soft-float vs hard-float, glibc moderna vs libc del vendor molto
piu' vecchia/minimale). Utili come conferma che il codice sorgente (incluse
le fix applicate in questa sessione) compila e linka correttamente per
target ARM, NON pronti per il flash su hardware reale.

File inclusi: ruby_start, ruby_rt_vehicle, ruby_tx_telemetry, ruby_update,
ruby_logger, ruby_initdhcp, ruby_init_wifi, ruby_sik_config, ruby_alive,
ruby_video_proc, ruby_dbg, ruby_update_worker.
