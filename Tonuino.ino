/* tonUINO - A DIY Musicbox
 *
 * Copyright © 2019-2020 Thorsten Voß
 *                  2020 Till Smejkal
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <DFMiniMp3.h>
#include <MFRC522.h>
#include <JC_Button.h>

/* Uncomment this if you have extra buttons for volume up and down */
#define FIVEBUTTONS

/* Uncomment this if you have a status LED on in your system */
#define STATUS_LED

/* Uncomment one of the below to define what should be done when shutting
   down the system */
//#define USE_SLEEP_MODE
#define USE_SHUTDOWN_PIN

/* Uncomment if the system still has an unused input pin */
#define HAS_UNUSED_INPUT


/* Update these according to your hardware setup */
/* Pins usage for the buttons */
#define PLAY_BUTTON_PIN A1
#define NEXT_BUTTON_PIN A2
#define PREV_BUTTON_PIN A3
#ifdef FIVEBUTTONS
#define VOLUP_BUTTON_PIN A4
#define VOLDOWN_BUTTON_PIN A5
#endif

/* Pin usage for the MP3 player module */
#define DFPLAYER_INPUT_PIN 2
#define DFPLAYER_OUTPUT_PIN 3
#define DFPLAYER_BUSY_PIN 4

/* Pin usage for the RFID module */
#define RFID_RST_PIN 9
#define RFID_SS_PIN 10

/* Pin usage for the battery voltage input */
#define VOLTAGE_PIN A0

/* Pin usage for other status LED */
#ifdef STATUS_LED
#define STATUS_LED_PIN 8
#endif

/* Pin usage for the shutdown pin */
#ifdef USE_SHUTDOWN_PIN
#define SHUTDOWN_PIN 7
#endif

/* Is there still an unused pin that we can use for seeding our random
   number generator? */
#ifdef HAS_UNUSED_INPUT
#define UNUSED_INPUT_PIN A7
#endif


/* Additional includes needed for optional features */
#ifdef STATUS_LED
#include <FastLED.h>
#endif

#ifdef USE_SLEEP_MODE
#include <avr/sleep.h>
#endif


/* Update theses according to your behavioral requirements */
/* The duration the system should wait before shutting down */
static const uint32_t STANDBY_TIME_MS = 600000;     // == 10 minutes

/* The duration the system waits before stopping a paused playback */
static const uint32_t STOP_PLAYBACK_MS = 60000;     // == 1 minute

/* The duration the system waits before exiting from the admin menu
   when there is no further input */
static const uint32_t ABORT_MENU_MS = 60000;        // == 1 minute

#ifdef STATUS_LED
/* How long the system should wait between consecutive outputs of
   the current battery voltage. */
static const uint32_t PRINT_VOLTAGE_MS = 120000;     // == 2 minutes
#endif

/* The duration after which a button press is considered a long press */
static const uint32_t LONG_PRESS_MS = 1000;         // == 1 second


/* Forward declare very important free standing functions. */
void shutdown();


/****
 * Generic event loop interface/implementation
 ****/
using callback_func = bool(*)();

class Event
{
   protected:
    Event *next;

    callback_func handle;

    friend class EventManager;

   public:
    Event(const callback_func &f) : next{nullptr}, handle{f} {}

    virtual ~Event() = default;

   public:
    virtual void check_and_handle(uint32_t ts) = 0;
    virtual void clear() {}
};

class EventManager
{
   private:
    Event *events;

   public:
    EventManager() : events{nullptr} {}

    EventManager& add(Event *e) 
    {
        e->next = events;
        events = e;

        Serial.print(F("Event added: "));
        Serial.println(reinterpret_cast<uint16_t>(e), HEX);

        return *this;
    }

    EventManager& remove(const Event *e)
    {
        Event **p = &events;
        for (Event *n=*p; n; p = &(n->next), n = n->next) {
            if (e == n) {
                *p = n->next;
                break;
            }
        }

        Serial.print(F("Event removed: "));
        Serial.println(reinterpret_cast<uint16_t>(e), HEX);

        return *this;
    }

    void loop()
    {
        uint32_t ms = millis();

        for (Event *e = events; e; e = e->next)
            e->check_and_handle(ms);

        for (Event *e = events; e; e = e->next)
            e->clear();
    }
};

/***
 * Specific Events
 ***/

/* This helper class is used to have a constant (aka. per event loop iteration) view on the JC_Button
   button's internal state
*/
class ButtonWrapper
{
   private:
    Button bt;
    bool needs_update;

   public:
    ButtonWrapper(uint8_t pin) : bt{pin}, needs_update{true} {}

    void begin() { bt.begin(); }

    void read() {
        if (needs_update) {
            bt.read();
            needs_update = false;
        }
    }

    void clear() { needs_update = true; }

    bool isPressed() { return bt.isPressed(); }
    bool isReleased() { return bt.isReleased(); }

    bool wasPressed() { return bt.wasPressed(); }
    bool wasReleased() { return bt.wasReleased(); }

    bool pressedFor(uint32_t ms) { return bt.pressedFor(ms); }
    bool releasedFor(uint32_t ms) { return bt.releasedFor(ms); }

    uint32_t lastChange() { return bt.lastChange(); }
};

/* Helper class to sort out the problem that when you press a button long
   there will be a ButtonLongPressedEvent triggered and also a ButtonPressedEvent
   once the button is released again. This helper class will internally track
   if a button was already pressed long and then only forward one call as well as
   no call when the button is released again. */
class ButtonPressDispatcher
{
   private:
    callback_func short_press;
    callback_func long_press;

    bool was_long_pressed;

   public:
    ButtonPressDispatcher(const callback_func &sp, const callback_func &lp) : 
        short_press{sp}, long_press{lp}, was_long_pressed{false}
    {}

    bool long_pressed()
    {
        if (!was_long_pressed) {
            was_long_pressed = true;
            return long_press();
        }

        return true;
    }

    bool short_pressed()
    {
        if (was_long_pressed) {
            was_long_pressed = false;
            return true;
        }

        return short_press();
    }
};


class ButtonPressedEvent : public Event
{
   private:
    ButtonWrapper *bt;

   public:
    ButtonPressedEvent(ButtonWrapper *bt, const callback_func &f) : Event{f}, bt{bt} {}

    void check_and_handle(uint32_t ms)
    {
        bt->read();
        if (bt->wasReleased())
            this->handle();
    }

    void clear()
    {
        bt->clear();
    }
};

class ButtonLongPressedEvent : public Event
{
   private:
    ButtonWrapper *bt;
    uint32_t long_press_ms;

   public:
    ButtonLongPressedEvent(ButtonWrapper *bt, const callback_func &f, uint32_t long_press_ms = LONG_PRESS_MS)
        : Event{f}, bt{bt}, long_press_ms{long_press_ms}
    {}

