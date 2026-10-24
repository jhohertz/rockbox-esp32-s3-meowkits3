/*
 * This config file is for the MeowKit ESP32-S3 (Xtensa LX7).
 *
 * Only the parts that are actually implemented are enabled.
 */

/* For Rolo and boot loader */
#define MODEL_NUMBER 133
#define MODEL_NAME   "MeowKit ESP32-S3"

#define CONFIG_CPU ESP32S3

/* HAVE_SYSTEM_LIBC -- ESP-IDF supplies the C library (newlib/picolibc) and links
 * it normally, so Rockbox's own libc must not be built alongside it: it defines
 * the same names (memcpy, strlen, qsort, mktime, errno accessors and the rest),
 * and with both archives on the link line the implementation a call lands on
 * depends on archive order rather than on intent, which is the kind of bug that
 * shows up as behaviour rather than as a link error.
 *
 * It is deliberately not defined here. A config header is pulled in by every
 * domain in the build, and this target has two: the firmware, which links the
 * platform library, and codecs and plugins, which are separate executables with no
 * C library in their link at all, whose C library is the codec_api / plugin_api
 * jump table. Defining it here made the freestanding domain behave as though it
 * linked picolibc too -- firmware/include/file.h reached for <unistd.h>, which
 * resolved to the toolchain's copy inside a -ffreestanding translation unit whose
 * include order shadows the sys/cdefs.h that copy depends on, and one header line
 * turned into thousands of parse errors. tools/root.make supplies it to the
 * firmware domain only.
 */

/* Core runs at 240 MHz; the APB/peripheral bus is 80 MHz.
 *
 * This has to agree with CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ in the IDF project,
 * because IDF is what actually sets the clock. It cannot be derived from
 * sdkconfig.h here: this header is pulled in by every domain and by the
 * SOURCES preprocessor, and IDF's CONFIG_* names must not reach either (see
 * the PPCFLAGS note in tools/root.make). system-xtensa.c asserts the match
 * instead, in a firmware-domain TU where sdkconfig.h is reachable.
 */
#define CPU_FREQ 240000000

/* Not set, on purpose. Rockbox's cpu_boost() exists to call
 * set_cpu_frequency(), and on this SoC the clock tree belongs to ESP-IDF:
 * IDF moves the analog regulator bias (dbias) along with the core clock via
 * REGI2C and the ROM's PVT calibration, which Rockbox cannot reproduce. Hand
 * writing CPUPERIOD_SEL and the LDO slave count is the kind of change that
 * browns out the chip instead of failing visibly. Power management, if wanted,
 * goes through IDF's esp_pm DFS rather than around it. */

/* LCD: ST7789. The panel's GRAM is 240x320, but the reference firmware drives it
 * rotated 90 degrees (LovyanGFX offset_rotation = 3), so it is used as 320x240. */
#define HAVE_LCD_COLOR
#define HAVE_LCD_BITMAP
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_DEPTH  16
#define LCD_PIXELFORMAT RGB565
/* 320x240 across a 2.0" diagonal is 400 pixels diagonal, i.e. 200 dpi. */
#define LCD_DPI    200

/* Three display power states. The backlight dominates the panel, but the panel
 * keeps driving its gates as long as it is enabled, so DISPOFF follows the
 * backlight and SLPIN is reached only after the backlight has been off for a
 * while. Rockbox asks for both: lcd_enable() is the shallow state the backlight
 * driver uses, lcd_sleep() the deeper one the timeout reaches.
 */
#define HAVE_LCD_ENABLE
#define HAVE_LCD_SLEEP
#define LCD_SLEEP_TIMEOUT (2*HZ)
#define HAVE_LCD_SHUTDOWN

#define HAVE_BACKLIGHT
/* GPIO 42 is routed through the GPIO matrix to LEDC channel 7, so the backlight is
 * a PWM duty rather than a level. The LED is active low. */
#define HAVE_BACKLIGHT_BRIGHTNESS
#define MIN_BRIGHTNESS_SETTING     0
#define MAX_BRIGHTNESS_SETTING     255
#define DEFAULT_BRIGHTNESS_SETTING 200

/*
 * Big statics go to PSRAM.
 *
 * Both of these used to name sections of Rockbox's own linker script
 * (.framebuffer, .psrambss), which this path does not have. Naming a section
 * nothing in the script claims does not fail: the linker keeps it as an orphan
 * and drops it wherever its default rules happen to put it, which on this
 * script was between IDF's flash regions -- and IDF's script then refuses the
 * result with "the gap between .flash.rodata and .flash.tdata must not exist".
 * That is the shape every one of these mistakes takes here, so the rule is:
 * name a section IDF's script knows about, or name none.
 *
 * .ext_ram.bss is that section: IDF's script places it in PSRAM and its startup
 * zeroes it, provided CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is set in the
 * IDF project.
 *
 * The 337 KB internal DRAM cannot hold the framebuffer (150 KB) or the locale
 * and unicode tables, and taking them out is what leaves room for the transfer
 * buffers and DMA rings, which do have to stay internal because GDMA's
 * descriptor window cannot reach PSRAM.
 *
 * The simulator has no PSRAM, so there the attribute stays empty.
 */
