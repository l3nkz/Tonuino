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
//#define STATUS_LED

/* Uncomment one of the below to define what should be done when shutting
   down the system */
//#define USE_SLEEP_MODE
#define USE_SHUTDOWN_PIN

/* Uncomment if the system still has an unused input pin */
#define HAS_UNUSED_INPUT

/* Uncomment this if you want more serial debugging support */
//#define SERIAL_DEBUG


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
static const uint32_t STANDBY_SHUTDOWN_MS = 600000;     // == 10 minutes

/* The duration the system should wait before shutting down when
   in locked mode. */
static const uint32_t LOCKED_SHUTDOWN_MS = 5000;   // == 5 seconds

/* The duration the system waits before stopping a paused playback */
static const uint32_t STOP_PLAYBACK_MS = 60000;     // == 1 minute

/* The duration the system waits before exiting from the admin menu
   when there is no further input */
static const uint32_t ABORT_MENU_MS = 60000;        // == 1 minute

#if defined(STATUS_LED) and defined(SERIAL_DEBUG)
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
    virtual void reset() {}
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

    void reset()
    {
        for (Event *e = events; e; e = e->next)
            e->reset();
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

    void reset() {}

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

    void reset() { last_true = false; }

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

class AnalogMonitor
{
   private:
    int pin;
    int val;

   protected:
    bool needs_update;

   public:
    AnalogMonitor(int pin) : pin{pin}, val{0}, needs_update{true}
    {}

    virtual ~AnalogMonitor() = default;

    virtual void read() {
        if (needs_update) {
            val = analogRead(pin);
            needs_update = false;
        }
    }

    virtual int value() {
        if (needs_update)
            read();

        return val;
    }

    virtual void clear() {
        needs_update = true;
    }
};

template<int N>
class AveragedAnalogMonitor : public AnalogMonitor
{
   private:
    int buffer[N];
    int pos;
    int count;

   public:
    AveragedAnalogMonitor(int pin) : AnalogMonitor{pin}, buffer{0}, pos{0}, count{0}
    {}

    int value() {
        if (this->needs_update) {
            int val = AnalogMonitor::value();

            buffer[pos] = val;
            pos = (pos + 1) % N;
            if (count < N)
                count = count + 1;
        }

        int sum = 0;
        for (int i = 0; i < count; ++i)
            sum += buffer[i];

        return sum/count;
    }
};

template<template<typename> class Trigger, template<typename> class Comperator>
class AnalogEvent : public Event
{
   private:
    AnalogMonitor *monitor;
    Trigger<Comperator<int>> trigger;

   public:
    AnalogEvent(AnalogMonitor *monitor, int reference, const callback_func &f) : Event{f},
        monitor{monitor}, trigger{reference}
    {}

    void check_and_handle(uint32_t ms)
    {
        int val = monitor->value();

        if (trigger.triggered(val))
            this->handle();
    }

    void reset() { trigger.reset(); }

    void clear() { monitor->clear(); }
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
        if (last_track == track)
            return;

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
enum class FolderModes : uint8_t {
    ALBUM = 2,
    PARTY = 3,
    ONE = 4,
    AUDIOBOOK = 5,
};

enum class SpecialModes : uint8_t {
    ADMIN = 1,
    LOCKED = 2,
    UNLOCKED = 3,
};

class RFIDCard
{
   private:
    static const uint32_t cookie = 0x1337b347;
    static const byte version = 0x03;

   public:
    enum class Type : uint8_t {
        NONE = 0,
        FOLDER = 1,
        SPECIAL = 2,
    };

    struct Folder {
        uint8_t folder;
        FolderModes mode;
        uint8_t special;
        uint8_t special2;
    };

    struct Special {
        SpecialModes mode;
    };

    Type type;

   private:
    union {
        Folder f;
        Special s;
    } content;

   public:
    RFIDCard(Type t = Type::NONE) : type{t}
    {}

    Folder* folder()
    {
        if (type != Type::FOLDER)
            return nullptr;

        return &(content.f);
    }

    Special* special()
    {
        if (type != Type::SPECIAL)
            return nullptr;

        return &(content.s);
    }

    bool deserialize(byte b[18])
    {
        /* Before extracting the data check the card's cookie and version */
        uint32_t c = (static_cast<uint32_t>(b[0]) << 24) |
                     (static_cast<uint32_t>(b[1]) << 16) |
                     (static_cast<uint32_t>(b[2]) << 8)  |
                     static_cast<uint32_t>(b[3]);

        uint8_t v = b[4];

        if (c != cookie)
            return false;

        if (v == version) {
            type = static_cast<Type>(b[5]);
            switch (type) {
                case Type::FOLDER:
                    content.f.folder = b[6];
                    content.f.mode = static_cast<FolderModes>(b[7]);
                    content.f.special = b[8];
                    content.f.special2 = b[9];
                    break;
                case Type::SPECIAL:
                    content.s.mode = static_cast<SpecialModes>(b[6]);
                    break;
                case Type::NONE:
                    /* This should never happen */
                    return false;
            }
        } else if (v == 0x2) {
            /* We can still understand the old format. For now, keep it in */
            type = Type::FOLDER;
            content.f.folder = b[5];
            content.f.mode = static_cast<FolderModes>(b[6]);
            content.f.special = b[7];
            content.f.special2 = b[8];
        } else {
            return false;
        }

        return true;
    }

    bool serialize(byte b[16]) const
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
        b[5] = static_cast<byte>(type);
        switch (type) {
            case Type::FOLDER:
                b[6] = content.f.folder;
                b[7] = static_cast<byte>(content.f.mode);
                b[8] = content.f.special;
                b[9] = content.f.special2;
                break;
            case Type::SPECIAL:
                b[6] = static_cast<byte>(content.s.mode);
                break;
            case Type::NONE:
                /* This should never happen */
                return false;
        }

        return true;
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

#ifdef SERIAL_DEBUG
   public:
#else
   private:
#endif
    MFRC522 mfrc522;

   private:
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
        if (!card.serialize(buffer))
            return false;

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
     * 4        version (currently 2)
     * 5        volume
     * 6        minimum volume
     * 7        maximum volume
     * 8        last folder valid (0 == no, 1 == yes)
     * 9-12     last folder
     * 13       equalizer
     * 14-18    FREE
     * 19       status bits (see status enum)
     * 20       number of folder progresses
     * 21-...   progress of audio book folders
     **/

   private:
    /**
     * Status bits in the EEPROM:
     * bit      meaning
     * ----------------
     * 0        locked
     * 1-7      FREE
     **/
    enum class Status : byte {
        LOCKED = 0x1,
    };

   private:
    static const uint32_t cookie = 0xdeadbeef;
    static const byte version = 0x02;

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

    uint8_t equalizer;

    bool locked;

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

        equalizer = EEPROM.read(13);

        byte status = EEPROM.read(19);
        locked = (status & static_cast<byte>(Status::LOCKED)) != 0;

        /* Read the saved audio book progresses */
        byte nr_pogresses = EEPROM.read(20);
        Progress **n = &progresses;
        for (byte i = 0; i < nr_pogresses; ++i) {
            Progress *p = new Progress;

            p->folder = EEPROM.read(21+i*2);
            p->track = EEPROM.read(22+i*2);
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
        last_folder_valid = false;
        equalizer = static_cast<uint8_t>(DfMp3_Eq_Normal);
        locked = false;
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

        EEPROM.update(13, equalizer);

        byte status = 0;
        if (locked) status |= static_cast<byte>(Status::LOCKED);

        EEPROM.update(19, status);

        /* Write the saved audio book progresses */
        byte i = 0;
        for (Progress *n = progresses; n; n = n->next) {
            EEPROM.update(21+i*2, n->folder);
            EEPROM.update(22+i*2, n->track);
            i++;
        }
        EEPROM.update(20, i);
    }

    bool last_card(RFIDCard::Folder *folder)
    {
        if (!last_folder_valid)
            return false;

        folder->folder = last_folder;
        folder->mode = static_cast<FolderModes>(last_mode);
        folder->special = last_special;
        folder->special2 = last_special2;

        return true;
    }

    void save_last_card(const RFIDCard::Folder *folder)
    {
        last_folder = folder->folder;
        last_mode = static_cast<uint8_t>(folder->mode);
        last_special = folder->special;
        last_special2 = folder->special2;

        last_folder_valid = true;
    }

    uint8_t progress(uint8_t folder)
    {
        for (Progress *i = progresses; i; i = i->next) {
            if (i->folder == folder)
                return i->track;
        }

        /* If we don't know the folder yet, start with the first
           track in the folder. */
        return 1;
    }

    void save_progress(uint8_t folder, uint8_t track)
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

#ifdef SERIAL_DEBUG
    void dump()
    {
        Serial.println(F("Settings:"));
        Serial.print(F("V: ")); Serial.print(volume);
        Serial.print(F(" (")); Serial.print(min_volume);
        Serial.print(F(" - ")); Serial.print(max_volume); Serial.println(F(")"));
        if (last_folder_valid) {
            Serial.print(F("Folder: ")); Serial.print(last_folder);
            Serial.print(F(" ")); Serial.print(static_cast<uint8_t>(last_mode));
            switch(static_cast<FolderModes>(last_mode)) {
                case FolderModes::ALBUM: Serial.println(F(" A")); break;
                case FolderModes::PARTY: Serial.println(F(" P")); break;
                case FolderModes::ONE:
                    Serial.print(F(" O (")); Serial.print(last_special); Serial.println(")");
                    break;
                case FolderModes::AUDIOBOOK: Serial.println(F(" B")); break;
            }
        }
        Serial.print(F("EQ: ")); Serial.println(equalizer);
        Serial.print(F("Locked: ")); Serial.println(locked ? F("yes") : F("no"));
        if (progresses) {
            Serial.print(F("Progress: "));
            for (Progress *n = progresses; n; n = n->next) {
                Serial.print(n->folder); Serial.print("->"); Serial.print(n->track);
                Serial.print(F(" "));
            }
        }
    }
#endif
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

    virtual void activate() {}
    virtual void deactivate() {}

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
    virtual bool is_playing() { return false; }

    virtual bool battery_high() { return true; }
    virtual bool battery_low() { return true; }
    bool battery_critical()
    {
        Serial.println(F("Battery level is critical! Shutting down."));
        shutdown();

        return true;
    }

    template <class M, class ...Args>
    Mode* switch_to(Args... args)
    {
        deactivate();

        Mode *m = new M(this, args...);
        delete this;

        m->activate();

        return m;
    }
};

#ifdef STATUS_LED
class LEDMode : public Mode
{
   protected:
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
        *battery_led = CRGB::Green;
        FastLED.show();

        return true;
    }

    bool battery_low()
    {
        *battery_led = CRGB::Red;
        FastLED.show();

        return true;
    }

    template <class M, class ...Args>
    LEDMode* switch_to(Args... args)
    {
        deactivate();

        LEDMode *m = new M(this, args...);
        delete this;

        m->activate();

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
   private:
    bool save_progress;

    void _stop()
    {
        settings->remove_progress(folder);
        save_progress = false;
    }

    bool _next()
    {
        if (!PlayerMode::_next()) {
            settings->remove_progress(folder);
            save_progress = false;

            return false;
        }

        return true;
    }

   public:
    AudioBookPlayerMode(uint8_t folder) : PlayerMode{folder}, save_progress{true}
    {
        Serial.print(F("Folder: "));
        Serial.print(folder);
        Serial.println(F(" mode: AUDIOBOOK"));

        /* Get from the EEPROM where we last finished listening */
        this->cur_track = settings->progress(folder);
        Serial.print(F("Resume playback at track: "));
        Serial.println(this->cur_track);
    }

    ~AudioBookPlayerMode()
    {
        if (save_progress)
            /* Save back to the EEPROM where we stopped listening this time */
            settings->save_progress(folder, this->cur_track);
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


/* Forward declare all possible system modes */
class StandbyMode;
class PlaybackMode;
class AdminMode;
class LockedMode;


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

#ifdef SERIAL_DEBUG
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
            /* Entering Admin Mode */
            case 'a': success = true; mode = mode->switch_to<AdminMode>(); break;

            /* Shutting down the system */
            case 's': shutdown(); break;

            /* Dump system settings to serial */
            case 'S': success = true; settings->dump(); break;

            /* Print help message */
            case 'h':
                Serial.println(F("The following commands are available:"));
                Serial.println(F("P - Stop"));
                Serial.println(F("p - Play/Pause"));
                Serial.println(F("r - Previous"));
                Serial.println(F("n - Next"));
                Serial.println(F("u - Volume Up"));
                Serial.println(F("d - Volume Down"));
#ifndef FIVEBUTTONS
                Serial.println(F("R - Volume Down"));
                Serial.println(F("N - Volume Up"));
#endif
                Serial.println(F("a - Admin Mode"));
                Serial.println(F("s - Shutdown"));
                Serial.println(F("S - Dump Settings"));
                Serial.println(F("h - Help"));
                break;
        }
    }

    return success;
}
#endif


class StandbyMode : public DefaultMode
{
   private:
    TimerEvent standby_timer;

   public:
    StandbyMode(DefaultMode *current) : DefaultMode{current},
        standby_timer{STANDBY_SHUTDOWN_MS, []() -> bool { return mode->timer(); }}
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
        RFIDCard card(RFIDCard::Type::FOLDER);

        if (!settings->last_card(card.folder())) {
            standby_timer.reset();
            return false;
        }

        mode = switch_to<PlaybackMode>(card.folder());
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
        standby_timer.reset();

        RFIDCard card;
        if (!rfid_reader->read_card(card)) {
            Serial.println(F("Failed to read card."));
            mp3_player->playMp3FolderTrack(404);
            return false;
        }

        if (card.type == RFIDCard::Type::FOLDER) {
            mode = switch_to<PlaybackMode>(card.folder());
        } else if (card.type == RFIDCard::Type::SPECIAL) {
            RFIDCard::Special *s = card.special();

            switch (s->mode) {
                case SpecialModes::ADMIN:
                    mode = switch_to<AdminMode>();
                    break;
                case SpecialModes::LOCKED:
                    settings->locked = true;
                    mode = switch_to<LockedMode>();
                    break;
                case SpecialModes::UNLOCKED:
                    break;
            }
        }

        return true;
    }
};