    void check_and_handle(uint32_t ms)
    {
        bt->read();
        if (bt->pressedFor(long_press_ms))
            this->handle();
    }

    void clear()
    {
        bt->clear();
    }
};

class AdminModeEvent : public Event
{
   private:
    ButtonWrapper *play, *next, *prev;
    uint32_t long_press_ms;

   public:
    AdminModeEvent(ButtonWrapper *play, ButtonWrapper *next, ButtonWrapper *prev, const callback_func &f,
                   uint32_t long_press_ms = LONG_PRESS_MS)
        : Event{f}, play{play}, next{next}, prev{prev}, long_press_ms{long_press_ms}
    {}

    void check_and_handle(uint32_t ms)
    {
        play->read();
        next->read();
        prev->read();
        if (play->pressedFor(long_press_ms) && next->pressedFor(long_press_ms) && prev->pressedFor(long_press_ms))
            this->handle();
    }

    void clear()
    {
        play->clear();
        next->clear();
        prev->clear();
    }
};

/* These classes are helper classes to define different types of analog events
    - edge triggered vs value triggered
    - less vs more vs equal vs ...
*/
template<class Comp>
class ValueTrigger
{
   private:
    int reference;

   public:
    ValueTrigger(int ref) : reference{ref}
    {}

    bool triggered(int val)
    {
        return Comp::compare(val, reference);
    }
};

template<class Comp>
class EdgeTrigger
{
   private:
    int reference;
    bool last_true;

   public:
    EdgeTrigger(int ref) : reference{ref}, last_true{false}
    {}

    bool triggered(int val)
    {
        bool res = Comp::compare(val, reference);

        if (res && !last_true) {
            last_true = true;
            return true;
        } else {
            last_true = res;
            return false;
        }
    }
};

template<class T>
struct EqualComp
{
    static bool compare(const T& a, const T& b) { return a == b; }
};

template<class T>
struct UnEqualComp
{
    static bool compare(const T& a, const T& b) { return a != b; }
};

template<class T>
struct LessComp
{
    static bool compare(const T& a, const T& b) { return a < b; }
};

template<class T>
struct LessEqualComp
{
    static bool compare(const T& a, const T& b) { return a <= b; }
};

template<class T>
struct MoreComp
{
    static bool compare(const T& a, const T& b) { return a > b; }
};

template<class T>
struct MoreEqualComp
{
    static bool compare(const T& a, const T& b) { return a >= b; }
};

template<template<typename> class Trigger, template<typename> class Comperator>
class AnalogEvent : public Event
{
   private:
    int pin;
    Trigger<Comperator<int>> trigger;

   public:
    AnalogEvent(int pin, int reference, const callback_func &f) : Event{f},
        pin{pin}, trigger{reference}
    {}

    void check_and_handle(uint32_t ms)
    {
        int val = analogRead(pin);

        if (trigger.triggered(val))
            this->handle();
    }
};

class TimerEvent : public Event
{
   protected:
    uint32_t duration;
    bool repeated;
    uint32_t next_ms;

   public:
    TimerEvent(uint32_t duration, const callback_func &f, bool repeated=false) : Event{f},
        duration{duration}, repeated{repeated}, next_ms{0}
    {
        next_ms = millis() + duration;
    }

    void check_and_handle(uint32_t ms)
    {
        if (next_ms != 0 && ms >= next_ms) {
            this->handle();

            if (repeated)
                next_ms += duration;
            else
                next_ms = 0;
        }
    }

    void reset()
    {
        /* Move the timer forward on reset */
        next_ms = millis() + duration;
    }
};

/* Forward declare the RFIDReader class */
class RFIDReader;

class NewRFIDCardEvent : public Event
{
   private:
    RFIDReader *reader;

   public:
    NewRFIDCardEvent(const callback_func &f) : Event{f}, reader{nullptr}
    {}

    void rfid_reader(RFIDReader *r) { reader = r; }

    void check_and_handle(uint32_t ms);
};

class TrackFinishedEvent : public Event
{
   private:
    bool triggered;
    uint16_t last_track;

   public:
    TrackFinishedEvent(const callback_func &f) : Event{f}, triggered{false}, last_track{0}
    {}

    void trigger(uint16_t track)
    {
        triggered = true;
        last_track = track;
    }

    void check_and_handle(uint32_t ms)
    {
        if (triggered)
            this->handle();
    }

    void clear() { triggered = false; }
};

class SerialEvent : public Event
{
   public:
    SerialEvent(const callback_func &f) : Event{f}
    {}

    void check_and_handle(uint32_t ms)
    {
        if (Serial.available() > 0)
            this->handle();
    }
};


/****
 * Notification class for the DFMiniMP3 module
 ****/
class MP3Notification
{
   public:
    static TrackFinishedEvent *e;

   public:
    static void OnError(uint16_t ec)
    {
        Serial.print(F("DFMiniPlayer encountered an error:"));
        Serial.println(ec);
    }

    static void OnPlayFinished(uint16_t track)
    {
        e->trigger(track);
    }

    static void OnCardOnline(uint16_t code)
    {}

    static void OnUsbOnline(uint16_t code)
    {}

    static void OnCardInserted(uint16_t code)
    {}

    static void OnUsbInserted(uint16_t code)
    {}

    static void OnCardRemoved(uint16_t code)
    {}

    static void OnUsbRemoved(uint16_t code)
    {}
};

using MP3Player = DFMiniMp3<SoftwareSerial, MP3Notification>;


/****
 * RFID module management
 ****/
class RFIDCard
{
   private:
    static const uint32_t cookie = 0x1337b347;
    static const byte version = 0x02;

   public:
    enum class Mode : uint8_t {
        ALBUM = 2,
        PARTY = 3,
        ONE = 4,
        AUDIOBOOK = 5,
    };

    uint8_t folder;
    Mode mode;
    uint8_t special;
    uint8_t special2;

    bool deserialize(byte b[18])
    {
        /* Before extracting the data check the card's cookie and version */
        uint32_t c = (static_cast<uint32_t>(b[0]) << 24) |
                     (static_cast<uint32_t>(b[1]) << 16) |
                     (static_cast<uint32_t>(b[2]) << 8)  |
                     static_cast<uint32_t>(b[3]);

        if (c != cookie)
            return false;
        if (b[4] != version)
            return false;

        folder = b[5];
        mode = static_cast<Mode>(b[6]);
        special = b[7];
        special2 = b[8];

        return true;
    }

    void serialize(byte b[16]) const
    {
        /* Zero the buffer out first */
        memset(b, 0, 16*sizeof(byte));

        /* Write our cookie */
        b[0] = static_cast<byte>(cookie >> 24);
        b[1] = static_cast<byte>(cookie >> 16);
        b[2] = static_cast<byte>(cookie >> 8);
        b[3] = static_cast<byte>(cookie);

        /* Version */
        b[4] = version;

        /* Next is all the real data */
        b[5] = folder;
        b[6] = static_cast<byte>(mode);
        b[7] = special;
        b[8] = special2;
    }
};

