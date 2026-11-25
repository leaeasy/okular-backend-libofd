#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 OFD plugin contributors
# SPDX-License-Identifier: CC0-1.0
"""Create the tiny OFD test font without third-party font dependencies.

The A glyph is a solid square and B is a triangle, deliberately unlike system
fonts. The fixture tests font embedding and advances, not typography.
SFNT table layout: https://learn.microsoft.com/en-us/typography/opentype/spec/otff
"""
import struct
from pathlib import Path


def pack(fmt, *values):
    return struct.pack('>' + fmt, *values)


def glyph(points):
    xs, ys = zip(*points)
    data = pack('hhhhhHH', 1, min(xs), min(ys), max(xs), max(ys), len(points) - 1, 0)
    data += bytes([1] * len(points))  # On-curve, signed 16-bit coordinate deltas.
    for coordinates in (xs, ys):
        previous = 0
        for value in coordinates:
            data += pack('h', value - previous)
            previous = value
    return data + bytes(len(data) % 2)


def checksum(data):
    padded = data + bytes(-len(data) % 4)
    return sum(struct.unpack('>' + 'I' * (len(padded) // 4), padded)) & 0xffffffff


head = bytearray(54)
struct.pack_into('>IIIIHH', head, 0, 0x10000, 0x10000, 0, 0x5f0f3cf5, 0, 1000)
struct.pack_into('>hhhhHHhhh', head, 36, 0, 0, 800, 800, 0, 8, 2, 1, 0)
hhea = bytearray(36)
struct.pack_into('>IhhhHhhhhhh', hhea, 0, 0x10000, 800, -200, 0, 1000, 0, 200, 800, 1, 0, 0)
struct.pack_into('>H', hhea, 34, 3)
glyphs = [pack('hhhhh', 0, 0, 0, 0, 0), glyph([(0, 0), (800, 0), (800, 800), (0, 800)]), glyph([(0, 0), (800, 0), (400, 800)])]
offsets = [0]
for data in glyphs:
    offsets.append(offsets[-1] + len(data))
cmap4 = pack('7H', 4, 32, 0, 4, 4, 1, 0) + pack('9H', 66, 0xffff, 0, 65, 0xffff, 0xffc0, 1, 0, 0)
names = [(1, 'OFDTest Shapes'), (2, 'Regular'), (4, 'OFDTest Shapes Regular'), (6, 'OFDTestShapes-Regular')]
strings = b''
records = b''
for name_id, text in names:
    encoded = text.encode('utf-16-be')
    records += pack('6H', 3, 1, 0x409, name_id, len(encoded), len(strings))
    strings += encoded
tables = {
    b'cmap': pack('HHHHI', 0, 1, 3, 1, 12) + cmap4,
    b'glyf': b''.join(glyphs),
    b'head': bytes(head),
    b'hhea': bytes(hhea),
    b'hmtx': pack('HhHhHh', 1000, 0, 1000, 0, 1000, 0),
    b'loca': pack('4I', *offsets),
    b'maxp': pack('I14H', 0x10000, 3, 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0),
    b'name': pack('3H', 0, len(names), 6 + 12 * len(names)) + records + strings,
    b'post': pack('I', 0x30000) + bytes(28),
}
count = len(tables)
power = 2 ** (count.bit_length() - 1)
font = pack('I4H', 0x10000, count, power * 16, power.bit_length() - 1, count * 16 - power * 16)
body = b''
head_offset = 0
for tag, data in sorted(tables.items()):
    offset = 12 + 16 * count + len(body)
    font += tag + pack('3I', checksum(data), offset, len(data))
    if tag == b'head':
        head_offset = offset
    body += data + bytes(-len(data) % 4)
font = bytearray(font + body)
struct.pack_into('>I', font, head_offset + 8, (0xb1b0afba - checksum(font)) & 0xffffffff)
Path(__file__).with_name('OFDTest.ttf').write_bytes(font)
