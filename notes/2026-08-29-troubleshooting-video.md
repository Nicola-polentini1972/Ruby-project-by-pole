# Troubleshooting video — 2026-08-29

Riepilogo di una sessione di analisi su due problemi video segnalati dall'utente, con riferimenti al codice per riprendere il discorso.

## 1. VLC non visualizza correttamente i file generati dall'air unit

**Sintomo iniziale:** i file video registrati (ricevuti via radio e salvati sulla ground station) non si vedono bene in VLC su Linux Mint.

**Causa individuata nel codice:**
- La registrazione lato ground station (`code/r_station/rx_video_recording.cpp`, funzione `_thread_video_recording`) scrive uno **stream elementare H264/H265 raw (Annex-B), senza contenitore** — nessun mux, nessun timestamp. File temporaneo: `FILE_TEMP_VIDEO_FILE "tmpVideo.h26x"` (`code/base/config_file_names.h:65`).
- Il file finale (`code/r_utils/ruby_video_proc.cpp`, `store_video()`) viene salvato in `FOLDER_MEDIA` come `video-<nome_veicolo>-<bootcount>-<timestamp>.h264`/`.h265`, con `.info` (fps calcolato, durata, risoluzione) e opzionali `.osd`/`.srt` a fianco. **Nessun mux automatico in MP4.**
- Il mux in MP4 esiste ma è **opzionale**, attivato solo quando l'utente esporta la registrazione su chiavetta USB dal menu Storage (`code/r_central/menu/menu_storage.cpp:697,724`), che lancia `./ruby_video_proc <info> <tmp_out.mp4>` → `process_video()` in `ruby_video_proc.cpp:237-294`, che fa `ffmpeg -framerate <fps> -y -i raw.h264 -c:v copy out.mp4` usando l'fps calcolato salvato nel `.info`.

