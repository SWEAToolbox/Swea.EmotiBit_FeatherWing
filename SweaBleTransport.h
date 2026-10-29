/**************************************************************************/
/*!
    @file     SweaBleTransport.h

    SWEA: BLE transport that replaces EmotiBitWiFi when built with -DSWEA_BLE.
    It exposes the subset of the EmotiBitWiFi API that EmotiBit.cpp uses, so
    the upstream sampling, SD logging and packet code is untouched.

    Protocol: docs/SWEA_BLE_PROTOCOL.md (version 1, draft).
    Session mode (firmware 0.11): START_STREAM opens a session that survives disconnects; notifications are then
    built with a continuous seq even without a phone and kept in /swea/outbox.bin on the SD card until STOP.
    Wi-Fi is never initialised in this build.
*/
/**************************************************************************/
#pragma once
#ifdef SWEA_BLE

#include <Arduino.h>
#include <FS.h>
#include "SweaBleCodec.h"

// EmotiBit.cpp compares status() against the Arduino Wi-Fi constant. No Wi-Fi header is
// included in this build, so provide the one value that is used.
#ifndef WL_CONNECTED
#define WL_CONNECTED 3
#endif
#ifndef WL_DISCONNECTED
#define WL_DISCONNECTED 6
#endif

class SweaBleTransport
{
public:
	static const uint8_t PROTOCOL_VERSION = 1;
	static const uint8_t MAX_CREDENTIALS = 12;	// kept only so shared code compiles

	// ---- API used by EmotiBit.cpp (same names as EmotiBitWiFi) -------------------------
	uint8_t begin(int32_t timeout = 61500, uint8_t maxAttemptsPerCred = 2, uint16_t attemptDelay = 3000);
	void end();
	int8_t disconnect();
	int8_t update(String &syncPackets, uint16_t &syncPacketCounter);
	uint8_t readControl(String &packet);
	int8_t sendData(const String &message);
	int8_t addCredential(const String &ssid, const String &userid, const String &username, const String &password);
	void setDeviceId(const String emotibitDeviceId);
	bool isConnected();
	bool isOff();
	void updateStatus();
	uint8_t status(bool update = true);
	uint8_t getNumCredentials();
	bool isEnterpriseNetworkListed();
	static uint8_t getMaxNumCredentialAllowed();

	// ---- SWEA specific ---------------------------------------------------------------
	bool isStreaming() const { return _session; }	// a session is active (set by START_STREAM, cleared by STOP_STREAM; survives a disconnect)

	/** The SD card file system (owned by EmotiBit; the Arduino SD object on the ESP32) for the session outbox. Without it the outbox is unavailable. Call once the card is up. */
	void setSd(fs::FS *sd) { _sd = sd; }

	/** Opens the pairing window for this many seconds (button press, boot without bonds, ENTER_PAIRING), prints the passkey on
	 *  Serial and lifts a PairingGuard lockout. Since firmware 0.12 an unknown phone can pair with the correct passkey even
	 *  while the window is closed; the window only extends the time a phone has to finish the pairing. */
	void openPairing(uint16_t seconds = 120);
	bool isPairingOpen() const { return _pairingOpen; }

	// Called from the BLE callbacks (NimBLE host task). They only set flags or queue work for update().
	void onConnected(uint16_t connHandle);
	// An unknown phone is asked to pair only after this long: our app starts the pairing itself right after connecting (Android then asks
	// for the passkey and the app answers); a Security Request from the board first would make Android show a consent prompt instead.
	void requestSecurityLater() { _securityAtMs = millis() + 10000; }
	void onDisconnected();
	void onEncryptionChange(bool encrypted);
	void onMtu(uint16_t mtu);
	void onSubscribe(bool subscribed);
	void onControlWrite(const uint8_t *data, size_t len);
	uint32_t passkey() const { return _passkey; }

private:
	static const uint8_t CMD_QUEUE = 4;
	static const uint8_t CMD_MAX_LEN = 8;
	struct Command { uint8_t len; uint8_t d[CMD_MAX_LEN]; };

