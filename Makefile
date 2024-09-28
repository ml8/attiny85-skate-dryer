build: main.c
	avr-gcc -Wall -Os -mmcu=attiny85 main.c

hex: build
	avr-objcopy -O ihex -j .text -j .data a.out a.hex

flash: hex
	avrdude -C /opt/homebrew/etc/avrdude.conf -v -p t85 -c usbtiny -U flash:w:a.hex:i -B 6kHz

clean:
	rm -f a.out a.hex