class RFIDReader
{
   private:
    static const constexpr byte sector = 1;
    static const constexpr byte auth_block = sector * 4 + 3;    // Always the last block in a 4-block sector contains
                                                                // the authentication key.
    static const constexpr byte data_block = sector * 4;        // We want to write our data in the first block of a
                                                                // sector;

    MFRC522 mfrc522;
    MFRC522::MIFARE_Key key;

   public:
    RFIDReader(byte ss_pin, byte rst_pin) : mfrc522{ss_pin, rst_pin}
    {
        /* Initialize the used key */
        for (byte i = 0; i < 6; ++i)
            key.keyByte[i] = 0xFF;
    }

    void begin()
    {
        SPI.begin();        // Init SPI bus
        mfrc522.PCD_Init(); // Init MFRC522
    }

    void print_version()
    {
        mfrc522.PCD_DumpVersionToSerial();
    }

    bool read_card(RFIDCard &card)
    {
        MFRC522::PICC_Type mifare_type = mfrc522.PICC_GetType(mfrc522.uid.sak);
        MFRC522::StatusCode status = MFRC522::STATUS_ERROR;

        byte buffer[18];
        byte size = sizeof(buffer);
        memset(buffer, 0, 18*sizeof(byte));

        if ((mifare_type == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
            (mifare_type == MFRC522::PICC_TYPE_MIFARE_1K ) ||
            (mifare_type == MFRC522::PICC_TYPE_MIFARE_4K ) )
        {
            Serial.println(F("Authenticating using key A"));
            status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, auth_block, &key, &(mfrc522.uid));
        } else {
            Serial.println(F("Unsupported MIFARE type"));
            return false;
        }

        if (status != MFRC522::STATUS_OK) {
            Serial.print(F("PCD_Authenticate() failed: "));
            Serial.println(mfrc522.GetStatusCodeName(status));
            return false;
        }

        status = mfrc522.MIFARE_Read(data_block, buffer, &size);

        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();

        if (status != MFRC522::STATUS_OK) {
            Serial.print(F("Read failed: "));
            Serial.println(mfrc522.GetStatusCodeName(status));

            return false;
        }

        return card.deserialize(buffer);
    }

    bool write_card(const RFIDCard &card)
    {
        byte buffer[16];
        MFRC522::StatusCode status = MFRC522::STATUS_ERROR;

        /* Serialize the content of the card. */
        card.serialize(buffer);

        MFRC522::PICC_Type mifare_type = mfrc522.PICC_GetType(mfrc522.uid.sak);

        if ((mifare_type == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
            (mifare_type == MFRC522::PICC_TYPE_MIFARE_1K ) ||
            (mifare_type == MFRC522::PICC_TYPE_MIFARE_4K ) )
        {
            Serial.println(F("Authenticating using key A"));
            status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, auth_block, &key, &(mfrc522.uid));
        } else {
            Serial.println(F("Unsupported MIFARE type"));
            return false;
        }

        if (status != MFRC522::STATUS_OK) {
            Serial.print(F("Authentication failed: "));
            Serial.println(mfrc522.GetStatusCodeName(status));
            return false;
        }

        status = mfrc522.MIFARE_Write(data_block, buffer, 16);

        if (status != MFRC522::STATUS_OK) {
            Serial.print(F("Writing data to card failed: "));
            Serial.println(mfrc522.GetStatusCodeName(status));
        }

        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();

        return status == MFRC522::STATUS_OK;
    }

    bool card_available()
    {
        return (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial());
    }

    void shutdown()
    {
        mfrc522.PCD_AntennaOff();
        mfrc522.PCD_SoftPowerDown();
    }
};

void NewRFIDCardEvent::check_and_handle(uint32_t ms)
{
    if (reader && reader->card_available())
        this->handle();
}

/****
 * Saving/Restoring system settings in the EEPROM
 ****/
class Settings
{
    /**
     * Settings layout in EEPROM:
     * address  content
     * ----------------
     * 0-3      cookie
     * 4        version (currently 1)
     * 5        volume
     * 6        minimum volume
     * 7        maximum volume
     * 8        last folder valid (0 == no, 1 == yes)
     * 9-12     last folder
     * 13       number of folder progresses
     * 14-...   progress audio book folders
     **/

   private:
    static const uint32_t cookie = 0xdeadbeef;
    static const byte version = 0x01;

   private:
    bool last_folder_valid;
    uint8_t last_folder;
    uint8_t last_mode;
    uint8_t last_special;
    uint8_t last_special2;

    struct Progress {
        Progress *next;
        uint8_t folder;
        uint8_t track;
    };

    Progress *progresses;


   public:
    uint8_t volume;
    uint8_t min_volume;
    uint8_t max_volume;

   private:
    bool from_eeprom()
    {
        /* First read the cookie to see if the content can be used as is. */
        uint32_t c = (static_cast<uint32_t>(EEPROM.read(0)) << 24) |
                     (static_cast<uint32_t>(EEPROM.read(1)) << 16) |
                     (static_cast<uint32_t>(EEPROM.read(2)) << 8) |
                     static_cast<uint32_t>(EEPROM.read(3));

        if (cookie != c)
            return false;

        /* Next check the version */
        byte v = EEPROM.read(4);
        if (version != v)
            return false;

        /* Extract the data */
        volume = EEPROM.read(5);
        min_volume = EEPROM.read(6);
        max_volume = EEPROM.read(7);

        last_folder_valid = EEPROM.read(8) == 0x01;
        if (last_folder_valid) {
            last_folder = EEPROM.read(9);
            last_mode = EEPROM.read(10);
            last_special = EEPROM.read(11);
            last_special2 = EEPROM.read(12);
        }

        /* Read the saved audio book progresses */
        byte nr_pogresses = EEPROM.read(13);
        Progress **n = &progresses;
        for (byte i = 0; i < nr_pogresses; ++i) {
            Progress *p = new Progress;

            p->folder = EEPROM.read(14+i*2);
            p->track = EEPROM.read(15+i*2);
            p->next = nullptr;

            *n = p;
            n = &p->next;
        }

        return true;
    }

    void default_init()
    {
        volume = 12;
        min_volume = 5;
        max_volume = 25;
        last_folder_valid = true;
        last_folder = 1;
        last_mode = static_cast<uint8_t>(RFIDCard::Mode::ALBUM);
    }

   public:
    Settings() : progresses{nullptr}
    {
        if (!from_eeprom()) {
            Serial.println(F("Failed to load settings from EEPROM"));
            default_init();
        }
    }

