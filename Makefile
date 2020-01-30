ARDUINO_OPTIONS ?= --config-file arduino-cli.yaml
TTY ?= /dev/ttyUSB0
BAUDRATE ?= 115200

all: Tonuino.ino.arduino.avr.nano.hex

upload: Tonuino.ino.arduino.avr.nano.hex
	lsof ${TTY} | awk "NR > 1 {system(\"kill -15 \" $2)}"
	arduino-cli ${ARDUINO_OPTIONS} upload -v -b arduino:avr:nano:cpu=atmega328old -p ${TTY} -i Tonuino.ino.arduino.avr.nano.hex

Tonuino.ino.arduino.avr.nano.hex: Tonuino.ino
	arduino-cli ${ARDUINO_OPTIONS} compile -b arduino:avr:nano:cpu=atmega328old --warnings more Tonuino.ino

console:
	minicom -D ${TTY} -b ${BAUDRATE} -o

clean:
	rm -f Tonuino.ino.arduino.avr.nano.*

.PHONY: upload console clean