**Sintomo dopo approfondimento:** non era un problema di container ma di **artefatti/blocchi verdi** (corruzione dei macroblocchi) — confermato che disabilitare la decodifica hardware in VLC non risolve. Conclusione: corruzione reale nei dati (probabile packet loss sul link radio durante la registrazione, che propaga l'errore nei P-frame fino al prossimo keyframe). RubyFPV non fa alcun repair/error-concealment sui NAL persi in fase di registrazione. Decoder diversi (VLC aggiornato su Windows/Flatpak vs VLC datato dei repo apt di Mint) mascherano la corruzione in modo diverso, da cui la differenza percepita tra sistemi operativi.

**Stato:** l'utente userà Windows per la riproduzione. Punto aperto non ancora affrontato: se si vuole risolvere alla radice, servirebbe controllare i log/telemetria della sessione di volo per packet loss/retransmission sul link radio in corrispondenza dei punti con artefatti, ed eventualmente rivedere i parametri FEC/radio.

## 2. Frame persi nella registrazione onboard (1080p60, 25 Mbps)

**Setup:** veicolo OpenIPC SSC338Q con backend encoder **waybeam** (non majestic). La registrazione onboard (dual-VENC: canale 0 live streaming, canale 1 registrazione su SD) è supportata solo su waybeam (`hwcam_be_supports_onboard_recording()` in `code/base/hardware_cam_backend.cpp:190`, true solo per `HWCAM_BE_WAYBEAM`).

**Sintomo:** frame persi **solo nel file `.ts` sulla SD**, non nello streaming live a terra — esclude quindi un problema generale di CPU/overflow sul socket UDP del percorso di cattura live (`video_source_majestic.cpp`), che riguarderebbe entrambi i canali.

**Causa individuata nel codice:** RubyFPV **non scrive** il file `.ts` — si limita a configurare il canale di registrazione e ad avviarlo/fermarlo via chiamate HTTP locali al demone waybeam (`curl localhost/api/v1/record/start|stop|status`, in `code/base/hardware_cam_maj.cpp:163-175`, invocato da `code/r_vehicle/ruby_rx_commands.cpp:940-975`). Tutta la logica di encoding e scrittura su SD (`/mnt/mmcblk0p1/...`) è interna al demone closed-source waybeam sulla SoC — **fuori dal codice e dal controllo di RubyFPV**. L'unico file scritto da RubyFPV in questo percorso è il sidecar `.osd` (telemetria/overlay), in `code/r_vehicle/rx_osd_recording_vehicle.cpp`, non il video.

**Stato:** nessuna azione possibile lato codice RubyFPV. Piste da verificare lato utente/hardware:
1. Qualità/velocità della scheda SD (25 Mbps ≈ 3.1 MB/s sostenuti in scrittura sequenziale — schede economiche o filesystem frammentato/pieno possono avere cali).
2. Versione firmware waybeam/OpenIPC sulla camera (possibile bug noto già risolto in versioni più recenti).
3. Log del demone waybeam sul device via SSH, in corrispondenza dei frame persi.

### Aggiornamento — verifica diretta via SSH sull'unità di aria (192.168.11.232, OpenIPC 2.6.06.15, backend waybeam)

Collegati via SSH (`root@192.168.11.232`, credenziali default `root/12345`), controllati:
- `logread` (syslog) e `/var/log/logs/log_system.txt` / `log_errors_soft.txt`: nessuna riga specifica su frame drop/record loggata a livello applicativo.
- `dmesg` sul device: trovati **warning ripetuti del driver hardware dell'encoder** (SigmaStar MI VENC), in corrispondenza di `FPS (Input, Output, Cur, Deband) = (60, 60, 60000, 60)`:
  ```
  [MI WRN]: _MI_VENC_AbortFrame[3785]: Fail to re-encode/discard, generate next GOP.
  SuperFrmMode:0 IFrmBitsThr:0 PFrmBitsThr:0
  ```
  Questo conferma che il frame drop avviene **a livello di encoder hardware della SoC (SSC338Q)**, non per un collo di bottiglia di scrittura SD: l'encoder non riesce a stare al passo con l'input e scarta il frame corrente, generando un nuovo GOP. Con dual-VENC attivo (canale live + canale registrazione, entrambi 1080p60/25Mbps) il carico combinato supera il throughput realtime del blocco encoder.
  - Verificato anche `curl localhost/api/v1/record/status` → `{"active":false,...}`: la registrazione non era in corso al momento del controllo, quindi i warning nel buffer dmesg risalgono a un tentativo precedente.

### Test a 40 Mbps — nessun AbortFrame, ma scoperta la vera causa

Test live via SSH: avviata registrazione onboard a 40 Mbps, monitorato per ~4,6 minuti campionando `curl localhost/api/v1/record/status` e il conteggio di `AbortFrame` nel dmesg ogni 5s. Risultato: **nessun nuovo `AbortFrame`** durante tutta la finestra (contatore fermo a 6, tutti antecedenti), frame rate calcolato dai contatori costantemente **~60 fps**, bitrate reale ~42 Mbps. I warning `AbortFrame` visti prima sembrano quindi un evento isolato (probabile riconfigurazione/avvio canale), non la causa sistematica del frame loss segnalato.

### Causa reale trovata: limite hard a 2 GB (2³¹−1 byte) nel demone waybeam

Fermata la registrazione di test: `curl localhost/api/v1/record/stop` → stato finale `"stop_reason":"write_error"`, e il file `.ts` risultante (`rec_00h09m35s_7d28.ts`) ha dimensione **esattamente 2147483647 byte (2³¹−1, INT32_MAX)**. Controllando altri file storici nella cartella `/mnt/mmcblk0p1/ruby/` su device, un secondo file (`rec_00h01m06s_1208.ts`) ha **la stessa identica dimensione** — comportamento deterministico e riproducibile, non un glitch occasionale.

Non è un limite del filesystem (la SD è **exFAT**, nessun limite di 4GB come FAT32) né della configurazione (`/etc/waybeam.json` ha `"maxMB": 5000`, che dovrebbe permettere fino a 5GB con rotazione automatica in nuovi segmenti — ma `"segments":1` conferma che la rotazione non è mai scattata).

**waybeam è open source** ([github.com/OpenIPC/waybeam_venc](https://github.com/OpenIPC/waybeam_venc)) — verificato il codice sorgente (`src/star6e_ts_recorder.c`, funzione `open_new_segment()`):
```c
state->fd = open(state->path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
```
Manca `O_LARGEFILE`, e non è definito `_FILE_OFFSET_BITS 64` / `_LARGEFILE64_SOURCE`. Su questo target ARM 32-bit, `off_t` resta quindi a 32 bit con segno: il kernel rifiuta (`EFBIG`) qualunque scrittura che porterebbe il file oltre 2³¹−1 byte, **indipendentemente** dal `maxMB` configurato. La logica di rotazione applicativa (contatori `uint64_t` corretti, confrontati con `maxMB`) è scritta bene ma non fa in tempo a scattare perché il vero limite di sistema (2GB) arriva prima della soglia configurata (5000MB).

Verificato anche che il bug è presente nel branch `master` attuale di waybeam_venc (nessuna release più recente lo corregge) — **un aggiornamento firmware non risolverebbe il problema oggi**.

**Workaround pratico (non ancora applicato sul device):** impostare `record.maxMB` sotto ~1900 in `/etc/waybeam.json`, così la rotazione applicativa (funzionante) chiude il segmento prima di toccare il vero limite di sistema a 2GB.

**Issue aperta upstream:** [github.com/OpenIPC/firmware/issues/2328](https://github.com/OpenIPC/firmware/issues/2328) (aperta su `OpenIPC/firmware` perché le Issues sono disabilitate sul repo `waybeam_venc`), con root cause, riferimento al codice sorgente e fix suggerita (build con `_FILE_OFFSET_BITS=64` / `open64()`, o clamp difensivo di `maxMB` sui target 32-bit).

**Da fare:**
1. Applicare il workaround (`maxMB` ~1900) sul device e verificare che sopravviva a un riavvio.
2. Seguire l'issue GitHub per un eventuale fix upstream.
3. Non ancora verificata la velocità/qualità della scheda SD (probabilmente non più rilevante, la vera causa è confermata essere il bug 2GB).