    ~Settings()
    {
        for (Progress *n = progresses; n; n = n->next) {
            Progress *t = n;
            n = n->next;
            delete t;
        }

        progresses = nullptr;
    }

    void to_eeprom()
    {
        /* First write cookie and version in the EEPROM */
        EEPROM.update(0, static_cast<byte>(cookie >> 24));
        EEPROM.update(1, static_cast<byte>(cookie >> 16));
        EEPROM.update(2, static_cast<byte>(cookie >> 8));
        EEPROM.update(3, static_cast<byte>(cookie));
        EEPROM.update(4, version);

        /* Write the data */
        EEPROM.update(5, volume);
        EEPROM.update(6, min_volume);
        EEPROM.update(7, max_volume);
        EEPROM.update(8, last_folder_valid ? 0x01 : 0x00);
        if (last_folder_valid) {
            EEPROM.update(9, last_folder);
            EEPROM.update(10, last_mode);
            EEPROM.update(11, last_special);
            EEPROM.update(12, last_special2);
        }

        /* Write the saved audio book progresses */
        byte i = 0;
        for (Progress *n = progresses; n; n = n->next) {
            EEPROM.update(14+i*2, n->folder);
            EEPROM.update(15+i*2, n->track);
            i++;
        }
        EEPROM.update(13, i);
    }

    bool last_card(RFIDCard &card)
    {
        if (!last_folder_valid)
            return false;

        card.folder = last_folder;
        card.mode = static_cast<RFIDCard::Mode>(last_mode);
        card.special = last_special;
        card.special2 = last_special2;

        return true;
    }

    void save_last_card(const RFIDCard &card)
    {
        last_folder = card.folder;
        last_mode = static_cast<uint8_t>(card.mode);
        last_special = card.special;
        last_special2 = card.special2;
        last_folder_valid = true;
    }

    uint8_t progress(uint8_t folder)
    {
        for (Progress *i = progresses; i; i = i->next) {
            if (i->folder == folder)
                return i->track;
        }

        return 0;
    }

    void progress(uint8_t folder, uint8_t track)
    {
        for (Progress *i = progresses; i; i = i->next) {
            if (i->folder == folder) {
                i->track = track;
                return;
            }
        }

        Progress *n = new Progress;
        n->folder = folder;
        n->track = track;
        n->next = progresses;
        progresses = n;
    }

    void remove_progress(uint8_t folder)
    {
        Progress **p = &progresses;
        for (Progress *n = *p; n; p = &(n->next), n = n->next) {
            if (n->folder == folder) {
                *p = n->next;
                delete n;
                return;
            }
        }
    }
};


/****
 * Generic interface for all the modes in the system
 ****/
class Mode
{
   protected:
    /* "Copy" constructor so that we can extract state from the currently
       active mode instead of building it from scratch every time. */
    Mode(Mode *current)
    {}

   public:
    Mode() = default;

    virtual ~Mode() = default;

   public:
    virtual bool play() { return true; }
    virtual bool stop() { return true; }
    virtual bool next() { return true; }
    virtual bool prev() { return true; }
    virtual bool volume_up() { return true; }
    virtual bool volume_down() { return true; }
    virtual bool track_finished() { return true; }
    virtual bool new_card() { return true; }
    virtual bool timer() { return true; }
    virtual bool admin_mode() { return true; }
    virtual bool is_playing() { return false; }

    template <class M, class ...Args>
    Mode* switch_to(Args... args)
    {
        Mode *m = new M(this, args...);
        delete this;

        return m;
    }
};

#ifdef STATUS_LED
class LEDMode : public Mode
{
   private:
    CRGB *battery_led;

   protected:
    LEDMode(LEDMode *current) : Mode{current}, battery_led{current->battery_led}
    {}

   public:
    LEDMode(CRGB *battery_led) : battery_led{battery_led}
    {}

    ~LEDMode() = default;

    bool battery_high()
    {
        Serial.println(F("Battery level is high."));
        *battery_led = CRGB::Green;
        FastLED.show();

        return true;
    }

    bool battery_low()
    {
        Serial.println(F("Battery level is low"));
        *battery_led = CRGB::Red;
        FastLED.show();

        return true;
    }

    bool battery_critical()
    {
        Serial.println(F("Battery level is critical! Shutting down."));
        shutdown();

        return true;
    }

    template <class M, class ...Args>
    LEDMode* switch_to(Args... args)
    {
        LEDMode *m = new M(this, args...);
        delete this;

        return m;
    }
};

using DefaultMode = LEDMode;
#else
using DefaultMode = Mode;
#endif

/*****
 * System management
 *****/

/***
 * Global wrappers for the different modules
 ***/
static MP3Player *mp3_player;
static RFIDReader *rfid_reader;
static Settings *settings;


#if defined(USE_SHUTDOWN_PIN)
/***
 * Class that implements the necessary features in combination with the
 * shutdown pin scenario. Within its constructor it will set to pin to HIGH
 * or LOW and the corresponding shutdown() function will reset the pin again.
 ***/
template <int MODE>
class PinShutdownMode
{
   private:
    byte shutdown_pin;

   public:
    PinShutdownMode(byte pin = SHUTDOWN_PIN) : shutdown_pin{pin}
    {
        pinMode(shutdown_pin, OUTPUT);
        digitalWrite(shutdown_pin, MODE);
    }

    void shutdown()
    {
        pinMode(shutdown_pin, INPUT);
    }
};

using SDMode = PinShutdownMode<HIGH>;
#elif defined(USE_SLEEP_MODE)
class SleepShutdownMode
{
   public:
    void shutdown()
    {
        set_sleep_mode(SLEEP_MODE_PWR_DOWN);
        cli();
        sleep_mode();
    }
};

using SDMode = SleepShutdownMode;
#endif


/***
 * Generic interface for the different playback modes
 ***/
class PlayerMode
{
   protected:
    uint8_t folder;
    uint8_t cur_track;
    const uint16_t max_tracks;
    bool was_paused;
    bool changed_track;

    bool play_track(uint8_t track)
    {
        mp3_player->playFolderTrack(folder, track);
        return true;
    }

    virtual void _stop()
    {
        cur_track = 1;
    }

    virtual bool _next()
    {
        if (cur_track == max_tracks)
            return false;

        cur_track++;
        return true; 
    }

    virtual bool _prev()
    {
        if (cur_track == 1)
            return false;

        cur_track--;
        return true;
    }

    PlayerMode(uint8_t folder) : folder{folder}, cur_track{1},
        max_tracks{mp3_player->getFolderTrackCount(folder)}, was_paused{false}, changed_track{false}
    {}

   public:
    virtual ~PlayerMode() = default;