#ifndef SIMULATOR
#define PSRAM_BSS_ATTR      __attribute__ ((section(".ext_ram.bss")))

#define IRAM_LCDFRAMEBUFFER __attribute__ ((section(".ext_ram.bss")))
#endif

/* The sector cache is sized by fs_defines.h from MEMORYSIZE, which on this board
 * is 16 -- the flash size. That is the wrong input here: cache_buffer[] is
 * DMA-visible (the SDHOST IDMAC fills it directly), and DMA-visible memory is
 * the one class of object this platform cannot move to PSRAM, because esp32s3
 * declares no SOC_SDMMC_PSRAM_DMA_CAPABLE. Its budget is therefore the 337 KB
 * internal DRAM region, which is full to 1,020 bytes.
 *
 * 64 entries is 32 KB of that region. Taking it to the small-memory figure
 * (32 entries, one sector each = 16 KB) halves the number of distinct sectors
 * held; directory traversal is already served by the 1 MB dircache in PSRAM, so
 * this is a cache-depth decision rather than a correctness one.
 *
 * MAX_OPEN_FILES has to come down with it: the comment in fs_defines.h requires
 * the cache to have enough buffers for every handle plus a few for the cache
 * client and the filesystem code, and 32 entries against 31 handles leaves one
 * spare. 11 handles is the value the tree already uses below MEMORYSIZE 8.
 */
#define DC_NUM_ENTRIES      32
#define DC_MAP_NUM_ENTRIES  128
#define MAX_OPEN_FILES      11
#define MAX_OPEN_DIRS       12

#define CONFIG_STORAGE STORAGE_SD

/* No SDMMC_HOST_NUM_SD_CONTROLLERS here on purpose. That define is what pulls in
 * Rockbox's generic SDHOST protocol layer (drivers/sdmmc_host.c) and expects the
 * target to supply a set of controller ops. On this target the controller is
 * driven by ESP-IDF's own SD driver, so target/xtensa/sd-idf.c implements the
 * sd_* device entry points directly and the generic layer is not built. */

#define BOOTDIR "/.rockbox"

/* No plugin loader exists for this core, but apps/plugin.c and apps/codecs.c are
 * built unconditionally and every target config defines these. Codecs are real:
 * they are loaded from storage into CODEC_SIZE worth of PSRAM at runtime. */
/* A synchronous dircache build takes audio_buffer_available() outright -- the
 * whole arena -- capped only by DIRCACHE_LIMIT:
 *
 *     size = audio_buffer_available() + dircache_runinfo.bufsize;
 *     size = MAX(DIRCACHE_MIN, MIN(size, DIRCACHE_LIMIT));
 *
 * This target's arena is 4 MB (XT_AUDIOBUF_SIZE, half of PSRAM), so the default
 * 6 MB limit lets the cache claim all of it and everything allocated afterwards
 * from the same arena fails: playlist_init()'s 117 KB fileref table came back as
 * BUFLIB_ERRCODE_MEMBLOCK_ALLOC_FAILED, and because buflib reports failure as a
 * negative code while the caller only tests for zero, that surfaced three frames
 * later as a NULL write.
 *
 * 2 MB was still too generous, and that was measured rather than judged. Walking
 * the allocator at the moment playback sizes its file buffer:
 *
 *   ARENA at playback start: used=4095 KiB in 22 allocs, free=0 KiB in 0 runs
 *     +2228228 B  dircache          <- 53% of the arena
 *     +932184  B  playback file buffer
 *     ...playlist, skins, fonts...
 *
 * Zero free bytes and zero free runs. The 932 KiB file buffer is what
 * audio_reset_buffer_noalloc() carves pcmbuf out of, and with crossfade enabled
 * pcmbuf wants MIN_BUFFER_SIZE (3 s x BYTERATE = 529 KB) plus
 * (fade_out_delay + fade_out_duration) x BYTERATE -- about 1.15 MB total. It
 * does not fit in 932 KiB, and the result is
 * "audio_reset_buffer_noalloc(): EOM (1149612 > 932248)".
 *
 * Dropping to DIRCACHE_MIN is the right lever, not turning crossfade off: the
 * cache is a performance feature whose size is already understood to be
 * negotiable (dircache.h calls MIN/LIMIT "provision" bounds), while the audio
 * path has no fallback. Costs directory-browse speed on a very large card;
 * raises the ceiling on everything that allocates after boot, which on this
 * target is everything that matters. The stored size is clamped by
 * dircache_init() -> MIN(last_size, DIRCACHE_LIMIT), so an existing card setting
 * cannot defeat this. */
#define DIRCACHE_LIMIT   (1024*1024*1) /* 1 MB: DIRCACHE_MIN, leaves ~3 MB free */