class PlaybackMode : public DefaultMode
{
   private:
    PlayerMode *pmode;

    bool timer_active;
    TimerEvent abort_timer;

    void set_play_mode(const RFIDCard::Folder *folder)
    {
        if (pmode)
            delete pmode;

        switch(folder->mode) {
            case FolderModes::ALBUM:
                pmode = new AlbumPlayerMode(folder->folder);
                break;
            case FolderModes::PARTY:
                pmode = new PartyPlayerMode(folder->folder);
                break;
            case FolderModes::ONE:
                pmode = new RepeatOnePlayerMode(folder->folder, folder->special);
                break;
            case FolderModes::AUDIOBOOK:
                pmode = new AudioBookPlayerMode(folder->folder);
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

    void activate()
    {
        pmode->play();
    }

    void deactivate()
    {
        pmode->pause();
    }

   public:
    PlaybackMode(DefaultMode *current, const RFIDCard::Folder *folder) : DefaultMode{current}, pmode{nullptr},
        timer_active{false}, abort_timer{STOP_PLAYBACK_MS, []() -> bool { return mode->timer(); }}
    {
        Serial.println(F("Started Playback mode"));

        /* Remember that we are currently playing this card in the EEPROM */
        settings->save_last_card(folder);

        set_play_mode(folder);
    }

    ~PlaybackMode()
    {
        if (pmode)
            delete pmode;

        deactivate_timer();
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
        RFIDCard card;
        if (!rfid_reader->read_card(card)) {
            Serial.println(F("Failed to read card."));
            return false;
        }

        if (card.type == RFIDCard::Type::FOLDER) {
            mode = switch_to<PlaybackMode>(card.folder());
        } else if (card.type == RFIDCard::Type::SPECIAL) {
            RFIDCard::Special *s = card.special();

            switch (s->mode) {
                case SpecialModes::ADMIN:
                    mode = switch_to<AdminMode>();
                    break;
                case SpecialModes::LOCKED:
                    settings->locked = true;
                    mode = switch_to<LockedMode>();
                    break;
                case SpecialModes::UNLOCKED:
                    break;
            }
        }

        return true;
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
};

class AdminMode : public DefaultMode
{
   private:
    class MessagePlayer
    {
       private:
        uint16_t queue;
        bool completed;

       public:
        MessagePlayer() : queue{0}, completed{true}
        {}

        void play(uint16_t track, bool wait=false)
        {
            if (!completed) {
                queue = track;
                return;
            }

            mp3_player->playMp3FolderTrack(track);
            completed = !wait;
        }

        void track_finished()
        {
            if (!completed && queue != 0)
                mp3_player->playMp3FolderTrack(queue);

            queue = 0;
            completed = true;
        }
    };

   private:
    class Menu
    {
       protected:
        Menu *parent;
        MessagePlayer *player;

        Menu(Menu *parent) : parent{parent}, player{parent->player}
        {}

        Menu(MessagePlayer *player) : parent{nullptr}, player{player}
        {}

        virtual Menu* _done() { return nullptr; }

       public:
        virtual ~Menu() = default;

        virtual void activate() {}

        virtual void next() {}
        virtual void prev() {}
        virtual void volume_up() {}
        virtual void volume_down() {}

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

        uint16_t intro;
        uint16_t selections;

       private:
        void inline _next()
        {
            if (cur == maximum)
                cur = minimum;
            else
                cur++;
        }

        void inline _prev()
        {
            if (cur == minimum)
                cur = maximum;
            else
                cur--;
        }

       protected:
        Menu* _done()
        {
            *dest = cur;
            return nullptr;
        }

        void reset(T start, T minimum, T maximum, uint16_t intro=0, uint16_t selections=0)
        {
            cur = start;
            this->minimum = minimum;
            this->maximum = maximum;

            this->intro = intro;
            this->selections = selections;
        }

       public:
        SelectMenu(Menu *parent, T start, T minimum, T maximum, T* dest, uint16_t intro=0, uint16_t selections=0) : Menu{parent},
            cur{start}, minimum{minimum}, maximum{maximum}, dest{dest}, intro{intro}, selections{selections}
        {}

        SelectMenu(MessagePlayer *player, T start, T minimum, T maximum, T* dest, uint16_t intro=0, uint16_t selections=0) : Menu{player},
            cur{start}, minimum{minimum}, maximum{maximum}, dest{dest}, intro{intro}, selections{selections}
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

            if(intro != 0)
                this->player->play(intro);
        }

        void next()
        {
            _next();
            Serial.print(F("current: "));
            Serial.println(cur);

            if (selections != 0)
                this->player->play(selections+(cur-minimum));
        }

        void volume_up()
        {
            for (int i = 0; i < 10; ++i)
                _next();

            Serial.print(F("current: "));
            Serial.println(cur);

            if (selections != 0)
                this->player->play(selections+(cur-minimum));
        }

        void prev()
        {
            _prev();
            Serial.print(F("current: "));
            Serial.println(cur);

            if (selections != 0)
                this->player->play(selections+(cur-minimum));
        }

        void volume_down()
        {
            for (int i = 0; i < 10; ++i)
                _prev();

            Serial.print(F("current: "));
            Serial.println(cur);

            if (selections != 0)
                this->player->play(selections+(cur-minimum));
        }
    };

    class FolderCardMenu : public SelectMenu<uint8_t>
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
        FolderModes mode;
        uint8_t special;
        uint8_t special2;

        void done_choose_folder()
        {
            SelectMenu::_done();

            folder = value;

            reset(static_cast<uint8_t>(FolderModes::ALBUM),
                  static_cast<uint8_t>(FolderModes::ALBUM),
                  static_cast<uint8_t>(FolderModes::AUDIOBOOK),
                  360, 361);
            step = ChooseMode;
        }

        void done_choose_mode()
        {
            SelectMenu::_done();

            mode = static_cast<FolderModes>(value);
            switch (mode) {
                case FolderModes::ALBUM:
                case FolderModes::PARTY:
                case FolderModes::AUDIOBOOK:
                    reset(0, 0, 0, 400, 0);
                    step = WaitForCard;
                    break;
                case FolderModes::ONE:
                    reset(1, 1, mp3_player->getFolderTrackCount(folder), 370, 1);
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
                    reset(0, 0, 0, 400, 0);
                    step = WaitForCard;
                    break;
            }
        }

        void done_choose_special2()
        {
            SelectMenu::_done();

            special2 = value;
            reset(0, 0, 0, 400, 0);
            step = WaitForCard;
        }

        void program_card()
        {
            RFIDCard card(RFIDCard::Type::FOLDER);
            RFIDCard::Folder *f = card.folder();
            f->folder = folder;
            f->mode = mode;
            f->special = special;
            f->special2 = special2;

            if (!rfid_reader->write_card(card)) {
                Serial.println(F("Failed to program card"));
                this->player->play(403, true);
            } else {
                Serial.println(F("Card programmed successfully"));
                this->player->play(402, true);
            }
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
        FolderCardMenu(Menu *parent) : SelectMenu(parent, 1, 1, 100, &value, 350, 1),
            step{Steps::ChooseFolder}, value{0}, folder{0}, mode{FolderModes::ALBUM},
            special{0}, special2{0}
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
                case ProgramCard:
                    break;
            }

            SelectMenu::activate();
        }

        void new_card()
        {
            if (step == WaitForCard) {
                Serial.println(F("Now press enter to program this card."));
                this->player->play(401);
                step = ProgramCard;
            }
        }
    };

