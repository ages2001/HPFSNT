#!/usr/bin/env python3
"""bin2h.py FILE NAME: a binary file as a C array"""
import sys
d = open(sys.argv[1], 'rb').read()
print('/* generated from %s */' % sys.argv[1].split('/')[-1])
print('static const unsigned char %s[%d] = {' % (sys.argv[2], len(d)))
for i in range(0, len(d), 16):
    print('    ' + ', '.join('0x%02X' % b for b in d[i:i+16]) + ',')
print('};')
