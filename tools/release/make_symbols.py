#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The release's symbols, for crash reports: haumea-symbols-<version>.zip.

    make_symbols.py <output.zip> [--linux <haumea> <haumea.debug>]
                    [--windows <haumea.exe> <haumea.pdb>] [--macos <haumea> <haumea.dSYM>]
    make_symbols.py --build-id <binary>

The zip holds what tools/telemetry/symbols.py looks for:

    linux-amd64/haumea.debug
    windows-gl3/haumea.pdb
    macos-arm64/haumea.dSYM/...
    BUILD_IDS.txt    "<platform> <build id>" per line

A build id is what the editor puts in its reports' build_id
(tools/haumea/crashreports.cpp findBuildId): the ELF's GNU build-id in
lowercase hex, the PE's CodeView PDB GUID and age in the symbol-server form,
the Mach-O UUID in uppercase hex.
"""
import struct
import sys
import zipfile
from pathlib import Path


def elf_build_id(data):
	if data[4] != 2 or data[5] != 1:
		raise ValueError('not a 64-bit little-endian ELF')
	shoff, = struct.unpack_from('<Q', data, 0x28)
	shentsize, shnum = struct.unpack_from('<HH', data, 0x3A)
	for i in range(shnum):
		sh_type, = struct.unpack_from('<I', data, shoff + i * shentsize + 4)
		if sh_type != 7:	# SHT_NOTE
			continue
		offset, size = struct.unpack_from('<QQ', data, shoff + i * shentsize + 0x18)
		at = offset
		while at + 12 <= offset + size:
			namesz, descsz, kind = struct.unpack_from('<III', data, at)
			name = data[at + 12:at + 12 + namesz]
			desc_at = at + 12 + (namesz + 3) // 4 * 4
			if kind == 3 and name == b'GNU\0':	# NT_GNU_BUILD_ID
				return data[desc_at:desc_at + descsz].hex()
			at = desc_at + (descsz + 3) // 4 * 4
	return None


def pe_build_id(data):
	pe, = struct.unpack_from('<I', data, 0x3C)
	if data[pe:pe + 4] != b'PE\0\0':
		raise ValueError('not a PE file')
	sections, = struct.unpack_from('<H', data, pe + 6)
	optional_size, = struct.unpack_from('<H', data, pe + 20)
	optional = pe + 24
	magic, = struct.unpack_from('<H', data, optional)
	dirs = optional + (112 if magic == 0x20B else 96)
	debug_rva, debug_size = struct.unpack_from('<II', data, dirs + 6 * 8)
	table = optional + optional_size

	def file_offset(rva):
		for i in range(sections):
			s = table + i * 40
			vsize, va, rawsize, raw = struct.unpack_from('<IIII', data, s + 8)
			if va <= rva < va + max(vsize, rawsize):
				return raw + rva - va
		raise ValueError('rva outside the sections')

	at = file_offset(debug_rva)
	for i in range(debug_size // 28):
		kind, = struct.unpack_from('<I', data, at + i * 28 + 12)
		raw_ptr, = struct.unpack_from('<I', data, at + i * 28 + 24)
		if kind == 2 and data[raw_ptr:raw_ptr + 4] == b'RSDS':	# IMAGE_DEBUG_TYPE_CODEVIEW
			d1, d2, d3 = struct.unpack_from('<IHH', data, raw_ptr + 4)
			d4 = data[raw_ptr + 12:raw_ptr + 20]
			age, = struct.unpack_from('<I', data, raw_ptr + 20)
			return '%08X%04X%04X%s%X' % (d1, d2, d3, d4.hex().upper(), age)
	return None


def macho_build_id(data):
	magic, = struct.unpack_from('<I', data, 0)
	if magic != 0xFEEDFACF:
		raise ValueError('not a 64-bit Mach-O')
	ncmds, = struct.unpack_from('<I', data, 16)
	at = 32
	for _ in range(ncmds):
		cmd, size = struct.unpack_from('<II', data, at)
		if cmd == 0x1B:	# LC_UUID
			return data[at + 8:at + 24].hex().upper()
		at += size
	return None


def build_id(path):
	data = Path(path).read_bytes()
	if data[:4] == b'\x7fELF':
		return elf_build_id(data)
	if data[:2] == b'MZ':
		return pe_build_id(data)
	return macho_build_id(data)


def main(argv):
	if argv[:1] == ['--build-id']:
		print(build_id(argv[1]) or '')
		return 0
	output, rest = Path(argv[0]), argv[1:]
	entries, ids = [], []
	while rest:
		flag, binary, symbols = rest[:3]
		rest = rest[3:]
		platform = {'--linux': 'linux-amd64', '--windows': 'windows-gl3', '--macos': 'macos-arm64'}[flag]
		bid = build_id(binary)
		if not bid:
			raise SystemExit(f'{binary} has no build id')
		ids.append(f'{platform} {bid}\n')
		symbols = Path(symbols)
		if symbols.is_dir():
			# A dSYM, perhaps as a downloaded artifact's folder under another name.
			for f in sorted(p for p in symbols.rglob('*') if p.is_file()):
				entries.append((f, f'{platform}/haumea.dSYM/{f.relative_to(symbols).as_posix()}'))
		else:
			entries.append((symbols, f'{platform}/{symbols.name}'))
	with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as z:
		for source, name in entries:
			z.write(source, name)
		z.writestr('BUILD_IDS.txt', ''.join(ids))
	print(f'{output}: {len(entries)} file(s); ' + ' '.join(i.strip() for i in ids))
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