	String _deviceId = "";
	volatile bool _on = false;
	volatile bool _connected = false;
	volatile bool _encrypted = false;
	volatile bool _session = false;		// session active: notifications are built and kept even without a phone
	volatile uint16_t _mtu = 23;
	volatile uint16_t _connHandle = 0xFFFF;
	volatile bool _dataSubscribed = false;
	volatile bool _advPending = false;	// advertising starts from the first update() of the main loop
	volatile bool _streamGo = false;	// live data and replay wait for START_STREAM or RESUME_FROM on each connection
	volatile bool _pairingOpen = false;
	uint32_t _pairingUntil = 0;
	uint32_t _passkey = 0;

	uint16_t _seq = 0;
	uint32_t _lastStatusMs = 0;
	uint8_t _profile = 0;
	uint8_t _batteryPct = 0xFF;			// 0xFF = unknown until a B% record has been seen
	swea::RecordRing _ring;
	uint8_t *_ringMem = nullptr;
	swea::FrameWindow _win;				// last sent notifications, for RESUME_FROM
	uint8_t *_winMem = nullptr;
	bool _replay = false;
	uint16_t _replaySeq = 0, _replayEnd = 0;
	bool _gap = false;					// frames were lost: a RESUME_FROM asked for frames no longer held, or the outbox threw some away
	bool _lost = false;					// notifications were thrown away (outbox full, no outbox) in this session
	bool _finished = false;				// FINISH received: no new values are taken in
	bool _sessionFrames = false;		// the session has produced at least one notification

	// SD outbox (/swea/outbox.bin): records [seq:u16 len:u8 bytes] of notifications nobody received live.
	fs::FS *_sd = nullptr;
	fs::File _outFile;
	bool _outOpen = false, _outFail = false, _outSeeding = false;
	swea::OutboxIndex _outIdx;
	uint32_t *_outIdxMem = nullptr;
	uint32_t _outSyncMs = 0;
	bool _replayPosValid = false;		// next outbox record to replay sits at _replayPos (sequential reads need no search)
	uint32_t _replayPos = 0;
	uint16_t _replayPosSeq = 0;
	bool outboxUsable();
	bool outboxAppend(uint16_t seq, const uint8_t *data, size_t len);
	void outboxSeedFromWindow(uint16_t beforeSeq);
	bool outboxRead(uint16_t seq, uint8_t *out, size_t &len);
	void outboxClear();
	void beginSession();
	void endSession();
	bool produceFrame(size_t cap);
	void flushRing();
	swea::ParsedPacket _pp;
	uint32_t _notifyFailures = 0;
	uint32_t _lastNotifyMs = 0;
	uint32_t _connectedAtMs = 0;
	uint32_t _securityAtMs = 0;
	bool _recording = false;				// as commanded over BLE (RECORD); the SD card state is owned by EmotiBit
	static const uint8_t CTRL_OUT = 4;
	String _ctrlOut[CTRL_OUT];			// text control packets that EmotiBit's own parser consumes via readControl()
	uint8_t _ctrlHead = 0, _ctrlTail = 0;
	bool pushControlPacket(const String &packet);

	Command _cmds[CMD_QUEUE];
	volatile uint8_t _cmdHead = 0, _cmdTail = 0;
	portMUX_TYPE _cmdMux = portMUX_INITIALIZER_UNLOCKED;

	void handleLine(const char *s, size_t len);
	void drain();
	void processCommands();
	void execute(const Command &c);
	void loadPasskey();

	void startBle();
	void stopBle();
	bool notifyData(const uint8_t *buf, size_t len);
	void sendStatus(bool notify);
	void sendControlReply(uint8_t opcode, uint8_t status, const uint8_t *args, size_t n);
};

#endif // SWEA_BLE