    bool play()
    {
        if (!was_paused) {
            Serial.print(F("Start track "));
            Serial.println(cur_track);
            play_track(cur_track);
        } else if (was_paused && changed_track) {
            Serial.print(F("Resume playback at track "));
            Serial.println(cur_track);
            play_track(cur_track);
        } else {
            Serial.println(F("Resume playback"));
            mp3_player->start();
        }

        was_paused = changed_track = false;
        return true;
    }

    bool pause()
    {
        Serial.println(F("Pause playback"));
        mp3_player->pause();
        was_paused = true;

        return true;
    }

    bool stop()
    {
        Serial.println(F("Stop playback"));
        this->_stop();
        mp3_player->stop();

        return true;
    }

    bool next()
    {
        if (this->_next()) {
            Serial.print(F("Goto next track: "));
            Serial.println(cur_track);
            if (!was_paused) {
                return play_track(cur_track);
            } else {
                changed_track = true;
                return true;
            }
        }

        return false;
    }

    bool prev()
    {
        if (this->_prev()) {
            Serial.print(F("Goto previous track: "));
            Serial.println(cur_track);

            if (!was_paused) {
                return play_track(cur_track);
            } else {
                changed_track = true;
                return true;
            }
        }

        return false;
    }

    bool is_playing()
    {
        return !digitalRead(DFPLAYER_BUSY_PIN);
    }
};

/***
 * Specific playback modes
 ***/
class AlbumPlayerMode : public PlayerMode
{
   public:
    AlbumPlayerMode(uint8_t folder) : PlayerMode{folder}
    {
        Serial.print(F("Folder: "));
        Serial.print(folder);
        Serial.println(F(" mode: ALBUM"));
    }
};

class AudioBookPlayerMode : public PlayerMode
{
   public:
    AudioBookPlayerMode(uint8_t folder) : PlayerMode{folder}
    {
        Serial.print(F("Folder: "));
        Serial.print(folder);
        Serial.println(F(" mode: PLAYBOOK"));

        /* Get from the EEPROM where we last finished listening */
        this->cur_track = settings->progress(folder);
        Serial.print(F("Resume playback at track: "));
        Serial.println(this->cur_track);
    }

    ~AudioBookPlayerMode()
    {
        /* Save back to the EEPROM where we stopped listening this time */
        settings->progress(folder, this->cur_track);
    }
};

class PartyPlayerMode : public PlayerMode
{
   private:
    uint8_t *queue;
    uint8_t cur_ele;

   private:
    void _stop()
    {
        cur_ele = 0;
        this->cur_track = queue[cur_ele];
    }

    bool _next()
    {
        cur_ele = (cur_ele + 1) % this->max_tracks;
        this->cur_track = queue[cur_ele];

        return true;
    }

    bool _prev()
    {
        cur_ele = (cur_ele - 1) % this->max_tracks;
        this->cur_track = queue[cur_ele];

        return true;
    }

   public:
    PartyPlayerMode(uint8_t folder) : PlayerMode{folder}, queue{nullptr}, cur_ele{0}
    {
        Serial.print(F("Folder: "));
        Serial.print(folder);
        Serial.println(F(" mode: PARTY"));

        /* Generate our title queue */
        queue = new uint8_t[this->max_tracks];
        for (uint16_t i = 0; i < this->max_tracks; ++i)
            queue[i] = i+1;                             // fill in the possible titles

        for (uint16_t i = 0; i < this->max_tracks; ++i) {
            uint16_t j = random(0, this->max_tracks);   // find another random position
            uint8_t t = queue[j];                       // and shuffle the data around
            queue[j] = queue[i];
            queue[i] = t;
        }
    }

    ~PartyPlayerMode()
    {
        delete queue;
    }
};

class RepeatOnePlayerMode : public PlayerMode
{
   private:
    void _stop() {}
    bool _next() { return false; }
    bool _prev() { return false; }

   public:
    RepeatOnePlayerMode(uint8_t folder, uint8_t track) : PlayerMode{folder}
    {
        Serial.print(F("Folder: "));
        Serial.print(folder);
        Serial.print(F(" mode: ONE track: "));
        Serial.println(track);

        this->cur_track = track;
    }
};


/***
 * Specific system modes
 ***/

/* The shutdown manager should be created as soon as possible during bootup */
static SDMode sd_mode;

/* Global variables required for system management */
static EventManager mgr;
static DefaultMode *mode;


/* Free standing functions required for system management */

/* Shutting down the system */
void shutdown()
{
    rfid_reader->shutdown();
    mp3_player->sleep();

    /* Cleanup everything */
    delete mode;
    delete rfid_reader;
    delete mp3_player;

    /* Make sure we save our current system state */
    settings->to_eeprom();
    delete settings;

    /* Depending on your setup you either want to enter sleep mode
       or use the shutdown pin to shutdown the whole system.
       Which one will be used depends on the previously configured
       shutdown mode. */
    sd_mode.shutdown();
}

/* Method to dispatch input from the serial */
bool handle_serial_event()
{
    bool success = true;

    while (Serial.available() > 0) {
        int in = Serial.read();

        switch (in) {
            /* PlayButton long */
            case 'P': success &= mode->stop(); break;

            /* PlayButton short */
            case 'p': success &= mode->play(); break;

            /* PrevButton */
            case 'r': success &= mode->prev(); break;

            /* NextButton */
            case 'n': success &= mode->next(); break;

            /* VolupButton */
            case 'u': success &= mode->volume_up(); break;

            /* VoldownButton */
            case 'd': success &= mode->volume_down(); break;
#ifndef FIVEBUTTONS
            /* PrevButton long */
            case 'R': success &= mode->volume_down(); break;

            /* NextButton long */
            case 'N': success &= mode->volume_up(); break;
#endif
            case 'a': success &= mode->admin_mode(); break;
        }
    }

    return success;
}


/* Forward declare all possible system modes */
class StandbyMode;
class PlaybackMode;
class AdminMode;


class StandbyMode : public DefaultMode
{
   private:
    TimerEvent standby_timer;

   public:
      /* "Copy"-constructor so that we can extract the necessary state from the currently
       active mode, instead of building it from scratch again. */
    StandbyMode(DefaultMode *current) : DefaultMode{current},
        standby_timer{STANDBY_TIME_MS, []() -> bool { return mode->timer(); }}
    {
        Serial.println(F("Started Standby mode"));

        mgr.add(&standby_timer);
    }

    ~StandbyMode()
    {
        /* We have to make sure that the timer event gets removed when we delete this instance */
        mgr.remove(&standby_timer);
    }

    bool play()
    {
        RFIDCard card;

        if (!settings->last_card(card)) {
            standby_timer.reset();
            return false;
        }

        mode = switch_to<PlaybackMode>(card);
        return true;
    }

    bool timer()
    {
        Serial.println(F("Shutting down ..."));
        shutdown();
        return true;
    }