/* The recording PCM ring is sized at compile time from these two, but it is
 * allocated with core_alloc_maximum(), which grants whatever is left in the
 * arena rather than what the caller hoped for. The MEMORYSIZE <= 16 branch in
 * pcm_record.c leaves both at their defaults, 256 chunks of 2048 samples, which
 * is a 2 MB ring in a 4 MB arena already holding a 2 MB dircache cap plus the
 * fonts and skins. The grant came back at 910 KiB, every ring index still
 * wrapped at the compile-time 2 MB, and the capture path wrote audio off the end
 * of its own allocation -- straight onto buflib's handle table, which grows down
 * from the top of that same arena. A font's handle entry ended up holding PCM
 * data.
 *
 * The ring therefore has to shrink to fit the grant, but it must shrink by chunk
 * COUNT and not by chunk size. PCM_CHUNK_SIZE is PCM_CHUNK_SAMP * PCM_SAMP_SIZE
 * and it is the unit both pcm_rec_have_more() and enc_pcmbuf_read() advance by,
 * so it is the contract between the DMA hand-off and the encoder. Setting
 * PCM_CHUNK_SAMP to 1024 halves PCM_CHUNK_SIZE to 4096 -- precisely the size our
 * driver slices its reads into, which is what made it look safe -- and capture
 * goes silent while every ADC register still reads back correct. Confirmed by
 * bisect to this commit and then by re-applying its two capture changes
 * separately: moving es7210_init() out of the playback sink is innocent, this
 * resize is what killed the signal.
 *
 * 64 * 2048 * 4 = 512 KiB, about 2.9 seconds of stereo 44.1 kHz, which fits the
 * grant with room left for the stream and encode buffers that follow it. */
#define PCM_NUM_CHUNKS   64

/* The tagcache is what Rockbox calls the database: it indexes the ID3/Vorbis
 * tags on the card so the Tree can offer Artist / Album / Genre / Critter
 * browsing, and it is what "View database info" under Debug refers to. This
 * tree gates it on HAVE_TAGCACHE (current master renamed the knob to
 * CONFIG_DATABASE); apps/SOURCES pulls in the implementation at lines 61 and
 * 183.
 *
 * It arrives with two companions automatically, both from the MEMORYSIZE >= 8
 * block in config.h: HAVE_TC_RAMCACHE, which keeps the index in RAM, and
 * HAVE_PICTUREFLOW_INTEGRATION. Memory is not the constraint here -- 8 MB of
 * PSRAM -- but note the interaction above: the dircache is capped at 2 MB of a
 * 4 MB arena precisely so later allocations from the same arena succeed, and the
 * tagcache thread allocates from it too. If something starts failing with
 * BUFLIB_ERRCODE_MEMBLOCK_ALLOC_FAILED after this, lower DIRCACHE_LIMIT further
 * rather than dropping the tagcache. */
#define HAVE_TAGCACHE

/* TEMPORARY: Rockbox's own thread invariants, off by default and compiled out
 * as `do {} while (0)`. One of them -- "UPPT->wrong thread" in
 * wakeup_thread_transfer() -- asserts precisely what we are crashing on: that
 * the thread releasing a priority-inheriting object is the thread recorded as
 * its owner. With the check off that violation walks a garbage pointer and dies
 * as a bare LoadProhibited; with it on the kernel names the thread itself.
 * Remove once the thread seam is settled. */
/* Off for now, and not because the checks are wrong. Honoured since thread.c
 * stopped clobbering it, enabling these immediately turned a known port defect
 * fatal: thread_exit_final() ends in THREAD_PANICF("thread_exit->K:*R") because
 * in the gated-task model switch_thread() legitimately returns to a thread that
 * has exited -- its FreeRTOS task is still alive, pending on its gate -- which
 * upstream treats as "must never be reached". Until thread_exit is taught the
 * task model, enabling this panic-loops the device instead of informing us.
 *
 * Worth recording what it did find on the way: with these checks on, the
 * runnable list was observed to contain scroll/tick/power/dircache five or six
 * times each, i.e. threads are added to core_entry.rtr without their previous
 * entry being removed. That duplication is the leading candidate for the play
 * hang, since it lets switch_thread() select one thread twice and hand it a
 * second gate. */
#define THREAD_EXTRA_CHECKS 0

#define CODEC_SIZE 0x100000 /* 1 MiB */
#define PLUGIN_BUFFER_SIZE 0x80000

#define CONFIG_KEYPAD ESP32S3_PAD

/* PCF8563 on the I2C bus at 0x51. It is powered from the PMU's LDO1, so it keeps
 * running while the SoC is off. */
#define CONFIG_RTC RTC_PCF8563

/* AXP173 at 0x34: supplies the board, measures the battery, and owns the power
 * button. Charging is controlled by the chip; Rockbox only monitors it. */
#define CONFIG_CHARGING CHARGING_MONITOR
#define CONFIG_BATTERY_MEASURE (VOLTAGE_MEASURE | CURRENT_MEASURE)
#define HAVE_AXP173

#define BATTERY_CAPACITY_DEFAULT 1400 /* mAh, per the board's cell */
#define BATTERY_CAPACITY_MIN     1400
#define BATTERY_CAPACITY_MAX     1400
#define BATTERY_CAPACITY_INC     0


/* I2C uses the hardware master peripheral at 0x60013000, on GPIO 1/2. */
#define CONFIG_I2C I2C_XTENSA

