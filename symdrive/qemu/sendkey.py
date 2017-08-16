#!/usr/bin/python

"""Module sendkey.

This program converts strings into QEMU sendkey commands
"""
import sys
import time

def special_print(msg):
    print msg
    sys.stdout.flush()
    time.sleep(0.1)

def translate(input):
    QEMU = {}
    # QEMU[''] = 'shift'
    # QEMU[''] = 'shift_r'
    # QEMU[''] = 'alt'
    # QEMU[''] = 'alt_r'
    # QEMU[''] = 'altgr'
    # QEMU[''] = 'altgr_r'
    # QEMU[''] = 'ctrl'
    # QEMU[''] = 'ctrl_r'
    # QEMU[''] = 'menu'
    # QEMU[''] = 'esc'
    QEMU['1'] = '1'
    QEMU['2'] = '2'
    QEMU['3'] = '3'
    QEMU['4'] = '4'
    QEMU['5'] = '5'
    QEMU['6'] = '6'
    QEMU['7'] = '7'
    QEMU['8'] = '8'
    QEMU['9'] = '9'
    QEMU['0'] = '0'
    QEMU['-'] = 'minus'
    QEMU['='] = 'equal'
    # QEMU[''] = 'backspace'
    # QEMU[''] = 'tab'
    QEMU['q'] = 'q'
    QEMU['w'] = 'w'
    QEMU['e'] = 'e'
    QEMU['r'] = 'r'
    QEMU['t'] = 't'
    QEMU['y'] = 'y'
    QEMU['u'] = 'u'
    QEMU['i'] = 'i'
    QEMU['o'] = 'o'
    QEMU['p'] = 'p'
    # QEMU[''] = 'ret'
    QEMU['a'] = 'a'
    QEMU['s'] = 's'
    QEMU['d'] = 'd'
    QEMU['f'] = 'f'
    QEMU['g'] = 'g'
    QEMU['h'] = 'h'
    QEMU['j'] = 'j'
    QEMU['k'] = 'k'
    QEMU['l'] = 'l'
    QEMU['z'] = 'z'
    QEMU['x'] = 'x'
    QEMU['c'] = 'c'
    QEMU['v'] = 'v'
    QEMU['b'] = 'b'
    QEMU['n'] = 'n'
    QEMU['m'] = 'm'
    QEMU[','] = 'comma'
    QEMU['.'] = 'dot'
    QEMU['/'] = 'slash'
    QEMU['*'] = 'asterisk'
    QEMU[' '] = 'spc'
    # QEMU[''] = 'caps_lock'
    # QEMU[''] = 'f1'
    # QEMU[''] = 'f2'
    # QEMU[''] = 'f3'
    # QEMU[''] = 'f4'
    # QEMU[''] = 'f5'
    # QEMU[''] = 'f6'
    # QEMU[''] = 'f7'
    # QEMU[''] = 'f8'
    # QEMU[''] = 'f9'
    # QEMU[''] = 'f10'
    # QEMU[''] = 'num_lock'
    # QEMU[''] = 'scroll_lock'
    # QEMU[''] = 'kp_divide'
    # QEMU[''] = 'kp_multiply'
    # QEMU[''] = 'kp_subtract'
    # QEMU[''] = 'kp_add'
    # QEMU[''] = 'kp_enter'
    # QEMU[''] = 'kp_decimal'
    # QEMU[''] = 'sysrq'
    # QEMU[''] = 'kp_0'
    # QEMU[''] = 'kp_1'
    # QEMU[''] = 'kp_2'
    # QEMU[''] = 'kp_3'
    # QEMU[''] = 'kp_4'
    # QEMU[''] = 'kp_5'
    # QEMU[''] = 'kp_6'
    # QEMU[''] = 'kp_7'
    # QEMU[''] = 'kp_8'
    # QEMU[''] = 'kp_9'
    # QEMU[''] = '<'
    # QEMU[''] = 'f11'
    # QEMU[''] = 'f12'
    # QEMU[''] = 'print'
    # QEMU[''] = 'home'
    # QEMU[''] = 'pgup'
    # QEMU[''] = 'pgdn'
    # QEMU[''] = 'end'
    # QEMU[''] = 'left'
    # QEMU[''] = 'up'
    # QEMU[''] = 'down'
    # QEMU[''] = 'right'
    # QEMU[''] = 'insert'
    # QEMU[''] = 'delete'
    str = 'sendkey ' + QEMU[input]
    special_print(str)
    
def main():
    # parse command line options
    argstring = ' '.join(sys.argv[1:])

    for c in argstring:
        translate(c)
    special_print ("sendkey ret")
    special_print ("~")
    special_print ("quit")
    
main()