    bool new_card()
    {
        RFIDCard card;

        if (!rfid_reader->read_card(card)) {
            standby_timer.reset();
            return false;
        }

        mode = switch_to<PlaybackMode>(card);
        return true;
    }

    bool admin_mode()
    {
        mode = switch_to<AdminMode>();
        return true;
    }
};

class PlaybackMode : public DefaultMode
{
   private:
    PlayerMode *pmode;

    bool timer_active;
    TimerEvent abort_timer;

    void set_play_mode(const RFIDCard &card)
    {
        if (pmode)
            delete pmode;

        switch(card.mode) {
            case RFIDCard::Mode::ALBUM:
                pmode = new AlbumPlayerMode(card.folder);
                break;
            case RFIDCard::Mode::PARTY:
                pmode = new PartyPlayerMode(card.folder);
                break;
            case RFIDCard::Mode::ONE:
                pmode = new RepeatOnePlayerMode(card.folder, card.special);
                break;
            case RFIDCard::Mode::AUDIOBOOK:
                pmode = new AudioBookPlayerMode(card.folder);
                break;
        }
    }

    void activate_timer()
    {
        if (timer_active)
            return;

        timer_active = true;
        abort_timer.reset();
        mgr.add(&abort_timer);
    }

    void deactivate_timer()
    {
        if (!timer_active)
            return;

        mgr.remove(&abort_timer);
        timer_active = false;
    }

   public:
    PlaybackMode(DefaultMode *current, const RFIDCard &card) : DefaultMode{current}, pmode{nullptr},
        timer_active{false}, abort_timer{STOP_PLAYBACK_MS, []() -> bool { return mode->timer(); }}
    {
        Serial.println(F("Started Playback mode"));

        /* Remember that we are currently playing this card in the EEPROM */
        settings->save_last_card(card);

        set_play_mode(card);
        pmode->play();
    }

    ~PlaybackMode()
    {
        deactivate_timer();
        pmode->pause();
        if (pmode)
            delete pmode;
    }

    bool play()
    {
        if (pmode->is_playing()) {
            activate_timer();
            return pmode->pause();
        } else {
            deactivate_timer();
            return pmode->play();
        }
    }

    bool stop()
    {
        activate_timer();
        return pmode->stop();
    }

    bool next()
    {
        if (pmode->next()) {
            abort_timer.reset();
            return true;
        }

        return false;
    }

    bool prev()
    {
        if (pmode->prev()) {
            abort_timer.reset();
            return true;
        }

        return false;
    }

    bool volume_up()
    {
        if (settings->volume == settings->max_volume)
            return false;

        settings->volume++;
        mp3_player->increaseVolume();

        Serial.print(F("Increase volume ("));
        Serial.print(settings->volume);
        Serial.println(F(")"));

        return true;
    }

    bool volume_down()
    {
        if (settings->volume == settings->min_volume)
            return false;

        settings->volume--;
        mp3_player->decreaseVolume();

        Serial.print(F("Decrease volume ("));
        Serial.print(settings->volume);
        Serial.println(F(")"));

        return true;
    }

    bool track_finished()
    {
        Serial.println(F("Current track finished"));

        if (!pmode->next()) {
            /* We reached the end of the currently playback mode. Nothing more to play. 
               Go into standby. */
            Serial.println(F("Done playing"));
            mode = switch_to<StandbyMode>();
        }

        return true;
    }

    bool new_card()
    {
        /* Read in the current card */
        RFIDCard card;

        if (!rfid_reader->read_card(card)) {
            Serial.println(F("Failed to read card"));
            return false;
        }

        Serial.println(F("Switch playback according to card settings"));

        /* Stop the old playback */
        pmode->pause();

        deactivate_timer();

        /* Remember that we are currently playing this card in the EEPROM */
        settings->save_last_card(card);

        set_play_mode(card);
        return pmode->play();
    }

    bool timer()
    {
        /* Fall back to standby mode */
        mode = switch_to<StandbyMode>();
        return true;
    }

    bool is_playing()
    {
        return pmode->is_playing();
    }

    bool admin_mode()
    {
        mode = switch_to<AdminMode>();
        return true;
    }
};

class AdminMode : public DefaultMode
{
   private:
    class Menu
    {
       protected:
        Menu *parent;

        Menu(Menu *parent) : parent{parent}
        {}

        virtual Menu* _done() { return nullptr; }

       public:
        virtual ~Menu() = default;

        virtual void activate() {}

        virtual void next() {}
        virtual void prev() {}
        virtual void volume_up() {}
        virtual void volume_down() {}

        virtual void track_finished() {}
        virtual void new_card() {}

        Menu* done()
        {
            Menu *n = _done();
            if (!n) {
                n = parent;
                delete this;
            }

            if (n)
                n->activate();

            return n;
        }

        Menu* abort()
        {
            Menu *p = parent;
            delete this;

            if (p)
                p->activate();

            return p;
        }
    };

    template<class T>
    class SelectMenu : public Menu
    {
       private:
        T cur;
        T minimum;
        T maximum;

        T *dest;
        bool play;

       protected:
        Menu* _done()
        {
            *dest = cur;
            return nullptr;
        }

        void reset(T start, T minimum, T maximum)
        {
            cur = start;
            this->minimum = minimum;
            this->maximum = maximum;
        }

       public:
        SelectMenu(Menu *parent, T start, T minimum, T maximum, T* dest, bool play=true) : Menu{parent},
            cur{start}, minimum{minimum}, maximum{maximum}, dest{dest}, play{play}
        {}

        void activate()
        {
            Serial.print(F("Please choose between "));
            Serial.print(minimum);
            Serial.print(F(" and "));
            Serial.print(maximum);
            Serial.print(F(" (current: "));
            Serial.print(cur);
            Serial.println(F(")"));
        }

        void next()
        {
            if (cur == maximum)
                cur = minimum;
            else
                cur++;

            if (play) {
                Serial.print(F("current: "));
                Serial.println(cur);
            }
        }

        void volume_up()
        {
            bool p = play;
            play = false;

            for (int i = 0; i < 10; ++i)
                next();

            play = p;
            if (play) {
                Serial.print(F("current: "));
                Serial.println(cur);
            }
        }

        void prev()
        {
            if (cur == minimum)
                cur = maximum;
            else
                cur--;

            if (play) {
                Serial.print(F("current: "));
                Serial.println(cur);
            }
        }

        void volume_down()
        {
            bool p = play;
            play = false;

            for (int i = 0; i < 10; ++i)
                prev();

            play = p;
            if (play) {
                Serial.print(F("current: "));
                Serial.println(cur);
            }
        }
    };

    class ProgramCardMenu : public SelectMenu<uint8_t>
    {
       private:
        enum Steps : int {
            ChooseFolder,
            ChooseMode,
            ChooseSpecial,
            ChooseSpecial2,
            WaitForCard,
            ProgramCard,
        };