/* The peripheral is a command-list machine: a transaction is programmed whole and
 * executed by the CMD_Controller. That is exactly the shape of an i2c_descriptor,
 * so the asynchronous layer is the natural fit and the synchronous API is built on
 * it. One bus is wired to the outside world.
 */
#define HAVE_I2C_ASYNC

/* This target's __i2c_async_submit() is a blocking transaction: it programs the
 * command list and then polls TRANS_COMPLETE, typically 1.6 ms for the 14-byte
 * touch read and up to its 10 ms bound on a NACK. i2c_async_queue() holds
 * disable_irq_save() across the submit, and on xtensa that is PS.INTLEVEL = 15 --
 * everything off, including the level-2 USB interrupt that feeds the DAC. Measured:
 * with the 20 Hz touch poll live the USB Audio page loses about 1.8% of frames
 * (Filled 18-19 against a gate of 22); with it off, almost none. Deferring the
 * submit until the mask is dropped keeps the bus exclusive and gives the audio
 * interrupt its milliseconds back. See firmware/drivers/i2c-async.c.
 */
#define I2C_ASYNC_SUBMIT_UNMASKED

/* FT6336 capacitive touch at 0x38. Its INT pin is left unconnected on this board,
 * so the controller is polled from the button tick instead of interrupting.
 */
#define HAVE_TOUCHSCREEN
#define HAVE_FT6x06
#define FT6x06_NUM_POINTS 2
#define HAVE_BUTTON_DATA

/* The controller's own axes are the panel's short side first: registers 0x03/0x04
 * run 0..240 and 0x05/0x06 run 0..320, while the panel is driven as 320x240. The
 * reference firmware uses display x = 320 - raw_y, display y = raw_x.
 */
#define DEFAULT_TOUCHSCREEN_CALIBRATION { .A=0, .B=-1, .C=LCD_WIDTH, .D=1, .E=0, .F=0, .divider=1 }

/*
 * USB: the stack is compiled, the hardware bring-up is not.
 *
 * CONFIG_USBOTG == USBOTG_DESIGNWARE selects firmware/drivers/usb-designware.c,
 * whose endpoint register arithmetic matches this core exactly (DIEPCTL at
 * OTGBASE+0x900+0x20*n, DOEPCTL at 0xb00+0x20*n, DIEPTXF at 0x104), and the driver
 * checks USB_NUM_ENDPOINTS against the core's own HWCONF count at startup
 * ("USB_NUM_ENDPOINTS too big"), so the 6 below is a claim the silicon will confirm
 * rather than one asserted silently.
 *
 * Enabling the stack removes a boot hazard rather than creating one. The "if USB is
 * plugged in, enter USB mode before mounting" loop in apps/main.c is guarded by
 * #ifndef HAVE_USBSTACK, and because the AXP173 reports VBUS present whenever the
 * board is powered from the host, that loop is exactly why USB_NONE was needed.
 * DESIGNWARE also sets USB_STATUS_BY_EVENT, so usb_start_monitoring() is the trivial
 * variant that never polls usb_detect().
 *
 * What is deliberately not live yet:
 *
 *  - usb_enable() is a no-op in both directions, so usb_core_init() and
 *    usb_drv_init() never run. usb_dw_check_hw() writes GUSBCFG's PHY field and
 *    compares the whole register back, which on this core panics with "PHY type not
 *    supported": the PHY fields here are phyif (bit 3), ulpiutmisel (4), fsintf (5)
 *    and physel (6), and ESP-IDF's own driver sets phyif = 1. That is the first
 *    bring-up task; until it is done nothing may reach the core.
 *  - The pads. GPIO19/20 are shared between USB-Serial-JTAG and the OTG core through
 *    USB_WRAP, so routing them to OTG silences the console and esptool. They move
 *    only while USB mode is active and return to USJ on reset.
 *
 * USB_ENABLE_SERIAL is set here, which no other target in the tree does: this port is
 * the first to compile firmware/usbstack/usb_serial.c. Its built-in consumer is logf
 * (firmware/logf.c flushes its ring to usb_serial_send), and apps/debug_menu.c
 * carries a "USB Serial driver (logf)" toggle. Its RX side primes EP_OUT and then
 * discards what arrives, so serial control needs a consumer hook in the driver.
 *
 * USB buffers are cached like everything else on this target, so the coherence
 * question the audio DMA needed applies here too. That is a bring-up task, not a
 * compile one, and it is unresolved.
 */
