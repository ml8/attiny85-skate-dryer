import os
import ycm_core

flags = [
        '-D__AVR_ATtiny85__',
        '-Wall',
        '-std=c99',
        '-I/opt/homebrew/Cellar/avr-gcc@8/8.5.0_2/avr/include',
        '-I/opt/homebrew/Cellar/avr-gcc@8/8.5.0_2/avr/include/avr',
        #'-mmcu=attiny85',
        '-D__AVR__',
        ]

def Settings(**kwargs):
    return { 'flags': flags }