    class SpecialCardMenu : public SelectMenu<uint8_t>
    {
       private:
        enum Steps : int {
            ChooseMode,
            WaitForCard,
            ProgramCard,
        };

        Steps step;
        uint8_t value;

        SpecialModes mode;

       private:
        void done_choose_mode()
        {
            SelectMenu::_done();

            mode = static_cast<SpecialModes>(value);

            reset(0, 0, 0, 400, 0);
            step = WaitForCard;
        }

        void program_card()
        {
            RFIDCard card(RFIDCard::Type::SPECIAL);
            card.special()->mode = mode;

            if (!rfid_reader->write_card(card)) {
                Serial.println(F("Failed to program card"));
                mp3_player->playMp3FolderTrack(403);
            } else {
                Serial.println(F("Card programmed successfully"));
                mp3_player->playMp3FolderTrack(402);
            }
        }

        Menu* _done()
        {
            switch(step) {
                case ChooseMode:
                    done_choose_mode();
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
        SpecialCardMenu(Menu *parent) : SelectMenu(parent, 1, 1, 3, &value, 380, 381), step{Steps::ChooseMode}
        {}

        void activate()
        {
            switch(step) {
                case ChooseMode:
                    Serial.println(F("Choose the card mode"));
                    break;
                case WaitForCard:
                    Serial.println(F("Waiting for RFID card"));
                    break;
                case ProgramCard:
                    break;
            }

            SelectMenu::activate();
        }

        void new_card()
        {
            if (step == WaitForCard) {
                Serial.println(F("Now press enter to program this card."));
                step = ProgramCard;
                mp3_player->playMp3FolderTrack(401);
            }
        }
    };

    class MainMenu : public SelectMenu<int>
    {
       private:
#ifdef SERIAL_DEBUG
        static const constexpr int nr_submenus = 7;
#else
        static const constexpr int nr_submenus = 5;
#endif

        enum Items : int {
            Exit = 0,
            MinVolume = 1,
            MaxVolume = 2,
            Equalizer = 3,
            FolderCard = 4,
            SpecialCard = 5,
#ifdef SERIAL_DEBUG
            DumpCard = 6,
            DumpSettings = 7
#endif
        };

        int submenu;

        Menu* _done() {
            SelectMenu::_done();

            Menu *next = nullptr;
            switch (static_cast<Items>(submenu)) {
                case Exit:
                    break;
                case MinVolume:
                    next = new SelectMenu<uint8_t>(this, settings->min_volume, 0, settings->max_volume, &settings->min_volume, 320, 1);
                    break;
                case MaxVolume:
                    next = new SelectMenu<uint8_t>(this, settings->max_volume, settings->min_volume, 30, &settings->max_volume, 330, 1);
                    break;
                case Equalizer:
                    next = new SelectMenu<uint8_t>(this, settings->equalizer, 0, 5, &settings->equalizer, 340, 341);
                    break;
                case FolderCard:
                    next = new FolderCardMenu(this);
                    break;
                case SpecialCard:
                    next = new SpecialCardMenu(this);
                    break;
#ifdef SERIAL_DEBUG
                case DumpCard:
                    rfid_reader->mfrc522.PICC_DumpToSerial(&(rfid_reader->mfrc522.uid));
                    next = this;
                    break;
                case DumpSettings:
                    settings->dump();
                    next = this;
                    break;
#endif
            }

            return next;
        }

        void activate()
        {
            switch (static_cast<Items>(submenu)) {
                case Equalizer:
                    mp3_player->setEq(static_cast<DfMp3_Eq>(settings->equalizer));
                    break;
                default:
                    break;
            }

            reset(0, 0, nr_submenus, 310, 311);
            SelectMenu::activate();
        }

       public:
        MainMenu(MessagePlayer *player) : SelectMenu(player, 0, 0, nr_submenus, &submenu, 310, 311)
        {}
    };

   private:
    Menu *menu;
    MessagePlayer m_player;
    TimerEvent abort_timer;

    void activate()
    {
        menu->activate();
    }

   public:
    AdminMode(DefaultMode *current) : DefaultMode{current}, menu{nullptr}, m_player{},
        abort_timer{ABORT_MENU_MS, []() -> bool { return mode->timer(); }}
    {
        Serial.println(F("Started Admin mode"));

        mgr.add(&abort_timer);
        menu = new MainMenu(&m_player);
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
        m_player.track_finished();
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

class LockedMode : public DefaultMode
{
   private:
    TimerEvent shutdown_timer;

#ifdef STATUS_LED
    bool led_on;
    TimerEvent blink_timer;
#endif

   private:
#ifdef STATUS_LED
    void switch_led_state()
    {
        if (led_on) {
            *(this->battery_led) = CRGB::Black;
            led_on = false;
        } else {
            *(this->battery_led) = CRGB::Red;
            led_on = true;
        }

        FastLED.show();
    }
#endif

    void activate()
    {
        Serial.println(F("The system is locked. Use unlock card to use the system again."));
#ifdef STATUS_LED
        switch_led_state();
#endif
    }

   public:
#ifndef STATUS_LED
    LockedMode(DefaultMode *current) : DefaultMode{current},
        shutdown_timer(LOCKED_SHUTDOWN_MS, []() -> bool { shutdown(); return true; })
    {
    }
#else
    LockedMode(DefaultMode *current) : DefaultMode{current},
        shutdown_timer(LOCKED_SHUTDOWN_MS, []() -> bool { shutdown(); return true; }),
        led_on{false},
        blink_timer(100, []() -> bool { return mode->timer(); }, true)
    {
        mgr.add(&shutdown_timer);
        mgr.add(&blink_timer);
    }
#endif

    ~LockedMode()
    {
        mgr.remove(&shutdown_timer);
#ifdef STATUS_LED
        mgr.remove(&blink_timer);
#endif

        mgr.reset();
    }

#ifdef STATUS_LED
    bool timer()
    {
        switch_led_state();
        return true;
    }
#endif

    bool new_card()
    {
        RFIDCard card;
        if (!rfid_reader->read_card(card)) {
            Serial.println(F("Failed to read card."));
            return false;
        }

        if (card.type == RFIDCard::Type::FOLDER) {
            Serial.println(F("The system is locked!"));
        } else if (card.type == RFIDCard::Type::SPECIAL) {
            RFIDCard::Special *s = card.special();

            switch (s->mode) {
                case SpecialModes::UNLOCKED:
                    Serial.println(F("Unlocking system"));
                    settings->locked = false;
                    mode = switch_to<StandbyMode>();
                    break;
                default:
                    Serial.println(F("The system is locked!"));
                    break;
            }
        }

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

static AveragedAnalogMonitor<8> voltage_monitor(VOLTAGE_PIN);

static AnalogEvent<EdgeTrigger, MoreEqualComp> battery_high_event(&voltage_monitor, battery_event_reference_value(3.7),
    []() -> bool { return mode->battery_high(); });
static AnalogEvent<EdgeTrigger, LessEqualComp> battery_low_event(&voltage_monitor, battery_event_reference_value(3.5),
    []() -> bool { return mode->battery_low(); });
static AnalogEvent<EdgeTrigger, LessComp> battery_critical_event(&voltage_monitor, battery_event_reference_value(3.0),
    []() -> bool { return mode->battery_critical(); });

#ifdef SERIAL_DEBUG
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
#endif

#ifdef SERIAL_DEBUG
static SerialEvent serial_event(handle_serial_event);
#endif


/* Arduino specific functions */
void setup()
{
    /* Startup our serial */
    Serial.begin(115200);

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
    mp3_player->setEq(static_cast<DfMp3_Eq>(settings->equalizer));
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

    // Lets get started :)
    Serial.println(F("\n _____         _____ _____ _____ _____"));
    Serial.println(F("|_   _|___ ___|  |  |     |   | |     |"));
    Serial.println(F("  | | | . |   |  |  |-   -| | | |  |  |"));
    Serial.println(F("  |_| |___|_|_|_____|_____|_|___|_____|\n"));
    Serial.println(F("Licensed under GNU/GPL"));

    /* Register event handlers and insert the events to the event manager */
    mgr.add(&play_pause_event);
    mgr.add(&stop_event);

    mgr.add(&next_event);
    mgr.add(&prev_event);
    mgr.add(&volup_event);
    mgr.add(&voldown_event);

    mgr.add(&track_finished_event);

    new_card_event.rfid_reader(rfid_reader);
    mgr.add(&new_card_event);

#ifdef STATUS_LED
    pinMode(VOLTAGE_PIN, INPUT);
    analogReference(INTERNAL);

    mgr.add(&battery_high_event);
    mgr.add(&battery_low_event);
    mgr.add(&battery_critical_event);
#ifdef SERIAL_DEBUG
    mgr.add(&voltage_print_event);
#endif
#endif

#ifdef SERIAL_DEBUG
    mgr.add(&serial_event);
#endif

    if (settings->locked)
        mode = mode->switch_to<LockedMode>();
    else
        mode = mode->switch_to<StandbyMode>();
}

void loop()
{
    mgr.loop();
    mp3_player->loop();
}