#define HAVE_USBSTACK
#define CONFIG_USBOTG USBOTG_DESIGNWARE
/* Seven endpoints, read from the silicon: GHWCFG2 decodes numdeveps = 7
 * (hwcfg2=0x224dd930, measured at boot), and IDF's own usb_struct.h declares
 * in_ep_reg[7] and out_ep_reg[7], so the register file really has seven. IDF's
 * usb_dwc_cfg.h says OTG_NUM_EPS 6, which is the odd one out and is contradicted
 * by both of those. What the core does *not* have is seven IN FIFOs: GHWCFG4
 * reports 4 dedicated transmit FIFOs (hwcfg4=0xd3f0a030), so usb_dw_check_hw()
 * clamps n_ptxfifos to 4 regardless of how many endpoints exist.
 *
 * usb_dw_check_hw() compares this against the core's own count and panics with
 * "USB_NUM_ENDPOINTS too big" if it is over-declared, so a wrong value fails
 * loudly at bring-up rather than silently.
 *
 * The endpoint pool has to carry the whole profile. Serial uses three (OUT, IN,
 * interrupt for the notification), storage two, HID one and audio two, so
 * CDC+HID+MSC needs six and CDC+HID+Audio needs six, and both fit under seven
 * with one spare. Adding MSC *and* audio would need eight, which the core cannot
 * build -- hence the two profiles, and never a combination of them.
 */
#define USB_NUM_ENDPOINTS 7

/* Turnaround time, in PHY clocks, written to GUSBCFG.usbtrdtim (bits 13:10).
 * The driver's default is 9, derived from an 8-bit UTMI PHY at 60 MHz; this core
 * has an internal full-speed PHY and its own reset value for the field is 5,
 * measured at boot as GUSBCFG=0x1440. Eight other targets in the tree override it
 * to 5, including stm32h743.h, whose internal FS PHY is the closest analogue to
 * this one. TRDT too long only delays the turnaround, but there is no reason to
 * ask the silicon for a value it did not choose for itself.
 */
#define USB_DW_TURNAROUND 5

#define USB_DW_FORCE_DEVICE_MODE    /* OTG-capable core; pin it to device */
#define USB_ENABLE_SERIAL

/* The receive-ring hand-off in usb_audio.c runs with interrupts masked, as upstream
 * does: the mixer thread consumes ring slots while the isochronous interrupt fills
 * them. That guard is not optional here -- without it the mixer reads a slot mid-write
 * and a size that does not match the bytes present, which is audible as real audio
 * fragments inside noise. The lock-up this was added to avoid was the CDC transmit
 * path blocking on a stalled endpoint, which the target now prevents on its own.
 */

/* The isochronous packet size this target arms per frame, which is also the OUT endpoint's
 * declared wMaxPacketSize and therefore the ceiling on what the descriptor may promise.
 * The device FIFO RAM measures 200 words; after NPTX 16 and PTX 8 x 6 the RX side is 136
 * words = 544 bytes, and usb-xtensa.c asserts it can hold one isochronous packet plus EP0's
 * and the CDC OUT packet. That caps this at 320: 512 bytes would need 176 RX words, which
 * the split cannot pay for, so raising it is not an option on this silicon.
 *
 * 320 carries Rockbox's 44.1 kHz (176 bytes a frame), 48 kHz (192) and 64 kHz (256). There
 * is no 80 kHz entry to carry: hw_freq_sampr[] is 192/176/96/88/64/48/44/32/24/22/16/12/11/8,
 * so 64 kHz is the ceiling this packet can hold. The class default of 1023 could never be
 * armed here, which is why usb_audio.c exposes USB_AUDIO_BUFFER_SIZE at all.
 *
 * HW_SAMPR_CAPS is SAMPR_CAP_ALL_96, so hw_freq_sampr still contains 88.2 kHz (353 bytes a
 * frame) and 96 kHz (384) -- both over this packet size, and a frame that does not fit is
 * dropped silently. USB_AUDIO_MAX_PACKET_BYTES below is what stops the descriptor from
 * offering them; without it the host is free to request 96 kHz, as the CDC log showed. The
 * aligned alternative is HW_SAMPR_CAPS itself, which is how every other target expresses
 * "the hardware cannot carry these rates" -- but that knob feeds local playback and
 * recording as well, so it costs 88.2/96 kHz off the player, not just off the DAC.
 */
#define USB_AUDIO_BUFFER_SIZE 320

/* Advertise only the sample rates whose per-frame payload (2 channels x 16-bit = 4 bytes)
 * fits the armed packet. The class default of 0 advertises all of hw_freq_sampr, as every
 * other target does; here it follows the buffer size so the two cannot drift apart. */
#define USB_AUDIO_MAX_PACKET_BYTES  USB_AUDIO_BUFFER_SIZE

