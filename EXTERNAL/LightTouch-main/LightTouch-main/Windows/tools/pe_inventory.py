"""Bounded PE import inventory; never loads or executes native code."""
import struct


AUXILIARY_LAUNCHERS = {
    'Lib/site-packages/pip/_vendor/distlib/' + stem + '.exe': machine
    for stems, machine in ((('t32', 'w32'), 0x14c), (('t64-arm', 'w64-arm'), 0xaa64))
    for stem in stems
}


def inspect_pe(data, relative_name):
    """Require x64 runtime code; identify only exact pip launcher data exceptions."""
    def require(condition, detail):
        if not condition:
            raise ValueError('{}: {}'.format(relative_name, detail))

    def unpack(format, offset):
        size = struct.calcsize(format)
        require(0 <= offset <= len(data) - size, 'truncated PE metadata')
        return struct.unpack_from(format, data, offset)

    require(len(data) >= 64 and data[:2] == b'MZ', 'missing DOS header')
    pe, = unpack('<I', 60)
    require(data[pe:pe + 4] == b'PE\0\0', 'missing PE signature')
    machine, count = unpack('<HH', pe + 4)
    require(machine == AUXILIARY_LAUNCHERS.get(relative_name, 0x8664), 'unexpected machine {}'.format(hex(machine)))
    require(1 <= count <= 96, 'invalid section count')
    optional_size, = unpack('<H', pe + 20)
    optional = pe + 24
    magic, = unpack('<H', optional)
    require(magic == (0x10b if machine == 0x14c else 0x20b), 'invalid optional-header format')
    directory = 96 if magic == 0x10b else 112
    require(optional_size >= directory and optional + optional_size <= len(data), 'truncated optional header')
    headers, = unpack('<I', optional + 60)
    require(headers <= len(data), 'headers exceed file')
    sections = []
    for index in range(count):
        size, address, raw_size, raw = unpack('<IIII', optional + optional_size + index * 40 + 8)
        require(raw + raw_size <= len(data), 'section exceeds file')
        sections.append((address, raw_size, raw))

    def offset(rva, length):
        candidates = []
        if 0 <= rva < headers and rva + length <= headers:
            candidates.append(rva)
        for address, raw_size, raw in sections:
            delta = rva - address
            if 0 <= delta < raw_size and delta + length <= raw_size:
                candidates.append(raw + delta)
        require(len(candidates) == 1, 'unmapped or ambiguous RVA')
        return candidates[0]

    def dll_name(rva):
        value = bytearray()
        for index in range(260):
            byte = data[offset(rva + index, 1)]
            if byte == 0:
                require(value and all(32 < b < 127 and b not in b'/\\:' for b in value), 'invalid DLL name')
                return value.decode('ascii')
            value.append(byte)
        raise ValueError('{}: unterminated DLL name'.format(relative_name))

    result = {'machine': hex(machine), 'auxiliary_launcher_data': relative_name in AUXILIARY_LAUNCHERS, 'imports': []}
    directories, = unpack('<I', optional + directory - 4)
    if directories < 2:
        return result
    require(optional_size >= directory + 16, 'truncated import directory')
    import_rva, import_size = unpack('<II', optional + directory + 8)
    if import_rva == 0:
        require(import_size == 0, 'import size without import address')
        return result
    require(import_size >= 20, 'truncated import descriptor table')
    for index in range(min(import_size // 20, 1024)):
        descriptor = unpack('<IIIII', offset(import_rva + index * 20, 20))
        if descriptor == (0, 0, 0, 0, 0):
            return result
        result['imports'].append(dll_name(descriptor[3]))
    raise ValueError('{}: import table has no bounded terminator'.format(relative_name))