        Steps step;
        uint8_t value;

        uint8_t folder;
        RFIDCard::Mode mode;
        uint8_t special;
        uint8_t special2;

        void done_choose_folder()
        {
            SelectMenu::_done();

            folder = value;

            reset(static_cast<uint8_t>(RFIDCard::Mode::ALBUM),
                  static_cast<uint8_t>(RFIDCard::Mode::ALBUM),
                  static_cast<uint8_t>(RFIDCard::Mode::AUDIOBOOK));
            step = ChooseMode;
        }

        void done_choose_mode()
        {
            SelectMenu::_done();

            mode = static_cast<RFIDCard::Mode>(value);
            switch (mode) {
                case RFIDCard::Mode::ALBUM:
                case RFIDCard::Mode::PARTY:
                case RFIDCard::Mode::AUDIOBOOK:
                    step = WaitForCard;
                    break;
                case RFIDCard::Mode::ONE:
                    reset(0,0,100);
                    step = ChooseSpecial;
                    break;
            }
        }

        void done_choose_special()
        {
            SelectMenu::_done();

            special = value;
            switch (mode) {
                default:
                    step = WaitForCard;
                    break;
            }
        }

        void done_choose_special2()
        {
            SelectMenu::_done();

            special = value;
            step = WaitForCard;
        }

        void program_card()
        {
            RFIDCard card;
            card.folder = folder;
            card.mode = mode;
            card.special = special;
            card.special2 = special2;

            if (!rfid_reader->write_card(card))
                Serial.println(F("Failed to program card"));
            else
                Serial.println(F("Card programmed successfully"));
        }

        Menu* _done()
        {
            switch (step) {
                case ChooseFolder:
                    done_choose_folder();
                    return this;
                case ChooseMode:
                    done_choose_mode();
                    return this;
                case ChooseSpecial:
                    done_choose_special();
                    return this;
                case ChooseSpecial2:
                    done_choose_special2();
                    return this;
                case WaitForCard:
                    Serial.println(F("Still waiting for the RFID card"));
                    return this;
                case ProgramCard:
                    program_card();
                    return nullptr;
            }

            return nullptr;
        }

       public:
        ProgramCardMenu(Menu *parent) : SelectMenu(parent, 1, 1, 100, &value), step{Steps::ChooseFolder},
            value{0}, folder{0}, mode{RFIDCard::Mode::ALBUM}, special{0}, special2{0}
        {}

        void activate()
        {
            switch(step) {
                case ChooseFolder:
                    Serial.println(F("Choose the folder"));
                    break;
                case ChooseMode:
                    Serial.println(F("Choose the playback mode"));
                    break;
                case ChooseSpecial:
                    Serial.println(F("Choose the special value"));
                    break;
                case ChooseSpecial2:
                    Serial.println(F("Choose the second special value"));
                    break;
                case WaitForCard:
                    Serial.println(F("Waiting for RFID card"));
                    break;
            }

            SelectMenu::activate();
        }

        void new_card()
        {
            if (step == WaitForCard) {
                Serial.println(F("Now press enter to program this card."));
                step = ProgramCard;
            }
        }
    };

    class MainMenu : public SelectMenu<int>
    {
       private:
        enum Items : int {
            Exit = 0,
            MinVolume = 1,
            MaxVolume = 2,
            ProgramCard = 3,
        };

        int submenu;

        Menu* _done() {
            SelectMenu::_done();

            Menu *next = nullptr;
            switch (static_cast<Items>(submenu)) {
                case Exit:
                    break;
                case MinVolume:
                    next = new SelectMenu<uint8_t>(this, settings->min_volume, 0, settings->max_volume, &settings->min_volume);
                    break;
                case MaxVolume:
                    next = new SelectMenu<uint8_t>(this, settings->max_volume, settings->min_volume, 30, &settings->max_volume);
                    break;
                case ProgramCard:
                    next = new ProgramCardMenu(this);
                    break;
            }

            return next;
        }

        void activate()
        {
            reset(0, 0, 3);
            SelectMenu::activate();
        }

       public:
        MainMenu() : SelectMenu(nullptr, 0, 0, 4, &submenu)
        {}
    };

   private:
    Menu *menu;
    TimerEvent abort_timer;

   public:
    AdminMode(DefaultMode *current) : DefaultMode{current}, menu{nullptr},
        abort_timer{ABORT_MENU_MS, []() -> bool { return mode->timer(); }}
    {
        Serial.println(F("Started Admin mode"));

        mgr.add(&abort_timer);

        menu = new MainMenu();
        menu->activate();
    }

    ~AdminMode()
    {
        if (menu)
            delete menu;
        mgr.remove(&abort_timer);
    }

    bool play() 
    {
        abort_timer.reset();
        menu = menu->done();
        if (!menu)
            mode = switch_to<StandbyMode>();

        return true;
    }

    bool stop()
    {
        abort_timer.reset();
        menu = menu->abort();
        if (!menu)
            mode = switch_to<StandbyMode>();

        return true;
    }

    bool next()
    {
        menu->next();

        abort_timer.reset();
        return true;
    }

    bool prev()
    {
        menu->prev();

        abort_timer.reset();
        return true;
    }

    bool volume_up()
    {
        menu->volume_up();

        abort_timer.reset();
        return true;
    }

    bool volume_down()
    {
        menu->volume_down();

        abort_timer.reset();
        return true;
    }

    bool track_finished()
    {
        menu->track_finished();
        return true;
    }

    bool new_card()
    {
        menu->new_card();
        return true;
    }

    bool timer()
    {
        menu = menu->abort();
        if (!menu)
            mode = switch_to<StandbyMode>();

        return true;
    }
};


static SoftwareSerial mp3_player_serial(DFPLAYER_INPUT_PIN, DFPLAYER_OUTPUT_PIN);

#ifdef STATUS_LED
CRGB leds[1];
#endif

static ButtonWrapper play_button(PLAY_BUTTON_PIN);
static ButtonWrapper next_button(NEXT_BUTTON_PIN);
static ButtonWrapper prev_button(PREV_BUTTON_PIN);
#ifdef FIVEBUTTONS
static ButtonWrapper volup_button(VOLUP_BUTTON_PIN);
static ButtonWrapper voldown_button(VOLDOWN_BUTTON_PIN);
#endif

/* Events */
static ButtonPressDispatcher play_button_dispatcher([]() -> bool {return mode->play(); }, []() -> bool { return mode->stop(); });
static ButtonPressedEvent play_pause_event(&play_button, []() -> bool { return play_button_dispatcher.short_pressed(); });
static ButtonLongPressedEvent stop_event(&play_button, []() -> bool { return play_button_dispatcher.long_pressed(); });