/* Cushion, declared in slots (one slot = one full-speed packet = one ms at any rate this
 * device negotiates). Unit convention follows target/hosted/hiby/usb-dac-hiby.c, which
 * declares DAC_RING_FRAMES 8192 with "~186 ms at 44.1 kHz" as the comment: the buffer is
 * declared in the unit the code indexes by, and wall clock is written down for the human.
 *
 * The mixer consumes mix_frame_size (MIX_FRAME_SAMPLES 256 frames = 1024 bytes) per pass, a
 * pass every 256 frames, i.e. 5.33 slots every 5.33 ms with a slot holding one packet. The
 * restart gate is therefore a floor that one pass sits below: with the 22-slot gate below the
 * trough is roughly 16.7 slots, versus the 10.7 the previous 16/32 geometry allowed. The gate
 * is only meaningful in wall clock -- a slot's width comes from the arm -- so widening the arm
 * means dividing the gate by the same factor or the cushion silently changes with it.
 *
 * Arming two packets per transfer was tried once fairly, with the gate divided in half so
 * nothing else moved, and it changed nothing: Frames dropped stayed at ~10/s and the queue sat
 * exactly on the new gate (Min/Max 6/11). That outcome is itself the measurement -- had the
 * core delivered two packets per completion, the frame number would have advanced by 2 each
 * time and the class's adjacency test would have counted ~500 drops a second rather than 10.
 * So this core retires every isochronous packet as its own transfer whatever PKTCNT holds, and
 * a wider arm cannot hold the endpoint open across a frame boundary. It also costs cushion: a
 * trough of 6 two-millisecond slots is 12 ms where the one-packet geometry gives 17 ms. An
 * even earlier revision armed four packets and cut NR_BUFFERS to 12 with a gate of 6 at the
 * same time, so the 110 -> 553 underflow cycles it produced measured the shrunken ring, not
 * the arm; that result is void, and the fair re-test above is what replaced it.
 *
 * Cost is deliberate and should be re-checked after any change: NR_BUFFERS *
 * REAL_DSP_BUF_SIZE is 48 * 1280 = 61,440 bytes, requested from core_alloc() in
 * usb_audio_request_buf(). The stride does not follow the arm -- REAL_DSP_BUF_SIZE comes from
 * BUFFER_SIZE*4, which is why usb_audio.c's slot array already tolerates a completion carrying
 * two packets -- so a wider arm would cost queue width, not memory.
 * usb_audio_request_buf(). core_alloc maps onto the audiobuffer, which on this board is the
 * 4 MB buflib arena (MEMORY.md section 3), NOT the PSRAM window. ALLOC FAILED! on the USB
 * Audio debug page is the live check that the request was satisfied.
 */
#define NR_BUFFERS               48  /* 48 slots: ~48 ms at 48 kHz, one packet per slot */

/* This gate is also the feedback controller's setpoint. usb_audio.c forms the command as
 *
 *     samples_fb = 48 - (queue - MINIMUM_BUFFERS_QUEUED)/4, clamped to +/-1 sample
 *
 * so a gate above the queue's natural equilibrium leaves the controller permanently pressed
 * toward its +1 clamp, modulating the host's send rate by up to 1% -- which is audible as
 * warble on sustained material, invisible in digital silence, and independent of volume.
 * Measured on this board with a gate of 24: the queue sits at 22.5-23.5, Samples used per
 * Frame floats between 48.5 and 48.9 rather than settling, and a burst-marker capture through
 * the speaker shows 0.17 ms of peak-to-peak time-base swing over 200 ms windows, about
 * +/-0.35%, which is the hunting itself.
 *
 * The device also plays slightly fast: a 1 kHz tone measured 1000.33 Hz against the host's own
 * capture clock, i.e. the DAC consumes 48,016 frames/s rather than 48,000 (+333 ppm). So the
 * command that actually holds the queue is 48.016, not 48.000 -- a steady demand of 0.016
 * samples per frame, comfortably inside the clamp and 0.6 cents, which is not a pitch problem.
 * It does mean the nominal in samples_base is wrong by that much, and any residual drift here
 * should be read against 48.016 rather than 48.
 *
 * 22 was meant to put the setpoint at the measured equilibrium, but the queue never gets
 * there: with this gate it averages 18-19 (Buffers filled), so the error term is about -3.5
 * slots and the command sits near 48.875 -- just inside the clamp, held there to make up for
 * the ~2% of frames that never arrive. Demand 48.875 against consumption 48.016 is a +1.79%
 * surplus, which is the loss. The gate is simply unreachable while the clamp is one sample,
 * which is why the controller reads as permanently pressed rather than hunting.
 *
 * 22 is where the gate belongs. The queue cannot reach the setpoint because roughly 2% of
 * frames never arrive, and the controller's law is
 *
 *     samples_fb = samples_base - (queue - gate)/4, clamped to +/-1 sample
 *
 * so in steady state the deficit is set by the loss, not by the gate: queue = gate - 4L, with
 * L about 0.875 frames a frame here. Two readings from one build family confirm it -- gate 22
 * gave Buffers filled 18-19, gate 19 gave 15-16, both a deficit of ~3.5 slots and both the
 * same command near 48.875. Buffers filled is therefore an output of the loss, not a target:
 * dropping the gate moved the whole queue down, changed nothing else, and cost 3 ms of cushion
 * and re-prebuffer length.
 *
 * The clamp also sets the ceiling. With the limit at one sample the controller can compensate
 * at most a 4-slot deficit, so 22 is the highest gate that stays off the clamp. 24 presses it
 * -- and the queue sitting at 22.5-23.5 under a gate of 24, with Samples used per Frame
 * floating between 48.5 and 48.9 rather than settling, is exactly that saturation: the host's
 * rate pinned at +/-1 whatever the queue does, audible as warble on sustained material. A
 * deeper cushion needs the gate and usb_audio_target_fb_limit_samples() raised together.
 *
 * The deficit is a usable measurement: L = (gate - Buffers filled)/4 frames per frame, so on
 * any build a single debug-page glance gives the loss rate -- here 0.875, i.e. ~875 frames/s,
 * which agrees with the ~950/s derived independently from the host's hw_ptr against the
 * measured DAC consumption.
 */
#define MINIMUM_BUFFERS_QUEUED   22  /* restart gate, mute length, and the controller setpoint */

/* Re-arm the isochronous OUT endpoint before copying the completed packet, using two
 * alternating receive buffers (usb_audio.c's USB_AUDIO_RX_PINGPONG).
 *
 * Measured: no effect on the drops. Frames dropped stayed at ~10/s and Samples used per Frame
 * near 48.7, unchanged from the single-buffer ordering -- and the two-packet arm, which widens
 * the same window instead of shortening it, also did nothing. Do not cite this as a latency
 * fix. It is kept because handing the hardware its next buffer before touching the payload is
 * the shape TinyUSB's audio_device.c, espressif's usb_device_uac and
 * firmware/target/hosted/hiby/usb-dac-hiby.c all use.
 *
 * Cost is zero: the second span comes out of the same borrowed storage transfer buffer
 * (usb-xtensa.c's usb_audio_target_rx_buffer()), which is 2 * max(read, write) = 1024 bytes
 * and carries 2 * 320 = 640. Both halves are 32-byte aligned because REAL_BUF_SIZE is.
 *
 * usb_audio.c exposes USB_AUDIO_RX_ARM_SIZE for a wider arm; it is deliberately left at its
 * default here, one packet, for the reasons recorded against NR_BUFFERS above.
 */
#define USB_AUDIO_RX_PINGPONG    1

/* The frame-parity PID assertion in usb_dw_epstart() is load-bearing on this core, not an
 * artifact. Dropping it -- arming isochronous OUT without SETD0PIDEF/SETD1PIDOF, the shape
 * TinyUSB uses because it programs the PID once at edpt_open instead of per transfer -- was
 * measured on this board: Frames dropped went from ~10/s to ~100/s and the queue bottomed out
 * at Min 1 every time. Each isochronous OUT transfer therefore needs an explicit expectation
 * written with its arm. Do not try to skip it again. */

/* The isochronous completion path runs the codec's DSP chain -- usb_audio.c's dsp_process()
 * on the instance it takes from dsp_get_config(CODEC_IDX_AUDIO) -- rather than queueing
 * packets verbatim. A bypass macro was in place while the noise was being localised and is
 * gone now: the transport measured innocent, so the chain is restored to match every other
 * target.
 *
 * The cost is per-packet work inside the USB interrupt at ~1000/s. If Frames dropped rises
 * once this is back, the response is to move that work out of the handler -- a pump thread,
 * as firmware/target/hosted/hiby/usb-dac-hiby.c does -- not to skip the chain.
 *
 * Note usb_audio.c sets dsp_buf_size[] from dsp_process()'s output count, and the class
 * documents that samples consumed and produced need not correspond: a packet the chain
 * absorbs lands as a zero-length slot. That is what usb_audio_target_get_more() exists to
 * skip, rather than tearing the mixer channel down and muting for MINIMUM_BUFFERS_QUEUED
 * every time one appears.
 */
/* Rockbox's own vendor id, deliberately not Espressif's 0x303A/0x1001: that pair is
 * what the ROM's USB-Serial-JTAG reports, so a kernel log could not tell the console
 * from this device -- and both appear on the same port within seconds of each other
 * during bring-up. */
/* High/speed interpacket timeout calibration, 7 (the maximum). TinyUSB's
 * dwc2_core_init() sets this on every DWC2 including the esp32s2/esp32s3 internal PHY,
 * with the databook note that it exists to absorb the delay a PHY introduces when it
 * drives the line state -- which varies per PHY. Rockbox's driver defaults it to 0.
 * The driver writes GUSBCFG wholesale, so the field has to arrive pre-shifted
 * (bits 30:26). */
#define USB_DW_TOUTCAL            (7u << 26)

#define USB_VENDOR_ID  0x1234       /* Rockbox */
#define USB_PRODUCT_ID 0x4002       /* distinct from the USB-Serial-JTAG device */
/* DMA buffers must start on a cache line: committing or invalidating a range would
 * otherwise drag lines belonging to other objects along. */
#define USB_DEVBSS_ATTR __attribute__((aligned(32)))
/* Charge mode as the factory default. Rockbox's default is mass storage, which
 * on this board is the wrong first answer: usb_slave_mode() unmounts the volumes,
 * and taking the OTG core onto GPIO19/20 removes USB-Serial-JTAG -- which is both
 * the bring-up console and the flash port. The user picks mass storage from
 * General -> USB mode when they want the card exposed; the profile then becomes
 * CDC+HID+MSC, and charge mode with "USB DAC: while charge only" gives
 * CDC+HID+Audio. Neither can be built at once: the core has four dedicated IN
 * FIFOs and both profiles need exactly four.
 *
 * Rockbox's own default is mass storage, and this restores it: insertion takes the
 * player to USB_INSERTED, which is the state usb.c uses to enable the mass-storage
 * driver (firmware/usb.c:244), so the profile is CDC+HID+MSC. Charge mode stays
 * reachable two ways, as on every other HAVE_USB_POWER target -- the power button at
 * insertion toggles usb_power_only (firmware/usb.c:492), and Settings -> General ->
 * USB has the choice. With "USB DAC: while charge only" the charge profile is
 * CDC+HID+Audio. Neither profile can be built at once: the core has four dedicated IN
 * FIFOs and both need exactly four.
 *
 * Making this the default means the pads move to the OTG core as soon as a cable is
 * sensed, so flashing no longer has a boot-time window on USJ. That is what
 * `make usbflash` is for: one byte on the CDC OUT endpoint hands the port back and
 * asks the ROM for download mode. See AGENTS.md.
 */