#ifdef FIVEBUTTONS
static ButtonPressedEvent next_event(&next_button, []() -> bool { return mode->next(); });
static ButtonPressedEvent prev_event(&prev_button, []() -> bool { return mode->prev(); });
static ButtonPressedEvent volup_event(&volup_button, []() -> bool { return mode->volume_up(); });
static ButtonPressedEvent voldown_event(&voldown_button, []() -> bool { return mode->volume_down(); });
#else
static ButtonPressDispatcher next_button_dispatcher([]() -> bool { return mode->next(); }, []() -> bool { return mode->volume_up(); });
static ButtonPressedEvent next_event(&next_button, []() -> bool { return next_button_dispatcher.short_pressed(); });
static ButtonLongPressedEvent volup_event(&next_button, []() -> bool { return next_button_dispatcher.long_pressed(); });

static ButtonPressDispatcher prev_button_dispatcher([]() -> bool { return mode->prev(); }, []() -> bool { return mode->volume_down(); });
static ButtonPressedEvent prev_event(&prev_button, []() -> bool { return prev_button_dispatcher.short_pressed(); });
static ButtonLongPressedEvent voldown_event(&prev_button, []() -> bool { return prev_button_dispatcher.long_pressed(); });
#endif

static AdminModeEvent admin_event(&play_button, &next_button, &prev_button, []() -> bool { return mode->admin_mode(); });

static NewRFIDCardEvent new_card_event([]() -> bool { return mode->new_card(); });
static TrackFinishedEvent track_finished_event([]() -> bool { return mode->track_finished(); });
TrackFinishedEvent *MP3Notification::e = &track_finished_event; // We need to give the MP3Notification callback class a reference to
                                                                // our TrackFinishedEvent, so that it can be properly handled in the
                                                                // event loop.

#ifdef STATUS_LED
/* This method will calculate the ADC value interpreted from the
   voltage at the voltage pin, so that we can decide in which state
   the battery currently is.

   The system assumes that there is some for of voltage divider
   where the battery voltage is scaled to a comparable value.
   This scaled battery voltage is then interpreted by the internal
   ADC in we now have to calculate which value return by the ADC
   corresponds to which real battery voltage

   Adapt the below constants according to your circuit.
 */
static const int R1_kohm = 499;
static const int R2_kohm = 100;
static const constexpr float factor = static_cast<float>(R1_kohm + R2_kohm) / static_cast<float>(R2_kohm);

/* The internal reference voltage (either 1.1V if set to INTERNAL or 3.5V - 5V of not)
   I highly recommend using the constant internal reference voltage of 1.1V! */
static const constexpr float ref_voltage = 1.1;

constexpr int battery_event_reference_value(float battery_voltage)
{
    return (battery_voltage / factor) * (1023 / ref_voltage);
}

static AnalogEvent<EdgeTrigger, MoreEqualComp> battery_high_event(VOLTAGE_PIN, battery_event_reference_value(3.5) + 2,
    []() -> bool { return mode->battery_high(); });
static AnalogEvent<EdgeTrigger, LessEqualComp> battery_low_event(VOLTAGE_PIN, battery_event_reference_value(3.5),
    []() -> bool { return mode->battery_low(); });
static AnalogEvent<EdgeTrigger, LessComp> battery_critical_event(VOLTAGE_PIN, battery_event_reference_value(3.0),
    []() -> bool { return mode->battery_critical(); });

bool print_battery_voltage()
{
    int val = analogRead(VOLTAGE_PIN);
    Serial.print(F("Current battery voltage: "));

    int voltage = val * ref_voltage / 1023 * factor * 1000;
    Serial.print(voltage);
    Serial.print(F(" mV ("));
    Serial.print(val);
    Serial.println(F(")"));

    return true;

}
static TimerEvent voltage_print_event(PRINT_VOLTAGE_MS, print_battery_voltage, true);
#endif

static SerialEvent serial_event(handle_serial_event);


/* Arduino specific functions */
void setup()
{
    /* Startup our serial */
    Serial.begin(115200);

    // Lets get started :)
    Serial.println(F("\n _____         _____ _____ _____ _____"));
    Serial.println(F("|_   _|___ ___|  |  |     |   | |     |"));
    Serial.println(F("  | | | . |   |  |  |-   -| | | |  |  |"));
    Serial.println(F("  |_| |___|_|_|_____|_____|_|___|_____|\n"));
    Serial.println(F("Licensed under GNU/GPL"));

    /* Load our previous settings */
    settings = new Settings();

    /* Initialize all the module wrapper classes */
    mp3_player = new MP3Player(mp3_player_serial);
    mp3_player->begin();
    Serial.println(F("Setting up MP3 player - done"));

    rfid_reader = new RFIDReader(RFID_SS_PIN, RFID_RST_PIN);

    rfid_reader->begin();
    Serial.println(F("Setting up RFIDReader - done"));
    rfid_reader->print_version();

    mp3_player->setVolume(settings->volume);
    mp3_player->setEq(DfMp3_Eq_Normal);
    mp3_player->setPlaybackSource(DfMp3_PlaySource_Sd);

    /* Start the default mode */
#ifdef STATUS_LED
    FastLED.addLeds<NEOPIXEL, STATUS_LED_PIN>(leds, 1);

    mode = new DefaultMode(&leds[0]);
#else
    mode = new DefaultMode();
#endif

    /* Setup the input type for the buttons to INPUT_PULLUP */
    pinMode(PLAY_BUTTON_PIN, INPUT_PULLUP);
    pinMode(NEXT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(PREV_BUTTON_PIN, INPUT_PULLUP);
#ifdef FIVEBUTTONS
    pinMode(VOLUP_BUTTON_PIN, INPUT_PULLUP);
    pinMode(VOLDOWN_BUTTON_PIN, INPUT_PULLUP);
#endif

    /* Register event handlers and insert the events to the event manager */
    mgr.add(&play_pause_event);
    mgr.add(&stop_event);

    mgr.add(&next_event);
    mgr.add(&prev_event);
    mgr.add(&volup_event);
    mgr.add(&voldown_event);

    mgr.add(&admin_event);
    mgr.add(&track_finished_event);

    new_card_event.rfid_reader(rfid_reader);
    mgr.add(&new_card_event);

#ifdef STATUS_LED
    pinMode(VOLTAGE_PIN, INPUT);
    analogReference(INTERNAL);

    mgr.add(&battery_high_event);
    mgr.add(&battery_low_event);
    mgr.add(&battery_critical_event);
    mgr.add(&voltage_print_event);
#endif

    mgr.add(&serial_event);

    RFIDCard last_card;
    if (settings->last_card(last_card))
        mode = mode->switch_to<PlaybackMode>(last_card);
    else
        mode = mode->switch_to<StandbyMode>();
}

void loop()
{
    mgr.loop();
    mp3_player->loop();
}