#define USBMODE_DEFAULT USB_MODE_MASS_STORAGE

/* The mode choice is only built when this is defined (apps/settings_list.c:2546);
 * without it USBMODE_DEFAULT is inert and usb.c's power-monitoring path does not
 * exist. It also brings the charging indication into the status bar, which is a screen
 * change: look at it rather than assume it.
 */
#define HAVE_USB_POWER
/* ALLOCATE_BUFFER_SIZE is 2 * max(read, write), and it is static because the DWC's
 * DMA is an AHB master on the internal bus and cannot reach the PSRAM window where
 * Rockbox's arena lives. Full speed caps the useful size anyway: the default 64 KB
 * per direction would need 128 KB of the 337 KB static region, which is what
 * overflowed dram0_0_seg by 152,968 bytes. */
#define USB_READ_BUFFER_SIZE  (4 * 1024)
#define USB_WRITE_BUFFER_SIZE (4 * 1024)

/* Audio: ES8311 codec on I2S1. Every rate Rockbox supports is reachable from the
 * 40 MHz XTAL; only 44100 and 88200 are off by ~7 Hz (0.000064 %). */
#define HW_SAMPR_CAPS SAMPR_CAP_ALL_96

#define HAVE_ES8311

/* The ES7210 ADC sits on the same I2S bus as the codec, on the receiver half of a
 * second I2S port. See firmware/target/xtensa/pcm-xtensa.c. */
/* NOTE for future bisects: HAVE_RECORDING cannot be toggled to isolate a
 * regression. struct codec_api in lib/rbcodec/codecs/codecs.h has members inside
 * #ifdef HAVE_RECORDING, so switching it off moves the offset of every member
 * after them -- and the codecs are separately linked .codec images that call
 * through those pointers at fixed offsets. Turning it off makes every codec on
 * the card call the wrong function: codec_load_file starts returning -1 and the
 * UI misbehaves, which looks like a new fault but is only the ABI shift. Any
 * bisect that touches this define must rebuild and recopy the codecs too. */
#define HAVE_RECORDING

/* The ES7210 is the only input the board has. */
#define INPUT_SRC_CAPS SRC_CAP_MIC

/* Rockbox only creates the "rec mic gain" setting when DEFAULT_REC_MIC_GAIN is
 * defined, and settings_list.c defines it solely for UDA1380, TLV320, WM8975,
 * WM1870, WM8758 and WM8731 targets. This board is an ES8311 with an ES7210 on
 * the receive half, so none of those apply: the setting was never created,
 * global_settings.rec_mic_gain stayed 0, and es7210_init() therefore programmed
 * gain code 0 -- 0 dB, the bottom of the ladder. The MeowKit reference firmware
 * defaults its ES7210 to ES7210_GAIN_30DB, and REG43/44 bits 3:0 step 3 dB from
 * 0 to 30 dB, which is what gain_code() maps onto. Without this the captured
 * samples peak around 10-13 out of 32767, roughly -68 dBFS, and the VU meter
 * correctly shows nothing at all. */
#define DEFAULT_REC_MIC_GAIN 30

/* Recording shares the I2S clock with playback, so the reachable rates are the
 * same set. */
#define REC_SAMPR_CAPS HW_SAMPR_CAPS

/* The NS4150B amplifier sits between the codec and the speaker and is enabled from
 * the PCA9557 expander. Without it there is no sound at all. */
#define HAVE_SPEAKER

/* One volume register for both channels, so the codec has mono volume. The
 * AUDIOHW_SETTING itself lives in es8311.h, which audiohw.h includes after
 * defining the macro. */
#define AUDIOHW_CAPS MONO_VOL_CAP

/* logf, deliberately. Once GPIO19/20 go to the OTG core there is no printf: the
 * console lives on USB-Serial-JTAG, and IDF's stdout goes with it. logf() pushes
 * straight to usb_serial_send() under USB_ENABLE_SERIAL (firmware/logf.c:246), so
 * the trace continues over the CDC endpoint the moment the class enumerates --
 * which is the only way to see usb_core.c and usb-designware.c messages during the
 * attach. The 16 KB logfbuffer is already PSRAM_BSS_ATTR, so this costs nothing in
 * internal SRAM; it does take 16 KB of the ~26 KB idle in the PSRAM window.
 * Precedent for enabling it from a model header rather than the build mode:
 * ipod6g.h, ipodnano3g.h, ipodnano4g.h.
 */
#define ROCKBOX_HAS_LOGF 1
